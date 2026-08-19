/*
 * Tests for src/harness/argv.ts. Run with: npm test
 *
 * New coverage, not a port -- harness-pre.js had no test. What these pin is the
 * behaviour a reader would otherwise be free to "fix": recording is on when
 * nobody asked, `?record=` is a request rather than a filename, and every
 * unrecognised option is forwarded to a C++ parser that ignores it on purpose.
 */

import { test } from 'node:test';
import { deepStrictEqual, strictEqual } from 'node:assert';
import { Argv, DefaultRecordPath, RecordPath } from './argv.ts';

function Visit(Search: string): void {
  Object.defineProperty(globalThis, 'location', {
    configurable: true,
    value: { search: Search }
  });
}

/* The default is the whole design: a crash is only reproducible if the session
   was already being recorded when nobody had any reason to expect one. */

test('recording is on when nobody asked', () => {
  Visit('');
  deepStrictEqual(Argv(), ['--record', DefaultRecordPath]);
  strictEqual(RecordPath(), DefaultRecordPath);
});

test('?record=off is the only way to have no recording', () => {
  Visit('?record=off');
  deepStrictEqual(Argv(), []);
  strictEqual(RecordPath(), null);
});

test('?record names the file', () => {
  Visit('?record=/mine.rec');
  deepStrictEqual(Argv(), ['--record', '/mine.rec']);
  strictEqual(RecordPath(), '/mine.rec');
});

/* A bare = is a typo, and honouring it would open a recording called "" and
   then hand ParseArgs an empty filename. */

test('a bare ?record= falls back to the default rather than an empty name', () => {
  Visit('?record=');
  deepStrictEqual(Argv(), ['--record', DefaultRecordPath]);
  strictEqual(RecordPath(), DefaultRecordPath);
});

test('a valueless option is a flag with no value', () => {
  Visit('?headless&record=off');
  deepStrictEqual(Argv(), ['--headless']);
});

test('an option with a value is two entries', () => {
  Visit('?seed=999&record=off');
  deepStrictEqual(Argv(), ['--seed', '999']);
});

/* ParseArgs walks argv in order, so the order the URL gave them is the order it
   sees -- which is what makes a query string a faithful stand-in for a command
   line rather than an unordered bag. */

test('URL order is argv order', () => {
  Visit('?seed=999&headless&text=/out.log&record=off');
  deepStrictEqual(Argv(), ['--seed', '999', '--headless', '--text', '/out.log']);
});

test('the recording flag comes last, after whatever the URL asked for', () => {
  Visit('?seed=1&headless');
  deepStrictEqual(Argv(),
                  ['--seed', '1', '--headless', '--record', DefaultRecordPath]);
});

/* Both are answered by the page and mean nothing to ParseArgs. --record is
   added by RecordPath() instead, so forwarding ?record as well would pass it
   twice and let the second win. */

test('record and crashlog are not forwarded', () => {
  Visit('?record=/mine.rec&crashlog=https%3A%2F%2Fexample.com%2Fc&seed=7');
  deepStrictEqual(Argv(), ['--seed', '7', '--record', '/mine.rec']);
});

/* Deliberate, and the reason is on the C++ side: harness::ParseArgs is an
   if/else-if chain with no else (harness.cpp:357), so an option it does not know
   is ignored rather than rejected. Filtering here would mean a second list of
   page options to keep in step with platform/query.ts, and a new one forgotten
   there would silently stop reaching the game. */

test('page options are forwarded, because ParseArgs ignores what it does not know', () => {
  Visit('?sfx=off&wipesaves&record=off');
  deepStrictEqual(Argv(), ['--sfx', 'off', '--wipesaves']);
});

test('a repeated option is forwarded twice, as a command line would allow', () => {
  Visit('?seed=1&seed=2&record=off');
  deepStrictEqual(Argv(), ['--seed', '1', '--seed', '2']);
});

test('no location at all is an empty query, not a throw', () => {
  Reflect.deleteProperty(globalThis, 'location');
  deepStrictEqual(Argv(), ['--record', DefaultRecordPath]);
});
