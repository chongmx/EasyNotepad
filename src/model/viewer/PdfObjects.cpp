#include "PdfObjects.h"

#include "enp/model/package/Inflate.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace enp::model::viewer::pdf {
namespace {

bool isWhite(char c)
{
    return c == ' ' || c == '\n' || c == '\r' || c == '\t' || c == '\f' || c == '\0';
}

bool isDelimiter(char c)
{
    return c == '(' || c == ')' || c == '<' || c == '>' || c == '[' || c == ']' || c == '{' || c == '}' || c == '/' || c == '%';
}

int hexValue(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

} // namespace

const Object* Object::get(std::string_view key) const
{
    if (!isDict())
        return nullptr;
    for (const DictEntry& e : dict)
        if (e.key == key)
            return &e.value;
    return nullptr;
}

// ---- lexer ------------------------------------------------------------------------------------

void Lexer::skipSpace()
{
    while (at_ < data_.size()) {
        const char c = data_[at_];
        if (isWhite(c)) {
            ++at_;
        } else if (c == '%') {
            while (at_ < data_.size() && data_[at_] != '\n' && data_[at_] != '\r')
                ++at_;
        } else {
            break;
        }
    }
}

bool Lexer::readString(std::string& out)
{
    int nesting = 1;
    ++at_; // (
    while (at_ < data_.size()) {
        char c = data_[at_++];
        if (c == '\\') {
            if (at_ >= data_.size())
                break;
            c = data_[at_++];
            switch (c) {
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case '\r':
                if (at_ < data_.size() && data_[at_] == '\n')
                    ++at_;
                break;
            case '\n': break;
            default:
                if (c >= '0' && c <= '7') {
                    int v = c - '0';
                    for (int k = 0; k < 2 && at_ < data_.size() && data_[at_] >= '0' && data_[at_] <= '7'; ++k)
                        v = v * 8 + (data_[at_++] - '0');
                    out += char(v & 0xFF);
                } else {
                    out += c;
                }
            }
            continue;
        }
        if (c == '(')
            ++nesting;
        else if (c == ')' && --nesting == 0)
            return true;
        out += c;
    }
    return true; // unterminated: what was read
}

bool Lexer::readHexString(std::string& out)
{
    ++at_; // <
    int high = -1;
    while (at_ < data_.size()) {
        const char c = data_[at_++];
        if (c == '>')
            break;
        const int v = hexValue(c);
        if (v < 0)
            continue;
        if (high < 0) {
            high = v;
        } else {
            out += char((high << 4) | v);
            high = -1;
        }
    }
    if (high >= 0)
        out += char(high << 4);
    return true;
}

std::string Lexer::readName()
{
    std::string name;
    ++at_; // /
    while (at_ < data_.size() && !isWhite(data_[at_]) && !isDelimiter(data_[at_])) {
        const char c = data_[at_++];
        if (c == '#' && at_ + 1 < data_.size() && hexValue(data_[at_]) >= 0 && hexValue(data_[at_ + 1]) >= 0) {
            name += char((hexValue(data_[at_]) << 4) | hexValue(data_[at_ + 1]));
            at_ += 2;
        } else {
            name += c;
        }
    }
    return name;
}

bool Lexer::next(Object& out, int depth)
{
    out = Object();
    skipSpace();
    if (at_ >= data_.size())
        return false;
    if (depth > 64) { // nested too deeply: the rest of this object is skipped
        at_ = data_.size();
        return false;
    }
    const char c = data_[at_];
    if (c == '/') {
        out.type = Object::Type::Name;
        out.text = readName();
        return true;
    }
    if (c == '(') {
        out.type = Object::Type::String;
        return readString(out.text);
    }
    if (c == '<') {
        if (at_ + 1 < data_.size() && data_[at_ + 1] == '<') {
            at_ += 2;
            out.type = Object::Type::Dict;
            for (;;) {
                skipSpace();
                if (at_ >= data_.size())
                    return true;
                if (data_[at_] == '>' && at_ + 1 < data_.size() && data_[at_ + 1] == '>') {
                    at_ += 2;
                    return true;
                }
                Object key;
                if (!next(key, depth + 1))
                    return true;
                if (key.type != Object::Type::Name) {
                    if (key.type == Object::Type::Operator && key.text == ">") // a lone '>': broken, stop
                        return true;
                    continue;
                }
                DictEntry entry;
                entry.key = std::move(key.text);
                skipSpace();
                if (at_ < data_.size() && data_[at_] == '>' && at_ + 1 < data_.size() && data_[at_ + 1] == '>') {
                    out.dict.push_back(std::move(entry)); // a key without a value
                    continue;
                }
                if (!next(entry.value, depth + 1))
                    return true;
                if (out.dict.size() < 4096)
                    out.dict.push_back(std::move(entry));
            }
        }
        out.type = Object::Type::String;
        return readHexString(out.text);
    }
    if (c == '[') {
        ++at_;
        out.type = Object::Type::Array;
        for (;;) {
            skipSpace();
            if (at_ >= data_.size())
                return true;
            if (data_[at_] == ']') {
                ++at_;
                return true;
            }
            Object item;
            if (!next(item, depth + 1))
                return true;
            if (out.array.size() < (std::size_t(1) << 20))
                out.array.push_back(std::move(item));
        }
    }
    if (c == ']' || c == '>' || c == ')' || c == '{' || c == '}') {
        ++at_;
        if (c == '>' && at_ < data_.size() && data_[at_] == '>')
            ++at_;
        out.type = Object::Type::Operator;
        out.text = std::string(1, c);
        return true;
    }
    if ((c >= '0' && c <= '9') || c == '+' || c == '-' || c == '.') {
        const std::size_t start = at_;
        ++at_;
        while (at_ < data_.size() && ((data_[at_] >= '0' && data_[at_] <= '9') || data_[at_] == '.' || data_[at_] == '-'))
            ++at_;
        const std::string token(data_.substr(start, at_ - start));
        out.type = Object::Type::Number;
        out.number = std::strtod(token.c_str(), nullptr);
        if (!std::isfinite(out.number))
            out.number = 0.0;
        // "12 0 R": an indirect reference.
        const bool integer = token.find('.') == std::string::npos && token[0] != '+' && token[0] != '-';
        if (integer) {
            const std::size_t save = at_;
            skipSpace();
            const std::size_t genStart = at_;
            while (at_ < data_.size() && data_[at_] >= '0' && data_[at_] <= '9')
                ++at_;
            if (at_ > genStart && at_ - genStart < 6) {
                const int gen = std::atoi(std::string(data_.substr(genStart, at_ - genStart)).c_str());
                skipSpace();
                if (at_ < data_.size() && data_[at_] == 'R' &&
                    (at_ + 1 >= data_.size() || isWhite(data_[at_ + 1]) || isDelimiter(data_[at_ + 1]))) {
                    ++at_;
                    out.type = Object::Type::Ref;
                    out.generation = gen;
                    return true;
                }
            }
            at_ = save;
        }
        return true;
    }
    // A keyword: true, false, null, or an operator.
    const std::size_t start = at_;
    while (at_ < data_.size() && !isWhite(data_[at_]) && !isDelimiter(data_[at_]))
        ++at_;
    if (at_ == start) { // a stray delimiter
        ++at_;
        out.type = Object::Type::Operator;
        out.text = std::string(1, c);
        return true;
    }
    const std::string_view word = data_.substr(start, at_ - start);
    if (word == "true" || word == "false") {
        out.type = Object::Type::Bool;
        out.boolean = word == "true";
    } else if (word == "null") {
        out.type = Object::Type::Null;
    } else {
        out.type = Object::Type::Operator;
        out.text = std::string(word);
    }
    return true;
}

// ---- filters ----------------------------------------------------------------------------------

namespace {

std::optional<std::string> unpredict(std::string data, const Object* parms)
{
    if (!parms || !parms->isDict())
        return data;
    const int predictor = int(parms->get("Predictor") ? parms->get("Predictor")->num(1) : 1);
    if (predictor < 2)
        return data;
    const int colors = std::clamp(int(parms->get("Colors") ? parms->get("Colors")->num(1) : 1), 1, 32);
    const int bpc = std::clamp(int(parms->get("BitsPerComponent") ? parms->get("BitsPerComponent")->num(8) : 8), 1, 16);
    const int columns = std::clamp(int(parms->get("Columns") ? parms->get("Columns")->num(1) : 1), 1, 1 << 20);
    const std::size_t bpp = std::size_t(std::max(1, colors * bpc / 8));
    const std::size_t rowBytes = (std::size_t(columns) * colors * bpc + 7) / 8;
    if (predictor == 2) { // TIFF: 8-bit components only
        if (bpc != 8)
            return data;
        for (std::size_t row = 0; row + rowBytes <= data.size(); row += rowBytes)
            for (std::size_t i = bpp; i < rowBytes; ++i)
                data[row + i] = char(std::uint8_t(data[row + i]) + std::uint8_t(data[row + i - bpp]));
        return data;
    }
    std::string out;
    out.reserve(data.size());
    std::string prev(rowBytes, '\0'), cur(rowBytes, '\0');
    for (std::size_t at = 0; at < data.size(); at += rowBytes + 1) {
        const int type = std::uint8_t(data[at]);
        const std::size_t n = std::min(rowBytes, data.size() - at - 1);
        for (std::size_t i = 0; i < rowBytes; ++i) {
            const std::uint8_t raw = i < n ? std::uint8_t(data[at + 1 + i]) : 0;
            const std::uint8_t left = i >= bpp ? std::uint8_t(cur[i - bpp]) : 0;
            const std::uint8_t up = std::uint8_t(prev[i]);
            const std::uint8_t upLeft = i >= bpp ? std::uint8_t(prev[i - bpp]) : 0;
            std::uint8_t v = raw;
            switch (type) {
            case 1: v = std::uint8_t(raw + left); break;
            case 2: v = std::uint8_t(raw + up); break;
            case 3: v = std::uint8_t(raw + ((int(left) + int(up)) >> 1)); break;
            case 4: {
                const int p = int(left) + int(up) - int(upLeft);
                const int pa = std::abs(p - int(left)), pb = std::abs(p - int(up)), pc = std::abs(p - int(upLeft));
                v = std::uint8_t(raw + (pa <= pb && pa <= pc ? left : pb <= pc ? up : upLeft));
                break;
            }
            default: break;
            }
            cur[i] = char(v);
        }
        out.append(cur, 0, n);
        std::swap(prev, cur);
    }
    return out;
}

std::optional<std::string> lzw(std::string_view in, bool earlyChange, std::size_t limit)
{
    std::string out;
    std::vector<std::string> table;
    auto reset = [&]() {
        table.clear();
        for (int i = 0; i < 256; ++i)
            table.emplace_back(1, char(i));
        table.emplace_back(); // 256 clear
        table.emplace_back(); // 257 end
    };
    reset();
    int bits = 9;
    std::uint32_t buffer = 0;
    int have = 0;
    std::string previous;
    for (std::size_t i = 0; i < in.size(); ++i) {
        buffer = (buffer << 8) | std::uint8_t(in[i]);
        have += 8;
        while (have >= bits) {
            const int code = int((buffer >> (have - bits)) & ((1u << bits) - 1));
            have -= bits;
            if (code == 256) {
                reset();
                bits = 9;
                previous.clear();
                continue;
            }
            if (code == 257)
                return out;
            std::string entry;
            if (code < int(table.size()) && (code < 256 || !table[size_t(code)].empty()))
                entry = table[size_t(code)];
            else if (code == int(table.size()) && !previous.empty())
                entry = previous + previous[0];
            else
                return out; // broken: what was decoded
            out += entry;
            if (out.size() > limit)
                return std::nullopt;
            if (!previous.empty() && table.size() < 4096)
                table.push_back(previous + entry[0]);
            previous = std::move(entry);
            const int next = int(table.size()) + (earlyChange ? 1 : 0);
            if (next >= (1 << bits) && bits < 12)
                ++bits;
        }
    }
    return out;
}

std::optional<std::string> ascii85(std::string_view in)
{
    std::string out;
    std::uint32_t tuple = 0;
    int count = 0;
    for (std::size_t i = 0; i < in.size(); ++i) {
        const char c = in[i];
        if (c == '~')
            break;
        if (isWhite(c))
            continue;
        if (c == 'z' && count == 0) {
            out.append(4, '\0');
            continue;
        }
        if (c < '!' || c > 'u')
            continue;
        tuple = tuple * 85 + std::uint32_t(c - '!');
        if (++count == 5) {
            for (int k = 3; k >= 0; --k)
                out += char((tuple >> (k * 8)) & 0xFF);
            tuple = 0;
            count = 0;
        }
    }
    if (count > 1) {
        for (int k = count; k < 5; ++k)
            tuple = tuple * 85 + 84;
        for (int k = 3; k >= 5 - count; --k)
            out += char((tuple >> (k * 8)) & 0xFF);
    }
    return out;
}

std::optional<std::string> asciiHex(std::string_view in)
{
    std::string out;
    int high = -1;
    for (char c : in) {
        if (c == '>')
            break;
        const int v = hexValue(c);
        if (v < 0)
            continue;
        if (high < 0) {
            high = v;
        } else {
            out += char((high << 4) | v);
            high = -1;
        }
    }
    if (high >= 0)
        out += char(high << 4);
    return out;
}

std::optional<std::string> runLength(std::string_view in)
{
    std::string out;
    for (std::size_t i = 0; i < in.size();) {
        const int n = std::uint8_t(in[i++]);
        if (n == 128)
            break;
        if (n < 128) {
            const std::size_t len = std::min<std::size_t>(std::size_t(n) + 1, in.size() - i);
            out.append(in.substr(i, len));
            i += len;
        } else if (i < in.size()) {
            out.append(std::size_t(257 - n), in[i++]);
        }
    }
    return out;
}

} // namespace

std::optional<std::string> decodeFilter(std::string_view name, std::string data, const Object* parms, std::size_t limit)
{
    if (name == "FlateDecode" || name == "Fl") {
        std::optional<std::string> out = package::inflate(data, true, limit);
        if (!out)
            out = package::inflate(data, false, limit); // a raw deflate stream without the zlib header
        if (!out)
            return std::nullopt;
        return unpredict(std::move(*out), parms);
    }
    if (name == "LZWDecode" || name == "LZW") {
        const bool early = !(parms && parms->get("EarlyChange") && parms->get("EarlyChange")->num(1) == 0);
        std::optional<std::string> out = lzw(data, early, limit);
        if (!out)
            return std::nullopt;
        return unpredict(std::move(*out), parms);
    }
    if (name == "ASCII85Decode" || name == "A85")
        return ascii85(data);
    if (name == "ASCIIHexDecode" || name == "AHx")
        return asciiHex(data);
    if (name == "RunLengthDecode" || name == "RL")
        return runLength(data);
    if (name == "Crypt")
        return data; // the identity crypt filter
    return std::nullopt;
}

// ---- the file ---------------------------------------------------------------------------------

bool File::open(std::string bytes, std::string* error)
{
    bytes_ = std::move(bytes);
    const std::size_t header = std::string_view(bytes_).substr(0, 1024).find("%PDF-");
    if (header == std::string_view::npos) {
        if (error)
            *error = "not a PDF file (no %PDF header)";
        return false;
    }
    bool ok = false;
    const std::size_t tail = bytes_.size() > 4096 ? bytes_.size() - 4096 : 0;
    const std::size_t sx = std::string_view(bytes_).rfind("startxref");
    if (sx != std::string_view::npos && sx >= tail) {
        Lexer lex(bytes_, sx + 9);
        Object offset;
        if (lex.next(offset) && offset.isNumber() && offset.number >= 0 && offset.number < double(bytes_.size()))
            ok = readXref(std::size_t(offset.number), 0);
    }
    if (!ok || !trailer_.get("Root"))
        rebuildXref();
    if (!trailer_.get("Root")) {
        if (error)
            *error = "the PDF has no document catalog (damaged file)";
        return false;
    }
    encrypted_ = trailer_.get("Encrypt") != nullptr && !trailer_.get("Encrypt")->isNull();
    return true;
}

bool File::readXref(std::size_t offset, int depth)
{
    if (depth > 32 || offset >= bytes_.size())
        return false;
    Lexer lex(bytes_, offset);
    lex.skipSpace();
    const std::size_t at = lex.position();
    if (std::string_view(bytes_).substr(at, 4) == "xref") {
        lex.seek(at + 4);
        if (!readXrefTable(lex))
            return false;
        Object trailer;
        if (!lex.next(trailer) || !trailer.isDict())
            return false;
        if (trailer_.isNull())
            trailer_ = trailer;
        if (const Object* stm = trailer.get("XRefStm"); stm && stm->isNumber())
            readXref(std::size_t(stm->number), depth + 1);
        if (const Object* prev = trailer.get("Prev"); prev && prev->isNumber())
            readXref(std::size_t(prev->number), depth + 1);
        return true;
    }
    // An xref stream: "n g obj << /Type /XRef ... >> stream".
    Object num, gen, kw;
    if (!lex.next(num) || !lex.next(gen) || !lex.next(kw) || kw.text != "obj")
        return false;
    Object dict;
    if (!lex.next(dict) || !dict.isDict())
        return false;
    Object streamKw;
    const std::size_t afterDict = lex.position();
    if (!lex.next(streamKw) || streamKw.text != "stream")
        return false;
    std::size_t start = lex.position();
    if (start < bytes_.size() && bytes_[start] == '\r')
        ++start;
    if (start < bytes_.size() && bytes_[start] == '\n')
        ++start;
    (void)afterDict;
    dict.type = Object::Type::Stream;
    dict.streamStart = start;
    dict.streamLength = streamLength(dict, start);
    if (!readXrefStream(dict))
        return false;
    if (trailer_.isNull()) {
        trailer_ = dict;
        trailer_.type = Object::Type::Dict;
    }
    if (const Object* prev = dict.get("Prev"); prev && prev->isNumber())
        readXref(std::size_t(prev->number), depth + 1);
    return true;
}

bool File::readXrefTable(Lexer& lex)
{
    for (int sections = 0; sections < 100000; ++sections) {
        Object first;
        const std::size_t save = lex.position();
        if (!lex.next(first))
            return false;
        if (first.type == Object::Type::Operator && first.text == "trailer")
            return true;
        Object count;
        if (!first.isNumber() || !lex.next(count) || !count.isNumber()) {
            lex.seek(save);
            return false;
        }
        const long start = long(first.number), n = long(count.number);
        if (n < 0 || n > 10000000)
            return false;
        for (long i = 0; i < n; ++i) {
            Object off, generation, type;
            if (!lex.next(off) || !lex.next(generation) || !lex.next(type))
                return false;
            const int number = int(start + i);
            if (type.text == "n" && off.number > 0 && !xref_.count(number))
                xref_[number] = XrefEntry{1, std::size_t(off.number), 0};
            else if (type.text == "f" && !xref_.count(number))
                xref_[number] = XrefEntry{0, 0, 0};
        }
    }
    return false;
}

bool File::readXrefStream(const Object& stream)
{
    std::optional<std::string> data = streamData(stream);
    const Object* w = stream.get("W");
    if (!data || !w || w->type != Object::Type::Array || w->array.size() < 3)
        return false;
    const int w0 = int(w->array[0].num()), w1 = int(w->array[1].num()), w2 = int(w->array[2].num());
    if (w0 < 0 || w1 < 0 || w2 < 0 || w0 > 8 || w1 > 8 || w2 > 8)
        return false;
    const std::size_t rowSize = std::size_t(w0 + w1 + w2);
    if (rowSize == 0)
        return false;
    std::vector<std::pair<long, long>> ranges;
    if (const Object* index = stream.get("Index"); index && index->type == Object::Type::Array) {
        for (std::size_t i = 0; i + 1 < index->array.size(); i += 2)
            ranges.push_back({long(index->array[i].num()), long(index->array[i + 1].num())});
    } else {
        ranges.push_back({0, long(stream.get("Size") ? stream.get("Size")->num() : 0)});
    }
    auto field = [&](std::size_t at, int width, std::uint64_t otherwise) {
        if (width == 0)
            return otherwise;
        std::uint64_t v = 0;
        for (int i = 0; i < width; ++i)
            v = (v << 8) | std::uint8_t((*data)[at + size_t(i)]);
        return v;
    };
    std::size_t at = 0;
    for (const auto& [start, count] : ranges) {
        for (long i = 0; i < count && at + rowSize <= data->size(); ++i, at += rowSize) {
            const int number = int(start + i);
            const std::uint64_t type = field(at, w0, 1);
            const std::uint64_t a = field(at + size_t(w0), w1, 0);
            const std::uint64_t b = field(at + size_t(w0 + w1), w2, 0);
            if (xref_.count(number))
                continue;
            if (type == 1)
                xref_[number] = XrefEntry{1, std::size_t(a), 0};
            else if (type == 2)
                xref_[number] = XrefEntry{2, std::size_t(a), int(b)};
            else
                xref_[number] = XrefEntry{0, 0, 0};
        }
    }
    return true;
}

void File::rebuildXref()
{
    xref_.clear();
    cache_.clear();
    const std::string_view b(bytes_);
    std::vector<int> objectStreams;
    for (std::size_t at = b.find("obj"); at != std::string_view::npos; at = b.find("obj", at + 3)) {
        if (at == 0 || !isWhite(b[at - 1]) || (at + 3 < b.size() && !isWhite(b[at + 3]) && !isDelimiter(b[at + 3])))
            continue;
        if (at >= 6 && b.substr(at - 3, 3) == "end")
            continue;
        // Back over "<num> <gen> ".
        std::size_t i = at - 1;
        while (i > 0 && isWhite(b[i]))
            --i;
        std::size_t genEnd = i + 1;
        while (i > 0 && b[i] >= '0' && b[i] <= '9')
            --i;
        if (genEnd == i + 1)
            continue;
        while (i > 0 && isWhite(b[i]))
            --i;
        const std::size_t numEnd = i + 1;
        while (i > 0 && b[i] >= '0' && b[i] <= '9')
            --i;
        const std::size_t numStart = (b[i] >= '0' && b[i] <= '9') ? i : i + 1;
        if (numStart >= numEnd)
            continue;
        const int number = std::atoi(std::string(b.substr(numStart, numEnd - numStart)).c_str());
        xref_[number] = XrefEntry{1, numStart, 0};
    }
    // The trailer: the last one in the file, else one that names the catalog.
    trailer_ = Object();
    for (std::size_t at = b.rfind("trailer"); at != std::string_view::npos; at = at > 0 ? b.rfind("trailer", at - 1) : std::string_view::npos) {
        Lexer lex(b, at + 7);
        Object t;
        if (lex.next(t) && t.isDict() && t.get("Root")) {
            trailer_ = t;
            break;
        }
        if (at == 0)
            break;
    }
    // Objects inside object streams, and the catalog when no trailer names it.
    std::vector<int> numbers;
    for (const auto& [n, e] : xref_)
        numbers.push_back(n);
    for (int n : numbers) {
        Object ref;
        ref.type = Object::Type::Ref;
        ref.number = n;
        const Object& o = resolve(ref);
        if (o.type == Object::Type::Stream && o.get("Type") && o.get("Type")->isName("ObjStm")) {
            std::optional<std::string> data = streamData(o);
            const int count = int(o.get("N") ? o.get("N")->num() : 0);
            if (!data)
                continue;
            Lexer lex(*data);
            for (int k = 0; k < count && k < 100000; ++k) {
                Object num, off;
                if (!lex.next(num) || !lex.next(off))
                    break;
                const int number = int(num.number);
                if (!xref_.count(number))
                    xref_[number] = XrefEntry{2, std::size_t(n), k};
            }
        } else if (trailer_.isNull() && o.isDict() && o.get("Type") && o.get("Type")->isName("Catalog")) {
            trailer_.type = Object::Type::Dict;
            DictEntry root;
            root.key = "Root";
            root.value = ref;
            trailer_.dict.push_back(root);
        }
    }
    if (trailer_.isNull()) // the catalog in an object stream
        for (const auto& [n, e] : xref_) {
            if (e.type != 2)
                continue;
            Object ref;
            ref.type = Object::Type::Ref;
            ref.number = n;
            const Object& o = resolve(ref);
            if (o.isDict() && o.get("Type") && o.get("Type")->isName("Catalog")) {
                trailer_.type = Object::Type::Dict;
                DictEntry root;
                root.key = "Root";
                root.value = ref;
                trailer_.dict.push_back(root);
                break;
            }
        }
}

std::size_t File::streamLength(const Object& dict, std::size_t start)
{
    const std::string_view b(bytes_);
    std::size_t length = std::string_view::npos;
    if (const Object* len = dict.get("Length")) {
        const Object& l = len->type == Object::Type::Ref && resolving_ < 8 ? resolve(*len) : *len;
        if (l.isNumber() && l.number >= 0)
            length = std::size_t(l.number);
    }
    if (length != std::string_view::npos && start + length <= b.size()) {
        std::size_t after = start + length;
        while (after < b.size() && isWhite(b[after]))
            ++after;
        if (b.substr(after, 9) == "endstream")
            return length;
    }
    // A wrong or missing /Length: up to "endstream".
    const std::size_t end = b.find("endstream", start);
    if (end == std::string_view::npos)
        return b.size() - std::min(start, b.size());
    std::size_t e = end;
    if (e > start && b[e - 1] == '\n')
        --e;
    if (e > start && b[e - 1] == '\r')
        --e;
    return e - start;
}

std::optional<Object> File::parseAt(std::size_t offset, int expected)
{
    if (offset >= bytes_.size())
        return std::nullopt;
    Lexer lex(bytes_, offset);
    Object num, gen, kw;
    if (!lex.next(num) || !lex.next(gen) || !lex.next(kw) || !num.isNumber() || kw.text != "obj")
        return std::nullopt;
    if (int(num.number) != expected)
        return std::nullopt;
    Object o;
    if (!lex.next(o))
        return std::nullopt;
    if (o.type == Object::Type::Operator && o.text == "endobj")
        return Object();
    if (o.isDict()) {
        const std::size_t save = lex.position();
        Object kw2;
        if (lex.next(kw2) && kw2.type == Object::Type::Operator && kw2.text == "stream") {
            std::size_t start = lex.position();
            if (start < bytes_.size() && bytes_[start] == '\r')
                ++start;
            if (start < bytes_.size() && bytes_[start] == '\n')
                ++start;
            o.type = Object::Type::Stream;
            o.streamStart = start;
            o.streamLength = streamLength(o, start);
        } else {
            lex.seek(save);
        }
    }
    return o;
}

std::optional<Object> File::fromObjectStream(int stream, int index, int number)
{
    auto data = objStmData_.find(stream);
    if (data == objStmData_.end()) {
        Object ref;
        ref.type = Object::Type::Ref;
        ref.number = stream;
        const Object& s = resolve(ref);
        if (s.type != Object::Type::Stream)
            return std::nullopt;
        std::optional<std::string> bytes = streamData(s);
        if (!bytes)
            return std::nullopt;
        const int count = int(s.get("N") ? s.get("N")->num() : 0);
        const std::size_t first = std::size_t(std::max(0.0, s.get("First") ? s.get("First")->num() : 0));
        std::vector<std::pair<int, std::size_t>>& idx = objStmIndex_[stream];
        Lexer lex(*bytes);
        for (int k = 0; k < count && k < 100000; ++k) {
            Object n, off;
            if (!lex.next(n) || !lex.next(off))
                break;
            idx.push_back({int(n.number), first + std::size_t(std::max(0.0, off.number))});
        }
        data = objStmData_.emplace(stream, std::move(*bytes)).first;
    }
    const std::vector<std::pair<int, std::size_t>>& idx = objStmIndex_[stream];
    std::size_t offset = std::string::npos;
    if (index >= 0 && std::size_t(index) < idx.size() && idx[size_t(index)].first == number)
        offset = idx[size_t(index)].second;
    else
        for (const auto& [n, off] : idx)
            if (n == number)
                offset = off;
    if (offset == std::string::npos || offset >= data->second.size())
        return std::nullopt;
    Lexer lex(data->second, offset);
    Object o;
    if (!lex.next(o))
        return std::nullopt;
    return o;
}

std::optional<Object> File::loadObject(int number)
{
    const auto it = xref_.find(number);
    if (it == xref_.end())
        return std::nullopt;
    if (it->second.type == 1)
        return parseAt(it->second.offset, number);
    if (it->second.type == 2)
        return fromObjectStream(int(it->second.offset), it->second.index, number);
    return std::nullopt;
}

const Object& File::resolve(const Object& o)
{
    if (o.type != Object::Type::Ref)
        return o;
    const int number = int(o.number);
    const auto hit = cache_.find(number);
    if (hit != cache_.end())
        return *hit->second;
    if (resolving_ > 32)
        return null_;
    ++resolving_;
    std::optional<Object> loaded = loadObject(number);
    --resolving_;
    auto slot = std::make_unique<Object>(loaded ? std::move(*loaded) : Object());
    const Object& ref = *slot;
    cache_[number] = std::move(slot);
    return ref;
}

const Object* File::resolvedGet(const Object& dict, std::string_view key)
{
    const Object* v = dict.get(key);
    if (!v)
        return nullptr;
    const Object& r = resolve(*v);
    return r.isNull() ? nullptr : &r;
}

std::optional<std::string> File::streamData(const Object& stream, std::string* imageFilter)
{
    if (stream.type != Object::Type::Stream || stream.streamStart > bytes_.size())
        return std::nullopt;
    std::string data = bytes_.substr(stream.streamStart, std::min(stream.streamLength, bytes_.size() - stream.streamStart));
    const Object* filter = resolvedGet(stream, "Filter");
    const Object* parms = resolvedGet(stream, "DecodeParms");
    if (!parms)
        parms = resolvedGet(stream, "DP");
    if (!filter)
        filter = resolvedGet(stream, "F");
    if (!filter)
        return data;
    std::vector<std::pair<std::string, const Object*>> chain;
    if (filter->type == Object::Type::Name) {
        chain.push_back({filter->text, parms && parms->isDict() ? parms : nullptr});
    } else if (filter->type == Object::Type::Array) {
        for (std::size_t i = 0; i < filter->array.size(); ++i) {
            const Object& f = resolve(filter->array[i]);
            const Object* p = nullptr;
            if (parms && parms->type == Object::Type::Array && i < parms->array.size()) {
                const Object& pp = resolve(parms->array[i]);
                p = pp.isDict() ? &pp : nullptr;
            }
            if (f.type == Object::Type::Name)
                chain.push_back({f.text, p});
        }
    }
    for (const auto& [name, p] : chain) {
        if (name == "DCTDecode" || name == "DCT" || name == "JPXDecode" || name == "CCITTFaxDecode" || name == "CCF" ||
            name == "JBIG2Decode") {
            if (imageFilter)
                *imageFilter = name == "DCT" ? "DCTDecode" : name == "CCF" ? "CCITTFaxDecode" : name;
            return data;
        }
        std::optional<std::string> out = decodeFilter(name, std::move(data), p, kMaxStreamBytes);
        if (!out)
            return std::nullopt;
        data = std::move(*out);
    }
    return data;
}

} // namespace enp::model::viewer::pdf
