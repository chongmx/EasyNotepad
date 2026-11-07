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
    std::map<std::string, int> notShown;              // "Word document: text boxes" -> files that have them
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
            for (const std::string& n : doc.notShown) { // "3 charts" -> "charts"
                const std::size_t space = n.find(' ');
                const bool numbered = !n.empty() && n[0] >= '0' && n[0] <= '9' && space != std::string::npos;
                ++notShown[format + ": " + (numbered ? n.substr(space + 1) : n)];
            }
            if (!ok && failures.size() < 40)
                failures.push_back(path + ": " + doc.error);
        }
    };
    walk(folder, 0);
    for (const auto& [format, n] : counts)
        std::printf("  %-24s %5d read, %4d not read\n", format.c_str(), n[0], n[1]);
    for (const auto& [what, files] : notShown)
        std::printf("  not shown in %5d files: %s\n", files, what.c_str());
    for (const std::string& f : failures)
        std::printf("    %s\n", f.c_str());
}

// ---- Reading off the UI thread: the reader service and the view-model (MVVM) ----------------------

namespace viewertest {

// Files kept in memory (read on the reader service's thread: guarded).
class MemoryFiles final : public interfaces::IFileService {
public:
    std::map<std::string, std::string> files;
    std::mutex mutex;
    std::optional<std::string> read(const std::string& path, std::string* error) override
    {
        std::lock_guard<std::mutex> lock(mutex);
        const auto it = files.find(path);
        if (it == files.end()) {
            if (error)
                *error = "no such file";
            return std::nullopt;
        }
        return it->second;
    }
    bool write(const std::string&, std::string_view, std::string*) override { return false; }
    std::vector<interfaces::DirectoryEntry> list(const std::string&) override { return {}; }
    bool isDirectory(const std::string&) override { return false; }
    std::string absolute(const std::string& path) override { return path; }
    bool createDirectories(const std::string&) override { return false; }
};

// Runs main-thread tasks until `done` or about 10 s.
template <class F>
bool pumpUntil(F done)
{
    for (int i = 0; i < 2000 && !done(); ++i) {
        os::dispatch::pump();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    os::dispatch::pump();
    return done();
}

} // namespace viewertest

TEST(viewer_reads_documents_off_the_ui_thread)
{
    using namespace viewertest;
    using State = viewmodel::ViewerViewModel::State;
    MemoryFiles files;
    for (const auto& [name, bytes] : sampleDocuments())
        files.files["/docs/" + name] = bytes;
    services::DocumentReaderService reader(&files);
    int trims = 0;
    {
        viewmodel::ViewerViewModel viewer(&reader, [&trims]() { ++trims; });
        // A Word document: Loading until the service delivers it, on the main thread.
        viewer.open(1, "/docs/sample.docx");
        CHECK(viewer.find(1) && viewer.find(1)->state == State::Loading && viewer.loadingCount() == 1);
        CHECK(pumpUntil([&]() { return viewer.find(1)->state != State::Loading; }));
        CHECK(viewer.find(1)->state == State::Ready && viewer.find(1)->doc.shape == mv::Shape::Flow);
        // Its picture's bytes come out once.
        CHECK(viewer.takeMedia(1, "word/media/image1.png") == tinyPicture());
        CHECK(viewer.takeMedia(1, "word/media/image1.png").empty());

        // A PDF: pages are read when shown, and dropped when far away.
        viewer.open(2, "/docs/sample.pdf");
        CHECK(pumpUntil([&]() { return viewer.find(2)->state != State::Loading; }));
        const viewmodel::ViewerViewModel::Entry* pdf = viewer.find(2);
        CHECK(pdf->state == State::Ready && pdf->doc.pages.pages.size() == 2 && !pdf->doc.pages.pages[0].loaded);
        viewer.showPages(2, 0, 0);
        CHECK(pumpUntil([&]() { return viewer.loadingCount() == 0; }));
        CHECK(pdf->doc.pages.pages[0].loaded && pdf->doc.pages.pages[1].loaded); // the one shown and its neighbour
        CHECK(runWith(pdf->doc.pages.pages[0], "Page one") != nullptr);
        CHECK(!viewer.takeMedia(2, "pdf-image-7").empty()); // the page's picture, encoded (QOI)

        // A file that is not there: Failed, with the reason.
        viewer.open(3, "/docs/missing.pdf");
        CHECK(pumpUntil([&]() { return viewer.find(3)->state != State::Loading; }));
        CHECK(viewer.find(3)->state == State::Failed && !viewer.find(3)->doc.error.empty());

        // Closed while it is read: the late result is dropped, nothing stays anywhere.
        viewer.open(4, "/docs/sample.pptx");
        viewer.release(4);
        CHECK(!viewer.find(4));
        CHECK(pumpUntil([&]() { return reader.pendingTickets() == 0; }));
        CHECK(!viewer.find(4));
        // Opened again: a new generation, read again.
        const std::uint64_t before = viewer.find(1)->generation;
        viewer.release(1);
        viewer.open(1, "/docs/sample.docx");
        CHECK(viewer.find(1)->generation != before);
        CHECK(pumpUntil([&]() { return viewer.find(1)->state == State::Ready; }));

        const int trimsBefore = trims;
        viewer.releaseAll();
        CHECK(viewer.entryCount() == 0 && trims > trimsBefore);
        CHECK(pumpUntil([&]() { return reader.pendingTickets() == 0; }));
        // Destroyed with reads in flight: their results are never delivered to it.
        viewer.open(5, "/docs/sample.xlsx");
    }
    CHECK(pumpUntil([&]() { return reader.pendingTickets() == 0; }));
}

// ---- Charts, text boxes, comments, shapes and SmartArt ---------------------------------------------

namespace viewertest {

// The chart page of a Word document whose only content is chart `xml`.
inline mv::Page wordChart(const std::string& xml)
{
    const std::string bytes = zip({{"_rels/.rels", packageRels("word/document.xml")},
                                   {"word/_rels/document.xml.rels", rels({{"rId6", "chart", "charts/chart1.xml"}})},
                                   {"word/charts/chart1.xml", xml},
                                   {"word/document.xml",
                                    R"(<w:document xmlns:w="w" xmlns:r="r" xmlns:a="a"><w:body><w:p><w:r><w:drawing><wp:inline xmlns:wp="wp"><wp:extent cx="4572000" cy="2743200"/>
<a:graphic><a:graphicData><c:chart xmlns:c="c" r:id="rId6"/></a:graphicData></a:graphic></wp:inline></w:drawing></w:r></w:p></w:body></w:document>)"}});
    mv::ViewerDocument doc = mv::readDocx(bytes);
    return doc.flow.drawings.empty() ? mv::Page() : std::move(doc.flow.drawings.front());
}

inline std::size_t filledWith(const mv::Page& page, std::uint32_t color)
{
    std::size_t n = 0;
    for (const mv::PathItem& p : page.paths)
        n += p.fill == color ? 1 : 0;
    return n;
}

} // namespace viewertest

TEST(viewer_draws_charts_from_their_cached_values)
{
    using namespace viewertest;
    // Office's default theme colours: accent1 and accent2 for the two series.
    const std::uint32_t accent1 = mv::rgba(0x44, 0x72, 0xC4), accent2 = mv::rgba(0xED, 0x7D, 0x31);
    const mv::Page column = wordChart(chartXml());
    CHECK(column.width > 300.0f && column.height > 200.0f); // the drawing's extent: 360 x 216 points
    CHECK(filledWith(column, accent1) >= 3 + 1 && filledWith(column, accent2) >= 3 + 1); // three bars each, and the legend's swatch
    CHECK(column.plainText().find("Sales") != std::string::npos && column.plainText().find("South") != std::string::npos);
    CHECK(column.plainText().find("2026") != std::string::npos); // the legend
    // The tallest bar is the largest value (30, the second series' third): its top is the highest.
    float top1 = 1e9f, top2 = 1e9f;
    for (const mv::PathItem& p : column.paths) {
        if (p.subpaths.empty() || p.subpaths[0].size() != 4)
            continue;
        if (p.fill == accent1)
            top1 = std::min(top1, p.subpaths[0][0].y);
        if (p.fill == accent2)
            top2 = std::min(top2, p.subpaths[0][0].y);
    }
    CHECK(top2 < top1);

    // Every kind draws something, and nothing crashes on any grouping.
    for (const char* kind : {"barChart", "lineChart", "areaChart", "pieChart", "doughnutChart", "radarChart", "scatterChart"})
        for (const char* grouping : {"clustered", "stacked", "percentStacked", "standard"}) {
            const mv::Page page = wordChart(chartXml(kind, grouping));
            CHECK(page.paths.size() >= 3);
        }
    const mv::Page pie = wordChart(chartXml("pieChart", "standard"));
    CHECK(filledWith(pie, accent1) >= 1 && filledWith(pie, accent2) >= 1); // slices take the accents in turn
    CHECK(pie.plainText().find("North") != std::string::npos);              // a pie's legend names its categories
    // Not a chart: nothing drawn, and the document says so.
    const mv::ViewerDocument broken = mv::readDocx(zip({{"_rels/.rels", packageRels("word/document.xml")},
                                                        {"word/_rels/document.xml.rels", rels({{"rId6", "chart", "charts/chart1.xml"}})},
                                                        {"word/charts/chart1.xml", "<c:chartSpace xmlns:c=\"c\"/>"},
                                                        {"word/document.xml", R"(<w:document xmlns:w="w" xmlns:r="r"><w:body><w:p><w:r><w:drawing><c:chart xmlns:c="c" r:id="rId6"/></w:drawing></w:r></w:p></w:body></w:document>)"}}));
    CHECK(broken.flow.drawings.empty() && !broken.notShown.empty());
}

TEST(viewer_shows_text_boxes_comments_shapes_and_smartart)
{
    using namespace viewertest;
    namespace md = model::markdown;
    std::map<std::string, std::string> samples;
    for (auto& [name, bytes] : sampleDocuments())
        samples[name] = bytes;

    const mv::ViewerDocument word = mv::readDocx(samples["sample.docx"]);
    CHECK(word.notShown.empty());
    CHECK(word.flow.drawings.size() == 1);
    bool drawing = false, quoted = false, heading = false, comment = false;
    for (const md::Block& b : word.flow.blocks) {
        drawing = drawing || (b.kind == md::BlockKind::Drawing && b.level == 0);
        quoted = quoted || (b.quote > 0 && md::plainText(b.spans) == "Inside a text box");
        heading = heading || (b.kind == md::BlockKind::Heading && md::plainText(b.spans) == "Comments");
        comment = comment || (b.marker == "c1." && md::plainText(b.spans) == "Ana: Check this.");
    }
    CHECK(drawing && quoted && heading && comment);
    CHECK(mv::plainText(word).find("Reviewed[c1]") != std::string::npos); // the comment's mark where it was made

    const mv::ViewerDocument book = mv::readXlsx(samples["sample.xlsx"]);
    CHECK(book.notShown.empty());
    CHECK(book.shape == mv::Shape::Sheet && book.book.sheets.size() == 2 && book.book.sheets[0].drawings.size() == 2);
    if (book.book.sheets[0].drawings.size() == 2) {
        const mv::SheetDrawing& chart = book.book.sheets[0].drawings[0];
        CHECK(chart.row0 == 5 && chart.col0 == 2 && chart.row1 == 20 && chart.col1 == 8);
        CHECK(chart.page.width > 100.0f && chart.page.height > 100.0f && chart.page.plainText().find("Sales") != std::string::npos);
        const mv::SheetDrawing& arrow = book.book.sheets[0].drawings[1];
        CHECK(std::fabs(arrow.page.width - 150.0f) < 0.01f && std::fabs(arrow.page.height - 60.0f) < 0.01f);
        CHECK(filledWith(arrow.page, mv::rgba(0xFF, 0xC0, 0x00)) == 1 && arrow.page.plainText() == "Look here");
        CHECK(!arrow.page.paths.empty() && arrow.page.paths[0].subpaths[0].size() == 7); // a right arrow's outline
    }
    CHECK(book.book.sheets[0].rowCount >= 21); // the grid reaches the drawings

    const mv::ViewerDocument deck = mv::readPptx(samples["sample.pptx"]);
    CHECK(deck.notShown.empty());
    if (!deck.pages.pages.empty()) {
        const std::string text = deck.pages.pages[0].plainText();
        CHECK(text.find("Sales") != std::string::npos);    // the chart
        CHECK(text.find("Step one") != std::string::npos); // the SmartArt diagram's drawn shape
        CHECK(filledWith(deck.pages.pages[0], mv::rgba(0x70, 0xAD, 0x47)) >= 1);
        // The diagram's shape is placed in its frame: at x = 5,000,000 EMU = 393.7 points.
        bool placed = false;
        for (const mv::TextBox& b : deck.pages.pages[0].boxes)
            if (!b.paragraphs.empty() && !b.paragraphs[0].runs.empty() && b.paragraphs[0].runs[0].text == "Step one")
                placed = std::fabs(b.box.x - (5000000.0f + 100000.0f) / 12700.0f) < 0.1f; // its txXfrm
        CHECK(placed);
    }
}

TEST(viewer_formats_fractions_colours_and_finds_scripts)
{
    using mv::formatNumber;
    CHECK_EQ(formatNumber(3.25, "# ?/?"), std::string("3 1/4"));
    CHECK_EQ(formatNumber(0.5, "# ?/?"), std::string("  1/2"));
    CHECK_EQ(formatNumber(2, "# ?/?"), std::string("2    ")); // a whole number: the fraction's room blank
    CHECK_EQ(formatNumber(-1.75, "# ?\?/?\?"), std::string("-1  3/4 "));
    CHECK_EQ(formatNumber(0.3, "?/4"), std::string("1/4"));
    std::uint32_t color = 0;
    CHECK_EQ(formatNumber(-5, "0;[Red]0", false, &color), std::string("5"));
    CHECK(color == mv::rgba(255, 0, 0));
    formatNumber(5, "0;[Red]0", false, &color);
    CHECK(color == 0);
    formatNumber(5, "[Color3]0", false, &color);
    CHECK(color == mv::rgba(255, 0, 0));

    CHECK(mv::scriptsIn("plain ASCII, caf\xC3\xA9, \xCE\xB1\xCE\xB2") == 0);
    CHECK(mv::scriptsIn("\xE6\x9D\xB1\xE4\xBA\xAC") == mv::ScriptHan);        // 東京
    CHECK(mv::scriptsIn("\xE3\x81\x8B\xE3\x81\xAA") == mv::ScriptHan);        // かな
    CHECK(mv::scriptsIn("\xED\x95\x9C\xEA\xB8\x80") == mv::ScriptHangul);     // 한글
    CHECK(mv::scriptsIn("\xF0\xA0\x80\x80 and \xEA\xB0\x80") == (mv::ScriptHan | mv::ScriptHangul)); // U+20000, 가
    CHECK(mv::scriptsIn("\xE6\x9D") == 0); // cut short: not read past the end
}

TEST(viewer_reads_the_challenge_files)
{
    // tests/data/viewer/make_challenge_files.py writes them: one of each format, each with the
    // features hardest to get right.
    namespace md = model::markdown;
    const std::string dir = std::string(ENP_SOURCE_DIR) + "/tests/data/viewer/";
    auto read = [&](const char* name) {
        const std::optional<std::string> bytes = os::fs::readFile(dir + name);
        CHECK(bytes.has_value());
        return mv::readViewerDocument(name, bytes ? *bytes : std::string());
    };

    // Word: the header above the body and the footer below it, line breaks kept.
    {
        const mv::ViewerDocument doc = read("challenge.docx");
        CHECK(doc.shape == mv::Shape::Flow && doc.flow.blocks.size() > 4);
        if (doc.flow.blocks.size() > 4) {
            const std::vector<md::Block>& b = doc.flow.blocks;
            CHECK(md::plainText(b.front().spans).find("header") != std::string::npos);
            CHECK(b[1].kind == md::BlockKind::Rule);
            bool footer = false; // after the body and its notes, before the comments
            for (std::size_t i = 1; i < b.size(); ++i)
                footer = footer || (b[i - 1].kind == md::BlockKind::Rule && md::plainText(b[i].spans) == "Page 1");
            CHECK(footer);
            CHECK(mv::plainText(doc).find("stop, line\nbreak") != std::string::npos);
        }
        // The floating picture: one inch, at the right margin, level with its paragraph, the text
        // beside it 0.125 inch away.
        CHECK(doc.flow.floats.size() == 1);
        if (doc.flow.floats.size() == 1) {
            const mv::FlowDocument::Float& f = doc.flow.floats[0];
            CHECK(std::fabs(f.width - 72.0f) < 0.01f && std::fabs(f.height - 72.0f) < 0.01f);
            CHECK(f.fromH == 1 && f.alignH == 2 && f.fromV == 2 && f.alignV == -1 && f.wrap == 1 && !f.behind);
            CHECK(std::fabs(f.dist[0] - 9.0f) < 0.01f && f.dist[1] == 0.0f);
            CHECK(f.block < doc.flow.blocks.size() && md::plainText(doc.flow.blocks[f.block].spans).find("floating picture") != std::string::npos);
        }
    }

    // Excel: number format colours and fractions, rich text, sizes, borders, wrapping, rotation,
    // and Chinese characters (the view's fonts).
    {
        const mv::ViewerDocument doc = read("challenge.xlsx");
        CHECK(doc.shape == mv::Shape::Sheet && !doc.book.sheets.empty());
        if (doc.book.sheets.empty())
            return;
        const mv::Workbook& book = doc.book;
        const mv::Sheet& s = book.sheets[0];
        const mv::Cell* negative = s.cell(4, 1);
        // [Red] from the format; the conditional format (below 0: dark red on pink) takes the colour over.
        CHECK(negative && negative->text == "(1,234.50)" && negative->color == mv::rgba(0x9C, 0, 6) && negative->fill == mv::rgba(0xFF, 0xC7, 0xCE));
        CHECK(s.cell(6, 1) && s.cell(6, 1)->fill == 0); // 12.3%: not below 0
        CHECK(s.cell(9, 1) && s.cell(9, 1)->text == "3  1/4 "); // built-in 13, "# ?\?/?\?": padded
        const mv::Cell* rich = s.cell(11, 0);
        CHECK(rich && rich->text == "Rich text" && rich->rich > 0 && rich->rich < book.richTexts.size());
        if (rich && rich->rich > 0 && rich->rich < book.richTexts.size()) {
            const std::vector<mv::CellRun>& runs = book.richTexts[rich->rich];
            CHECK(runs.size() == 2 && !runs[0].own && runs[1].own && runs[1].begin == 5 && runs[1].end == 9 &&
                  (runs[1].flags & mv::Cell::Bold) && runs[1].color == mv::rgba(0xC0, 0, 0));
        }
        CHECK(s.cell(11, 1) && s.cell(11, 1)->text == "\xE6\x9D\xB1\xE4\xBA\xAC" && (doc.scripts & mv::ScriptHan));
        const mv::Cell* header = s.cell(0, 0);
        CHECK(header && header->halfPoints == 28 && header->border > 0 && header->border < book.borders.size());
        if (header && header->border > 0 && header->border < book.borders.size()) {
            const mv::Border& b = book.borders[header->border];
            CHECK(b.style[0] == mv::Border::Thin && b.style[3] == mv::Border::Medium && b.color[3] == mv::rgba(0x2E, 0x75, 0xB6) &&
                  b.color[0] == 0);
        }
        const mv::Cell* total = s.cell(14, 0);
        CHECK(total && total->border > 0 && total->border < book.borders.size() &&
              book.borders[total->border].style[3] == mv::Border::Double);
        CHECK(s.cell(11, 2) && (s.cell(11, 2)->flags & mv::Cell::Wrap) && s.cell(19, 1) && (s.cell(19, 1)->flags & mv::Cell::Wrap));
        CHECK(s.cell(19, 0) && s.cell(19, 0)->rotation == 90);
        // The Looks sheet: fonts by name, pattern and gradient fills, diagonals, indent, shrink to fit.
        CHECK(book.defaultFont == "Calibri" && book.sheets.size() == 5);
        if (book.sheets.size() == 5) {
            // The Data sheet's tables: Excel's default style (the accent's header, banded rows, light
            // lines) and the workbook's own (its dxfs).
            const mv::Sheet& data = book.sheets[1];
            const mv::Cell* head = data.cell(0, 0);
            CHECK(head && head->fill == mv::rgba(0x2E, 0x75, 0xB6) && head->color == mv::rgba(255, 255, 255) && (head->flags & mv::Cell::Bold));
            const mv::Cell* band = data.cell(1, 1);
            CHECK(band && band->fill != 0 && data.cell(2, 1) && data.cell(2, 1)->fill == 0);
            CHECK(band && band->border > 0 && band->border < book.borders.size() && book.borders[band->border].style[3] == mv::Border::Thin);
            const mv::Cell* purple = data.cell(0, 4);
            CHECK(purple && purple->fill == mv::rgba(0x70, 0x30, 0xA0) && (purple->flags & mv::Cell::Bold));
            CHECK(data.cell(1, 4) && data.cell(1, 4)->fill == mv::rgba(0xE4, 0xDF, 0xEC) && data.cell(2, 4) && data.cell(2, 4)->fill == 0);
            CHECK(data.cell(3, 5) && data.cell(3, 5)->fill == mv::rgba(0xE4, 0xDF, 0xEC)); // the third row: banded again
            // The Rules sheet's conditional formats, on the saved values.
            const mv::Sheet& rules = book.sheets[4];
            CHECK(rules.cell(0, 0) && rules.cell(0, 0)->fill == mv::rgba(0xF8, 0x69, 0x6B));  // colour scale: the lowest red
            CHECK(rules.cell(9, 0) && rules.cell(9, 0)->fill == mv::rgba(0x63, 0xBE, 0x7B));  // the highest green
            CHECK(rules.cell(4, 0) && rules.cell(4, 0)->fill != 0 && rules.cell(4, 0)->fill != rules.cell(0, 0)->fill);
            const mv::CellMark* low = rules.mark(0, 1);
            const mv::CellMark* high = rules.mark(9, 1);
            CHECK(low && high && std::fabs(low->bar - 0.1f) < 0.001f && std::fabs(high->bar - 0.9f) < 0.001f && high->barColor == mv::rgba(0x63, 0x8E, 0xC6));
            const mv::CellMark* rising = rules.mark(0, 2);   // 10 of 1..10: the top third
            const mv::CellMark* falling = rules.mark(9, 2); // 1
            CHECK(rising && rising->icon == mv::CellMark::ArrowUp && falling && falling->icon == mv::CellMark::ArrowDown);
            CHECK(rules.cell(5, 3) && rules.cell(5, 3)->fill == mv::rgba(0xFF, 0xC7, 0xCE) && rules.cell(4, 3) && rules.cell(4, 3)->fill == 0); // > 5
            int top = 0;
            for (std::uint32_t r = 0; r < 10; ++r)
                if (const mv::Cell* c = rules.cell(r, 4); c && (c->flags & mv::Cell::Bold) && c->fill == mv::rgba(0xC6, 0xEF, 0xCE))
                    ++top;
            CHECK(top == 3);
            CHECK(rules.cell(1, 5) && (rules.cell(1, 5)->flags & mv::Cell::Italic) && rules.cell(0, 5) && !(rules.cell(0, 5)->flags & mv::Cell::Italic)); // banana, apple
            CHECK(rules.mark(9, 6) && rules.mark(9, 6)->icon == mv::CellMark::Star && rules.mark(9, 6)->level == 2);
            CHECK(rules.mark(0, 7) && rules.mark(0, 7)->icon == mv::CellMark::Quarters && rules.mark(0, 7)->level == 0);
            bool reported = false; // the expression rule
            for (const std::string& n : doc.notShown)
                reported = reported || n.find("conditional format") != std::string::npos;
            CHECK(reported);
            const mv::Sheet& looks = book.sheets[3];
            auto named = [&](std::uint32_t r, std::uint32_t c) {
                const mv::Cell* cell = looks.cell(r, c);
                return cell && cell->font > 0 && cell->font <= book.fontNames.size() ? book.fontNames[cell->font - 1u] : std::string();
            };
            CHECK(named(0, 0) == "Cambria" && named(0, 1) == "Arial" && looks.cell(0, 1) && (looks.cell(0, 1)->flags & mv::Cell::Italic));
            auto style = [&](std::uint32_t r, std::uint32_t c) -> const mv::FillStyle* {
                const mv::Cell* cell = looks.cell(r, c);
                return cell && cell->fillStyle > 0 && cell->fillStyle < book.fills.size() ? &book.fills[cell->fillStyle] : nullptr;
            };
            const mv::FillStyle* grid = style(1, 0);
            CHECK(grid && grid->kind == mv::FillStyle::Hatch && grid->lines == mv::FillStyle::Grid && grid->dark &&
                  grid->color == mv::rgba(255, 0, 0) && looks.cell(1, 0)->fill == mv::rgba(255, 255, 255));
            const mv::FillStyle* down = style(1, 1);
            CHECK(down && down->lines == mv::FillStyle::Down && !down->dark);
            CHECK(looks.cell(1, 2) && looks.cell(1, 2)->fillStyle == 0 && looks.cell(1, 2)->fill == mv::rgba(128, 128, 128)); // medium grey: half
            const mv::FillStyle* linear = style(2, 0);
            CHECK(linear && linear->kind == mv::FillStyle::Linear && linear->degree == 90.0f && linear->stopCount == 2 &&
                  linear->stop[0] == mv::rgba(255, 255, 255) && linear->position[1] == 1.0f);
            CHECK(style(2, 1) && style(2, 1)->kind == mv::FillStyle::Path);
            const mv::Cell* diagonal = looks.cell(3, 0);
            CHECK(diagonal && diagonal->border > 0 && diagonal->border < book.borders.size());
            if (diagonal && diagonal->border > 0 && diagonal->border < book.borders.size()) {
                const mv::Border& b = book.borders[diagonal->border];
                CHECK(b.style[4] == mv::Border::Thin && b.diagonalUp && b.diagonalDown && b.color[4] == mv::rgba(0xC0, 0, 0));
            }
            CHECK(looks.cell(3, 1) && looks.cell(3, 1)->indent == 2);
            CHECK(looks.cell(4, 0) && (looks.cell(4, 0)->flags & mv::Cell::Shrink));
        }
    }

    // PDF: a page's scripts are found when it is read.
    {
        mv::ViewerDocument doc = read("challenge.pdf");
        CHECK(doc.shape == mv::Shape::Pages && !doc.pages.pages.empty());
        if (!doc.pages.pages.empty()) {
            CHECK(doc.pages.load(0));
            CHECK(doc.pages.pages[0].scripts & mv::ScriptHan);
        }
    }
}

TEST(viewer_reads_word_table_alignment_and_cell_lines)
{
    using namespace viewertest;
    namespace md = model::markdown;
    const std::string document = R"(<w:document xmlns:w="w"><w:body>
<w:tbl><w:tblPr><w:jc w:val="center"/></w:tblPr><w:tr><w:tc><w:p><w:r><w:t>one</w:t></w:r></w:p><w:p><w:r><w:t>two</w:t><w:br/><w:t>three</w:t></w:r></w:p></w:tc></w:tr></w:tbl>
<w:tbl><w:tblPr><w:jc w:val="right"/></w:tblPr><w:tr><w:tc><w:tcPr><w:jc w:val="left"/></w:tcPr><w:p><w:r><w:t>r</w:t></w:r></w:p></w:tc></w:tr></w:tbl>
<w:sectPr><w:headerReference w:type="first" r:id="rIdH1" xmlns:r="r"/><w:headerReference w:type="default" r:id="rIdH2" xmlns:r="r"/></w:sectPr></w:body></w:document>)";
    const std::string bytes = zip({{"_rels/.rels", packageRels("word/document.xml")},
                                   {"word/_rels/document.xml.rels", rels({{"rIdH1", "header", "header1.xml"}, {"rIdH2", "header", "header2.xml"}})},
                                   {"word/document.xml", document},
                                   {"word/header1.xml", R"(<w:hdr xmlns:w="w"><w:p><w:r><w:t>first page</w:t></w:r></w:p></w:hdr>)"},
                                   {"word/header2.xml", R"(<w:hdr xmlns:w="w"><w:p><w:r><w:t>every page</w:t></w:r></w:p></w:hdr>)"}});
    const mv::ViewerDocument doc = mv::readDocx(bytes);
    const std::vector<md::Block>& b = doc.flow.blocks;
    CHECK(b.size() == 4);
    if (b.size() != 4)
        return;
    CHECK(md::plainText(b[0].spans) == "every page" && b[1].kind == md::BlockKind::Rule); // the default header
    CHECK(b[2].kind == md::BlockKind::Table && b[2].textAlign == 1 && md::plainText(b[2].rows[0][0]) == "one\ntwo\nthree");
    CHECK(b[3].kind == md::BlockKind::Table && b[3].textAlign == 2); // the cell's own jc is not the table's
}

TEST(viewer_draws_slides_as_office_does)
{
    // What a side-by-side comparison with PowerPoint's own render found (docs/plan/25).
    using namespace viewertest;
    const std::string presentation = R"(<p:presentation xmlns:p="p" xmlns:r="r"><p:sldIdLst><p:sldId id="256" r:id="rId2"/></p:sldIdLst>
<p:sldSz cx="9144000" cy="6858000"/></p:presentation>)";
    // The theme's third background fill style: a radial gradient in the referenced colour (phClr).
    const std::string theme = R"(<a:theme xmlns:a="a"><a:themeElements><a:clrScheme name="t"><a:dk1><a:srgbClr val="000000"/></a:dk1>
<a:lt1><a:srgbClr val="FFFFFF"/></a:lt1><a:lt2><a:srgbClr val="EEECE1"/></a:lt2><a:accent1><a:srgbClr val="4F81BD"/></a:accent1></a:clrScheme>
<a:fontScheme name="s"><a:majorFont><a:latin typeface="Georgia"/></a:majorFont><a:minorFont><a:latin typeface="Calibri"/></a:minorFont></a:fontScheme>
<a:fmtScheme name="f"><a:fillStyleLst><a:solidFill><a:schemeClr val="phClr"/></a:solidFill></a:fillStyleLst>
<a:bgFillStyleLst><a:solidFill><a:schemeClr val="phClr"/></a:solidFill><a:solidFill><a:schemeClr val="phClr"/></a:solidFill>
<a:gradFill><a:gsLst><a:gs pos="0"><a:schemeClr val="phClr"><a:tint val="80000"/></a:schemeClr></a:gs><a:gs pos="100000"><a:schemeClr val="phClr"><a:shade val="30000"/></a:schemeClr></a:gs></a:gsLst>
<a:path path="circle"><a:fillToRect l="50000" t="50000" r="50000" b="50000"/></a:path></a:gradFill></a:bgFillStyleLst></a:fmtScheme></a:themeElements></a:theme>)";
    // The master numbers its date, footer and slide number 2-4, the slide number right-aligned.
    const std::string master = R"(<p:sldMaster xmlns:p="p" xmlns:a="a"><p:cSld><p:spTree>
<p:sp><p:nvSpPr><p:nvPr><p:ph type="body" idx="1"/></p:nvPr></p:nvSpPr><p:spPr><a:xfrm><a:off x="0" y="0"/><a:ext cx="100" cy="100"/></a:xfrm></p:spPr></p:sp>
<p:sp><p:nvSpPr><p:nvPr><p:ph type="ftr" idx="3"/></p:nvPr></p:nvSpPr><p:spPr/><p:txBody><a:lstStyle><a:lvl1pPr algn="ctr"/></a:lstStyle><a:p/></p:txBody></p:sp>
<p:sp><p:nvSpPr><p:nvPr><p:ph type="sldNum" idx="4"/></p:nvPr></p:nvSpPr><p:spPr/><p:txBody><a:lstStyle><a:lvl1pPr algn="r"/></a:lstStyle><a:p/></p:txBody></p:sp>
</p:spTree></p:cSld><p:clrMap bg1="lt1" tx1="dk1" bg2="lt2" tx2="dk2"/>
<p:txStyles><p:bodyStyle><a:lvl1pPr algn="l"><a:defRPr sz="2000"><a:solidFill><a:schemeClr val="tx1"/></a:solidFill></a:defRPr></a:lvl1pPr></p:bodyStyle>
<p:otherStyle><a:lvl1pPr><a:defRPr sz="1800"><a:solidFill><a:schemeClr val="tx1"/></a:solidFill></a:defRPr></a:lvl1pPr></p:otherStyle></p:txStyles></p:sldMaster>)";
    const std::string layout = R"(<p:sldLayout xmlns:p="p" xmlns:a="a"><p:cSld><p:bg><p:bgRef idx="1003"><a:schemeClr val="bg2"/></p:bgRef></p:bg><p:spTree>
