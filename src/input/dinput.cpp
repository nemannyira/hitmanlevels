#include "dinput.h"

#include <atomic>
#include <cstring>

#include "../devcmd.h"
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
const GUID kSysKeyboard = {0x6F1D2B61, 0xD5A0, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};

struct DIMOUSESTATE {
    LONG lX, lY, lZ;
    BYTE rgbButtons[4];
};

std::atomic<bool> g_blocked{false};
void* g_mouse;     // IDirectInputDevice8A* of the system mouse
void* g_keyboard;  // ... and keyboard
unsigned char g_injectedKeys[256];      // dev commands: keys reported as held by buffered reads
unsigned char g_injectedButtons[4];
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
    if (!g_blocked && DevInjecting() && (self == g_keyboard || self == g_mouse)) {
        if (hr < 0) {  // not acquired (window in the background): report only the injected input
            memset(data, 0, size);
            hr = 0;
        }
        if (self == g_keyboard && size >= 256)
            DevInjectKeyboard(static_cast<unsigned char*>(data));
        else if (self == g_mouse && size >= sizeof(DIMOUSESTATE)) {
            auto m = static_cast<DIMOUSESTATE*>(data);
            DevInjectMouse(&m->lX, &m->lY, m->rgbButtons, &m->lZ);
        }
    }
    return hr;
}

using GetDeviceData_t = HRESULT(WINAPI*)(void* self, DWORD objSize, void* objData, DWORD* inOut, DWORD flags);
GetDeviceData_t oGetDeviceData;
// Buffered reads see injected keys / buttons as press and release events.
void AppendInjectedEvents(void* self, DWORD objSize, void* objData, DWORD* inOut, DWORD capacity)
{
    unsigned char now[256] = {};
    long dx = 0, dy = 0, wheel = 0;
    unsigned char buttons[4] = {};
    if (self == g_keyboard)
        DevInjectKeyboard(now);
    unsigned char* last = self == g_keyboard ? g_injectedKeys : g_injectedButtons;
    int count = self == g_keyboard ? 256 : 4;
    if (self == g_mouse) {
        if (!DevInjecting())
            memset(buttons, 0, sizeof(buttons));
        else
            DevInjectMouse(&dx, &dy, buttons, &wheel);
        memcpy(now, buttons, 4);
    }
    for (int k = 0; k < count && *inOut < capacity; ++k) {
        if (now[k] == last[k])
            continue;
        auto e = reinterpret_cast<DWORD*>(static_cast<char*>(objData) + *inOut * objSize);
        memset(e, 0, objSize);
        e[0] = self == g_keyboard ? DWORD(k) : DWORD(12 + k);  // DIK code / DIMOFS_BUTTON0 + k
        e[1] = now[k];
        e[2] = GetTickCount();
        ++*inOut;
        last[k] = now[k];
    }
    if (self == g_mouse) {
        for (int axis = 0; axis < 3 && *inOut < capacity; ++axis) {
            long v = axis == 0 ? dx : axis == 1 ? dy : wheel;
            if (!v)
                continue;
            auto e = reinterpret_cast<DWORD*>(static_cast<char*>(objData) + *inOut * objSize);
            memset(e, 0, objSize);
            e[0] = DWORD(axis * 4);
            e[1] = DWORD(v);
            e[2] = GetTickCount();
            ++*inOut;
        }
    }
}

HRESULT WINAPI hkGetDeviceData(void* self, DWORD objSize, void* objData, DWORD* inOut, DWORD flags)
{
    DWORD capacity = inOut ? *inOut : 0;
    HRESULT hr = oGetDeviceData(self, objSize, objData, inOut, flags);
    if (!g_blocked && objData && inOut && objSize >= 16 && !(flags & 1) && (self == g_keyboard || self == g_mouse)) {
        bool any = DevInjecting() || memchr(self == g_keyboard ? g_injectedKeys : g_injectedButtons, 0x80,
                                             self == g_keyboard ? 256 : 4);
        if (any) {
            if (hr < 0) {
                *inOut = 0;
                hr = 0;
            }
            AppendInjectedEvents(self, objSize, objData, inOut, capacity);
        }
    }
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
    if (IsEqualGUID(guid, kSysKeyboard))
        g_keyboard = *device;
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
