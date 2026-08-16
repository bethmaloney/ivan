# tools/web — collecting and diagnosing a browser crash

The goal is that an ordinary session, played with no foresight and no special
flags, produces enough on a crash to find the bug. That means two things have to
be true *before* the crash, because neither can be arranged afterwards:

- the binary has to carry function names, or the trap prints byte offsets that
  nothing can resolve;
- the session has to have been recording, or there is no way to reproduce it.

Both are on by default in a `WASM_BROWSER` build. Nothing below needs enabling.

## What you get when it crashes

The console prints a report and it is also saved to `localStorage`, so it
survives the reload or the closed tab that usually follows:

```
ivan: unhandled rejection
RuntimeError: memory access out of bounds
    at character::Move (ivan.wasm:0x...)
    ...

--- crash report ---
build   v059-55-gf7d61e1
seed    1755312345
keys    403
stored  ivanHarness.reports() / ivanHarness.save()

Reproduce it natively:
  ivanHarness.saveRecording()   then
  ./ivan --replay session.rec
```

From the console:

```js
ivanHarness.reports()        // every stored report, newest first
ivanHarness.save()           // newest one as a .json file
ivanHarness.saveRecording()  // just the .rec, ready for --replay
ivanHarness.text()           // the live recording, crash or no crash
ivanHarness.report('note')   // file one for something that did not crash
ivanHarness.clear()          // forget them
```

**The recording is the valuable part.** A stack says where it stopped; the
recording says how to get back there. It carries the seed in its header, so

```bash
./ivan --replay session.rec
```

replays the session on the native build, where gdb, valgrind and ASan all apply.
That works because `harness::RecordKey` flushes every key as it writes it
(`FeLib/Source/harness.cpp:545`) — a trap leaves the recording complete but for
its `# end keys=` trailer, so the keys that led to the crash survive it.

Recording changes nothing about how the game plays. The seed it pins is the same
`time(0)` the game would have used anyway (`Main/Source/main.cpp:154`); it just
writes it down. Opt out with `?record=off` if you ever need to.

## Why a stripped trap is not a lead

```
Uncaught (in promise) RuntimeError: memory access out of bounds
    at ivan.wasm:0xed4dd
```

Those are byte offsets. A build without `--profiling-funcs` has no name section
and no DWARF, so **nothing** maps them back to a function — not `emsymbolizer`,
which needs DWARF; not the browser; and not a later rebuild, because the offsets
belong to the exact binary that produced them and even that binary no longer
knows its own function names. The information was never emitted.

This is why names are unconditional for browser builds rather than an option:
the cost is ~800KB paid once at build time, and the loss is a report from a
crash nobody can reproduce to order. `--profiling-funcs` changes no codegen.

Match the report's `build` field against the binary before trusting a trace.
Symbolizing one build's offsets against another's gives confident, wrong
answers, which is worse than no answer.

## Turning the screws further

`-DWASM_SAFE_HEAP=ON` bounds-checks every load and store, naming the offending
access and address outright. It is slow enough to make the game hard to steer by
hand, so use it with a recording that already reproduces the crash rather than
while hunting for one:

```bash
emcmake cmake -S . -B build-web-safe -DCMAKE_BUILD_TYPE=Release -DWIZARD=ON \
  -DPORTABLE_BUILD=ON -DWASM_BROWSER=ON -DWASM_SAFE_HEAP=ON
```

`-DWASM_DEBUG=OFF` drops `ASSERTIONS=2` and `STACK_OVERFLOW_CHECK=2` if you want
the faster build back. It defaults ON for browser builds and OFF for node, where
the differential corpora would pay the cost on every run.

## If it will not reproduce natively

That is a finding, not a dead end. wasm32 traps on accesses x86-64 absorbs: a
read past a buffer is still inside a mapped page natively and returns garbage,
where in wasm it is either outside linear memory or caught by `SAFE_HEAP`. Same
for a null dereference, and for a `long` that was 8 bytes when a save was written
and is 4 here (HARNESS.md §6.8, §7.9).

So a WASM-only crash usually means a real latent defect that native testing
structurally cannot see — the class HARNESS.md §6.6 and §9.4 were about. Reach
for `SAFE_HEAP` plus the recording, and compare a native `--trace` of the same
recording against the WASM one to find the last frame they agreed on.

## Sending reports somewhere

