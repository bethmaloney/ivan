/*
 * Tests for src/harness/report.ts. Run with: npm test
 *
 * New coverage, not a port -- harness-pre.js had no test, and the thing it does
 * is hard to see working: every path runs while something else has already gone
 * wrong, and the whole module is written so that nothing in it can throw. A bug
 * in here does not fail loudly, it quietly produces a report with no
 * reproduction in it, which is indistinguishable from a crash that had none.
 *
 * The fixture below is a real recording header (harness.cpp:303) rather than an
 * invented one, because the seed and the version are parsed back out of it with
 * a regex and a made-up shape would test the regex against itself.
 */

import { test } from 'node:test';
import { deepStrictEqual, match, ok, strictEqual } from 'node:assert';
import { Api, Build, Endpoint, Reports, Reset, Take, Text, Wire } from './report.ts';

const Header = '# written by tools/play/play.py\n'
               + 'ivan-record 1 seed=999 ivan=0.59\n';

const Recording = Header
                  + 'K 1 0 0 13\t# enter\n'
                  + 'K 2 0 0 13\t# enter\n'
                  + 'K 3 0 0 106\t# j\n';

interface World {
  Stored: Map<string, string>;
  Errors: string[];
  Warnings: string[];
  Posts: { Where: string; Body: string; Keepalive: boolean }[];
}

interface Fake {
  Search?: string;
  Recording?: string | null;
  Endpoint?: string;

  /* No FS at all is a crash before the runtime attached it, and localStorage
     that throws is a browser in private mode with the quota at zero. Both are
     paths a report has to survive rather than add to. */

  NoFS?: boolean;
  StorageFails?: boolean;
}

function Define(Name: string, Value: unknown): void {
  Object.defineProperty(globalThis, Name, { configurable: true, value: Value });
}

function Page(Options: Fake = {}): World {
  const World_: World = { Stored: new Map(), Errors: [], Warnings: [], Posts: [] };
  const Recorded = Options.Recording === undefined ? Recording : Options.Recording;

  Reset();

  Define('location', {
    search: Options.Search ?? '',
    href: 'https://playivan.pages.dev/play/' + (Options.Search ?? '')
  });

  Define('navigator', { userAgent: 'test-agent' });

  Define('localStorage', {
    getItem: (Key: string) => World_.Stored.get(Key) ?? null,
    setItem: (Key: string, Value: string) => {
      if(Options.StorageFails)
        throw new Error('QuotaExceededError');

      World_.Stored.set(Key, Value);
    },
    removeItem: (Key: string) => void World_.Stored.delete(Key)
  });

  Define('Module', Options.NoFS ? {} : {
    FS: {
      readFile: (Path: string) => {
        if(Recorded === null)
          throw new Error('ENOENT: ' + Path);

        return new TextEncoder().encode(Recorded);
      }
    }
  });

  Define('fetch', (Where: string, Init: RequestInit) => {
    World_.Posts.push({
      Where,
      Body: String(Init.body),
      Keepalive: Init.keepalive === true
    });

    return Promise.resolve(new Response(''));
  });

  Define('IVAN_CRASH_ENDPOINT', Options.Endpoint ?? '');

  console.error = (Message: string) => void World_.Errors.push(Message);
  console.warn = (Message: string) => void World_.Warnings.push(Message);

  return World_;
}

function Stored(World_: World): unknown[] {
  return JSON.parse(World_.Stored.get('ivan.crashReports') ?? '[]') as unknown[];
}

/* ---- what makes a report worth having --------------------------------- */

test('the seed and the version are parsed back out of the recording', () => {
  Page();

  const Report = Build('aborted', 'assertion failed');

  strictEqual(Report.seed, '999');
  strictEqual(Report.ivan, '0.59');
  strictEqual(Report.recording, Recording);
});

test('the key count is the keys, not the lines', () => {
  Page();

  /* Three K lines against six lines of file: a comment header and a tab-separated
     trailing comment on every key. Counting lines would overstate it and make a
     report look like a longer session than it was. */

  strictEqual(Build('aborted', '').keys, 3);
});

test('what went wrong and where it went wrong are both in the report', () => {
  Page({ Search: '?seed=999' });

  const Report = Build('unhandled rejection', 'RuntimeError: unreachable');

  strictEqual(Report.what, 'unhandled rejection');
  strictEqual(Report.detail, 'RuntimeError: unreachable');
  strictEqual(Report.url, 'https://playivan.pages.dev/play/?seed=999');
  strictEqual(Report.userAgent, 'test-agent');
  deepStrictEqual(Report.args, ['--seed', '999', '--record', '/session.rec']);
  match(Report.when, /^\d{4}-\d\d-\d\dT/);
});

