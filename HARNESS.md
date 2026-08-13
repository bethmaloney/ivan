# IVAN Differential Test Harness — Progress & Handoff

**Status:** working and verified. Not committed. Nothing here is on a branch yet.
**Last updated:** 2026-08-14

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
| Clean build from scratch | exit 0, 84 warnings — **identical count to pre-change baseline** |
| Headless boot (no display, no ALSA) | reaches main menu |
| Replay end-to-end | exit 0 |
| Two replays → trace comparison | **byte-identical** |
| Two replays over **1,067,118 RNG draws** + full world gen | **byte-identical** (155 frames) |
| Same, under 100% CPU load on all cores | **byte-identical** |
| Non-combat corpus, **8** isolated runs (trace, PNG, text) | **1 distinct outcome**, 441 frames every time |
| Combat corpus, 200 auto-play turns, 8 isolated runs | **1 distinct outcome** — was 5, see §6.5 |
| Combat corpus, **2,800** auto-play turns, 36 deaths, 16 concurrent runs on a saturated machine | **1 distinct outcome** (trace and text) |
| Same binary, harness options varied (`--text` on/off) | **2 distinct outcomes** — see §6.6 |
| savediff exit codes (0 same / 1 differ) | correct, with side-by-side hexdump |
| Two replays → screen PNG, text sidecar and text log | **byte-identical** |
| savediff on real saves from two identical replays | `.sav` SAME, `.wm` SAME, level file **DIFF** — see §6.4 |

The CPU-load test is the meaningful one *for the risk it was designed to test* — wall-clock
leaking into game state. Saturating the machine and still getting identical output is real
evidence about the clock.

**Read the determinism rows narrowly.** Combat is now reproducible — §6.5 is found and fixed —
but only *for a fixed binary and a fixed set of harness options*. Change what else the process
allocates and the run changes with it, because game logic still reads uninitialized memory
(§6.6). So the harness is usable as a native-vs-native regression oracle today, and is **not
yet** usable for native-vs-WASM, where nothing about the allocation pattern is held constant.

Two rules for anyone measuring determinism here, both learned by getting it wrong first:
**give every run its own directory** (a portable build writes saves and question history into
the launch directory, so runs contaminate each other), and **use at least 8 runs** — the
combat divergence is flaky enough that pairs agree by chance often enough to fool you.

The capture layer being byte-identical across runs matters too: it means a PNG or a text
layer can be a golden artifact in CI, not just a debugging aid.

### Not yet verified

- **The remaining uninitialized reads of §6.6.** Located by memcheck, not yet fixed. This is
  now the top priority, and it blocks seam 1 specifically.
- **DJGPP / SDL1 branches.** Edited but no toolchain available to compile them.

The auto-play AI **does** run now (§7.2) and generates thousands of turns of real play, and as
of §6.5 it reproduces: 2,800 turns including 36 deaths replay identically 16 times over. It is
a usable differential corpus for native-vs-native work.

---

## 3. Build and run

### Dependencies

```
sudo apt-get install -y libsdl2-dev libsdl2-mixer-dev libpng-dev libpcre3-dev
```

All four are required. Without `libsdl2-mixer-dev` and `libpcre3-dev` the link fails.

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

### Run headless

```bash
SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy ./ivan --replay in.rec --trace out.jsonl --seed 999
```

Run from a directory containing `Graphics/ Script/ Music/ Sound/` (symlinks are fine).

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

### Critical limitation — save files are NOT a native-vs-WASM oracle

`SAVE_COMPATIBILITY` is dead code, so `long`/`ulong` serialize at **native width**. A WASM
save is ILP32; every field after the first `long` sits at a different offset. `--word-size`
exists to make this explicit rather than silently decoding a 32-bit save with 64-bit fields.

**Consequence for the port plan:** savediff works for native-vs-native regression testing but
cannot validate the WASM port. Frame hashing and the state stream carry that load — which is
an argument for building the state-digest layer sooner rather than later.

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

