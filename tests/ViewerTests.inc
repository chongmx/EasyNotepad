// Tests of the document readers (plan 25): ZIP, XML, number formats, Word, Excel, PowerPoint and
// PDF, on files built here byte by byte. Included by TestMain.cpp (its TEST / CHECK macros).
//
// ENP_VIEWER_SAMPLES=<folder> also reads every .docx, .xlsx, .pptx and .pdf under that folder
// (for example ref/libreoffice-core's test data) and reports what each gave: a robustness run.

namespace mv = model::viewer;
namespace mp = model::package;

#include "ViewerFixtures.h"


TEST(package_inflate_and_zip)
{
    using namespace viewertest;
    const std::string text = "The quick brown fox jumps over the lazy dog. " + std::string(70000, 'z');
    CHECK(mp::inflate(deflateStored(text), false, 1 << 20) == text);         // raw, two stored blocks
    CHECK(mp::inflate(zlibStored("hello"), true, 1 << 20) == std::string("hello")); // zlib-wrapped
    CHECK(!mp::inflate(deflateStored(text), false, 1000));                    // over the limit: refused
    CHECK(!mp::inflate("\x07garbage", false, 1 << 20));                       // broken

    const std::string archive = zip({{"a.txt", "alpha"}, {"Dir/B.xml", "<b/>"}});
    mp::ZipArchive z;
    std::string error;
    CHECK(z.open(archive, &error));
    CHECK_EQ(z.entries().size(), std::size_t(2));
    CHECK(z.read("a.txt") == std::string("alpha"));
    CHECK(z.read("dir/b.xml") == std::string("<b/>")); // part names match without case
    CHECK(z.read("/a.txt") == std::string("alpha"));
    CHECK(!z.read("missing"));
    mp::ZipArchive stored;
    CHECK(stored.open(zip({{"s", "stored data"}}, false)) && stored.read("s") == std::string("stored data"));
    mp::ZipArchive broken;
    CHECK(!broken.open(archive.substr(0, archive.size() - 10), &error)); // the directory record cut off
    CHECK(!broken.open("not a zip", &error));
}

TEST(package_xml_reader)
{
    using mp::XmlReader;
    using E = XmlReader::Event;
    XmlReader x(R"(<?xml version="1.0"?><!-- c --><w:doc xmlns:w="u" a='1' w:b="x &amp; y"><w:t>A&lt;B&#x263A;&#65;</w:t><e/><![CDATA[<raw>]]></w:doc>)");
    CHECK(x.next() == E::Start && x.name() == "w:doc" && x.localName() == "doc");
    CHECK(x.attribute("a") == "1" && x.attribute("w:b") == "x & y" && x.attribute("b") == "x & y" && !x.hasAttribute("c"));
    CHECK(x.next() == E::Start && x.localName() == "t");
    CHECK(x.next() == E::Text && x.text() == "A<B\xE2\x98\xBA" "A");
    CHECK(x.next() == E::End && x.localName() == "t");
    CHECK(x.next() == E::Start && x.localName() == "e" && x.next() == E::End); // self-closing: Start, then End
    CHECK(x.next() == E::Text && x.text() == "<raw>");
    CHECK(x.next() == E::End && x.next() == E::Eof);

    // A DOCTYPE with entities is skipped, never expanded (no "billion laughs").
    XmlReader bomb("<!DOCTYPE x [<!ENTITY a \"aaaaaaaa\"><!ENTITY b \"&a;&a;&a;\">]><x>&b;</x>");
    CHECK(bomb.next() == E::Start && bomb.next() == E::Text && bomb.text() == "&b;");
    // Too deep, unterminated, mismatched: errors, not crashes.
    std::string deep;
    for (int i = 0; i < 300; ++i)
        deep += "<a>";
    XmlReader tooDeep(deep);
    E e;
    while ((e = tooDeep.next()) == E::Start) {
    }
    CHECK(e == E::Error);
    XmlReader cut("<a><b attr=\"x");
    CHECK(cut.next() == E::Start && cut.next() == E::Error);
    XmlReader skip("<r><skip><deep><x/></deep></skip><keep/></r>");
    CHECK(skip.next() == E::Start && skip.next() == E::Start && skip.localName() == "skip");
    skip.skip();
    CHECK(skip.next() == E::Start && skip.localName() == "keep");
    XmlReader text("<p>a<b>b</b>c</p>");
    CHECK(text.next() == E::Start && text.elementText() == "abc");
}

