/*
 * Browser-side music (HARNESS.md §9.8).
 *
 * The companion to sfx.ts, and it splits the work the same way: the wasm module
 * decides *what* should be playing and this decides *how*. What crosses the
 * boundary is a playlist of MIDI filenames -- the same strings the level scripts
 * name and audio.cpp keeps in `Tracks` -- plus master volume and the intensity
 * the game recomputes every turn.
 *
 * Nothing here synthesizes MIDI. `Music/*.mid` are never fetched by the page;
 * they are rendered ahead of time into OGG stems (tools/music/split-stems.py,
 * tools/music/render-stems.py) and this plays those. That is what lets the
 * browser build drop RtMidi, the MIDI parser and the playback engine -- about
 * 4,700 lines that could not have worked on this target anyway -- rather than
 * port them.
 *
 * ---- why the stems ----
 *
 * IVAN's music is adaptive: `character::Be` sets an intensity from the
 * player's worst body part every turn (char.cpp:1062), and audio.cpp turns it
 * into a per-channel MIDI volume. Those volumes only ever take three shapes --
 * constant, falling with intensity, rising with intensity -- so three
 * pre-rendered stems behind three gain nodes reproduce the native mix exactly
 * rather than approximating it. split-stems.py has the derivation.
 *
 * It matters more than it sounds: 62% of all the notes in the game's music sit
 * on the rising curve, so a player at full health is meant to be hearing about
 * a third of the piece. Rendering one flat mix would have shipped music that
 * was missing most of itself.
 *
 * ---- why this streams and sfx does not ----
 *
 * The one real structural difference from sfx.ts. Effects are decoded into
 * AudioBuffers and cached, which is right for a few hundred KB of wav. Music
 * cannot be: decoded audio is float32 at the context rate, about 23MB per
 * minute per stereo stem, and Dungeon3 is 7.3 minutes -- half a gigabyte for
 * one dungeon's three stems, for audio that is played once through.
 *
 * So each stem is an <audio> element behind a MediaElementAudioSourceNode.
 * Memory stays flat, and playback starts on the first few KB instead of after
 * a multi-megabyte fetch completes. The cost is that three elements keep three
 * independent clocks, which is what Sync below is for.
 *
 * From the console:
 *
 *   ivanMusic.stats()     track, stems, intensity, volume, drift, counters
 *   ivanMusic.playlist()  what the module last handed down
 *
 * Query string:
 *
 *   ?music=off            never play anything (still tracks what would have)
 *   ?musicbase=<url>      fetch from somewhere other than the page's own Music/
 *   ?musiccurve=linear    volume as a straight ratio instead of the GM square law
 */

import * as Query from '../platform/query.ts';

/* Must match split-stems.py's STEMS, and the order is the order they are
   mixed in -- const first because it is the one always present. */

const StemNames = ['const', 'fadeout', 'fadein'];

/* ivanconfig's scale, as sfx.ts uses. iconf.cpp:314 offers 0..127. */

const MaxVolume = 127;

/* audio.cpp:78. The game moves CurrentIntensity one step per 15ms toward the
   target rather than jumping, so a full sweep takes about 1.9 seconds. That
   slew is part of how the music behaves -- the mix opening up as a fight
   turns bad is meant to be a swell, not a switch -- so the ramps here are
   given the same duration rather than an arbitrary smoothing constant. */

const UsPerVolumeChange = 15000;

/* How long to wait for every stem to be playable before starting any of
   them. This is the thing that actually keeps them together: media elements
   begin when they individually have data, so calling play() on three at once
   starts the smallest one first. Measured in a real session before this
   barrier existed, the fade-out stem -- the largest of the three -- came in
   **166ms** behind the other two, which is a flam on every attack.

   The timeout is a fallback, not the plan: a stem that will not load must not
   hold the other two silent forever. Starting a thinner mix late beats
   starting nothing. */

const ReadyTimeoutMs = 5000;

