#include "crypto.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <bcrypt.h>
#else  // (the site's upload check, built to WebAssembly: website/check)
#include <unistd.h>

#include "monocypher-ed25519.h"
#endif

namespace h2::levelfmt::crypto {

#ifdef _WIN32
namespace {

// a hash of the system's (CNG): its provider opened once
template <size_t N>
std::array<uint8_t, N> Hash(LPCWSTR algorithm, const void* data, size_t size)
{
    std::array<uint8_t, N> out{};
    BCRYPT_ALG_HANDLE alg = nullptr;
    if (BCryptOpenAlgorithmProvider(&alg, algorithm, nullptr, 0) != 0)
        return out;
    BCRYPT_HASH_HANDLE h = nullptr;
    if (BCryptCreateHash(alg, &h, nullptr, 0, nullptr, 0, 0) == 0) {
        const uint8_t* p = static_cast<const uint8_t*>(data);
        bool ok = true;
        while (size && ok) {  // (ULONG lengths)
            ULONG n = size > 0x40000000 ? 0x40000000 : ULONG(size);
            ok = BCryptHashData(h, const_cast<PUCHAR>(p), n, 0) == 0;
            p += n;
            size -= n;
        }
        if (!ok || BCryptFinishHash(h, out.data(), ULONG(N), 0) != 0)
            out.fill(0);
        BCryptDestroyHash(h);
    }
    BCryptCloseAlgorithmProvider(alg, 0);
    return out;
}

} // namespace

Sha256Digest Sha256(const void* data, size_t size) { return Hash<32>(BCRYPT_SHA256_ALGORITHM, data, size); }
Sha512Digest Sha512(const void* data, size_t size) { return Hash<64>(BCRYPT_SHA512_ALGORITHM, data, size); }

bool RandomBytes(void* out, size_t size)
{
    return size <= 0xFFFFFFFF &&
           BCryptGenRandom(nullptr, static_cast<PUCHAR>(out), ULONG(size), BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0;
}
#else
// SHA-256 (FIPS 180-4); SHA-512 is Monocypher's
Sha256Digest Sha256(const void* data, size_t size)
{
    static const uint32_t k[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98,
        0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
        0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8,
        0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
        0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819,
        0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
        0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7,
        0xc67178f2};
    uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    auto rotr = [](uint32_t x, int n) { return (x >> n) | (x << (32 - n)); };
    auto block = [&](const uint8_t* b) {
        uint32_t w[64];
        for (int i = 0; i < 16; ++i)
            w[i] = uint32_t(b[4 * i]) << 24 | uint32_t(b[4 * i + 1]) << 16 | uint32_t(b[4 * i + 2]) << 8 | b[4 * i + 3];
        for (int i = 16; i < 64; ++i) {
            uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = h[0], b2 = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; ++i) {
            uint32_t t1 = hh + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + k[i] + w[i];
            uint32_t t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b2) ^ (a & c) ^ (b2 & c));
            hh = g;
            g = f;
            f = e;
            e = d + t1;
            d = c;
            c = b2;
            b2 = a;
            a = t1 + t2;
        }
        h[0] += a;
        h[1] += b2;
        h[2] += c;
        h[3] += d;
        h[4] += e;
        h[5] += f;
        h[6] += g;
        h[7] += hh;
    };
    const uint8_t* p = static_cast<const uint8_t*>(data);
    size_t n = size;
    for (; n >= 64; n -= 64, p += 64)
        block(p);
    uint8_t last[128] = {};
    for (size_t i = 0; i < n; ++i)
        last[i] = p[i];
    last[n] = 0x80;
    size_t end = n < 56 ? 64 : 128;
    uint64_t bits = uint64_t(size) * 8;
    for (int i = 0; i < 8; ++i)
        last[end - 1 - i] = uint8_t(bits >> (8 * i));
    block(last);
    if (end == 128)
        block(last + 64);
    Sha256Digest out;
    for (int i = 0; i < 8; ++i)
        for (int j = 0; j < 4; ++j)
            out[4 * i + j] = uint8_t(h[i] >> (24 - 8 * j));
    return out;
}

Sha512Digest Sha512(const void* data, size_t size)
{
    Sha512Digest out;
    crypto_sha512(out.data(), static_cast<const uint8_t*>(data), size);
    return out;
}

bool RandomBytes(void* out, size_t size)
{
    for (uint8_t* p = static_cast<uint8_t*>(out); size;) {
        size_t n = size < 256 ? size : 256;  // (getentropy's limit)
        if (getentropy(p, n) != 0)
            return false;
        p += n;
        size -= n;
    }
    return true;
}
#endif

std::string Hex(const uint8_t* data, size_t size)
{
    static const char digits[] = "0123456789abcdef";
    std::string s(size * 2, '0');
    for (size_t i = 0; i < size; ++i) {
        s[2 * i] = digits[data[i] >> 4];
        s[2 * i + 1] = digits[data[i] & 15];
    }
    return s;
}

bool FromHex(const std::string& hex, uint8_t* out, size_t size)
{
    if (hex.size() != size * 2)
        return false;
    auto digit = [](char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1; };
    for (size_t i = 0; i < size; ++i) {
        int hi = digit(hex[2 * i]), lo = digit(hex[2 * i + 1]);
        if (hi < 0 || lo < 0)
            return false;
        out[i] = uint8_t(hi * 16 + lo);
    }
    return true;
}

} // namespace h2::levelfmt::crypto
