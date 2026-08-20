# Port log

What the port found, in the order it found it. `PORTING.md` is the living document — the seams,
the harness reference, where the port stands; this is the record behind it.

**The `§` numbers are stable identifiers.** Comments in `Main/`, `FeLib/`, `audio/`, `tools/` and
`web/` cite them (`§6.6d`, `§9.4`, `§9.11`), so a section keeps its number even when its content
is corrected. Numbers that are absent were resolved into another section and are noted where they
went.

Every number here was measured. Where a measurement is corpus-dependent the corpus is named,
because most of them are — `tools/corpora/README.md` holds the check values.

---

## 6. Findings

### 6.1 Pre-existing bugs, found by determinism testing and fixed

Latent in the shipping game; visible only once two runs were compared.

- **`fastscriptmember() = default`** (`Main/Include/script.h`) left a POD member uninitialized
  while `Save()` wrote it unconditionally — **1,975 bytes of uninitialized heap into every `.sav`**
  (423 four-byte fields). Undefined behaviour, a heap disclosure in a file players share, and a
  source of save nondeterminism. Now `fastscriptmember() : Member() { }`.

- **`bitmap(v2)` never initialized its pixels.** `iosystem::Menu` fades in the previous
  double-buffer contents, so a fresh process blended uninitialized memory and never drew the same
  menu twice. Fixed by using the existing `bitmap(v2, col16)` overload in `graphics::SetMode`.

- **Four RNG streams, not one.** Game logic drew from `std::rand()` in two places while a
  background audio thread mutated it — a data race whose effect depended on the scheduler.
  `fantasyname/namegen.cc:24` held a third generator, an `mt19937` seeded from
  `high_resolution_clock`, producing the player's default name, which reaches the screen and the
  save filename. Game decisions now draw from the pinned MT via `femath::Shuffle`; the audio
  thread and `sfx.cpp` each got a private xorshift; namegen is pinned to the harness seed. The
  fourth stream was `clock()` in the auto-play AI — §6.5, found later and much more consequential.

- **MIDI failure aborted startup.** `audio::Init` called `ABORT("MIDI Out Error")` when
  `RtMidiOut` construction threw, which happens on any machine with no ALSA sequencer — every
  standard CI runner, and WSL2. **IVAN could not launch at all on such machines.** `audio::isInit`
  existed and was read nowhere, so there was no degradation path. Init failure is non-fatal now
  and the four entry points that dereference `midiout` are guarded. Note `ivanconfig::Initialize()`
  calls two of them *after* `audio::Init`, so without the guards an early return segfaults rather
  than aborting.

### 6.2 The `.wm` residue — open

**`.wm` files differ by 17 bytes between identical replays** in some states. Traced to
uninitialized heap from raw `SaveFile.Write()` over `Alloc2D`'d buffers in
`area::Save`/`worldmap::Save` and the wsquare region — 358 such bytes, confirmed with a
pattern-filling allocator. Same class as `fastscriptmember`, distinct family.

Deliberately not fixed: blind-zeroing those buffers risks masking a genuine "this should have been
written" bug. §7.6 is the work.

Under the `MALLOC_PERTURB_` differencing of §6.4a — which exposes every uninitialized byte that
reaches a file, not only the ones that happen to differ — `.wm` and `.sav` leak **zero** bytes on
all committed corpora. That is much better evidence than a plain comparison, because it does not
depend on the allocator handing back different garbage. It is not proof: `MALLOC_PERTURB_` covers
only the heap, and no corpus visits enough of the world map to write the whole `.wm`. The 17-byte
residue is state-dependent, so a passing `.wm` comparison is not evidence the bug is gone.

### 6.3 Architecture facts — established, don't re-derive

- RNG is Mersenne Twister; `femath::Rand()` is the funnel, `femath::SetSeed` at `femath.cpp:78`.
  The seed is already persisted into saves (`game.cpp:3458-3460`, restored at `:3540`) — the game
  was designed to be reproducible across save/load.
- **The MT stream does not depend on the width of `ulong`**, which is the single most load-bearing
  assumption in the port. The state array is `ulong[624]`, so 64-bit natively and 32-bit under
  Emscripten — but `SetSeed` masks the seed and every subsequent word with `& 0xffffffff`
  (`femath.cpp:87`, `:90`), the twist only combines words that are already 32-bit, the tempering
  masks are 32-bit constants, and `Rand()` returns `y & 0x7FFFFFFF`. Nothing escapes into the high
  half at either width. Checked by reading every line of the generator: if this were wrong, every
  determinism number in this file would be native-only.
- Rendering is entirely software into a `bitmap` RGB565 double buffer. The **only** GPU contact is
  one streaming texture blit in `BlitDBToScreen`. This is why the WASM port is tractable, why
  `--headless` is free, and why the frontend seam is clean.
- Input funnels through `globalwindowhandler::GetKey`/`ReadKey` only.
- `time(0)` never touches game logic — highscore timestamps and `GameBegan`/`LastLoad` only.
- No pointer-ordered iteration in logic. The one `std::map<cchar*,...>` (`script.h:129`) uses a
  content comparator, so no ASLR sensitivity.
- ~330 entity classes generated by macros (154 `ITEM(`, 110 `CHARACTER(`, plus terrain, god, room
  and material). Deep virtual hierarchy, 263 virtuals in `char.h` alone.
- `-ffast-math` appeared in the upstream legacy `.mak` files but **not** in `CMakeLists.txt`. Keep
  it that way. (Those makefiles were never in this fork, and the DOS build they served is gone.)
- libm was a real portability risk and is pinned — §6.7. Game code calls `portmath::` rather than
  `<cmath>`, and `portmath/check-callers.py` fails on a direct call. `sqrt` stays on the host
  (IEEE-754 mandates correct rounding, WASM has a native `f64.sqrt`), as do `fmod`, `floor`, `ceil`
  and `abs`.

### 6.4 Level files diverged between identical replays — closed for every path the corpora reach

Two replays of one recording under one seed gave `.sav` SAME, `.wm` SAME, level file **DIFF**, with
**no frame-hash divergence at all** — trace, PNG and text layer byte-identical across 8 runs.
Neither mechanism reached the screen, which is the class of bug §7.5's state digest is for and
neither pixel comparison nor play ever finds.

Both mechanisms are closed: mechanism one by §6.6c (`character::TemporaryStateCounter`, 1,860
bytes → 0), mechanism two by §6.6d, which found the "raw pointers" of §6.4b were stale heap
pointers in uninitialized `trapdata` fields rather than a second mechanism at all. §6.6e closed a
third family that appeared when §9.4 changed what the corpora generate. Eight ordinary replays now
produce one level file, with no fixed heap fill and no ASLR trick.

The §7.7 padding fix shrank the divergence roughly 4× without closing it — differing bytes 179 →
40, and the first difference moved from offset 1961 to 691807. That the early region went clean is
the informative part: it is where `graphicid`'s padding byte was serialized once per cached tile.

#### 6.4a The measurement technique — the reusable part

Two runs differing by 40 bytes understates the exposure by 46×; 40 is only what two runs happen to
differ by, since the allocator usually hands back similar garbage. Hold the heap fill constant and
vary it instead:

```bash
# same corpus, two fill values, own directory each
MALLOC_PERTURB_=42 ./ivan --replay noncombat.rec ...
MALLOC_PERTURB_=99 ./ivan --replay noncombat.rec ...
cmp -l a/Save/*.40 b/Save/*.40 | wc -l          # 1,860
```

1,860 bytes differed across 44 regions and **every one was exactly the glibc fill byte** — `0xd5`
(`42 ^ 0xff`) against `0x9c` (`99 ^ 0xff`), no exceptions. Runs sharing a fill value were
byte-identical. So uninitialized heap was the only remaining content source.

**Pair the byte count with a layout check** — that is what turns a family into a field. Group the
differing offsets into contiguous runs and look at the shape before theorising:

```bash
cmp -l a/Save/*.40 b/Save/*.40 | awk '{print $1-1}' > offs.txt   # 0-based offsets
```

All 1,860 bytes sat inside sixteen disjoint 128-byte windows, one per character on the level,
4-byte aligned. `TemporaryStateCounter[STATES]` is `int[32]` — 128 bytes — and the only field of
that shape in the write path. The confirmation was better than the arithmetic: within each window
the defined slots were exactly the ones whose `TemporaryState` bit was set, because `Initialize`
assigns a counter only where the bit is set while `character::Save` writes all 32 unconditionally.
Windows with no state bits leaked all 128 bytes; a heavily-flagged monster leaked 44. The slots
that came back defined most often decoded to INFRA_VISION, SEARCHING, GAS_IMMUNITY and LEPROSY —
ordinary monster class-states. Do that cross-check: a shape that matches by accident will not also
decode to something sensible.

**Three traps in this technique**, all of which cost time:

- **`MALLOC_PERTURB_=255` is not a zeroed heap.** It looks like one (`255 ^ 0xff == 0`) and so
  looks like a way to simulate a complete zero-init fix without touching code. glibc fills on free
  with the raw byte and on malloc with the complement, but chunks served from **tcache bypass the
  malloc-time fill**, so recycled memory comes back `0xff`. A run under `=255` is a mix of `0x00`
  and `0xff`. Differencing two *arbitrary* fills is still sound — that relies only on the two
  differing.
- **valgrind's `write`/`writev` frames name whichever call flushed the 8KB `filebuf`**, not the
  call that put the bad bytes in it. Both contexts here pointed at `lsquare::Save` and the carrier
  was a `character` array serialized further up the same buffer. The *origin* half of a memcheck
  report is sound; the read half is not, for buffered output.
- Which is fixed by **making the stream unbuffered for one diagnostic run** (§6.6e):

  ```cpp
  File.rdbuf()->pubsetbuf(0, 0);   // before open(), in outputfile::outputfile
  ```

  Every `<<` becomes its own syscall and memcheck's stack names the actual writer. Do this first.

`valgrind --track-origins=yes` then reads:

```
Syscall param write(buf) points to uninitialised byte(s)
  ... lsquare::Save → level::Save → dungeon::SaveLevel → game::Save → commandsystem::GoDown
Uninitialised value was created by a heap allocation
  ... sysbase<zombie, humanoid, characterprototype>::Spawn
      → protosystem::BalancedCreateMonster → level::GenerateNewMonsters → game::EnterArea
```

**That is what made §6.4 and §6.6 one bug rather than two.** Monsters generated by
`sysbase::Spawn` carried partially-uninitialized members and the descend-autosave serialized them.

#### 6.4b A value that looks like a pointer is evidence about the chunk, not the writer

The residue left after the fill is held constant was not uninitialized memory and not wall clock.
`setarch -R` (ASLR off) collapsed the level file to one distinct outcome across 4 runs while the
`.sav` kept varying. 18 sites per level, 8 bytes each, every one reading as an x86-64 userspace
heap pointer — and the difference between two runs was **one constant across all 18**, which is
the ASLR slide:

```
0x5dc30cbec2a0 → 0x62ca8e0b02a0    delta 0x507814c4000
0x5dc30cbe9340 → 0x62ca8e0ad340    delta 0x507814c4000
...  18 sites, 1 distinct delta
```

The reasoning was sound to the last step, and the last step was wrong: nobody had saved a pointer.
An uninitialized `ulong` in a chunk that previously held one reads exactly like this, because the
fill reaches only bytes the allocator actually filled (the tcache trap above) and the stale value
moves with the heap base. Initializing `trapdata` (§6.6d) took 4 distinct level files to 1 with
ASLR left on, and `lsquare::Save` never needed touching.

The count is a property of what is on the level, so name the corpus alongside it: these did not
appear on `noncombat` at all, and on `autoplay-200`'s fought-over level 480 bytes differed with
ASLR on against 400 with it off.

### 6.5 Replays diverged once monsters acted — `clock()` was the auto-play AI's RNG

Nineteen decisions in the `AutoPlayAI*` family drew from `clock()` — process CPU time — instead of
the pinned Mersenne Twister. `femath::SetSeed` does not reach `clock()`, so two replays of one
recording gave the AI different decisions, and the run diverged in real game state the moment it
was making any.

That is why the symptom looked like "once monsters act": these functions run only under auto-play
and their branches fire when a fight starts. `AutoPlayAIPray` was gated on
`StateIsActivated(PANIC) && clock()%10==0` and then picked its god with `clock()%iKGTot`;
`AutoPlayAIDropThings` fired on `clock()%100<5` once burdened; `AutoPlayAINavigateDungeon` picked
wander durations and retreat targets the same way.

Sites, all `RAND_N` now: `char.cpp` 2929, 3007, 3018, 3035, 3075, 3331, 3343, 3346, 3361, 3383,
3458, 3497, 3498, 3500, 3502 and `human.cpp` 3714, 3765, 3797. Fourth member of §6.1's RNG family.

After the fix, 200 auto-play turns give 1 distinct outcome in 8 isolated runs (was 5), and 2,800
turns with 36 deaths across two dungeon levels give 1 in 16 concurrent runs on a saturated
machine — identical down to the frame count, the frame hashes, the cumulative RNG count and the
full string stream.

**All three leads recorded before the fix were wrong**, and each cost real time.
`SaveSeed`/`LoadSeed` nesting: disproved, `nest` is 0 across every run of every corpus.
`UpdateTick`: already pinned — `whandler.h:102` makes `Tick` a draw counter during replay and the
standby-animation loop is never reached. `SeedModifier`/visual randomness: 92 bracketed draws out
of 1,748,754, and `LoadSeed` discards them anyway. What found it was the `clock()`/`SDL_GetTicks()`
inventory plus the draw attribution below. Nothing was learned by reasoning about the leads.

#### 6.5a Trace diagnostics, and the eight-run rule

The trace carries three fields beyond `rng`:

```json
{"frame":684,"hash":"f311d7a6648ef42f","rng":1748754,"index":681,
 "grng":1748662,"nest":0,"depth":0}
```

- `grng` — draws made **outside** any `SaveSeed`/`LoadSeed` bracket, i.e. the ones that advance the
  game stream. `rng - grng` is the visual randomness. **Reach for this first**: it turns "the
  traces differ" into "the *game* diverged" versus "only the animation did", in one field.
- `nest` — cumulative count of `SaveSeed` calls entered at depth > 0. **Must stay 0.** Positive
  means the single `mtb` backup slot is silently corrupting the game stream.
- `depth` — bracket depth at the frame, which catches an unbalanced bracket.

**Two rules for measuring determinism here, both learned by getting them wrong.**

*Give every run its own directory.* A portable build writes `Save/`, `SndDebug.txt` and
`.QuestionHistory_*.txt` into the launch directory, and wizard-mode activation calls `game::Save()`
— runs sharing a directory contaminate each other. That artifact alone produced a spurious
440-frame outcome cluster.

*Use at least 8 runs, and count distinct outcomes rather than comparing a pair.* This divergence
was flaky enough that two runs frequently agreed by chance. Two-sample comparisons produced two
confidently wrong diagnoses during the investigation — first "uninitialized heap, proven by
`MALLOC_PERTURB_`", then "gameplay is actually deterministic" — both of which evaporated at 6–8
samples. `run-corpus.sh` defaults to 8 for this reason, and pins the harness options besides,
because §6.6 records that changing them changes the allocation history.

Other wall-clock reads, checked and cleared for these corpora but **not safe in general**:
`hiteffect.cpp:70`/`:419`/`:249-252`, `item.cpp:168`, and ~20 animation sites in `game.cpp`
(2131–2912). A probe logging every clock read showed none of them is reached in a headless replay;
the only live site is `game.cpp:4219`, whose decision at `:4199` is already replay-guarded. They
will bite as soon as a corpus enables the alt-silhouette or throws an item, and they reach pixels,
so they break frame hashing rather than game state.