/* A detail is whatever the failing path happened to be holding -- an Error, a
   string, a rejected promise's undefined reason. String(null) is "null", which is
   at least honest; a report with `detail: undefined` does not survive JSON. */

test('a missing detail is a string, not undefined', () => {
  Page();
  strictEqual(Build('aborted', null).detail, '');
  strictEqual(Build('aborted', undefined).detail, '');
});

/* ---- ?record=off ------------------------------------------------------ */

test('under ?record=off there is no recording and the report says so', () => {
  const World_ = Page({ Search: '?record=off' });

  strictEqual(Text(), null);

  const Report = Build('aborted', 'x');

  strictEqual(Report.recording, null);
  strictEqual(Report.seed, null);
  strictEqual(Report.keys, 0);

  Take('aborted', 'x');
  match(World_.Errors[0] ?? '', /No recording -- started with \?record=off\./);
});

/* ---- truncation ------------------------------------------------------- */

test('an oversized recording keeps its head, so the seed survives the cut', () => {
  const Long = Header + 'K 1 0 0 13\t# enter\n'.repeat(40000);

  ok(Long.length > 512 * 1024, 'fixture must exceed the cap to test it');

  const World_ = Page({ Recording: Long });
  const Report = Build('aborted', 'x');

  strictEqual(Report.recordingTruncated, true);
  strictEqual(Report.recording?.length, 512 * 1024);

  /* The point of keeping the head rather than the tail: a truncated report is
     still a reproducible one, because --replay needs the seed. */

  strictEqual(Report.seed, '999');
  strictEqual(World_.Warnings.length, 0);
});

test('a recording under the cap is not flagged', () => {
  Page();
  strictEqual(Build('aborted', 'x').recordingTruncated, false);
});

/* ---- storage ---------------------------------------------------------- */

test('reports are stored newest first and capped', () => {
  const World_ = Page();

  for(let Nth = 0; Nth < 7; ++Nth) {
    Reset();
    Take('aborted', 'crash ' + Nth);
  }

  const All = Stored(World_) as { detail: string }[];

  strictEqual(All.length, 5);
  strictEqual(All[0]?.detail, 'crash 6');
  strictEqual(All[4]?.detail, 'crash 2');
});

test('a report is taken once, so the first cause is the one kept', () => {
  const World_ = Page();

  Take('aborted', 'the real cause');
  Take('uncaught error', 'the knock-on');

  const All = Stored(World_) as { detail: string }[];

  strictEqual(All.length, 1);
  strictEqual(All[0]?.detail, 'the real cause');
});

test('report() by hand reopens it, so a second bug can be filed', () => {
  const World_ = Page();

  Take('aborted', 'the real cause');
  Api.report('and this looked odd too');

  strictEqual(Stored(World_).length, 2);
});

test('clear() forgets them', () => {
  const World_ = Page();

  Take('aborted', 'x');
  strictEqual(Reports().length, 1);

  Api.clear();
  strictEqual(Reports().length, 0);
  strictEqual(World_.Stored.has('ivan.crashReports'), false);
});

test('a stored value that is not an array reads as none', () => {
  const World_ = Page();

  World_.Stored.set('ivan.crashReports', '{"not":"an array"}');
  deepStrictEqual(Reports(), []);
});

/* ---- nothing here may throw ------------------------------------------- */

test('storage that refuses still leaves the report on the console', () => {
  const World_ = Page({ StorageFails: true });

  Take('aborted', 'assertion failed');

  strictEqual(Stored(World_).length, 0);
  match(World_.Warnings[0] ?? '', /storing a report failed/);

  /* The half that matters: the recording is printed, so it is recoverable by
     hand from a browser that will not store it. */

  match(World_.Errors[0] ?? '', /ivan-record 1 seed=999/);
});

test('a crash before the runtime attached FS reports without a recording', () => {
  const World_ = Page({ NoFS: true });

  strictEqual(Text(), null);

  Take('aborted', 'died in the loader');

  const All = Stored(World_) as { recording: string | null; detail: string }[];

  strictEqual(All.length, 1);
  strictEqual(All[0]?.recording, null);
  strictEqual(All[0]?.detail, 'died in the loader');
});

test('a recording that cannot be read is a warning, not a second failure', () => {
  const World_ = Page({ Recording: null });

  strictEqual(Text(), null);
  match(World_.Warnings[0] ?? '', /reading \/session\.rec failed/);
});

/* ---- the endpoint ----------------------------------------------------- */

test('no endpoint configured posts nothing', () => {
  const World_ = Page();

  strictEqual(Endpoint(), '');

  Take('aborted', 'x');

  deepStrictEqual(World_.Posts, []);
});

