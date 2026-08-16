#!/usr/bin/env python3

"""Render the MIDI stems to OGG for the browser build (HARNESS.md §9.8).

split-stems.py cuts each track into the three curves the intensity system
drives; this turns those into audio the page can stream, and drops the result
into Music/ beside the .mid they came from.

Run it when a .mid changes, and commit what it produces. It is deliberately
not part of the build: it would make fluidsynth and a soundfont a hard
dependency of `-DWASM_BROWSER=ON` for everyone, and it would bake whichever
soundfont happened to be on the builder's machine into their copy of the game.
Rendering once and committing the result means every player hears the same
music, which is more than the native path can say -- there the soundtrack is
whatever the local MIDI device makes of it.

    tools/music/render-stems.py                    # into Music/
    tools/music/render-stems.py --quality 5        # bigger, better
    IVAN_SOUNDFONT=/path/to.sf2 tools/music/render-stems.py

Needs fluidsynth to render and ffmpeg or oggenc to encode:

    sudo apt install fluidsynth vorbis-tools

---- levels ----

The three stems are summed by three gain nodes in the page, so what matters is
that the *sum* does not clip, not that each part is loud. They are rendered at
one gain and then, if the sum peaks over full scale, scaled by one common
factor -- the same factor for all three, because anything else would change
the balance between them and the balance is the thing being preserved.
"""

import argparse
import glob
import os
import shutil
import subprocess
import sys
import tempfile
import wave

try:
    import numpy
except ImportError:
    # Only the clipping check needs it, and that degrades to "assume it is
    # fine" rather than to a broken render. Summing ~39 million samples per
    # track in a Python loop is not a fallback worth having.
    numpy = None

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))

# Ordered by preference. The first that exists wins unless IVAN_SOUNDFONT says
# otherwise. FluidR3 is the usual full General MIDI set; TimGM6mb is the small
# one Debian ships with timidity and is a reasonable fallback.
SOUNDFONTS = [
    "/usr/share/sounds/sf2/FluidR3_GM.sf2",
    "/usr/share/soundfonts/FluidR3_GM.sf2",
    "/usr/share/sounds/sf2/default-GM.sf2",
    "/usr/share/soundfonts/default-GM.sf2",
    "/usr/share/sounds/sf2/TimGM6mb.sf2",
]

SAMPLE_RATE = 44100

# fluidsynth's own default is 0.2, which leaves a lot of headroom unused. This
# is louder but still well short of the point where a dense passage clips
# before the stems are even summed; the check below catches what it misses.
GAIN = 0.5

FULL_SCALE = 32767


def find_soundfont():
    chosen = os.environ.get("IVAN_SOUNDFONT")

    if chosen:
        if not os.path.exists(chosen):
            sys.exit("IVAN_SOUNDFONT does not exist: %s" % chosen)

        return chosen

    for path in SOUNDFONTS:
        if os.path.exists(path):
            return path

    sys.exit(
        "no soundfont found. Install one (sudo apt install fluid-soundfont-gm)\n"
        "or point IVAN_SOUNDFONT at a .sf2"
    )


def need(*names):
    for name in names:
        found = shutil.which(name)

        if found:
            return found

    return None


def render(midi, wav, soundfont):
    subprocess.run(
        [
            "fluidsynth", "-ni", "-F", wav,
            "-r", str(SAMPLE_RATE),
            "-g", str(GAIN),
            "-q",
            soundfont, midi,
        ],
        check=True,
        stdout=subprocess.DEVNULL,
    )


def samples_of(path):
    """Interleaved 16-bit samples as a widened numpy array, plus the channel count."""

    with wave.open(path, "rb") as handle:
        if handle.getsampwidth() != 2:
            sys.exit("expected 16-bit output from fluidsynth: %s" % path)

        raw = handle.readframes(handle.getnframes())

        return handle.getnchannels(), numpy.frombuffer(raw, dtype="<i2").astype(numpy.int32)