### 6.6 Game logic reads uninitialized memory — closed for every path the corpora reach

Once §6.5 was fixed a second, weaker source became visible. It does not show up by running the same
command twice; it shows up when you **change what else the process allocates**. `--trace` alone
against `--trace --text` gave two outcomes, one per option set and each self-consistent, and a
fixed `MALLOC_PERTURB_` collapsed them to one. A fixed fill collapsing the difference is the proof:
the divergent input is the *content* of uninitialized heap, and `--text` shifts the allocation
history that decides it. Each option set being internally reproducible is why 16 concurrent runs of
one command line agreed perfectly and this stayed hidden.

**That is the durable part — varying the harness options is a cheap proxy for varying the
allocation history**, and it found something that twice-running a command never would. The
option-set sensitivity itself closed with §6.6a.

valgrind over the corpora named the reads and their origins. The important addition was
`write`/`writev` — the same defect reaching a *file* rather than a branch, which is what makes §6.4
and §6.6 one bug. Origins resolved to `humanoid::MakeBodyPart`/`playerkind::MakeBodyPart` rather
than the `sysbase::Spawn` first recorded, which said the uninitialized members were on the
**bodypart** objects a character builds and not only on the character.

**`graphicid` was fixed first.** `graphicid() = default` left the struct uninitialized while
`operator<` memcmps *every byte* of it — it is the `std::map<graphicid, tile>` key — and the
serializer writes every byte with a raw `Write`. Padding was a live input to both the graphics
cache ordering and the save content. Now zeroed in the constructor. Same shape as
`fastscriptmember`: **grep for `= default` on any struct that gets memcmp'd or raw-written.**

And there was padding to hit because of a build bug, §7.7: `NO_ALIGNMENT` expanded to
`__attribute__((packed))` only when `GCC` was defined, and `add_definitions(-DGCC)` sat in
`FeLib/CMakeLists.txt:18` — a *sibling* directory to `Main/`, so it never reached it.

```
Main's flags   (no -DGCC):  sizeof(graphicid)=48  alignof=4   <- one tail padding byte
FeLib's flags  (-DGCC):     sizeof(graphicid)=47  alignof=1
```

One struct, two layouts, one binary — an ODR violation, with `igraph.cpp:364` writing
`sizeof(Value)` bytes from a `Main` translation unit.

**`new type()` is not a shortcut for the rest of this family.** It worked for `database`, which has
no user-provided default constructor; `bodypart`, `item` and `character` all do, so
value-initialization just calls the constructor and changes nothing. `memset` is out — these are
polymorphic. It has to be per-member initialization, class by class, and the mechanism that reaches
every constructor including ones that do not exist is an **in-class initializer on the
declaration**. That is what §6.6a–e all use.

#### 6.6a The bodypart family

`bodypart() : Master(0) { }` initialized one member of ten; nine of the rest are written straight
to the level file by `bodypart::Save`. `head`, `arm` and `leg` are worse — `arm` and `leg` have no
default constructor at all, only `rightarm()` etc., which `Init` their gear slots, so their saved
scalars were indeterminate too. Fixed in `Main/Include/bodypart.h`.

| | before | after |
|---|---|---|
| valgrind errors / contexts, noncombat | 22,632 / 57 | **137 / 13** |
| `bodypart::UpdateFlags` + `arm`/`leg`/`head::SignalPossibleUsabilityChange` | 44 contexts | **0** |
| player HP under `MALLOC_PERTURB_` 255 / 42 / 99 | 35/37, 23/36, 29/37 | **identical in all three** |
| uninitialized bytes in the level file | 1,860 | 1,787 |

**The HP row is the result that matters**: character stats used to depend on which garbage the
allocator returned. The save-leak row is the one that did not move — bodypart was not §6.4a's
carrier, 73 bytes of 1,860.

**Why zero, per field** — the argument is not uniform and it is worth not pretending it is.
`HP`/`MaxHP` is provable: `CalculateMaxHP` opens with `HPDelta = MaxHP - HP`, the damage already
taken, zero for a fresh part, and zeroed the first call yields `HP == MaxHP`, which is literally
what `RestoreHP()` assigns. `BloodMaterial`, `NormalMaterial` and `BodyPartVolume` are
unobservable — `character::CreateBodyPart` assigns all three unconditionally (`char.cpp:5845`)
before any read or save. The six picture fields are **only a defined placeholder, not a correct
sprite**: `UpdateBodyPartPicture` assigns them and `NO_PIC_UPDATE` skips it (`proto.cpp:661`,
`script.cpp:412`), so a part drawn without a picture update was already wrong and this makes it
wrong repeatably rather than randomly. Do not read the fix as having corrected that.

#### 6.6b `character`'s owned pointers and `BodyParts`

`~character()` frees `Action`, `PolymorphBackup`, `SquareUnder`, `BodyPartSlot`,
`OriginalBodyPartID` and `CWeaponSkill`. The default constructor nulled the first three and missed
the last three, so nulling them completes an existing pattern.

`BodyParts` is the interesting one, because the codebase names a competing default:
`virtual void CalculateBodyParts() { BodyParts = 1; }` (`char.h:809`). The two answer different
questions — the virtual says how many parts the *species* has and runs in `Initialize` immediately
before `BodyPartSlot = new bodypartslot[BodyParts]`, while the constructor's value has to say how
many slots *exist*, and before `Initialize` none do. It matters because the destructor does
`for(c = 0; c < BodyParts; ++c) delete GetBodyPart(c);`:

| `BodyParts` | `BodyPartSlot` | destroying an un-Initialized character |
|---|---|---|
| garbage | garbage | unbounded loop over a wild pointer — the old behaviour |
| 1 | null | dereferences null |
| **0** | **null** | no-op loop, then a well-defined `delete[]` on null |

Nothing observable changed — `Initialize` always assigns `BodyParts` before anything reads it. It
makes the destructor's assumption enforced rather than assumed, the protection `Action` and
`SquareUnder` already had.

#### 6.6c `character` and `playerkind`'s saved and read members

In-class initializers in `Main/Include/char.h` and `Main/Include/human.h`. Closes §6.4a outright.

| | before | after |
|---|---|---|
| valgrind errors / contexts, noncombat | 137 / 13 | **0 / 0** |
| uninitialized bytes, noncombat level file | 1,860 | **0** |
| uninitialized bytes, autoplay descended-from level | 1,860 | **0** |
| uninitialized bytes, autoplay fought-over level, ASLR off | 1,616 | **400** — §6.6d's family |
| trace and full string stream, both corpora | — | **byte-identical to before the change** |

**Nothing observable changed, and that is the expected result** rather than a disappointment. What
went away is undefined behaviour and 1,860 bytes of heap per level file.

**Four defects, and three were found by measuring rather than by reading the constructor.**

- **`TemporaryStateCounter[STATES]` — the entire save leak.** `Initialize` assigns a counter only
  where the matching state bit is set; `character::Save` writes all 32 regardless. Attribution and
  layout evidence in §6.4a. Zero is unobservable: every in-memory read is behind a check on the
  state's own bit, so the serializer was the only consumer that did not check.
- **`CarriedWeight`** — `CalculateAll` runs `CalculateAttributeBonuses` before
  `CalculateVolumeAndWeight`, and `leg::CalculateAttributeBonuses` reaches `CalculateBurdenState`,
  so the first burden calculation of every character's life read the weight before the function
  that sets it had run.
- **`AttributeBonus[]` and `CarryingBonus`** — `Initialize` calls `UpdatePictures()` before
  `CalculateAll()`, and `GetAttribute` returns `BaseExperience` plus `AttributeBonus`. The lookups
  choosing the player's head and arm sprites by attribute read it first. **This one reached
  pixels.**
- **`playerkind::Talent` and `Weakness`** — 8 of 13 contexts, the largest group. `Initialize` runs
  `CreateBodyParts` before `PostConstruct`, and `CreateBodyParts` reaches `GetNaturalExperience`
  via `InitSpecialAttributes`, which compares both against `TalentOfAttribute` to decide whether to
  scale a limb's natural experience. Every arm and leg the player was built with had its attributes
  settled by indeterminate memory.

**All four are ordering bugs, and initializing the member does not fix the ordering.** It makes the
early read return a defined value; whether the read should happen at all is left open deliberately.
`CarriedWeight` and `AttributeBonus` self-correct, because `CalculateAll` recomputes both a few
lines later and `IsInitializing()` suppresses the side effect in between. `Talent` and `Weakness`
do not — the limb attributes persist.

**Why zero, per field**, and two where the obvious answer is wrong:

- **`BurdenState` is not zeroed.** `OVER_LOADED` is 0 and `UNBURDENED` is 3, so zeroing it would
  name the *worst* burden state while looking like a neutral default. It gets `UNBURDENED`, which
  is also what `CalculateBurdenState` computes from the zeroed `CarriedWeight`.
- **`Talent`/`Weakness` zero is provable, not a placeholder.** `ivandef.h` says
  `/* 0 reserved for no talent */` and the four real talents start at 1, so a zeroed pair matches
  neither branch and the limb gets its unscaled natural experience — which is what the ordering
  already produced almost always, since indeterminate memory rarely equals one of four small
  values. This makes it the answer every time rather than most of the time.
- `AttributeBonus[]` is provable: `CalculateAttributeBonuses` opens by setting every entry to 0 and
  adding each equipped item's enchantment, which is exactly what it computes for a character with
  nothing equipped.
- `SquaresUnder` gets 0 for §6.6b's reason — the count has to agree with the array.
- The rest (`ID`, `Stamina`, `MaxStamina`, `MyVomitMaterial`, `CommandFlags`, `BaseExperience[]`,
  `Volume`, `Weight`, `BodyVolume`, `HP`, `MaxHP`, `DodgeValue`) are unobservable: each is assigned
  unconditionally before any read or save, on both the spawn and the load path. They are in for
  §6.6b's reason — a member that reaches a file should not depend on that staying true.

#### 6.6d The fluid family, and the pointers that were not pointers

In-class initializers in `Main/Include/fluid.h` and `Main/Include/trap.h`.

