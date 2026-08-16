# CLAUDE.md

## What this fork is

A fork of [Attnam/ivan](https://github.com/Attnam/ivan) (`origin`; this fork is `fork` →
`bethmaloney/ivan`) that turns IVAN into a **browser game**. The shape of the port:

- **Keep the game logic.** `Main/` is ~106k lines of 25-year-old C++ and it stays, compiled to
  WASM by Emscripten.
- **Replace the presentation.** Graphics, sound, music, input, UI and config are being moved
  out of C++/SDL and into native JavaScript on the page. SDL and everything that exists only
  to serve it comes out as each piece crosses.
- **Host it on Cloudflare Pages.** Project `playivan`, live at
  [playivan.pages.dev](https://playivan.pages.dev); landing page at `/`, game at `/play/`.

Sound effects (§9.7), music (§9.8) and saves (§9.10) have already crossed and the page owns
them. Graphics, input and UI have not. `audio/` — RtMidi, the MIDI parser, the playback engine
— is already excluded from the Emscripten build.

## Builds

Three targets, three build dirs, all needing `-DWIZARD=ON -DPORTABLE_BUILD=ON`.

```bash
# native — the reference build, and where gdb/valgrind/ASan apply
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DWIZARD=ON -DPORTABLE_BUILD=ON
cmake --build build -j$(nproc)
cmake --build build --target savediff        # EXCLUDE_FROM_ALL, built separately

# WASM under node — the seam-1 oracle
source ~/emsdk/emsdk_env.sh
emcmake cmake -S . -B build-wasm -DCMAKE_BUILD_TYPE=Release -DWIZARD=ON -DPORTABLE_BUILD=ON
cmake --build build-wasm -j$(nproc)

# WASM in a browser — the actual product
emcmake cmake -S . -B build-web -DCMAKE_BUILD_TYPE=Release -DWIZARD=ON -DPORTABLE_BUILD=ON \
  -DWASM_BROWSER=ON
cmake --build build-web -j$(nproc)
emrun --no_browser --port 8111 build-web/Main
```

Dependencies (native only): `libsdl2-dev libsdl2-mixer-dev libpng-dev`. The WASM targets take
SDL2, SDL2_mixer and libpng from Emscripten's ports.

## Testing — run the corpora

There is no unit test suite for the game. The oracle is two committed recordings with golden
traces, text logs and screenshots.

```bash
tools/corpora/verify-corpora.sh          # 8 runs each: self-consistency + golden. ~9s
tools/corpora/verify-corpora.sh -n 1     # smoke test
tools/corpora/compare-targets.sh         # native vs WASM, both corpora
node tools/web/music.test.js             # music.js contract and mixing arithmetic, no browser
node tools/web/saves.test.js             # saves.js contract: IndexedDB sync rules, no browser
```

**Run `verify-corpora.sh` before and after any change to `Main/`, `FeLib/`, the compiler flags
or the build.** A change that moves the goldens has changed the game; that may be correct, but
it is never incidental and `--update` is a deliberate act with a written reason.

To reach a game state worth testing, drive it with `tools/play/play.py` — it replays from the
main menu each time and prints the screen's text layer plus a PNG.

```bash
python3 tools/play/play.py new --seed 999 --start
python3 tools/play/play.py send down left '>'
python3 tools/play/play.py auto 200
```

## Deploying

**A push to `main` deploys.** `.github/workflows/deploy.yml` builds native and replays the
corpora, runs the two node tests, builds the browser target with the pinned emsdk and runs
`dist.py`, then publishes to Cloudflare Pages. A pull request runs everything except the
publish. It needs two repo secrets, `CLOUDFLARE_API_TOKEN` and `CLOUDFLARE_ACCOUNT_ID`.

The manual path is still the one to use when checking a build by hand:

```bash
tools/web/dist.py                      # -> dist/ (rebuilt from scratch), from build-web/Main
tools/web/serve.py 8113 dist           # check it locally first
# --branch main stays explicit. The git branch is now main too, but wrangler
# labelling the upload anything other than main makes it a preview nobody visits.
npx wrangler pages deploy dist --project-name=playivan --branch main
```

## Working conventions

**Comment only what is unusual or non-obvious, one or two lines at most.** Ordinary code gets
no comment. The reason a line looks wrong but is right belongs next to it; everything else
belongs in `HARNESS.md` or a `README.md`.

**Measure, don't assume.** This is the codebase's stated first principle and it has caught out
its own documentation more than once (§9.9 corrected two claims that were assumptions in the
clothes of measurements). Numbers in these documents mean somebody ran it. If you write one,
run it. If you find one that is wrong, correct it in place and say what it was.

## Gotchas

- **`PORTABLE_BUILD=ON` is required for testing.** Without it `DATADIR` is baked in and the
  binary aborts with `Script/define.dat not found`.
- **`FeLib/CMakeLists.txt` uses `file(GLOB ...)`.** An incremental `make` in an existing build
  dir silently misses new files. Re-run `cmake`, don't just `make`.
- **`-fexceptions` is not a tuning knob.** IVAN throws as ordinary control flow
  (`areachangerequest` on every level change); Emscripten disables throwing by default and the
  build dies in the main menu with a bare `Aborted(undefined)`. It is a compile *and* link flag,
  globally — a TU built without it cannot catch what another one throws.
- **Emscripten does not forward the process environment.** `SDL_VIDEODRIVER=dummy` and friends
  reach nothing under node; that is why `--headless` is a program flag.
- **A modal alert during a replay eats a recorded key** and desynchronises the run. Anything
  that can call `iosystem::AlertConfirmMsg` on a failure path is a hazard for the harness.
