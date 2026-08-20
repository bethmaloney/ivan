# web/ — the browser frontend

The half of the port that is not C++. `Main/` decides what happens; this decides
what the player sees, hears and types. Every line of JavaScript the browser runs
is bundled from here. `tools/web/` is the page emcc wraps around the module
(`shell.html`), the landing page and the tooling that assembles and serves both.

Node 24, pinned in `.nvmrc` at the repo root and in CI. With nvm:

```bash
nvm install 24       # once
nvm use              # in the repo; reads .nvmrc
```

```bash
cd web
npm ci
npm run check        # typecheck + lint + tests. No browser, 9.8s
npm run build        # -> dist/ivan-page.js
npm run e2e          # the browser suite. Needs an assembled ../dist (below)
```

Almost all of that 9.8s is deliberate sleeps, and the suites sleep because they
drive real timers rather than reaching inside a module to fake a clock. `node
--test` runs the files concurrently, so the wall clock is the slowest of them:
saves at 9.1s, of which 4.6s is one case that waits out all ten `.tmp` retries at
400ms to prove a stale temporary file cannot wedge saving for the session, and
most of the rest is six debounce waits at 320ms. Music is 3.1s behind it, from a
500ms drift interval and a 400ms alignment pass. Typecheck is 0.38s and lint
0.21s.

## How the bundle reaches the page

`Main/CMakeLists.txt` runs `build.mjs` into the build directory beside
`ivan.html`, and `tools/web/shell.html` loads it with an ordinary
`<script src="ivan-page.js">` placed after the `Module` literal and before
emcc's own script tag. Both halves of that position matter: the bundle assigns
the globals an `EM_JS` body looks up, so they must exist before `ivan.js` runs,
and it needs `Module` to already be there to hang the saves' `preRun` hook on.
`dist.py` copies it like any other build output.

The target is `ALL` and always runs rather than being dependency-tracked —
esbuild takes single-digit milliseconds, and the failure that buys off is the one
`LINK_DEPENDS` exists for: a stale bundle is silent. A browser build needs node
and `web/node_modules`; both are checked when CMake configures, so a missing one
names itself rather than failing inside a custom command.

What is left of the coupling is small and deliberate: the bundle is an IIFE that
assigns a handful of globals, because the wasm side reaches them by name out of
`EM_JS` bodies (below), and it reaches four things back on `Module` — `FS`,
`IDBFS`, `addRunDependency` and `removeRunDependency`, all named in
`EXPORTED_RUNTIME_METHODS` rather than taken from `ivan.js`'s own scope the way a
`--pre-js` could. None of this is a `--pre-js` link input any more;
`docs/port-log.md` §9.12 has what that cost and why it went.

## The toolchain, and why each piece

| | |
|---|---|
| **esbuild** | bundles and strips types. No config file, sub-100ms. Vite's value is a dev server with HMR, and a game that boots through a multi-megabyte wasm download cannot usefully hot-reload. |
| **tsc `--noEmit`** | the only type checker. esbuild does not check, and oxlint does not either. TypeScript 7, the native compiler — both projects check in 0.24s. |
| **oxlint** | one binary, no plugin tree. Correctness rules only. |
| **`node --test`** | built into Node 24, which also strips the types — so the tests need no runner, no transform and no dependency. |
| **Playwright** | the only thing in the repo that tests a browser. |

**No Prettier, and that is not an oversight.** This tree is hand-formatted —
aligned comment blocks, PascalCase locals matching the C++ house style. A
formatter would churn all of it and then fight it. `.editorconfig` at the repo
root is the formatting authority.

**Tests sit next to what they test**: `query.ts` and `query.test.ts` are siblings,
and there is no `test/` directory. `*.spec.ts` under `e2e/` is the browser suite
and a different runner, so the two never collide.

That makes the tsconfig split load-bearing rather than tidy. `tsconfig.json`
covers `src/` with `lib: es2020 + dom` and **no node types**, so an
`import 'node:fs'` in page code is a compile error — and it `exclude`s
`**/*.test.ts`, because a test beside its subject imports `node:test` and would
otherwise force node's types into the project the guard depends on.
`tsconfig.test.json` picks those files up, along with `e2e/` and the build
scripts, where node is the point. Handing `src/` the node types so the tests
could live in it would give the guard away for nothing.

`erasableSyntaxOnly` confines the whole tree to TypeScript that erases to
nothing — no enums, no parameter properties — because both things that read it
only strip types rather than compiling them.

