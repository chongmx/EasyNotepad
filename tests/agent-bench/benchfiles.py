"""What the real-life sets' make.py scripts share: the folder they write, the questions and media
lists, the checks, and the file formats (PDF, Word, Excel, e-mail) written with the standard library.

    import benchfiles as b
    b.start(here)            # writes into here/data
    ... b.ask(...), b.pdf(...), b.media["speech"].append(...)
    b.finish(about)          # here/media.json and here/questions.json
"""
import json
import os
import zipfile
from xml.sax.saxutils import escape

DATA = ""
HERE = ""
questions = []
media = {"speech": [], "pictures": []}


def start(here):
    global DATA, HERE
    HERE, DATA = here, os.path.join(here, "data")
    questions.clear()
    media["speech"].clear()
    media["pictures"].clear()


def finish(about):
    os.makedirs(DATA, exist_ok=True)
    with open(os.path.join(HERE, "media.json"), "w", encoding="utf-8") as f:
        json.dump(media, f, indent=1)
    with open(os.path.join(HERE, "questions.json"), "w", encoding="utf-8") as f:
        json.dump({"about": about, "questions": questions}, f, indent=1)
    print(len(questions), "questions;", len(media["speech"]), "recordings;", len(media["pictures"]), "photos to make")


def path_of(rel):
    full = os.path.join(DATA, rel)
    os.makedirs(os.path.dirname(full), exist_ok=True)
    return full


def write(rel, text):
    with open(path_of(rel), "w", encoding="utf-8", newline="\n") as f:
        f.write(text)


def ask(qid, kind, question, *checks):
    questions.append({"id": qid, "kind": kind, "question": question, "checks": list(checks)})


def number(v, tol=0.01):
    return {"number": round(v, 6), "tol": tol}


def text(*words):
    return {"text": list(words)}


def money(v):
    return f"{v:,.2f}"


# --- File formats, written with the standard library -------------------------------------------

def pdf(rel, lines):
    """A one-page PDF with the lines in Helvetica (a text layer, as a PDF printed from an app)."""
    def esc(s):
        return s.replace("\\", "\\\\").replace("(", "\\(").replace(")", "\\)")
    body = ["BT", "/F1 11 Tf", "14 TL", "56 790 Td"]
    for line in lines:
        if line.startswith("# "):
            body += ["/F1 16 Tf", f"({esc(line[2:])}) Tj", "T*", "/F1 11 Tf", "T*"]
        else:
            body += [f"({esc(line)}) Tj", "T*"]
    body.append("ET")
    stream = "\n".join(body).encode("latin-1")
    objects = [
        b"<< /Type /Catalog /Pages 2 0 R >>",
        b"<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
        b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 595 842] /Resources << /Font << /F1 5 0 R >> >> /Contents 4 0 R >>",
        b"<< /Length " + str(len(stream)).encode() + b" >>\nstream\n" + stream + b"\nendstream",
        b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /Encoding /WinAnsiEncoding >>",
    ]
    out = bytearray(b"%PDF-1.4\n")
    offsets = []
    for i, o in enumerate(objects, 1):
        offsets.append(len(out))
        out += f"{i} 0 obj\n".encode() + o + b"\nendobj\n"
    xref = len(out)
    out += f"xref\n0 {len(objects) + 1}\n0000000000 65535 f \n".encode()
    for off in offsets:
        out += f"{off:010d} 00000 n \n".encode()
    out += f"trailer\n<< /Size {len(objects) + 1} /Root 1 0 R >>\nstartxref\n{xref}\n%%EOF\n".encode()
    with open(path_of(rel), "wb") as f:
        f.write(out)


