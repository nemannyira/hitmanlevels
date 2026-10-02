#include "placement.h"

#include <cmath>
#include <cstring>
#include <vector>

#include "imgui.h"

#include "../display.h"
#include "../fov.h"
#include "../game.h"
#include "../log.h"
#include "../scene.h"
#include "guides.h"
#include "highlight.h"
#include "history.h"
#include "level.h"
#include "model.h"
#include "scene_panel.h"
#include "viewport.h"

namespace h2::editor {

namespace {

constexpr float kRad = 3.14159265f / 180.0f;
constexpr float kRotateStep = 15.0f;  // degrees per wheel notch
constexpr float kLiftStep = 5.0f;     // cm per wheel notch (Ctrl: the grid step)

// placing a model
std::string g_sourcePath;
std::string g_templatePath;  // placing a working item made from this template
std::string g_label;
PartClones g_preview;          // the model's parts; Main() is the transform being placed
engine::ZGEOM* g_previewRoom;  // room the preview was created in
engine::ZGEOM* g_hitRoom;      // room of the surface under the mouse

// grabbing an existing object
engine::ZGEOM* g_grab;
ObjectState g_grabBefore;
std::vector<engine::ZGEOM*> g_grabParts;  // its visible parts (for the box)
std::vector<Transform> g_grabOffsets;     // ... in its frame

Transform g_base;      // orientation at the start (as authored / as it was)
float g_yaw;           // extra turn around the world up axis, degrees
float g_lift;          // distance from the surface along its normal, cm
float g_point[3];      // the surface point under the mouse
float g_normal[3] = {0, 1, 0};  // surface normal there
bool g_haveNormal;
bool g_alignToSurface;  // N: the model's up follows the surface normal
float g_distance = 800.0f;  // along the mouse ray when nothing is under it
bool g_pickPending;
int g_pickFrame;           // ImGui frame of the pending pick request
bool g_haveHit;
float g_mouseX, g_mouseY;  // mouse position of the pending pick
std::string g_refused;     // why the last click did nothing
double g_refusedTime;

// the object being moved: the preview's main part or the grabbed object
engine::ZGEOM* Moving() { return g_grab ? g_grab : g_preview.Main(); }
const std::vector<engine::ZGEOM*>& ShapeParts() { return g_grab ? g_grabParts : g_preview.parts; }
const std::vector<Transform>& ShapeOffsets() { return g_grab ? g_grabOffsets : g_preview.offsets; }

bool GrabbingActor() { return g_grab && engine::IsA(g_grab, "ZActor"); }

bool ShapeBox(const Transform& t, Box& box)
{
    if (GrabbingActor()) {
        ActorBox(t, box);
        return true;
    }
    return PartsBox(ShapeParts(), ShapeOffsets(), t, box);
}

// Ray through a screen position (origin = camera).
bool ScreenRay(float sx, float sy, float origin[3], float dir[3])
{
    game::ZCAMERA* cam = game::MainCamera();
    if (!cam || RenderWidth() <= 0)
        return false;
    float cm[9];
    game::GetWorldTransform(cam, cm, origin);
    float tanH = CameraTanHalfFov(cam);
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

void OnPick(engine::ZGEOM* hit, const float* point, const float* normal)
{
    g_pickPending = false;
    if (!Moving())
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
        // keep the previous normal while the new one is within ~3 degrees (no jitter)
        bool same = g_haveNormal && normal &&
                    normal[0] * g_normal[0] + normal[1] * g_normal[1] + normal[2] * g_normal[2] > 0.9986f;
        if (normal && !same)
            memcpy(g_normal, normal, sizeof(g_normal));
        g_haveNormal = normal != nullptr;
    } else {
        g_haveHit = false;
        g_haveNormal = false;
        if (!g_grab)
            PointOnRay(g_mouseX, g_mouseY);
    }
}

const float* SurfaceNormal()
{
    static const float up[3] = {0, 1, 0};
    return g_haveHit && g_haveNormal ? g_normal : up;
}

float ShapeSize()
{
    engine::ZGEOM* m = Moving();
    if (!m)
        return 50.0f;
    Box box;
    if (!ShapeBox(GetTransform(m), box))
        return 50.0f;
    float s = 0;
    for (int corner : {1, 2, 4}) {
        float dx = box.c[corner][0] - box.c[0][0], dy = box.c[corner][1] - box.c[0][1],
              dz = box.c[corner][2] - box.c[0][2];
        s = std::fmax(s, std::sqrt(dx * dx + dy * dy + dz * dz));
    }
    return s > 1.0f && s < 1e5f ? s : 50.0f;
}

float GridStepFor(float size)
{
    float snap = SnapMoveStep();
    return snap > 0 ? snap : GridStep(size);
}

// The surface point, on the grid with Ctrl held (in the plane of the surface).
void TargetPoint(float out[3])
{
    memcpy(out, g_point, sizeof(g_point));
    if (!ImGui::GetIO().KeyCtrl || !g_haveHit)
        return;
    float u[3], v[3];
    PlaneAxes(SurfaceNormal(), u, v);
    float step = GridStepFor(ShapeSize());
    float du = out[0] * u[0] + out[1] * u[1] + out[2] * u[2];
    float dv = out[0] * v[0] + out[1] * v[1] + out[2] * v[2];
    float su = std::round(du / step) * step - du, sv = std::round(dv / step) * step - dv;
    for (int k = 0; k < 3; ++k)
        out[k] += u[k] * su + v[k] * sv;
}

// Like Blender's surface placement: the model turned by the wheel (and with
// N, stood up along the surface normal), its box face against the surface
// resting on the point under the mouse, lifted along the normal by Alt+wheel.
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
    const float* n = SurfaceNormal();
    if (g_alignToSurface && g_haveNormal && g_haveHit && !GrabbingActor()) {  // characters stand upright
        // rotate world up onto the normal (Rodrigues), applied to every axis
        float axis[3] = {n[2], 0.0f, -n[0]};  // up x n
        float sinA = std::sqrt(axis[0] * axis[0] + axis[2] * axis[2]), cosA = n[1];
        if (sinA > 1e-4f) {
            for (float& a : axis)
                a /= sinA;
            for (int row = 0; row < 3; ++row) {
                float* v = t.mat + row * 3;
                float kv[3] = {axis[1] * v[2] - axis[2] * v[1], axis[2] * v[0] - axis[0] * v[2],
                               axis[0] * v[1] - axis[1] * v[0]};
                float kd = axis[0] * v[0] + axis[1] * v[1] + axis[2] * v[2];
                for (int i = 0; i < 3; ++i)
                    v[i] = v[i] * cosA + kv[i] * sinA + axis[i] * kd * (1.0f - cosA);
            }
        }
    }
    t.pos[0] = t.pos[1] = t.pos[2] = 0;
    Box box;
    float face[3] = {0, 0, 0};
    if (ShapeBox(t, box))
        BoxBottom(box, n, face);
    float target[3];
    TargetPoint(target);
    for (int i = 0; i < 3; ++i)
        t.pos[i] = target[i] + n[i] * g_lift - face[i];
    return t;
}

void DestroyPreview()
{
    SetPickIgnore(nullptr);
    SetHighlightExtra(nullptr, 0);
    g_preview.Destroy();
}

// The objects the model is copied from: the visible parts of an item
// template (largest first), or the scene object using the model.
std::vector<engine::ZGEOM*> Sources()
{
    if (!g_templatePath.empty())
        if (engine::ZGEOM* tmpl = Resolve(g_templatePath)) {
            std::vector<engine::ZGEOM*> parts = VisibleParts(tmpl);
            if (!parts.empty())
                return parts;
        }
    engine::ZGEOM* source = Resolve(g_sourcePath);
    return source ? std::vector<engine::ZGEOM*>{source} : std::vector<engine::ZGEOM*>{};
}

void Refuse(const char* why)
{
    g_refused = why;
    g_refusedTime = ImGui::GetTime();
}

void Commit(bool keepPlacing)
{
    if (!g_haveHit) {
        Refuse("no surface under the mouse: point at a floor, wall or object");
        return;
    }
    std::vector<engine::ZGEOM*> sources = Sources();
    engine::ZGEOM* source = sources.empty() ? nullptr : sources[0];
    Transform t = CurrentTransform();
    if (!keepPlacing)
        DestroyPreview();
    std::string err;
    engine::ZGEOM* placed = nullptr;
    if (!g_templatePath.empty()) {
        // the item's own transform: the model sits in it as in the template
        engine::ZGEOM* tmpl = Resolve(g_templatePath);
        if (tmpl && source) {
            Transform modelInTemplate = Relative(GetTransform(tmpl), GetTransform(source));
            placed = PlaceItem(tmpl, Compose(t, Inverse(modelInTemplate)), &err);
        } else {
            err = "item template not found";
        }
    } else if (source) {
        placed = DuplicateObject(source, &err, &t, g_hitRoom);
    }
    if (!placed)
        Log("place %s failed: %s", g_label.c_str(), source ? err.c_str() : "source object not found");
    if (!keepPlacing) {
        g_sourcePath.clear();
        g_templatePath.clear();
        if (placed)
            Select(placed);
    }
}

void EndGrab(bool keep)
{
    engine::ZGEOM* g = g_grab;
    g_grab = nullptr;
    g_grabParts.clear();
    g_grabOffsets.clear();
    SetPickIgnore(nullptr);
    if (!g || !IsLive(g))
        return;
    if (keep) {
        ObjectState after = GetState(g);
        SetState(g, g_grabBefore);  // Change() applies and records it
        Change(g, g_grabBefore, after, "grab");
    } else {
        SetState(g, g_grabBefore);
    }
}

void CommitGrab()
{
    if (!g_haveHit) {
        Refuse("no surface under the mouse: point at a floor, wall or object");
        return;
    }
    EndGrab(true);
}

// Grid in the plane of the surface, the model's box and its footprint on the
// surface, the snap target; red when nothing is under the mouse.
void DrawGuide(const Transform& t)
{
    if (!Moving())
        return;
    bool hit = g_haveHit;
    ImU32 main = hit ? IM_COL32(64, 224, 255, 255) : IM_COL32(255, 80, 70, 255);
    const float* n = SurfaceNormal();
    float u[3], v[3];
    PlaneAxes(n, u, v);
    float size = ShapeSize();
    float step = GridStepFor(size);
    float radius = std::fmax(size * 2.0f, step * 6.0f);
    radius = std::fmin(radius, step * 50.0f);
    float target[3];
    TargetPoint(target);
    bool floor = std::fabs(n[1]) > 0.7f;
    if (hit)
        DrawGrid(target, u, v, step, radius, (main & ~IM_COL32_A_MASK) | IM_COL32(0, 0, 0, 150), floor);
    Box box;
    if (ShapeBox(t, box)) {
        if (hit)
            DrawFootprint(box, target, n, (main & ~IM_COL32_A_MASK) | IM_COL32(0, 0, 0, 45), main);
        DrawBox(box, (main & ~IM_COL32_A_MASK) | IM_COL32(0, 0, 0, 200), 1.5f);
    }
    if (!hit)
        return;
    float tip[3];
    for (int k = 0; k < 3; ++k)
        tip[k] = target[k] + n[k] * std::fmax(g_lift, std::fmin(size * 0.5f, step * 2.0f));
    DrawLine3D(target, tip, main, 2.0f);  // surface normal (up to the lifted model)
    DrawSnapTarget(target, IM_COL32(255, 255, 255, 255));
}

void DrawHint()
{
    ImGuiIO& io = ImGui::GetIO();
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    char text[512];
    char lift[32] = "on the surface";
    if (g_lift > 0)
        snprintf(lift, sizeof(lift), "%.0f cm", g_lift);
    snprintf(text, sizeof(text),
             "%s\n%s   Esc: cancel\n"
             "wheel: turn (%.0f deg)   Alt+wheel: lift (%s)   Ctrl: snap to grid   N: align to surface (%s)",
             g_grab ? ("moving " + PathOf(g_grab)).c_str() : g_label.c_str(),
             g_grab ? "click: drop it here" : "click: place   Shift+click: place more", g_yaw, lift,
             g_alignToSurface ? "on" : "off");
    bool refused = !g_refused.empty() && ImGui::GetTime() - g_refusedTime < 2.0;
    const char* warning = refused ? g_refused.c_str() : !g_haveHit ? "no surface under the mouse" : nullptr;
    ImVec2 p(io.MousePos.x + 18, io.MousePos.y + 18);
    ImVec2 size = ImGui::CalcTextSize(text);
    ImVec2 warnSize = warning ? ImGui::CalcTextSize(warning) : ImVec2(0, 0);
    float w = std::fmax(size.x, warnSize.x);
    float h = size.y + (warning ? warnSize.y + 2 : 0);
    dl->AddRectFilled(ImVec2(p.x - 6, p.y - 4), ImVec2(p.x + w + 6, p.y + h + 4), IM_COL32(0, 0, 0, 170), 4);
    dl->AddText(p, IM_COL32(255, 220, 120, 255), text);
    if (warning)
        dl->AddText(ImVec2(p.x, p.y + size.y + 2), IM_COL32(255, 90, 80, 255), warning);
}

// The object and everything under it (the pick looks through them).
std::vector<engine::ZGEOM*> Subtree(engine::ZGEOM* root)
{
    std::vector<engine::ZGEOM*> out;
    int start = SceneModel().Find(root);
    if (start < 0)
        start = SceneModel(0).Find(root);
    const engine::SceneSnapshot& snap = SceneModel();
    if (start < 0)
        return {root};
    std::vector<int> queue{start};
    for (size_t q = 0; q < queue.size() && q < 4096; ++q) {
        const engine::GeomInfo& g = snap.geoms[queue[q]];
        out.push_back(g.geom);
        queue.insert(queue.end(), g.children.begin(), g.children.end());
    }
    return out;
}

void ResetCommon(const Transform& base)
{
    g_base = base;
    g_yaw = 0;
    g_lift = 0;
    g_haveHit = false;
    g_haveNormal = false;
    g_pickPending = false;
    g_refused.clear();
}

} // namespace