<p:sp><p:nvSpPr><p:nvPr><p:ph type="body" idx="3"/></p:nvPr></p:nvSpPr><p:spPr><a:xfrm><a:off x="0" y="0"/><a:ext cx="4000000" cy="600000"/></a:xfrm></p:spPr>
<p:txBody><a:lstStyle><a:lvl1pPr><a:defRPr cap="all"><a:latin typeface="+mj-lt"/></a:defRPr></a:lvl1pPr></a:lstStyle><a:p/></p:txBody></p:sp>
<p:sp><p:nvSpPr><p:nvPr><p:ph idx="4"/></p:nvPr></p:nvSpPr><p:spPr><a:xfrm><a:off x="0" y="700000"/><a:ext cx="4000000" cy="3000000"/></a:xfrm></p:spPr>
<p:txBody><a:lstStyle><a:lvl1pPr><a:defRPr><a:solidFill><a:schemeClr val="tx1"><a:tint val="75000"/></a:schemeClr></a:solidFill></a:defRPr></a:lvl1pPr></a:lstStyle><a:p/></p:txBody></p:sp>
</p:spTree></p:cSld></p:sldLayout>)";
    const std::string slide = R"(<p:sld xmlns:p="p" xmlns:a="a" xmlns:r="r"><p:cSld><p:spTree>