Nothing in `src/` imports a test, so esbuild never reaches one: the bundle
follows imports from `src/main.ts`.

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

- `src/bridge/contract.test.ts` parses the `EM_JS` blocks out of the C++ and
  compares them against `contract.ts`, in both directions — a call that is not
  declared fails, and a declaration nothing calls any more fails too. Parsed
  rather than trusted, for the same reason `dist.py` parses `SoundEffects.cfg`
  instead of globbing `Sound/`: a declaration checked against itself proves
  nothing. Its first run found two bridges no document had ever mentioned, which
  is why the table above is the only one left.
- `e2e/boot.spec.ts` asserts the live page has every one of them as a function.

`contract.ts` covers the C++ → page direction only, and the gap it left was the
*page* → page one: `music.ts` calls `ivanSfx.context()` and `ivanSfx.master()` to
borrow the audio context, and `shell.html` drives the same master gain for mute.
Renaming either would have silenced the music with no error and no failing test.
Both are declared on `IvanSfx` in `src/bridge/globals.d.ts` now and the caller is
inside the tree `tsc` reads. `shell.html` is still outside it.

## The browser suite, and why it has to exist

```bash
emcmake cmake -S . -B build-web -DCMAKE_BUILD_TYPE=Release \
  -DWIZARD=ON -DPORTABLE_BUILD=ON -DWASM_BROWSER=ON
cmake --build build-web -j$(nproc)
tools/web/dist.py
cd web && npm run e2e         # 15-21s for the current thirteen
```

It runs against an assembled `dist/` served by `tools/web/serve.py`, not a dev
server, so what it exercises is what gets deployed: real wasm, real IDBFS, real
autoplay policy.

**This is what replaces the coverage the golden traces are about to lose.** The
committed corpora prove the game still plays the way it did, and they do it by
hashing the C++ `bitmap` double buffer before it reaches a texture
(`docs/port-log.md` §6.3, §9.3). That works only while rendering is C++. As
graphics, input and the UI cross into this directory, the subject of that hash
crosses with them — the traces will keep passing and cover less. Nothing else in
the repo has ever tested a browser: every node suite in here is a contract test
against stubs, and `shell.html` is untested.

They assert that the page boots without a page error, that the canvas keeps its
800×600 backing store, that it draws more than one colour, that every bridge is
present, that the console APIs exist, that **a keystroke reaches the C++ input
path** (asserted through `ivanHarness.text()`, the live recording, so it covers
the browser, the shell, asyncify and `GET_KEY`), that the query string reaches
the game as argv, that the first gesture releases the audio context, that a crash
report is built, stored, POSTed and survives a reload, that the saves are mounted
writable, that **startup was actually held back while they were read**, that **a
save survives a reload**, and that `?saves=off` still plays. Plus the list in
`ivanPage.modules`, so a module that threw on the way up is a failed assertion
rather than a quietly missing feature.

Three of those are about the saves and only one is obvious. The mount and the
round trip through IDBFS would both keep passing with the run dependency removed
entirely — measured, not assumed: with the hold taken out the page boots, mounts,
syncs and survives a reload, and the only test that moves is the one that reads
`Module.totalDependencies` and finds 1 where there should be 2.

Not a pixel comparison: the main menu fades in and the seed varies, so a golden
image needs a fixed seed and a settled frame first. That is the obvious next one.

## Sound effects — `src/audio/sfx.ts`

The wasm module decides *what* to play, this decides *how*. Everything up to and
including the choice of file stays in C++ — `Sound/SoundEffects.cfg`, its 153
patterns, the regex match against the message text, and the private xorshift that
picks between several files for one pattern. `docs/port-log.md` §9.7 argues for
that boundary; what crosses it is a path:

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

**Effects play 12dB below native, on purpose.** The wavs are mastered at full scale — 98 of the 155
decodable files under `Sound/` peak within 1dB of 0dBFS, and `blunt3.wav` is −9.2dBFS RMS under a
0dBFS peak — and both builds play them at that level, 0.07dB apart. So a gain node between the voices
and the shared master takes a quarter of it, and the master is left alone because the music hangs off
it too. `docs/port-log.md` §9.7a has the arithmetic on both sides and the numbers. `?sfxgain=1` is
native's level if you want to hear what was changed.

```js
ivanSfx.stats()     // {played, dropped, failed, cached, voices, trim}
ivanSfx.played()    // the last few hundred paths, newest last
ivanSfx.state()     // AudioContext state, or 'none' before the first sound
ivanSfx.bus()       // the effects gain node; set .gain.value to try a level by ear
```

