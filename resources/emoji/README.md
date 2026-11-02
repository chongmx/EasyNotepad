# Emoji pack

`noto-emoji-72.zip`: the 4109 72 px PNG images of [Noto Emoji](https://github.com/googlefonts/noto-emoji) (`2D/png/72`, commit `99701e07d3d1fe1bdbcdb14964547f0fd592966f`), stored uncompressed and named by code points (hex, at least 4 digits, `_`-joined, without U+FE0F).

Country and subdivision flags come from `third_party/region-flags` (public domain), scaled into the same 72 px square.

Licence: Apache License 2.0 (see `LICENSE`); flag images are public domain.

Emoji properties (`src/model/EmojiData.inc`) are from Unicode 18.0.

- Emoji 16.0: 3781 of 3781 recommended emoji have a picture.
- Emoji 17.0: 3944 of 3944 recommended emoji have a picture.
- Emoji 18.0: 3963 of 3963 recommended emoji have a picture.

Rebuild with `python3 scripts/emoji/build_emoji_pack.py`.