| autoplay-200, 210 keys, seed 999 | before | after |
|---|---|---|
| level-file bytes leaked, fill 42 vs 99, ASLR off | 400 in 59 regions | **0** |
| valgrind errors / contexts | 283 / 3 | **0 / 0** |
| distinct level files, 8 isolated ordinary runs, ASLR **on** | 8 | **1** |
| distinct level files, 4 runs at a fixed fill, ASLR **on** (§6.4b's test) | 4 | **1** |
| trace, string stream, frame count, RNG count | — | **byte-identical** |

**The ordinary-run row is the one that matters.** Every previous section in this series needed a
fixed heap fill or ASLR disabled before the level file would reproduce at all; this is the first
measurement taken with neither.

**The region histogram picked the fields**, the way §6.4a's offset layout did. The 400 bytes were
26 regions of 4, 29 of 8 and 4 of 16 — and 26×4 + 29×8 + 4×16 = 400 with nothing left over. Four
bytes is `imagedata::SpecialFlags`, an `int`. Eight and sixteen are `ulong`s, which `imagedata` has
none of. That said the family was wrong before any code changed.

**`trapdata` was 296 of the 400.** `trapdata() : Next(0) { }` (`trap.h:34`) initializes one member
of four and neither `fluid` constructor finishes the job: the ground one (`fluid.cpp:26`) assigns
`TrapID` and `VictimID` and leaves `BodyParts`; the item one (`fluid.cpp:38`) assigns only `TrapID`
and leaves `VictimID` and `BodyParts` adjacent. `fluid::Save` opens with `SaveFile << TrapData` and
`operator<<` writes all three unconditionally (`trap.cpp:50`) — so a ground fluid leaks 8 bytes and
an item fluid 16, which is the histogram.

**And a live read nobody had named:** `fluid::IsStuckTo` (`fluid.cpp:778`) compares
`TrapData.VictimID` directly, and for fluids on items and bodyparts nothing ever assigns it — only
`StepOnEffect` does (`:753`), the ground path. `bodypart.cpp:3250` and `:3291` turn that comparison
into `IGNORE_TRAPS` on a death check. Garbage rarely equals a live character ID, so it read false
almost always, which is why nothing visible broke — the same shape as §6.6c's `Talent`/`Weakness`.

**Two fields that looked uninitialized and were not**, both read off a mem-init list without
checking the member's own type. `DripPos` — `v2::v2()` is `: X(0), Y(0)` (`v2.h:41`), so
default-constructing the member zeroes it. And `SpecialFlags` reaches less than expected:
`AddLiquidToPicture` computes `ValidityMap` at `fluid.cpp:543` unconditionally but dereferences it
only inside `if(Shadow)` (`:551`) and behind `!Shadow ||` (`:592`), and the blood path passes
`Shadow == 0` (`:88`). Both valgrind contexts were the pointer load in
`GetBodyBitmapValidityMap` itself — the map is computed from garbage and then not read.

**Why zero, per field** — here every one is provable rather than a placeholder, the first time in
this series that has been true. `SpecialFlags` is `ST_NORMAL`, and `ST_NORMAL` is 0
(`ivandef.h:265`); `fluid::Draw:195` already computes `MotherItem ? MotherItem->GetSpecialFlags() :
0` for this same object, and `BodyBitmapValidityMap[0]` is the one `CreateBodyBitmapValidityMaps`
fills with `memset(…, 0xFF, …)` (`igraph.cpp:444`) — every pixel valid, which is what a pool with
no bodypart shape needs. `AlphaAverage` 0 is what `bitmap::Fade` computes for a picture with no
non-transparent pixels (`bitmap.cpp:1093`) and pairs with the constructor's `AlphaSum(0)`; note
`Fade()` is not a read of it, it takes the field by non-const reference and writes it, and the
reachable read is `fluid.cpp:82`/`:84` when `AddLiquidToPicture` returns early at `:555`.
`DripColor`/`DripAlpha` are unobservable — `DripTimer(0)` forces `Animate`'s assignment branch
(`fluid.cpp:468`) before any read at `:490`. `TrapData`'s three each match what some other path
already assigns for "none": `TrapID = 0` is what the item constructor assigns and what
`PreProcessForBone` uses (`fluid.cpp:768`), `VictimID = 0` is what `UnStick()` and `Destroy()`
reset it to, `BodyParts = 0` is "stuck to nothing".

#### 6.6e Two more fields that reach a level file

Found because §9.4 changed what the corpora generate: a different dungeon reaches different
classes, and two ordinary replays started producing four distinct level files again out of four
runs — with trace, text log and screenshot still byte-identical, the §6.4 shape exactly.

| noncombat | before | after |
|---|---|---|
| level-file bytes leaked, fill 42 vs 99 | 4 in 1 region | **0** |
| distinct level files, 4 ordinary runs | 4 | **1** |
| valgrind errors / contexts | 1 / 1 | **0 / 0** |

- **`itemtrapbase::Team`** (`trap.h`). `itemtrapbase() : Active(false) { }` initializes one member
  of three and `Save` writes `Team` unconditionally, so any level holding an unarmed item trap — a
  mine, in this corpus — carried four bytes of heap. Now `NO_TEAM`, which is what
  `TeleportRandomly` resets it to and what an unowned trap means. Nothing reads it while `Active`
  is false: `CanBeSeenBy` short-circuits on `!Active`.
- **`earth::PictureIndex`** (`lterras.h`). `PostConstruct` picks one of four earth tiles with
  `RAND() & 3`, but does not run on every path that creates an `earth`, and `Save` writes the field
  on all of them. Now 0, one of the four values `PostConstruct` itself chooses.

This is where the unbuffered-stream trick in §6.4a comes from: memcheck confidently named
`earth::Save`, the earth field was zeroed, and the leak did not move. `pubsetbuf(0, 0)` named
`itemtrapbase::Save` immediately.

### 6.7 The host libm is no longer an input

`sin`, `cos`, `atan`, `log10` and `pow` are not correctly rounded by any standard, so every libm is
entitled to its own answer in the last ulp — and IVAN feeds those answers through `int()` and
`(short)` truncation. Game code calls `portmath::` now, musl vendored at a pinned commit and used
by native and WASM alike, so both platforms run the same algorithm. Full notes in
`portmath/README.md`.

**The symbol is `sincos`, not `sin`/`cos`.** GCC rewrites `sin(x)` and `cos(x)` on the same
argument into a single `sincos(x)`, and glibc's `sincos` is not obliged to agree with its own `sin`
and `cos` — so whether two builds match can depend on whether the compiler chose to fuse, with no
visible call site either way. The first inventory taken here interposed `sin` and `cos` and
reported **zero calls** from a binary making 18,368 of them. Anything that greps for `sin(` misses
this.

**World generation is where the two libms agree**, which is not where the risk was assumed to be.
Replaying every recorded call through musl and comparing bit-for-bit against glibc:

| site | function | calls | differ |
|---|---|---|---|
| `bitmap::DrawPolygon` | `sincos` | 3,600 | 216 |
| `worldmap::Generate` (Poisson sampler) | `sincos` | 3,400 | 132 |
| `femath::NormalDistributedRand` | `sincos` | 1,764 | 106 |
| `character::GetAdjustedStaminaCost` | `log10` | 73 | 18 |
| `character::CheckForBlockWithArm` | `log10` | 1 | 1 |
| `worldmap::PeriodicSimplexNoiseAltitude` | `sincos` | 9,604 | **0** |
| every `log` site | `log` | 8,967 | **0** |

473 of 27,496, all by exactly one ulp. World gen's arguments are `x/XSize * 2π` for integer `x` and
both implementations round those identically; the disagreements are elsewhere, including 106 in the
Box-Muller transform that decides which way monsters wander.

**Nothing observable changed, and that is luck rather than structure.** Traces, text logs,
screenshots, `.wm` and level files were byte-identical to the previous binary — checked against
saves kept from it, not only against the goldens. So 473 calls in live game logic returned a
different number and no state moved, because none of them truncated across a boundary in these
runs. That holds for this seed and these corpora and nothing more, which is the argument *for*
pinning: without it the first symptom would be an unexplained frame-hash divergence in a WASM
build, thousands of frames from the cause.

**The same musl source under two compilers is a different claim, and it holds too.** A probe
linking `portmath/src/*.c` with `pm_log`, `pm_sincos`, `pm_log10`, `pm_atan` and `pm_pow` over
2,000 arguments each — the shapes `NormalDistributedRand` and world gen actually produce — gives
**bit-identical output** from `gcc -O2` and from `emcc -O2`, 14,000 evaluations, same md5. The
`-ffp-contract=off` and `-fno-builtin` on the portmath target earn that, and `__FP_FAST_FMA` is
undefined on both targets so the `log`/`pow` fast paths agree as well. This mattered during §9.4:
being able to *eliminate* the math in one cheap measurement was worth more than another guess.

**The instrument was `LD_PRELOAD`** — interposing libm and recording
`__builtin_return_address(0)` gives every call with its caller, which is how the table was built.
`dladdr` turns the return address into a module offset `addr2line` resolves. A source grep would
have found the wrong sites and missed `sincos` entirely.

### 6.8 `time_t` would not have compiled under Emscripten

`game.cpp:3594` does `SaveFile >> TimePlayedBeforeLastLoad` on a `time_t`. It compiled only because
`time_t` *is* `long` on x86-64, so it bound that overload by accident. Under Emscripten, where musl
uses a 64-bit `time_t` on 32-bit targets, `time_t` is `long long`, nothing binds, and the build
fails — confirmed by compiling the real header against a `long long`. Three sites: `game.cpp:3496`,
`:3594`, and `hscore.h`'s `std::vector<time_t>`, whose element operator the container serializer
instantiates.

Fixed by fixing the width, because `time_t` was the symptom: `long` is 8 bytes here and 4 under
Emscripten, and **every container writes its length as a `ulong`**, so at native width a WASM save
diverges at the first container and never resynchronises. Both go out as 8 explicit little-endian
bytes now, the idiom `short`/`ushort` already used. `long long` gets its own overload — C++ keeps it
distinct from `long` even at equal width — and that is what makes `time_t` bind on both targets.

**The save format did not change and `SAVE_FILE_VERSION` did not move.** On a little-endian host of
the same width, explicit bytes are what the raw write already produced. Verified: both corpora gave
`.wm`, level and `.sav` files identical to saves kept from before the change, the single exception
being one byte of autoplay's `.sav` — the `GetTimeSpent` second-boundary flake.

This replaced the `SAVE_COMPATIBILITY` block, written for this same problem on mingw and dead code:
the macro is defined nowhere, so its `#if` was always false.

### 6.9 A strict aliasing violation decided whether a corpse had a square

Reported from a browser session as `memory access out of bounds` in
`stackslot::SignalVolumeAndWeightChange`, under `stack::AddItem` ← `lsquare::AddItem` ←
`character::CreateCorpse` ← `dog::CreateCorpse` ← `character::Die`. A pet puppy was kicked to death
by a banana grower in New Attnam. **This was the crash §9.6 had reported and could not reproduce,
and the same bug as §9.4's open auto-play divergence.**

`character::Die` opened with

```cpp
square* SquareUnder[MAX_SQUARES_UNDER];
lsquare** LSquareUnder = reinterpret_cast<lsquare**>(SquareUnder);
memset(SquareUnder, 0, sizeof(SquareUnder));
```

then wrote the array through `SquareUnder[c] = GetSquareUnder(c)` and read it back through
`LSquareUnder[0]` at all seven use sites. Writing an object through one pointer type and reading it
through another is a strict aliasing violation, so the compiler may assume the stores cannot affect
the loads. **Clang does, and folds every read back to the `memset`'s zero.** `CreateCorpse` was
handed a null `lsquare*`, `lsquare::AddItem` loaded `Stack` through it, and the release browser
build trapped several dereferences downstream — which is why the reported frame is not the faulting
line. GCC does not exploit it, so the whole thing was invisible natively.

Fixed by declaring the array as what every read of it already was, `lsquare*`, filled with the
existing `GetLSquareUnder(c)` accessor (`char.h:781`) — the same cast written once and legally.
Every dereference was already guarded by `!game::IsInWilderness()`.

**The measurement that identified it**, since the trap location did not. A `printf` of the same slot
through both names, in the same function:

```
DIE-DIAG after Remove()      local0=0x206d3d0     <- read as square*
DIE-DIAG before SignalDeath  local0=0             <- read as lsquare*, same address
```

Two reads of one address giving two values is aliasing or a miscompile and nothing else. Note that
adding `&SquareUnder[0]` to the same `printf` **made the bug disappear** — taking the address
forces the array to memory and defeats the assumption. A Heisenbug that evaporates when you take an
address is this class's signature; do not conclude from it that the previous run was wrong.
Confirmed twice over on one build differing only in that flag: `-fno-strict-aliasing` clears the
fault, and the fixed source with strict aliasing on reaches the identical RNG count (2,303,482)
that `-fno-strict-aliasing` reached on the broken source.

**Three things worth taking from it.**

*A `reinterpret_cast` between two object pointer types is a portability defect, not a style one.* It
is the only instance of this exact pattern in the tree — `reinterpret_cast<T**>` over `Main/` and
`FeLib/` finds one other family, `allocate.h`, which only ever reads back through the type it wrote.
The `reinterpret_cast<ushort&>(x)` idiom over an `int` in the save readers — `stack.cpp:201`,
`proto.h:117`, `script.cpp:274`, `cmdswapweap.cpp:114` — is the same class, writes only half of each
target, and is unaudited.

*A WASM-only crash was a real defect, exactly as §9.6 predicted.* Not an Emscripten problem: a
latent bug x86-64 absorbs because the wild pointer still lands in a mapped page.

*The second host paid for itself.* This bug was reachable from the committed auto-play corpus and
had already been measured as a native-vs-WASM divergence in §9.4, where it sat behind a list of
correctly-ruled-out causes. What the browser added was a *symptom* — a stack with `character::Die`
in it — and that was the whole difference between an open item and a one-line fix.

---

## 7. Open items, and one closed one

§6.4 and §6.6 are closed for every path the corpora reach. §7.1 (test savediff against real saves),
§7.2 (validate the auto-play AI as a corpus — `tools/play/README.md` has what it takes to drive it)
and §7.3 (wire it into CI) are done and retired, as are §7.6a, §7.6b and §7.6c, whose findings are
recorded in §6.6b–d where they were fixed. §7.7 is done too and is kept below, because three build
files point at it for the lesson rather than the fix.

### 7.4 PNG dump on trace mismatch — half done

The writer exists (`--shot`, `--shot-dir`, `harness::WriteShot`) and produces a real PNG rather than
the mislabelled BMP `bitmap::Save` writes. Missing is the *trigger*: comparing a trace against a
golden during the run and dumping both frames plus a diff image at the first mismatch, with ~30
frames of preceding context. The pieces are `WriteShot` plus a golden-trace reader.

### 7.5 The state-digest layer

Frame hashing validates the port while C++ renders; it cannot validate the rewrite, and neither can
savediff for anything that never reaches a file. A periodic structured digest — player stats,
position, inventory IDs, level layout hash, RNG count — crossing the C API is what validates both.
§6.4 is the class of bug it exists for: a divergence with no frame-hash divergence at all.

### 7.6 The `.wm` uninitialized-heap residue

§6.2. Same family as §6.6, and `area::Save`'s `FlagMap` is *not* an instance — `area::area()`
memsets it. Re-derive the 358-byte inventory rather than trusting it.

### 7.7 What a directory-scoped flag cost — closed, and cited from the build files

`NO_ALIGNMENT` expanded to `__attribute__((packed))` only when `GCC` was defined, and
`add_definitions(-DGCC)` sat in `FeLib/CMakeLists.txt:18` — a *sibling* directory to `Main/`, so it
never reached it, and `graphicid` was 47 bytes in FeLib and 48 in Main. **One struct, two layouts,
one binary.** Resolved by testing `__GNUC__` in the header — the TODO already sitting on that
CMakeLists line — and then *removing* the packing rather than propagating it. This is why the
Emscripten port flags are global (`add_compile_options` at the top level, §9.3) and why
`portmath/CMakeLists.txt` says what it says.

**The bug was the disagreement, not the layout**, so either direction would have fixed it. That made
the layout a free choice, decided on other evidence: unpacking introduced **0** new warnings against
301 for packing everywhere. Packing removes the compiler's alignment guarantee for *every* member,
and `igraph.cpp:247` hands `GI.Color` — a `ushort[4]` member — to `Colorize()`, where it decays to a
`ushort*` carrying an alignment promise a packed struct does not make. That is UB: free on x86, a
fault or a slow path on ARM, and variable across WASM engines, which is the destination. One byte per
cached tile is not worth it.

**Why they were packed originally, which is not what it looks like.** The removed comment above the
pragma said it outright: `/* memcmp doesn't like alignment of structure members */`. It was never a
size optimisation. `operator<` is a `memcmp` over the whole object, so padding bytes are compared;
padding is indeterminate, so two objects with identical members can compare unequal, which breaks the
strict weak ordering `std::map` requires. `git log -S NO_ALIGNMENT` bottoms out at a commit
"modified by nukes to support 64-bit systems" — it arrived during a 32→64-bit port, exactly when
padding layouts shift.

**So packing and the zeroing constructors solve the same problem** — making every byte defined — and
only the constructors keep the alignment guarantee. Unpacking is safe *because* `graphicid` got its
`memset` and `configid` a zero-init constructor. Removing either silently reintroduces the original
bug: **do not delete that `memset` on the grounds that every member is assigned, because no member
owns the padding.**

Two other things this turned up. `configid`'s packing was always a no-op — two `int`s have no padding
on any platform, and it was applied by pattern-match from `graphicid` during that same 64-bit port.
And `#pragma pack(1)` under `#ifdef VC` wrapped both structs; `VC` is defined nowhere (MSVC sets
`_MSC_VER`), so it was dead — but it is the identical hand-maintained-flag pattern, and had anyone
defined it, `graphicid` would have been 47 on MSVC against 48 everywhere else.

`NO_ALIGNMENT` is gone. `HARDWARE_LAYOUT` replaces it and applies only to `graphics.h`'s
`vesainfo`/`modeinfo`, which are VESA BIOS blocks filled by a real-mode interrupt at spec-defined
offsets — there packing is mandatory, because the layout is defined outside this program. Naming the
macro for that one real use is what stopped "unpack it" from looking like it would break the DOS
build.

**Cost:** `SAVE_FILE_VERSION` 136→137 and `BONE_FILE_VERSION` 120→121 (bone files carry a whole
`level`, so they contain `graphicid` too). And the warning baseline moved, because `LIKE_PRINTF` was
gated on the same broken macro and printf format checking switched on in `Main` for the first time —
§7.8. There is no way to fix the packing via `__GNUC__` and keep those hidden: doing so needs a
directory-scoped flag, which is the mechanism that caused this bug.

### 7.8 The format bugs `LIKE_PRINTF` exposed

49 warnings, all in `Main`, all pre-existing and invisible until §7.7 turned format checking on
there for the first time: 25 `-Wformat=`, 20 `-Wformat-security`, 4 `-Wformat-extra-args`. `FeLib`
has been format-checked all along and is clean.

- `gods.cpp:316` — `ADD_MESSAGE("You feel the music resonate within you.", GetName())`: a format
  with no conversion specifier and an argument passed anyway.
- `human.cpp:1055` and `:1079` — two specifiers, three arguments; the god's pronoun is dropped.
- The 20 `-Wformat-security` are the `ADD_MESSAGE(SomeString)` pattern, where the format string is
  not a literal. **That is a live crash risk rather than a style nit**: the player types their own
  character name and it reaches messages, so a name containing `%s` is read as a conversion.

`-Wno-format-security` covers only 20 of the 49 — measured with the flags themselves rather than
inferred. Blanket `-Werror` is fragile against a compiler bump: at GCC 13.3 the pre-existing
`-Wstringop-overflow=` warnings become errors too and `FeLib` fails before `Main` is reached.

### 7.9 Make the save format host-independent

**Half the format is already portable.** `truth`/`char`/`uchar` go out via `Put()` as one byte;
`short`/`ushort` are written explicitly little-endian (`save.h:218`); `festring` and `cchar*` are a
`ushort` length plus raw bytes; entity type IDs and flags are cast to `ushort`. None of it cares
what host wrote it. `RAW_SAVE_LOAD` is `Write(&Value, sizeof(Value))`, and of what goes through it
only `long`/`ulong` differed (8 vs 4) — closed by §6.8. `int`, `uint`, `double`, `v2`, `rect`,
`packv2`, `expid` and `configid` are all the same width on both targets and little-endian on both.

What is left:

1. **Containers** — the width question is settled; open is whether 8 bytes per container length is
   worth it. `uint` would halve it and is equally portable.
2. **Raw struct writes.** `graphicid` (`igraph.cpp:364`) and `configid` (`game.cpp:5393`) are
   written with `sizeof`. §7.7 made their layouts flag-independent, which is necessary and not
   sufficient — they still carry host layout, and they agree today only because both targets happen
   to lay them out the same way. Converting them to field-by-field, as `dangerid`, `killreason` and
   `massacreid` already are, removes the question permanently.
3. **Truncation.** On wasm32 `long` is 4 bytes, so a saved value above 32 bits narrows on load.
   Nothing in the tree is known to store one; it has not been audited.
4. **savediff** decoders move to the new offsets, and `--word-size` becomes a legacy-save flag
   rather than a required guess.

`SAVE_FILE_VERSION` exists (`game.cpp:76`, written at `:3454`, checked at `:3519`) and the stamp is
an `int`, 4 bytes on both platforms, so a WASM build can read a native save's version and reject it
cleanly rather than mis-decoding it. Bundle this into the next version bump. The harness can
validate it: replay a corpus, save, load, re-save and compare on one platform to prove the format
round-trips, then native-vs-native savediff to prove semantics did not move.

---

## 8. What is worth offering upstream

Nothing has been offered. `origin` is `Attnam/ivan`, untouched; the fork is the remote named `fork`.

**The uninitialized-member commits and the compiler-independence commit are the ones that matter to
anyone who is not porting.** The second is eleven defects that have been in the game for years and
that nobody could have found without compiling it twice: a font blit whose width was
`20 / sizeof(ulong)`, a dozen expressions letting the compiler choose which random draw went where,
and six comparators letting the standard library choose how to break a tie (§9.4). They change what
a given seed generates, which is what makes them awkward to offer — the fix is not a no-op for
anyone's saved game.

Independently upstreamable, depending on nothing the harness adds: §6.1's four pre-existing bug
fixes (the MIDI one fixes a launch failure on any machine without an ALSA sequencer, harness or no
harness), the uninitialized-member work of §6.6a–e, §6.7's vendored libm, §6.8's explicit save
widths, §9.2's PCRE removal and CMake 4 fix, and §9.11. §6.8 costs upstream nothing, since the save
format does not move on any platform that currently builds.

A clean native build is exit 0 with **128 warnings**: 79 pre-existing `-Wstringop-overflow=` plus
§7.8's 49 format warnings. Expect the `-Wstringop-overflow=` count to wobble whenever a widely
included header changes — all of them are the same line, `festring.h:172` (`++REFS(Data)` in the
copy constructor), reported once per inlining context, so adding initializers to `char.h` moves it.
Nothing is being fixed or hidden when it does.

---

## 9. The port

### 9.1 Native scaling — not started

The window is created without `SDL_WINDOW_RESIZABLE` (`graphics.cpp:207`), `GraphicsScale` is
integer-only, and `SetMode` sets both `SDL_RenderSetLogicalSize` and `SDL_RenderSetScale`, which
manage the same state. Scaling pixels bigger is *not* the same as showing more dungeon — see the
comment at `game.cpp:287` ("no way to fit as scaler is integer and not float"). SDL3's
`SDL_SetRenderLogicalPresentation` has explicit LETTERBOX/STRETCH/INTEGER_SCALE modes and is worth
considering. The SDL surface is only ~1,800 lines.

### 9.2 PCRE dropped, and CMake 4

PCRE was used in three files: `Main/Source/message.cpp` (a dead include), `Main/Source/game.cpp`
(the auto-pickup pattern) and `FeLib/Source/sfx.cpp` (the sound-effect triggers). No call site used
capture groups — every `pcre_exec` passed a NULL output vector and tested `>= 0` — so each became a
`std::regex_search` over the same `(ptr, GetSize())` range under the ECMAScript grammar.
`pcre_study` has no equivalent and was dropped; `std::regex` compiles once at config load either
way.

**The grammar change was measured, not assumed**, because the patterns are *data*: 153 shipped in
`Sound/SoundEffects.cfg` plus the default `AutoPickUpMatching`, several using `(?:` and negative
lookahead `(?!`, both of which ECMAScript has (lookbehind, which it does not, is unused).
`tools/regexdiff/` compiles all 154 under both engines and compares their verdicts on every
distinct string the corpora draw: **111,342 comparisons, 0 mismatches, 0 compile failures**.

```bash
g++ -std=c++11 -O1 -o regexdiff tools/regexdiff/regexdiff.cpp -lpcre   # needs libpcre3-dev
./regexdiff Sound/SoundEffects.cfg tools/corpora/*.text.log
```

It is deliberately not in CMake — it needs the dependency this removed, the same way savediff is
`EXCLUDE_FROM_ALL` so the oracle cannot be broken by the thing it judges. Rerun it if
`SoundEffects.cfg` or the `AutoPickUpMatching` default changes; it is the only thing standing
between a config-file regex and a silent behaviour change. Note the corpora do **not** exercise
either regex path — headless runs make no sound, and the default auto-pickup pattern is disabled
with a leading `!` — so `verify-corpora.sh` passing is evidence of no regression elsewhere, not
evidence the swap works.

**CMake 4's premise was wrong and the real error was elsewhere.** "CMake 4.x refuses `VERSION 3.5`"
is not true: 4.0.3 configures and builds the tree unchanged, emitting only a deprecation warning.
What *did* hard-error, found by running 4.0.3 rather than reasoning about it, was
`IGNORE_EXTRA_WHITESPACES=ON` running `cmake_policy(SET CMP0004 OLD)` — **CMake 4 removed that OLD
behaviour outright**, so setting it is an error rather than a warning. The option defaults to OFF,
which is why an ordinary build looked fine. `cmake_minimum_required` is `VERSION 3.10...4.0` now:
the floor clears the deprecation, the ceiling declares the project tested against 4.0. Verified as a
matrix — {3.28.3, 4.0.3} × {whitespace option OFF, ON} all configure with zero warnings and zero
errors, against a baseline where two of the four cells failed. The raised floor moves policy
defaults from 3.5 to the running version, so the build was re-verified rather than assumed: a
CMake 4-built binary replays both corpora 8/8 against the goldens.

### 9.3 The Emscripten build, and the headless path

`emcmake cmake` configures with zero warnings and zero errors and the build exits 0. SDL2,
SDL2_mixer and libpng come from Emscripten's ports, so the three `find_package(SDL2 REQUIRED)` calls
and FeLib's libpng lookup are wrapped in `if(NOT EMSCRIPTEN)` with their result variables left
empty — everything downstream interpolates them into `target_link_libraries`, where an empty
variable expands to nothing, so no other line had to move.

**The port flags are global, and that is §7.7's lesson applied rather than ignored.** `-sUSE_SDL=2`
is a compile flag as much as a link flag — it is what puts the port's headers on the include path —
so a target that misses it cannot find `SDL.h`, and a target that gets a different set from its
neighbours is exactly how `-DGCC` gave `graphicid` two layouts in one binary. Hence
`add_compile_options` at the top level, not `add_definitions` per directory.

**Three hazards this was braced for do not exist.** RtMidi needs no stubbing — it auto-defines
`__RTMIDI_DUMMY__` when no API macro is set (`RtMidi.h:571`), and the ALSA branch in
`audio/CMakeLists.txt` is gated on `CMAKE_SYSTEM_NAME MATCHES "Linux"`, which is `Emscripten` here,
so the `pthread_create` calls at `RtMidi.cpp:1555`/`:1617` are never compiled. SDL2_mixer is
available as a port, so the audio problem was narrower than "stub audio" — only MIDI is genuinely
impossible. And `audio.cpp`'s `SDL_CreateThread` return value is never checked (`audio.cpp:146`), so
a failed thread creation just means the audio loop never runs.

**The actual blocker was that Emscripten's SDL2 port is DOM-bound at video init.** Under node the
binary reached `main`, parsed its harness arguments, read the recording through `NODERAWFS` and
created the trace file — then aborted in `emscripten_get_screen_size` with `ReferenceError: screen
is not defined`. Shimming `globalThis.screen` moved it exactly one step, to `document is not
defined` in `emscripten_set_pointerlockchange_callback_on_thread`. Piecemeal shimming is a losing
game; the port registers browser event callbacks and wants a canvas.

**`--headless` is the way around it.** Of the three options — a real browser, a DOM under node via
jsdom, or a path needing neither — the third is what this codebase is shaped for, because §6.3
establishes that rendering is entirely software into a `bitmap` double buffer and the only GPU
contact is one streaming texture blit. Skipping the window, renderer, texture and blit while still
calling `TraceFrame()`, which hashes `DOUBLE_BUFFER` *before* `PrepareBuffer()` and so never touches
the texture, leaves the identical game code running under bare node with no DOM at all. Native runs
are byte-identical with it and without.

Two things that turned out to be part of "no video":

- **The audio device is the other half.** Emscripten's SDL2 audio backend wants an `AudioContext`,
  which node does not have, so `Mix_OpenAudio` fails — and the failure path in
  `soundeffects::initSound` calls `iosystem::AlertConfirmMsg`, which draws a dialog and then
  **blocks on `GET_KEY()`**. During a replay that eats a recorded key and desynchronises the run.
  `--headless` declines to open a device rather than opening one and handling the failure. General
  hazard: *any* modal alert during a replay consumes a key the recording did not budget for.
- **The environment does not cross into the module.** `SDL_VIDEODRIVER=dummy` reaches nothing under
  node, so the decision had to be a program flag rather than an env var.

Asyncify is on (`WASM_ASYNCIFY`) for the blocking `SDL_WaitEvent` in `GetKey`. A replay does reach
it: `iosystem::Menu` and `bitmap::FadeToScreen` call `globalwindowhandler::WaitUntil`, which is
`SDL_Delay`, so a replay unwinds and rewinds through the menu fade on every run. JSPI or
`-sPROXY_TO_PTHREAD` later.

**And `-fexceptions` is not a tuning knob.** Emscripten disables exception *throwing* by default, so
`__cxa_throw` lowers to `abort()`, and IVAN throws as ordinary control flow: `areachangerequest` on
every level change, `quitrequest` on quit, `genericException` out of the prototype database. Without
it the WASM build dies drawing the main menu with a bare `Aborted(undefined)` and no stack. It is a
compile *and* link flag, globally, for the §7.7 reason — the compile half emits the landing pads, so
a translation unit built without it cannot catch what another one throws.

### 9.4 Native vs WASM: what the first real frame comparison found

`tools/corpora/compare-targets.sh` replays each corpus on both builds and compares. Getting from "it
runs" to `targets agree` meant fixing **twelve distinct defects across some fifty sites**. Every one
is a real bug that has been in IVAN for years, every one is invisible to any test that compares a
build against itself, and none is about Emscripten: they are places where the program left a choice
to the compiler or the standard library.

**They fall into four classes.** Start with the singleton, because it is the clearest.
`cachedfont::PrintCharacter` walked a 9×9 character cell a `ulong` at a time, ending at
`FontPtr + (20 / sizeof(ulong))` — two words and *eight* pixels on a 64-bit host, five words and
*ten* on a 32-bit one, and nine on neither. **Every 64-bit build of IVAN has been dropping the ninth
column**, the shadow's right edge: one dim pixel per character, for as long as 64-bit builds have
existed. Now copied a pixel at a time, which is width-independent and agrees with the
`NormalMaskedBlit` fallback the guard above it falls through to.

**(a) Unsequenced draws from the shared RNG.** C++ leaves function arguments, and the operands of
arithmetic and relational operators, unsequenced with respect to each other. So

```cpp
EyeColor = MakeRGB16(R + RAND_N(41), G + RAND_N(41), B + RAND_N(41));
```

takes three draws in an order the compiler picks. GCC picks right-to-left, Clang left-to-right, both
correct. The draw *count* is identical, so the RNG stream stays in lockstep and every determinism
test in this file passes on both builds — what differs is which draw lands in which field. That one
line is why the player's eyes came out a different colour under Emscripten, which is how the class
was found: two pixels, at `(198,243)` and `(200,243)`, in a 16×16 sprite.

The rule is written where anyone adding RNG code will meet it, above the `RAND` macros in
`FeLib/Include/femath.h`. Sites fixed:

| Site | What it decided |
|---|---|
| `human.cpp` `playerkind::PostConstruct` | the player's hair and eye colour |
| `femath.h` `region::Randomize` | **where every generated room sits and how big it is** |
| `level.cpp` `GenerateDungeon` fill loop | ground vs over terrain for all 1,600 squares of every level |
| `level.cpp` `CreateRoomSquare` ×11, via a new overload | the same, per room square |
| `level.cpp` fountain and altar placement | which square in the room |
| `fluid.cpp` `AddLiquidToPicture` | the speckle colour in a pool of blood |
| `lsquare.cpp`, `char.cpp`, `bodypart.cpp`, `human.cpp`, `nonhuman.cpp`, `gods.cpp`, `lterras.cpp`, `game.cpp`, `igraph.cpp`, `bitmap.cpp` | to-hit and damage rolls, lightning and explosion colours, sparkle and scar positions, will-power contests |
| `gear.cpp`, `miscitem.cpp`, `lterras.cpp` `InitMaterials` | the volume of an item's two materials |

Not every multi-draw expression is a bug, and the ones left alone are left alone on purpose: `&&`,
`||` and `?:` sequence their operands by definition, and `RAND()%36 + RAND()%36` gives the same
value whichever half is drawn first. Roughly half the 61 candidate sites are of that kind.

**Two traps in finding these.** A line-based grep misses them — `AddLiquidToPicture` spreads three
draws over three lines, so the scanner has to split on statements. And a *visible* draw can be
unsequenced with a *hidden* one: `SetLTerrain(GTerrain->Instantiate(), OTerrain->Instantiate())` has
no `RAND` in it at all, and it is the single most consequential site in the list.

**(b) Ties broken by the standard library.** `std::sort` is not stable and `std::priority_queue` says
nothing about equal elements, so a comparator that inspects less than the whole object leaves the
rest to libstdc++ or libc++ — which disagree.

| Site | Comparator looked at | What it decided |
|---|---|---|
| `worldmap.cpp` `distancetoattnam` | distance only | **where the towns and dungeon entrances go** |
| `lsquare.cpp` ground/over border partners | tile priority only | what is drawn over what, and 3% of every level file |
| `char.cpp` `svpriorityelement` | strength value only | which body part a bite lands on — the two legs are always a tie |
| `level.cpp` `nodepointerstorer` | distance, then diagonals | the route every monster and the auto-play AI walks |
| `wterra.cpp` `DrawOrderer`, `god.cpp` `materialsorter`, `game.cpp` `NameOrderer` | one field each | draw order, wish material, list order |

Fixed either by making the order total (a body-part index, a position) or by moving to
`std::stable_sort` where the input order is already deterministic. Prefer the total order when there
is an obvious tie-breaker; it cannot depend on the implementation at all.

Note how far each is from where it hurts. `distancetoattnam` is nine lines of world generation and it
decided the whole overworld; `svpriorityelement` is a heap of at most ten elements and it decided
which leg a hedgehog bit.

**(c) Undefined behaviour the two compilers exploit differently** — §6.9, the strict aliasing
violation in `character::Die`, which was this section's last open divergence. What had been ruled out
was correctly ruled out; the list simply did not include "the program is lying to the optimizer".

**(d) Identity that depends on the allocator** — §9.11, found later by a longer corpus.

**How to pick it up.** The technique that localised every one of these is worth following rather than
reinventing:

1. `compare-targets.sh` gives the first differing frame and whether `rng` moved with it.
2. Bracket it with a temporary `fprintf` of `harness::GetRandCount()` at a frequent, portable event
   — `msgsystem::AddMessage` and `character::Move` both work well — built on *both* targets and
   diffed. That turns "somewhere in 1,039 draws" into "between these two events".
3. Inside the bracket, run the native build under gdb with a breakpoint on `femath::Rand` and
   aggregate the backtraces by call site. The histogram names the suspect.
4. For a pixel difference rather than a draw-count difference, `--shot-dir` on a truncated recording
   plus a PNG differ localises it to single pixels, and a gdb watchpoint on that pixel of
   `graphics::DoubleBuffer->Image[y][x]` names the code that wrote it.

The first divergence in this hunt was one pixel of a text shadow; the second was two pixels of a
sprite's eyes. Neither would have survived being described as "a hash mismatch".

### 9.5 The browser host

`-DWASM_BROWSER=ON` produces a page that plays. In headless Chrome 143 it reaches the main menu —
artwork, fonts, all five entries — and driven with eight `ENTER` keystrokes over CDP it plays the
intro, generates a world, creates a character and lands on the world map with the side panel
populated and "Turn 0" on the clock. Console output for the whole run was one line,
`MidiOutDummy: This class provides no functionality.`

**This is a second host, not seam-1 progress.** §9.3's `--headless` was the way *around* the DOM; this
is the way *into* it. The browser build has never been compared against a golden and cannot be an
oracle in its current shape — `NODERAWFS` is what lets the harness write a trace, and turning it off
is the first thing `WASM_BROWSER` does. Frame comparison stays on the node host.

**What was in the way was three build-system facts, not game code.** Nothing in `Main/` or `FeLib/`
changed to make this work, which is the first real evidence that the headless path and §9.4 left the
game genuinely portable.

- `NODERAWFS` binds to node's `fs` module. In a browser it is not a degraded filesystem, it is a
  missing one, so the option had to become mutually exclusive with the browser target rather than
  merely defaulted differently.
- With it off, MEMFS starts empty. `Graphics/` and `Script/` are preloaded to **absolute** paths
  because `PORTABLE_BUILD` returns `"./"` from `game::GetDataDir()` (`game.cpp:5426`) while the
  module starts with its working directory at `/`, so the `"./Graphics/..."` every call site builds
  resolves to exactly the `/Graphics` in the package. Verified in the generated package metadata.
- Emitting `.js` gives you no page. The SDL2 port looks up `#canvas` at video init, so the executable
  has to be emitted as `.html` for emcc to generate one.

**Two hazards predicted here did not appear**, worth recording as plainly as the successes. Asyncify
carries real input: §9.3 established it unwinds `SDL_Delay` through the menu fade, but a replay is
answered by the harness *before* the blocking `SDL_WaitEvent` in `GetKey`, so no node run had ever
proven a keystroke could cross it — a browser one has. And the audio alert never fired:
`Mix_OpenAudio` was expected to fail before a user gesture and drop into `AlertConfirmMsg`, but an
`AudioContext` is constructible while suspended, so the open succeeds.

**What is open.** `ASYNCIFY=1` instruments the whole binary, all 7.5MB of it, and nothing has been
measured against a native frame time — `ASYNCIFY_ONLY` or JSPI is the tuning knob if it needs one.
Input beyond the auto-play set is mostly exercised: `` ` ``, `y`, `~` and 400 `.` crossed `GetKey`
in §9.6; the arrow keys, `>`, `?` and a shifted `S` crossed it in §9.10, which covers movement,
descent, the command list and the save prompt. What is left is the rest of the command set, and "no
reason to expect trouble" is what §9.4 was full of.

### 9.6 Collecting a browser crash, and why a stripped trap is not a lead

A wasm trap out of a release build looks like this, and it is worth being precise about how little it
contains:

```
Uncaught (in promise) RuntimeError: memory access out of bounds
    at ivan.wasm:0xed4dd
```

Those are byte offsets into the binary. A release build carries **no name section and no DWARF** —
measured with `llvm-objdump -h`. So nothing maps `0xed4dd` back to a function: not `emsymbolizer`,
which needs DWARF; not the browser; and not a later rebuild, because the offsets belong to the exact
binary that produced them and even that binary no longer knows its own function names. **The
information was never emitted.** A trace of this shape is a prompt to rebuild, not a lead.

That splits into two problems: make the next trap legible, and make the crash reproducible somewhere
with a debugger. **Neither can be arranged after the fact, so neither is optional.** This was the
design mistake worth recording: the first version of both was opt-in — build with `WASM_DEBUG`, play
with `?record=`. That is useless against the only crash that matters, the one a player hits once in
an ordinary session. By the time you know you want a recording, the session that would have produced
one is over; by the time you know you want names, the binary that would have carried them has already
printed its offsets. An option that has to be set *before* the thing it captures is not an option, it
is a trap. Both are unconditional in a `WASM_BROWSER` build.

**Names, always.** `--profiling-funcs` keeps the name section, changes no codegen, and costs about
800KB — 8.3MB against 7.5MB, measured. The asymmetry settles it: that cost is paid once at build time
where nobody notices, against a report from a crash that by definition cannot be re-taken to order.
Verified with `llvm-nm`, which reads back demangled C++ — `character::CanMove() const` and 1,835
others.

**Assertions, on for the browser and off for node.** `WASM_DEBUG` (`ASSERTIONS=2`,
`STACK_OVERFLOW_CHECK=2`) is the half with a runtime cost, so it splits by host: the node host runs
the differential corpora, where the measurement is the point and the cost is a tax on every run; the
browser host is played by a human who would rather be told what went wrong. The stack check earns its
place separately — the stack is generous but the level generator recurses, and an overflow is
otherwise indistinguishable from the out-of-bounds access it gets mistaken for, which is exactly the
reported symptom. `WASM_SAFE_HEAP` stays opt-in: it instruments every load and store, naming the
access and the address outright, at a slowdown that makes the game hard to steer. Turn it on once a
recording reproduces the crash, not while hunting one.

**Recording, always, and it changes nothing.** This had to be checked rather than assumed, since
turning a harness mode on for every player is exactly the kind of change that quietly alters the
game. It does not: `main.cpp:154` seeds from `harness::GetSeedOverride()` when there is one and
`time(0)` when there is not, and a recording with no `--seed` sets the override to `time(0)` anyway
(`harness.cpp:442`). Same seed, same game — the recording only writes it down. The cost is a flushed
line per keystroke. `?record=off` opts out.

**What makes the recording worth having was already in the harness.** `RecordKey` flushes every key
as it writes it (`harness.cpp:545`), so a trap leaves the recording complete but for its
`# end keys=` trailer — the keys that led to the crash survive the crash, and the seed rides along in
the header, so the file is a deterministic reproduction rather than a description of one. Nothing in
the harness had to change. It had no way to be *reached* from a browser, which is all
`web/src/harness/` supplies: query string to argv on the way in (`argv.ts`), MEMFS to a report on the
way out (`report.ts`).

**The report outlives the tab.** A crash is usually followed by a reload or a close, either of which
takes MEMFS with it, so reports are kept in `localStorage`. Each carries the failure text, the
recording, the seed, the key count and a build id from `git describe --tags --always --dirty` — the
last because a stack trace symbolized against the wrong binary gives confident, wrong answers, and a
dirty tree is precisely the build nobody else can reproduce. The POST hook behind
`IVAN_CRASH_ENDPOINT` is inert unless one is set; local-only is the default.

**Measured end to end**, and the last three rows are the ones that matter, because they were taken on
a session opened at a bare `ivan.html` — no query string, nothing a player would have had to know in
advance:

| Run | Result |
|---|---|
| 8-key browser recording, replayed natively | exit 0, **358 frames** |
| 403-key auto-play browser recording, replayed natively | exit 0, **752 frames** |
| Either, on the native harness's reading of the missing trailer | correctly reported as "cut short" |
| Plain `ivan.html`, no query string | argv is `--record /session.rec`, **recording present unasked** |
| Report after a failure, then a page reload | **survives** — build id, seed, key count, recording |
| The recording carried *inside* that report, replayed natively | exit 0, **346 frames** |

The failure in the fifth row was injected as a rejected `WebAssembly.RuntimeError`, which is the
shape a real trap takes here: ASYNCIFY means `main` runs inside a promise, so a trap arrives as an
unhandled rejection rather than a synchronous throw. It exercises the real handler, but it is not a
real trap, and the distinction is worth keeping — what is proven is the collection path, not that a
genuine out-of-bounds is caught.

**A WASM-only crash is a finding, not a dead end.** wasm32 traps on accesses x86-64 absorbs silently:
a read past a buffer is still inside a mapped page natively and returns garbage, where in wasm it is
either outside linear memory or caught by `SAFE_HEAP`. Same for a null dereference, and for a `long`
that was 8 bytes when a save was written and is 4 here (§6.8, §7.9). So a crash that will not
reproduce natively usually means a real latent defect that native testing structurally cannot see.

**The crash that prompted all of it is §6.9, and it was reported twice.** The first report came from a
build with no name section, so nothing could be recovered from it, and 400 turns of auto-play under
`WASM_DEBUG` did not reproduce it. The second came from a build with names, and that report alone was
enough: it named `character::Die` → `dog::CreateCorpse` → `lsquare::AddItem`, and its recording
replayed the path on the node build first go. The difference between the two reports is exactly the
800KB of `--profiling-funcs` this section argues for.

**What the second report needed, in order** — only the first step was foreseen. The recording
reproduced the *path* on the WASM node build (the RNG stream matches the report's key-by-key `rng`
column exactly) but **not the trap**: release WASM absorbed the wild pointer the browser build
happened to fault on, since the two have different heap layouts. `WASM_SAFE_HEAP=ON` turned that back
into a hard failure at the first bad access rather than several dereferences later, which is the case
the option was added for. Relink with `--profiling-funcs` as well
(`-DCMAKE_EXE_LINKER_FLAGS=--profiling-funcs`, which `WASM_BROWSER` sets and the node host does not)
or the names are gone again.

**And the native build could not reproduce it, which was itself the second finding.** The same
recording replays natively without crashing because native and WASM generate a *different New
Attnam*. So "replay it natively and the whole native toolchain applies" held only after the WASM run
had localised the bug. Do not assume a browser recording lands in the same game state natively; check
the frame trace first.

**What is still missing from a report:** nothing carries game state — dungeon, level, turn, character
— so a report says where the program stopped and how to get back there, but not what the game thought
was happening. The recording makes that recoverable rather than lost, so it is a convenience rather
than a gap. Worth adding only if a crash turns up that the recording cannot reproduce.

### 9.7 Sound effects, played by the page

The browser build makes sound and the wav files are no longer part of the download. Driven over CDP
in headless Chrome 143 — ten `ENTER`, then `` ` ``, `y`, `~` and 120 `.` of wizard auto-play — the
page fetched, decoded and played five distinct effects chosen by the game itself: `dooropen.wav`,
`DoorResists1.wav`, `DoorResists3.wav`, `bark.wav` and `howl1.wav`. Zero dropped, one deliberate 404
reported as one console line and nothing else.

**Audio moved out of the wasm module rather than into it.** SDL_mixer is still what plays sound on
every other target; on Emscripten `soundeffects` opens no device, allocates no channels and loads no
wav. It matches the message, picks the file, and hands the filename to `web/src/audio/sfx.ts` through
an `EM_JS` bridge. That file owns the `AudioContext`, the fetch, the decode cache and the voice cap.

**The boundary is deliberately below the regex, and that is the whole design decision.** Everything
upstream of the filename stays in C++: `Sound/SoundEffects.cfg`, its 153 patterns,
`findMatchingSound`, and the private xorshift that chooses between several files for one pattern. Two
reasons, neither about effort:

- **One copy of the pattern table.** Moving matching to JS would put a second copy of 153 regexes in
  a second engine, and the thing they match against is English prose that changes. §9.2 needed
  `tools/regexdiff` and 111,342 comparisons to prove PCRE and `std::regex` agreed on these patterns;
  a third engine is a third thing to keep proving.
- **`NextSoundRand` stays on the C++ side of the wall.** §6.5 made it a private stream so that which
  sounds are installed cannot shift the game's RNG. Cutting here preserves that property literally
  unchanged rather than re-establishing it in JavaScript.

The right fix for the underlying design — the game has no sound *events*, only sentences that regexes
are matched against — is [issue #1](https://github.com/bethmaloney/ivan/issues/1). This change was
placed where it is so as not to entrench the string matching by exporting it.

**What the page does that SDL_mixer could not.**

- **Nothing under `Sound/` is preloaded.** 26MB of wav against the 3.3MB of `Graphics/` and `Script/`
  in `ivan.data`, so preloading it would have made first load an order of magnitude slower for audio
  that may never play. Each file is fetched at first use and cached by the browser thereafter.
- **Latency is a buffer rather than a mixer chunk.** `Mix_OpenAudio`'s 8000-sample request rounds up
  to 8192 (`SDL_audio.c:1431`), about 186ms between a blow landing and the sound of it. WebAudio
  schedules on the sample.
- **The autoplay policy is handled where it lives.** SDL2's backend registers emscripten's
  `autoResumeAudioContext` on the context *SDL* opened (`SDL_emscriptenaudio.c:233`), and SDL now
  opens none, so the sfx module registers the same three listeners for its own.

**`Sound/SoundEffects.cfg` is preloaded on its own, and missing it is a silent failure.** The pattern
table stays in C++, so `initSound` still reads the file with `fopen` — out of a MEMFS that no longer
contains the directory it lives in. When the open fails, `initSound` settles on `SoundState = -1`
(the `ABORT` beside it is commented out) and `playSound` returns before reaching the bridge. Nothing
is printed and nothing traps: the game is simply silent, and the JS layer looks broken because it is
never called. It is 10KB and it is preloaded explicitly. This was a real bug in the first version of
this change, found by the CDP run above.

**A first sound is played late rather than dropped.** The first use of an effect has to fetch and
decode, and the first version dropped it and warmed the cache instead, on the theory that a late
sound is worse than none. That was wrong for this game: it made the first door of every session
silent, which is exactly when a player is deciding whether the game has sound. IVAN is turn-based and
the message is still on screen, so it plays on arrival within a 250ms bound. The bound is what still
rules out the case the rule was written for — a stalled fetch landing over an unrelated turn — and
the *suspended context* case, where `currentTime` does not advance and queued sounds do not play late
but all at once on the first keystroke.

**The oracle is untouched, and it is the same `__EMSCRIPTEN__`.** The node host compiles every branch
above, so this was verified rather than assumed: `compare-targets.sh` reports targets agree on both
corpora after the change and `verify-corpora.sh` matches golden. Nothing reaches the new code there
because `--headless` returns from `initSound` before it, which is also why the seam-1 measurement
never hears anything.

**What is open.** Nobody has listened to it — headless Chrome has no audio device here (§9.9 corrects
that, by accident). The wavs are still wavs: 26MB across 153 files, now fetched individually.
Converting them to OGG is a data-only change — `Mix_LoadWAV_RW` falls through to `Mix_LoadMusic_RW`
for non-RIFF magic (`mixer.c:822`) so the native path takes them too, and only the filenames in
`SoundEffects.cfg` change. Expect roughly 26MB → 2MB. And `-sUSE_SDL_MIXER=2` is dead weight on the
browser build now: nothing calls `Mix_*` there, and `sfx.h` includes `SDL_mixer.h` for the
`Mix_Chunk*` in `SoundFile` and that is all. Dropping the port has a wider blast radius than it looks
— `FeAudio` links it too — so it is left alone deliberately.

#### 9.7a Effects at full scale, and the one place the page disagrees with native

§9.7 left "nobody has listened to it" open. Somebody has now, and the answer was that the sound
effects are about four times louder than they should be while the music is fine. Four times is 12dB,
which is a big enough number to be a bug, so the first question was whether the port had introduced
one. It had not.

**Both paths take the same `lSfxVol` and apply it the same way, within 0.07dB.** The bridge hands
over `lSfxVol` (`sfx.cpp:381`) and SDL_mixer is given the same number by `Mix_Volume(iChannel,
lSfxVol)` (`sfx.cpp:396`). Native then computes `(master * channel * chunk) / MIX_MAX_VOLUME^2`,
which for a chunk loaded at `MIX_MAX_VOLUME` and a master left alone is `lSfxVol` exactly
(SDL_mixer's `mixer.c:385`), and scales each sample by `volume / SDL_MIX_MAXVOLUME` (SDL's
`SDL_mixer.c:84`). At the default `SfxVolume` of 127 that is **×127/128**; `sfx.ts` applies
**×127/127**. The port is 0.07dB louder than the reference build, which is nothing, and both play the
file at the level it was mastered at.

**The level is in the files.** Decoding every wav under `Sound/` — 155 of the 159 without needing a
codec — gives a median peak of **−0.21dBFS**, with **98 of 155 peaking within 1dB of full scale**,
and a median RMS of **−15.9dBFS**. The effects that fire constantly are the dense ones as well as the
hot ones: `blunt3.wav` is **−9.2dBFS RMS beneath a 0dBFS peak**, `punch3.wav` −9.8, `Hiccup.wav`
−9.6. Sixteen voices may sum over that with nothing limiting them, in either build. So the effects
are loud natively too, and always have been.

| | |
|---|---|
| native per-sample scale at `SfxVolume` 127 | ×0.9922 (`mixer.c:385`, `SDL_mixer.c:84`) |
| page per-sample scale at `SfxVolume` 127 | ×1.0 (`sfx.ts`) |
| difference | **0.07dB** — not the 12dB reported |
| wavs decoded | 155 of 159 (`punch.wav` MP3, three ADPCM) |
| median peak / median RMS | **−0.21dBFS** / **−15.9dBFS** |
| peaking within 1dB of full scale | **98 of 155** |
| hottest of the constantly-firing effects | `blunt3.wav`, **−9.2dBFS RMS** at a 0dBFS peak |

**So the change is a deliberate divergence rather than a fix, and it is written as one.** A gain node
between the voices and the shared master takes **12dB** off everything the effects path plays, and
nothing else changes: the slider stays linear, as `Mix_Volume`'s is, and the trim is one number in one
place rather than a factor folded into every voice. It hangs *below* the master rather than on it
because the master is shared — `music.ts` connects its stems to the same node, and the mute button
drives it — so the music that was already fine is untouched. `?sfxgain=` overrides the number without
a rebuild and `?sfxgain=1` is what native sounds like, which is the comparison anyone doubting this
paragraph should run first.

**Two things found on the way that are not about gain.**

- **`punch.wav` has never played natively.** It is MPEG Layer 3 inside a RIFF container (format tag
  0x55), and SDL's wav loader answers that with `SDL_SetError("MPEG formats not supported")`
  (`SDL_wave.c:1753`), so `Mix_LoadWAV` returns NULL and `playSound` falls through to silence. It is
  one of the five files `SoundEffects.cfg:95` picks between for `You.* hit`, the most frequent combat
  message in the game, so **one hit in five has been silent on every native build**. The page does not
  use SDL's decoder, so it probably plays it and is that much busier than native — unverified here,
  and checkable from `ivanSfx.played()` against `stats().failed` after a few blows. The other three
  exotic files are ADPCM, which SDL does support: `lightning.wav` and `vomit.wav` MS, `destroyed.wav`
  IMA.
- **The two volume sliders diverge as they come down, faithfully.** Music is `(v/127)^2` because that
  is what a synth does with CC7 (§9.8); effects are linear because that is what `Mix_Volume` does. At
  64/64 the effects sit 2× above the music, at 32/32 **4×**. Native behaves identically, so this is
  upstream IVAN and not something the port should quietly correct — but it is the other way to arrive
  at "four times too loud", and worth knowing before reaching for the trim above.

**What is not measured.** Neither build's output was captured. The two gain chains were read from
source on both sides and the file levels decoded from the wavs themselves; that is arithmetic and
content, not a recording. A capture of the native path — SDL's `disk` audio driver writes the mixed
stream to a file — would close it, and would also settle the `punch.wav` question in the browser.

### 9.8 Music, played by the page

This is the change that **retires `audio/` on this target** rather than porting it. RtMidi, the MIDI
parser, the playback engine and their helpers — about 4,700 lines — are no longer compiled for
Emscripten at all; `libFeAudio.a` holds `audio.cpp.o` and nothing else. What is left of `audio.cpp`
is the part the game calls: the playlist, the playback state, the volume and the intensity. Nothing
synthesizes MIDI in the browser. `Music/*.mid` are never fetched by the page; they are the *source*
for pre-rendered OGG stems, and those are what stream.

**Three things this found before any code was written, all of which changed the design.**

**1. Six of the eleven tracks are the same note-free file.** `Empty.mid`, `defeat.mid`,
`mainmenu.mid`, `newgame.mid`, `victory.mid` and `world.mid` are byte-identical — md5
`29be858c8269a7618c68db9aa0152ade`. It is the project template: 18 tracks, 96 program changes, 27,654
controller events, and **zero note-ons**. The main menu, the world map, victory and defeat are silent
in the native game too, and always have been. There are five pieces of music in IVAN, totalling 17.9
minutes, and "this area is silent" is a normal state the page has to represent rather than a fault to
report.

**2. The adaptive mix is live, and it carries most of the music.** `character::Be` recomputes an
intensity every turn from the player's *worst* body part — `127 - MinHPPercent`, `char.cpp:1062` —
and `audio::SendVolumeMessage` turns it into a per-channel MIDI volume. `MPB_PB_NO_VOL`
(`midiplayback.cpp:792`) drops the file's own CC7 on the way through, so the game owns channel volume
outright and the composer's automation there is never heard; the files' CC11 expression rides
underneath and is heard.

The fade-in group holds **13,683 of the 22,037 notes, 62%**, and it is at volume 0 at intensity 0. A
player at full health hears about a third of the piece, and the mix opens up as they get hurt.
Cathedral is 363 notes constant against 1,416 fade-in. **Rendering one flat mix per track would have
shipped music missing most of itself**, and it would have sounded fine — the kind of loss nobody
would think to look for.

**3. The intensity system is exactly three curves, so three stems reproduce it rather than
approximate it.** From the two constant tables at `audio.cpp:84-89`:

| group | channels | volume | notes |
|---|---|---|---|
| `const` | 0–4, 9 | `127` | 4,679 |
| `fadeout` | 5–8, 10 | `127 - intensity` | 3,675 |
| `fadein` | 11–15 | `0 + intensity` | 13,683 |

Every one of the sixteen channels follows one of these and nothing else does anything. So splitting
the MIDI on those groups, rendering each, and giving the three to three WebAudio gain nodes is not a
model of the intensity system — it *is* the intensity system, with the mixing moved from a MIDI
device to a gain node. `tools/music/split-stems.py` derives the grouping from the same two tables, so
the two can be diffed if upstream ever retunes the mix.

**`GetCurrentlyPlayedFile` is the one readback, and it is why this could not be pure
fire-and-forget like §9.7.** `dungeon::PrepareMusic` calls it at every level change to decide whether
the new area shares the track already playing and should therefore not restart it
(`dungeon.cpp:174`). It is a plain synchronous `EM_JS` returning an `int` index into the playlist — no
promise, no callback into wasm, nothing for asyncify to unwind, no string whose lifetime someone has
to own. Resolved by name at the moment it is asked for, so it stays correct across the reorder
`ClearMIDIPlaylist` performs.

**Track choice moved to the page, and that is safe for the reason §6.5 made it safe.**
`NextTrackRand` was a private xorshift precisely so that neither which music is installed nor when a
track happens to end could shift the game's own RNG (`audio.cpp:54`). Moving the choice out of the
module keeps that property by construction: there is no longer a shared stream for it to draw from.

**Music streams; effects are cached. That is the one real structural difference from §9.7.**
`decodeAudioData` produces float32 at the context rate — about **23MB per minute per stereo stem**.
`Dungeon3` is 7.31 minutes, so its three stems would be roughly **half a gigabyte** of decoded audio
for one dungeon, played once through. So each stem is an `<audio>` element behind a
`MediaElementAudioSourceNode`: memory stays flat and playback starts on the first few KB rather than
after a multi-megabyte fetch completes. The cost is that three elements keep three clocks, and these
stems are the same piece of music, so drift is heard as a doubled attack rather than as a timing
error.

**The first real session found that the drift was not drift.** `ivanMusic.stats().drift` reported
`[-0.166, 0.0001]`: the fade-in stem locked to the leader within **0.1ms** and the fade-out stem —
the largest of the three files — sat **166ms behind**. Two of three staying exact says the clocks were
never the problem. The *start* was: `Play` called `play()` on all three at once, and a media element
begins when it individually has data, so the biggest file started last. 166ms is a flam on every
attack, and the correction as first written could not have recovered it: it was under the 250ms seek
threshold, so only the 0.2% nudge applied, which closes 166ms in **83 seconds**. That was not a
correction, it was a rounding error with a counter attached.

Three changes, in the order they matter:

- **A readiness barrier.** Nothing plays until every stem reports `HAVE_FUTURE_DATA`, and then all of
  them start in one tick. This is the actual fix; the rest is a safety net. A stem that errors, or a
  five-second timeout, releases the barrier too — a thinner mix late beats silence.
- **One hard alignment 400ms in**, on a 10ms tolerance. `play()` is not sample-synchronous across
  elements even when all are ready, and a seek is cheap only here: the gap lands in the first moment
  of the track rather than mid-phrase.
- **Thresholds that can converge.** Seek past 50ms rather than 250ms, and nudge up to 0.5% rather
  than 0.2%. One gap beats ten seconds of flam.

After the fix, sampling `stats().drift` by hand over about a minute of a three-stem track: start skew
**166ms → ~20ms**, and the nudge closes the rest to sub-millisecond over a few seconds at almost
exactly the 0.5% cap — 5ms per second of playback. No seek fires; the whole correction is inaudible.
The residual 20ms is deliberately left to the nudge: tightening `Align` to seek it out would trade a
convergence nobody can hear for a gap everybody can, and 20ms decays below the flam threshold within
about three seconds. Seeks are counted separately in `ivanMusic.stats()` because routine seeks in
steady state would mean this design is the wrong one. One reading worth knowing: a stem sitting at
`0.0029` is not drift, it is ~128 samples at 44.1kHz — one render quantum, the floor of what
`currentTime` reports, and it alternates with `0` rather than growing.

**The two modules share one `AudioContext` but not one gain, and both halves matter.** Sharing the
context is the easy half: browsers cap how many a page may have, each costs a device connection, and
two would need two resumes off the same gesture — so the sfx module exposes the one it owns and the
music module joins it. Not sharing the *gain* is the half that was a bug first: the music volume went
onto the shared master, where it would have scaled the sound effects too, since effects pass through
the same node with their own `SfxVolume` applied per sound. Music hangs its own gain off the master
now.

Second half of the same bug: `ivanconfig::Initialize` calls `audio::SetVolumeLevel` at startup
(`iconf.cpp:1343`), long before a gesture can have let a context exist — so the volume is *normally*
set before there is anywhere to put it, and the first version dropped it and started every session at
full volume regardless of the setting. The remembered value is applied when the node is created.

**Ramps use the game's own slew rate.** `audio.cpp:78` moves `CurrentIntensity` one step per 15ms
toward the target, so a full sweep takes 1.905s. The gain ramps are given that duration rather than an
arbitrary smoothing constant: the mix opening up as a fight turns bad is meant to be a swell, not a
switch.

**One pseudo-device keeps `ivanconfig` unchanged.** `GetMIDIOutputDevices` reports a single "Web
Audio" device on this target. That is what turns the soundtrack on by default — `Initialize` enables
it whenever the count is non-zero (`iconf.cpp:1292`) — and what the options menu shows against "Use
MIDI soundtrack", with "no" still available by cycling past it. No change to `iconf.cpp` at all.

**`Music/stems.json` is why the page never probes.** It records which stems each track actually has,
and is generated by the splitter rather than maintained. Without it, a track with no stems and a track
whose files failed to deploy look identical — and since six of the eleven tracks are legitimately
silent, swallowing 404s would make a broken deploy indistinguishable from an ordinary quiet area.

**Rendering is offline and the result is committed**, like `Sound/`'s wavs. Making it a build step
would put fluidsynth and a soundfont in the way of every `-DWASM_BROWSER=ON` and bake whichever
soundfont was on the builder's machine into their copy. Rendering once means every player hears the
same music — which is more than the native path can say, where the soundtrack is whatever the local
MIDI device makes of it.

**Two erase bugs fixed on the way past.** `ClearMIDIPlaylist` and `RemoveMIDIFile` both reused an
iterator `vector::erase` had invalidated; `RemoveMIDIFile` also skipped the element after every match
and could step past `end()`. Undefined either way. They survived because nothing calls
`RemoveMIDIFile` and because the other one's misuse happens to do the right thing on a vector — luck
that stops holding the moment anything else changes, and this change makes both run on a path that
then pushes the result across a boundary.

**What is measured.** The stem split is lossless per group — every emitted stem carries its channels'
events exactly, meta and timing identical, CC7 stripped — and the only events dropped are controllers
on groups with zero notes, inaudible by construction. Both corpora match golden and
`compare-targets.sh` reports targets agree.

**What is open.** The soundfont is an unmade decision: pre-rendering picks one canonical instrument
set for every player, which is a change in kind from the native path, and it is worth choosing by ear
rather than by whichever file `find_soundfont` reaches first. Long-run drift is unwatched — steady
state is measured over a minute or so, and what a full seven-minute `Dungeon3` does has not been sat
through; `stats().seeks` is the number that would matter.

### 9.9 A site to put it on

`tools/web/dist.py` assembles 70.4MB into a tree that serves the landing page at `/` and the game at
`/play/`. Both were driven in headless Chrome from the assembled tree, not from the build directory:
the page loads, the module boots, eight `ENTER` keystrokes create a character and reach the world map,
and every asset either page can request answers 200.

**The front door is not the game, and that is the only structural decision here.** `/` is a 30KB
page; `/play/` is where the download starts. Splitting them costs one link and means a shared URL
opens instantly rather than committing whoever clicked it to a download they did not ask for.

| | |
|---|---|
| landing page | 30KB, plus 113KB of fonts over 5 files |
| game, on disk | 11.0MB — `ivan.wasm` 7.8, `ivan.data` 3.4, `ivan.js` 0.3 |
| game, **on the wire** | **5.0MB** — measured against the CDN with the cache disabled |
| sound effects | 25.4MB over 159 wav, fetched on demand |
| music stems | 33.8MB over 14 ogg, streamed |
| **total, on disk** | **70.4MB** |

**On the wire is the number that matters, and it is less than half the one on disk.** Cloudflare
brotli-compresses `application/wasm`, taking `ivan.wasm` from 7.48MB to **1.65MB** — a 4.5× cut on
the single largest thing a player waits for. `ivan.data` is already-compressed PNG and does not move.
Nothing had to be configured for this, but it does mean any size quoted where a player reads it has
to come from a measurement against the deployed site rather than from `ls`: the landing page said
11MB until this was checked, which was true of the disk and wrong about the download.

**`shell_minimal.html` is gone, and replacing it hit a trap worth writing down.** emcc runs the shell
through its C preprocessor before substituting anything, and it treats **any** line whose first
non-whitespace character is `#` as a directive (`src/parseTools.mjs:149`):

```
error: shell.html:18: Unknown preprocessor directive #canvas
```

That rules out CSS id selectors at the start of a line — `#status {` is an unknown directive, not a
rule. The stock shell only survives because its CSS ships minified onto one line. The fix is to style
by class and leave ids for JavaScript and for SDL, which has to find `#canvas` by id at video init
regardless.

**The assembly step verifies the assets rather than trusting a glob, and that paid immediately.**
`dist.py` parses `Sound/SoundEffects.cfg` for every wav a pattern can name and `Music/stems.json` for
every stem it promises, then looks for each by name. The first run reported one missing file:
`explosion3.wav`. The file was on disk as **`explosion3.WAV`**. Case-insensitive filesystems hide it,
which is why it survived on Windows and macOS; over HTTP and on Linux it is a 404, and §9.7's design
makes a 404 silent by construction — one console line, no error, a game quietly missing a sound. This
is older than the web build and was never a web bug; the web is the first place it could not hide. A
glob would have agreed with itself and found nothing, which is the whole argument for parsing the
config: the check is only worth having if it can disagree with the directory.

**The stems are rendered, committed and heard.** Forcing `Dungeon.mid` in a browser served from the
assembled tree gives stems `["const","fadeout","fadein"]`, `started 1`, `failed 0`; gains at
intensity 0 of `[1, 1, 0]` exactly as §9.8's table says; and drift after ~12s of `[-0.017, -0.003]`
with 18 corrections and 0 seeks. So the fetch, the decode, the three-way mix and the drift correction
all work over HTTP from the deploy layout.

**And it has been heard, which is not how anyone planned it.** This section first claimed the mix was
unheard because "headless Chrome has no speakers". That is false on this machine: under WSLg a
`--headless=new` Chrome routes audio to the Windows host like any other client, and the check above
left `Dungeon.mid` playing in a browser with no window to close it from. Two things follow, and the
second is the useful one. A headless browser here is audible, so anything that calls
`ivanMusic.setPlaying(true)` should stop it again and kill the browser when the check ends — and
`pkill -f "tools/web/serve.py"` is not the way to do it, because the pattern matches the shell running
it, so the shell dies first and the servers outlive the command meant to end them. Kill by pid. And
**"no speakers" was an assumption doing the work of a measurement**, in a document whose whole
argument is against exactly that. The mix being unjudged is still true; the reason given for it was
not.

**Cloudflare Pages, for one reason that dominates the rest.** 60MB of the payload is media, and the
free tier does not meter bandwidth, where Netlify and Vercel both cap at 100GB/month — which this
game reaches in a few thousand sessions. The constraints that would have ruled a host out are
satisfied: the `application/wasm` MIME type, and a per-file limit above 7.8MB. Nothing here needs
COOP/COEP, there being no pthreads and no `SharedArrayBuffer`.

`_headers` revalidates the wasm bundle rather than caching it hard: none of those filenames are
content-hashed, so a redeploy reuses `ivan.wasm` and a browser holding a long `max-age` copy would
keep playing an old build with no way to find out. Media caches for a day, fonts for a week. Verified
live.

**Cloudflare Pages does not answer byte ranges, and that was the one host property §9.8 depended
on.** A `Range: bytes=100-199` on a stem comes back `200` with the whole 2.6MB file and no
`Accept-Ranges` header, warm cache or cold:

```
$ curl -sI -H 'Range: bytes=100-199' .../Music/Dungeon.const.ogg
HTTP/2 200
content-length: 2631966
```

`tools/web/serve.py` implements 206 precisely so that local testing would not flatter a host that
does not — and then the host did not. **Measured cost, smaller than the setup suggests: stems still
start in 1.0s**, `started 1`, `failed 0`. Progressive download is enough to begin playback; what
ranges buy is seeking and an early `duration`, and the page needs `duration` only for drift
correction, which it can wait for. So this degrades what §9.8 built rather than breaking it, and the
degradation was measured rather than assumed in either direction. Worth revisiting if a long track
starts late in practice — `Dungeon3`'s stems are 5MB each and its three must all arrive. R2 answers
ranges and the music module already takes `?musicbase=<url>`, so moving just the music is a query
parameter rather than a migration.

**What is open.** The crash endpoint is still inert: no endpoint has ever been set, so a player's
crash report reaches `localStorage` and nowhere else (§9.6). One `env:` line on the `package` job
would do it, and a Pages Function on the other end would turn every stranger's crash into a
replayable recording — which is exactly what §9.6 built and nothing is collecting. And the landing
page quotes the source with nothing checking that it still says that: the body part numbers, the
message templates, the opening text and the key list are real values lifted from `Script/item.dat`,
`char.cpp`, `game.cpp` and `command.cpp`. If those change the page is wrong, and only a reader would
notice.

### 9.10 Saves that survive the tab

A game saved in a browser is still there after a reload, and after Chrome has been closed and
reopened. Played from the assembled `dist/` tree, seeded with `?seed=999` so the run lands where
`noncombat.rec` does — Belyer Asu, UT lvl 1, turn 3, HP 37/37 — the level-entry autosave writes three
files and every one comes back byte-identical (SHA-256 over each, before and after). Driving the main
menu's second entry then prints **"Game loaded successfully."** with the same character, HP and gold.

The game did not learn any of this. One line of C++ moved and the rest is a page.

**Where the player's data lives, and why it had to move.** `GetUserDataDir()` answers `/ivan/` on this
target instead of `PORTABLE_BUILD`'s `"./"` (`save.cpp:838`), and `/ivan` is an IDBFS mount. It had
to be a different *directory*, not merely a different filesystem: `"./"` resolves to `/`, which is
where `--preload-file` puts `Graphics/` and `Script/`, and an IDBFS mount cannot be laid over a
populated MEMFS root. Splitting them is the cut that should have existed anyway — read-only game data
on one side, the player's on the other — and one mount then covers everything the player accumulates:
`Save/`, `Bones/`, `Scrshot/`, `ivan.cfg`, the highscore table, the answer to the name prompt.
`GetDataDir()` is untouched. The node host is untouched too: it is `NODERAWFS` and writes traces
relative to the launch directory, so the branch is behind `IVAN_WASM_BROWSER`, a compile definition
that exists because nothing else distinguishes the two hosts at that level — both are
`__EMSCRIPTEN__`.

**IndexedDB rather than localStorage, and this is a measurement rather than a preference.** One
dungeon level of the non-combat corpus is **3.5MB** of save files natively, of which the level file
alone is ~1MB, and a run visits dozens of levels. localStorage is 5–10MB per origin, holds strings
(so +33% for base64) and writes synchronously on the main thread; it would run out before the player
left Under Water Tunnel. Emscripten also has no localStorage filesystem backend — MEMFS, NODEFS,
IDBFS, WORKERFS, PROXYFS — so it would have meant hand-rolling a serializer as well. Three reasons,
any one sufficient.

**`.bkp` files are off on this target.** `outputfile` copies the previous save to `<name>.bkp` before
overwriting it (`save.cpp:70`), which on the non-combat corpus is 1,270,887 of the save set's
3,647,069 bytes — **35% of what a run costs to keep**, duplicated into IndexedDB. The `.tmp` staging
beside it still covers a crash during a write, which the constructor's own comment calls the useful
half. What is given up is the crash-during-level-generation case in `iosystem::ContinueMenu`, whose
recovery prompt is an `AlertConfirmMsg` — the shape the replay harness would rather never meet.

**The sync is debounced, and not only to coalesce.** `outputfile` writes `<name>.tmp` and copies it
over the final name on close, so a sync taken mid-save pushes a megabyte of temporary file into
IndexedDB and deletes it again on the next pass. The saves module waits for the writes to stop and
refuses outright while a `.tmp` is on disk.

The wait is free, for a reason worth stating because it is not obvious: **the game blocks inside wasm
and only returns to the JS event loop when asyncify unwinds it at the input wait.** A timer cannot
fire until the game is idle, which is exactly when a sync should happen. In practice this is so
effective that the `.tmp` guard never fired in any measured run — the temporary files are always gone
before the first timer gets a turn. It stays, because "never observed" is not "cannot happen", and the
failure it prevents is silent.

**`FS_DEBUG` is not a debug mode.** The saves module learns that a save was written from
`FS.trackingDelegate`, which exists only when the module is linked `-sFS_DEBUG=1`. The name suggests a
cost that is not there: `settings.js:393` defines the option as exactly "register file system
callbacks using trackingDelegate in library_fs.js", and `libfs.js` is the only file in the whole JS
library that mentions it. Counted rather than assumed — **17 `#if FS_DEBUG` blocks, of which 14 guard
an optional hook call, 1 captures a local for one of them, and 2 guard a `dbg()` line in
`forceLoadFile`**, a lazy-file path this build does not take because `ivan.data` arrives preloaded. In
the shipped `ivan.js` the hooks compile to
`FS.trackingDelegate["onWriteToFile"]?.(stream.path, bytesWritten)` and eleven more of that shape,
which is a property lookup and a short-circuit for the ten nobody registers. Without the flag there
is no failure to see — the mount populates, the game plays, and nothing it writes ever leaves MEMFS.

**The first run in a real browser hung on the loading bar, and the bug was in the handling of the
bug.** `Track()` threw because that build had no `trackingDelegate`, the throw happened inside a
callback *IndexedDB* invokes rather than inside the promise chain around it, so the `.catch()` never
saw it and `removeRunDependency` was never called. A page that would not start, because of a save
feature. The comment above that block said "nothing below may leave the dependency held" and the code
did not honour it; it is a `try/finally` now, and the test that would have caught it exists.

**Four hazards, all with an escape hatch, because persistence creates failures ephemerality could
not.**

- **A poisoned save.** A save the game cannot load now fails on every load, forever, and a player who
  cannot reach the menu cannot use a console API that lives behind it. `?wipesaves` deletes the
  database before the mount. A delete that IndexedDB *queues* because another tab holds the database
  open fires `onblocked`, and the first version reported that as success — sending someone straight
  back into the save they were escaping. It says so now.
- **Two tabs.** Both mount the same database and each holds its own MEMFS, so whichever syncs last
  overwrites the other's saves wholesale. An exclusive `navigator.locks` lease is taken for the life
  of the page; a tab that cannot get it still reads the saves, never writes them, and says which tab
  to close. Measured: first tab writable, second read-only and warned, first unaffected.
- **Eviction.** `navigator.storage.persist()` is requested once and was granted here.
- **A full disk.** IDBFS's own `autoPersist` discards the error from its sync (`libidbfs.js`,
  `onPersistComplete`), which is how a full disk becomes a game that quietly stops saving. This drives
  the sync itself so the failure is counted, kept dirty for the next attempt, printed, and put on the
  page.

**What is measured.**

| Check | Result |
|---|---|
| Autosave set written in a browser | 3 files, 1.15MB — `.40` 918,482, `.sav` 171,487, `.wm` 63,667. **No `.bkp`** |
| Same set after a page reload | **byte-identical**, all three, by SHA-256 |
| Same set after quitting and reopening Chrome | **byte-identical** |
| Continue Game after a reload | **"Game loaded successfully."** — same character, HP 37/37, gold |
| `S` save-and-flee, then reload | the plain set survives **and** the `AutoSave` set it replaced is **gone** — deletions cross too, which is what stops a dead character reappearing on the Continue menu |
| Populate cost at startup | **12–25ms** for 4.75–5.9MB, inside a 550–850ms page load |
| Sync after an autosave | **2–30ms** |
| Write events per sync | 578–2,336 tracked writes became **4–5 syncs** |
| `.tmp` files reaching IndexedDB | **0**, `tempDeferrals` 0 — the debounce alone was enough every run |
| IndexedDB usage against files on disk | **0.85MB stored for 5.9MB of saves** — Chrome's LevelDB compresses them ~7× |
| Quota offered | 618,611MB, and `navigator.storage.persisted()` **true** |
| `verify-corpora.sh` after the `save.cpp` change | **matches golden** on both corpora |
| `compare-targets.sh` | **targets agree** — the node host still resolves `"./"` |

**Two findings that are not about saves**, both free with driving the browser this far and both
narrowing §9.5's open item. Arrow keys, `>`, `?`, `S` and `y` all cross `GetKey` in a browser: the
command list draws, the descent works, the save prompt appears and answers. And `?seed=999`
reproduces the corpus in a browser — the page turns the query string into argv (§9.6), so the same
seven keys land on the same character in the same place, which is the row `tools/corpora/README.md`
gives for `noncombat.rec`. That is not frame equality and is not an oracle, but it makes the browser
host steerable to a known state, which it was not before.

A driver detail worth writing down, because it cost a run: **SDL tracks modifier state from real Shift
key events, not from the `shiftKey` flag on a synthetic one.** `Input.dispatchKeyEvent` with
`modifiers: 8` is not enough to send a capital `S`; the Shift press has to bracket it the way a
keyboard would. The first two attempts looked exactly like "the browser build ignores `S`", which
would have been a much more alarming finding than the truth.

**Deploying it found two things wrong with the deploy recipe**, and both are why `PORTING.md` spells
the command out. The project is `playivan`, not `ivan` — that one fails loudly. And **`--branch main`
is not optional, and it fails silently**: without it wrangler labels the upload `Preview`, prints a
URL that serves the new build perfectly, and leaves `playivan.pages.dev` on the previous production
deployment. A deploy that looks finished and changed nothing anyone visits — verified by fetching the
live `ivan.js` and finding the old size and no `IDBFS` in it. The Environment column of
`wrangler pages deployment list` is what says so. The general lesson: **check the deployed bytes, not
the deploy command's exit code.** `curl` the live `ivan.js` for the build id — `--profiling-funcs` and
§9.6's git-describe stamp make that a one-line check, and it is the only thing that distinguishes a
deploy from an upload.

**What is open.** The death path is not tested end to end: deletion is proven both ways, in the
contract test and by save-and-flee removing the `AutoSave` set, but nobody has died in a browser and
confirmed `RemoveSaveFile` clears the whole set out of IndexedDB. Nothing has been played long enough
to be big — every measurement here is one or two levels, a full run is tens of megabytes, and while
the quota is six hundred gigabytes and populate is 12ms at 6MB, neither number has been taken at
50MB. And a save cannot be got out of the browser: savediff is the sharpest tool in this repo for
anything that does not reach the screen, and a browser save is currently unreachable by it. An
`ivanSaves.export()` beside `ivanHarness.saveRecording()` would close that, and would make a crash
report carry the state §9.6 records as its one gap.

### 9.11 Two reads of memory the program never wrote

Adding a third corpus that reaches the second dungeon level (`autoplay-2000`) immediately broke
`compare-targets.sh`, in two independent ways and both past key 1559. Neither was a regression and
neither was reachable by the 210-key corpus, because both live on the path a level takes when it is
saved and read back, and neither shorter corpus ever reloads a level.

Replay the corpus four ways and compare traces:

| | frames | first disagreement |
|---|---|---|
| native, `--trace --text --shot` | 2,699 | — |
| native, `--trace` only | 2,676 | frame 1562, **7 draws** |
| WASM, `--trace --text --shot` | 2,681 | frame 1562, **7 draws** |
| WASM, `--trace` only | 2,681 | byte-identical to WASM `+text` |

Every one of those four is deterministic *against itself*. They disagree with each other.

**The 1,600 draws were not an unsequenced expression.** The native/WASM split is at frame 2200 and
1,600 is exactly one 80×20 level, so the first reading was §9.4(a) again. It is not. Dumping
`__builtin_return_address` for every draw in the window and resolving the histogram put 3,200 of the
4,114 draws at one site: `char.cpp:3391`, the loop in `character::AutoPlayAINavigateDungeon` that
drains every square of the level looking for one it can path to, one draw per square. Native drained
3,200 squares, WASM drained 1,600.

The cause is four lines away, in `character::AutoPlayAICheckAreaLevelChangedAndReset`:

```cpp
static area* areaPrevious=NULL;
area* Area = game::GetCurrentArea();
if(Area != areaPrevious){          // "am I somewhere new?"
```

Leaving a level deletes it, and the next level is free to land on the address the last one just freed.
Under Emscripten's dlmalloc it does: descending to UT 2 reused the address UT 1 had released, this
test saw no change, and the cached `vv2AllDungeonSquares` kept **1,600 dangling `lsquare*` into the
deleted level**. Native's glibc handed back a different address, took the branch, and refilled the
vector with UT 2's real 3,200 squares — UT 2 is 160×20, which is where the second 1,600 came from. So
the two targets were not disagreeing about randomness at all: one of them was walking a freed level.
Fixed by asking the dungeon and level indices, which do not depend on the allocator, and keeping the
pointer test as well so a reloaded game — every address new, no index changed — still resets.

**`--text` was the same shape of bug, one level up.** With that closed, three of the four rows agreed
and native `+text` still did not. `harness::RecordText` only copies strings, so the suspicion was
again an unsequenced draw; again it was not. The extra draws were retries inside
`level::GetRandomSquare` called from `GenerateNewMonsters`, and logging the decision showed both runs
proposing the *same* square (36,14) for the *same* monster (a mushroom) in the *same* room (6) — and
`room::DontGenerateMonsters()` answering `0` in one run and `1` in the other.

```cpp
room() : LastMasterSearchTick(0), MasterID(0) { }              // Flags uninitialised
void room::Save(outputfile& F) const { F << Pos << Size << Index << DivineMaster << MasterID; }
void room::Load(inputfile& F)        { F >> Pos >> Size >> Index >> DivineMaster >> MasterID; }
```

`Flags` — which carries `NO_MONSTER_GENERATION`, and which four rooms of the Underwater Tunnel script
set — is initialised nowhere, written by nothing, and read by `DontGenerateMonsters()`. It is set once
at generation (`level.cpp:471`) and lost the first time the level round-trips through a save. **Every
reloaded room has been reading that word out of whatever the allocator last left there**, and
`--text`'s extra `festring` allocations were enough to change the answer. Fixed by initialising it and
by putting it in `Save`/`Load`, which is a format change: `SAVE_FILE_VERSION` 137 → 138. `Master` got
an initialiser in the same constructor; it is guarded by `LastMasterSearchTick`, which is 0, and
`game::GetTick()` is also 0 on the tick a replay starts on.

`compare-targets.sh` reports targets agree on all three corpora now, and all four rows above are
byte-identical at 2,676 frames. Of the three corpora only `autoplay-2000`'s goldens moved, which is
the check that the fixes are as narrow as they claim: the other two never reload a level.

The save set crossed with it, and that is the sharper result — savediff had been reporting every level
file on `autoplay-2000` as divergent between the targets:

| `autoplay-2000` save set, native vs WASM | before | after |
|---|---|---|
| `40`, `AutoSave.40`, `AutoSave.41` (level files) | **DIFF**, up to 537 of 537 blocks | **SAME** |
| `AutoSave.wm`, `wm` | `AutoSave.wm` **DIFF**, 22 of 27 blocks | **SAME** |
| `sav` | SUSPECT, 1 block | **SAME** |
| `AutoSave.sav` | **DIFF**, 12 of 45 blocks | SUSPECT, 1 block — `GetTimeSpent` |

The one survivor is the wall-clock second boundary no determinism work removes. Everything else that
had been different is byte-identical, which is what says these two defects were the whole of it rather
than the first two of several.

**What to take from it.** Both are §9.4's fourth class, and neither yields to the first instinct the
numbers invite:

- **An exact multiple of the level size is not proof of an unsequenced draw.** It was the size of a
  cache nobody had invalidated. Get the call-site histogram before believing the arithmetic.
- **A raw pointer is not an identity.** `ptr != previous` answers "is this a different object?" only
  while the old object is alive. Across a free it asks the allocator, and the two targets have
  different ones. Anything that survives a delete needs a name the allocator does not pick.
- **A `--text`-only difference deserves the same alarm as a cross-target one.** It says the golden is
  not what the game does, it is what the game does *while being watched* — and here it was pointing at
  a real bug in the game, not at the harness.

### 9.12 A build for the page's own half, and a browser to test it in

The four JavaScript files in `tools/web/` were 1,795 lines with no build, no type checker, no linter
and no lockfile, plus 221 more inside `shell.html` that nothing could even lint. That was survivable
while the page owned three features. It stops being survivable at the next step, because graphics,
input and the UI are a different order of magnitude — §9.1 puts the SDL surface alone at ~1,800 lines
— and because of what those files *were*: emcc `--pre-js` link inputs.

**Being a link input costs three things.** Every edit costs a full relink, and CMake can only see the
file through a hand-maintained `LINK_DEPENDS` string whose failure mode is silent — the build reports
itself up to date and the page keeps serving the previous copy. The module graph is the *order of the
flags*: `music.js` borrowed the `AudioContext` that `sfx.js` owned, and nothing but the order of two
`--pre-js` arguments enforced it. And no bundler can exist, so there is no TypeScript, no npm
library, no source map and no content-hashed filename — which is why `dist.py` sends `no-cache` on
the JS it deploys.

`web/` is that half as a project; `web/README.md` is its operating manual. The toolchain choices and
what each buys are there. **All four files have crossed, `tools/web/` has no JavaScript, and the
build has no `--pre-js`.** `LINK_DEPENDS` is down to `shell.html` alone, an edit to any of the page's
code costs a bundle measured in milliseconds rather than a relink CMake could only be told about by
hand, and `ivan.js` lost 6,076 bytes.

**The bridge was undocumented and nothing checked it.** An `EM_JS` body is a string of JavaScript
pasted into `ivan.js`. Nothing type-checks it, so `Music.setVolume(Level)` resolving to `undefined`
is a **silent no-op**, not an error — and the corpora structurally cannot see it, because a headless
replay makes no sound. This is the same class of failure §9.7 and §9.9 kept running into, and the
reason `dist.py` parses `SoundEffects.cfg` instead of globbing `Sound/`. So the targets are declared
once in `web/src/bridge/contract.ts` and checked from both ends: `contract.test.ts` parses the
`EM_JS` blocks out of `FeLib/Source`, `audio/` and `Main/Source` — brace-counting, not a regex that
stops at the first `}` — and diffs them against the declaration **in both directions**, so a call
that is not declared fails and a declaration nothing calls any more fails too. `e2e/boot.spec.ts`
asserts the live page has each one. Its first run found two bridges no document mentioned:
`IvanMusicVolume` (`audio.cpp:587`) and `IvanMusicPlaying` (`audio.cpp:628`). The count was three;
the answer is six.

**There is a second contract and it is nearly closed.** `ivanSfx` has six methods and three consumers
wanting different subsets: the C++ calls only `play`, while the music module and `shell.html` call
`context()` and `master()` — music borrows the context rather than opening a second one, and the mute
button drives the shared master gain. `contract.ts` covers the C++ → page direction only, so dropping
or renaming `context` or `master` during a port would silence the music and break mute with no error
and no failing test. Both are declared on `IvanSfx` in `web/src/bridge/globals.d.ts` now, so the
compiler holds the callee, and the caller is inside the tree `tsc` reads. `shell.html` is the piece
still outside it, and the piece that has been untestable all along.

**The browser suite exists because the goldens are about to stop covering the screen.** Nothing in
this repo had ever tested a browser. That gap was tolerable while rendering was C++ and will not stay
tolerable, for a structural reason rather than a matter of diligence: the golden traces work because
rendering is software into a `bitmap` double buffer and `TraceFrame()` hashes it *before*
`PrepareBuffer()`. **As graphics and the UI cross into `web/`, the subject of that hash crosses with
them.** `verify-corpora.sh` will keep passing and will stop being evidence about what a player sees.
It is not being weakened; it is being narrowed, silently, by work happening elsewhere.

`web/e2e/` runs against an assembled `dist/` behind `serve.py` rather than a dev server, so what it
exercises is what gets deployed: real wasm, real IDBFS, real autoplay policy. `serve.py` is the more
forgiving of the two hosts because it answers byte ranges where Cloudflare Pages does not (§9.9), so a
range-related failure there would be a real one in production too. **Not a pixel comparison, yet** —
the main menu fades in and the seed varies, so a golden image needs a fixed seed and a settled frame
first. That is the obvious next assertion and the one that would actually replace what the traces are
losing.

**What the crossings taught, in the order they happened.**

- **Sfx crossed first and the full flip happened there rather than at the last one.** The plan was for
  the bundle to take `sfx.js`'s position on the emcc command line as a `--pre-js`, with the
  `<script>` deferred to a later commit gated on `addRunDependency`. That was wrong in a way worth
  recording: `build.mjs` already emits an IIFE whose documented reason for existing is that it is
  *not* a `--pre-js`, so making it one would have contradicted the artifact it builds.
- **Ordering moved from the command line into the module graph.** When sfx crossed alone what carried
  the sfx-before-music dependency was the shell's `<script>` running before `ivan.js` plus a lazy
  `globalThis.ivanSfx` read on the music side. Now both are in one bundle it is an `import` and a
  call-time lookup — the only form of this dependency a reader can see without opening a build file.
  The `<script>` position still matters: it sits after the `Module` literal and before
  `{{{ SCRIPT }}}`, so the globals exist before `ivan.js` runs and whatever crosses next has `Module`
  there to hang a run dependency on.
- **Music was the easy second crossing, and that is why it went second.** 674 lines touching no
  Emscripten internal at all: no `Module`, no `FS`, no run dependency, nothing substituted by
  `configure_file`. The whole of its coupling was `globalThis.ivanSfx` in and `globalThis.ivanMusic`
  out.
- **The harness crossed third and the design decision it was supposed to need had already been
  made.** The blocker recorded for it was that `harness-pre.js.in` "sets `Module.arguments` before the
  runtime starts and is generated by `configure_file`". Both facts were true and neither was a
  blocker: the `<script>` position sfx established is *already* before the runtime starts, and
  `configure_file` had been replaced before anybody noticed — `build.mjs` had defined both
  `IVAN_BUILD_ID` and `IVAN_CRASH_ENDPOINT` since sfx crossed. **The general lesson is about the
  note-taking rather than the port: a blocker recorded once is not re-checked when the ground under it
  moves.**
- **That forwarding was worse than nothing, and it took a measurement to see it.**
  `Main/CMakeLists.txt` ran the bundle through
  `cmake -E env "IVAN_CRASH_ENDPOINT=${WASM_CRASH_ENDPOINT}"`. With the cache variable at its empty
  default that *sets the variable to empty*, so an ambient `IVAN_CRASH_ENDPOINT` was clobbered and the
  override `build.mjs` documents could not be used through a CMake build at all. Measured on cmake
  3.28.3:
  `IVAN_CRASH_ENDPOINT=https://x cmake -E env "IVAN_CRASH_ENDPOINT=" node -p process.env.IVAN_CRASH_ENDPOINT`
  prints an empty string, and without the wrapper prints the URL. Silent for as long as it existed,
  because no endpoint has ever been set anywhere. `harness-pre.js.in` was the only consumer outside
  `web/` of the `find_package(Git)` + `git describe` block, the `WASM_CRASH_ENDPOINT` cache variable,
  the `configure_file` and the `cmake -E env` wrapper, so the whole supply chain went with the file.
- **An `.env` file was considered and rejected.** The endpoint is not a secret: it is `--define`d into
  `ivan-page.js`, which every player downloads. `?crashlog=` already redirects a session without a
  rebuild, which is the whole dev case, and the only consumer of a baked value is a production default
  set once in CI, where `env:` is the native mechanism. An `.env` would have added a file format, a
  gitignore entry, an example to keep in sync and a `--env-file-if-exists` flag to serve one string in
  one job — and invited a real secret to be filed beside a published one. Worth revisiting at the
  third build-time knob; the count is not growing because the query string absorbs this class of
  option instead.
- **`typeof` is the seam that makes an esbuild `--define` testable.** `node --test` strips types
  rather than running esbuild, so a bare `IVAN_BUILD_ID` is a `ReferenceError` at import, before any
  assertion. `platform/build.ts` reads each through `typeof X === 'undefined' ? fallback : X`, the one
  reference form that does not throw on an undeclared name. Verified in the built bundle: esbuild
  substitutes both occurrences and folds the ternary away, leaving no `typeof` and no identifier, and
  under node an unqualified name still resolves through `globalThis` so a test can set one.
- **Saves crossed last, and the premise that had blocked it was already false.**
  `addRunDependency` and `removeRunDependency` are indeed module-scope in `ivan.js` and absent from
  `-sEXPORTED_RUNTIME_METHODS=FS,IDBFS,callMain` — but they were on `Module` anyway, in every browser
  build this repo has ever produced. `link.py:1619-1640` exports both under `FORCE_FILESYSTEM`, and
  `link.py:1457-1461` sets `FORCE_FILESYSTEM=1` for any build using `--preload-file`, which this one
  does three times over. **Grepping the generated `ivan.js` for `Module["addRunDependency"]=` was the
  whole investigation and it should have come first.** The flag names the pair explicitly now, because
  a page that depends on a side effect of preloading a directory is one `--preload-file` away from a
  Continue menu drawn over saves that had not arrived.
- **The run dependency was the right mechanism and the alternative was never attractive.** It is
  Emscripten's only documented way to defer `run()`, and it is not foreign machinery bolted on beside
  the startup: `file_packager.py:992` holds `ivan.data` back through exactly the same call. What
  changed is that a property lookup on a shared object replaced a name in `ivan.js`'s own scope, which
  is strictly better — the old form worked only because the file was pasted into that scope, and it
  failed silently for anything that was not.

**Ported tests grew coverage, and mutation testing is what says so.** The three node suites that
crossed were each one sequential narrative sharing a single stubbed world; they are independent cases
now. `sfx.test.ts` and the harness cases are new coverage rather than ports — neither file had a test
— and what they pin is the behaviour a reader would otherwise be free to "fix": the 16-voice cap drops
rather than mixes, a failed fetch is cached as a failure, a sound more than 250ms late is thrown away,
a suspended context drops rather than queues, recording is on when nobody asked, `?record=` is a
request rather than a filename, page options *are* forwarded to a C++ parser that ignores them on
purpose, and a truncated recording keeps its head so the seed survives the cut.

Two mutations are worth recording because they found guarantees the code kept by accident. In the
harness, `Post()` editing its argument in place is invisible because `Store()` runs first — so the
trimming rule was pulled out into an exported `Wire()` and asserted directly. And in the browser
suite, **with the run dependency removed entirely the page still boots, still mounts, still syncs, and
the save still survives the reload**: twelve of the thirteen browser tests pass on a build that has
lost the one thing the saves crossing was blocked on. What moves is `Module.totalDependencies`, the
high-water mark `shell.html`'s `monitorRunDependencies` keeps — **2 with the hold, 1 without.** That
is the assertion, and without the mutation it would have been a `populateMs >= 0` that could never
fail.

**What is left to cross is C++.** Graphics, input and the UI, and none of them is a file move — they
are SDL surfaces, an SDL event loop and a C++ menu system, so each is a rewrite against a browser API
rather than a translation. The golden traces still cover all three, which is exactly the coverage this
section opened by saying they are about to lose.

**Two smaller things still open.** `shell.html`'s 221 lines are unlintable and untested, and they hold
the key handling, the progress bar, the crash panel and mute. And the GitHub actions are a release
behind — `checkout`, `setup-node`, `cache` and `upload-artifact` are all `@v4`, target Node 20 and are
being force-run on Node 24 with a deprecation warning every run. Pre-existing, unrelated to anything
above, and worth its own commit so a failure points at the right cause.
