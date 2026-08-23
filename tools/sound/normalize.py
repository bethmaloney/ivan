#!/usr/bin/env python3

"""Normalise Sound/*.wav to a declared per-category loudness (docs/port-log.md §9.7b).

The set was scavenged rather than mastered: 100 of the 159 wavs peaked within
1dB of full scale and their loudness spanned 27dB, so which of the five
`You.* hit` files the xorshift picked changed how loud the blow was by up to
18dB. This puts every file at a target its category declares, which is what
makes the variants behind one message interchangeable -- the thing the config
has always assumed and the files never did.

    tools/sound/normalize.py                       # the whole set, in place
    tools/sound/normalize.py --dry-run             # print the plan, write nothing
    tools/sound/normalize.py --check               # exit 1 if anything is off target
    tools/sound/normalize.py Sound/blade1.wav ...  # just these, same targets

Needs ffmpeg. Re-running is a no-op: the targets below are constants, not
something re-derived from the files, so a normalised set measures back at its
target and nothing is rewritten. That is what makes it usable on a batch of
replacement sounds -- drop them in, run it, and they arrive at the level the
rest of the set already sits at.

---- how loudness is measured ----

EBU R128 integrated loudness, which is gated: it discards blocks more than 20LU
below the ungated mean, so leading and trailing silence does not drag a short
effect's number down the way plain RMS does. The gate also needs about three
seconds of programme to settle, and 23 of these files are shorter than 400ms --
those measure as -70 LUFS, the "silence" floor. So each file is looped past the
gate before being measured and the loudness of one copy is read off the loop.
K-weighting is worth the trouble over RMS here because the set spans EarthQuake
to whistlemagic, and unweighted RMS puts a low rumble far louder than it sounds.

---- where the numbers came from ----

REFERENCE is the Fight median as it stands today, minus the 12.04dB the page
used to take off at the bus (§9.7a). Fight because it is the workhorse -- 63% of
the sound the corpora provoke is a hit, a miss or a blade -- so anchoring there
is what keeps the game as loud as it was while the trim goes away.

Each OFFSET is likewise its category's median today, relative to Fight's. So the
balance between categories is the one the game already had, preserved rather
than invented: what this pass removes is the spread *within* a category, and the
divergence between the page and native. Both halves of that are deliberate, and
neither is a mix decision made here. Change a number below to make one, by ear.
"""

import argparse
import collections
import math
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
SOUND = os.path.join(REPO, "Sound")
CONFIG = os.path.join(SOUND, "SoundEffects.cfg")

# LUFS. See "where the numbers came from" above before changing it: this is the
# level of the whole effects bed, and the page no longer trims it afterwards.
REFERENCE = -24.9

# dB relative to REFERENCE, per SoundEffects.cfg section. The comment on each is
# where that category sat before this pass, which is also how it was derived.
OFFSETS = {
    "Milestones":       -0.7,   # n=1   today -13.6
    "Status Effects":   -3.6,   # n=6   today -16.5
    "Discontent":       -2.5,   # n=1   today -15.4
    "Door":            -10.9,   # n=7   today -23.8
    "Explosions":       +3.9,   # n=5   today  -9.0
    "Fight":            +0.0,   # n=25  today -12.9
    "Environment":      -6.2,   # n=13  today -19.1
    "Traps":            -1.2,   # n=4   today -14.1
    "Player Hurt":      -0.5,   # n=7   today -13.4
    "Player Actions":   -9.0,   # n=21  today -21.9
    "Drink":            -3.0,   # n=2   today -15.9
    "Eat":              -7.0,   # n=3   today -19.9
    "Monsters":         +2.0,   # n=16  today -10.9
    "Death Msgs":       +0.8,   # n=34  today -12.2
    "Instruments":      -3.6,   # n=6   today -16.5
    "Wands":            +2.6,   # n=7   today -10.3
    "Items Destroyed":  +1.7,   # n=5   today -11.2
    "Main Menu SFX":    -3.2,   # n=5   today -16.1
}