/* One hard alignment shortly after the start, because play() on several
   elements is not sample-synchronous even once they are all ready. A seek is
   cheap here and nowhere else: nothing musical has been established yet, so
   the gap it costs lands in the first moment of the track rather than in the
   middle of a phrase. */

const AlignAfterMs = 400;
const AlignTolerance = 0.010;

/* Steady-state correction, in seconds; the rate nudge is a fraction.

   5ms is comfortably under the ~10ms where a doubled attack starts to sound
   like a flam rather than one instrument. 0.5% of playback rate is about 9
   cents of pitch, brief and only while a correction is outstanding.

   The seek threshold is what the 166ms measurement moved. It used to be
   250ms, which was far too generous to ever fire: closing 166ms at the old
   0.2% takes 83 seconds, so the nudge was not a correction, it was a
   rounding error with a counter attached. Anything past 50ms is now seeked,
   because one gap beats ten seconds of flam. */

const DriftTolerance = 0.005;
const DriftMaxRate = 0.005;
const DriftSeek = 0.050;
const DriftCheckMs = 500;

/* HAVE_FUTURE_DATA: enough buffered to start and keep going. Waiting for
   HAVE_ENOUGH_DATA instead would be stricter than needed and would hold the
   start behind a whole-file estimate the browser is free to be pessimistic
   about. */

const HaveFutureData = 3;

interface StemNode {
  Name: string;
  Media: HTMLAudioElement;
  Gain: GainNode;
  Source: MediaElementAudioSourceNode;
}

interface LoadedTrack {
  Name: string;
  Stems: StemNode[];
}

/* Music/stems.json: track filename -> the stems that were actually rendered
   for it. Taken on trust, as it was when this was a --pre-js -- it is built by
   render-stems.py and served from the same origin, and a malformed one shows up
   as a track that plays nothing, which is already a normal answer. */

type Manifest = Record<string, string[] | undefined>;

let Ctx: AudioContext | null = null;
let MusicGain: GainNode | null = null;       /* ours alone, and where the music volume lives */
let Stems: Manifest | null = null;           /* null until fetched */
let Playlist: string[] = [];                 /* filenames, in the module's order */
let Playing = false;                         /* what the module last asked for */
let Current: LoadedTrack | null = null;      /* the track now loaded, or null */
let Volume = 0;
let Intensity = 0;

/* ReturnType rather than `number`, because two tsconfigs compile this file and
   only one of them has lib.dom's timers to itself (tsconfig.test.json adds
   node's, which return an object). */

let Timer: ReturnType<typeof setInterval> | null = null;

const Counts = { started: 0, finished: 0, failed: 0, corrections: 0, seeks: 0 };

/* ---- context ----------------------------------------------------------
   Borrowed from sfx.ts so the page has one AudioContext and one autoplay
   resume path. Only if it is actually there: sfx assigns its global at the
   entry point, and a build that dropped the module would leave nothing to
   borrow. */

function Context(): AudioContext | null {
  if(Ctx)
    return Ctx;

  if(!globalThis.ivanSfx?.context)
    return null;

  const Open = globalThis.ivanSfx.context();

  if(!Open)
    return null;

  Ctx = Open;

  /* Shared with sfx -- effects pass through this node too. Its own destination
     if sfx has no gain to lend, which no build does today. */

  const Shared = globalThis.ivanSfx.master() ?? Open.destination;

  /* A node of our own between the stems and the shared master, and the music
     volume lives on it rather than on the master itself. Two reasons, and
     both are bugs if this is skipped:

       - the master is shared, so writing the music volume there would scale
         the sound effects by it as well. They have their own setting
         (SfxVolume) applied per sound in sfx.ts.
       - ivanconfig::Initialize calls audio::SetVolumeLevel long before any
         gesture has let a context exist (iconf.cpp:1343), so the volume is
         almost always set before there is anywhere to put it. Applying the
         remembered value here is what makes that first call count. */

  MusicGain = Open.createGain();
  MusicGain.gain.value = Amplitude(Volume);
  MusicGain.connect(Shared);

  return Ctx;
}

