#include "freecam.h"

#include <windows.h>
#include <cmath>

#include "imgui.h"

#include "../engine/geoms.h"
#include "../engine/messages.h"
#include "../fov.h"
#include "../game.h"
#include "../hook.h"
#include "../input/dinput.h"
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
    SetViewDistance(1.0f);
}

} // namespace

void InstallFreeCam()
{
    Hook("ZEngineDataBase::FrameUpdate", game::ZEngineDataBase_FrameUpdate, hkFrameUpdate, &oFrameUpdate);
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
    ImGui::TextDisabled("right mouse: look, WASD move, Q/E down/up,\nShift fast, wheel speed");
}

} // namespace h2::editor
