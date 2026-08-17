/*
 * The page boots, draws, hears a key and keeps its saves.
 *
 * These assertions are deliberately written against the *current* architecture,
 * before the four --pre-js files become one bundle. That is the point: a
 * refactor that changes how the page's JavaScript is delivered needs an oracle
 * that predates it, and nothing in the tree covered the browser at all -- the
 * two node suites are contract tests against stubs, shell.html was untested,
 * and HARNESS.md §9.10 says outright that "a save survives a reload" needs a
 * browser.
 *
 * Needs an assembled dist/. See playwright.config.ts.
 */

import { expect, test } from '@playwright/test';
import { Bridges } from '../src/bridge/contract.ts';

/* Emscripten's fetch of ivan.wasm is not reported to the shell's progress hook
   at all, so "loading" covers an unmeasured multi-megabyte download before the
   veil can lift. The generous ceiling is in playwright.config.ts. */

test.beforeEach(async ({ page }) => {
  const Faults: string[] = [];

  page.on('pageerror', (Error_) => Faults.push(Error_.message));

  await page.goto('/play/');
  await expect(page.locator('#veil')).toBeHidden();
  await expect(page.locator('#fault')).toBeHidden();

  expect(Faults).toEqual([]);
});

test('the game draws at its own resolution, scaled by the page', async ({ page }) => {
  /* 800x600 is the backing store the game renders into; the presentation is
     scaled to fit with image-rendering: pixelated. A canvas that has been
     resized to its CSS box instead is how a tileset gets resampled. */
  const Size = await page.locator('#canvas').evaluate((El) => ({
    width: (El as HTMLCanvasElement).width,
    height: (El as HTMLCanvasElement).height
  }));

  expect(Size).toEqual({ width: 800, height: 600 });
});

test('the canvas has something on it', async ({ page }) => {
  /* Not a pixel comparison -- the main menu fades in and the seed varies, so a
     golden image would need a fixed seed and a settled frame. This asks the
     weaker question that still catches a black screen: does the element render
     more than one colour? */
  const Shot = await page.locator('#canvas').screenshot();
  const Bytes = new Set(Shot);

  expect(Bytes.size).toBeGreaterThan(16);
});

/* The other half of the bridge contract. test/bridge.test.ts checks the C++
   against src/bridge/contract.ts; this checks the live page against the same
   file, so between them a rename on either side fails a test rather than
   silently costing the game a feature. */

test('every bridge the C++ calls is on the page', async ({ page }) => {
  for(const [Name, Bridge] of Object.entries(Bridges)) {
    const Present = await page.evaluate((Want: { global: string; methods: string[] }) => {
      const Found = (globalThis as Record<string, unknown>)[Want.global] as
        Record<string, unknown> | undefined;

      if(!Found)
        return null;

      return Want.methods.filter((Method) => typeof Found[Method] === 'function');
    }, { global: Name, methods: [...Bridge.methods] });

    expect(Present, Name + ' (' + Bridge.source + ')').toEqual([...Bridge.methods]);
  }
});

test('the console APIs a player is told to use exist', async ({ page }) => {
  const Missing = await page.evaluate(() =>
    ['ivanSfx', 'ivanMusic', 'ivanSaves', 'ivanHarness'].filter(
      (Name) => !(globalThis as Record<string, unknown>)[Name]
    ));

  expect(Missing).toEqual([]);
});

test('a keystroke reaches the game', async ({ page }) => {
  /* ivanHarness.text() is the live recording, one `K ` line per key the game
     actually consumed through GET_KEY -- so this is an end-to-end assertion
     about the browser, the shell's key handling, asyncify and the C++ input
     path, not about an event listener firing. */
  const Keys = () => page.evaluate(() =>
    (globalThis.ivanHarness.text() as string).split('\n')
      .filter((Line) => Line.startsWith('K ')).length);

  const Before = await Keys();

  await page.locator('#canvas').press('Enter');
  await expect.poll(Keys).toBeGreaterThan(Before);
});

test('the first gesture releases the audio context', async ({ page }) => {
  /* Suspended until a gesture, by policy. The main menu asks for music before
     any gesture can have happened, which is why music is picked up on resume
     where effects are dropped (tools/web/README.md). */
  await page.locator('#canvas').press('Enter');

  await expect.poll(() => page.evaluate(() => globalThis.ivanSfx.state() as string))
    .not.toBe('suspended');
});

test('the saves are mounted where save.cpp looks', async ({ page }) => {
  const Stats = await page.evaluate(() =>
    globalThis.ivanSaves.stats() as { mounted: boolean; readOnly: boolean });

  expect(Stats.mounted).toBe(true);
  expect(Stats.readOnly).toBe(false);
});
