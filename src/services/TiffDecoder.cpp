#include "TiffDecoder.h"

#include "enp/os/Memory.h"

#include <libdeflate.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>

namespace enp::services::tiff {

namespace {

constexpr int kMaxSide = 1 << 14; // as stb_image's limit for the other formats

// ---- The file: byte order, tags --------------------------------------------------------------

struct File {
    const unsigned char* data = nullptr;
    size_t size = 0;
    bool le = true;
    uint32_t u16(size_t at) const
    {
        if (at + 2 > size)
            return 0;
        return le ? uint32_t(data[at] | (data[at + 1] << 8)) : uint32_t((data[at] << 8) | data[at + 1]);
    }
    uint32_t u32(size_t at) const
    {
        if (at + 4 > size)
            return 0;
        const unsigned char* p = data + at;
        return le ? uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24
                  : uint32_t(p[3]) | uint32_t(p[2]) << 8 | uint32_t(p[1]) << 16 | uint32_t(p[0]) << 24;
    }
};

struct Tag {
    uint32_t type = 0, count = 0;
    size_t at = 0; // where the value is: in the entry when it fits in 4 bytes, else where the entry points
};

// A tag's values as integers (BYTE, SHORT, LONG); empty when it is absent.
std::vector<uint32_t> values(const File& f, const Tag* t)
{
    std::vector<uint32_t> v;
    if (!t)
        return v;
    const int size = t->type == 1 || t->type == 6 || t->type == 7 ? 1 : t->type == 3 || t->type == 8 ? 2 : t->type == 4 || t->type == 9 ? 4 : 0;
    if (size == 0 || t->count > (1u << 24))
        return v;
    v.reserve(t->count);
    for (uint32_t i = 0; i < t->count; ++i) {
        const size_t at = t->at + size_t(i) * size_t(size);
        if (at + size_t(size) > f.size)
            break;
        v.push_back(size == 1 ? f.data[at] : size == 2 ? f.u16(at) : f.u32(at));
    }
    return v;
}

// ---- Bits, most significant first --------------------------------------------------------------

struct Bits {
    const unsigned char* data;
    size_t size;
    size_t pos = 0; // in bits
    bool more() const { return pos < size * 8; }
    uint32_t peek(int n) const
    {
        uint32_t v = 0;
        for (int i = 0; i < n; ++i) {
            const size_t p = pos + size_t(i);
            v = (v << 1) | (p < size * 8 ? (data[p >> 3] >> (7 - (p & 7))) & 1u : 0u);
        }
        return v;
    }
    uint32_t read(int n)
    {
        const uint32_t v = peek(n);
        pos += size_t(n);
        return v;
    }
    void alignByte() { pos = (pos + 7) & ~size_t(7); }
};

// ---- PackBits, LZW, Deflate --------------------------------------------------------------------

bool packBits(const unsigned char* in, size_t n, std::vector<unsigned char>& out, size_t want)
{
    size_t i = 0;
    while (i < n && out.size() < want) {
        const int c = static_cast<signed char>(in[i++]);
        if (c >= 0) {
            const size_t len = std::min(size_t(c) + 1, n - i);
            out.insert(out.end(), in + i, in + i + len);
            i += len;
        } else if (c != -128 && i < n) {
            out.insert(out.end(), size_t(1 - c), in[i++]);
        }
    }
    return true;
}

bool lzw(const unsigned char* in, size_t n, std::vector<unsigned char>& out, size_t want)
{
    if (n >= 2 && in[0] == 0 && (in[1] & 1)) // the old, least-significant-bit-first variant
        return false;
    std::vector<uint16_t> prefix(4096), length(4096);
    std::vector<unsigned char> suffix(4096), first(4096);
    for (int k = 0; k < 256; ++k)
        suffix[size_t(k)] = first[size_t(k)] = static_cast<unsigned char>(k), length[size_t(k)] = 1;
    Bits b{in, n};
    int width = 9, next = 258, old = -1;
    auto emit = [&](int code) {
        const size_t len = length[size_t(code)];
        const size_t at = out.size();
        out.resize(at + len);
        for (size_t i = len; i-- > 0;) {
            out[at + i] = suffix[size_t(code)];
            code = prefix[size_t(code)];
        }
    };
    while (out.size() < want && b.pos + size_t(width) <= n * 8) {
        const int code = int(b.read(width));
        if (code == 256) {
            width = 9, next = 258, old = -1;
            continue;
        }
        if (code == 257)
            break;
        if (old < 0) {
            if (code > 255)
                return false;
            emit(code);
            old = code;
            continue;
        }
        if (code > next || next >= 4096)
            return false;
        const unsigned char head = code < next ? first[size_t(code)] : first[size_t(old)];
        if (code < next)
            emit(code);
        prefix[size_t(next)] = uint16_t(old);
        suffix[size_t(next)] = head;
        first[size_t(next)] = first[size_t(old)];
        length[size_t(next)] = uint16_t(length[size_t(old)] + 1);
        if (code == next)
            emit(code);
        ++next;
        old = code;
        if (next + 1 >= (1 << width) && width < 12) // TIFF's early change
            ++width;
    }
    return true;
}

bool inflate(const unsigned char* in, size_t n, std::vector<unsigned char>& out, size_t want)
{
    std::unique_ptr<libdeflate_decompressor, void (*)(libdeflate_decompressor*)> d(libdeflate_alloc_decompressor(),
                                                                                   libdeflate_free_decompressor);
    if (!d)
        return false;
    out.resize(want);
    size_t got = 0;
    libdeflate_result r = libdeflate_zlib_decompress(d.get(), in, n, out.data(), want, &got);
    if (r != LIBDEFLATE_SUCCESS && r != LIBDEFLATE_SHORT_OUTPUT && r != LIBDEFLATE_INSUFFICIENT_SPACE)
        r = libdeflate_deflate_decompress(d.get(), in, n, out.data(), want, &got); // a raw stream
    if (r != LIBDEFLATE_SUCCESS && r != LIBDEFLATE_SHORT_OUTPUT && r != LIBDEFLATE_INSUFFICIENT_SPACE)
        return false;
    out.resize(r == LIBDEFLATE_INSUFFICIENT_SPACE ? want : got);
    return true;
}

// ---- CCITT fax: Modified Huffman, Group 3, Group 4 (ITU-T T.4, T.6) -----------------------------

struct Code {
    const char* bits;
    int run;
};

// The run-length codes: white, black, and the long make-up codes both share.
const Code kWhite[] = {
    {"00110101", 0},   {"000111", 1},     {"0111", 2},       {"1000", 3},       {"1011", 4},       {"1100", 5},
    {"1110", 6},       {"1111", 7},       {"10011", 8},      {"10100", 9},      {"00111", 10},     {"01000", 11},
    {"001000", 12},    {"000011", 13},    {"110100", 14},    {"110101", 15},    {"101010", 16},    {"101011", 17},
    {"0100111", 18},   {"0001100", 19},   {"0001000", 20},   {"0010111", 21},   {"0000011", 22},   {"0000100", 23},
    {"0101000", 24},   {"0101011", 25},   {"0010011", 26},   {"0100100", 27},   {"0011000", 28},   {"00000010", 29},
    {"00000011", 30},  {"00011010", 31},  {"00011011", 32},  {"00010010", 33},  {"00010011", 34},  {"00010100", 35},
    {"00010101", 36},  {"00010110", 37},  {"00010111", 38},  {"00101000", 39},  {"00101001", 40},  {"00101010", 41},
    {"00101011", 42},  {"00101100", 43},  {"00101101", 44},  {"00000100", 45},  {"00000101", 46},  {"00001010", 47},
    {"00001011", 48},  {"01010010", 49},  {"01010011", 50},  {"01010100", 51},  {"01010101", 52},  {"00100100", 53},
    {"00100101", 54},  {"01011000", 55},  {"01011001", 56},  {"01011010", 57},  {"01011011", 58},  {"01001010", 59},
    {"01001011", 60},  {"00110010", 61},  {"00110011", 62},  {"00110100", 63},  {"11011", 64},     {"10010", 128},
    {"010111", 192},   {"0110111", 256},  {"00110110", 320}, {"00110111", 384}, {"01100100", 448}, {"01100101", 512},
    {"01101000", 576}, {"01100111", 640}, {"011001100", 704}, {"011001101", 768}, {"011010010", 832}, {"011010011", 896},
    {"011010100", 960}, {"011010101", 1024}, {"011010110", 1088}, {"011010111", 1152}, {"011011000", 1216},
    {"011011001", 1280}, {"011011010", 1344}, {"011011011", 1408}, {"010011000", 1472}, {"010011001", 1536},
    {"010011010", 1600}, {"011000", 1664}, {"010011011", 1728},
};
const Code kBlack[] = {
    {"0000110111", 0},    {"010", 1},           {"11", 2},            {"10", 3},            {"011", 4},
    {"0011", 5},          {"0010", 6},          {"00011", 7},         {"000101", 8},        {"000100", 9},
    {"0000100", 10},      {"0000101", 11},      {"0000111", 12},      {"00000100", 13},     {"00000111", 14},
    {"000011000", 15},    {"0000010111", 16},   {"0000011000", 17},   {"0000001000", 18},   {"00001100111", 19},
    {"00001101000", 20},  {"00001101100", 21},  {"00000110111", 22},  {"00000101000", 23},  {"00000010111", 24},
    {"00000011000", 25},  {"000011001010", 26}, {"000011001011", 27}, {"000011001100", 28}, {"000011001101", 29},
    {"000001101000", 30}, {"000001101001", 31}, {"000001101010", 32}, {"000001101011", 33}, {"000011010010", 34},
    {"000011010011", 35}, {"000011010100", 36}, {"000011010101", 37}, {"000011010110", 38}, {"000011010111", 39},
    {"000001101100", 40}, {"000001101101", 41}, {"000011011010", 42}, {"000011011011", 43}, {"000001010100", 44},
    {"000001010101", 45}, {"000001010110", 46}, {"000001010111", 47}, {"000001100100", 48}, {"000001100101", 49},
    {"000001010010", 50}, {"000001010011", 51}, {"000000100100", 52}, {"000000110111", 53}, {"000000111000", 54},
    {"000000100111", 55}, {"000000101000", 56}, {"000001011000", 57}, {"000001011001", 58}, {"000000101011", 59},
    {"000000101100", 60}, {"000001011010", 61}, {"000001100110", 62}, {"000001100111", 63}, {"0000001111", 64},
    {"000011001000", 128}, {"000011001001", 192}, {"000001011011", 256}, {"000000110011", 320}, {"000000110100", 384},
    {"000000110101", 448}, {"0000001101100", 512}, {"0000001101101", 576}, {"0000001001010", 640},
    {"0000001001011", 704}, {"0000001001100", 768}, {"0000001001101", 832}, {"0000001110010", 896},
    {"0000001110011", 960}, {"0000001110100", 1024}, {"0000001110101", 1088}, {"0000001110110", 1152},
    {"0000001110111", 1216}, {"0000001010010", 1280}, {"0000001010011", 1344}, {"0000001010100", 1408},
    {"0000001010101", 1472}, {"0000001011010", 1536}, {"0000001011011", 1600}, {"0000001100100", 1664},
    {"0000001100101", 1728},
};
const Code kLong[] = {
    {"00000001000", 1792},  {"00000001100", 1856},  {"00000001101", 1920},  {"000000010010", 1984}, {"000000010011", 2048},
    {"000000010100", 2112}, {"000000010101", 2176}, {"000000010110", 2240}, {"000000010111", 2304}, {"000000011100", 2368},
    {"000000011101", 2432}, {"000000011110", 2496}, {"000000011111", 2560},
};

constexpr int kLookup = 13; // the longest code's bits

struct Entry {
    int16_t run = -1;
    uint8_t bits = 0;
};

// For each colour, the code read from the next 13 bits.
struct Tables {
    std::vector<Entry> table[2]; // 0 white, 1 black
    Tables()
    {
        for (int c = 0; c < 2; ++c) {
            table[c].assign(size_t(1) << kLookup, Entry{});
            auto add = [&](const Code* codes, size_t n) {
                for (size_t i = 0; i < n; ++i) {
                    const int len = int(std::strlen(codes[i].bits));
                    uint32_t v = 0;
                    for (int k = 0; k < len; ++k)
                        v = (v << 1) | uint32_t(codes[i].bits[k] == '1');
                    const uint32_t from = v << (kLookup - len), count = 1u << (kLookup - len);
                    for (uint32_t s = 0; s < count; ++s)
                        table[c][from + s] = Entry{int16_t(codes[i].run), uint8_t(len)};
                }
            };
            if (c == 0)
                add(kWhite, sizeof(kWhite) / sizeof(kWhite[0]));
            else
                add(kBlack, sizeof(kBlack) / sizeof(kBlack[0]));
            add(kLong, sizeof(kLong) / sizeof(kLong[0]));
        }
    }
};

// One run of a colour: make-up codes, then the terminating one. -1 when the bits are no code.
int readRun(Bits& b, const Tables& t, int colour)
{
    int total = 0;
    for (int guard = 0; guard < 64; ++guard) {
        const Entry e = t.table[colour][b.peek(kLookup)];
        if (e.run < 0 || !b.more())
            return -1;
        b.pos += e.bits;
        total += e.run;
        if (e.run < 64)
            return total;
    }
    return -1;
}

enum class Mode { Pass, Horizontal, V0, VR1, VR2, VR3, VL1, VL2, VL3, Bad };

Mode readMode(Bits& b)
{
    const uint32_t v = b.peek(7);
    struct M {
        uint32_t code;
        int len;
        Mode mode;
    };
    static const M modes[] = {{1, 1, Mode::V0},          {3, 3, Mode::VR1},  {2, 3, Mode::VL1},  {1, 3, Mode::Horizontal},
                              {1, 4, Mode::Pass},        {3, 6, Mode::VR2},  {2, 6, Mode::VL2},  {3, 7, Mode::VR3},
                              {2, 7, Mode::VL3}};
    for (const M& m : modes)
        if ((v >> (7 - m.len)) == m.code) {
            b.pos += size_t(m.len);
            return m.mode;
        }
    return Mode::Bad;
}

// An end-of-line code (with fill bits before it): skipped. True when there was one.
bool skipEol(Bits& b)
{
    const size_t start = b.pos;
    int zeros = 0;
    while (b.more() && b.peek(1) == 0 && zeros < 4096) {
        ++b.pos;
        ++zeros;
    }
    if (zeros >= 11 && b.more()) {
        ++b.pos; // the 1 that ends it
        return true;
    }
    b.pos = start;
    return false;
}

// Decodes `rows` rows of `width` pixels into one byte a pixel: 1 black, 0 white.
// kind: 2 Modified Huffman, 3 Group 3 (options: bit 0 two-dimensional), 4 Group 4.
bool ccitt(const unsigned char* in, size_t n, int kind, uint32_t options, int width, int rows, std::vector<unsigned char>& black)
{
    static const Tables tables;
    Bits b{in, n};
    black.assign(size_t(width) * size_t(rows), 0);
    std::vector<int> ref{width, width}, cur; // changing elements: where each new colour starts
    cur.reserve(size_t(width) + 4);
    const bool twoD = kind == 4 || (kind == 3 && (options & 1));
    for (int y = 0; y < rows; ++y) {
        bool oneD = !twoD;
        if (kind == 3) {
            skipEol(b);
            if (twoD) // a tag bit after the end of line: 1 the row is one-dimensional
                oneD = b.read(1) == 1;
        }
        cur.clear();
        if (oneD) {
            int a0 = 0, colour = 0;
            while (a0 < width) {
                const int run = readRun(b, tables, colour);
                if (run < 0)
                    return y > 0; // what was decoded stays (a damaged end)
                a0 = std::min(width, a0 + run);
                cur.push_back(a0);
                colour ^= 1;
            }
        } else {
            int a0 = -1, colour = 0;
            size_t i = 0; // where b1 is searched from in the reference line
            for (int steps = 0; a0 < width; ++steps) {
                if (steps > 2 * width + 8)
                    return y > 0; // damaged: no progress along the row
                // b1: the first change on the reference line right of a0 to the other colour; b2 the next.
                while (i > 0 && ref[i - 1] > a0)
                    --i;
                while ((ref[i] <= a0 || (i & 1) != size_t(colour)) && ref[i] < width)
                    ++i;
                const int b1 = ref[i], b2 = ref[std::min(i + 1, ref.size() - 1)];
                const Mode m = readMode(b);
                if (m == Mode::Bad)
                    return y > 0;
                if (m == Mode::Pass) {
                    a0 = b2;
                } else if (m == Mode::Horizontal) {
                    const int start = std::max(a0, 0);
                    const int r1 = readRun(b, tables, colour), r2 = r1 < 0 ? -1 : readRun(b, tables, colour ^ 1);
                    if (r2 < 0)
                        return y > 0;
                    const int a1 = std::min(width, start + r1), a2 = std::min(width, a1 + r2);
                    cur.push_back(a1);
                    cur.push_back(a2);
                    a0 = a2;
                } else {
                    const int d = m == Mode::V0 ? 0 : m == Mode::VR1 ? 1 : m == Mode::VR2 ? 2 : m == Mode::VR3 ? 3
                                : m == Mode::VL1 ? -1 : m == Mode::VL2 ? -2 : -3;
                    const int a1 = std::clamp(b1 + d, std::max(a0, 0), width);
                    cur.push_back(a1);
                    a0 = a1;
                    colour ^= 1;
                }
            }
        }
        // The row's pixels from its changes.
        unsigned char* row = black.data() + size_t(y) * size_t(width);
        int x = 0, colour = 0;
        for (int at : cur) {
            at = std::clamp(at, x, width);
            if (colour)
                std::fill(row + x, row + at, 1);
            x = at;
            colour ^= 1;
        }
        if (colour && x < width)
            std::fill(row + x, row + width, 1);
        ref = cur;
        while (!ref.empty() && ref.back() >= width)
            ref.pop_back();
        ref.push_back(width);
        ref.push_back(width);
        if (kind == 2)
            b.alignByte(); // Modified Huffman: each row starts on a byte
    }
    return true;
}

} // namespace

bool decode(std::string_view bytes, std::vector<unsigned char>& rgba, int& width, int& height, std::string& note)
{
    os::mem::Scope scope("images");
    File f{reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size()};
    if (f.size < 8 || !((f.data[0] == 'I' && f.data[1] == 'I') || (f.data[0] == 'M' && f.data[1] == 'M'))) {
        note = "not a TIFF file";
        return false;
    }
    f.le = f.data[0] == 'I';
    if (f.u16(2) == 43) {
        note = "BigTIFF is not supported yet";
        return false;
    }
    const size_t ifd = f.u32(4);
    const uint32_t count = f.u16(ifd);
    if (ifd < 8 || ifd + 2 + size_t(count) * 12 > f.size) {
        note = "damaged TIFF: no picture directory";
        return false;
    }
    std::vector<std::pair<uint32_t, Tag>> tags;
    for (uint32_t i = 0; i < count; ++i) {
        const size_t e = ifd + 2 + size_t(i) * 12;
        Tag t;
        t.type = f.u16(e + 2);
        t.count = f.u32(e + 4);
        const int size = t.type == 3 || t.type == 8 ? 2 : t.type == 4 || t.type == 9 || t.type == 11 ? 4 : t.type == 5 || t.type == 12 ? 8 : 1;
        t.at = uint64_t(size) * t.count <= 4 ? e + 8 : f.u32(e + 8);
        tags.emplace_back(f.u16(e), t);
    }
    auto find = [&](uint32_t id) -> const Tag* {
        for (const auto& [k, t] : tags)
            if (k == id)
                return &t;
        return nullptr;
    };
    auto one = [&](uint32_t id, uint32_t fallback) {
        const std::vector<uint32_t> v = values(f, find(id));
        return v.empty() ? fallback : v[0];
    };
    width = int(one(256, 0));
    height = int(one(257, 0));
    if (width <= 0 || height <= 0 || width > kMaxSide || height > kMaxSide) {
        note = width <= 0 || height <= 0 ? "damaged TIFF: no size" : "too large (more than 16384 pixels across)";
        return false;
    }
    const int spp = int(std::clamp<uint32_t>(one(277, 1), 1, 8));
    const int bps = int(one(258, 1));
    const uint32_t compression = one(259, 1);
    const uint32_t photometric = one(262, spp >= 3 ? 2 : 1);
    const bool reversed = one(266, 1) == 2; // fill order: bits from the least significant
    const bool planar = one(284, 1) == 2 && spp > 1;
    const uint32_t predictor = one(317, 1);
    const std::vector<uint32_t> extra = values(f, find(338));
    if (one(339, 1) == 3) {
        note = "TIFF with floating-point samples is not supported yet";
        return false;
    }
    if (compression == 6 || compression == 7) {
        note = "JPEG-compressed TIFF is not supported yet";
        return false;
    }
    if (compression != 1 && compression != 2 && compression != 3 && compression != 4 && compression != 5 && compression != 8 &&
        compression != 32946 && compression != 32773) {
        note = "TIFF compression " + std::to_string(compression) + " is not supported yet";
        return false;
    }
    if (bps != 1 && bps != 2 && bps != 4 && bps != 8 && bps != 16) {
        note = std::to_string(bps) + "-bit TIFF samples are not supported yet";
        return false;
    }
    if (photometric != 0 && photometric != 1 && photometric != 2 && photometric != 3 && photometric != 5) {
        note = photometric == 6 ? "YCbCr TIFF is not supported yet" : "TIFF colour type " + std::to_string(photometric) + " is not supported yet";
        return false;
    }
    if ((compression >= 2 && compression <= 4) && (bps != 1 || spp != 1)) {
        note = "damaged TIFF: fax compression of a picture that is not black and white";
        return false;
    }
    // Strips, or tiles.
    const bool tiled = find(322) && find(324);
    const int chunkW = tiled ? int(one(322, 0)) : width;
    const int chunkH = tiled ? int(one(323, 0)) : int(std::min<uint32_t>(one(278, uint32_t(height)), uint32_t(height)));
    const std::vector<uint32_t> offsets = values(f, find(tiled ? 324 : 273));
    std::vector<uint32_t> lengths = values(f, find(tiled ? 325 : 279));
    if (chunkW <= 0 || chunkH <= 0 || offsets.empty()) {
        note = "damaged TIFF: no picture data";
        return false;
    }
    const int across = tiled ? (width + chunkW - 1) / chunkW : 1;
    const int down = (height + chunkH - 1) / chunkH;
    const int planes = planar ? spp : 1;
    const int chunkSpp = planar ? 1 : spp;
    const size_t chunkRow = (size_t(chunkW) * size_t(chunkSpp) * size_t(bps) + 7) / 8;
    const size_t imageRow = (size_t(width) * size_t(chunkSpp) * size_t(bps) + 7) / 8;
    if (offsets.size() < size_t(across) * size_t(down) * size_t(planes)) {
        note = "damaged TIFF: missing parts of the picture";
        return false;
    }
    // Each plane's samples, rows of imageRow bytes (as stored: packed bits, or 16-bit in file order).
    std::vector<std::vector<unsigned char>> plane(size_t(planes), std::vector<unsigned char>(imageRow * size_t(height)));
    std::vector<unsigned char> raw, input;
    const uint32_t t4 = one(292, 0);
    for (int p = 0; p < planes; ++p)
        for (int cy = 0; cy < down; ++cy)
            for (int cx = 0; cx < across; ++cx) {
                const size_t index = (size_t(p) * size_t(down) + size_t(cy)) * size_t(across) + size_t(cx);
                const size_t at = offsets[index];
                size_t len = index < lengths.size() ? lengths[index] : f.size - std::min<size_t>(at, f.size);
                if (at >= f.size)
                    continue;
                len = std::min(len, f.size - at);
                const unsigned char* src = f.data + at;
                if (reversed) {
                    input.assign(src, src + len);
                    for (unsigned char& c : input) {
                        c = static_cast<unsigned char>(((c * 0x0802u & 0x22110u) | (c * 0x8020u & 0x88440u)) * 0x10101u >> 16);
                    }
                    src = input.data();
                }
                const int rows = tiled ? chunkH : std::min(chunkH, height - cy * chunkH);
                const size_t want = chunkRow * size_t(rows);
                raw.clear();
                bool ok = true;
                if (compression == 1) {
                    raw.assign(src, src + std::min(len, want));
                } else if (compression == 32773) {
                    ok = packBits(src, len, raw, want);
                } else if (compression == 5) {
                    ok = lzw(src, len, raw, want);
                } else if (compression == 8 || compression == 32946) {
                    ok = inflate(src, len, raw, want);
                } else {
                    std::vector<unsigned char> black;
                    ok = ccitt(src, len, int(compression), t4, chunkW, rows, black);
                    // Bits as stored: 1 is black where white is zero, else 0 is black.
                    raw.assign(want, 0);
                    for (int y = 0; y < rows; ++y)
                        for (int x = 0; x < chunkW; ++x)
                            if ((black[size_t(y) * size_t(chunkW) + size_t(x)] != 0) == (photometric == 0))
                                raw[size_t(y) * chunkRow + size_t(x >> 3)] |= static_cast<unsigned char>(0x80 >> (x & 7));
                }
                if (!ok) {
                    note = compression == 5 ? "damaged or old-style LZW in this TIFF" : "damaged TIFF picture data";
                    return false;
                }
                raw.resize(want, 0);
                // The horizontal predictor: each sample stored as the difference from the one before.
                if (predictor == 2 && (bps == 8 || bps == 16))
                    for (int y = 0; y < rows; ++y) {
                        unsigned char* r = raw.data() + size_t(y) * chunkRow;
                        if (bps == 8) {
                            for (size_t i = size_t(chunkSpp); i < size_t(chunkW) * size_t(chunkSpp); ++i)
                                r[i] = static_cast<unsigned char>(r[i] + r[i - size_t(chunkSpp)]);
                        } else {
                            for (size_t i = size_t(chunkSpp); i < size_t(chunkW) * size_t(chunkSpp); ++i) {
                                unsigned char* a = r + i * 2;
                                const unsigned char* b = a - size_t(chunkSpp) * 2;
                                const unsigned v = f.le ? unsigned(a[0] | a[1] << 8) + unsigned(b[0] | b[1] << 8)
                                                        : unsigned(a[0] << 8 | a[1]) + unsigned(b[0] << 8 | b[1]);
                                a[f.le ? 0 : 1] = static_cast<unsigned char>(v & 0xFF);
                                a[f.le ? 1 : 0] = static_cast<unsigned char>((v >> 8) & 0xFF);
                            }
                        }
                    }
                // Into the plane: the tile's or strip's rows, as far as the picture reaches.
                const size_t x0 = (size_t(cx) * size_t(chunkW) * size_t(chunkSpp) * size_t(bps)) / 8;
                const size_t copy = std::min(chunkRow, imageRow - std::min(imageRow, x0));
                for (int y = 0; y < rows && cy * chunkH + y < height; ++y)
                    std::memcpy(plane[size_t(p)].data() + size_t(cy * chunkH + y) * imageRow + x0, raw.data() + size_t(y) * chunkRow, copy);
            }
    // Samples to RGBA.
    const std::vector<uint32_t> map = values(f, find(320));
    const int colours = photometric == 2 ? 3 : photometric == 5 ? 4 : 1;
    const bool alpha = spp > colours && !extra.empty() && (extra[0] == 1 || extra[0] == 2);
    const bool premultiplied = alpha && extra[0] == 1;
    if (photometric == 3 && map.size() < size_t(3) << bps) {
        note = "damaged TIFF: palette missing";
        return false;
    }
    if (spp < colours) {
        note = "damaged TIFF: too few samples for its colours";
        return false;
    }
    const unsigned top = (1u << std::min(bps, 16)) - 1;
    auto sample = [&](int x, int y, int s) -> unsigned { // 0..top
        const std::vector<unsigned char>& pl = plane[size_t(planar ? s : 0)];
        const size_t index = size_t(x) * size_t(chunkSpp) + size_t(planar ? 0 : s);
        const unsigned char* row = pl.data() + size_t(y) * imageRow;
        if (bps == 8)
            return row[index];
        if (bps == 16)
            return f.le ? unsigned(row[index * 2] | row[index * 2 + 1] << 8) : unsigned(row[index * 2] << 8 | row[index * 2 + 1]);
        const size_t bit = index * size_t(bps);
        return (row[bit >> 3] >> (8 - bps - int(bit & 7))) & top;
    };
    auto to8 = [&](unsigned v) { return static_cast<unsigned char>(bps == 16 ? v >> 8 : bps == 8 ? v : v * 255 / top); };
    rgba.assign(size_t(width) * size_t(height) * 4, 255);
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x) {
            unsigned char* o = rgba.data() + (size_t(y) * size_t(width) + size_t(x)) * 4;
            if (photometric == 0 || photometric == 1) {
                unsigned char g = to8(sample(x, y, 0));
                if (photometric == 0)
                    g = static_cast<unsigned char>(255 - g);
                o[0] = o[1] = o[2] = g;
            } else if (photometric == 3) {
                const unsigned i = sample(x, y, 0);
                const size_t n = size_t(1) << bps;
                o[0] = static_cast<unsigned char>(map[i] >> 8), o[1] = static_cast<unsigned char>(map[n + i] >> 8),
                o[2] = static_cast<unsigned char>(map[2 * n + i] >> 8);
            } else if (photometric == 2) {
                for (int c = 0; c < 3; ++c)
                    o[c] = to8(sample(x, y, c));
            } else { // CMYK: ink taken away from white
                const unsigned k = to8(sample(x, y, 3));
                for (int c = 0; c < 3; ++c)
                    o[c] = static_cast<unsigned char>((255 - to8(sample(x, y, c))) * (255 - k) / 255);
            }
            if (alpha) {
                o[3] = to8(sample(x, y, colours));
                if (premultiplied && o[3] > 0 && o[3] < 255)
                    for (int c = 0; c < 3; ++c)
                        o[c] = static_cast<unsigned char>(std::min(255, o[c] * 255 / o[3]));
            }
        }
    return true;
}

} // namespace enp::services::tiff
