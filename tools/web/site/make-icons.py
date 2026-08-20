#!/usr/bin/env python3

"""Derive the site's favicons from ivan.ico. Not run at build time.

    tools/web/site/make-icons.py

Writes, beside this script:

    favicon.ico   the 16, 32 and 48 layers, PNG-encoded. What tabs use.
    icon.png      the 128 layer, RGBA. The high-DPI and fallback icon.

`ivan.ico` is the master and holds six hand-drawn sizes (16 to 256). Those
small layers are drawn, not scaled: a browser handed only the 128 and asked
for 16 smooth-scales it, and smooth-scaling pixel art is how the pick-axe
turns to mush. That is the whole reason the .ico is still here and the whole
reason favicon.ico ships alongside a perfectly good PNG.

The layers are re-encoded as PNG rather than copied as the original BMPs
because PNG-in-ICO is the same pixels an eighth of the size (2.2KB against
15KB). Every browser that can run a wasm build reads it.

This does NOT come from Graphics/Icon.bmp, which is a different and worse
image for this purpose: 32x32, 8-bit, no alpha, and it is a live runtime asset
(igraph.cpp:74 hands it to SDL_SetWindowIcon). icon.png used to be that file
upscaled to 128, which is why it was blocky and sat on a white box.
"""

import io
import os
import struct
import sys

try:
    from PIL import Image
except ImportError:
    sys.exit("needs Pillow: pip install Pillow")

HERE = os.path.dirname(os.path.abspath(__file__))
MASTER = os.path.join(HERE, "ivan.ico")
FAVICON_SIZES = (16, 32, 48)
PNG_SIZE = 128


def build_ico(layers):
    """Assemble an ICO from already-encoded PNG blobs.

    Written out by hand because Pillow's ICO writer resizes a single image to
    the sizes it is given rather than storing the distinct layers handed to it,
    which would throw away the hand-drawn small sizes this exists to keep.
    """

    out = bytearray(struct.pack("<HHH", 0, 1, len(layers)))
    offset = 6 + 16 * len(layers)

    for size, blob in layers:
        # A 0 in the width/height byte means 256; nothing here is that big.
        out += struct.pack("<BBBBHHII", size, size, 0, 0, 1, 32, len(blob), offset)
        offset += len(blob)

    for _, blob in layers:
        out += blob

    return bytes(out)


def main():
    master = Image.open(MASTER)
    available = {w for w, h in master.ico.sizes()}
    missing = [s for s in FAVICON_SIZES + (PNG_SIZE,) if s not in available]

    if missing:
        sys.exit("ivan.ico has no %s layer; it holds %s"
                 % (missing, sorted(available)))

    layers = []

    for size in FAVICON_SIZES:
        buf = io.BytesIO()
        master.ico.getimage((size, size)).convert("RGBA").save(
            buf, format="PNG", optimize=True)
        layers.append((size, buf.getvalue()))

    favicon = os.path.join(HERE, "favicon.ico")

    with open(favicon, "wb") as handle:
        handle.write(build_ico(layers))

    icon = os.path.join(HERE, "icon.png")
    master.ico.getimage((PNG_SIZE, PNG_SIZE)).convert("RGBA").save(
        icon, format="PNG", optimize=True)

    # Re-read rather than trust the write: a favicon that is subtly wrong is
    # invisible until it is on the CDN.
    check = Image.open(favicon)

    for size in FAVICON_SIZES:
        got = list(check.ico.getimage((size, size)).convert("RGBA").get_flattened_data())
        want = list(master.ico.getimage((size, size)).convert("RGBA").get_flattened_data())

        if got != want:
            sys.exit("favicon.ico %dx%d does not match the master" % (size, size))

    print("favicon.ico  %s  %d bytes" % ("/".join(str(s) for s in FAVICON_SIZES),
                                         os.path.getsize(favicon)))
    print("icon.png     %dx%d  %d bytes" % (PNG_SIZE, PNG_SIZE, os.path.getsize(icon)))


if __name__ == "__main__":
    main()
