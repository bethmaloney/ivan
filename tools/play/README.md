# tools/play — driving IVAN headlessly and looking at the result

`play.py` turns a headless IVAN into something you can steer one keystroke at a
time and then *read*. It exists because a frame hash proves two builds agree
but tells you nothing about what is on the screen, so there is no way to decide
what to press next — which made it impossible to reach any interesting game
state, and therefore impossible to test one.

## Quick start

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DWIZARD=ON -DPORTABLE_BUILD=ON
cmake --build build -j$(nproc)

python3 tools/play/play.py new --seed 999 --start   # land on the world map
python3 tools/play/play.py send down left '>'       # walk to the cave, enter it
python3 tools/play/play.py auto 200                 # let the AI play 200 turns
python3 tools/play/play.py keys                     # what send accepts
```

Every capturing command prints the **text layer** of the final screen and the
path of a **PNG** beside it. The PNG is the map and the sprites; the text layer
is the side panel, the message log, the menus and the prompts. Read both — the
font is 8x8, so the text layer is what makes a screen legible, and the PNG is
what shows you the terrain.

## Why it replays from scratch

A session is not a live process. It is a list of keys, and every command
rewrites that list and replays it from the beginning under a pinned seed. That
is what makes a session reproducible, resumable after a crash, and rewindable —
`undo` is just a shorter list. The cost is about a second and a half per
command, nearly all of it world generation.

The consequence worth remembering: the game is *always* driven from the main
menu. There is no "current position" to lose.

## Where the capture comes from

`play.py` is a wrapper. The capture lives in the harness and works without it:

```
--shot file.png   Write the screen when the run ends, plus a file.txt sidecar
                  holding every string on that screen.
--shot-dir dir    Write every frame that differs from the one before it to
                  dir/frame-NNNNNN.png, with a .txt sidecar each.
--text file       Log every string the game draws, in draw order, all session.
```

The text layer is captured in `rawbitmap::Printf`/`PrintfUnshaded`, the two
funnels every glyph in the game passes through, so it is complete by
construction rather than by enumeration. Strings drawn into an offscreen bitmap
— which is how menus, inventory lists and the message area are built — are
tagged `buf` and their coordinates are relative to that bitmap; strings drawn
straight to the screen are tagged `db`.

`--text` is the one to reach for when something scrolled past: the sidecar holds
one screen, the text log holds the whole session, including the intro story and
messages that were overwritten before you could see them. It is also the cheap
option — a few kilobytes against `--shot-dir`'s hundreds of megabytes.

**`--frames` / `--shot-dir` is expensive.** The PNGs are uncompressed, and a
session that reaches the first dungeon level draws about 440 distinct frames —
roughly 600MB. `play.py` empties the directory on each run rather than appending,
and reports the size afterwards. Use it when you need to see something that
happened mid-sequence; otherwise `--shot` plus `--text` answers most questions.

### Zooming in

The tiles are 16x16 and the font 8x8, so cropping and upscaling is often the
difference between guessing and knowing:

```bash
convert build/play-session/screen.png -crop 220x200+100+160 +repage \
        -filter point -resize 400% /tmp/zoom.png
```

`-filter point` matters — anything smoothing turns the pixel art to mush.

## Keys

Movement is the **arrow and page keys** (`up down left right ne nw se sw`), not
the digits. The in-game help lists `789/456/123`, but those are numpad
scancodes; the plain ASCII digits do nothing.

There are deliberately **no one-letter direction aliases**. IVAN's own commands
are single letters and a bare single character has to mean that character, so
that `>`, `?` and `S` work as typed — which would make `e` the eat command
rather than east. `play.py keys` prints the full table plus the common in-game
commands.

`text:word` types a word a character at a time, `0x150` is a raw code, and
`name*3` repeats.

## Sequences that are known to work

At **seed 999**:

| Keys | Where it lands |
|---|---|
| `enter*4` | world map, turn 0, nothing wasted |
| `enter*4 down left >` | Under Water Tunnel level 1, turn 3 |

Four ENTERs is exactly character creation: pick the default character, accept
the pre-filled name, dismiss the two intro screens. A fifth reaches the world
map where ENTER is not a command, and the game answers "Unknown key" without
spending a turn — harmless, but it clutters the message log.

`down left >` is **specific to seed 999**: it walks onto the cave mouth one tile
west of the start and enters. Another seed generates another island, so find its
cave by zooming into the screenshot — the entrance is an obvious dark arch.

## The wizard auto-play AI

`play.py auto N` gives the AI N turns. It appends the activation keys the first
time and then N `.` presses.

Three things are worth knowing, because none of them is guessable:

- **Wizard mode is `` ` ``, not `~`.** Backtick asks "Do you want to cheat,
  cheater? [y/N]" and needs the `y`. `~` is the auto-play toggle and is ignored
  until wizard mode is actually on, which is why `-DWIZARD=ON` alone does
  nothing.
- **One `.` is one AI action.** Auto-play is not self-driving: the key timeout
  that modes 2–4 were supposed to use is computed and thrown away —
  `SetKeyTimeout` has no callers. Good news for a harness, since it means turns
  advance on input rather than on how fast the machine is.
- **Stay in mode 1** (one `~`). At mode ≥ 2 any key that is not `.` or `~`
  switches auto-play back off.

It is durable. It resurrects on death (wizard mode forces that), resets its
navigation after 10 idle turns, and after 50 spawns and reads a scroll of
earthquake to dig itself out. 3000 turns at seed 999 reaches tunnel level 2 with
a levelled-up character, having died 20 times.

**It is not a differential corpus.** Replaying the same auto-play recording gives
5 distinct outcomes in 8 runs — different turn counts, and different maximum HP,
so different level-ups. See HARNESS.md §6.5. It is not auto-play's fault: any
corpus diverges once monsters act, and auto-play was just the first thing to get
that far. Until it is fixed, treat auto-play as a crash fuzzer, not an oracle.

If you measure this yourself: **give every run its own directory** and **use at
least 8 runs**. The divergence is flaky, and pairs agree by chance often enough
to produce confident wrong answers.

## Session directory

Defaults to `build/play-session`, overridden with `--session`. The game is a
portable build, so it writes its config, saves and bone files into the directory
it is launched from; the session directory is that directory, with the read-only
data directories symlinked in. Nothing lands in the repository, and deleting the
session directory really does reset everything.

```
session.json   seed and key list — the actual state
session.rec    regenerated from session.json every run
screen.png     the final screen
screen.txt     its text layer
text.log       every string drawn all session
trace.jsonl    per-frame hash trace
run.log        the game's stdout and stderr
Save/          real save files, one set per run
```

`Save/` accumulates one timestamped set per replay, because entering a level
autosaves. Two runs of the same key list therefore give two save sets to feed
`savediff`.
