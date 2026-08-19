/*
 * The bundle's entry point, and the only place a global is assigned.
 *
 * The globals below are the module's public surface in both directions: the
 * console API a developer reaches for, and the names the wasm side calls out of
 * EM_JS bodies. They are declared in bridge/globals.d.ts rather than here,
 * because the browser test asserts against the same declarations and cannot see
 * into this module.
 */

import { BuildId } from './platform/build.ts';
import * as Query from './platform/query.ts';
import * as Sfx from './audio/sfx.ts';
import * as Music from './audio/music.ts';
import * as Harness from './harness/report.ts';
import * as Saves from './saves/saves.ts';

/* What the browser test asserts against, so a module that fails to initialise
   is a failed assertion rather than a page quietly missing a feature. */

const Loaded: string[] = [];

globalThis.ivanPage = {
  build: BuildId(),
  modules: Loaded
};

if(!Query.Enabled('page'))
  console.warn('ivanPage: everything the page owns is off (?page=off)');

/* Assigned unconditionally, including under ?sfx=off: the module still records
   what it would have played, and sfx.cpp's EM_JS body reaches the object either
   way -- a missing global would be a silent no-op rather than a quiet game. */

globalThis.ivanSfx = Sfx.Api;
Loaded.push('sfx');

/* After sfx in this file only because it borrows that module's AudioContext --
   lazily, at the first track, so the order is not load-bearing. Install() is
   the autoplay listener, here rather than at music.ts's top level so that
   importing the module does not require a document. */

globalThis.ivanMusic = Music.Api;
Music.Install();
Loaded.push('music');

/* Install() has to happen before ivan.js rather than merely before a gesture:
   it puts the query string on Module.arguments, and the runtime reads that once,
   at startup. The shell's <script> order is what guarantees it.

   Deliberately outside the ?page=off warning above: that is a debugging switch,
   and one that also threw away --seed, --headless and the recording would take
   the instrument away at the moment it is being reached for. */

globalThis.ivanHarness = Harness.Api;
Harness.Install();
Loaded.push('harness');

/* Install() puts a hook on Module.preRun rather than on the document, so like
   the harness it depends on this bundle running before ivan.js.

   ?saves=off leaves the API assigned and reporting read-only, because a console
   asking why nothing saved should get an answer rather than a missing object. */

globalThis.ivanSaves = Saves.Api;
Saves.Install();
Loaded.push('saves');
