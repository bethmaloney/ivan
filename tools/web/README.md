# tools/web — the browser frontend

**The frontend has moved to `web/`; see `web/README.md`.** All four of the
files that used to be here have crossed — `web/src/audio/sfx.ts`,
`web/src/audio/music.ts`, `web/src/harness/` and `web/src/saves/` — bundled
rather than linked, and their operating manuals moved with them. **No JavaScript
is left here and there are no `--pre-js` inputs at all.** The C++ → page bridge
is declared in `web/src/bridge/contract.ts` and checked from both ends.

Four things live here now, and none of them is JavaScript:

| | |
|---|---|
| `shell.html` | the page emcc wraps around the module |
| `site/` | the landing page, and the fonts and images it serves |
| `dist.py` | assembles a deployable tree from a build plus the repo's assets |
| `serve.py` | a static server that answers byte ranges, for testing either of them |

---

# Diagnosing a browser crash

**The report itself is `web/src/harness/` now, and its manual moved with it — see
`web/README.md`.** `ivanHarness.reports()`, `ivanHarness.saveRecording()`, the
console output, `?record=off` and `?crashlog=` are all documented there.

What stays here is the half that is about the *build* rather than the page: why a
browser build carries function names unconditionally, the two knobs that make a
crash easier to pin down, and what it means when a WASM crash will not reproduce
natively.

The goal both halves serve is that an ordinary session, played with no foresight
and no special flags, produces enough on a crash to find the bug. Two things have
to be true *before* the crash, because neither can be arranged afterwards:

- the binary has to carry function names, or the trap prints byte offsets that
  nothing can resolve;
- the session has to have been recording, or there is no way to reproduce it.

Both are on by default in a `WASM_BROWSER` build. Nothing below needs enabling.

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

---

# Saves — the player's data, in IndexedDB

**`saves.js` is `web/src/saves/saves.ts` now, and its manual moved with it — see
`web/README.md`.** `ivanSaves.stats()`, `?wipesaves`, `?saves=off` and the
checklist for when saves do not persist are all documented there.

What stays here is the half that is about the *build*. Two link flags in the
top-level `CMakeLists.txt` are load-bearing and neither fails loudly:

- **`-lidbfs.js -sFS_DEBUG=1`.** The first links the IndexedDB backend, which is
  a separate JS library. The second is not a debug mode despite the name —
  `settings.js:392` defines it as exactly "register file system callbacks using
  trackingDelegate", which is how the page learns that the game wrote a save.
  Without it `FS.trackingDelegate` does not exist, the mount populates, the game
  plays, and nothing after boot is ever written back.
- **`-sEXPORTED_RUNTIME_METHODS=...,addRunDependency,removeRunDependency`.** The
  run dependency that holds `main()` back while IndexedDB is copied into MEMFS.
  `FORCE_FILESYSTEM` already exports the pair and `--preload-file` turns that on
  (`link.py:1637`), so dropping the two names would *not* break the build today;
  they are there so the page depends on something the flags state rather than on
  a side effect of preloading `Graphics/`. Without them the failure is a Continue
  menu drawn over saves that had not arrived yet.

---

# shell.html — the page around the module

`--shell-file` input, wired in from the top-level `CMakeLists.txt`. Without it
emcc emits `shell_minimal.html`, which is an emscripten.org logo, a "Resize
canvas" checkbox and a debug textarea — a page that advertises what built the
thing rather than what the thing is.

## What must not change

Four things are load-bearing, because the runtime and the page's own bundle both
reach into them by name:

- **`id="canvas"`** — Emscripten's SDL2 port looks the canvas up by that id at
  video init. Rename it and the game has nothing to draw on.
- **`var Module`, global, before `ivan.js` loads.** `ivan.js` opens with
  `var Module = typeof Module != 'undefined' ? Module : {}`, and the bundle takes
  the same object, so the canvas, the status hooks, the `preRun` hook and run
  dependency that hold startup back for IndexedDB, and `--record`'s argv all
  arrive through it. `onAbort` is chained rather than replaced: the shell sets one
  and `web/src/harness` wraps it.
