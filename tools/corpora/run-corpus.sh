#!/bin/sh
# Replay one corpus into an isolated directory and capture its trace.
#
# The harness options are pinned here on purpose. HARNESS.md §6.6 records that
# changing them changes the process's allocation history, which used to change
# the run with it; that sensitivity is closed, but a golden trace is only a
# golden trace if the command line that produced it is fixed. Change the options
# and you must regenerate every golden file.
#
# Usage: run-corpus.sh <corpus.rec> <outdir>
# Env:   IVAN_BIN   path to the ivan binary (default build/Main/ivan)
#        IVAN_DATA  directory holding Graphics/ Script/ Music/ Sound/ (default .)

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
[ -x "$IVAN_BIN" ] || { echo "no ivan binary: $IVAN_BIN" >&2; exit 2; }

CORPUS=$(cd "$(dirname "$CORPUS")" && pwd)/$(basename "$CORPUS")

# Every run gets its own directory. A portable build writes Save/, SndDebug.txt
# and .QuestionHistory_*.txt into the directory it is launched from, so runs
# sharing one contaminate each other -- HARNESS.md §6.5a, where that artifact
# alone produced a spurious outcome cluster.
rm -rf "$OUTDIR"
mkdir -p "$OUTDIR"
OUTDIR=$(cd "$OUTDIR" && pwd)

for d in Graphics Script Music Sound; do
  ln -sfn "$IVAN_DATA/$d" "$OUTDIR/$d"
done

cd "$OUTDIR"
SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy "$IVAN_BIN" \
  --replay "$CORPUS" \
  --trace trace.jsonl \
  --text text.log \
  --shot screen.png \
  > run.log 2>&1

# The trace's own header carries the seed and resolution, so a truncated run is
# visible without reading the game's stdout.
[ -s trace.jsonl ] || { echo "empty trace from $CORPUS" >&2; exit 1; }
