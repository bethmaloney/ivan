/*
 * Tests for src/saves/saves.ts. Run with: npm test
 *
 * A port of tools/web/saves.test.js, which was one sequential narrative sharing
 * a single world; these are the same checks as independent cases, each building
 * its own page. What the module decides is when to copy MEMFS into IndexedDB,
 * and every one of those decisions is checkable without a browser:
 *
 *   - the module waits for the saves before main() runs, and boots anyway if
 *     IndexedDB will not answer, because a page that will not start is worse
 *     than a page that cannot save
 *   - a burst of writes is one sync, not one per file
 *   - a sync never runs while a .tmp is on disk, which is what stops a
 *     megabyte of half-written level file reaching IndexedDB (save.cpp:31)
 *   - two syncs never overlap, and a write during one is not lost
 *   - a failed sync stays dirty and says so out loud
 *   - a second tab does not write at all
 *
 * What it cannot check is that IndexedDB survives a reload. That needs a
 * browser; see docs/port-log.md §9.10 and e2e/boot.spec.ts.
 */

import { test } from 'node:test';
import { deepStrictEqual, match, ok, strictEqual } from 'node:assert';
import { posix } from 'node:path';
import { Api, Install, Reset } from './saves.ts';

const Dir = 1;
const File = 2;

/* One autosave through outputfile, at the sizes a real one has: the level file
   is the megabyte that makes syncing a .tmp expensive rather than untidy. */

const Save = '/ivan/Save/Belyer_Asu.sav';
const WorldMap = '/ivan/Save/Belyer_Asu.wm';
const Level = '/ivan/Save/Belyer_Asu.40';

interface FakeFS {
  trackingDelegate?: Record<string, ((...Args: never[]) => unknown) | undefined>;

  /* Test knobs. */
  Syncs: boolean[];
  SyncError: Error | null;
  SyncDelayMs: number;
  Overlaps: number;
  InFlight: number;
  Mounts: string[];

  mkdir(Path: string): void;
  mount(Backend: unknown, Opts: unknown, Path: string): void;
  readdir(Path: string): string[];
  stat(Path: string): { mode: number; size: number };
  isDir(Mode: number): boolean;
  unlink(Path: string): void;
  syncfs(Populate: boolean, Done: (Error_?: Error | null) => void): void;

  /* The stub's own half: what the runtime does when the game writes. */
  Write(Path: string, Size?: number): void;
  Remove(Path: string): void;
  Has(Path: string): boolean;
}

/* A filesystem just deep enough for the walk saves.ts does: it looks for a
   .tmp anywhere under the mount and it enumerates the mount to wipe it. */

function MakeFS(): FakeFS {
  const Files = new Map<string, number>();
  const Dirs = new Set(['/', '/ivan']);

  function Told(Name: string, ...Args: unknown[]): void {
    const Fn = Fs.trackingDelegate?.[Name];

    (Fn as ((...A: unknown[]) => unknown) | undefined)?.(...Args);
  }

  const Fs: FakeFS = {
    trackingDelegate: {},

    Syncs: [],
    SyncError: null,
    SyncDelayMs: 0,
    Overlaps: 0,
    InFlight: 0,
    Mounts: [],

    mkdir(Path) { Dirs.add(Path); },
    mount(_Backend, _Opts, Path) { Fs.Mounts.push(Path); },

    readdir(Path) {
      if(!Dirs.has(Path))
        throw new Error('ENOENT ' + Path);

      const Kids: string[] = [];

      function Add(Full: string): void {
        if(posix.dirname(Full) === Path)
          Kids.push(posix.basename(Full));
      }

      for(const D of Dirs)
        if(D !== Path)
          Add(D);

      for(const F of Files.keys())
        Add(F);

      return ['.', '..', ...Kids];
    },

    stat(Path) {
      if(Dirs.has(Path))
        return { mode: Dir, size: 0 };

      const Size = Files.get(Path);

      if(Size === undefined)
        throw new Error('ENOENT ' + Path);

      return { mode: File, size: Size };
    },

    isDir: (Mode) => Mode === Dir,

    unlink(Path) {
      Files.delete(Path);
      Told('onDeletePath', Path);
    },

    syncfs(Populate, Done) {
      Fs.Syncs.push(Populate);
      Fs.InFlight++;

      if(Fs.InFlight > 1)
        Fs.Overlaps++;

      setTimeout(() => {
        Fs.InFlight--;
        Done(Fs.SyncError);
      }, Fs.SyncDelayMs);
    },

    /* The bytes land, then the delegate is told (libfs.js:1375). */

    Write(Path, Size) {
      const Parent = posix.dirname(Path);
      const Bytes = Size ?? 1024;

      if(!Dirs.has(Parent)) {
        Dirs.add(Parent);
        Told('onMakeDirectory', Parent);
      }

      Files.set(Path, Bytes);
      Told('onWriteToFile', Path, Bytes);
    },

    Remove(Path) { Fs.unlink(Path); },
    Has: (Path) => Files.has(Path)
  };

  return Fs;
}

