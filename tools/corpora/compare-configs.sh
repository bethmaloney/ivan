#!/bin/sh
# Replay every corpus at several dungeon zoom levels on one build and check the game did not change.
#
# A third question, beside the two the other scripts ask. verify-corpora.sh asks "did this build
# change?" and compare-targets.sh asks "do these two builds agree?"; this one asks "does one build
# agree with *itself* when the player has configured it differently?", and the answer has to be yes:
# a zoom level is a preference about pixels, not a fork in the game.
#
# It is not a hypothetical. game::GetScreenXSize() (game.cpp:286) is the window width in tiles
# divided by ivanconfig::GetStartingDungeonGfxScale(), game::PosCurrentlyOnScreen() is built from
# it, and every visual effect that gates on that and then calls RAND() puts the player's zoom level
# into the game's random stream unless a femath::SaveSeed bracket keeps it out.
#
# Usage: compare-configs.sh [-s "SCALES"] [-c NAME]
#        -s  DungeonGfxScale values to compare; the first is the reference arm. Only 1-6 are
#            accepted: cycleoption::LoadValue does not clamp (config.cpp:285), and
#            DungeonGfxScale = 0 divides by zero in game::GetScreenXSize.
#        -c  one corpus by name instead of all of them
# Env:   IVAN_BIN  build to test (default build/Main/ivan)
#        WORK      scratch directory (default build/corpus-configs)
#
# Exit: 0 if every corpus ends on the same game-stream position at every scale, 1 otherwise.
#
# docs/port-log.md §6.10a is the write-up: exactly what a pass establishes, and what it cannot see.
#
# The default covers the whole 1-6 range because the tree is now clean across it. It was not, and
# the site that closed it is worth writing down as history: fluid::imagedata::Animate
# (fluid.cpp:463) started a blood drip with an unbracketed RandomizePixel(), once per stained
# square that level::Draw puts on screen -- the only site of this class the committed corpora
# reach. Unbracketed, measured with -s "1 2 3 4 5 6": autoplay-200 ended at grng 1,714,427 at
# scales 1-5 and 1,813,687 at 6, and autoplay-2000 at 6,106,089 at scales 1-3 against 7,220,615,
# 7,744,800 and 8,361,791. Bracketed -- with level::DrawExplosion (level.cpp:1157), which this
# sweep then found on autoplay-2000 alone -- every corpus agrees at every scale: grng 1,566,177,
# 6,612,194, 1,074,979 and 1,098,228. Moving those goldens was the point of that change.
#
# Read a pass narrowly. The oracle is one integer per corpus, over four recordings, at one window
# size, with EnhancedLights pinned off below. A pass says that no draw these keys reach follows the
# zoom; it cannot see a site the corpora never reach, one gated on the window size rather than the
# scale, or a screen-derived value that reaches game state without drawing at all.

set -eu

SCALES="1 2 3 4 5 6"
ONLY=
while [ $# -gt 0 ]; do
  case $1 in
    -s) SCALES=$2; shift 2 ;;
    -c) ONLY=$2; shift 2 ;;
    *) echo "usage: $0 [-s \"SCALES\"] [-c NAME]" >&2; exit 2 ;;
  esac
done

for s in $SCALES; do
  case $s in
    [1-6]) ;;
    *) echo "$0: DungeonGfxScale $s is outside 1-6" >&2; exit 2 ;;
  esac
done

HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
IVAN_BIN=${IVAN_BIN:-$REPO/build/Main/ivan}
WORK=${WORK:-$REPO/build/corpus-configs}
REFERENCE=$(echo "$SCALES" | { read -r first _; echo "$first"; })
FAILED=0

rm -rf "$WORK"

echo "build:  $IVAN_BIN"
echo "scales: $SCALES"
echo

