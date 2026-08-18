#!/usr/bin/env python3

"""Split each Music/*.mid into the stems the intensity system implies.

The browser build does not synthesize MIDI (HARNESS.md §9.8). It plays
pre-rendered audio, and the only reason that can reproduce what the native
build does is that IVAN's adaptive music turns out to be a three-way split
and nothing more.

`audio::SendVolumeMessage` (audio.cpp:265) is the *only* source of channel
volume during playback: `MPB_PB_NO_VOL` makes the playback engine drop the
file's own CC7 events (midiplayback.cpp:792), so whatever automation the
composer wrote there is never heard. What is heard is

    CC7[ch] = (IntensityVolume[ch] * MasterVolume) / 127

and `IntensityVolume` comes from two constant tables (audio.cpp:84-89):

    InitialIntensityVolume  = 127 for channels 0..10,  0 for 11..15
    DeltaVolumePerIntensity =   0 for 0-4 and 9,      -1 for 5-8 and 10,
                               +1 for 11..15

    IntensityVolume[ch] = clamp(Initial[ch] + Delta[ch] * intensity, 0, 127)

so across sixteen channels there are exactly three curves, and every channel
follows one of them:

    const      channels 0-4, 9      127                  (never varies)
    fade-out   channels 5-8, 10     127 - intensity      (loudest at full HP)
    fade-in    channels 11-15       0   + intensity      (silent at full HP)

Intensity is `127 - MinHPPercent`, recomputed every turn in `character::Be`
from the player's *worst* body part (char.cpp:1062). So the fade-in group is
the wounded-player layer, and on these five tracks it holds 62% of the notes.

Splitting on those three groups and handing the pieces to three WebAudio gain
nodes is therefore not an approximation of the intensity system -- it is the
intensity system, with the mixing moved from a MIDI device to a gain node.

What this emits is MIDI, not audio; `render-stems.py` turns these into OGG.
Channel volume is stripped here for the same reason the playback engine strips
it, so that the render is at full scale and the gain node is the only thing
deciding loudness.

Usage:  tools/music/split-stems.py [--outdir DIR] [Music/*.mid ...]
"""

import argparse
import json
import os
import struct
import sys

# audio.cpp:84-89. Kept as the same two tables rather than as a hand-copied
# list of groups, so that if upstream ever retunes the mix this file can be
# diffed against it directly.

INITIAL_INTENSITY_VOLUME = [127] * 11 + [0] * 5
DELTA_VOLUME_PER_INTENSITY = [0, 0, 0, 0, 0, -1, -1, -1, -1, 0, -1, 1, 1, 1, 1, 1]

# Suffixes are what web/src/audio/music.ts asks for, so the two have to agree.
# See StemNames there.

STEMS = ("const", "fadeout", "fadein")

CHANNEL_VOLUME = 7  # CC7, the controller MPB_PB_NO_VOL drops.

META = 0xFF
SYSEX = (0xF0, 0xF7)
END_OF_TRACK = 0x2F
NOTE_ON = 0x90
CONTROL_CHANGE = 0xB0

# Status bytes carrying one data byte rather than two.
ONE_DATA_BYTE = (0xC0, 0xD0)


def stem_of(channel):
    """Which of the three curves this channel follows, by its delta."""

    delta = DELTA_VOLUME_PER_INTENSITY[channel]

    if delta == 0:
        return "const"

    return "fadeout" if delta < 0 else "fadein"


def read_vlq(data, i):
    value = 0

    while True:
        byte = data[i]
        i += 1
        value = (value << 7) | (byte & 0x7F)

        if not byte & 0x80:
            return value, i


def write_vlq(value):
    out = bytearray([value & 0x7F])
    value >>= 7

    while value:
        out.insert(0, (value & 0x7F) | 0x80)
        value >>= 7

    return bytes(out)


class Event:
    __slots__ = ("tick", "data", "channel", "is_note_on", "is_volume")

    def __init__(self, tick, data, channel, is_note_on, is_volume):
        self.tick = tick
        self.data = data
        self.channel = channel
        self.is_note_on = is_note_on
        self.is_volume = is_volume


