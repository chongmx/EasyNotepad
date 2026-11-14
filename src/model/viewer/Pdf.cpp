// PDF: the page tree into fixed pages, each read when first shown. A page's content streams are
// interpreted into the shared display list: paths (filled and stroked, flattened), text placed
// glyph by glyph from the fonts' widths and mapped to Unicode (ToUnicode maps, the standard
// encodings and their glyph names), images (JPEG as is, raw samples as QOI) and form XObjects.
// Not yet: clipping, shadings and patterns, annotations, Type 3 glyph shapes (their text is
// shown in our font), embedded font outlines (all text uses our font at the PDF's positions),
// encrypted files.
#include "enp/model/viewer/ImagePack.h"
#include "enp/model/viewer/Viewer.h"

#include "PdfObjects.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>

namespace enp::model::viewer {
namespace pdf {
namespace {

// ---- encodings and glyph names ------------------------------------------------------------------

const char* const kAsciiNames[95] = {
    "space", "exclam", "quotedbl", "numbersign", "dollar", "percent", "ampersand", "quotesingle", "parenleft", "parenright",
    "asterisk", "plus", "comma", "hyphen", "period", "slash", "zero", "one", "two", "three", "four", "five", "six", "seven",
    "eight", "nine", "colon", "semicolon", "less", "equal", "greater", "question", "at", "A", "B", "C", "D", "E", "F", "G",
    "H", "I", "J", "K", "L", "M", "N", "O", "P", "Q", "R", "S", "T", "U", "V", "W", "X", "Y", "Z", "bracketleft",
    "backslash", "bracketright", "asciicircum", "underscore", "grave", "a", "b", "c", "d", "e", "f", "g", "h", "i", "j", "k",
    "l", "m", "n", "o", "p", "q", "r", "s", "t", "u", "v", "w", "x", "y", "z", "braceleft", "bar", "braceright", "asciitilde"};

const char* const kLatin1Names[96] = {
    "nbspace", "exclamdown", "cent", "sterling", "currency", "yen", "brokenbar", "section", "dieresis", "copyright",
    "ordfeminine", "guillemotleft", "logicalnot", "sfthyphen", "registered", "macron", "degree", "plusminus", "twosuperior",
    "threesuperior", "acute", "mu", "paragraph", "periodcentered", "cedilla", "onesuperior", "ordmasculine", "guillemotright",
    "onequarter", "onehalf", "threequarters", "questiondown", "Agrave", "Aacute", "Acircumflex", "Atilde", "Adieresis",
    "Aring", "AE", "Ccedilla", "Egrave", "Eacute", "Ecircumflex", "Edieresis", "Igrave", "Iacute", "Icircumflex",
    "Idieresis", "Eth", "Ntilde", "Ograve", "Oacute", "Ocircumflex", "Otilde", "Odieresis", "multiply", "Oslash", "Ugrave",
    "Uacute", "Ucircumflex", "Udieresis", "Yacute", "Thorn", "germandbls", "agrave", "aacute", "acircumflex", "atilde",
    "adieresis", "aring", "ae", "ccedilla", "egrave", "eacute", "ecircumflex", "edieresis", "igrave", "iacute",
    "icircumflex", "idieresis", "eth", "ntilde", "ograve", "oacute", "ocircumflex", "otilde", "odieresis", "divide",
    "oslash", "ugrave", "uacute", "ucircumflex", "udieresis", "yacute", "thorn", "ydieresis"};

struct NamedGlyph {
    const char* name;
    std::uint32_t cp;
};
const NamedGlyph kMoreNames[] = {
    {"bullet", 0x2022}, {"endash", 0x2013}, {"emdash", 0x2014}, {"quoteleft", 0x2018}, {"quoteright", 0x2019},
    {"quotedblleft", 0x201C}, {"quotedblright", 0x201D}, {"quotesinglbase", 0x201A}, {"quotedblbase", 0x201E},
    {"ellipsis", 0x2026}, {"fi", 0xFB01}, {"fl", 0xFB02}, {"ff", 0xFB00}, {"ffi", 0xFB03}, {"ffl", 0xFB04},
    {"dagger", 0x2020}, {"daggerdbl", 0x2021}, {"trademark", 0x2122}, {"minus", 0x2212}, {"Euro", 0x20AC},
    {"fraction", 0x2044}, {"guilsinglleft", 0x2039}, {"guilsinglright", 0x203A}, {"perthousand", 0x2030},
    {"florin", 0x0192}, {"circumflex", 0x02C6}, {"tilde", 0x02DC}, {"Scaron", 0x0160}, {"scaron", 0x0161},
    {"OE", 0x0152}, {"oe", 0x0153}, {"Zcaron", 0x017D}, {"zcaron", 0x017E}, {"Ydieresis", 0x0178}, {"dotlessi", 0x0131},
    {"Lslash", 0x0141}, {"lslash", 0x0142}, {"ring", 0x02DA}, {"caron", 0x02C7}, {"breve", 0x02D8},
    {"dotaccent", 0x02D9}, {"hungarumlaut", 0x02DD}, {"ogonek", 0x02DB}, {"space", 0x20}, {"nbspace", 0xA0},
    {"hyphen", 0x2D}, {"quotesingle", 0x27}, {"grave", 0x60}, {"arrowright", 0x2192}, {"arrowleft", 0x2190},
    {"arrowup", 0x2191}, {"arrowdown", 0x2193}, {"checkmark", 0x2713}, {"infinity", 0x221E}, {"lessequal", 0x2264},
    {"greaterequal", 0x2265}, {"notequal", 0x2260}, {"approxequal", 0x2248}, {"summation", 0x2211}, {"product", 0x220F},
    {"integral", 0x222B}, {"radical", 0x221A}, {"partialdiff", 0x2202}, {"Delta", 0x2206}, {"Omega", 0x2126},
    {"pi", 0x03C0}, {"mu", 0xB5}, {"lozenge", 0x25CA}, {"apple", 0xF8FF}, {"Gamma", 0x0393}, {"Theta", 0x0398},
    {"Lambda", 0x039B}, {"Sigma", 0x03A3}, {"Phi", 0x03A6}, {"Psi", 0x03A8}, {"alpha", 0x03B1}, {"beta", 0x03B2},
    {"gamma", 0x03B3}, {"delta", 0x03B4}, {"epsilon", 0x03B5}, {"lambda", 0x03BB}, {"sigma", 0x03C3}, {"theta", 0x03B8},
    {"omega", 0x03C9}, {"degree", 0xB0}, {"periodcentered", 0xB7}};

std::uint32_t glyphNameToUnicode(std::string_view name)
{
    if (const std::size_t dot = name.find('.'); dot != std::string_view::npos && dot > 0) // "a.sc", "one.oldstyle"
        name = name.substr(0, dot);
    for (int i = 0; i < 95; ++i)
        if (name == kAsciiNames[i])
            return std::uint32_t(32 + i);
    for (int i = 0; i < 96; ++i)
        if (name == kLatin1Names[i])
            return std::uint32_t(0xA0 + i);
    for (const NamedGlyph& g : kMoreNames)
        if (name == g.name)
            return g.cp;
    auto hex = [](std::string_view s) -> std::uint32_t {
        std::uint32_t v = 0;
        for (char c : s) {
            int d = c >= '0' && c <= '9' ? c - '0' : c >= 'A' && c <= 'F' ? c - 'A' + 10 : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
            if (d < 0)
                return 0;
            v = v * 16 + std::uint32_t(d);
        }
        return v;
    };
    if (name.size() == 7 && name.substr(0, 3) == "uni")
        return hex(name.substr(3));
    if (name.size() >= 5 && name.size() <= 7 && name[0] == 'u')
        return hex(name.substr(1));
    return 0;
}

const std::uint16_t kWinAnsiHigh[32] = {0x20AC, 0, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160,
                                        0x2039, 0x0152, 0, 0x017D, 0, 0, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013,
                                        0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0, 0x017E, 0x0178};
const std::uint16_t kMacRomanHigh[128] = {
    0xC4,   0xC5,   0xC7,   0xC9,   0xD1,   0xD6,   0xDC,   0xE1,   0xE0,   0xE2,   0xE4,   0xE3,   0xE5,   0xE7,   0xE9,   0xE8,
    0xEA,   0xEB,   0xED,   0xEC,   0xEE,   0xEF,   0xF1,   0xF3,   0xF2,   0xF4,   0xF6,   0xF5,   0xFA,   0xF9,   0xFB,   0xFC,
    0x2020, 0xB0,   0xA2,   0xA3,   0xA7,   0x2022, 0xB6,   0xDF,   0xAE,   0xA9,   0x2122, 0xB4,   0xA8,   0x2260, 0xC6,   0xD8,
    0x221E, 0xB1,   0x2264, 0x2265, 0xA5,   0xB5,   0x2202, 0x2211, 0x220F, 0x03C0, 0x222B, 0xAA,   0xBA,   0x03A9, 0xE6,   0xF8,
    0xBF,   0xA1,   0xAC,   0x221A, 0x0192, 0x2248, 0x2206, 0xAB,   0xBB,   0x2026, 0xA0,   0xC0,   0xC3,   0xD5,   0x0152, 0x0153,
    0x2013, 0x2014, 0x201C, 0x201D, 0x2018, 0x2019, 0xF7,   0x25CA, 0xFF,   0x0178, 0x2044, 0x20AC, 0x2039, 0x203A, 0xFB01, 0xFB02,
    0x2021, 0xB7,   0x201A, 0x201E, 0x2030, 0xC2,   0xCA,   0xC1,   0xCB,   0xC8,   0xCD,   0xCE,   0xCF,   0xCC,   0xD3,   0xD4,
    0xF8FF, 0xD2,   0xDA,   0xDB,   0xD9,   0x0131, 0x02C6, 0x02DC, 0xAF,   0x02D8, 0x02D9, 0x02DA, 0xB8,   0x02DD, 0x02DB, 0x02C7};

// Advance widths (1/1000 em) of codes 32..126 in Helvetica and Times-Roman, for the standard 14
// fonts that come without /Widths; Courier is 600 throughout.
const std::uint16_t kHelvetica[95] = {278, 278, 355, 556, 556, 889, 667, 191, 333, 333, 389, 584, 278, 333, 278, 278, 556, 556, 556,
                                      556, 556, 556, 556, 556, 556, 556, 278, 278, 584, 584, 584, 556, 1015, 667, 667, 722, 722, 667,
                                      611, 778, 722, 278, 500, 667, 556, 833, 722, 778, 667, 778, 722, 667, 611, 722, 667, 944, 667,
                                      667, 611, 278, 278, 278, 469, 556, 333, 556, 556, 500, 556, 556, 278, 556, 556, 222, 222, 500,
                                      222, 833, 556, 556, 556, 556, 333, 500, 278, 556, 500, 722, 500, 500, 500, 334, 260, 334, 584};
const std::uint16_t kTimes[95] = {250, 333, 408, 500, 500, 833, 778, 180, 333, 333, 500, 564, 250, 333, 250, 278, 500, 500, 500,
                                  500, 500, 500, 500, 500, 500, 500, 278, 278, 564, 564, 564, 444, 921, 722, 667, 667, 722, 611,
                                  556, 722, 722, 333, 389, 722, 611, 889, 722, 722, 556, 722, 667, 556, 611, 722, 722, 944, 722,
                                  722, 611, 333, 278, 333, 469, 500, 333, 444, 500, 444, 500, 444, 333, 500, 500, 278, 278, 500,
                                  278, 778, 500, 500, 500, 500, 333, 389, 278, 500, 500, 722, 500, 500, 444, 480, 200, 480, 541};

void appendUtf8(std::string& out, std::uint32_t cp)
{
    if (cp == 0 || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
        cp = 0xFFFD;
    if (cp < 0x80) {
        out += char(cp);
    } else if (cp < 0x800) {
        out += char(0xC0 | (cp >> 6));
        out += char(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += char(0xE0 | (cp >> 12));
        out += char(0x80 | ((cp >> 6) & 0x3F));
        out += char(0x80 | (cp & 0x3F));
    } else {
        out += char(0xF0 | (cp >> 18));
        out += char(0x80 | ((cp >> 12) & 0x3F));
        out += char(0x80 | ((cp >> 6) & 0x3F));
        out += char(0x80 | (cp & 0x3F));
    }
}

// UTF-16BE (a ToUnicode destination) as UTF-8.
std::string utf16ToUtf8(std::string_view b)
{
    std::string out;
    for (std::size_t i = 0; i + 1 < b.size(); i += 2) {
        std::uint32_t u = (std::uint32_t(std::uint8_t(b[i])) << 8) | std::uint8_t(b[i + 1]);
        if (u >= 0xD800 && u <= 0xDBFF && i + 3 < b.size()) {
            const std::uint32_t lo = (std::uint32_t(std::uint8_t(b[i + 2])) << 8) | std::uint8_t(b[i + 3]);
            if (lo >= 0xDC00 && lo <= 0xDFFF) {
                u = 0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00);
                i += 2;
            }
        }
        appendUtf8(out, u);
    }
    return out;
}

std::uint32_t codeOf(std::string_view b)
{
    std::uint32_t v = 0;
    for (std::size_t i = 0; i < b.size() && i < 4; ++i)
        v = (v << 8) | std::uint8_t(b[i]);
    return v;
}

// ---- fonts ------------------------------------------------------------------------------------

// The Latin ligatures (U+FB00..FB06) as their letters: what is copied, and what any font draws.
std::string expandLigatures(const std::string& in)
{
    static const char* const kLetters[] = {"ff", "fi", "fl", "ffi", "ffl", "st", "st"};
    std::string out;
    for (std::size_t i = 0; i < in.size();) {
        if (i + 2 < in.size() && std::uint8_t(in[i]) == 0xEF && std::uint8_t(in[i + 1]) == 0xAC && std::uint8_t(in[i + 2]) <= 0x86) {
            out += kLetters[std::uint8_t(in[i + 2]) - 0x80];
            i += 3;
            continue;
        }
        out += in[i++];
    }
    return out;
}

struct Font {
    std::map<std::uint32_t, std::string> multi; // simple fonts: codes whose glyph name names several letters ("f_i")
    bool twoByte = false;
    std::map<std::uint32_t, std::string> toUnicode;
    std::array<std::uint32_t, 256> encoding{}; // simple fonts: code -> Unicode
    std::vector<float> widths;                 // simple fonts, from firstChar (glyph units)
    int firstChar = 0;
    float missingWidth = 0.0f;
    std::map<std::uint32_t, float> cidWidths;  // CID fonts
    float defaultWidth = 1000.0f;
    float scale = 0.001f; // glyph units to text space (Type 3: FontMatrix)
    const std::uint16_t* standard = nullptr; // Helvetica / Times widths when /Widths is missing
    bool courier = false;
    std::uint8_t flags = 0;

    float width(std::uint32_t code) const
    {
        if (twoByte) {
            const auto it = cidWidths.find(code);
            return (it == cidWidths.end() ? defaultWidth : it->second) * scale;
        }
        if (code >= std::uint32_t(firstChar) && code - std::uint32_t(firstChar) < widths.size())
            return widths[code - std::uint32_t(firstChar)] * scale;
        if (courier)
            return 0.6f;
        if (standard && code >= 32 && code <= 126)
            return standard[code - 32] * 0.001f;
        if (missingWidth > 0)
            return missingWidth * scale;
        return standard ? 0.5f : 0.5f;
    }

    std::string text(std::uint32_t code) const
    {
        const auto it = toUnicode.find(code);
        if (it != toUnicode.end())
            return expandLigatures(it->second);
        if (const auto m = multi.find(code); m != multi.end())
            return m->second;
        std::string out;
        if (!twoByte && code < 256 && encoding[code] >= 0xFB00 && encoding[code] <= 0xFB06) {
            appendUtf8(out, encoding[code]);
            return expandLigatures(out);
        }
        if (!twoByte && code < 256 && encoding[code]) {
            appendUtf8(out, encoding[code]);
        } else if (!twoByte && code >= 32 && code < 127) {
            out += char(code);
        } else if (twoByte && toUnicode.empty() && code >= 32 && code < 0xD800) {
            appendUtf8(out, code); // Identity without a map: often the Unicode value in practice (Identity-H with UCS-2 CIDs)
        }
        return out;
    }
};

void readToUnicode(File& file, const Object& stream, Font& font)
{
    const std::optional<std::string> data = file.streamData(stream);
    if (!data)
        return;
    Lexer lex(*data);
    Object o;
    std::vector<Object> operands;
    bool sawTwoByteRange = false, sawOneByteRange = false;
    while (lex.next(o)) {
        if (o.type != Object::Type::Operator) {
            if (operands.size() < 4096)
                operands.push_back(std::move(o));
            continue;
        }
        if (o.text == "endcodespacerange") {
            for (std::size_t i = 0; i + 1 < operands.size(); i += 2)
                (operands[i].text.size() >= 2 ? sawTwoByteRange : sawOneByteRange) = true;
        } else if (o.text == "endbfchar") {
            for (std::size_t i = 0; i + 1 < operands.size(); i += 2)
                if (operands[i].type == Object::Type::String && operands[i + 1].type == Object::Type::String)
                    font.toUnicode[codeOf(operands[i].text)] = utf16ToUtf8(operands[i + 1].text);
        } else if (o.text == "endbfrange") {
            for (std::size_t i = 0; i + 2 < operands.size(); i += 3) {
                const std::uint32_t lo = codeOf(operands[i].text), hi = codeOf(operands[i + 1].text);
                if (hi < lo || hi - lo > 65535)
                    continue;
                const Object& dst = operands[i + 2];
                if (dst.type == Object::Type::String) {
                    std::string base = dst.text;
                    for (std::uint32_t c = lo; c <= hi; ++c) {
                        font.toUnicode[c] = utf16ToUtf8(base);
                        if (!base.empty()) { // the last UTF-16 unit counts up
                            std::size_t k = base.size();
                            while (k-- > 0) {
                                if (++reinterpret_cast<unsigned char&>(base[k]) != 0)
                                    break;
                            }
                        }
                    }
                } else if (dst.type == Object::Type::Array) {
                    for (std::uint32_t c = lo; c <= hi && c - lo < dst.array.size(); ++c)
                        if (dst.array[c - lo].type == Object::Type::String)
                            font.toUnicode[c] = utf16ToUtf8(dst.array[c - lo].text);
                }
            }
        }
        operands.clear();
    }
    if (sawTwoByteRange && !sawOneByteRange)
        font.twoByte = true;
}

std::unique_ptr<Font> loadFont(File& file, const Object& dict)
{
    auto font = std::make_unique<Font>();
    const Object* subtype = file.resolvedGet(dict, "Subtype");
    const Object* base = file.resolvedGet(dict, "BaseFont");
    std::string name = base && base->type == Object::Type::Name ? base->text : std::string();
    const bool type0 = subtype && subtype->isName("Type0");
    const Object* descriptorHolder = &dict;
    if (type0) {
        font->twoByte = true;
        if (const Object* desc = file.resolvedGet(dict, "DescendantFonts"); desc && desc->type == Object::Type::Array && !desc->array.empty()) {
            const Object& cid = file.resolve(desc->array[0]);
            descriptorHolder = &cid;
            if (const Object* dw = file.resolvedGet(cid, "DW"))
                font->defaultWidth = float(dw->num(1000));
            if (const Object* w = file.resolvedGet(cid, "W"); w && w->type == Object::Type::Array) {
                const std::vector<Object>& a = w->array;
                for (std::size_t i = 0; i + 1 < a.size();) {
                    const Object& first = file.resolve(a[i]);
                    const Object& second = file.resolve(a[i + 1]);
                    if (second.type == Object::Type::Array) {
                        std::uint32_t c = std::uint32_t(std::max(0.0, first.num()));
                        for (const Object& v : second.array)
                            if (font->cidWidths.size() < 200000)
                                font->cidWidths[c++] = float(file.resolve(v).num());
                        i += 2;
                    } else if (i + 2 < a.size()) {
                        const std::uint32_t lo = std::uint32_t(std::max(0.0, first.num())), hi = std::uint32_t(std::max(0.0, second.num()));
                        const float v = float(file.resolve(a[i + 2]).num());
                        for (std::uint32_t c = lo; c <= hi && c - lo < 65536 && font->cidWidths.size() < 200000; ++c)
                            font->cidWidths[c] = v;
                        i += 3;
                    } else {
                        break;
                    }
                }
            }
        }
    } else {
        // Simple font: the encoding, then the widths.
        const bool symbolic = name.find("Symbol") != std::string::npos || name.find("Dingbats") != std::string::npos;
        for (std::uint32_t c = 32; c < 127; ++c)
            font->encoding[c] = c;
        const Object* enc = file.resolvedGet(dict, "Encoding");
        std::string baseEncoding = "StandardEncoding";
        if (enc && enc->type == Object::Type::Name)
            baseEncoding = enc->text;
        else if (enc && enc->isDict())
            if (const Object* be = file.resolvedGet(*enc, "BaseEncoding"); be && be->type == Object::Type::Name)
                baseEncoding = be->text;
        if (baseEncoding == "MacRomanEncoding") {
            for (int c = 0; c < 128; ++c)
                font->encoding[size_t(128 + c)] = kMacRomanHigh[c];
        } else if (!symbolic) { // WinAnsi; Standard's upper half differs, but WinAnsi's is the useful guess
            for (int c = 0; c < 32; ++c)
                font->encoding[size_t(128 + c)] = kWinAnsiHigh[c];
            for (int c = 160; c < 256; ++c)
                font->encoding[size_t(c)] = std::uint32_t(c);
            if (baseEncoding == "StandardEncoding") {
                font->encoding[39] = 0x2019;
                font->encoding[96] = 0x2018;
            }
        }
        if (enc && enc->isDict())
            if (const Object* diff = file.resolvedGet(*enc, "Differences"); diff && diff->type == Object::Type::Array) {
                int code = 0;
                for (const Object& item : diff->array) {
                    const Object& v = file.resolve(item);
                    if (v.isNumber()) {
                        code = int(v.number);
                    } else if (v.type == Object::Type::Name) {
                        if (code >= 0 && code < 256) {
                            const std::uint32_t cp = glyphNameToUnicode(v.text);
                            if (cp) {
                                font->encoding[size_t(code)] = cp;
                            } else if (v.text.find('_') != std::string::npos) { // "f_f_i": its parts
                                std::string letters;
                                std::size_t start = 0;
                                while (start <= v.text.size()) {
                                    const std::size_t bar = v.text.find('_', start);
                                    const std::string part = v.text.substr(start, bar == std::string::npos ? std::string::npos : bar - start);
                                    if (const std::uint32_t c = glyphNameToUnicode(part))
                                        appendUtf8(letters, c);
                                    if (bar == std::string::npos)
                                        break;
                                    start = bar + 1;
                                }
                                if (!letters.empty())
                                    font->multi[std::uint32_t(code)] = letters;
                            }
                        }
                        ++code;
                    }
                }
            }
        if (const Object* fc = file.resolvedGet(dict, "FirstChar"))
            font->firstChar = int(fc->num());
        if (const Object* w = file.resolvedGet(dict, "Widths"); w && w->type == Object::Type::Array)
            for (const Object& v : w->array)
                font->widths.push_back(float(file.resolve(v).num()));
        if (subtype && subtype->isName("Type3"))
            if (const Object* m = file.resolvedGet(dict, "FontMatrix"); m && m->type == Object::Type::Array && !m->array.empty())
                font->scale = float(file.resolve(m->array[0]).num(0.001));
        if (font->widths.empty()) {
            if (name.find("Courier") != std::string::npos)
                font->courier = true;
            else if (name.find("Times") != std::string::npos)
                font->standard = kTimes;
            else
                font->standard = kHelvetica;
        }
    }
    if (const Object* tu = file.resolvedGet(dict, "ToUnicode"); tu && tu->type == Object::Type::Stream)
        readToUnicode(file, *tu, *font);
    if (type0)
        font->twoByte = true;
    // Style: from the name, then the descriptor's flags.
    const std::size_t plus = name.find('+');
    if (plus != std::string::npos)
        name = name.substr(plus + 1);
    auto has = [&](const char* s) { return name.find(s) != std::string::npos; };
    if (has("Bold") || has("Black") || has("Heavy") || has("Semibold") || has("Demi"))
        font->flags |= Bold;
    if (has("Italic") || has("Oblique"))
        font->flags |= Italic;
    if (has("Courier") || has("Mono") || has("Consol") || has("Code"))
        font->flags |= Mono;
    const Object* descriptor = file.resolvedGet(*descriptorHolder, "FontDescriptor");
    if (descriptor) {
        const int f = int(file.resolvedGet(*descriptor, "Flags") ? file.resolvedGet(*descriptor, "Flags")->num() : 0);
        if (f & 1)
            font->flags |= Mono;
        if (f & 64)
            font->flags |= Italic;
        if (f & 262144)
            font->flags |= Bold;
        if (const Object* mw = file.resolvedGet(*descriptor, "MissingWidth"))
            font->missingWidth = float(mw->num());
        if (const Object* fw = file.resolvedGet(*descriptor, "FontWeight"); fw && fw->num() >= 600)
            font->flags |= Bold;
    }
    return font;
}

// ---- images -----------------------------------------------------------------------------------

struct ColorSpace {
    enum Kind { Gray, Rgb, Cmyk, Indexed, Tint, Pattern, Unknown } kind = Gray;
    int components = 1;
    std::string lookup;        // Indexed: hival+1 entries of base colours
    int baseComponents = 3;    // Indexed: of the base space
    Kind baseKind = Rgb;
    int hival = 0;
};

std::uint32_t toColor(const ColorSpace& cs, const float* v, int n, float alpha)
{
    auto c = [](float x) { return unsigned(std::clamp(x, 0.0f, 1.0f) * 255.0f + 0.5f); };
    const unsigned a = c(alpha);
    auto fromKind = [&](ColorSpace::Kind kind, const float* comp, int count) -> std::uint32_t {
        if (kind == ColorSpace::Rgb && count >= 3)
            return rgba(c(comp[0]), c(comp[1]), c(comp[2]), a);
        if (kind == ColorSpace::Cmyk && count >= 4) {
            const float k = comp[3];
            return rgba(c((1 - comp[0]) * (1 - k)), c((1 - comp[1]) * (1 - k)), c((1 - comp[2]) * (1 - k)), a);
        }
        if (kind == ColorSpace::Tint && count >= 1)
            return rgba(c(1 - comp[0]), c(1 - comp[0]), c(1 - comp[0]), a);
        const float g = count >= 1 ? comp[0] : 0.0f;
        return rgba(c(g), c(g), c(g), a);
    };
    if (cs.kind == ColorSpace::Indexed && n >= 1) {
        const int i = std::clamp(int(v[0]), 0, cs.hival);
        float base[4] = {0, 0, 0, 0};
        for (int k = 0; k < cs.baseComponents && k < 4; ++k) {
            const std::size_t at = std::size_t(i * cs.baseComponents + k);
            base[k] = at < cs.lookup.size() ? std::uint8_t(cs.lookup[at]) / 255.0f : 0.0f;
        }
        return fromKind(cs.baseKind, base, cs.baseComponents);
    }
    return fromKind(cs.kind, v, n);
}

// ---- the interpreter --------------------------------------------------------------------------

struct Matrix {
    double a = 1, b = 0, c = 0, d = 1, e = 0, f = 0;
    // this, then m (PDF's row-vector order: p' = p * this * m)
    Matrix then(const Matrix& m) const
    {
        return {a * m.a + b * m.c, a * m.b + b * m.d, c * m.a + d * m.c, c * m.b + d * m.d, e * m.a + f * m.c + m.e, e * m.b + f * m.d + m.f};
    }
    Point apply(double x, double y) const { return {float(a * x + c * y + e), float(b * x + d * y + f)}; }
};

struct GState {
    Matrix ctm;
    ColorSpace fillSpace, strokeSpace;
    std::uint32_t fill = kBlack, stroke = kBlack;
    float fillAlpha = 1.0f, strokeAlpha = 1.0f;
    float lineWidth = 1.0f;
    // Text state
    Font* font = nullptr;
    float fontSize = 12.0f, charSpacing = 0, wordSpacing = 0, hscale = 1.0f, leading = 0, rise = 0;
    int render = 0;
    bool patternFill = false, patternStroke = false;
    std::string fillPattern, strokePattern; // the pattern's name in the resources, with patternFill / patternStroke
    std::vector<float> dash;                // the dash array (user space); empty: solid
    std::int32_t clip = -1;                 // into the page's clips: the clip path's bounds (page points)
};

// A picture a page draws: read later, by its key (PageSource::loadImage), with the resources it
// was drawn with (its colour space may be named there). Points into the file's objects.
struct PictureRef {
    const Object* image = nullptr;
    const Object* resources = nullptr;
};

class PageReader {
public:
    PageReader(File& file, Page& page, std::map<std::string, PictureRef>& pictures, std::map<const Object*, std::unique_ptr<Font>>& fonts)
        : file_(file), page_(page), pictures_(pictures), fonts_(fonts)
    {
        for (std::uint32_t c = 32; c < 127; ++c)
            fallback_.encoding[c] = c;
        fallback_.standard = kHelvetica;
    }

    void run(const Object& pageDict, const Object* resources, const Matrix& base)
    {
        GState g;
        g.ctm = base;
        state_ = g;
        base_ = base;
        const Object* contents = file_.resolvedGet(pageDict, "Contents");
        if (!contents)
            return;
        std::string data;
        if (contents->type == Object::Type::Stream) {
            if (std::optional<std::string> d = file_.streamData(*contents))
                data = std::move(*d);
        } else if (contents->type == Object::Type::Array) {
            for (const Object& part : contents->array) {
                const Object& s = file_.resolve(part);
                if (std::optional<std::string> d = file_.streamData(s)) {
                    data += *d;
                    data += '\n';
                }
            }
        }
        interpret(data, resources, 0);
        flushRun();
    }

private:
    float num(std::size_t i) const { return i < operands_.size() ? float(operands_[i].num()) : 0.0f; }

    ColorSpace colorSpace(const Object& o, const Object* resources, int depth = 0)
    {
        ColorSpace cs;
        const Object& v = file_.resolve(o);
        std::string name;
        const Object* arr = nullptr;
        if (v.type == Object::Type::Name) {
            name = v.text;
        } else if (v.type == Object::Type::Array && !v.array.empty()) {
            arr = &v;
            name = file_.resolve(v.array[0]).text;
        }
        if (name == "DeviceGray" || name == "G" || name == "CalGray") {
            cs.kind = ColorSpace::Gray;
            cs.components = 1;
        } else if (name == "DeviceRGB" || name == "RGB" || name == "CalRGB") {
            cs.kind = ColorSpace::Rgb;
            cs.components = 3;
        } else if (name == "DeviceCMYK" || name == "CMYK") {
            cs.kind = ColorSpace::Cmyk;
            cs.components = 4;
        } else if (name == "ICCBased" && arr && arr->array.size() > 1) {
            const Object& stream = file_.resolve(arr->array[1]);
            const int n = int(stream.get("N") ? file_.resolve(*stream.get("N")).num(3) : 3);
            cs.kind = n == 1 ? ColorSpace::Gray : n == 4 ? ColorSpace::Cmyk : ColorSpace::Rgb;
            cs.components = n == 1 ? 1 : n == 4 ? 4 : 3;
        } else if ((name == "Indexed" || name == "I") && arr && arr->array.size() >= 4 && depth < 3) {
            const ColorSpace base = colorSpace(arr->array[1], resources, depth + 1);
            cs.kind = ColorSpace::Indexed;
            cs.components = 1;
            cs.baseKind = base.kind;
            cs.baseComponents = base.components;
            cs.hival = std::clamp(int(file_.resolve(arr->array[2]).num()), 0, 255);
            const Object& lookup = file_.resolve(arr->array[3]);
            if (lookup.type == Object::Type::String)
                cs.lookup = lookup.text;
            else if (lookup.type == Object::Type::Stream)
                if (std::optional<std::string> d = file_.streamData(lookup))
                    cs.lookup = std::move(*d);
        } else if ((name == "Separation" || name == "DeviceN") && arr) {
            cs.kind = ColorSpace::Tint;
            cs.components = name == "DeviceN" && arr->array.size() > 1 && file_.resolve(arr->array[1]).type == Object::Type::Array
                                ? int(file_.resolve(arr->array[1]).array.size())
                                : 1;
        } else if (name == "Lab" ) {
            cs.kind = ColorSpace::Gray; // approximated by L
            cs.components = 3;
        } else if (name == "Pattern") {
            cs.kind = ColorSpace::Pattern;
            cs.components = 0;
        } else if (!name.empty() && resources && depth < 3) {
            // A name from the resources' /ColorSpace.
            if (const Object* spaces = file_.resolvedGet(*resources, "ColorSpace"))
                if (const Object* entry = spaces->get(name))
                    return colorSpace(*entry, resources, depth + 1);
            cs.kind = ColorSpace::Unknown;
        }
        return cs;
    }

    void setColor(bool fill)
    {
        GState& g = state_;
        ColorSpace& cs = fill ? g.fillSpace : g.strokeSpace;
        if (cs.kind == ColorSpace::Pattern || (!operands_.empty() && operands_.back().type == Object::Type::Name)) {
            (fill ? g.patternFill : g.patternStroke) = true;
            (fill ? g.fillPattern : g.strokePattern) = !operands_.empty() && operands_.back().type == Object::Type::Name ? operands_.back().text : std::string();
            return;
        }
        (fill ? g.patternFill : g.patternStroke) = false;
        float v[8] = {};
        int n = 0;
        for (const Object& o : operands_)
            if (o.isNumber() && n < 8)
                v[n++] = float(o.number);
        (fill ? g.fill : g.stroke) = toColor(cs, v, n, fill ? g.fillAlpha : g.strokeAlpha);
    }

    void deviceColor(bool fill, ColorSpace::Kind kind, int n)
    {
        ColorSpace cs;
        cs.kind = kind;
        cs.components = n;
        (fill ? state_.fillSpace : state_.strokeSpace) = cs;
        (fill ? state_.patternFill : state_.patternStroke) = false;
        float v[4] = {};
        for (int i = 0; i < n; ++i)
            v[i] = num(std::size_t(i));
        (fill ? state_.fill : state_.stroke) = toColor(cs, v, n, fill ? state_.fillAlpha : state_.strokeAlpha);
    }

    static std::uint32_t withAlpha(std::uint32_t c, float a)
    {
        return (c & 0x00FFFFFFu) | (std::uint32_t(std::clamp(a, 0.0f, 1.0f) * float((c >> 24) & 0xFF)) << 24);
    }

    // ---- paths
    void moveTo(float x, float y)
    {
        path_.emplace_back();
        closed_.push_back(false);
        path_.back().push_back(state_.ctm.apply(x, y));
        cx_ = x;
        cy_ = y;
    }
    void lineTo(float x, float y)
    {
        if (path_.empty())
            moveTo(cx_, cy_);
        if (path_.back().size() < 100000)
            path_.back().push_back(state_.ctm.apply(x, y));
        cx_ = x;
        cy_ = y;
    }
    void curveTo(float x1, float y1, float x2, float y2, float x3, float y3)
    {
        if (path_.empty())
            moveTo(cx_, cy_);
        const float x0 = cx_, y0 = cy_;
        // Segments by the curve's size on the page.
        const Point p0 = state_.ctm.apply(x0, y0), p3 = state_.ctm.apply(x3, y3);
        const float span = std::fabs(p3.x - p0.x) + std::fabs(p3.y - p0.y);
        const int n = std::clamp(int(span / 4.0f), 4, 24);
        for (int i = 1; i <= n; ++i) {
            const float t = float(i) / float(n), u = 1.0f - t;
            const float x = u * u * u * x0 + 3 * u * u * t * x1 + 3 * u * t * t * x2 + t * t * t * x3;
            const float y = u * u * u * y0 + 3 * u * u * t * y1 + 3 * u * t * t * y2 + t * t * t * y3;
            path_.back().push_back(state_.ctm.apply(x, y));
        }
        cx_ = x3;
        cy_ = y3;
    }
    void paint(bool fill, bool stroke, bool close)
    {
        if (close)
            for (std::size_t i = 0; i < closed_.size(); ++i)
                closed_[i] = true;
        if (pendingClip_ && !path_.empty()) { // W / W* before this: the path's bounds clip what follows
            float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
            for (const std::vector<Point>& sub : path_)
                for (const Point& q : sub) {
                    x0 = std::min(x0, q.x);
                    y0 = std::min(y0, q.y);
                    x1 = std::max(x1, q.x);
                    y1 = std::max(y1, q.y);
                }
            if (state_.clip >= 0 && std::size_t(state_.clip) < page_.clips.size()) { // inside the clip already set
                const Rect& c = page_.clips[std::size_t(state_.clip)];
                x0 = std::max(x0, c.x);
                y0 = std::max(y0, c.y);
                x1 = std::min(x1, c.x + c.w);
                y1 = std::min(y1, c.y + c.h);
            }
            if (page_.clips.size() < 65536) {
                page_.clips.push_back(Rect{x0, y0, std::max(0.0f, x1 - x0), std::max(0.0f, y1 - y0)});
                state_.clip = std::int32_t(page_.clips.size() - 1);
            }
        }
        pendingClip_ = false;
        if ((fill || stroke) && !path_.empty() && budget()) {
            PathItem item;
            item.evenOdd = evenOdd_;
            if (fill && !state_.patternFill)
                item.fill = state_.fill;
            if (fill && state_.patternFill) // a shading pattern: the path filled with its gradient
                item.fill = patternGradient(state_.fillPattern, item.gradient);
            const Matrix& m = state_.ctm;
            const float scale = float(std::sqrt(std::fabs(m.a * m.d - m.b * m.c)));
            if (stroke && !state_.patternStroke) {
                item.stroke = state_.stroke;
                item.lineWidth = std::max(0.1f, state_.lineWidth * scale);
                for (float d : state_.dash)
                    item.dash.push_back(std::max(0.1f, d * scale));
            }
            if (item.fill || item.stroke) {
                for (std::size_t i = 0; i < path_.size(); ++i)
                    if (path_[i].size() >= 2) {
                        item.subpaths.push_back(std::move(path_[i]));
                        item.closed.push_back(fill || closed_[i]);
                    }
                if (!item.subpaths.empty()) {
                    flushRun();
                    page_.clip = state_.clip;
                    page_.add(std::move(item));
                }
            }
        }
        path_.clear();
        closed_.clear();
    }

    // A PDF function at `t` into `out` (types 2, 3 and a one-input sampled 0). False when not read.
    bool evaluate(const Object& fn, double t, std::vector<double>& out, int depth = 0)
    {
        if (depth > 8 || !fn.isDict())
            return false;
        const Object* typeObj = file_.resolvedGet(fn, "FunctionType");
        const int type = typeObj ? int(typeObj->num(-1)) : -1;
        double d0 = 0.0, d1 = 1.0;
        if (const Object* domain = file_.resolvedGet(fn, "Domain"); domain && domain->type == Object::Type::Array && domain->array.size() >= 2) {
            d0 = file_.resolve(domain->array[0]).num(0);
            d1 = file_.resolve(domain->array[1]).num(1);
        }
        t = std::clamp(t, std::min(d0, d1), std::max(d0, d1));
        out.clear();
        if (type == 2) {
            const Object* c0 = file_.resolvedGet(fn, "C0");
            const Object* c1 = file_.resolvedGet(fn, "C1");
            const double n = file_.resolvedGet(fn, "N") ? file_.resolvedGet(fn, "N")->num(1) : 1.0;
            const std::size_t count = std::max(c0 && c0->type == Object::Type::Array ? c0->array.size() : 1,
                                               c1 && c1->type == Object::Type::Array ? c1->array.size() : 1);
            const double k = std::pow(std::max(0.0, (t - d0) / (d1 - d0 == 0.0 ? 1.0 : d1 - d0)), n);
            for (std::size_t i = 0; i < count && i < 8; ++i) {
                const double a = c0 && c0->type == Object::Type::Array && i < c0->array.size() ? file_.resolve(c0->array[i]).num(0) : 0.0;
                const double b = c1 && c1->type == Object::Type::Array && i < c1->array.size() ? file_.resolve(c1->array[i]).num(1) : 1.0;
                out.push_back(a + k * (b - a));
            }
            return true;
        }
        if (type == 3) {
            const Object* fns = file_.resolvedGet(fn, "Functions");
            if (!fns || fns->type != Object::Type::Array || fns->array.empty())
                return false;
            std::vector<double> bounds;
            if (const Object* b = file_.resolvedGet(fn, "Bounds"); b && b->type == Object::Type::Array)
                for (const Object& v : b->array)
                    bounds.push_back(file_.resolve(v).num());
            std::size_t i = 0;
            while (i < bounds.size() && t >= bounds[i])
                ++i;
            i = std::min(i, fns->array.size() - 1);
            const double lo = i == 0 ? d0 : bounds[i - 1], hi = i < bounds.size() ? bounds[i] : d1;
            double e0 = 0.0, e1 = 1.0;
            if (const Object* enc = file_.resolvedGet(fn, "Encode"); enc && enc->type == Object::Type::Array && enc->array.size() >= 2 * (i + 1)) {
                e0 = file_.resolve(enc->array[2 * i]).num(0);
                e1 = file_.resolve(enc->array[2 * i + 1]).num(1);
            }
            const double u = hi > lo ? e0 + (t - lo) / (hi - lo) * (e1 - e0) : e0;
            return evaluate(file_.resolve(fns->array[i]), u, out, depth + 1);
        }
        if (type == 0 && fn.type == Object::Type::Stream) { // sampled, one input
            const Object* size = file_.resolvedGet(fn, "Size");
            const Object* range = file_.resolvedGet(fn, "Range");
            const int bps = int(file_.resolvedGet(fn, "BitsPerSample") ? file_.resolvedGet(fn, "BitsPerSample")->num(8) : 8);
            if (!size || size->type != Object::Type::Array || size->array.empty() || !range || range->type != Object::Type::Array || (bps != 8 && bps != 16))
                return false;
            const int samples = std::max(1, int(file_.resolve(size->array[0]).num(1)));
            const std::size_t outputs = range->array.size() / 2;
            const std::optional<std::string> data = file_.streamData(fn);
            if (!data || outputs == 0)
                return false;
            const double x = (t - d0) / (d1 - d0 == 0.0 ? 1.0 : d1 - d0) * double(samples - 1);
            const int i0 = std::clamp(int(x), 0, samples - 1), i1 = std::min(samples - 1, i0 + 1);
            const double f = x - double(i0);
            const std::size_t bytes = std::size_t(bps / 8);
            auto sample = [&](int i, std::size_t c) {
                const std::size_t at = (std::size_t(i) * outputs + c) * bytes;
                if (at + bytes > data->size())
                    return 0.0;
                double v = std::uint8_t((*data)[at]);
                if (bytes == 2)
                    v = v * 256.0 + std::uint8_t((*data)[at + 1]);
                return v / (bytes == 2 ? 65535.0 : 255.0);
            };
            for (std::size_t c = 0; c < outputs && c < 8; ++c) {
                const double r0 = file_.resolve(range->array[2 * c]).num(0), r1 = file_.resolve(range->array[2 * c + 1]).num(1);
                const double v = sample(i0, c) * (1.0 - f) + sample(i1, c) * f;
                out.push_back(r0 + v * (r1 - r0));
            }
            return true;
        }
        return false;
    }

    // An axial (2) or radial (3) shading as a gradient along its axis in page points, `m` taking its
    // coordinates there. Returns its first colour, or 0 when it is not one of those.
    std::uint32_t shadingGradient(const Object& sh, const Matrix& m, Gradient& out)
    {
        out = Gradient();
        const Object* typeObj = file_.resolvedGet(sh, "ShadingType");
        const int type = typeObj ? int(typeObj->num()) : 0;
        const Object* coords = file_.resolvedGet(sh, "Coords");
        const Object* fn = file_.resolvedGet(sh, "Function");
        if ((type != 2 && type != 3) || !coords || coords->type != Object::Type::Array || !fn)
            return 0;
        auto c = [&](std::size_t i) { return i < coords->array.size() ? file_.resolve(coords->array[i]).num() : 0.0; };
        ColorSpace cs;
        if (const Object* space = sh.get("ColorSpace"))
            cs = colorSpace(*space, resources_);
        double t0 = 0.0, t1 = 1.0;
        if (const Object* domain = file_.resolvedGet(sh, "Domain"); domain && domain->type == Object::Type::Array && domain->array.size() >= 2) {
            t0 = file_.resolve(domain->array[0]).num(0);
            t1 = file_.resolve(domain->array[1]).num(1);
        }
        std::vector<double> values, part;
        for (int i = 0; i <= 8; ++i) {
            const double t = t0 + (t1 - t0) * double(i) / 8.0;
            values.clear();
            if (fn->type == Object::Type::Array) { // one function per component
                for (const Object& f : fn->array)
                    if (evaluate(file_.resolve(f), t, part) && !part.empty())
                        values.push_back(part[0]);
            } else if (!evaluate(*fn, t, values)) {
                return 0;
            }
            float v[8] = {};
            for (std::size_t k = 0; k < values.size() && k < 8; ++k)
                v[k] = float(values[k]);
            out.stops.push_back({float(i) / 8.0f, toColor(cs, v, int(std::min<std::size_t>(8, values.size())), state_.fillAlpha)});
        }
        out.absolute = true;
        if (type == 2) {
            out.from = m.apply(c(0), c(1));
            out.to = m.apply(c(2), c(3));
        } else {
            out.radial = true;
            const float k = float(std::sqrt(std::fabs(m.a * m.d - m.b * m.c)));
            out.from = m.apply(c(0), c(1));
            out.r0 = float(c(2)) * k;
            out.to = m.apply(c(3), c(4));
            out.r1 = float(c(5)) * k;
        }
        return out.stops.front().color;
    }

    // The gradient of shading pattern `name` (in the resources' /Pattern): its first colour, or a
    // light grey for a tiling pattern (its cells are not drawn).
    std::uint32_t patternGradient(const std::string& name, Gradient& out)
    {
        out = Gradient();
        if (!resources_ || name.empty())
            return 0;
        const Object* patterns = file_.resolvedGet(*resources_, "Pattern");
        const Object* ref = patterns ? patterns->get(name) : nullptr;
        if (!ref)
            return 0;
        const Object& pattern = file_.resolve(*ref);
        const Object* typeObj = file_.resolvedGet(pattern, "PatternType");
        if (!typeObj || int(typeObj->num()) != 2)
            return rgba(200, 200, 200, std::uint32_t(state_.fillAlpha * 255.0f));
        Matrix m;
        if (const Object* mo = file_.resolvedGet(pattern, "Matrix"); mo && mo->type == Object::Type::Array && mo->array.size() == 6)
            m = Matrix{file_.resolve(mo->array[0]).num(1), file_.resolve(mo->array[1]).num(), file_.resolve(mo->array[2]).num(),
                       file_.resolve(mo->array[3]).num(1), file_.resolve(mo->array[4]).num(), file_.resolve(mo->array[5]).num()};
        const Object* sh = file_.resolvedGet(pattern, "Shading");
        return sh ? shadingGradient(*sh, m.then(base_), out) : 0;
    }

    // `sh`: the shading fills the clip (or the page).
    void shade(const std::string& name, const Object* resources)
    {
        const Object* shadings = file_.resolvedGet(*resources, "Shading");
        const Object* ref = shadings ? shadings->get(name) : nullptr;
        if (!ref || !budget())
            return;
        PathItem item;
        item.fill = shadingGradient(file_.resolve(*ref), state_.ctm, item.gradient);
        if (!item.fill && item.gradient.stops.empty())
            return;
        Rect r{0.0f, 0.0f, page_.width, page_.height};
        if (state_.clip >= 0 && std::size_t(state_.clip) < page_.clips.size())
            r = page_.clips[std::size_t(state_.clip)];
        item.subpaths.push_back({{r.x, r.y}, {r.x + r.w, r.y}, {r.x + r.w, r.y + r.h}, {r.x, r.y + r.h}});
        item.closed.push_back(true);
        flushRun();
        page_.clip = state_.clip;
        page_.add(std::move(item));
    }

    bool budget()
    {
        return page_.items.size() < 400000;
    }

    // ---- text
    void flushRun()
    {
        if (!run_.text.empty()) {
            // A run of spaces only is nothing to draw.
            if (run_.text.find_first_not_of(' ') != std::string::npos && budget()) {
                page_.clip = runClip_;
                page_.add(std::move(run_));
            }
        }
        run_ = GlyphRun();
        runOpen_ = false;
    }

    void showText(const std::string& bytes)
    {
        GState& g = state_;
        Font* font = g.font ? g.font : &fallback_; // text before any Tf: Helvetica
        const std::size_t step = font->twoByte ? 2 : 1;
        for (std::size_t i = 0; i + step <= bytes.size(); i += step) {
            const std::uint32_t code = step == 2 ? (std::uint32_t(std::uint8_t(bytes[i])) << 8) | std::uint8_t(bytes[i + 1]) : std::uint8_t(bytes[i]);
            const float w0 = font->width(code);
            const Matrix trm = Matrix{g.fontSize * g.hscale, 0, 0, g.fontSize, 0, g.rise}.then(tm_).then(g.ctm);
            const Point origin = trm.apply(0, 0);
            const float size = float(std::sqrt(trm.c * trm.c + trm.d * trm.d));
            const std::string text = font->text(code);
            const bool invisible = g.render == 3 || g.render == 7;
            std::uint32_t color = g.render == 1 || g.render == 5 ? g.stroke : g.fill;
            if (invisible)
                color &= 0x00FFFFFFu;
            if (!text.empty() && size > 0.5f && size < 2000.0f) {
                const float xLength = float(std::sqrt(trm.a * trm.a + trm.b * trm.b));
                const float advance = w0 * xLength;
                // The run goes along the text's x axis on the page (turned text, a turned page).
                const float angle = xLength > 0.0f ? float(std::atan2(trm.b, trm.a) * 180.0 / 3.14159265358979) : 0.0f;
                const float rx = float(std::cos(run_.angle * 3.14159265358979 / 180.0)), ry = float(std::sin(run_.angle * 3.14159265358979 / 180.0));
                const float dx = origin.x - run_.x, dy = origin.y - run_.baseline;
                const float along = dx * rx + dy * ry, across = -dx * ry + dy * rx;
                const bool same = runOpen_ && std::fabs(angle - run_.angle) < 0.5f && std::fabs(across) < 0.5f && std::fabs(size - run_.size) < 0.05f &&
                                  run_.color == color && run_.flags == font->flags && runClip_ == state_.clip;
                const float expected = run_.width;
                float pos = along; // where this glyph starts along the run
                if (same && along >= expected - size * 0.3f && along <= expected + size * 3.0f) {
                    if (along > expected + size * 0.2f && run_.text.back() != ' ') { // a gap: a space for copying
                        run_.offsets.push_back(expected);
                        run_.text += ' ';
                    }
                } else {
                    flushRun();
                    run_.x = origin.x;
                    run_.baseline = origin.y;
                    run_.angle = angle;
                    run_.size = size;
                    run_.color = color;
                    run_.flags = font->flags;
                    runClip_ = state_.clip;
                    runOpen_ = true;
                    pos = 0.0f;
                }
                // Each code point of the glyph's text, spread over its advance (ligatures).
                std::size_t points = 0;
                for (unsigned char ch : text)
                    if ((ch & 0xC0) != 0x80)
                        ++points;
                std::size_t k = 0;
                for (std::size_t at = 0; at < text.size();) {
                    std::size_t len = 1;
                    const unsigned char ch = static_cast<unsigned char>(text[at]);
                    if (ch >= 0xF0)
                        len = 4;
                    else if (ch >= 0xE0)
                        len = 3;
                    else if (ch >= 0xC0)
                        len = 2;
                    run_.offsets.push_back(pos + advance * float(k) / float(std::max<std::size_t>(1, points)));
                    run_.text.append(text, at, len);
                    at += len;
                    ++k;
                }
                run_.width = pos + advance;
                if (run_.text.size() > 4000)
                    flushRun();
            }
            // Advance the text matrix.
            float tx = w0 * g.fontSize + g.charSpacing;
            if (step == 1 && code == 32)
                tx += g.wordSpacing;
            tx *= g.hscale;
            tm_ = Matrix{1, 0, 0, 1, tx, 0}.then(tm_);
        }
    }

    void setFont(const std::string& name, const Object* resources)
    {
        state_.font = nullptr;
        if (!resources)
            return;
        const Object* fonts = file_.resolvedGet(*resources, "Font");
        if (!fonts)
            return;
        const Object* ref = fonts->get(name);
        if (!ref)
            return;
        const Object& dict = file_.resolve(*ref);
        if (!dict.isDict())
            return;
        auto it = fonts_.find(&dict);
        if (it == fonts_.end())
            it = fonts_.emplace(&dict, loadFont(file_, dict)).first;
        state_.font = it->second.get();
    }

    // ---- images and forms
    void drawImage(const Object& image, const std::string& key)
    {
        const Matrix& m = state_.ctm;
        const Point p[4] = {m.apply(0, 0), m.apply(1, 0), m.apply(0, 1), m.apply(1, 1)};
        float x0 = p[0].x, y0 = p[0].y, x1 = x0, y1 = y0;
        for (const Point& q : p) {
            x0 = std::min(x0, q.x);
            y0 = std::min(y0, q.y);
            x1 = std::max(x1, q.x);
            y1 = std::max(y1, q.y);
        }
        if (x1 - x0 < 0.05f || y1 - y0 < 0.05f || !budget())
            return;
        // Read later (a picture that cannot be read is drawn as a light box by the views).
        pictures_.emplace(key, PictureRef{&image, resources_});
        ImageItem item;
        item.box = Rect{x0, y0, x1 - x0, y1 - y0};
        item.media = key;
        flushRun();
        page_.clip = state_.clip;
        page_.add(std::move(item));
    }

public:
    // A picture this reader's file names, read now (PageSource::loadImage).
    std::string pictureBytes(const PictureRef& ref)
    {
        resources_ = ref.resources;
        return ref.image ? imageBytes(*ref.image) : std::string();
    }

private:
    std::string imageBytes(const Object& image)
    {
        std::string filter;
        std::optional<std::string> data = file_.streamData(image, &filter);
        if (!data)
            return {};
        if (filter == "DCTDecode") { // JPEG, decoded by the reader service (with its soft mask)
            SoftMask sm;
            if (softMask(image, sm))
                return packMasked(*data, sm.bytes, sm.width, sm.height, sm.raw);
            return std::move(*data);
        }
        if (!filter.empty())
            return {}; // JPEG 2000, CCITT, JBIG2: not decoded yet
        const int w = int(file_.resolvedGet(image, "Width") ? file_.resolvedGet(image, "Width")->num() : 0);
        const int h = int(file_.resolvedGet(image, "Height") ? file_.resolvedGet(image, "Height")->num() : 0);
        if (w <= 0 || h <= 0 || double(w) * double(h) > 16.0e6)
            return {};
        const Object* maskObj = file_.resolvedGet(image, "ImageMask");
        const bool mask = maskObj && maskObj->type == Object::Type::Bool && maskObj->boolean;
        int bpc = mask ? 1 : int(file_.resolvedGet(image, "BitsPerComponent") ? file_.resolvedGet(image, "BitsPerComponent")->num(8) : 8);
        if (bpc != 1 && bpc != 2 && bpc != 4 && bpc != 8 && bpc != 16)
            return {};
        ColorSpace cs;
        if (!mask) {
            const Object* space = image.get("ColorSpace");
            if (space)
                cs = colorSpace(*space, resources_);
        }
        const int comps = mask ? 1 : std::max(1, cs.components);
        bool invert = false;
        if (const Object* decode = file_.resolvedGet(image, "Decode"); decode && decode->type == Object::Type::Array && decode->array.size() >= 2)
            invert = file_.resolve(decode->array[0]).num() > file_.resolve(decode->array[1]).num();
        const std::size_t rowBytes = (std::size_t(w) * comps * bpc + 7) / 8;
        if (data->size() < rowBytes * std::size_t(h))
            data->resize(rowBytes * std::size_t(h), '\0');
        std::vector<std::uint8_t> rgba(std::size_t(w) * std::size_t(h) * 4);
        const int maxValue = (1 << std::min(bpc, 8)) - 1;
        auto sample = [&](const std::uint8_t* row, int index) -> int {
            if (bpc == 8)
                return row[index];
            if (bpc == 16)
                return row[index * 2];
            const int bit = index * bpc;
            return (row[bit / 8] >> (8 - bpc - bit % 8)) & maxValue;
        };
        // Common kinds without a colour computed per pixel: 8-bit RGB, grey and CMYK straight from
        // the bytes; one component of up to 8 bits (indexed, grey, a tint) through a table of every
        // value made once. The rest per pixel, below.
        const auto* bytes = reinterpret_cast<const std::uint8_t*>(data->data());
        const std::size_t pixels = std::size_t(w) * std::size_t(h);
        if (!mask && bpc == 8 && !invert && cs.kind == ColorSpace::Rgb && comps == 3) {
            for (std::size_t i = 0; i < pixels; ++i) {
                rgba[i * 4] = bytes[i * 3];
                rgba[i * 4 + 1] = bytes[i * 3 + 1];
                rgba[i * 4 + 2] = bytes[i * 3 + 2];
                rgba[i * 4 + 3] = 255;
            }
        } else if (!mask && bpc == 8 && !invert && cs.kind == ColorSpace::Gray && comps == 1) {
            for (std::size_t i = 0; i < pixels; ++i) {
                rgba[i * 4] = rgba[i * 4 + 1] = rgba[i * 4 + 2] = bytes[i];
                rgba[i * 4 + 3] = 255;
            }
        } else if (!mask && bpc == 8 && !invert && cs.kind == ColorSpace::Cmyk && comps == 4) {
            for (std::size_t i = 0; i < pixels; ++i) {
                const unsigned k = 255u - bytes[i * 4 + 3];
                for (int ch = 0; ch < 3; ++ch)
                    rgba[i * 4 + std::size_t(ch)] = std::uint8_t(((255u - bytes[i * 4 + std::size_t(ch)]) * k + 127u) / 255u);
                rgba[i * 4 + 3] = 255;
            }
        } else if (!mask && comps == 1 && bpc <= 8) {
            std::uint32_t table[256];
            for (int s = 0; s <= maxValue; ++s) {
                float v[4] = {cs.kind == ColorSpace::Indexed ? float(s) : float(s) / float(maxValue), 0, 0, 0};
                if (invert && cs.kind != ColorSpace::Indexed)
                    v[0] = 1.0f - v[0];
                table[s] = toColor(cs, v, 1, 1.0f);
            }
            for (int y = 0; y < h; ++y) {
                const std::uint8_t* row = bytes + std::size_t(y) * rowBytes;
                std::uint8_t* px = &rgba[std::size_t(y) * std::size_t(w) * 4];
                for (int x = 0; x < w; ++x, px += 4) {
                    const std::uint32_t c = table[sample(row, x)];
                    px[0] = std::uint8_t(c & 0xFF);
                    px[1] = std::uint8_t((c >> 8) & 0xFF);
                    px[2] = std::uint8_t((c >> 16) & 0xFF);
                    px[3] = 255;
                }
            }
        } else
        for (int y = 0; y < h; ++y) {
            const std::uint8_t* row = reinterpret_cast<const std::uint8_t*>(data->data()) + std::size_t(y) * rowBytes;
            for (int x = 0; x < w; ++x) {
                std::uint8_t* px = &rgba[(std::size_t(y) * std::size_t(w) + std::size_t(x)) * 4];
                if (mask) {
                    const int v = sample(row, x);
                    const bool paint = invert ? v != 0 : v == 0;
                    const std::uint32_t c = state_.fill;
                    px[0] = std::uint8_t(c & 0xFF);
                    px[1] = std::uint8_t((c >> 8) & 0xFF);
                    px[2] = std::uint8_t((c >> 16) & 0xFF);
                    px[3] = paint ? 255 : 0;
                    continue;
                }
                float v[4] = {0, 0, 0, 0};
                for (int k = 0; k < comps && k < 4; ++k) {
                    const int s = sample(row, x * comps + k);
                    v[k] = cs.kind == ColorSpace::Indexed ? float(s) : float(s) / float(bpc == 16 ? 255 : maxValue);
                    if (invert && cs.kind != ColorSpace::Indexed)
                        v[k] = 1.0f - v[k];
                }
                const std::uint32_t c = toColor(cs, v, comps, 1.0f);
                px[0] = std::uint8_t(c & 0xFF);
                px[1] = std::uint8_t((c >> 8) & 0xFF);
                px[2] = std::uint8_t((c >> 16) & 0xFF);
                px[3] = 255;
            }
        }
        // A soft mask gives the alpha: here when it is raw samples of the same size, otherwise
        // packed with the picture for the reader service.
        // (Raw pixels: the reader service shrinks them to the size shown, so nothing is encoded here.)
        SoftMask sm;
        if (!softMask(image, sm))
            return packRgba(rgba, w, h);
        if (sm.raw && sm.width == w && sm.height == h) {
            for (std::size_t i = 0; i < std::size_t(w) * std::size_t(h); ++i)
                rgba[i * 4 + 3] = std::uint8_t(sm.bytes[i]);
            return packRgba(rgba, w, h);
        }
        return packMasked(packRgba(rgba, w, h), sm.bytes, sm.width, sm.height, sm.raw);
    }

    struct SoftMask {
        std::string bytes; // raw: width * height samples; otherwise a JPEG
        int width = 0, height = 0;
        bool raw = true;
    };

    // The image's soft mask (SMask): 8-bit samples, or a JPEG. False when it has none we can read.
    bool softMask(const Object& image, SoftMask& out)
    {
        const Object* smask = file_.resolvedGet(image, "SMask");
        if (!smask || smask->type != Object::Type::Stream)
            return false;
        out.width = int(file_.resolvedGet(*smask, "Width") ? file_.resolvedGet(*smask, "Width")->num() : 0);
        out.height = int(file_.resolvedGet(*smask, "Height") ? file_.resolvedGet(*smask, "Height")->num() : 0);
        if (out.width <= 0 || out.height <= 0 || out.width > (1 << 14) || out.height > (1 << 14))
            return false;
        std::string filter;
        std::optional<std::string> data = file_.streamData(*smask, &filter);
        if (!data)
            return false;
        out.raw = filter.empty();
        if (out.raw) {
            const Object* bits = file_.resolvedGet(*smask, "BitsPerComponent");
            if ((bits && bits->num(8) != 8) || data->size() < std::size_t(out.width) * std::size_t(out.height))
                return false;
        } else if (filter != "DCTDecode") {
            return false;
        }
        out.bytes = std::move(*data);
        return true;
    }

    void doXObject(const std::string& name, const Object* resources, int depth)
    {
        if (!resources)
            return;
        const Object* xobjects = file_.resolvedGet(*resources, "XObject");
        if (!xobjects)
            return;
        const Object* ref = xobjects->get(name);
        if (!ref)
            return;
        const Object& x = file_.resolve(*ref);
        if (x.type != Object::Type::Stream)
            return;
        const Object* subtype = file_.resolvedGet(x, "Subtype");
        if (subtype && subtype->isName("Image")) {
            const std::string key = ref->type == Object::Type::Ref ? "pdf-image-" + std::to_string(int(ref->number))
                                                                   : "pdf-image-" + std::to_string(reinterpret_cast<std::uintptr_t>(&x));
            drawImage(x, key);
        } else if (subtype && subtype->isName("Form") && depth < 12) {
            std::optional<std::string> data = file_.streamData(x);
            if (!data)
                return;
            const GState saved = state_;
            const Matrix savedTm = tm_, savedTlm = tlm_;
            if (const Object* m = file_.resolvedGet(x, "Matrix"); m && m->type == Object::Type::Array && m->array.size() == 6) {
                Matrix fm{file_.resolve(m->array[0]).num(1), file_.resolve(m->array[1]).num(), file_.resolve(m->array[2]).num(),
                          file_.resolve(m->array[3]).num(1), file_.resolve(m->array[4]).num(), file_.resolve(m->array[5]).num()};
                state_.ctm = fm.then(state_.ctm);
            }
            const Object* formResources = file_.resolvedGet(x, "Resources");
            interpret(*data, formResources ? formResources : resources, depth + 1);
            state_ = saved;
            tm_ = savedTm;
            tlm_ = savedTlm;
        }
    }

    void interpret(const std::string& data, const Object* resources, int depth)
    {
        const Object* savedResources = resources_;
        resources_ = resources;
        Lexer lex(data);
        Object o;
        std::vector<GState> stack;
        operands_.clear();
        while (lex.next(o)) {
            if (++operations_ > 3000000)
                break;
            if (o.type != Object::Type::Operator) {
                if (operands_.size() < 64)
                    operands_.push_back(std::move(o));
                continue;
            }
            const std::string& op = o.text;
            GState& g = state_;
            if (op == "q") {
                if (stack.size() < 64)
                    stack.push_back(g);
            } else if (op == "Q") {
                if (!stack.empty()) {
                    state_ = stack.back();
                    stack.pop_back();
                }
            } else if (op == "cm" && operands_.size() >= 6) {
                g.ctm = Matrix{num(0), num(1), num(2), num(3), num(4), num(5)}.then(g.ctm);
            } else if (op == "w") {
                g.lineWidth = num(0);
            } else if (op == "d" && !operands_.empty() && operands_[0].type == Object::Type::Array) {
                g.dash.clear();
                bool any = false;
                for (const Object& v : operands_[0].array)
                    if (v.isNumber() && g.dash.size() < 16) {
                        g.dash.push_back(std::max(0.0f, float(v.number)));
                        any = any || v.number > 0.0;
                    }
                if (!any)
                    g.dash.clear();
            } else if (op == "W" || op == "W*") {
                pendingClip_ = true;
            } else if (op == "sh" && !operands_.empty() && resources) {
                shade(operands_[0].text, resources);
            } else if (op == "gs" && !operands_.empty() && resources) {
                if (const Object* states = file_.resolvedGet(*resources, "ExtGState"))
                    if (const Object* s = states->get(operands_[0].text)) {
                        const Object& gs = file_.resolve(*s);
                        if (const Object* lw = file_.resolvedGet(gs, "LW"))
                            g.lineWidth = float(lw->num(1));
                        if (const Object* ca = file_.resolvedGet(gs, "ca")) {
                            g.fillAlpha = float(ca->num(1));
                            g.fill = withAlpha(g.fill | 0xFF000000u, g.fillAlpha);
                        }
                        if (const Object* ca = file_.resolvedGet(gs, "CA")) {
                            g.strokeAlpha = float(ca->num(1));
                            g.stroke = withAlpha(g.stroke | 0xFF000000u, g.strokeAlpha);
                        }
                    }
            } else if (op == "m" && operands_.size() >= 2) {
                moveTo(num(0), num(1));
            } else if (op == "l" && operands_.size() >= 2) {
                lineTo(num(0), num(1));
            } else if (op == "c" && operands_.size() >= 6) {
                curveTo(num(0), num(1), num(2), num(3), num(4), num(5));
            } else if (op == "v" && operands_.size() >= 4) {
                curveTo(cx_, cy_, num(0), num(1), num(2), num(3));
            } else if (op == "y" && operands_.size() >= 4) {
                curveTo(num(0), num(1), num(2), num(3), num(2), num(3));
            } else if (op == "h") {
                if (!closed_.empty())
                    closed_.back() = true;
            } else if (op == "re" && operands_.size() >= 4) {
                const float x = num(0), y = num(1), w = num(2), h = num(3);
                moveTo(x, y);
                lineTo(x + w, y);
                lineTo(x + w, y + h);
                lineTo(x, y + h);
                closed_.back() = true;
                cx_ = x;
                cy_ = y;
            } else if (op == "S") {
                paint(false, true, false);
            } else if (op == "s") {
                paint(false, true, true);
            } else if (op == "f" || op == "F" || op == "f*") {
                evenOdd_ = op == "f*";
                paint(true, false, true);
            } else if (op == "B" || op == "B*") {
                evenOdd_ = op == "B*";
                paint(true, true, false);
            } else if (op == "b" || op == "b*") {
                evenOdd_ = op == "b*";
                paint(true, true, true);
            } else if (op == "n") {
                paint(false, false, false);
            } else if (op == "g") {
                deviceColor(true, ColorSpace::Gray, 1);
            } else if (op == "G") {
                deviceColor(false, ColorSpace::Gray, 1);
            } else if (op == "rg") {
                deviceColor(true, ColorSpace::Rgb, 3);
            } else if (op == "RG") {
                deviceColor(false, ColorSpace::Rgb, 3);
            } else if (op == "k") {
                deviceColor(true, ColorSpace::Cmyk, 4);
            } else if (op == "K") {
                deviceColor(false, ColorSpace::Cmyk, 4);
            } else if ((op == "cs" || op == "CS") && !operands_.empty()) {
                const bool fill = op == "cs";
                ColorSpace cs = colorSpace(operands_[0], resources);
                (fill ? g.fillSpace : g.strokeSpace) = cs;
                float zero[4] = {0, 0, 0, 0};
                if (cs.kind == ColorSpace::Cmyk)
                    zero[3] = 1.0f; // initial colour: black
                (fill ? g.fill : g.stroke) = toColor(cs, zero, cs.components, fill ? g.fillAlpha : g.strokeAlpha);
                (fill ? g.patternFill : g.patternStroke) = cs.kind == ColorSpace::Pattern;
            } else if (op == "sc" || op == "scn") {
                setColor(true);
            } else if (op == "SC" || op == "SCN") {
                setColor(false);
            } else if (op == "BT") {
                tm_ = tlm_ = Matrix{};
            } else if (op == "ET") {
                flushRun();
            } else if (op == "Tc") {
                g.charSpacing = num(0);
            } else if (op == "Tw") {
                g.wordSpacing = num(0);
            } else if (op == "Tz") {
                g.hscale = num(0) / 100.0f;
            } else if (op == "TL") {
                g.leading = num(0);
            } else if (op == "Ts") {
                g.rise = num(0);
            } else if (op == "Tr") {
                g.render = int(num(0));
            } else if (op == "Tf" && operands_.size() >= 2) {
                setFont(operands_[0].text, resources);
                g.fontSize = num(1);
            } else if ((op == "Td" || op == "TD") && operands_.size() >= 2) {
                if (op == "TD")
                    g.leading = -num(1);
                tlm_ = Matrix{1, 0, 0, 1, num(0), num(1)}.then(tlm_);
                tm_ = tlm_;
            } else if (op == "Tm" && operands_.size() >= 6) {
                tlm_ = tm_ = Matrix{num(0), num(1), num(2), num(3), num(4), num(5)};
            } else if (op == "T*") {
                tlm_ = Matrix{1, 0, 0, 1, 0, -g.leading}.then(tlm_);
                tm_ = tlm_;
            } else if (op == "Tj" && !operands_.empty()) {
                showText(operands_[0].text);
            } else if (op == "'" && !operands_.empty()) {
                tlm_ = Matrix{1, 0, 0, 1, 0, -g.leading}.then(tlm_);
                tm_ = tlm_;
                showText(operands_.back().text);
            } else if (op == "\"" && operands_.size() >= 3) {
                g.wordSpacing = num(0);
                g.charSpacing = num(1);
                tlm_ = Matrix{1, 0, 0, 1, 0, -g.leading}.then(tlm_);
                tm_ = tlm_;
                showText(operands_[2].text);
            } else if (op == "TJ" && !operands_.empty() && operands_[0].type == Object::Type::Array) {
                for (const Object& item : operands_[0].array) {
                    if (item.type == Object::Type::String) {
                        showText(item.text);
                    } else if (item.isNumber()) {
                        const float tx = -float(item.number) / 1000.0f * g.fontSize * g.hscale;
                        tm_ = Matrix{1, 0, 0, 1, tx, 0}.then(tm_);
                    }
                }
            } else if (op == "Do" && !operands_.empty()) {
                doXObject(operands_[0].text, resources, depth);
            } else if (op == "BI") {
                // An inline image: its dictionary up to ID, then its data up to EI (not drawn yet).
                Object item;
                while (lex.next(item) && !(item.type == Object::Type::Operator && item.text == "ID")) {
                }
                const std::string_view rest = lex.data();
                std::size_t at = lex.position() + 1;
                for (;;) {
                    at = rest.find("EI", at);
                    if (at == std::string_view::npos) {
                        at = rest.size();
                        break;
                    }
                    const bool before = at > 0 && (rest[at - 1] == ' ' || rest[at - 1] == '\n' || rest[at - 1] == '\r' || rest[at - 1] == '\t');
                    const bool after = at + 2 >= rest.size() || rest[at + 2] == ' ' || rest[at + 2] == '\n' || rest[at + 2] == '\r' ||
                                       rest[at + 2] == '\t';
                    if (before && after)
                        break;
                    at += 2;
                }
                lex.seek(std::min(rest.size(), at + 2));
            }
            operands_.clear();
        }
        resources_ = savedResources;
    }

    File& file_;
    Page& page_;
    std::map<std::string, PictureRef>& pictures_;
    std::map<const Object*, std::unique_ptr<Font>>& fonts_;
    const Object* resources_ = nullptr;
    GState state_;
    Matrix tm_, tlm_;
    std::vector<Object> operands_;
    std::vector<std::vector<Point>> path_;
    std::vector<bool> closed_;
    float cx_ = 0, cy_ = 0;
    GlyphRun run_;
    bool runOpen_ = false;
    std::int32_t runClip_ = -1; // the clip the open run was started under
    bool pendingClip_ = false;  // W / W*: the next path painting (or n) sets the clip
    bool evenOdd_ = false;      // the path being painted is filled by the even-odd rule (f*, B*, b*)
    Matrix base_;               // the page's own transform (patterns are drawn in it)
    Font fallback_;
    std::size_t operations_ = 0;
};

// ---- the document -----------------------------------------------------------------------------

struct PageInfo {
    const Object* dict = nullptr;
    const Object* resources = nullptr;
    Matrix base;
};

class Source : public PageSource {
public:
    File file;
    std::vector<PageInfo> pages;
    std::map<const Object*, std::unique_ptr<Font>> fonts;
    // The pictures the pages read so far draw, by key: a few pointers each, for the document's life.
    std::map<std::string, PictureRef> pictures;

    // The reader service reads a page's content on one thread and its pictures on others: the
    // file's object cache, the fonts and the pictures' table are used by one at a time.
    std::mutex mutex;

    bool load(std::size_t index, Page& page) override
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (index >= pages.size())
            return false;
        PageReader reader(file, page, pictures, fonts);
        reader.run(*pages[index].dict, pages[index].resources, pages[index].base);
        return true;
    }

    std::string loadImage(const std::string& key) override
    {
        std::lock_guard<std::mutex> lock(mutex);
        const auto it = pictures.find(key);
        if (it == pictures.end())
            return {};
        Page scratch;
        PageReader reader(file, scratch, pictures, fonts);
        return reader.pictureBytes(it->second);
    }

    // The page tree, depth first, with inherited attributes.
    void collect(const Object& node, const Object* resources, const Object* mediaBox, const Object* cropBox, int rotate, int depth,
                 ViewerDocument& doc)
    {
        if (depth > 64 || pages.size() >= 20000)
            return;
        if (const Object* r = file.resolvedGet(node, "Resources"))
            resources = r;
        if (const Object* m = file.resolvedGet(node, "MediaBox"))
            mediaBox = m;
        if (const Object* c = file.resolvedGet(node, "CropBox"))
            cropBox = c;
        if (const Object* r = file.resolvedGet(node, "Rotate"))
            rotate = int(r->num());
        const Object* type = file.resolvedGet(node, "Type");
        const Object* kids = file.resolvedGet(node, "Kids");
        if (kids && kids->type == Object::Type::Array && !(type && type->isName("Page"))) {
            for (const Object& kid : kids->array) {
                const Object& k = file.resolve(kid);
                if (k.isDict() && &k != &node)
                    collect(k, resources, mediaBox, cropBox, rotate, depth + 1, doc);
            }
            return;
        }
        if (!node.isDict())
            return;
        double box[4] = {0, 0, 612, 792};
        auto readBox = [&](const Object* b, double* out) {
            if (!b || b->type != Object::Type::Array || b->array.size() < 4)
                return false;
            double v[4];
            for (int i = 0; i < 4; ++i)
                v[i] = file.resolve(b->array[size_t(i)]).num();
            out[0] = std::min(v[0], v[2]);
            out[1] = std::min(v[1], v[3]);
            out[2] = std::max(v[0], v[2]);
            out[3] = std::max(v[1], v[3]);
            return out[2] - out[0] >= 1 && out[3] - out[1] >= 1;
        };
        if (!readBox(mediaBox, box)) {
            box[0] = 0;
            box[1] = 0;
            box[2] = 612;
            box[3] = 792;
        }
        double crop[4];
        if (readBox(cropBox, crop)) {
            box[0] = std::max(box[0], crop[0]);
            box[1] = std::max(box[1], crop[1]);
            box[2] = std::min(box[2], crop[2]);
            box[3] = std::min(box[3], crop[3]);
            if (box[2] - box[0] < 1 || box[3] - box[1] < 1)
                readBox(mediaBox, box);
        }
        rotate = ((rotate % 360) + 360) % 360;
        const double x0 = box[0], y0 = box[1], x1 = box[2], y1 = box[3];
        PageInfo info;
        info.dict = &node;
        info.resources = resources;
        Page page;
        page.loaded = false;
        if (rotate == 90) {
            info.base = Matrix{0, 1, 1, 0, -y0, -x0};
            page.width = float(y1 - y0);
            page.height = float(x1 - x0);
        } else if (rotate == 180) {
            info.base = Matrix{-1, 0, 0, 1, x1, -y0};
            page.width = float(x1 - x0);
            page.height = float(y1 - y0);
        } else if (rotate == 270) {
            info.base = Matrix{0, -1, -1, 0, y1, x1};
            page.width = float(y1 - y0);
            page.height = float(x1 - x0);
        } else {
            info.base = Matrix{1, 0, 0, -1, -x0, y1};
            page.width = float(x1 - x0);
            page.height = float(y1 - y0);
        }
        pages.push_back(info);
        page.title = "Page " + std::to_string(pages.size());
        if (node.get("Annots"))
            ++annotated;
        doc.pages.pages.push_back(std::move(page));
    }

    int annotated = 0;
};

} // namespace
} // namespace pdf

ViewerDocument readPdf(std::string bytes)
{
    ViewerDocument doc;
    doc.format = "PDF document";
    auto source = std::make_shared<pdf::Source>();
    std::string error;
    if (!source->file.open(std::move(bytes), &error)) {
        doc.error = error;
        return doc;
    }
    if (source->file.encrypted()) {
        doc.error = "the PDF is encrypted (a password or permissions): encrypted PDFs are not read yet";
        return doc;
    }
    const pdf::Object* root = source->file.resolvedGet(source->file.trailer(), "Root");
    const pdf::Object* pages = root ? source->file.resolvedGet(*root, "Pages") : nullptr;
    if (!pages) {
        doc.error = "the PDF has no pages (damaged page tree)";
        return doc;
    }
    source->collect(*pages, nullptr, nullptr, nullptr, 0, 0, doc);
    if (doc.pages.pages.empty()) {
        doc.error = "the PDF has no pages";
        return doc;
    }
    if (source->annotated)
        doc.notShown.push_back("annotations and form fields");
    doc.pages.source = std::move(source);
    doc.shape = Shape::Pages;
    return doc;
}

} // namespace enp::model::viewer
