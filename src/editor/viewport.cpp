#include "viewport.h"

#include <cmath>
#include <cstring>
#include <string>

#include "imgui.h"
#include "ImGuizmo.h"

#include "../display.h"
#include "../fov.h"
#include "../game.h"
#include "../overlay/overlay.h"
#include "freecam.h"
#include "guides.h"
#include "highlight.h"
#include "history.h"
#include "level.h"
#include "model.h"
#include "mission.h"
#include "vehicles.h"
#include "npc_panel.h"
#include "playtest.h"
#include "placement.h"
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
std::string g_message;  // result of the last duplicate/delete, shown in the toolbar
bool g_pickInvisible = false;  // also pick objects without a drawn mesh (triggers, markers...)
bool g_npcMode = false;        // only characters (ZActor) are pickable

bool IsActor(engine::ZGEOM* g) { return engine::IsA(g, "ZActor"); }

// Pick volume of a character: a sphere around its body (the skinned mesh is
// invisible to the GPU picking and the node bounds are not reliable for it).
void ActorSphere(engine::ZGEOM* g, float c[3], float& r)
{
    Transform t = GetTransform(g);
    c[0] = t.pos[0];
    c[1] = t.pos[1] + 90.0f;
    c[2] = t.pos[2];
    r = 70.0f;
}

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

// Visual aids while moving an object (after Blender's transform overlays): a
// grid on the level the object stood on when the drag began, its box and its
// footprint on that level with drop lines, and the line of the axis it moves
// along. A faint grid only while the mouse is over the gizmo.
Transform g_dragStart;
float g_dragFloor;  // height of the box bottom when the drag began
bool g_dragging;

float BoxFloor(const Box& b)
{
    float y = b.c[0][1];
    for (auto& c : b.c)
        y = std::fmin(y, c[1]);
    return y;
}

void DrawMoveGuide(engine::ZGEOM* sel, bool using_, bool over)
{
    Transform t = GetTransform(sel);
    Box box;
    if (!ObjectBox(sel, t, box))
        return;
    if (using_ && !g_dragging) {
        g_dragging = true;
        g_dragStart = t;
        g_dragFloor = BoxFloor(box);
    }
    if (!using_)
        g_dragging = false;
    if (!using_ && !over)
        return;
    float floor = g_dragging ? g_dragFloor : BoxFloor(box);
    const engine::ZBaseGeom* n = sel->node;
    float size = 2.0f * std::fmax(n->boundHalf[0], std::fmax(n->boundHalf[1], n->boundHalf[2]));
    if (!(size > 1.0f && size < 1e5f))
        size = 2.0f * n->boundRadius;
    float step = g_snap ? g_snapMove : GridStep(size);
    float radius = std::fmin(std::fmax(size * 2.5f, step * 6.0f), step * 50.0f);
    float center[3] = {0, floor, 0};
    for (auto& c : box.c)
        center[0] += c[0] / 8.0f, center[2] += c[2] / 8.0f;
    static const float up[3] = {0, 1, 0}, x[3] = {1, 0, 0}, z[3] = {0, 0, 1};
    ImU32 grid = using_ ? IM_COL32(255, 200, 40, 150) : IM_COL32(255, 200, 40, 70);
    DrawGrid(center, x, z, step, radius, grid, using_);
    if (!using_)
        return;
    DrawFootprint(box, center, up, IM_COL32(255, 200, 40, 40), IM_COL32(255, 200, 40, 230));
    DrawBox(box, IM_COL32(255, 200, 40, 200), 1.5f);
    if (g_op == ImGuizmo::TRANSLATE) {
        // the axis the object moves along (the dominant one), through where it started
        float d[3] = {t.pos[0] - g_dragStart.pos[0], t.pos[1] - g_dragStart.pos[1], t.pos[2] - g_dragStart.pos[2]};
        float len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        int k = std::fabs(d[0]) >= std::fabs(d[1]) && std::fabs(d[0]) >= std::fabs(d[2]) ? 0
                : std::fabs(d[1]) >= std::fabs(d[2])                                      ? 1
                                                                                         : 2;
        if (len > 0.5f && std::fabs(d[k]) > 0.97f * len) {
            float a[3], b[3];
            for (int i = 0; i < 3; ++i) {
                a[i] = g_dragStart.pos[i] - (i == k ? 100000.0f : 0.0f);
                b[i] = g_dragStart.pos[i] + (i == k ? 100000.0f : 0.0f);
            }
            DrawLine3D(a, b, k == 0 ? kAxisX : (k == 1 ? kAxisY : kAxisZ), 1.5f);
        }
        ImGuiIO& io = ImGui::GetIO();
        char text[96];
        snprintf(text, sizeof(text), "dx %.0f  dy %.0f  dz %.0f cm  (%.0f)", d[0], d[1], d[2], len);
        ImGui::GetForegroundDrawList()->AddText(ImVec2(io.MousePos.x + 18, io.MousePos.y + 14),
                                                IM_COL32(255, 220, 120, 255), text);
    }
}

