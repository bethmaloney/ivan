#!/bin/sh
# Replay one corpus into an isolated directory and capture its trace.
#
# The harness options are pinned here on purpose. docs/port-log.md §6.6 records that
# changing them changes the process's allocation history, which used to change
# the run with it; that sensitivity is closed, but a golden trace is only a
# golden trace if the command line that produced it is fixed. Change the options
# and you must regenerate every golden file.
#
# --headless is one of those pinned options and is what makes this script work
# on both targets: it drops the window, the renderer and the audio device while
# leaving the software rendering that TraceFrame() hashes untouched. Native runs
# produce byte-identical traces, text logs and screenshots with it and without
# it; the WASM build cannot run at all without it, because Emscripten's SDL2
# port binds to the DOM at video init (docs/port-log.md §9.3).
#
# Usage: run-corpus.sh <corpus.rec> <outdir>
# Env:   IVAN_BIN   path to the ivan binary (default build/Main/ivan). A path
#                   ending in .js is run under node, which is how the same
#                   script replays the Emscripten build.
#        IVAN_DATA  directory holding Graphics/ Script/ Music/ Sound/ (default .)
#        IVAN_CONF  newline-separated `Name = value;` lines to plant as the run's
#                   config file. Empty by default, which writes no file at all, so
#                   every option keeps its compiled-in default and an unset run is
#                   byte-for-byte what it was before this knob existed.

set -eu

if [ $# -ne 2 ]; then
  echo "usage: $0 <corpus.rec> <outdir>" >&2
  exit 2
fi

CORPUS=$1
OUTDIR=$2
REPO=$(cd "$(dirname "$0")/../.." && pwd)
IVAN_BIN=${IVAN_BIN:-$REPO/build/Main/ivan}
IVAN_DATA=${IVAN_DATA:-$REPO}

[ -f "$CORPUS" ] || { echo "no such corpus: $CORPUS" >&2; exit 2; }

# The Emscripten build is ivan.js plus ivan.wasm and is launched through node;
# a native build is executed directly. Nothing else differs between the two.
case $IVAN_BIN in
  *.js)
    IVAN_RUNNER=node
    [ -f "$IVAN_BIN" ] || { echo "no ivan.js: $IVAN_BIN" >&2; exit 2; }
    command -v node > /dev/null || { echo "node not on PATH" >&2; exit 2; }
    ;;
  *)
    IVAN_RUNNER=
    [ -x "$IVAN_BIN" ] || { echo "no ivan binary: $IVAN_BIN" >&2; exit 2; }
    ;;
esac

IVAN_BIN=$(cd "$(dirname "$IVAN_BIN")" && pwd)/$(basename "$IVAN_BIN")

CORPUS=$(cd "$(dirname "$CORPUS")" && pwd)/$(basename "$CORPUS")

# Every run gets its own directory. A portable build writes Save/, SndDebug.txt
# and .QuestionHistory_*.txt into the directory it is launched from, so runs
# sharing one contaminate each other -- docs/port-log.md §6.5a, where that artifact
# alone produced a spurious outcome cluster.
rm -rf "$OUTDIR"
mkdir -p "$OUTDIR"
OUTDIR=$(cd "$OUTDIR" && pwd)

for d in Graphics Script Music Sound; do
  ln -sfn "$IVAN_DATA/$d" "$OUTDIR/$d"
done

# A config file is the only way to pin an option for one run: ivanconfig::Initialize
# snapshots the window size and the zoom once, right after configsystem::Load(), and
# nothing reassigns them afterwards. It is ivan.conf, not ivan.cfg -- the .cfg name is
# inside #ifdef WIN32 (iconf.cpp:1315) and no target here takes that branch -- and it
# goes in the run directory, which is GetUserDataDir() under PORTABLE_BUILD.
if [ -n "${IVAN_CONF:-}" ]; then
  printf '%s\n' "$IVAN_CONF" > "$OUTDIR/ivan.conf"
fi

# The two SDL_*DRIVER variables are redundant now that --headless never asks SDL
# for a device, and are kept only so that dropping --headless by hand still runs
# on a machine with no display. Note Emscripten does not forward the process
# environment into the module, so under node they reach nothing at all -- which
# is the second reason the no-device decision had to move into the program.
cd "$OUTDIR"
SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy ${IVAN_RUNNER} "$IVAN_BIN" \
  --replay "$CORPUS" \
  --trace trace.jsonl \
  --text text.log \
  --shot screen.png \
  --headless \
  > run.log 2>&1

# The trace's own header carries the seed and resolution, so a truncated run is
# visible without reading the game's stdout.
[ -s trace.jsonl ] || { echo "empty trace from $CORPUS" >&2; exit 1; }
