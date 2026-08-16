/*
 * Saves that survive the tab (HARNESS.md §9.10).
 *
 * The game writes saves with fopen and knows nothing about any of this. All
 * that changes for it is where GetUserDataDir() points: /ivan/ on this target
 * rather than "./" (save.cpp), and /ivan is an IDBFS mount. Everything the
 * player accumulates goes there -- Save/, Bones/, ivan.cfg, the highscore
 * table, the answers to the name prompt -- while Graphics/ and Script/ stay in
 * the read-only MEMFS that ivan.data populates at /.
 *
 * IndexedDB rather than localStorage, and the reason is a measurement rather
 * than a preference: one dungeon level of the non-combat corpus is 3.5MB of
 * save files, of which the level file alone is ~1MB, and a real run visits
 * dozens of levels. localStorage is 5-10MB per origin, holds strings (so +33%
 * for base64), and writes synchronously on the main thread. It would run out
 * before the player left Under Water Tunnel.
 *
 * IDBFS keeps a MEMFS in front of IndexedDB and copies between them on demand,
 * so the two jobs here are: read it in before main() runs, and write it back
 * after the game has written a save.
 *
 * WRITING BACK IS DEBOUNCED, AND NOT ONLY TO COALESCE. outputfile writes to
 * <name>.tmp and copies it over the final name on close (save.cpp:31), so a
 * sync taken mid-save would push a megabyte of temporary file into IndexedDB
 * and delete it again on the next pass. Flush waits for the writes to stop and
 * refuses outright while a .tmp is on disk.
 *
 * The wait costs nothing, because of how the module runs: the game blocks
 * inside wasm and only returns to the JS event loop when asyncify unwinds it
 * at the input wait. A timer therefore cannot fire until the game is idle,
 * which is exactly when a sync should happen.
 *
 * From the console:
 *
 *   ivanSaves.stats()      mounted, dirty, syncs, failures, lastError
 *   ivanSaves.flush()      sync now; resolves when IndexedDB has it
 *   ivanSaves.estimate()   navigator.storage.estimate(), in MB
 *   ivanSaves.files()      what is in the mount, with sizes
 *   ivanSaves.wipe()       delete every save, then reload
 *
 * Query string:
 *
 *   ?saves=off             do not mount; play in a scratch filesystem
 *   ?wipesaves             delete the database before mounting
 *
 * ?wipesaves is the escape hatch that has to exist. Persistent saves mean a
 * save that crashes on load crashes on every load, and a player who cannot
 * reach the menu cannot use a console API that lives behind it.
 */

