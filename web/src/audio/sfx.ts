/*
 * Browser-side sound effects (HARNESS.md §9.7).
 *
 * The wasm module decides *what* to play and this decides *how*. Everything up
 * to and including the choice of file stays in C++ -- Sound/SoundEffects.cfg,
 * its 153 patterns, the regex match against the message text, and the private
 * xorshift that picks between several files for one pattern (sfx.cpp:277). What
 * crosses the boundary is a path, through the EM_JS bridge in sfx.cpp.
 *
 * The point of the split is not tidiness, it is the three things SDL_mixer
 * could not do on this target:
 *
 *   - Nothing is preloaded. Sound/ is 26MB of wav against the 3.3MB of
 *     Graphics/ and Script/ in ivan.data, so preloading it would have made the
 *     first load an order of magnitude slower for audio that may never play.
 *     Each file is fetched the first time it is asked for and cached by the
 *     browser thereafter, so the cost is a few KB at the moment of use.
 *   - Latency is a buffer, not a mixer chunk. Mix_OpenAudio's 8000-sample
 *     request rounds up to 8192 (SDL_audio.c:1431), about 186ms, which is a
 *     long time between a blow landing and the sound of it. WebAudio schedules
 *     on the sample.
 *   - The autoplay policy is handled where it lives. See below.
 *
 * From the console:
 *
 *   ivanSfx.stats()     counts: played, dropped, cached, failed
 *   ivanSfx.played()    the last few hundred paths, newest last
 *   ivanSfx.state()     AudioContext state, or 'none' before the first sound
 *
 * Query string:
 *
 *   ?sfx=off            never play anything (still records what would have)
 *   ?sfxbase=<url>      fetch from somewhere other than the page's own Sound/
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

const Buffers = new Map<string, Slot>();
let Voices = 0;
const Log: string[] = [];
const Counts = { played: 0, dropped: 0, failed: 0 };

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
  if(!Buffer || !Ctx || !Master || Voices >= MaxVoices) {
    Counts.dropped++;
    return;
  }

  const Gain = Ctx.createGain();
  Gain.gain.value = Math.max(0, Math.min(1, Volume / MaxVolume));

  const Source = Ctx.createBufferSource();
  Source.buffer = Buffer;
  Source.connect(Gain);
  Gain.connect(Master);

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
  stats: () => ({
    played: Counts.played,
    dropped: Counts.dropped,
    failed: Counts.failed,
    cached: Buffers.size,
    voices: Voices
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
  Buffers.clear();
  Voices = 0;
  Log.length = 0;
  Counts.played = 0;
  Counts.dropped = 0;
  Counts.failed = 0;
}
