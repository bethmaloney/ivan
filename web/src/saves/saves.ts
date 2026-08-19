/*
 * Saves that survive the tab. Console API, query options and what to check when
 * they do not persist are in web/README.md; the design argument, including why
 * IndexedDB rather than localStorage, is docs/port-log.md §9.10.
 *
 * The game writes saves with fopen and knows nothing about any of this. All that
 * changes for it is where GetUserDataDir() points: /ivan/ on this target rather
 * than "./" (save.cpp), and /ivan is an IDBFS mount. IDBFS keeps a MEMFS in
 * front of IndexedDB and copies between them on demand, so the two jobs here
 * are: read it in before main() runs, and write it back after the game has
 * written a save.
 *
 * WRITING BACK IS DEBOUNCED, AND NOT ONLY TO COALESCE. outputfile writes to
 * <name>.tmp and copies it over the final name on close (save.cpp:31), so a
 * sync taken mid-save would push a megabyte of temporary file into IndexedDB
 * and delete it again on the next pass. Flush waits for the writes to stop and
 * refuses outright while a .tmp is on disk.
 *
 * The wait costs nothing, because of how the module runs: the game blocks inside
 * wasm and only returns to the JS event loop when asyncify unwinds it at the
 * input wait. A timer therefore cannot fire until the game is idle, which is
 * exactly when a sync should happen.
 */

import * as Query from '../platform/query.ts';

/* Both the mount point and the IndexedDB database name: IDBFS keys its
   database on the mount path (libidbfs.js, getDB(mount.mountpoint)). Change
   one and you have changed the other, and every existing save is orphaned. */

const Mount = '/ivan';

/* Only files under here are the player's. A .tmp is a save in progress and
   nothing else in the mount ends that way. */

const Temp = '.tmp';

/* Long enough to let a burst of writes finish, short enough that the gap
   between "the game saved" and "the browser has it" is not a window a player
   could close the tab in. A level change writes two save sets seconds apart
   and gets two syncs, which is correct rather than wasteful -- the first one
   is a save worth keeping on its own. */

const DebounceMs = 200;

/* A sync that finds a .tmp waits this long and asks again. The bound exists
   so a crash between opening a .tmp and removing it cannot wedge saving for
   the rest of the session: after this many attempts the .tmp is stale and
   syncing it is better than syncing nothing. */

const TempRetryMs = 400;
const MaxTempRetries = 10;

/* Two things are taken under this name and deliberately the same one: the run
   dependency startup is held by, and the Web Lock that gives a single tab the
   right to write. Emscripten's own file packager holds a dependency the same
   way for ivan.data (file_packager.py:992), so this is the mechanism startup
   already runs on rather than a second one bolted beside it. */

const Tag = 'ivan-saves';

let Mounted = false;
let ReadOnly = false;          /* another tab holds the lock, or the mount failed */
let Dirty = false;
let Timer: ReturnType<typeof setTimeout> | null = null;
let Syncing = false;
let Again = false;
let TempWaits = 0;
let Waiters: ((Error_: unknown) => void)[] = [];

const Counts = { syncs: 0, writes: 0, failures: 0, tempDeferrals: 0 };
let LastError: string | null = null;
let LastSyncMs = 0;

/* How long the module was held at startup reading IndexedDB back. The number
   that decides whether this scales: it is paid on every load and it grows
   with the size of the save set, where the sync after a save does not. */

let PopulateMs = 0;

/* ---- talking to the page ---------------------------------------------
   This decides, shell.html displays, the same split the sfx module and the
   mute button use. Nothing here touches the DOM. */

function Announce(Kind: string, Detail?: unknown): void {
  try {
    dispatchEvent(new CustomEvent('ivan-saves', {
      detail: { kind: Kind, detail: String(Detail ?? '') }
    }));
  } catch {
    /* An event that cannot be dispatched must not take the save with it. */
  }
}

function Fail(What: string, Error_: unknown): void {
  Counts.failures++;
  LastError = What + ': ' + String(Error_);
  console.error('ivanSaves: ' + LastError);
  Announce('error', What);
}

function Said(Error_: unknown): string {
  return String((Error_ instanceof Error ? Error_.message : null) ?? Error_);
}

/* ---- the filesystem ---------------------------------------------------
   Module.FS and Module.IDBFS, and nothing else. Both are named by
   -sEXPORTED_RUNTIME_METHODS; the bare module-scope FS this used to fall back
   on was reachable only while this file was a --pre-js pasted into ivan.js's
   own scope, and from the bundle it never existed. Taking neither still has to
   be survivable, because the alternative is a page that will not boot over a
   save feature. */

