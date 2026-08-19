/*
 * Tests for tools/web/saves.js. Run with: node tools/web/saves.test.js
 *
 * What saves.js decides is when to copy MEMFS into IndexedDB, and every one of
 * those decisions is checkable without a browser:
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
 * browser; see HARNESS.md §9.10.
 */

'use strict';

var Path = require('path');

var Failures = 0;
var Checks = 0;

function Check(What, Got, Want) {
  Checks++;

  if(JSON.stringify(Got) === JSON.stringify(Want))
    return;

  Failures++;
  console.log('FAIL ' + What + '\n  got  ' + JSON.stringify(Got) +
              '\n  want ' + JSON.stringify(Want));
}

/* ---- stubs -------------------------------------------------------------- */

var DIR = 1;
var FILE = 2;

/* A filesystem just deep enough for the walk saves.js does: it looks for a
   .tmp anywhere under the mount and it enumerates the mount to wipe it. */

function MakeFS() {
  var Files = new Map();
  var Dirs = new Set(['/', '/ivan']);

  var Fs = {
    trackingDelegate: {},

    /* Test knobs. */
    Syncs: [],
    SyncError: null,
    SyncDelayMs: 0,
    Overlaps: 0,
    InFlight: 0,
    Mounts: [],

    mkdir: function (P) { Dirs.add(P); },
    mount: function (Backend, Opts, P) { Fs.Mounts.push(P); },

    readdir: function (P) {
      if(!Dirs.has(P))
        throw new Error('ENOENT ' + P);

      var Kids = [];

      function Add(Full) {
        if(Path.posix.dirname(Full) === P)
          Kids.push(Path.posix.basename(Full));
      }

      Dirs.forEach(function (D) { if(D !== P) Add(D); });
      Files.forEach(function (Size, F) { Add(F); });

      return ['.', '..'].concat(Kids);
    },

    stat: function (P) {
      if(Dirs.has(P))
        return { mode: DIR, size: 0 };

      if(Files.has(P))
        return { mode: FILE, size: Files.get(P) };

      throw new Error('ENOENT ' + P);
    },

    isDir: function (Mode) { return Mode === DIR; },

    unlink: function (P) {
      Files.delete(P);
      if(Fs.trackingDelegate['onDeletePath'])
        Fs.trackingDelegate['onDeletePath'](P);
    },

    syncfs: function (Populate, Done) {
      Fs.Syncs.push(Populate);
      Fs.InFlight++;

      if(Fs.InFlight > 1)
        Fs.Overlaps++;

      setTimeout(function () {
        Fs.InFlight--;
        Done(Fs.SyncError);
      }, Fs.SyncDelayMs);
    }
  };

  /* What the runtime does when the game writes: the bytes land, then the
     delegate is told (libfs.js:1375). */

  Fs.Write = function (P, Size) {
    var Parent = Path.posix.dirname(P);

    if(!Dirs.has(Parent)) {
      Dirs.add(Parent);
      if(Fs.trackingDelegate['onMakeDirectory'])
        Fs.trackingDelegate['onMakeDirectory'](Parent);
    }

    Files.set(P, Size == null ? 1024 : Size);

    if(Fs.trackingDelegate['onWriteToFile'])
      Fs.trackingDelegate['onWriteToFile'](P, Size == null ? 1024 : Size);
  };

  Fs.Remove = function (P) { Fs.unlink(P); };
  Fs.Has = function (P) { return Files.has(P); };

  return Fs;
}

var Announced = [];
var Deleted = [];
var Reloads = 0;
var Persists = 0;
var Dependencies = [];

/* Rebuilt for each scenario, because saves.js is a one-shot IIFE: it reads the
   query string, registers its preRun hook and never runs again. */