TEST(viewer_number_formats)
{
    using mv::formatNumber;
    CHECK_EQ(formatNumber(1234.5, "General"), std::string("1234.5"));
    CHECK_EQ(formatNumber(0.1 + 0.2, ""), std::string("0.3"));
    CHECK_EQ(formatNumber(-7, "General"), std::string("-7"));
    CHECK_EQ(formatNumber(3.14159, "0.00"), std::string("3.14"));
    CHECK_EQ(formatNumber(1234567, "#,##0"), std::string("1,234,567"));
    CHECK_EQ(formatNumber(1234.5, "#,##0.00"), std::string("1,234.50"));
    CHECK_EQ(formatNumber(0.256, "0%"), std::string("26%"));
    CHECK_EQ(formatNumber(0.256, "0.00%"), std::string("25.60%"));
    CHECK_EQ(formatNumber(-2, "0.00;(0.00)"), std::string("(2.00)"));
    CHECK_EQ(formatNumber(-2, "0.00"), std::string("-2.00"));
    CHECK_EQ(formatNumber(0, "0.00;-0.00;\"zero\""), std::string("zero"));
    CHECK_EQ(formatNumber(1234.5, "\"$\"#,##0.00"), std::string("$1,234.50"));
    CHECK_EQ(formatNumber(1234.5, "[$\xE2\x82\xAC-407] #,##0.00"), std::string("\xE2\x82\xAC 1,234.50"));
    CHECK_EQ(formatNumber(12345.678, "0.00E+00"), std::string("1.23E+04"));
    CHECK_EQ(formatNumber(45000, "yyyy-mm-dd"), std::string("2023-03-15"));
    CHECK_EQ(formatNumber(45000, "m/d/yyyy"), std::string("3/15/2023"));
    CHECK_EQ(formatNumber(45000, "d mmm yyyy"), std::string("15 Mar 2023"));
    CHECK_EQ(formatNumber(45000, "dddd, mmmm d"), std::string("Wednesday, March 15"));
    CHECK_EQ(formatNumber(0.75, "h:mm AM/PM"), std::string("6:00 PM"));
    CHECK_EQ(formatNumber(0.5 + 1.0 / 1440 * 5, "hh:mm:ss"), std::string("12:05:00"));
    CHECK_EQ(formatNumber(1.5, "[h]:mm"), std::string("36:00"));
    CHECK_EQ(formatNumber(60, "yyyy-mm-dd"), std::string("1900-02-29")); // Excel's phantom leap day
    CHECK_EQ(formatNumber(61, "yyyy-mm-dd"), std::string("1900-03-01"));
    CHECK_EQ(formatNumber(0, "yyyy-mm-dd", true), std::string("1904-01-01"));
    CHECK(mv::isDateFormat("d-mmm-yy") && mv::isDateFormat("[h]:mm") && !mv::isDateFormat("0.00") && !mv::isDateFormat("[Red]0"));
    CHECK_EQ(std::string(mv::builtinNumberFormat(14)), std::string("m/d/yyyy"));
}

TEST(viewer_reads_word_documents)
{
    using namespace viewertest;
    namespace md = model::markdown;
    const std::string styles = R"(<w:styles xmlns:w="w"><w:style w:type="paragraph" w:styleId="Heading1"><w:name w:val="heading 1"/></w:style>
<w:style w:type="paragraph" w:styleId="Sub"><w:name w:val="My sub"/><w:basedOn w:val="Heading1"/></w:style></w:styles>)";
    const std::string numbering = R"x(<w:numbering xmlns:w="w">
<w:abstractNum w:abstractNumId="0"><w:lvl w:ilvl="0"><w:start w:val="1"/><w:numFmt w:val="decimal"/><w:lvlText w:val="%1."/></w:lvl>
<w:lvl w:ilvl="1"><w:start w:val="1"/><w:numFmt w:val="lowerLetter"/><w:lvlText w:val="%1.%2)"/></w:lvl></w:abstractNum>
<w:abstractNum w:abstractNumId="1"><w:lvl w:ilvl="0"><w:numFmt w:val="bullet"/><w:lvlText w:val="*"/></w:lvl></w:abstractNum>
<w:num w:numId="1"><w:abstractNumId w:val="0"/></w:num><w:num w:numId="2"><w:abstractNumId w:val="1"/></w:num></w:numbering>)x";
    auto item = [](const char* text, int num, int level) {
        return std::string("<w:p><w:pPr><w:numPr><w:ilvl w:val=\"") + std::to_string(level) + "\"/><w:numId w:val=\"" + std::to_string(num) +
               "\"/></w:numPr></w:pPr><w:r><w:t>" + text + "</w:t></w:r></w:p>";
    };
    const std::string document = std::string(R"(<w:document xmlns:w="w" xmlns:r="r" xmlns:a="a"><w:body>
<w:p><w:pPr><w:pStyle w:val="Heading1"/></w:pPr><w:r><w:t>Title Text</w:t></w:r></w:p>
<w:p><w:pPr><w:pStyle w:val="Sub"/></w:pPr><w:r><w:t>Based on a heading</w:t></w:r></w:p>
<w:p><w:r><w:t xml:space="preserve">Hello </w:t></w:r><w:r><w:rPr><w:b/></w:rPr><w:t>world</w:t></w:r><w:del><w:r><w:delText>gone</w:delText></w:r></w:del>
<w:r><w:fldChar w:fldCharType="begin"/></w:r><w:r><w:instrText>PAGE</w:instrText></w:r><w:r><w:fldChar w:fldCharType="separate"/></w:r><w:r><w:t>7</w:t></w:r><w:r><w:fldChar w:fldCharType="end"/></w:r></w:p>
)") + item("one", 1, 0) + item("one-a", 1, 1) + item("two", 1, 0) + item("dot", 2, 0) +
                                 R"(<w:tbl><w:tr><w:tc><w:p><w:r><w:t>H1</w:t></w:r></w:p></w:tc><w:tc><w:p><w:r><w:t>H2</w:t></w:r></w:p></w:tc></w:tr>