def parse_track(data, i):
    """Return (events, next_offset) for one MTrk, with running status resolved.

    Events are re-emitted later with explicit status bytes. Expanding running
    status costs a byte per event in the intermediate file, which is never
    shipped -- only the OGGs are -- and it removes the one thing most likely
    to go quietly wrong when events are dropped out of the middle of a stream.
    """

    if data[i:i + 4] != b"MTrk":
        raise ValueError("expected MTrk at offset %d" % i)

    length = struct.unpack(">I", data[i + 4:i + 8])[0]
    end = i + 8 + length
    j = i + 8
    tick = 0
    status = None
    events = []

    while j < end:
        delta, j = read_vlq(data, j)
        tick += delta

        if data[j] & 0x80:
            status = data[j]
            j += 1

        if status is None:
            raise ValueError("running status with no preceding status byte")

        if status == META:
            kind = data[j]
            payload_start = j
            j += 1
            payload_len, j = read_vlq(data, j)
            j += payload_len

            # A meta event clears running status.
            blob = bytes([META]) + data[payload_start:j]
            status = None

            if kind == END_OF_TRACK:
                continue  # Re-added on write, exactly once.

            events.append(Event(tick, blob, None, False, False))
            continue

        if status in SYSEX:
            payload_start = j
            payload_len, j = read_vlq(data, j)
            j += payload_len
            events.append(
                Event(tick, bytes([status]) + data[payload_start:j], None, False, False)
            )
            status = None
            continue

        high = status & 0xF0
        width = 1 if high in ONE_DATA_BYTE else 2
        payload = data[j:j + width]
        j += width

        is_note_on = high == NOTE_ON and payload[1] != 0
        is_volume = high == CONTROL_CHANGE and (payload[0] & 0x7F) == CHANNEL_VOLUME

        events.append(
            Event(tick, bytes([status]) + payload, status & 0x0F, is_note_on, is_volume)
        )

    if j != end:
        raise ValueError("track overran its declared length (%d != %d)" % (j, end))

    return events, end


def parse(data):
    if data[:4] != b"MThd":
        raise ValueError("not a MIDI file")

    fmt, ntracks, division = struct.unpack(">HHH", data[8:14])
    i = 14
    tracks = []

    for _ in range(ntracks):
        events, i = parse_track(data, i)
        tracks.append(events)

    if i != len(data):
        raise ValueError("trailing bytes after the last track")

    return fmt, division, tracks


def build(fmt, division, tracks):
    out = bytearray(b"MThd" + struct.pack(">IHHH", 6, fmt, len(tracks), division))

    for events in tracks:
        body = bytearray()
        previous = 0

        for event in events:
            body += write_vlq(event.tick - previous)
            body += event.data
            previous = event.tick

        body += write_vlq(0) + bytes([META, END_OF_TRACK, 0])
        out += b"MTrk" + struct.pack(">I", len(body)) + body

    return bytes(out)


def stem_for(tracks, wanted):
    """Every non-channel event, plus the channels on `wanted`'s curve.

    Meta and sysex are copied into all three stems so that tempo, time
    signature and track structure stay identical and the renders line up
    sample for sample -- they are played simultaneously, so any drift between
    them is audible as a phasing artefact rather than as a wrong mix.
    """

    kept = []
    notes = 0

    for events in tracks:
        out = []

        for event in events:
            if event.channel is None:
                out.append(event)
                continue

            if stem_of(event.channel) != wanted:
                continue

            # The game owns channel volume; see the module docstring.
            if event.is_volume:
                continue

            if event.is_note_on:
                notes += 1

            out.append(event)

        kept.append(out)

    return kept, notes


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("files", nargs="*", help="MIDI files (default: Music/*.mid)")
    parser.add_argument("--outdir", default="build-music", help="where to write stems")
    args = parser.parse_args()

    files = args.files

    if not files:
        import glob

        files = sorted(glob.glob("Music/*.mid"))

    if not files:
        sys.exit("no MIDI files found; run from the repository root")

    os.makedirs(args.outdir, exist_ok=True)
    written = 0
    skipped = []
    manifest = {}

    for path in files:
        with open(path, "rb") as handle:
            fmt, division, tracks = parse(handle.read())

        base = os.path.splitext(os.path.basename(path))[0]
        emitted = []

        for stem in STEMS:
            kept, notes = stem_for(tracks, stem)

            # A stem with no notes is not silence worth shipping: Cathedral has
            # nothing on the fade-out curve, and the placeholder that six of the
            # eleven files are byte-identical to has nothing on any curve. The
            # player simply never asks for a stem that was not emitted.
            if notes == 0:
                continue

            target = os.path.join(args.outdir, "%s.%s.mid" % (base, stem))

            with open(target, "wb") as handle:
                handle.write(build(fmt, division, kept))

            emitted.append("%s(%d)" % (stem, notes))
            written += 1

        # Recorded even when empty. The page has to be able to tell "this track
        # is deliberately silent" from "this deploy is missing a file", and six
        # of the eleven tracks are legitimately silent -- probing for them and
        # swallowing the 404 would make a broken deploy look normal.
        manifest[os.path.basename(path)] = [s.split("(")[0] for s in emitted]

        if emitted:
            print("%-16s %s" % (base, " ".join(emitted)))
        else:
            skipped.append(base)

    with open(os.path.join(args.outdir, "stems.json"), "w") as handle:
        json.dump(manifest, handle, indent=2, sort_keys=True)
        handle.write("\n")

    if skipped:
        print("\nno notes, nothing emitted: %s" % ", ".join(skipped))

    print("\n%d stems written to %s/, plus stems.json" % (written, args.outdir))


if __name__ == "__main__":
    main()
