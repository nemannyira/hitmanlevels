#include "screenshot.h"

#include <windows.h>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "config.h"
#include "d3d8types.h"
#include "display.h"
#include "log.h"

using namespace d3d8;

namespace h2 {

namespace {

constexpr int kDev_GetDisplayMode = 8;
constexpr int kDev_CreateImageSurface = 27;
constexpr int kDev_GetFrontBuffer = 30;
constexpr int kSurf_Release = 2;
constexpr int kSurf_LockRect = 9;
constexpr int kSurf_UnlockRect = 10;

bool WriteBmp(const std::string& path, int w, int h, const unsigned char* bits, int pitch)
{
    FILE* f = fopen(path.c_str(), "wb");
    if (!f)
        return false;
    BITMAPFILEHEADER fh = {};
    BITMAPINFOHEADER ih = {};
    ih.biSize = sizeof(ih);
    ih.biWidth = w;
    ih.biHeight = -h;  // top-down
    ih.biPlanes = 1;
    ih.biBitCount = 32;
    fh.bfType = 0x4D42;
    fh.bfOffBits = sizeof(fh) + sizeof(ih);
    fh.bfSize = fh.bfOffBits + DWORD(w) * h * 4;
    fwrite(&fh, sizeof(fh), 1, f);
    fwrite(&ih, sizeof(ih), 1, f);
    for (int y = 0; y < h; ++y)
        fwrite(bits + y * pitch, 4, w, f);
    fclose(f);
    return true;
}

uint32_t Crc32(const unsigned char* p, size_t n, uint32_t crc = 0)
{
    static uint32_t table[256];
    if (!table[1])
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k)
                c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
    crc = ~crc;
    for (size_t i = 0; i < n; ++i)
        crc = table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

void Be32(std::string& out, uint32_t v)
{
    for (int s = 24; s >= 0; s -= 8)
        out += char((v >> s) & 0xFF);
}

void Chunk(std::string& out, const char* type, const std::string& data)
{
    Be32(out, uint32_t(data.size()));
    std::string body = std::string(type, 4) + data;
    out += body;
    Be32(out, Crc32(reinterpret_cast<const unsigned char*>(body.data()), body.size()));
}

// rows of RGB (each led by filter 0) -> a PNG file's bytes
std::string EncodePng(int w, int h, const std::vector<unsigned char>& rgb)
{
    std::string raw;
    raw.reserve(size_t(h) * (w * 3 + 1));
    for (int y = 0; y < h; ++y) {
        raw += '\0';
        raw.append(reinterpret_cast<const char*>(&rgb[size_t(y) * w * 3]), size_t(w) * 3);
    }
    std::string z = "\x78\x01";  // zlib, no compression: stored blocks of at most 65535 bytes
    for (size_t at = 0; at < raw.size() || at == 0;) {
        size_t n = std::min<size_t>(65535, raw.size() - at);
        z += char(at + n == raw.size() ? 1 : 0);
        z += char(n & 0xFF);
        z += char(n >> 8);
        z += char(~n & 0xFF);
        z += char((~n >> 8) & 0xFF);
        z.append(raw, at, n);
        at += n;
        if (n == 0)
            break;
    }
    uint32_t a = 1, b = 0;  // Adler-32
    for (unsigned char c : raw) {
        a = (a + c) % 65521;
        b = (b + a) % 65521;
    }
    Be32(z, (b << 16) | a);
    std::string png = "\x89PNG\r\n\x1a\n", ihdr;
    Be32(ihdr, uint32_t(w));
    Be32(ihdr, uint32_t(h));
    ihdr += std::string("\x08\x02\x00\x00\x00", 5);  // 8 bits, RGB, deflate, no filter method, no interlace
    Chunk(png, "IHDR", ihdr);
    Chunk(png, "IDAT", z);
    Chunk(png, "IEND", "");
    return png;
}

} // namespace

bool SaveBackBuffer(void* device, const std::string& path)
{
    constexpr int kDev_GetBackBuffer = 16;
    constexpr int kSurf_GetDesc = 8;
    void* surf = nullptr;
    if (Call<HRESULT>(device, kDev_GetBackBuffer, UINT(0), DWORD(0), &surf) < 0 || !surf)
        return false;
    DWORD desc[8] = {};  // D3DSURFACE_DESC: Format, Type, Usage, Pool, Size, MultiSampleType, Width, Height
    bool ok = false;
    if (Call<HRESULT>(surf, kSurf_GetDesc, desc) >= 0 && (desc[0] == 21 || desc[0] == 22)) {  // A8R8G8B8 / X8R8G8B8
        D3DLOCKED_RECT lr;
        if (Call<HRESULT>(surf, kSurf_LockRect, &lr, static_cast<const RECT*>(nullptr), DWORD(0x10)) >= 0) {  // READONLY
            ok = WriteBmp(path, int(desc[6]), int(desc[7]), static_cast<unsigned char*>(lr.pBits), lr.Pitch);
            Call<HRESULT>(surf, kSurf_UnlockRect);
        }
    }
    Call<ULONG>(surf, kSurf_Release);
    return ok;
}

bool SaveScreenshot(void* device, const std::string& path)
{
    D3DDISPLAYMODE mode;
    if (Call<HRESULT>(device, kDev_GetDisplayMode, &mode) < 0)
        return false;
    void* surf = nullptr;
    if (Call<HRESULT>(device, kDev_CreateImageSurface, mode.Width, mode.Height, DWORD(D3DFMT_A8R8G8B8), &surf) < 0)
        return false;
    bool ok = false;
    if (Call<HRESULT>(device, kDev_GetFrontBuffer, surf) >= 0) {
        D3DLOCKED_RECT lr;
        if (Call<HRESULT>(surf, kSurf_LockRect, &lr, static_cast<const RECT*>(nullptr), DWORD(0)) >= 0) {
            ok = WriteBmp(path, int(mode.Width), int(mode.Height), static_cast<unsigned char*>(lr.pBits), lr.Pitch);
            Call<HRESULT>(surf, kSurf_UnlockRect);
        }
    }
    Call<ULONG>(surf, kSurf_Release);
    return ok;
}

bool SavePreviewPng(void* device, const std::string& path, int width)
{
    D3DDISPLAYMODE mode;
    if (Call<HRESULT>(device, kDev_GetDisplayMode, &mode) < 0 || mode.Width < 2 || mode.Height < 2)
        return false;
    void* surf = nullptr;
    if (Call<HRESULT>(device, kDev_CreateImageSurface, mode.Width, mode.Height, DWORD(D3DFMT_A8R8G8B8), &surf) < 0)
        return false;
    int sw = int(mode.Width), sh = int(mode.Height);
    int w = std::min(width, sw), h = std::max(1, int(int64_t(sh) * w / sw));
    std::vector<unsigned char> rgb(size_t(w) * h * 3);
    bool ok = false;
    if (Call<HRESULT>(device, kDev_GetFrontBuffer, surf) >= 0) {
        D3DLOCKED_RECT lr;
        if (Call<HRESULT>(surf, kSurf_LockRect, &lr, static_cast<const RECT*>(nullptr), DWORD(0)) >= 0) {
            const unsigned char* bits = static_cast<const unsigned char*>(lr.pBits);
            for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x) {
                    int x0 = x * sw / w, x1 = std::max(x0 + 1, (x + 1) * sw / w);
                    int y0 = y * sh / h, y1 = std::max(y0 + 1, (y + 1) * sh / h);
                    unsigned sum[3] = {}, n = 0;
                    for (int yy = y0; yy < y1; ++yy)
                        for (int xx = x0; xx < x1; ++xx, ++n) {
                            const unsigned char* p = bits + yy * lr.Pitch + xx * 4;  // B G R A
                            sum[0] += p[2];
                            sum[1] += p[1];
                            sum[2] += p[0];
                        }
                    unsigned char* o = &rgb[(size_t(y) * w + x) * 3];
                    for (int c = 0; c < 3; ++c)
                        o[c] = static_cast<unsigned char>(sum[c] / n);
                }
            Call<HRESULT>(surf, kSurf_UnlockRect);
            ok = true;
        }
    }
    Call<ULONG>(surf, kSurf_Release);
    if (!ok)
        return false;
    std::string png = EncodePng(w, h, rgb);
    FILE* f = fopen(path.c_str(), "wb");
    if (!f)
        return false;
    ok = fwrite(png.data(), 1, png.size(), f) == png.size();
    fclose(f);
    return ok;
}

void PeriodicScreenshot(void* device)
{
    if (g_config.screenshotEvery <= 0)
        return;
    static double next = g_config.screenshotEvery * 1000.0;
    double now = NowMs();
    if (now < next)
        return;
    next = now + g_config.screenshotEvery * 1000.0;
    std::string dir = GameDir() + "Screens";
    CreateDirectoryA(dir.c_str(), nullptr);
    char name[64];
    snprintf(name, sizeof(name), "\\shot_%04d.bmp", int(now / 1000.0));
    if (!SaveScreenshot(device, dir + name))
        Log("screenshot failed");
}

} // namespace h2