void BeginPlacement(const std::string& sourcePath, const std::string& label, const std::string& templatePath)
{
    CancelPlacement();
    g_sourcePath = sourcePath;
    g_templatePath = templatePath;
    std::vector<engine::ZGEOM*> sources = Sources();
    std::string err;
    if (sources.empty() || !g_preview.Spawn(sources, "EditorPreview", &err)) {
        Log("placement: cannot create the preview: %s", sources.empty() ? "model not found" : err.c_str());
        g_sourcePath.clear();
        g_templatePath.clear();
        return;
    }
    SetPickIgnore(g_preview.parts);
    g_previewRoom = RoomOf(g_preview.Main());
    g_hitRoom = nullptr;
    g_label = templatePath.empty() ? label : label + " (working item)";
    ResetCommon(GetTransform(sources[0]));
    ImGuiIO& io = ImGui::GetIO();
    PointOnRay(io.MousePos.x, io.MousePos.y);
    g_preview.Place(CurrentTransform());
    Select(nullptr);
}

void BeginGrab(engine::ZGEOM* geom)
{
    CancelPlacement();
    if (!geom || !IsLive(geom))
        return;
    g_grab = geom;
    g_grabBefore = GetState(geom);
    Transform frame = GetTransform(geom);
    g_grabParts = VisibleParts(geom);
    if (g_grabParts.empty())
        g_grabParts.push_back(geom);
    for (engine::ZGEOM* part : g_grabParts)
        g_grabOffsets.push_back(Relative(frame, GetTransform(part)));
    SetPickIgnore(Subtree(geom));
    ResetCommon(frame);
}

