# IVAN Differential Test Harness — Progress & Handoff

**Status:** working and verified. Thirty-four commits on `master` of the fork (§8). Nothing has
been offered upstream. The corpora are committed (§5b), the host libm is no longer an input
(§6.7), and the one known Emscripten build error is fixed (§6.8).

**Seam 1 is open and half proven.** The WASM build runs headless under bare node (§9.3's
no-video path, now `--headless`), and on the non-combat corpus a native replay and a WASM
replay produce **byte-identical frame traces, text logs, screenshots, world maps and level
files**. The auto-play corpus agrees for its first 389 frames and then diverges inside a long
combat; what is found and fixed, what is left, and how to hunt the rest is §9.4.

**The game is also playable in a browser.** `-DWASM_BROWSER=ON` produces a page that boots to
the main menu, generates a world and takes keystrokes — §9.5. That is a second host for the
same WASM core, not progress on seam 1, and the two should not be conflated: the browser build
is unmeasured against the goldens and is not an oracle.
**Last updated:** 2026-08-16

---

## 1. Why this exists

The long-term goal is porting IVAN to the web as a **WASM core + TypeScript frontend**
(keep `Main/` — ~106k lines of game logic — compiled to WASM via Emscripten; rewrite only
the presentation layer in TS, where scaling, the message log, inventory and config UI become
tractable).

IVAN has **no test suite**. Zero. No `add_test`, no gtest, no Catch2. Rewriting or porting
100k+ lines of 25-year-old C++ with no oracle is how forks die. So before any port work
starts, we need a way to prove the new thing behaves like the old thing.

This harness is that oracle. It records a session, replays it deterministically, and hashes
every frame so two builds can be compared byte-for-byte. It also **captures the screen** —
as a PNG and as a text layer — which is what makes it possible to steer the game to a state
worth testing in the first place (§4, §5a).

### The two seams (important — do not conflate)

1. **Framebuffer equality** — while C++ still renders. Native vs WASM must produce identical
   frame hashes. This validates the *toolchain* (Emscripten, libm, float behaviour).
