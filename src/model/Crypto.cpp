#include "enp/model/Crypto.h"

#include "enp/model/Hash.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace enp::model::crypto {

namespace {

// ---- MD5 (RFC 1321) ------------------------------------------------------------------------------

std::uint32_t rotl32(std::uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }

struct Md5Table {
    std::uint32_t k[64];
    Md5Table()
    {
        for (int i = 0; i < 64; ++i) // floor(|sin(i + 1)| * 2^32): exact in a double
            k[i] = std::uint32_t(std::floor(std::fabs(std::sin(double(i + 1))) * 4294967296.0));
    }
};

void md5Block(std::uint32_t h[4], const unsigned char* p)
{
    static const Md5Table table;
    static const int kShift[4][4] = {{7, 12, 17, 22}, {5, 9, 14, 20}, {4, 11, 16, 23}, {6, 10, 15, 21}};
    std::uint32_t m[16];
    for (int i = 0; i < 16; ++i)
        m[i] = std::uint32_t(p[i * 4]) | std::uint32_t(p[i * 4 + 1]) << 8 | std::uint32_t(p[i * 4 + 2]) << 16 |
               std::uint32_t(p[i * 4 + 3]) << 24;
    std::uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
    for (int i = 0; i < 64; ++i) {
        std::uint32_t f;
        int g;
        const int round = i / 16;
        if (round == 0) {
            f = (b & c) | (~b & d);
            g = i;
        } else if (round == 1) {
            f = (b & d) | (c & ~d);
            g = (5 * i + 1) % 16;
        } else if (round == 2) {
            f = b ^ c ^ d;
            g = (3 * i + 5) % 16;
        } else {
            f = c ^ (b | ~d);
            g = (7 * i) % 16;
        }
        const std::uint32_t next = d;
        d = c;
        c = b;
        b = b + rotl32(a + f + table.k[i] + m[g], kShift[round][i % 4]);
        a = next;
    }
    h[0] += a;
    h[1] += b;
    h[2] += c;
    h[3] += d;
}

// ---- SHA-384 and SHA-512 (FIPS 180-4) --------------------------------------------------------------

constexpr std::array<std::uint64_t, 80> kSha512 = {
    0x428a2f98d728ae22, 0x7137449123ef65cd, 0xb5c0fbcfec4d3b2f, 0xe9b5dba58189dbbc, 0x3956c25bf348b538, 0x59f111f1b605d019,
    0x923f82a4af194f9b, 0xab1c5ed5da6d8118, 0xd807aa98a3030242, 0x12835b0145706fbe, 0x243185be4ee4b28c, 0x550c7dc3d5ffb4e2,
    0x72be5d74f27b896f, 0x80deb1fe3b1696b1, 0x9bdc06a725c71235, 0xc19bf174cf692694, 0xe49b69c19ef14ad2, 0xefbe4786384f25e3,
    0x0fc19dc68b8cd5b5, 0x240ca1cc77ac9c65, 0x2de92c6f592b0275, 0x4a7484aa6ea6e483, 0x5cb0a9dcbd41fbd4, 0x76f988da831153b5,
    0x983e5152ee66dfab, 0xa831c66d2db43210, 0xb00327c898fb213f, 0xbf597fc7beef0ee4, 0xc6e00bf33da88fc2, 0xd5a79147930aa725,
    0x06ca6351e003826f, 0x142929670a0e6e70, 0x27b70a8546d22ffc, 0x2e1b21385c26c926, 0x4d2c6dfc5ac42aed, 0x53380d139d95b3df,
    0x650a73548baf63de, 0x766a0abb3c77b2a8, 0x81c2c92e47edaee6, 0x92722c851482353b, 0xa2bfe8a14cf10364, 0xa81a664bbc423001,
    0xc24b8b70d0f89791, 0xc76c51a30654be30, 0xd192e819d6ef5218, 0xd69906245565a910, 0xf40e35855771202a, 0x106aa07032bbd1b8,
    0x19a4c116b8d2d0c8, 0x1e376c085141ab53, 0x2748774cdf8eeb99, 0x34b0bcb5e19b48a8, 0x391c0cb3c5c95a63, 0x4ed8aa4ae3418acb,
    0x5b9cca4f7763e373, 0x682e6ff3d6b2b8a3, 0x748f82ee5defb2fc, 0x78a5636f43172f60, 0x84c87814a1f0ab72, 0x8cc702081a6439ec,
    0x90befffa23631e28, 0xa4506cebde82bde9, 0xbef9a3f7b2c67915, 0xc67178f2e372532b, 0xca273eceea26619c, 0xd186b8c721c0c207,
    0xeada7dd6cde0eb1e, 0xf57d4f7fee6ed178, 0x06f067aa72176fba, 0x0a637dc5a2c898a6, 0x113f9804bef90dae, 0x1b710b35131c471b,
    0x28db77f523047d84, 0x32caab7b40c72493, 0x3c9ebe0a15c9bebc, 0x431d67c49c100d4c, 0x4cc5d4becb3e42b6, 0x597f299cfc657e2a,
    0x5fcb6fab3ad6faec, 0x6c44198c4a475817};

std::uint64_t rotr64(std::uint64_t x, int n) { return (x >> n) | (x << (64 - n)); }

void sha512Block(std::uint64_t h[8], const unsigned char* p)
{
    std::uint64_t w[80];
    for (int i = 0; i < 16; ++i) {
        w[i] = 0;
        for (int k = 0; k < 8; ++k)
            w[i] = (w[i] << 8) | p[i * 8 + k];
    }
    for (int i = 16; i < 80; ++i) {
        const std::uint64_t s0 = rotr64(w[i - 15], 1) ^ rotr64(w[i - 15], 8) ^ (w[i - 15] >> 7);
        const std::uint64_t s1 = rotr64(w[i - 2], 19) ^ rotr64(w[i - 2], 61) ^ (w[i - 2] >> 6);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    std::uint64_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
    for (int i = 0; i < 80; ++i) {
        const std::uint64_t t1 = hh + (rotr64(e, 14) ^ rotr64(e, 18) ^ rotr64(e, 41)) + ((e & f) ^ (~e & g)) + kSha512[std::size_t(i)] + w[i];
        const std::uint64_t t2 = (rotr64(a, 28) ^ rotr64(a, 34) ^ rotr64(a, 39)) + ((a & b) ^ (a & c) ^ (b & c));
        hh = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    h[0] += a;
    h[1] += b;
    h[2] += c;
    h[3] += d;
    h[4] += e;
    h[5] += f;
    h[6] += g;
    h[7] += hh;
}

std::string sha512Family(std::string_view data, const std::uint64_t (&start)[8], std::size_t outBytes)
{
    std::uint64_t h[8];
    std::memcpy(h, start, sizeof(h));
    const auto* p = reinterpret_cast<const unsigned char*>(data.data());
    std::size_t n = data.size();
    for (; n >= 128; p += 128, n -= 128)
        sha512Block(h, p);
    unsigned char last[256] = {};
    std::memcpy(last, p, n);
    last[n] = 0x80;
    const std::size_t blocks = n + 1 + 16 <= 128 ? 1 : 2;
    const std::uint64_t bits = std::uint64_t(data.size()) * 8; // the high 64 bits of the length are 0
    for (int i = 0; i < 8; ++i)
        last[blocks * 128 - 1 - std::size_t(i)] = (unsigned char)(bits >> (i * 8));
    for (std::size_t b = 0; b < blocks; ++b)
        sha512Block(h, last + b * 128);
    std::string out;
    out.reserve(outBytes);
    for (std::size_t i = 0; i < outBytes; ++i)
        out += char((h[i / 8] >> ((7 - i % 8) * 8)) & 0xFF);
    return out;
}

// ---- AES (FIPS 197) ----------------------------------------------------------------------------------

std::uint8_t xtime(std::uint8_t x) { return std::uint8_t((x << 1) ^ ((x & 0x80) ? 0x1B : 0)); }

std::uint8_t gmul(std::uint8_t a, std::uint8_t b)
{
    std::uint8_t r = 0;
    for (; b; b >>= 1, a = xtime(a))
        if (b & 1)
            r ^= a;
    return r;
}

// The S-box from its definition (the multiplicative inverse in GF(2^8), then the affine map),
// built once.
struct SBoxes {
    std::uint8_t s[256], inv[256];
    SBoxes()
    {
        for (int x = 0; x < 256; ++x) {
            std::uint8_t v = 0;
            if (x) // x^254 = x^-1
                for (int y = 1; y < 256; ++y)
                    if (gmul(std::uint8_t(x), std::uint8_t(y)) == 1) {
                        v = std::uint8_t(y);
                        break;
                    }
            const auto rot = [](std::uint8_t b, int n) { return std::uint8_t((b << n) | (b >> (8 - n))); };
            const std::uint8_t out = std::uint8_t(v ^ rot(v, 1) ^ rot(v, 2) ^ rot(v, 3) ^ rot(v, 4) ^ 0x63);
            s[x] = out;
            inv[out] = std::uint8_t(x);
        }
    }
};

const SBoxes& boxes()
{
    static const SBoxes b;
    return b;
}

class Aes {
public:
    bool setKey(std::string_view key)
    {
        if (key.size() != 16 && key.size() != 24 && key.size() != 32)
            return false;
        const std::uint8_t* s = boxes().s;
        const int nk = int(key.size() / 4);
        rounds_ = nk + 6;
        const int words = 4 * (rounds_ + 1);
        std::memcpy(keys_, key.data(), key.size());
        std::uint8_t rcon = 1;
        for (int i = nk; i < words; ++i) {
            std::uint8_t t[4];
            std::memcpy(t, keys_ + (i - 1) * 4, 4);
            if (i % nk == 0) {
                const std::uint8_t first = t[0];
                t[0] = std::uint8_t(s[t[1]] ^ rcon);
                t[1] = s[t[2]];
                t[2] = s[t[3]];
                t[3] = s[first];
                rcon = xtime(rcon);
            } else if (nk > 6 && i % nk == 4) {
                for (std::uint8_t& b : t)
                    b = s[b];
            }
            for (int k = 0; k < 4; ++k)
                keys_[i * 4 + k] = std::uint8_t(keys_[(i - nk) * 4 + k] ^ t[k]);
        }
        return true;
    }

    void encrypt(std::uint8_t b[16]) const
    {
        const std::uint8_t* s = boxes().s;
        addKey(b, 0);
        for (int r = 1; r <= rounds_; ++r) {
            for (int i = 0; i < 16; ++i)
                b[i] = s[b[i]];
            shiftRows(b);
            if (r != rounds_)
                for (int c = 0; c < 4; ++c) {
                    std::uint8_t* col = b + c * 4;
                    const std::uint8_t a0 = col[0], a1 = col[1], a2 = col[2], a3 = col[3], all = std::uint8_t(a0 ^ a1 ^ a2 ^ a3);
                    col[0] = std::uint8_t(a0 ^ all ^ xtime(std::uint8_t(a0 ^ a1)));
                    col[1] = std::uint8_t(a1 ^ all ^ xtime(std::uint8_t(a1 ^ a2)));
                    col[2] = std::uint8_t(a2 ^ all ^ xtime(std::uint8_t(a2 ^ a3)));
                    col[3] = std::uint8_t(a3 ^ all ^ xtime(std::uint8_t(a3 ^ a0)));
                }
            addKey(b, r);
        }
    }

    void decrypt(std::uint8_t b[16]) const
    {
        const std::uint8_t* inv = boxes().inv;
        addKey(b, rounds_);
        for (int r = rounds_ - 1; r >= 0; --r) {
            unshiftRows(b);
            for (int i = 0; i < 16; ++i)
                b[i] = inv[b[i]];
            addKey(b, r);
            if (r != 0)
                for (int c = 0; c < 4; ++c) {
                    std::uint8_t* col = b + c * 4;
                    const std::uint8_t a0 = col[0], a1 = col[1], a2 = col[2], a3 = col[3];
                    col[0] = std::uint8_t(gmul(a0, 14) ^ gmul(a1, 11) ^ gmul(a2, 13) ^ gmul(a3, 9));
                    col[1] = std::uint8_t(gmul(a0, 9) ^ gmul(a1, 14) ^ gmul(a2, 11) ^ gmul(a3, 13));
                    col[2] = std::uint8_t(gmul(a0, 13) ^ gmul(a1, 9) ^ gmul(a2, 14) ^ gmul(a3, 11));
                    col[3] = std::uint8_t(gmul(a0, 11) ^ gmul(a1, 13) ^ gmul(a2, 9) ^ gmul(a3, 14));
                }
        }
    }

private:
    void addKey(std::uint8_t b[16], int round) const
    {
        for (int i = 0; i < 16; ++i)
            b[i] ^= keys_[round * 16 + i];
    }
    // The state is column by column: byte (row r, column c) at r + 4c. Row r turns left by r.
    static void shiftRows(std::uint8_t b[16])
    {
        std::uint8_t t[16];
        for (int c = 0; c < 4; ++c)
            for (int r = 0; r < 4; ++r)
                t[r + 4 * c] = b[r + 4 * ((c + r) % 4)];
        std::memcpy(b, t, 16);
    }
    static void unshiftRows(std::uint8_t b[16])
    {
        std::uint8_t t[16];
        for (int c = 0; c < 4; ++c)
            for (int r = 0; r < 4; ++r)
                t[r + 4 * ((c + r) % 4)] = b[r + 4 * c];
        std::memcpy(b, t, 16);
    }
    std::uint8_t keys_[240] = {};
    int rounds_ = 0;
};

} // namespace

std::string md5(std::string_view data)
{
    std::uint32_t h[4] = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476};
    const auto* p = reinterpret_cast<const unsigned char*>(data.data());
    std::size_t n = data.size();
    for (; n >= 64; p += 64, n -= 64)
        md5Block(h, p);
    unsigned char last[128] = {};
    std::memcpy(last, p, n);
    last[n] = 0x80;
    const std::size_t blocks = n + 1 + 8 <= 64 ? 1 : 2;
    const std::uint64_t bits = std::uint64_t(data.size()) * 8;
    for (int i = 0; i < 8; ++i)
        last[blocks * 64 - 8 + std::size_t(i)] = (unsigned char)(bits >> (i * 8));
    for (std::size_t b = 0; b < blocks; ++b)
        md5Block(h, last + b * 64);
    std::string out;
    out.reserve(16);
    for (std::uint32_t v : h)
        for (int i = 0; i < 4; ++i)
            out += char((v >> (i * 8)) & 0xFF);
    return out;
}

std::string sha256(std::string_view data)
{
    Sha256 hash;
    hash.update(data.data(), data.size());
    return hash.digest();
}

std::string sha384(std::string_view data)
{
    static const std::uint64_t kStart[8] = {0xcbbb9d5dc1059ed8, 0x629a292a367cd507, 0x9159015a3070dd17, 0x152fecd8f70e5939,
                                            0x67332667ffc00b31, 0x8eb44a8768581511, 0xdb0c2e0d64f98fa7, 0x47b5481dbefa4fa4};
    return sha512Family(data, kStart, 48);
}

std::string sha512(std::string_view data)
{
    static const std::uint64_t kStart[8] = {0x6a09e667f3bcc908, 0xbb67ae8584caa73b, 0x3c6ef372fe94f82b, 0xa54ff53a5f1d36f1,
                                            0x510e527fade682d1, 0x9b05688c2b3e6c1f, 0x1f83d9abfb41bd6b, 0x5be0cd19137e2179};
    return sha512Family(data, kStart, 64);
}

std::string rc4(std::string_view key, std::string_view data)
{
    std::string out(data);
    if (key.empty())
        return out;
    std::uint8_t s[256];
    for (int i = 0; i < 256; ++i)
        s[i] = std::uint8_t(i);
    for (int i = 0, j = 0; i < 256; ++i) {
        j = (j + s[i] + std::uint8_t(key[std::size_t(i) % key.size()])) & 0xFF;
        std::swap(s[i], s[j]);
    }
    int i = 0, j = 0;
    for (char& c : out) {
        i = (i + 1) & 0xFF;
        j = (j + s[i]) & 0xFF;
        std::swap(s[i], s[j]);
        c = char(std::uint8_t(c) ^ s[(s[i] + s[j]) & 0xFF]);
    }
    return out;
}

std::optional<std::string> aesCbcEncrypt(std::string_view key, std::string_view iv, std::string_view data)
{
    Aes aes;
    if (iv.size() != 16 || data.size() % 16 != 0 || !aes.setKey(key))
        return std::nullopt;
    std::string out(data);
    std::uint8_t chain[16];
    std::memcpy(chain, iv.data(), 16);
    for (std::size_t at = 0; at < out.size(); at += 16) {
        std::uint8_t b[16];
        for (int i = 0; i < 16; ++i)
            b[i] = std::uint8_t(std::uint8_t(out[at + std::size_t(i)]) ^ chain[i]);
        aes.encrypt(b);
        std::memcpy(chain, b, 16);
        std::memcpy(&out[at], b, 16);
    }
    return out;
}

std::optional<std::string> aesCbcDecrypt(std::string_view key, std::string_view iv, std::string_view data)
{
    Aes aes;
    if (iv.size() != 16 || data.size() % 16 != 0 || !aes.setKey(key))
        return std::nullopt;
    std::string out(data);
    std::uint8_t chain[16];
    std::memcpy(chain, iv.data(), 16);
    for (std::size_t at = 0; at < out.size(); at += 16) {
        std::uint8_t b[16], cipher[16];
        std::memcpy(cipher, &out[at], 16);
        std::memcpy(b, cipher, 16);
        aes.decrypt(b);
        for (int i = 0; i < 16; ++i)
            out[at + std::size_t(i)] = char(b[i] ^ chain[i]);
        std::memcpy(chain, cipher, 16);
    }
    return out;
}

} // namespace enp::model::crypto