Reports are local-only by default: `localStorage` and the console, with
`ivanHarness.save()` to get one out by hand.

The POST hook is wired but inert. Set an endpoint at build time, or per-session
without a rebuild:

```bash
emcmake cmake ... -DWASM_CRASH_ENDPOINT=https://example.com/ivan-crash
```

```
ivan.html?crashlog=https://example.com/ivan-crash
```

It sends the whole report as JSON, `keepalive` so it outlives the page — which a
crash is usually followed by. `keepalive` caps a body at 64KB, so a long
recording is dropped from the POST and kept locally rather than losing the
report; the key count says so. Nothing is sent when no endpoint is set.

---

# sfx.js — sound effects, played by the page

`sfx.js` is the other half of the browser build's JavaScript, and it has
nothing to do with crashes. The wasm module decides *what* to play; this
decides *how*. HARNESS.md §9.7 has the design argument; this is the operating
manual.

## Where the boundary is

Everything up to and including the choice of file stays in C++ —
`Sound/SoundEffects.cfg`, its 153 patterns, the regex match against the message
text, and the private xorshift that picks between several files for one
pattern. What crosses is a path, through an `EM_JS` bridge in
`FeLib/Source/sfx.cpp`:

```
soundeffects::playSound("The dog bites you!")   [C++]
  -> findMatchingSound  -> "bark.wav"
  -> IvanSfxPlay("./Sound/bark.wav", 127)       [bridge]
  -> ivanSfx.play(...)                          [JS: fetch, decode, schedule]
```

Fire and forget: no return value, nothing calls back into wasm, so asyncify has
nothing to unwind and a slow fetch cannot stall the frame that asked for the
sound. Everything that can fail — a missing file, a decode error, a context the
autoplay policy has not released — fails on the JS side and is silent, which is
what the SDL_mixer path does with a null chunk.

## From the console

```js
ivanSfx.stats()     // {played, dropped, failed, cached, voices}
ivanSfx.played()    // the last few hundred paths, newest last
ivanSfx.state()     // AudioContext state, or 'none' before the first sound
```

```
ivan.html?sfx=off              never play anything (still records what would have)
ivan.html?sfxbase=<url>        fetch from somewhere other than the page's Sound/
```

`played()` is the useful one when something is wrong, because it records the
call whether or not a sound came out. A path in `played()` with `stats().played`
not moving means the module and the bridge are fine and the problem is the
fetch, the decode or the context.

## When it is silent

In the order worth checking:

1. **`ivanSfx.played()` is empty.** Nothing is crossing the bridge, so the
   problem is in C++, not here. Almost always `SoundState`: `initSound` reads
   `Sound/SoundEffects.cfg` out of MEMFS, and if the preload is missing it
   settles on `-1` and `playSound` returns before the bridge. Nothing is
   printed when this happens. Check the file is in the package:
   `grep -c SoundEffects.cfg build-web/Main/ivan.js`.
2. **`stats().failed` is climbing.** The wavs are fetched over HTTP, not read
   from `ivan.data`, so `Sound/` has to be served beside `ivan.html`. The build
   symlinks it there; a deploy that copies only the emcc output will not have
   it. One console line per missing file per session.
3. **`state()` is `suspended`.** The autoplay policy has not released the
   context. It resumes on the first `keydown`, `mousedown` or `touchstart`;
   sounds requested before that are dropped rather than queued, deliberately —
   a suspended context does not advance `currentTime`, so queued sounds would
   all fire at once on the first keystroke.
4. **`state()` is `none`.** No sound has been requested yet, or the browser has
   no `AudioContext`.

## Editing it

`sfx.js` is a `--pre-js`, which means it is a link input CMake can only see
through `LINK_DEPENDS` (`Main/CMakeLists.txt`). That is wired, so an edit
relinks — but if you move or rename the file, wire it again. The failure mode
is silent: the build reports itself up to date and the page keeps serving the
previous copy, so the edit looks like it did nothing.

There is no `configure_file` step and nothing is substituted at build time. What
would have been build-time settings are query-string options instead, so they
can be changed without a rebuild, exactly as `?crashlog=` is.

---

# music.js — the soundtrack, played by the page

The same split as `sfx.js`, one level up: the module says what should be
playing, this plays it. HARNESS.md §9.8 has the design argument. Nothing here
synthesizes MIDI — the `.mid` are never fetched by the page.