2. **State-stream equality** — once the TS frontend takes over rendering. Pixels will
   legitimately differ then (that's the point of the rewrite); the golden master becomes the
   state crossing the C API.

Seam 1 is where we are now. Pixel comparison validates the *port*; state comparison
validates the *rewrite*. You need both, in that order.

---

## 2. Current state

### Verified by direct measurement (not agent claims)

| Test | Result |
|---|---|
| Clean build from scratch | exit 0, **128 warnings** — 79 pre-existing `-Wstringop-overflow=`, plus 49 format warnings §7.7 made visible |
| `sizeof(graphicid)` under every combination of `-DGCC` / `-DVC` | **48 everywhere** — layout no longer depends on a build flag |
| Headless boot (no display, no ALSA) | reaches main menu |
| Replay end-to-end | exit 0 |
| Two replays → trace comparison | **byte-identical** |
| Two replays over **1,067,118 RNG draws** + full world gen | **byte-identical** (155 frames) |
| Same, under 100% CPU load on all cores | **byte-identical** |
| Non-combat corpus, **8** isolated runs (trace, PNG, text) | **1 distinct outcome**, 441 frames every time |
| Combat corpus, 200 auto-play turns, 8 isolated runs | **1 distinct outcome** — was 5, see §6.5 |
| Combat corpus, **2,825** auto-play turns, 16 concurrent runs on a saturated machine | **1 distinct outcome**, 3,880 frames, 9,687,815 RNG draws |
| Same binary, harness options varied (`--text` on/off) | **1 distinct outcome** on both corpora — the old 2 predates §6.6a, see §6.6 |
| savediff exit codes (0 same / 1 differ / 2 error) | correct, with side-by-side hexdump |
| Two replays → screen PNG, text sidecar and text log | **byte-identical** |
| savediff on real saves from two identical replays | `.sav` SAME, `.wm` SAME, level file **DIFF** — see §6.4 |
| Crashes across 78 runs of all corpora | **none**, every run exit 0 |
| `valgrind` uninitialized reads, non-combat corpus, after §6.6c | **0 errors / 0 contexts**, from 137 / 13 |
| `valgrind` uninitialized reads, auto-play corpus, after §6.6d | **0 / 0**, from 283 / 3 |
| Player HP under three different `MALLOC_PERTURB_` values, after §6.6a | **identical**; was three different values |
| Uninitialized bytes reaching a level file, after §6.6d | **0** on both corpora, every level file; was 1,860 then 400 |
| Level file across 8 isolated **ordinary** runs (ASLR on, no fill), after §6.6d | **1 distinct**, from 8 — no `MALLOC_PERTURB_`, no `setarch` |
| Both corpora committed, replayed 8× each against committed goldens | trace, text log and PNG **1 distinct** and matching golden, ~9s total |
| Host-libm calls made by the game after §6.7 | **0** on both corpora, from 27,496 |
| musl vs glibc on those 27,496 calls | **473 differ**, all 1 ulp — and no observable state moves (§6.7) |
| Saves before vs after §6.7 and §6.8 | **byte-identical**, except the one `GetTimeSpent` byte |
| `long long` (Emscripten's `time_t`) against `save.h` | **compiles**, was a hard overload failure (§6.8) |
| Configure under {CMake 3.28.3, 4.0.3} x {`IGNORE_EXTRA_WHITESPACES` OFF, ON} after §9.2 | **0 warnings, 0 errors** in all 4; was 1 deprecation + 1 hard error |
| Corpora replayed against goldens using a **CMake 4-built** binary | **8/8 match** on both corpora |
| PCRE vs `std::regex` on all 154 shipped patterns (§9.2) | **0 mismatches** in 111,342 comparisons |
| `emcmake cmake` over the whole tree after §9.3 | **0 warnings, 0 errors** |
| Emscripten build of the whole tree | exit 0 — `ivan.js` + a 6.2MB `ivan.wasm` |
| `sizeof(long)` under `wasm32-unknown-emscripten` | **4** — measured, not assumed; the premise of §6.8 |
| Native build and both corpora after the §9.3 CMake change | unchanged, **8/8 against goldens** |
| WASM binary run under node, `--headless` | **runs both corpora to completion**, 2.4s for non-combat |
| Native `--headless` vs native windowed, both corpora | trace, text log, screenshot and sidecar **byte-identical** |
| **Native vs WASM, non-combat corpus** | trace, text log, screenshot, sidecar, `.wm` and level file **all byte-identical**; `.sav` differs by the one `GetTimeSpent` byte |
| **Native vs WASM, auto-play corpus** | identical for 389 of 593 frames, and `.wm` and the descent level file are byte-identical; then diverges (§9.4) |
| `portmath` output under GCC vs under Emscripten, 14,000 evaluations | **bit-identical** — §6.7's pin holds across compilers, not just across libms |
| `emcmake cmake -DWASM_BROWSER=ON`, then build | **0 warnings, 0 errors**, exit 0 — `ivan.html` + a 3.3MB `ivan.data` |
| Node-host defaults after the browser option landed | unchanged: `NODERAWFS=ON`, `ivan.js`, no preload |
| Browser build loaded in headless Chrome 143 | **reaches the main menu** — art, fonts and all five entries |
| Same, driven 8x ENTER over CDP | **intro, world gen, character, world map**, "Turn 0", side panel populated |
| Page console for that whole run | one line, `MidiOutDummy: This class provides no functionality.` — no abort, no exception |
| Release WASM inspected for symbols | **no `name` section, no DWARF** — a trap's offsets are unrecoverable (§9.6) |
| Same build with `WASM_DEBUG=ON` | `name` section present, 0x8b9f4 bytes; 8.3MB against 7.5MB |
| Browser recording pulled from MEMFS, replayed natively | exit 0 — **358 frames** (8 keys) and **752 frames** (403 keys) |
| 400 turns of auto-play in a browser under `WASM_DEBUG` | **no trap, no assertion** — does not reproduce the reported crash (§9.6) |
| Level file across 4 ordinary runs, after §6.6e | **1 distinct** on both corpora; was 4 |
| Uninitialized bytes reaching a level file, after §6.6e | **0** on both corpora, `MALLOC_PERTURB_` 42 vs 99 |
| `valgrind` uninitialized reads, after §6.6e | **0 / 0** on both corpora |

Everything above was re-measured from a clean build on 2026-08-15 and still holds. The
level-file divergence is now **closed for both families the corpora reach** — `character`
(§6.6c) and `fluid`/`trapdata` (§6.6d, which took §7.6a with it). The row above it, from §6.4,
predates that and is what savediff reported before either fix.

The CPU-load test is the meaningful one *for the risk it was designed to test* — wall-clock
leaking into game state. Saturating the machine and still getting identical output is real
evidence about the clock.

**Read the determinism rows narrowly.** Combat is reproducible — §6.5 is found and fixed — and
as of §6.6c it no longer depends on the allocation pattern either: the trace and the whole
string stream are byte-identical across three `MALLOC_PERTURB_` values and across both harness
option sets. As of §6.6d **valgrind reports no uninitialized read on either corpus**, and the
level file reproduces across ordinary runs with no fixed heap fill and no ASLR trick. That was
the specific thing blocking seam 1, and for the paths these two corpora reach it is gone.

It is **not** gone in general, and the reason is coverage rather than a known remaining defect:
neither corpus visits the whole world map, `.wm` still has the §6.2/§7.6 residue, and every
number here is a property of the levels these 210 keys generate. Treat native-vs-WASM as
unblocked for the reached paths and unproven elsewhere, and keep widening the corpus before
claiming more. The way §6.6d found a family that three prior passes had walked past is the
argument for not trusting a clean valgrind as coverage — and §6.6e is that argument being
proved right a second time, by two fields no earlier pass had reached.

**The determinism rows and the native-vs-WASM rows measure different things, and the second is
much harder.** A build reproducing itself only needs the *program* to be deterministic. Two
builds agreeing needs every place the program leaves a choice to the *compiler or the standard
library* to have been closed as well, and §9.4 is a list of a dozen such places that no
same-build test could ever have found.

Two rules for anyone measuring determinism here, both learned by getting it wrong first:
**give every run its own directory** (a portable build writes saves and question history into
the launch directory, so runs contaminate each other), and **use at least 8 runs** — the
combat divergence is flaky enough that pairs agree by chance often enough to fool you.

The capture layer being byte-identical across runs matters too: it means a PNG or a text
layer can be a golden artifact in CI, not just a debugging aid.

### Not yet verified

- **The rest of the auto-play corpus, native vs WASM.** It agrees for 389 of 593 frames and then
  diverges in combat. §9.4 says what has been ruled out and how to continue.
- **Uninitialized reads outside the two corpora.** Both corpora are clean under memcheck as of
  §6.6e; nothing is known-broken and unfixed. What is unverified is everything they do not
  reach — the rest of the world map, and the `.wm` residue of §6.2/§7.6. §6.6e is the second
  time a corpus change alone surfaced a family three earlier passes had walked past, so read a
  clean memcheck as "clean on what this corpus touches" and nothing more.
- **Unsequenced draws and library ties outside what §9.4 fixed.** The scan behind §9.4 finds
  *visible* draws sharing a statement. A visible draw unsequenced with a draw hidden inside a
  called function is the same bug and no grep finds it — that is what
  `SetLTerrain(GTerrain->Instantiate(), OTerrain->Instantiate())` was, and it was the most
  consequential site of the lot. Likewise the comparators audited were the ones the corpora
  reach.
- **DJGPP / SDL1 branches.** Edited but no toolchain available to compile them. `--headless` is
  guarded in the SDL branches only; the DJGPP `BlitDBToScreen` still writes to the VESA frame
  buffer.

The auto-play AI **does** run now (§7.2) and generates thousands of turns of real play, and as
of §6.5 it reproduces: 2,800 turns including 36 deaths replay identically 16 times over. It is
a usable differential corpus for native-vs-native work.

---

## 3. Build and run

### Dependencies

```
sudo apt-get install -y libsdl2-dev libsdl2-mixer-dev libpng-dev
```

All three are required. Without `libsdl2-mixer-dev` the link fails. `libpcre3-dev` used to be a
fourth — §9.2 replaced it with `std::regex`.

### Build

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DWIZARD=ON -DPORTABLE_BUILD=ON
cmake --build build -j$(nproc)
cmake --build build --target savediff      # separate: savediff is EXCLUDE_FROM_ALL
```

Two gotchas that will waste your time:

- **`PORTABLE_BUILD=ON` is required for testing.** Without it, `DATADIR` is baked in as
  `/usr/local/share/ivan` and the binary aborts with `Script/define.dat not found` unless
  you actually install it. Portable mode resolves data relative to the launch directory.
- **`FeLib/CMakeLists.txt` uses `file(GLOB ...)`.** An incremental `make` in an existing
  build dir will silently miss `harness.cpp`. CI must re-run `cmake`, not just `make`.

`savediff` is deliberately `EXCLUDE_FROM_ALL` and links nothing — it is the differential
oracle and must build even when the game build is broken.

### Build for WASM

```bash
source ~/emsdk/emsdk_env.sh
emcmake cmake -S . -B build-wasm -DCMAKE_BUILD_TYPE=Release -DWIZARD=ON -DPORTABLE_BUILD=ON
cmake --build build-wasm -j$(nproc)      # produces build-wasm/Main/ivan.{js,wasm}
```

**The toolchain is pinned to emsdk 6.0.6** (`./emsdk install 6.0.6 && ./emsdk activate 6.0.6`),
and pinning it is the same argument as pinning musl in §6.7: a different LLVM can change float
codegen or struct layout, and the first symptom would be an unexplained frame-hash divergence
thousands of frames from the cause. `latest` is not a toolchain, it is a moving target.

SDL2, SDL2_mixer and libpng come from Emscripten's own ports rather than the host — see §9.3
for what that changed in the CMake files and why the flags are global. Four build options exist
only on this target: `WASM_NODERAWFS` (ON, real filesystem under node — the harness needs to
write its trace somewhere), `WASM_ASYNCIFY` (ON, for interactive input; a replay does reach
`SDL_Delay` through the menu fade, so leave it on), and `WASM_BROWSER` / `WASM_PRELOAD_AUDIO`,
both below.

### Build for the browser

```bash
source ~/emsdk/emsdk_env.sh
emcmake cmake -S . -B build-web -DCMAKE_BUILD_TYPE=Release -DWIZARD=ON -DPORTABLE_BUILD=ON \
  -DWASM_BROWSER=ON
cmake --build build-web -j$(nproc)   # ivan.{html,js,wasm} + a 3.3MB ivan.data
emrun --no_browser --port 8111 build-web/Main
```

Use a **separate build directory** from `build-wasm`. That one is the seam-1 oracle and the two
hosts do not share a configuration.

`WASM_BROWSER` flips the target from node to a page: `WASM_NODERAWFS` defaults off (it binds to
node's `fs` module, and setting both is a hard error rather than a link that half works),
`Graphics/` and `Script/` are preloaded into `ivan.data` at the MEMFS root, and the executable
is emitted as `.html` so emcc generates the page and the canvas with it — Emscripten's SDL2 port
looks up `#canvas` at video init and has nothing to bind to otherwise.

The preload paths are absolute because `PORTABLE_BUILD` makes `game::GetDataDir()` and
`GetUserDataDir()` both return `"./"` (`game.cpp:5426`, `save.cpp:814`) while the module starts
with its working directory at `/`, so the `"./Graphics/..."` every call site builds resolves to
exactly the `/Graphics` in the package.

`WASM_PRELOAD_AUDIO` (OFF) adds `Music/` and `Sound/`. They are ~28MB against the other two
directories' 3.5MB and the game runs without either — a missing `Sound/SoundEffects.cfg` leaves
`initSound` at `SoundState = -1` (`sfx.cpp:131`; the `ABORT` beside it is commented out) and the
wavs load lazily per effect, while `audio::LoadMIDIFile` only pushes a filename onto a vector
(`audio.cpp:449`) and RtMidi is a dummy on this target regardless. Untested with it ON.

`emrun` rather than `python3 -m http.server` because it serves `.wasm` as `application/wasm`
without argument. It has to be HTTP either way — `file://` will not fetch the wasm. No
COOP/COEP headers are needed, there being no pthreads and no `SharedArrayBuffer`.

**`-fexceptions` is not optional on this target and is not a tuning knob.** Emscripten disables
exception *throwing* by default, so `__cxa_throw` lowers to `abort()`, and IVAN throws as
ordinary control flow: `areachangerequest` on every level change, `quitrequest` on quit,
`genericException` out of the prototype database. Without it the WASM build dies drawing the
main menu with a bare `Aborted(undefined)` and no stack. It is in the global compile *and* link
flags in `CMakeLists.txt` for the §7.7 reason — the compile half is what emits the landing pads,
so a translation unit built without it cannot catch what another one throws.

### Run headless

```bash
./ivan --replay in.rec --trace out.jsonl --seed 999 --headless          # native
node build-wasm/Main/ivan.js --replay in.rec --trace out.jsonl --headless   # WASM
```

Run from a directory containing `Graphics/ Script/ Music/ Sound/` (symlinks are fine).

`--headless` is what makes those two command lines the same program. It skips the window, the
renderer, the streaming texture, the final blit and `Mix_OpenAudio`, and skips nothing else:
rendering is software into the bitmap double buffer either way, and `TraceFrame()` hashes that
buffer before any of the skipped work. Native runs produce byte-identical traces, text logs and
screenshots with it and without it, measured on both corpora.

It replaces `SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy`, which still works natively and is
still passed by `run-corpus.sh` as a belt-and-braces measure. Two reasons the decision had to
move into the program: Emscripten's SDL2 port binds to the DOM at video init whatever the
driver hint says, and **Emscripten does not forward the process environment into the module**,
so under node those two variables reach nothing at all.

### Reproduce the determinism test

```bash
# hand-authored recording: 8x ENTER starts a game (world gen, ~1.07M RNG draws)
{ echo '# probe'; echo 'ivan-record 1 seed=999 ivan=0.59'
  for i in $(seq 1 8); do printf 'K %d 0 0 13\t# enter\n' $i; done
  echo '# end keys=8'; } > probe.rec

for n in a b; do
  SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
    ./ivan --replay probe.rec --trace gen$n.jsonl --seed 999 > /dev/null 2>&1
done
diff gena.jsonl genb.jsonl && echo IDENTICAL
```

---

## 4. The harness

### CLI

```
--record   [file]     Record every key the game reads to file.
--replay   [file]     Play back a recording instead of reading real input.
--trace    [file]     Write a per-frame JSONL hash trace to file.
--seed     [number]   Pin the RNG seed. Continuing a saved game ignores this
                      (the seed is stored in the save).
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
  void TraceFrame();

  inline truth IsCapturingText();
  void RecordText(const void* Target, int X, int Y, int Color, cchar*);
  void WriteShot(cchar* FileName, cchar* Reason);

  inline truth IsHeadless();       // --headless, read by graphics.cpp,
                                   // whandler.cpp and sfx.cpp

  inline void CountRand();         // ++RandCount
  inline ulong GetRandCount();

  truth HasSeedOverride();
  ulong GetSeedOverride();
}
```

**Zero-cost-when-disabled is a hard requirement** — this ships in the game. The mode queries
are inline static-bool reads; `TraceFrame()` and `RecordKey()` early-out before any
allocation, formatting or I/O.

### Integration points

| File | Hook |
|---|---|
| `FeLib/Source/whandler.cpp` | record/replay at every `GetKey`/`ReadKey` return path |
| `FeLib/Source/graphics.cpp` | `TraceFrame()` in all three `BlitDBToScreen` definitions |
| `FeLib/Source/femath.cpp` | `CountRand()` in `femath::Rand()` |
| `FeLib/Source/rawbit.cpp` | `RecordText()` in `Printf` and `PrintfUnshaded` |
| `Main/Source/main.cpp` | `ParseArgs`, seed override, `Shutdown`, `--help` text |
| `FeLib/Source/graphics.cpp` | `IsHeadless()` guards `SDL_Init`'s video bit, the window, the renderer, the texture, `SetScale`, `SwitchMode` and the blit |
| `FeLib/Source/whandler.cpp` | `IsHeadless()` guards `SDL_ShowWindow` |
| `FeLib/Source/sfx.cpp` | `IsHeadless()` declines to open an audio device |

`TraceFrame()` hashes **`DOUBLE_BUFFER`, before `PrepareBuffer()`**. This is deliberate:
`PrepareBuffer()` applies stretch regions and xBRZ scaling, so hashing its output would make
traces depend on the user's `GraphicsScale`/`DungeonGfxScale` config and be useless in CI.

### Record format

```
# IVAN differential harness recording
# K <seq> <frame> <rng> <key>
ivan-record 1 seed=999 ivan=0.59
K 1 0 0 13	# enter
# end keys=8
```

`<seq>` is an integrity check — an out-of-sequence value is a hard failure, which catches
edited or spliced recordings. `<frame>` and `<rng>` are debug context only; replay never
waits on them. The `# end keys=N` trailer is **required**: it distinguishes a complete
recording from one cut short at a line boundary, which is the shape a crash leaves.

Useful key codes: `ENTER`=13, `ESC`=27, `UP`=328, `DOWN`=336, `LEFT`=331, `RIGHT`=333,
`HOME`/NW=327, `PGUP`/NE=329, `END`/SW=335, `PGDN`/SE=337, `>`=62, `?`=63,
`S`(save and quit)=83, `~`(autoplay)=126.

Movement is those arrow and page keys. The in-game help advertises `789/456/123`, but those
are numpad *scancodes* — the plain ASCII digits `0x31`–`0x39` do nothing at all. `play.py keys`
prints the whole table.

### Trace format

```json
{"frame":-1,"hash":"0000000000000000","rng":0,"index":-1,"ver":1,"seed":999,"w":800,"h":600}
{"frame":1,"hash":"1f9775f69f20a1a2","rng":0,"index":0,"grng":0,"nest":0,"depth":0}
```

Header line carries version, seed and resolution. Frames identical to the preceding one are
omitted, so frame numbers skip. `rng` is the cumulative `femath::Rand()` call count; `grng`,
`nest` and `depth` are the draw-attribution fields documented in §6.5a — `grng` is the one to
compare first, and `nest` must stay 0.

**`rng` is the diagnostic that matters.** When two builds diverge, the RNG counter localises
it to a specific draw — usually thousands of frames before anything visible changes. Without
it you are bisecting pixels; with it you get something close to a stack trace.

Caveat: `rng` counts *every* MT draw including ones inside `femath::SaveSeed`/`LoadSeed`
brackets that are later discarded. `grng` is the same count with those excluded, which is what
you want when deciding whether the *game* diverged or only its animation.

### Screen capture — the thing that makes the game reachable

A frame hash proves two builds agree; it says nothing about **what is on the screen**. With
only hashes there is no way to decide what to press next, so no interesting game state could
be reached, so no interesting game state could be tested. That was the real reason §7.1 was
blocked — not the save code.

Two outputs, and you want both:

- **PNG of the double buffer.** The map, the sprites, the terrain.
- **Text layer.** Captured in `rawbitmap::Printf`/`PrintfUnshaded`, the two funnels every
  glyph passes through, so it is complete by construction rather than by enumeration. The
  side panel, message log, menus, prompts and the in-game help all come back as real text
  instead of 8x8 pixel blocks.

`--shot` writes both: `foo.png` and a `foo.txt` sidecar for the same frame. `--text` logs
every string for the whole session, which is how you read something that scrolled past.

The capture point is **replay exhaustion** — the moment the game asks for a key it does not
have. That is exactly the screen a player would be looking at when they pressed the next key,
so it is the right frame to review and the right frame to decide from.

Two details that were not free:

- **Text is handed over one frame late, on purpose.** Strings accumulate as they are drawn
  and are moved to the "current frame" set when the next `BlitDBToScreen` happens. Without
  that two-stage handover a shot taken from `Shutdown` — which runs *after* the last blit —
  would find the accumulator already cleared and report no text on a screen that plainly has
  some. A frame that draws nothing keeps the previous set and the sidecar says so.
- **Strings overdrawn within one frame are dropped from the layout view.** The panel is
  redrawn after a move, so `Turn 0` and `Turn 1` both land at (704,360) and only the second
  is visible. The chronological draw-order list keeps both.

Strings drawn into an offscreen bitmap — how menus, inventory lists and the message area are
built — are tagged `buf`, with coordinates relative to that bitmap; strings drawn straight to
the screen are tagged `db`. Both are kept: filtering to `db` would lose every menu.

PNG is written by hand, ~90 lines, using deflate **stored** blocks. Reasons, in order:
`bitmap::Save(cfestring&)` writes a *BMP* despite the name it is usually given, stages it
through `outputfile` (a `.tmp` moved on close — wrong for a capture meant to survive a
crash), and emits no BMP row padding, so it is only correct when the width is a multiple of
four. libpng is linked into FeLib but `harness.cpp` compiles in the DJGPP and SDL1 branches
too, and zlib is not linked explicitly — only transitively through libpng. Stored blocks need
no compressor at all. The cost is size: ~1.4MB at 800x600. That is the right trade for a
debugging capture, and it is why `--shot-dir` skips frames identical to their predecessor.

---

## 5. savediff — `tools/savediff/`

Compares two saved games. A save is several files (`<stem>.sav`, `<stem>.wm`, and one
`<stem>.<dungeon><level>` per generated level), so the directory form is normal. Files pair
by **role suffix, never by name**, because `game::SaveName` stamps the stem with a timestamp.

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

### The save format is a native-vs-WASM oracle now, and this section used to say it was not

**Measured 2026-08-15: a native `--headless` replay and a WASM one produce byte-identical `.wm`
and level files on the non-combat corpus, at the same size, decoded at the same
`--word-size 64`.** The `.sav` differs by the single `GetTimeSpent` byte below. On the auto-play
corpus the `.wm` and the descent level file are byte-identical too, and what differs afterwards
is real game state (§9.4), not format.

What this section used to say — that `long`/`ulong` serialize at native width, so a WASM save is
ILP32 and every field after the first `long` sits at a different offset — was true when it was
written and was fixed by §6.8, which writes both as 8 explicit little-endian bytes on every
target. That closed the container-length problem with it, since every container writes its size
as a `ulong`. `--word-size` survives for reading saves written before that change.

Two things are still open and are not contradicted by the measurement above: `graphicid` and
`configid` are still written with a raw `sizeof` (§7.9 item 4), so they carry host layout and
agree only because both targets happen to lay those two structs out the same way; and a `long`
holding more than 32 bits would still narrow on load under wasm32. Neither has been audited.

**Consequence for the port plan:** savediff is now a second, independent check on seam 1 rather
than a native-only regression tool — and a sharper one than frame hashing for anything that does
not reach the screen, which is exactly the class §6.4 is about.

### Other savediff notes

- Decoding stops at three "cliffs" (`dungeonscript` in `.sav`, `Room` in level files,
  `wsquare::Save` in `.wm`). Past those lies the polymorphic class graph; a partial reader
  that appears to work on one save will desynchronise on another and emit confidently wrong
  field labels — strictly worse than raw byte offsets. **Do not extend decoding past them.**
- Level files exist only for levels actually visited. An `ONLY_A`/`ONLY_B` role is a real
  finding (the runs took different paths), not a pairing bug.
- The `.<D><L>` extension is ambiguous: ELPURI_CAVE levels 10–12 and FUNGAL_CAVE levels 0–2
  both produce `.110`/`.111`/`.112`. Keep it opaque; decompose only for display.
- Run `savediff --known-nondeterminism` for the full inventory of legitimate divergences
  (`GetTimeSpent`, timestamped filenames, the reseed at `game.cpp:3458`, bone files).
- **Use savediff rather than hand-rolled `cmp` loops, and the auto-play corpus is why.** It
  produces two files whose extension is `.40` — `<stem>.40` and `<stem>.AutoSave.40` — so pairing
  by extension silently compares a 892KB file against a 1,067KB one and reports six figures of
  difference. savediff pairs by role and gets it right. Same reason the `.bkp` files need
  `--include-backups` rather than being globbed in by accident.
- **`GetTimeSpent` is one byte of the `.sav`, and it is flaky, not fill-dependent.** On the
  non-combat corpus it sits at offset 164071 as the low byte of an 8-byte little-endian long that
  reads 0 or 1 — whether the replay crossed a wall-clock second. 8 runs at a *fixed*
  `MALLOC_PERTURB_` give two distinct `.sav` files because of it, on the tip and on `4803f04`
  alike. Expect it; it is what `--ignore-timespent` is for. It also means a two-run `.sav`
  comparison agreeing proves nothing — the §6.5a eight-run rule applies here too.

---

## 5a. play.py — `tools/play/`

The driver that turns the capture layer into something you can steer. Full notes in
`tools/play/README.md`; the essentials:

```bash
python3 tools/play/play.py new --seed 999 --start   # land on the world map
python3 tools/play/play.py send down left '>'       # walk to the cave, enter it
python3 tools/play/play.py undo 2                   # rewind
python3 tools/play/play.py keys                     # key table + command crib
```

**A session is a list of keys, not a live process.** Every command rewrites the list and
replays from the main menu under a pinned seed. That is what makes it reproducible, resumable
after a crash and rewindable — `undo` is just a shorter list. It costs ~1.5s per command,
almost all world generation.

The game runs *inside* the session directory with the data directories symlinked in, because
a portable build writes its config, saves and bone files into the launch directory. Without
that, driving the game litters the repository root (`SndDebug.txt`,
`.QuestionHistory_What_is_your_name.txt`, `Save/`).

Two things learned the hard way, both now enforced in the tool:

- **Movement is the arrow and page keys, not the digits.** The in-game help advertises
  `789/456/123`, but those are numpad scancodes; plain ASCII digits do nothing at all.
- **No one-letter direction aliases.** IVAN's commands are single letters and a bare single
  character has to mean that character so `>`, `?` and `S` work as typed. An `e`-for-east
  alias silently becomes *eat* — which is exactly what happened, and why the aliases stop at
  two characters where nothing collides.

---

## 5b. The committed corpora — `tools/corpora/`

Two recordings, their golden traces and golden text logs, and the scripts that replay and
check them. Full notes in `tools/corpora/README.md`.

```bash
tools/corpora/verify-corpora.sh          # 8 runs each: self-consistency, then golden
tools/corpora/verify-corpora.sh --update # regenerate the goldens deliberately
tools/corpora/compare-targets.sh         # native vs WASM: do two builds agree? (§9.4)
```

About nine seconds for both corpora at eight runs each, which is cheap enough for CI (§7.3).

The two scripts answer different questions and you want both. `verify-corpora.sh` compares a
build against the committed goldens — *did this build change?* `compare-targets.sh` replays each
corpus on two builds and compares them against each other — *do these two builds agree?* A
change that moves both builds identically passes the second and fails the first; a
compiler-dependent expression (§9.4) does the reverse, which is why nine such bugs survived
every determinism test in this document.

| | keys | lands on | frames | RNG draws |
|---|---|---|---|---|
| `noncombat.rec` | 7 | UT lvl 1, turn 3, HP 37/37 | 365 | 1,075,023 |
| `autoplay-200.rec` | 210 | UT lvl 1, turn 161, **HP 24/35** | 592 | 1,517,713 |

Both are seed 999; `autoplay-200` extends `noncombat` with wizard mode and 200 AI actions.
The HP and frame counts are the check values a regenerated corpus is confirmed against.

**These numbers moved once, in §9.4, and the older ones are all over §6.6a–d.** Until then the
corpus landed at turn 202 with HP 29/37 and 441 / 679 frames. That is the same 210 keys and the
same seed read by GCC 13; §9.4 stopped a dozen expressions letting the compiler choose which
random draw went where, so the world these keys generate is now one world instead of one per
compiler. When comparing against a measurement in §6.6a–d, check which set of numbers it was
taken against before concluding anything.

**Why this needed committing at all.** Every determinism number in this document is relative
to these two key sequences, and until now they existed only as a paragraph describing how to
recreate them. A differential oracle whose input has to be reconstructed from prose is not an
oracle — and the port work depends on this one.

Two things the scripts enforce, both learned by getting them wrong (§6.5a): every run gets its
own directory, and the default is 8 runs rather than 2. `run-corpus.sh` also **pins the harness
options**, because §6.6 records that changing them changes the allocation history; a golden
trace only means something if the command line that produced it is fixed.

Saves are deliberately not compared by `verify-corpora.sh` — see `tools/corpora/README.md`.
Level files and `.wm` do reproduce (§6.6d, §6.6e), but `.sav` carries the `GetTimeSpent` byte of
§5, which is a wall-clock flake and gives 2 distinct `.sav` files in 8 runs on the longer corpus.
`compare-targets.sh` does run savediff and prints its verdict, but never lets it decide the exit
status, for the same reason.

## 6. Findings

### 6.1 Pre-existing bugs found and fixed

These are **not** harness bugs. They were latent in the shipping game and surfaced only
because determinism testing makes them visible.

- **`fastscriptmember() = default`** (`Main/Include/script.h`) left a POD member
  uninitialized while `Save()` wrote it unconditionally — putting **1,975 bytes of
  uninitialized heap into every `.sav` file** (423 four-byte fields). Undefined behaviour,
  a heap-content leak into a file users share, and a source of save nondeterminism.
  Fixed: `fastscriptmember() : Member() { }`.

- **`bitmap(v2)` never initialized its pixels.** `iosystem::Menu` fades in the previous
  double-buffer contents, so a fresh process blended uninitialized memory and never drew the
  same menu twice. Fixed by using the existing `bitmap(v2, col16)` overload in
  `graphics::SetMode`.

- **Four RNG streams, not one.** Game logic drew from `std::rand()` in two places while a
  background audio thread also mutated it — a genuine data race whose effect depended on the
  scheduler. And `fantasyname/namegen.cc:24` held a third generator, an `mt19937` seeded from
  `high_resolution_clock`, producing the player's default name (which reaches the screen and
  the save filename). Fixed: game decisions draw from the pinned MT via a new
  `femath::Shuffle`; the audio thread and `sfx.cpp` each got a private xorshift; namegen is
  pinned to the harness seed. The fourth stream was `clock()` in the auto-play AI — see §6.5,
  found later and much more consequential.

- **MIDI failure aborted startup.** `audio::Init` called `ABORT("MIDI Out Error")` when
  `RtMidiOut` construction threw, which happens on any machine without an ALSA sequencer —
  i.e. every standard CI runner, and WSL2. **IVAN could not launch at all on such machines.**
  `audio::isInit` existed but was read nowhere, so there was no degradation path. Fixed:
  init failure is now non-fatal, and the entry points that dereference `midiout`
  (`ChangeMIDIOutputDevice`, `GetMIDIOutputDevices`, `SendMIDIEvent`, `PlayMIDIFile`) are
  guarded. Note `ivanconfig::Initialize()` calls two of those *after* `audio::Init`, so
  without the guards an early return segfaults instead of aborting.

### 6.2 Known-unfixed

**`.wm` files differ by 17 bytes between identical replays.** Traced to uninitialized heap
from raw `SaveFile.Write()` calls over `Alloc2D`'d buffers in `area::Save`/`worldmap::Save`
and the wsquare region (358 such bytes total, confirmed with a pattern-filling allocator).
Same *class* as the `fastscriptmember` bug but a distinct family.

Deliberately not fixed: blind-zeroing those buffers risks masking a genuine "this should have
been written" bug. Needs its own investigation.

Qualification added since: in the real-save comparison of §6.4 the `.wm` files came out
**byte-identical**. So the 17-byte residue is state-dependent, not a property of every `.wm`.
That is what uninitialized heap looks like — it matches whenever the allocator happens to hand
back the same bytes — and it means a passing `.wm` comparison is not evidence the bug is gone.

Stronger qualification (2026-08-15): under the `MALLOC_PERTURB_` differencing of §6.4a — which
exposes *every* uninitialized byte that reaches a file, not just the ones that happen to differ
— `.wm` and `.sav` leak **zero** bytes on both the non-combat and the 200-turn auto-play corpus.
That is much better evidence than a plain comparison, because it does not depend on the
allocator handing back different garbage. It is still not proof: `MALLOC_PERTURB_` only covers
the heap, and neither corpus visits enough of the world map to write the whole `.wm`. Re-run it
on a corpus that does before calling §7.6 closed.

### 6.3 Architecture facts (established, don't re-derive)

- RNG is Mersenne Twister; `femath::Rand()` is the funnel, `femath::SetSeed` at
  `femath.cpp:78`. The seed is already persisted into saves (`game.cpp:3458-3460`, restored
  at `:3540`) — the game was designed to be reproducible across save/load.
- **The MT stream does not depend on the width of `ulong`, which is the single most
  load-bearing assumption in the port.** The state array is `ulong[624]`, so it is 64-bit
  natively and 32-bit under Emscripten — but `SetSeed` masks both the seed and every
  subsequent word with `& 0xffffffff` (`femath.cpp:87`, `:90`), the twist only ever combines
  words that are already 32-bit, the tempering masks are 32-bit constants, and `Rand()`
  returns `y & 0x7FFFFFFF`. Nothing can escape into the high half at either width. Checked by
  reading every line of the generator, because if this were wrong every determinism number in
  this document would be native-only and the harness would be measuring nothing.
- Rendering is entirely software into a `bitmap` RGB565 double-buffer. The **only** GPU
  contact is one streaming texture blit in `BlitDBToScreen`. This is why the WASM port is
  tractable and why the TS-frontend seam is clean.
- Input funnels through `globalwindowhandler::GetKey`/`ReadKey` only.
- `time(0)` never touches game logic (highscore timestamps and `GameBegan`/`LastLoad` only).
- No pointer-ordered iteration in logic — the one `std::map<cchar*,...>` (`script.h:129`)
  uses a content comparator, so no ASLR sensitivity.
- ~330 entity classes generated by macros (154 `ITEM(`, 110 `CHARACTER(`, plus terrain/god/
  room/material/action). Deep virtual hierarchy, 263 virtuals in `char.h` alone.
- `-ffast-math` appears in the legacy DJGPP `.mak` files but **not** in `CMakeLists.txt`.
  Keep it that way.
- **libm was a real portability risk, and is now pinned — see §6.7.** Game code calls
  `portmath::` rather than `<cmath>`, and `portmath/check-callers.py` fails on a direct
  call. The original text of this bullet named `sin`/`cos` in world generation as the
  exposure; measurement says the mechanism and the location were both wrong, which §6.7
  records. `sqrt` is safe and stays on the host (IEEE-754 mandates correct rounding;
  WASM has a native `f64.sqrt`), as do `fmod`, `floor`, `ceil` and `abs`.

---

### 6.4 Level files diverge between two identical replays — FIXED for both corpora

**Both mechanisms below are closed.** Mechanism one by §6.6c (the `character` family, 1,860 bytes
→ 0) and mechanism two by §6.6d, which found that the "raw pointers" of §6.4b were stale heap
pointers inside uninitialized `trapdata` fields rather than a second mechanism at all. A third
family turned up in the auto-play corpus once the first was out of the way, and §6.6d closed that
too. Eight ordinary replays now produce one level file. The rest of this section is kept because
the measurement technique is the reusable part, and because two of its three diagnoses were wrong
in ways worth not repeating.


Two replays of the same recording under the same seed, compared with `savediff`:

| Role | Verdict |
|---|---|
| `AutoSave.sav` | SAME |
| `.wm` | SAME |
| `AutoSave.40` (UNDER_WATER_TUNNEL level 0) | **DIFF** |

**The §7.7 padding fix shrank this by roughly 4x but did not close it.** Re-measured at
`52259cc`, non-combat corpus, 8 isolated runs — all 8 level files distinct:

| | before §7.7 | after §7.7 |
|---|---|---|
| differing blocks | 61 of 218 | **2 of 218** |
| differing bytes | 179 | **40** |
| first difference | offset 1961 | **offset 691807** |

The first difference moving from 1961 to 691807 is the informative part: the early region, where
the `graphicid` padding byte was serialized once per cached tile, is now clean. The fix did what
it was for. What is left is underneath it, and it is **two separate mechanisms**, neither of
which is the `GetTimeSpent` or `Alloc2D` candidate this section used to name. Both of those
guesses were wrong.

Note this remains a *save-content* divergence with **no frame-hash divergence at all** — trace,
PNG and text layer are byte-identical across all 8 runs. Neither mechanism reaches the screen,
which is precisely the class of bug the state-digest layer of §7.5 is meant to catch and neither
pixel comparison nor gameplay ever will.

### 6.4a Mechanism one: uninitialized heap, 1,860 bytes per level — FIXED (§6.6c)

**The 40 bytes above understate the exposure by 46x.** 40 is only what two runs happen to
differ by; the allocator usually hands back similar garbage. The real figure comes from holding
the heap fill constant and varying it:

```bash
# same corpus, two fill values, own directory each
MALLOC_PERTURB_=42 ./ivan --replay noncombat.rec ...
MALLOC_PERTURB_=99 ./ivan --replay noncombat.rec ...
cmp -l a/Save/*.40 b/Save/*.40 | wc -l          # 1,860
```

1,860 bytes differ across 44 regions, and **every one is exactly the glibc fill byte** —
`0xd5` (= `42 ^ 0xff`) against `0x9c` (= `99 ^ 0xff`), zero exceptions. Runs sharing a fill
value are byte-identical, including the level file. So uninitialized heap is the *only*
remaining content source here, and 1,860 bytes of it reach every level file.

**Now 0** after §6.6c — the whole 1,860 was one array, `character::TemporaryStateCounter`. Note
the 1,787 this section used to quote for the post-§6.6a state did **not** reproduce: a clean
rebuild of `4803f04` measures 1,860, unchanged. Re-derive the number rather than quoting it. It
is corpus-dependent, and the point of the technique is that it measures the real exposure instead
of the fraction two runs happen to disagree on.

**Pair the byte count with a layout check** — that is what turned this from a family into a
field. Group the differing offsets into contiguous runs and look at the shape before theorising:

```bash
cmp -l a/Save/*.40 b/Save/*.40 | awk '{print $1-1}' > offs.txt   # 0-based offsets
```

All 1,860 bytes sat inside **sixteen disjoint 128-byte windows**, one per character on the level,
4-byte aligned. `TemporaryStateCounter[STATES]` is `int[32]` — 128 bytes — and it was the only
field of that shape in the write path. The confirmation was better than the arithmetic: within
each window the slots holding *defined* data were exactly the ones whose `TemporaryState` bit was
set, because `Initialize` assigns a counter only where the bit is set while `character::Save`
writes all 32 unconditionally. Windows with no state bits set leaked all 128 bytes; the one
belonging to a heavily-flagged monster leaked 44.

Slots 7, 16, 17 and 19 came back defined most often, which decodes to INFRA_VISION, SEARCHING,
GAS_IMMUNITY and LEPROSY — ordinary monster class-states, exactly what you would expect. That
cross-check is worth doing: a shape that matches by accident will not also decode to something
that makes sense.

**One trap in this technique.** `MALLOC_PERTURB_=255` looks like it should give a zeroed heap
(`255 ^ 0xff == 0`) and so simulate a complete zero-init fix without touching code. It does not:
glibc fills on free with the raw byte and on malloc with the complement, but chunks served from
**tcache bypass the malloc-time fill**, so recycled memory comes back as `0xff`. A run under
`=255` is therefore a mix of `0x00` and `0xff`, not a zero heap, and it cannot be used as an
equivalence oracle for "does this fix do exactly what zeroing would". Differencing two *arbitrary*
fill values is still sound — that only relies on the two runs differing, not on the fill being
uniform.

`valgrind --track-origins=yes` catches it at the I/O boundary and names the origin:

```
Syscall param write(buf) points to uninitialised byte(s)
  ... lsquare::Save → level::Save → operator<<(outputfile&, level const*)
      → dungeon::SaveLevel → game::Save → game::EnterArea → commandsystem::GoDown
Uninitialised value was created by a heap allocation
  ... sysbase<zombie, humanoid, characterprototype>::Spawn
      → protosystem::BalancedCreateMonster → level::GenerateNewMonsters
      → dungeon::PrepareLevel → game::EnterArea
```

**So §6.4 and §6.6 are the same bug.** Monsters generated into a level by `sysbase::Spawn` carry
partially-uninitialized members, and the descend-autosave serializes them. That is the `Spawn`
family §6.6 lists as not fixed; fixing it should close both sections at once. Same class as the
`fastscriptmember` bug in §6.1 — heap content leaking into a file users share — and about the
same size (1,860 bytes against 1,975). All of that held, and §6.6c closed both as predicted.

**But do not use these stacks to pick the field.** The `write`/`writev` frames name whichever
call happened to *flush* the 8KB `filebuf`, not the call that put the bad bytes in it, so they
localise to a write path and no further. Both contexts here pointed at `lsquare::Save` and one at
`operator<<(outputfile&, graphicdata const&)`, and the actual carrier was neither — it was a
`character` array serialized further up the same buffer. The origin half of the report is sound;
the read half is not, for buffered output. The offset-layout check above is what discriminates.

### 6.4b Mechanism two: raw heap pointers in level saves (new)

The residue left after the heap fill is held constant is **not** uninitialized memory and not
wall-clock. It is pointers. `setarch -R` (ASLR off) collapses the level file to one distinct
outcome across 4 runs while the `.sav` files keep varying:

| 4 runs, `MALLOC_PERTURB_=42` | ASLR on | ASLR off |
|---|---|---|
| `AutoSave.40` | 4 distinct | **1 distinct** |
| `AutoSave.sav` / `.sav` | 2–3 distinct | 3 distinct (this is `GetTimeSpent`, §5) |
| `.wm` | 1 distinct | 1 distinct |

**Re-measured 2026-08-15: the pointers are level-dependent, not universal.** On the non-combat
corpus they do not appear at all — 8 runs at a fixed fill give **1 distinct** level file with ASLR
left on, on `4803f04` and on the tip alike. They do appear on the auto-play corpus's fought-over
level: 480 bytes differ with ASLR on against 400 with it off, so 80 ASLR-dependent bytes there.
So the count in this section is a property of what is on the level, and the corpus has to be named
alongside it.

**Closed by §6.6d, and the diagnosis below is where it went wrong.** The reasoning was sound right
up to the last step: the fields really do read as heap pointers, and really do survive the fill and
collapse under `setarch -R`. What does not follow is that someone saved a pointer. An uninitialized
`ulong` in a chunk that previously held a pointer reads exactly like this — the fill only reaches
bytes the allocator actually filled (see the tcache trap in §6.4a), and the stale value moves with
the heap base. Initializing `trapdata` took these 4 distinct level files to 1 with ASLR left on.
The lesson is the one §6.4a already states in a different key: **a value that looks like a pointer
is evidence about the chunk's history, not about the writer's intent.**

18 sites per level, 8 bytes each, every one reading as an x86-64 userspace heap pointer — and
the difference between two runs is **one constant across all 18**:

```
0x5dc30cbec2a0 → 0x62ca8e0b02a0    delta 0x507814c4000
0x5dc30cbe9340 → 0x62ca8e0ad340    delta 0x507814c4000
0x5dc30ce35b40 → 0x62ca8e2f9b40    delta 0x507814c4000
...  18 sites, 1 distinct delta
```

A single shared delta *is* the ASLR slide. Eighteen live pointers are being written into every
level file verbatim. Not yet localised to a field — the sites sit past savediff's decodable
prefix, and the deliberate rule of §5 is not to extend decoding past the cliffs. The write path
is the `lsquare::Save` chain above; start there.

This matters beyond determinism: a pointer in a save file is a heap-address disclosure in a file
players share, and it cannot survive a reload on any platform, let alone a WASM one.

### 6.5 Replays diverge once monsters act — FOUND AND FIXED

**Cause: `clock()` was the auto-play AI's random number generator.**

Nineteen decisions in the `AutoPlayAI*` family drew from `clock()` — process CPU time — instead
of the pinned Mersenne Twister. `femath::SetSeed` does not reach `clock()`, so two replays of one
recording gave the AI different decisions, and the run diverged in real game state the moment it
was making any.

That is exactly why the symptom appeared "once monsters act". These functions only run under
auto-play, and the branches that carry them fire when a fight starts: `AutoPlayAIPray` is gated
on `StateIsActivated(PANIC) && clock()%10==0` and then picks its god with `clock()%iKGTot`;
`AutoPlayAIDropThings` fires on `clock()%100<5` once burdened; `AutoPlayAINavigateDungeon`
picks wander durations and retreat targets the same way.

Sites, all now `RAND_N`: `char.cpp` 2929, 3007, 3018, 3035, 3075, 3331, 3343, 3346, 3361, 3383,
3458, 3497, 3498, 3500, 3502 (`AutoPlayAIDropThings`, `IsAutoplayAICanPickup`,
`AutoPlayAINavigateDungeon`, `AutoPlayAIPray`) and `human.cpp` 3714, 3765, 3797
(`humanoid::AutoPlayAIequip`).

This is the same family as the three streams in §6.1 and brings the count to four: game logic
also drew from `std::rand()`, from `namegen`'s own `mt19937`, and from `clock()`.

Measured after the fix:

| Corpus | Runs | Distinct outcomes |
|---|---|---|
| 200 auto-play turns (the corpus that gave 5 of 8) | 8 sequential, isolated dirs | **1** |
| 2,800 auto-play turns, 36 deaths, 2 dungeon levels | 16 concurrent, machine saturated | **1** |

Identical down to the frame count, the frame hashes, the cumulative RNG count and the full
string stream.

**All three of the leads previously recorded here were wrong**, which is worth keeping because
each cost real time:

- **`SaveSeed`/`LoadSeed` nesting: disproved.** The harness now counts bracket depth and reports
  it per frame; `nest` is 0 across every run of every corpus measured, and the brackets are
  balanced. They cannot nest.
- **`UpdateTick`: already pinned.** `whandler.h:102` makes `Tick` a draw counter during replay,
  and the standby-animation loop that would otherwise call it is skipped entirely because
  `GetKey` returns the replay key before reaching it.
- **`SeedModifier` / visual randomness: not the carrier.** Bracketed draws are 92 out of
  1,748,754 in the 200-turn corpus. Visual randomness is a rounding error in this corpus, and
  bracketed draws are discarded by `LoadSeed` anyway.

The instrument that actually found it was the `clock()`/`SDL_GetTicks()` inventory plus the
per-frame draw attribution below. Nothing was learned by reasoning about the leads.

### 6.5a Diagnostics added while chasing §6.5 (keep these)

The trace now carries three more fields per frame:

```json
{"frame":684,"hash":"f311d7a6648ef42f","rng":1748754,"index":681,
 "grng":1748662,"nest":0,"depth":0}
```

- `grng` — draws made **outside** any `SaveSeed`/`LoadSeed` bracket, i.e. the ones that actually
  advance the game stream. `rng - grng` is the visual randomness.
- `nest` — cumulative count of `SaveSeed` calls entered at depth > 0. **Must stay 0.** If it ever
  goes positive, the single `mtb` backup slot is silently corrupting the game stream and that is
  a real bug, not a diagnostic curiosity.
- `depth` — bracket depth at the frame, which catches an unbalanced bracket.

`grng` is what turns "the traces differ" into "the *game* diverged" versus "only the animation
did", in one field. Reach for it first.

Other wall-clock reads, checked and cleared for this corpus but **not** safe in general:
`hiteffect.cpp:70`/`:419` (effect lifetime on a 3s CPU-time timeout), `hiteffect.cpp:249-252`,
`item.cpp:168`, and ~20 animation sites in `game.cpp` (2131–2912). A temporary probe that logged
every clock read showed **none of them is reached** in a headless replay — the only live site is
`game.cpp:4219`, whose decision at `:4199` is already replay-guarded. They will bite as soon as a
corpus enables the alt-silhouette or throws an item, and they reach pixels, so they break frame
hashing rather than game state.

Reproduce the original divergence on a pre-fix binary (the sample size matters — see below):

```bash
for i in $(seq 1 8); do
  mkdir -p r$i && ( cd r$i
    for d in Graphics Script Music Sound; do ln -sfn ../../$d .; done
    SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy ./ivan \
      --replay auto.rec --text x.log >/dev/null 2>&1
    grep '^T ' x.log | cut -d' ' -f3- > strings.txt )   # drop frame numbers
done
md5sum r*/strings.txt | awk '{print $1}' | sort -u | wc -l
```

**Methodological warning, learned the hard way.** This divergence is *flaky* — two runs
frequently agree by chance. Two-sample comparisons produced two confidently wrong diagnoses
during this investigation (first "uninitialized heap, proven by `MALLOC_PERTURB_`", then
"gameplay is actually deterministic"), both of which evaporated at 6–8 samples. Do not conclude
anything here from fewer than 8 runs, and count distinct outcomes rather than comparing a pair.
Also give every run its own directory: IVAN is a portable build that writes `Save/`,
`SndDebug.txt` and `.QuestionHistory_*.txt` into the launch directory, and wizard mode
activation calls `game::Save()`, so runs sharing a directory contaminate each other — that
artifact alone produced a spurious 440-frame outcome cluster.

### 6.6 Game logic reads uninitialized memory — proven, fixed for both corpora (§6.6a–c)

Once §6.5 was fixed a second, weaker source became visible. It does not show up by running the
same command twice; it shows up when you **change what else the process allocates**:

| Condition | 3 runs each | Distinct outcomes |
|---|---|---|
| `--trace` alone vs `--trace --text` | 6 runs total | **2** — one per option set, each self-consistent |
| The same two, under `MALLOC_PERTURB_=42` | 6 runs total | **1** — both option sets converge |

A fixed heap fill collapsing the difference is the proof: the divergent input is the *content* of
uninitialized heap, and `--text` shifts the allocation history that decides what that content is.
Each option set is internally reproducible, which is why 16 concurrent runs of one command line
agree perfectly and this stayed hidden.

**That table no longer reproduces, and it stopped reproducing before §6.6c.** Rebuilt clean at
`4803f04` and re-run — 3 runs per option set, on the non-combat corpus and on the 200-turn
auto-play corpus, with no `MALLOC_PERTURB_` — every one gives **1 distinct trace**. So §6.6a
closed the option-set sensitivity and this row was simply never re-measured. Kept as written
because the *technique* is the durable part: varying the harness options is a cheap proxy for
varying the allocation history, and it found something twice-running a command never would.

`valgrind --tool=memcheck --track-origins=yes` over the 200-turn corpus reports 248 uninitialized
reads. The ones in game logic, with origins:

| Read | Origin |
|---|---|
| `graphicid::operator<` (`igraph.h:65`), and the whole `std::map` tree around it | stack, `object::UpdatePictures` — **fixed** |
| `operator<<(outputfile&, const graphicid&)` (`igraph.cpp:364`) | same — **fixed** |
| `bodypart::UpdateFlags` (`bodypart.cpp:3535`), 18× | heap, `sysbase::Spawn` (`ivandef.h:54`) |
| `bodypart::Be` (`bodypart.cpp:1899`) | same |
| `character::CalculateBurdenState` (`char.cpp:2374`) | same |
| `playerkind::GetNaturalExperience` (`human.cpp:6423`, `:6426`) | same |
| `item::GetBaseDamage` (`item.cpp:48`) — via `DataBase->DamageBonus` | heap, `databasecreator<item>::ReadFrom` (`database.cpp:111`) |
| `LimitRef<double>` (`felibdef.h:75`) | heap, `databasecreator<character>::ReadFrom` |

Re-run 2026-08-15 on the **non-combat** corpus (22,632 errors, 57 contexts). Different corpus,
so read it as a complement rather than a contradiction — but three things changed:

| Read site | Origin | Note |
|---|---|---|
| `bodypart::UpdateFlags`, 12× | heap, `humanoid::MakeBodyPart` / `playerkind::MakeBodyPart` | more precise than "`sysbase::Spawn`" |
| `arm` / `leg` / `head::SignalPossibleUsabilityChange`, 32× | same | **not previously listed** |
| `playerkind::GetNaturalExperience`, 8× | heap, `game::Init` | origin is `game::Init`, not `Spawn` |
| `character::CalculateBurdenState` | heap, `sysbase<guard, humanoid, ...>::Spawn` | as documented |
| `object::UpdatePictures`, 2× | heap, `game::Init` | the *stack* instance was fixed; this heap one is live |
| **`write` / `writev` (the save file itself)** | heap, `sysbase<zombie…>::Spawn`, `sysbase<hedgehog…>::Spawn` | **this is §6.4a** |

The `write`/`writev` rows are the important addition: they are the same defect reaching a *file*
rather than a branch, which is what makes §6.4 and §6.6 one bug and not two. `item::GetBaseDamage`
and `LimitRef<double>` did not appear in this corpus — it never throws an item.

`MakeBodyPart` rather than `Spawn` as the origin is worth acting on: it says the uninitialized
members are on the **bodypart** objects a character builds, not only on the character.

**Fixed: `graphicid`.** `graphicid() = default` left the struct uninitialized while `operator<`
memcmps *every byte* of it (it is the `std::map<graphicid, tile>` key) and the serializer writes
every byte with a raw `Write`. So padding was a live input to both the graphics cache ordering and
the save content. Now zeroed in the constructor. Identical shape to the `fastscriptmember` bug in
§6.1 — worth grepping for `= default` on any struct that gets memcmp'd or raw-written.

**And there was padding to hit, because of a build bug — now fixed, see §7.7.** `NO_ALIGNMENT`
expanded to `__attribute__((packed))` only when `GCC` was defined, and `add_definitions(-DGCC)`
sat in `FeLib/CMakeLists.txt:18` — a *sibling* directory to `Main/`, so it never reached it:

```
Main's flags   (no -DGCC):  sizeof(graphicid)=48  alignof=4   <- one tail padding byte
FeLib's flags  (-DGCC):     sizeof(graphicid)=47  alignof=1
```

One struct, two layouts, one binary — an ODR violation, and `igraph.cpp:364` writes
`sizeof(Value)` bytes from a `Main` translation unit. **This is almost certainly the §6.4
level-file divergence**: `graphicid` is serialized once per cached tile per object on a level, and
its padding byte was garbage. Both halves now agree at 48 bytes and the zeroing constructor
defines the padding, so §6.4 is worth re-measuring — it may already be closed.

**Not fixed: the `Spawn` and database families.** `sysbase::Spawn` does `new type` and the
per-class constructors initialize only some members (`item::item()` sets 6 of them). Those need
in-class initializers or fuller constructors, class by class. `database.cpp:111` was changed to
`new database()` so the prototype table is value-initialized — correct in itself, but it made **no
measured difference** to this corpus, so the live carrier is elsewhere in that list.

Note `new type()` is **not** available as a shortcut for the rest of this family. It worked for
`database` because that class has no user-provided default constructor; `bodypart`, `item` and
`character` all do, so value-initialization just calls the constructor and changes nothing. And
`memset` is out — these are polymorphic. It has to be per-member initialization, class by class.

### 6.6a The bodypart family — FIXED (new)

First slice of the `Spawn` family. `bodypart() : Master(0) { }` initialized one member of ten;
nine of the rest are written straight to the level file by `bodypart::Save`. `head`, `arm` and
`leg` are worse — `arm` and `leg` have no default constructor at all, only `rightarm()` etc. which
`Init` their gear slots, so their saved scalars were indeterminate too.

Fixed with in-class initializers on the declarations (`Main/Include/bodypart.h`), which is the
mechanism that reaches every constructor including the ones that do not exist.

| | before | after |
|---|---|---|
| valgrind errors / contexts, non-combat corpus | 22,632 / 57 | **137 / 13** |
| `bodypart::UpdateFlags` + `arm`/`leg`/`head::SignalPossibleUsabilityChange` reads | 44 contexts | **0** |
| player HP under `MALLOC_PERTURB_` 255 / 42 / 99 | 35/37, 23/36 | **29/37 in all three** |
| uninitialized bytes in the level file | 1,860 | 1,787 |

**The HP row is the result that matters.** Character stats used to depend on which garbage the
allocator returned; they no longer do. That is the §6.6 defect proper — game logic reading
uninitialized memory — and it is closed for this family.

**The save-leak row is the one that did not move**, and it corrects an expectation recorded here
earlier: bodypart was not the carrier of §6.4a. 73 bytes of 1,860. Valgrind's remaining
`write`/`writev` contexts still name `sysbase<zombie…>::Spawn` and `sysbase<hedgehog…>::Spawn`,
so the bytes come from `character`'s own members, not from its bodyparts. §7.6b is that work.

**Why zero, per field** — the argument is not uniform and it is worth not pretending it is:

- `HP`/`MaxHP` — provable. `CalculateMaxHP` opens with `HPDelta = MaxHP - HP`, the damage already
  taken, which is zero for a fresh part. Zeroed, the first call yields `HP == MaxHP`, and
  `RestoreHP()`/`FastRestoreHP()` are literally that assignment. Zero reproduces an invariant the
  code states elsewhere in its own words.
- `BloodMaterial`, `NormalMaterial`, `BodyPartVolume` — unobservable. `character::CreateBodyPart`
  assigns all of them unconditionally (`char.cpp:5845`) before anything reads or saves them.
- The six picture fields — **zero is only a defined placeholder**, not a correct sprite.
  `UpdateBodyPartPicture` assigns them and `NO_PIC_UPDATE` skips it (`proto.cpp:661`,
  `script.cpp:412`). A part drawn or saved without a picture update was already wrong; this makes
  it wrong repeatably instead of randomly. Do not read the fix as having corrected that.

### 6.6b `character`'s owned pointers and `BodyParts` — FIXED (new)

`~character()` frees `Action`, `PolymorphBackup`, `SquareUnder`, `BodyPartSlot`,
`OriginalBodyPartID` and `CWeaponSkill`. The default constructor nulled the first three and missed
the last three, so nulling them completes an existing pattern rather than introducing one.

`BodyParts` is the interesting one, because the codebase names a competing default:
`virtual void CalculateBodyParts() { BodyParts = 1; }` (`char.h:809`). **One is wrong here.** The
two answer different questions — the virtual says how many parts the *species* has and runs in
`Initialize` immediately before `BodyPartSlot = new bodypartslot[BodyParts]`, while the
constructor's value has to say how many slots *exist*, and before `Initialize` none do. It matters
because the destructor does `for(c = 0; c < BodyParts; ++c) delete GetBodyPart(c);`, which indexes
`BodyPartSlot`:

| `BodyParts` | `BodyPartSlot` | destroying an un-Initialized character |
|---|---|---|
| garbage | garbage | unbounded loop over a wild pointer — the old behaviour |
| 1 | null | dereferences null |
| **0** | **null** | no-op loop, then a well-defined `delete[]` on null |

**This changes nothing observable today** — the trace is byte-identical before and after, because
`Initialize` always assigns `BodyParts` before anything reads it. It is a guard that makes the
destructor's assumption enforced rather than assumed, which is the same protection `Action` and
`SquareUnder` already had.

### 6.6c `character` and `playerkind`'s saved and read members — FIXED (new)

The rest of §7.6b. Same mechanism as §6.6a — in-class initializers on the declarations in
`Main/Include/char.h` and `Main/Include/human.h` — and it closes §6.4a outright.

| | before | after |
|---|---|---|
| valgrind errors / contexts, non-combat corpus | 137 / 13 | **0 / 0** |
| uninitialized bytes in the non-combat level file | 1,860 | **0** |
| uninitialized bytes in the auto-play descended-from level | 1,860 | **0** |
| uninitialized bytes in the auto-play fought-over level, ASLR off | 1,616 | **400** — a different family, closed by §6.6d |
| clean-build warnings | 133 | 129 |
| trace and full string stream, both corpora | — | **byte-identical to before the change** |

**Nothing observable changed, and that is the expected result** rather than a disappointment.
The trace, the frame count, the cumulative RNG count and the whole `--text` stream match the
previous commit byte for byte on both corpora, and 8 isolated runs of the auto-play corpus give
one distinct outcome. What went away is undefined behaviour and 1,860 bytes of heap per level
file, not a gameplay difference.

**Four defects, and only one of them was on §7.6b's list.** The list was derived by reading the
constructor; these were found by measuring.

- **`TemporaryStateCounter[STATES]` — the entire save leak**, and not named in §7.6b at all.
  `Initialize` assigns a counter only where the matching state bit is set; `character::Save`
  writes all 32 regardless. Attribution and the layout evidence are in §6.4a. Zero is
  unobservable: every in-memory read is behind a check on the state's own bit, so the serializer
  was the only consumer that did not check.
- **`CarriedWeight`** — `CalculateAll` runs `CalculateAttributeBonuses` before
  `CalculateVolumeAndWeight`, and `leg::CalculateAttributeBonuses` reaches `CalculateBurdenState`,
  so the first burden calculation of every character's life reads the weight before the function
  that sets it has run. This was the one `character::` context valgrind still reported.
- **`AttributeBonus[]` and `CarryingBonus`** — `Initialize` calls `UpdatePictures()` before
  `CalculateAll()`, and `GetAttribute` returns `BaseExperience` plus `AttributeBonus`. So the
  lookups that choose the player's head and arm sprites by attribute read it first. This one
  reached pixels.
- **`playerkind::Talent` and `Weakness`** — 8 of the 13 contexts, the largest group.
  `Initialize` runs `CreateBodyParts` before `PostConstruct`, and `CreateBodyParts` reaches
  `GetNaturalExperience` via `InitSpecialAttributes`, which compares both against
  `TalentOfAttribute` to decide whether to scale a limb's natural experience. Every arm and leg
  the player was built with had its attributes settled by indeterminate memory.

**All four are ordering bugs, and initializing the member does not fix the ordering.** It makes
the early read return a defined value; whether that read should be happening at all is a separate
question, left open deliberately. `CarriedWeight` and `AttributeBonus` self-correct, because
`CalculateAll` recomputes both a few lines later and `IsInitializing()` suppresses the side
effect in between. `Talent` and `Weakness` do not self-correct — the limb attributes persist.

**Why zero, per field** — as in §6.6a the argument is not uniform, and two fields are worth
singling out because the obvious answer is wrong:

- **`BurdenState` is not zeroed.** `OVER_LOADED` is 0 and `UNBURDENED` is 3, so zeroing it would
  name the *worst* burden state while looking like a neutral default. It gets `UNBURDENED`, which
  is also what `CalculateBurdenState` computes from the zeroed `CarriedWeight`.
- **`Talent`/`Weakness` zero is provable, not a placeholder.** `ivandef.h` says
  `/* 0 reserved for no talent */` and the four real talents start at 1, so a zeroed pair matches
  neither branch and the limb gets its unscaled natural experience. That is what the ordering
  already produced almost always — indeterminate memory rarely equals one of four small values —
  so this makes it the answer every time rather than most of the time.
- `AttributeBonus[]` is provable: `CalculateAttributeBonuses` opens by setting every entry to 0
  and adding each equipped item's enchantment, so 0 is exactly what it computes for a character
  with nothing equipped.
- `SquaresUnder` gets 0 for the reason `BodyParts` does in §6.6b — the count has to agree with the
  array, and `SquareUnder` is null until `Initialize` allocates it.
- The rest (`ID`, `Stamina`, `MaxStamina`, `MyVomitMaterial`, `CommandFlags`, `BaseExperience[]`,
  `Volume`, `Weight`, `BodyVolume`, `HP`, `MaxHP`, `DodgeValue`) are unobservable: each is
  assigned unconditionally before any read or save, on both the spawn and the load path. They are
  in for the §6.6b reason — a member that reaches a file should not depend on that staying true —
  and measurement confirms none of them was leaking.

**Two corrections to §7.6b's list.** It named `v2HoldPos`, which *both* constructors already
assign (`char.cpp:556` and `:613`); the list was built from the mem-init lists and missed the
constructor bodies. And it missed `TemporaryStateCounter[]` and `BaseExperience[]` entirely — the
first being the whole of the leak it was written to chase.

`MALLOC_PERTURB_` is no longer needed as a workaround on either corpus — §6.6d closed the fluid
family. It remains useful as a *measurement* technique, which is what it was always better at.

### 6.6d The fluid family, and the pointers that were not pointers — FIXED (new)

All of §7.6c, and §7.6a fell out with it. Same mechanism again — in-class initializers on the
declarations, this time in `Main/Include/fluid.h` and `Main/Include/trap.h`.

The corpus **is** committed now, as `tools/corpora/autoplay-200.rec` (§5b) — it was not when
this section was written, and regenerating it from prose was the gap that closed. It is 210 keys
and lands at turn 202 on UT lvl 1 with HP 29/37; that HP is the §6.6a check value, so a corpus
reaching it is the same one. The measurement is §6.4a's, with `setarch -R` added per §6.4b.

| auto-play corpus, 210 keys, seed 999 | before | after |
|---|---|---|
| level-file bytes leaked, fill 42 vs 99, ASLR off | 400 in 59 regions | **0** |
| valgrind errors / contexts | 283 / 3 | **0 / 0** |
| distinct level files, 8 isolated ordinary runs, ASLR **on** | 8 | **1** |
| distinct level files, 4 runs at a fixed fill, ASLR **on** (§6.4b's test) | 4 | **1** |
| trace, string stream, frame count (679), RNG count (1,670,383) | — | **byte-identical** |

**The region histogram is what picked the fields**, exactly as the offset-layout check did in
§6.4a. The 400 bytes were 26 regions of 4, 29 of 8 and 4 of 16 — and 26×4 + 29×8 + 4×16 = 400
with nothing left over. Four bytes is `imagedata::SpecialFlags`, an `int`. Eight and sixteen are
`ulong`s, which `imagedata` has none of. That is what said the section had the wrong family before
any of it was changed.

**`trapdata` was 296 of the 400, and §7.6c did not name it.** `trapdata() : Next(0) { }`
(`trap.h:34`) initializes one member of four, and neither `fluid` constructor finishes the job:
the ground one (`fluid.cpp:26`) assigns `TrapID` and `VictimID` and leaves `BodyParts`; the item
one (`fluid.cpp:38`) assigns only `TrapID` and leaves `VictimID` and `BodyParts` adjacent.
`fluid::Save` opens with `SaveFile << TrapData` and `operator<<` writes all three unconditionally
(`trap.cpp:50`) — so a ground fluid leaks 8 bytes and an item fluid 16, which is the histogram.

**§7.6a was the same bug seen through ASLR, not a separate one.** Its signature — 8-byte fields
that survive `MALLOC_PERTURB_` and collapse under `setarch -R` — is what an uninitialized `ulong`
looks like when the freed chunk it lands in happens to hold a stale heap pointer: the fill only
reaches bytes the allocator actually filled, and the stale pointer moves with the heap base. The
section's own diagnosis said exactly this ("a stale pointer, copied into a heap buffer that is
then written, rather than a pointer someone meant to save") and then filed it as separate work.
It is closed by this commit on this corpus, and `lsquare::Save` never needed touching.

**Two corrections to §7.6c's list**, both found the same way §6.6c's were — by measuring:

- **`DripPos` is not uninitialized.** `v2::v2()` is `: X(0), Y(0)` (`v2.h:41`, a fork change
  carrying a `//was: v2() = default;`), so default-constructing the member zeroes it. Same shape
  as §7.6b's `v2HoldPos`: read off the mem-init list without checking the member's own type.
- **`SpecialFlags` reaches less than the section claimed.** It said which validity map a pool of
  blood picks is decided by uninitialized memory. `AddLiquidToPicture` computes `ValidityMap` at
  `fluid.cpp:543` unconditionally but only dereferences it inside `if(Shadow)` (`:551`) and behind
  `!Shadow ||` (`:592`), and the blood path passes `Shadow == 0` (`:88`). Both valgrind contexts
  are `Use of uninitialised value of size 8` **in `GetBodyBitmapValidityMap` itself** — the
  pointer load, not a pixel decision. The map is computed from garbage and then not read.

**What §7.6c missed in the other direction:** `fluid::IsStuckTo` (`fluid.cpp:778`) compares
`TrapData.VictimID` directly, and for fluids on items and bodyparts nothing ever assigns it —
only `StepOnEffect` does (`:753`), and that is the ground path. `bodypart.cpp:3250` and `:3291`
turn that comparison into `IGNORE_TRAPS` on a death check. Garbage rarely equals a live character
ID, so it read false almost always, which is why nothing visible broke — the same shape as
§6.6c's `Talent`/`Weakness`.

**Why zero, per field** — the §6.6a test, and here every one of them is provable rather than a
placeholder, which is the first time that has been true in this series:

- **`SpecialFlags` is `ST_NORMAL`, and `ST_NORMAL` is 0** (`ivandef.h:265`). `fluid::Draw:195`
  already computes `MotherItem ? MotherItem->GetSpecialFlags() : 0` for this same object, and
  `DrawBodyArmorPicture:452` passes 0. `BodyBitmapValidityMap[0]` is the one
  `CreateBodyBitmapValidityMaps` fills with `memset(…, 0xFF, …)` (`igraph.cpp:444`) — every pixel
  valid, no bodypart restriction, which is what a pool with no bodypart shape needs. §7.6c asked
  for care here on the grounds that zero is a real value; it is, and it is the right one.
- **`AlphaAverage`** — 0 is what `bitmap::Fade` itself computes for a picture with no
  non-transparent pixels (`bitmap.cpp:1093`), and it pairs with the constructor's `AlphaSum(0)`.
  Note `Fade()` is *not* a read of it: `bitmap::Fade` takes it by non-const reference and writes
  it. The reachable read is `fluid.cpp:82`/`:84`, when `AddLiquidToPicture` returns early at
  `:555` without having assigned it.
- **`DripColor`/`DripAlpha`** — unobservable. `DripTimer(0)` forces `Animate`'s assignment branch
  (`fluid.cpp:468`) before any read at `:490`, on every path. In for the §6.6b reason only.
- **`TrapData`'s three** — `TrapID = 0` is what the item constructor already assigns and what
  `PreProcessForBone` uses for "no trap" (`fluid.cpp:768`); `VictimID = 0` is what the ground
  constructor assigns and what `UnStick()` and `Destroy()` reset it to (`fluid.h:63`,
  `fluid.cpp:671`); `BodyParts = 0` is "stuck to nothing", the state `UnStick(int)` walks toward.

**Nothing observable changed**, again — trace, string stream, frame count and RNG count all
byte-identical to the previous commit, and 8 isolated runs give one distinct outcome. What went
away is undefined behaviour and the last of the per-level heap residue.

**The row that matters this time is the ordinary-run one.** Before, eight ordinary replays — ASLR
on, no `MALLOC_PERTURB_`, nothing special — produced eight different level files. Now they produce
one. Every previous section in this series needed a fixed heap fill or ASLR disabled before the
level file would reproduce at all; this is the first measurement taken with neither.

### 6.6e Two more fields that reach a level file — FIXED (new)

Same family as §6.6a-d, found the same way, and found because §9.4 changed what the corpora
generate: a different dungeon reaches different classes. Two ordinary replays started producing
four distinct level files again out of four runs, with the trace, the text log and the
screenshot still byte-identical — the §6.4 shape exactly.

| noncombat corpus | before | after |
|---|---|---|
| level-file bytes leaked, fill 42 vs 99 | 4 in 1 region | **0** |
| distinct level files, 4 ordinary runs | 4 | **1** |
| valgrind errors / contexts | 1 / 1 | **0 / 0** |

- **`itemtrapbase::Team`** (`trap.h`). `itemtrapbase() : Active(false) { }` initialises one
  member of three and `Save` writes `Team` unconditionally, so any level holding an unarmed item
  trap — a mine, in this corpus — carried four bytes of heap. Now `NO_TEAM`, which is what
  `TeleportRandomly` resets it to and what an unowned trap means. Nothing reads it while
  `Active` is false: `CanBeSeenBy` short-circuits on `!Active`.
- **`earth::PictureIndex`** (`lterras.h`). `PostConstruct` picks one of four earth tiles with
  `RAND() & 3`, but it does not run on every path that creates an `earth`, and `Save` writes the
  field on all of them. Now 0, which is one of the four values `PostConstruct` itself chooses.

**The technique that found it is worth more than the fix, and it is new here.** §6.4a warns that
valgrind's `write`/`writev` frames name whichever call flushed the 8KB `filebuf`, not the call
that put the bad bytes in it — and that warning cost real time again: memcheck confidently named
`earth::Save`, the earth field was zeroed, and the leak did not move. Making the save stream
**unbuffered** for one diagnostic run collapses the two:

```cpp
File.rdbuf()->pubsetbuf(0, 0);   // before open(), in outputfile::outputfile
```

Every `<<` then becomes its own syscall, so memcheck's stack names the actual writer.
`itemtrapbase::Save` appeared immediately. Do this first next time; it turns §6.4a's "the read
half of the report is not sound for buffered output" from a caveat into a non-issue.

### 6.7 The host libm is no longer an input — FIXED (new)

`sin`, `cos`, `atan`, `log10` and `pow` are not correctly rounded by any standard, so every
libm is entitled to its own answer in the last ulp, and IVAN feeds those answers through
`int()` and `(short)` truncation. Game code now calls `portmath::` — musl vendored at a
pinned commit, used by native and WASM alike — so both platforms run the same algorithm.
Full notes in `portmath/README.md`; the parts worth having here are the two corrections and
the one number.

**The symbol is `sincos`, not `sin`/`cos`.** GCC rewrites `sin(x)` and `cos(x)` on the same
argument into a single `sincos(x)`, and glibc's `sincos` is not obliged to agree with its own
`sin` and `cos` — so whether two builds match can depend on whether the compiler chose to
fuse, with no visible call site either way. The first inventory taken here interposed `sin`
and `cos` and reported **zero calls** from a binary making 18,368 of them. Anything that
greps for `sin(` will miss this.

**World generation is where the two libms agree.** §6.3 named `worldmap.cpp:757-760` as the
exposure. Replaying every recorded call through musl and comparing bit-for-bit against glibc:

| site | function | calls | differ |
|---|---|---|---|
| `bitmap::DrawPolygon` | `sincos` | 3,600 | 216 |
| `worldmap::Generate` (Poisson sampler) | `sincos` | 3,400 | 132 |
| `femath::NormalDistributedRand` | `sincos` | 1,764 | 106 |
| `character::GetAdjustedStaminaCost` | `log10` | 73 | 18 |
| `character::CheckForBlockWithArm` | `log10` | 1 | 1 |
| `worldmap::PeriodicSimplexNoiseAltitude` | `sincos` | 9,604 | **0** |
| every `log` site | `log` | 8,967 | **0** |

473 of 27,496, all by exactly one ulp. World gen's arguments are `x/XSize * 2π` for integer
`x` and both implementations round those identically; the disagreements are elsewhere,
including 106 in the Box-Muller transform that decides which way monsters wander.

**Nothing observable changed, and that is luck rather than structure.** Traces, text logs,
screenshots, `.wm` and level files are byte-identical to the previous binary — checked
against saves kept from it, not only against the goldens. So 473 calls in live game logic
returned a different number and no state moved, because none of them truncated across a
boundary in these runs. That holds for this seed and these two corpora and nothing more,
which is the argument *for* pinning: without it the first symptom would be an unexplained
frame-hash divergence in a WASM build, thousands of frames from the cause.

**Re-measured across compilers, 2026-08-15, and it holds.** Everything in this section compared
musl against glibc with one compiler. What a WASM port actually needs is the *same* musl source
compiled by two different compilers agreeing, which is a different claim and was never tested.
It is now: a probe linking `portmath/src/*.c` with `pm_log`, `pm_sincos`, `pm_log10`, `pm_atan`
and `pm_pow` over 2,000 arguments each — the shapes `NormalDistributedRand` and world gen
actually produce — gives **bit-identical output** from `gcc -O2` and from `emcc -O2`, 14,000
evaluations, same md5. The `-ffp-contract=off` and `-fno-builtin` on the portmath target are
what earn that, and `__FP_FAST_FMA` is undefined on both targets so the `log`/`pow` fast paths
agree as well. This mattered: when §9.4's hunt reached a float-heavy AI function, being able to
*eliminate* the math in one cheap measurement was worth more than another guess.

**The instrument was `LD_PRELOAD`, and it is worth reusing.** Interposing libm and recording
`__builtin_return_address(0)` gives every call with its caller, which is how the site table
above was built — a source grep would have found the wrong sites and missed `sincos`
entirely. `dladdr` turns the return address into a module offset that `addr2line` resolves.

### 6.8 `time_t` would not have compiled under Emscripten — FIXED (new)

`game.cpp:3594` does `SaveFile >> TimePlayedBeforeLastLoad` on a `time_t`. It compiles today
only because `time_t` *is* `long` on x86-64, so it binds that overload by accident. Under
Emscripten, where musl uses a 64-bit `time_t` on 32-bit targets, `time_t` is `long long`,
nothing binds, and the build fails — confirmed by compiling the real header against a
`long long` rather than by reading it. Three sites: `game.cpp:3496`, `:3594`, and
`hscore.h`'s `std::vector<time_t>`, whose element operator the container serializer
instantiates.

Fixed by fixing the width, because `time_t` was the symptom: `long` is 8 bytes here and 4
under Emscripten, and **every container writes its length as a `ulong`**, so at native width
a WASM save diverges at the first container and never resynchronises. Both now go out as 8
explicit little-endian bytes, the idiom `short`/`ushort` already use. `long long` gets its
own overload — C++ keeps it distinct from `long` even at equal width — and that is what
makes `time_t` bind on both targets.

**The save format did not change and `SAVE_FILE_VERSION` did not move.** On a little-endian
host of the same width, explicit bytes are what the raw write already produced. Verified:
both corpora give `.wm`, level and `.sav` files identical to saves kept from before the
change, the single exception being one byte of autoplay's `.sav` at offset 172799 — the
`GetTimeSpent` second-boundary flake of §5.

This replaced the `SAVE_COMPATIBILITY` block, which was written for this same problem on
mingw and was dead code: the macro is defined nowhere, so its `#if` was always false.

## 7. Open items, roughly in priority order

**§6.6 was what blocked seam 1** — a WASM build shares no allocation pattern with a native one, so
every uninitialized read becomes a phantom diff that owes nothing to Emscripten. §6.5, §7.7, §6.6a
and §7.6b are done, and **both corpora now report no uninitialized read at all**.

The `Spawn`/`MakeBodyPart` family took three passes and is closed: bodyparts (§6.6a, 22,632
contexts → 137), `character`'s pointers (§6.6b), and `character`/`playerkind`'s saved and read
scalars (§6.6c, 137 → 0, and the 1,860-byte level-file leak → 0). §6.4a is closed with it.

**The pre-port gates are now closed too.** The corpora are committed artifacts rather than
instructions (§5b), game math runs on a vendored libm so native and WASM compute the same
numbers (§6.7), and the `time_t` overload that would have failed the Emscripten build outright
is fixed along with the container-length width behind it (§6.8). **The Emscripten build itself
now compiles and links** (§9.3), so the port is past the build-system stage entirely.

**The native-vs-WASM frame comparison exists and runs** — `tools/corpora/compare-targets.sh`,
on top of the `--headless` path that closed §9.3's DOM blocker. It passes outright on the
non-combat corpus and gets 389 of 593 frames into the auto-play one. **The single most valuable
thing to do next is to finish it**: §9.4 names the remaining divergence, what has been ruled out,
and the four-step technique that localised the nine already fixed. Everything else in this
section is smaller. CI (§7.3) should wire in `compare-targets.sh` alongside `verify-corpora.sh`
once it passes on both corpora; the format warnings (§7.8) and §9's step 1 (native scaling) are
independent of all of it.

The fluid family took one pass (§6.6d, 283 → 0, and 400 bytes → 0) and closed §7.6a with it —
those were stale heap pointers sitting in uninitialized `trapdata` fields, not pointers anyone
meant to save. **For the paths these two corpora reach, §6.4 and §6.6 are now closed entirely**:
eight ordinary replays produce one level file, with no fixed heap fill and no ASLR trick. The
qualifier that survives is coverage — neither corpus visits the whole world map, and §7.9's
format work has not started.

§7.7 also cleared the way for §7.9, the save-format work: both are deliberate format breaks, and
`SAVE_FILE_VERSION` has now moved once already, so the second break is cheaper than the first.

### 7.1 ~~Get savediff tested against real saves~~ — DONE

Resolved by the screen capture layer. The diagnosis in the old version of this section was
wrong in an instructive way, so it is worth recording what was actually happening:

- **8× ENTER was four too many.** Four ENTERs *is* character creation. The 5th onwards land on
  the world map where ENTER is not a command, and the game answers "Unknown key. Press '?' for
  a list of commands." — which is why `rng` stayed pinned. The game was not stuck; it was
  in-game and rejecting the input. One screenshot said so immediately.
- **The character starts on a tiny island** and must find an underwater tunnel. The intro text
  says so explicitly; it had simply never been read. At seed 999 the cave mouth is one tile
  **west** of the start, plainly visible as a dark arch once the screenshot is cropped and
  upscaled. `enter*4 down left >` reaches UNDER_WATER_TUNNEL level 1 at turn 3.
- **`S` was never needed.** Entering a level **autosaves**, so a full save set (`.sav`, `.wm`,
  `.<D><L>`) appears without any explicit save command. The earlier "S produced no save file"
  conclusion was drawn from a run that never got in-game in the first place.

Result of the comparison: `.sav` and `.wm` SAME, level file DIFF — see §6.4.

Lesson worth keeping: every hour spent on that section was spent guessing at a screen nobody
could see. Capture came far cheaper than the guessing did.

### 7.2 ~~Validate the auto-play AI as a test corpus~~ — WORKS, and now reproduces

**The AI runs.** `python3 tools/play/play.py auto 200`. The guess in the old version of this
section was right — wizard mode does need runtime activation — but the missing piece was which
key:

- **`` ` `` (backtick, 0x60) activates wizard mode**, not `~`. It asks "Do you want to cheat,
  cheater? This action cannot be undone. [y/N]" and needs the `y`. `command.cpp:141`.
- `~` (0x7E) is `AutoPlay`, and it is `WizardModeFunction`-gated, so it is simply ignored until
  the above has happened. That is why sending `126` did nothing.
- Then **the AI acts once per `.`**. It is not self-driving, and the reason is a bug:
  `game::AutoPlayModeApply` computes `iTimeout` for modes 2-4 and then **discards it** —
  `globalwindowhandler::SetKeyTimeout` has **no callers anywhere in the tree**. The whole
  timeout-driven auto-play path is dead code.

That dead code is a gift here. Auto-play advancing on input rather than on the wall clock is
exactly what a deterministic corpus generator needs; had the timeout worked, the turn count
would have depended on how fast the machine was.

Use **mode 1** (one `~`). At mode >= 2 any key that is not `.` or `~` disables auto-play again
(`char.cpp:3819`), and the ESC force-stop reads `IsKeyPressed`, which the harness pins to false
during replay.

Measured, seed 999, from `enter*4 down left >`:

| Turns | Wall clock | Reached |
|---|---|---|
| 200 | 12.8s including world gen | UT lvl 1, fighting, ate a corpse, equipped a ring of polymorph |
| 3000 | ~4 min | UT lvl 2, HP max 37→45, Dex and Agi raised, whip skill 3, Hungry and Burdened, **died 20 times** |

The deaths are the useful part: `char.cpp:1753` forces `bInstaResurrect` for an auto-playing
player, so the AI resurrects and keeps going. It never needs restarting. It also self-recovers
when stuck — after 10 idle turns it resets its navigation, and after 50 it spawns and reads a
**scroll of earthquake** to dig itself out (`char.cpp:3623`, commented `UsingTerribleEarthquake
Solution` — its author's assessment, not mine).

So as a *volume* generator it is everything §7.2 hoped for: dungeon generation, combat, item
use, equipment, skills, hunger, status effects, death.

**And it is now a usable differential corpus.** Replaying the same auto-play recording used to
give 5 distinct outcomes in 8 runs; that was §6.5, which this exercise is what uncovered, and it
is fixed. A 2,800-turn recording — two dungeon levels, 36 deaths — now replays identically across
16 concurrent runs on a saturated machine. The remaining caveat is §6.6: hold the harness options
constant, because changing them changes the allocation pattern and the run with it.

### 7.3 Wire it into CI

The five existing workflows in `.github/workflows/` **only build; none run the binary.**
That is why the MIDI abort was never caught. Add a job that:
- builds with `-DPORTABLE_BUILD=ON`
- replays a committed recording under `SDL_VIDEODRIVER=dummy`
- diffs the trace against a committed golden trace

Commit golden traces (they're just hashes, tiny). Upload PNGs as artifacts on failure only.

The five workflows are inherited from upstream and are due to be replaced wholesale rather than
extended, so treat the notes here and in §7.8 as input to that rewrite, not as constraints on
it. The sequencing that matters is the other way round: get the WASM run working locally first
(§9.3), then automate what is already known to pass.

### 7.4 Add PNG dump on mismatch — half done

The writer now exists (`--shot`, `--shot-dir`, and `harness::WriteShot` for programmatic use),
and it is a real PNG rather than the mislabelled BMP `bitmap::Save` produces. What is still
missing is the *trigger*: comparing a trace against a golden one during the run and dumping
both frames plus a diff image at the first mismatch, with ~30 frames of preceding context.

Straightforward now — the pieces are `WriteShot` plus a golden trace reader. Do this together
with §7.3 so CI failures arrive with pictures attached.

### 7.5 Add the state-digest layer

Per §5, savediff cannot validate the WASM port. A periodic structured digest (player stats,
position, inventory IDs, level layout hash, RNG count) crossing the C API is what will
validate both the port *and* the eventual TS rewrite. This is more urgent than originally
scoped.

### 7.6 Fix the `.wm` uninitialized-heap residue (§6.2)

Note this is the same family as §6.6, and `area::Save`'s `FlagMap` is *not* an instance of it —
`area::area()` memsets it. Re-derive that inventory rather than trusting the 358-byte figure.

### 7.6a ~~Remove the raw pointers from level saves (§6.4b)~~ — DONE, see §6.6d

Not a separate defect. The signature this section named — 8-byte fields that survive
`MALLOC_PERTURB_` and collapse under `setarch -R` — is what an uninitialized `ulong` looks like
when it lands in a freed chunk holding a stale heap pointer, and the section's own reading of it
was right. What it got wrong was filing it as independent of §6.6: initializing `trapdata` closed
it, `lsquare::Save` was never involved, and the choice it posed (index, role tag, or not saved)
did not need making. On the auto-play corpus, 4 runs at a fixed fill with ASLR **on** went from
4 distinct level files to 1.

Do not read it as closed everywhere. §6.4b already established the count is a property of what is
on the level, and neither corpus visits the whole map.

### 7.6b ~~Initialize `character`'s saved members (§6.4a)~~ — DONE, see §6.6c

Closed the 1,860-byte level-file leak and took valgrind on the non-combat corpus from 137 errors
in 13 contexts to zero. Two things this section got wrong are recorded in §6.6c: it named
`v2HoldPos`, which both constructors already assign, and it missed `TemporaryStateCounter[]`,
which turned out to be the entire leak. The list was read off the constructors; the answer came
from grouping the differing save offsets into contiguous runs and looking at the shape.

### 7.6c ~~Initialize `fluid::imagedata`~~ — DONE, see §6.6d

Took the auto-play corpus from 283 errors in 3 contexts to zero and its 400-byte level-file leak
to zero, and closed §7.6a on the way. Three things this section got wrong are recorded in §6.6d:
it named `DripPos`, which `v2`'s own constructor zeroes; it overstated `SpecialFlags` as deciding
which validity map a pool of blood picks, when the map is computed and then not read on that
path; and it missed `trapdata` entirely — 296 of the 400 bytes, and a live read in
`fluid::IsStuckTo` that decides `IGNORE_TRAPS` on a death.

The one instruction it got right is the one that found the answer: *re-derive the leak rather
than assuming*. The region histogram (26×4 + 29×8 + 4×16 = 400) named `ulong` fields that
`imagedata` does not have, before any code was changed.

### 7.7 ~~Decide what to do about `-DGCC`~~ — DONE, unpacked

Resolved by testing `__GNUC__` in the header, which is the TODO that was already sitting on
`FeLib/CMakeLists.txt:18`, and by then *removing* the packing from the two game structs rather
than propagating it.

**The bug was the disagreement, not the layout.** `graphicid` was 47 bytes in FeLib and 48 in
Main, so either packing everywhere or unpacking everywhere would have fixed it. That made the
layout a free choice, decided on other evidence:

| | packed | unpacked |
|---|---|---|
| new warnings from the layout change | 301 | **0** |
| `-Waddress-of-packed-member` | 1 | 0 |
| `sizeof(graphicid)` | 47 | 48 |

Packing removes the compiler's alignment guarantee for *every* member. `igraph.cpp:247` hands
`GI.Color` — a `ushort[4]` member — to `Colorize()`, where it decays to a `ushort*` carrying an
alignment promise a packed struct does not make. That is UB: free on x86, a fault or a slow path
on ARM, and variable across WASM engines, which is the destination. One byte per cached tile is
not worth it.

**Why they were packed originally, which is not what it looks like.** The removed comment above
the pragma said it outright: `/* memcmp doesn't like alignment of structure members */`. It was
never a size optimisation. `operator<` is a `memcmp` over the whole object, so padding bytes are
compared; padding is indeterminate, so two objects with identical members can compare unequal,
which breaks the strict weak ordering `std::map` requires. `git log -S NO_ALIGNMENT` bottoms out
at `3e50767 "…modified by nukes to support 64-bit systems"` — it arrived during a 32→64-bit port,
exactly when padding layouts shift.

**So packing and the zeroing constructors solve the same problem** — making every byte defined —
and only the constructors keep the alignment guarantee. Unpacking is safe *because* `graphicid`
got its `memset` in `bfec4d1` and `configid` got a zero-init constructor at the same time as this
change. Removing either one silently reintroduces the original bug. Do not delete that `memset`
on the grounds that every member is assigned: **no member owns the padding.**

Two other things this turned up:

- **`configid`'s packing was always a no-op.** Two `int`s have no padding on any platform; it was
  applied by pattern-match from `graphicid` during that same 64-bit port and never did anything.
- **`#pragma pack(1)` under `#ifdef VC`** wrapped both structs. `VC` is defined nowhere (MSVC sets
  `_MSC_VER`), so it was dead — but it is the identical hand-maintained-flag pattern, and had
  anyone defined it, `graphicid` would have been 47 on MSVC against 48 everywhere else. Removed.

`NO_ALIGNMENT` is gone. `HARDWARE_LAYOUT` replaces it and is applied only to `graphics.h`'s
`vesainfo`/`modeinfo`, which are VESA BIOS blocks filled by a real-mode interrupt at spec-defined
offsets — there packing is mandatory, because the layout is defined outside this program. Naming
the macro for that one real use is what stopped "unpack it" from looking like it would break the
DOS build.

**Cost:** `SAVE_FILE_VERSION` 136→137 and `BONE_FILE_VERSION` 120→121 (bone files carry a whole
`level`, so they contain `graphicid` too). And the warning baseline moves 84→133, because
`LIKE_PRINTF` was gated on the same broken macro and printf format checking has now switched on
in `Main` for the first time. All 49 new warnings are pre-existing format bugs, listed in §7.8.
There is no way to fix the packing via `__GNUC__` and keep them hidden: doing so needs a
directory-scoped flag, which is the mechanism that caused this bug.

### 7.8 Fix the format bugs `LIKE_PRINTF` just exposed (new)

49 warnings, all in `Main`, all pre-existing and invisible until §7.7 turned format checking on:
25 `-Wformat=`, 20 `-Wformat-security`, 4 `-Wformat-extra-args`. Confirmed real:

- `gods.cpp:316` — `ADD_MESSAGE("You feel the music resonate within you.", GetName())`: a format
  with no conversion specifier and an argument passed anyway.
- `human.cpp:1055` and `:1079` — two specifiers, three arguments; the god's pronoun is dropped.
- The 20 `-Wformat-security` are the `ADD_MESSAGE(SomeString)` pattern, where the format string is
  not a literal. That is a live crash risk rather than a style nit, because the player types their
  own character name and it reaches messages: a name containing `%s` is read as a conversion.

`FeLib` has been format-checked all along and is clean, so the whole list is `Main`.

**Worth knowing when the workflows get rewritten:** `-Wno-format-security`, which the five
existing workflows pass alongside `-Werror`, covers only 20 of the 49 — the 25 `-Wformat=` and 4
`-Wformat-extra-args` are not suppressed by it. Measured with the workflows' own flags rather
than inferred. Two consequences for whatever replaces them: the suppression list needs widening
if the format bugs are not fixed first, and blanket `-Werror` is fragile against a compiler
bump — at GCC 13.3 the 80 pre-existing `-Wstringop-overflow=` become errors too and `FeLib`
fails before `Main` is even reached.

### 7.9 Make the save format host-independent (new, scoped)

§5 says savediff cannot be a native-vs-WASM oracle. That is true of the *current format* and it is
a format problem, not a tool problem — savediff reads faithfully, the two platforms simply write
different files. The scope turns out to be small, because **half the format is already portable**.

**Tier 1, already portable and endian-independent.** `truth`/`char`/`uchar` go out via `Put()` as
one byte. `short`/`ushort` are written explicitly little-endian, byte by byte (`save.h:218`).
`festring` and `cchar*` are a `ushort` length plus raw bytes. Entity type IDs and flags are cast
to `ushort`. None of this cares what host wrote it.

**Tier 2, raw object representation** — `RAW_SAVE_LOAD` is `Write(&Value, sizeof(Value))`:

| Written as | x86-64 | wasm32 | Verdict |
|---|---|---|---|
| `int`, `uint` | 4 | 4 | portable |
| `double` | 8 | 8 | portable, IEEE-754, both little-endian |
| `v2`, `rect`, `packv2` | 8/16/4 | same | portable, `int`/`short` members |
| `expid`, `configid` | 8 | 8 | portable, two `int`s each |
| **`long`, `ulong`** | **8** | **4** | **the only broken primitive** |

**But that one break is pervasive, because every container writes its size as `ulong`** —
`save.h:299` for `vector` and the same for `deque`, `list`, `map`, `set`. Every container in every
save carries it in its length prefix, which is why the divergence would start early and never
resynchronise. The flip side is that fixing `long`/`ulong` fixes the containers for free.

**The work:**

1. ~~**Make `long`/`ulong` fixed width.**~~ **DONE — §6.8.** Both write 8 explicit
   little-endian bytes, following the file's own `short`/`ushort` idiom. The
   `SAVE_COMPATIBILITY` path this section pointed at was dead code and has been replaced.
   Note the truncation caveat it raised is real and now explicit rather than accidental: on
   wasm32 `long` is 4 bytes, so a saved value above 32 bits narrows on load. Nothing in the
   tree is known to store one, but it has not been audited.
2. **Containers** — the width question is settled by (1); what is still open is whether 8
   bytes per container length is worth it. `uint` would halve it and is equally portable.
3. ~~**`time_t` may not even compile.**~~ **DONE — §6.8.** It was a hard build error, not a
   maybe: confirmed by compiling `save.h` against a `long long`. Three sites, including
   `hscore.h`'s `std::vector<time_t>`, which this section did not name.
4. **Raw struct writes.** `graphicid` (`igraph.cpp:364`) and `configid` (`game.cpp:5393`) are
   written with `sizeof`. §7.7 made their layouts flag-independent, which is necessary but not
   sufficient — they still carry host layout. Converting them to field-by-field, the way
   `dangerid`, `killreason` and `massacreid` already are, removes the question permanently and is
   the real fix.
5. **savediff** decoders move to the new offsets, and `--word-size` becomes a legacy-save flag
   rather than a required guess.

**What makes this affordable:** `SAVE_FILE_VERSION` exists (`game.cpp:76`, written at `:3454`,
checked at `:3519`) and the stamp itself is an `int`, 4 bytes on both platforms — so a WASM build
can read a native save's version and reject it cleanly rather than mis-decoding it. §7.7 has
already spent one version bump, so bundle this into the next one.

**And the harness can validate it**, which was not true before: replay a corpus, save, load,
re-save and compare on one platform to prove the format round-trips, then use native-vs-native
savediff to prove the change did not alter semantics.

---

## 8. Change inventory

Committed on **`master` of the fork `bethmaloney/ivan`**, thirty-four commits on top of upstream
`de528ac`. `origin` still points at `Attnam/ivan` and is untouched; the fork is the remote
named `fork`. **Nothing has been offered upstream.**

| | Commit | Covers |
|---|---|---|
| 1 | `d4bda10` audio: continue without music when MIDI init fails | §6.1 |
| 2 | `bfec4d1` Initialise memory that game logic and saves read | §6.1, §6.6 |
| 3 | `a6f0c4a` Draw every game decision from the seeded generator | §6.1, §6.5 |
| 4 | `ada30b4` Add a record/replay/trace harness | §4, §6.5a |
| 5 | `2b79f7f` Add savediff, a differential reader for saved games | §5 |
| 6 | `9a19bf2` Add play.py, a driver for steering the game | §5a |
| 7 | `21a050b` Add HARNESS.md | this file |
| 8 | `dfb8294` HARNESS.md: record the commit inventory | §8 |
| 9 | `1f23c0a` Give graphicid and configid one layout, and unpack them | §6.6, §7.7 |
| 10 | `52259cc` Strip trailing whitespace from game.cpp | — |
| 11 | `4803f04` Initialise bodypart members that logic and saves read | §6.6a, §6.6b |
| 12 | `3589916` HARNESS.md: re-verify from a clean build, and record what is left | §6.4a, §6.4b, §6.6a, §6.6b |
| 13 | `ebec9e7` Initialise character and playerkind members that logic and saves read | §6.6c, §7.6b |
| 14 | `7d36be0` HARNESS.md: record the character fix and what it uncovered | §6.4a, §6.4b, §6.6, §6.6c, §7.6c |
| 15 | `a1ce777` Initialise fluid and trapdata members that logic and saves read | §6.6d, §7.6a, §7.6c |
| 16 | `22b84c5` HARNESS.md: record the fluid fix and the pointers that were not | §2, §6.4, §6.4b, §6.6d, §7.6a, §7.6c |
| 17 | `270e756` Commit the differential corpora as artifacts, not instructions | §5b |
| 18 | `ab63c72` Route game math through a vendored libm so platforms agree | §6.3, §6.7 |
| 19 | `de6c19b` Serialize long and ulong as explicit little-endian bytes | §6.8, §7.9 |
| 20 | `8b91eb4` HARNESS.md: record the corpora, the libm pin and the time_t fix | §2, §5b, §6.3, §6.7, §6.8, §7.9 |
| 21 | `74980f1` Replace PCRE with std::regex and drop the dependency | §3, §9.2 |
| 22 | `93119af` Make the build CMake 4 clean | §2, §9.2 |
| 23 | `37cdb01` Build for Emscripten with SDL2, SDL2_mixer and libpng from ports | §3, §9.3 |
| 24 | `84ba551` HARNESS.md: record the Emscripten build and the emsdk pin | §2, §3, §6.3, §7.3, §7.8, §8, §9.3 |
| 25 | `54f27ed` Give the game a headless path so it needs no display or audio device | §2, §3, §4, §9.3 |
| 26 | `8db4967` Build for Emscripten with exceptions enabled | §3, §9.3 |
| 27 | `4428a26` Initialise trap and terrain members that level files read | §2, §6.6e |
| 28 | `7ff0d4b` Stop the compiler and the standard library deciding the game | §2, §5, §5b, §9.4 |
| 29 | `b7c138f` HARNESS.md: record the headless path and the first native-vs-WASM comparison | §2, §3, §4, §5, §5b, §6.6e, §6.7, §7, §8, §9.3, §9.4 |
| 30 | `663d197` HARNESS.md: re-measure the warning baseline and correct §9.4's count | §2, §8, §9.4 |
| 31 | `4ec9fe1` Build for the browser as well as for node | §2, §3, §9.3, §9.5 |
| 32 | `f92c257` HARNESS.md: record the browser host | §1, §2, §3, §8, §9.3, §9.5 |
| 33 | `459721d` Make a browser crash collectable and legible | §3, §9.5, §9.6 |
| 34 | HARNESS.md: record the crash-collection path | §2, §8, §9.5, §9.6 |

**Commits 27 and 28 are the ones worth upstream's attention.** 27 is two more of the
uninitialized-member family. 28 is eleven defects that have been in the game for years and that
nobody could have found without compiling it twice: a font blit whose width was
`20 / sizeof(ulong)`, a dozen expressions that let the compiler choose which random draw went
where, and six comparators that let the standard library choose how to break a tie. They change
what a given seed generates, which is the one thing that makes them awkward to offer — the fix
is not a no-op for anyone's saved game.

**Commits 1–3, 11, 13, 15, 18, 19, 21 and 22 depend on nothing the harness adds and are
separately upstreamable.** 18 and 19 are portability fixes rather than bug fixes — 19 in
particular turns an accidental overload binding into a defined one and costs upstream nothing,
since the save format does not move on any platform that currently builds. 21 removes a
dependency and 22 fixes a hard error under CMake 4; both are wins for upstream regardless of
the port.

**Commits 1–3, 11, 13 and 15 are pre-existing bug fixes.**
They are pre-existing bugs that determinism testing merely made visible — the MIDI one fixes a
launch failure on any machine without an ALSA sequencer, harness or no harness. That is why they
are ordered first, and why `audio.cpp` and `graphics.cpp` were split by hunk rather than letting a
bug fix ride along inside the harness commit.

**Commits 1–7 each build independently**, verified by checking each one out into an isolated
worktree and building it. Note `cmake` must be re-run per commit, not just `make`, because
`FeLib/CMakeLists.txt` globs its sources.

A clean build at the tip is exit 0 with **128 warnings**: 79 pre-existing `-Wstringop-overflow=`
plus the 49 format warnings commit 9 made visible (§7.8). Commit 9 introduced **no** warnings of
its own — the layout change is clean, and the 49 are pre-existing bugs that `LIKE_PRINTF` had
never been able to report in `Main`. Commit 13 introduced none either and removed four: the
`-Wstringop-overflow=` baseline drops 84 → 80. All 84 were the same line, `festring.h:172`
(`++REFS(Data)` in the copy constructor), reported once per inlining context, and adding
initializers to `char.h`/`human.h` changes what GCC inlines. Nothing was fixed and nothing was
hidden — expect that count to wobble whenever a widely-included header changes. It wobbled again
at commit 28, 80 → 79, for the same reason: `typedef.h`, `femath.h` and `level.h` all moved.

Files touched, by area:

```
portable math     portmath/                    (vendored musl + facade + checker)
                  Main/Source/worldmap.cpp     Main/Source/wsquare.cpp
                  Main/Source/char.cpp         Main/Source/human.cpp
                  Main/Source/gear.cpp         Main/Source/miscitem.cpp
                  Main/Source/level.cpp        Main/Source/game.cpp
                  Main/Source/igraph.cpp       Main/Source/itemset.cpp
                  Main/Source/wmapset.cpp      FeLib/Source/femath.cpp
                  FeLib/Source/bitmap.cpp
save format       FeLib/Include/save.h
corpora           tools/corpora/
harness core      FeLib/Include/harness.h      FeLib/Source/harness.cpp
integration       FeLib/Source/whandler.cpp    FeLib/Include/whandler.h
                  FeLib/Source/graphics.cpp    FeLib/Source/femath.cpp
                  FeLib/Source/rawbit.cpp      Main/Source/main.cpp
                  Main/Source/game.cpp
RNG unification   FeLib/Include/femath.h       FeLib/Source/sfx.cpp
                  audio/audio.cpp              fantasyname/namegen.{cc,h}
                  Main/Source/char.cpp         Main/Source/human.cpp
                  Main/Source/worldmap.cpp
uninitialised     Main/Include/script.h        Main/Include/igraph.h
                  Main/Source/database.cpp     FeLib/Source/graphics.cpp
                  Main/Include/bodypart.h      Main/Include/char.h
                  Main/Include/human.h         Main/Include/trap.h
                  Main/Include/lterras.h
headless          FeLib/Source/graphics.cpp    FeLib/Source/whandler.cpp
                  FeLib/Source/sfx.cpp
draw order        FeLib/Include/femath.h       FeLib/Include/typedef.h
                  Main/Source/char.cpp         Main/Source/human.cpp
                  Main/Source/nonhuman.cpp     Main/Source/bodypart.cpp
                  Main/Source/level.cpp        Main/Include/level.h
                  Main/Source/lsquare.cpp      Main/Source/lterras.cpp
                  Main/Source/fluid.cpp        Main/Source/gods.cpp
                  Main/Source/gear.cpp         Main/Source/miscitem.cpp
                  Main/Source/game.cpp         Main/Source/igraph.cpp
                  FeLib/Source/bitmap.cpp
tie breaking      Main/Source/worldmap.cpp     Main/Source/wterra.cpp
                  Main/Source/lsquare.cpp      Main/Source/char.cpp
                  Main/Source/level.cpp        Main/Source/god.cpp
                  Main/Source/game.cpp
struct layout     FeLib/Include/felibdef.h     FeLib/Include/graphics.h
                  Main/Include/igraph.h        Main/Include/game.h
tools             tools/savediff/              tools/play/
build             CMakeLists.txt               FeLib/CMakeLists.txt
                  Main/CMakeLists.txt          audio/CMakeLists.txt
                  .gitignore
```

`rawbit.cpp` is the only file the capture work added to the list: two guarded
`harness::RecordText` calls and one include. `.gitignore` gained `build/` and `build-*/`,
which were untracked but not ignored — a careless `git add -A` would have committed 35MB of
objects plus a `play-session/` directory of saves.

---

## 9. Reference: the wider port plan

Recommended approach, for context on why the harness is shaped this way:

1. **Fix scaling natively first** — the window is created without `SDL_WINDOW_RESIZABLE`
   (`graphics.cpp:207`), `GraphicsScale` is integer-only, and `SetMode` sets both
   `SDL_RenderSetLogicalSize` and `SDL_RenderSetScale` which manage the same state. Note that
   scaling pixels bigger is *not* the same as showing more dungeon — see the comment at
   `game.cpp:287` ("no way to fit as scaler is integer and not float"). SDL3's
   `SDL_SetRenderLogicalPresentation` has explicit LETTERBOX/STRETCH/INTEGER_SCALE modes and
   is worth considering; the SDL surface area is only ~1,800 lines.
2. **Drop PCRE — DONE.** Was used in 3 files: `Main/Source/message.cpp` (a dead include),
   `Main/Source/game.cpp` (the auto-pickup pattern) and `FeLib/Source/sfx.cpp` (the
   sound-effect trigger patterns). No call site used capture groups — every `pcre_exec`
   passed a NULL output vector and tested `>= 0` — so each became a `std::regex_search`
   over the same `(ptr, GetSize())` range, under the ECMAScript grammar. `pcre_study` has
   no equivalent and was dropped; `std::regex` compiles once at config-load either way.
   `find_package(PCRE REQUIRED)` is gone from both `CMakeLists.txt` and
   `cmake/FindPCRE.cmake` is deleted, so the dependency is off the build.

   **The grammar change was measured, not assumed.** PCRE is Perl-compatible and ECMAScript
   is not, and the patterns are *data* — 153 shipped in `Sound/SoundEffects.cfg` plus the
   default `AutoPickUpMatching`, several using `(?:` and negative lookahead `(?!`, both of
   which ECMAScript has (lookbehind, which it does not have, is unused). `tools/regexdiff/`
   compiles all 154 under both engines and compares their verdicts on every distinct string
   the two corpora draw: **111,342 comparisons, 0 mismatches, 0 compile failures**.

   ```bash
   g++ -std=c++11 -O1 -o regexdiff tools/regexdiff/regexdiff.cpp -lpcre   # needs libpcre3-dev
   ./regexdiff Sound/SoundEffects.cfg tools/corpora/*.text.log
   ```

   It is deliberately not in CMake — it needs the dependency this item removed, the same way
   savediff is `EXCLUDE_FROM_ALL` so the oracle cannot be broken by the thing it judges. Rerun
   it if `SoundEffects.cfg` or the `AutoPickUpMatching` default changes; it is the only thing
   standing between a config-file regex and a silent behaviour change.

   Note the corpora themselves do **not** exercise either regex path (headless runs make no
   sound, and the default auto-pickup pattern is disabled with a leading `!`), so
   `verify-corpora.sh` passing is evidence of no regression elsewhere, not evidence the
   swap works. The differential run above is what covers that.

   **CMake 4 — DONE, and this item's premise was wrong.** "CMake 4.x refuses `VERSION 3.5`"
   is not true: 4.0.3 configures and builds the tree unchanged, emitting only a deprecation
   warning ("Compatibility with CMake < 3.10 will be removed from a *future* version"). The
   removal threshold in CMake 4.0 is below 3.5, and 3.5 sits just above it. Worth stating
   plainly because it was the stated reason to touch the build at all.

   The thing that *did* hard-error under CMake 4 was somewhere else entirely, and was found
   only by running 4.0.3 rather than reasoning about it: `IGNORE_EXTRA_WHITESPACES=ON` ran
   `cmake_policy(SET CMP0004 OLD)`, and **CMake 4 removed that OLD behaviour outright**, so
   setting it is a hard error rather than a warning. The option defaults to OFF, which is why
   an ordinary build looked fine. Whitespace in library names now gets stripped instead of
   tolerated — that is what CMP0004 NEW wants, and it works on every CMake version, where the
   policy escape hatch works on none from 4.0 on.

   `cmake_minimum_required` is now `VERSION 3.10...4.0`: the floor clears the deprecation
   warning, the ceiling declares the project tested against 4.0. Verified as a matrix —
   {CMake 3.28.3, CMake 4.0.3} x {whitespace option OFF, ON} all configure with **zero
   warnings and zero errors**, against a baseline where two of those four cells failed. The
   raised floor moves policy defaults from 3.5 to the running version, so the build was
   re-verified rather than assumed: 128 warnings either way, and the **CMake 4-built binary
   replays both corpora 8/8 against the committed goldens**. `-std=c++11` is sufficient for
   `std::regex` and needed no bump.
3. **Emscripten build — RUNS HEADLESS UNDER NODE. The DOM blocker is closed.**

   `emcmake cmake` configures the tree with zero warnings and zero errors, the build exits 0,
   and it produces `ivan.js` plus a 6.2MB `ivan.wasm`. Build commands and the emsdk pin are
   in §3. The native build and both corpora are unaffected — re-measured, 8/8 against goldens.

   **What the CMake change is.** SDL2, SDL2_mixer and libpng come from Emscripten's ports, so
   the three `find_package(SDL2 REQUIRED)` calls (FeLib, Main, audio) and FeLib's libpng
   lookup are wrapped in `if(NOT EMSCRIPTEN)` and their result variables left empty —
   everything downstream interpolates them into `target_link_libraries`, where an empty
   variable expands to nothing, so no other line had to move.

   **The port flags are global, and that is the §7.7 lesson applied rather than ignored.**
   `-sUSE_SDL=2` is a compile flag as much as a link flag — it is what puts the port's headers
   on the include path — so a target that misses it cannot find `SDL.h`, and a target that
   gets a different set from its neighbours is exactly how `-DGCC` gave `graphicid` two
   layouts in one binary. Hence `add_compile_options` at the top level, not `add_definitions`
   per directory.

   **Three hazards this step was braced for turned out not to exist:**

   - **RtMidi needs no stubbing.** It auto-defines `__RTMIDI_DUMMY__` when no API macro is
     set (`RtMidi.h:571`), and the ALSA branch in `audio/CMakeLists.txt` is gated on
     `CMAKE_SYSTEM_NAME MATCHES "Linux"`, which is `Emscripten` here. It compiles to no-ops
     and prints "MidiOutDummy: This class provides no functionality." The `pthread_create`
     calls at `RtMidi.cpp:1555`/`:1617` are inside the ALSA path and are never compiled.
   - **SDL2_mixer is available as a port** (`embuilder build sdl2_mixer` succeeds), so the
     audio problem is narrower than "stub audio" — only MIDI is genuinely impossible.
   - **`audio.cpp`'s `SDL_CreateThread` is harmless.** Its return value is never checked
     (`audio.cpp:146`), so a failed thread creation just means the audio loop never runs.

   **The actual blocker: Emscripten's SDL2 port is DOM-bound at video init.** Run under node
   the binary reaches `main`, parses its harness arguments, reads the recording off the real
   filesystem via `NODERAWFS` and creates the trace file — then aborts in
   `emscripten_get_screen_size` with `ReferenceError: screen is not defined`. Shimming
   `globalThis.screen` moves it exactly one step, to `document is not defined` in
   `emscripten_set_pointerlockchange_callback_on_thread`. Piecemeal shimming is a losing game;
   the port registers browser event callbacks and wants a canvas.

   **That blocker is gone — the no-video path is `--headless` (§4).** Of the three ways out
   considered (run it in a real browser, put a DOM under node with jsdom, or give the game a
   path that needs neither), the third is the one this codebase is shaped for: §6.3 already
   establishes that rendering is entirely software into a `bitmap` double buffer and that the
   only GPU contact in the tree is one streaming texture blit. Skipping the window, the
   renderer, the texture and that blit — while still calling `TraceFrame()`, which hashes
   `DOUBLE_BUFFER` *before* `PrepareBuffer()` and so never touches the texture — leaves the
   identical game code running under bare node with no DOM at all, and it is byte-identical
   natively with and without.

   Two things that turned out to be part of "no video" and were not obvious:

   - **The audio device is the other half.** Emscripten's SDL2 audio backend wants an
     `AudioContext`, which node does not have, so `Mix_OpenAudio` fails — and the failure path
     in `soundeffects::initSound` calls `iosystem::AlertConfirmMsg`, which draws a dialog and
     then **blocks on `GET_KEY()`**. During a replay that eats a recorded key and desynchronises
     the run. `--headless` declines to open a device rather than opening one and handling the
     failure. Worth remembering as a general hazard: *any* modal alert during a replay consumes
     a key the recording did not budget for.
   - **The environment does not cross into the module.** `SDL_VIDEODRIVER=dummy` reaches nothing
     under node, so the decision had to be a program flag rather than an env var.

   Asyncify is wired up behind `WASM_ASYNCIFY` (default ON) for the blocking `SDL_WaitEvent` in
   `GetKey`. The old note here said a replay never reaches it; that is wrong. `iosystem::Menu`
   and `bitmap::FadeToScreen` call `globalwindowhandler::WaitUntil`, which is `SDL_Delay`, so a
   replay unwinds and rewinds through the menu fade on every run. Leave it on. JSPI or
   `-sPROXY_TO_PTHREAD` later. It now also carries **real keystrokes** through `GetKey`, which
   the node host never exercised because the harness answers before the blocking call — §9.5.

   **The browser half of this step is done too, and it is a different host rather than more of
   the same one.** `--headless` was the way *around* the DOM; `-DWASM_BROWSER=ON` is the way
   *into* it, and the game plays. §9.5.

   Three pre-port hazards were already dealt with: the host libm (§6.7), the `time_t` overload
   that would have failed this step outright (§6.8), and the MT width question now recorded in
   §6.3. A fourth was not anticipated at all and is the reason the build first died in the main
   menu: **Emscripten compiles with exception throwing disabled**, and IVAN throws as ordinary
   control flow. `-fexceptions`, §3.

4. **Make the game compiler-independent — half done, and where the interesting bugs were.**
   Eleven defects where the source left a choice to the compiler or the standard library rather
   than making it. The non-combat corpus now matches native-vs-WASM byte for byte; the auto-play
   corpus matches for 389 of 593 frames. Findings, technique and what is left: **§9.4**.
5. **TinySoundFont** for MIDI (RtMidi cannot work in WASM), IDBFS for saves, touch input.
   **IDBFS is now the one that blocks something real** — the browser build plays but cannot
   save across a reload (§9.5).
6. **SDL3 / JSPI / threads** as optional performance and quality passes.

### 9.4 Native vs WASM: what the first real frame comparison found (new)

With `--headless` in place, `tools/corpora/compare-targets.sh` replays each committed corpus on
both builds and compares the traces. Where that stands:

| | non-combat | auto-play 200 |
|---|---|---|
| frame trace | **identical** | identical for 389 of 593 frames |
| text log, screenshot, sidecar | **identical** | identical to the same point |
| `.wm` | **identical** | **identical** |
| level file | **identical** | descent level identical; the fought-over one differs |
| `.sav` | one byte — `GetTimeSpent`, §5 | differs after the divergence |

Getting from "it runs" to that table meant fixing eleven distinct defects across some fifty
sites. Every one is a real bug that has been in IVAN for years, every one is invisible to any
test that compares a build against itself, and none of them is about Emscripten: they are places
where the program left a choice to the compiler or to the standard library.

**They fall into three classes**, and the third is a single bug, so start with it. `cachedfont::
PrintCharacter` walked a 9x9 character cell a `ulong` at a time, ending at
`FontPtr + (20 / sizeof(ulong))` — two words and *eight* pixels on a 64-bit host, five words and
*ten* on a 32-bit one, and nine on neither. Every 64-bit build of IVAN has been dropping the
ninth column, which is the shadow's right edge: one dim pixel per character, for as long as
64-bit builds have existed. It is now copied a pixel at a time, which is width-independent and
agrees with the `NormalMaskedBlit` fallback the guard above it falls through to. The other two
classes are larger.

**(a) Unsequenced draws from the shared RNG.** C++ leaves function arguments, and the operands
of arithmetic and relational operators, unsequenced with respect to each other. So

```cpp
EyeColor = MakeRGB16(R + RAND_N(41), G + RAND_N(41), B + RAND_N(41));
```

takes three draws in an order the compiler picks. GCC picks right-to-left and Clang picks
left-to-right, and both are correct. The draw *count* is identical, so the RNG stream stays in
lockstep and every determinism test in this document passes on both builds — what differs is
which draw lands in which field. That one line is why the player's eyes came out a different
colour under Emscripten, which is how this whole class was found: two pixels, at
`(198,243)` and `(200,243)`, in a 16x16 sprite.

The rule is now written where anyone adding RNG code will meet it, above the `RAND` macros in
`FeLib/Include/femath.h`. Sites fixed:

| Site | What it decided |
|---|---|
| `human.cpp` `playerkind::PostConstruct` | the player's hair and eye colour |
| `femath.h` `region::Randomize` | **where every generated room sits and how big it is** |
| `level.cpp` `GenerateDungeon` fill loop | ground vs over terrain for all 1,600 squares of every level |
| `level.cpp` `CreateRoomSquare` x11, via a new overload | the same, per room square |
| `level.cpp` fountain and altar placement | which square in the room |
| `fluid.cpp` `AddLiquidToPicture` | the speckle colour in a pool of blood |
| `lsquare.cpp`, `char.cpp`, `bodypart.cpp`, `human.cpp`, `nonhuman.cpp`, `gods.cpp`, `lterras.cpp`, `game.cpp`, `igraph.cpp`, `bitmap.cpp` | to-hit rolls, damage rolls, lightning and explosion colours, sparkle and scar positions, will-power contests |
| `gear.cpp`, `miscitem.cpp`, `lterras.cpp` `InitMaterials` | the volume of an item's two materials |

Not every multi-draw expression is a bug, and the ones left alone are left alone on purpose:
`&&`, `||` and `?:` sequence their operands by definition, and `RAND()%36 + RAND()%36` gives the
same value whichever half is drawn first. Roughly half the 61 candidate sites are of that kind.

**Two traps in finding these.** A line-based grep misses them — `AddLiquidToPicture` spreads its
three draws over three lines, and the scanner has to split on statements. And a *visible* draw
can be unsequenced with a *hidden* one: `SetLTerrain(GTerrain->Instantiate(), OTerrain->Instantiate())`
has no `RAND` in it at all, and it is the single most consequential site in the list.

**(b) Ties broken by the standard library.** `std::sort` is not stable and `std::priority_queue`
says nothing about equal elements, so a comparator that inspects less than the whole object
leaves the rest to libstdc++ or libc++ — which disagree.

| Site | Comparator looked at | What it decided |
|---|---|---|
| `worldmap.cpp` `distancetoattnam` | distance only | **where the towns and dungeon entrances go** |
| `lsquare.cpp` ground/over border partners | tile priority only | what is drawn over what, and 3% of every level file |
| `char.cpp` `svpriorityelement` | strength value only | which body part a bite lands on — the two legs are always a tie |
| `level.cpp` `nodepointerstorer` | distance, then diagonals | the route every monster and the auto-play AI walks |
| `wterra.cpp` `DrawOrderer`, `god.cpp` `materialsorter`, `game.cpp` `NameOrderer` | one field each | draw order, wish material, list order |

Fixed either by making the order total (a body-part index, a position) or by moving to
`std::stable_sort`, where the input order is already deterministic. Prefer the total order when
there is an obvious tie-breaker; it cannot depend on the implementation at all.

Note how far each of these is from where it hurts. `distancetoattnam` is nine lines of world
generation and it decided the whole overworld; `svpriorityelement` is a heap of at most ten
elements and it decided which leg a hedgehog bit.

**What is still open.** On the auto-play corpus the two builds agree exactly — same frame
hashes, same RNG count, same text — through world generation, dungeon generation, the descent
and the first ~190 auto-play turns, and then diverge at frame 390 during a long fight with a
hedgehog in which the player dies and insta-resurrects. At the divergence the RNG counts part
company by 18 draws, so it is game state and not rendering. Ruled out so far: the pinned math
(§6.7, now measured across compilers), `long` width in the fluid and volume paths,
uninitialized memory (§6.6e, both corpora valgrind-clean), and pointer-ordered container
iteration (§6.3 still holds — every `std::map` in the tree is keyed by an ID, a `v2` or a
`configid`).

**How to pick it up.** The technique that localised every one of the nine is worth following
rather than reinventing:

1. `compare-targets.sh` gives the first differing frame and whether `rng` moved with it.
2. Bracket it with a temporary `fprintf` of `harness::GetRandCount()` at a frequent, portable
   event — `msgsystem::AddMessage` and `character::Move` both work well — built on *both*
   targets and diffed. That turns "somewhere in 1,039 draws" into "between these two events".
3. Inside the bracket, run the native build under gdb with a breakpoint on `femath::Rand` and
   aggregate the backtraces by call site. The histogram names the suspect.
4. For a pixel difference instead of a draw-count difference, `--shot-dir` on a truncated
   recording plus a PNG differ localises it to single pixels, and a gdb watchpoint on that
   pixel of `graphics::DoubleBuffer->Image[y][x]` names the code that wrote it.

The first divergence in this run was one pixel of a text shadow; the second was two pixels of a
sprite's eyes. Neither would have survived being described as "a hash mismatch".

---

### 9.5 The browser host: what it took and what it proved (new)

**Measured 2026-08-16: `-DWASM_BROWSER=ON` produces a page that plays.** Loaded in headless
Chrome 143 it reaches the main menu — artwork, fonts, all five entries — and driven with eight
`ENTER` keystrokes over CDP it plays the intro, generates a world, creates a character and lands
on the world map with the side panel populated and "Turn 0" on the clock. Console output for
the entire run was one line, `MidiOutDummy: This class provides no functionality.` No abort, no
exception. Build recipe in §3.

**This is a second host, not seam-1 progress, and the distinction matters.** §9.3's `--headless`
was the way *around* the DOM so the differential harness could run under bare node; this is the
way *into* it. The browser build has never been compared against a golden and cannot be an
oracle in its current shape — `NODERAWFS` is what let the harness write a trace, and turning it
off is the first thing `WASM_BROWSER` does. Frame comparison stays on the node host.

**What was actually in the way was three build-system facts, not game code.** Nothing in
`Main/` or `FeLib/` changed to make this work, which is the first real evidence that the
headless path and the compiler-independence work (§9.4) left the game genuinely portable:

- `NODERAWFS` binds to node's `fs` module. In a browser it is not a degraded filesystem, it is
  a missing one, so the option had to become mutually exclusive with the browser target rather
  than merely defaulted differently.
- With it off, MEMFS starts empty. `Graphics/` and `Script/` are preloaded to absolute paths
  because `PORTABLE_BUILD` returns `"./"` from both directory functions and the module's working
  directory is `/`. Verified in the generated package metadata: `"/Graphics/Char.png"`.
- Emitting `.js` gives you no page. The SDL2 port looks up `#canvas` at video init, so the
  executable has to be emitted as `.html` for emcc to generate one.

**Two hazards this section was braced for did not appear.** Both were predicted here before the
run and both were wrong, which is worth recording as plainly as the successes:

- **Asyncify carries real input.** §9.3 established it unwinds `SDL_Delay` through the menu
  fade, but a replay is answered by the harness *before* the blocking `SDL_WaitEvent` in
  `GetKey`, so no node run had ever proven a keystroke could cross it. A browser one has now.
- **The audio alert never fired.** `Mix_OpenAudio` was expected to fail before a user gesture
  and drop into `iosystem::AlertConfirmMsg`, which blocks on `GET_KEY()`. It did not — an
  `AudioContext` is constructible while suspended, so the open succeeds. `WASM_PRELOAD_AUDIO`
  was OFF for this run, so whether anything is audible is untested.

**What is open.**

- **Saves do not survive a reload.** MEMFS is ephemeral and `GetUserDataDir()` is `"./"`, so
  `Save/`, `Scrshot/` and `ivan.cfg` all vanish with the tab. This is §9 step 5's IDBFS, and it
  is now the item blocking something a player would notice rather than a theoretical one.
- **`ASYNCIFY=1` instruments the whole binary**, all 7.5MB of it. Nothing has been measured
  against a native frame time; `ASYNCIFY_ONLY` or JSPI is the tuning knob if it needs one.
- **Audio is unexercised.** Headless Chrome has no audio device, so `WASM_PRELOAD_AUDIO=ON`
  needs a human with speakers, not another CDP script.
- **Input beyond the auto-play set is unexercised.** `` ` ``, `y`, `~` and 400 `.` have all
  crossed `GetKey` in a browser (§9.6). Direction keys, `>` and the rest of the command set go
  the same way, so there is no reason to expect trouble, but "no reason to expect trouble" is
  what §9.4 was full of.

---

### 9.6 Collecting a browser crash, and why a stripped trap is not a lead (new)

A wasm trap out of a release build looks like this, and it is worth being precise about how
little it contains:

```
Uncaught (in promise) RuntimeError: memory access out of bounds
    at ivan.wasm:0xed4dd
```

Those are byte offsets into the binary. A release build carries **no name section and no
DWARF** — measured with `llvm-objdump -h`, the sections are TYPE, IMPORT, FUNCTION, TABLE,
MEMORY, GLOBAL, EXPORT, ELEM, DATACOUNT, CODE, DATA and nothing else. So nothing maps `0xed4dd`
back to a function: not `emsymbolizer`, which needs DWARF; not the browser; and not a later
rebuild, because the offsets belong to the exact binary that produced them and even that binary
no longer knows its own function names. **The information was never emitted.** A trace of this
shape is a prompt to rebuild, not a lead, and time spent staring at it is wasted.

That splits into two independent problems: make the next trap legible, and make the crash
reproducible somewhere with a debugger.

**Legible: `WASM_DEBUG`.** `--profiling-funcs` keeps the name section at no codegen cost, so
frames print as `character::Move`. Measured: the debug wasm carries an 0x8b9f4-byte `name`
section against the release build's none, for 8.3MB against 7.5MB. `ASSERTIONS=2` and
`STACK_OVERFLOW_CHECK=2` come with it, the second because the 8MB stack (§3) is generous but the
level generator recurses, and an overflow is otherwise indistinguishable from the out-of-bounds
access it gets mistaken for. `WASM_SAFE_HEAP` is a separate option because it instruments every
load and store: it names the access and the address outright, at a slowdown that makes the game
hard to steer by hand. Turn it on once a recording reproduces the crash, not while hunting one.

**Reproducible: `tools/web/harness-pre.js`.** The point is not to debug in the browser but to
get the crash onto the native build, where gdb, valgrind and ASan all apply and where the
harness already replays a session deterministically. The node host reads argv and writes through
NODERAWFS; a browser has neither, so the pre-js turns the query string into argv and reads the
recording back out of MEMFS:

```
http://localhost:8112/ivan.html?record=/session.rec&seed=999
```

**What makes this work was already in the harness.** `RecordKey` flushes every key as it writes
it (`harness.cpp:545`), and the comment above `ReadTrailer` says a killed recorder "loses nothing
but the trailer". A trap therefore leaves the recording complete but for its `# end keys=` line —
the keys that led to the crash survive the crash, and the seed rides along in the header, so the
file is self-contained. Nothing in the harness had to change; it had no way to be *reached* from
a browser, which is all the pre-js supplies.

**Measured 2026-08-16, end to end.** A session driven with `?record=/session.rec&seed=999`,
extracted from MEMFS with `ivanHarness.text()`, replayed on the native binary:

| Run | Result |
|---|---|
| 8-key browser recording, replayed natively | exit 0, **358 frames** |
| 403-key auto-play browser recording, replayed natively | exit 0, **752 frames** |
| Either, on the native harness's reading of the missing trailer | correctly reported as "cut short" |

**A WASM-only crash is a finding, not a dead end.** wasm32 traps on accesses x86-64 absorbs
silently: a read past a buffer is still inside a mapped page natively and returns garbage, where
in wasm it is either outside linear memory or caught by `SAFE_HEAP`. The same goes for a null
dereference and for a `long` that was 8 bytes when a save was written and is 4 here (§6.8, §7.9).
So a crash that will not reproduce natively usually means a real latent defect that native
testing structurally cannot see — the class §6.6 and §9.4 were about, and an argument for
keeping the second host rather than a reason to distrust it.

**One crash is reported and unreproduced.** A `memory access out of bounds` during ordinary
interactive play, reported against a release build, so its trace is unsymbolizable per the above
and nothing can be recovered from it. **400 turns of auto-play under `WASM_DEBUG` did not
reproduce it** — clean run, no trap, no assertion — which rules out the auto-play path and means
the next attempt needs the steps that produced it, recorded with `?record=`.