# Sample peak, dBFS. Only one file in the set wants a gain that would cross it,
# and the cap costs that file 2dB of its target -- cheaper than the alternative,
# since sixteen voices already sum here with nothing limiting them.
CEILING = -1.0

# Below this, the gain is not worth a rewrite: 0.1dB is inaudible and rewriting
# every file on every run would churn 26MB of git history for nothing.
TOLERANCE = 0.1

# Seconds of looped audio to hand the R128 gate. Three would do; this is slack.
MEASURE_SECONDS = 12.0


def run(command):
    # errors="replace" because ffmpeg echoes the input's metadata back, and some
    # of these carry a latin-1 copyright string that is not valid UTF-8.
    return subprocess.run(
        command, check=True, stdout=subprocess.DEVNULL,
        stderr=subprocess.PIPE, text=True, errors="replace",
    ).stderr


def probe(path, fields):
    out = subprocess.run(
        ["ffprobe", "-v", "error", "-show_entries", fields,
         "-of", "default=noprint_wrappers=1:nokey=1", path],
        check=True, capture_output=True, text=True,
    ).stdout.split()

    return out


def categories():
    """Every file SoundEffects.cfg can play, mapped to the sections naming it.

    A file may appear under several -- holy.wav is an explosion, a player action
    and a death -- so the caller averages their targets rather than picking one.
    """

    found = collections.defaultdict(list)
    section = None

    with open(CONFIG, encoding="utf-8", errors="replace") as handle:
        for line in handle:
            heading = re.match(r"^### (.+?) ###", line.strip())

            if heading:
                section = heading.group(1).strip()
                continue

            text = line.strip()

            if not text or text.startswith("#"):
                continue

            parts = text.split(";")

            if len(parts) < 3:
                continue

            for name in parts[1].split(","):
                name = name.strip()

                if name and section not in found[name]:
                    found[name].append(section)

    unknown = sorted({s for sections in found.values() for s in sections} - set(OFFSETS))

    if unknown:
        sys.exit("SoundEffects.cfg has sections with no target in OFFSETS: %s"
                 % ", ".join(unknown))

    return found


def loudness(path):
    """Integrated LUFS of one copy, measured on enough copies to pass the gate."""

    duration = float(probe(path, "format=duration")[0])
    loops = max(1, int(MEASURE_SECONDS / max(duration, 0.01)))

    report = run(["ffmpeg", "-hide_banner", "-nostats", "-stream_loop", str(loops),
                  "-i", path, "-af", "ebur128", "-f", "null", "-"])

    found = re.search(r"Integrated loudness:.*?I:\s*(-?[\d.]+|-inf) LUFS", report, re.S)

    if not found or found.group(1) == "-inf":
        return None

    return float(found.group(1))


def peak(path):
    report = run(["ffmpeg", "-hide_banner", "-nostats", "-i", path,
                  "-af", "astats=measure_perchannel=none", "-f", "null", "-"])

    found = re.search(r"Peak level dB:\s*(-?[\d.]+|-inf)", report)

    return -math.inf if not found or found.group(1) == "-inf" else float(found.group(1))


def apply_gain(path, gain):
    """Rewrite in place at 16-bit PCM, keeping the rate and the channel count.

    16-bit rather than the source depth is the point of doing it here: 31 of
    these are 8-bit, and attenuating an 8-bit file by 12dB and re-quantising to
    8-bit throws away two of its bits. It also lands the ten files that are not
    plain PCM -- five float, three ADPCM, and punch.wav, which is MP3 inside a
    RIFF container and has therefore never loaded on the native path at all
    (§9.7a) -- on a format both SDL and every browser decoder accept.
    """

    rate, channels = probe(path, "stream=sample_rate,channels")
    temporary = path + ".normalizing.wav"

    # -map_metadata 0 is ffmpeg's default and is passed anyway, because losing
    # it here would destroy evidence: 61 of these carry RIFF tags, and they are
    # the only record of where any of the set came from. Some of that record is
    # unwelcome -- explosion2.wav says "Copyright 2000, Sounddogs.com" -- which
    # is exactly why it should survive a pass that rewrites every file.
    try:
        run(["ffmpeg", "-hide_banner", "-nostats", "-y", "-i", path,
             "-af", "volume=%.3fdB" % gain, "-map_metadata", "0",
             "-c:a", "pcm_s16le", "-ar", rate, "-ac", channels, temporary])
        os.replace(temporary, path)
    finally:
        if os.path.exists(temporary):
            os.remove(temporary)


