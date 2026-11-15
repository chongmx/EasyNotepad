#include "PdfCrypt.h"

#include "enp/model/Crypto.h"

#include <algorithm>
#include <cmath>

namespace enp::model::viewer::pdf {

namespace {

namespace cr = enp::model::crypto;

// The 32 bytes a password is padded with (and the whole of an empty one).
constexpr char kPad[] = "\x28\xBF\x4E\x5E\x4E\x75\x8A\x41\x64\x00\x4E\x56\xFF\xFA\x01\x08"
                        "\x2E\x2E\x00\xB6\xD0\x68\x3E\x80\x2F\x0C\xA9\xFE\x64\x53\x69\x7A";
const std::string_view kPadding(kPad, 32);

std::string padded(std::string_view password)
{
    std::string out(password.substr(0, 32));
    out.append(kPadding.substr(0, 32 - out.size()));
    return out;
}

// A password typed as UTF-8 in the bytes revisions 2 to 4 use (PDFDocEncoding, which is Latin-1
// for the letters a keyboard types); empty when it has a character Latin-1 lacks.
std::string latin1(std::string_view utf8)
{
    std::string out;
    for (std::size_t i = 0; i < utf8.size();) {
        const auto c = std::uint8_t(utf8[i]);
        if (c < 0x80) {
            out += char(c);
            ++i;
        } else if ((c & 0xE0) == 0xC0 && i + 1 < utf8.size()) {
            const unsigned cp = ((c & 0x1Fu) << 6) | (std::uint8_t(utf8[i + 1]) & 0x3Fu);
            if (cp > 0xFF)
                return {};
            out += char(cp);
            i += 2;
        } else {
            return {};
        }
    }
    return out;
}

std::string xorKey(std::string_view key, int with)
{
    std::string out(key);
    for (char& c : out)
        c = char(std::uint8_t(c) ^ std::uint8_t(with));
    return out;
}

// Revision 6's hash (algorithm 2.B): SHA-256, then rounds of AES-128 and SHA-2 until the last
// byte allows it, at least 64. Revision 5: SHA-256 alone.
std::string hash2B(std::string_view password, std::string_view salt, std::string_view userKey, int revision)
{
    std::string input;
    input.append(password).append(salt).append(userKey);
    std::string k = cr::sha256(input);
    if (revision < 6)
        return k;
    std::string block, e;
    for (int round = 0; round < 64 || (!e.empty() && int(std::uint8_t(e.back())) > round - 32); ++round) {
        block.clear();
        block.append(password).append(k).append(userKey);
        std::string k1;
        k1.reserve(block.size() * 64);
        for (int i = 0; i < 64; ++i)
            k1 += block;
        e = cr::aesCbcEncrypt(std::string_view(k).substr(0, 16), std::string_view(k).substr(16, 16), k1).value_or(std::string());
        if (e.size() < 16)
            return {};
        unsigned sum = 0;
        for (int i = 0; i < 16; ++i)
            sum += std::uint8_t(e[std::size_t(i)]);
        k = sum % 3 == 0 ? cr::sha256(e) : sum % 3 == 1 ? cr::sha384(e) : cr::sha512(e);
    }
    return k.substr(0, 32);
}

std::string bytesOf(File& file, const Object& dict, std::string_view key)
{
    const Object* o = file.resolvedGet(dict, key);
    return o && o->type == Object::Type::String ? o->text : std::string();
}

} // namespace

Security::Result Security::open(File& file, const Object& encrypt, std::string_view firstId, std::string_view password, std::string* error)
{
    auto unsupported = [error](const std::string& why) {
        if (error)
            *error = why;
        return Result::Unsupported;
    };
    const Object* filter = file.resolvedGet(encrypt, "Filter");
    if (!filter || !filter->isName("Standard"))
        return unsupported("the PDF is encrypted for certain people (with their certificates), which this does not read");
    auto number = [&](std::string_view key, double otherwise) {
        const Object* o = file.resolvedGet(encrypt, key);
        return o && o->isNumber() ? o->number : otherwise;
    };
    const int v = int(number("V", 0));
    const int r = int(number("R", 0));
    const double length = number("Length", 40);
    keyBytes_ = std::size_t(std::clamp(int(length) / 8, 5, 16));
    if (const Object* em = file.resolvedGet(encrypt, "EncryptMetadata"); em && em->type == Object::Type::Bool)
        encryptMetadata_ = em->boolean;

    if (v == 1 || v == 2) {
        strings_ = streams_ = Method::Rc4;
        if (v == 1)
            keyBytes_ = 5;
    } else if (v == 4 || v == 5) {
        if (const Object* cf = file.resolvedGet(encrypt, "CF"); cf && cf->isDict())
            for (const DictEntry& entry : cf->dict) {
                const Object& f = file.resolve(entry.value);
                if (!f.isDict())
                    continue;
                const Object* cfm = file.resolvedGet(f, "CFM");
                Method m = Method::None;
                if (cfm && cfm->isName("V2"))
                    m = Method::Rc4;
                else if (cfm && cfm->isName("AESV2"))
                    m = Method::Aes128;
                else if (cfm && cfm->isName("AESV3"))
                    m = Method::Aes256;
                else if (cfm && !cfm->isName("None"))
                    return unsupported("the PDF is encrypted with a method this does not read (" + cfm->text + ")");
                filters_[entry.key] = m;
                // Revision 4's RC4: the filter's /Length, in bytes (some writers give bits).
                if (const Object* len = file.resolvedGet(f, "Length"); len && len->isNumber() && m == Method::Rc4 && v == 4) {
                    const int l = int(len->number);
                    keyBytes_ = std::size_t(std::clamp(l > 16 ? l / 8 : l, 5, 16));
                }
                if (m == Method::Aes128)
                    keyBytes_ = 16;
            }
        auto byName = [&](std::string_view key) {
            const Object* name = file.resolvedGet(encrypt, key);
            if (!name || name->type != Object::Type::Name || name->text == "Identity")
                return Method::None;
            const auto it = filters_.find(name->text);
            return it == filters_.end() ? Method::None : it->second;
        };
        streams_ = byName("StmF");
        strings_ = byName("StrF");
    } else {
        return unsupported("the PDF is encrypted with a method this does not read (version " + std::to_string(v) + ")");
    }

    const std::string o = bytesOf(file, encrypt, "O"), u = bytesOf(file, encrypt, "U");
    if (r >= 2 && r <= 4) {
        if (o.size() < 32 || u.size() < 16)
            return unsupported("the PDF's encryption dictionary is damaged");
        const std::uint32_t p = std::uint32_t(std::int64_t(std::llround(number("P", 0))));
        if (tryLegacy(password, r, o, u, p, firstId))
            return Result::Opened;
        const std::string other = latin1(password);
        if (!other.empty() && other != password && tryLegacy(other, r, o, u, p, firstId))
            return Result::Opened;
        return Result::WrongPassword;
    }
    if (r == 5 || r == 6) {
        const std::string oe = bytesOf(file, encrypt, "OE"), ue = bytesOf(file, encrypt, "UE");
        if (o.size() < 48 || u.size() < 48 || oe.size() < 32 || ue.size() < 32)
            return unsupported("the PDF's encryption dictionary is damaged");
        keyBytes_ = 32;
        // UTF-8, at most 127 bytes (the SASLprep profile is not applied: it changes no letter typed
        // on a keyboard but a few rare spaces and joiners).
        if (tryAes256(password.substr(0, 127), r, o, u, oe, ue))
            return Result::Opened;
        return Result::WrongPassword;
    }
    return unsupported("the PDF is encrypted with a method this does not read (revision " + std::to_string(r) + ")");
}

// Revisions 2 to 4 (algorithms 2, 4, 5 and 7): the key from the user's password; the owner's
// password gives the user's (RC4 over /O), which gives the key.
bool Security::tryLegacy(std::string_view password, int revision, std::string_view o, std::string_view u, std::uint32_t p,
                         std::string_view id)
{
    auto keyFrom = [&](std::string_view user) {
        std::string input = padded(user);
        input.append(o.substr(0, 32));
        for (int i = 0; i < 4; ++i)
            input += char((p >> (i * 8)) & 0xFF);
        input.append(id);
        if (revision >= 4 && !encryptMetadata_)
            input.append("\xFF\xFF\xFF\xFF", 4);
        std::string h = cr::md5(input);
        if (revision >= 3)
            for (int i = 0; i < 50; ++i)
                h = cr::md5(std::string_view(h).substr(0, keyBytes_));
        return h.substr(0, keyBytes_);
    };
    auto userMatches = [&](const std::string& key) {
        if (revision == 2)
            return cr::rc4(key, kPadding) == u.substr(0, 32);
        std::string input(kPadding);
        input.append(id);
        std::string x = cr::rc4(key, cr::md5(input));
        for (int i = 1; i <= 19; ++i)
            x = cr::rc4(xorKey(key, i), x);
        return x == u.substr(0, 16);
    };
    std::string key = keyFrom(password);
    if (userMatches(key)) {
        key_ = std::move(key);
        return true;
    }
    // The owner's password: its padded MD5 (hashed 50 times more from revision 3) unlocks /O.
    std::string h = cr::md5(padded(password));
    if (revision >= 3)
        for (int i = 0; i < 50; ++i)
            h = cr::md5(h);
    const std::string ownerKey = h.substr(0, keyBytes_);
    std::string user(o.substr(0, 32));
    if (revision == 2) {
        user = cr::rc4(ownerKey, user);
    } else {
        for (int i = 19; i >= 0; --i)
            user = cr::rc4(xorKey(ownerKey, i), user);
    }
    key = keyFrom(user);
    if (userMatches(key)) {
        key_ = std::move(key);
        return true;
    }
    return false;
}

// Revisions 5 and 6 (algorithms 2.A, 11 and 12): the password with /O's or /U's validation salt
// hashes to their first 32 bytes; with the key salt, to the key that unwraps /OE or /UE.
bool Security::tryAes256(std::string_view password, int revision, std::string_view o, std::string_view u, std::string_view oe,
                         std::string_view ue)
{
    const std::string_view u48 = u.substr(0, 48);
    const std::string zero(16, '\0');
    if (hash2B(password, o.substr(32, 8), u48, revision) == o.substr(0, 32)) {
        const std::string wrap = hash2B(password, o.substr(40, 8), u48, revision);
        if (std::optional<std::string> key = cr::aesCbcDecrypt(wrap, zero, oe.substr(0, 32))) {
            key_ = std::move(*key);
            return true;
        }
    }
    if (hash2B(password, u.substr(32, 8), {}, revision) == u.substr(0, 32)) {
        const std::string wrap = hash2B(password, u.substr(40, 8), {}, revision);
        if (std::optional<std::string> key = cr::aesCbcDecrypt(wrap, zero, ue.substr(0, 32))) {
            key_ = std::move(*key);
            return true;
        }
    }
    return false;
}

std::string Security::decrypt(Method method, int number, int generation, std::string_view data) const
{
    if (method == Method::None)
        return std::string(data);
    std::string key = key_;
    if (method != Method::Aes256) { // a key per object (algorithm 1)
        std::string input = key_;
        for (int i = 0; i < 3; ++i)
            input += char((unsigned(number) >> (i * 8)) & 0xFF);
        for (int i = 0; i < 2; ++i)
            input += char((unsigned(generation) >> (i * 8)) & 0xFF);
        if (method == Method::Aes128)
            input.append("sAlT");
        key = cr::md5(input).substr(0, std::min<std::size_t>(key_.size() + 5, 16));
    }
    if (method == Method::Rc4)
        return cr::rc4(key, data);
    // AES: a 16-byte IV, then whole blocks; the padding (PKCS #5) taken off when it is one.
    if (data.size() < 32)
        return {};
    const std::string_view body = data.substr(16, (data.size() - 16) / 16 * 16);
    std::string out = cr::aesCbcDecrypt(key, data.substr(0, 16), body).value_or(std::string());
    if (!out.empty()) {
        const std::size_t pad = std::uint8_t(out.back());
        if (pad >= 1 && pad <= 16 && pad <= out.size() &&
            std::all_of(out.end() - std::ptrdiff_t(pad), out.end(), [pad](char c) { return std::uint8_t(c) == pad; }))
            out.resize(out.size() - pad);
    }
    return out;
}

std::string Security::string(int number, int generation, std::string_view data) const
{
    return decrypt(strings_, number, generation, data);
}

std::string Security::stream(int number, int generation, std::string_view data, std::string_view filter) const
{
    Method method = streams_;
    if (!filter.empty()) {
        if (filter == "Identity")
            return std::string(data);
        const auto it = filters_.find(filter);
        if (it != filters_.end())
            method = it->second;
    }
    return decrypt(method, number, generation, data);
}

} // namespace enp::model::viewer::pdf
