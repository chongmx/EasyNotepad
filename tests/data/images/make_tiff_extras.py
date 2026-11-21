#!/usr/bin/env python3
"""Makes the TIFF fixtures Windows' encoder (make_tiffs.ps1) cannot: tiles, the horizontal
predictor, planar samples, CMYK, big-endian 16-bit. Python's standard library only.

    python3 tests/data/images/make_tiff_extras.py

Each is red on the left half and blue on the right (grey: light, dark), as the other fixtures.
"""
import os
import struct
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
RED, BLUE = (230, 20, 20), (20, 20, 230)


def write(name, width, height, tags, chunks, big_endian=False, tiled=False):
    """tags: {id: (type, [values])}; chunks: the compressed strips or tiles, in order."""
    e = ">" if big_endian else "<"
    out = bytearray(b"MM\x00\x2a" if big_endian else b"II\x2a\x00") + b"\0\0\0\0"
    offsets = []
    for c in chunks:
        offsets.append(len(out))
        out += c
        if len(out) % 2:
            out += b"\0"
    tags = dict(tags)
    tags[256] = (4, [width])
    tags[257] = (4, [height])
    tags[324 if tiled else 273] = (4, offsets)
    tags[325 if tiled else 279] = (4, [len(c) for c in chunks])
    # Values that do not fit in an entry go before the directory.
    size = {3: 2, 4: 4}
    entries = []
    for tag in sorted(tags):
        typ, vals = tags[tag]
        data = b"".join(struct.pack(e + ("H" if typ == 3 else "I"), v) for v in vals)
        if len(data) > 4:
            at = len(out)
            out += data + (b"\0" if len(data) % 2 else b"")
            entries.append(struct.pack(e + "HHII", tag, typ, len(vals), at))
        else:
            entries.append(struct.pack(e + "HHI", tag, typ, len(vals)) + data.ljust(4, b"\0"))
    ifd = len(out)
    out += struct.pack(e + "H", len(entries)) + b"".join(entries) + b"\0\0\0\0"
    out[4:8] = struct.pack(e + "I", ifd)
    with open(os.path.join(HERE, name), "wb") as f:
        f.write(out)


def pixel(x, width):
    return RED if x < width // 2 else BLUE


def packbits(data):
    out = bytearray()
    for i in range(0, len(data), 128):
        part = data[i:i + 128]
        out += bytes([len(part) - 1]) + part
    return bytes(out)


# Tiled RGB, Deflate with the predictor: 40 x 20 in 16 x 16 tiles (3 across, 2 down).
W, H, T = 40, 20, 16
tiles = []
for ty in range(0, H, T):
    for tx in range(0, W, T):
        rows = bytearray()
        for y in range(T):
            prev = (0, 0, 0)
            for x in range(T):
                px = pixel(tx + x, W) if tx + x < W and ty + y < H else (0, 0, 0)
                rows += bytes((px[c] - prev[c]) & 255 for c in range(3))
                prev = px
        tiles.append(zlib.compress(bytes(rows)))
write("tiled-predictor.tif", W, H, {258: (3, [8, 8, 8]), 259: (3, [8]), 262: (3, [2]), 277: (3, [3]), 284: (3, [1]),
                                     317: (3, [2]), 322: (3, [T]), 323: (3, [T])}, tiles, tiled=True)

# Planar RGB (all the red, then the green, then the blue), PackBits.
w, h = 8, 4
planes = [packbits(bytes(pixel(x, w)[c] for y in range(h) for x in range(w))) for c in range(3)]
write("planar.tif", w, h, {258: (3, [8, 8, 8]), 259: (3, [32773]), 262: (3, [2]), 277: (3, [3]), 278: (4, [h]),
                           284: (3, [2])}, planes)

# CMYK, uncompressed: red is magenta and yellow ink, blue cyan and magenta.
cmyk = bytes(v for y in range(h) for x in range(w) for v in ((0, 225, 225, 0) if x < w // 2 else (225, 225, 0, 0)))
write("cmyk.tif", w, h, {258: (3, [8, 8, 8, 8]), 259: (3, [1]), 262: (3, [5]), 277: (3, [4]), 278: (4, [h])}, [cmyk])

# Big-endian 16-bit grey, Deflate with the predictor: light left, dark right.
rows = bytearray()
for y in range(h):
    prev = 0
    for x in range(w):
        v = (220 if x < w // 2 else 30) * 257
        rows += struct.pack(">H", (v - prev) & 0xFFFF)
        prev = v
write("grey16-predictor-be.tif", w, h, {258: (3, [16]), 259: (3, [8]), 262: (3, [1]), 277: (3, [1]), 278: (4, [h]),
                                         317: (3, [2])}, [zlib.compress(bytes(rows))], big_endian=True)
print("made the extra TIFF fixtures in", HERE)