function Load(Options) {
  Options = Options || {};

  var Fs = MakeFS();

  /* A module linked without -sFS_DEBUG has no trackingDelegate at all
     (libfs.js:61), which is how this first reached a browser. */
  if(Options.noTracking)
    delete Fs.trackingDelegate;

  Announced = [];
  Deleted = [];
  Dependencies = [];

  global.Module = { FS: Fs, IDBFS: { name: 'IDBFS' } };
  global.FS = Fs;

  global.location = {
    search: Options.search || '',
    reload: function () { Reloads++; }
  };

  global.addRunDependency = function (Name) { Dependencies.push(Name); };
  global.removeRunDependency = function (Name) {
    Dependencies.splice(Dependencies.indexOf(Name), 1);
  };

  global.CustomEvent = function (Name, Init) {
    this.type = Name;
    this.detail = Init && Init.detail;
  };

  global.dispatchEvent = function (Event) { Announced.push(Event.detail); };

  /* node 22 defines navigator as a getter-only global, so it has to be
     redefined rather than assigned. */
  Object.defineProperty(global, 'navigator', { configurable: true, value: {
    storage: {
      persist: function () { Persists++; return Promise.resolve(true); },
      estimate: function () {
        return Promise.resolve({ usage: 3670016, quota: 1073741824 });
      }
    },

    locks: Options.noLocks ? undefined : {
      request: function (Name, Opts, Body) {
        return Promise.resolve(Body(Options.tabTaken ? null : { name: Name }));
      }
    }
  }});

  global.indexedDB = {
    deleteDatabase: function (Name) {
      Deleted.push(Name);

      var Request = {};

      setTimeout(function () {
        if(Options.wipeBlocked)
          Request.onblocked();
        else
          Request.onsuccess();
      }, 0);

      return Request;
    }
  };

  global.document = {
    visibilityState: 'visible',
    addEventListener: function (Name, Fn) { Fs.OnVisibility = Fn; }
  };

  delete require.cache[require.resolve('./saves.js')];
  require('./saves.js');

  return { fs: Fs, saves: global.ivanSaves, module: global.Module };
}

function Tick(Ms) {
  return new Promise(function (Resolve) { setTimeout(Resolve, Ms || 0); });
}

/* Longer than saves.js's 200ms debounce, short enough to keep the suite quick.
   Not a constant it exports: the test asserting the behaviour rather than the
   number is the point, and a debounce this test had to be told about would be
   a debounce it could not catch a change to. */

function AfterDebounce() {
  return Tick(320);
}

/* ---- the tests ---------------------------------------------------------- */

