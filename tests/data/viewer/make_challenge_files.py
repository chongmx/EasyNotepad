#!/usr/bin/env python3
"""Writes the viewer's challenge documents: one Word, Excel, PowerPoint and PDF file each, built to
exercise what their readers find hard (style inheritance, numbering, merged and nested tables,
number formats, master and layout inheritance, group transforms, compressed object streams,
incremental updates, ...). Python's standard library only; run it to rebuild the files:

    python3 tests/data/viewer/make_challenge_files.py

tests/ViewerTests.inc (viewer_reads_the_challenge_files) checks what each must give.
"""
import os
import struct
import zipfile
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
FIXED_DATE = (2026, 1, 1, 0, 0, 0)  # every entry the same timestamp: the files rebuild byte for byte


def write_zip(path, parts):
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as z:
        for name, data in parts:
            info = zipfile.ZipInfo(name, FIXED_DATE)
            info.compress_type = zipfile.ZIP_DEFLATED
            z.writestr(info, data.encode("utf-8") if isinstance(data, str) else data)


def png(width, height, pixel):
    """A PNG of pixel(x, y) -> (r, g, b)."""
    raw = b"".join(b"\x00" + b"".join(bytes(pixel(x, y)) for x in range(width)) for y in range(height))

    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)

    return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)) + \
        chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b"")