/* ---- volume -----------------------------------------------------------
   A MIDI volume of v is an amplitude of (v/127)^2: 40*log10(v/127) dB, the
   curve GM and DLS specify and the one the stems were rendered against.

   The game's own arithmetic is (IntensityVolume * MasterVolume) / 127 fed
   through that once (audio.cpp:275). Applying the curve to each of the two
   separately and multiplying gives the same number for any power law, which
   is why master can live on one shared gain node and intensity on the three
   stem nodes instead of every gain having to know both. */

function Amplitude(Level: number): number {
  const Ratio = Math.max(0, Math.min(1, Level / MaxVolume));

  return Query.Setting('musiccurve') === 'linear' ? Ratio : Ratio * Ratio;
}

/* audio.cpp:84-89, the two tables split-stems.py cut the stems on. */

function VolumeForStem(Stem: string): number {
  if(Stem === 'const')
    return MaxVolume;

  if(Stem === 'fadeout')
    return Math.max(0, MaxVolume - Intensity);

  return Math.min(MaxVolume, Intensity);
}

function ApplyIntensity(Ramp: number): void {
  const Track = Current;
  const Open = Ctx;

  if(!Track || !Open)
    return;

  for(const Stem of Track.Stems) {
    const Target = Amplitude(VolumeForStem(Stem.Name));
    const Now = Open.currentTime;

    Stem.Gain.gain.cancelScheduledValues(Now);
    Stem.Gain.gain.setValueAtTime(Stem.Gain.gain.value, Now);

    if(Ramp > 0)
      Stem.Gain.gain.linearRampToValueAtTime(Target, Now + Ramp);
    else
      Stem.Gain.gain.setValueAtTime(Target, Now);
  }
}

/* ---- loading ----------------------------------------------------------
   The manifest says which stems each track actually has, and is the reason
   the page never probes. Six of the eleven tracks are byte-identical to a
   note-free template -- the main menu, the world map, victory and defeat are
   silent in the native game too -- so "no stems" is a normal answer, and
   asking for files that were never rendered would make a genuinely broken
   deploy indistinguishable from an ordinary silent area. */

function Root(): string {
  const Base = Query.Setting('musicbase');

  return Base ? Base.replace(/\/$/, '') : './Music';
}

function LoadManifest(): Promise<Manifest> {
  if(Stems)
    return Promise.resolve(Stems);

  return fetch(Root() + '/stems.json')
    .then((Response) => {
      if(!Response.ok)
        throw new Error('HTTP ' + Response.status);

      return Response.json() as Promise<Manifest>;
    })
    .then((Parsed) => {
      Stems = Parsed;
      return Stems;
    })
    .catch((Reason: Error) => {
      /* An empty manifest is silence everywhere rather than an exception on
         every level change, and it says so once. */

      console.warn('ivan: no music manifest (' + Reason.message + ')');
      Stems = {};
      Counts.failed++;
      return Stems;
    });
}

function StemsFor(Name: string): string[] {
  const Have = Stems?.[Name];

  if(!Have)
    return [];

  return StemNames.filter((Stem) => Have.indexOf(Stem) >= 0);
}

/* ---- a loaded track ---------------------------------------------------- */

function Element(Name: string, Stem: string): HTMLAudioElement {
  const Url = Root() + '/' + Name.replace(/\.mid$/i, '') + '.' + Stem + '.ogg';
  const Media = new Audio();

  Media.src = Url;
  Media.preload = 'auto';
  Media.loop = false;

  return Media;
}

function Build(Name: string, Open: AudioContext, Out: GainNode): LoadedTrack | null {
  const Built = StemsFor(Name).map((Stem): StemNode => {
    const Media = Element(Name, Stem);
    const Gain = Open.createGain();

    /* Set before the first frame is heard rather than ramped up to, so that
       a track entered at low health opens at the right mix instead of
       sweeping into it. */

    Gain.gain.value = Amplitude(VolumeForStem(Stem));

    const Source = Open.createMediaElementSource(Media);

    Source.connect(Gain);
    Gain.connect(Out);

    return { Name: Stem, Media, Gain, Source };
  });

  return Built.length ? { Name, Stems: Built } : null;
}