// Ray through the mouse against the objects' bounding spheres: the nearest
// entry wins, big spheres (rooms, groups) only when nothing smaller is hit.
float g_clickX, g_clickY;

engine::ZGEOM* SphereHit(float mx, float my, bool characters)
{
    ImGuiIO& io = ImGui::GetIO();
    game::ZCAMERA* cam = game::MainCamera();
    if (!cam)
        return nullptr;
    float cm[9], origin[3];
    game::GetWorldTransform(cam, cm, origin);
    float tanH = CameraTanHalfFov(cam);
    float tanV = tanH * float(RenderHeight()) / float(RenderWidth());
    float nx = (mx / io.DisplaySize.x) * 2.0f - 1.0f;
    float ny = 1.0f - (my / io.DisplaySize.y) * 2.0f;
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
        float c[3], r;
        if (characters) {
            if (!IsActor(g.geom))
                continue;
            ActorSphere(g.geom, c, r);
        } else {
            if (IsActor(g.geom))
                continue;
            if (!g_pickInvisible && !WasDrawnRecently(g.node))
                continue;
            if (!GetBounds(g.geom, c, r))
                continue;
        }
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
    return best;
}

void PickSphere(float mx, float my) { Select(SphereHit(mx, my, g_npcMode)); }

void DrawActorBox(engine::ZGEOM* g)
{
    Transform t = GetTransform(g);
    const float* f = t.mat;
    const float* r = t.mat + 6;
    const float hw = 35.0f, h = 185.0f;
    float corners[8][3];
    for (int i = 0; i < 8; ++i) {
        float sx = (i & 1) ? hw : -hw, sz = (i & 2) ? hw : -hw, y = (i & 4) ? h : 0.0f;
        for (int k = 0; k < 3; ++k)
            corners[i][k] = t.pos[k] + r[k] * sx + f[k] * sz + (k == 1 ? y : 0.0f);
    }
    ImVec2 s[8];
    for (int i = 0; i < 8; ++i)
        if (!WorldToScreen(corners[i], s[i].x, s[i].y))
            return;
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    const int edges[12][2] = {{0, 1}, {1, 3}, {3, 2}, {2, 0}, {4, 5}, {5, 7}, {7, 6}, {6, 4}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
    for (auto& e : edges)
        dl->AddLine(s[e[0]], s[e[1]], IM_COL32(80, 220, 255, 255), 2.0f);
}

// Exact pick of what is drawn under the cursor; bounding spheres as fallback
// (e.g. skinned characters drawn through vertex shaders).
void Pick()
{
    ImGuiIO& io = ImGui::GetIO();
    g_clickX = io.MousePos.x;
    g_clickY = io.MousePos.y;
    if (engine::ZGEOM* point = RoutePointAt(g_clickX, g_clickY)) {  // a marker of a shown route
        Select(point);
        return;
    }
    if (g_npcMode) {
        PickSphere(g_clickX, g_clickY);
        return;
    }
    RequestPick(g_clickX, g_clickY, [](engine::ZGEOM* hit, const float*, const float*) {
        if (hit)
            Select(hit);
        else
            PickSphere(g_clickX, g_clickY);
    });
}

void Shortcuts()
{
    ImGuiIO& io = ImGui::GetIO();
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S, false))  // also while typing
        SaveShortcut();
    if (io.WantCaptureKeyboard || FreeCamLooking() || Placing() || NpcPlacing() || MissionPicking() || VehiclePicking())
        return;  // these have their own keys
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
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_D)) {
            DuplicateSelection();
        } else if (ImGui::IsKeyPressed(ImGuiKey_Delete)) {
            DeleteSelection();
        } else if (ImGui::IsKeyPressed(ImGuiKey_G)) {
            BeginGrab(sel);
        } else if (ImGui::IsKeyPressed(ImGuiKey_H)) {
            ObjectState before = GetState(sel), after = before;
            after.hidden = !before.hidden;
            Change(sel, before, after, after.hidden ? "hide" : "show");
        } else if (ImGui::IsKeyPressed(ImGuiKey_F)) {
            FlyCameraTo(GetTransform(sel).pos);
        }
    }
}