- **`<script src="ivan-page.js">`, after the `Module` literal and before
  `{{{ SCRIPT }}}`.** Both halves matter. After, because the bundle assigns onto
  `Module`; before, because `Module.arguments` is read by the runtime once, at
  startup, so a bundle loaded later would lose the seed and the recording without
  any error to show for it.
- **`{{{ SCRIPT }}}`** — where emcc substitutes the script tag.

## No line may begin with a hash

emcc runs the shell through its C preprocessor first, and it treats **any** line
whose first non-whitespace character is `#` as a directive
(`src/parseTools.mjs:149`), failing the link on one it does not recognise:

```
error: shell.html:18: Unknown preprocessor directive #canvas
```

That rules out CSS id selectors at the start of a line, which is why everything
in the shell is styled by class and the ids are left for JavaScript and for SDL
to find. The stock shell only gets away with `#status{...}` because its CSS
ships minified onto a single line.

## What it does beyond drawing a canvas

- **Scales the canvas.** The game draws 800×600 and keeps its backing store at
  that size; the page scales the presentation to fit, with
  `image-rendering: pixelated` so a 16×16 tileset survives the trip up.
- **Takes the scrolling keys.** Arrows, space, Page Up/Down, Home and End
  scroll the page out from under a roguelike. Only those, only unmodified, and
  only when focus is not on one of the buttons in the bar, so browser shortcuts
  keep working and the page stays navigable by keyboard.
- **Says when it dies.** On abort or an unhandled rejection it shows a panel
  pointing at `ivanHarness.save()`, because "nothing happened" is the worst
  possible reading of a crash.
- **Mutes.** Through the master gain `web/src/audio/sfx.ts` owns, which is also what
  `web/src/audio/music.ts` connects its stems to, so one node covers both.

`LINK_DEPENDS` in `Main/CMakeLists.txt` names it, and it is the only entry left
now that nothing is a `--pre-js`: the shell reaches emcc only through
`LINK_FLAGS`, so without it nothing relinks when the shell changes and the edit
silently appears to do nothing.

---

# site/ — the landing page

Static, self-contained, and deliberately not the game: the front door is a 30KB
page rather than a 5MB one, and `/play/` is where the download starts.

```
site/
  index.html        the whole page -- markup and CSS, no script
  icon.png          Graphics/Icon.bmp at 128px. A pick-axe and a banana.
  screenshot.webp   frame 594 of the autoplay-200 corpus, lossless, 60KB
  fonts/            Grenze, Spectral and IBM Plex Mono, latin subset only
    fetch-fonts.py  regenerates the above; not run at build time
```

**The fonts are self-hosted on purpose.** A page that pulls them from
`fonts.gstatic.com` tells Google who is playing IVAN, which is not something a
page needs to do to draw a headline. `fetch-fonts.py` deduplicates by URL,
because a variable font answers every weight in its range with the same file —
Grenze 600 and 700 are one download, and writing it twice would put a copy of it
on every first load for nothing.

**Everything on the page is real**, and that is the point rather than a
flourish. The key list is `command.cpp:92`. The screenshot is a replayed
recording — frame 594 of the autoplay-200 corpus — rather than a staged shot.
If either changes, the page is wrong, so check it when they do.

The page no longer prints those sources beside what it took from them. That was
a deliberate trim for the reader, not a licence to let them drift: this file is
where the provenance lives now.

---

# dist.py — assembling a deploy

```bash
tools/web/dist.py                     # -> dist/
tools/web/serve.py 8113 dist          # check it before pushing it
```

Builds this, from `build-web/Main` plus the repo's asset directories:

```
dist/
  index.html  icon.png  screenshot.webp  fonts/  _headers
  play/
    index.html                    emcc's ivan.html, renamed
    ivan.js  ivan.wasm  ivan.data
    Sound/*.wav                   fetched on demand by the sfx module
    Music/*.ogg  stems.json       streamed by web/src/audio/music.ts
```

`Sound/` and `Music/` sit beside the *game page* rather than at the site root
because both are resolved relative to whatever asks for them — the module hands
the sfx module the string `"./Sound/name.wav"` itself — so moving them
would need a `?sfxbase=` on every link.

