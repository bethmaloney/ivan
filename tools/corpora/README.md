# tools/corpora — the committed differential corpora

Three recordings, their golden traces and golden text logs, and the scripts that
replay and check them. This is the oracle PORTING.md is built around, in the one
form that makes it an oracle: committed artifacts rather than instructions for
regenerating some. A fourth recording lives under `effects/` and has no golden on
purpose — see below.

```bash
tools/corpora/verify-corpora.sh            # 8 runs each, self-consistency + golden
tools/corpora/verify-corpora.sh -n 1       # quick smoke test
tools/corpora/verify-corpora.sh --update   # rewrite the goldens from this build

tools/corpora/compare-targets.sh           # native vs WASM, all three corpora
tools/corpora/compare-configs.sh           # one build at thirteen display configs
tools/corpora/fuzz-visual.sh               # one build at six --visual-seed values
```

Four scripts, four questions: *did this build change?*, *do these two builds
agree?*, *does one build agree with itself when the player configured it
differently?*, *does anything the game draws decide anything the game keeps?*
Only the first is in CI.

Each run writes two traces. `game.jsonl` is sampled from `game::Run`'s loop, one
record per step of the game, and carries no screen-derived quantity -- not even a
frame number. `frames.jsonl` hashes the double buffer, one record per frame whose
pixels moved. They were one file until §6.10d; splitting them is what lets the
game trace survive graphics crossing into `web/`, and what lets `fuzz-visual.sh`
vary the picture while asserting the game held still.

About twenty seconds for all three at the default eight runs — 18.6s wall, 86s
user, on 22 idle cores. *Those figures predate docs/port-log.md §7.12's
re-recording, which shortened `autoplay-2000` from 49,151 game steps to 36,710;
re-measured on a 4-core container the three take 24s.* `fuzz-visual.sh` is 24s for four corpora at six seeds. Over half of that is `autoplay-2000`: one run of it
costs 9.9s against 2.4s and 2.0s for the other two. It was twelve seconds for
all three while that corpus made 6.3M random draws; §6.10's brackets moved it to
10.7M and §6.10c's second generator took it back to 10.3M, the difference being
the draws the brackets used to make and throw away.

## The corpora

All three are seed 999 and all start with `enter*4`, which is exactly character
creation — pick the default character, accept the pre-filled name, dismiss the
two intro screens. Each is a prefix of the next.

| | keys | lands on | exercises |
|---|---|---|---|
| `noncombat.rec` | 7 | UT lvl 1, turn 3, HP 37/37 | world gen, character creation, level gen, descent |
| `autoplay-200.rec` | 210 | UT lvl 1, turn 192, HP 35/35 | the above plus combat, item use, equipment, hunger, death |
| `autoplay-2000.rec` | 2010 | UT lvl 2, turn 1703, HP 28/36 | the above plus a **second dungeon level** — level 2 generation, both directions of the stairs, and the turn-boundary autosaves |

`left up >` is **specific to seed 999**: the start tile is New Attnam itself, and
the cave mouth is one north of the tile west of it. Another seed generates another
island — and so does the same seed after any change that moves the game's stream
before world generation, which is why all three were re-recorded for
docs/port-log.md §7.12. It was `down left >` until then.