<p:sp><p:nvSpPr><p:nvPr><p:ph type="body" idx="3"/></p:nvPr></p:nvSpPr><p:spPr/><p:txBody><a:bodyPr/><a:p><a:r><a:t>Section title</a:t></a:r></a:p></p:txBody></p:sp>
<p:sp><p:nvSpPr><p:nvPr><p:ph idx="4"/></p:nvPr></p:nvSpPr><p:spPr/><p:txBody><a:bodyPr/><a:p><a:r><a:t>Grey subtitle</a:t></a:r></a:p>
<a:p><a:r><a:rPr i="1"/><a:t>Slanted</a:t></a:r><a:r><a:rPr><a:hlinkClick r:id="rId9"/></a:rPr><a:t>link</a:t></a:r>
<a:r><a:rPr><a:latin typeface="Arial"/></a:rPr><a:t>In Arial</a:t></a:r></a:p></p:txBody></p:sp>
<p:sp><p:nvSpPr><p:nvPr/></p:nvSpPr><p:spPr><a:xfrm><a:off x="5000000" y="0"/><a:ext cx="2000000" cy="1000000"/></a:xfrm><a:prstGeom prst="rect"/></p:spPr>
<p:style><a:lnRef idx="0"><a:schemeClr val="accent1"/></a:lnRef><a:fillRef idx="1"><a:schemeClr val="accent1"/></a:fillRef><a:effectRef idx="0"><a:schemeClr val="accent1"/></a:effectRef>
<a:fontRef idx="minor"><a:schemeClr val="lt1"/></a:fontRef></p:style><p:txBody><a:bodyPr/><a:p><a:r><a:t>White text</a:t></a:r></a:p></p:txBody></p:sp>
<p:cxnSp><p:nvCxnSpPr/><p:spPr><a:xfrm><a:off x="0" y="5000000"/><a:ext cx="2540000" cy="1270000"/></a:xfrm><a:prstGeom prst="bentConnector3"/>
<a:ln><a:solidFill><a:srgbClr val="000000"/></a:solidFill></a:ln></p:spPr></p:cxnSp>
<p:graphicFrame><p:nvGraphicFramePr/><p:xfrm><a:off x="0" y="3000000"/><a:ext cx="2540000" cy="762000"/></p:xfrm><a:graphic><a:graphicData><a:tbl>
<a:tblPr firstRow="1"><a:tableStyleId>{LINES}</a:tableStyleId></a:tblPr><a:tblGrid><a:gridCol w="1270000"/><a:gridCol w="1270000"/></a:tblGrid>
<a:tr h="381000"><a:tc><a:txBody><a:bodyPr/><a:p><a:r><a:t>a</a:t></a:r></a:p></a:txBody><a:tcPr/></a:tc><a:tc><a:txBody><a:bodyPr/><a:p><a:r><a:t>b</a:t></a:r></a:p></a:txBody><a:tcPr/></a:tc></a:tr>
<a:tr h="381000"><a:tc><a:txBody><a:bodyPr/><a:p><a:r><a:t>c</a:t></a:r></a:p></a:txBody>
<a:tcPr><a:lnB w="38100"><a:solidFill><a:srgbClr val="FF0000"/></a:solidFill></a:lnB></a:tcPr></a:tc>
<a:tc><a:txBody><a:bodyPr/><a:p><a:r><a:t>d</a:t></a:r></a:p></a:txBody><a:tcPr/></a:tc></a:tr></a:tbl></a:graphicData></a:graphic></p:graphicFrame>
</p:spTree></p:cSld></p:sld>)";
    // A table style with a 2-point green line under its first row and none elsewhere.
    const std::string tableStyles = R"(<a:tblStyleLst xmlns:a="a"><a:tblStyle styleId="{LINES}" styleName="Lines">
