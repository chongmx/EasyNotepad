#!/usr/bin/env python3
"""Builds the emoji pack and the emoji property table for rendered Markdown.

    python3 scripts/emoji/build_emoji_pack.py [--noto-commit SHA] [--unicode 18.0] [--cache DIR]

Writes:
  resources/emoji/noto-emoji-72.zip   every 72 px PNG of Noto Emoji (2D), stored uncompressed (the PNGs
                                      are already compressed; deflate gains nothing), named
                                      "<key>.png": lower-case hex code points of at least 4 digits joined
                                      by "_", without U+FE0F (e.g. "2b07", "1f469_200d_1f52c", "0023_20e3")
  resources/emoji/LICENSE             the image licence (Apache 2.0; flags are public domain)
  resources/emoji/README.md           where it came from (commit, Unicode version, coverage)
  src/model/EmojiData.inc             code point ranges with the emoji properties the segmenter needs
                                      (Emoji, Emoji_Presentation, Emoji_Modifier_Base, Extended_Pictographic)

Country and subdivision flags are not in the 72 px folder: they come from third_party/region-flags
(public domain), scaled into the same 72 px square with Pillow (pip install pillow); without
Pillow they are left out and flags show as their two letters. Everything else uses only the
standard library. Downloads are cached (--cache), so a rerun is quick.
"""
import argparse
import concurrent.futures
import hashlib
import io
import json
import os
import re
import sys
import time
import urllib.request
import zipfile

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
NOTO = "googlefonts/noto-emoji"
PNG_DIR = "2D/png/72"


def fetch(url, cache_dir=None, attempts=4):
    path = None
    if cache_dir:
        path = os.path.join(cache_dir, hashlib.sha1(url.encode()).hexdigest())
        if os.path.exists(path):
            return open(path, "rb").read()
    for attempt in range(attempts):
        try:
            data = urllib.request.urlopen(urllib.request.Request(url, headers={"User-Agent": "easynotepad-emoji-build"}),
                                          timeout=60).read()
            if path:
                with open(path + ".tmp", "wb") as f:
                    f.write(data)
                os.replace(path + ".tmp", path)
            return data
        except Exception:
            if attempt + 1 == attempts:
                raise
            time.sleep(1.5 * (attempt + 1))


def key_of(codepoints):
    """The pack's key for a sequence: hex of at least 4 digits, '_'-joined, FE0F dropped."""
    return "_".join("%04x" % cp for cp in codepoints if cp != 0xFE0F)


def noto_key(filename):
    m = re.fullmatch(r"emoji_u([0-9a-fA-F_]+)\.png", filename)
    if not m:
        return None
    return key_of([int(part, 16) for part in m.group(1).split("_")])


