# web/ — the browser frontend

The half of the port that is not C++. `Main/` decides what happens; this decides
what the player sees, hears and types. `tools/web/` is the build tooling that
assembles and serves it (`dist.py`, `serve.py`); everything the browser actually
runs lives here.

```bash
cd web
npm ci
npm run check        # typecheck + lint + tests. No browser, ~2s
npm run build        # -> dist/ivan-page.js
npm run e2e          # the browser suite. Needs an assembled ../dist (below)
```

## Why the JavaScript is not a `--pre-js` any more

It was, and that is what this replaces. The four files in `tools/web/` were emcc
link inputs, which had three costs:

- **Every edit cost a relink.** CMake could only see them through a
  hand-maintained `LINK_DEPENDS` string, and the failure mode when that string is
  wrong is silent — the build reports itself up to date and the page keeps
  serving the previous copy.
- **The module graph was the order of flags in `CMakeLists.txt`.** `music.js`
  borrows the `AudioContext` that `sfx.js` owns, and nothing but the order of two
  `--pre-js` arguments enforced it. It is an `import` now.
- **No bundler could exist**, so no TypeScript, no npm library, no source map,
  and no content-hashed filename — which is why `dist.py` has to send `no-cache`
  on the JS it deploys.

What is left of the coupling is small and deliberate: the bundle is an IIFE that
assigns a handful of globals, because the wasm side reaches them by name out of
`EM_JS` bodies (see below), and `saves.js` needs `addRunDependency` /
`removeRunDependency`, which are `EXPORTED_RUNTIME_METHODS` rather than
inlined-scope internals.

## The toolchain, and why each piece

| | |
|---|---|
| **esbuild** | bundles and strips types. No config file, sub-100ms. Vite's value is a dev server with HMR, and a game that boots through a multi-megabyte wasm download cannot usefully hot-reload. |
| **tsc `--noEmit`** | the only type checker. esbuild does not check, and oxlint does not either. TypeScript 7, which is the native compiler — both projects check in 0.24s. |
| **oxlint** | one binary, no plugin tree. Correctness rules only. |
| **`node --test`** | built into Node 22, which also strips the types — so the contract tests need no runner, no transform and no dependency. |
| **Playwright** | the only thing in the repo that tests a browser. |

**No Prettier, and that is not an oversight.** This tree is hand-formatted —
aligned comment blocks, PascalCase locals matching the C++ house style. A
formatter would churn all of it and then fight it. `.editorconfig` at the repo
root is the formatting authority.

`tsconfig.json` covers `src/` with `lib: es2020 + dom` and **no node types**, so
an `import 'node:fs'` in page code is a compile error. `tsconfig.test.json`
covers `test/`, `e2e/` and the build scripts, where node is the point.
`erasableSyntaxOnly` confines the whole tree to TypeScript that erases to
nothing — no enums, no parameter properties — because both things that read it
only strip types rather than compiling them.

## The bridge, and the one contract that spans both languages

Six functions cross from C++ into the page, as `EM_JS` bodies that look their
target up on `globalThis`:

| global | methods | declared in |
|---|---|---|
| `ivanSfx` | `play` | `FeLib/Source/sfx.cpp` |
| `ivanMusic` | `setPlaylist`, `setPlaying`, `setVolume`, `setIntensity`, `currentIndex` | `audio/audio.cpp` |

An `EM_JS` body is a string of JavaScript pasted into `ivan.js`. Nothing checks
it: `Music.setVolume(Level)` resolving to `undefined` is a **silent no-op**, not
an error, and the corpora cannot see it because a headless replay makes no sound.
So the names are declared once in `src/bridge/contract.ts` and checked from both
ends:

- `test/bridge.test.ts` parses the `EM_JS` blocks out of the C++ and compares
  them against `contract.ts`, in both directions — a call that is not declared
  fails, and a declaration nothing calls any more fails too. Parsed rather than
  trusted, for the same reason `dist.py` parses `SoundEffects.cfg` instead of
  globbing `Sound/`: a declaration checked against itself proves nothing.
- `e2e/boot.spec.ts` asserts the live page has every one of them as a function.

The first run of that test found two bridges — `IvanMusicPlaying` and
`IvanMusicVolume` — that `tools/web/README.md`'s bridge table does not mention.

## The browser suite, and why it has to exist

```bash
emcmake cmake -S . -B build-web -DCMAKE_BUILD_TYPE=Release \
  -DWIZARD=ON -DPORTABLE_BUILD=ON -DWASM_BROWSER=ON
cmake --build build-web -j$(nproc)
tools/web/dist.py
cd web && npm run e2e         # ~7s for the current seven
```

It runs against an assembled `dist/` served by `tools/web/serve.py`, not a dev
server, so what it exercises is what gets deployed: real wasm, real IDBFS, real
autoplay policy.

**This is what replaces the coverage the golden traces are about to lose.** The
committed corpora prove the game still plays the way it did, and they do it by
hashing the C++ `bitmap` double buffer before it reaches a texture (HARNESS.md
§6.3, §9.3). That works only while rendering is C++. As graphics, input and the
UI cross into this directory, the subject of that hash crosses with them — the
traces will keep passing and cover less. Nothing else in the repo has ever
tested a browser: the two node suites in `tools/web/` are contract tests against
stubs, `shell.html` is untested, and HARNESS.md §9.10 says outright that "a save
survives a reload" needs one.

The current seven assert that the page boots without a page error, that the
canvas keeps its 800×600 backing store, that it draws more than one colour, that
every bridge is present, that the console APIs exist, that **a keystroke reaches
the C++ input path** (asserted through `ivanHarness.text()`, the live recording,
so it covers the browser, the shell, asyncify and `GET_KEY`), that the first
gesture releases the audio context, and that the saves are mounted writable.

Not a pixel comparison: the main menu fades in and the seed varies, so a golden
image needs a fixed seed and a settled frame first. That is the obvious next one.

## What is here

```
web/
  src/
    main.ts               the entry point, and the only place a global is assigned
    platform/query.ts     the query string, once, for all of ?sfx=off ... ?wipesaves
    bridge/contract.ts    the six EM_JS targets, checked from both ends
    bridge/globals.d.ts   the console APIs, typed
    env.d.ts              IVAN_BUILD_ID and IVAN_CRASH_ENDPOINT, esbuild --define
  test/                   node --test, no browser
  e2e/                    Playwright, against an assembled dist/
  build.mjs               esbuild
```

Modules cross into `src/` one at a time, and `src/main.ts` keeps the list it has
initialised so the browser test fails on a module that did not load rather than
on a feature that is quietly missing.