<w:tr><w:tc><w:tcPr><w:gridSpan w:val="2"/></w:tcPr><w:p><w:r><w:t>wide</w:t></w:r></w:p></w:tc></w:tr></w:tbl>
<w:p><w:hyperlink r:id="rId9"><w:r><w:t>a link</w:t></w:r></w:hyperlink><w:r><w:footnoteReference w:id="5"/></w:r></w:p>
<w:p><w:r><w:drawing><wp:inline xmlns:wp="wp"><wp:docPr id="1" name="p" descr="A chart picture"/><a:graphic><a:graphicData><pic:pic xmlns:pic="pic"><pic:blipFill><a:blip r:embed="rId5"/></pic:blipFill></pic:pic></a:graphicData></a:graphic></wp:inline></w:drawing></w:r></w:p>
<w:p></w:p><w:sectPr/></w:body></w:document>)";
    const std::string footnotes = R"(<w:footnotes xmlns:w="w"><w:footnote w:type="separator" w:id="-1"><w:p><w:r><w:t>---</w:t></w:r></w:p></w:footnote>
<w:footnote w:id="5"><w:p><w:r><w:t>The note.</w:t></w:r></w:p></w:footnote></w:footnotes>)";
    const std::string bytes = zip({{"_rels/.rels", packageRels("word/document.xml")},
                                   {"word/_rels/document.xml.rels",
                                    rels({{"rId1", "styles", "styles.xml"},
                                          {"rId2", "numbering", "numbering.xml"},
                                          {"rId3", "footnotes", "footnotes.xml"},
                                          {"rId5", "image", "media/image1.png"},
                                          {"rId9", "hyperlink", "https://example.com/"}})},
                                   {"word/document.xml", document},
                                   {"word/styles.xml", styles},
                                   {"word/numbering.xml", numbering},
                                   {"word/footnotes.xml", footnotes},
                                   {"word/media/image1.png", "PNGDATA"}});
    CHECK_EQ(mv::viewerFormat("Report.DOCX", bytes), std::string("Word document"));
    const mv::ViewerDocument doc = mv::readViewerDocument("Report.docx", bytes);
    CHECK(doc.shape == mv::Shape::Flow && doc.error.empty());
    const std::vector<md::Block>& b = doc.flow.blocks;
    CHECK(b.size() >= 10);
    if (b.size() < 10)
        return;
    CHECK(b[0].kind == md::BlockKind::Heading && b[0].level == 1 && md::plainText(b[0].spans) == "Title Text");
    CHECK(b[1].kind == md::BlockKind::Heading && b[1].level == 1); // through basedOn
    CHECK(md::plainText(b[2].spans) == "Hello world7");              // no deleted text, no field instruction
    CHECK(b[2].spans.size() >= 2 && b[2].spans[1].text == "world" && (b[2].spans[1].style & md::Bold));
    CHECK(b[3].kind == md::BlockKind::Numbered && b[3].marker == "1." && b[3].level == 0);
    CHECK(b[4].kind == md::BlockKind::Numbered && b[4].marker == "1.a)" && b[4].level == 1);
    CHECK(b[5].kind == md::BlockKind::Numbered && b[5].marker == "2.");
    CHECK(b[6].kind == md::BlockKind::Bullet);
    CHECK(b[7].kind == md::BlockKind::Table && b[7].rows.size() == 2 && b[7].rows[0].size() == 2 &&
          md::plainText(b[7].rows[0][1]) == "H2" && md::plainText(b[7].rows[1][0]) == "wide");
    CHECK(b[8].spans.size() >= 1 && (b[8].spans[0].style & md::Link) && b[8].spans[0].url == "https://example.com/");
    CHECK(md::plainText(b[8].spans).find("[1]") != std::string::npos); // the footnote's mark
    CHECK(b[9].spans.size() == 1 && (b[9].spans[0].style & md::Image) && b[9].spans[0].url == "word/media/image1.png" &&
          b[9].spans[0].text == "A chart picture");
    CHECK(doc.flow.media.count("word/media/image1.png") == 1);
    const std::string all = mv::plainText(doc);
    CHECK(all.find("The note.") != std::string::npos);   // footnotes follow the body
    CHECK(all.find("gone") == std::string::npos && all.find("PAGE") == std::string::npos && all.find("---") == std::string::npos);

    const mv::ViewerDocument broken = mv::readViewerDocument("x.docx", "PK\x03\x04 not really a zip");
    CHECK(broken.shape == mv::Shape::None && !broken.error.empty());
    const mv::ViewerDocument empty = mv::readDocx(zip({{"_rels/.rels", packageRels("word/document.xml")}}));
    CHECK(empty.shape == mv::Shape::None && empty.error.find("missing") != std::string::npos);
}