function Filesystem(): typeof FS | null {
  return Module.FS ?? null;
}

function Backend(): Emscripten.FileSystemType | null {
  return Module.IDBFS ?? null;
}

function Walk(Path: string, Each: (Path: string, Size: number) => void): void {
  const Fs = Filesystem();

  if(!Fs)
    return;

  let Names: string[];

  try {
    Names = Fs.readdir(Path);
  } catch {
    return;
  }

  for(const Name of Names) {
    if(Name === '.' || Name === '..')
      continue;

    const Full = Path + '/' + Name;
    let Stat: FS.Stats;

    try {
      Stat = Fs.stat(Full);
    } catch {
      continue;
    }

    if(Fs.isDir(Stat.mode))
      Walk(Full, Each);
    else
      Each(Full, Stat.size);
  }
}

function HasTemp(): boolean {
  let Found = false;

  Walk(Mount, (Path) => {
    if(Path.slice(-Temp.length) === Temp)
      Found = true;
  });

  return Found;
}

/* ---- syncing ----------------------------------------------------------
   One sync at a time. A second request arriving mid-flight is remembered
   rather than run, because IDBFS reconciles the whole mount against the
   whole database and two of those interleaved would race over the same
   records. */

function Settle(Error_: unknown): void {
  const Pending = Waiters;

  Waiters = [];

  for(const Resolve of Pending)
    Resolve(Error_ ?? null);
}

function Sync(): void {
  if(Syncing) {
    Again = true;
    return;
  }

  const Fs = Filesystem();

  if(!Mounted || ReadOnly || !Fs) {
    Settle(null);
    return;
  }

  /* Never persist a half-written save. */

  if(HasTemp() && TempWaits < MaxTempRetries) {
    TempWaits++;
    Counts.tempDeferrals++;
    Schedule(TempRetryMs);
    return;
  }

  TempWaits = 0;
  Syncing = true;
  Dirty = false;

  const Began = Date.now();

  Fs.syncfs(false, (Error_: unknown) => {
    Syncing = false;
    LastSyncMs = Date.now() - Began;

    if(Error_) {
      /* Put the dirt back: the files are still only in MEMFS, and the next
         write must try again rather than assume this one landed. */
      Dirty = true;
      Fail('writing saves to IndexedDB', Said(Error_));
    } else {
      Counts.syncs++;
      LastError = null;
    }

    if(Again) {
      Again = false;
      Sync();
      return;
    }

    Settle(Error_);
  });
}

function Schedule(Delay: number): void {
  if(Timer !== null)
    clearTimeout(Timer);

  Timer = setTimeout(() => {
    Timer = null;
    Sync();
  }, Delay);
}

function Touched(Path: string): void {
  if(!Mounted || ReadOnly)
    return;

  if(String(Path).lastIndexOf(Mount + '/', 0) !== 0)
    return;

  Counts.writes++;
  Dirty = true;
  Schedule(DebounceMs);
}

/* Every way the mount can change. Deletions matter as much as writes: the
   game removes the save set when the player dies (game.cpp:3843), and a
   deletion that never reached IndexedDB would put a dead character back on
   the Continue menu after a reload. */

function Track(): void {
  const Fs = Filesystem();

  /* Typed as always present and is not: trackingDelegate only exists when the
     module was linked -sFS_DEBUG=1, which the browser target sets. A build
     without it would sync the saves that happen to be in MEMFS at boot and
     nothing after, which is worse than not saving, because it looks like it is
     working. */

  const Delegate = Fs?.trackingDelegate as Partial<typeof FS.trackingDelegate>
                                           | undefined;

  if(!Delegate) {
    ReadOnly = true;
    Fail('watching for saves', 'FS.trackingDelegate missing (link -sFS_DEBUG=1)');
    return;
  }

  Delegate.onWriteToFile = Touched;
  Delegate.onDeletePath = Touched;
  Delegate.onMakeDirectory = Touched;
  Delegate.onMovePath = (From: string, To: string) => { Touched(From); Touched(To); };
}

/* ---- one tab at a time ------------------------------------------------
   Two tabs on one origin share one database and each holds its own MEMFS, so
   whichever syncs last overwrites the other's saves wholesale. The lock is
   held for the life of the page; a tab that cannot get it plays with syncing
   switched off and says so, which loses that session's progress but not the
   run already stored. */