/* The three handlers saves.ts sets on a deleteDatabase request, and nothing
   else of IDBOpenDBRequest: a Partial of the real one cannot be called through,
   because every handler's `this` is the complete interface. */

interface FakeRequest {
  onsuccess?: () => void;
  onerror?: () => void;
  onblocked?: () => void;
  error?: unknown;
}

interface Fake {
  Search?: string;

  /* A module linked without -sFS_DEBUG has no trackingDelegate at all
     (libfs.js:61), which is how this first reached a browser. */

  NoTracking?: boolean;

  NoLocks?: boolean;
  TabTaken?: boolean;
  WipeBlocked?: boolean;

  /* No run dependency exported at all: the page still saves, it just cannot
     promise the menu waited. */

  NoDependency?: boolean;
}

interface World {
  Fs: FakeFS;
  Announced: { kind: string; detail: string }[];
  Deleted: string[];
  Dependencies: string[];
  Errors: string[];
  Warnings: string[];
  Persists: number;
  Reloads: number;
  Hidden(): void;
}

function Define(Name: string, Value: unknown): void {
  Object.defineProperty(globalThis, Name, { configurable: true, value: Value });
}

/* One page, built fresh per test. The module keeps its state at module scope
   and node caches the module, so Reset() is what stops one case's mount,
   counters and debounce timer leaking into the next. */

function Page(Options: Fake = {}): World {
  Reset();

  const Fs = MakeFS();

  if(Options.NoTracking)
    delete Fs.trackingDelegate;

  const Held: string[] = [];

  const State: World = {
    Fs,
    Announced: [],
    Deleted: [],
    Dependencies: Held,
    Errors: [],
    Warnings: [],
    Persists: 0,
    Reloads: 0,
    Hidden: () => {}
  };

  Define('Module', {
    FS: Fs,
    IDBFS: { name: 'IDBFS' },

    addRunDependency: Options.NoDependency ? undefined : (Name: string) => {
      Held.push(Name);
    },
    removeRunDependency: Options.NoDependency ? undefined : (Name: string) => {
      Held.splice(Held.indexOf(Name), 1);
    }
  });

  Define('location', {
    search: Options.Search ?? '',
    reload: () => { State.Reloads++; }
  });

  Define('dispatchEvent', (Event_: CustomEvent<{ kind: string; detail: string }>) => {
    State.Announced.push(Event_.detail);
  });

  /* node defines navigator as a getter-only global, so it has to be redefined
     rather than assigned. */

  Define('navigator', {
    storage: {
      persist: () => { State.Persists++; return Promise.resolve(true); },
      estimate: () => Promise.resolve({ usage: 3670016, quota: 1073741824 })
    },

    locks: Options.NoLocks ? undefined : {
      request: (Name: string, _Opts: unknown, Body: (Lock: unknown) => unknown) =>
        Promise.resolve(Body(Options.TabTaken ? null : { name: Name }))
    }
  });

  Define('indexedDB', {
    deleteDatabase: (Name: string) => {
      State.Deleted.push(Name);

      const Request: FakeRequest = {};

      setTimeout(() => {
        if(Options.WipeBlocked)
          Request.onblocked?.();
        else
          Request.onsuccess?.();
      }, 0);

      return Request;
    }
  });

  Define('document', {
    visibilityState: 'visible',
    addEventListener: (_Name: string, Fn: () => void) => {
      State.Hidden = () => {
        Define('document', { ...document, visibilityState: 'hidden' });
        Fn();
      };
    }
  });

  console.error = (...Args: unknown[]) => void State.Errors.push(Args.join(' '));
  console.warn = (...Args: unknown[]) => void State.Warnings.push(Args.join(' '));

  Install();

  return State;
}

function Tick(Ms = 0): Promise<void> {
  return new Promise((Resolve) => { setTimeout(Resolve, Ms); });
}

