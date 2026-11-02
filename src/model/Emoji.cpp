#include "enp/model/Emoji.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace enp::model::emoji {

namespace {

struct Range {
    char32_t first, last;
    unsigned char flags;
};
enum : unsigned char { kEmoji = 1, kPresentation = 2, kModifierBase = 4, kPictographic = 8 };

constexpr Range kRanges[] = {
#include "EmojiData.inc"
};

unsigned char flagsOf(char32_t cp)
{
    const Range* end = std::end(kRanges);
    const Range* it = std::upper_bound(std::begin(kRanges), end, cp, [](char32_t c, const Range& r) { return c < r.first; });
    if (it == std::begin(kRanges))
        return 0;
    --it;
    return cp <= it->last ? it->flags : 0;
}

constexpr char32_t kZwj = 0x200D, kVs15 = 0xFE0E, kVs16 = 0xFE0F, kKeycap = 0x20E3, kCancelTag = 0xE007F, kBlackFlag = 0x1F3F4;
bool isModifier(char32_t cp) { return cp >= 0x1F3FB && cp <= 0x1F3FF; }
bool isRegional(char32_t cp) { return cp >= 0x1F1E6 && cp <= 0x1F1FF; }
bool isTag(char32_t cp) { return cp >= 0xE0020 && cp <= 0xE007E; }
bool isKeycapBase(char32_t cp) { return (cp >= '0' && cp <= '9') || cp == '#' || cp == '*'; }

// The code point at text[pos] and its length; length 0 at the end or on invalid UTF-8.
char32_t decode(std::string_view text, size_t pos, size_t* length)
{
    *length = 0;
    if (pos >= text.size())
        return 0;
    const unsigned char c = static_cast<unsigned char>(text[pos]);
    size_t n = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : (c >> 3) == 0x1E ? 4 : 0;
    if (n == 0 || pos + n > text.size())
        return 0;
    char32_t cp = n == 1 ? c : n == 2 ? (c & 0x1F) : n == 3 ? (c & 0x0F) : (c & 0x07);
    for (size_t i = 1; i < n; ++i) {
        const unsigned char cc = static_cast<unsigned char>(text[pos + i]);
        if ((cc & 0xC0) != 0x80)
            return 0;
        cp = (cp << 6) | (cc & 0x3F);
    }
    *length = n;
    return cp;
}

void appendHex(std::string& key, char32_t cp)
{
    if (cp == kVs16)
        return;
    char hex[12];
    std::snprintf(hex, sizeof(hex), "%04x", unsigned(cp));
    if (!key.empty())
        key += '_';
    key += hex;
}

// One element of an emoji sequence (an emoji with its selector, skin tone or tags) at `pos`.
// Returns the bytes it spans, 0 if none; `*emoji` says whether it shows as an emoji by itself.
size_t element(std::string_view text, size_t pos, std::string& key, bool* emoji)
{
    size_t n = 0;
    const char32_t cp = decode(text, pos, &n);
    if (n == 0)
        return 0;
    const unsigned char f = flagsOf(cp);
    if (!(f & (kEmoji | kPictographic)) || isKeycapBase(cp))
        return 0;
    size_t end = pos + n;
    std::string local;
    appendHex(local, cp);
    bool presentation = (f & kPresentation) != 0;
    size_t m = 0;
    const char32_t next = decode(text, end, &m);
    if (next == kVs15) // asked to be text
        return 0;
    if (next == kVs16) {
        presentation = true;
        end += m;
    } else if ((f & kModifierBase) && isModifier(next)) {
        presentation = true;
        appendHex(local, next);
        end += m;
    }
    if (cp == kBlackFlag) { // tag sequence: subdivision flags (England, Scotland, Wales)
        size_t at = end;
        std::string tags;
        bool tagged = false;
        for (;;) {
            size_t t = 0;
            const char32_t c = decode(text, at, &t);
            if (isTag(c)) {
                appendHex(tags, c);
                at += t;
                tagged = true;
            } else if (c == kCancelTag && tagged) {
                appendHex(tags, c);
                at += t;
                local += "_" + tags;
                end = at;
                presentation = true;
                break;
            } else {
                break;
            }
        }
    }
    key += key.empty() ? local : "_" + local;
    *emoji = presentation;
    return end - pos;
}

} // namespace

size_t match(std::string_view text, size_t pos, std::string* key)
{
    size_t n = 0;
    const char32_t cp = decode(text, pos, &n);
    if (n == 0)
        return 0;
    std::string k;
    // Keycap: [0-9#*] FE0F? 20E3
    if (isKeycapBase(cp)) {
        size_t at = pos + n, m = 0;
        char32_t c = decode(text, at, &m);
        if (c == kVs16) {
            at += m;
            c = decode(text, at, &m);
        }
        if (c != kKeycap)
            return 0;
        appendHex(k, cp);
        appendHex(k, kKeycap);
        if (key)
            *key = std::move(k);
        return at + m - pos;
    }
    // Flag: a pair of regional indicators (one alone shows as its letter).
    if (isRegional(cp)) {
        size_t m = 0;
        const char32_t second = decode(text, pos + n, &m);
        appendHex(k, cp);
        if (!isRegional(second)) {
            if (key)
                *key = std::move(k);
            return n;
        }
        appendHex(k, second);
        if (key)
            *key = std::move(k);
        return n + m;
    }
    bool emoji = false;
    size_t length = element(text, pos, k, &emoji);
    if (length == 0)
        return 0;
    // ZWJ sequence: element (ZWJ element)+; a text-default element joined to others shows as emoji.
    bool joined = false;
    for (;;) {
        size_t z = 0;
        if (decode(text, pos + length, &z) != kZwj)
            break;
        std::string part;
        bool partEmoji = false;
        const size_t next = element(text, pos + length + z, part, &partEmoji);
        if (next == 0)
            break; // a joiner with nothing to join: not part of the emoji (dropped as invisible)
        k += "_200d_" + part;
        length += z + next;
        joined = true;
    }
    if (!emoji && !joined)
        return 0;
    if (key)
        *key = std::move(k);
    return length;
}

size_t invisible(std::string_view text, size_t pos)
{
    size_t n = 0;
    const char32_t cp = decode(text, pos, &n);
    return (cp == kVs15 || cp == kVs16 || cp == kZwj || cp == kKeycap || cp == kCancelTag || isTag(cp)) ? n : 0;
}

std::vector<std::string> components(const std::string& key)
{
    std::vector<std::string> parts;
    size_t start = 0;
    for (;;) {
        const size_t at = key.find("_200d_", start);
        parts.push_back(key.substr(start, at == std::string::npos ? std::string::npos : at - start));
        if (at == std::string::npos)
            break;
        start = at + 6;
    }
    return parts;
}

std::string withoutModifiers(const std::string& key)
{
    std::string out;
    size_t start = 0;
    while (start <= key.size()) {
        size_t end = key.find('_', start);
        if (end == std::string::npos)
            end = key.size();
        const std::string part = key.substr(start, end - start);
        const unsigned long cp = std::strtoul(part.c_str(), nullptr, 16);
        if (!(cp >= 0x1F3FB && cp <= 0x1F3FF))
            out += (out.empty() ? "" : "_") + part;
        start = end + 1;
    }
    return out;
}

const char* unicodeVersion()
{
    return ENP_EMOJI_UNICODE_VERSION;
}

} // namespace enp::model::emoji
