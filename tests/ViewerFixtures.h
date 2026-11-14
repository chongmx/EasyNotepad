// Builders of test documents for the viewer's readers (plan 25): ZIP archives, OOXML packages and
// PDF files written byte by byte, plus one small sample of each format. Used by the unit tests
// (ViewerTests.inc) and the UI test (UiIdTest.cpp).
#pragma once

#include "enp/model/viewer/Viewer.h"

#include <array>
#include <cstdio>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace viewertest {





inline std::string le16(unsigned v)
{
    return std::string{char(v & 0xFF), char((v >> 8) & 0xFF)};
}

inline std::string le32(unsigned v)
{
    return le16(v & 0xFFFF) + le16(v >> 16);
}

// Raw DEFLATE made of stored blocks: valid input for any inflater.
inline std::string deflateStored(std::string_view data)
{
    std::string out;
    std::size_t at = 0;
    do {
        const std::size_t n = std::min<std::size_t>(65535, data.size() - at);
        const bool last = at + n >= data.size();
        out += char(last ? 1 : 0);
        out += le16(unsigned(n));
        out += le16(unsigned(~n & 0xFFFF));
        out.append(data.substr(at, n));
        at += n;
    } while (at < data.size());
    return out;
}

// zlib-wrapped (PDF FlateDecode): header, stored blocks, Adler-32.
inline std::string zlibStored(std::string_view data)
{
    unsigned a = 1, b = 0;
    for (unsigned char c : data) {
        a = (a + c) % 65521;
        b = (b + a) % 65521;
    }
    const unsigned adler = (b << 16) | a;
    std::string out = "\x78\x01";
    out += deflateStored(data);
    for (int s = 24; s >= 0; s -= 8)
        out += char((adler >> s) & 0xFF);
    return out;
}

// A ZIP archive of `files` (name, content); deflated (stored blocks) when `deflate`.
inline std::string zip(const std::vector<std::pair<std::string, std::string>>& files, bool deflate = true)
{
    std::string out, dir;
    for (const auto& [name, content] : files) {
        const std::string packed = deflate ? deflateStored(content) : content;
        const unsigned offset = unsigned(out.size());
        out += "PK\x03\x04";
        out += le16(20) + le16(0) + le16(deflate ? 8 : 0) + le16(0) + le16(0) + le32(0);
        out += le32(unsigned(packed.size())) + le32(unsigned(content.size()));
        out += le16(unsigned(name.size())) + le16(0) + name + packed;
        dir += "PK\x01\x02";
        dir += le16(20) + le16(20) + le16(0) + le16(deflate ? 8 : 0) + le16(0) + le16(0) + le32(0);
        dir += le32(unsigned(packed.size())) + le32(unsigned(content.size()));
        dir += le16(unsigned(name.size())) + le16(0) + le16(0) + le16(0) + le16(0) + le32(0) + le32(offset) + name;
    }
    const unsigned dirOffset = unsigned(out.size());
    out += dir;
    out += "PK\x05\x06";
    out += le16(0) + le16(0) + le16(unsigned(files.size())) + le16(unsigned(files.size()));
    out += le32(unsigned(dir.size())) + le32(dirOffset) + le16(0);
    return out;
}

inline const char* const kRels = R"(<?xml version="1.0" encoding="UTF-8"?>
<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">
<Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument" Target="%s"/>
</Relationships>)";

inline std::string packageRels(const std::string& main)
{
    std::string s = kRels;
    s.replace(s.find("%s"), 2, main);
    return s;
}

inline std::string rels(const std::vector<std::array<std::string, 3>>& items) // id, type, target (type "hyperlink" = external)
{
    std::string s = "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">";
    for (const auto& [id, type, target] : items)
        s += "<Relationship Id=\"" + id + "\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/" + type +
             "\" Target=\"" + target + "\"" + (type == "hyperlink" ? " TargetMode=\"External\"" : "") + "/>";
    return s + "</Relationships>";
}

