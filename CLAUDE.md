# CLAUDE.md

## What this fork is

A fork of [Attnam/ivan](https://github.com/Attnam/ivan) (`origin`; this fork is `fork` →
`bethmaloney/ivan`) that turns IVAN into a **browser game**, deployed to Cloudflare Pages at
[playivan.pages.dev](https://playivan.pages.dev). The game logic stays as C++ compiled to WASM; the
presentation moves piece by piece into TypeScript in `web/`.

**`PORTING.md` is the reference** — the two seams, the harness, where the port stands.
**`docs/port-log.md`** is what each finding was and how it was found, under `§` numbers that comments
across the tree cite. Read `PORTING.md` before changing anything in `Main/`, `FeLib/` or the build.

Sound effects, music, the harness and saves have crossed into `web/`; graphics, input and the UI have
not. There is no JavaScript in `tools/web/` and no `--pre-js` in the build.

## Builds

Three targets, three build dirs, all needing `-DWIZARD=ON -DPORTABLE_BUILD=ON`. `PORTING.md` has what
each is for and which flags are not negotiable.

```bash
# native — the reference build, and where gdb/valgrind/ASan apply
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DWIZARD=ON -DPORTABLE_BUILD=ON
cmake --build build -j$(nproc)
cmake --build build --target savediff        # EXCLUDE_FROM_ALL, built separately

# WASM under node — the seam-1 oracle
source ~/emsdk/emsdk_env.sh
emcmake cmake -S . -B build-wasm -DCMAKE_BUILD_TYPE=Release -DWIZARD=ON -DPORTABLE_BUILD=ON
cmake --build build-wasm -j$(nproc)

# WASM in a browser — the actual product. Needs `cd web && npm ci` first
emcmake cmake -S . -B build-web -DCMAKE_BUILD_TYPE=Release -DWIZARD=ON -DPORTABLE_BUILD=ON \
  -DWASM_BROWSER=ON
cmake --build build-web -j$(nproc)
emrun --no_browser --port 8111 build-web/Main
```

Native dependencies: `libsdl2-dev libsdl2-mixer-dev libpng-dev`. The WASM targets take SDL2 and
libpng from Emscripten's ports — not SDL2_mixer, which only the native playback path needs — and the
toolchain is pinned to emsdk 6.0.6.

## Claude Code on the web

`.claude/hooks/session-start.sh` provisions a session's container, which starts with none
of the toolchain. It installs SDL2 and libpng from apt, node from `.nvmrc` via nvm,
`web/node_modules` from the committed lockfile, and emsdk at `deploy.yml`'s `EMSDK_VERSION`
-- so the versions live in the files that already pin them, not in the hook. It then
pre-builds the Emscripten ports, points `IVAN_CHROMIUM` at the container's pre-installed
Chromium when that is not the revision `@playwright/test` pins, and exports `EMSDK` and a
`PATH` that keeps `.nvmrc`'s node ahead of the SDK's bundled one -- `find_program(IVAN_NODE)`
takes the first `node` it finds, and that is what bundles `web/`.

It is guarded on `CLAUDE_CODE_REMOTE` and does nothing on a developer's own machine. Cold
it takes ~62s -- 35s of that is emsdk's 301MB download and ~25s the ports; the container's
state is cached afterwards, so later sessions re-run it in ~2s.

The ports are the one part that needs egress beyond the SDK: emcc fetches each one's source
the first time it links it. On Claude Code on the web `github.com/<owner>/<repo>/archive/...`
answers **403** -- and only that endpoint. `/releases/download/` is fine, which is how the
`ogg` port arrives, and so is `git clone` of the very same tag. `libpng` comes from
`storage.googleapis.com` and was never affected; it only failed because `zlib` is its
dependency.

So `.claude/hooks/seed-emscripten-ports.py` clones the refused repos -- today `libsdl-org/SDL`,
`libsdl-org/SDL_mixer` and `madler/zlib` -- into the port cache at the tag emcc asked for,
beside the `.emscripten_url` marker that makes `up_to_date()` accept them. A seeded port is
never fetched and never hash-checked, which is required rather than incidental: a clone is not
byte-identical to GitHub's generated tarball and would fail the port's sha512. The script reads
the tags out of emcc's own "retrieving port" output rather than naming any, so an emsdk bump
moves them without editing it. `EMCC_LOCAL_PORTS` is the mechanism that looks right for this
and is not -- it needs a `SUBDIR` attribute that only `sdl2` defines.

With that, all three targets build in a web session and `compare-targets.sh` reports native and
WASM agreeing. Where the archive endpoint is *not* blocked the script is a plain `embuilder`
call and clones nothing.

## Testing

There is no unit test suite for the game. The oracle is three committed recordings with golden
traces, text logs and screenshots.

```bash
tools/corpora/verify-corpora.sh          # 8 runs each: self-consistency + golden. ~17s
tools/corpora/verify-corpora.sh -n 1     # smoke test
tools/corpora/compare-targets.sh         # native vs WASM, all corpora. ~26s
tools/corpora/compare-configs.sh         # one build at ten display configs. ~50s
tools/corpora/fuzz-visual.sh             # one build at six --visual-seed values. ~24s
```

Four scripts, four questions: *did this build change?*, *do these two builds agree?*, *does one
build agree with itself when the player configured it differently?*, *does anything the game draws
decide anything the game keeps?* Only the first runs in CI. `docs/port-log.md` §6.10a is what a
`compare-configs.sh` pass does and does not establish, and §6.10d is why the last two are not the
same check.

Each run writes two traces: `game.jsonl`, sampled from the game's own loop and carrying no
screen-derived quantity, and `frames.jsonl`, which hashes the double buffer. **When graphics crosses
into `web/`, the first survives and the second is replaced** — which is the whole reason they were
separated (§6.10d).

**Run `verify-corpora.sh` before and after any change to `Main/`, `FeLib/`, the compiler flags or the
build.** A change that moves the goldens has changed the game; that may be correct, but it is never
incidental and `--update` is a deliberate act with a written reason.

The page's own half is tested from `web/`, which needs **Node 24** (`.nvmrc`; `nvm use` reads it).
See `web/README.md`.

```bash
cd web && npm ci
npm run check          # tsc + oxlint + node --test. No browser, 9.8s
npm run e2e            # Playwright against an assembled dist/. 15-21s
```

`npm run e2e` needs a browser build assembled first (`-DWASM_BROWSER=ON`, then `tools/web/dist.py`)
because it runs against `dist/` rather than a dev server. It is the only thing here that tests a
browser, and it matters more as the port continues: the golden traces hash the C++ double buffer, so
as graphics and input cross into `web/` they keep passing and cover less.

`npm run check` includes the bridge contract test, which parses the `EM_JS` blocks out of the C++ —
**so a renamed `EM_JS` fails a JavaScript test.** That is deliberate; `PORTING.md` says why.

To reach a game state worth testing, drive it with `tools/play/play.py` — it replays from the main
menu each time and prints the screen's text layer plus a PNG.

```bash
python3 tools/play/play.py new --seed 999 --start
python3 tools/play/play.py send down left '>'
python3 tools/play/play.py auto 200
```

## Deploying

**A push to `main` deploys.** `.github/workflows/deploy.yml` is five jobs, named after the five
things that can be wrong: `corpora` (native build, replay the goldens), `modules` (`npm run check`,
plus `npm run build` so a bundle that will not build is caught before the emsdk job), `package`
(browser build with the pinned emsdk, then `dist.py`), `browser` (Playwright against `package`'s
artifact, so it tests the bytes that are about to be uploaded) and `publish`. A pull request runs
everything except the publish, which needs `CLOUDFLARE_API_TOKEN` and `CLOUDFLARE_ACCOUNT_ID`.

The manual path, for checking a build by hand:

```bash
tools/web/dist.py                      # -> dist/ (rebuilt from scratch), from build-web/Main
tools/web/serve.py 8113 dist           # check it locally first
# --branch main stays explicit: wrangler labelling the upload anything else makes it
# a preview nobody visits, and it fails silently.
npx wrangler pages deploy dist --project-name=playivan --branch main
```

## Working conventions

**Comment only what is unusual or non-obvious, one or two lines at most.** Ordinary code gets no
comment. The reason a line looks wrong but is right belongs next to it; everything else belongs in
`PORTING.md`, `docs/port-log.md` or a `README.md`.

**Measure, don't assume.** This is the codebase's stated first principle and it has caught out its own
documentation more than once. Numbers in these documents mean somebody ran it. If you write one, run
it. If you find one that is wrong, correct it in place and say what it was.

## Gotchas

- **`PORTABLE_BUILD=ON` is required for testing.** Without it `DATADIR` is baked in and the binary
  aborts with `Script/define.dat not found`.
- **`FeLib/CMakeLists.txt` uses `file(GLOB ...)`.** An incremental `make` in an existing build dir
  silently misses new files. Re-run `cmake`, don't just `make`.
- **`-fexceptions` is not a tuning knob.** IVAN throws as ordinary control flow
  (`areachangerequest` on every level change); Emscripten disables throwing by default and the build
  dies in the main menu with a bare `Aborted(undefined)`. It is a compile *and* link flag, globally —
  a TU built without it cannot catch what another one throws.
- **Emscripten does not forward the process environment.** `SDL_VIDEODRIVER=dummy` and friends reach
  nothing under node; that is why `--headless` is a program flag.
- **A modal alert during a replay eats a recorded key** and desynchronises the run. Anything that can
  call `iosystem::AlertConfirmMsg` on a failure path is a hazard for the harness.
- **The crash endpoint is `IVAN_CRASH_ENDPOINT` in the environment**, read by `web/build.mjs`. It was
  `-DWASM_CRASH_ENDPOINT`; nothing in CMake computes a build id or an endpoint any more.
- **A new sound file must go through `tools/sound/normalize.py`.** Every wav under `Sound/` sits at a
  loudness its `SoundEffects.cfg` section declares, and nothing scales them afterwards — the page's
  bus is at unity (§9.7b). A file dropped in at its own level is as wrong as one at the wrong
  sample rate. `--check` says whether the set is on target; `--dry-run` prints the plan.
