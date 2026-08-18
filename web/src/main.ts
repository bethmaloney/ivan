/*
 * The bundle's entry point, and the only place a global is assigned.
 *
 * Load order used to be the order of --pre-js flags in CMakeLists.txt, which is
 * why music.js carried a comment saying it must come after sfx.js. It is an
 * import now, so the graph says it instead of a build file.
 *
 * The globals below are the module's public surface in both directions: the
 * console API a player or a developer reaches for, and the names the wasm side
 * calls out of EM_JS bodies. Nothing else is exported to the page. They are
 * declared in bridge/globals.d.ts rather than here, because the browser test
 * asserts against the same declarations and cannot see into this module.
 */

import * as Query from './platform/query.ts';
import * as Sfx from './audio/sfx.ts';
import * as Music from './audio/music.ts';

/* Modules land here as they cross: sfx and music have, saves and harness have
   not, then graphics, input and the UI. The list is what the browser test
   asserts against, so a module that fails to initialise is a failed assertion
   rather than a page that is quietly missing a feature. */

const Loaded: string[] = [];

globalThis.ivanPage = {
  build: IVAN_BUILD_ID,
  modules: Loaded
};

if(!Query.Enabled('page'))
  console.warn('ivanPage: everything the page owns is off (?page=off)');

/* Sound effects (HARNESS.md §9.7). Assigned unconditionally, including under
   ?sfx=off: the module still records what it would have played, and sfx.cpp's
   EM_JS body reaches the object either way -- a missing global would be a
   silent no-op rather than a quiet game. */

globalThis.ivanSfx = Sfx.Api;
Loaded.push('sfx');

/* Music (HARNESS.md §9.8). After sfx in this file only because it reads
   globalThis.ivanSfx to borrow the context -- lazily, at the first track, so the
   order is not load-bearing the way it was when these were two --pre-js flags.

   Install() is the autoplay listener the module wants registered before any
   gesture, and it is here rather than at music.ts's top level so that importing
   the module does not require a document. */

globalThis.ivanMusic = Music.Api;
Music.Install();
Loaded.push('music');