/* Longer than the module's 200ms debounce, short enough to keep the suite
   quick. Not a constant it exports: the test asserting the behaviour rather
   than the number is the point, and a debounce this test had to be told about
   would be a debounce it could not catch a change to. */

function AfterDebounce(): Promise<void> {
  return Tick(320);
}

/* preRun is where the runtime calls what Install() registered. Two microtask
   turns is what the wipe-then-claim-then-mount chain needs before the syncfs
   callback has run. */

async function Booted(): Promise<void> {
  const Hooks = Module.preRun;

  ok(Array.isArray(Hooks) && Hooks.length === 1, 'one preRun hook is registered');

  Hooks[0]?.();

  await Tick();
  await Tick();
}

/* ---- boot -------------------------------------------------------------- */

test('nothing is mounted until the runtime calls preRun', () => {
  const State = Page();

  strictEqual(Api.stats().mounted, false);
  deepStrictEqual(State.Fs.Mounts, []);
});

test('startup waits for the saves, and is released once they are in', async () => {
  const State = Page();
  const Hooks = Module.preRun as (() => void)[];

  Hooks[0]?.();

  deepStrictEqual(State.Dependencies, ['ivan-saves'],
                  'main() must not reach the Continue menu first');

  await Tick();
  await Tick();

  deepStrictEqual(State.Fs.Mounts, ['/ivan'], 'the mount is where save.cpp looks');
  deepStrictEqual(State.Fs.Syncs, [true], 'IndexedDB is read, not written');
  deepStrictEqual(State.Dependencies, []);
  strictEqual(Api.stats().mounted, true);
  strictEqual(Api.stats().readOnly, false);
  ok(State.Persists > 0, 'storage is asked to be persistent');
});

/* The module scope FS this used to fall back on only existed while the file was
   pasted into ivan.js. From the bundle there is one source for both. */

test('a module with no FS on it turns saving off rather than the page', async () => {
  const State = Page();
  const Hook = (Module.preRun as (() => void)[])[0];

  /* Taken before the module is replaced, because replacing it is the scenario:
     a runtime that reached preRun without the runtime methods the link flags
     name. */

  Define('Module', { addRunDependency: () => {}, removeRunDependency: () => {} });

  Hook?.();

  await Tick();

  strictEqual(Api.stats().readOnly, true);
  strictEqual(Api.stats().mounted, false);
  match(State.Errors.join('\n'), /FS or IDBFS missing/);
});

/* ---- a save ------------------------------------------------------------ */

test('a burst of writes is one sync', async () => {
  const State = Page();

  await Booted();

  /* One autosave is eight files through outputfile, and the whole point of the
     debounce is that this is one write to IndexedDB rather than eight. */

  State.Fs.Write(Save, 171560);
  State.Fs.Write(WorldMap, 64769);
  State.Fs.Write(Level, 892168);

  strictEqual(State.Fs.Syncs.length, 1, 'a write does not sync immediately');
  strictEqual(Api.stats().dirty, true, 'but is remembered');

  await AfterDebounce();

  deepStrictEqual(State.Fs.Syncs, [true, false]);
  strictEqual(Api.stats().dirty, false, 'the mount is clean afterwards');
  strictEqual(Api.stats().syncs, 1);
  strictEqual(Api.stats().failures, 0);
});

test('writes outside the mount are ignored', async () => {
  const State = Page();

  await Booted();

  /* The recording src/harness reads lives at /session.rec, and Graphics/ and
     Script/ are at the MEMFS root. None of it belongs in IndexedDB. */

  State.Fs.Write('/session.rec', 400);
  State.Fs.Write('/Graphics/Char.png', 1000);

  await AfterDebounce();

  strictEqual(State.Fs.Syncs.length, 1);
  strictEqual(Api.stats().dirty, false);
});

test('a save in progress defers the sync until the .tmp is gone', async () => {
  const State = Page();

  await Booted();

  /* outputfile opens <name>.tmp, fills it, copies it over the final name and
     removes it. Syncing in the middle would put the .tmp in IndexedDB and take
     it out again on the next pass -- a megabyte each way for a file that is
     never read. */

  State.Fs.Write(Level + '.tmp', 892168);

  await AfterDebounce();

  strictEqual(State.Fs.Syncs.length, 1, 'a .tmp defers the sync');
  ok(Api.stats().tempDeferrals > 0, 'and says why');
  strictEqual(Api.stats().dirty, true, 'while staying dirty');

  State.Fs.Remove(Level + '.tmp');

  await Tick(600);

  strictEqual(State.Fs.Syncs.length, 2, 'and syncs once the save is finished');
  strictEqual(State.Fs.Has(Level + '.tmp'), false,
              'the .tmp never reached IndexedDB');
});

