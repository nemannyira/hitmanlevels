#include "screenshot.h"

#include <windows.h>
#include <cstdio>
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