def target_of(name, sections):
    if not sections:
        return REFERENCE

    return REFERENCE + sum(OFFSETS[s] for s in sections) / len(sections)


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("files", nargs="*", help="wavs to do; default is all of Sound/")
    parser.add_argument("--dry-run", action="store_true", help="print the plan only")
    parser.add_argument("--check", action="store_true",
                        help="exit 1 if anything is off target; writes nothing")
    parser.add_argument("--tolerance", type=float, default=TOLERANCE,
                        help="dB of error to leave alone (default %(default)s)")
    options = parser.parse_args()

    sections_of = categories()
    paths = options.files or sorted(
        os.path.join(SOUND, n) for n in os.listdir(SOUND) if n.endswith(".wav"))

    orphans = []
    capped = []
    landed = collections.defaultdict(list)
    moved = 0
    off = 0

    for path in paths:
        name = os.path.basename(path)
        sections = sections_of.get(name, [])

        if not sections:
            orphans.append(name)

        target = target_of(name, sections)

        for section in sections:
            landed[section].append((name, target))

        now = loudness(path)

        if now is None:
            print("%-24s silent, skipped" % name)
            continue

        gain = target - now
        headroom = CEILING - peak(path)

        if gain > headroom:
            capped.append((name, gain - headroom))
            gain = headroom

        # The epsilon is not decoration: a target is a sum of two one-decimal
        # constants, so a file sitting exactly on the tolerance comes out at
        # 0.1000000000000058 and would be rewritten on every run forever.
        if abs(gain) <= options.tolerance + 1e-6:
            continue

        off += 1
        print("%-24s %6.1f -> %6.1f LUFS  %+6.2f dB%s"
              % (name, now, now + gain, gain, "  [%s]" % "+".join(sections) if sections else ""))

        if options.dry_run or options.check:
            continue

        apply_gain(path, gain)
        moved += 1

    if orphans:
        print("\nnot named by SoundEffects.cfg, so put at the plain reference: %s"
              % ", ".join(orphans))

    if capped:
        print("\nheld at the %.0fdBFS ceiling, and so short of target by: %s"
              % (CEILING, ", ".join("%s %.1fdB" % (n, d) for n, d in capped)))

    # A category whose files do not all land together, which happens when one of
    # them belongs to another category too and so sits at the mean of both. Worth
    # printing rather than leaving to be discovered: it is the residue of a
    # config decision, and the fix for it is to give that message its own file.
    for section in sorted(landed):
        levels = landed[section]
        spread = max(t for _, t in levels) - min(t for _, t in levels)

        if spread > options.tolerance:
            # The one furthest from what this section alone would have asked
            # for, which is the file doing the pulling -- and not the quietest,
            # since a shared file lands above its section as often as below.
            own = REFERENCE + OFFSETS[section]
            outlier = max(levels, key=lambda pair: abs(pair[1] - own))
            print("%-20s spans %.1fdB, pulled %s by %s, which is shared"
                  % (section, spread, "up" if outlier[1] > own else "down", outlier[0]))

    print("\n%d of %d off target by more than %.2gdB%s"
          % (off, len(paths), options.tolerance,
             "; %d rewritten" % moved if moved else ""))

    if options.check and off:
        return 1

    return 0


if __name__ == "__main__":
    sys.exit(main())
