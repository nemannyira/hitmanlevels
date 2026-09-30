#include "dinput.h"

#include <atomic>
#include <cstring>

#include "../hook.h"
#include "../log.h"

namespace h2 {

namespace {

// vtable slots
constexpr int kDI_CreateDevice = 3;       // IDirectInput8A
constexpr int kDev_GetDeviceState = 9;    // IDirectInputDevice8A
constexpr int kDev_GetDeviceData = 10;
constexpr int kDev_SetCooperativeLevel = 13;

constexpr DWORD DISCL_EXCLUSIVE = 0x1, DISCL_NONEXCLUSIVE = 0x2;

void* VtableEntry(void* obj, int index) { return (*reinterpret_cast<void***>(obj))[index]; }

const GUID kSysMouse = {0x6F1D2B60, 0xD5A0, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};

struct DIMOUSESTATE {
    LONG lX, lY, lZ;
    BYTE rgbButtons[4];
};

std::atomic<bool> g_blocked{false};
void* g_mouse;  // IDirectInputDevice8A* of the system mouse
std::atomic<long> g_dx, g_dy, g_wheel;

using SetCooperativeLevel_t = HRESULT(WINAPI*)(void* self, HWND wnd, DWORD flags);
SetCooperativeLevel_t oSetCooperativeLevel;
HRESULT WINAPI hkSetCooperativeLevel(void* self, HWND wnd, DWORD flags)
{
    if (flags & DISCL_EXCLUSIVE)
        flags = (flags & ~DISCL_EXCLUSIVE) | DISCL_NONEXCLUSIVE;
    return oSetCooperativeLevel(self, wnd, flags);
}

using GetDeviceState_t = HRESULT(WINAPI*)(void* self, DWORD size, void* data);
GetDeviceState_t oGetDeviceState;
HRESULT WINAPI hkGetDeviceState(void* self, DWORD size, void* data)
{
    HRESULT hr = oGetDeviceState(self, size, data);
    if (hr >= 0 && self == g_mouse && size >= sizeof(DIMOUSESTATE)) {
        auto m = static_cast<DIMOUSESTATE*>(data);
        g_dx += m->lX;
        g_dy += m->lY;
        g_wheel += m->lZ;
    }
    if (hr >= 0 && g_blocked)
        memset(data, 0, size);
    return hr;
}

using GetDeviceData_t = HRESULT(WINAPI*)(void* self, DWORD objSize, void* objData, DWORD* inOut, DWORD flags);
GetDeviceData_t oGetDeviceData;
HRESULT WINAPI hkGetDeviceData(void* self, DWORD objSize, void* objData, DWORD* inOut, DWORD flags)
{
    HRESULT hr = oGetDeviceData(self, objSize, objData, inOut, flags);
    if (hr >= 0 && self == g_mouse && objData && inOut && objSize >= 8) {
        // DIDEVICEOBJECTDATA: dwOfs, dwData, ...; DIMOFS_X/Y/Z = 0/4/8
        for (DWORD i = 0; i < *inOut; ++i) {
            auto e = reinterpret_cast<const DWORD*>(static_cast<const char*>(objData) + i * objSize);
            long v = static_cast<long>(e[1]);
            if (e[0] == 0) g_dx += v;
            else if (e[0] == 4) g_dy += v;
            else if (e[0] == 8) g_wheel += v;
        }
    }
    if (hr >= 0 && g_blocked && inOut)
        *inOut = 0;  // events were drained, the game sees none
    return hr;
}

using CreateDevice_t = HRESULT(WINAPI*)(void* self, REFGUID guid, void** device, void* outer);
CreateDevice_t oCreateDevice;
HRESULT WINAPI hkCreateDevice(void* self, REFGUID guid, void** device, void* outer)
{
    HRESULT hr = oCreateDevice(self, guid, device, outer);
    if (hr < 0 || !device || !*device)
        return hr;
    if (IsEqualGUID(guid, kSysMouse))
        g_mouse = *device;
    if (!oGetDeviceState) {  // all devices share one vtable
        Hook("IDirectInputDevice8::SetCooperativeLevel", VtableEntry(*device, kDev_SetCooperativeLevel),
             hkSetCooperativeLevel, &oSetCooperativeLevel);
        Hook("IDirectInputDevice8::GetDeviceState", VtableEntry(*device, kDev_GetDeviceState), hkGetDeviceState,
             &oGetDeviceState);
        Hook("IDirectInputDevice8::GetDeviceData", VtableEntry(*device, kDev_GetDeviceData), hkGetDeviceData,
             &oGetDeviceData);
    }
    return hr;
}

} // namespace

void HookDirectInput(void* directInput8)
{
    if (!directInput8 || oCreateDevice)
        return;
    Hook("IDirectInput8::CreateDevice", VtableEntry(directInput8, kDI_CreateDevice), hkCreateDevice, &oCreateDevice);
}

void SetGameInputBlocked(bool blocked) { g_blocked = blocked; }
bool GameInputBlocked() { return g_blocked; }

MouseDelta TakeMouseDelta() { return {g_dx.exchange(0), g_dy.exchange(0), g_wheel.exchange(0)}; }

} // namespace h2
