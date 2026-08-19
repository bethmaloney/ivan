/*
 * The globals the page puts on the window, as the console and the browser test
 * see them.
 *
 * Deliberately loose for the one that has not moved: ivanSaves is the shape
 * tools/web/saves.js exposes today, declared so a test can reach it without
 * `any`. It gets a real type when it crosses into src/ and stops being a
 * --pre-js.
 *
 * ivanSfx, ivanMusic and ivanHarness have crossed, so their declarations here
 * are now the ones the modules are checked against rather than descriptions of
 * someone else's object.
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
  /* Substituted at build time; the other three are read from the query string
     at call time, so they cannot disagree with what the runtime was given. */

  build: string;

  endpoint(): string;
  args(): string[];
  recordingPath(): string | null;

  /* An inline import rather than a top-level one: this file is a global script,
     and importing at the top would make it a module and take every `declare var`
     below off globalThis. */

  reports(): import('../harness/report.ts').Report[];
  clear(): void;

  /* Null under ?record=off, which is the only way to have no recording. */

  text(): string | null;

  /* False when there was nothing to write, so a console user gets an answer
     rather than a silent no-op. */

  save(): boolean;
  saveRecording(): boolean;

  report(Note: string): void;
}

/* Not a bridge: nothing in the C++ reads it. It is what the page says about
   itself -- the build it came from, and the modules that have crossed into the
   bundle and initialised. main.ts owns it; the browser test asserts on it. */

interface IvanPage {
  build: string;
  modules: string[];
}

/* The Emscripten module object, as the page's own half of it sees it.
 *
 * Narrow on purpose: @types/emscripten describes a complete EmscriptenModule,
 * and every field of it web/ does not touch is a field a reader would have to
 * check against reality. shell.html puts the rest on the same object (canvas,
 * setStatus, monitorRunDependencies) and ivan.js fills in what the runtime
 * owns.
 *
 * Both members are optional for the same reason and it is not defensiveness:
 * `arguments` is unset until Install() sets it, and FS is attached by the
 * runtime, so neither exists for the window between the Module literal in
 * shell.html and main(). A crash inside that window is exactly the case a
 * report has to survive. */

interface IvanModule {
  arguments?: string[];
  onAbort?: (What: unknown) => void;
  FS?: typeof FS;
}

declare var Module: IvanModule;

declare var ivanPage: IvanPage;
declare var ivanSfx: IvanSfx;
declare var ivanMusic: IvanMusic;
declare var ivanSaves: IvanSaves;
declare var ivanHarness: IvanHarness;
