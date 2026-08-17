/*
 * Tests for src/platform/query.ts. Run with: npm test
 *
 * The three shapes here are the three the four shipped files were each writing
 * for themselves, and the cases worth pinning are the ones where they differed:
 * `?sfx` with no value is not `?sfx=off`, and an absent option is on.
 */

import { test } from 'node:test';
import { strictEqual } from 'node:assert';
import { Enabled, Present, Setting } from '../src/platform/query.ts';

function Visit(Search: string): void {
  Object.defineProperty(globalThis, 'location', {
    configurable: true,
    value: { search: Search }
  });
}

test('an absent option is on', () => {
  Visit('');
  strictEqual(Enabled('sfx'), true);
});

test('=off is the off switch', () => {
  Visit('?sfx=off');
  strictEqual(Enabled('sfx'), false);
});

test('and nothing else is', () => {
  Visit('?sfx=no');
  strictEqual(Enabled('sfx'), true);

  Visit('?sfx');
  strictEqual(Enabled('sfx'), true);

  Visit('?sfx=OFF');
  strictEqual(Enabled('sfx'), true);
});

test('one option off leaves its neighbours alone', () => {
  Visit('?sfx=off&music=off&saves=off');
  strictEqual(Enabled('sfx'), false);
  strictEqual(Enabled('music'), false);
  strictEqual(Enabled('record'), true);
});

test('a value comes back verbatim', () => {
  Visit('?musicbase=https%3A%2F%2Fr2.example%2Fmusic%2F');
  strictEqual(Setting('musicbase'), 'https://r2.example/music/');
});

test('an absent value is null, not empty', () => {
  Visit('?sfx=off');
  strictEqual(Setting('musicbase'), null);
});

/* ?wipesaves takes no value, and a trailing = is a typo rather than a
   retraction -- getting this wrong means declining to wipe a save set that the
   player cannot get past. */

test('a valueless option is present', () => {
  Visit('?wipesaves');
  strictEqual(Present('wipesaves'), true);

  Visit('?wipesaves=');
  strictEqual(Present('wipesaves'), true);

  Visit('?saves=off');
  strictEqual(Present('wipesaves'), false);
});

/* Under node there is no location at all until a test sets one, and the harness
   itself imports these modules before any test body runs. */

test('no location is an empty query, not a throw', () => {
  Reflect.deleteProperty(globalThis, 'location');
  strictEqual(Enabled('sfx'), true);
  strictEqual(Setting('musicbase'), null);
  strictEqual(Present('wipesaves'), false);
});