def parse_ranges(text, wanted):
    """emoji-data.txt -> {property: [(lo, hi)]}"""
    props = {p: [] for p in wanted}
    for line in text.splitlines():
        line = line.split("#", 1)[0].strip()
        if not line:
            continue
        cps, prop = [part.strip() for part in line.split(";")[:2]]
        if prop not in props:
            continue
        lo, _, hi = cps.partition("..")
        props[prop].append((int(lo, 16), int(hi or lo, 16)))
    return props


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--noto-commit", help="Noto Emoji commit (default: the current main)")
    ap.add_argument("--unicode", default="18.0", help="Unicode version for the emoji properties and the coverage report")
    ap.add_argument("--cache", default=os.path.join(ROOT, "build", "emoji-cache"))
    args = ap.parse_args()
    os.makedirs(args.cache, exist_ok=True)

    commit = args.noto_commit
    if not commit:
        info = json.loads(fetch(f"https://api.github.com/repos/{NOTO}/commits/main"))
        commit = info["sha"]
    print(f"Noto Emoji commit {commit}", flush=True)
    tree = json.loads(fetch(f"https://api.github.com/repos/{NOTO}/git/trees/{commit}?recursive=1", args.cache))
    if tree.get("truncated"):
        sys.exit("the repository listing is truncated")
    names = sorted(e["path"].rsplit("/", 1)[1] for e in tree["tree"]
                   if e["type"] == "blob" and e["path"].startswith(PNG_DIR + "/") and e["path"].endswith(".png"))
    print(f"{len(names)} images in {PNG_DIR}", flush=True)

    def download(name):
        return name, fetch(f"https://raw.githubusercontent.com/{NOTO}/{commit}/{PNG_DIR}/{name}", args.cache)

    images = {}
    with concurrent.futures.ThreadPoolExecutor(24) as pool:
        for i, (name, data) in enumerate(pool.map(download, names), 1):
            key = noto_key(name)
            if key and data[:8] == b"\x89PNG\r\n\x1a\n":
                images[key] = data
            if i % 500 == 0:
                print(f"  {i}/{len(names)}", flush=True)
    licence = fetch(f"https://raw.githubusercontent.com/{NOTO}/{commit}/LICENSE", args.cache)

    # Flags: "MY.png" -> regional indicators M Y; "GB-ENG.png" -> black flag + tags "gbeng" + cancel tag.
    flag_names = sorted(e["path"].rsplit("/", 1)[1] for e in tree["tree"]
                        if e["type"] == "blob" and e["path"].startswith("third_party/region-flags/png/"))
    try:
        from PIL import Image
    except ImportError:
        Image = None
        print("Pillow is not installed: flags are left out", flush=True)
    if Image:
        def flag_key(stem):
            if re.fullmatch(r"[A-Z]{2}", stem):
                return key_of([0x1F1E6 + ord(c) - ord("A") for c in stem])
            if re.fullmatch(r"[A-Z]{2}-[A-Z0-9]{1,3}", stem):
                tags = [0xE0000 + ord(c) for c in stem.replace("-", "").lower()]
                return key_of([0x1F3F4] + tags + [0xE007F])
            return None

        def flag_png(name):
            data = fetch(f"https://raw.githubusercontent.com/{NOTO}/{commit}/third_party/region-flags/png/{name}", args.cache)
            # A git symlink downloads as its target's name ("FR.png"): follow it once.
            if not data.startswith(b"\x89PNG") and len(data) < 256 and data.strip().endswith(b".png"):
                target = data.strip().decode().rsplit("/", 1)[-1]
                data = fetch(f"https://raw.githubusercontent.com/{NOTO}/{commit}/third_party/region-flags/png/{target}", args.cache)
            return name, data

        added = 0
        with concurrent.futures.ThreadPoolExecutor(16) as pool:
            for name, data in pool.map(flag_png, flag_names):
                key = flag_key(name[:-4])
                if not key or key in images or not data.startswith(b"\x89PNG"):
                    continue
                flag = Image.open(io.BytesIO(data)).convert("RGBA")
                w, h = flag.size
                scale = min(68.0 / w, 56.0 / h) # inside the 72 px square, a little margin, as the font draws them
                flag = flag.resize((max(1, round(w * scale)), max(1, round(h * scale))), Image.LANCZOS)
                canvas = Image.new("RGBA", (72, 72), (0, 0, 0, 0))
                canvas.paste(flag, ((72 - flag.width) // 2, (72 - flag.height) // 2))
                out = io.BytesIO()
                canvas.save(out, "PNG", optimize=True)
                images[key] = out.getvalue()
                added += 1
        print(f"{added} flags added from third_party/region-flags", flush=True)

    out_dir = os.path.join(ROOT, "resources", "emoji")
    os.makedirs(out_dir, exist_ok=True)
    pack = os.path.join(out_dir, "noto-emoji-72.zip")
    with zipfile.ZipFile(pack + ".tmp", "w", zipfile.ZIP_STORED) as z:
        for key in sorted(images):
            info = zipfile.ZipInfo(key + ".png", date_time=(1980, 1, 1, 0, 0, 0)) # reproducible
            info.compress_type = zipfile.ZIP_STORED
            z.writestr(info, images[key])
    os.replace(pack + ".tmp", pack)
    with open(os.path.join(out_dir, "LICENSE"), "wb") as f:
        f.write(licence)

    # Properties for the segmenter, from the Unicode version asked for.
    version = args.unicode
    data_txt = fetch(f"https://www.unicode.org/Public/{version}.0/ucd/emoji/emoji-data.txt", args.cache).decode()
    wanted = ["Emoji", "Emoji_Presentation", "Emoji_Modifier_Base", "Extended_Pictographic"]
    props = parse_ranges(data_txt, wanted)
    bits = {"Emoji": 1, "Emoji_Presentation": 2, "Emoji_Modifier_Base": 4, "Extended_Pictographic": 8}
    flags = {}
    for prop, ranges in props.items():
        for lo, hi in ranges:
            for cp in range(lo, hi + 1):
                flags[cp] = flags.get(cp, 0) | bits[prop]
    runs = []
    for cp in sorted(flags):
        if runs and runs[-1][1] == cp - 1 and runs[-1][2] == flags[cp]:
            runs[-1][1] = cp
        else:
            runs.append([cp, cp, flags[cp]])
    inc = os.path.join(ROOT, "src", "model", "EmojiData.inc")
    with open(inc, "w") as f:
        f.write(f"// Generated by scripts/emoji/build_emoji_pack.py from Unicode {version} emoji-data.txt. Do not edit.\n")
        f.write("// {first, last, flags}: 1 Emoji, 2 Emoji_Presentation, 4 Emoji_Modifier_Base, 8 Extended_Pictographic.\n")
        f.write(f"#define ENP_EMOJI_UNICODE_VERSION \"{version}\"\n")
        for i in range(0, len(runs), 4):
            f.write("    " + " ".join("{0x%X, 0x%X, %d}," % tuple(r) for r in runs[i:i + 4]) + "\n")

    # Coverage: the fully-qualified sequences of each emoji version, against the pack.
    report = []
    for v in sorted({"16.0", "17.0", version}, key=lambda s: tuple(map(int, s.split(".")))):
        url = (f"https://www.unicode.org/Public/emoji/{v}/emoji-test.txt" if v == "16.0"
               else f"https://www.unicode.org/Public/{v}.0/emoji/emoji-test.txt")
        try:
            test = fetch(url, args.cache).decode()
        except Exception:
            continue
        rgi = []
        for line in test.splitlines():
            m = re.match(r"^([0-9A-F ]+?)\s*;\s*fully-qualified", line)
            if m:
                rgi.append(key_of([int(x, 16) for x in m.group(1).split()]))
        missing = [k for k in rgi if k not in images]
        report.append((v, len(rgi), len(rgi) - len(missing), missing))
        print(f"Emoji {v}: {len(rgi) - len(missing)} of {len(rgi)} recommended emoji have a picture"
              + (f" (missing e.g. {', '.join(missing[:6])})" if missing else ""), flush=True)

    with open(os.path.join(out_dir, "README.md"), "w") as f:
        f.write("# Emoji pack\n\n")
        f.write(f"`noto-emoji-72.zip`: the {len(images)} 72 px PNG images of [Noto Emoji](https://github.com/{NOTO}) "
                f"(`{PNG_DIR}`, commit `{commit}`), stored uncompressed and named by code points "
                "(hex, at least 4 digits, `_`-joined, without U+FE0F).\n\n")
        f.write("Country and subdivision flags come from `third_party/region-flags` (public domain), scaled into the "
                "same 72 px square.\n\n")
        f.write("Licence: Apache License 2.0 (see `LICENSE`); flag images are public domain.\n\n")
        f.write(f"Emoji properties (`src/model/EmojiData.inc`) are from Unicode {version}.\n\n")
        for v, total, found, missing in report:
            f.write(f"- Emoji {v}: {found} of {total} recommended emoji have a picture.\n")
        f.write("\nRebuild with `python3 scripts/emoji/build_emoji_pack.py`.\n")
    size = os.path.getsize(pack)
    print(f"wrote {pack} ({size / 1048576:.2f} MB, {len(images)} images) and {inc} ({len(runs)} ranges)")


if __name__ == "__main__":
    main()
