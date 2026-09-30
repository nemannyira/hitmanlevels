#include "viewport.h"

#include <cmath>
#include <cstring>

#include "imgui.h"
#include "ImGuizmo.h"

#include "../display.h"
#include "../fov.h"
#include "../game.h"
#include "../overlay/overlay.h"
#include "freecam.h"
#include "highlight.h"
#include "history.h"
#include "model.h"
#include "scene_panel.h"

namespace h2::editor {

namespace {

ImGuizmo::OPERATION g_op = ImGuizmo::TRANSLATE;
ImGuizmo::MODE g_mode = ImGuizmo::WORLD;
bool g_snap = false;
float g_snapMove = 25.0f;   // cm
float g_snapAngle = 15.0f;  // degrees
bool g_wasUsing;
bool g_toolbarVisible = true;
bool g_pickInvisible = false;  // also pick objects without a drawn mesh (triggers, markers...)

// engine [forward, up, right] + pos  <->  gizmo rows [X=right, Y=up, Z=forward, T]
void ToGizmo(const Transform& t, float m[16])
{
    const float* f = t.mat;
    const float* u = t.mat + 3;
    const float* r = t.mat + 6;
    float out[16] = {r[0], r[1], r[2], 0, u[0], u[1], u[2], 0, f[0], f[1], f[2], 0, t.pos[0], t.pos[1], t.pos[2], 1};
    memcpy(m, out, sizeof(out));
}

Transform FromGizmo(const float m[16])
{
    Transform t;
    for (int i = 0; i < 3; ++i) {
        t.mat[6 + i] = m[0 + i];  // right
        t.mat[3 + i] = m[4 + i];  // up
        t.mat[0 + i] = m[8 + i];  // forward
        t.pos[i] = m[12 + i];
    }
    Orthonormalize(t.mat);
    return t;
}

bool FreeCamLooking() { return ImGui::IsMouseDown(ImGuiMouseButton_Right); }

// Ray through the mouse against the objects' bounding spheres: the nearest
// entry wins, big spheres (rooms, groups) only when nothing smaller is hit.
void Pick()
{
    ImGuiIO& io = ImGui::GetIO();
    game::ZCAMERA* cam = game::MainCamera();
    if (!cam)
        return;
    float cm[9], origin[3];
    game::GetWorldTransform(cam, cm, origin);
    float tanH = std::tan(cam->fov * 0.5f) * HorizontalTanScale();
    float tanV = tanH * float(RenderHeight()) / float(RenderWidth());
    float nx = (io.MousePos.x / io.DisplaySize.x) * 2.0f - 1.0f;
    float ny = 1.0f - (io.MousePos.y / io.DisplaySize.y) * 2.0f;
    float dir[3];
    for (int i = 0; i < 3; ++i)
        dir[i] = cm[i] + cm[6 + i] * nx * tanH + cm[3 + i] * ny * tanV;
    float len = std::sqrt(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
    for (float& v : dir)
        v /= len;

    const auto& snap = SceneModel(2000);
    engine::ZGEOM* best = nullptr;
    float bestScore = 1e30f;
    for (const auto& g : snap.geoms) {
        if (g.depth == 0 || !IsLive(g.geom) || IsHidden(g.geom))
            continue;
        if (!g_pickInvisible && !WasDrawnRecently(g.node))
            continue;
        float c[3], r;
        if (!GetBounds(g.geom, c, r))
            continue;
        float oc[3] = {c[0] - origin[0], c[1] - origin[1], c[2] - origin[2]};
        float dist2 = oc[0] * oc[0] + oc[1] * oc[1] + oc[2] * oc[2];
        if (dist2 < r * r)
            continue;  // the camera is inside it (room, big group)
        float tc = oc[0] * dir[0] + oc[1] * dir[1] + oc[2] * dir[2];
        float miss2 = dist2 - tc * tc;
        if (tc < 0 || miss2 > r * r)
            continue;
        float entry = tc - std::sqrt(r * r - miss2);
        // prefer what is in front, and small objects over the groups containing them
        float score = entry + r * 2.0f;
        if (score < bestScore) {
            bestScore = score;
            best = g.geom;
        }
    }
    Select(best);
}

void Shortcuts()
{
    ImGuiIO& io = ImGui::GetIO();
    if (io.WantCaptureKeyboard || FreeCamLooking())
        return;
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Z))
        Undo();
    else if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Y))
        Redo();
    else if (ImGui::IsKeyPressed(ImGuiKey_W))
        g_op = ImGuizmo::TRANSLATE;
    else if (ImGui::IsKeyPressed(ImGuiKey_E))
        g_op = ImGuizmo::ROTATE;
    else if (ImGui::IsKeyPressed(ImGuiKey_Q))
        g_mode = g_mode == ImGuizmo::WORLD ? ImGuizmo::LOCAL : ImGuizmo::WORLD;
    else if (ImGui::IsKeyPressed(ImGuiKey_Escape))
        Select(nullptr);
    else if (engine::ZGEOM* sel = Selected(); sel && IsLive(sel)) {
        if (ImGui::IsKeyPressed(ImGuiKey_H)) {
            ObjectState before = GetState(sel), after = before;
            after.hidden = !before.hidden;
            Change(sel, before, after, after.hidden ? "hide" : "show");
        } else if (ImGui::IsKeyPressed(ImGuiKey_F)) {
            Transform t = GetTransform(sel);
            float cam[3] = {t.pos[0] - t.mat[0] * 300.0f, t.pos[1] + 150.0f, t.pos[2] - t.mat[2] * 300.0f};
            SetFreeCamEnabled(true);
            SetFreeCamPosition(cam);
        }
    }
}