### 6.3 Architecture facts (established, don't re-derive)

- RNG is Mersenne Twister; `femath::Rand()` is the funnel, `femath::SetSeed` at
  `femath.cpp:78`. The seed is already persisted into saves (`game.cpp:3458-3460`, restored
  at `:3540`) — the game was designed to be reproducible across save/load.
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
- **libm is a real portability risk.** `sin`/`cos` are called in world generation
  (`worldmap.cpp:757-760`, `:1260-1261`) and are not correctly-rounded by any standard;
  glibc and Emscripten's musl-derived libm will differ in the last ulp, which through
  `int(...)` truncation becomes a different tile. Vendor a fixed `sin`/`cos` **before**
  comparing native against WASM, or you will chase phantom diffs. `sqrt` is safe (IEEE-754
  mandates correct rounding; WASM has a native `f64.sqrt`).

---

### 6.4 Level files diverge between two identical replays (new, unfixed)

Two replays of the same recording under the same seed, compared with `savediff`:

| Role | Verdict |
|---|---|
| `AutoSave.sav` | SAME |
| `.wm` | SAME |
| `AutoSave.40` (UNDER_WATER_TUNNEL level 0) | **DIFF** |

179 bytes of 892,168 differ (0.02%), scattered across 61 of 218 4KiB blocks. First difference
at offset 1961, past savediff's decodable prefix (which ends at 1640), so the fields cannot be
named. The differing values are small and close — `0x84` vs `0x87` at the first site — which
looks like a counter rather than a pointer or a heap smear, and the pair of runs did take
different wall-clock times.

Not yet attributed. Candidates worth checking first, in order: the `GetTimeSpent` family
already listed in `savediff --known-nondeterminism`, then the same raw
`SaveFile.Write()`-over-`Alloc2D` pattern as §6.2 but in the level and `Room` region.

Note this is a *save-content* divergence with **no frame-hash divergence at all** — the trace,
the PNG and the text layer were byte-identical across the same two runs. Whatever this is, it
does not reach the screen, which is precisely the class of bug the state-digest layer of §7.5
is meant to catch and neither pixel comparison nor gameplay ever will.

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

### 6.6 Game logic reads uninitialized memory — proven, partly fixed (new)

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

**Fixed: `graphicid`.** `graphicid() = default` left the struct uninitialized while `operator<`
memcmps *every byte* of it (it is the `std::map<graphicid, tile>` key) and the serializer writes
every byte with a raw `Write`. So padding was a live input to both the graphics cache ordering and
the save content. Now zeroed in the constructor. Identical shape to the `fastscriptmember` bug in
§6.1 — worth grepping for `= default` on any struct that gets memcmp'd or raw-written.

**And there is padding to hit, because of a build bug.** `NO_ALIGNMENT` is
`__attribute__((packed))` only when `GCC` is defined, and `add_definitions(-DGCC)` sits in
`FeLib/CMakeLists.txt:18` — a *sibling* directory to `Main/`, so it never reaches it. Verified
against the real header:

```
Main's flags   (no -DGCC):  sizeof(graphicid)=48  alignof=4   <- one tail padding byte
FeLib's flags  (-DGCC):     sizeof(graphicid)=47  alignof=1
```

One struct, two layouts, one binary — an ODR violation, and `igraph.cpp:364` writes
`sizeof(Value)` bytes from a `Main` translation unit. **This is almost certainly the §6.4
level-file divergence**: `graphicid` is serialized once per cached tile per object on a level, and
its padding byte was garbage. Not fixed here because moving `-DGCC` up to the root `CMakeLists.txt`
changes the on-disk save size from 48 to 47 bytes per `graphicid` — a save-format break, and
therefore a call to make deliberately rather than as a side effect.