function Teardown(): void {
  if(!Current)
    return;

  for(const Stem of Current.Stems) {
    Stem.Media.pause();
    Stem.Media.removeAttribute('src');
    Stem.Media.load();          /* releases the network request and buffers */
    Stem.Source.disconnect();
    Stem.Gain.disconnect();
  }

  Current = null;
}

/* Every caller wants the same thing from a rejected play(): nothing. It is
   almost always the autoplay policy rather than a fault -- sfx.ts registers the
   resume listeners on the shared context and Resume below picks the music back
   up once they fire. An older browser returns undefined instead of a promise. */

function Launch(Media: HTMLAudioElement): void {
  const Begun = Media.play();

  if(Begun && Begun.catch)
    Begun.catch(() => {});
}

/* ---- synchronisation ---------------------------------------------------
   Three media elements are three clocks, and these stems are the same piece
   of music, so any drift between them is heard as a doubled attack rather
   than as a timing error. The first stem leads and the others are pulled
   toward it by a fraction of a percent of playback rate, which is far below
   what can be heard as pitch and needs no gap in the audio.

   A seek is the fallback for the case rate cannot fix -- a stalled element
   that has fallen a quarter second behind -- and it is counted separately
   because if it ever happens routinely, this design is the wrong one. */

function Sync(): void {
  const Leader = Current && Current.Stems.length > 1 ? Current.Stems[0]?.Media : null;

  if(!Current || !Leader || Leader.paused)
    return;

  for(const Stem of Current.Stems.slice(1)) {
    if(Stem.Media.paused || !Stem.Media.duration)
      continue;

    const Drift = Stem.Media.currentTime - Leader.currentTime;

    if(Math.abs(Drift) > DriftSeek) {
      Stem.Media.currentTime = Leader.currentTime;
      Stem.Media.playbackRate = 1;
      Counts.seeks++;
      continue;
    }

    if(Math.abs(Drift) <= DriftTolerance) {
      if(Stem.Media.playbackRate !== 1)
        Stem.Media.playbackRate = 1;

      continue;
    }

    /* Behind the leader (negative drift) means play slightly faster. */
    const Nudge = Math.max(-DriftMaxRate, Math.min(DriftMaxRate, -Drift));

    Stem.Media.playbackRate = 1 + Nudge;
    Counts.corrections++;
  }
}

/* ---- playback ----------------------------------------------------------
   Track choice stays a matter for the page, and it is the same rule
   audio::Loop follows: when a track ends, pick one from the playlist at
   random -- possibly the same one again -- and play it once through
   (audio.cpp:181). It is not a shuffle and does not avoid repeats.

   The generator this replaces was deliberately private (audio.cpp:54), so
   that neither which music is installed nor when a track happens to end
   could shift the game's own RNG. Moving the choice out of the module keeps
   that property by construction: there is no longer a shared stream for it
   to draw from. */

function Pick(): string | null {
  return Playlist[Math.floor(Math.random() * Playlist.length)] ?? null;
}

function Advance(): void {
  Counts.finished++;
  Teardown();
  Begin();
}

function Begin(): void {
  /* Context() last, and only if something is actually going to play: it is
     what opens sfx's AudioContext, and ?music=off must not cost a device
     connection for audio that will never be asked for. */

  if(!Query.Enabled('music') || !Playing || Current)
    return;

  const Open = Context();

  if(!Open || !MusicGain)
    return;

  const Out = MusicGain;

  void (async () => {
    await LoadManifest();

    if(!Playing || Current)
      return;

    const Name = Pick();

    if(!Name)
      return;

    /* A track with no stems is not a failure and not worth retrying: it is
       one of the silent ones, and the game will ask again at the next level
       change. Starting a timer to poll for it would be a busy loop over an
       area that is supposed to be quiet. */

    const Track = Build(Name, Open, Out);

    if(!Track)
      return;

    Current = Track;

    Track.Stems[0]?.Media.addEventListener('ended', () => {
      if(Current === Track)
        Advance();
    });

    for(const Stem of Track.Stems)
      Stem.Media.addEventListener('error', () => {
        /* A stem listed in the manifest that will not load is a real fault,
           unlike a track with none. Say so once and let the other stems
           carry on -- a thinner mix beats silence. */

        if(Current !== Track)
          return;

        Counts.failed++;
        console.warn('ivan: no music stem ' + Track.Name + '.' + Stem.Name);
      });

    Play(Track);
  })();
}