# The committed corpora, then the ones under effects/. Those are separate because they have no
# golden: what they exercise is a wand, whose animation is *meant* to look different at a different
# zoom, so there is nothing about them for verify-corpora.sh to pin -- and keeping them out of its
# glob keeps them out of its way. They are not optional garnish either. No committed corpus casts
# anything, so without them this script would be asserting a property that holds vacuously.
#
# effects/beams.rec is the seed 999 prefix the other corpora share, then wizard mode, `1` five
# times so the character survives its own beams, `$` for the scrolls of wishing, three wishes
# (lightning, acid rain, fireballs) and two zaps. What it reaches, counted with gdb breakpoints:
# lsquare::DrawParticles 6 times and level::DrawExplosion once -- and lsquare::DrawLightning not at
# all. The fireball zap detonates a gas grenade, warp gas teleports the character across the level,
# and the last key lands on an apply prompt instead of firing the wand, so the lightning bracket is
# covered by no recording in the tree. Re-record it wishing the lightning wand *last*, so the
# fireball cannot displace the character before its zap. tools/play/play.py is what wrote it.
for rec in "$HERE"/*.rec "$HERE"/effects/*.rec; do
  name=$(basename "$rec" .rec)
  [ -z "$ONLY" ] || [ "$ONLY" = "$name" ] || continue
  printf '%s:' "$name"

  # EnhancedLights is pinned off for the same reason the harness options are pinned on:
  # level::RevealDistantLightsToPlayer (level.cpp:3141) reveals squares by iterating the on-screen
  # rectangle, so with it on the zoom decides how much of the map the player knows and the
  # auto-play corpora part company long before they reach a visual effect. Measured on autoplay-200
  # at scale 1 against scale 2: grng 1,517,633 against 1,714,427 with it on, 1,714,427 both with it
  # off. That is a defect of the same family and not a SaveSeed candidate -- the function makes no
  # draws, it mutates Reveal state -- so it is not this script's to find.
  for s in $SCALES; do
    IVAN_CONF="DungeonGfxScale = $s;
EnhancedLights = 0;" IVAN_BIN=$IVAN_BIN "$HERE/run-corpus.sh" "$rec" "$WORK/$name/$s" &
  done
  wait

  # Only the *final* grng is compared, and that is the shape of the check rather than a weakening of
  # it. grng is the running count of the draws the game kept, so two arms that played the same game
  # must end on the same number. The per-frame column is left alone on purpose: a record is emitted
  # only when the hash or the rng moved, and an effect that draws on fewer squares blits fewer
  # times, so two arms can sample one identical stream at different points. Measured on
  # autoplay-2000, scale 1 against scale 2, both ending at grng 6,612,194: at frame 661 scale 1
  # samples grng 1,710,053 and scale 2 samples 1,712,375, which is scale 1's *next* sample. Diffing
  # the column would call that a failure. frames.jsonl, screen.png and the map-area text differ
  # legitimately too -- the dungeon is drawn at a different size, which is the point of the option.
  bad=0
  for s in $SCALES; do
    trace=$WORK/$name/$s/game.jsonl
    grng=$(sed -n 's/.*"grng":\([0-9]*\).*/\1/p' "$trace" | tail -1)
    rng=$(sed -n 's/.*"rng":\([0-9]*\).*/\1/p' "$trace" | tail -1)
    nest=$(sed -n 's/.*"nest":\([0-9]*\).*/\1/p' "$trace" | sort -u | tr -d '\n')

    if [ "$nest" != 0 ]; then
      [ "$bad" -eq 0 ] && printf '\n'
      bad=1; FAILED=1
      printf '  FAIL scale %s: nest reached %s -- the single mtb backup slot is being reused\n' \
             "$s" "$nest"
    fi

    if [ "$s" = "$REFERENCE" ]; then
      expected=$grng
      reference_rng=$rng
      continue
    fi

    if [ "$grng" != "$expected" ]; then
      [ "$bad" -eq 0 ] && printf '\n'
      bad=1; FAILED=1
      printf '  FAIL scale %s ends at grng %s, scale %s at %s\n' \
             "$s" "$grng" "$REFERENCE" "$expected"
    fi

    # The arms have to be genuinely different runs, or every comparison above is vacuous. A planted
    # config that the game never read -- ivan.cfg instead of ivan.conf is the way to get that, since
    # only WIN32 takes the .cfg branch (iconf.cpp:1315) -- produces two byte-identical traces and a
    # pass that means nothing. It is the *frame* trace that must differ: the zoom is meant to change
    # what is drawn, and the game trace is the thing being asserted equal, so checking that one for
    # liveness would demand the failure it is testing for.
    if cmp -s "$WORK/$name/$REFERENCE/frames.jsonl" "$WORK/$name/$s/frames.jsonl"; then
      [ "$bad" -eq 0 ] && printf '\n'
      bad=1; FAILED=1
      printf '  FAIL scale %s produced a trace identical to scale %s: the config was not read\n' \
             "$s" "$REFERENCE"
    fi
  done

  # rng used to count the discarded draws of a SaveSeed bracket, so rng moving while grng did not
  # was the signature of a working bracket. Since §6.10c there are no such draws on this generator
  # and it has not moved on any corpus; kept because the one surviving bracket could still move it.
  # Reported, never fatal.
  if [ "$bad" -eq 0 ]; then
    printf ' grng %s at every scale' "$expected"
    [ "$rng" != "$reference_rng" ] && printf ', rng %s to %s' "$reference_rng" "$rng"
    printf '\n'
  fi
done

if [ "$FAILED" -ne 0 ]; then
  echo "configs DISAGREE -- artifacts left in $WORK" >&2
  exit 1
fi

echo "configs agree"