(async function () {
  /* -- boot ------------------------------------------------------------- */

  var World = Load();

  Check('nothing is mounted before preRun', World.saves.stats().mounted, false);
  Check('and one preRun hook is registered', World.module.preRun.length, 1);

  World.module.preRun[0]();

  Check('startup waits for the saves', Dependencies, ['ivan-saves']);

  await Tick(0);
  await Tick(0);

  Check('the mount is where save.cpp looks', World.fs.Mounts, ['/ivan']);
  Check('IndexedDB is read, not written', World.fs.Syncs, [true]);
  Check('and startup is released', Dependencies, []);
  Check('mounted', World.saves.stats().mounted, true);
  Check('and writable', World.saves.stats().readOnly, false);
  Check('storage is asked to be persistent', Persists > 0, true);

  /* -- a save ------------------------------------------------------------ */

  /* One autosave is eight files through outputfile, and the whole point of the
     debounce is that this is one write to IndexedDB rather than eight. */

  World.fs.Write('/ivan/Save/Belyer_Asu.sav', 171560);
  World.fs.Write('/ivan/Save/Belyer_Asu.wm', 64769);
  World.fs.Write('/ivan/Save/Belyer_Asu.40', 892168);

  Check('a write does not sync immediately', World.fs.Syncs.length, 1);
  Check('but is remembered', World.saves.stats().dirty, true);

  await AfterDebounce();

  Check('a burst of writes is one sync', World.fs.Syncs, [true, false]);
  Check('and the mount is clean afterwards', World.saves.stats().dirty, false);
  Check('counted', World.saves.stats().syncs, 1);
  Check('no failures', World.saves.stats().failures, 0);

  /* -- what is not the player's ------------------------------------------ */

  /* The recording web/src/harness reads lives at /session.rec, and Graphics/ and
     Script/ are at the MEMFS root. None of it belongs in IndexedDB. */

  World.fs.Write('/session.rec', 400);
  World.fs.Write('/Graphics/Char.png', 1000);

  await AfterDebounce();

  Check('writes outside the mount are ignored', World.fs.Syncs.length, 2);

  /* -- a save in progress ------------------------------------------------ */

  /* outputfile opens <name>.tmp, fills it, copies it over the final name and
     removes it. Syncing in the middle would put the .tmp in IndexedDB and take
     it out again on the next pass -- a megabyte each way for a file that is
     never read. */

  World.fs.Write('/ivan/Save/Belyer_Asu.40.tmp', 892168);

  await AfterDebounce();

  Check('a .tmp defers the sync', World.fs.Syncs.length, 2);
  Check('and says why', World.saves.stats().tempDeferrals > 0, true);
  Check('while staying dirty', World.saves.stats().dirty, true);

  World.fs.Remove('/ivan/Save/Belyer_Asu.40.tmp');

  await Tick(600);

  Check('and syncs once the save is finished', World.fs.Syncs.length, 3);
  Check('the .tmp never reached IndexedDB', World.fs.Has('/ivan/Save/Belyer_Asu.40.tmp'), false);

  /* -- overlapping ------------------------------------------------------- */

  /* IDBFS reconciles the whole mount against the whole database, so two of
     those at once would race over the same records. A write arriving mid-sync
     has to queue rather than start a second one -- and must not be dropped. */

  World.fs.SyncDelayMs = 300;
  World.fs.Write('/ivan/Save/Belyer_Asu.sav', 171999);

  await AfterDebounce();

  Check('a sync is in flight', World.saves.stats().syncing, true);

  /* Written and flushed while the first sync is still running, which is the
     collision itself rather than a write that merely happens to be nearby --
     the debounce would otherwise let the first one finish first and there
     would be nothing to queue. */

  var Collided = World.fs.Syncs.length;

  World.fs.Write('/ivan/Save/Belyer_Asu.40', 892999);

  var Queued = World.saves.flush();

  Check('the second does not start', World.fs.Syncs.length, Collided);

  await Queued;

  Check('it runs after the first', World.fs.Syncs.length, Collided + 1);
  Check('and no two syncs overlapped', World.fs.Overlaps, 0);
  Check('with nothing left dirty', World.saves.stats().dirty, false);

  World.fs.SyncDelayMs = 0;

  /* -- deletion ---------------------------------------------------------- */

  /* The save set is removed when the player dies (game.cpp:3843). A deletion
     that never reached IndexedDB would put the dead character back on the
     Continue menu after a reload, which is worse than losing the save. */

  var Before = World.fs.Syncs.length;

  World.fs.Remove('/ivan/Save/Belyer_Asu.sav');

  await AfterDebounce();

  Check('a deletion syncs like a write', World.fs.Syncs.length, Before + 1);

  /* -- flush ------------------------------------------------------------- */

  Before = World.fs.Syncs.length;
  World.fs.Write('/ivan/ivan.cfg', 900);

  await World.saves.flush();

  Check('flush does not wait for the debounce', World.fs.Syncs.length, Before + 1);
  Check('and resolves after the sync, not before', World.saves.stats().syncing, false);

  /* -- failure ----------------------------------------------------------- */

  /* IDBFS's own autoPersist discards this error, which is how a full disk
     becomes a game that quietly stops saving. */

  World.fs.SyncError = new Error('QuotaExceededError');
  World.fs.Write('/ivan/Save/Belyer_Asu.40', 892168);

  await AfterDebounce();

  Check('a failed sync is counted', World.saves.stats().failures, 1);
  Check('and reported', World.saves.stats().lastError !== null, true);
  Check('and told to the page', Announced[Announced.length - 1].kind, 'error');
  Check('and stays dirty so the next write retries', World.saves.stats().dirty, true);

  World.fs.SyncError = null;
  World.fs.Write('/ivan/Save/Belyer_Asu.40', 892168);

  await AfterDebounce();

  Check('recovering clears the error', World.saves.stats().lastError, null);

  /* -- wipe -------------------------------------------------------------- */

  await World.saves.wipe(false);

  Check('wipe empties the mount', World.saves.files(), []);
  Check('and pushes the deletions through', World.saves.stats().dirty, false);

  /* -- a second tab ------------------------------------------------------ */

  /* Two tabs share one database and hold separate copies of MEMFS, so the
     second one to sync overwrites the first one's saves wholesale. */

  World = Load({ tabTaken: true });
  World.module.preRun[0]();

  await Tick(0);
  await Tick(0);

  Check('a second tab still reads the saves', World.fs.Syncs, [true]);
  Check('but will not write them', World.saves.stats().readOnly, true);
  Check('and warns the page', Announced[0].kind, 'readonly');

  World.fs.Write('/ivan/Save/Belyer_Asu.sav', 171560);

  await AfterDebounce();

  Check('nothing it does reaches IndexedDB', World.fs.Syncs, [true]);
  Check('and it does not even accumulate dirt', World.saves.stats().dirty, false);

  /* -- a browser without Web Locks --------------------------------------- */

  World = Load({ noLocks: true });
  World.module.preRun[0]();

  await Tick(0);
  await Tick(0);

  Check('no lock API is not a reason to stop saving', World.saves.stats().readOnly, false);

  /* -- ?wipesaves -------------------------------------------------------- */

  /* The escape hatch for a save the game cannot load, which is the failure
     persistence introduces: without it, one bad save bricks the tab forever
     and the console API that would fix it is behind a menu that never draws. */

  World = Load({ search: '?wipesaves' });
  World.module.preRun[0]();

  await Tick(0);
  await Tick(0);

  Check('the database is deleted before the mount', Deleted, ['/ivan']);
  Check('and the game still boots', World.saves.stats().mounted, true);
  Check('with startup released', Dependencies, []);
  Check('and no complaint', World.saves.stats().failures, 0);

  /* IndexedDB queues a delete that another connection is holding open and
     fires onblocked instead. Reporting that as done sends the player straight
     back into the save they were trying to escape. */

  World = Load({ search: '?wipesaves', wipeBlocked: true });
  World.module.preRun[0]();

  await Tick(0);
  await Tick(0);

  Check('a blocked wipe is not reported as a wipe', World.saves.stats().failures, 1);
  Check('and says which tab to close',
        /another tab/.test(World.saves.stats().lastError), true);
  Check('while still booting', World.saves.stats().mounted, true);

  /* -- ?saves=off -------------------------------------------------------- */

  World = Load({ search: '?saves=off' });

  Check('opting out registers no hook', World.module.preRun, undefined);
  Check('and reports itself read-only', World.saves.stats().readOnly, true);

  /* -- no way to watch for writes -----------------------------------------
     The run dependency is held across a callback IndexedDB invokes, so a throw
     in there escapes every promise handler around it and startup is never
     released -- a page frozen on the loading bar because of a save feature.
     This is the shape of the first failure this hit in a real browser. */

  World = Load({ noTracking: true });
  World.module.preRun[0]();

  await Tick(0);
  await Tick(0);

  Check('a module that cannot be watched still boots', Dependencies, []);
  Check('with saving off rather than the page', World.saves.stats().readOnly, true);
  Check('and the reason said out loud', World.saves.stats().failures, 1);

  /* -- IndexedDB refusing -------------------------------------------------
     Private windows and blocked third-party storage both look like this. The
     game has to start anyway. */

  World = Load();
  World.fs.SyncError = new Error('SecurityError');
  World.module.preRun[0]();

  await Tick(0);
  await Tick(0);

  Check('a refused database still releases startup', Dependencies, []);
  Check('and turns saving off rather than the game', World.saves.stats().readOnly, true);
  Check('loudly', World.saves.stats().failures, 1);

  console.log((Failures ? 'FAILED ' : 'ok ') + (Checks - Failures) + '/' + Checks +
              ' checks');
  process.exit(Failures ? 1 : 0);
})();
