#include "zip.h"

#include <windows.h>
#include <cctype>
#include <cstring>

namespace h2::engine {

namespace {

struct Bits {
    const uint8_t* p;
    const uint8_t* end;
    uint32_t buf = 0;
    int count = 0;
    bool bad = false;

    int Bit()
    {
        if (count == 0) {
            if (p >= end) {
                bad = true;
                return 0;
            }
            buf = *p++;
            count = 8;
        }
        int b = buf & 1;
        buf >>= 1;
        --count;
        return b;
    }
    uint32_t Get(int n)
    {
        uint32_t v = 0;
        for (int i = 0; i < n; ++i)
            v |= uint32_t(Bit()) << i;
        return v;
    }
};

// canonical Huffman code: counts per length, symbols by code
struct Huffman {
    uint16_t counts[16] = {};
    uint16_t symbols[320] = {};

    bool Build(const uint8_t* lengths, int n)
    {
        memset(counts, 0, sizeof(counts));
        for (int i = 0; i < n; ++i)
            counts[lengths[i]]++;
        counts[0] = 0;
        uint16_t offs[16] = {};
        for (int i = 1; i < 16; ++i)
            offs[i] = uint16_t(offs[i - 1] + counts[i - 1]);
        for (int i = 0; i < n; ++i)
            if (lengths[i])
                symbols[offs[lengths[i]]++] = uint16_t(i);
        return true;
    }
    int Decode(Bits& in) const
    {
        int code = 0, first = 0, index = 0;
        for (int len = 1; len < 16; ++len) {
            code |= in.Bit();
            int count = counts[len];
            if (code - count < first)
                return symbols[index + (code - first)];
            index += count;
            first += count;
            first <<= 1;
            code <<= 1;
            if (in.bad)
                return -1;
        }
        return -1;
    }
};

const uint16_t kLenBase[] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
                             35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
const uint8_t kLenExtra[] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
const uint16_t kDistBase[] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
                              257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
const uint8_t kDistExtra[] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

bool Codes(Bits& in, std::vector<uint8_t>& out, const Huffman& lit, const Huffman& dist)
{
    for (;;) {
        int sym = lit.Decode(in);
        if (sym < 0 || in.bad)
            return false;
        if (sym < 256) {
            out.push_back(uint8_t(sym));
        } else if (sym == 256) {
            return true;
        } else {
            sym -= 257;
            if (sym >= 29)
                return false;
            size_t len = kLenBase[sym] + in.Get(kLenExtra[sym]);
            int ds = dist.Decode(in);
            if (ds < 0 || ds >= 30)
                return false;
            size_t d = kDistBase[ds] + in.Get(kDistExtra[ds]);
            if (d > out.size())
                return false;
            size_t from = out.size() - d;
            for (size_t i = 0; i < len; ++i)
                out.push_back(out[from + i]);
        }
    }
}

std::string Normal(std::string s)
{
    for (char& c : s)
        c = c == '\\' ? '/' : char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

uint32_t U32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (uint32_t(p[3]) << 24); }
uint16_t U16(const uint8_t* p) { return uint16_t(p[0] | (p[1] << 8)); }

} // namespace

bool Inflate(const uint8_t* data, size_t n, std::vector<uint8_t>& out, size_t size)
{
    out.clear();
    out.reserve(size);
    Bits in{data, data + n};
    int last;
    do {
        last = in.Bit();
        int type = int(in.Get(2));
        if (type == 0) {
            in.count = 0;  // to a byte boundary
            if (in.p + 4 > in.end)
                return false;
            uint16_t len = U16(in.p);
            in.p += 4;
            if (in.p + len > in.end)
                return false;
            out.insert(out.end(), in.p, in.p + len);
            in.p += len;
        } else if (type == 1) {
            uint8_t l[288];
            int i = 0;
            for (; i < 144; ++i) l[i] = 8;
            for (; i < 256; ++i) l[i] = 9;
            for (; i < 280; ++i) l[i] = 7;
            for (; i < 288; ++i) l[i] = 8;
            Huffman lit, dist;
            lit.Build(l, 288);
            uint8_t dl[30];
            memset(dl, 5, sizeof(dl));
            dist.Build(dl, 30);
            if (!Codes(in, out, lit, dist))
                return false;
        } else if (type == 2) {
            int hlit = int(in.Get(5)) + 257, hdist = int(in.Get(5)) + 1, hclen = int(in.Get(4)) + 4;
            static const uint8_t order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
            uint8_t cl[19] = {};
            for (int i = 0; i < hclen; ++i)
                cl[order[i]] = uint8_t(in.Get(3));
            Huffman clh;
            clh.Build(cl, 19);
            uint8_t lens[320] = {};
            int idx = 0;
            while (idx < hlit + hdist) {
                int sym = clh.Decode(in);
                if (sym < 0)
                    return false;
                if (sym < 16) {
                    lens[idx++] = uint8_t(sym);
                } else {
                    int rep = 0;
                    uint8_t val = 0;
                    if (sym == 16) {
                        if (!idx)
                            return false;
                        val = lens[idx - 1];
                        rep = 3 + int(in.Get(2));
                    } else if (sym == 17) {
                        rep = 3 + int(in.Get(3));
                    } else {
                        rep = 11 + int(in.Get(7));
                    }
                    if (idx + rep > hlit + hdist)
                        return false;
                    while (rep--)
                        lens[idx++] = val;
                }
            }
            Huffman lit, dist;
            lit.Build(lens, hlit);
            dist.Build(lens + hlit, hdist);
            if (!Codes(in, out, lit, dist))
                return false;
        } else {
            return false;
        }
        if (in.bad)
            return false;
    } while (!last);
    return out.size() == size;
}

bool ReadZipEntry(const std::string& path, const std::string& name, std::vector<uint8_t>& out, std::string* error)
{
    auto fail = [&](const char* why) {
        if (error)
            *error = why;
        return false;
    };
    HANDLE f = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE)
        return fail("cannot open the archive");
    LARGE_INTEGER sz;
    GetFileSizeEx(f, &sz);
    std::vector<uint8_t> zip(size_t(sz.QuadPart));
    DWORD got = 0;
    bool ok = ReadFile(f, zip.data(), DWORD(zip.size()), &got, nullptr) && got == zip.size();
    CloseHandle(f);
    if (!ok || zip.size() < 22)
        return fail("cannot read the archive");
    // end of central directory
    size_t eocd = std::string::npos;
    for (size_t i = zip.size() - 22; i + 1 > 0 && zip.size() - i < 0x10000 + 22; --i) {
        if (U32(&zip[i]) == 0x06054B50) {
            eocd = i;
            break;
        }
        if (i == 0)
            break;
    }
    if (eocd == std::string::npos)
        return fail("not a zip archive");
    uint16_t entries = U16(&zip[eocd + 10]);
    size_t p = U32(&zip[eocd + 16]);
    std::string want = Normal(name);
    for (int e = 0; e < entries && p + 46 <= zip.size(); ++e) {
        if (U32(&zip[p]) != 0x02014B50)
            return fail("bad central directory");
        uint16_t method = U16(&zip[p + 10]);
        uint32_t csize = U32(&zip[p + 20]), usize = U32(&zip[p + 24]);
        uint16_t nlen = U16(&zip[p + 28]), xlen = U16(&zip[p + 30]), clen = U16(&zip[p + 32]);
        uint32_t local = U32(&zip[p + 42]);
        std::string n(reinterpret_cast<const char*>(&zip[p + 46]), nlen);
        p += 46 + nlen + xlen + clen;
        if (Normal(n) != want)
            continue;
        if (local + 30 > zip.size())
            return fail("bad local header");
        size_t data = local + 30 + U16(&zip[local + 26]) + U16(&zip[local + 28]);
        if (data + csize > zip.size())
            return fail("truncated entry");
        if (method == 0) {
            out.assign(zip.begin() + data, zip.begin() + data + csize);
            return true;
        }
        if (method != 8)
            return fail("unsupported compression");
        if (!Inflate(&zip[data], csize, out, usize))
            return fail("inflate failed");
        return true;
    }
    return fail("no such entry");
}

} // namespace h2::engine