test('?crashlog redirects the post without a rebuild', () => {
  const World_ = Page({
    Search: '?crashlog=https%3A%2F%2Fexample.com%2Fc',
    Endpoint: 'https://built-in.example/c'
  });

  strictEqual(Endpoint(), 'https://example.com/c');

  Take('aborted', 'x');

  strictEqual(World_.Posts.length, 1);
  strictEqual(World_.Posts[0]?.Where, 'https://example.com/c');
  strictEqual(World_.Posts[0]?.Keepalive, true);
});

/* A bare = is a typo. Reading it as "post nowhere" would silently switch off
   reporting for the session it was typed in. */

test('a bare ?crashlog= falls back to the built-in endpoint', () => {
  Page({ Search: '?crashlog=', Endpoint: 'https://built-in.example/c' });
  strictEqual(Endpoint(), 'https://built-in.example/c');
});

test('a post that fits keeps its recording', () => {
  const World_ = Page({ Endpoint: 'https://built-in.example/c' });

  Take('aborted', 'x');

  const Sent = JSON.parse(World_.Posts[0]?.Body ?? '{}') as { recording: string | null };

  strictEqual(Sent.recording, Recording);
});

/* keepalive caps the body at 64KB, and a request that exceeds it is rejected
   outright -- so the choice is between a report without its recording and no
   report at all. The key count goes in its place, and the recording is still in
   localStorage for ivanHarness.saveRecording(). */

test('a post too large for keepalive drops the recording and says so', () => {
  const Long = Header + 'K 1 0 0 13\t# enter\n'.repeat(4000);
  const World_ = Page({ Recording: Long, Endpoint: 'https://built-in.example/c' });

  Take('aborted', 'x');

  const Sent = JSON.parse(World_.Posts[0]?.Body ?? '{}') as
    { recording: string | null; recordingOmitted?: string };

  strictEqual(Sent.recording, null);
  match(Sent.recordingOmitted ?? '', /4000 keys held locally/);
  ok(World_.Posts[0] !== undefined && World_.Posts[0].Body.length < 64 * 1024);

  /* The stored copy is untouched, so the recording is still there for
     ivanHarness.saveRecording() after a report was posted without it. */

  const Kept = Stored(World_) as { recording: string | null }[];

  strictEqual(Kept[0]?.recording, Long);
});

/* Asserted against Wire() rather than through Take(), because Store() runs before
   Post() and a Post() that edited its argument in place would look correct from
   the outside. That ordering is not a guarantee anybody wrote down. */

test('building the wire body leaves the report it was given alone', () => {
  Page();

  const Report = Build('aborted', 'x');

  Report.recording = Header + 'K 1 0 0 13\t# enter\n'.repeat(4000);
  Report.keys = 4000;

  const Body = JSON.parse(Wire(Report)) as { recording: string | null };

  strictEqual(Body.recording, null);
  ok(Report.recording !== null, 'the report keeps its own recording');
  strictEqual(Report.recording.length, Header.length + 19 * 4000);
});

/* ---- the console API -------------------------------------------------- */

test('text() is the live recording, crash or no crash', () => {
  Page();
  strictEqual(Api.text(), Recording);
});

test('the three query-derived fields are read when asked, not at load', () => {
  Page({ Search: '?seed=1&record=/mine.rec' });

  deepStrictEqual(Api.args(), ['--seed', '1', '--record', '/mine.rec']);
  strictEqual(Api.recordingPath(), '/mine.rec');

  Page({ Search: '?record=off' });

  deepStrictEqual(Api.args(), []);
  strictEqual(Api.recordingPath(), null);
});

test('save() and saveRecording() say no rather than nothing', () => {
  const World_ = Page({ NoFS: true });

  strictEqual(Api.save(), false);
  match(World_.Warnings.join('\n'), /no stored reports/);

  strictEqual(Api.saveRecording(), false);
  match(World_.Warnings.join('\n'), /no recording/);
});

/* After a reload the module is fresh and MEMFS is empty, but the report that
   caused the reload is still in localStorage -- which is the case the whole
   storage half exists for. */

test('saveRecording() falls back to the stored report after a reload', () => {
  const Before = Page();

  Take('aborted', 'x');

  const Survived = Before.Stored.get('ivan.crashReports') ?? '';

  ok(Survived.includes('ivan-record 1 seed=999'), 'the report must hold the recording');

  /* The reload: a fresh module, an empty MEMFS, and only localStorage carried
     across. */

  const After = Page({ NoFS: true });

  After.Stored.set('ivan.crashReports', Survived);
  strictEqual(Text(), null);

  /* No document under node, so the download itself cannot succeed. What is
     asserted is that it got as far as trying, which means it found the recording
     in the stored report rather than giving up at the empty MEMFS. */

  strictEqual(Api.saveRecording(), false);
  match(After.Warnings.join('\n'), /saving session\.rec failed/);
});