<a:firstRow><a:tcStyle><a:tcBdr><a:bottom><a:ln w="25400"><a:solidFill><a:srgbClr val="00B050"/></a:solidFill></a:ln></a:bottom></a:tcBdr></a:tcStyle></a:firstRow>
</a:tblStyle></a:tblStyleLst>)";
    const std::string bytes = zip({{"_rels/.rels", packageRels("ppt/presentation.xml")},
                                   {"ppt/_rels/presentation.xml.rels", rels({{"rId2", "slide", "slides/slide1.xml"}, {"rId3", "tableStyles", "tableStyles.xml"}})},
                                   {"ppt/tableStyles.xml", tableStyles},
                                   {"ppt/presentation.xml", presentation},
                                   {"ppt/slides/_rels/slide1.xml.rels",
                                    rels({{"rId1", "slideLayout", "../slideLayouts/slideLayout1.xml"}, {"rId9", "hyperlink", "https://example.com/"}})},
                                   {"ppt/slides/slide1.xml", slide},
                                   {"ppt/slideLayouts/_rels/slideLayout1.xml.rels", rels({{"rId1", "slideMaster", "../slideMasters/slideMaster1.xml"}})},
                                   {"ppt/slideLayouts/slideLayout1.xml", layout},
                                   {"ppt/slideMasters/_rels/slideMaster1.xml.rels", rels({{"rId1", "theme", "../theme/theme1.xml"}})},
                                   {"ppt/slideMasters/slideMaster1.xml", master},
                                   {"ppt/theme/theme1.xml", theme}});
    const mv::ViewerDocument doc = mv::readViewerDocument("deck.pptx", bytes);
    CHECK(doc.shape == mv::Shape::Pages && doc.pages.pages.size() == 1);
    if (doc.pages.pages.size() != 1)
        return;
    const mv::Page& p = doc.pages.pages[0];
    // bgRef 1003: the theme's radial background, centred, in bg2 lightened and darkened.
    CHECK(p.backgroundGradient.radial && p.backgroundGradient.stops.size() == 2 && std::fabs(p.backgroundGradient.cx - 0.5f) < 0.01f &&
          std::fabs(p.backgroundGradient.cy - 0.5f) < 0.01f);
    auto find = [&](const char* text) -> const mv::TextRun* {
        for (const mv::TextBox& b : p.boxes)
            for (const mv::TextParagraph& para : b.paragraphs)
                for (const mv::TextRun& r : para.runs)
                    if (r.text == text)
                        return &r;
        return nullptr;
    };
    auto align = [&](const char* text) {
        for (const mv::TextBox& b : p.boxes)
            for (const mv::TextParagraph& para : b.paragraphs)
                if (!para.runs.empty() && para.runs[0].text == text)
                    return para.align;
        return -1;
    };
    // Placeholders idx 3 and 4 inherit from the layout's, not the master's footer and slide number.
    CHECK(align("SECTION TITLE") == 0 && align("Grey subtitle") == 0);
    CHECK(find("SECTION TITLE") != nullptr); // cap="all"
    // tx1 with a 75% tint, in linear light: #898989 (HSL would give #404040).
    const mv::TextRun* grey = find("Grey subtitle");
    CHECK(grey && std::abs(int(grey->color & 0xFF) - 0x89) <= 2 && std::abs(int((grey->color >> 8) & 0xFF) - 0x89) <= 2);
    const mv::TextRun* slanted = find("Slanted");
    CHECK(slanted && (slanted->flags & mv::Italic));
    // Typefaces: the layout's +mj-lt (the theme's major font), the theme's minor font when none is
    // named, a run's own.
    CHECK(find("SECTION TITLE") && find("SECTION TITLE")->font == "Georgia");
    CHECK(grey && grey->font == "Calibri");
    CHECK(find("In Arial") && find("In Arial")->font == "Arial");
    const mv::TextRun* link = find("link");
    CHECK(link && link->color == mv::rgba(0x05, 0x63, 0xC1) && (link->flags & mv::Underline)); // the theme's hlink
    const mv::TextRun* white = find("White text");
    CHECK(white && white->color == mv::rgba(255, 255, 255)); // fontRef over the master's otherStyle
    bool elbow = false;
    for (const mv::PathItem& path : p.paths)
        if (path.stroke && path.subpaths.size() == 1 && path.subpaths[0].size() == 4)
            elbow = std::fabs(path.subpaths[0][1].x - path.subpaths[0][2].x) < 0.01f; // across, down, across
    CHECK(elbow);
    // Table lines: the style's under its first row (both cells), the cell's own under "c"; no others.
    int green = 0, red = 0, other = 0;
    for (const mv::PathItem& path : p.paths) {
        if (!path.stroke || path.fill || path.subpaths.size() != 1 || path.subpaths[0].size() != 2 || path.subpaths[0][0].y < 230.0f)
            continue;
        if (path.stroke == mv::rgba(0, 0xB0, 0x50) && std::fabs(path.lineWidth - 2.0f) < 0.01f)
            ++green;
        else if (path.stroke == mv::rgba(255, 0, 0) && std::fabs(path.lineWidth - 3.0f) < 0.01f)
            ++red;
        else
            ++other;
    }
    CHECK(green == 2 && red == 1 && other == 0);
}