## Where the boundary is

The playlist is the game's. `dungeon::PrepareMusic` builds it from the level
scripts and `audio.cpp` keeps it, exactly as on the native build; what crosses
is that list, plus the master volume and the intensity the game recomputes
every turn:

```
dungeon::PrepareMusic          [C++]  playlist for this level
  -> audio::LoadMIDIFile              -> Tracks
  -> IvanMusicPlaylist("Dungeon.mid,Dungeon2.mid")   [bridge]
  -> ivanMusic.setPlaylist(...)       [JS: choose, fetch, loop, mix]

character::Be                  [C++]  every turn
  -> audio::IntensityLevel(127 - worstBodyPartHP)
  -> ivanMusic.setIntensity(...)      [JS: three gain nodes]
```

One value travels back, because `PrepareMusic` branches on it:
`GetCurrentlyPlayedFile` is an `IvanMusicCurrentIndex()` readback resolved
against the playlist. It is a plain synchronous `EM_JS` returning an `int` — no
promise, no callback into wasm, nothing for asyncify.

## The stems

IVAN's music is adaptive: as the player's worst body part gets worse, some
instrument groups fade out and others fade in. `audio.cpp`'s two volume tables
only ever produce three curves, so each track is pre-rendered as up to three
OGG stems and mixed here by three gain nodes:

| stem | MIDI channels | gain |
|---|---|---|
| `const` | 0–4, 9 | full, always |
| `fadeout` | 5–8, 10 | `127 - intensity` |
| `fadein` | 11–15 | `intensity` |

Regenerate them from the `.mid` with `tools/music/render-stems.py` and commit
the result. `Music/stems.json` says which stems each track has, and is why the
page never probes for a file that was never rendered: six of the eleven tracks
have no notes at all, so "no stems" is a normal answer rather than a fault.

Unlike effects, stems are **streamed** through `<audio>` elements rather than
decoded into `AudioBuffer`s. Decoded audio is about 23MB per minute per stem,
and `Dungeon3` is 7.3 minutes — half a gigabyte for one dungeon if it were
cached the way `sfx.js` caches wavs.

## From the console

```js
ivanMusic.stats()      // {track, stems, gains, intensity, volume, drift, ...}
ivanMusic.playlist()   // what the module last handed down
```

```
ivan.html?music=off            never play anything
ivan.html?musicbase=<url>      fetch from somewhere other than the page's Music/
ivan.html?musiccurve=linear    volume as a straight ratio, not the GM square law
```

`stats().drift` is the one to watch. Three media elements keep three clocks, and
these stems are the same piece of music, so drift between them is heard as a
doubled attack rather than as a timing error. They are pulled back with a 0.2%
playback-rate nudge, which is inaudible; `corrections` counts those. If `seeks`
is climbing, the rate nudge is not keeping up and something is wrong.

## When it is silent

1. **`stats().track` is `null` and `playlist()` is empty.** Nothing crossed the
   bridge. Either the game has music off — `ChangeMIDIOutputDevice(0)`, which
   is the "Use MIDI soundtrack: no" option — or `audio::Init` never ran.
2. **`stats().track` is `null` but `playlist()` is not.** The playlist is all
   silent tracks. Normal on the main menu, the world map, and any dungeon whose
   script names `Empty.mid`; those six files are byte-identical and have no
   notes. Check `Music/stems.json`.
3. **`stats().failed` is climbing.** A stem in the manifest would not load.
   `Music/` has to be served beside `ivan.html`; the build symlinks it, a deploy
   that copies only the emcc output will not have it. Or the stems were never
   rendered — `ls Music/*.ogg`.
4. **`state()` is `suspended`.** The autoplay policy. Unlike effects, music is
   picked up when the context resumes rather than dropped, because the main menu
   asks for it before any gesture can have happened.

## Editing it

Same `--pre-js` and `LINK_DEPENDS` caveat as `sfx.js`, and one more: `music.js`
is listed *after* `sfx.js` on the command line and depends on that order. It
borrows the `AudioContext` `sfx.js` owns rather than opening a second one, so
`ivanSfx` has to exist by the time it runs.

`node tools/web/music.test.js` covers the module contract and the mixing
arithmetic against stubs — the playlist, the index readback, the
restart-or-keep rule at a level change, and intensity to gains. It needs no
browser and no rendered audio.
