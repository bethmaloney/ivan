# tools/web — the page around the module, and the deploy

**The frontend itself is `web/`.** All four JavaScript files that used to live here have crossed and
are bundled rather than linked, so **there is no JavaScript here and no `--pre-js` in the build**;
their operating manuals moved with them to `web/README.md`. What is left is the page emcc wraps
around the module, the landing page, and the tooling that assembles and serves both.

| | |
|---|---|
| `shell.html` | the page emcc wraps around the module |
| `site/` | the landing page, and the fonts and images it serves |
| `dist.py` | assembles a deployable tree from a build plus the repo's assets |
| `serve.py` | a static server that answers byte ranges, for testing either of them |

The build flags that make a browser crash legible and the saves mount work are in `PORTING.md`; the
crash-report console API is in `web/README.md`, and the reasoning behind both is `docs/port-log.md`
§9.6 and §9.10.

## shell.html

A `--shell-file` input, wired in from the top-level `CMakeLists.txt`. Without it emcc emits
`shell_minimal.html`, which is an emscripten.org logo, a "Resize canvas" checkbox and a debug
textarea — a page that advertises what built the thing rather than what the thing is.

**Four things are load-bearing, because the runtime and the page's own bundle reach into them by
name.**

- **`id="canvas"`** — Emscripten's SDL2 port looks the canvas up by that id at video init. Rename it
  and the game has nothing to draw on.
- **`var Module`, global, before `ivan.js` loads.** `ivan.js` opens with
  `var Module = typeof Module != 'undefined' ? Module : {}`, and the bundle takes the same object, so
  the canvas, the status hooks, the `preRun` hook and run dependency that hold startup back for
  IndexedDB, and `--record`'s argv all arrive through it. `onAbort` is chained rather than replaced:
  the shell sets one and `web/src/harness` wraps it.
- **`<script src="ivan-page.js">`, after the `Module` literal and before `{{{ SCRIPT }}}`.** Both
  halves matter. After, because the bundle assigns onto `Module`; before, because `Module.arguments`
  is read by the runtime once, at startup, so a bundle loaded later would lose the seed and the
  recording with no error to show for it.
- **`{{{ SCRIPT }}}`** — where emcc substitutes its own script tag.

**No line may begin with a hash.** emcc runs the shell through its C preprocessor first and treats
**any** line whose first non-whitespace character is `#` as a directive (`src/parseTools.mjs:149`),
failing the link on one it does not recognise:

```
error: shell.html:18: Unknown preprocessor directive #canvas
```

That rules out CSS id selectors at the start of a line, which is why everything in the shell is
styled by class and the ids are left for JavaScript and for SDL to find. The stock shell only gets
away with `#status{...}` because its CSS ships minified onto a single line.

**What it does beyond drawing a canvas.** It scales the canvas — the game draws 800×600 and keeps its
backing store at that size, and the page scales the presentation to fit with
`image-rendering: pixelated` so a 16×16 tileset survives the trip up. It takes the scrolling keys
(arrows, space, Page Up/Down, Home, End) so they do not scroll the page out from under a roguelike:
only those, only unmodified, and only when focus is not on one of the buttons in the bar, so browser
shortcuts keep working and the page stays navigable by keyboard. It shows a panel pointing at
`ivanHarness.save()` on abort or an unhandled rejection, because "nothing happened" is the worst
possible reading of a crash. And it mutes, through the master gain `web/src/audio/sfx.ts` owns, which
is also what `web/src/audio/music.ts` connects its stems to, so one node covers both.

`LINK_DEPENDS` in `Main/CMakeLists.txt` names it, and it is the only entry left now that nothing is a
`--pre-js`: the shell reaches emcc only through `LINK_FLAGS`, so without it nothing relinks when the
shell changes and the edit silently appears to do nothing.

## site/ — the landing page

Static, self-contained, and deliberately not the game: the front door is a 30KB page rather than a
5MB one, and `/play/` is where the download starts.

```
site/
  index.html        the whole page -- markup and CSS, no script
  icon.png          Graphics/Icon.bmp at 128px. A pick-axe and a banana.
  screenshot.webp   frame 594 of the autoplay-200 corpus, lossless, 60KB
  fonts/            Grenze, Spectral and IBM Plex Mono, latin subset only
    fetch-fonts.py  regenerates the above; not run at build time
```

**The fonts are self-hosted on purpose.** A page that pulls them from `fonts.gstatic.com` tells
Google who is playing IVAN, which is not something a page needs to do to draw a headline.
`fetch-fonts.py` deduplicates by URL, because a variable font answers every weight in its range with
the same file — Grenze 600 and 700 are one download, and writing it twice would put a copy of it on
every first load for nothing.

