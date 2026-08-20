#!/usr/bin/env python3

"""Assemble the deployable site from a browser build and the repo's assets.

    tools/web/dist.py [--out dist] [--build build-web/Main]
    tools/web/serve.py 8113 dist      # then check it before deploying

The layout it writes, and why it is this shape:

    dist/
      index.html          the landing page
      favicon.ico         the game's own icon at 16/32/48, for tabs
      icon.png            the same icon at 128, for everything else
      screenshot.webp     a frame from a recorded session
      fonts/              woff2 + fonts.css, self-hosted
      _headers            Cloudflare Pages cache and security policy
      play/
        index.html        the game page -- emcc's ivan.html, renamed
        ivan.js  ivan.wasm  ivan.data
        ivan-page.js      the page's own JavaScript, bundled from web/
        Sound/*.wav       fetched on demand by web/src/audio/sfx.ts
        Music/*.ogg       streamed by web/src/audio/music.ts, plus stems.json

The game lives under /play/ rather than at the root so that the front door
stays a 30KB page instead of a 5MB one -- 11MB on disk, but the CDN serves the
wasm brotli'd, and 5MB is what was measured off the wire.

Sound/ and Music/ sit *beside* the game page rather than at the site root
because both are resolved relative to the page that asks for them: the sfx
module is handed "./Sound/name.wav" by the wasm module itself, so moving them
would need a ?sfxbase= on every link.

What is deliberately NOT copied:

    Music/*.mid   the source the stems were rendered from. Nothing on this
                  target can play them -- the MIDI parser and playback engine
                  are not compiled for Emscripten at all -- so they are 1.5MB
                  of files with no reader.
    Sound/SoundEffects.cfg
                  already inside ivan.data, where initSound fopen()s it out of
                  MEMFS. A second copy on disk would never be read.
    fonts/fetch-fonts.py, site/make-icons.py, site/ivan.ico
                  build-time tooling, and the icon master the second of them
                  reads. favicon.ico and icon.png are generated from it and
                  committed, so a deploy never needs Pillow.
"""

import argparse
import json
import os
import re
import shutil
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
SITE = os.path.join(HERE, "site")

# emcc's outputs. ivan.html is renamed on the way in; the others keep their
# names because the script tag emcc generates inside the page refers to
# ivan.js by name, and they have to stay siblings for it to resolve.
# ivan-page.js is emitted beside them by the same build (Main/CMakeLists.txt)
# rather than by emcc, and is required here for the same reason the rest are: a
# page whose bundle is missing loads, draws, and is silently missing everything
# web/ owns.
BUILD_FILES = ["ivan.html", "ivan.js", "ivan.wasm", "ivan.data",
               "ivan-page.js", "ivan-page.js.map"]

# Cloudflare Pages. No COOP/COEP here on purpose: those are needed for
# SharedArrayBuffer, this build has no pthreads and does not use it, and
# turning them on would break the page's ability to load anything cross-origin
# for no gain.
#
# The wasm bundle is revalidated rather than cached hard, because none of its
# filenames are content-hashed -- a redeploy reuses ivan.wasm, so a browser
# holding a long max-age copy would keep playing last week's build with no way
# to know. no-cache still allows a 304, which costs a round trip and no bytes.
# The media can cache properly: it changes only when the game's assets do.
HEADERS = """\
/*
  X-Content-Type-Options: nosniff
  Referrer-Policy: strict-origin-when-cross-origin

/index.html
  Cache-Control: no-cache

/play/index.html
  Cache-Control: no-cache

/play/ivan.js
  Cache-Control: no-cache

/play/ivan-page.js
  Cache-Control: no-cache

/play/ivan.wasm
  Cache-Control: no-cache

/play/ivan.data
  Cache-Control: no-cache

/play/Sound/*
  Cache-Control: public, max-age=86400

/play/Music/*
  Cache-Control: public, max-age=86400

/fonts/*
  Cache-Control: public, max-age=604800

/screenshot.webp
  Cache-Control: public, max-age=604800

/icon.png
  Cache-Control: public, max-age=604800

/favicon.ico
  Cache-Control: public, max-age=604800
"""


def megs(count):
    return "%.1f MB" % (count / 1048576.0)


def tree_size(path):
    total = 0

    for root, _, files in os.walk(path):
        for name in files:
            total += os.path.getsize(os.path.join(root, name))

    return total


def copy_dir(source, target, suffixes=None, skip=()):
    """Copy a flat directory, optionally filtered by extension."""

    os.makedirs(target, exist_ok=True)
    copied = 0

    for name in sorted(os.listdir(source)):
        full = os.path.join(source, name)

        if not os.path.isfile(full) or name in skip:
            continue

        if suffixes and not name.lower().endswith(suffixes):
            continue

        shutil.copy2(full, os.path.join(target, name))
        copied += 1

    return copied


def wanted_effects():
    """The wav files SoundEffects.cfg can ask for.

    Format is `Description;Files;Regex`, with Files a comma-separated list a
    random one is chosen from (see the header of the file itself). Lines are
    commented with '#'. Parsed rather than globbed because the point of the
    check is to catch a pattern naming a file nobody rendered -- a glob would
    agree with itself and prove nothing.
    """

    names = set()
    config = os.path.join(REPO, "Sound", "SoundEffects.cfg")

    with open(config, encoding="utf-8", errors="replace") as handle:
        for line in handle:
            line = line.strip()

            if not line or line.startswith("#"):
                continue

            fields = line.split(";")

            if len(fields) < 3:
                continue

            for name in fields[1].split(","):
                name = name.strip()

                if name:
                    names.add(name)

    return names