TEST(viewer_numbers_word_headings_through_their_styles)
{
    using namespace viewertest;
    namespace md = model::markdown;
    // Heading styles that name only the list: the level is the one whose pStyle names the style.
    const std::string styles = R"(<w:styles xmlns:w="w">
<w:style w:type="paragraph" w:styleId="App1"><w:name w:val="heading 1"/><w:pPr><w:numPr><w:numId w:val="7"/></w:numPr></w:pPr></w:style>
<w:style w:type="paragraph" w:styleId="App2"><w:name w:val="heading 2"/><w:pPr><w:numPr><w:numId w:val="7"/></w:numPr></w:pPr></w:style></w:styles>)";
    const std::string numbering = R"(<w:numbering xmlns:w="w"><w:abstractNum w:abstractNumId="3">
<w:lvl w:ilvl="0"><w:start w:val="1"/><w:numFmt w:val="upperLetter"/><w:pStyle w:val="App1"/><w:lvlText w:val="%1."/></w:lvl>
<w:lvl w:ilvl="1"><w:start w:val="1"/><w:numFmt w:val="decimal"/><w:pStyle w:val="App2"/><w:lvlText w:val="%1.%2"/></w:lvl></w:abstractNum>
<w:num w:numId="7"><w:abstractNumId w:val="3"/></w:num></w:numbering>)";
    const std::string document = R"(<w:document xmlns:w="w"><w:body>
