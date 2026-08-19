/*
 * Crash reports from a browser session. Console API, query options and the
 * three failure paths are in web/README.md; the argument for collecting any of
 * this unconditionally is docs/port-log.md §9.6.
 *
 * A trap in the wasm prints a stack trace to a console nobody is looking at and
 * the tab is closed. What makes a report worth having instead is that
 * harness::RecordKey flushes every key as it writes it (harness.cpp:545), so a
 * trap leaves the recording complete but for its trailer: the keys that led to
 * the crash survive the crash, and the seed rides along in the header. A report
 * therefore carries a deterministic reproduction rather than a description of
 * one -- `./ivan --replay session.rec`.
 *
 * Reports go to localStorage so they survive the reload or the closed tab that
 * usually follows a crash, and are POSTed when an endpoint is configured.
 */

import { BuildId, CrashEndpoint } from '../platform/build.ts';
import * as Query from '../platform/query.ts';
import { Argv, RecordPath } from './argv.ts';

const StoreKey = 'ivan.crashReports';
const MaxStoredReports = 5;

/* A long session's recording is unbounded and a report has to fit in
   localStorage beside four others. Truncating keeps the head of the recording,
   which is where the seed is -- a report whose recording was cut still names the
   run that produced it, and `keys` says how much is missing. */

const MaxRecordingBytes = 512 * 1024;

/* keepalive lets a POST outlive the page, which a crash is often followed by.
   The spec caps a keepalive body at 64KB, so a report that would exceed it goes
   as a second best: everything but the recording. Under the real ceiling, so a
   report near the line is not rejected over the JSON around the recording. */

const MaxKeepaliveBytes = 60000;

/* location and navigator exist in a browser and not under node, and this module
   is imported by its own test. The same shape platform/query.ts uses, for the
   same reason. */

interface MaybePage {
  location?: { href?: string } | undefined;
  navigator?: { userAgent?: string } | undefined;
}

export interface Report {
  what: string;
  detail: string;
  build: string;
  ivan: string | null;
  seed: string | null;
  keys: number;
  recording: string | null;
  recordingTruncated: boolean;
  args: string[];
  when: string;
  url: string;
  userAgent: string;
}

/* Set on the copy that is POSTed rather than on the stored one: the local copy
   keeps its recording, and only the wire version has to explain the absence. */

type Sent = Report & { recordingOmitted?: string };

/* Read at call time rather than captured at load, so it cannot disagree with the
   argv actually handed to the runtime. `?crashlog=` with nothing after it falls
   back to the built-in rather than switching reporting off -- ?record=off is how
   you ask for less, and a bare = is a typo. */

export function Endpoint(): string {
  return Query.Setting('crashlog') || CrashEndpoint();
}

/* ---- nothing below may throw ------------------------------------------
   It all runs inside the module's own error path, and a handler that fails
   there replaces the crash being reported with its own -- which is a worse bug
   than the one being chased, because it is invisible. Every entry point is
   wrapped and every failure is swallowed after a console warning. */

function Safely<T>(What: string, Fn: () => T, Fallback: T): T {
  try {
    return Fn();
  } catch (Error) {
    console.warn('ivanHarness: ' + What + ' failed: ' + Error);
    return Fallback;
  }
}

export function Reports(): Report[] {
  return Safely('reading stored reports', () => {
    const Stored: unknown = JSON.parse(localStorage.getItem(StoreKey) || '[]');
    return Array.isArray(Stored) ? (Stored as Report[]) : [];
  }, []);
}

function Store(New: Report): void {
  Safely('storing a report', () => {
    localStorage.setItem(StoreKey,
                         JSON.stringify([New, ...Reports()].slice(0, MaxStoredReports)));
  }, undefined);
}

export function Clear(): void {
  Safely('clearing', () => localStorage.removeItem(StoreKey), undefined);
}

/* ---- reading MEMFS ----------------------------------------------------
   FS is on Module because -sEXPORTED_RUNTIME_METHODS names it, and it is not
   there at all until the runtime has initialised -- so a crash before that
   point produces a report with no recording rather than a second error. */

export function Text(): string | null {
  const Path = RecordPath();

  if(!Path)
    return null;

  return Safely('reading ' + Path, () => {
    const Data = Module.FS?.readFile(Path);
    return Data ? new TextDecoder().decode(Data) : null;
  }, null);
}

function Download(Name: string, Text_: string, Type: string): boolean {
  return Safely('saving ' + Name, () => {
    const Link = document.createElement('a');

    Link.href = URL.createObjectURL(new Blob([Text_], { type: Type }));
    Link.download = Name;
    document.body.appendChild(Link);
    Link.click();
    document.body.removeChild(Link);
    URL.revokeObjectURL(Link.href);

    return true;
  }, false);
}

/* ---- the report ------------------------------------------------------
   The seed and the game version are parsed back out of the recording header
   rather than plumbed through from C++, because the recording is the thing that
   has to carry them anyway for --replay to work. One source, not two. */