def docx(rel, paragraphs):
    """A Word document: one paragraph per item; "# " makes a heading (bold, larger)."""
    paras = []
    for p in paragraphs:
        if p.startswith("# "):
            paras.append('<w:p><w:r><w:rPr><w:b/><w:sz w:val="32"/></w:rPr><w:t xml:space="preserve">'
                         + escape(p[2:]) + "</w:t></w:r></w:p>")
        else:
            paras.append('<w:p><w:r><w:t xml:space="preserve">' + escape(p) + "</w:t></w:r></w:p>")
    document = ('<?xml version="1.0" encoding="UTF-8" standalone="yes"?>'
                '<w:document xmlns:w="http://schemas.openxmlformats.org/wordprocessingml/2006/main"><w:body>'
                + "".join(paras) + "</w:body></w:document>")
    with zipfile.ZipFile(path_of(rel), "w", zipfile.ZIP_DEFLATED) as z:
        z.writestr("[Content_Types].xml",
                   '<?xml version="1.0" encoding="UTF-8"?><Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types">'
                   '<Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/>'
                   '<Default Extension="xml" ContentType="application/xml"/>'
                   '<Override PartName="/word/document.xml" ContentType="application/vnd.openxmlformats-officedocument.wordprocessingml.document.main+xml"/>'
                   "</Types>")
        z.writestr("_rels/.rels",
                   '<?xml version="1.0" encoding="UTF-8"?><Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">'
                   '<Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument" Target="word/document.xml"/>'
                   "</Relationships>")
        z.writestr("word/document.xml", document)


def xlsx(rel, sheet, rows):
    """An Excel workbook with one sheet: numbers as numbers, the rest as shared strings."""
    strings, index = [], {}

    def col(i):
        s = ""
        i += 1
        while i:
            i, r = divmod(i - 1, 26)
            s = chr(65 + r) + s
        return s

    cells = []
    for r, row in enumerate(rows, 1):
        cs = []
        for c, v in enumerate(row):
            ref = f"{col(c)}{r}"
            if isinstance(v, (int, float)):
                cs.append(f'<c r="{ref}"><v>{v}</v></c>')
            else:
                if v not in index:
                    index[v] = len(strings)
                    strings.append(v)
                cs.append(f'<c r="{ref}" t="s"><v>{index[v]}</v></c>')
        cells.append(f'<row r="{r}">' + "".join(cs) + "</row>")
    ns = 'xmlns="http://schemas.openxmlformats.org/spreadsheetml/2006/main"'
    rns = 'xmlns:r="http://schemas.openxmlformats.org/officeDocument/2006/relationships"'
    with zipfile.ZipFile(path_of(rel), "w", zipfile.ZIP_DEFLATED) as z:
        z.writestr("[Content_Types].xml",
                   '<?xml version="1.0" encoding="UTF-8"?><Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types">'
                   '<Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/>'
                   '<Default Extension="xml" ContentType="application/xml"/>'
                   '<Override PartName="/xl/workbook.xml" ContentType="application/vnd.openxmlformats-officedocument.spreadsheetml.sheet.main+xml"/>'
                   '<Override PartName="/xl/worksheets/sheet1.xml" ContentType="application/vnd.openxmlformats-officedocument.spreadsheetml.worksheet+xml"/>'
                   '<Override PartName="/xl/sharedStrings.xml" ContentType="application/vnd.openxmlformats-officedocument.spreadsheetml.sharedStrings+xml"/>'
                   "</Types>")
        z.writestr("_rels/.rels",
                   '<?xml version="1.0" encoding="UTF-8"?><Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">'
                   '<Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument" Target="xl/workbook.xml"/>'
                   "</Relationships>")
        z.writestr("xl/workbook.xml", f'<?xml version="1.0" encoding="UTF-8"?><workbook {ns} {rns}><sheets>'
                   f'<sheet name="{escape(sheet)}" sheetId="1" r:id="rId1"/></sheets></workbook>')
        z.writestr("xl/_rels/workbook.xml.rels",
                   '<?xml version="1.0" encoding="UTF-8"?><Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">'
                   '<Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/worksheet" Target="worksheets/sheet1.xml"/>'
                   '<Relationship Id="rId2" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/sharedStrings" Target="sharedStrings.xml"/>'
                   "</Relationships>")
        z.writestr("xl/worksheets/sheet1.xml", f'<?xml version="1.0" encoding="UTF-8"?><worksheet {ns}><sheetData>'
                   + "".join(cells) + "</sheetData></worksheet>")
        z.writestr("xl/sharedStrings.xml", f'<?xml version="1.0" encoding="UTF-8"?><sst {ns} count="{len(strings)}" '
                   f'uniqueCount="{len(strings)}">' + "".join(f"<si><t>{escape(s)}</t></si>" for s in strings) + "</sst>")


def eml(rel, sender, to, subject, date, body):
    write(rel, f"From: {sender}\nTo: {to}\nSubject: {subject}\nDate: {date}\n"
               "MIME-Version: 1.0\nContent-Type: text/plain; charset=utf-8\n\n" + body.strip() + "\n")

