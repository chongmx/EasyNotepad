# Subsets the metric-compatible document fonts (Latin, Greek, Cyrillic, punctuation, symbols),
# strips hinting, and packs them into one deflated zip with their licence texts.
#   pack_fonts.py <unpacked .deb root> <out.zip>
import io, os, sys, zipfile
from fontTools import subset
from fontTools.ttLib import TTFont

root, out = sys.argv[1], sys.argv[2]
faces = []
for d in ("crosextra", "liberation"):
    folder = os.path.join(root, "usr/share/fonts/truetype", d)
    faces += [os.path.join(folder, f) for f in sorted(os.listdir(folder)) if f.endswith(".ttf")]

ranges = [(0x20, 0x7E), (0xA0, 0x24F), (0x250, 0x2FF), (0x300, 0x36F), (0x370, 0x3FF), (0x400, 0x4FF), (0x1E00, 0x1EFF),
          (0x2000, 0x206F), (0x20A0, 0x20CF), (0x2100, 0x214F), (0x2150, 0x218F), (0x2190, 0x21FF), (0x2200, 0x22FF),
          (0x2300, 0x23FF), (0x2500, 0x257F), (0x25A0, 0x25FF), (0x2600, 0x26FF), (0xFB00, 0xFB06), (0xFFFD, 0xFFFD)]
unicodes = [u for a, b in ranges for u in range(a, b + 1)]

options = subset.Options()
options.hinting = False
options.desubroutinize = True
options.layout_features = ["kern", "liga", "clig", "calt", "ccmp", "locl", "mark", "mkmk"]
options.name_IDs = ["*"]
options.notdef_outline = True
options.glyph_names = False
options.drop_tables += ["DSIG", "hdmx", "VDMX", "LTSH", "gasp"]

# The OFL's Reserved Font Names (Carlito, Liberation) may not name a modified version: the subsets
# get names of their own. Original family -> new family.
renames = {"Carlito": "EnpDoc SansC", "Caladea": "EnpDoc SerifC", "Liberation Sans": "EnpDoc Sans",
           "Liberation Serif": "EnpDoc Serif", "Liberation Mono": "EnpDoc Mono"}


def rename(font):
    name = font["name"]
    family = name.getDebugName(16) or name.getDebugName(1)
    style = name.getDebugName(17) or name.getDebugName(2) or "Regular"
    new = renames[family]
    full = f"{new} {style}" if style != "Regular" else new
    ps = f"{new.replace(' ', '')}-{style.replace(' ', '')}"
    for rec in list(name.names):
        if rec.nameID in (1, 3, 4, 6, 16, 17, 18, 21, 22):
            name.removeNames(nameID=rec.nameID)
    for pid, eid, lid in ((3, 1, 0x409), (1, 0, 0)):
        name.setName(new, 1, pid, eid, lid)
        name.setName(style, 2, pid, eid, lid)
        name.setName(f"{ps};subset for EasyNotepad", 3, pid, eid, lid)
        name.setName(full, 4, pid, eid, lid)
        name.setName(ps, 6, pid, eid, lid)
    note = (f"Subset of {family} (Latin, Greek, Cyrillic, punctuation, symbols; hinting removed), "
            f"renamed as the SIL Open Font License requires of modified versions.")
    name.setName(note, 10, 3, 1, 0x409)
    return ps


total_in = total_sub = 0
with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
    for path in faces:
        font = TTFont(path)
        s = subset.Subsetter(options)
        s.populate(unicodes=unicodes)
        s.subset(font)
        ps = rename(font)
        buf = io.BytesIO()
        font.save(buf)
        data = buf.getvalue()
        total_in += os.path.getsize(path)
        total_sub += len(data)
        z.writestr(ps + ".ttf", data)
        print(f"{os.path.basename(path):32s} {os.path.getsize(path):8d} -> {len(data):8d}  {ps}.ttf")
    for pkg in ("fonts-crosextra-carlito", "fonts-crosextra-caladea", "fonts-liberation"):
        lic = os.path.join(root, "usr/share/doc", pkg, "copyright")
        if os.path.exists(lic):
            z.write(lic, f"LICENSE-{pkg.replace('fonts-', '').replace('crosextra-', '')}.txt")
print(f"fonts: {total_in} -> {total_sub} subset; zip {os.path.getsize(out)} bytes")