void DrawToolbar()
{
    if (ImGui::RadioButton("Objects", !g_npcMode)) g_npcMode = false;
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("A click in the world selects an object (a model, an item, a point)");
    ImGui::SameLine();
    if (ImGui::RadioButton("NPCs", g_npcMode)) g_npcMode = true;
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("A click in the world selects a character (also through walls and models)");
    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();
    if (ImGui::RadioButton("Move (W)", g_op == ImGuizmo::TRANSLATE)) g_op = ImGuizmo::TRANSLATE;
    ImGui::SameLine();
    if (ImGui::RadioButton("Rotate (E)", g_op == ImGuizmo::ROTATE)) g_op = ImGuizmo::ROTATE;
    ImGui::SameLine();
    if (ImGui::RadioButton("World", g_mode == ImGuizmo::WORLD)) g_mode = ImGuizmo::WORLD;
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("The gizmo's arrows follow the map's axes (Q switches)");
    ImGui::SameLine();
    if (ImGui::RadioButton("Local (Q)", g_mode == ImGuizmo::LOCAL)) g_mode = ImGuizmo::LOCAL;
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("The gizmo's arrows follow the object's own axes (Q switches)");
    ImGui::SameLine();
    {
        engine::ZGEOM* sel = Selected();
        ImGui::BeginDisabled(!sel || !IsLive(sel) || Placing());
        if (ImGui::Button("Grab (G)"))
            BeginGrab(sel);
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("Move the selected object onto the surface under the mouse; click drops it, Esc puts it back");
    }
    ImGui::SameLine();
    ImGui::Checkbox("Snap", &g_snap);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Moving and turning with the gizmo goes in steps (set beside it)");
    if (g_snap) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 4.5f);
        if (g_op == ImGuizmo::ROTATE)
            ImGui::DragFloat("##snap", &g_snapAngle, 1.0f, 1.0f, 90.0f, "%.0f deg");
        else
            ImGui::DragFloat("##snap", &g_snapMove, 1.0f, 1.0f, 1000.0f, "%.0f cm");
    }
    ImGui::SameLine();
    ImGui::Checkbox("Pick invisible", &g_pickInvisible);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("A click can select what the game does not draw too: points, hidden objects,\n"
                          "collision boxes (the Scene tab lists everything)");
    ImGui::SameLine();
    ImGui::BeginDisabled(!CanUndo());
    if (ImGui::Button("Undo"))
        Undo();
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip(CanUndo() ? "Undo %s (Ctrl+Z)" : "Nothing to undo", UndoLabel().c_str());
    ImGui::SameLine();
    ImGui::BeginDisabled(!CanRedo());
    if (ImGui::Button("Redo"))
        Redo();
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip(CanRedo() ? "Redo %s (Ctrl+Y)" : "Nothing to redo", RedoLabel().c_str());
    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();
    DrawSaveControl();
    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();
    DrawPlayTestControl();
    if (!g_message.empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled("  %s", g_message.c_str());
    }
}