`autoplay-200.rec` extends `noncombat.rec` with `` ` `` `y` `~` (wizard mode,
confirm, auto-play mode 1) and then 200 `.` presses, one AI action each. Stay in
mode 1 — at mode ≥ 2 any key that is not `.` or `~` switches auto-play back off.

`autoplay-2000.rec` is the same again with 2000 presses instead of 200. It costs
about 4x the wall clock of the other two together, and it is the only corpus that
leaves the first dungeon level: the AI descends to UT 2 in the last quarter of
the run and crosses back and forth afterwards. That is what makes it the one to
reach for when the thing under test scales with the save set or with level size —
the browser save path (docs/port-log.md §9.10) is measured on it, because on
`autoplay-200` the save set never exceeds one level and the cost being measured
never shows up.

### Check values

These are the numbers that say a regenerated corpus is *the same* corpus. They
are recorded across docs/port-log.md §6.6a–d as the check values for those fixes.

| | noncombat | autoplay-200 | autoplay-2000 |
|---|---|---|---|
| game steps (`game.jsonl`) | 426 | 5,497 | 36,710 |
| trace frames (`frames.jsonl`) | 483 | 762 | 2,886 |
| cumulative RNG draws | 43,422 | 774,406 | 4,757,433 |
| game-stream draws (`grng`) | 43,422 | 774,406 | 4,757,244 |
| final HP | 37/37 | **35/35** | **28/36** |
| final turn | 3 | 192 | 1,703 |

These moved once, deliberately, in the commit that made the game compiler
independent (docs/port-log.md §9.4). The key sequences did not change and neither did
the seed; what changed is that a dozen expressions used to let the compiler
decide which random draw went where, so the world these 210 keys generate is
now the same world under GCC and under Clang instead of two different ones. The
old values (441 / 679 frames, 1,670,383 draws, HP 29/37, turn 202) are the
GCC 13 reading of the same corpus and appear throughout §6.6a-d.

The `autoplay-2000` column moved a second time, in the commit that fixed the two
defects in §9.11 below. Only that column moved — both fixes are on the reload
path and neither shorter corpus ever reloads a level, which is the check that
says the fixes were as narrow as they claimed. Its old values (2,698 frames,
5,038,226 draws, HP 41/43, turn 1,750) describe a run in which a room read its
no-monster-generation flag out of freed heap.

`game steps` and `trace frames` are new rows: §6.10d split the one trace into
two, and `trace frames` is no longer the count it was, because a record is now
emitted when the pixels move rather than when the pixels or the draw count move.
`autoplay-2000` reads 2,741 where the single trace read 2,758.

All three `cumulative RNG draws` figures moved a fourth time, in the commit that
gave presentation its own generator (§6.10c) — and *only* that row moved. The
brackets' draws stopped being made rather than being made and discarded, so
`grng` did not move a single row and neither did any text log. What did move is
the frame hashes on the 290 of 596 `autoplay-200` records and 744 of 2,759
`autoplay-2000` records where a drip or an explosion drew; `noncombat` reaches
none and its hashes are untouched.

Both auto-play columns moved a third time, in the commit that put
`femath::SaveSeed` brackets around the random draws of four visual effects
(docs/port-log.md §6.10): `fluid::imagedata::Animate`'s blood drip,
`level::DrawExplosion`'s mirror flag, `lsquare::DrawParticles` and
`lsquare::DrawLightning`. Those draws were being made on the *game's* stream
once per on-screen stained square or explosion, so the player's
`DungeonGfxScale` decided how many of them happened and two players sharing a
seed did not share a game. Moving these goldens is what fixing that costs, and
it is not a small correction: the drip alone runs 2,048 times on
`autoplay-200` and 6,382 times on `autoplay-2000` (gdb breakpoint counts), and
none of those draws is on the game stream any more.

`noncombat` is byte-identical to its previous golden, which is the check that
says the change was as narrow as it claims — it reaches none of the four
functions, 0 hits on all of them. Both auto-play corpora first differ at frame
381, and that frame is the signature of the fix rather than of a behaviour
change: the frame hash is unchanged (`309fecf0a8598d40`) and `rng` is unchanged
(1,119,452) while `grng` drops by exactly five, 1,119,408 to 1,119,403. Five
draws moved off the game stream; the same total was drawn and the same pixels
came out. The first hash change is one frame later, and the trajectory shifts
from there, which is how a five-draw difference becomes 36 more turns on
`autoplay-200` and 209 more on `autoplay-2000`.

`compare-configs.sh` is the check that says the property was actually bought:
every corpus now ends on the same `grng` at all six `DungeonGfxScale` values,
where before `autoplay-200` ended at 1,714,427 at scales 1-5 against 1,813,687
at 6. Read that narrowly — it is one integer per corpus, over four recordings,
at one window size, with `EnhancedLights` pinned off, and §6.10a is the list of
what it still cannot see.

One thing that move cost, worth knowing before you rely on either auto-play
corpus for the death path: the characters now survive. Distinct death phrases
fell 12 → 2 on `autoplay-200` and 14 → 2 on `autoplay-2000`, while distinct
"is slain" subjects rose 1 → 4 and 6 → 8. The keys are unchanged, so this is
the new trajectory rather than a different recording.

`nest` must be 0 on every frame of both. If it ever goes positive the single
`mtb` backup slot is corrupting the game stream — docs/port-log.md §6.5a.

## `effects/` — recordings with no golden, on purpose

`effects/beams.rec` is 84 keys: the same seed 999 prefix, wizard mode, `1` five
times so the character survives its own beams, `$` for scrolls of wishing, three
wishes and two zaps. It exists because **no committed corpus casts anything**, so
without it `compare-configs.sh` would be asserting a property that holds
vacuously.

It has no golden trace and no golden text log, and **must not acquire one**. What
it exercises is a wand animation that is meant to look different at a different
zoom; its assertion is cross-arm equality rather than equality against a
committed artifact, and keeping it out of `verify-corpora.sh`'s `*.rec` glob
keeps it out of the way of the three corpora that do have goldens. It is outside
`compare-targets.sh`'s glob for the same reason, which is what that decision
costs: the two `lsquare` sites are the only ones in §6.10 never compared
native-vs-WASM.

Measured, it reaches `lsquare::DrawParticles` 6 times and `level::DrawExplosion`
once, and `lsquare::DrawLightning` **not at all** — the fireball zap detonates a
gas grenade, warp gas teleports the character across the level, and the last key
lands on an apply prompt instead of firing the wand. So re-record it, wishing the
lightning wand *last*; do not give it a golden. §6.10b.

## Regenerating

The corpora are generated by `tools/play/play.py`, not by hand:

```bash
python3 tools/play/play.py --session build/corpus-session new --seed 999 --start
python3 tools/play/play.py --session build/corpus-session send down left '>'
cp build/corpus-session/session.rec tools/corpora/noncombat.rec