GRADIENT = png(64, 32, lambda x, y: (x * 4, y * 8, 160))
CHECKER = png(16, 16, lambda x, y: (230, 60, 40) if (x // 4 + y // 4) % 2 else (250, 250, 250))

CT_HEAD = '<?xml version="1.0" encoding="UTF-8" standalone="yes"?>\n'
PKG_RELS = 'http://schemas.openxmlformats.org/package/2006/relationships'
DOC_REL = 'http://schemas.openxmlformats.org/officeDocument/2006/relationships'


def rels(items):
    """items: (id, type, target[, external])"""
    out = [CT_HEAD, f'<Relationships xmlns="{PKG_RELS}">']
    for item in items:
        rid, kind, target = item[:3]
        external = len(item) > 3 and item[3]
        prefix = kind if kind.startswith("http") else f"{DOC_REL}/{kind}"
        out.append(f'<Relationship Id="{rid}" Type="{prefix}" Target="{target}"' + (' TargetMode="External"' if external else '') + '/>')
    out.append("</Relationships>")
    return "".join(out)


def content_types(overrides, defaults=(("rels", "application/vnd.openxmlformats-package.relationships+xml"), ("xml", "application/xml"),
                                       ("png", "image/png"))):
    out = [CT_HEAD, '<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types">']
    out += [f'<Default Extension="{e}" ContentType="{t}"/>' for e, t in defaults]
    out += [f'<Override PartName="/{p}" ContentType="{t}"/>' for p, t in overrides]
    out.append("</Types>")
    return "".join(out)


THEME = CT_HEAD + '''<a:theme xmlns:a="http://schemas.openxmlformats.org/drawingml/2006/main" name="Challenge"><a:themeElements>
<a:clrScheme name="Challenge"><a:dk1><a:sysClr val="windowText" lastClr="000000"/></a:dk1><a:lt1><a:sysClr val="window" lastClr="FFFFFF"/></a:lt1>
<a:dk2><a:srgbClr val="1F3864"/></a:dk2><a:lt2><a:srgbClr val="EEECE1"/></a:lt2><a:accent1><a:srgbClr val="2E75B6"/></a:accent1>
<a:accent2><a:srgbClr val="C55A11"/></a:accent2><a:accent3><a:srgbClr val="7F7F7F"/></a:accent3><a:accent4><a:srgbClr val="BF9000"/></a:accent4>
<a:accent5><a:srgbClr val="2F5597"/></a:accent5><a:accent6><a:srgbClr val="548235"/></a:accent6><a:hlink><a:srgbClr val="0563C1"/></a:hlink>
<a:folHlink><a:srgbClr val="954F72"/></a:folHlink></a:clrScheme>
<a:fontScheme name="Challenge"><a:majorFont><a:latin typeface="Georgia"/></a:majorFont><a:minorFont><a:latin typeface="Calibri"/></a:minorFont></a:fontScheme>
<a:fmtScheme name="Challenge"><a:fillStyleLst><a:solidFill><a:schemeClr val="phClr"/></a:solidFill></a:fillStyleLst></a:fmtScheme>
</a:themeElements></a:theme>'''


def chart(kind, series, categories, title, extra=""):
    """A chart part: kind is a c:*Chart element name; series: (name, [values]). Cached values only."""
    def ser(i, name, values):
        pts = "".join(f'<c:pt idx="{j}"><c:v>{v}</c:v></c:pt>' for j, v in enumerate(values) if v is not None)
        cats = "".join(f'<c:pt idx="{j}"><c:v>{c}</c:v></c:pt>' for j, c in enumerate(categories))
        return (f'<c:ser><c:idx val="{i}"/><c:order val="{i}"/><c:tx><c:strRef><c:f>Sheet1!$B${i + 1}</c:f><c:strCache><c:ptCount val="1"/>'
                f'<c:pt idx="0"><c:v>{name}</c:v></c:pt></c:strCache></c:strRef></c:tx>'
                f'<c:cat><c:strRef><c:f>Sheet1!$A$2:$A${len(categories) + 1}</c:f><c:strCache><c:ptCount val="{len(categories)}"/>{cats}</c:strCache></c:strRef></c:cat>'
                f'<c:val><c:numRef><c:f>Sheet1!$B$2</c:f><c:numCache><c:formatCode>General</c:formatCode><c:ptCount val="{len(values)}"/>{pts}</c:numCache></c:numRef></c:val></c:ser>')
    body = "".join(ser(i, n, v) for i, (n, v) in enumerate(series))
    return (CT_HEAD + '<c:chartSpace xmlns:c="http://schemas.openxmlformats.org/drawingml/2006/chart" '
            'xmlns:a="http://schemas.openxmlformats.org/drawingml/2006/main" xmlns:r="http://schemas.openxmlformats.org/officeDocument/2006/relationships">'
            f'<c:chart><c:title><c:tx><c:rich><a:bodyPr/><a:p><a:r><a:t>{title}</a:t></a:r></a:p></c:rich></c:tx><c:overlay val="0"/></c:title>'
            f'<c:autoTitleDeleted val="0"/><c:plotArea><c:layout/><c:{kind}>{extra}<c:varyColors val="0"/>{body}'
            f'<c:axId val="1"/><c:axId val="2"/></c:{kind}><c:catAx><c:axId val="1"/><c:delete val="0"/><c:crossAx val="2"/></c:catAx>'
            '<c:valAx><c:axId val="2"/><c:delete val="0"/><c:majorGridlines/><c:numFmt formatCode="#,##0" sourceLinked="0"/><c:crossAx val="1"/></c:valAx>'
            '</c:plotArea><c:legend><c:legendPos val="r"/><c:overlay val="0"/></c:legend><c:plotVisOnly val="1"/></c:chart></c:chartSpace>')


# ---- Word ---------------------------------------------------------------------------------------------

W = 'xmlns:w="http://schemas.openxmlformats.org/wordprocessingml/2006/main" xmlns:r="http://schemas.openxmlformats.org/officeDocument/2006/relationships" ' \
    'xmlns:wp="http://schemas.openxmlformats.org/drawingml/2006/wordprocessingDrawing" xmlns:a="http://schemas.openxmlformats.org/drawingml/2006/main" ' \
    'xmlns:pic="http://schemas.openxmlformats.org/drawingml/2006/picture" xmlns:c="http://schemas.openxmlformats.org/drawingml/2006/chart" ' \
    'xmlns:wps="http://schemas.microsoft.com/office/word/2010/wordprocessingShape" xmlns:mc="http://schemas.openxmlformats.org/markup-compatibility/2006" ' \
    'xmlns:v="urn:schemas-microsoft-com:vml" xmlns:w14="http://schemas.microsoft.com/office/word/2010/wordml" mc:Ignorable="w14"'


def run(text, props=""):
    space = ' xml:space="preserve"' if text != text.strip() else ""
    return f'<w:r>{"<w:rPr>" + props + "</w:rPr>" if props else ""}<w:t{space}>{text}</w:t></w:r>'


def para(content, ppr=""):
    return f'<w:p>{"<w:pPr>" + ppr + "</w:pPr>" if ppr else ""}{content}</w:p>'


def item(text, num, level):
    return para(run(text), f'<w:numPr><w:ilvl w:val="{level}"/><w:numId w:val="{num}"/></w:numPr>')


def make_docx(path):
    styles = CT_HEAD + f'''<w:styles {W}>
<w:docDefaults><w:rPrDefault><w:rPr><w:rFonts w:ascii="Calibri"/><w:sz w:val="24"/></w:rPr></w:rPrDefault>
<w:pPrDefault><w:pPr><w:spacing w:after="160"/></w:pPr></w:pPrDefault></w:docDefaults>
<w:style w:type="paragraph" w:default="1" w:styleId="Normal"><w:name w:val="Normal"/></w:style>
<w:style w:type="paragraph" w:styleId="Title"><w:name w:val="Title"/><w:basedOn w:val="Normal"/><w:pPr><w:jc w:val="center"/></w:pPr>
<w:rPr><w:sz w:val="56"/><w:color w:val="1F3864"/></w:rPr></w:style>
<w:style w:type="paragraph" w:styleId="Heading1"><w:name w:val="heading 1"/><w:basedOn w:val="Normal"/><w:pPr><w:outlineLvl w:val="0"/><w:spacing w:before="360"/></w:pPr>
<w:rPr><w:b/><w:sz w:val="36"/><w:color w:val="2E75B6"/></w:rPr></w:style>
<w:style w:type="paragraph" w:styleId="MyHeading"><w:name w:val="My chapter heading"/><w:basedOn w:val="Heading1"/><w:rPr><w:color w:val="C55A11"/></w:rPr></w:style>
<w:style w:type="paragraph" w:styleId="Quote"><w:name w:val="Quote"/><w:basedOn w:val="Normal"/><w:pPr><w:ind w:left="864" w:right="864"/></w:pPr><w:rPr><w:i/></w:rPr></w:style>
<w:style w:type="paragraph" w:styleId="NumberedPara"><w:name w:val="Numbered para"/><w:basedOn w:val="Normal"/><w:pPr><w:numPr><w:numId w:val="3"/></w:numPr></w:pPr></w:style>
<w:style w:type="character" w:styleId="Strong"><w:name w:val="Strong"/><w:rPr><w:b/></w:rPr></w:style>
<w:style w:type="character" w:styleId="CodeChar"><w:name w:val="Code char"/><w:rPr><w:rFonts w:ascii="Consolas"/></w:rPr></w:style>
<w:style w:type="table" w:styleId="Grid"><w:name w:val="Table Grid"/><w:tblPr><w:tblBorders><w:top w:val="single"/></w:tblBorders></w:tblPr>
<w:pPr><w:jc w:val="right"/></w:pPr></w:style>
</w:styles>'''
    numbering = CT_HEAD + f'''<w:numbering {W}>
<w:abstractNum w:abstractNumId="0"><w:lvl w:ilvl="0"><w:start w:val="1"/><w:numFmt w:val="decimal"/><w:lvlText w:val="%1."/></w:lvl>
<w:lvl w:ilvl="1"><w:start w:val="1"/><w:numFmt w:val="lowerLetter"/><w:lvlText w:val="(%2)"/></w:lvl>
<w:lvl w:ilvl="2"><w:start w:val="1"/><w:numFmt w:val="lowerRoman"/><w:lvlText w:val="%1.%2.%3"/></w:lvl></w:abstractNum>
<w:abstractNum w:abstractNumId="1"><w:lvl w:ilvl="0"><w:numFmt w:val="bullet"/><w:lvlText w:val="•"/></w:lvl>
<w:lvl w:ilvl="1"><w:numFmt w:val="bullet"/><w:lvlText w:val="o"/></w:lvl></w:abstractNum>
<w:abstractNum w:abstractNumId="2"><w:lvl w:ilvl="0"><w:start w:val="4"/><w:numFmt w:val="upperRoman"/><w:lvlText w:val="%1)"/></w:lvl></w:abstractNum>
<w:num w:numId="1"><w:abstractNumId w:val="0"/></w:num>
<w:num w:numId="2"><w:abstractNumId w:val="1"/></w:num>
<w:num w:numId="3"><w:abstractNumId w:val="2"/></w:num>
<w:num w:numId="4"><w:abstractNumId w:val="0"/><w:lvlOverride w:ilvl="0"><w:startOverride w:val="7"/></w:lvlOverride></w:num>
</w:numbering>'''
    footnotes = CT_HEAD + f'''<w:footnotes {W}><w:footnote w:type="separator" w:id="-1"><w:p><w:r><w:separator/></w:r></w:p></w:footnote>
<w:footnote w:type="continuationSeparator" w:id="0"><w:p><w:r><w:continuationSeparator/></w:r></w:p></w:footnote>
<w:footnote w:id="1"><w:p><w:r><w:t>First footnote, with </w:t></w:r><w:r><w:rPr><w:b/></w:rPr><w:t>bold</w:t></w:r><w:r><w:t> text.</w:t></w:r></w:p></w:footnote>
<w:footnote w:id="2"><w:p><w:r><w:t>Second footnote.</w:t></w:r></w:p><w:p><w:r><w:t>Its second paragraph.</w:t></w:r></w:p></w:footnote></w:footnotes>'''
    endnotes = CT_HEAD + f'''<w:endnotes {W}><w:endnote w:id="5"><w:p><w:r><w:t>The only endnote.</w:t></w:r></w:p></w:endnote></w:endnotes>'''
    comments = CT_HEAD + f'''<w:comments {W}><w:comment w:id="7" w:author="Reviewer One" w:initials="R1"><w:p><w:r><w:t>Is this number right?</w:t></w:r></w:p></w:comment>
<w:comment w:id="8" w:author="Reviewer Two"><w:p><w:r><w:t>Yes: checked twice.</w:t></w:r></w:p></w:comment></w:comments>'''
    header = CT_HEAD + f'<w:hdr {W}>{para(run("Challenge report — header"), "<w:jc w:val=\"right\"/>")}</w:hdr>'
    footer = CT_HEAD + f'<w:ftr {W}>{para(run("Page ") + "<w:r><w:fldChar w:fldCharType=\"begin\"/></w:r><w:r><w:instrText> PAGE </w:instrText></w:r><w:r><w:fldChar w:fldCharType=\"separate\"/></w:r>" + run("1") + "<w:r><w:fldChar w:fldCharType=\"end\"/></w:r>", "<w:jc w:val=\"center\"/>")}</w:ftr>'

    inline_picture = ('<w:r><w:drawing><wp:inline distT="0" distB="0" distL="0" distR="0"><wp:extent cx="1828800" cy="914400"/>'
                      '<wp:docPr id="10" name="Gradient" descr="A blue gradient"/><a:graphic><a:graphicData uri="http://schemas.openxmlformats.org/drawingml/2006/picture">'
                      '<pic:pic><pic:nvPicPr><pic:cNvPr id="10" name="g.png"/><pic:cNvPicPr/></pic:nvPicPr><pic:blipFill><a:blip r:embed="rIdImg"/><a:stretch><a:fillRect/></a:stretch></pic:blipFill>'
                      '<pic:spPr><a:xfrm><a:off x="0" y="0"/><a:ext cx="1828800" cy="914400"/></a:xfrm><a:prstGeom prst="rect"/></pic:spPr></pic:pic></a:graphicData></a:graphic></wp:inline></w:drawing></w:r>')
    chart_run = ('<w:r><w:drawing><wp:inline><wp:extent cx="5486400" cy="3200400"/><wp:docPr id="11" name="Chart 1"/><a:graphic>'
                 '<a:graphicData uri="http://schemas.openxmlformats.org/drawingml/2006/chart"><c:chart r:id="rIdChart"/></a:graphicData></a:graphic></wp:inline></w:drawing></w:r>')
    # A text box (wps), with a VML fallback that must not be read twice.
    text_box = ('<w:r><mc:AlternateContent><mc:Choice Requires="wps"><w:drawing><wp:anchor behindDoc="0" locked="0" layoutInCell="1" allowOverlap="1">'
                '<wp:extent cx="2743200" cy="914400"/><wp:docPr id="12" name="Text Box 1"/><a:graphic><a:graphicData uri="http://schemas.microsoft.com/office/word/2010/wordprocessingShape">'
                '<wps:wsp><wps:spPr><a:prstGeom prst="rect"/><a:solidFill><a:srgbClr val="FFF2CC"/></a:solidFill></wps:spPr><wps:txbx><w:txbxContent>'
                + para(run("Boxed: ") + run("important", "<w:b/><w:color w:val=\"C00000\"/>"))
                + '<w:tbl><w:tr><w:tc><w:p><w:r><w:t>box cell</w:t></w:r></w:p></w:tc></w:tr></w:tbl>'
                '</w:txbxContent></wps:txbx><wps:bodyPr/></wps:wsp></a:graphicData></a:graphic></wp:anchor></w:drawing></mc:Choice>'
                '<mc:Fallback><w:pict><v:shape><v:textbox><w:txbxContent>' + para(run("FALLBACK COPY: must not show")) + '</w:txbxContent></v:textbox></v:shape></w:pict></mc:Fallback>'
                '</mc:AlternateContent></w:r>')

    table = ('<w:tbl><w:tblPr><w:tblStyle w:val="Grid"/><w:tblW w:w="0" w:type="auto"/></w:tblPr><w:tblGrid><w:gridCol w:w="2000"/><w:gridCol w:w="2000"/><w:gridCol w:w="2000"/></w:tblGrid>'
             '<w:tr><w:tc>' + para(run("Region", "<w:b/>")) + '</w:tc><w:tc>' + para(run("Q1", "<w:b/>")) + '</w:tc><w:tc>' + para(run("Q2", "<w:b/>")) + '</w:tc></w:tr>'
             '<w:tr><w:tc><w:tcPr><w:vMerge w:val="restart"/></w:tcPr>' + para(run("North (two rows)")) + '</w:tc>'
             '<w:tc><w:tcPr><w:shd w:val="clear" w:fill="E2EFDA"/></w:tcPr>' + para(run("1,200")) + '</w:tc><w:tc>' + para(run("1,450")) + '</w:tc></w:tr>'
             '<w:tr><w:tc><w:tcPr><w:vMerge/></w:tcPr><w:p/></w:tc><w:tc><w:tcPr><w:gridSpan w:val="2"/></w:tcPr>' + para(run("spans two columns")) + '</w:tc></w:tr>'
             '<w:tr><w:tc>' + para(run("Nested")) + '</w:tc><w:tc><w:tbl><w:tr><w:tc>' + para(run("inner A")) + '</w:tc><w:tc>' + para(run("inner B")) + '</w:tc></w:tr></w:tbl><w:p/></w:tc>'
             '<w:tc>' + para(item("cell list item", 2, 0)[5:-6] if False else run("plain")) + '</w:tc></w:tr></w:tbl>')

    body = "".join([
        para(run("Challenge Report"), '<w:pStyle w:val="Title"/>'),
        para(run("Centred, "), '<w:jc w:val="center"/>')[:-6] + run("coloured", '<w:color w:val="2E75B6"/>') + run(", large", '<w:sz w:val="40"/>') + run(" and small.", '<w:sz w:val="16"/>') + '</w:p>',
        para(run("Right-aligned text."), '<w:jc w:val="right"/>'),
        para(run("Highlighted", '<w:highlight w:val="yellow"/>') + run(", underlined", '<w:u w:val="single"/>') + run(", struck", '<w:strike/>') +
             run(", shaded", '<w:shd w:val="clear" w:fill="DDEBF7"/>') + run(" and ") + '<w:r><w:rPr><w:rStyle w:val="Strong"/></w:rPr><w:t>strong</w:t></w:r>' +
             run(" with ") + '<w:r><w:rPr><w:rStyle w:val="CodeChar"/></w:rPr><w:t>code()</w:t></w:r>' + run(".")),
        para(run("A paragraph indented 0.5 inch with a 0.25 inch first line, long enough to wrap onto a second line so the indents can be seen."),
             '<w:ind w:left="720" w:firstLine="360"/>'),
        para(run("A hanging indent: the first line starts left of the rest, as in a bibliography entry that runs on."), '<w:ind w:left="720" w:hanging="720"/>'),
        para(run("A shaded paragraph."), '<w:shd w:val="clear" w:fill="F2F2F2"/>'),
        para(run("Chapter One"), '<w:pStyle w:val="MyHeading"/>'),
        item("first", 1, 0), item("second", 1, 0), item("second, a", 1, 1), item("second, b", 1, 1), item("second, b, i", 1, 2), item("third", 1, 0),
        item("dot", 2, 0), item("circle", 2, 1),
        item("restarted at seven", 4, 0), item("then eight", 4, 0),
        para(run("Style-numbered IV")), para(run("Style-numbered V")),
        para(run("A quotation, indented from both sides and in italics by its style."), '<w:pStyle w:val="Quote"/>'),
        para(run("Footnotes") + '<w:r><w:rPr><w:vertAlign w:val="superscript"/></w:rPr><w:footnoteReference w:id="1"/></w:r>' + run(" twice") +
             '<w:r><w:footnoteReference w:id="2"/></w:r>' + run(" and an endnote") + '<w:r><w:endnoteReference w:id="5"/></w:r>' + run(".")),
        para('<w:commentRangeStart w:id="7"/>' + run("Revenue was 1,234.") + '<w:commentRangeEnd w:id="7"/><w:r><w:commentReference w:id="7"/></w:r>' +
             run(" Reply") + '<w:r><w:commentReference w:id="8"/></w:r>'),
        para('<w:hyperlink r:id="rIdLink"><w:r><w:rPr><w:rStyle w:val="Hyperlink"/></w:rPr><w:t>an external link</w:t></w:r></w:hyperlink>' + run(", ") +
             '<w:hyperlink w:anchor="_Toc1"><w:r><w:t>an internal one</w:t></w:r></w:hyperlink>'),
        para(run("Tracked: ") + '<w:ins w:id="20" w:author="A">' + run("inserted") + '</w:ins>' + '<w:del w:id="21" w:author="A"><w:r><w:delText>DELETED</w:delText></w:r></w:del>' + run(" end.")),
        para(run("Field result: ") + '<w:r><w:fldChar w:fldCharType="begin"/></w:r><w:r><w:instrText xml:space="preserve"> DATE \\@ "d MMMM yyyy" </w:instrText></w:r>'
             '<w:r><w:fldChar w:fldCharType="separate"/></w:r>' + run("1 January 2026") + '<w:r><w:fldChar w:fldCharType="end"/></w:r>'),
        '<w:sdt><w:sdtPr><w:alias w:val="Client"/></w:sdtPr><w:sdtContent>' + para(run("Inside a content control.")) + '</w:sdtContent></w:sdt>',
        para(run("Tab") + '<w:r><w:tab/></w:r>' + run("stop, line") + '<w:r><w:br/></w:r>' + run("break, symbol ") +
             '<w:r><w:sym w:font="Wingdings" w:char="F0FC"/></w:r>' + run(", no-break") + '<w:r><w:noBreakHyphen/></w:r>' + run("hyphen.")),
        # Tab stops as a table of contents sets them: a right stop with a dot leader at the margin,
        # a centre stop, and a decimal stop lining up the amounts.
        para(run("Chapter One") + '<w:r><w:tab/></w:r>' + run("7"), '<w:tabs><w:tab w:val="right" w:leader="dot" w:pos="9000"/></w:tabs>'),
        para(run("Left") + '<w:r><w:tab/></w:r>' + run("centred") + '<w:r><w:tab/></w:r>' + run("12.50"),
             '<w:tabs><w:tab w:val="center" w:pos="4500"/><w:tab w:val="decimal" w:pos="8000"/></w:tabs>'),
        para(run("Unicode: 中文, العربية, é vs é, \U0001F600, ∑ x².")),
        para(inline_picture + run(" inline after a picture.")),
        table,
        para(text_box + run("Text beside the text box.")),
        para(chart_run),
        '<w:sectPr><w:headerReference w:type="default" r:id="rIdHeader"/><w:footerReference w:type="default" r:id="rIdFooter"/>'
        '<w:pgSz w:w="12240" w:h="15840"/><w:pgMar w:top="1440" w:right="1440" w:bottom="1440" w:left="1440"/></w:sectPr>',
    ])
    document = CT_HEAD + f'<w:document {W}><w:body>{body}</w:body></w:document>'
    chart_part = chart("barChart", [("Plan", [120, 140, 90, 160]), ("Actual", [110, 150, 95, 170])], ["Q1", "Q2", "Q3", "Q4"], "Plan vs actual",
                       '<c:barDir val="col"/><c:grouping val="clustered"/><c:gapWidth val="80"/>')
    write_zip(os.path.join(HERE, "challenge.docx"), [
        ("[Content_Types].xml", content_types([("word/document.xml", "application/vnd.openxmlformats-officedocument.wordprocessingml.document.main+xml")])),
        ("_rels/.rels", rels([("rId1", "officeDocument", "word/document.xml")])),
        ("word/_rels/document.xml.rels", rels([
            ("rIdStyles", "styles", "styles.xml"), ("rIdNum", "numbering", "numbering.xml"), ("rIdFn", "footnotes", "footnotes.xml"),
            ("rIdEn", "endnotes", "endnotes.xml"), ("rIdCom", "comments", "comments.xml"), ("rIdTheme", "theme", "theme/theme1.xml"),
            ("rIdImg", "image", "media/gradient.png"), ("rIdChart", "chart", "charts/chart1.xml"),
            ("rIdHeader", "header", "header1.xml"), ("rIdFooter", "footer", "footer1.xml"),
            ("rIdLink", "hyperlink", "https://www.example.com/report?q=1&amp;r=2", True)])),
        ("word/document.xml", document), ("word/styles.xml", styles), ("word/numbering.xml", numbering),
        ("word/footnotes.xml", footnotes), ("word/endnotes.xml", endnotes), ("word/comments.xml", comments),
        ("word/header1.xml", header), ("word/footer1.xml", footer), ("word/theme/theme1.xml", THEME),
        ("word/media/gradient.png", GRADIENT), ("word/charts/chart1.xml", chart_part),
    ])


# ---- Excel --------------------------------------------------------------------------------------------

X = 'xmlns="http://schemas.openxmlformats.org/spreadsheetml/2006/main" xmlns:r="http://schemas.openxmlformats.org/officeDocument/2006/relationships"'


def make_xlsx(path):
    shared = ["Item", "Value", "Format", "Rich ", "text", "東京", "Total"]
    sst = CT_HEAD + f'<sst {X} count="9" uniqueCount="7">' + "".join(
        f"<si><t>{s}</t></si>" if i not in (3,) else
        '<si><r><t xml:space="preserve">Rich </t></r><r><rPr><b/><color rgb="FFC00000"/></rPr><t>text</t></r><rPh sb="0" eb="1"><t>IGNORED</t></rPh></si>'
        for i, s in enumerate(shared)) + "</sst>"
    styles = CT_HEAD + f'''<styleSheet {X}>
<numFmts count="6"><numFmt numFmtId="164" formatCode="yyyy\\-mm\\-dd"/><numFmt numFmtId="165" formatCode="[h]:mm:ss"/>
<numFmt numFmtId="166" formatCode="#,##0.00;[Red]\\(#,##0.00\\);\\-"/><numFmt numFmtId="167" formatCode="0.0%"/>
<numFmt numFmtId="168" formatCode="[$€-407] #,##0.00"/><numFmt numFmtId="169" formatCode="&quot;Qty: &quot;0"/></numFmts>
<fonts count="4"><font><sz val="11"/><name val="Calibri"/></font><font><b/><sz val="14"/><color theme="4"/><name val="Calibri"/></font>
<font><i/><u/><sz val="11"/><color rgb="FF7030A0"/><name val="Calibri"/></font><font><sz val="9"/><color theme="1" tint="0.5"/><name val="Calibri"/></font></fonts>
<fills count="4"><fill><patternFill patternType="none"/></fill><fill><patternFill patternType="gray125"/></fill>
<fill><patternFill patternType="solid"><fgColor rgb="FFFFF2CC"/><bgColor indexed="64"/></patternFill></fill>
<fill><patternFill patternType="solid"><fgColor theme="4" tint="0.7999816888943144"/></patternFill></fill></fills>
<borders count="3"><border/><border><left style="thin"><color auto="1"/></left><right style="thin"><color auto="1"/></right><top style="thin"><color auto="1"/></top><bottom style="medium"><color rgb="FF2E75B6"/></bottom></border>
<border><bottom style="double"><color auto="1"/></bottom></border></borders>
<cellStyleXfs count="1"><xf numFmtId="0" fontId="0" fillId="0" borderId="0"/></cellStyleXfs>
<cellXfs count="14">
<xf numFmtId="0" fontId="0" fillId="0" borderId="0" xfId="0"/>
<xf numFmtId="0" fontId="1" fillId="3" borderId="1" xfId="0" applyFont="1" applyFill="1" applyBorder="1"><alignment horizontal="center" vertical="center"/></xf>
<xf numFmtId="164" fontId="0" fillId="0" borderId="0" xfId="0" applyNumberFormat="1"/>
<xf numFmtId="165" fontId="0" fillId="0" borderId="0" xfId="0" applyNumberFormat="1"/>
<xf numFmtId="166" fontId="0" fillId="0" borderId="0" xfId="0" applyNumberFormat="1"/>
<xf numFmtId="167" fontId="0" fillId="0" borderId="0" xfId="0" applyNumberFormat="1"/>
<xf numFmtId="168" fontId="0" fillId="0" borderId="0" xfId="0" applyNumberFormat="1"/>
<xf numFmtId="11" fontId="0" fillId="0" borderId="0" xfId="0" applyNumberFormat="1"/>
<xf numFmtId="13" fontId="0" fillId="0" borderId="0" xfId="0" applyNumberFormat="1"/>
<xf numFmtId="49" fontId="0" fillId="0" borderId="0" xfId="0" applyNumberFormat="1"/>
<xf numFmtId="169" fontId="2" fillId="2" borderId="0" xfId="0" applyNumberFormat="1" applyFont="1" applyFill="1"><alignment horizontal="right" wrapText="1"/></xf>
<xf numFmtId="0" fontId="3" fillId="0" borderId="2" xfId="0" applyFont="1" applyBorder="1"><alignment textRotation="90"/></xf>
<xf numFmtId="18" fontId="0" fillId="0" borderId="0" xfId="0" applyNumberFormat="1"/>
<xf numFmtId="4" fontId="1" fillId="0" borderId="2" xfId="0" applyNumberFormat="1" applyFont="1" applyBorder="1"/>
</cellXfs></styleSheet>'''

    def c(ref, value, t=None, s=None, f=None):
        attrs = f' r="{ref}"' + (f' t="{t}"' if t else "") + (f' s="{s}"' if s is not None else "")
        inner = (f"<f>{f}</f>" if f else "") + (f"<is><t>{value}</t></is>" if t == "inlineStr" else f"<v>{value}</v>" if value is not None else "")
        return f"<c{attrs}>{inner}</c>"

    rows = [
        (1, 'ht="24" customHeight="1"', [c("A1", 0, "s", 1), c("B1", 1, "s", 1), c("C1", 2, "s", 1)]),
        (2, "", [c("A2", "date (1900 system)", "inlineStr"), c("B2", 46023, s=2), c("C2", "yyyy-mm-dd", "inlineStr")]),
        (3, "", [c("A3", "Excel's 29 Feb 1900", "inlineStr"), c("B3", 60, s=2), c("C3", "the leap day that never was", "inlineStr")]),
        (4, "", [c("A4", "elapsed time", "inlineStr"), c("B4", 1.5208333333333333, s=3), c("C4", "[h]:mm:ss", "inlineStr")]),
        (5, "", [c("A5", "negative, red section", "inlineStr"), c("B5", -1234.5, s=4), c("C5", "#,##0.00;[Red](#,##0.00);-", "inlineStr")]),
        (6, "", [c("A6", "zero, its own section", "inlineStr"), c("B6", 0, s=4)]),
        (7, "", [c("A7", "percent", "inlineStr"), c("B7", 0.12345, s=5)]),
        (8, "", [c("A8", "euro, German locale tag", "inlineStr"), c("B8", 9876.5, s=6)]),
        (9, "", [c("A9", "scientific", "inlineStr"), c("B9", 0.000123456, s=7)]),
        (10, "", [c("A10", "fraction", "inlineStr"), c("B10", 3.25, s=8)]),
        (11, "", [c("A11", "text format", "inlineStr"), c("B11", "007", "str", 9)]),
        (12, "", [c("A12", 3, "s"), c("B12", 5, "s"), c("C12", 42, s=10)]),
        (13, "", [c("A13", "boolean", "inlineStr"), c("B13", 1, "b"), c("C13", "#DIV/0!", "e", f="1/0")]),
        (14, "", [c("A14", "time 1:30 PM", "inlineStr"), c("B14", 0.5625, s=12)]),
        (15, "", [c("A15", 6, "s", 13), c("B15", 12345.678, s=13, f="SUM(B2:B10)")]),
        (17, "", [c("A17", "merged across and down", "inlineStr", 1)]),
        (20, 'ht="57"', [c("A20", "rotated", "inlineStr", 11), c("B20", "Wrapped text, aligned right, over three lines", "inlineStr", 10)]),
        (25, 'hidden="1"', [c("A25", "HIDDEN ROW", "inlineStr")]),
    ]
    # Cells without r: the next columns of their row.
    implicit = '<row r="22"><c t="inlineStr"><is><t>implicit A22</t></is></c><c t="inlineStr"><is><t>implicit B22</t></is></c></row>'
    sheet_rows = "".join(f'<row r="{r}" {extra}>' + "".join(cells) + "</row>" for r, extra, cells in rows if r < 22) + implicit + \
        "".join(f'<row r="{r}" {extra}>' + "".join(cells) + "</row>" for r, extra, cells in rows if r > 22) + \
        '<row r="1048576"><c r="XFD1048576" t="inlineStr"><is><t>the last cell</t></is></c></row>'
    sheet1 = CT_HEAD + f'''<worksheet {X}><dimension ref="A1:XFD1048576"/>
<sheetViews><sheetView workbookViewId="0"><pane xSplit="1" ySplit="1" topLeftCell="B2" activePane="bottomRight" state="frozen"/></sheetView></sheetViews>
<sheetFormatPr defaultRowHeight="15"/><cols><col min="1" max="1" width="28" customWidth="1"/><col min="2" max="2" width="16" customWidth="1"/>
<col min="3" max="3" width="30" customWidth="1"/><col min="4" max="4" hidden="1" width="9"/></cols>
<sheetData>{sheet_rows}</sheetData><mergeCells count="1"><mergeCell ref="A17:C18"/></mergeCells>
<conditionalFormatting sqref="B2:B10"><cfRule type="cellIs" dxfId="0" priority="1" operator="lessThan"><formula>0</formula></cfRule></conditionalFormatting>
<drawing r:id="rId1"/></worksheet>'''
    data_rows = "".join(f'<row r="{i + 2}"><c r="A{i + 2}" t="inlineStr"><is><t>{m}</t></is></c><c r="B{i + 2}"><v>{v}</v></c><c r="C{i + 2}"><v>{w}</v></c></row>'
                        for i, (m, v, w) in enumerate([("Jan", 10, 4), ("Feb", 14, 9), ("Mar", 9, 12), ("Apr", 18, 7)]))
    sheet2 = CT_HEAD + f'<worksheet {X}><sheetViews><sheetView showGridLines="0" workbookViewId="0"/></sheetViews><sheetData><row r="1"><c r="A1" t="inlineStr"><is><t>Month</t></is></c></row>{data_rows}</sheetData></worksheet>'
    sheet3 = CT_HEAD + f'<worksheet {X}><sheetData><row r="1"><c r="A1" t="inlineStr"><is><t>secret</t></is></c></row></sheetData></worksheet>'
    workbook = CT_HEAD + f'''<workbook {X}><workbookPr date1904="0"/><bookViews><workbookView activeTab="0"/></bookViews><sheets>
<sheet name="Formats &amp; cells" sheetId="1" r:id="rId1"/><sheet name="Data" sheetId="2" r:id="rId2"/><sheet name="Hidden" sheetId="3" state="veryHidden" r:id="rId3"/>
</sheets><definedNames><definedName name="Total">'Formats &amp; cells'!$B$15</definedName></definedNames></workbook>'''
    drawing = CT_HEAD + '''<xdr:wsDr xmlns:xdr="http://schemas.openxmlformats.org/drawingml/2006/spreadsheetDrawing" xmlns:a="http://schemas.openxmlformats.org/drawingml/2006/main"
 xmlns:r="http://schemas.openxmlformats.org/officeDocument/2006/relationships" xmlns:c="http://schemas.openxmlformats.org/drawingml/2006/chart">
<xdr:twoCellAnchor><xdr:from><xdr:col>4</xdr:col><xdr:colOff>0</xdr:colOff><xdr:row>1</xdr:row><xdr:rowOff>0</xdr:rowOff></xdr:from>
<xdr:to><xdr:col>11</xdr:col><xdr:colOff>304800</xdr:colOff><xdr:row>16</xdr:row><xdr:rowOff>95250</xdr:rowOff></xdr:to>
<xdr:graphicFrame macro=""><xdr:nvGraphicFramePr><xdr:cNvPr id="2" name="Chart 1"/><xdr:cNvGraphicFramePr/></xdr:nvGraphicFramePr><xdr:xfrm><a:off x="0" y="0"/><a:ext cx="0" cy="0"/></xdr:xfrm>
<a:graphic><a:graphicData uri="http://schemas.openxmlformats.org/drawingml/2006/chart"><c:chart r:id="rId1"/></a:graphicData></a:graphic></xdr:graphicFrame><xdr:clientData/></xdr:twoCellAnchor>
<xdr:oneCellAnchor><xdr:from><xdr:col>4</xdr:col><xdr:colOff>0</xdr:colOff><xdr:row>18</xdr:row><xdr:rowOff>0</xdr:rowOff></xdr:from><xdr:ext cx="1219200" cy="609600"/>
<xdr:pic><xdr:nvPicPr><xdr:cNvPr id="3" name="Picture 2"/><xdr:cNvPicPr/></xdr:nvPicPr><xdr:blipFill><a:blip r:embed="rId2"/><a:stretch><a:fillRect/></a:stretch></xdr:blipFill>
<xdr:spPr><a:prstGeom prst="rect"/></xdr:spPr></xdr:pic><xdr:clientData/></xdr:oneCellAnchor>
<xdr:twoCellAnchor><xdr:from><xdr:col>7</xdr:col><xdr:colOff>0</xdr:colOff><xdr:row>18</xdr:row><xdr:rowOff>0</xdr:rowOff></xdr:from>
<xdr:to><xdr:col>10</xdr:col><xdr:colOff>0</xdr:colOff><xdr:row>22</xdr:row><xdr:rowOff>0</xdr:rowOff></xdr:to>
<xdr:sp macro="" textlink=""><xdr:nvSpPr><xdr:cNvPr id="4" name="Callout"/><xdr:cNvSpPr/></xdr:nvSpPr><xdr:spPr><a:xfrm rot="-600000"><a:off x="0" y="0"/><a:ext cx="0" cy="0"/></a:xfrm>
<a:prstGeom prst="wedgeRoundRectCallout"><a:avLst/></a:prstGeom><a:solidFill><a:schemeClr val="accent2"><a:lumMod val="40000"/><a:lumOff val="60000"/></a:schemeClr></a:solidFill>
<a:ln w="19050"><a:solidFill><a:schemeClr val="accent2"/></a:solidFill></a:ln></xdr:spPr>
<xdr:txBody><a:bodyPr anchor="ctr"/><a:p><a:pPr algn="ctr"/><a:r><a:rPr sz="1100" b="1"/><a:t>Q2 beat plan</a:t></a:r></a:p></xdr:txBody></xdr:sp><xdr:clientData/></xdr:twoCellAnchor>
</xdr:wsDr>'''
    chart_part = chart("lineChart", [("Sales", [10, 14, 9, 18]), ("Costs", [4, 9, 12, 7])], ["Jan", "Feb", "Mar", "Apr"], "Sales and costs",
                       '<c:grouping val="standard"/>')
    write_zip(os.path.join(HERE, "challenge.xlsx"), [
        ("[Content_Types].xml", content_types([("xl/workbook.xml", "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet.main+xml")])),
        ("_rels/.rels", rels([("rId1", "officeDocument", "xl/workbook.xml")])),
        ("xl/_rels/workbook.xml.rels", rels([("rId1", "worksheet", "worksheets/sheet1.xml"), ("rId2", "worksheet", "worksheets/sheet2.xml"),
                                             ("rId3", "worksheet", "worksheets/sheet3.xml"), ("rId4", "sharedStrings", "sharedStrings.xml"),
                                             ("rId5", "styles", "styles.xml"), ("rId6", "theme", "theme/theme1.xml")])),
        ("xl/workbook.xml", workbook), ("xl/sharedStrings.xml", sst), ("xl/styles.xml", styles), ("xl/theme/theme1.xml", THEME),
        ("xl/worksheets/sheet1.xml", sheet1), ("xl/worksheets/sheet2.xml", sheet2), ("xl/worksheets/sheet3.xml", sheet3),
        ("xl/worksheets/_rels/sheet1.xml.rels", rels([("rId1", "drawing", "../drawings/drawing1.xml")])),
        ("xl/drawings/drawing1.xml", drawing),
        ("xl/drawings/_rels/drawing1.xml.rels", rels([("rId1", "chart", "../charts/chart1.xml"), ("rId2", "image", "../media/checker.png")])),
        ("xl/charts/chart1.xml", chart_part), ("xl/media/checker.png", CHECKER),
    ])


# ---- PowerPoint ---------------------------------------------------------------------------------------

P = 'xmlns:a="http://schemas.openxmlformats.org/drawingml/2006/main" xmlns:r="http://schemas.openxmlformats.org/officeDocument/2006/relationships" ' \
    'xmlns:p="http://schemas.openxmlformats.org/presentationml/2006/main"'
EMU = 12700


def sp(id_, name, x, y, w, h, geom="rect", fill=None, text=None, extra_sppr="", body='<a:bodyPr/>', ph=None, xfrm_attrs=""):
    nv = f'<p:nvSpPr><p:cNvPr id="{id_}" name="{name}"/><p:cNvSpPr/><p:nvPr>{ph or ""}</p:nvPr></p:nvSpPr>'
    xfrm = f'<a:xfrm{xfrm_attrs}><a:off x="{x * EMU}" y="{y * EMU}"/><a:ext cx="{w * EMU}" cy="{h * EMU}"/></a:xfrm>' if w else ""
    fill_xml = f'<a:solidFill><a:srgbClr val="{fill}"/></a:solidFill>' if fill and not fill.startswith("<") else (fill or "")
    sppr = f'<p:spPr>{xfrm}{f"<a:prstGeom prst=\"{geom}\"><a:avLst/></a:prstGeom>" if geom else ""}{fill_xml}{extra_sppr}</p:spPr>'
    tx = f'<p:txBody>{body}<a:lstStyle/>{text}</p:txBody>' if text is not None else ""
    return f"<p:sp>{nv}{sppr}{tx}</p:sp>"


def tp(text, props="", ppr=""):
    return f'<a:p>{"<a:pPr " + ppr + "/>" if ppr else ""}<a:r><a:rPr lang="en-US" {props}/><a:t>{text}</a:t></a:r></a:p>'


def make_pptx(path):
    master = CT_HEAD + f'''<p:sldMaster {P}><p:cSld><p:bg><p:bgPr><a:gradFill><a:gsLst><a:gs pos="0"><a:srgbClr val="FFFFFF"/></a:gs><a:gs pos="100000"><a:srgbClr val="DDEBF7"/></a:gs></a:gsLst>
<a:lin ang="5400000"/></a:gradFill></p:bgPr></p:bg><p:spTree><p:nvGrpSpPr><p:cNvPr id="1" name=""/><p:cNvGrpSpPr/><p:nvPr/></p:nvGrpSpPr><p:grpSpPr/>
{sp(2, "Title", 36, 20, 648, 70, ph='<p:ph type="title"/>', text=tp("Master title prompt"))}
{sp(3, "Body", 36, 110, 648, 360, ph='<p:ph type="body" idx="1"/>', text=tp("Master body prompt"))}
{sp(4, "Band", 0, 510, 720, 30, fill="2E75B6")}
{sp(5, "Slide number", 620, 512, 80, 24, ph='<p:ph type="sldNum" sz="quarter" idx="12"/>', text='<a:p><a:fld id="{{1}}" type="slidenum"><a:rPr lang="en-US"/><a:t>&#8249;#&#8250;</a:t></a:fld></a:p>')}
</p:spTree></p:cSld><p:clrMap bg1="lt1" tx1="dk1" bg2="lt2" tx2="dk2" accent1="accent1" accent2="accent2" accent3="accent3" accent4="accent4" accent5="accent5" accent6="accent6" hlink="hlink" folHlink="folHlink"/>
<p:sldLayoutIdLst><p:sldLayoutId id="2147483649" r:id="rId1"/></p:sldLayoutIdLst>
<p:txStyles><p:titleStyle><a:lvl1pPr algn="l"><a:defRPr sz="3600" b="1"><a:solidFill><a:schemeClr val="tx2"/></a:solidFill></a:defRPr></a:lvl1pPr></p:titleStyle>
<p:bodyStyle><a:lvl1pPr marL="228600" indent="-228600"><a:buChar char="&#8226;"/><a:defRPr sz="2400"><a:solidFill><a:schemeClr val="tx1"/></a:solidFill></a:defRPr></a:lvl1pPr>
<a:lvl2pPr marL="685800" indent="-228600"><a:buChar char="&#8211;"/><a:defRPr sz="2000"/></a:lvl2pPr></p:bodyStyle><p:otherStyle><a:lvl1pPr><a:defRPr sz="1800"/></a:lvl1pPr></p:otherStyle></p:txStyles></p:sldMaster>'''
    layout = CT_HEAD + f'''<p:sldLayout {P} type="obj"><p:cSld name="Title and Content"><p:spTree><p:nvGrpSpPr><p:cNvPr id="1" name=""/><p:cNvGrpSpPr/><p:nvPr/></p:nvGrpSpPr><p:grpSpPr/>
{sp(2, "Title", 36, 30, 648, 60, ph='<p:ph type="title"/>', text=tp("Layout title prompt"))}
{sp(3, "Content", 36, 110, 648, 380, ph='<p:ph idx="1"/>', text=tp("Layout content prompt"))}
</p:spTree></p:cSld></p:sldLayout>'''
    # Slide 1: title and body placeholders filled; bullets on two levels; an autonumbered list; autofit.
    slide1 = CT_HEAD + f'''<p:sld {P}><p:cSld><p:spTree><p:nvGrpSpPr><p:cNvPr id="1" name=""/><p:cNvGrpSpPr/><p:nvPr/></p:nvGrpSpPr><p:grpSpPr/>
{sp(2, "Title 1", 0, 0, 0, 0, geom=None, ph='<p:ph type="title"/>', text=tp("Inheritance and text"))}
{sp(3, "Content 2", 0, 0, 0, 0, geom=None, ph='<p:ph idx="1"/>', body='<a:bodyPr><a:normAutofit fontScale="85000" lnSpcReduction="10000"/></a:bodyPr>',
    text=tp("Bullet from the master") + tp("Second level", ppr='lvl="1"') + tp("No bullet here", ppr='lvl="0"').replace('<a:pPr lvl="0"/>', '<a:pPr><a:buNone/></a:pPr>') +
    '<a:p><a:pPr><a:buFont typeface="+mj-lt"/><a:buAutoNum type="arabicPeriod"/></a:pPr><a:r><a:rPr lang="en-US"/><a:t>Numbered one</a:t></a:r></a:p>' +
    '<a:p><a:pPr><a:buAutoNum type="arabicPeriod"/></a:pPr><a:r><a:rPr lang="en-US"/><a:t>Numbered two</a:t></a:r></a:p>' +
    '<a:p><a:r><a:rPr lang="en-US" b="1"/><a:t>Bold, </a:t></a:r><a:r><a:rPr lang="en-US" i="1"><a:solidFill><a:schemeClr val="accent2"/></a:solidFill></a:rPr><a:t>accent italic, </a:t></a:r>'
    '<a:r><a:rPr lang="en-US" u="sng"><a:hlinkClick r:id="rIdLink"/></a:rPr><a:t>a link</a:t></a:r><a:br><a:rPr lang="en-US"/></a:br><a:r><a:rPr lang="en-US" sz="1200"/><a:t>after a line break, small</a:t></a:r></a:p>')}
</p:spTree></p:cSld><p:clrMapOvr><a:masterClrMapping/></p:clrMapOvr></p:sld>'''
    # Slide 2: shapes, rotation, flips, a group in a group (scaled), gradients, lines with arrows and dashes, custom geometry, a cropped picture.
    grad = '<a:gradFill rotWithShape="1"><a:gsLst><a:gs pos="0"><a:schemeClr val="accent1"><a:lumMod val="60000"/><a:lumOff val="40000"/></a:schemeClr></a:gs><a:gs pos="100000"><a:schemeClr val="accent1"/></a:gs></a:gsLst><a:lin ang="5400000" scaled="0"/></a:gradFill>'
    group = (f'<p:grpSp><p:nvGrpSpPr><p:cNvPr id="20" name="Outer group"/><p:cNvGrpSpPr/><p:nvPr/></p:nvGrpSpPr><p:grpSpPr><a:xfrm><a:off x="{420 * EMU}" y="{250 * EMU}"/><a:ext cx="{240 * EMU}" cy="{120 * EMU}"/>'
             f'<a:chOff x="0" y="0"/><a:chExt cx="{480 * EMU}" cy="{240 * EMU}"/></a:xfrm></p:grpSpPr>'
             + sp(21, "In outer", 0, 0, 200, 100, "ellipse", "70AD47", tp("outer", 'sz="2400"', 'algn="ctr"'), body='<a:bodyPr anchor="ctr"/>')
             + f'<p:grpSp><p:nvGrpSpPr><p:cNvPr id="22" name="Inner group"/><p:cNvGrpSpPr/><p:nvPr/></p:nvGrpSpPr><p:grpSpPr><a:xfrm><a:off x="{240 * EMU}" y="{0}"/><a:ext cx="{240 * EMU}" cy="{240 * EMU}"/>'
             f'<a:chOff x="0" y="0"/><a:chExt cx="{120 * EMU}" cy="{120 * EMU}"/></a:xfrm></p:grpSpPr>'
             + sp(23, "In inner", 10, 10, 100, 100, "star5", "FFC000") + '</p:grpSp></p:grpSp>')
    custom = (f'<p:sp><p:nvSpPr><p:cNvPr id="30" name="Custom"/><p:cNvSpPr/><p:nvPr/></p:nvSpPr><p:spPr><a:xfrm><a:off x="{40 * EMU}" y="{380 * EMU}"/><a:ext cx="{160 * EMU}" cy="{100 * EMU}"/></a:xfrm>'
              '<a:custGeom><a:avLst/><a:gdLst/><a:rect l="0" t="0" r="r" b="b"/><a:pathLst><a:path w="160" h="100"><a:moveTo><a:pt x="0" y="100"/></a:moveTo>'
              '<a:lnTo><a:pt x="40" y="0"/></a:lnTo><a:cubicBezTo><a:pt x="80" y="-40"/><a:pt x="120" y="140"/><a:pt x="160" y="40"/></a:cubicBezTo><a:lnTo><a:pt x="160" y="100"/></a:lnTo><a:close/></a:path></a:pathLst></a:custGeom>'
              '<a:solidFill><a:srgbClr val="9DC3E6"/></a:solidFill><a:ln w="12700"><a:solidFill><a:srgbClr val="1F3864"/></a:solidFill></a:ln></p:spPr></p:sp>')
    arrow = (f'<p:cxnSp><p:nvCxnSpPr><p:cNvPr id="31" name="Arrow"/><p:cNvCxnSpPr/><p:nvPr/></p:nvCxnSpPr><p:spPr><a:xfrm flipV="1"><a:off x="{230 * EMU}" y="{380 * EMU}"/><a:ext cx="{150 * EMU}" cy="{90 * EMU}"/></a:xfrm>'
             '<a:prstGeom prst="straightConnector1"><a:avLst/></a:prstGeom><a:ln w="28575"><a:solidFill><a:srgbClr val="C00000"/></a:solidFill><a:prstDash val="dash"/><a:tailEnd type="triangle"/></a:ln></p:spPr></p:cxnSp>')
    picture = (f'<p:pic><p:nvPicPr><p:cNvPr id="32" name="Cropped"/><p:cNvPicPr/><p:nvPr/></p:nvPicPr><p:blipFill><a:blip r:embed="rIdImg"/><a:srcRect l="25000" r="25000"/><a:stretch><a:fillRect/></a:stretch></p:blipFill>'
               f'<p:spPr><a:xfrm><a:off x="{420 * EMU}" y="{390 * EMU}"/><a:ext cx="{120 * EMU}" cy="{100 * EMU}"/></a:xfrm><a:prstGeom prst="rect"><a:avLst/></a:prstGeom></p:spPr></p:pic>')
    slide2 = CT_HEAD + f'''<p:sld {P}><p:cSld><p:spTree><p:nvGrpSpPr><p:cNvPr id="1" name=""/><p:cNvGrpSpPr/><p:nvPr/></p:nvGrpSpPr><p:grpSpPr/>
{sp(2, "Title 1", 0, 0, 0, 0, geom=None, ph='<p:ph type="title"/>', text=tp("Shapes and transforms"))}
{sp(10, "Rotated", 60, 130, 160, 80, "roundRect", grad, tp("rotated 30°", 'sz="1800"', 'algn="ctr"'), body='<a:bodyPr anchor="ctr"/>', xfrm_attrs=' rot="1800000"')}
{sp(11, "Flipped", 250, 120, 120, 100, "rtTriangle", "ED7D31", xfrm_attrs=' flipH="1"')}
{sp(12, "Vertical text", 390, 110, 40, 130, "rect", "FFF2CC", tp("VERTICAL", 'sz="1400"'), body='<a:bodyPr vert="vert270" anchor="ctr"/>')}
{group}{custom}{arrow}{picture}
</p:spTree></p:cSld></p:sld>'''
    # Slide 3: a table with merges and a style, a doughnut chart, speaker notes.
    def tc(text, attrs="", fill=None):
        f = f'<a:tcPr><a:solidFill><a:srgbClr val="{fill}"/></a:solidFill></a:tcPr>' if fill else "<a:tcPr/>"
        return f'<a:tc{attrs}><a:txBody><a:bodyPr/><a:lstStyle/>{tp(text, "sz=\"1400\"")}</a:txBody>{f}</a:tc>'
    rows = [[tc("Name", fill="2E75B6"), tc("Q1", fill="2E75B6"), tc("Q2", fill="2E75B6")],
            [tc("Spans two rows", ' rowSpan="2"'), tc("10"), tc("12")],
            [tc("", ' vMerge="1"'), tc("spans two columns", ' gridSpan="2"'), tc("", ' hMerge="1"')]]
    table = (f'<p:graphicFrame><p:nvGraphicFramePr><p:cNvPr id="40" name="Table"/><p:cNvGraphicFramePr/><p:nvPr/></p:nvGraphicFramePr>'
             f'<p:xfrm><a:off x="{36 * EMU}" y="{110 * EMU}"/><a:ext cx="{330 * EMU}" cy="{150 * EMU}"/></p:xfrm><a:graphic><a:graphicData uri="http://schemas.openxmlformats.org/drawingml/2006/table">'
             f'<a:tbl><a:tblPr firstRow="1" bandRow="1"><a:tableStyleId>{{5C22544A-7EE6-4342-B048-85BDC9FD1C3A}}</a:tableStyleId></a:tblPr><a:tblGrid><a:gridCol w="{130 * EMU}"/><a:gridCol w="{100 * EMU}"/><a:gridCol w="{100 * EMU}"/></a:tblGrid>'
             + "".join(f'<a:tr h="{50 * EMU}">' + "".join(r) + "</a:tr>" for r in rows) + '</a:tbl></a:graphicData></a:graphic></p:graphicFrame>')
    chart_frame = (f'<p:graphicFrame><p:nvGraphicFramePr><p:cNvPr id="41" name="Chart"/><p:cNvGraphicFramePr/><p:nvPr/></p:nvGraphicFramePr>'
                   f'<p:xfrm><a:off x="{390 * EMU}" y="{110 * EMU}"/><a:ext cx="{300 * EMU}" cy="{220 * EMU}"/></p:xfrm><a:graphic><a:graphicData uri="http://schemas.openxmlformats.org/drawingml/2006/chart">'
                   '<c:chart xmlns:c="http://schemas.openxmlformats.org/drawingml/2006/chart" r:id="rIdChart"/></a:graphicData></a:graphic></p:graphicFrame>')
    slide3 = CT_HEAD + f'''<p:sld {P}><p:cSld><p:spTree><p:nvGrpSpPr><p:cNvPr id="1" name=""/><p:cNvGrpSpPr/><p:nvPr/></p:nvGrpSpPr><p:grpSpPr/>
{sp(2, "Title 1", 0, 0, 0, 0, geom=None, ph='<p:ph type="title"/>', text=tp("Table and chart"))}{table}{chart_frame}
</p:spTree></p:cSld></p:sld>'''
    notes3 = CT_HEAD + f'''<p:notes {P}><p:cSld><p:spTree><p:nvGrpSpPr><p:cNvPr id="1" name=""/><p:cNvGrpSpPr/><p:nvPr/></p:nvGrpSpPr><p:grpSpPr/>
{sp(2, "Slide image", 0, 0, 0, 0, geom=None, ph='<p:ph type="sldImg"/>')}{sp(3, "Notes", 0, 0, 0, 0, geom=None, ph='<p:ph type="body" idx="1"/>', text=tp("Say the totals out loud.") + tp("Then pause."))}
</p:spTree></p:cSld></p:notes>'''
    presentation = CT_HEAD + f'''<p:presentation {P}><p:sldMasterIdLst><p:sldMasterId id="2147483648" r:id="rIdMaster"/></p:sldMasterIdLst>
<p:sldIdLst><p:sldId id="256" r:id="rIdS1"/><p:sldId id="257" r:id="rIdS2"/><p:sldId id="258" r:id="rIdS3"/></p:sldIdLst><p:sldSz cx="9144000" cy="6858000"/><p:notesSz cx="6858000" cy="9144000"/></p:presentation>'''
    chart_part = chart("doughnutChart", [("Share", [45, 30, 25])], ["Web", "Store", "Phone"], "Channels", '<c:holeSize val="55"/>').replace(
        '<c:varyColors val="0"/>', '<c:varyColors val="1"/>').replace('<c:axId val="1"/><c:axId val="2"/>', '')
    slide_rels = lambda extra=(): rels([("rIdLayout", "slideLayout", "../slideLayouts/slideLayout1.xml")] + list(extra))
    write_zip(os.path.join(HERE, "challenge.pptx"), [
        ("[Content_Types].xml", content_types([("ppt/presentation.xml", "application/vnd.openxmlformats-officedocument.presentationml.presentation.main+xml")])),
        ("_rels/.rels", rels([("rId1", "officeDocument", "ppt/presentation.xml")])),
        ("ppt/_rels/presentation.xml.rels", rels([("rIdMaster", "slideMaster", "slideMasters/slideMaster1.xml"), ("rIdS1", "slide", "slides/slide1.xml"),
                                                  ("rIdS2", "slide", "slides/slide2.xml"), ("rIdS3", "slide", "slides/slide3.xml"),
                                                  ("rIdTheme", "theme", "theme/theme1.xml")])),
        ("ppt/presentation.xml", presentation),
        ("ppt/slideMasters/slideMaster1.xml", master),
        ("ppt/slideMasters/_rels/slideMaster1.xml.rels", rels([("rId1", "slideLayout", "../slideLayouts/slideLayout1.xml"), ("rId2", "theme", "../theme/theme1.xml")])),
        ("ppt/slideLayouts/slideLayout1.xml", layout),
        ("ppt/slideLayouts/_rels/slideLayout1.xml.rels", rels([("rId1", "slideMaster", "../slideMasters/slideMaster1.xml")])),
        ("ppt/theme/theme1.xml", THEME),
        ("ppt/slides/slide1.xml", slide1), ("ppt/slides/_rels/slide1.xml.rels", slide_rels([("rIdLink", "hyperlink", "https://www.example.com/", True)])),
        ("ppt/slides/slide2.xml", slide2), ("ppt/slides/_rels/slide2.xml.rels", slide_rels([("rIdImg", "image", "../media/gradient.png")])),
        ("ppt/slides/slide3.xml", slide3), ("ppt/slides/_rels/slide3.xml.rels", slide_rels([("rIdChart", "chart", "../charts/chart1.xml"),
                                                                                           ("rIdNotes", "notesSlide", "../notesSlides/notesSlide1.xml")])),
        ("ppt/notesSlides/notesSlide1.xml", notes3), ("ppt/charts/chart1.xml", chart_part), ("ppt/media/gradient.png", GRADIENT),
    ])


# ---- PDF ----------------------------------------------------------------------------------------------

def make_pdf(path):
    """Revision 1: classic xref. Revision 2 (an incremental update): an xref stream, objects packed in an
    object stream, a page replaced, more pages. Fonts: a Type 0 font with a ToUnicode map, a simple
    font with a Differences encoding (glyph names, a ligature), the standard 14. Images: raw RGB with a
    soft mask, indexed, an image mask, a JPEG-less DCT-free set. Filters: Flate with a PNG predictor,
    LZW, ASCII85, ASCIIHex, chained. A page rotated, one cropped, a form XObject in a form."""
    out = bytearray(b"%PDF-1.7\n%\xe2\xe3\xcf\xd3\n")
    offsets = {}

    def obj(num, body):
        offsets[num] = len(out)
        out.extend(f"{num} 0 obj\n".encode() + (body if isinstance(body, bytes) else body.encode("latin-1")) + b"\nendobj\n")

    def stream(dict_, data, filters=()):
        """filters: names applied in order of decoding (first listed = first to undo)."""
        enc = data
        for f in reversed(filters):
            if f == "FlateDecode":
                enc = zlib.compress(enc)
            elif f == "ASCIIHexDecode":
                enc = enc.hex().upper().encode() + b">"
            elif f == "ASCII85Decode":
                enc = a85(enc)
            elif f == "LZWDecode":
                enc = lzw(enc)
        flt = ""
        if filters:
            flt = " /Filter " + (f"/{filters[0]}" if len(filters) == 1 else "[" + " ".join("/" + f for f in filters) + "]")
        return f"<< {dict_}{flt} /Length {len(enc)} >>\nstream\n".encode() + enc + b"\nendstream"

    def a85(data):
        res = bytearray()
        for i in range(0, len(data), 4):
            chunk = data[i:i + 4]
            pad = 4 - len(chunk)
            n = int.from_bytes(chunk + b"\0" * pad, "big")
            if n == 0 and pad == 0:
                res += b"z"
                continue
            digits = []
            for _ in range(5):
                digits.append(n % 85)
                n //= 85
            res += bytes(33 + d for d in reversed(digits))[:5 - pad]
        return bytes(res) + b"~>"

    def lzw(data):
        table = {bytes([i]): i for i in range(256)}
        codes, w, nxt = [256], b"", 258
        for ch in data:
            wc = w + bytes([ch])
            if wc in table:
                w = wc
                continue
            codes.append(table[w])
            table[wc] = nxt
            nxt += 1
            w = bytes([ch])
        if w:
            codes.append(table[w])
        codes.append(257)
        # Widths: 9 bits until the table reaches 511 (early change), then 10, ...
        bits, buf, nbits, size = 9, 0, 0, 258
        res = bytearray()
        for i, code in enumerate(codes):
            buf = (buf << bits) | code
            nbits += bits
            while nbits >= 8:
                res.append((buf >> (nbits - 8)) & 0xFF)
                nbits -= 8
            if i > 0 and code not in (256, 257):
                size += 1
            if size + 1 >= (1 << bits) and bits < 12:
                bits += 1
        if nbits:
            res.append((buf << (8 - nbits)) & 0xFF)
        return bytes(res)

    page_tree = 2
    # 1 catalog, 2 pages, 3.. fonts and pages.
    obj(1, "<< /Type /Catalog /Pages 2 0 R /PageLabels << /Nums [0 << /S /r >> 2 << /S /D >>] >> >>")
    obj(4, "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica-Bold /Encoding /WinAnsiEncoding >>")
    obj(5, "<< /Type /Font /Subtype /Type1 /BaseFont /Times-Roman /Encoding << /Type /Encoding /BaseEncoding /WinAnsiEncoding "
           "/Differences [1 /f_i /quoteright /emdash /Euro 128 /bullet] >> >>")
    cmap = b"""/CIDInit /ProcSet findresource begin 12 dict begin begincmap /CMapName /Challenge def
1 begincodespacerange <0000> <FFFF> endcodespacerange
3 beginbfchar <0003> <0020> <0024> <0041> <0101> <4E2D> endbfchar
2 beginbfrange <0044> <005D> <0061> <0102> <0103> [<6587> <5B57>] endbfrange
endcmap CMapName currentdict /CMap defineresource pop end end"""
    obj(6, stream("", cmap, ("FlateDecode",)))
    obj(7, "<< /Type /Font /Subtype /Type0 /BaseFont /ABCDEE+NotoSans-Regular /Encoding /Identity-H /DescendantFonts [8 0 R] /ToUnicode 6 0 R >>")
    obj(8, "<< /Type /Font /Subtype /CIDFontType2 /BaseFont /ABCDEE+NotoSans-Regular /CIDSystemInfo << /Registry (Adobe) /Ordering (Identity) /Supplement 0 >> "
           "/DW 1000 /W [3 [260] 36 [640] 68 93 560 257 [1000 1000 1000]] /FontDescriptor 9 0 R >>")
    obj(9, "<< /Type /FontDescriptor /FontName /ABCDEE+NotoSans-Regular /Flags 32 /ItalicAngle 0 /Ascent 1069 /Descent -293 /CapHeight 714 /StemV 80 /FontBBox [-621 -389 2800 1067] >>")
    # Images: raw RGB 8x8 with a soft mask; an indexed one; a 1-bit image mask.
    rgb = bytes(v for y in range(8) for x in range(8) for v in (x * 32, y * 32, 200))
    alpha = bytes(255 if (x + y) % 3 else 80 for y in range(8) for x in range(8))
    obj(10, stream("/Type /XObject /Subtype /Image /Width 8 /Height 8 /ColorSpace /DeviceRGB /BitsPerComponent 8 /SMask 11 0 R", rgb, ("FlateDecode",)))
    obj(11, stream("/Type /XObject /Subtype /Image /Width 8 /Height 8 /ColorSpace /DeviceGray /BitsPerComponent 8", alpha, ("FlateDecode",)))
    obj(12, stream("/Type /XObject /Subtype /Image /Width 4 /Height 2 /ColorSpace [/Indexed /DeviceRGB 2 <FF000000FF000000FF>] /BitsPerComponent 2",
                   bytes([0b00011000, 0b00011000]), ("ASCIIHexDecode",)))
    obj(13, stream("/Type /XObject /Subtype /Image /Width 8 /Height 8 /ImageMask true /BitsPerComponent 1 /Decode [1 0]",
                   bytes([0x18, 0x3C, 0x7E, 0xFF, 0xFF, 0x7E, 0x3C, 0x18])))
    # A form XObject drawing another form.
    obj(14, stream("/Type /XObject /Subtype /Form /BBox [0 0 100 100] /Resources << /XObject << /Inner 15 0 R >> >>",
                   b"0 0.5 0 rg 0 0 100 100 re f q 0.5 0 0 0.5 25 25 cm /Inner Do Q"))
    obj(15, stream("/Type /XObject /Subtype /Form /BBox [0 0 100 100] /Matrix [1 0 0 1 0 0]", b"1 1 0 rg 0 0 m 100 0 l 50 100 l h f"))
    resources = "<< /Font << /H 4 0 R /T 5 0 R /U 7 0 R >> /XObject << /Im 10 0 R /Ix 12 0 R /Mask 13 0 R /Form 14 0 R >> /ExtGState << /Half << /ca 0.5 /CA 0.5 >> >> >>"
    page1 = (b"BT /H 24 Tf 72 720 Td (Challenge PDF) Tj ET\n"
             b"BT /T 12 Tf 72 690 Td (Encoded: \x01nd, it\x02s \x03 and \x04 5 \x80 item) Tj ET\n"
             b"BT /U 14 Tf 72 664 Td <002400440045004600030101010201030003> Tj ET\n"
             b"BT /H 12 Tf 72 640 Td [(K) 80 (erned) -250 (words)] TJ 0 -16 Td 3 Tr (invisible text) Tj 0 Tr 0 -16 Td 150 Tz (wide) Tj 100 Tz 5 Ts ( raised) Tj ET\n"
             b"% a comment in the content stream\n"
             b"q /Half gs 1 0 0 rg 72 520 200 60 re f Q\n"
             b"0 0 1 RG 4 w 1 J [6 3] 0 d 300 520 m 400 580 l 500 520 l S\n"
             b"0.2 0.4 0.6 0.1 k 300 450 80 40 re f\n"
             b"/DeviceRGB cs 0.9 0.6 0.1 sc 400 450 m 440 490 480 410 520 450 c f\n"
             b"q 64 0 0 64 72 420 cm /Im Do Q q 64 0 0 32 150 430 cm /Ix Do Q q 0 0.6 0 rg 32 0 0 32 230 430 cm /Mask Do Q\n"
             b"q 1 0 0 1 72 300 cm /Form Do Q\n"
             b"BI /W 2 /H 2 /CS /G /BPC 8 ID \x00\xff\xff\x00 EI\n")
    obj(16, stream("", page1, ("FlateDecode",)))
    obj(17, f"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Resources {resources} /Contents 16 0 R /Annots [18 0 R] >>")
    obj(18, "<< /Type /Annot /Subtype /Link /Rect [72 715 250 740] /A << /S /URI /URI (https://www.example.com/) >> >>")
    # Page 2: rotated 90, inherits its MediaBox from the tree; LZW + ASCII85, chained.
    page2 = b"BT /H 18 Tf 72 500 Td (Page two is rotated) Tj ET 0 0 0 RG 72 72 m 540 720 l S"
    obj(19, stream("", page2, ("ASCII85Decode", "LZWDecode")))
    obj(20, f"<< /Type /Page /Parent 2 0 R /Rotate 90 /Resources {resources} /Contents 19 0 R >>")
    # Page 3: cropped; its content in two streams (an array).
    obj(21, stream("", b"q 1 0 0 rg 0 0 612 792 re f Q BT /H 18 Tf 100 650", ("FlateDecode",)))
    obj(22, stream("", b"Td (Cropped to a box) Tj ET", ()))
    obj(23, f"<< /Type /Page /Parent 2 0 R /CropBox [50 400 562 742] /Resources {resources} /Contents [21 0 R 22 0 R] >>")
    obj(page_tree, "<< /Type /Pages /Kids [17 0 R 24 0 R] /Count 3 /MediaBox [0 0 612 792] >>")
    obj(24, "<< /Type /Pages /Parent 2 0 R /Kids [20 0 R 23 0 R] /Count 2 >>")
    xref1 = len(out)
    nums = sorted(offsets)
    out.extend(f"xref\n0 {max(nums) + 1}\n0000000000 65535 f \n".encode())
    for n in range(1, max(nums) + 1):
        out.extend(f"{offsets[n]:010d} 00000 n \n".encode() if n in offsets else b"0000000000 00000 f \n")
    out.extend(f"trailer\n<< /Size {max(nums) + 1} /Root 1 0 R /Info << /Title (Challenge) >> >>\nstartxref\n{xref1}\n%%EOF\n".encode())

    # Revision 2: page 1's text changed (object 16 replaced), 40 more pages; the new objects packed in an
    # object stream, found through a cross-reference stream with a PNG predictor.
    rev2_page1 = page1.replace(b"(Challenge PDF)", b"(Challenge PDF, revised)")
    offsets.clear()
    obj(16, stream("", rev2_page1, ("FlateDecode",)))
    content_many = 25
    obj(content_many, stream("", b"BT /H 16 Tf 72 700 Td (A page of many) Tj ET", ("FlateDecode",)))
    packed = []  # (num, body) in the object stream
    kids = ["17 0 R", "24 0 R"]
    first_num = 100
    for i in range(40):
        num = first_num + i
        packed.append((num, f"<< /Type /Page /Parent 2 0 R /Resources {resources} /Contents {content_many} 0 R >>"))
        kids.append(f"{num} 0 R")
    packed.append((page_tree, f"<< /Type /Pages /Kids [{' '.join(kids)}] /Count 43 /MediaBox [0 0 612 792] >>"))
    header, bodies, pos = [], [], 0
    for num, body in packed:
        header.append(f"{num} {pos}")
        bodies.append(body)
        pos += len(body) + 1
    head = " ".join(header) + " "
    objstm_num = 99
    obj(objstm_num, stream(f"/Type /ObjStm /N {len(packed)} /First {len(head)}", (head + "\n".join(bodies) + "\n").encode("latin-1"), ("FlateDecode",)))
    xref_num = 98
    xref_pos = len(out)
    entries = {16: (1, offsets[16], 0), content_many: (1, offsets[content_many], 0), objstm_num: (1, offsets[objstm_num], 0), xref_num: (1, xref_pos, 0)}
    for idx, (num, _) in enumerate(packed):
        entries[num] = (2, objstm_num, idx)
    index = sorted(entries)
    ranges, rows = [], []
    for n in index:
        if ranges and ranges[-1][0] + ranges[-1][1] == n:
            ranges[-1][1] += 1
        else:
            ranges.append([n, 1])
        t, a, b = entries[n]
        rows.append(bytes([t]) + a.to_bytes(4, "big") + b.to_bytes(2, "big"))
    # PNG "Up" predictor on 7-byte rows.
    predicted, prev = bytearray(), bytes(7)
    for row in rows:
        predicted.append(2)
        predicted.extend((row[i] - prev[i]) & 0xFF for i in range(7))
        prev = row
    size = max(index) + 1
    data = zlib.compress(bytes(predicted))
    xref_dict = (f"<< /Type /XRef /Size {size} /Root 1 0 R /Prev {xref1} /W [1 4 2] /Index [{' '.join(f'{a} {b}' for a, b in ranges)}] "
                 f"/Filter /FlateDecode /DecodeParms << /Predictor 12 /Columns 7 >> /Length {len(data)} >>")
    out.extend(f"{xref_num} 0 obj\n{xref_dict}\nstream\n".encode() + data + b"\nendstream\nendobj\n")
    out.extend(f"startxref\n{xref_pos}\n%%EOF\n".encode())
    with open(path, "wb") as f:
        f.write(out)


if __name__ == "__main__":
    make_docx(os.path.join(HERE, "challenge.docx"))
    make_xlsx(os.path.join(HERE, "challenge.xlsx"))
    make_pptx(os.path.join(HERE, "challenge.pptx"))
    make_pdf(os.path.join(HERE, "challenge.pdf"))
    for name in ("challenge.docx", "challenge.xlsx", "challenge.pptx", "challenge.pdf"):
        print(f"{name:18} {os.path.getsize(os.path.join(HERE, name)):7} bytes")
