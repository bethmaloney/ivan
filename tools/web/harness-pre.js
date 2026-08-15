/*
 * Browser-side half of the harness (HARNESS.md §4, §9.6).
 *
 * The node host gets its harness arguments from argv and writes its outputs to
 * the real filesystem through NODERAWFS. A browser has neither, so this does
 * both jobs: it turns the query string into argv before main() runs, and it
 * hands the recording back afterwards -- including after a crash, which is the
 * case that matters.
 *
 * That last part works because harness::RecordKey flushes every key as it
 * writes it (harness.cpp:545). A wasm trap leaves the recording in MEMFS
 * complete but for its trailer, so the keys that led to the crash survive the
 * crash and can be replayed on the native build, where there is a debugger.
 *
 *   ivan.html?record=/session.rec&seed=999
 *
 * Then, from the console or automatically on a crash:
 *
 *   ivanHarness.download('/session.rec')
 *
 * and replay what comes out natively:
 *
 *   ./ivan --replay session.rec        # the seed rides along in the header
 */

(function () {
  var Mod = typeof Module !== 'undefined' ? Module : (Module = {});

  /* Query string to argv. A bare key becomes a flag with no value, so
     ?headless is --headless while ?seed=999 is --seed 999. The order the
     browser preserves is the order ParseArgs sees. */

  var Args = [];
  var Query = new URLSearchParams(location.search);

  Query.forEach(function (Value, Key) {
    Args.push('--' + Key);

    if (Value !== '')
      Args.push(Value);
  });

  if (Args.length)
    Mod.arguments = Args;

  /* Everything below is diagnostics, and none of it may throw: this runs
     inside the module's own error path, and a handler that fails there would
     replace the crash being reported with its own. */

  var Harness = {
    args: Args,

    /* Read a file out of MEMFS. Returns null rather than throwing, because
       every caller is either a crash handler or a human at a console. */

    read: function (Name) {
      try {
        return Mod.FS.readFile(Name);
      } catch (Error) {
        console.warn('ivanHarness: cannot read ' + Name + ': ' + Error);
        return null;
      }
    },

    /* Save a MEMFS file to disk through the browser's download path. */

    download: function (Name) {
      var Data = Harness.read(Name);

      if (!Data)
        return false;

      var Link = document.createElement('a');
      Link.href = URL.createObjectURL(new Blob([Data], {type: 'application/octet-stream'}));
      Link.download = Name.replace(/^.*\//, '');
      document.body.appendChild(Link);
      Link.click();
      document.body.removeChild(Link);
      URL.revokeObjectURL(Link.href);
      return true;
    },

    /* The recording as text, for pasting somewhere without a file dialog. */

    text: function (Name) {
      var Data = Harness.read(Name);
      return Data ? new TextDecoder().decode(Data) : null;
    }
  };

  /* Which file --record was pointed at, so the crash handler knows what to
     save without being told twice. */

  var RecordName = null;

  for (var c = 0; c < Args.length - 1; ++c)
    if (Args[c] === '--record')
      RecordName = Args[c + 1];

  Harness.recording = RecordName;

  var Reported = false;

  function Report(What, Detail) {
    if (Reported)
      return;

    Reported = true;

    console.error('ivan: ' + What + '\n' + Detail);

    if (!RecordName) {
      console.error('ivan: no --record in the query string, so there is nothing'
                    + ' to replay. Reload with ?record=/session.rec and'
                    + ' reproduce it.');
      return;
    }

    var Text = Harness.text(RecordName);

    if (Text) {
      console.error('ivan: the recording up to the crash follows. Save it and'
                    + ' replay it natively with --replay.\n' + Text);
      Harness.download(RecordName);
    }
  }

  Harness.report = Report;

  /* Three ways the module reports a failure, and they do not overlap: abort()
     for a runtime assertion, a rejected promise for a trap unwinding out of
     asyncify, and window.onerror for everything thrown on the main thread. */

  var PriorAbort = Mod.onAbort;

  Mod.onAbort = function (Reason) {
    Report('aborted', Reason);

    if (PriorAbort)
      PriorAbort(Reason);
  };

  addEventListener('unhandledrejection', function (Event) {
    var Reason = Event.reason;
    Report('unhandled rejection', (Reason && Reason.stack) || Reason);
  });

  addEventListener('error', function (Event) {
    Report('uncaught error', (Event.error && Event.error.stack) || Event.message);
  });

  globalThis.ivanHarness = Harness;
})();
