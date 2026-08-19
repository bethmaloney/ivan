/*
 * The page boots, draws, hears a key and keeps its saves.
 *
 * These assertions were written against the architecture that predates the
 * bundle, while all four of the page's files were still --pre-js inputs, and
 * that was the point: a refactor that changes how the page's JavaScript is
 * delivered needs an oracle older than the refactor. All four have crossed since
 * and every assertion here still holds, which is the only evidence anybody has
 * that the delivery change was behaviour-preserving -- nothing else in the tree
 * covers the browser at all. The node suites are contract tests against stubs,
 * shell.html is untested, and HARNESS.md §9.10 says outright that "a save
 * survives a reload" needs a browser.
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

/* main.ts appends to this as each module initialises, so a module that threw on
   the way up is a failed assertion here rather than a page that is quietly
   missing a feature -- which is the failure class the whole port has to avoid,
   since a headless replay cannot hear the difference. */

test('every module that has crossed announced itself', async ({ page }) => {
  const Announced = await page.evaluate(() => ({
    build: globalThis.ivanPage.build,
    modules: globalThis.ivanPage.modules
  }));

  expect(Announced.modules).toEqual(['sfx', 'music', 'harness', 'saves']);
  expect(Announced.build).not.toBe('');
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

/* The one thing that could have broken silently when the harness stopped being a
   --pre-js. Module.arguments is read by the runtime once, at startup, so a bundle
   that assigned it a moment too late would lose the seed and the recording with
   no error anywhere -- and nothing else in the tree would notice, because a
   recording nobody asked for is not missed until a crash needs one.

   ?seed=999 coming back out of the recording header is proof of the whole chain:
   the bundle ran before ivan.js, ParseArgs saw the argv it built, and the seed it
   pinned is the one the header now offers to --replay. */

test('the query string reaches the game as argv', async ({ page }) => {
  await page.goto('/play/?seed=999');
  await expect(page.locator('#veil')).toBeHidden();

  expect(await page.evaluate(() => globalThis.ivanHarness.args()))
    .toEqual(['--seed', '999', '--record', '/session.rec']);

  /* Written by harness.cpp:303 from the seed ParseArgs pinned, not echoed back
     from the page. */

  expect(await page.evaluate(() => globalThis.ivanHarness.text()))
    .toContain('ivan-record 1 seed=999');
});

test('the first gesture releases the audio context', async ({ page }) => {
  /* Suspended until a gesture, by policy. The main menu asks for music before
     any gesture can have happened, which is why music is picked up on resume
     where effects are dropped (tools/web/README.md). */
  await page.locator('#canvas').press('Enter');

  await expect.poll(() => page.evaluate(() => globalThis.ivanSfx.state() as string))
    .not.toBe('suspended');
});

/* The crash path, which has no coverage anywhere else and cannot get any without
   a browser: report.test.ts stubs localStorage, MEMFS and fetch, so between them
   the node cases check every rule and none of the three real things.
 *
 * Reported by hand rather than by crashing on purpose. A real trap would leave
 * the module dead and the rest of this file has to keep running, and the code
 * under test is the same either way -- ivanHarness.report() reaches Take()
 * through the identical path onAbort does. */

test('a crash report is built, stored and posted', async ({ page }) => {
  const Posted: string[] = [];

  await page.route('https://collector.test/c', async (Route) => {
    Posted.push(Route.request().postData() ?? '');
    await Route.fulfill({ status: 204, body: '' });
  });

  await page.goto('/play/?seed=4242&crashlog=https%3A%2F%2Fcollector.test%2Fc');
  await expect(page.locator('#veil')).toBeHidden();
  await page.locator('#canvas').press('Enter');

  /* The game consumes a key when it next reaches GET_KEY, not when the event
     fires, so the recording is a line longer some moments after the press. */

  await expect.poll(() => page.evaluate(() =>
    (globalThis.ivanHarness.text() ?? '').split('\n')
      .filter((Line) => Line.startsWith('K ')).length)).toBeGreaterThan(0);

  await page.evaluate(() => globalThis.ivanHarness.report('a report by hand'));

  const Stored = await page.evaluate(() => globalThis.ivanHarness.reports());

  expect(Stored.length).toBe(1);

  /* Parsed back out of a header the C++ wrote, so this is the assertion that the
     report carries a reproduction rather than a description of one. */

  expect(Stored[0]?.seed).toBe('4242');
  expect(Stored[0]?.keys).toBeGreaterThan(0);
  expect(Stored[0]?.build).not.toBe('unknown');

  await expect.poll(() => Posted.length).toBe(1);
  expect(JSON.parse(Posted[0] ?? '{}')).toMatchObject({ seed: '4242' });

  /* Why localStorage and not a variable: a crash is usually followed by the
     reload or the closed tab that would take the report with it. */

  await page.reload();
  await expect(page.locator('#veil')).toBeHidden();
  expect(await page.evaluate(() => globalThis.ivanHarness.reports().length)).toBe(1);
});

test('the saves are mounted where save.cpp looks', async ({ page }) => {
  const Stats = await page.evaluate(() => globalThis.ivanSaves.stats());

  expect(Stats.mounted).toBe(true);
  expect(Stats.readOnly).toBe(false);
});

/* The one thing that could have broken silently when saves stopped being a
   --pre-js, and the reason the link flags now name the pair outright. From the
   bundle the run dependency is a property on Module rather than a name in
   ivan.js's own scope, and a build that lost it would not fail anywhere: the
   mount would still happen, just after main() had drawn a Continue menu over
   saves that had not arrived. Nothing in the node suite can see it, because its
   Module is whatever the test defines.

   Counted rather than inferred. shell.html's monitorRunDependencies keeps the
   high-water mark of what startup was waiting for, and it is 2 here --
   ivan.data's and the saves'. Verified by mutation, which is the only reason to
   prefer it to the obvious assertions: with the hold removed the page still
   boots, still mounts, still syncs and still survives a reload, and every other
   test in this file still passes. This one reads 1. */

test('the game was held back while the saves were read', async ({ page }) => {
  const Held = await page.evaluate(() => globalThis.Module.totalDependencies);
  const Stats = await page.evaluate(() => globalThis.ivanSaves.stats());

  expect(Held).toBe(2);
  expect(Stats.failures).toBe(0);
  expect(Stats.lastError).toBe(null);
});

/* The assertion HARNESS.md §9.10 says needs a browser, and the one the node
   suite structurally cannot make: its FS is a stub, so it can only prove that a
   sync was asked for, never that anything came back. This writes into the mount
   the way the game does, flushes, reloads, and finds it -- the whole round trip
   through a real IDBFS, which is the half the stub cannot reach. The ordering
   that puts the saves there *before* the game looks is the test above this one;
   this only says they came back.

   Written through Module.FS rather than by playing, because a real save needs a
   character created through several menus and the file it produces is a
   megabyte. What is under test is the mount and the sync, and those cannot tell
   the two apart. */

test('a save survives a reload', async ({ page }) => {
  const Written = await page.evaluate(async () => {
    globalThis.Module.FS?.writeFile('/ivan/probe.sav', 'Belyer of Attnam');
    await globalThis.ivanSaves.flush();

    return globalThis.ivanSaves.stats();
  });

  expect(Written.failures).toBe(0);
  expect(Written.syncs).toBeGreaterThan(0);

  await page.reload();
  await expect(page.locator('#veil')).toBeHidden();

  const Found = await page.evaluate(() => ({
    Files: globalThis.ivanSaves.files().map((F) => F.path),
    Text: new TextDecoder().decode(
      globalThis.Module.FS?.readFile('/ivan/probe.sav') ?? new Uint8Array())
  }));

  expect(Found.Files).toContain('/ivan/probe.sav');
  expect(Found.Text).toBe('Belyer of Attnam');

  /* Left behind it would be the next run's mount, and this suite shares one
     IndexedDB origin (playwright.config.ts pins it to one worker for that
     reason). */

  await page.evaluate(async () => {
    globalThis.Module.FS?.unlink('/ivan/probe.sav');
    await globalThis.ivanSaves.flush();
  });
});

/* ?saves=off is the switch a player is told to reach for when a save will not
   load, so it has to leave a page that plays rather than a page that throws. */

test('?saves=off plays in a scratch filesystem', async ({ page }) => {
  await page.goto('/play/?saves=off');
  await expect(page.locator('#veil')).toBeHidden();

  const Stats = await page.evaluate(() => globalThis.ivanSaves.stats());

  expect(Stats.mounted).toBe(false);
  expect(Stats.readOnly).toBe(true);
});
