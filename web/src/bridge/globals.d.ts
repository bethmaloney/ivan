/*
 * The globals the page puts on the window, as the console and the browser test
 * see them.
 *
 * Each declaration is the type its own module is checked against, not a
 * description of someone else's object.
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

interface IvanSavesStats {
  mounted: boolean;
  readOnly: boolean;
  dirty: boolean;
  syncing: boolean;
  syncs: number;
  writes: number;
  failures: number;

  /* Syncs refused because a .tmp was on disk. Not a fault count: outputfile
     leaves one there for the length of a save, so deferring is the module
     working (save.cpp:31). */

  tempDeferrals: number;

  /* Reading IndexedDB back at startup, and one sync afterwards. The first is
     paid on every load and grows with the save set; the second does not. */

  populateMs: number;
  lastSyncMs: number;
  lastError: string | null;
}

interface IvanSaves {
  /* Both the mount point and the IndexedDB database name -- IDBFS keys its
     database on the mountpoint, so they cannot differ. */

  mount: string;

  stats(): IvanSavesStats;
  files(): { path: string; bytes: number }[];
  bytes(): number;

  /* Resolves with whatever syncfs reported, so null is the success case. */

  flush(): Promise<unknown>;

  estimate(): Promise<{ usageMB: number; quotaMB: number; persisted: null } | null>;

  /* Reloads afterwards unless told not to: the running game still holds the
     state it just had wiped and would write it straight back. */

  wipe(Reload?: boolean): Promise<void>;
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

  /* shell.html's, not the runtime's: the high-water mark of what startup was
     ever waiting for. Read by the browser test, which is the only assertion
     anywhere that the saves actually held main() back. */

  totalDependencies: number;

  onAbort?: (What: unknown) => void;
  preRun?: (() => void)[] | (() => void);
  FS?: typeof FS;
  IDBFS?: Emscripten.FileSystemType;

  /* Optional for the same reason FS is, and it is the whole of why saves.js
     was the last --pre-js: these two are module-scope in ivan.js and reach the
     page only as properties. FORCE_FILESYSTEM exports them and --preload-file
     turns that on, but the link flags name them outright rather than inherit
     them. They exist by the time preRun runs, which is the only moment saves
     asks for them. */

  addRunDependency?: (Id: string) => void;
  removeRunDependency?: (Id: string) => void;
}

declare var Module: IvanModule;

declare var ivanPage: IvanPage;
declare var ivanSfx: IvanSfx;
declare var ivanMusic: IvanMusic;
declare var ivanSaves: IvanSaves;
declare var ivanHarness: IvanHarness;