function Ready(Stem: StemNode): Promise<void> {
  if(Stem.Media.readyState >= HaveFutureData)
    return Promise.resolve();

  /* Whichever lands first: enough data to start, a load failure, or the
     fallback. A failure resolves rather than rejects -- one stem that will not
     load must not hold the other two silent. Settling twice needs no guard,
     because resolving a settled promise is a no-op.

     The fallback is cleared on the way out rather than left to fire into a
     settled promise. It costs a browser nothing either way, but five seconds is
     long enough to hold node's event loop open well past the end of a test
     run -- measured at 2.5s of dead time across this module's suite. */

  let Fallback: ReturnType<typeof setTimeout>;

  return new Promise<void>((Resolve) => {
    const Finish = (): void => { Resolve(); };

    Stem.Media.addEventListener('canplay', Finish, { once: true });
    Stem.Media.addEventListener('error', Finish, { once: true });
    Fallback = setTimeout(Finish, ReadyTimeoutMs);
  }).finally(() => { clearTimeout(Fallback); });
}

/* One pass to put them on the same sample, once they are all running. See
   AlignAfterMs: play() is not synchronous across elements even when every
   one of them is ready, and a few milliseconds here is worth more than the
   nudge can earn back in a minute. */

function Align(Track: LoadedTrack): void {
  const Leader = Track.Stems.length > 1 ? Track.Stems[0]?.Media : null;

  if(Current !== Track || !Leader || Leader.paused)
    return;

  for(const Stem of Track.Stems.slice(1)) {
    if(Stem.Media.paused)
      continue;

    if(Math.abs(Stem.Media.currentTime - Leader.currentTime) > AlignTolerance) {
      Stem.Media.currentTime = Leader.currentTime;
      Counts.seeks++;
    }
  }
}

function Play(Track: LoadedTrack): void {
  Counts.started++;

  /* Every stem ready, then all of them started in one tick. Starting them as
     each becomes ready is what put the largest stem 166ms behind the others,
     and no amount of correction afterwards is as good as not doing it. */

  void (async () => {
    await Promise.all(Track.Stems.map(Ready));

    if(Current !== Track)
      return;

    for(const Stem of Track.Stems)
      Launch(Stem.Media);

    setTimeout(() => { Align(Track); }, AlignAfterMs);

    if(!Timer)
      Timer = setInterval(Sync, DriftCheckMs);
  })();
}

function Resume(): void {
  if(!Query.Enabled('music') || !Playing || !Ctx || Ctx.state !== 'running')
    return;

  if(!Current) {
    Begin();
    return;
  }

  for(const Stem of Current.Stems)
    if(Stem.Media.paused)
      Launch(Stem.Media);
}

function Stop(): void {
  Teardown();

  if(Timer) {
    clearInterval(Timer);
    Timer = null;
  }
}

/* ---- the module's side -------------------------------------------------
   Everything below is called from audio.cpp's __EMSCRIPTEN__ branches. */

function SetPlaylist(Names: string): void {
  const Next = Names ? Names.split(',').filter(Boolean) : [];
  const Same = Next.length === Playlist.length &&
               Next.every((Name, At) => Name === Playlist[At]);

  if(Same)
    return;

  Playlist = Next;

  /* dungeon::PrepareMusic changes the playlist on every level change and
     expects the music not to restart when the new area shares the track
     already playing (dungeon.cpp:174). It works that out from
     GetCurrentlyPlayedFile, but doing it here as well makes the rule hold
     whatever the module asks for: a track still on the list keeps playing,
     and only one that has been dropped is cut short. */

  if(Current && Playlist.indexOf(Current.Name) < 0)
    Stop();

  if(Playing)
    Begin();
}

