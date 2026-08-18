/*
 * The bundle's entry point, and the only place a global is assigned.
 *
 * Load order used to be the order of --pre-js flags in CMakeLists.txt, which is
 * why music.js carried a comment saying it must come after sfx.js. It is an
 * import now, so the graph says it instead of a build file.
 *
 * The globals below are the module's public surface in both directions: the
 * console API a player or a developer reaches for, and the names the wasm side
 * calls out of EM_JS bodies. Nothing else is exported to the page.
 */

import * as Query from './platform/query.ts';

declare global {
  /* `var` is not a style choice here: it is the only declaration form that
     puts a name on globalThis, which is what an EM_JS body looks it up on. */
  var ivanPage: {
    build: string;
    modules: string[];
  };
}

/* Modules land here as they cross: sfx, music, saves, harness, then graphics,
   input and the UI. The list is what the browser test asserts against, so a
   module that fails to initialise is a failed assertion rather than a page
   that is quietly missing a feature. */

const Loaded: string[] = [];

globalThis.ivanPage = {
  build: IVAN_BUILD_ID,
  modules: Loaded
};

if(!Query.Enabled('page'))
  console.warn('ivanPage: everything the page owns is off (?page=off)');
