#include "placement.h"

#include <cmath>
#include <cstring>

#include "imgui.h"

#include "../display.h"
#include "../fov.h"
#include "../game.h"
#include "../log.h"
#include "../scene.h"
#include "highlight.h"
#include "level.h"
#include "model.h"
#include "scene_panel.h"
#include "viewport.h"

namespace h2::editor {

namespace {

constexpr float kRad = 3.14159265f / 180.0f;
constexpr float kRotateStep = 15.0f;  // degrees per wheel notch

std::string g_sourcePath;
std::string g_label;
engine::ZGEOM* g_preview;
engine::ZGEOM* g_previewRoom;  // room the preview was created in
engine::ZGEOM* g_hitRoom;      // room of the surface under the mouse
Transform g_base;      // source orientation (as authored)
float g_yaw;           // extra turn around the world up axis, degrees
float g_point[3];      // where the preview stands
float g_distance = 800.0f;  // along the mouse ray when nothing is under it
bool g_pickPending;
int g_pickFrame;           // ImGui frame of the pending pick request
bool g_haveHit;
float g_mouseX, g_mouseY;  // mouse position of the pending pick

// Ray through a screen position (origin = camera).
bool ScreenRay(float sx, float sy, float origin[3], float dir[3])
{
    game::ZCAMERA* cam = game::MainCamera();
    if (!cam || RenderWidth() <= 0)
        return false;
    float cm[9];
    game::GetWorldTransform(cam, cm, origin);
    float tanH = std::tan(cam->fov * 0.5f) * HorizontalTanScale();
    float tanV = tanH * float(RenderHeight()) / float(RenderWidth());
    float nx = (sx / float(RenderWidth())) * 2.0f - 1.0f;
    float ny = 1.0f - (sy / float(RenderHeight())) * 2.0f;
    for (int i = 0; i < 3; ++i)
        dir[i] = cm[i] + cm[6 + i] * nx * tanH + cm[3 + i] * ny * tanV;
    float len = std::sqrt(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
    for (int i = 0; i < 3; ++i)
        dir[i] /= len;
    return true;
}

void PointOnRay(float sx, float sy)
{
    float o[3], d[3];
    if (!ScreenRay(sx, sy, o, d))
        return;
    for (int i = 0; i < 3; ++i)
        g_point[i] = o[i] + d[i] * g_distance;
}

void OnPick(engine::ZGEOM* hit, const float* point)
{
    g_pickPending = false;
    if (!g_preview)
        return;
    if (hit)
        if (engine::ZGEOM* room = RoomOf(hit))
            g_hitRoom = room;
    if (point) {
        memcpy(g_point, point, sizeof(g_point));
        g_haveHit = true;
        float o[3], d[3];
        if (ScreenRay(g_mouseX, g_mouseY, o, d)) {
            float dx = point[0] - o[0], dy = point[1] - o[1], dz = point[2] - o[2];
            g_distance = std::sqrt(dx * dx + dy * dy + dz * dz);
        }
    } else {
        g_haveHit = false;
        PointOnRay(g_mouseX, g_mouseY);
    }
}

Transform CurrentTransform()
{
    Transform t = g_base;
    float c = std::cos(g_yaw * kRad), s = std::sin(g_yaw * kRad);
    for (int row = 0; row < 3; ++row) {
        float* v = t.mat + row * 3;
        float x = v[0], z = v[2];
        v[0] = x * c + z * s;
        v[2] = -x * s + z * c;
    }
    memcpy(t.pos, g_point, sizeof(t.pos));
    return t;
}

void DestroyPreview()
{
    SetPickIgnore(nullptr);
    if (g_preview && IsSpawned(g_preview))
        DestroySpawned(g_preview);
    g_preview = nullptr;
}

void Commit(bool keepPlacing)
{
    engine::ZGEOM* source = Resolve(g_sourcePath);
    Transform t = CurrentTransform();
    if (!keepPlacing)
        DestroyPreview();
    std::string err;
    engine::ZGEOM* placed = source ? DuplicateObject(source, &err, &t, g_hitRoom) : nullptr;
    if (!placed)
        Log("place %s failed: %s", g_label.c_str(), source ? err.c_str() : "source object not found");
    if (!keepPlacing) {
        g_sourcePath.clear();
        if (placed)
            Select(placed);
    }
}

void DrawHint()
{
    ImGuiIO& io = ImGui::GetIO();
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    char text[256];
    snprintf(text, sizeof(text), "%s\nclick: place   Shift+click: place more\nwheel: turn (%.0f deg)   Esc: cancel%s",
             g_label.c_str(), g_yaw, g_haveHit ? "" : "\n(no surface under the mouse)");
    ImVec2 p(io.MousePos.x + 18, io.MousePos.y + 18);
    ImVec2 size = ImGui::CalcTextSize(text);
    dl->AddRectFilled(ImVec2(p.x - 6, p.y - 4), ImVec2(p.x + size.x + 6, p.y + size.y + 4), IM_COL32(0, 0, 0, 170), 4);
    dl->AddText(p, IM_COL32(255, 220, 120, 255), text);
}

} // namespace

void BeginPlacement(const std::string& sourcePath, const std::string& label)
{
    CancelPlacement();
    engine::ZGEOM* source = Resolve(sourcePath);
    if (!source)
        return;
    std::string err;
    g_preview = SpawnClone(source, "EditorPreview", &err);
    if (!g_preview) {
        Log("placement: cannot create the preview: %s", err.c_str());
        return;
    }
    SetPickIgnore(g_preview);
    g_previewRoom = RoomOf(g_preview);
    g_hitRoom = nullptr;
    g_sourcePath = sourcePath;
    g_label = label;
    g_base = GetTransform(source);
    g_yaw = 0;
    g_haveHit = false;
    g_pickPending = false;
    ImGuiIO& io = ImGui::GetIO();
    PointOnRay(io.MousePos.x, io.MousePos.y);
    SetTransform(g_preview, CurrentTransform());
    Select(nullptr);
}

void CancelPlacement()
{
    DestroyPreview();
    g_sourcePath.clear();
}

bool Placing() { return !g_sourcePath.empty(); }
std::string PlacementSource() { return g_sourcePath; }

void UpdatePlacement()
{
    if (!Placing())
        return;
    if (!g_preview || !IsLive(g_preview)) {
        CancelPlacement();
        return;
    }
    ImGuiIO& io = ImGui::GetIO();
    if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        CancelPlacement();
        return;
    }
    bool looking = ImGui::IsMouseDown(ImGuiMouseButton_Right);
    bool overWorld = !io.WantCaptureMouse && !looking;
    if (overWorld) {
        if (g_pickPending && ImGui::GetFrameCount() - g_pickFrame > 10)
            g_pickPending = false;  // the request was taken over (or the frame never rendered)
        if (!g_pickPending) {
            g_pickPending = true;
            g_pickFrame = ImGui::GetFrameCount();
            g_mouseX = io.MousePos.x;
            g_mouseY = io.MousePos.y;
            RequestPick(g_mouseX, g_mouseY, OnPick);
        }
        if (io.MouseWheel != 0.0f)
            g_yaw = std::fmod(g_yaw + (io.MouseWheel > 0 ? kRotateStep : -kRotateStep) + 360.0f, 360.0f);
        DrawHint();
    }
    if (g_hitRoom && g_hitRoom != g_previewRoom && IsLive(g_hitRoom)) {
        engine::ZGEOM* source = Resolve(g_sourcePath);
        std::string err;
        engine::ZGEOM* moved = source ? SpawnClone(source, "EditorPreview", &err, g_hitRoom) : nullptr;
        if (moved) {
            DestroyPreview();
            g_preview = moved;
            g_previewRoom = g_hitRoom;
            SetPickIgnore(g_preview);
        } else {
            g_previewRoom = g_hitRoom;  // keep the old one, do not retry every frame
        }
    }
    Transform t = CurrentTransform();
    Transform cur = GetTransform(g_preview);
    if (memcmp(&t, &cur, sizeof(t)) != 0)
        SetTransform(g_preview, t);
    if (overWorld && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
        Commit(io.KeyShift);
}

void InstallPlacement()
{
    OnScenePushed([](const std::string&) {
        g_preview = nullptr;  // destroyed with the scene objects of the editor
        g_previewRoom = g_hitRoom = nullptr;
        SetPickIgnore(nullptr);
        g_sourcePath.clear();
    });
}

} // namespace h2::editor