TEST(viewer_reads_excel_workbooks)
{
    using namespace viewertest;
    const std::string workbook = R"(<workbook xmlns:r="r"><workbookPr/><sheets><sheet name="Data" sheetId="1" r:id="rId1"/>
<sheet name="Secret" sheetId="2" state="hidden" r:id="rId2"/></sheets></workbook>)";
    const std::string shared = R"(<sst><si><t>Name</t></si><si><r><t>Rich</t></r><r><t>Text</t></r><rPh><t>yomi</t></rPh></si></sst>)";
    const std::string styles = R"(<styleSheet><numFmts><numFmt numFmtId="164" formatCode="0.00%"/></numFmts>
<fonts><font/><font><b/><color rgb="FFFF0000"/></font></fonts>
<fills><fill><patternFill patternType="none"/></fill><fill><patternFill patternType="gray125"/></fill><fill><patternFill patternType="solid"><fgColor rgb="FFFFFF00"/></patternFill></fill></fills>
<cellXfs><xf numFmtId="0"/><xf numFmtId="164"/><xf fontId="1" fillId="2"><alignment horizontal="center"/></xf><xf numFmtId="14"/></cellXfs></styleSheet>)";
    const std::string sheet1 = R"(<worksheet><sheetViews><sheetView><pane ySplit="1" topLeftCell="A2" state="frozen"/></sheetView></sheetViews>
<sheetFormatPr defaultRowHeight="15"/><cols><col min="2" max="3" width="20.5" customWidth="1"/></cols><sheetData>
<row r="1"><c r="A1" t="s"><v>0</v></c><c r="B1" t="s" s="2"><v>1</v></c></row>
<row r="2" ht="30" customHeight="1"><c r="A2" s="1"><v>0.256</v></c><c r="B2" s="3"><v>45000</v></c><c r="C2"><f>A2*2</f><v>0.512</v></c>
<c r="D2" t="b"><v>1</v></c><c r="E2" t="inlineStr"><is><t>inline</t></is></c><c r="F2" t="e"><v>#DIV/0!</v></c></row>
<row r="5"><c r="AA5"><v>42</v></c></row></sheetData><mergeCells count="1"><mergeCell ref="A3:B4"/></mergeCells></worksheet>)";
    const std::string sheet2 = R"(<worksheet><sheetData><row r="1"><c r="A1"><v>1</v></c></row></sheetData></worksheet>)";
    const std::string bytes = zip({{"_rels/.rels", packageRels("xl/workbook.xml")},
                                   {"xl/_rels/workbook.xml.rels",
                                    rels({{"rId1", "worksheet", "worksheets/sheet1.xml"},
                                          {"rId2", "worksheet", "worksheets/sheet2.xml"},
                                          {"rId3", "sharedStrings", "sharedStrings.xml"},
                                          {"rId4", "styles", "styles.xml"}})},
                                   {"xl/workbook.xml", workbook},
                                   {"xl/sharedStrings.xml", shared},
                                   {"xl/styles.xml", styles},
                                   {"xl/worksheets/sheet1.xml", sheet1},
                                   {"xl/worksheets/sheet2.xml", sheet2}});
    const mv::ViewerDocument doc = mv::readViewerDocument("Book1.xlsx", bytes);
    CHECK(doc.shape == mv::Shape::Sheet && doc.book.sheets.size() == 2);
    if (doc.book.sheets.size() != 2)
        return;
    const mv::Sheet& s = doc.book.sheets[0];
    CHECK(s.name == "Data" && !s.hidden && doc.book.sheets[1].hidden);
    CHECK(s.frozenRows == 1 && s.frozenColumns == 0);
    CHECK(s.rowCount == 5 && s.columnCount == 27); // AA is column 27
    CHECK(s.columnWidth(1) == 20.5f && s.columnWidth(2) == 20.5f && s.columnWidth(0) == s.defaultColumnWidth);
    auto text = [&](std::uint32_t r, std::uint32_t c) {
        const mv::Cell* cell = s.cell(r, c);
        return cell ? cell->text : std::string("<none>");
    };
    CHECK_EQ(text(0, 0), std::string("Name"));
    CHECK_EQ(text(0, 1), std::string("RichText")); // runs joined, phonetic hint left out
    const mv::Cell* bold = s.cell(0, 1);
    CHECK(bold && (bold->flags & mv::Cell::Bold) && bold->fill == model::viewer::rgba(255, 255, 0) &&
          bold->color == model::viewer::rgba(255, 0, 0) && bold->align == 1);
    CHECK_EQ(text(1, 0), std::string("25.60%"));
    CHECK_EQ(text(1, 1), std::string("3/15/2023"));
    CHECK_EQ(text(1, 2), std::string("0.512"));
    CHECK(s.cell(1, 2) && s.cell(1, 2)->formula == "A2*2" && (s.cell(1, 2)->flags & mv::Cell::Number));
    CHECK_EQ(text(1, 3), std::string("TRUE"));
    CHECK_EQ(text(1, 4), std::string("inline"));
    CHECK(s.cell(1, 5) && (s.cell(1, 5)->flags & mv::Cell::Error) && s.cell(1, 5)->text == "#DIV/0!");
    CHECK_EQ(text(4, 26), std::string("42"));
    CHECK(!s.cell(3, 0));
    CHECK(s.row(1) && s.row(1)->height == 30.0f);
    CHECK(s.merges.size() == 1 && s.merges[0].row0 == 2 && s.merges[0].col1 == 1 && s.merges[0].row1 == 3);
    CHECK(mv::plainText(doc).find("Name\tRichText") != std::string::npos);
}

