#include "vehicles.h"

#include <windows.h>
#include <algorithm>
#include <cmath>
#include <map>
#include <string>
#include <vector>

#include "imgui.h"

extern "C" {
#include "lauxlib.h"
#include "lua.h"
}

#include "../engine/geoms.h"
#include "../log.h"
#include "../mainloop.h"
#include "../scene.h"
#include "../script/events.h"
#include "../script/lua_vm.h"
#include "../script/objects_api.h"
#include "freecam.h"
#include "highlight.h"
#include "level.h"
#include "model.h"
#include "playtest.h"
#include "scene_panel.h"
#include "testtools.h"

namespace h2::editor {

namespace {

constexpr float kRad = 3.14159265f / 180.0f;
constexpr float kStep = 20.0f;         // cm between the curve's samples
constexpr size_t kMaxPoints = 256;     // a route's points
constexpr size_t kMaxDrives = 64;
constexpr float kMinSpeed = 10.0f, kMaxSpeed = 20000.0f;  // cm/s (0.36 .. 720 km/h)

struct Sample {
    float pos[3];
    float dist;  // along the curve from its start
};

struct Drive {
    int id = 0;
    engine::ZGEOM* object = nullptr;
    std::vector<Sample> curve;
    float length = 0.0f;
    float speed = 800.0f;
    bool loop = false;
    bool reverse = false;
    bool running = true;
    bool arrived = false;
    float at = 0.0f;       // distance driven along the curve
    float pitch0 = 0.0f;   // the object's own roll / pitch kept (pitch: added to the road's)
    float roll0 = 0.0f;
    float turn = 0.0f;     // its long side across its own z: driven turned by 90 degrees
    std::string owner;
};

std::vector<Drive> g_drives;
int g_nextId = 1;
double g_last;
std::vector<std::pair<int, std::string>> g_arrived;  // fired after the update (id, object path)

float Dist(const float a[3], const float b[3])
{
    float d[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]};
    return std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
}

// a Catmull-Rom curve through the points, sampled every kStep cm
std::vector<Sample> Curve(const std::vector<std::vector<float>>& pts, bool loop)
{
    std::vector<Sample> out;
    size_t n = pts.size();
    if (n < 2)
        return out;
    auto P = [&](long i) -> const float* {
        if (loop)
            return pts[size_t((i % long(n) + long(n)) % long(n))].data();
        return pts[size_t(std::clamp<long>(i, 0, long(n) - 1))].data();
    };
    size_t segments = loop ? n : n - 1;
    float dist = 0.0f;
    float last[3] = {P(0)[0], P(0)[1], P(0)[2]};
    out.push_back({{last[0], last[1], last[2]}, 0.0f});
    for (size_t s = 0; s < segments; ++s) {
        const float *p0 = P(long(s) - 1), *p1 = P(long(s)), *p2 = P(long(s) + 1), *p3 = P(long(s) + 2);
        int steps = std::max(1, int(std::ceil(Dist(p1, p2) / kStep)));
        for (int k = 1; k <= steps; ++k) {
            float t = float(k) / float(steps), t2 = t * t, t3 = t2 * t;
            float q[3];
            for (int c = 0; c < 3; ++c)
                q[c] = 0.5f * ((2.0f * p1[c]) + (-p0[c] + p2[c]) * t + (2.0f * p0[c] - 5.0f * p1[c] + 4.0f * p2[c] - p3[c]) * t2 +
                               (-p0[c] + 3.0f * p1[c] - 3.0f * p2[c] + p3[c]) * t3);
            dist += Dist(last, q);
            out.push_back({{q[0], q[1], q[2]}, dist});
            memcpy(last, q, sizeof(last));
        }
        if (out.size() > 200000)
            break;
    }
    return out;
}

// the curve's point at `at` and its direction
void At(const Drive& d, float at, float pos[3], float dir[3])
{
    const std::vector<Sample>& c = d.curve;
    auto it = std::lower_bound(c.begin(), c.end(), at, [](const Sample& s, float v) { return s.dist < v; });
    size_t i = std::clamp<size_t>(size_t(it - c.begin()), 1, c.size() - 1);
    const Sample &a = c[i - 1], &b = c[i];
    float span = b.dist - a.dist;
    float t = span > 0.0f ? std::clamp((at - a.dist) / span, 0.0f, 1.0f) : 0.0f;
    for (int k = 0; k < 3; ++k) {
        pos[k] = a.pos[k] + (b.pos[k] - a.pos[k]) * t;
        dir[k] = b.pos[k] - a.pos[k];
    }
}

void Place(Drive& d)
{
    if (d.curve.size() < 2)
        return;
    float pos[3], dir[3];
    At(d, d.at, pos, dir);
    float flat = std::sqrt(dir[0] * dir[0] + dir[2] * dir[2]);
    if (flat < 1e-3f)
        return;
    float yaw = std::atan2(dir[0], dir[2]) / kRad + (d.reverse ? 180.0f : 0.0f) + d.turn;
    float pitch = std::atan2(dir[1], flat) / kRad * (d.reverse ? -1.0f : 1.0f);
    Transform t;
    EulerToMatrix(yaw, pitch + d.pitch0, d.roll0, t.mat);
    memcpy(t.pos, pos, sizeof(t.pos));
    SetTransform(d.object, t);
}

// Which way the object is long, in its own frame: a car drives along its long side. Its parts'
// boxes (node bounds), spread along its x (right) and z (forward)
float LongSideTurn(engine::ZGEOM* object)
{
    Transform frame = GetTransform(object);
    const float* f = frame.mat;      // forward (z)
    const float* r = frame.mat + 6;  // right (x)
    std::vector<engine::ZGEOM*> parts = VisibleParts(object);
    if (parts.empty())
        parts.push_back(object);
    float lo[2] = {1e30f, 1e30f}, hi[2] = {-1e30f, -1e30f};
    for (engine::ZGEOM* p : parts) {
        Transform pt = GetTransform(p);
        const engine::ZBaseGeom* n = p->node;
        float c[3], h = std::max({n->boundHalf[0], n->boundHalf[1], n->boundHalf[2], 1.0f});
        for (int k = 0; k < 3; ++k)  // its box's middle in the world (node space: x right, y up, z forward)
            c[k] = pt.pos[k] + pt.mat[6 + k] * n->boundCenter[0] + pt.mat[3 + k] * n->boundCenter[1] +
                   pt.mat[k] * n->boundCenter[2];
        float d[3] = {c[0] - frame.pos[0], c[1] - frame.pos[1], c[2] - frame.pos[2]};
        float x = d[0] * r[0] + d[1] * r[1] + d[2] * r[2], z = d[0] * f[0] + d[1] * f[1] + d[2] * f[2];
        // the part's own extent along the object's axes
        float ex = 0.0f, ez = 0.0f;
        for (int k = 0; k < 3; ++k) {
            const float* axis = pt.mat + (k == 0 ? 6 : k == 1 ? 3 : 0);
            float half = n->boundHalf[k];
            ex += std::fabs(axis[0] * r[0] + axis[1] * r[1] + axis[2] * r[2]) * half;
            ez += std::fabs(axis[0] * f[0] + axis[1] * f[1] + axis[2] * f[2]) * half;
        }
        (void)h;
        lo[0] = std::min(lo[0], x - ex);
        hi[0] = std::max(hi[0], x + ex);
        lo[1] = std::min(lo[1], z - ez);
        hi[1] = std::max(hi[1], z + ez);
    }
    float sx = hi[0] - lo[0], sz = hi[1] - lo[1];
    Log("vehicles: %s is %.0f cm across, %.0f cm long (%d parts)", engine::SafeString(object->name).c_str(), sx, sz,
        int(parts.size()));
    return sx > sz * 1.2f ? 90.0f : 0.0f;  // (which end is the front: the C1-1 Ferrari's; "reverse" turns it)
}

bool IsGameVehicle(engine::ZGEOM* g)
{
    for (engine::ZBaseGeom* n = g && g->node ? g->node : nullptr; n; n = n->parent)
        if (n->geom && engine::IsA(n->geom, "ZMoveOnSpline"))
            return true;
    return false;
}

void Update()
{
    double now = double(GetTickCount64());
    double dt = std::clamp((now - g_last) / 1000.0, 0.0, 0.25);
    g_last = now;
    if (g_drives.empty() || ScenePaused() || !engine::SceneDataValid())
        return;
    dt *= GameSpeed();
    for (Drive& d : g_drives) {
        if (!d.running || d.arrived)
            continue;
        if (!d.object || !IsLive(d.object)) {
            d.running = false;
            continue;
        }
        d.at += float(dt) * d.speed;
        if (d.at >= d.length) {
            if (d.loop) {
                d.at = std::fmod(d.at, d.length);
            } else {
                d.at = d.length;
                d.arrived = true;
                g_arrived.push_back({d.id, PathOf(d.object)});
            }
        }
        Place(d);
    }
    std::vector<std::pair<int, std::string>> arrived;
    arrived.swap(g_arrived);
    for (auto& [id, path] : arrived)
        script::FireArrived(id, path);
}

Drive* Find(int id)
{
    for (Drive& d : g_drives)
        if (d.id == id)
            return &d;
    return nullptr;
}

// ---- Lua ----

// h2._drive(object path, {{x,y,z}, ...}, speed, loop, reverse) -> id
int LuaDrive(lua_State* L)
{
    engine::ZGEOM* g = Resolve(luaL_checkstring(L, 1));
    if (!g || !IsLive(g))
        return luaL_error(L, "drive: the object is not in the loaded scene");
    luaL_checktype(L, 2, LUA_TTABLE);
    std::vector<std::vector<float>> pts;
    for (lua_Integer i = 1; i <= lua_Integer(kMaxPoints) + 1; ++i) {
        if (lua_rawgeti(L, 2, i) == LUA_TNIL) {
            lua_pop(L, 1);
            break;
        }
        std::vector<float> p(3);
        for (int k = 0; k < 3; ++k) {
            lua_rawgeti(L, -1, k + 1);
            lua_Number v = lua_tonumber(L, -1);
            lua_pop(L, 1);
            if (!std::isfinite(v) || std::fabs(v) > 1e6)
                return luaL_error(L, "drive: point %d is not {x, y, z}", int(i));
            p[k] = float(v);
        }
        lua_pop(L, 1);
        pts.push_back(p);
    }
    std::string err;
    int id = StartDrive(g, pts, float(luaL_optnumber(L, 3, 800.0)), lua_toboolean(L, 4) != 0, lua_toboolean(L, 5) != 0,
                        "script", &err);
    if (!id)
        return luaL_error(L, "drive: %s", err.c_str());
    lua_pushinteger(L, id);
    return 1;
}

// h2._drivectl(id, "stop" | "go" | "speed" [, v] | "running" | "progress")
int LuaDriveCtl(lua_State* L)
{
    Drive* d = Find(int(luaL_checkinteger(L, 1)));
    std::string op = luaL_checkstring(L, 2);
    if (!d)
        return 0;
    if (op == "stop")
        d->running = false;
    else if (op == "go")
        d->running = true;
    else if (op == "speed")
        d->speed = std::clamp(float(luaL_checknumber(L, 3)), kMinSpeed, kMaxSpeed);
    else if (op == "running")
        lua_pushboolean(L, d->running && !d->arrived);
    else if (op == "progress")
        lua_pushnumber(L, d->length > 0.0f ? d->at / d->length : 1.0);
    else
        return luaL_error(L, "drive: no such control %s", op.c_str());
    return op == "running" || op == "progress" ? 1 : 0;
}

const char* const kLuaDrive = R"lua(
h2.drive = function(obj, route, opts)
  assert(type(obj) == "table" and obj._path, "drive: an object (h2.find)")
  opts = opts or {}
  local pts = {}
  local function add(p)
    if type(p) == "table" and p._path then
      local x, y, z = p:pos()
      pts[#pts + 1] = {x, y, z}
    elseif type(p) == "table" and type(p[1]) == "number" then
      pts[#pts + 1] = {p[1], p[2], p[3]}
    else
      error("drive: a route of objects, {x, y, z} tables or a list of points", 3)
    end
  end
  if type(route) == "table" and route._path then
    for _, p in ipairs(route:points()) do add(p) end
  else
    assert(type(route) == "table", "drive: a route")
    for _, p in ipairs(route) do add(p) end
  end
  local id = h2._drive(obj._path, pts, opts.speed or 800, opts.loop, opts.reverse)
  local v = {id = id, object = obj}
  function v:stop() h2._drivectl(id, "stop") end
  function v:go() h2._drivectl(id, "go") end
  function v:speed(s) h2._drivectl(id, "speed", s) end
  function v:running() return h2._drivectl(id, "running") end
  function v:progress() return h2._drivectl(id, "progress") end
  return v
end
)lua";

// ---- the editor: the selection's route ----

LevelVehicle* EntryFor(const std::string& path)
{
    for (LevelVehicle& v : CurrentLevel().vehicles)
        if (v.object == path)
            return &v;
    return nullptr;
}

bool g_picking;
std::string g_pickFor;  // the vehicle's path
bool g_pickPending;
int g_pickFrame;

void OnPick(engine::ZGEOM* hit, const float* point, const float*)
{
    g_pickPending = false;
    if (!g_picking || !point)
        return;
    LevelVehicle* v = EntryFor(g_pickFor);
    if (!v || v->route.size() >= kMaxPoints)
        return;
    engine::ZGEOM* room = hit ? RoomOf(hit) : nullptr;
    if (!room)
        room = RoomAt(point);
    if (!room)
        room = Resolve("ROOT");
    Transform t = {{0, 0, 1, 0, 1, 0, 1, 0, 0}, {point[0], point[1], point[2]}};
    std::string err;
    if (engine::ZGEOM* p = PlacePoint(t, room, &err)) {
        v->route.push_back(PathOf(p));
        CurrentLevel().dirty = true;
    }
}

} // namespace

int StartDrive(engine::ZGEOM* object, const std::vector<std::vector<float>>& points, float speed, bool loop,
               bool reverse, const std::string& owner, std::string* error)
{
    auto fail = [&](const std::string& why) {
        if (error)
            *error = why;
        return 0;
    };
    if (!object || !IsLive(object))
        return fail("no such object");
    if (IsGameVehicle(object))
        return fail("a vehicle of the game (its spline keeps it): drive a copy of its model");
    if (ActorOf(object))
        return fail("a character: npc:walkto walks it");
    if (points.size() < 2 || points.size() > kMaxPoints)
        return fail("a route of 2 .. 256 points");
    // one drive an object: a new one replaces the old
    g_drives.erase(std::remove_if(g_drives.begin(), g_drives.end(), [&](const Drive& d) { return d.object == object; }),
                   g_drives.end());
    if (g_drives.size() >= kMaxDrives)
        return fail("64 vehicles drive already");
    Drive d;
    d.id = g_nextId++;
    d.object = object;
    d.curve = Curve(points, loop);
    d.length = d.curve.empty() ? 0.0f : d.curve.back().dist;
    if (d.length < 1.0f)
        return fail("the route's points are all in one place");
    d.speed = std::clamp(speed, kMinSpeed, kMaxSpeed);
    d.loop = loop;
    d.reverse = reverse;
    d.owner = owner;
    float yaw;
    MatrixToEuler(GetTransform(object).mat, yaw, d.pitch0, d.roll0);
    d.pitch0 = 0.0f;  // the road's pitch instead
    d.turn = LongSideTurn(object);
    g_drives.push_back(std::move(d));
    Place(g_drives.back());
    return g_drives.back().id;
}

void StopDrives(const std::string& owner)
{
    g_drives.erase(std::remove_if(g_drives.begin(), g_drives.end(),
                                  [&](const Drive& d) { return owner.empty() || d.owner == owner; }),
                   g_drives.end());
}

void ApplyVehicles(const std::vector<LevelVehicle>& vehicles)
{
    StopDrives("level");
    for (const LevelVehicle& v : vehicles) {
        engine::ZGEOM* g = ResolveTarget(v.object);
        std::vector<std::vector<float>> pts;
        for (const std::string& p : v.route)
            if (engine::ZGEOM* pg = ResolveTarget(p); pg && IsLive(pg)) {
                Transform t = GetTransform(pg);
                pts.push_back({t.pos[0], t.pos[1], t.pos[2]});
            }
        std::string err;
        if (!StartDrive(g, pts, v.speed, v.loop, v.reverse, "level", &err))
            Log("vehicles: %s does not drive: %s", v.object.c_str(), err.c_str());
    }
}

bool VehiclePicking() { return g_picking; }

void UpdateVehiclePick()
{
    ImGuiIO& io = ImGui::GetIO();
    if (ImGui::IsKeyPressed(ImGuiKey_Escape) || ImGui::IsKeyPressed(ImGuiKey_Enter)) {
        g_picking = false;
        return;
    }
    LevelVehicle* v = EntryFor(g_pickFor);
    if (!v) {
        g_picking = false;
        return;
    }
    char hint[160];
    snprintf(hint, sizeof(hint), "click the road: point %d of the route   Esc / Enter: done", int(v->route.size()) + 1);
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    ImVec2 p(io.MousePos.x + 18, io.MousePos.y + 18);
    ImVec2 size = ImGui::CalcTextSize(hint);
    dl->AddRectFilled(ImVec2(p.x - 6, p.y - 4), ImVec2(p.x + size.x + 6, p.y + size.y + 4), IM_COL32(0, 0, 0, 170), 4);
    dl->AddText(p, IM_COL32(255, 220, 120, 255), hint);
    bool overWorld = !io.WantCaptureMouse && !ImGui::IsMouseDown(ImGuiMouseButton_Right);
    if (g_pickPending && ImGui::GetFrameCount() - g_pickFrame > 10)
        g_pickPending = false;
    if (overWorld && !g_pickPending && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        g_pickPending = true;
        g_pickFrame = ImGui::GetFrameCount();
        RequestPick(io.MousePos.x, io.MousePos.y, OnPick);
    }
}

// the route drawn: a line through its points
void DrawRoute(const LevelVehicle& v)
{
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    bool have = false;
    ImVec2 prev;
    for (const std::string& p : v.route) {
        engine::ZGEOM* g = ResolveTarget(p);
        if (!g)
            continue;
        Transform t = GetTransform(g);
        float lift[3] = {t.pos[0], t.pos[1] + 30.0f, t.pos[2]};
        float sx, sy;
        if (!WorldToScreen(lift, sx, sy)) {
            have = false;
            continue;
        }
        ImVec2 q(sx, sy);
        dl->AddCircleFilled(q, 5.0f, IM_COL32(255, 160, 40, 230));
        if (have)
            dl->AddLine(prev, q, IM_COL32(255, 160, 40, 200), 2.0f);
        prev = q;
        have = true;
    }
}

void DrawVehicleForSelection(engine::ZGEOM* geom)
{
    if (!geom || !IsLive(geom) || ActorOf(geom) || CurrentScene().empty() ||
        (!CurrentLevel().baseScene.empty() && CurrentLevel().baseScene != CurrentScene()))
        return;
    const SpawnedObject* spawned = FindSpawned(geom);
    if (spawned && spawned->point)
        return;  // a point: not a vehicle
    std::string path = TargetFor(geom);
    if (path.empty())
        return;
    ImGui::SeparatorText("Vehicle");
    if (IsGameVehicle(geom)) {
        ImGui::TextDisabled("a vehicle of the game: it follows its own track");
        ImGui::TextDisabled("(a copy of its model can drive a route)");
        return;
    }
    LevelVehicle* v = EntryFor(path);
    if (!v) {
        if (ImGui::Button("Make it drive a route")) {
            CurrentLevel().vehicles.push_back({path});
            CurrentLevel().dirty = true;
            if (CurrentLevel().baseScene.empty())
                CurrentLevel().baseScene = CurrentScene();
            g_pickFor = path;
            g_picking = true;
            g_pickPending = false;
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("A car (or anything) driving along points you click on the road,\n"
                              "when the level is played. Scripts: h2.drive(object, route, {speed=}).");
        return;
    }
    DrawRoute(*v);
    ImGui::Text("route: %d points", int(v->route.size()));
    ImGui::SameLine();
    bool picking = g_picking && g_pickFor == path;
    if (ImGui::SmallButton(picking ? "clicking the road... (Esc)" : "Add points")) {
        g_picking = !picking;
        g_pickFor = path;
        g_pickPending = false;
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Clear")) {
        v->route.clear();
        CurrentLevel().dirty = true;
    }
    float kmh = v->speed * 0.036f;
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 7);
    if (ImGui::DragFloat("speed", &kmh, 0.5f, 1.0f, 200.0f, "%.0f km/h")) {
        v->speed = std::clamp(kmh / 0.036f, kMinSpeed, kMaxSpeed);
        CurrentLevel().dirty = true;
    }
    if (ImGui::Checkbox("round and round", &v->loop))
        CurrentLevel().dirty = true;
    ImGui::SameLine();
    if (ImGui::Checkbox("its front is its back", &v->reverse))
        CurrentLevel().dirty = true;
    bool driving = false;
    for (const Drive& d : g_drives)
        driving |= d.object == geom && d.running && !d.arrived;
    if (ImGui::SmallButton(driving ? "Stop" : "Try it")) {
        if (driving) {
            StopDrives("level");
        } else {
            std::vector<LevelVehicle> one{*v};
            ApplyVehicles(one);
        }
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("It drives while the world runs (Freeze off, or the editor closed)");
    ImGui::SameLine();
    if (ImGui::SmallButton("No route")) {
        auto& all = CurrentLevel().vehicles;
        all.erase(std::remove_if(all.begin(), all.end(), [&](const LevelVehicle& e) { return e.object == path; }),
                  all.end());
        CurrentLevel().dirty = true;
        g_picking = false;
    }
}

void InstallVehicles()
{
    script::Register("_drive", LuaDrive);
    script::Register("_drivectl", LuaDriveCtl);
    if (lua_State* L = script::State())
        if (luaL_dostring(L, kLuaDrive) != LUA_OK) {
            Log("vehicles: %s", lua_tostring(L, -1));
            lua_pop(L, 1);
        }
    OnFrameEnd(Update);
    OnSceneLeaving([](const std::string&) {
        g_drives.clear();
        g_picking = false;
    });
    g_last = double(GetTickCount64());
}

} // namespace h2::editor