function Claim(): Promise<boolean> {
  if(!navigator.locks?.request)
    return Promise.resolve(true);

  return new Promise((Resolve) => {
    navigator.locks.request(Tag, { ifAvailable: true }, (Lock) => {
      Resolve(!!Lock);

      if(!Lock)
        return;

      /* Never resolves, so the lock is released only when the tab goes. */
      return new Promise<void>(() => {});
    }).catch(() => { Resolve(true); });
  });
}

/* ---- boot -------------------------------------------------------------
   A run dependency, so the module waits. Without it main() reaches the menu
   and iosystem::ContinueMenu enumerates a Save/ that IndexedDB has not been
   copied into yet -- "You don't have any previous saves." over a save that
   was about to arrive.

   Module.addRunDependency rather than the bare name this called while it was a
   --pre-js. It is the same function: FORCE_FILESYSTEM exports both halves of
   the pair (link.py:1637), and --preload-file turns that on, so the data
   package's own loader holds ivan.data back through exactly this property. The
   flag names them anyway rather than inheriting them from a file we happen to
   preload. */

export function Boot(): void {
  const Fs = Filesystem();
  const Backing = Backend();

  if(!Fs || !Backing) {
    ReadOnly = true;
    Fail('mounting', 'FS or IDBFS missing from the module');
    return;
  }

  /* A page that cannot hold main() back can still save; what it cannot do is
     guarantee the menu sees the saves that were already there. Worth one line
     rather than the boot. */

  void Populate(Fs, Backing, Hold());
}

async function Populate(Fs: typeof FS, Backing: Emscripten.FileSystemType,
                        Release: () => void): Promise<void> {
  try {
    await Wipe(Query.Present('wipesaves'));

    if(!await Claim()) {
      ReadOnly = true;
      console.warn('ivanSaves: another tab has IVAN open; saving is off in this one');
      Announce('readonly');
    }

    try {
      Fs.mkdir(Mount);
    } catch {
      /* Already there. */
    }

    Fs.mount(Backing, {}, Mount);
    Mounted = true;

    /* try/finally rather than the catch below, because this callback is
       invoked by IndexedDB rather than awaited here: a throw in it would
       escape every handler around it and leave startup held forever -- a page
       stuck on the loading bar over a save feature, which is exactly what
       happened the first time this ran against a build with no
       trackingDelegate. */

    const Began = Date.now();

    Fs.syncfs(true, (Error_: unknown) => {
      PopulateMs = Date.now() - Began;

      try {
        if(Error_) {
          ReadOnly = true;
          Fail('reading saves from IndexedDB', Said(Error_));
        } else if(!ReadOnly) {
          Track();
          Persist();
        }
      } catch(Thrown) {
        ReadOnly = true;
        Fail('starting the save watcher', Said(Thrown));
      } finally {
        Release();
      }
    });
  } catch(Error_) {
    ReadOnly = true;
    Fail('mounting', Said(Error_));
    Release();
  }
}

/* Nothing this returns may leave the dependency held. A page that will not
   boot is a worse outcome than a page that cannot save, and this runs before
   anything a player could read an error from. */

function Hold(): () => void {
  const Add = Module.addRunDependency;
  const Remove = Module.removeRunDependency;

  if(!Add || !Remove) {
    console.warn('ivanSaves: no run dependency available; the menu may be drawn '
                 + 'before the saves arrive');
    return () => {};
  }

  Add(Tag);

  let Done = false;

  return () => {
    if(Done)
      return;

    Done = true;
    Remove(Tag);
  };
}

function Wipe(Yes: boolean): Promise<void> {
  if(!Yes || !globalThis.indexedDB)
    return Promise.resolve();

  /* Before the mount, so there is no open connection to block on. This is
     the path that recovers a save the game cannot load. */

  return new Promise((Resolve) => {
    const Request = indexedDB.deleteDatabase(Mount);

    Request.onsuccess = () => {
      console.warn('ivanSaves: saves deleted (?wipesaves)');
      Resolve();
    };

    Request.onerror = () => {
      Fail('deleting the saves', Request.error ?? 'unknown');
      Resolve();
    };

    /* Another tab holds the database open. The delete is queued and will
       happen when that tab closes, which is not what someone typing
       ?wipesaves is asking for -- and reporting it as done would send them
       back into the save that will not load. Say it, and let the mount below
       proceed on the data that is still there. */

    Request.onblocked = () => {
      Fail('deleting the saves', 'another tab has IVAN open; close it and reload');
      Resolve();
    };
  });
}

