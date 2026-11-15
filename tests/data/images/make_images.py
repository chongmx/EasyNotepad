#!/usr/bin/env python3
"""Makes the image fixtures: one small picture per format and variant the editor opens.

    python3 tests/data/images/make_images.py      (needs Pillow: pip install pillow)

Every picture is 8 x 4 pixels: its left half red and its right half blue (grey ones: light and
dark), so a test can tell it was decoded, not only recognised. The same Pillow gives the same
files. Formats the app does not decode yet (WebP, TIFF, AVIF, JPEG 2000) are made too: they must
be recognised as images and say why they cannot be shown, never be taken for binary files.
"""
import os
import struct

from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
W, H = 8, 4


def colour():
    img = Image.new("RGB", (W, H), (230, 20, 20))
    img.paste((20, 20, 230), (W // 2, 0, W, H))
    return img


def grey():
    img = Image.new("L", (W, H), 220)
    img.paste(30, (W // 2, 0, W, H))
    return img


def save(img, name, **options):
    img.save(os.path.join(HERE, name), **options)


def hdr(name):
    # Radiance HDR, flat (not run-length) RGBE pixels: a header, then 4 bytes a pixel.
    out = bytearray(b"#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y %d +X %d\n" % (H, W))
    for _ in range(H):
        for x in range(W):
            out += bytes((200, 10, 10, 128)) if x < W // 2 else bytes((10, 10, 200, 128))
    with open(os.path.join(HERE, name), "wb") as f:
        f.write(out)


def main():
    rgb, l = colour(), grey()
    rgba = rgb.convert("RGBA")
    rgba.putpixel((0, 0), (230, 20, 20, 0))  # a transparent corner
    save(rgb, "rgb.png")
    save(rgba, "rgba.png")
    save(rgb.convert("P", palette=Image.Palette.ADAPTIVE, colors=4), "palette.png")
    save(l, "grey.png")
    save(l.point(lambda v: v * 257, "I").convert("I;16"), "grey16.png")  # 16-bit: 220 -> 56540
    save(rgb, "upper.PNG")                     # the extension in capitals
    save(rgb, "png-named.jpg", format="PNG")   # the name says JPEG, the content PNG
    save(rgb, "baseline.jpg", quality=95)
    save(rgb, "progressive.jpeg", quality=95, progressive=True)
    save(l, "grey.jpg", quality=95)
    save(rgb, "subsampled.jpg", quality=95, subsampling=2)
    save(rgb, "anim.gif")
    save(rgb, "rgb.bmp")
    save(rgba, "rgba.bmp")
    save(rgb.convert("P", palette=Image.Palette.ADAPTIVE, colors=4), "palette.bmp")
    save(rgb, "rgb.tga")
    save(rgb, "rle.tga", compression="tga_rle")
    save(rgb, "rgb.ppm")
    save(l, "grey.pgm")
    save(rgb, "rgb.qoi")
    save(rgba, "icon.ico", sizes=[(W, H)])
    hdr("flat.hdr")
    # Recognised, not decoded yet.
    save(rgb, "lossy.webp", quality=90)
    save(rgb, "rgb.tif")
    save(rgb, "rgb.jp2")


if __name__ == "__main__":
    main()
