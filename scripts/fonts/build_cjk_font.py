#!/usr/bin/env python3
"""Builds the bundled CJK font: Noto Sans CJK SC cut to the letters in common use.

    python3 scripts/fonts/build_cjk_font.py [--noto-commit SHA] [--cache DIR]

Needs fontTools (pip install fonttools). Writes:
  resources/fonts/NotoSansCJK-Common.otf   CFF outlines (ImGui's loader cannot read CFF2), no hinting
                                           or layout tables: the letters of GB 2312 (simplified
                                           Chinese), Big5 level 1 (traditional), JIS X 0208 level 1
                                           (Japanese), KS X 1001 Hangul (Korean), kana, bopomofo, CJK
                                           punctuation and full-width forms
  resources/fonts/NotoSansCJK-LICENSE.txt  the font's licence (SIL Open Font License 1.1)

The app uses it only when the system has no CJK font ImGui can read (src/view/Fonts.cpp:
ScriptFonts). The same commit and options give the same file.
"""
import argparse
import hashlib
import os
import sys
import urllib.request

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
NOTO = "notofonts/noto-cjk"
COMMIT = "f8d157532fbfaeda587e826d4cd5b21a49186f7c"
SOURCE = "Sans/OTF/SimplifiedChinese/NotoSansCJKsc-Regular.otf"


def fetch(url, cache_dir):
    path = os.path.join(cache_dir, hashlib.sha256(url.encode()).hexdigest()[:16] + "-" + url.rsplit("/", 1)[-1]) if cache_dir else None
    if path and os.path.exists(path):
        with open(path, "rb") as f:
            return f.read()
    with urllib.request.urlopen(url, timeout=120) as r:
        data = r.read()
    if path:
        os.makedirs(cache_dir, exist_ok=True)
        with open(path, "wb") as f:
            f.write(data)
    return data


def double_byte(codec, leads, trails):
    out = set()
    for a in leads:
        for b in trails:
            try:
                text = bytes([a, b]).decode(codec)
            except UnicodeDecodeError:
                continue
            out.update(ord(c) for c in text)
    return out


def letters():
    cps = set()
    cps |= double_byte("gb2312", range(0xA1, 0xF8), range(0xA1, 0xFF))  # 6763 Han: simplified Chinese
    cps |= double_byte("big5", range(0xA1, 0xC7), list(range(0x40, 0x7F)) + list(range(0xA1, 0xFF)))  # level 1: 5401
    cps |= double_byte("euc_jp", range(0xA1, 0xD0), range(0xA1, 0xFF))  # JIS level 1: 2965 kanji
    cps |= double_byte("euc_kr", range(0xA1, 0xCA), range(0xA1, 0xFF))  # symbols and 2350 Hangul
    for lo, hi in [(0x3000, 0x303F), (0x3040, 0x30FF), (0x3100, 0x312F), (0x3130, 0x318F), (0x31F0, 0x31FF),
                   (0xFF00, 0xFFEF)]:
        cps.update(range(lo, hi + 1))
    return sorted(c for c in cps if c >= 0x2E80)  # Latin and the like: the app's own fonts


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--noto-commit", default=COMMIT)
    ap.add_argument("--cache", help="keep the downloads here")
    args = ap.parse_args()
    try:
        from fontTools import subset
        from fontTools.ttLib import TTFont
    except ImportError:
        sys.exit("fontTools is needed: pip install fonttools")

    base = f"https://raw.githubusercontent.com/{NOTO}/{args.noto_commit}/"
    source = fetch(base + SOURCE, args.cache)
    licence = fetch(base + "Sans/LICENSE", args.cache)
    out_dir = os.path.join(ROOT, "resources", "fonts")
    src_path = os.path.join(args.cache or out_dir, "NotoSansCJKsc-Regular.source.otf")
    with open(src_path, "wb") as f:
        f.write(source)

    options = subset.Options()
    options.layout_features = []
    options.hinting = False
    options.notdef_outline = True
    options.name_IDs = ["*"]
    options.drop_tables += ["vhea", "vmtx", "VORG", "BASE", "GPOS", "GSUB", "GDEF"]
    font = TTFont(src_path, recalcTimestamp=False)
    sub = subset.Subsetter(options)
    sub.populate(unicodes=letters())
    sub.subset(font)
    out = os.path.join(out_dir, "NotoSansCJK-Common.otf")
    font.save(out)
    if not args.cache:
        os.remove(src_path)
    with open(os.path.join(out_dir, "NotoSansCJK-LICENSE.txt"), "wb") as f:
        f.write(licence)
    with open(out, "rb") as f:
        data = f.read()
    print(f"{out}: {len(data)} bytes, {len(letters())} letters, sha256 {hashlib.sha256(data).hexdigest()}")


if __name__ == "__main__":
    main()