<w:p><w:pPr><w:pStyle w:val="App1"/></w:pPr><w:r><w:t>Help</w:t></w:r></w:p>
<w:p><w:pPr><w:pStyle w:val="App2"/></w:pPr><w:r><w:t>Introduction</w:t></w:r></w:p>
<w:p/>
<w:p><w:pPr><w:pStyle w:val="App2"/></w:pPr><w:r><w:t>Directives</w:t></w:r></w:p></w:body></w:document>)";
    const std::string bytes = zip({{"_rels/.rels", packageRels("word/document.xml")},
                                   {"word/_rels/document.xml.rels", rels({{"rId1", "styles", "styles.xml"}, {"rId2", "numbering", "numbering.xml"}})},
                                   {"word/document.xml", document},
                                   {"word/styles.xml", styles},
                                   {"word/numbering.xml", numbering}});
    const mv::ViewerDocument doc = mv::readDocx(bytes);
    const std::vector<md::Block>& b = doc.flow.blocks;
    CHECK(b.size() == 4);
    if (b.size() != 4)
        return;
    CHECK_EQ(md::plainText(b[0].spans), std::string("A. Help"));
    CHECK_EQ(md::plainText(b[1].spans), std::string("A.1 Introduction"));
    CHECK(b[2].kind == md::BlockKind::Paragraph && md::plainText(b[2].spans) == " "); // an empty paragraph keeps its line
    CHECK_EQ(md::plainText(b[3].spans), std::string("A.2 Directives"));
}

