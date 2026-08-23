# Porting IVAN to the browser

A fork of [Attnam/ivan](https://github.com/Attnam/ivan) that turns IVAN into a browser game, live at
[playivan.pages.dev](https://playivan.pages.dev). This is the reference: the seams the port is built
around, the harness that proves it, and where the work stands. Commands are in `CLAUDE.md`; what each
finding was and how it was found is in `docs/port-log.md`.

## The shape of the port

- **Keep the game logic.** `Main/` is ~106k lines of 25-year-old C++ and it stays, compiled to WASM
  by Emscripten.
- **Replace the presentation.** Graphics, sound, music, input, UI and config move out of C++/SDL and
  into TypeScript on the page. SDL and everything that exists only to serve it comes out as each
  piece crosses.
- **Host it on Cloudflare Pages.** Project `playivan`; landing page at `/`, game at `/play/`.

## Why the harness exists

IVAN has **no test suite**. No `add_test`, no gtest, no Catch2. Rewriting or porting 100k+ lines of
25-year-old C++ with no oracle is how forks die, so the oracle came before the port work.

The harness records a session, replays it deterministically, and hashes every frame so two builds can
be compared byte for byte. It also **captures the screen** — as a PNG and as a text layer — which is
what makes it possible to steer the game to a state worth testing in the first place. With only
hashes there is no way to decide what to press next, so no interesting game state can be reached, so
none can be tested.

### The two seams — do not conflate them

1. **Framebuffer equality**, while C++ still renders. Native and WASM must produce identical frame
   hashes. This validates the *toolchain*: Emscripten, libm, float behaviour, struct layout.
2. **State-stream equality**, once the page takes over rendering. Pixels will legitimately differ
   then — that is the point of the rewrite — and the golden master becomes the state crossing the C
   API. `docs/port-log.md` §7.5.

Seam 1 is where the work is. Pixel comparison validates the *port*; state comparison validates the
*rewrite*. You need both, in that order.

**The determinism rows and the native-vs-WASM rows measure different things, and the second is much
harder.** A build reproducing itself only needs the *program* to be deterministic. Two builds
agreeing needs every place the program leaves a choice to the *compiler or the standard library* to
have been closed as well — §9.4 is a list of a dozen such places that no same-build test could ever
have found.

## Where it stands

**Seam 1 is open and proven on all three committed corpora.** The WASM build runs headless under bare
node, and on every corpus a native replay and a WASM replay produce byte-identical frame traces, text
logs, screenshots, world maps and level files. The `.sav` differs by one byte — `GetTimeSpent`, a
wall-clock second boundary that no determinism work removes.

**The game is playable in a browser**, which is a second host for the same WASM core and not progress
on seam 1. The browser build has never been compared against a golden and is not an oracle:
`NODERAWFS` is what lets the harness write a trace, and turning it off is the first thing
`WASM_BROWSER` does.

| Crossed into `web/` | Still C++ |
|---|---|
| sound effects (§9.7), music (§9.8), the harness (§9.6), saves (§9.10) | graphics, input, the UI |

`audio/` — RtMidi, the MIDI parser, the playback engine, ~4,700 lines — is no longer compiled for
Emscripten at all. `tools/web/` holds `shell.html`, the build tooling (`dist.py`, `serve.py`) and the
landing page; **there is no JavaScript in it and no `--pre-js` in the build**.

What is left to cross is not a file move: SDL surfaces, an SDL event loop and a C++ menu system, so
each is a rewrite against a browser API. The golden traces still cover all three — which is the
coverage they are about to lose, and why `web/e2e/` exists.

### What is measured, and what that does and does not cover

| | |
|---|---|
| Each corpus, 8 isolated runs — trace, text log, PNG | **1 distinct outcome**, matching the committed golden |
| The longest corpus, 16 concurrent runs on a saturated machine | **1 distinct outcome**, byte-identical to the unloaded run — 1,972 turns, 2 deaths, 10.7M RNG draws |
| Each corpus at six `DungeonGfxScale` values, one build | **the same game-stream position (`grng`)** at every scale, with distant lights off (§6.10a) |
| Each corpus at six `--visual-seed` values, one build | **a byte-identical game trace**, and a frame trace that differs — the liveness half (§6.10d) |
| Native vs WASM, all three corpora | trace, text log, screenshot, sidecar, `.wm` and every level file **byte-identical** |
| Native `--headless` vs native windowed | **byte-identical** on every artifact |
| valgrind uninitialized reads, every corpus | **0 errors / 0 contexts** |
| Uninitialized bytes reaching a level file | **0**, at two `MALLOC_PERTURB_` values |
| Level file across 8 ordinary runs — ASLR on, no heap fill | **1 distinct** |
| Host-libm calls made by the game | **0**, from 27,496 (§6.7) |
| `portmath` under GCC vs under emcc, 14,000 evaluations | **bit-identical** |

The CPU-load row is the meaningful one *for the risk it was designed to test* — wall-clock leaking
into game state. Saturating the machine and still getting identical output is real evidence about the
clock. Its turns/deaths/draws read "2,800 turns, 36 deaths, 9.7M RNG draws" until §6.10 re-measured
them: those figures came from an ad-hoc auto-play run made while the harness was being written,
before any corpus was committed, and never described `autoplay-2000.rec`. The "1 distinct outcome"
half was right then and is right now.

**Read all of it narrowly.** Every number is a property of the levels these three key sequences
generate. Native-vs-WASM is unblocked *for the reached paths* and unproven elsewhere:

- **No corpus generates New Attnam**, and a 116-key browser recording that enters it diverges at
  frame 513 during the town's *generation*, ~1.06M draws in, native taking 3,274 more draws than
  WASM. That is coverage, not a regression — it is the §9.4 hunt again on a level no corpus reaches,
  and the recording reproduces on demand. **Widening the corpora is the single most valuable thing to
  do next.**
- **A clean memcheck means clean on what this corpus touches**, nothing more. §6.6e is the second
  time a corpus change alone surfaced a family that three earlier passes had walked past.
- **`.wm` still has the §6.2 residue**, and no corpus visits enough of the world map to write the
  whole file.
- **Every run above uses one window size and one zoom.** `run-corpus.sh` pins the configuration,
  which is what makes the comparisons mean anything and is also why the player's `DungeonGfxScale`
  sat in the game's random stream unnoticed for the whole port (§6.10). `compare-configs.sh` varies
  that one axis on four recordings and compares one integer each; §6.10a is the list of what it
  still cannot see, starting with the fact that it pins `EnhancedLights` off and the shipped default
  is on.

## Builds

Three targets, three build directories, all needing `-DWIZARD=ON -DPORTABLE_BUILD=ON`. Commands in
`CLAUDE.md`.

| dir | what it is for |
|---|---|
| `build` | native — the reference build, and where gdb, valgrind and ASan apply |
| `build-wasm` | WASM under node — the seam-1 oracle. `NODERAWFS`, no preload |
| `build-web` | WASM in a browser — the actual product |

Use a **separate directory** for each. `build-wasm` is the oracle and the two WASM hosts do not share
a configuration.

Native dependencies: `libsdl2-dev libsdl2-mixer-dev libpng-dev`, all three required — without
SDL2_mixer the link fails. The WASM targets take SDL2 and libpng from Emscripten's ports, and not
SDL2_mixer: nothing there calls a `Mix_*` symbol, because playback belongs to the page (§9.13).

**gcc and clang are the only supported compilers.** The top-level `-std=c++11` already assumed it —
MSVC rejects that spelling — so upstream's MSVC paths could never have run, and they are deleted
rather than carried untested. emcc is clang, so the WASM targets need no case of their own.

**SDL2 is the only SDL, and little-endian the only byte order.** `find_package(SDL2 REQUIRED)` is the
only lookup in the tree and Emscripten is pinned to `-sUSE_SDL=2`, so `SDL_MAJOR_VERSION` is 2 on
every target — measured 2.30.0 natively. Upstream's `SDL_MAJOR_VERSION == 1` branches and the
`SDL_BYTEORDER == SDL_BIG_ENDIAN` ones are deleted rather than carried untested: they had no
toolchain here, and they had stopped compiling anyway. The big-endian arm of `graphics.cpp` was the
worse of the two — it replaced `BlitDBToScreen` with a body referencing two identifiers that exist
nowhere in the tree, and put `graphics::Stretch`, every `SetSRegion*`, `DrawAboveAll` and
`PrepareBuffer` in the `#else`, so the file it produced could not have linked. `SDL_VERSION_ATLEAST`
is a different thing and stays: 2.26 moved the mouse position onto `SDL_MouseWheelEvent`, and the
port's SDL may be older than the system's.

**There is no `USE_SDL` any more either.** The top level defined it unconditionally, nothing could
unset it, and none of its sixteen `#ifdef` blocks had an `#else` — it was the switch against
upstream's FeDX backend, and that side went with the DOS build. The configuration it appeared to
protect could not have compiled regardless: `bitmap.h`, `specialkeys.h` and `devcons.cpp` include
`SDL.h` unguarded, and `whandler.h` used `SDL_Event`, `SDL_Keycode`, `SDL_GetTicks` and
`SDL_GameController*` outside its own guards. **SDL is a hard dependency of this codebase and reads
as one now.**

**The toolchain is pinned to emsdk 6.0.6**, and pinning it is the same argument as pinning musl
(§6.7): a different LLVM can change float codegen or struct layout, and the first symptom would be an
unexplained frame-hash divergence thousands of frames from the cause. `latest` is not a toolchain, it
is a moving target.

`savediff` is deliberately `EXCLUDE_FROM_ALL` and links nothing — it is the differential oracle and
must build even when the game build is broken. Same reason `tools/regexdiff/` is outside CMake.

### Flags that are not tuning knobs

- **`-fexceptions`** — Emscripten disables exception *throwing* by default, so `__cxa_throw` lowers
  to `abort()`, and IVAN throws as ordinary control flow: `areachangerequest` on every level change,
  `quitrequest` on quit, `genericException` out of the prototype database. Without it the build dies
  drawing the main menu with a bare `Aborted(undefined)`. It is a compile *and* link flag, globally —
  the compile half emits the landing pads, so a TU built without it cannot catch what another one
  throws.
- **`WASM_ASYNCIFY`** (ON) — for the blocking `SDL_WaitEvent` in `GetKey`. A replay does reach it:
  `iosystem::Menu` and `bitmap::FadeToScreen` call `WaitUntil`, which is `SDL_Delay`, so a replay
  unwinds and rewinds through the menu fade on every run. Leave it on.
- **`WASM_NODERAWFS`** (ON for node, forced off for the browser) — it binds to node's `fs` module. In
  a browser that is a missing filesystem, not a degraded one, so setting both it and `WASM_BROWSER`
  is a hard error rather than a link that half works.
- **`-sFS_DEBUG=1`** on the browser target is **not a debug mode**. It registers the filesystem
  callbacks `web/src/saves/saves.ts` needs, and nothing else — measured, §9.10.
- **`--profiling-funcs`** on the browser target keeps the wasm name section: 800KB, no codegen
  change, and the difference between a crash report that names `character::Die` and one that prints
  byte offsets nothing can resolve (§9.6).
- **`WASM_PRELOAD_AUDIO`** (OFF) no longer governs whether the game makes a sound. §9.7 moved
  playback to the page; turning this ON would put 26MB of wav back into `ivan.data` to populate a
  MEMFS nothing reads. Untested with it ON.

The browser target preloads `Graphics/` and `Script/` to **absolute** paths, because `PORTABLE_BUILD`
makes `game::GetDataDir()` return `"./"` (`game.cpp:5426`) while the module starts with its working
directory at `/`. `GetUserDataDir()` answers `/ivan/` here instead, which is the IDBFS mount holding
everything the player accumulates (§9.10). The one file under `Sound/` that is preloaded,
unconditionally and on its own, is `SoundEffects.cfg` — the pattern table stays in C++, `initSound`
opens it with `fopen`, and without it the game goes silent with **nothing printed** (§9.7). 10KB.

A browser build also needs node and `web/node_modules`; both are checked when CMake configures, so a
missing one names itself rather than failing inside a custom command.

Serve it over HTTP either way — `file://` will not fetch the wasm — and no COOP/COEP headers are
needed, there being no pthreads and no `SharedArrayBuffer`. `emrun` serves `.wasm` as
`application/wasm` without argument, but it is **single-threaded**, so one browser keepalive
connection wedges it for every other client: fine for playing, not fine for a script driving it
repeatedly, which is what `tools/web/serve.py` is for.

## Testing

The oracle is three committed recordings with golden traces, text logs and screenshots —
`tools/corpora/README.md` has the table, the check values and how to regenerate them. Four scripts
answer four different questions and you want all of them:

- **`verify-corpora.sh`** compares a build against the committed goldens: *did this build change?*
- **`compare-targets.sh`** replays each corpus on native and WASM and compares them against each
  other: *do these two builds agree?*
- **`compare-configs.sh`** replays each corpus at every `DungeonGfxScale` on one build and compares
  the game-stream draw count: *does one build agree with itself when the player configured it
  differently?* It is the newest and the narrowest — one integer per corpus, one config axis — and
  §6.10a is the honest account of its reach.
- **`fuzz-visual.sh`** replays each corpus varying only `--visual-seed` and compares the game trace:
  *does anything the game draws decide anything the game keeps?* It and `compare-configs.sh` cover
  different mistakes and neither contains the other — the sweep sees a *game*-generator draw behind a
  visibility test, this sees a *presentation*-generator value reaching game state, and a
  game-generator draw that is not camera-gated (`bodypart::DrawScars`) is seen by neither. §6.10c has
  the table. **CI runs only the first** (`deploy.yml`), so no invariance property has a regression
  test.

A change that moves both builds identically passes the second and fails the first; a
compiler-dependent expression (§9.4) does the reverse, which is why a dozen such bugs survived every
determinism test in this repo. The third asks about the *player's* configuration rather than the
build's, and it found a bug the other two could not express: until §6.10 every run in the tree shared
one window size, so nothing could see that the zoom level was moving the game's random stream.

A fourth recording, `tools/corpora/effects/beams.rec`, sits under `effects/` with **no golden and
deliberately no way to acquire one** — it exists so `compare-configs.sh` has a corpus that casts
something, and its animation is *meant* to differ between zoom levels. It is outside the other two
scripts' globs, which is what §6.10b costs and records.

**Run `verify-corpora.sh` before and after any change to `Main/`, `FeLib/`, the compiler flags or the
build.** A change that moves the goldens has changed the game. That may be correct, but it is never
incidental, and `--update` is a deliberate act with a written reason.

Two things the scripts enforce, both learned by getting them wrong (§6.5a): **every run gets its own
directory**, because a portable build writes saves and question history into the launch directory, and
**the default is 8 runs rather than 2**, because a flaky divergence lets pairs agree by chance often
enough to fool you. `run-corpus.sh` also pins the harness options, because changing them changes the
allocation history and a golden trace only means something if the command line that produced it is
fixed.

Saves are deliberately not compared by `verify-corpora.sh`; `compare-targets.sh` runs savediff and
prints its verdict but never lets it decide the exit status. Level files and `.wm` do reproduce, but
`.sav` carries the `GetTimeSpent` byte, which is a wall-clock flake.

The page's own half is tested from `web/` — `npm run check` (no browser) and `npm run e2e` (Playwright
against an assembled `dist/`). `web/README.md` has both. `npm run check` includes the bridge contract
test, which parses the `EM_JS` blocks out of the C++, **so a renamed `EM_JS` fails a JavaScript
test.** That is deliberate: an `EM_JS` body is a string pasted into `ivan.js`, so a method that no
longer exists is a silent no-op rather than an error, and the corpora cannot see it because a headless
replay makes no sound.

### Steering the game

`tools/play/play.py` replays from the main menu each time and prints the screen's text layer plus a
PNG. **A session is a list of keys, not a live process** — every command rewrites the list and
replays under a pinned seed, which is what makes it reproducible, resumable after a crash and
rewindable. Full notes in `tools/play/README.md`, including the wizard auto-play AI, which is how the
longer corpora were generated.

## The harness

### CLI

```
--record   [file]     Record every key the game reads to file.
--replay   [file]     Play back a recording instead of reading real input.
--trace    [file]     Write the game's JSONL trace: one record per game step,
                      carrying the turn, the clock, the player, the creation
                      counters and the draw counts. Nothing in it comes from the
                      screen, so it outlives the renderer (§6.10d).
--frame-trace [file]  Write the presentation's JSONL trace: one record per frame
                      whose pixels differ from the previous frame's, carrying a
                      hash of the double buffer.
--seed     [number]   Pin the RNG seed. Continuing a saved game ignores this
                      (the seed is stored in the save).
--visual-seed [n|random]
                      Seed the presentation generator (§6.10c), which decides
                      nothing the game keeps. Fixed by default, because frame
                      hashes and screenshots have to reproduce; "random" is the
                      arm that asserts nothing leaks from it into grng.
--shot     [file.png] Write the screen when the run ends, plus a file.txt
                      sidecar holding every string on that screen.
--shot-dir [dir]      Write every frame that differs from the one before it to
                      dir/frame-NNNNNN.png, with a .txt sidecar each. About
                      1.4MB per frame and several hundred frames per session,
                      so ~600MB to reach the first dungeon level.
--text     [file]     Log every string the game draws, in draw order, for the
                      whole session.
--headless            Run with no window, no renderer and no audio device. The
                      game still renders in full into the double buffer, so
                      --trace, --shot and --text all work. Required for the
                      WASM build under node; free and byte-identical natively.
```

**`--headless` is what makes the native and WASM command lines the same program.** It skips the
window, the renderer, the streaming texture, the final blit and `Mix_OpenAudio`, and skips nothing
else: rendering is software into the bitmap double buffer either way, and `TraceFrame()` hashes that
buffer before any of the skipped work.

It replaces `SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy`, which still works natively and is still
passed by `run-corpus.sh` belt-and-braces. Two reasons the decision had to move into the program:
Emscripten's SDL2 port binds to the DOM at video init whatever the driver hint says, and **Emscripten
does not forward the process environment into the module**, so under node those variables reach
nothing at all.

### API — `FeLib/Include/harness.h`

```cpp
namespace harness
{
  void ParseArgs(int, char**);
  void Shutdown();

  inline truth IsRecording();      // inline reads of a static bool —
  inline truth IsReplaying();      // zero cost when disabled
  inline truth IsTracing();

  void RecordKey(int);
  truth NextReplayKey(int&);
  void TraceGameStep();            // game::Run's loop, and TraceFrame
  void TraceFrame();               // BlitDBToScreen
  void SetGameStateReader(gamestatereader);

  inline truth IsCapturingText();
  void RecordText(const void* Target, int X, int Y, int Color, cchar*);
  void WriteShot(cchar* FileName, cchar* Reason);

  inline truth IsHeadless();       // read by graphics.cpp, whandler.cpp, sfx.cpp

  inline void CountRand();         // ++RandCount, the game generator
  inline ulong GetRandCount();
  inline ulong GetGameRandCount(); // the same count, brackets excluded

  inline void CountVisualRand();   // ++VisualRandCount, the presentation one
  inline ulong GetVisualRandCount();

  inline void EnterSeedBracket();  // femath::SaveSeed/LoadSeed only; Enter
  inline void LeaveSeedBracket();  // counts a bracket entered at depth > 0

  truth HasSeedOverride();
  ulong GetSeedOverride();
  ulong GetVisualSeed();           // --visual-seed; always has a value
}
```

**Zero-cost-when-disabled is a hard requirement** — this ships in the game. The mode queries are
inline static-bool reads; `TraceFrame()` and `RecordKey()` early-out before any allocation,
formatting or I/O.

| File | Hook |
|---|---|
| `FeLib/Source/whandler.cpp` | record/replay at every `GetKey`/`ReadKey` return path; `IsHeadless()` guards `SDL_ShowWindow` |
| `FeLib/Source/graphics.cpp` | `TraceFrame()` in all three `BlitDBToScreen` definitions; `IsHeadless()` guards `SDL_Init`'s video bit, the window, the renderer, the texture, `SetScale`, `SwitchMode` and the blit |
| `FeLib/Source/femath.cpp` | `CountRand()` in `femath::Rand()`, `CountVisualRand()` in `visualrand::Rand()`; `Enter`/`LeaveSeedBracket()` in `SaveSeed`/`LoadSeed` |
| `FeLib/Source/rawbit.cpp` | `RecordText()` in `Printf` and `PrintfUnshaded` |
| `FeLib/Source/sfx.cpp` | `IsHeadless()` declines to open an audio device |
| `Main/Source/main.cpp` | `ParseArgs`, seed override, `Shutdown`, `--help` text |

`TraceFrame()` hashes **`DOUBLE_BUFFER`, before `PrepareBuffer()`**. This is deliberate:
`PrepareBuffer()` applies stretch regions and xBRZ scaling, so hashing its output would make traces
depend on the user's `GraphicsScale`/`DungeonGfxScale` config and be useless in CI. It is also what
makes `--headless` free, since nothing skipped has run yet.

### Record format

```
# IVAN differential harness recording
# K <seq> <frame> <rng> <key>
ivan-record 1 seed=999 ivan=0.59
K 1 0 0 13	# enter
# end keys=8
```

`<seq>` is an integrity check — an out-of-sequence value is a hard failure, which catches edited or
spliced recordings. `<frame>` and `<rng>` are debug context only; replay never waits on them. The
`# end keys=N` trailer is **required**: it distinguishes a complete recording from one cut short at a
line boundary, which is the shape a crash leaves. `RecordKey` flushes every key as it writes it, so a
crashed session loses nothing but the trailer — which is what makes a browser crash report carry a
deterministic reproduction (§9.6).

Useful key codes: `ENTER`=13, `ESC`=27, `UP`=328, `DOWN`=336, `LEFT`=331, `RIGHT`=333, `HOME`/NW=327,
`PGUP`/NE=329, `END`/SW=335, `PGDN`/SE=337, `>`=62, `?`=63, `S`=83, `` ` ``=96 (wizard mode), `~`=126
(autoplay). **Movement is the arrow and page keys.** The in-game help advertises `789/456/123`, but
those are numpad *scancodes* — the plain ASCII digits do nothing at all. `play.py keys` prints the
whole table.

### Trace format

Two files, and **which one a fact belongs in is the question worth getting right** (§6.10d). The game
trace outlives the renderer; the frame trace is replaced by whatever draws the game next.

```json
{"trace":"game","ver":2,"seed":999}
{"step":0,"turn":0,"tick":0,"dl":[0,0],"pos":[0,0],"hp":0,"chars":0,"items":0,"rng":0,"grng":0,"nest":0,"depth":0}
```

```json
{"trace":"frames","ver":2,"visual_seed":6818,"w":800,"h":600}
{"frame":1,"hash":"1f9775f69f20a1a2","vrng":0,"index":0}
```

The game trace is sampled from `game::Run`'s loop — one iteration is one step of the game, and none
of it depends on anything being drawn. A record is emitted when the game moved, so a game blocked on
a key costs one line. It carries **no frame number on purpose**: that was the one screen-derived
quantity that could have stayed, in the file whose point is not to have any, and a recording's `K`
lines carry `rng` beside the frame, so a key still cross-references to a record through a number
both files take from the game.

The frame trace is sampled from `BlitDBToScreen`. Frames identical to the preceding one are omitted,
so frame numbers skip, and `vrng` — the presentation generator's draws — is the only draw count in
it.

**`rng` is the diagnostic that matters.** When two builds diverge, the RNG counter localises it to a
specific draw — usually long before anything visible changes. Without it you are bisecting pixels;
with it you get something close to a stack trace. Sampling per game step rather than per frame made
it sharper and the goldens larger: `autoplay-2000` went from 2,759 records to 49,152, 273KB to
6.9MB, 64KB to 746KB gzipped. That is the price of localising a divergence to a game step, and the
trigger is one function (`SameState`, `harness.cpp`) if it ever needs paying back.

`grng`, `nest` and `depth` are the draw-attribution fields of §6.5a. Both count the **game**
generator only: presentation draws are `visualrand`'s and reach neither (§6.10c). `rng` counts every
draw on it including ones inside a `femath::SaveSeed`/`LoadSeed` bracket that are later discarded;
**`grng` is the same count with those excluded, and it is the one to compare first** — it says
whether the *game* diverged or only its animation. `nest` must stay 0, and since §6.10c there is one
bracket left in the tree that could move it (`character::ActivateRandomState`).

### Screen capture

A frame hash proves two builds agree; it says nothing about what is on the screen. Two outputs, and
you want both:

- **PNG of the double buffer** — the map, the sprites, the terrain.
- **Text layer**, captured in `rawbitmap::Printf`/`PrintfUnshaded`, the two funnels every glyph passes
  through, so it is complete by construction rather than by enumeration. The side panel, message log,
  menus, prompts and the in-game help all come back as real text instead of 8×8 pixel blocks.

`--shot` writes both for one frame; `--text` logs every string for the whole session, which is how
you read something that scrolled past. The capture point is **replay exhaustion** — the moment the
game asks for a key it does not have, which is exactly the screen a player would be looking at when
they pressed the next key.

Four details that were not free:

- **Text is handed over one frame late, on purpose.** Strings accumulate as they are drawn and move
  to the "current frame" set at the next `BlitDBToScreen`. Without that two-stage handover a shot
  taken from `Shutdown` — which runs *after* the last blit — would find the accumulator cleared and
  report no text on a screen that plainly has some. A frame that draws nothing keeps the previous set
  and the sidecar says so.
- **Strings overdrawn within one frame are dropped from the layout view.** The panel is redrawn after
  a move, so `Turn 0` and `Turn 1` both land at (704,360) and only the second is visible. The
  chronological draw-order list keeps both.
- **`buf` and `db` tags.** Strings drawn into an offscreen bitmap — how menus, inventory lists and
  the message area are built — are tagged `buf`, with coordinates relative to that bitmap; strings
  drawn straight to the screen are tagged `db`. Both are kept: filtering to `db` would lose every
  menu.
- **The PNG writer is ~90 lines by hand, using deflate stored blocks.** `bitmap::Save(cfestring&)`
  writes a *BMP* despite the name it is usually given, stages it through a `.tmp` moved on close —
  wrong for a capture meant to survive a crash — and emits no BMP row padding, so it is only correct
  when the width is a multiple of four. libpng is linked into FeLib but zlib only transitively, so
  compressing would mean linking one more library than the build needs. Stored blocks need no
  compressor at all. The cost is size, ~1.4MB at 800×600, which is why `--shot-dir` skips frames
  identical to their predecessor.

## savediff — `tools/savediff/`

Compares two saved games. A save is several files (`<stem>.sav`, `<stem>.wm`, and one
`<stem>.<dungeon><level>` per generated level), so the directory form is normal. Files pair by **role
suffix, never by name**, because `game::SaveName` stamps the stem with a timestamp.

```
savediff [options] <dir-a> <dir-b>
  --summary               verdict and hash table only
  --json                  JSONL, one object per role
  --window N              hexdump context around first difference (64)
  --ignore-timespent      SUSPECT verdicts do not affect exit status
  --word-size 32|64       width of long/ulong in the saves (default 64)
  --known-nondeterminism  inventory of legitimate divergences
Exit: 0 all match, 1 something differs, 2 error.
```

**It is a native-vs-WASM oracle, and a sharper one than frame hashing for anything that does not
reach the screen** — which is exactly the class §6.4 is about. §6.8 made `long`/`ulong` write as 8
explicit little-endian bytes on every target, which closed the container-length problem with it, so a
WASM save and a native save are byte-identical but for `GetTimeSpent`. `--word-size` survives for
reading saves written before that change — its help text still describes the world before it, which
§7.9d records. `graphicid` and `configid` were the two remaining raw `sizeof` writes and are now
audited: `configid` is field-by-field, `graphicid`'s layout is pinned by `offsetof` asserts on both
targets rather than converted, and nothing saved can reach 32 bits in a reachable game. §7.9 has the
measurements and what a future `SAVE_FILE_VERSION` bump should carry.

- **Use savediff rather than a hand-rolled `cmp` loop.** The auto-play corpus produces two files
  whose extension is `.40` — `<stem>.40` and `<stem>.AutoSave.40` — so pairing by extension silently
  compares a 892KB file against a 1,067KB one and reports six figures of difference. savediff pairs
  by role. Same reason `.bkp` files need `--include-backups` rather than being globbed in by
  accident.
- **Decoding stops at three cliffs** (`dungeonscript` in `.sav`, `Room` in level files,
  `wsquare::Save` in `.wm`). Past those lies the polymorphic class graph, and a partial reader that
  appears to work on one save will desynchronise on another and emit confidently wrong field labels —
  strictly worse than raw byte offsets. **Do not extend decoding past them.**
- **Level files exist only for levels actually visited.** An `ONLY_A`/`ONLY_B` role is a real finding
  — the runs took different paths — not a pairing bug.
- **The `.<D><L>` extension is ambiguous**: ELPURI_CAVE levels 10–12 and FUNGAL_CAVE levels 0–2 both
  produce `.110`/`.111`/`.112`. Keep it opaque; decompose only for display.
- **`GetTimeSpent` is one byte, and it is flaky rather than fill-dependent.** It is the low byte of an
  8-byte little-endian long that reads 0 or 1 — whether the replay crossed a wall-clock second. 8
  runs at a *fixed* `MALLOC_PERTURB_` give two distinct `.sav` files because of it. That also means a
  two-run `.sav` comparison agreeing proves nothing; §6.5a's eight-run rule applies here too.
- `savediff --known-nondeterminism` prints the full inventory of legitimate divergences.

## Hazards

Things that have cost real time here, beyond the flags above.

- **`PORTABLE_BUILD=ON` is required for testing.** Without it `DATADIR` is baked in as
  `/usr/local/share/ivan` and the binary aborts with `Script/define.dat not found` unless you install
  it. Portable mode resolves data relative to the launch directory — which is also why every test run
  needs its own directory.
- **`FeLib/CMakeLists.txt` uses `file(GLOB ...)`.** An incremental `make` in an existing build
  directory silently misses new files. Re-run `cmake`, don't just `make`.
- **A modal alert during a replay eats a recorded key** and desynchronises the run. Anything that can
  reach `iosystem::AlertConfirmMsg` on a failure path is a hazard for the harness — which is why
  `--headless` declines to open an audio device rather than opening one and handling the failure, and
  why `.bkp` backups are off on the browser target.
- **A `--text`-only difference deserves the same alarm as a cross-target one.** It says the golden is
  not what the game does, it is what the game does *while being watched*. Twice now that has been
  pointing at a real bug in the game rather than at the harness (§6.6, §9.11).
- **valgrind's `write`/`writev` frames name whichever call flushed the buffer**, not the call that
  put the bad bytes in it. Make the stream unbuffered for one diagnostic run (§6.4a) before believing
  the stack.
- **A value that looks like a pointer is evidence about the chunk's history, not the writer's
  intent** (§6.4b). And a raw pointer is not an identity once the object it named can be freed
  (§9.11).
- **A player's configuration is an input to the game's random stream.** `game::PosCurrentlyOnScreen`
  is built from the window size and `DungeonGfxScale`, so a `RAND()` behind a visibility test puts
  the player's zoom into the stream the monsters roll from — four effects were doing it (§6.10).
  Anything that draws per on-screen square, or returns early when something is off screen, belongs on
  `VRAND` (§6.10c); `compare-configs.sh` is the check, and nothing in CI runs it. **The test is not
  "is this drawing pixels"** — a blood stain is not what anyone pictures as a visual effect and is
  the site the §6.10 code read walked past. It is *could the number of times this runs depend on
  anything outside the game*.
- **A save/restore bracket around a draw is a second generator spelled the expensive way** (§6.10c).
  If the reason for a bracket is "this must not advance the shared stream", the honest shape is a
  separate stream; a bracket is right only when the *same* stream must be rewound, which in this tree
  is one caller.
- **Check the deployed bytes, not the deploy command's exit code.** `wrangler` without
  `--branch main` labels the upload `Preview`, prints a URL that serves the new build perfectly, and
  leaves the production site on the previous deployment (§9.10).

## Reading the log

`docs/port-log.md` keeps the findings under stable `§` numbers, cited from comments across the tree.
The ones worth knowing about before touching anything:

| | |
|---|---|
| §6.3 | architecture facts — the MT width argument, the software double buffer. Don't re-derive these |
| §6.4a | the measurement technique behind every uninitialized-memory fix here |
| §6.5a | the diagnostics in the trace, and the eight-run rule |
| §6.6 | why `= default` on a struct that gets memcmp'd or raw-written is a bug |
| §6.9 | the strict aliasing violation, and why a Heisenbug that evaporates when you take an address is this class's signature |
| §6.10 | the player's zoom in the game's RNG — and why the code read found the sites that did not matter while the differential test found the ones that did |
| §6.10c | the game and presentation generators, why the brackets that preceded them were the same idea at three times the cost, and which mistake each corpus script can and cannot see |
| §6.10d | why the trace is two files, and which facts belong in the half that outlives the renderer |
| §9.4 | the twelve places the program left a choice to the compiler, and the technique that found them |
| §9.7 | why the audio boundary sits below the regex |
| §9.12 | why the bridge contract test exists, and what the browser suite is replacing |