TEST(viewer_reads_powerpoint_slides)
{
    using namespace viewertest;
    const std::string presentation = R"(<p:presentation xmlns:p="p" xmlns:r="r"><p:sldIdLst><p:sldId id="256" r:id="rId2"/></p:sldIdLst>
<p:sldSz cx="9144000" cy="6858000"/></p:presentation>)";
    const std::string theme = R"(<a:theme xmlns:a="a"><a:themeElements><a:clrScheme name="t"><a:dk1><a:sysClr val="windowText" lastClr="000000"/></a:dk1>
<a:lt1><a:srgbClr val="FFFFFF"/></a:lt1><a:accent1><a:srgbClr val="FF0000"/></a:accent1></a:clrScheme></a:themeElements></a:theme>)";
    const std::string master = R"(<p:sldMaster xmlns:p="p" xmlns:a="a"><p:cSld><p:bg><p:bgPr><a:solidFill><a:srgbClr val="EEEEEE"/></a:solidFill></p:bgPr></p:bg>
<p:spTree><p:sp><p:nvSpPr><p:nvPr/></p:nvSpPr><p:spPr><a:xfrm><a:off x="0" y="6350000"/><a:ext cx="9144000" cy="508000"/></a:xfrm><a:prstGeom prst="rect"/>
<a:solidFill><a:srgbClr val="0000FF"/></a:solidFill></p:spPr></p:sp>
<p:sp><p:nvSpPr><p:nvPr><p:ph type="title"/></p:nvPr></p:nvSpPr><p:spPr><a:xfrm><a:off x="1" y="1"/><a:ext cx="2" cy="2"/></a:xfrm></p:spPr>
<p:txBody><a:p><a:r><a:t>Click to edit</a:t></a:r></a:p></p:txBody></p:sp></p:spTree></p:cSld>
<p:clrMap bg1="lt1" tx1="dk1" bg2="lt2" tx2="dk2"/><p:txStyles><p:titleStyle><a:lvl1pPr><a:defRPr sz="4400"/></a:lvl1pPr></p:titleStyle>
<p:bodyStyle><a:lvl1pPr><a:buChar char="&#8226;"/><a:defRPr sz="3200"/></a:lvl1pPr></p:bodyStyle></p:txStyles></p:sldMaster>)";
    const std::string layout = R"(<p:sldLayout xmlns:p="p" xmlns:a="a"><p:cSld><p:spTree>
<p:sp><p:nvSpPr><p:nvPr><p:ph type="title"/></p:nvPr></p:nvSpPr><p:spPr><a:xfrm><a:off x="457200" y="274638"/><a:ext cx="8229600" cy="1143000"/></a:xfrm></p:spPr></p:sp>
<p:sp><p:nvSpPr><p:nvPr><p:ph idx="1"/></p:nvPr></p:nvSpPr><p:spPr><a:xfrm><a:off x="457200" y="1600200"/><a:ext cx="8229600" cy="4525963"/></a:xfrm></p:spPr></p:sp>
</p:spTree></p:cSld></p:sldLayout>)";
    const std::string slide = R"(<p:sld xmlns:p="p" xmlns:a="a" xmlns:r="r"><p:cSld><p:spTree>
<p:sp><p:nvSpPr><p:nvPr><p:ph type="title"/></p:nvPr></p:nvSpPr><p:spPr/><p:txBody><a:bodyPr/><a:p><a:r><a:t>Hello Slides</a:t></a:r></a:p></p:txBody></p:sp>
<p:sp><p:nvSpPr><p:nvPr><p:ph idx="1"/></p:nvPr></p:nvSpPr><p:spPr/><p:txBody><a:bodyPr/><a:p><a:r><a:t>First point</a:t></a:r></a:p>
<a:p><a:pPr><a:buNone/></a:pPr><a:r><a:t>No bullet</a:t></a:r></a:p></p:txBody></p:sp>
<p:sp><p:nvSpPr><p:nvPr/></p:nvSpPr><p:spPr><a:xfrm><a:off x="1270000" y="2540000"/><a:ext cx="2540000" cy="1270000"/></a:xfrm>
<a:prstGeom prst="ellipse"/><a:solidFill><a:schemeClr val="accent1"/></a:solidFill></p:spPr>
<p:txBody><a:bodyPr anchor="ctr"/><a:p><a:pPr algn="ctr"/><a:r><a:rPr sz="1800" b="1"><a:solidFill><a:srgbClr val="00FF00"/></a:solidFill></a:rPr><a:t>Bold green</a:t></a:r></a:p></p:txBody></p:sp>
<p:grpSp><p:nvGrpSpPr/><p:grpSpPr><a:xfrm><a:off x="0" y="0"/><a:ext cx="1270000" cy="1270000"/><a:chOff x="0" y="0"/><a:chExt cx="2540000" cy="2540000"/></a:xfrm></p:grpSpPr>
<p:pic><p:nvPicPr/><p:blipFill><a:blip r:embed="rId3"/></p:blipFill><p:spPr><a:xfrm><a:off x="1270000" y="1270000"/><a:ext cx="1270000" cy="1270000"/></a:xfrm></p:spPr></p:pic></p:grpSp>
</p:spTree></p:cSld></p:sld>)";
    const std::string notes = R"(<p:notes xmlns:p="p" xmlns:a="a"><p:cSld><p:spTree><p:sp><p:nvSpPr><p:nvPr><p:ph type="body"/></p:nvPr></p:nvSpPr>