def wanted_stems():
    """The ogg files stems.json promises, as `Track.stem.ogg`.

    A track with an empty list is not a fault -- six of the eleven have no
    notes at all, and the page relies on "no stems" being a normal answer
    rather than probing for a file that was never rendered.
    """

    names = set()

    with open(os.path.join(REPO, "Music", "stems.json")) as handle:
        manifest = json.load(handle)

    for track, stems in manifest.items():
        stem_base = re.sub(r"\.mid$", "", track)

        for stem in stems:
            names.add("%s.%s.ogg" % (stem_base, stem))

    return names


def check(label, wanted, directory):
    """Confirm every file something asks for is actually there."""

    missing = sorted(name for name in wanted
                     if not os.path.isfile(os.path.join(directory, name)))

    if missing:
        print("\n  MISSING %s (%d):" % (label, len(missing)), file=sys.stderr)

        for name in missing[:12]:
            print("    %s" % name, file=sys.stderr)

        if len(missing) > 12:
            print("    ... and %d more" % (len(missing) - 12), file=sys.stderr)

    return missing


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--out", default=os.path.join(REPO, "dist"),
                        help="where to write the site (default: dist/)")
    parser.add_argument("--build", default=os.path.join(REPO, "build-web", "Main"),
                        help="the browser build directory (default: build-web/Main)")
    args = parser.parse_args()

    out = os.path.abspath(args.out)
    play = os.path.join(out, "play")

    for name in BUILD_FILES:
        if not os.path.isfile(os.path.join(args.build, name)):
            sys.exit("no %s in %s\n"
                     "Build the browser target first:\n"
                     "  emcmake cmake -S . -B build-web -DCMAKE_BUILD_TYPE=Release \\\n"
                     "      -DWIZARD=ON -DPORTABLE_BUILD=ON -DWASM_BROWSER=ON\n"
                     "  cmake --build build-web -j$(nproc)"
                     % (name, args.build))

    # Rebuilt from scratch every time. An incremental copy into a directory
    # that already has a previous deploy in it is how a file that was deleted
    # from the repo keeps getting published.
    if os.path.isdir(out):
        shutil.rmtree(out)

    os.makedirs(play)

    # ---- the site itself
    shutil.copy2(os.path.join(SITE, "index.html"), os.path.join(out, "index.html"))
    shutil.copy2(os.path.join(SITE, "icon.png"), os.path.join(out, "icon.png"))
    shutil.copy2(os.path.join(SITE, "favicon.ico"), os.path.join(out, "favicon.ico"))
    shutil.copy2(os.path.join(SITE, "screenshot.webp"), os.path.join(out, "screenshot.webp"))

    fonts = copy_dir(os.path.join(SITE, "fonts"), os.path.join(out, "fonts"),
                     skip=("fetch-fonts.py",))

    # ---- the game
    for name in BUILD_FILES:
        target = "index.html" if name == "ivan.html" else name
        shutil.copy2(os.path.join(args.build, name), os.path.join(play, target))

    effects = copy_dir(os.path.join(REPO, "Sound"), os.path.join(play, "Sound"),
                       suffixes=(".wav",))

    stems = copy_dir(os.path.join(REPO, "Music"), os.path.join(play, "Music"),
                     suffixes=(".ogg", ".json"))

    with open(os.path.join(out, "_headers"), "w") as handle:
        handle.write(HEADERS)

    # ---- does it actually have what the page will ask for?
    missing = check("sound effects", wanted_effects(), os.path.join(play, "Sound"))
    missing += check("music stems", wanted_stems(), os.path.join(play, "Music"))

    core = sum(os.path.getsize(os.path.join(play, n))
               for n in ["index.html", "ivan.js", "ivan.wasm", "ivan.data",
                         "ivan-page.js"])

    print("%-22s %s" % ("landing page", megs(os.path.getsize(os.path.join(out, "index.html")))))
    print("%-22s %s  (%d files)" % ("fonts", megs(tree_size(os.path.join(out, "fonts"))), fonts))
    # "on disk", not "first load": the CDN serves the wasm brotli'd, which is
    # 7.8MB down to 1.65MB, so the number a player actually waits for is about
    # 5MB. Labelling this one "first load" is what put 11MB on the landing page.
    print("%-22s %s" % ("game, on disk", megs(core)))
    print("%-22s %s  (%d files)" % ("sound effects", megs(tree_size(os.path.join(play, "Sound"))), effects))
    print("%-22s %s  (%d files)" % ("music stems", megs(tree_size(os.path.join(play, "Music"))), stems))
    print("%-22s %s" % ("total", megs(tree_size(out))))
    print("\n%s" % out)

    if missing:
        sys.exit("\n%d file(s) the page can ask for are not in the build. "
                 "A missing one fails silently at runtime -- see tools/web/README.md."
                 % len(missing))


if __name__ == "__main__":
    main()
