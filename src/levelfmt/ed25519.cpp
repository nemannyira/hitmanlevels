// Ed25519 (RFC 8032) after TweetNaCl (public domain): field elements of 16 limbs of 16 bits, the
// group's operations without branches on secret data. Its constants (d, sqrt(-1), the base point) are
// worked out from their definitions once instead of written down.
#include <cstring>
#include <vector>

#include "crypto.h"

namespace h2::levelfmt::crypto {

namespace {

using i64 = int64_t;
using gf = i64[16];

void Set(gf r, const gf a)
{
    for (int i = 0; i < 16; ++i)
        r[i] = a[i];
}

void Carry(gf o)
{
    for (int i = 0; i < 16; ++i) {
        o[i] += i64(1) << 16;
        i64 c = o[i] >> 16;
        o[(i + 1) * (i < 15)] += c - 1 + 37 * (c - 1) * (i == 15);
        o[i] -= c * 65536;
    }
}

// p and q swapped when b is 1 (b 0 or 1)
void Select(gf p, gf q, int b)
{
    i64 c = ~(i64(b) - 1);
    for (int i = 0; i < 16; ++i) {
        i64 t = c & (p[i] ^ q[i]);
        p[i] ^= t;
        q[i] ^= t;
    }
}

void Pack(uint8_t* o, const gf n)
{
    gf m, t;
    Set(t, n);
    Carry(t);
    Carry(t);
    Carry(t);
    for (int j = 0; j < 2; ++j) {
        m[0] = t[0] - 0xffed;
        for (int i = 1; i < 15; ++i) {
            m[i] = t[i] - 0xffff - ((m[i - 1] >> 16) & 1);
            m[i - 1] &= 0xffff;
        }
        m[15] = t[15] - 0x7fff - ((m[14] >> 16) & 1);
        int b = int((m[15] >> 16) & 1);
        m[14] &= 0xffff;
        Select(t, m, 1 - b);
    }
    for (int i = 0; i < 16; ++i) {
        o[2 * i] = uint8_t(t[i] & 0xff);
        o[2 * i + 1] = uint8_t(t[i] >> 8);
    }
}

int Differ32(const uint8_t* x, const uint8_t* y)
{
    unsigned d = 0;
    for (int i = 0; i < 32; ++i)
        d |= unsigned(x[i] ^ y[i]);
    return (1 & ((d - 1) >> 8)) - 1;  // 0 equal, -1 not
}

int NotEqual(const gf a, const gf b)
{
    uint8_t c[32], d[32];
    Pack(c, a);
    Pack(d, b);
    return Differ32(c, d);
}

uint8_t Parity(const gf a)
{
    uint8_t d[32];
    Pack(d, a);
    return d[0] & 1;
}

void Unpack(gf o, const uint8_t* n)
{
    for (int i = 0; i < 16; ++i)
        o[i] = n[2 * i] + (i64(n[2 * i + 1]) << 8);
    o[15] &= 0x7fff;
}

void Add(gf o, const gf a, const gf b)
{
    for (int i = 0; i < 16; ++i)
        o[i] = a[i] + b[i];
}

void Sub(gf o, const gf a, const gf b)
{
    for (int i = 0; i < 16; ++i)
        o[i] = a[i] - b[i];
}

void Mul(gf o, const gf a, const gf b)
{
    i64 t[31] = {};
    for (int i = 0; i < 16; ++i)
        for (int j = 0; j < 16; ++j)
            t[i + j] += a[i] * b[j];
    for (int i = 0; i < 15; ++i)
        t[i] += 38 * t[i + 16];
    for (int i = 0; i < 16; ++i)
        o[i] = t[i];
    Carry(o);
    Carry(o);
}

void Square(gf o, const gf a) { Mul(o, a, a); }

// i^e for an exponent of `top` + 1 bits, all set but those in `clear` (bit numbers below top)
void PowAllOnes(gf o, const gf i, int top, int clear1, int clear2)
{
    gf c;
    Set(c, i);
    for (int a = top - 1; a >= 0; --a) {
        Square(c, c);
        if (a != clear1 && a != clear2)
            Mul(c, c, i);
    }
    Set(o, c);
}

void Invert(gf o, const gf i) { PowAllOnes(o, i, 254, 2, 4); }   // i^(p-2), p = 2^255-19
void Pow2523(gf o, const gf i) { PowAllOnes(o, i, 251, 1, -1); }  // i^((p-5)/8)

struct Constants {
    gf zero = {}, one = {1}, d, d2, sqrtm1;
    gf bx, by;  // the base point
};

const Constants& K();

// a point (X, Y, Z, T) of the curve
void PointAdd(gf p[4], gf q[4])
{
    const Constants& k = K();
    gf a, b, c, d, t, e, f, g, h;
    Sub(a, p[1], p[0]);
    Sub(t, q[1], q[0]);
    Mul(a, a, t);
    Add(b, p[0], p[1]);
    Add(t, q[0], q[1]);
    Mul(b, b, t);
    Mul(c, p[3], q[3]);
    Mul(c, c, k.d2);
    Mul(d, p[2], q[2]);
    Add(d, d, d);
    Sub(e, b, a);
    Sub(f, d, c);
    Add(g, d, c);
    Add(h, b, a);
    Mul(p[0], e, f);
    Mul(p[1], h, g);
    Mul(p[2], g, f);
    Mul(p[3], e, h);
}

void PointSwap(gf p[4], gf q[4], uint8_t b)
{
    for (int i = 0; i < 4; ++i)
        Select(p[i], q[i], b);
}

void PointPack(uint8_t* r, gf p[4])
{
    gf tx, ty, zi;
    Invert(zi, p[2]);
    Mul(tx, p[0], zi);
    Mul(ty, p[1], zi);
    Pack(r, ty);
    r[31] ^= Parity(tx) << 7;
}

void ScalarMult(gf p[4], gf q[4], const uint8_t* s)
{
    const Constants& k = K();
    Set(p[0], k.zero);
    Set(p[1], k.one);
    Set(p[2], k.one);
    Set(p[3], k.zero);
    for (int i = 255; i >= 0; --i) {
        uint8_t b = (s[i / 8] >> (i & 7)) & 1;
        PointSwap(p, q, b);
        PointAdd(q, p);
        PointAdd(p, p);
        PointSwap(p, q, b);
    }
}

void ScalarBase(gf p[4], const uint8_t* s)
{
    const Constants& k = K();
    gf q[4];
    Set(q[0], k.bx);
    Set(q[1], k.by);
    Set(q[2], k.one);
    Mul(q[3], k.bx, k.by);
    ScalarMult(p, q, s);
}

// the point of an encoding, negated (what verifying needs); false when it is not on the curve
bool UnpackNeg(gf r[4], const uint8_t p[32], const Constants& k)
{
    gf t, chk, num, den, den2, den4, den6;
    Set(r[2], k.one);
    Unpack(r[1], p);
    Square(num, r[1]);
    Mul(den, num, k.d);
    Sub(num, num, r[2]);
    Add(den, r[2], den);
    Square(den2, den);
    Square(den4, den2);
    Mul(den6, den4, den2);
    Mul(t, den6, num);
    Mul(t, t, den);
    Pow2523(t, t);
    Mul(t, t, num);
    Mul(t, t, den);
    Mul(t, t, den);
    Mul(r[0], t, den);
    Square(chk, r[0]);
    Mul(chk, chk, den);
    if (NotEqual(chk, num))
        Mul(r[0], r[0], k.sqrtm1);
    Square(chk, r[0]);
    Mul(chk, chk, den);
    if (NotEqual(chk, num))
        return false;
    if (Parity(r[0]) == (p[31] >> 7))
        Sub(r[0], k.zero, r[0]);
    Mul(r[3], r[0], r[1]);
    return true;
}

const Constants& K()
{
    static const Constants k = [] {
        Constants c;
        gf a = {121665}, b = {121666}, inv;
        Invert(inv, b);
        Mul(c.d, a, inv);
        Sub(c.d, c.zero, c.d);  // d = -121665/121666
        Add(c.d2, c.d, c.d);
        gf two = {2};
        PowAllOnes(c.sqrtm1, two, 252, 2, -1);  // 2^((p-1)/4) = 2^(2^253-5)
        // the base point: y = 4/5 with an even x (its encoding 0x58, 0x66...)
        uint8_t enc[32];
        enc[0] = 0x58;
        memset(enc + 1, 0x66, 31);
        gf p[4];
        UnpackNeg(p, enc, c);
        Sub(c.bx, c.zero, p[0]);
        Set(c.by, p[1]);
        return c;
    }();
    return k;
}

// the group's order L, little-endian
const i64 kL[32] = {0xed, 0xd3, 0xf5, 0x5c, 0x1a, 0x63, 0x12, 0x58, 0xd6, 0x9c, 0xf7, 0xa2, 0xde, 0xf9, 0xde, 0x14,
                    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0x10};

void ModL(uint8_t* r, i64 x[64])
{
    i64 carry;
    for (int i = 63; i >= 32; --i) {
        carry = 0;
        int j;
        for (j = i - 32; j < i - 12; ++j) {
            x[j] += carry - 16 * x[i] * kL[j - (i - 32)];
            carry = (x[j] + 128) >> 8;
            x[j] -= carry * 256;
        }
        x[j] += carry;
        x[i] = 0;
    }
    carry = 0;
    for (int j = 0; j < 32; ++j) {
        x[j] += carry - (x[31] >> 4) * kL[j];
        carry = x[j] >> 8;
        x[j] &= 255;
    }
    for (int j = 0; j < 32; ++j)
        x[j] -= carry * kL[j];
    for (int i = 0; i < 32; ++i) {
        x[i + 1] += x[i] >> 8;
        r[i] = uint8_t(x[i] & 255);
    }
}

void Reduce(uint8_t* r)  // 64 bytes in, 32 out (mod L)
{
    i64 x[64];
    for (int i = 0; i < 64; ++i)
        x[i] = r[i];
    memset(r, 0, 64);
    ModL(r, x);
}

// S < L (the scalar of a signature in its one canonical form)
bool BelowL(const uint8_t* s)
{
    for (int i = 31; i >= 0; --i) {
        if (s[i] < kL[i])
            return true;
        if (s[i] > kL[i])
            return false;
    }
    return false;  // equal
}

Sha512Digest HashParts(const uint8_t* a, size_t na, const uint8_t* b, size_t nb, const uint8_t* c, size_t nc)
{
    std::vector<uint8_t> buf;
    buf.reserve(na + nb + nc);
    buf.insert(buf.end(), a, a + na);
    buf.insert(buf.end(), b, b + nb);
    if (nc)
        buf.insert(buf.end(), c, c + nc);
    return Sha512(buf.data(), buf.size());
}

void Expand(const uint8_t seed[32], uint8_t d[64])
{
    Sha512Digest h = Sha512(seed, 32);
    memcpy(d, h.data(), 64);
    d[0] &= 248;
    d[31] &= 127;
    d[31] |= 64;
}

} // namespace

void Ed25519PublicKey(const uint8_t seed[32], uint8_t pub[32])
{
    uint8_t d[64];
    gf p[4];
    Expand(seed, d);
    ScalarBase(p, d);
    PointPack(pub, p);
    memset(d, 0, sizeof d);
}

void Ed25519Sign(uint8_t sig[64], const uint8_t* msg, size_t size, const uint8_t seed[32], const uint8_t pub[32])
{
    uint8_t d[64], r[64], h[64];
    Expand(seed, d);
    Sha512Digest rh = HashParts(d + 32, 32, msg, size, nullptr, 0);
    memcpy(r, rh.data(), 64);
    Reduce(r);
    gf p[4];
    ScalarBase(p, r);
    PointPack(sig, p);
    Sha512Digest hh = HashParts(sig, 32, pub, 32, msg, size);
    memcpy(h, hh.data(), 64);
    Reduce(h);
    i64 x[64] = {};
    for (int i = 0; i < 32; ++i)
        x[i] = r[i];
    for (int i = 0; i < 32; ++i)
        for (int j = 0; j < 32; ++j)
            x[i + j] += i64(h[i]) * d[j];
    ModL(sig + 32, x);
    memset(d, 0, sizeof d);
    memset(r, 0, sizeof r);
}

bool Ed25519Verify(const uint8_t sig[64], const uint8_t* msg, size_t size, const uint8_t pub[32])
{
    const Constants& k = K();
    gf p[4], q[4];
    if (!BelowL(sig + 32) || !UnpackNeg(q, pub, k))
        return false;
    uint8_t h[64], t[32];
    Sha512Digest hh = HashParts(sig, 32, pub, 32, msg, size);
    memcpy(h, hh.data(), 64);
    Reduce(h);
    ScalarMult(p, q, h);  // -[h]A
    ScalarBase(q, sig + 32);  // [S]B
    PointAdd(p, q);
    PointPack(t, p);
    return Differ32(sig, t) == 0;  // R = [S]B - [h]A
}

bool Ed25519SelfTest()
{
    struct Vector {
        const char *seed, *pub, *msg, *sig;
    };
    // RFC 8032 section 7.1, tests 1-3
    static const Vector vectors[] = {
        {"9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60",
         "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a", "",
         "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b"},
        {"4ccd089b28ff96da9db6c346ec114e0f5b8a319f35aba624da8cf6ed4fb8a6fb",
         "3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c", "72",
         "92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da085ac1e43e15996e458f3613d0f11d8c387b2eaeb4302aeeb00d291612bb0c00"},
        {"c5aa8df43f9f837bedb7442f31dcb7b166d38535076f094b85ce3a2e0b4458f7",
         "fc51cd8e6218a1a38da47ed00230f0580816ed13ba3303ac5deb911548908025", "af82",
         "6291d657deec24024827e69c3abe01a30ce548a284743a445e3680d7db5ac3ac18ff9b538d16f290ae67f760984dc6594a7c15e9716ed28dc027beceea1ec40a"},
    };
    for (const Vector& v : vectors) {
        uint8_t seed[32], pub[32], want[64], sig[64], made[32];
        std::vector<uint8_t> msg(strlen(v.msg) / 2);
        if (!FromHex(v.seed, seed, 32) || !FromHex(v.pub, pub, 32) || !FromHex(v.sig, want, 64) ||
            !FromHex(v.msg, msg.data(), msg.size()))
            return false;
        Ed25519PublicKey(seed, made);
        if (memcmp(made, pub, 32) != 0)
            return false;
        Ed25519Sign(sig, msg.data(), msg.size(), seed, pub);
        if (memcmp(sig, want, 64) != 0 || !Ed25519Verify(sig, msg.data(), msg.size(), pub))
            return false;
        sig[10] ^= 1;  // a changed signature
        if (Ed25519Verify(sig, msg.data(), msg.size(), pub))
            return false;
    }
    return true;
}

} // namespace h2::levelfmt::crypto
