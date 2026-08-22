#!/bin/sh
# Replay every committed corpus and check it against its golden trace and text log.
#
# Two checks, and they answer different questions:
#
#   self-consistency  N isolated runs of this build agree with each other.
#                     Catches nondeterminism introduced by a change.
#   golden            run 1 agrees with the committed artifact.
#                     Catches behaviour changes -- intended or not.
#
# N defaults to 8 because the divergences this harness was built to find are
# flaky: docs/port-log.md §6.5a records two confidently wrong diagnoses drawn from
# two-run comparisons, both of which evaporated at 6-8 samples. Pairs agree by
# chance often enough to fool you. Use 1 only for a quick smoke test.
#
# Recordings under effects/ are deliberately outside this script's *.rec glob:
# they have no golden and must not acquire one (docs/port-log.md §6.10b).
# compare-configs.sh is what replays them.
#
# Usage: verify-corpora.sh [-n RUNS] [--update]
#        --update  rewrite the golden files from this build instead of checking
#                  them. Self-consistency is still enforced first -- a build that
#                  cannot reproduce itself must not mint a golden.
# Env:   IVAN_BIN  path to the ivan binary (default build/Main/ivan)

set -eu

RUNS=8
UPDATE=0
while [ $# -gt 0 ]; do
  case $1 in
    -n) RUNS=$2; shift 2 ;;
    --update) UPDATE=1; shift ;;
    *) echo "usage: $0 [-n RUNS] [--update]" >&2; exit 2 ;;
  esac
done

HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
WORK=${WORK:-$REPO/build/corpus-verify}
FAILED=0

rm -rf "$WORK"

for rec in "$HERE"/*.rec; do
  name=$(basename "$rec" .rec)
  printf '%s: %d runs' "$name" "$RUNS"

  i=1
  while [ "$i" -le "$RUNS" ]; do
    "$HERE/run-corpus.sh" "$rec" "$WORK/$name/$i" &
    i=$((i + 1))
  done
  wait

  # Self-consistency. Count distinct outcomes rather than comparing a pair --
  # docs/port-log.md §6.5a again: with a flaky divergence, "run 1 == run 2" is a
  # coin flip and reporting it as agreement is how this went wrong twice.
  consistent=0
  for artifact in trace.jsonl text.log screen.png; do
    distinct=$(md5sum "$WORK/$name"/*/"$artifact" | awk '{print $1}' | sort -u | wc -l)
    if [ "$distinct" -ne 1 ]; then
      printf '\n  FAIL %s: %d distinct outcomes across %d runs\n' \
             "$artifact" "$distinct" "$RUNS"
      FAILED=1; consistent=1
    fi
  done
  [ "$consistent" -eq 0 ] && printf ' self-consistent'

  # Golden comparison.
  golden_bad=0
  for pair in "trace.jsonl:$name.trace.jsonl" "text.log:$name.text.log"; do
    produced=$WORK/$name/1/${pair%%:*}
    golden=$HERE/${pair##*:}
    if [ "$UPDATE" -eq 1 ]; then
      cp "$produced" "$golden"
    elif [ ! -f "$golden" ]; then
      printf '\n  FAIL no golden: %s (run with --update to create it)\n' "$golden"
      FAILED=1; golden_bad=1
    elif ! cmp -s "$produced" "$golden"; then
      printf '\n  FAIL %s differs from golden\n' "${pair%%:*}"
      diff "$golden" "$produced" | head -20 | sed 's/^/    /'
      FAILED=1; golden_bad=1
    fi
  done

  if [ "$UPDATE" -eq 1 ]; then
    printf ', golden updated\n'
  elif [ "$golden_bad" -eq 0 ] && [ "$consistent" -eq 0 ]; then
    printf ', matches golden\n'
  fi
done

if [ "$FAILED" -ne 0 ]; then
  echo "corpora FAILED -- artifacts left in $WORK" >&2
  exit 1
fi

echo "corpora OK"