// A PDF from object bodies ("<< ... >>" or "<< ... >>\nstream\n...\nendstream"), numbered from 1,
// with a classic cross-reference table.
inline std::string pdf(const std::vector<std::string>& objects, const std::string& trailerExtra = "", bool breakXref = false)
{
    std::string out = "%PDF-1.4\n%\xE2\xE3\xCF\xD3\n";
    std::vector<std::size_t> offsets;
    for (std::size_t i = 0; i < objects.size(); ++i) {
        offsets.push_back(out.size());
        out += std::to_string(i + 1) + " 0 obj\n" + objects[i] + "\nendobj\n";
    }
    const std::size_t xref = out.size();
    out += "xref\n0 " + std::to_string(objects.size() + 1) + "\n0000000000 65535 f \n";
    for (std::size_t o : offsets) {
        char line[32];
        std::snprintf(line, sizeof(line), "%010zu 00000 n \n", breakXref ? o + 7 : o);
        out += line;
    }
    out += "trailer\n<< /Size " + std::to_string(objects.size() + 1) + " /Root 1 0 R " + trailerExtra + ">>\nstartxref\n" +
           std::to_string(breakXref ? xref + 3 : xref) + "\n%%EOF\n";
    return out;
}

inline std::string stream(const std::string& dict, const std::string& data, bool flate)
{
    const std::string body = flate ? zlibStored(data) : data;
    return "<< " + dict + (flate ? " /Filter /FlateDecode" : "") + " /Length " + std::to_string(body.size()) + " >>\nstream\n" + body +
           "\nendstream";
}

inline std::string onePagePdf(const std::string& content, const std::string& fontDict = "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>",
                       const std::vector<std::string>& extra = {})
{
    std::vector<std::string> objects = {
        "<< /Type /Catalog /Pages 2 0 R >>",
        "<< /Type /Pages /Kids [3 0 R] /Count 1 /MediaBox [0 0 200 100] >>",
        "<< /Type /Page /Parent 2 0 R /Resources << /Font << /F1 4 0 R >> >> /Contents 5 0 R >>",
        fontDict,
        stream("", content, true),
    };
    for (const std::string& e : extra)
        objects.push_back(e);
    return pdf(objects);
}

inline const enp::model::viewer::GlyphRun* runWith(const enp::model::viewer::Page& page, const std::string& text)
{
    for (const enp::model::viewer::GlyphRun& g : page.glyphs)
        if (g.text.find(text) != std::string::npos)
            return &g;
    return nullptr;
}


// A 2x2 picture (PPM: the image decoder reads it whatever the part's name says).
inline std::string tinyPicture()
{
    return std::string("P6\n2 2\n255\n") + std::string("\xFF\x00\x00\x00\xFF\x00\x00\x00\xFF\xFF\xFF\xFF", 12);
}

// A chart part: `kind` is barChart (with barDir col), lineChart, pieChart, ...; two series of three
// categories, cached values only (as Office writes them).
inline std::string chartXml(const std::string& kind = "barChart", const std::string& grouping = "clustered")
{
    auto series = [](int idx, const char* name, const char* values) {
        return std::string("<c:ser><c:idx val=\"") + std::to_string(idx) + "\"/><c:tx><c:strRef><c:strCache><c:pt idx=\"0\"><c:v>" + name +
               "</c:v></c:pt></c:strCache></c:strRef></c:tx><c:cat><c:strRef><c:strCache><c:ptCount val=\"3\"/>"
               "<c:pt idx=\"0\"><c:v>North</c:v></c:pt><c:pt idx=\"1\"><c:v>South</c:v></c:pt><c:pt idx=\"2\"><c:v>West</c:v></c:pt>"
               "</c:strCache></c:strRef></c:cat><c:val><c:numRef><c:numCache><c:ptCount val=\"3\"/>" + values + "</c:numCache></c:numRef></c:val></c:ser>";
    };
    return "<c:chartSpace xmlns:c=\"c\" xmlns:a=\"a\"><c:chart><c:title><c:tx><c:rich><a:p><a:r><a:t>Sales</a:t></a:r></a:p></c:rich></c:tx></c:title>"
           "<c:plotArea><c:" + kind + ">" + (kind == "barChart" ? "<c:barDir val=\"col\"/>" : "") + "<c:grouping val=\"" + grouping + "\"/>" +
           series(0, "2025", "<c:pt idx=\"0\"><c:v>10</c:v></c:pt><c:pt idx=\"1\"><c:v>25</c:v></c:pt><c:pt idx=\"2\"><c:v>15</c:v></c:pt>") +
           series(1, "2026", "<c:pt idx=\"0\"><c:v>12</c:v></c:pt><c:pt idx=\"1\"><c:v>20</c:v></c:pt><c:pt idx=\"2\"><c:v>30</c:v></c:pt>") +
           "</c:" + kind + "><c:catAx/><c:valAx><c:majorGridlines/></c:valAx></c:plotArea><c:legend><c:legendPos val=\"b\"/></c:legend></c:chart></c:chartSpace>";
}