/* Storage a browser considers "best effort" can be evicted when the disk
   fills, and a deleted save is indistinguishable from one that was never
   written. Asking costs nothing and is silently declined when the browser
   does not want to grant it. */

function Persist(): void {
  if(navigator.storage?.persist)
    void navigator.storage.persist().catch(() => {});
}

/* ---- installation -----------------------------------------------------
   Called by main.ts rather than run on import, the same split music.ts and the
   harness made: a module is also imported by a test, where neither `document`
   nor `Module` exists, and a top-level reference to either would be a
   TypeError before the first assertion.

   The <script> that carries this bundle runs before {{{ SCRIPT }}}, so the
   hook is on Module.preRun before ivan.js reads it -- the same position in the
   startup the --pre-js occupied, and ahead of the data package's own hook for
   the same reason it was then. */

export function Install(): void {
  /* Hiding the tab is the last moment worth reacting to. It does not make the
     sync synchronous -- nothing can -- it only stops the debounce from being
     the reason a save missed. The ordinary case is already covered: the game
     yields to the event loop the moment it waits for the next key, which is
     the instruction after the one that saved. */

  document.addEventListener('visibilitychange', () => {
    if(document.visibilityState === 'hidden' && Dirty && Timer !== null) {
      clearTimeout(Timer);
      Timer = null;
      Sync();
    }
  });

  if(!Query.Enabled('saves')) {
    ReadOnly = true;
    return;
  }

  if(typeof Module.preRun === 'function')
    Module.preRun = [Module.preRun];
  else if(!Module.preRun)
    Module.preRun = [];

  Module.preRun.push(Boot);
}

/* Test-only, as music.ts's is. The state above is module scope and node caches
   the module, so this is what stops one case's mount, counters and debounce
   timer being the next one's. */

export function Reset(): void {
  if(Timer !== null)
    clearTimeout(Timer);

  Mounted = false;
  ReadOnly = false;
  Dirty = false;
  Timer = null;
  Syncing = false;
  Again = false;
  TempWaits = 0;
  Waiters = [];
  Counts.syncs = 0;
  Counts.writes = 0;
  Counts.failures = 0;
  Counts.tempDeferrals = 0;
  LastError = null;
  LastSyncMs = 0;
  PopulateMs = 0;
}

export const Api: IvanSaves = {
  mount: Mount,

  stats: () => ({
    mounted: Mounted,
    readOnly: ReadOnly,
    dirty: Dirty,
    syncing: Syncing,
    syncs: Counts.syncs,
    writes: Counts.writes,
    failures: Counts.failures,
    tempDeferrals: Counts.tempDeferrals,
    populateMs: PopulateMs,
    lastSyncMs: LastSyncMs,
    lastError: LastError
  }),

  flush: () => new Promise((Resolve) => {
    if(!Mounted || ReadOnly) {
      Resolve(null);
      return;
    }

    Waiters.push(Resolve);

    if(Timer !== null) {
      clearTimeout(Timer);
      Timer = null;
    }

    Sync();
  }),

  files: () => {
    const All: { path: string; bytes: number }[] = [];

    Walk(Mount, (Path, Size) => { All.push({ path: Path, bytes: Size }); });

    return All;
  },

  bytes: () => {
    let Total = 0;

    Walk(Mount, (_Path, Size) => { Total += Size; });

    return Total;
  },

  estimate: () => {
    if(!navigator.storage?.estimate)
      return Promise.resolve(null);

    return navigator.storage.estimate().then((E) => ({
      usageMB: +((E.usage ?? 0) / 1048576).toFixed(2),
      quotaMB: +((E.quota ?? 0) / 1048576).toFixed(0),
      persisted: null
    }));
  },

  /* Deletes the files rather than the database, so the deletion goes through
     the same sync the saves did and there is no open connection to fight.
     Reloads afterwards because the running game still holds the state it
     just had wiped and would write it straight back. */

  wipe: async (Reload?: boolean) => {
    const Fs = Filesystem();
    const Doomed: string[] = [];

    Walk(Mount, (Path) => { Doomed.push(Path); });

    for(const Path of Doomed) {
      try {
        Fs?.unlink(Path);
      } catch(Error_) {
        console.warn('ivanSaves: could not delete ' + Path + ': ' + Said(Error_));
      }
    }

    await Api.flush();

    if(Reload !== false)
      location.reload();
  }
};