```
?sfx=off              never play anything (still records what would have)
?sfxbase=<url>        fetch from somewhere other than the page's own Sound/
?sfxgain=<0..1>       the effects trim, default 0.25; 1 is what native sounds like
```

`played()` is the useful one when something is wrong, because it records the call
whether or not a sound came out. A path in `played()` with `stats().played` not
moving means the module and the bridge are fine and the problem is the fetch, the
decode or the context.

**When it is silent**, in the order worth checking:

1. **`ivanSfx.played()` is empty.** Nothing is crossing the bridge, so the
   problem is in C++, not here. Almost always `SoundState`: `initSound` reads
   `Sound/SoundEffects.cfg` out of MEMFS, and if the preload is missing it
   settles on `-1` and `playSound` returns before the bridge. Nothing is printed
   when this happens. Check the file is in the package:
   `grep -c SoundEffects.cfg build-web/Main/ivan.js`.
2. **`stats().failed` is climbing.** The wavs are fetched over HTTP, not read
   from `ivan.data`, so `Sound/` has to be served beside `ivan.html`. The build
   symlinks it there; a deploy that copies only the emcc output will not have
   it. One console line per missing file per session.
3. **`state()` is `suspended`.** The autoplay policy has not released the
   context. It resumes on the first `keydown`, `mousedown` or `touchstart`;
   sounds requested before that are dropped rather than queued, deliberately — a
   suspended context does not advance `currentTime`, so queued sounds would all
   fire at once on the first keystroke.
4. **`state()` is `none`.** No sound has been requested yet, or the browser has
   no `AudioContext`.

`sfx.test.ts` pins the parts that are deliberate rather than incidental and that
a reader would otherwise be free to "fix": the 16-voice cap drops rather than
mixes, a failed fetch is cached as a failure so one missing file is one console
line per session, a sound arriving more than 250ms late is thrown away, a
suspended context drops instead of queueing, and the trim is on the bus with
every voice routed through it — a voice reconnected straight to the master would
play at native's level again and nothing else would notice.

## Music — `src/audio/music.ts`

The same split as sfx, one level up: the module says what should be playing, this
plays it. Nothing here synthesizes MIDI — the `.mid` are never fetched by the
page. They are rendered ahead of time into OGG stems, which is what lets the
browser build drop RtMidi, the MIDI parser and the playback engine.
`docs/port-log.md` §9.8 has the design argument.

The playlist is the game's. `dungeon::PrepareMusic` builds it from the level
scripts and `audio.cpp` keeps it, exactly as on the native build; what crosses is
that list, plus the master volume and the intensity the game recomputes every
turn:

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
`GetCurrentlyPlayedFile` is an `IvanMusicCurrentIndex()` readback resolved against
the playlist by *name*, so it stays right across the reorder a level change
performs. A plain synchronous `EM_JS` returning an `int` — no promise, no callback
into wasm, nothing for asyncify.

### The stems

IVAN's music is adaptive: as the player's worst body part gets worse, some
instrument groups fade out and others fade in. `audio.cpp`'s two volume tables
only ever produce three curves, so each track is pre-rendered as up to three OGG
stems and mixed here by three gain nodes:

| stem | MIDI channels | gain |
|---|---|---|
| `const` | 0–4, 9 | full, always |
| `fadeout` | 5–8, 10 | `127 - intensity` |
| `fadein` | 11–15 | `intensity` |

Regenerate them from the `.mid` with `tools/music/render-stems.py` and commit the
result. `Music/stems.json` says which stems each track has, and is why the page
never probes for a file that was never rendered: six of the eleven tracks have no
notes at all, so "no stems" is a normal answer rather than a fault.

```js
ivanMusic.stats()      // {track, stems, gains, intensity, volume, drift, ...}
ivanMusic.playlist()   // what the module last handed down
```

```
?music=off             never play anything (still records what would have)
?musicbase=<url>       fetch from somewhere other than the page's own Music/
?musiccurve=linear     volume as a straight ratio, not the GM square law
```

`stats().drift` is the one to watch: it is each stem's offset from the leading
one, and drift between stems of the same piece is heard as a doubled attack
rather than as a timing error. Anything under 5ms is left alone, anything over is
pulled back with a playback-rate nudge of at most 0.5% — inaudible, and
`corrections` counts those. Past 50ms the nudge cannot close the gap in
reasonable time and the stem is seeked instead; if `seeks` is climbing in steady
state, this design is the wrong one.