**Everything on the page is real, and this file is where the provenance lives.** The key list is
`command.cpp:92`; the body part numbers, message templates and opening text come from
`Script/item.dat`, `char.cpp` and `game.cpp`; the screenshot is a replayed recording rather than a
staged shot. Nothing checks that any of it still says what the source says, so if one of those
changes the page is wrong and only a reader will notice.

## dist.py — assembling a deploy

```bash
tools/web/dist.py                     # -> dist/
tools/web/serve.py 8113 dist          # check it before pushing it
```

Built from `build-web/Main` plus the repo's asset directories:

```
dist/
  index.html  icon.png  screenshot.webp  fonts/  _headers
  play/
    index.html                    emcc's ivan.html, renamed
    ivan.js  ivan.wasm  ivan.data
    Sound/*.wav                   fetched on demand by the sfx module
    Music/*.ogg  stems.json       streamed by web/src/audio/music.ts
```

`Sound/` and `Music/` sit beside the *game page* rather than at the site root because both are
resolved relative to whatever asks for them — the module hands the sfx module the string
`"./Sound/name.wav"` itself — so moving them would need a `?sfxbase=` on every link. Not copied:
`Music/*.mid`, the source the stems were rendered from and which nothing on this target can play, and
`Sound/SoundEffects.cfg`, which is already inside `ivan.data` where `initSound` reads it out of
MEMFS.

`dist/` is rebuilt from scratch each time, because copying into a directory that still holds a
previous deploy is how a file deleted from the repo keeps getting published.

**It checks that the assets exist, and that is the part worth keeping.** A missing sound or stem
fails silently at runtime — one console line, no error, a game that is just quieter than it should be
— so the check happens at assembly time: every `.wav` named in `Sound/SoundEffects.cfg` is parsed out
of the config and looked for by name, and every stem `Music/stems.json` promises is looked for as
`Track.stem.ogg`. **Parsed rather than globbed, deliberately**: a glob would agree with itself and
prove nothing. The first run found `explosion3.WAV` on disk against `explosion3.wav` in the config —
invisible on Windows and macOS, a 404 over HTTP and a silent failure on Linux, and wrong since long
before the web build.

**`_headers` is Cloudflare Pages policy, written by the script.** The wasm bundle is `no-cache`
rather than cached hard, because none of its filenames are content-hashed — a redeploy reuses
`ivan.wasm`, so a browser holding a long `max-age` copy would keep playing last week's build with no
way to find out. `no-cache` still permits a 304, which costs a round trip and no bytes. Media caches
for a day and fonts for a week; those change only when the game's assets do. No COOP/COEP: those are
for `SharedArrayBuffer`, this build has no pthreads, and turning them on would only break
cross-origin loads.

## Deploying

```bash
tools/web/dist.py
npx wrangler pages deploy dist --project-name=playivan --branch main
```

**Both arguments are load-bearing and this file used to have the first one wrong.** The project is
`playivan`, not `ivan` — `wrangler pages project list` is the authority, and the wrong name fails
loudly. **`--branch` fails quietly, which is worse.** The project's production branch is `main` and
wrangler labels a deployment with the branch it thinks you are on; without the flag the deploy
succeeds, prints a URL that serves the new build, and leaves `playivan.pages.dev` on the previous
production deployment — a deploy that looks done and changed nothing anyone visits.
`wrangler pages deployment list --project-name=playivan` shows the Environment column that gives it
away. **Check the deployed bytes, not the exit code.**

`wrangler pages deploy` will not create the project for you; run
`wrangler pages project create <name> --production-branch=main` first. Uploads are content-addressed,
so a redeploy that changes one file uploads one file. A custom domain is attached in the Pages
dashboard under *Custom domains*, and needs the domain to exist first — `wrangler` deploys sites, it
does not register names.

**Give the edge a few seconds before believing a redeploy failed.** For a short window after a
deploy, `https://<project>.pages.dev/` can still serve the previous HTML even though the policy on it
is `max-age=0, must-revalidate`. The deployment-specific URL wrangler prints is authoritative
immediately, and `?x=<anything>` bypasses the stale copy:

```bash
curl -s "https://<project>.pages.dev/?x=$RANDOM" | grep something-you-changed
```

**Check it locally first, and know the one way the two hosts differ that matters.** `serve.py` sends
`no-store` on HTML and JS so a stale copy can never be mistaken for a build that did not take, while
`_headers` sends the real policy — and `serve.py` answers byte ranges where Cloudflare Pages does
not, which is measured and survivable rather than broken (`docs/port-log.md` §9.9). A range-related
failure against `serve.py` would therefore be a real one in production too.