<p:txBody><a:p><a:r><a:t>Say hello.</a:t></a:r></a:p></p:txBody></p:sp></p:spTree></p:cSld></p:notes>)";
    const std::string bytes = zip({{"_rels/.rels", packageRels("ppt/presentation.xml")},
                                   {"ppt/_rels/presentation.xml.rels", rels({{"rId2", "slide", "slides/slide1.xml"}})},
                                   {"ppt/presentation.xml", presentation},
                                   {"ppt/slides/_rels/slide1.xml.rels",
                                    rels({{"rId1", "slideLayout", "../slideLayouts/slideLayout1.xml"},
                                          {"rId3", "image", "../media/image1.png"},
                                          {"rId4", "notesSlide", "../notesSlides/notesSlide1.xml"}})},
                                   {"ppt/slides/slide1.xml", slide},
                                   {"ppt/slideLayouts/_rels/slideLayout1.xml.rels", rels({{"rId1", "slideMaster", "../slideMasters/slideMaster1.xml"}})},
                                   {"ppt/slideLayouts/slideLayout1.xml", layout},
                                   {"ppt/slideMasters/_rels/slideMaster1.xml.rels", rels({{"rId1", "theme", "../theme/theme1.xml"}})},
                                   {"ppt/slideMasters/slideMaster1.xml", master},
                                   {"ppt/theme/theme1.xml", theme},
                                   {"ppt/notesSlides/notesSlide1.xml", notes},
                                   {"ppt/media/image1.png", "PNGDATA"}});
    const mv::ViewerDocument doc = mv::readViewerDocument("deck.pptx", bytes);
    CHECK(doc.shape == mv::Shape::Pages && doc.pages.pages.size() == 1);
    if (doc.pages.pages.size() != 1)
        return;
    const mv::Page& p = doc.pages.pages[0];
    CHECK(std::fabs(p.width - 720.0f) < 0.01f && std::fabs(p.height - 540.0f) < 0.01f);
    CHECK(p.background == mv::rgba(0xEE, 0xEE, 0xEE));
    CHECK_EQ(p.title, std::string("Hello Slides"));
    CHECK_EQ(p.notes, std::string("Say hello."));
    // The master's own shape (a blue band) is behind; its title prompt is not drawn.
    CHECK(!p.paths.empty() && p.paths[0].fill == mv::rgba(0, 0, 255));
    CHECK(p.plainText().find("Click to edit") == std::string::npos);
    const mv::TextBox* title = nullptr;
    const mv::TextBox* body = nullptr;
    const mv::TextBox* shapeText = nullptr;
    for (const mv::TextBox& b : p.boxes) {
        const std::string t = b.paragraphs.empty() || b.paragraphs[0].runs.empty() ? "" : b.paragraphs[0].runs[0].text;
        if (t == "Hello Slides")
            title = &b;
        else if (t == "First point")
            body = &b;
        else if (t == "Bold green")
            shapeText = &b;
    }
    CHECK(title && std::fabs(title->box.x - 36.0f) < 0.01f && std::fabs(title->box.w - 648.0f) < 0.01f); // from the layout
    CHECK(title && title->paragraphs[0].runs[0].size == 44.0f);                                         // the master's title size
    CHECK(body && body->paragraphs.size() == 2 && body->paragraphs[0].bullet == "\xE2\x80\xA2" && body->paragraphs[1].bullet.empty());
    CHECK(body && body->paragraphs[0].runs[0].size == 32.0f);
    CHECK(shapeText && shapeText->anchor == 1 && shapeText->paragraphs[0].align == 1);
    CHECK(shapeText && (shapeText->paragraphs[0].runs[0].flags & mv::Bold) && shapeText->paragraphs[0].runs[0].color == mv::rgba(0, 255, 0));
    bool redEllipse = false;
    for (const mv::PathItem& path : p.paths)
        if (path.fill == mv::rgba(255, 0, 0) && path.subpaths.size() == 1 && path.subpaths[0].size() > 20)
            redEllipse = true;
    CHECK(redEllipse); // accent1 from the theme
    // The picture inside a group scaled by half: at (50, 50) points, 50 wide.
    CHECK(p.images.size() == 1 && std::fabs(p.images[0].box.x - 50.0f) < 0.01f && std::fabs(p.images[0].box.w - 50.0f) < 0.01f &&
          p.images[0].media == "ppt/media/image1.png" && doc.pages.media.count("ppt/media/image1.png") == 1);
}