TEST(viewer_reads_word_run_fonts)
{
    using namespace viewertest;
    namespace md = model::markdown;
    // docDefaults name the theme's body font; a run names its own; a document naming none is in
    // Times New Roman, as Word sets it.
    const std::string theme = R"(<a:theme xmlns:a="a"><a:themeElements><a:fontScheme name="f"><a:majorFont><a:latin typeface="Cambria"/></a:majorFont>
<a:minorFont><a:latin typeface="Calibri"/></a:minorFont></a:fontScheme></a:themeElements></a:theme>)";
    const std::string styles = R"(<w:styles xmlns:w="w"><w:docDefaults><w:rPrDefault><w:rPr><w:rFonts w:asciiTheme="minorHAnsi" w:hAnsiTheme="minorHAnsi"/></w:rPr></w:rPrDefault></w:docDefaults>
<w:style w:type="paragraph" w:styleId="Heading1"><w:name w:val="heading 1"/><w:rPr><w:rFonts w:asciiTheme="majorHAnsi"/></w:rPr></w:style></w:styles>)";
    const std::string document = R"(<w:document xmlns:w="w"><w:body>
<w:p><w:pPr><w:pStyle w:val="Heading1"/></w:pPr><w:r><w:t>Heading</w:t></w:r></w:p>
<w:p><w:r><w:t xml:space="preserve">Body </w:t></w:r><w:r><w:rPr><w:rFonts w:ascii="Courier New" w:hAnsi="Courier New"/></w:rPr><w:t>code</w:t></w:r></w:p></w:body></w:document>)";
    const std::string bytes = zip({{"_rels/.rels", packageRels("word/document.xml")},
                                   {"word/_rels/document.xml.rels", rels({{"rId1", "styles", "styles.xml"}, {"rId2", "theme", "theme/theme1.xml"}})},
                                   {"word/document.xml", document},
                                   {"word/styles.xml", styles},
                                   {"word/theme/theme1.xml", theme}});
    const mv::ViewerDocument doc = mv::readDocx(bytes);
    const std::vector<md::Block>& b = doc.flow.blocks;
    const std::vector<std::string>& fonts = doc.flow.fonts;
    CHECK(b.size() == 2);
    if (b.size() != 2 || b[1].spans.size() != 2)
        return;
    auto family = [&](const md::Span& s) { return s.font > 0 && s.font <= fonts.size() ? fonts[s.font - 1] : std::string(); };
    CHECK_EQ(family(b[0].spans[0]), std::string("Cambria"));     // the heading style: the theme's major font
    CHECK_EQ(family(b[1].spans[0]), std::string("Calibri"));     // docDefaults: the theme's minor font
    CHECK_EQ(family(b[1].spans[1]), std::string("Courier New")); // the run's own
    CHECK(!(b[1].spans[1].style & md::Code));                    // a fixed-pitch font is not code
    // No fonts named anywhere: Word's own default.
    const mv::ViewerDocument plain = mv::readDocx(zip({{"_rels/.rels", packageRels("word/document.xml")},
                                                       {"word/document.xml", R"(<w:document xmlns:w="w"><w:body><w:p><w:r><w:t>x</w:t></w:r></w:p></w:body></w:document>)"}}));
    CHECK(plain.flow.fonts.size() == 1 && plain.flow.fonts[0] == "Times New Roman");
}

