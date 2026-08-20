/*
 * Browser-side sound effects. Console API, query options and what to check when
 * it is silent are in web/README.md; the boundary argument is
 * docs/port-log.md §9.7.
 *
 * The wasm module decides *what* to play and this decides *how*: everything up
 * to and including the choice of file stays in C++, and what crosses the EM_JS
 * bridge in sfx.cpp is a path. Fire and forget, so nothing here can stall the
 * frame that asked for a sound -- and everything that can fail fails silently,
 * which is what the SDL_mixer path does with a null chunk.
 */

import * as Query from '../platform/query.ts';

/* Safari still ships only the prefixed constructor, and lib.dom does not
   declare it. */

interface PrefixedAudio {
  webkitAudioContext?: typeof AudioContext | undefined;
}

/* 16 to match the channel count sfx.cpp used to allocate, and dropping the
   sound when they are all busy is what Mix_PlayChannel's caller did too --
   the loop over channels in playSound simply falls out and returns. WebAudio
   would happily mix hundreds, but a hundred simultaneous copies of hit.wav
   is a bug that presents as a loud noise, not as an error. */

const MaxVoices = 16;

/* Volume arrives as ivanconfig's 0..127 (iconf.cpp:320). MIX_MAX_VOLUME is
   128 and the scrollbar's ceiling is 127, so 127 is full scale. */

const MaxVolume = 127;

/* And full scale is too loud, which is a property of the files rather than of
   anything here: 98 of the 155 decodable wavs under Sound/ peak within 1dB of
   0dBFS, and the ones that fire on every blow are dense as well as hot --
   blunt3.wav is -9.2dBFS RMS beneath a 0dBFS peak. Sixteen of those can sum
   here with nothing limiting them.

   The port did not make them loud and this is not a fix for the port. Native
   scales a chunk by (master * channel * chunk) / 128^2, which for
   Mix_Volume(channel, lSfxVol) is lSfxVol (SDL_mixer's mixer.c:385), and then
   by volume/128 per sample (SDL's SDL_mixer.c:84). So native plays a wav at
   127/128 where this plays it at 127/127: 0.07dB apart, and both at the level
   the file was mastered at.

   This is therefore the one place the page deliberately disagrees with the
   reference build, and it is a trim rather than a curve -- the slider stays
   linear, as SDL_mixer's is, and one node takes 12dB off everything the
   effects path plays. It hangs below the shared master rather than on it, so
   the music that same master carries keeps its own level. */

const Headroom = 0.25;

const MaxLog = 256;

/* How late a sound may arrive and still be worth playing. The first time an
   effect is used it has to be fetched and decoded, and dropping it outright
   would make the first door in every session silent -- a real cost, paid at
   exactly the moment a player is deciding whether the game has sound at all.
   Playing it late is the better trade here because IVAN is turn-based: the
   message that caused it is still on screen and nothing has moved. What this
   bound rules out is the other case, a stalled fetch that lands seconds later
   over an unrelated turn. 250ms is about fifteen frames, comfortably longer
   than a local fetch and decode and far short of noticeable drift. */

const MaxLatencyMs = 250;

/* A pending decode, a decoded buffer, or null once it has failed and been
   reported. The three states are what keeps one missing file to one console
   line per session rather than one per time the game asks for it. */

type Slot = AudioBuffer | Promise<AudioBuffer | null> | null;

let Ctx: AudioContext | null = null;
let Master: GainNode | null = null;
let Effects: GainNode | null = null;   /* the trim, and what every voice connects to */

const Buffers = new Map<string, Slot>();
let Voices = 0;
const Log: string[] = [];
const Counts = { played: 0, dropped: 0, failed: 0 };

/* ?sfxgain=0.5 to try another level by ear, ?sfxgain=1 for what native sounds
   like. Anything that is not a number in 0..1 is ignored rather than argued
   with, and a bare ?sfxgain= is a typo rather than a request for silence: a
   query string must not be a way to put a gain of 40 into the graph, or a
   trailing = a way to lose the sound and have nothing say why. */

function Trim(): number {
  const Asked = Query.Setting('sfxgain');

  if(Asked === null || Asked === '')
    return Headroom;

  const Level = Number(Asked);

  if(!Number.isFinite(Level) || Level < 0 || Level > 1)
    return Headroom;

  return Level;
}

/* ---- the audio context ------------------------------------------------
   Created on demand rather than at load, because a context created before
   any gesture starts suspended and counts against nothing useful.

   Resuming it is our job. SDL2's Emscripten backend calls emscripten's
   autoResumeAudioContext (SDL_emscriptenaudio.c:233), but that hooks the
   context *SDL* opened, and this is a different one -- SDL's is never opened
   at all now that initSound skips Mix_OpenAudio. So the same three listeners
   are registered here, once, for the context this module owns. */

export function Context(): AudioContext | null {
  if(Ctx)
    return Ctx;

  const Ctor: typeof AudioContext | undefined =
    globalThis.AudioContext ?? (globalThis as PrefixedAudio).webkitAudioContext;

  if(!Ctor)
    return null;

  const Opened = new Ctor();

  Ctx = Opened;
  Master = Opened.createGain();
  Master.connect(Opened.destination);

  Effects = Opened.createGain();
  Effects.gain.value = Trim();
  Effects.connect(Master);

  for(const Name of ['keydown', 'mousedown', 'touchstart']) {
    document.addEventListener(Name, () => {
      if(Opened.state === 'suspended')
        void Opened.resume();
    }, { once: true });
  }

  return Ctx;
}