test('a stale .tmp cannot wedge saving for the rest of the session', async () => {
  const State = Page();

  await Booted();

  /* A crash between opening a .tmp and removing it leaves one on disk forever.
     Ten deferrals at 400ms, and then the save goes through: syncing a stale
     temporary file is better than syncing nothing again. */

  State.Fs.Write(Level + '.tmp', 892168);
  State.Fs.Write(Save, 171560);

  await Tick(4600);

  strictEqual(Api.stats().tempDeferrals, 10);
  strictEqual(State.Fs.Syncs.length, 2, 'the save is written in the end');
});

/* ---- overlapping ------------------------------------------------------- */

test('a write during a sync queues rather than starting a second one', async () => {
  const State = Page();

  await Booted();

  /* IDBFS reconciles the whole mount against the whole database, so two of
     those at once would race over the same records. A write arriving mid-sync
     has to queue rather than start a second one -- and must not be dropped. */

  State.Fs.SyncDelayMs = 300;
  State.Fs.Write(Save, 171999);

  await AfterDebounce();

  strictEqual(Api.stats().syncing, true, 'a sync is in flight');

  /* Written and flushed while the first sync is still running, which is the
     collision itself rather than a write that merely happens to be nearby --
     the debounce would otherwise let the first one finish first and there
     would be nothing to queue. */

  const Collided = State.Fs.Syncs.length;

  State.Fs.Write(Level, 892999);

  const Queued = Api.flush();

  strictEqual(State.Fs.Syncs.length, Collided, 'the second does not start');

  await Queued;

  strictEqual(State.Fs.Syncs.length, Collided + 1, 'it runs after the first');
  strictEqual(State.Fs.Overlaps, 0, 'and no two syncs overlapped');
  strictEqual(Api.stats().dirty, false, 'with nothing left dirty');
});

/* ---- deletion ---------------------------------------------------------- */

test('a deletion syncs like a write', async () => {
  const State = Page();

  await Booted();

  State.Fs.Write(Save, 171560);

  await AfterDebounce();

  const Before = State.Fs.Syncs.length;

  /* The save set is removed when the player dies (game.cpp:3843). A deletion
     that never reached IndexedDB would put the dead character back on the
     Continue menu after a reload, which is worse than losing the save. */

  State.Fs.Remove(Save);

  await AfterDebounce();

  strictEqual(State.Fs.Syncs.length, Before + 1);
});

/* ---- flush ------------------------------------------------------------- */

test('flush does not wait for the debounce, and resolves after the sync', async () => {
  const State = Page();

  await Booted();

  const Before = State.Fs.Syncs.length;

  State.Fs.Write('/ivan/ivan.cfg', 900);

  await Api.flush();

  strictEqual(State.Fs.Syncs.length, Before + 1);
  strictEqual(Api.stats().syncing, false);
});

test('hiding the tab syncs rather than letting the debounce run out', async () => {
  const State = Page();

  await Booted();

  const Before = State.Fs.Syncs.length;

  State.Fs.Write(Save, 171560);
  State.Hidden();

  await Tick();

  strictEqual(State.Fs.Syncs.length, Before + 1);
});

/* ---- failure ----------------------------------------------------------- */

test('a failed sync is counted, reported and left dirty', async () => {
  const State = Page();

  await Booted();

  /* IDBFS's own autoPersist discards this error, which is how a full disk
     becomes a game that quietly stops saving. */

  State.Fs.SyncError = new Error('QuotaExceededError');
  State.Fs.Write(Level, 892168);

  await AfterDebounce();

  strictEqual(Api.stats().failures, 1);
  ok(Api.stats().lastError !== null, 'and reported');
  strictEqual(State.Announced.at(-1)?.kind, 'error', 'and told to the page');
  strictEqual(Api.stats().dirty, true, 'stays dirty so the next write retries');

  State.Fs.SyncError = null;
  State.Fs.Write(Level, 892168);

  await AfterDebounce();

  strictEqual(Api.stats().lastError, null, 'recovering clears the error');
});

/* ---- wipe -------------------------------------------------------------- */

test('wipe empties the mount and pushes the deletions through', async () => {
  const State = Page();

  await Booted();

  State.Fs.Write(Save, 171560);
  State.Fs.Write(Level, 892168);

  await AfterDebounce();
  await Api.wipe(false);

  deepStrictEqual(Api.files(), []);
  strictEqual(Api.bytes(), 0);
  strictEqual(Api.stats().dirty, false);
  strictEqual(State.Reloads, 0, 'told not to reload');
});