// One small document of each format, with a picture where the format has them: for the UI test,
// which opens, draws and closes them.
inline std::vector<std::pair<std::string, std::string>> sampleDocuments()
{
    std::vector<std::pair<std::string, std::string>> out;
    out.push_back({"sample.docx",
                   zip({{"_rels/.rels", packageRels("word/document.xml")},
                        {"word/_rels/document.xml.rels", rels({{"rId5", "image", "media/image1.png"}, {"rId9", "hyperlink", "https://example.com/"},
                                                                 {"rId6", "chart", "charts/chart1.xml"}, {"rId7", "comments", "comments.xml"}})},
                        {"word/charts/chart1.xml", chartXml()},
                        {"word/comments.xml", R"(<w:comments xmlns:w="w"><w:comment w:id="0" w:author="Ana"><w:p><w:r><w:t>Check this.</w:t></w:r></w:p></w:comment></w:comments>)"},
                        {"word/document.xml",
                         R"(<w:document xmlns:w="w" xmlns:r="r" xmlns:a="a"><w:body><w:p><w:r><w:rPr><w:b/></w:rPr><w:t>Sample</w:t></w:r></w:p>
<w:p><w:hyperlink r:id="rId9"><w:r><w:t>a link</w:t></w:r></w:hyperlink></w:p>
<w:tbl><w:tr><w:tc><w:p><w:r><w:t>A</w:t></w:r></w:p></w:tc><w:tc><w:p><w:r><w:t>B</w:t></w:r></w:p></w:tc></w:tr></w:tbl>
<w:p><w:r><w:drawing><a:blip r:embed="rId5"/></w:drawing></w:r></w:p>
<w:p><w:r><w:t>Reviewed</w:t></w:r><w:r><w:commentReference w:id="0"/></w:r></w:p>
<w:p><w:r><w:drawing><wp:inline xmlns:wp="wp"><wp:extent cx="4572000" cy="2743200"/><a:graphic><a:graphicData><c:chart xmlns:c="c" r:id="rId6"/></a:graphicData></a:graphic></wp:inline></w:drawing></w:r></w:p>
<w:p><w:r><w:drawing><wps:wsp xmlns:wps="wps"><wps:txbx><w:txbxContent><w:p><w:r><w:t>Inside a text box</w:t></w:r></w:p></w:txbxContent></wps:txbx></wps:wsp></w:drawing></w:r></w:p>
</w:body></w:document>)"},
                        {"word/media/image1.png", tinyPicture()}})});
    out.push_back({"sample.xlsx",
                   zip({{"_rels/.rels", packageRels("xl/workbook.xml")},
                        {"xl/_rels/workbook.xml.rels", rels({{"rId1", "worksheet", "worksheets/sheet1.xml"}, {"rId2", "worksheet", "worksheets/sheet2.xml"}})},
                        {"xl/workbook.xml", R"(<workbook xmlns:r="r"><sheets><sheet name="One" r:id="rId1"/><sheet name="Two" r:id="rId2"/></sheets></workbook>)"},
                        {"xl/worksheets/sheet1.xml",
                         R"(<worksheet><sheetViews><sheetView><pane xSplit="1" ySplit="1" state="frozen"/></sheetView></sheetViews><sheetData>
<row r="1"><c r="A1" t="inlineStr"><is><t>Name</t></is></c><c r="B1"><v>1.5</v></c></row><row r="300"><c r="Z300"><f>B1*2</f><v>3</v></c></row>
</sheetData><mergeCells><mergeCell ref="C3:D4"/></mergeCells></worksheet>)"},
                        {"xl/worksheets/sheet2.xml", R"(<worksheet><sheetData><row r="1"><c r="A1"><v>7</v></c></row></sheetData></worksheet>)"},
                        {"xl/worksheets/_rels/sheet1.xml.rels", rels({{"rId1", "drawing", "../drawings/drawing1.xml"}})},
                        {"xl/drawings/_rels/drawing1.xml.rels", rels({{"rId1", "chart", "../charts/chart1.xml"}})},
                        {"xl/charts/chart1.xml", chartXml("lineChart", "standard")},
                        {"xl/drawings/drawing1.xml",
                         R"(<xdr:wsDr xmlns:xdr="xdr" xmlns:a="a" xmlns:c="c" xmlns:r="r">
<xdr:twoCellAnchor><xdr:from><xdr:col>2</xdr:col><xdr:colOff>0</xdr:colOff><xdr:row>5</xdr:row><xdr:rowOff>0</xdr:rowOff></xdr:from>
<xdr:to><xdr:col>8</xdr:col><xdr:colOff>0</xdr:colOff><xdr:row>20</xdr:row><xdr:rowOff>0</xdr:rowOff></xdr:to>
<xdr:graphicFrame><a:graphic><a:graphicData><c:chart r:id="rId1"/></a:graphicData></a:graphic></xdr:graphicFrame><xdr:clientData/></xdr:twoCellAnchor>
<xdr:oneCellAnchor><xdr:from><xdr:col>9</xdr:col><xdr:colOff>0</xdr:colOff><xdr:row>2</xdr:row><xdr:rowOff>0</xdr:rowOff></xdr:from><xdr:ext cx="1905000" cy="762000"/>
<xdr:sp><xdr:spPr><a:prstGeom prst="rightArrow"/><a:solidFill><a:srgbClr val="FFC000"/></a:solidFill></xdr:spPr>
<xdr:txBody><a:bodyPr anchor="ctr"/><a:p><a:pPr algn="ctr"/><a:r><a:rPr sz="1200" b="1"/><a:t>Look here</a:t></a:r></a:p></xdr:txBody></xdr:sp><xdr:clientData/></xdr:oneCellAnchor>
</xdr:wsDr>)"}})});
    out.push_back({"sample.pptx",
                   zip({{"_rels/.rels", packageRels("ppt/presentation.xml")},
                        {"ppt/_rels/presentation.xml.rels", rels({{"rId2", "slide", "slides/slide1.xml"}, {"rId3", "slide", "slides/slide1.xml"}})},
                        {"ppt/presentation.xml",
                         R"(<p:presentation xmlns:p="p" xmlns:r="r"><p:sldIdLst><p:sldId r:id="rId2"/><p:sldId r:id="rId3"/></p:sldIdLst><p:sldSz cx="9144000" cy="6858000"/></p:presentation>)"},
                        {"ppt/slides/_rels/slide1.xml.rels", rels({{"rId3", "image", "../media/image1.png"}, {"rId4", "chart", "../charts/chart1.xml"},
                                                                   {"rId5", "diagramData", "../diagrams/data1.xml"},
                                                                   {"rId6", "diagramDrawing", "../diagrams/drawing1.xml"}})},
                        {"ppt/charts/chart1.xml", chartXml("pieChart", "standard")},
                        {"ppt/diagrams/data1.xml", R"(<dgm:dataModel xmlns:dgm="dgm" xmlns:dsp="dsp"><dgm:extLst><a:ext xmlns:a="a"><dsp:dataModelExt relId="rId6"/></a:ext></dgm:extLst></dgm:dataModel>)"},
                        {"ppt/diagrams/drawing1.xml",
                         R"(<dsp:drawing xmlns:dsp="dsp" xmlns:a="a"><dsp:spTree>
<dsp:sp><dsp:spPr><a:xfrm><a:off x="0" y="0"/><a:ext cx="1000000" cy="600000"/></a:xfrm><a:prstGeom prst="chevron"/><a:solidFill><a:srgbClr val="70AD47"/></a:solidFill></dsp:spPr>
<dsp:txBody><a:bodyPr anchor="ctr"/><a:p><a:pPr algn="ctr"/><a:r><a:t>Step one</a:t></a:r></a:p></dsp:txBody><dsp:txXfrm><a:off x="100000" y="0"/><a:ext cx="800000" cy="600000"/></dsp:txXfrm></dsp:sp>
</dsp:spTree></dsp:drawing>)"},
                        {"ppt/slides/slide1.xml",
                         R"(<p:sld xmlns:p="p" xmlns:a="a" xmlns:r="r"><p:cSld><p:spTree>
<p:sp><p:spPr><a:xfrm><a:off x="457200" y="457200"/><a:ext cx="4000000" cy="2000000"/></a:xfrm><a:prstGeom prst="roundRect"/><a:solidFill><a:srgbClr val="3366CC"/></a:solidFill></p:spPr>
<p:txBody><a:bodyPr anchor="ctr"/><a:p><a:pPr algn="ctr"/><a:r><a:rPr sz="2400" b="1"/><a:t>A slide with a long title that wraps over several lines</a:t></a:r></a:p></p:txBody></p:sp>
<p:pic><p:blipFill><a:blip r:embed="rId3"/></p:blipFill><p:spPr><a:xfrm><a:off x="5000000" y="3000000"/><a:ext cx="2000000" cy="2000000"/></a:xfrm></p:spPr></p:pic>
<p:graphicFrame><p:xfrm><a:off x="457200" y="2743200"/><a:ext cx="3600000" cy="2400000"/></p:xfrm><a:graphic><a:graphicData><c:chart xmlns:c="c" r:id="rId4"/></a:graphicData></a:graphic></p:graphicFrame>
<p:graphicFrame><p:xfrm><a:off x="5000000" y="457200"/><a:ext cx="3000000" cy="1800000"/></p:xfrm><a:graphic><a:graphicData><dgm:relIds xmlns:dgm="dgm" r:dm="rId5"/></a:graphicData></a:graphic></p:graphicFrame>
</p:spTree></p:cSld></p:sld>)"},
                        {"ppt/media/image1.png", tinyPicture()}})});
    const std::string picture = "<< /Type /XObject /Subtype /Image /Width 2 /Height 2 /BitsPerComponent 8 /ColorSpace /DeviceRGB /Length 12 >>\nstream\n" +
                                std::string("\xFF\x00\x00\x00\xFF\x00\x00\x00\xFF\xFF\xFF\xFF", 12) + "\nendstream";
    std::vector<std::string> objects = {
        "<< /Type /Catalog /Pages 2 0 R >>",
        "<< /Type /Pages /Kids [3 0 R 6 0 R] /Count 2 /MediaBox [0 0 300 200] >>",
        "<< /Type /Page /Parent 2 0 R /Resources << /Font << /F1 4 0 R >> /XObject << /Im1 7 0 R >> >> /Contents 5 0 R >>",
        "<< /Type /Font /Subtype /Type1 /BaseFont /Times-Bold >>",
        stream("", "0.2 0.6 0.2 rg 20 20 100 50 re f 0 0 1 RG 3 w 20 150 m 120 180 120 120 280 150 c S BT /F1 18 Tf 20 100 Td (Page one) Tj ET "
                   "q 60 0 0 60 200 20 cm /Im1 Do Q",
               true),
        "<< /Type /Page /Parent 2 0 R /Resources << /Font << /F1 4 0 R >> >> /Contents 8 0 R /Rotate 90 >>",
        picture,
        stream("", "BT /F1 12 Tf 10 10 Td (Page two, rotated) Tj ET", true),
    };
    out.push_back({"sample.pdf", pdf(objects)});
    return out;
}

// A PDF of `pages` pages, each with a line of text and a picture of its own (`side` pixels square,
// drawn half an inch wide: read, then shrunk).
inline std::string manyPagesPdf(int pages, int side = 96)
{
    std::vector<std::string> objects = {"<< /Type /Catalog /Pages 2 0 R >>", "", "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>"};
    std::string kids;
    for (int i = 0; i < pages; ++i) {
        const int page = int(objects.size()) + 1, content = page + 1, image = page + 2;
        kids += std::to_string(page) + " 0 R ";
        objects.push_back("<< /Type /Page /Parent 2 0 R /Resources << /Font << /F1 3 0 R >> /XObject << /Im1 " + std::to_string(image) +
                          " 0 R >> >> /Contents " + std::to_string(content) + " 0 R >>");
        objects.push_back(stream("", "BT /F1 12 Tf 20 150 Td (Page " + std::to_string(i + 1) + ") Tj ET q 36 0 0 36 20 20 cm /Im1 Do Q", true));
        std::string pixels(std::size_t(side) * std::size_t(side) * 3, '\0');
        for (std::size_t p = 0; p < pixels.size(); ++p)
            pixels[p] = char((p * 7 + std::size_t(i) * 31) & 0xFF);
        objects.push_back("<< /Type /XObject /Subtype /Image /Width " + std::to_string(side) + " /Height " + std::to_string(side) +
                          " /BitsPerComponent 8 /ColorSpace /DeviceRGB /Length " + std::to_string(pixels.size()) + " >>\nstream\n" + pixels +
                          "\nendstream");
    }
    objects[1] = "<< /Type /Pages /Kids [" + kids + "] /Count " + std::to_string(pages) + " /MediaBox [0 0 300 200] >>";
    return pdf(objects);
}

} // namespace viewertest