(function () {
  'use strict';

  var Mod = typeof Module !== 'undefined' ? Module : (Module = {});

  var Query = new URLSearchParams(location.search);
  var Enabled = Query.get('saves') !== 'off';
  var WipeFirst = Query.has('wipesaves');

  /* Both the mount point and the IndexedDB database name: IDBFS keys its
     database on the mount path (libidbfs.js, getDB(mount.mountpoint)). Change
     one and you have changed the other, and every existing save is orphaned. */

  var Mount = '/ivan';

  /* Only files under here are the player's. A .tmp is a save in progress and
     nothing else in the mount ends that way. */

  var Temp = '.tmp';

  /* Long enough to let a burst of writes finish, short enough that the gap
     between "the game saved" and "the browser has it" is not a window a player
     could close the tab in. A level change writes two save sets seconds apart
     and gets two syncs, which is correct rather than wasteful -- the first one
     is a save worth keeping on its own. */

  var DebounceMs = 200;

  /* A sync that finds a .tmp waits this long and asks again. The bound exists
     so a crash between opening a .tmp and removing it cannot wedge saving for
     the rest of the session: after this many attempts the .tmp is stale and
     syncing it is better than syncing nothing. */

  var TempRetryMs = 400;
  var MaxTempRetries = 10;

  var FSys = null;
  var Mounted = false;
  var ReadOnly = false;          /* another tab holds the lock, or the mount failed */
  var Dirty = false;
  var Timer = null;
  var Syncing = false;
  var Again = false;
  var TempWaits = 0;
  var Waiters = [];

  var Counts = { syncs: 0, writes: 0, failures: 0, tempDeferrals: 0 };
  var LastError = null;
  var LastSyncMs = 0;

  /* How long the module was held at startup reading IndexedDB back. The number
     that decides whether this scales: it is paid on every load and it grows
     with the size of the save set, where the sync after a save does not. */

  var PopulateMs = 0;

  /* ---- talking to the page ---------------------------------------------
     saves.js decides, shell.html displays, the same split sfx.js and the mute
     button use. Nothing here touches the DOM. */

  function Announce(Kind, Detail) {
    try {
      dispatchEvent(new CustomEvent('ivan-saves', {
        detail: { kind: Kind, detail: Detail == null ? '' : String(Detail) }
      }));
    } catch (Error) {
      /* An event that cannot be dispatched must not take the save with it. */
    }
  }

  function Fail(What, Error) {
    Counts.failures++;
    LastError = What + ': ' + Error;
    console.error('ivanSaves: ' + LastError);
    Announce('error', What);
  }

  /* ---- the filesystem ---------------------------------------------------
     FS and IDBFS are module-scope in the generated JS and also exported onto
     Module; taking either is fine, but taking neither has to be survivable,
     because the alternative is a page that will not boot over a save feature. */

  function Filesystem() {
    if (FSys)
      return FSys;

    FSys = Mod.FS || (typeof FS !== 'undefined' ? FS : null);
    return FSys;
  }

  function Backend() {
    return Mod.IDBFS || (typeof IDBFS !== 'undefined' ? IDBFS : null);
  }

  function Walk(Path, Each) {
    var Fs = Filesystem();
    var Names;

    try {
      Names = Fs.readdir(Path);
    } catch (Error) {
      return;
    }

    Names.forEach(function (Name) {
      if (Name === '.' || Name === '..')
        return;

      var Full = Path + '/' + Name;
      var Stat;

      try {
        Stat = Fs.stat(Full);
      } catch (Error) {
        return;
      }

      if (Fs.isDir(Stat.mode))
        Walk(Full, Each);
      else
        Each(Full, Stat.size);
    });
  }

  function HasTemp() {
    var Found = false;

    Walk(Mount, function (Path) {
      if (Path.slice(-Temp.length) === Temp)
        Found = true;
    });

    return Found;
  }

  /* ---- syncing ----------------------------------------------------------
     One sync at a time. A second request arriving mid-flight is remembered
     rather than run, because IDBFS reconciles the whole mount against the
     whole database and two of those interleaved would race over the same
     records. */

  function Settle(Error) {
    var Pending = Waiters;

    Waiters = [];
    Pending.forEach(function (Resolve) { Resolve(Error || null); });
  }

  function Sync() {
    if (Syncing) {
      Again = true;
      return;
    }

    if (!Mounted || ReadOnly) {
      Settle(null);
      return;
    }

    /* Never persist a half-written save. */

    if (HasTemp() && TempWaits < MaxTempRetries) {
      TempWaits++;
      Counts.tempDeferrals++;
      Schedule(TempRetryMs);
      return;
    }

    TempWaits = 0;
    Syncing = true;
    Dirty = false;

    var Began = Date.now();

    Filesystem().syncfs(false, function (Error) {
      Syncing = false;
      LastSyncMs = Date.now() - Began;

      if (Error) {
        /* Put the dirt back: the files are still only in MEMFS, and the next
           write must try again rather than assume this one landed. */
        Dirty = true;
        Fail('writing saves to IndexedDB', Error.message || Error);
      } else {
        Counts.syncs++;
        LastError = null;
      }

      if (Again) {
        Again = false;
        Sync();
        return;
      }

      Settle(Error);
    });
  }

  function Schedule(Delay) {
    if (Timer !== null)
      clearTimeout(Timer);

    Timer = setTimeout(function () {
      Timer = null;
      Sync();
    }, Delay);
  }

  function Touched(Path) {
    if (!Mounted || ReadOnly)
      return;

    if (String(Path).lastIndexOf(Mount + '/', 0) !== 0)
      return;

    Counts.writes++;
    Dirty = true;
    Schedule(DebounceMs);
  }

  /* Every way the mount can change. Deletions matter as much as writes: the
     game removes the save set when the player dies (game.cpp:3843), and a
     deletion that never reached IndexedDB would put a dead character back on
     the Continue menu after a reload. */

  function Track() {
    var Delegate = Filesystem().trackingDelegate;

    /* Only exists when the module was linked -sFS_DEBUG=1, which the browser
       target sets. A build without it would sync the saves that happen to be
       in MEMFS at boot and nothing after, which is worse than not saving,
       because it looks like it is working. */

    if (!Delegate) {
      ReadOnly = true;
      Fail('watching for saves', 'FS.trackingDelegate missing (link -sFS_DEBUG=1)');
      return;
    }

    Delegate['onWriteToFile'] = Touched;
    Delegate['onDeletePath'] = Touched;
    Delegate['onMakeDirectory'] = Touched;
    Delegate['onMovePath'] = function (From, To) { Touched(From); Touched(To); };
  }

  /* ---- one tab at a time ------------------------------------------------
     Two tabs on one origin share one database and each holds its own MEMFS, so
     whichever syncs last overwrites the other's saves wholesale. The lock is
     held for the life of the page; a tab that cannot get it plays with syncing
     switched off and says so, which loses that session's progress but not the
     run already stored. */

  function Claim() {
    if (!navigator.locks || !navigator.locks.request)
      return Promise.resolve(true);

    return new Promise(function (Resolve) {
      navigator.locks.request('ivan-saves', { ifAvailable: true }, function (Lock) {
        Resolve(!!Lock);

        if (!Lock)
          return;

        /* Never resolves, so the lock is released only when the tab goes. */
        return new Promise(function () {});
      }).catch(function () { Resolve(true); });
    });
  }

  /* ---- boot -------------------------------------------------------------
     A run dependency, so the module waits. Without it main() reaches the menu
     and iosystem::ContinueMenu enumerates a Save/ that IndexedDB has not been
     copied into yet -- "You don't have any previous saves." over a save that
     was about to arrive. */

  function Boot() {
    var Fs = Filesystem();
    var Backing = Backend();

    if (!Fs || !Backing) {
      ReadOnly = true;
      Fail('mounting', 'FS or IDBFS missing from the module');
      return;
    }

    addRunDependency('ivan-saves');

    var Done = false;

    function Release() {
      if (Done)
        return;

      Done = true;
      removeRunDependency('ivan-saves');
    }

    /* Nothing below may leave the dependency held. A page that will not boot
       is a worse outcome than a page that cannot save, and this runs before
       anything a player could read an error from. */

    Wipe(WipeFirst).then(Claim).then(function (Got) {
      if (!Got) {
        ReadOnly = true;
        console.warn('ivanSaves: another tab has IVAN open; saving is off in this one');
        Announce('readonly');
      }

      try {
        Fs.mkdir(Mount);
      } catch (Error) {
        /* Already there. */
      }

      Fs.mount(Backing, {}, Mount);
      Mounted = true;

      /* try/finally rather than a promise catch: this callback is invoked by
         IndexedDB, not by the chain below, so a throw in here would escape the
         .catch() and leave startup held forever -- a page stuck on the loading
         bar over a save feature, which is exactly what happened the first time
         this ran against a build with no trackingDelegate. */

      var Began = Date.now();

      Fs.syncfs(true, function (Error) {
        PopulateMs = Date.now() - Began;

        try {
          if (Error) {
            ReadOnly = true;
            Fail('reading saves from IndexedDB', Error.message || Error);
          } else if (!ReadOnly) {
            Track();
            Persist();
          }
        } catch (Thrown) {
          ReadOnly = true;
          Fail('starting the save watcher', Thrown && Thrown.message || Thrown);
        } finally {
          Release();
        }
      });
    }).catch(function (Error) {
      ReadOnly = true;
      Fail('mounting', Error && Error.message || Error);
      Release();
    });
  }

  function Wipe(Yes) {
    if (!Yes || !globalThis.indexedDB)
      return Promise.resolve();

    /* Before the mount, so there is no open connection to block on. This is
       the path that recovers a save the game cannot load. */

    return new Promise(function (Resolve) {
      var Request = indexedDB.deleteDatabase(Mount);

      Request.onsuccess = function () {
        console.warn('ivanSaves: saves deleted (?wipesaves)');
        Resolve();
      };

      Request.onerror = function () {
        Fail('deleting the saves', Request.error || 'unknown');
        Resolve();
      };

      /* Another tab holds the database open. The delete is queued and will
         happen when that tab closes, which is not what someone typing
         ?wipesaves is asking for -- and reporting it as done would send them
         back into the save that will not load. Say it, and let the mount below
         proceed on the data that is still there. */

      Request.onblocked = function () {
        Fail('deleting the saves', 'another tab has IVAN open; close it and reload');
        Resolve();
      };
    });
  }

  /* Storage a browser considers "best effort" can be evicted when the disk
     fills, and a deleted save is indistinguishable from one that was never
     written. Asking costs nothing and is silently declined when the browser
     does not want to grant it. */

  function Persist() {
    if (navigator.storage && navigator.storage.persist)
      navigator.storage.persist().catch(function () {});
  }

  /* Hiding the tab is the last moment worth reacting to. It does not make the
     sync synchronous -- nothing can -- it only stops the debounce from being
     the reason a save missed. The ordinary case is already covered: the game
     yields to the event loop the moment it waits for the next key, which is
     the instruction after the one that saved. */

  if (typeof document !== 'undefined' && document.addEventListener)
    document.addEventListener('visibilitychange', function () {
      if (document.visibilityState === 'hidden' && Dirty && Timer !== null) {
        clearTimeout(Timer);
        Timer = null;
        Sync();
      }
    });

  if (Enabled) {
    if (typeof Mod.preRun === 'function')
      Mod.preRun = [Mod.preRun];
    else if (!Mod.preRun)
      Mod.preRun = [];

    Mod.preRun.push(Boot);
  } else {
    ReadOnly = true;
  }

  globalThis.ivanSaves = {
    mount: Mount,

    stats: function () {
      return {
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
      };
    },

    flush: function () {
      return new Promise(function (Resolve) {
        if (!Mounted || ReadOnly) {
          Resolve(null);
          return;
        }

        Waiters.push(Resolve);

        if (Timer !== null) {
          clearTimeout(Timer);
          Timer = null;
        }

        Sync();
      });
    },

    files: function () {
      var All = [];

      Walk(Mount, function (Path, Size) { All.push({ path: Path, bytes: Size }); });

      return All;
    },

    bytes: function () {
      var Total = 0;

      Walk(Mount, function (Path, Size) { Total += Size; });

      return Total;
    },

    estimate: function () {
      if (!navigator.storage || !navigator.storage.estimate)
        return Promise.resolve(null);

      return navigator.storage.estimate().then(function (E) {
        return {
          usageMB: +(E.usage / 1048576).toFixed(2),
          quotaMB: +(E.quota / 1048576).toFixed(0),
          persisted: null
        };
      });
    },

    /* Deletes the files rather than the database, so the deletion goes through
       the same sync the saves did and there is no open connection to fight.
       Reloads afterwards because the running game still holds the state it
       just had wiped and would write it straight back. */

    wipe: function (Reload) {
      var Fs = Filesystem();
      var Doomed = [];

      Walk(Mount, function (Path) { Doomed.push(Path); });
      Doomed.forEach(function (Path) {
        try {
          Fs.unlink(Path);
        } catch (Error) {
          console.warn('ivanSaves: could not delete ' + Path + ': ' + Error);
        }
      });

      return globalThis.ivanSaves.flush().then(function () {
        if (Reload !== false)
          location.reload();
      });
    }
  };
})();