Not copied: `Music/*.mid`, which are the source the stems were rendered from and
which nothing on this target can play; and `Sound/SoundEffects.cfg`, which is
already inside `ivan.data` where `initSound` reads it out of MEMFS.

## It checks that the assets exist

This is the part worth keeping. A missing sound or stem **fails silently at
runtime** — one console line, no error, a game that is just quieter than it
should be — so the check happens at assembly time instead:

- every `.wav` named in `Sound/SoundEffects.cfg` is parsed out of the config and
  looked for by name;
- every stem `Music/stems.json` promises is looked for as `Track.stem.ogg`.

Parsed rather than globbed, deliberately: a glob would agree with itself and
prove nothing. The first run of this found `explosion3.WAV` on disk against
`explosion3.wav` in the config — invisible on Windows and macOS, a 404 over HTTP
and a silent failure on Linux, and wrong since long before the web build.

`dist/` is rebuilt from scratch each time, because copying into a directory that
still holds a previous deploy is how a file deleted from the repo keeps getting
published.

## _headers

Cloudflare Pages policy, written by the script.

No COOP/COEP: those are for `SharedArrayBuffer`, this build has no pthreads and
does not use it, and turning them on would only break cross-origin loads.

The wasm bundle is `no-cache` rather than cached hard, because none of its
filenames are content-hashed — a redeploy reuses `ivan.wasm`, so a browser
holding a long `max-age` copy would keep playing last week's build with no way to
find out. `no-cache` still permits a 304, which costs a round trip and no bytes.
The media caches for a day and the fonts for a week; those change only when the
game's assets do.

## Deploying it

```bash
tools/web/dist.py
npx wrangler pages deploy dist --project-name=playivan --branch main
```

**Both of those arguments are load-bearing and this file used to have the first
one wrong.** The project is `playivan`, not `ivan`; `wrangler pages project
list` is the authority, and the wrong name fails loudly. `--branch` fails
quietly, which is worse: the project's production branch is `main` and wrangler
labels a deployment with the branch it thinks you are on. The git branch here
was `master` when that bit us; it is `main` now, so the two agree, but the flag
stays explicit rather than trusting the inference. Without it the deploy
succeeds, prints a URL that serves the new build, and leaves
`playivan.pages.dev` on the previous production deployment — a deploy that looks
done and changed nothing anyone visits. `wrangler pages deployment list --project-name=playivan` shows the
Environment column that gives it away.

`wrangler pages deploy` will not create the project for you; run
`wrangler pages project create <name> --production-branch=main` first. Uploads
are content-addressed, so a redeploy that changes one file uploads one file.

A custom domain is attached in the Pages dashboard under *Custom domains*, and
needs the domain to exist first — `wrangler` deploys sites, it does not register
names.

**Give the edge a few seconds before believing a redeploy failed.** For a short
window after a deploy, `https://<project>.pages.dev/` can still serve the
previous HTML even though the policy on it is `max-age=0, must-revalidate`. The
deployment-specific URL wrangler prints is authoritative immediately, and
`?x=<anything>` bypasses the stale copy:

```bash
curl -s "https://<project>.pages.dev/?x=$RANDOM" | grep something-you-changed
```

## What this host does not do

**Cloudflare Pages does not answer byte ranges.** A `Range:` request on a music
stem returns `200` with the whole file and no `Accept-Ranges` header. `serve.py`
implements 206 specifically so local testing would not flatter a host that
doesn't, and this host doesn't.

Measured cost: stems still start in about a second, `failed 0`. Progressive
download is enough to begin playback — what ranges buy is seeking and an early
`duration`, and the page only wants `duration` for drift correction, which it
can wait for. If a long track ever starts late in practice (`Dungeon3` is three
5MB stems), R2 answers ranges and the music module already takes `?musicbase=<url>`,
so moving only the music is a query parameter rather than a migration.

Check it locally before pushing it, because the two differ in one way that
matters: `serve.py` sends `no-store` on HTML and JS so a stale copy can never be
mistaken for a build that did not take, while `_headers` sends the real policy.

```bash
tools/web/serve.py 8113 dist
```
