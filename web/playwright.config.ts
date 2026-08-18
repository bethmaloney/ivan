/*
 * The browser oracle (HARNESS.md §1: the two seams).
 *
 * The committed corpora prove the *game* still plays the way it did, and they
 * do it by hashing the C++ bitmap double buffer before it reaches a texture
 * (§6.3, §9.3). That works only while rendering is C++. As graphics, input and
 * the UI cross into this directory, the subject of that hash crosses with them
 * and the golden traces stop being evidence about what a player sees -- they
 * will keep passing and cover less. This is what replaces the part they lose.
 *
 * It runs against an assembled dist/ rather than a dev server, so what it
 * exercises is the thing that gets deployed: real wasm, real IDBFS, real
 * autoplay policy. That means it needs a browser build first, and it says so
 * rather than failing obscurely:
 *
 *   emcmake cmake -S . -B build-web -DCMAKE_BUILD_TYPE=Release \
 *     -DWIZARD=ON -DPORTABLE_BUILD=ON -DWASM_BROWSER=ON
 *   cmake --build build-web -j$(nproc)
 *   tools/web/dist.py
 *   npm run e2e
 *
 * serve.py is the server on purpose. It answers byte ranges where Cloudflare
 * Pages does not (tools/web/README.md), so it is the more forgiving of the two
 * -- a range-related failure here would be a real one there as well.
 */

import { defineConfig, devices } from '@playwright/test';

const Port = 8114;

export default defineConfig({
  testDir: './e2e',

  /* A wasm game that boots through a multi-megabyte download and a world
     generation is not a unit test. The default 30s expect timeout is too short
     for "the main menu is up" and far too short for "a game has started". */
  timeout: 120_000,
  expect: { timeout: 30_000 },

  /* One worker: the tests share one IndexedDB origin, and a save test racing a
     wipe test is a flake nobody can reproduce. */
  workers: 1,
  fullyParallel: false,
  forbidOnly: !!process.env.CI,
  retries: 0,

  reporter: process.env.CI ? [['list'], ['html', { open: 'never' }]] : 'list',

  use: {
    baseURL: `http://127.0.0.1:${Port}`,
    trace: 'retain-on-failure',
    video: 'retain-on-failure'
  },

  projects: [
    { name: 'chromium', use: { ...devices['Desktop Chrome'] } }
  ],

  webServer: {
    command: `../tools/web/serve.py ${Port} ../dist`,
    url: `http://127.0.0.1:${Port}/play/`,
    reuseExistingServer: !process.env.CI,
    stdout: 'pipe',
    stderr: 'pipe'
  }
});
