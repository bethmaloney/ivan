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

import { BuildId } from './platform/build.ts';
import * as Query from './platform/query.ts';
import * as Sfx from './audio/sfx.ts';
import * as Music from './audio/music.ts';
import * as Harness from './harness/report.ts';
import * as Saves from './saves/saves.ts';

/* Modules land here as they cross: sfx, music, the harness and saves have,
   then graphics, input and the UI. The list is what the browser test asserts
   against, so a module that fails to initialise is a failed assertion rather
   than a page that is quietly missing a feature. */

const Loaded: string[] = [];

globalThis.ivanPage = {
  build: BuildId(),
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

/* The harness (HARNESS.md §4, §9.6). Last of the three and the only one whose
   Install() has to happen before ivan.js rather than merely before a gesture: it
   puts the query string on Module.arguments, and the runtime reads that once, at
   startup. The shell's <script> order is what guarantees it.

   Deliberately outside the ?page=off warning above, like the other two but for a
   different reason: ?page=off is a debugging switch, and a switch that also threw
   away --seed, --headless and the recording would take the instrument away at the
   moment it is being reached for. */

globalThis.ivanHarness = Harness.Api;
Harness.Install();
Loaded.push('harness');

/* Saves (HARNESS.md §9.10). Last across, and the only one whose Install() puts
   a hook on Module.preRun rather than on the document -- so, like the harness,
   it depends on this bundle running before ivan.js and not merely before a
   gesture. The shell's <script> order is what guarantees that too.

   Outside the ?page=off warning as well, and unlike the other three the switch
   it does answer to is its own: ?saves=off leaves the API assigned and reporting
   read-only, because a console asking why nothing saved should get an answer
   rather than a missing object. */

globalThis.ivanSaves = Saves.Api;
Saves.Install();
Loaded.push('saves');