def summed_peak(paths):
    """Peak of the stems added together, as a fraction of full scale.

    They are summed the way the page sums them, but with every curve at its
    loudest at once. That never actually happens -- fadeout and fadein are
    complementary, so one is always down as the other comes up -- which makes
    this a worst case by a good margin, and that is the point of it.

    Stems of one track are the same piece and so the same length, but they are
    added into a buffer sized to the longest rather than assumed equal: a
    trailing reverb tail is enough to make them differ by a few samples.
    """

    if numpy is None or not paths:
        return 0.0

    parts = [samples_of(path)[1] for path in paths]
    total = numpy.zeros(max(len(part) for part in parts), dtype=numpy.int32)

    for part in parts:
        total[: len(part)] += part

    return int(numpy.abs(total).max()) / float(FULL_SCALE)


def scale(path, factor):
    channels, data = samples_of(path)
    scaled = numpy.clip(numpy.rint(data * factor), -FULL_SCALE, FULL_SCALE)

    with wave.open(path, "wb") as handle:
        handle.setnchannels(channels)
        handle.setsampwidth(2)
        handle.setframerate(SAMPLE_RATE)
        handle.writeframes(scaled.astype("<i2").tobytes())


def encode(wav, ogg, quality, encoder):
    if os.path.basename(encoder).startswith("ffmpeg"):
        command = [
            encoder, "-y", "-loglevel", "error",
            "-i", wav, "-c:a", "libvorbis", "-q:a", str(quality), ogg,
        ]
    else:
        command = [encoder, "-Q", "-q", str(quality), "-o", ogg, wav]

    subprocess.run(command, check=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--outdir", default=os.path.join(REPO, "Music"))
    parser.add_argument("--quality", type=float, default=3,
                        help="Vorbis VBR quality, -1..10 (default 3)")
    parser.add_argument("--keep", action="store_true",
                        help="keep the intermediate .mid and .wav")
    args = parser.parse_args()

    if not need("fluidsynth"):
        sys.exit("fluidsynth not found: sudo apt install fluidsynth")

    encoder = need("ffmpeg", "oggenc")

    if not encoder:
        sys.exit("no encoder found: sudo apt install ffmpeg  (or vorbis-tools)")

    soundfont = find_soundfont()
    print("soundfont: %s" % soundfont)
    print("encoder:   %s (q%s)\n" % (encoder, args.quality))

    work = tempfile.mkdtemp(prefix="ivan-stems-")

    try:
        split = subprocess.run(
            [sys.executable, os.path.join(HERE, "split-stems.py"), "--outdir", work],
            cwd=REPO, check=True, capture_output=True, text=True,
        )
        print(split.stdout)

        stems = sorted(glob.glob(os.path.join(work, "*.mid")))

        if not stems:
            sys.exit("split-stems.py produced nothing")

        # Group by track so the clipping check sees a whole mix at a time.
        tracks = {}

        for path in stems:
            base = os.path.basename(path)[: -len(".mid")]
            tracks.setdefault(base.rsplit(".", 1)[0], []).append(path)

        total = 0

        for name in sorted(tracks):
            wavs = []

            for midi in sorted(tracks[name]):
                wav = midi[: -len(".mid")] + ".wav"
                render(midi, wav, soundfont)
                wavs.append(wav)

            peak = summed_peak(wavs)

            if numpy is None and name == sorted(tracks)[0]:
                print("  (numpy missing: not checking whether the stems clip when summed)")

            if peak > 1.0:
                factor = 0.99 / peak
                print("  %-12s summed peak %.2f -- scaling all stems by %.3f"
                      % (name, peak, factor))

                for wav in wavs:
                    scale(wav, factor)

            sizes = []

            for wav in wavs:
                stem = os.path.basename(wav)[: -len(".wav")]
                ogg = os.path.join(args.outdir, stem + ".ogg")
                encode(wav, ogg, args.quality, encoder)
                size = os.path.getsize(ogg)
                total += size
                sizes.append("%s %.1fMB" % (stem.rsplit(".", 1)[1], size / 1e6))

            print("%-12s peak %.2f   %s" % (name, peak, "  ".join(sizes)))

        shutil.copy(os.path.join(work, "stems.json"),
                    os.path.join(args.outdir, "stems.json"))

        print("\n%.1fMB of OGG in %s/, plus stems.json" % (total / 1e6, args.outdir))
        print("Nothing is fetched until the game asks for that area's music.")

    finally:
        if args.keep:
            print("intermediates kept in %s" % work)
        else:
            shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    main()