void DrawToolbar()
{
    ImGui::SetNextWindowPos(ImVec2(ImGui::GetIO().DisplaySize.x * 0.5f, 34), ImGuiCond_FirstUseEver, ImVec2(0.5f, 0));
    if (!ImGui::Begin("Tools", &g_toolbarVisible, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::End();
        return;
    }
    if (ImGui::RadioButton("Move (W)", g_op == ImGuizmo::TRANSLATE)) g_op = ImGuizmo::TRANSLATE;
    ImGui::SameLine();
    if (ImGui::RadioButton("Rotate (E)", g_op == ImGuizmo::ROTATE)) g_op = ImGuizmo::ROTATE;
    ImGui::SameLine();
    if (ImGui::RadioButton("World", g_mode == ImGuizmo::WORLD)) g_mode = ImGuizmo::WORLD;
    ImGui::SameLine();
    if (ImGui::RadioButton("Local (Q)", g_mode == ImGuizmo::LOCAL)) g_mode = ImGuizmo::LOCAL;
    ImGui::SameLine();
    ImGui::Checkbox("Snap", &g_snap);
    if (g_snap) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(70);
        if (g_op == ImGuizmo::ROTATE)
            ImGui::DragFloat("##snap", &g_snapAngle, 1.0f, 1.0f, 90.0f, "%.0f deg");
        else
            ImGui::DragFloat("##snap", &g_snapMove, 1.0f, 1.0f, 1000.0f, "%.0f cm");
    }
    ImGui::SameLine();
    ImGui::Checkbox("Pick invisible", &g_pickInvisible);
    ImGui::SameLine();
    ImGui::BeginDisabled(!CanUndo());
    if (ImGui::Button("Undo"))
        Undo();
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!CanRedo());
    if (ImGui::Button("Redo"))
        Redo();
    ImGui::EndDisabled();
    ImGui::End();
}

void DrawViewport()
{
    ImGuizmo::BeginFrame();
    ImGuiIO& io = ImGui::GetIO();
    ImGuizmo::SetOrthographic(false);
    ImGuizmo::SetDrawlist(ImGui::GetBackgroundDrawList());
    ImGuizmo::SetRect(0, 0, io.DisplaySize.x, io.DisplaySize.y);
    if (g_toolbarVisible)
        DrawToolbar();
    Shortcuts();

    engine::ZGEOM* sel = Selected();
    float view[16], proj[16];
    bool haveCamera = CameraMatrices(view, proj);
    bool using_ = false;
    if (sel && IsLive(sel) && haveCamera && !FreeCamLooking()) {
        float m[16];
        ToGizmo(GetTransform(sel), m);
        float snap[3] = {g_snapMove, g_snapMove, g_snapMove};
        if (g_op == ImGuizmo::ROTATE)
            snap[0] = snap[1] = snap[2] = g_snapAngle;
        if (ImGuizmo::Manipulate(view, proj, g_op, g_mode, m, nullptr, g_snap ? snap : nullptr)) {
            if (!InContinuous())
                BeginContinuous(sel);
            SetTransform(sel, FromGizmo(m));
        }
        using_ = ImGuizmo::IsUsing();
    }
    if (g_wasUsing && !using_ && sel)
        EndContinuous(sel, g_op == ImGuizmo::ROTATE ? "rotate" : "move");
    g_wasUsing = using_;

    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !io.WantCaptureMouse && !ImGuizmo::IsOver() && !using_ &&
        haveCamera)
        Pick();
}

} // namespace

bool CameraMatrices(float view[16], float proj[16])
{
    game::ZCAMERA* cam = game::MainCamera();
    if (!cam || RenderWidth() <= 0)
        return false;
    float m[9], c[3];
    game::GetWorldTransform(cam, m, c);
    const float* f = m;
    const float* u = m + 3;
    const float* r = m + 6;
    float v[16] = {r[0], u[0], f[0], 0,
                   r[1], u[1], f[1], 0,
                   r[2], u[2], f[2], 0,
                   -(c[0] * r[0] + c[1] * r[1] + c[2] * r[2]), -(c[0] * u[0] + c[1] * u[1] + c[2] * u[2]),
                   -(c[0] * f[0] + c[1] * f[1] + c[2] * f[2]), 1};
    memcpy(view, v, sizeof(v));
    float tanH = std::tan(cam->fov * 0.5f) * HorizontalTanScale();
    float tanV = tanH * float(RenderHeight()) / float(RenderWidth());
    float zn = 5.0f, zf = 100000.0f;
    float p[16] = {1.0f / tanH, 0, 0, 0,
                   0, 1.0f / tanV, 0, 0,
                   0, 0, zf / (zf - zn), 1,
                   0, 0, -zn * zf / (zf - zn), 0};
    memcpy(proj, p, sizeof(p));
    return true;
}

void InstallViewport()
{
    static bool always = true;
    overlay::AddPanel({nullptr, &always, DrawViewport});
    overlay::AddPanel({"Tools", &g_toolbarVisible, [] {}});  // listed in the menu; drawn by the viewport
}

} // namespace h2::editor
