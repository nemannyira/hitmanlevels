#include "freecam.h"

#include <windows.h>
#include <intrin.h>
#include <algorithm>
#include <cmath>
#include <vector>

#include "imgui.h"

#include "../engine/geoms.h"
#include "../engine/messages.h"
#include "../fov.h"
#include "../game.h"
#include "../hook.h"
#include "../input/dinput.h"
#include "../loader.h"
#include "../log.h"
#include "../overlay/overlay.h"
#include "../scene.h"

namespace h2::editor {

namespace {

constexpr float kPi = 3.14159265f;

bool g_enabled;
bool g_panelVisible = true;
float g_pos[3];
float g_yaw, g_pitch;          // radians; yaw 0 looks along +Z, pitch > 0 looks up
float g_speed = 400.0f;        // world units (cm) per second
float g_lookSensitivity = 0.0025f;
float g_viewDistance = 4.0f;   // far plane multiplier while flying (the game's is short for an editor)
LARGE_INTEGER g_lastTick;

void Forward(float yaw, float pitch, float f[3])
{
    f[0] = std::sin(yaw) * std::cos(pitch);
    f[1] = std::sin(pitch);
    f[2] = std::cos(yaw) * std::cos(pitch);
}

// Engine matrix layout: rows [Z axis, Y axis, X axis] (forward, up, right).
void BuildMatrix(float yaw, float pitch, float m[9])
{
    float f[3];
    Forward(yaw, pitch, f);
    float r[3] = {f[2], 0.0f, -f[0]};  // up x forward (left-handed)
    float rl = std::sqrt(r[0] * r[0] + r[2] * r[2]);
    if (rl < 1e-6f) {
        r[0] = std::cos(yaw);
        r[2] = -std::sin(yaw);
        rl = 1.0f;
    }
    r[0] /= rl;
    r[2] /= rl;
    float u[3] = {f[1] * r[2] - f[2] * r[1], f[2] * r[0] - f[0] * r[2], f[0] * r[1] - f[1] * r[0]};  // f x r
    m[0] = f[0], m[1] = f[1], m[2] = f[2];
    m[3] = u[0], m[4] = u[1], m[5] = u[2];
    m[6] = r[0], m[7] = r[1], m[8] = r[2];
}

void CaptureFromCamera(game::ZCAMERA* cam)
{
    float m[9];
    game::GetWorldTransform(cam, m, g_pos);
    g_yaw = std::atan2(m[0], m[2]);
    float fy = m[1] < -1.0f ? -1.0f : (m[1] > 1.0f ? 1.0f : m[1]);
    g_pitch = std::asin(fy);
    // Just after a map loads the game's camera can sit in Hitman's head: back
    // off along the view (and up) so he is in sight, not around the lens.
    if (engine::ZGEOM* hitman = engine::Player()) {
        float h[3];
        engine::WorldPosition(hitman, h);
        float dx = g_pos[0] - h[0], dz = g_pos[2] - h[2], dy = g_pos[1] - h[1];
        if (dx * dx + dz * dz < 60.0f * 60.0f && dy > -20.0f && dy < 220.0f) {
            float f[3];
            Forward(g_yaw, g_pitch, f);
            for (int i = 0; i < 3; ++i)
                g_pos[i] -= f[i] * 250.0f;
            g_pos[1] += 80.0f;
        }
    }
    Log("free cam from %p: pos %.1f %.1f %.1f yaw %.1f pitch %.1f", cam, g_pos[0], g_pos[1], g_pos[2],
        g_yaw * 180 / kPi, g_pitch * 180 / kPi);
}

bool KeyDown(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

void UpdateControls(float dt)
{
    MouseDelta md = TakeMouseDelta();  // drain every frame so deltas do not pile up
    if (!overlay::IsOpen() || !ImGui::GetCurrentContext())
        return;
    ImGuiIO& io = ImGui::GetIO();
    bool looking = ImGui::IsMouseDown(ImGuiMouseButton_Right) && !io.WantCaptureMouse;
    if (!looking)
        return;
    g_yaw += md.dx * g_lookSensitivity;
    g_pitch -= md.dy * g_lookSensitivity;
    const float limit = kPi * 0.5f - 0.01f;
    g_pitch = g_pitch < -limit ? -limit : (g_pitch > limit ? limit : g_pitch);
    if (md.wheel)
        g_speed *= md.wheel > 0 ? 1.25f : 0.8f;

    float speed = g_speed * (KeyDown(VK_SHIFT) ? 4.0f : 1.0f) * (KeyDown(VK_CONTROL) ? 0.25f : 1.0f) * dt;
    float f[3];
    Forward(g_yaw, g_pitch, f);
    float r[3] = {std::cos(g_yaw), 0.0f, -std::sin(g_yaw)};
    float move[3] = {0, 0, 0};
    auto add = [&](const float* v, float s) {
        for (int i = 0; i < 3; ++i)
            move[i] += v[i] * s;
    };
    if (KeyDown('W')) add(f, 1);
    if (KeyDown('S')) add(f, -1);
    if (KeyDown('D')) add(r, 1);
    if (KeyDown('A')) add(r, -1);
    if (KeyDown('E')) move[1] += 1;
    if (KeyDown('Q')) move[1] -= 1;
    for (int i = 0; i < 3; ++i)
        g_pos[i] += move[i] * speed;
}

// Above the map (#22). A camera draws what it sees from the rooms it is in, each with its whole view, and
// from them through their portals (its visibility, 0x433170): the collision manager (renderer +0x1C) gives
// the rooms at the camera (vfunc 0x24, RenderD3D +0x27D60: (rooms, rooms' end, matrix, position, scale),
// ret 0x14: the rooms whose inside trees hold the point, else those a box of that size there meets, the
// rooms within another one taking its place), each is kept if it holds the camera's point (ZROOM vfunc
// 0x16C, 0x43DC50: (point, margin), ret 8), and a room seen through a portal is drawn within the portal only
// (0x432120). High above a map the free camera is in no room and nothing is drawn (grey). There the
// rooms its sight meets (the same question with a box as large as its far plane) are given as the ones it
// is in, those in front of it, nearest first: each is drawn with the whole view, as the room one stands in.
constexpr uintptr_t kRenderD3D_RoomsOfBox = 0x27D60;
constexpr uintptr_t kCameraRoomsCall = 0x43324B;    // (the return addresses of the camera's calls)
constexpr uintptr_t kZROOM_HoldsPoint = 0x43DC50;
constexpr uintptr_t kCameraHoldsCall = 0x4333C9;
// A camera's view takes 0x20000 bytes of its stack for its rooms, 0xF00 each: 34 (it stops at the end);
// two are left to the rooms seen through portals and its own room
constexpr int kOverviewRooms = 32;
// A room under the open sky has the map's backdrop to draw with it (room +0xB3, a geom ref: 433170 draws it
// once for the rooms that see out); the rooms inside the buildings have none. The renderer's room for the
// lit colours of what it draws (one vertex buffer of 0x20000, RenderD3D 0x10010050) holds what one sees from
// a map's open places, or from inside a building, not both at once (C1-1 from above with all its rooms:
// a vertex buffer index past the pool, 0x100090C0): the view from above takes one of the two.
constexpr size_t kRoomBackdrop = 0xB3;
bool g_overviewInside;  // the insides of the buildings (their floors, roofs left out) instead of the open map
bool g_overview;  // this frame's rooms are the ones the free camera sees

// The rooms in front of the camera, nearest first (their boxes: room +0x18 half extents, +0x24 centre, world),
// those under the open sky or those inside
int InFront(const float pos[3], void** rooms, int n)
{
    float f[3];
    Forward(g_yaw, g_pitch, f);
    std::vector<std::pair<float, void*>> seen;
    for (int k = 0; k < n; ++k) {
        auto box = reinterpret_cast<const float*>(reinterpret_cast<const unsigned char*>(rooms[k]) + 0x18);
        float d2 = 0, ahead = 0, reach = 0;
        for (int i = 0; i < 3; ++i) {
            float off = box[3 + i] - pos[i], out = std::fabs(off) - box[i];
            if (out > 0)
                d2 += out * out;
            ahead += off * f[i];
            reach += std::fabs(f[i]) * box[i];
        }
        bool open = *reinterpret_cast<const uint32_t*>(reinterpret_cast<const unsigned char*>(rooms[k]) +
                                                         kRoomBackdrop) != 0;
        // (a camera's own setup has a room of no size: EmptyGroupForBlankScreen, lit by nothing of the map's)
        if (ahead > -reach && box[0] + box[1] + box[2] > 0 && open != g_overviewInside)
            seen.push_back({d2, rooms[k]});
    }
    std::sort(seen.begin(), seen.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    int kept = std::min(int(seen.size()), kOverviewRooms);
    for (int k = 0; k < kept; ++k)
        rooms[k] = seen[k].second;
    return kept;
}

using RoomsOfBox_t = int(__fastcall*)(void* self, void* edx, void** rooms, void** end, float* mat, float* pos,
                                      float* scale);
RoomsOfBox_t oRoomsOfBox;

int __fastcall hkRoomsOfBox(void* self, void* edx, void** rooms, void** end, float* mat, float* pos, float* scale)
{
    int n = oRoomsOfBox(self, edx, rooms, end, mat, pos, scale);
    if (reinterpret_cast<uintptr_t>(_ReturnAddress()) != kCameraRoomsCall)
        return n;
    // the free camera's view (another camera's is somewhere else), in no room
    g_overview = false;
    game::ZCAMERA* cam = game::MainCamera();
    if (n || !g_enabled || !cam || !pos || std::fabs(pos[0] - g_pos[0]) > 1.0f ||
        std::fabs(pos[1] - g_pos[1]) > 1.0f || std::fabs(pos[2] - g_pos[2]) > 1.0f)
        return n;
    float sight[3] = {cam->farPlane, cam->farPlane, cam->farPlane};
    n = InFront(pos, rooms, oRoomsOfBox(self, edx, rooms, end, mat, pos, sight));
    g_overview = n > 0;
    return n;
}

using HoldsPoint_t = bool(__fastcall*)(void* room, void* edx, float* point, float margin);
HoldsPoint_t oHoldsPoint;

bool __fastcall hkHoldsPoint(void* room, void* edx, float* point, float margin)
{
    if (g_overview && reinterpret_cast<uintptr_t>(_ReturnAddress()) == kCameraHoldsCall)
        return true;
    return oHoldsPoint(room, edx, point, margin);
}

void OnRendererLoaded(HMODULE mod)
{
    Hook("RenderD3D::RoomsOfBox", reinterpret_cast<unsigned char*>(mod) + kRenderD3D_RoomsOfBox, hkRoomsOfBox,
         &oRoomsOfBox);
}

using FrameUpdate_t = void(__fastcall*)(void* self, void* edx);
FrameUpdate_t oFrameUpdate;

void __fastcall hkFrameUpdate(void* self, void* edx)
{
    oFrameUpdate(self, edx);

    LARGE_INTEGER now, freq;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);
    float dt = g_lastTick.QuadPart ? float(now.QuadPart - g_lastTick.QuadPart) / float(freq.QuadPart) : 0.0f;
    g_lastTick = now;
    dt = dt > 0.1f ? 0.1f : dt;

    // the editor closed: the game's camera again (#21: the player saw no Hitman)
    static bool wasOpen;
    bool open = overlay::IsOpen();
    if (wasOpen && !open && g_enabled)
        SetFreeCamEnabled(false);
    wasOpen = open;
    game::ZCAMERA* cam = game::MainCamera();
    if (!g_enabled || !cam) {
        TakeMouseDelta();
        return;
    }
    UpdateControls(dt);
    float m[9];
    BuildMatrix(g_yaw, g_pitch, m);
    game::SetWorldTransform(cam, m, g_pos);
}

void OnScene(const std::string&)
{
    g_enabled = false;  // the camera objects are recreated with the scene
    g_overview = false;
    SetViewDistance(1.0f);
}

} // namespace

void InstallFreeCam()
{
    Hook("ZEngineDataBase::FrameUpdate", game::ZEngineDataBase_FrameUpdate, hkFrameUpdate, &oFrameUpdate);
    Hook("ZROOM::HoldsPoint", kZROOM_HoldsPoint, hkHoldsPoint, &oHoldsPoint);
    OnModuleLoad("RenderD3D.dll", OnRendererLoaded);
    OnScenePushed(OnScene);
    overlay::AddPanel({"Camera", &g_panelVisible, DrawCameraPanel, overlay::Dock::Right});
}

bool FreeCamEnabled() { return g_enabled; }

void SetFreeCamEnabled(bool enabled)
{
    game::ZCAMERA* cam = game::MainCamera();
    if (enabled && !g_enabled && cam)
        CaptureFromCamera(cam);
    g_enabled = enabled && cam;
    SetViewDistance(g_enabled ? g_viewDistance : 1.0f);
}

bool CameraTransform(game::ZCAMERA* cam, float m[9], float pos[3])
{
    __try {
        game::GetWorldTransform(cam, m, pos);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;  // gone with a scene being loaded
    }
}

void FreeCamPosition(float out[3])
{
    if (!g_enabled) {
        float m[9], p[3];
        if (game::ZCAMERA* cam = game::MainCamera(); cam && CameraTransform(cam, m, p))
            memcpy(g_pos, p, sizeof(p));
    }
    for (int i = 0; i < 3; ++i)
        out[i] = g_pos[i];
}

void SetFreeCamPosition(const float pos[3])
{
    for (int i = 0; i < 3; ++i)
        g_pos[i] = pos[i];
}

void SetFreeCamAngles(float yawDeg, float pitchDeg)
{
    g_yaw = yawDeg * kPi / 180.0f;
    g_pitch = pitchDeg * kPi / 180.0f;
    float limit = 1.55f;
    g_pitch = g_pitch < -limit ? -limit : (g_pitch > limit ? limit : g_pitch);
}

void FlyCameraTo(const float target[3], float distance)
{
    SetFreeCamEnabled(true);
    g_pitch = -25.0f * kPi / 180.0f;
    float f[3];
    Forward(g_yaw, g_pitch, f);
    for (int i = 0; i < 3; ++i)
        g_pos[i] = target[i] - f[i] * distance;
    g_pos[1] += 60.0f;  // a person's middle rather than the feet
}

void FreeCamAngles(float& yawDeg, float& pitchDeg)
{
    yawDeg = g_yaw * 180.0f / kPi;
    pitchDeg = g_pitch * 180.0f / kPi;
}

bool* CameraPanelVisible() { return &g_panelVisible; }

void DrawCameraPanel()
{
    game::ZCAMERA* cam = game::MainCamera();
    bool enabled = g_enabled;
    ImGui::BeginDisabled(!cam);
    if (ImGui::Checkbox("Free camera", &enabled))
        SetFreeCamEnabled(enabled);
    ImGui::EndDisabled();
    if (!cam)
        ImGui::TextDisabled("no world camera yet");
    ImGui::SliderFloat("speed", &g_speed, 10.0f, 5000.0f, "%.0f cm/s", ImGuiSliderFlags_Logarithmic);
    if (ImGui::SliderFloat("view distance", &g_viewDistance, 1.0f, 12.0f, "x%.1f") && g_enabled)
        SetViewDistance(g_viewDistance);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("How far the free camera sees (the game's far plane and fog, times this).\n"
                          "Only while flying; the game itself keeps its own distance.");
    float pos[3];
    FreeCamPosition(pos);
    if (ImGui::InputFloat3("position", pos, "%.1f") && g_enabled)
        SetFreeCamPosition(pos);
    ImGui::Text("yaw %.1f  pitch %.1f", g_yaw * 180 / kPi, g_pitch * 180 / kPi);
    ImGui::Checkbox("From above: inside the buildings", &g_overviewInside);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Above the map (out of every room) the camera shows the open map with its roofs.\n"
                          "On: the floors inside the buildings instead (the game draws one or the other).");
    ImGui::TextDisabled("right mouse: look, WASD move, Q/E down/up,\nShift fast, wheel speed");
}

} // namespace h2::editor