export function Build(What: string, Detail: unknown): Report {
  let Recording = Text();
  let Truncated = false;

  if(Recording && Recording.length > MaxRecordingBytes) {
    Recording = Recording.slice(0, MaxRecordingBytes);
    Truncated = true;
  }

  const Header = Recording
    ? /ivan-record \d+ seed=(\d+) ivan=(\S+)/.exec(Recording)
    : null;

  const Page = globalThis as MaybePage;

  return {
    what: What,
    detail: String(Detail ?? ''),
    build: BuildId(),
    ivan: Header?.[2] ?? null,
    seed: Header?.[1] ?? null,
    keys: Recording ? (Recording.match(/^K /gm) ?? []).length : 0,
    recording: Recording,
    recordingTruncated: Truncated,
    args: Argv(),
    when: new Date().toISOString(),
    url: Page.location?.href ?? '',
    userAgent: Page.navigator?.userAgent ?? ''
  };
}

/* The body a report goes on the wire as, separated from sending it so that the
   trimming rule can be checked without a fetch.
 *
 * It copies rather than edits, and that is the whole of why it is a separate
 * function: the stored report keeps its recording and only the wire version has
 * to explain the absence. Store() happens before Post() today, so mutating in
 * place would be invisible -- which is exactly the kind of thing that stops
 * being true when somebody reorders two calls. */

export function Wire(Sending: Report): string {
  const Body = JSON.stringify(Sending);

  if(Body.length < MaxKeepaliveBytes)
    return Body;

  const Trimmed: Sent = { ...Sending, recording: null };

  Trimmed.recordingOmitted = 'too large to send with keepalive; '
                             + Sending.keys + ' keys held locally';

  return JSON.stringify(Trimmed);
}

function Post(Sending: Report): void {
  const Where = Endpoint();

  if(!Where)
    return;

  Safely('posting to ' + Where, () => {
    void fetch(Where, {
      method: 'POST',
      headers: { 'content-type': 'application/json' },
      body: Wire(Sending),
      keepalive: true,
      mode: 'cors'
    }).catch((Error: unknown) => {
      console.warn('ivanHarness: crash report POST failed: ' + Error);
    });
  }, undefined);
}

let Reported = false;

export function Take(What: string, Detail: unknown): void {
  if(Reported)
    return;

  Reported = true;

  const Made = Safely('building a report', () => Build(What, Detail), null);

  if(!Made) {
    console.error('ivan: ' + What + '\n' + String(Detail));
    return;
  }

  Store(Made);
  Post(Made);

  console.error(
    'ivan: ' + What + '\n' + Made.detail
    + '\n\n--- crash report ---'
    + '\nbuild   ' + Made.build
    + '\nseed    ' + Made.seed
    + '\nkeys    ' + Made.keys
    + '\nstored  ivanHarness.reports() / ivanHarness.save()'
    + (Made.recording
       ? '\n\nReproduce it natively:\n'
         + '  ivanHarness.saveRecording()   then\n'
         + '  ./ivan --replay session.rec\n\n'
         + Made.recording
       : '\n\nNo recording -- started with ?record=off.'));
}

/* ---- failure paths ---------------------------------------------------
   Three, and they do not overlap: onAbort for a runtime assertion, a rejected
   promise for a trap unwinding out of asyncify (which is the shape a wasm trap
   takes here, because ASYNCIFY means main runs inside a promise), and the error
   event for anything thrown synchronously on the main thread.

   Called by main.ts rather than run on import, so that importing this module
   does not require a Module or a window -- the same split music.ts made for its
   autoplay listener. It has to run before ivan.js, which the shell's script
   order guarantees: Module.arguments is read by the runtime at startup and
   setting it afterwards would silently lose the recording. */

export function Install(): void {
  Module.arguments = Argv();

  const Prior = Module.onAbort;

  Module.onAbort = (Reason: unknown) => {
    Take('aborted', Reason);
    Prior?.(Reason);
  };

  addEventListener('unhandledrejection', (Event_) => {
    const Reason: unknown = Event_.reason;
    Take('unhandled rejection',
         (Reason instanceof Error ? Reason.stack : null) ?? Reason);
  });

  addEventListener('error', (Event_) => {
    Take('uncaught error', Event_.error?.stack ?? Event_.message);
  });
}

export const Api: IvanHarness = {
  build: BuildId(),

  /* Functions rather than the values the --pre-js version exposed, because all
     three are derived from the query string and a captured copy could disagree
     with what the runtime was actually given. */

  endpoint: Endpoint,
  args: Argv,
  recordingPath: RecordPath,

  reports: Reports,
  clear: Clear,

  /* The live recording, whether or not anything has gone wrong -- useful for
     filing a bug about behaviour rather than a crash, and what the browser test
     counts keystrokes in. */

  text: Text,

  save: () => {
    const All = Reports();
    const Newest = All[0];

    if(!Newest) {
      console.warn('ivanHarness: no stored reports');
      return false;
    }

    return Download('ivan-crash-' + Newest.when.replace(/[:.]/g, '-') + '.json',
                    JSON.stringify(Newest, null, 2), 'application/json');
  },

  saveRecording: () => {
    const Recording = Text() ?? Reports()[0]?.recording;

    if(!Recording) {
      console.warn('ivanHarness: no recording');
      return false;
    }

    return Download('session.rec', Recording, 'text/plain');
  },

  /* Send a report for something that did not crash. */

  report: (Note: string) => {
    Reported = false;
    Take('reported by hand', Note || '');
  }
};

/* Test-only: Reported is module scope and node caches the module, so one test
   asserting the once-only rule would otherwise silence every test after it.
   Not on the global API -- nothing in the page or the C++ calls it. */

export function Reset(): void {
  Reported = false;
}
