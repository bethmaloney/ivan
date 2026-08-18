/*
 * The globals the page puts on the window, as the console and the browser test
 * see them.
 *
 * Deliberately loose for the two that have not moved: those are the shapes the
 * files in tools/web/ expose today, declared so a test can reach them without
 * `any`. Each one gets a real type as its module crosses into src/ and stops
 * being a --pre-js.
 *
 * ivanSfx (src/audio/sfx.ts) and ivanMusic (src/audio/music.ts) have crossed, so
 * their declarations here are now the ones the modules are checked against
 * rather than descriptions of someone else's object.
 *
 * `var` rather than `const`: it is the only declaration form that puts a name on
 * globalThis, which is where both an EM_JS body and a page.evaluate() look.
 */

interface IvanSfx {
  play(Path: string, Volume: number): void;

  /* Not console API: music borrows this context and this master rather than
     opening a second one, so both are part of the contract between them. */

  context(): AudioContext | null;
  master(): GainNode | null;

  stats(): { played: number; dropped: number; failed: number; cached: number; voices: number };
  played(): string[];
  state(): string;
}

interface IvanMusicStats {
  track: string | null;
  stems: string[];
  gains: number[];
  playing: boolean;
  intensity: number;
  volume: number;

  /* Each stem's offset from the leading one, or null when there is nothing
     playing or nothing to compare it against. The number to watch: three media
     elements keep three clocks, and drift between stems of the same piece is
     heard as a doubled attack rather than as a timing error. */

  drift: number[] | null;

  started: number;
  finished: number;
  failed: number;
  corrections: number;
  seeks: number;
  state: string;
}

interface IvanMusic {
  setPlaylist(Names: string): void;
  setPlaying(On: boolean): void;
  setVolume(Level: number): void;
  setIntensity(Level: number): void;
  currentIndex(): number;

  /* Not called from the C++, unlike the five above: it is the module's own
     autoplay listener, exposed because it is also the console's way of asking a
     silent tab to try again. */

  resume(): void;

  stats(): IvanMusicStats;
  playlist(): string[];
}

interface IvanSaves {
  stats(): {
    mounted: boolean;
    readOnly: boolean;
    dirty: boolean;
    syncs: number;
    writes: number;
    failures: number;
    lastError: string | null;
  };
  files(): unknown;
  bytes(): number;
  flush(): Promise<void>;
  wipe(): void;
}

interface IvanHarness {
  reports(): unknown[];
  text(): string;
  save(): void;
  saveRecording(): void;
  report(Note: string): void;
  clear(): void;
}

/* Not a bridge: nothing in the C++ reads it. It is what the page says about
   itself -- the build it came from, and the modules that have crossed into the
   bundle and initialised. main.ts owns it; the browser test asserts on it. */

interface IvanPage {
  build: string;
  modules: string[];
}

declare var ivanPage: IvanPage;
declare var ivanSfx: IvanSfx;
declare var ivanMusic: IvanMusic;
declare var ivanSaves: IvanSaves;
declare var ivanHarness: IvanHarness;
