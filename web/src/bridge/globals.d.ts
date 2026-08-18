/*
 * The globals the page puts on the window, as the console and the browser test
 * see them.
 *
 * Deliberately loose for now: these are the shapes the four files in tools/web/
 * expose today, declared so a test can reach them without `any`. Each one gets
 * a real type as its module crosses into src/ and stops being a --pre-js.
 *
 * `var` rather than `const`: it is the only declaration form that puts a name on
 * globalThis, which is where both an EM_JS body and a page.evaluate() look.
 */

interface IvanSfx {
  play(Path: string, Volume: number): void;
  stats(): { played: number; dropped: number; failed: number; cached: number; voices: number };
  played(): string[];
  state(): string;
}

interface IvanMusic {
  setPlaylist(Names: string): void;
  setPlaying(On: boolean): void;
  setVolume(Level: number): void;
  setIntensity(Level: number): void;
  currentIndex(): number;
  stats(): Record<string, unknown>;
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

declare var ivanSfx: IvanSfx;
declare var ivanMusic: IvanMusic;
declare var ivanSaves: IvanSaves;
declare var ivanHarness: IvanHarness;
