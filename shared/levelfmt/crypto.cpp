#include "crypto.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <bcrypt.h>

namespace h2::levelfmt::crypto {

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