void CancelPlacement()
{
    if (g_grab)
        EndGrab(false);
    DestroyPreview();
    g_sourcePath.clear();
    g_templatePath.clear();
}

bool Placing() { return !g_sourcePath.empty() || g_grab; }
bool Grabbing() { return g_grab != nullptr; }
std::string PlacementSource() { return g_sourcePath; }

void UpdatePlacement()
{
    if (!Placing())
        return;
    if (g_grab ? !IsLive(g_grab) : !g_preview.Live()) {
        if (g_grab) {  // gone (scene change): nothing to put back
            g_grab = nullptr;
            g_grabParts.clear();
            g_grabOffsets.clear();
            SetPickIgnore(nullptr);
        }
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
        if (io.MouseWheel != 0.0f) {
            float dir = io.MouseWheel > 0 ? 1.0f : -1.0f;
            if (io.KeyAlt) {
                float step = io.KeyCtrl ? GridStepFor(ShapeSize()) : kLiftStep;
                g_lift = std::fmax(0.0f, std::round((g_lift + dir * step) / step) * step);
            } else {
                g_yaw = std::fmod(g_yaw + dir * kRotateStep + 360.0f, 360.0f);
            }
        }
        DrawHint();
    }
    if (!g_grab && g_hitRoom && g_hitRoom != g_previewRoom && IsLive(g_hitRoom)) {
        // the engine draws a room's objects only with the room: move the preview over
        PartClones moved;
        std::string err;
        std::vector<engine::ZGEOM*> sources = Sources();
        if (!sources.empty() && moved.Spawn(sources, "EditorPreview", &err, g_hitRoom)) {
            DestroyPreview();
            g_preview = moved;
            SetPickIgnore(g_preview.parts);
        }
        g_previewRoom = g_hitRoom;  // also when it failed: do not retry every frame
    }
    if (overWorld && ImGui::IsKeyPressed(ImGuiKey_N, false))
        g_alignToSurface = !g_alignToSurface;
    if (!g_grab)
        SetHighlightExtra(g_preview.parts, 0xFF40E0FF);
    // a grabbed object stays where it is while the mouse is not over a surface
    Transform t = g_grab && !g_haveHit ? GetTransform(g_grab) : CurrentTransform();
    DrawGuide(t);
    if (g_grab) {
        Transform cur = GetTransform(g_grab);
        if (g_haveHit && memcmp(&t, &cur, sizeof(t)) != 0)
            SetTransform(g_grab, t);
    } else {
        g_preview.Place(t);
    }
    if (overWorld && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        if (g_grab)
            CommitGrab();
        else
            Commit(io.KeyShift);
    }
}

void InstallPlacement()
{
    OnScenePushed([](const std::string&) {
        g_preview.parts.clear();  // destroyed with the scene objects of the editor
        g_preview.offsets.clear();
        g_previewRoom = g_hitRoom = nullptr;
        g_grab = nullptr;
        g_grabParts.clear();
        g_grabOffsets.clear();
        SetPickIgnore(nullptr);
        SetHighlightExtra(nullptr, 0);
        g_sourcePath.clear();
        g_templatePath.clear();
    });
}

} // namespace h2::editor
