#include "screenshot.h"

#include <windows.h>
#include <wincodec.h>
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

constexpr int kDev_GetBackBuffer = 16;
constexpr int kDev_CreateImageSurface = 27;
constexpr int kDev_CopyRects = 28;
constexpr int kSurf_Release = 2;
constexpr int kSurf_GetDesc = 8;
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

// The back buffer (the game's own frame, whatever covers its window; before Present: without the overlay
// when called before it is drawn) copied into a system memory surface the caller locks and releases.
// Never the front buffer: in a window that is the whole screen, other programs' windows too (#48).
void* CopyBackBuffer(void* device, int& w, int& h)
{
    void* back = nullptr;
    if (Call<HRESULT>(device, kDev_GetBackBuffer, UINT(0), DWORD(0), &back) < 0 || !back)
        return nullptr;
    DWORD desc[8] = {};  // D3DSURFACE_DESC: Format, Type, Usage, Pool, Size, MultiSampleType, Width, Height
    void* mem = nullptr;
    if (Call<HRESULT>(back, kSurf_GetDesc, desc) >= 0 && (desc[0] == 21 || desc[0] == 22) && desc[5] == 0 &&
        desc[6] >= 2 && desc[7] >= 2 &&  // (A8R8G8B8 / X8R8G8B8, not multisampled: CopyRects takes it)
        Call<HRESULT>(device, kDev_CreateImageSurface, UINT(desc[6]), UINT(desc[7]), desc[0], &mem) >= 0 && mem &&
        Call<HRESULT>(device, kDev_CopyRects, back, static_cast<const RECT*>(nullptr), UINT(0), mem,
                      static_cast<const POINT*>(nullptr)) < 0) {
        Call<ULONG>(mem, kSurf_Release);
        mem = nullptr;
    }
    Call<ULONG>(back, kSurf_Release);
    w = int(desc[6]);
    h = int(desc[7]);
    return mem;
}

} // namespace

bool SaveBackBuffer(void* device, const std::string& path)
{
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
    int w = 0, h = 0;
    void* surf = CopyBackBuffer(device, w, h);
    if (!surf)
        return false;
    bool ok = false;
    D3DLOCKED_RECT lr;
    if (Call<HRESULT>(surf, kSurf_LockRect, &lr, static_cast<const RECT*>(nullptr), DWORD(0x10)) >= 0) {  // READONLY
        ok = WriteBmp(path, w, h, static_cast<unsigned char*>(lr.pBits), lr.Pitch);
        Call<HRESULT>(surf, kSurf_UnlockRect);
    }
    Call<ULONG>(surf, kSurf_Release);
    return ok;
}

bool SavePreviewPng(void* device, const std::string& path, int width)
{
    int sw = 0, sh = 0;
    void* surf = CopyBackBuffer(device, sw, sh);
    if (!surf)
        return false;
    int w = std::min(width, sw), h = std::max(1, int(int64_t(sh) * w / sw));
    std::vector<unsigned char> rgb(size_t(w) * h * 3);
    bool ok = false;
    D3DLOCKED_RECT lr;
    if (Call<HRESULT>(surf, kSurf_LockRect, &lr, static_cast<const RECT*>(nullptr), DWORD(0x10)) >= 0) {
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
    Call<ULONG>(surf, kSurf_Release);
    if (!ok)
        return false;
    return WritePng(path, w, h, rgb);
}

bool WritePng(const std::string& path, int w, int h, const std::vector<unsigned char>& rgb)
{
    if (w <= 0 || h <= 0 || rgb.size() < size_t(w) * h * 3)
        return false;
    std::vector<unsigned char> bgr(rgb.begin(), rgb.begin() + size_t(w) * h * 3);  // (WIC's PNG takes BGR)
    for (size_t i = 0; i < bgr.size(); i += 3)
        std::swap(bgr[i], bgr[i + 2]);
    HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    IWICImagingFactory* factory = nullptr;
    IWICStream* stream = nullptr;
    IWICBitmapEncoder* encoder = nullptr;
    IWICBitmapFrameEncode* frame = nullptr;
    WICPixelFormatGUID format = GUID_WICPixelFormat24bppBGR;
    std::wstring wpath(path.begin(), path.end());
    bool ok = SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) &&
              SUCCEEDED(factory->CreateStream(&stream)) &&
              SUCCEEDED(stream->InitializeFromFilename(wpath.c_str(), GENERIC_WRITE)) &&
              SUCCEEDED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) &&
              SUCCEEDED(encoder->Initialize(stream, WICBitmapEncoderNoCache)) &&
              SUCCEEDED(encoder->CreateNewFrame(&frame, nullptr)) && SUCCEEDED(frame->Initialize(nullptr)) &&
              SUCCEEDED(frame->SetSize(UINT(w), UINT(h))) && SUCCEEDED(frame->SetPixelFormat(&format)) &&
              format == GUID_WICPixelFormat24bppBGR &&
              SUCCEEDED(frame->WritePixels(UINT(h), UINT(w) * 3, UINT(bgr.size()), bgr.data())) &&
              SUCCEEDED(frame->Commit()) && SUCCEEDED(encoder->Commit());
    for (IUnknown* u : {static_cast<IUnknown*>(frame), static_cast<IUnknown*>(encoder), static_cast<IUnknown*>(stream),
                        static_cast<IUnknown*>(factory)})
        if (u)
            u->Release();
    if (SUCCEEDED(init))
        CoUninitialize();
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
