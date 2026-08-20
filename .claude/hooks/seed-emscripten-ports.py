#!/usr/bin/env python3
"""Build the Emscripten ports, cloning any whose archive URL is refused.

The ports IVAN links (-sUSE_SDL=2 -sUSE_SDL_MIXER=2 -sUSE_LIBPNG=1,
CMakeLists.txt:165) are not in the SDK. emcc fetches each one's source the first
time it links it, and on Claude Code on the web the github.com *archive*
endpoint answers 403 -- only that endpoint: `/releases/download/` is fine (which
is how the ogg port arrives) and so is `git clone` of the very same tag.

So this asks emcc what it wanted rather than hard-coding it: run embuilder, read
the "retrieving port: NAME from URL" lines out of a failure, clone each blocked
repo at that tag into the port cache, and retry. Nothing here names a version,
so an emsdk bump moves the tags without touching this file.

Seeding means writing the source where a successful download would have unpacked
it, plus the `.emscripten_url` marker holding the URL. tools/ports/__init__.py
checks that marker first (`up_to_date()`), so a seeded port is never fetched and
never hash-checked -- which is the point, since a clone is not byte-identical to
GitHub's generated tarball and would fail the port's sha512.

EMCC_LOCAL_PORTS is the mechanism that looks right for this and is not: it needs
the port to define SUBDIR, which of the four only sdl2 does, and emscripten's own
comment calls it "hacky ... not tested".

Exits 0 when every port is built, 1 otherwise, and says which URLs it could not
resolve. Idempotent: an already-built port is a no-op, and an already-seeded one
is re-used rather than re-cloned.
"""

import re
import shutil
import subprocess
import sys
from pathlib import Path

PORTS = ["sdl2", "sdl2_mixer", "libpng"]  # each pulls in its own dependencies
ROUNDS = 4  # a blocked dependency only names itself once the port above it runs

RETRIEVING = re.compile(r"retrieving port: (\S+) from (https://\S+)")
# github.com/OWNER/REPO/archive/[refs/tags/|refs/heads/]REF.(zip|tar.gz)
ARCHIVE = re.compile(
    r"https://github\.com/([^/]+)/([^/]+)/archive/"
    r"(?:refs/(?:tags|heads)/)?(.+?)\.(?:zip|tar\.gz)$"
)


def ports_dir() -> Path:
    out = subprocess.run(
        ["em-config", "PORTS"], capture_output=True, text=True, check=True
    )
    return Path(out.stdout.strip())


def unpacked_name(repo: str, ref: str) -> str:
    """What GitHub's generated archive unpacks to: REPO-REF, less a version 'v'."""
    if len(ref) > 1 and ref[0] == "v" and ref[1].isdigit():
        ref = ref[1:]
    return f"{repo}-{ref}"


def seed(name: str, url: str, cache: Path) -> bool:
    m = ARCHIVE.match(url)
    if not m:
        print(f"  {name}: not a github archive URL, cannot clone: {url}")
        return False
    owner, repo, ref = m.groups()

    target = cache / name
    subdir = target / unpacked_name(repo, ref)
    marker = target / ".emscripten_url"

    if subdir.is_dir() and marker.is_file() and marker.read_text().strip() == url:
        print(f"  {name}: already seeded at {subdir.name}")
        return True

    print(f"  {name}: cloning {owner}/{repo} at {ref}")
    if target.exists():
        shutil.rmtree(target)
    subdir.parent.mkdir(parents=True, exist_ok=True)
    clone = subprocess.run(
        ["git", "clone", "--depth", "1", "--branch", ref,
         f"https://github.com/{owner}/{repo}.git", str(subdir)],
        capture_output=True, text=True,
    )
    if clone.returncode != 0:
        print(f"  {name}: clone failed -- {clone.stderr.strip().splitlines()[-1:]}")
        return False

    # The generated archive has no .git, and leaving one behind would make the
    # port tree differ from what a download produces for no benefit.
    shutil.rmtree(subdir / ".git", ignore_errors=True)
    marker.write_text(url + "\n")
    return True


def build() -> tuple[bool, str]:
    run = subprocess.run(
        ["embuilder", "build", *PORTS], capture_output=True, text=True
    )
    return run.returncode == 0, run.stdout + run.stderr


def main() -> int:
    cache = ports_dir()
    seeded: set[str] = set()
    log = ""

    for attempt in range(ROUNDS):
        ok, log = build()
        if ok:
            # Says nothing about provenance when nothing was cloned: a warm cache
            # and a clean download are indistinguishable from here.
            done = ", ".join(PORTS)
            if seeded:
                print(f"built {done} -- cloned {', '.join(sorted(seeded))}")
            else:
                print(f"built {done}")
            return 0

        blocked = {
            name: url
            for name, url in RETRIEVING.findall(log)
            if ARCHIVE.match(url) and name not in seeded
        }
        if not blocked:
            break

        print(f"round {attempt + 1}: {len(blocked)} port(s) to clone")
        for name, url in sorted(blocked.items()):
            if seed(name, url, cache):
                seeded.add(name)
        if not seeded:
            break

    print("\nWARNING: could not build the Emscripten ports.")
    for name, url in sorted(set(RETRIEVING.findall(log))):
        print(f"  {name}: {url}")
    for err in sorted(set(re.findall(r"HTTP Error \d+: .*", log))):
        print(f"  {err}")
    print("\n  The native build and web/ are unaffected; both WASM targets cannot")
    print("  build until these resolve. See CLAUDE.md.")
    return 1


if __name__ == "__main__":
    sys.exit(main())