test('files() and bytes() report what is in the mount', async () => {
  const State = Page();

  await Booted();

  State.Fs.Write(Save, 171560);
  State.Fs.Write(Level, 892168);

  deepStrictEqual(Api.files().map((F) => F.path).sort(), [Level, Save]);
  strictEqual(Api.bytes(), 171560 + 892168);
});

/* ---- a second tab ------------------------------------------------------ */

test('a second tab reads the saves but never writes them', async () => {
  const State = Page({ TabTaken: true });

  await Booted();

  /* Two tabs share one database and hold separate copies of MEMFS, so the
     second one to sync overwrites the first one's saves wholesale. */

  deepStrictEqual(State.Fs.Syncs, [true], 'it still reads them');
  strictEqual(Api.stats().readOnly, true);
  strictEqual(State.Announced[0]?.kind, 'readonly', 'and warns the page');

  State.Fs.Write(Save, 171560);

  await AfterDebounce();

  deepStrictEqual(State.Fs.Syncs, [true], 'nothing it does reaches IndexedDB');
  strictEqual(Api.stats().dirty, false, 'it does not even accumulate dirt');
});

test('no Web Locks API is not a reason to stop saving', async () => {
  Page({ NoLocks: true });

  await Booted();

  strictEqual(Api.stats().readOnly, false);
});

/* ---- ?wipesaves -------------------------------------------------------- */

test('?wipesaves deletes the database before the mount', async () => {
  const State = Page({ Search: '?wipesaves' });

  /* The escape hatch for a save the game cannot load, which is the failure
     persistence introduces: without it, one bad save bricks the tab forever
     and the console API that would fix it is behind a menu that never draws. */

  await Booted();

  deepStrictEqual(State.Deleted, ['/ivan']);
  strictEqual(Api.stats().mounted, true, 'and the game still boots');
  deepStrictEqual(State.Dependencies, [], 'with startup released');
  strictEqual(Api.stats().failures, 0, 'and no complaint');
});

test('a blocked wipe is not reported as a wipe', async () => {
  Page({ Search: '?wipesaves', WipeBlocked: true });

  /* IndexedDB queues a delete that another connection is holding open and
     fires onblocked instead. Reporting that as done sends the player straight
     back into the save they were trying to escape. */

  await Booted();

  strictEqual(Api.stats().failures, 1);
  match(Api.stats().lastError ?? '', /another tab/, 'says which tab to close');
  strictEqual(Api.stats().mounted, true, 'while still booting');
});

/* ---- ?saves=off -------------------------------------------------------- */

test('?saves=off registers no hook and says it is read-only', () => {
  Page({ Search: '?saves=off' });

  strictEqual(Module.preRun, undefined);
  strictEqual(Api.stats().readOnly, true);
});

/* ---- booting anyway ---------------------------------------------------- */

test('a module that cannot be watched still boots', async () => {
  const State = Page({ NoTracking: true });

  /* The run dependency is held across a callback IndexedDB invokes, so a throw
     in there escapes every promise handler around it and startup is never
     released -- a page frozen on the loading bar because of a save feature.
     This is the shape of the first failure this hit in a real browser. */

  await Booted();

  deepStrictEqual(State.Dependencies, []);
  strictEqual(Api.stats().readOnly, true, 'with saving off rather than the page');
  strictEqual(Api.stats().failures, 1, 'and the reason said out loud');
});

test('a refused database still releases startup', async () => {
  const State = Page();

  /* Private windows and blocked third-party storage both look like this. The
     game has to start anyway. */

  State.Fs.SyncError = new Error('SecurityError');

  await Booted();

  deepStrictEqual(State.Dependencies, []);
  strictEqual(Api.stats().readOnly, true, 'saving off rather than the game');
  strictEqual(Api.stats().failures, 1, 'loudly');
});

test('a runtime with no run dependency to hold still mounts, and says so', async () => {
  const State = Page({ NoDependency: true });

  /* Exporting the pair is a link flag, and a build that lost it would otherwise
     fail as an empty Continue menu over a save that was about to arrive --
     a bug that reads as data loss. */

  await Booted();

  strictEqual(Api.stats().mounted, true);
  strictEqual(Api.stats().readOnly, false);
  match(State.Warnings.join('\n'), /no run dependency/);
});
