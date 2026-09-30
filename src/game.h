#pragma once
// Addresses in hitman2.exe (Steam build 275, image base 0x400000, unpacked).
// Names in comments are the game's own exported (decorated) symbols.
#include <cstddef>
#include <cstdint>

namespace h2::game {

constexpr uintptr_t kOep = 0x5A184E;

// ZSysInterface (partial). Global instance pointer: ?g_pSysInterface@@3PAVZSysInterface@@A
struct ZSysInterface {
    unsigned char _pad0[0x57];
    bool fullscreen;        // 0x57
    unsigned char _pad1[0x64 - 0x58];
    int width;              // 0x64  "Resolution" from Hitman2.ini
    int height;             // 0x68
    int windowWidth;        // 0x6C  copied from width/height by RenderD3D
    int windowHeight;       // 0x70
};
static_assert(offsetof(ZSysInterface, width) == 0x64);
static_assert(offsetof(ZSysInterface, windowHeight) == 0x70);

// ZCAMERA (partial), as read by RenderD3D when rendering through it
struct ZCAMERA {
    static constexpr uint32_t kOverlay = 0x8000;  // renders on top without clearing (UI, HUD)

    unsigned char _pad0[0x44];
    uint32_t flags;         // 0x44  bit 0: orthographic, bit 1: window changed, kOverlay
    unsigned char _pad1[0x4C - 0x48];
    float windowWidth;      // 0x4C  target window size in (game) pixels
    float windowHeight;     // 0x50
    float focal;            // 0x54  0.5 / tan(fov / 2)
    float fov;              // 0x58  horizontal field of view, radians
    unsigned char _pad2[0x60 - 0x5C];
    float nearPlane;        // 0x60  (renderer clamps to >= 5)
    float farPlane;         // 0x64
};
static_assert(offsetof(ZCAMERA, fov) == 0x58);

// Import address table slots
constexpr uintptr_t IAT_ScreenToClient = 0x6391F4;

// The camera the world was last rendered through (non-overlay, full screen
// width); tracked by the RenderCamera hook, cleared on scene changes.
ZCAMERA* MainCamera();
void NoteRenderedCamera(ZCAMERA* cam);
void ResetMainCamera();

constexpr uintptr_t g_pSysInterface = 0x6A6C5C;
inline ZSysInterface* SysInterface() { return *reinterpret_cast<ZSysInterface**>(g_pSysInterface); }

// ZEngineDataBase
constexpr uintptr_t ZEngineDataBase_MainLoop = 0x422920;           // ?MainLoop@ZEngineDataBase@@QAEX_N@Z
constexpr uintptr_t ZEngineDataBase_PushScene = 0x41B460;          // ?PushScene@ZEngineDataBase@@QAEPAUNewScene@@PBD@Z
constexpr uintptr_t ZEngineDataBase_ControlSceneChange = 0x421D50; // ?ControlSceneChange@ZEngineDataBase@@QAEXXZ
constexpr uintptr_t ZEngineDataBase_FrameUpdate = 0x4228E0;        // ?FrameUpdate@ZEngineDataBase@@QAEXXZ

// ZGEOM transform virtuals (all forward to the ZBaseGeom node at geom+8).
// Matrices are 3x3 floats with rows [Z axis, Y axis, X axis] = forward, up, right;
// world is left-handed with +Y up, units are centimetres.
constexpr int ZGEOM_SetLocalTransform = 0x78 / 4;  // (float mat[9], float pos[3]) relative to the parent node
constexpr int ZGEOM_SetWorldTransform = 0x80 / 4;  // (float mat[9], float pos[3])
constexpr int ZGEOM_GetWorldTransform = 0x84 / 4;  // (float mat[9], float pos[3])

template <typename Geom>
void GetWorldTransform(Geom* geom, float mat[9], float pos[3])
{
    using Fn = void(__thiscall*)(Geom*, float*, float*);
    (*reinterpret_cast<Fn**>(geom))[ZGEOM_GetWorldTransform](geom, mat, pos);
}

template <typename Geom>
void SetLocalTransform(Geom* geom, const float mat[9], const float pos[3])
{
    using Fn = void(__thiscall*)(Geom*, const float*, const float*);
    (*reinterpret_cast<Fn**>(geom))[ZGEOM_SetLocalTransform](geom, mat, pos);
}

template <typename Geom>
void SetWorldTransform(Geom* geom, const float mat[9], const float pos[3])
{
    using Fn = void(__thiscall*)(Geom*, const float*, const float*);
    (*reinterpret_cast<Fn**>(geom))[ZGEOM_SetWorldTransform](geom, mat, pos);
}

// Worker threads
constexpr uintptr_t StreamThreadProc = 0x5F9E20; // polls request events, fseek/fread, Sleep(15)
constexpr uintptr_t SoundThreadProc = 0x5EA7D0;  // TIME_CRITICAL mixer loop, Sleep(15)

} // namespace h2::game