python3 tools/play/play.py --session build/corpus-session auto 200
cp build/corpus-session/session.rec tools/corpora/autoplay-200.rec

tools/corpora/verify-corpora.sh --update
```

`autoplay-2000.rec` is the same prefix with a single larger `auto`, from its own
session directory so the 200-key corpus above is not disturbed:

```bash
python3 tools/play/play.py --session build/corpus-session-2000 new --seed 999 --start
python3 tools/play/play.py --session build/corpus-session-2000 send down left '>'
python3 tools/play/play.py --session build/corpus-session-2000 auto 2000
cp build/corpus-session-2000/session.rec tools/corpora/autoplay-2000.rec

tools/corpora/verify-corpora.sh --update
```

`auto 2000` rather than `auto 200` followed by `auto 1800`: both produce the same
key list, but `play.py` replays the whole session on every command, so the split
pays for the long replay twice.

Check the HP against the table above before trusting a regenerated corpus.

## The two divergences past key 1559 — both found here, both closed

Recording this corpus produced two disagreements that no earlier corpus could
reach, because both live on the *reload* path and neither shorter corpus ever
reloads a level. They are written up in docs/port-log.md §9.11; the short version is
that one was `--text` changing the native run and the other was native and WASM
parting company by 1,600 draws, and both turned out to be reads of memory the
program never wrote. Neither was an unsequenced RNG draw, which is what the
1,600 first suggested.

`compare-targets.sh` now reports `targets agree` on all three corpora, and all
four ways of replaying this one — native and WASM, with and without `--text` —
produce byte-identical traces. That last check is the one worth re-running after
any change near saving, level entry or the auto-play AI:

```bash
tools/corpora/compare-targets.sh
```

A `--text`-only difference is the harness perturbing what it measures, and it
is worth taking as seriously as a cross-target one: it means the golden is not
"what the game does", it is "what the game does while being watched".

## Two things that will bite you

**The harness options are pinned in `run-corpus.sh`.** docs/port-log.md §6.6 records
that changing them changes the process's allocation history, which used to change
the run with it. That sensitivity is closed, but a golden trace only means
anything if the command line that produced it is fixed. Change the options and
every golden must be regenerated.

**Use at least 8 runs, and give every run its own directory.** Both rules were
learned by getting them wrong. The divergences this harness finds are flaky
enough that pairs agree by chance — §6.5a records two confidently wrong
diagnoses drawn from two-run comparisons, both of which evaporated at 6–8
samples. And a portable build writes `Save/`, `SndDebug.txt` and
`.QuestionHistory_*.txt` into the directory it is launched from, so runs sharing
one contaminate each other. `run-corpus.sh` enforces both.

## What is deliberately not checked

**Save files.** `verify-corpora.sh` compares traces, text logs and screenshots,
not saves. Level files and `.wm` do reproduce now (§6.6d — eight ordinary runs,
one distinct level file, no fixed heap fill and no ASLR trick), but `.sav`
carries `GetTimeSpent`, one byte recording whether the replay crossed a
wall-clock second. On `autoplay-200` it reads 1 or 2 in `.sav` and 2 or 3 in
`AutoSave.sav` depending on machine load: eight runs on an idle machine give
one distinct file of each role, eight against a saturated one give two, and
both are expected. Use `savediff --ignore-timespent` for save comparison; it
pairs files by role rather than by name, which matters because `game::SaveName`
stamps the stem with a timestamp. `compare-targets.sh` runs it across the two
targets and reports it without letting it decide the exit status; since §9.11
every level file and `.wm` of all three corpora is byte-identical there, and
`GetTimeSpent` is the only thing left that differs.

**Any configuration but the compiled-in defaults.** `run-corpus.sh` writes no
config file at all unless `IVAN_CONF` is set, which is what makes every golden
comparable — and is also why the player's zoom sat in the game's random stream
for the whole port (§6.10). `compare-configs.sh` is the only thing that varies a
setting, it varies one (`DungeonGfxScale`), and it pins `EnhancedLights` off
where the shipped default is on. Window size, in particular, is never varied.

**Anything outside these three corpora.** Every determinism number in PORTING.md
is a property of the levels these key sequences generate. `autoplay-2000` reaches
a second dungeon level; none of them visits the whole world map. Widening the
corpus is how the remaining coverage qualifier gets retired.