/* ---- loading ----------------------------------------------------------
   The path the module hands over is "./Sound/name.wav" -- the same string
   the SDL_mixer branch passes to Mix_LoadWAV, and a working relative URL
   without modification, since PORTABLE_BUILD's data dir is "./". It is
   resolved against the page, so Sound/ has to be served beside ivan.html;
   the build symlinks it there (Main/CMakeLists.txt). */

function Resolve(Path: string): string {
  const Base = Query.Setting('sfxbase');

  if(!Base)
    return Path;

  return Base.replace(/\/$/, '') + '/' + Path.replace(/^.*\//, '');
}

function Load(Path: string): Slot {
  const Cached = Buffers.get(Path);

  if(Cached !== undefined)
    return Cached;

  const Pending = fetch(Resolve(Path))
    .then((Response) => {
      if(!Response.ok)
        throw new Error('HTTP ' + Response.status);

      return Response.arrayBuffer();
    })
    .then((Bytes) => {
      const Open = Context();

      if(!Open)
        throw new Error('no audio context');

      return Open.decodeAudioData(Bytes);
    })
    .then((Buffer) => {
      Buffers.set(Path, Buffer);
      return Buffer;
    })
    .catch((Reason: Error) => {
      /* Cached as a failure so one missing file is one console line for the
         session rather than one per time the game asks for it. A silent
         effect is the SDL_mixer behaviour too: Mix_LoadWAV returning null
         falls straight through playSound's `if(*sf->chunk)`. */

      Buffers.set(Path, null);
      Counts.failed++;
      console.warn('ivan: no sound for ' + Path + ' (' + Reason.message + ')');
      return null;
    });

  Buffers.set(Path, Pending);
  return Pending;
}

/* ---- playing ---------------------------------------------------------- */

function Start(Buffer: AudioBuffer | null, Volume: number): void {
  if(!Buffer || !Ctx || !Effects || Voices >= MaxVoices) {
    Counts.dropped++;
    return;
  }

  const Gain = Ctx.createGain();
  Gain.gain.value = Math.max(0, Math.min(1, Volume / MaxVolume));

  const Source = Ctx.createBufferSource();
  Source.buffer = Buffer;
  Source.connect(Gain);
  Gain.connect(Effects);

  Voices++;
  Source.onended = () => {
    Voices--;
    Gain.disconnect();
  };

  Source.start();
  Counts.played++;
}

export function Play(Path: string, Volume: number): void {
  if(Log.length >= MaxLog)
    Log.shift();

  Log.push(Path);

  if(!Query.Enabled('sfx') || !Context())
    return;

  const Buffer = Buffers.get(Path);

  /* Already decoded: play it now, on this call, with no promise in between.
     That is the common case after the first few minutes and it is worth
     keeping synchronous -- a sound scheduled a microtask late is a sound
     that drifts away from the frame that caused it. */

  if(Buffer && !(Buffer instanceof Promise)) {
    if(Ctx?.state === 'running')
      Start(Buffer, Volume);
    else
      Counts.dropped++;

    return;
  }

  if(Buffer === null) {
    Counts.dropped++;
    return;
  }

  /* Not loaded, or still loading. Play it when it lands, provided it lands
     soon enough to still belong to the turn that asked for it.

     The suspended-context check is repeated on arrival rather than only
     here, and that is the case it really exists for: currentTime does not
     advance while a context is suspended, so a sound scheduled against one
     does not play late, it waits and then fires together with every other
     queued sound the moment the autoplay policy releases it. Dropping them
     is the difference between a game that starts quietly and one that
     starts with a bang. */

  const Requested = performance.now();

  void (async () => {
    const Ready = await Load(Path);

    if(!Ready)
      return;

    if(performance.now() - Requested > MaxLatencyMs || Ctx?.state !== 'running') {
      Counts.dropped++;
      return;
    }

    Start(Ready, Volume);
  })();
}

export const Api: IvanSfx = {
  play: Play,

  /* Shared with music, which must not open a second context: browsers cap how
     many a page may have, each one costs a device connection, and two would
     need two separate resumes off the same gesture. Whichever of the two needs
     audio first creates it here, and the other joins it. */

  context: Context,
  master: () => Master,

  /* Console API, unlike master(): tuning the effects level is done by ear, and
     `ivanSfx.bus().gain.value = 0.4` is how you try one without a reload. */

  bus: () => Effects,
  stats: () => ({
    played: Counts.played,
    dropped: Counts.dropped,
    failed: Counts.failed,
    cached: Buffers.size,
    voices: Voices,

    /* What is in the graph rather than what the constant says, so a level
       changed through bus() reads back. */

    trim: Effects ? Effects.gain.value : Trim()
  }),
  played: () => Log.slice(),
  state: () => (Ctx ? Ctx.state : 'none')
};

/* Test-only: the module keeps its state at module scope, and node caches the
   module, so a second test file would otherwise inherit the first one's voices
   and counts. Not on the global API -- nothing in the page or the C++ calls it. */

export function Reset(): void {
  Ctx = null;
  Master = null;
  Effects = null;
  Buffers.clear();
  Voices = 0;
  Log.length = 0;
  Counts.played = 0;
  Counts.dropped = 0;
  Counts.failed = 0;
}