void EditMenu()
{
    engine::ZGEOM* sel = Selected();
    bool have = sel && IsLive(sel);
    if (ImGui::MenuItem(("Undo " + UndoLabel()).c_str(), "Ctrl+Z", false, CanUndo()))
        Undo();
    if (ImGui::MenuItem(("Redo " + RedoLabel()).c_str(), "Ctrl+Y", false, CanRedo()))
        Redo();
    ImGui::Separator();
    if (ImGui::MenuItem("Duplicate", "Ctrl+D", false, have))
        DuplicateSelection();
    if (ImGui::MenuItem("Grab onto a surface", "G", false, have && !Placing()))
        BeginGrab(sel);
    if (ImGui::MenuItem("Delete", "Del", false, have && FindSpawned(sel)))
        DeleteSelection();
    if (ImGui::MenuItem(have && IsHidden(sel) ? "Show" : "Hide", "H", false, have)) {
        ObjectState before = GetState(sel), after = before;
        after.hidden = !before.hidden;
        Change(sel, before, after, after.hidden ? "hide" : "show");
    }
    if (ImGui::MenuItem("Deselect", "Esc", false, have))
        Select(nullptr);
}

void DrawViewport()
{
    ImGuizmo::BeginFrame();
    ImGuiIO& io = ImGui::GetIO();
    ImGuizmo::SetOrthographic(false);
    ImGuizmo::SetDrawlist(ImGui::GetBackgroundDrawList());
    ImGuizmo::SetRect(0, 0, io.DisplaySize.x, io.DisplaySize.y);
    Shortcuts();
    if (Placing()) {
        UpdatePlacement();
        return;
    }
    if (NpcPlacing()) {
        UpdateNpcPlacement();
        return;
    }
    if (MissionPicking()) {
        UpdateMissionPick();
        return;
    }
    if (VehiclePicking()) {
        UpdateVehiclePick();
        return;
    }

    engine::ZGEOM* sel = Selected();
    float view[16], proj[16];
    bool haveCamera = CameraMatrices(view, proj);
    bool using_ = false;
    bool actor = sel && IsLive(sel) && IsActor(sel);
    if (sel && IsLive(sel) && haveCamera && !FreeCamLooking()) {  // characters: teleported (SetTransform)
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
    if (sel && IsLive(sel) && haveCamera && !actor)
        DrawMoveGuide(sel, using_, !using_ && ImGuizmo::IsOver());
    if (g_wasUsing && !using_ && sel)
        EndContinuous(sel, g_op == ImGuizmo::ROTATE ? "rotate" : "move");
    g_wasUsing = using_;

    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !io.WantCaptureMouse && !ImGuizmo::IsOver() && !using_ &&
        haveCamera)
        Pick();

    // characters are skinned (no wireframe highlight): draw a box around them
    if (sel && IsLive(sel) && IsActor(sel))
        DrawActorBox(sel);
}

} // namespace

engine::ZGEOM* CharacterAt(float x, float y) { return SphereHit(x, y, true); }

float SnapMoveStep() { return g_snap ? g_snapMove : 0.0f; }

void DuplicateSelection()
{
    engine::ZGEOM* sel = Selected();
    std::string err;
    if (engine::ZGEOM* g = DuplicateObject(sel, &err)) {
        Select(g);
        const SpawnedObject* s = FindSpawned(g);
        g_message = "created " + (s ? s->id : std::string("object")) + " (Ctrl+Z undoes)";
    } else {
        g_message = "duplicate failed: " + err;
    }
}

void DeleteSelection()
{
    engine::ZGEOM* sel = Selected();
    if (!sel || !IsLive(sel))
        return;
    if (!FindSpawned(sel)) {
        g_message = "only objects created by the level can be deleted (H hides scene objects)";
        return;
    }
    Select(nullptr);
    g_message = DeleteObject(sel) ? "deleted (Ctrl+Z undoes)" : "delete failed";
}

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
    float tanH = CameraTanHalfFov(cam);
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
    overlay::AddPanel({"Toolbar", &g_toolbarVisible, DrawToolbar, overlay::Dock::Toolbar});
    overlay::AddMenu("Edit", EditMenu);
}

} // namespace h2::editor
