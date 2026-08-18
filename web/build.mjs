/*
 * Bundle the page's own JavaScript (HARNESS.md §9).
 *
 *   node build.mjs [--outdir <dir>] [--watch] [--dev]
 *
 * One IIFE, loaded by the shell with an ordinary <script> before emcc's own
 * script tag. It is deliberately *not* an emcc --pre-js any more: the four
 * files this replaces were link inputs, so every edit to one of them cost a
 * full relink and CMake could only see them through a hand-maintained
 * LINK_DEPENDS string. An external bundle costs a rebuild measured in
 * milliseconds and cannot silently fail to be picked up.
 *
 * IIFE rather than ESM because the module is reached by name from the wasm
 * side: audio/audio.cpp and FeLib/Source/sfx.cpp call globalThis.ivanSfx and
 * friends out of EM_JS bodies, and a module's exports are not on globalThis.
 * The globals are assigned explicitly at the entry point; nothing else leaks.
 */

import { execFileSync } from 'node:child_process';
import { mkdirSync } from 'node:fs';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import * as esbuild from 'esbuild';

const Here = dirname(fileURLToPath(import.meta.url));
const Repo = resolve(Here, '..');

const Args = process.argv.slice(2);
const Watch = Args.includes('--watch');
const Dev = Args.includes('--dev');

/** `--outdir` with nothing after it is a typo, and resolving undefined throws
 * from inside path with no mention of the flag that caused it.
 *
 * @param {string[]} Argv
 * @returns {string} */

function OutDirFrom(Argv) {
  const At = Argv.indexOf('--outdir');

  if(At === -1)
    return join(Here, 'dist');

  const Given = Argv[At + 1];

  if(!Given || Given.startsWith('-')) {
    console.error('build.mjs: --outdir needs a directory');
    process.exit(2);
  }

  return resolve(Given);
}

const OutDir = OutDirFrom(Args);

/* The same build id harness-pre.js.in used to get from CMake's configure_file,
   so a crash report still names the build it came from. Taken from git rather
   than passed in, because the bundle is now built without CMake's involvement;
   IVAN_BUILD_ID in the environment overrides it, which is what CI and the
   CMake target will do. */

function BuildId() {
  if(process.env.IVAN_BUILD_ID)
    return process.env.IVAN_BUILD_ID;

  try {
    return execFileSync('git', ['describe', '--tags', '--always', '--dirty'],
                        { cwd: Repo, encoding: 'utf8' }).trim();
  } catch {
    return 'unknown';
  }
}

/* Annotated rather than left inferred: a bare object literal widens `format` and
   `legalComments` to string, so a typo in either would reach esbuild as a
   runtime error instead of a compile one. */

/** @type {import('esbuild').BuildOptions} */
const Options = {
  entryPoints: [join(Here, 'src', 'main.ts')],
  outfile: join(OutDir, 'ivan-page.js'),
  bundle: true,
  format: 'iife',
  target: 'es2020',
  sourcemap: true,
  minify: !Dev,
  legalComments: 'inline',
  logLevel: 'info',
  define: {
    IVAN_BUILD_ID: JSON.stringify(BuildId()),
    IVAN_CRASH_ENDPOINT: JSON.stringify(process.env.IVAN_CRASH_ENDPOINT ?? '')
  }
};

mkdirSync(OutDir, { recursive: true });

if(Watch) {
  const Context = await esbuild.context(Options);
  await Context.watch();
  console.log('watching ' + join(Here, 'src'));
} else {
  await esbuild.build(Options);
}