TEST(viewer_reads_pdf_pages)
{
    using namespace viewertest;
    const std::string content = "1 0 0 rg 10 10 50 20 re f\nBT /F1 12 Tf 20 60 Td (Hello PDF) Tj ET\n"
                                "BT /F1 10 Tf 20 30 Td [(Hel) -20 (lo) -300 (World)] TJ ET\n"
                                "0 0 1 RG 2 w 100 10 m 150 40 l S\n";
    const std::string bytes = onePagePdf(content);
    CHECK_EQ(mv::viewerFormat("a.pdf", bytes), std::string("PDF document"));
    mv::ViewerDocument doc = mv::readViewerDocument("a.pdf", bytes);
    CHECK(doc.shape == mv::Shape::Pages && doc.pages.pages.size() == 1 && doc.error.empty());
    if (doc.pages.pages.size() != 1)
        return;
    CHECK(!doc.pages.pages[0].loaded && doc.pages.pages[0].width == 200.0f && doc.pages.pages[0].height == 100.0f);
    CHECK(doc.pages.load(0));
    const mv::Page& p = doc.pages.pages[0];
    CHECK(p.loaded && p.paths.size() == 2);
    if (p.paths.size() == 2) {
        const mv::PathItem& rect = p.paths[0];
        CHECK(rect.fill == mv::rgba(255, 0, 0) && rect.subpaths.size() == 1 && rect.subpaths[0].size() == 4);
        // PDF y grows upwards: y 10..30 on a 100-point page is 70..90 from the top.
        CHECK(std::fabs(rect.subpaths[0][0].x - 10.0f) < 0.01f && std::fabs(rect.subpaths[0][0].y - 90.0f) < 0.01f);
        const mv::PathItem& line = p.paths[1];
        CHECK(line.fill == 0 && line.stroke == mv::rgba(0, 0, 255) && std::fabs(line.lineWidth - 2.0f) < 0.01f);
    }
    const mv::GlyphRun* hello = runWith(p, "Hello PDF");
    CHECK(hello && std::fabs(hello->x - 20.0f) < 0.01f && std::fabs(hello->baseline - 40.0f) < 0.01f && std::fabs(hello->size - 12.0f) < 0.01f);
    // Helvetica's widths place each glyph: 'H' is 0.722 em, so 'e' starts 8.664 points in.
    CHECK(hello && hello->offsets.size() == 9 && std::fabs(hello->offsets[1] - 8.664f) < 0.01f);
    const mv::GlyphRun* world = runWith(p, "Hello World"); // kerning joined; a wide gap became a space
    CHECK(world != nullptr);
    CHECK(p.plainText().find("Hello PDF") != std::string::npos);
    doc.pages.unload(0);
    CHECK(!doc.pages.pages[0].loaded && doc.pages.pages[0].glyphs.empty());
    CHECK(doc.pages.load(0) && runWith(doc.pages.pages[0], "Hello PDF"));

    // A broken cross-reference table: the objects are found by scanning the file.
    mv::ViewerDocument rebuilt = mv::readPdf(pdf({"<< /Type /Catalog /Pages 2 0 R >>", "<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
                                                  "<< /Type /Page /Parent 2 0 R /Contents 4 0 R >>", stream("", "BT 12 Tf (Scanned) Tj ET", false)},
                                                 "", true));
    CHECK(rebuilt.shape == mv::Shape::Pages && rebuilt.pages.load(0) && runWith(rebuilt.pages.pages[0], "Scanned"));
    // Encrypted files are refused with a reason; not a PDF at all is an error.
    const mv::ViewerDocument locked = mv::readPdf(pdf({"<< /Type /Catalog /Pages 2 0 R >>", "<< /Type /Pages /Kids [] /Count 0 >>",
                                                       "<< /Filter /Standard /V 1 >>"},
                                                      "/Encrypt 3 0 R "));
    CHECK(locked.shape == mv::Shape::None && locked.error.find("encrypted") != std::string::npos);
    CHECK(mv::readPdf("hello").shape == mv::Shape::None);
}