**Not fixed: the `Spawn` and database families.** `sysbase::Spawn` does `new type` and the
per-class constructors initialize only some members (`item::item()` sets 6 of them). Those need
in-class initializers or fuller constructors, class by class. `database.cpp:111` was changed to
`new database()` so the prototype table is value-initialized — correct in itself, but it made **no
measured difference** to this corpus, so the live carrier is elsewhere in that list.

Until this is done, `MALLOC_PERTURB_` is a usable workaround for cross-configuration comparison,
and it is a workaround, not a fix — it makes the reads return a constant instead of not happening.

## 7. Open items, roughly in priority order

**§6.6 now sits above all of these**, and it is specifically what blocks seam 1: a WASM build
shares no allocation pattern with a native one, so every uninitialized read becomes a phantom
diff that owes nothing to Emscripten. §6.5 is done.

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

### 7.7 Decide what to do about `-DGCC` (§6.6)

`add_definitions(-DGCC)` in `FeLib/CMakeLists.txt:18` does not reach `Main/`, so `NO_ALIGNMENT`
is packed in one half of the binary and a no-op in the other. Moving it to the root
`CMakeLists.txt` is the obvious fix and changes the save format; the TODO already on that line
("Use `__GNUC__` in the code instead") is the better one and changes it too. Either way it is a
deliberate save-format break, so it wants a decision, not a drive-by.

---

## 8. Change inventory

**Nothing is committed.** Working tree only.

Modified (19 files):
```
CMakeLists.txt              FeLib/Source/rawbit.cpp      Main/Source/char.cpp
FeLib/Include/femath.h      FeLib/Source/sfx.cpp         Main/Source/game.cpp
FeLib/Include/whandler.h    FeLib/Source/whandler.cpp    Main/Source/main.cpp
FeLib/Source/femath.cpp     audio/audio.cpp              Main/Source/worldmap.cpp
FeLib/Source/graphics.cpp   fantasyname/namegen.cc       fantasyname/namegen.h
Main/Include/script.h       Main/Include/igraph.h        Main/Source/database.cpp
Main/Source/human.cpp
```

The §6.5 and §6.6 work added three files to that list and touched two more:

- `Main/Source/char.cpp`, `Main/Source/human.cpp` — the 19 `clock()` sites (§6.5).
- `Main/Include/igraph.h` — `graphicid` zero-initialized (§6.6).
- `Main/Source/database.cpp` — `new database()` (§6.6).
- `FeLib/Include/harness.h`, `FeLib/Source/harness.cpp`, `FeLib/Source/femath.cpp` — the
  `grng`/`nest`/`depth` attribution (§6.5a). Two inline counter bumps in `CountRand` and one each
  in `SaveSeed`/`LoadSeed`; the zero-cost-when-disabled property is unaffected.

New (untracked):
```
FeLib/Include/harness.h
FeLib/Source/harness.cpp
tools/savediff/CMakeLists.txt
tools/savediff/Source/savediff.cpp
tools/play/play.py
tools/play/README.md
```

`rawbit.cpp` is the only file the capture work added to the modified list: two guarded
`harness::RecordText` calls and one include.

A clean build after all of this is **exit 0 with 84 warnings — the same count as the
pre-change baseline**, verified by rebuilding from scratch in a separate build directory.

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
2. **Drop PCRE** (used in 4 files, never uses capture groups — `std::regex` covers it),
   bump `cmake_minimum_required` from 3.5 (CMake 4.x refuses it) and `-std=c++11`.
3. **Emscripten build with Asyncify**, audio stubbed — get it rendering in a browser.
   The blocking `SDL_WaitEvent` inside `GetKey` is the core obstacle; Asyncify first,
   JSPI or `-sPROXY_TO_PTHREAD` later.
4. **TinySoundFont** for MIDI (RtMidi cannot work in WASM), IDBFS for saves, touch input.
5. **SDL3 / JSPI / threads** as optional performance and quality passes.