TEST(viewer_reads_word_layout)
{
    // Tab stops, Word's spacing, list indents, page breaks and tables with a table style.
    using namespace viewertest;
    namespace md = model::markdown;
    const std::string styles = R"(<w:styles xmlns:w="w">
<w:docDefaults><w:pPrDefault><w:pPr><w:spacing w:after="160" w:line="259" w:lineRule="auto"/></w:pPr></w:pPrDefault></w:docDefaults>
<w:style w:type="paragraph" w:styleId="List"><w:name w:val="List Paragraph"/><w:pPr><w:contextualSpacing/></w:pPr></w:style>
<w:style w:type="table" w:styleId="Grid"><w:tblPr><w:tblBorders><w:top w:val="single" w:sz="8" w:color="FF0000"/><w:insideH w:val="single" w:sz="4"/></w:tblBorders></w:tblPr>
<w:tblStylePr w:type="firstRow"><w:rPr><w:b/><w:color w:val="FFFFFF"/></w:rPr><w:tcPr><w:shd w:val="clear" w:fill="4472C4"/></w:tcPr></w:tblStylePr>
<w:tblStylePr w:type="band1Horz"><w:tcPr><w:shd w:val="clear" w:fill="D9E2F3"/></w:tcPr></w:tblStylePr></w:style></w:styles>)";
    const std::string numbering = R"(<w:numbering xmlns:w="w"><w:abstractNum w:abstractNumId="0"><w:lvl w:ilvl="0"><w:numFmt w:val="decimal"/><w:lvlText w:val="%1."/>
<w:pPr><w:ind w:left="720" w:hanging="360"/></w:pPr></w:lvl></w:abstractNum><w:num w:numId="1"><w:abstractNumId w:val="0"/></w:num></w:numbering>)";
    const std::string document = R"(<w:document xmlns:w="w"><w:body>
<w:p><w:pPr><w:tabs><w:tab w:val="right" w:leader="dot" w:pos="9000"/></w:tabs><w:spacing w:before="240" w:line="360" w:lineRule="exact"/></w:pPr>
<w:r><w:t>Chapter</w:t></w:r><w:r><w:tab/></w:r><w:r><w:t>7</w:t></w:r></w:p>
<w:p><w:pPr><w:pStyle w:val="List"/><w:numPr><w:ilvl w:val="0"/><w:numId w:val="1"/></w:numPr></w:pPr><w:r><w:t>item</w:t></w:r></w:p>
<w:p><w:r><w:br w:type="page"/></w:r><w:r><w:t>New page</w:t></w:r></w:p>
<w:tbl><w:tblPr><w:tblStyle w:val="Grid"/><w:tblLook w:val="04A0"/></w:tblPr><w:tblGrid><w:gridCol w:w="2000"/><w:gridCol w:w="4000"/></w:tblGrid>
<w:tr><w:tc><w:p><w:r><w:t>H1</w:t></w:r></w:p></w:tc><w:tc><w:p><w:r><w:t>H2</w:t></w:r></w:p></w:tc></w:tr>
<w:tr><w:tc><w:tcPr><w:gridSpan w:val="2"/></w:tcPr><w:p><w:pPr><w:jc w:val="center"/></w:pPr><w:r><w:t>wide</w:t></w:r></w:p></w:tc></w:tr>
<w:tr><w:tc><w:tcPr><w:vMerge w:val="restart"/><w:shd w:val="clear" w:fill="FFFF00"/></w:tcPr><w:p><w:r><w:t>a</w:t></w:r></w:p></w:tc><w:tc><w:p><w:r><w:t>b</w:t></w:r></w:p></w:tc></w:tr>
<w:tr><w:tc><w:tcPr><w:vMerge/></w:tcPr><w:p/></w:tc><w:tc><w:p><w:r><w:t>c</w:t></w:r></w:p></w:tc></w:tr></w:tbl>
</w:body></w:document>)";
    const std::string bytes = zip({{"_rels/.rels", packageRels("word/document.xml")},
                                   {"word/_rels/document.xml.rels", rels({{"rId1", "styles", "styles.xml"}, {"rId2", "numbering", "numbering.xml"}})},
                                   {"word/document.xml", document},
                                   {"word/styles.xml", styles},
                                   {"word/numbering.xml", numbering}});
    const mv::ViewerDocument doc = mv::readDocx(bytes);
    const std::vector<md::Block>& b = doc.flow.blocks;
    CHECK(b.size() == 4);
    if (b.size() != 4)
        return;
    // The tab stop, the paragraph's spacing (its own before and exact line, the defaults' after).
    CHECK(b[0].tabs.size() == 1 && b[0].tabs[0].kind == 2 && b[0].tabs[0].leader == '.' && std::fabs(b[0].tabs[0].pos - 450.0f) < 0.01f);
    CHECK(md::plainText(b[0].spans) == "Chapter\t7");
    CHECK(b[0].documentSpacing && std::fabs(b[0].spaceBefore - 12.0f) < 0.01f && std::fabs(b[0].spaceAfter - 8.0f) < 0.01f);
    CHECK(std::fabs(b[0].lineExact - 18.0f) < 0.01f && !b[0].lineAtLeast);
    // The list item: its level's indents, contextual spacing from its style.
    CHECK(b[1].kind == md::BlockKind::Numbered && std::fabs(b[1].indentLeft - 36.0f) < 0.01f && std::fabs(b[1].firstLine + 18.0f) < 0.01f);
    CHECK(b[1].contextual && std::fabs(b[1].lineMultiple - 259.0f / 240.0f) < 0.001f);
    // A page break at the start of a paragraph.
    CHECK(b[2].pageBreakBefore && !b[1].pageBreakBefore);
    // The table: its grid, the style's header row and bands, spans, merges, the cell's own shading.
    const md::Block& t = b[3];
    CHECK(t.kind == md::BlockKind::Table && t.columns.size() == 2 && std::fabs(t.columns[1] - 200.0f) < 0.01f);
    CHECK(t.looks.size() == 4 && t.looks[0].size() == 2);
    if (t.looks.size() != 4)
        return;
    const md::CellLook& head = t.looks[0][0];
    CHECK(head.fill == mv::rgba(0x44, 0x72, 0xC4) && head.bold && head.color == mv::rgba(255, 255, 255));
    CHECK(std::fabs(head.edges[1].width - 1.0f) < 0.01f && head.edges[1].color == mv::rgba(255, 0, 0)); // the table's top line
    CHECK(t.looks[1][0].span == 2 && t.looks[1][1].covered && t.looks[1][0].align == 1);
    CHECK(t.looks[1][0].fill == mv::rgba(0xD9, 0xE2, 0xF3));                                   // band 1
    CHECK(t.looks[2][0].fill == mv::rgba(255, 255, 0) && !t.looks[2][0].merged && t.looks[3][0].merged); // own shading; vMerge
    CHECK(t.looks[2][0].edges[3].width > 0.0f);                                                // insideH between rows
}