function SetPlaying(State: boolean): void {
  const Want = !!State;

  if(Want === Playing)
    return;

  Playing = Want;

  if(Playing)
    Begin();
  else
    Stop();
}

function SetVolume(Level: number): void {
  Volume = Level;

  /* Remembered even with no context yet; Context() applies it when it makes
     the node. This is the common case, not the edge one -- the volume is
     configured at startup and the context cannot exist until a gesture. */

  if(MusicGain && Ctx)
    MusicGain.gain.setValueAtTime(Amplitude(Volume), Ctx.currentTime);
}

function SetIntensity(Level: number): void {
  const Clamped = Math.max(0, Math.min(MaxVolume, Level | 0));

  if(Clamped === Intensity)
    return;

  const Steps = Math.abs(Clamped - Intensity);

  Intensity = Clamped;
  ApplyIntensity(Steps * UsPerVolumeChange / 1e6);
}

/* The one value that has to travel back into wasm, for
   dungeon::PrepareMusic. An index into the playlist as the module last
   pushed it, rather than a string: no allocation on either side, no
   lifetime to get wrong, and it stays correct across a reorder because it
   is resolved by name at the moment it is asked for. -1 is "nothing". */

function CurrentIndex(): number {
  return Current ? Playlist.indexOf(Current.Name) : -1;
}

export const Api: IvanMusic = {
  setPlaylist: SetPlaylist,
  setPlaying: SetPlaying,
  setVolume: SetVolume,
  setIntensity: SetIntensity,
  currentIndex: CurrentIndex,
  resume: Resume,

  playlist: () => Playlist.slice(),
  stats: () => {
    const Leader = Current?.Stems[0]?.Media.currentTime;
    let Drift: number[] | null = null;

    if(Current && Current.Stems.length > 1 && Leader !== undefined)
      Drift = Current.Stems.slice(1).map(
        (S) => +(S.Media.currentTime - Leader).toFixed(4));

    return {
      track: Current ? Current.Name : null,
      stems: Current ? Current.Stems.map((S) => S.Name) : [],
      gains: Current ? Current.Stems.map((S) => +S.Gain.gain.value.toFixed(3)) : [],
      playing: Playing,
      intensity: Intensity,
      volume: Volume,
      drift: Drift,
      started: Counts.started,
      finished: Counts.finished,
      failed: Counts.failed,
      corrections: Counts.corrections,
      seeks: Counts.seeks,
      state: Ctx ? Ctx.state : 'none'
    };
  }
};

/* The autoplay listeners sfx.ts installs resume the context but know nothing
   about media elements, which stay paused after a rejected play(). One more
   listener on the same three gestures picks the music up when that happens.
   Wanted at load rather than on first use, because unlike an effect the music
   may well have been asked for before any gesture at all -- the main menu asks
   on the way in.

   Called from main.ts rather than run on import, which is the one thing the
   move out of --pre-js changed here. A --pre-js is only ever evaluated in a
   browser; a module is also imported by a test, where `document` does not
   exist, and a top-level reference to it would be a TypeError before the first
   assertion. */

export function Install(): void {
  for(const Name of ['keydown', 'mousedown', 'touchstart'])
    document.addEventListener(Name, () => { setTimeout(Resume, 0); });
}

/* Test-only, as sfx.ts's is: the module keeps its state at module scope and node
   caches the module, so one test's track and counters would otherwise be the
   next one's. Clearing Timer is the part that matters beyond tidiness -- a live
   setInterval holds node's event loop open after the last test. */

export function Reset(): void {
  Stop();
  Ctx = null;
  MusicGain = null;
  Stems = null;
  Playlist = [];
  Playing = false;
  Volume = 0;
  Intensity = 0;
  Counts.started = 0;
  Counts.finished = 0;
  Counts.failed = 0;
  Counts.corrections = 0;
  Counts.seeks = 0;
}
