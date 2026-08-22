#!/bin/sh
# Replay every committed corpus on two builds and compare what came out.
#
# This is seam 1 of PORTING.md: while C++ still renders, a native build and
# a WASM build must produce the same frames. The frame trace is the verdict --
# a per-frame hash of the double buffer plus the cumulative RNG count -- and the
# text layer and the end-of-run screenshot come with it, so a failure arrives
# with something you can look at rather than a hash that differs.
#
# The two builds are compared against *each other*, not against the committed
# goldens. verify-corpora.sh does the golden check and answers a different
# question ("did this build change?"); this one answers "do these two builds
# agree?", and compare-configs.sh asks the third, "does one build agree with
# itself when the player configured it differently?" (docs/port-log.md §6.10a).
# All three are needed.
#
# Like verify-corpora.sh this replays $HERE/*.rec only, so the recordings under
# effects/ are not compared across targets -- §6.10b says what that costs.
#
# Usage: compare-targets.sh
# Env:   IVAN_BIN_A  first build   (default build/Main/ivan)
#        IVAN_BIN_B  second build  (default build-wasm/Main/ivan.js, run under
#                    node -- run-corpus.sh picks the runner from the extension)
#        SAVEDIFF    savediff binary, if built. When present the save set is
#                    compared too and reported, but it does not decide the exit
#                    status: PORTING.md records one legitimate divergence
#                    (game::Save's GetTimeSpent, a wall-clock second boundary)
#                    that no amount of determinism work will remove.
#
# Exit: 0 if every frame trace, text log and screenshot matches, 1 otherwise.

set -eu

HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
A=${IVAN_BIN_A:-$REPO/build/Main/ivan}
B=${IVAN_BIN_B:-$REPO/build-wasm/Main/ivan.js}
SAVEDIFF=${SAVEDIFF:-$REPO/build/tools/savediff/savediff}
WORK=${WORK:-$REPO/build/corpus-compare}
FAILED=0

rm -rf "$WORK"

echo "A: $A"
echo "B: $B"
echo

for rec in "$HERE"/*.rec; do
  name=$(basename "$rec" .rec)
  printf '%s:' "$name"

  IVAN_BIN=$A "$HERE/run-corpus.sh" "$rec" "$WORK/$name/a" &
  IVAN_BIN=$B "$HERE/run-corpus.sh" "$rec" "$WORK/$name/b" &
  wait

  bad=0
  for artifact in trace.jsonl text.log screen.png screen.txt; do
    if ! cmp -s "$WORK/$name/a/$artifact" "$WORK/$name/b/$artifact"; then
      [ "$bad" -eq 0 ] && printf '\n'
      bad=1; FAILED=1
      printf '  FAIL %s differs\n' "$artifact"

      # The first differing trace record is the useful part: its rng field says
      # whether the *game* diverged or only the animation did (docs/port-log.md §6.5a).
      if [ "$artifact" = trace.jsonl ]; then
        diff "$WORK/$name/a/$artifact" "$WORK/$name/b/$artifact" \
          | head -3 | sed 's/^/    /'
      fi
    fi
  done

  [ "$bad" -eq 0 ] && printf ' frames, text and screen all match\n'

  # Reported, never fatal -- see the SAVEDIFF note above.
  if [ -x "$SAVEDIFF" ]; then
    "$SAVEDIFF" --summary --ignore-timespent "$WORK/$name/a/Save" "$WORK/$name/b/Save" \
      | sed -n '/^ROLE/,$p' | sed 's/^/  /' || true
  fi
done

if [ "$FAILED" -ne 0 ]; then
  echo "targets DISAGREE -- artifacts left in $WORK" >&2
  exit 1
fi

echo "targets agree"
