#!/bin/sh
# Does the presentation stream reach game state?
#
# The fourth question, beside verify-corpora.sh's "did this build change?",
# compare-targets.sh's "do these two builds agree?" and compare-configs.sh's
# "does one build agree with itself when the player configured it differently?"
#
# Replay every corpus N times varying only --visual-seed, and compare the game
# trace. frames.jsonl and screen.png are *expected* to differ; that is what the
# seed is for and it is the liveness check below.
#
# This and compare-configs.sh answer different questions and neither contains the
# other. That one varies the camera, so it sees a draw on the *game* generator
# whose call count is camera-gated -- §6.10's bug. This one varies the visual
# seed, so it sees a value from the *presentation* generator reaching game state.
# It does NOT see bodypart::DrawScars (docs/port-log.md §6.10a): that is a game
# generator draw, and no visual seed touches it. Nothing here covers that row.
#
# What a pass establishes, exactly: over the draws these recordings reach,
# nothing visualrand produced (femath.h) reaches the game trace or the message
# stream. It says nothing about a site no corpus reaches, which is why
# effects/beams.rec is in the sweep -- the same reason compare-configs.sh needs
# it (§6.10b).
#
# Usage: fuzz-visual.sh [-n RUNS]
# Env:   IVAN_BIN  path to the ivan binary (default build/Main/ivan)

set -eu

RUNS=6
while [ $# -gt 0 ]; do
  case $1 in
    -n) RUNS=$2; shift 2 ;;
    *) echo "usage: $0 [-n RUNS]" >&2; exit 2 ;;
  esac
done

HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
WORK=${WORK:-$REPO/build/corpus-fuzz}
IVAN_BIN=${IVAN_BIN:-$REPO/build/Main/ivan}
FAILED=0

rm -rf "$WORK"
printf 'build:  %s\nruns:   %d per corpus\n\n' "$IVAN_BIN" "$RUNS"

for rec in "$HERE"/*.rec "$HERE"/effects/*.rec; do
  name=$(basename "$rec" .rec)
  printf '%s:' "$name"

  i=1
  while [ "$i" -le "$RUNS" ]; do
    # Seeds are fixed rather than drawn from $RANDOM: a failing run has to be
    # reproducible from the script's output alone, and "it disagreed at some
    # seed I no longer have" is not a bug report.
    IVAN_ARGS="--visual-seed $((1000 + i))" \
      "$HERE/run-corpus.sh" "$rec" "$WORK/$name/$i" &
    i=$((i + 1))
  done
  wait

  # The game must not follow the visual seed. text.log is in here too: it is the
  # message stream, so it moves on any trajectory change the trace could sample
  # past.
  bad=0
  for artifact in game.jsonl text.log; do
    distinct=$(md5sum "$WORK/$name"/*/"$artifact" | awk '{print $1}' | sort -u | wc -l)
    if [ "$distinct" -ne 1 ]; then
      printf '\n  FAIL %s: %d distinct outcomes across %d visual seeds\n' \
             "$artifact" "$distinct" "$RUNS"
      diff "$WORK/$name/1/$artifact" "$WORK/$name/2/$artifact" | head -4 | sed 's/^/    /'
      FAILED=1; bad=1
    fi
  done

  # Liveness. If the seed reached no pixel then the arms are the same run and
  # the agreement above means nothing -- the same trap compare-configs.sh guards
  # against, and the one this script actually fell into the first time the two
  # presentation disciplines shared a generator (§6.10c).
  live=$(md5sum "$WORK/$name"/*/frames.jsonl | awk '{print $1}' | sort -u | wc -l)
  if [ "$live" -eq 1 ]; then
    printf '\n  FAIL --visual-seed moved no pixel in %s: vacuous pass\n' "$name"
    FAILED=1; bad=1
  fi

  [ "$bad" -eq 0 ] && printf ' game trace held over %d seeds, %d distinct frame traces\n' \
                             "$RUNS" "$live"
done

if [ "$FAILED" -ne 0 ]; then
  echo "visual fuzz FAILED -- artifacts left in $WORK" >&2
  exit 1
fi

echo "presentation is isolated"