**When it is silent**, in the order worth checking:

1. **`stats().track` is `null` and `playlist()` is empty.** Nothing crossed the
   bridge. Either the game has music off — `ChangeMIDIOutputDevice(0)`, the "Use
   MIDI soundtrack: no" option — or `audio::Init` never ran.
2. **`stats().track` is `null` but `playlist()` is not.** The playlist is all
   silent tracks. Normal on the main menu, the world map, and any dungeon whose
   script names `Empty.mid`. Check `Music/stems.json`.
3. **`stats().failed` is climbing.** A stem in the manifest would not load.
   `Music/` has to be served beside `ivan.html`; the build symlinks it, and a
   deploy that copies only the emcc output will not have it. Or the stems were
   never rendered — `ls Music/*.ogg`.
4. **`state()` is `suspended`.** The autoplay policy. Unlike effects, music is
   picked up when the context resumes rather than dropped, because the main menu
   asks for it before any gesture can have happened.

## The harness — `src/harness/`

Two jobs, and they are the two a browser has no command line for:

```
argv.ts     the query string -> Module.arguments, before the runtime reads it
report.ts   a crash -> a report in localStorage, on the console, and POSTed
```

The goal is that an ordinary session, played with no foresight and no special
flags, produces enough on a crash to find the bug. Two things have to be true
*before* the crash, because neither can be arranged afterwards: the binary has to
carry function names, or a trap prints byte offsets nothing can resolve; and the
session has to have been recording, or there is no way to reproduce it. Both are
on by default in a `WASM_BROWSER` build. `PORTING.md` has the harness reference
and `docs/port-log.md` §9.6 the argument.

### What you get when it crashes

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

```js
ivanHarness.reports()        // every stored report, newest first
ivanHarness.save()           // newest one as a .json file
ivanHarness.saveRecording()  // just the .rec, ready for --replay
ivanHarness.text()           // the live recording, crash or no crash
ivanHarness.report('note')   // file one for something that did not crash
ivanHarness.clear()          // forget them
ivanHarness.build            // the build id the report will name
ivanHarness.args()           // the argv this session handed the runtime
ivanHarness.recordingPath()  // where the recording is, or null under ?record=off
ivanHarness.endpoint()       // where a report would be POSTed, '' for nowhere
```

The last three are functions rather than plain values: all three derive from the
query string, and reading them when asked rather than at load is what keeps them
from disagreeing with what the runtime was actually given.

**The recording is the valuable part.** A stack says where it stopped; the
recording says how to get back there, because it carries the seed in its header:
`./ivan --replay session.rec` replays the session on the native build, where gdb,
valgrind and ASan all apply. Recording is on by default and changes nothing about
how the game plays; `?record=off` opts out.

**Nothing in the three failure paths may throw** — `onAbort`, a rejected promise
for a trap unwinding out of asyncify, and the `error` event. All of it runs while
something has *already* gone wrong, so every entry point is wrapped and every
failure is swallowed after a console warning: a handler that throws replaces the
crash being reported with its own, which is worse than the bug being chased
because it is invisible. That is most of what the node tests check.

### Sending reports somewhere

Reports are local-only by default: `localStorage` and the console, with
`ivanHarness.save()` to get one out by hand. The POST hook is wired and inert
until an endpoint is set.

```bash
IVAN_CRASH_ENDPOINT=https://example.com/ivan-crash npm run build
```

```
ivan.html?crashlog=https://example.com/ivan-crash
```

An environment variable, not a CMake cache variable — `-DWASM_CRASH_ENDPOINT`
used to exist and is gone. The value is `--define`d into the bundle by
`build.mjs`, so it is a `web/` concern and nothing in CMake needs to know it. Not
a secret either way: it ships inside `ivan-page.js`, which every player downloads.

It sends the whole report as JSON, `keepalive` so it outlives the page — which a
crash is usually followed by. `keepalive` caps a body at 64KB, so a long recording
is dropped from the POST and kept locally rather than losing the report; the key
count says so, and the stored copy keeps its recording.

### Why the query string is argv

`Module.arguments` is read by the runtime once, at startup, which is the whole
reason the shell's `<script>` sits before `{{{ SCRIPT }}}` and after the `Module`
literal — and the reason the browser suite asserts that `?seed=999` comes back out
of the recording header. Every option the page does not answer itself is forwarded
verbatim, so `?sfx=off` reaches the game as `--sfx off`: `harness::ParseArgs` is
an if/else-if chain with no else (`harness.cpp:357`), so it is ignored rather than
rejected, and the page keeps one list of options rather than two that must agree.