TEST(viewer_reads_pdf_fonts_and_object_streams)
{
    using namespace viewertest;
    // A Type 0 font with a ToUnicode map: two-byte codes 1 and 2 are "A" and "B".
    const std::string cmap = "/CIDInit /ProcSet findresource begin 12 dict begin begincmap\n1 begincodespacerange <0000> <FFFF> endcodespacerange\n"
                             "2 beginbfchar <0001> <0041> <0002> <0042> endbfchar\n1 beginbfrange <0010> <0012> <0061> endbfrange\nendcmap end end\n";
    const std::string type0 = "<< /Type /Font /Subtype /Type0 /BaseFont /ABCDEF+Sans-Bold /Encoding /Identity-H /DescendantFonts [7 0 R] /ToUnicode 6 0 R >>";
    const std::string bytes = onePagePdf("BT /F1 20 Tf 10 50 Td <000100020010001100120002> Tj ET", type0,
                                         {stream("", cmap, true),
                                          "<< /Type /Font /Subtype /CIDFontType2 /BaseFont /ABCDEF+Sans-Bold /DW 500 /W [1 [600 700]] >>"});
    mv::ViewerDocument doc = mv::readPdf(bytes);
    CHECK(doc.shape == mv::Shape::Pages && doc.pages.load(0));
    const mv::GlyphRun* run = doc.pages.pages.empty() ? nullptr : runWith(doc.pages.pages[0], "ABabcB");
    CHECK(run && (run->flags & mv::Bold));
    // The widths of the W array: 'A' 0.6 em at 20 points, so 'B' starts 12 points in.
    CHECK(run && run->offsets.size() == 6 && std::fabs(run->offsets[1] - 12.0f) < 0.01f);

    // Objects inside an object stream, found through a cross-reference stream.
    const std::string objs = "1 0 2 40 3 90 ";
    std::string body = objs;
    body.resize(14, ' ');
    const std::string o1 = "<< /Type /Catalog /Pages 2 0 R >>";
    const std::string o2 = "<< /Type /Pages /Kids [3 0 R] /Count 1 >>";
    const std::string o3 = "<< /Type /Page /Parent 2 0 R /Contents 5 0 R >>";
    std::string data = body;
    auto place = [&](std::size_t offset, const std::string& o) {
        if (data.size() < 14 + offset)
            data.resize(14 + offset, ' ');
        data += o;
    };
    place(0, o1);
    place(40, o2);
    place(90, o3);
    std::string out = "%PDF-1.5\n";
    const std::size_t at4 = out.size();
    out += "4 0 obj\n" + stream("/Type /ObjStm /N 3 /First 14", data, true) + "\nendobj\n";
    const std::size_t at5 = out.size();
    out += "5 0 obj\n" + stream("", "BT 9 Tf 5 5 Td (Packed) Tj ET", true) + "\nendobj\n";
    const std::size_t at6 = out.size();
    std::string xref; // W [1 4 2]: type, offset or stream number, generation or index
    auto row = [&](int type, std::size_t a, int b) {
        xref += char(type);
        for (int s = 24; s >= 0; s -= 8)
            xref += char((a >> s) & 0xFF);
        xref += char((b >> 8) & 0xFF);
        xref += char(b & 0xFF);
    };
    row(0, 0, 0);
    row(2, 4, 0);
    row(2, 4, 1);
    row(2, 4, 2);
    row(1, at4, 0);
    row(1, at5, 0);
    row(1, at6, 0);
    out += "6 0 obj\n" + stream("/Type /XRef /Size 7 /W [1 4 2] /Root 1 0 R", xref, true) + "\nendobj\nstartxref\n" + std::to_string(at6) + "\n%%EOF\n";
    mv::ViewerDocument packed = mv::readPdf(out);
    CHECK(packed.shape == mv::Shape::Pages && packed.pages.pages.size() == 1);
    CHECK(packed.pages.load(0) && runWith(packed.pages.pages[0], "Packed"));
}

TEST(viewer_file_kind_detection)
{
    using namespace viewertest;
    const std::string docx = zip({{"word/document.xml", "<w:document/>"}});
    CHECK(model::detectFileType("a.docx", docx).kind == model::FileKind::Document);
    CHECK(model::detectFileType("a.docx", docx).format == "Word document");
    CHECK(model::detectFileType("b.xlsx", docx).format == "Excel workbook");
    CHECK(model::detectFileType("c.pptx", docx).format == "PowerPoint presentation");
    CHECK(model::detectFileType("d.pdf", "%PDF-1.7\n...").kind == model::FileKind::Document);
    CHECK(model::detectFileType("a.zip", docx).kind == model::FileKind::Binary); // a ZIP by any other name
}

// ENP_VIEWER_SAMPLES=<folder>: every document under it is read (nothing may crash or hang); the
// counts are printed. Not a pass/fail list: real files hold what the readers do not support yet.
TEST(viewer_reads_sample_folder)
{
    const char* folder = std::getenv("ENP_VIEWER_SAMPLES");
    if (!folder || !*folder)
        return;
    std::map<std::string, std::array<int, 2>> counts; // format -> read, failed
    std::vector<std::string> failures;
    std::function<void(const std::string&, int)> walk = [&](const std::string& dir, int depth) {
        if (depth > 12)
            return;
        for (const os::fs::DirEntry& e : os::fs::listDirectory(dir)) {
            const std::string path = dir + "/" + e.name;
            if (e.isDirectory) {
                walk(path, depth + 1);
                continue;
            }
            const std::optional<std::string> bytes = os::fs::readFile(path);
            if (!bytes || bytes->size() > (std::size_t(64) << 20))
                continue;
            const std::string format = model::viewer::viewerFormat(path, *bytes);
            if (format.empty())
                continue;
            model::viewer::ViewerDocument doc = model::viewer::readViewerDocument(path, *bytes);
            for (std::size_t i = 0; i < doc.pages.pages.size() && i < 50; ++i)
                doc.pages.load(i);
            const bool ok = doc.shape != model::viewer::Shape::None;
            ++counts[format][ok ? 0 : 1];
            if (!ok && failures.size() < 40)
                failures.push_back(path + ": " + doc.error);
        }
    };
    walk(folder, 0);
    for (const auto& [format, n] : counts)
        std::printf("  %-24s %5d read, %4d not read\n", format.c_str(), n[0], n[1]);
    for (const std::string& f : failures)
        std::printf("    %s\n", f.c_str());
}