## Saves — `src/saves/saves.ts`

The game writes saves with `fopen` and knows nothing about any of this. What
changed for it is one line: `GetUserDataDir()` answers `/ivan/` on this target
rather than `PORTABLE_BUILD`'s `"./"` (`save.cpp:838`), and `/ivan` is an IDBFS
mount. `docs/port-log.md` §9.10 has the design argument.

### What is in the mount

Everything the player accumulates, because it is one mount and not three:
`Save/`, `Bones/`, `Scrshot/`, `ivan.cfg`, the highscore table and the answer to
the name prompt. What is *not* in it is `Graphics/` and `Script/`, which are
read-only and live at the MEMFS root that `ivan.data` populates, and
`/session.rec`, which is the crash recording and belongs to `src/harness`.

`.bkp` backups are off on this target (`save.cpp:28`) — they are 35% of a save
set and IndexedDB would keep every byte of them.

### From the console

```js
ivanSaves.stats()      // mounted, dirty, syncs, failures, populateMs, lastError
ivanSaves.files()      // what is in the mount, with sizes
ivanSaves.bytes()      // their total
ivanSaves.estimate()   // navigator.storage.estimate(), in MB
ivanSaves.flush()      // sync now; resolves when IndexedDB has it
ivanSaves.wipe()       // delete every save, then reload
```

```
/play/?saves=off               do not mount; play in a scratch filesystem
/play/?wipesaves               delete the database before mounting
```

**`?wipesaves` is the one to know.** Persistent saves mean a save the game cannot
load fails on every load, and a player who cannot reach the menu cannot use a
console API that lives behind it. It deletes the database before the mount, so
there is no open connection to fight. If another tab has the game open, IndexedDB
queues the delete instead of doing it and the page says so rather than claiming
success.

### When saves do not persist

In the order worth checking:

1. **`ivanSaves.stats().readOnly` is true.** Three things set it, and all three
   print: another tab holds the lock, IndexedDB refused (a private window, or
   third-party storage blocked), or the page was opened with `?saves=off`.
2. **`stats().writes` stays at 0 while the game plainly saves.** The write
   tracker is `FS.trackingDelegate`, which only exists when the module is linked
   `-sFS_DEBUG=1`. Check with `grep -c trackingDelegate build-web/Main/ivan.js`.
   Without it the mount populates and the game plays, so this fails silently
   except for the one line the module prints at boot.
3. **`stats().failures` is climbing.** Read `stats().lastError`. A full disk
   arrives here as `QuotaExceededError`; the files stay in MEMFS and every later
   write retries.
4. **`stats().dirty` is stuck true with `tempDeferrals` climbing.** A `.tmp` is
   sitting in the mount and the sync is refusing to run over a half-written save.
   After ten attempts it gives up waiting and syncs anyway.

### Two tabs

Two tabs on one origin share one database and each holds its own MEMFS, so
whichever syncs last overwrites the other's saves wholesale. The first tab takes
a Web Lock for the life of the page; a tab that cannot get it plays with syncing
off and says so on the page, which loses that session's progress but not the run
already stored. A browser with no `navigator.locks` at all is not a reason to
stop saving.

## What is here

```
web/
  src/
    main.ts                 the entry point, and the only place a global is assigned
    env.d.ts                IVAN_BUILD_ID and IVAN_CRASH_ENDPOINT, esbuild --define
    audio/
      sfx.ts                sound effects, and sfx.test.ts
      music.ts              the soundtrack, and music.test.ts
    harness/
      argv.ts               the query string as argv, and argv.test.ts
      report.ts             crash reports, and report.test.ts
    saves/
      saves.ts              the player's data in IndexedDB, and saves.test.ts
    platform/
      query.ts              the query string, once, for ?sfx=off ... ?wipesaves
      build.ts              the two esbuild --define values, readable under node
    bridge/
      contract.ts           the six EM_JS targets, checked from both ends
      contract.test.ts      parses the C++ and diffs it against contract.ts
      globals.d.ts          the console APIs and the Module object, typed
  e2e/                      Playwright, against an assembled dist/
  build.mjs                 esbuild
```

Modules cross into `src/` one at a time, and `src/main.ts` keeps the list it has
initialised so the browser test fails on a module that did not load rather than
on a feature that is quietly missing. All four of the files that used to be in
`tools/web/` are here; graphics, input and the UI are what is left, and they are
still C++.
