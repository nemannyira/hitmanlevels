#include "vehicles.h"

#include <windows.h>
#include <algorithm>
#include <cctype>
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
#include "../engine/spawn.h"
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
    bool preview = false;  // Try it: it drives while the editor holds the world too
    // the object in its frame (ObjectFrame: a map's car group is at the world's origin, its parts
    // where they stand): the curve takes the frame, the object follows it
    float localPos[3] = {};
    float localRot[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
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
    Transform f;  // the frame on the curve
    EulerToMatrix(yaw, pitch + d.pitch0, d.roll0, f.mat);
    memcpy(f.pos, pos, sizeof(f.pos));
    // the object: its offset and rows in the frame's rows (forward, up, right)
    Transform t;
    for (int k = 0; k < 3; ++k)
        t.pos[k] = f.pos[k] + d.localPos[0] * f.mat[k] + d.localPos[1] * f.mat[3 + k] + d.localPos[2] * f.mat[6 + k];
    for (int j = 0; j < 3; ++j)
        for (int k = 0; k < 3; ++k)
            t.mat[j * 3 + k] = d.localRot[j * 3] * f.mat[k] + d.localRot[j * 3 + 1] * f.mat[3 + k] +
                               d.localRot[j * 3 + 2] * f.mat[6 + k];
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
    // its sides: parts named left / right (the C1-1 Ferrari's "D00r_Left/Door Left") in the frame
    float side[2] = {0.0f, 0.0f};
    int sides = 0, named = 0;
    auto sideOf = [](engine::ZGEOM* p) {
        for (int up = 0; p && up < 2; ++up) {  // the part's name, then its group's
            std::string n = engine::SafeString(p->name);
            for (char& ch : n)
                ch = char(std::tolower(static_cast<unsigned char>(ch)));
            bool left = n.find("left") != std::string::npos, right = n.find("right") != std::string::npos;
            if (left != right)
                return left ? 1 : -1;
            const engine::ZBaseGeom* parent = p->node ? p->node->parent : nullptr;
            p = parent && engine::IsReadable(parent, sizeof(*parent)) ? parent->geom : nullptr;
        }
        return 0;
    };
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
        if (int s = sideOf(p)) {
            side[0] += s * x;
            side[1] += s * z;
            sides += s;
            ++named;
        }
        lo[0] = std::min(lo[0], x - ex);
        hi[0] = std::max(hi[0], x + ex);
        lo[1] = std::min(lo[1], z - ez);
        hi[1] = std::max(hi[1], z + ez);
    }
    float sx = hi[0] - lo[0], sz = hi[1] - lo[1];
    bool alongX = sx > sz * 1.2f;
    // which end is its front: its left side (from the middle) turned to the front, as the game's
    // cars are made (x right, z forward: their *FrontLeft* wheels at -x, +z). Without named sides:
    // the long side's +x / +z ("its front is its back" turns it)
    float turn = alongX ? 90.0f : 0.0f;
    if (named > 0) {
        float mid[2] = {(lo[0] + hi[0]) * 0.5f, (lo[1] + hi[1]) * 0.5f};
        // the left side's direction: the named parts' places, less the middle once for each
        float left[2] = {side[0] - sides * mid[0], side[1] - sides * mid[1]};
        float front[2] = {left[1], -left[0]};  // left (-x) turned a quarter: forward (+z)
        if (alongX)
            turn = front[0] >= 0.0f ? 90.0f : -90.0f;
        else
            turn = front[1] >= 0.0f ? 0.0f : 180.0f;
    }
    Log("vehicles: %s is %.0f cm across, %.0f cm long (%d parts, %d named sides): its front turned %.0f",
        engine::SafeString(object->name).c_str(), sx, sz, int(parts.size()), named, turn);
    return turn;
}

// ---- the game's vehicles: their own tracks ----
//
// A ZMoveOnSpline's mover (+0x5C, vtable 0x65C884) keeps it on its spline:
// its place there (+0x04, 0 .. 1 along the track), the place it goes to
// (+0x4A), the way (+0x20: 1 / -1), its speed (+0x5B, cm/s), acceleration
// (+0x5F) and top speed (+0x63; the vehicle's own +0xC4 too: both 350 on
// C1-1's van, faster asked stays at that). Its vfunc 0 (0x596040: place,
// flag) sends it there along the track (it takes its speed with vfunc 0x08);
// scripts call it through the vehicle (MoveToSplineVal: DriveTrack).
constexpr uintptr_t kMoverVtable = 0x65C884;

char* TrackMover(engine::ZGEOM* g, engine::ZGEOM** vehicle = nullptr)
{
    for (engine::ZBaseGeom* n = g && g->node ? g->node : nullptr; n; n = n->parent) {
        if (!engine::IsReadable(n, sizeof(*n)))
            return nullptr;
        if (!n->geom || !engine::IsA(n->geom, "ZMoveOnSpline"))
            continue;
        auto m = *reinterpret_cast<char**>(reinterpret_cast<char*>(n->geom) + 0x5C);
        if (!m || !engine::IsReadable(m, 0x6C) || *reinterpret_cast<uintptr_t*>(m) != kMoverVtable)
            return nullptr;
        if (vehicle)
            *vehicle = n->geom;
        return m;
    }
    return nullptr;
}

// a vehicle's virtual call (thiscall, 2 or 4 arguments)
bool CarCall(engine::ZGEOM* car, int vfunc, uint32_t a, uint32_t b, uint32_t c, uint32_t d, int n)
{
    __try {
        void* fn = (*reinterpret_cast<void***>(car))[vfunc / 4];
        if (n == 2)
            reinterpret_cast<void(__thiscall*)(void*, uint32_t, uint32_t)>(fn)(car, a, b);
        else
            reinterpret_cast<void(__thiscall*)(void*, uint32_t, uint32_t, uint32_t, uint32_t)>(fn)(car, a, b, c, d);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

int g_lightEntriesMissing;  // (since the scene loaded: OnScenePushed)

uint32_t Bits(float f)
{
    uint32_t u;
    memcpy(&u, &f, 4);
    return u;
}

// The scene's light table (engine +0x5D1F, see inject.cpp): vfunc 0x24 (node) gives a node an entry of its
// own (0x4286F0: an empty one, {0, 1}), as the game does for the nodes it copies or registers at runtime
uint32_t RegisterLights(engine::ZBaseGeom* node)
{
    __try {
        uintptr_t sys = *reinterpret_cast<uintptr_t*>(0x6A6C5C);
        uintptr_t db = sys ? *reinterpret_cast<uintptr_t*>(sys + 0x98) : 0;
        void* table = db ? *reinterpret_cast<void**>(db + 0x5D1F) : nullptr;
        if (!table)
            return 0;
        auto reg = reinterpret_cast<uint32_t(__thiscall*)(void*, void*)>((*reinterpret_cast<void***>(table))[0x24 / 4]);
        return reg(table, node);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

bool OwnLightEntryOf(engine::ZGEOM* g)
{
    engine::ZBaseGeom* n = g ? g->node : nullptr;
    if (!n || !(n->flags & 0x8000))
        return false;
    uint32_t h = RegisterLights(n);
    if (!h) {
        static int told;
        ++g_lightEntriesMissing;
        if (told++ < 3)
            Log("vehicles: the scene's light table has no entry left for %s (it keeps the shared one)",
                engine::SafeString(g->name).c_str());
        return false;
    }
    *reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(n) + 0x6C) = h;
    n->flags |= 0x1000000;  // (its lights looked at again, as 0x5BB2C0 does)
    return true;
}

// a level's car: its parts (made with the map, with an empty entry shared) get light table entries of their own
void OwnLightEntries(engine::ZGEOM* car)
{
    int made = 0;
    for (engine::ZGEOM* p : VisibleParts(car))
        made += OwnLightEntryOf(p);
    Log("vehicles: %s: %d parts with light table entries of their own", engine::SafeString(car->name).c_str(), made);
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
    if (g_drives.empty() || !engine::SceneDataValid())
        return;
    bool paused = ScenePaused();
    if (!paused)
        dt *= GameSpeed();
    for (Drive& d : g_drives) {
        if (!d.running || d.arrived || (paused && !d.preview))
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

} // namespace

// As the game's scripts drive them (MoveOnSpline_if): SetMoveParams (vfunc 0x2B0, 0x591680: the
// mover's speed, acceleration and top speed, 0x5963B0, taken at once while it drives; the vehicle's
// +0xC4) and MoveToSplineVal (vfunc 0x2CC, 0x591810: the mover's vfunc 0, then the vehicle's arrival
// flags +0xD0 / +0xD1 cleared)
bool DriveTrack(engine::ZGEOM* vehicle, float to, float speed, std::string* error)
{
    engine::ZGEOM* group = nullptr;
    char* m = vehicle && IsLive(vehicle) ? TrackMover(vehicle, &group) : nullptr;
    if (!m || !group) {
        if (error)
            *error = "not a vehicle of the game (a copy drives a route: h2.drive)";
        return false;
    }
    if (!std::isfinite(to))
        to = 0.0f;
    to = std::clamp(to, 0.0f, 1.0f);
    if (speed > 0.0f && std::isfinite(speed)) {
        float s = std::clamp(speed, kMinSpeed, kMaxSpeed);
        float accel = *reinterpret_cast<float*>(m + 0x5F);  // (its own)
        if (!(accel > 0.0f && std::isfinite(accel)))
            accel = 400.0f;
        if (!CarCall(group, 0x2B0, Bits(s), Bits(accel), Bits(s), Bits(s), 4)) {
            if (error)
                *error = "it did not take the speed";
            return false;
        }
    }
    if (!CarCall(group, 0x2CC, Bits(to), 0, 0, 0, 2)) {
        if (error)
            *error = "its mover did not take it";
        return false;
    }
    return true;
}

float TrackPlace(engine::ZGEOM* vehicle)
{
    char* m = vehicle && IsLive(vehicle) ? TrackMover(vehicle) : nullptr;
    return m ? *reinterpret_cast<float*>(m + 4) : -1.0f;
}

bool StopTrack(engine::ZGEOM* vehicle)
{
    float at = TrackPlace(vehicle);
    return at >= 0.0f && DriveTrack(vehicle, at, 0.0f);
}

namespace {

// ---- Lua ----

// h2._track(object path, place 0..1 | nil (stop) [, km/h]) -> true / error
int LuaTrack(lua_State* L)
{
    engine::ZGEOM* g = Resolve(luaL_checkstring(L, 1));
    std::string err;
    bool ok = lua_isnoneornil(L, 2) ? StopTrack(g)
                                    : DriveTrack(g, float(luaL_checknumber(L, 2)),
                                                 float(luaL_optnumber(L, 3, 0.0) / 0.036), &err);
    if (!ok)
        return luaL_error(L, "track: %s", err.empty() ? "not a vehicle of the game" : err.c_str());
    lua_pushboolean(L, 1);
    return 1;
}

// h2._trackpos(object path) -> 0 .. 1 / nil
int LuaTrackPos(lua_State* L)
{
    float at = TrackPlace(Resolve(luaL_checkstring(L, 1)));
    if (at < 0.0f)
        return 0;
    lua_pushnumber(L, at);
    return 1;
}

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
h2.track = function(vehicle, to, kmh)
  assert(type(vehicle) == "table" and vehicle._path, "track: a vehicle of the game (h2.find)")
  return h2._track(vehicle._path, to, kmh)
end
h2.trackpos = function(vehicle)
  assert(type(vehicle) == "table" and vehicle._path, "trackpos: a vehicle of the game (h2.find)")
  return h2._trackpos(vehicle._path)
end
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

// a route's point there (in the room of that place)
std::string RoutePoint(const float point[3], engine::ZGEOM* hit)
{
    engine::ZGEOM* room = hit ? RoomOf(hit) : nullptr;
    if (!room)
        room = RoomAt(point);
    if (!room)
        room = Resolve("ROOT");
    Transform t = {{0, 0, 1, 0, 1, 0, 1, 0, 0}, {point[0], point[1], point[2]}};
    std::string err;
    engine::ZGEOM* p = PlacePoint(t, room, &err);
    return p ? PathOf(p) : "";
}

// a route starts where the object stands (then the clicked points)
void StartPoint(LevelVehicle& v, engine::ZGEOM* object)
{
    Transform f = ObjectFrame(object);
    std::string p = RoutePoint(f.pos, nullptr);
    if (!p.empty())
        v.route.push_back(p);
}

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
    Transform frame = ObjectFrame(object), now = GetTransform(object);
    float yaw;
    MatrixToEuler(frame.mat, yaw, d.pitch0, d.roll0);
    d.pitch0 = 0.0f;  // the road's pitch instead
    d.turn = LongSideTurn(object);
    // the object in the frame's rows, the frame's own turn taken out (the curve gives it)
    float fr[9];
    EulerToMatrix(yaw, 0.0f, d.roll0, fr);
    float rel[3] = {now.pos[0] - frame.pos[0], now.pos[1] - frame.pos[1], now.pos[2] - frame.pos[2]};
    for (int i = 0; i < 3; ++i)
        d.localPos[i] = rel[0] * fr[i * 3] + rel[1] * fr[i * 3 + 1] + rel[2] * fr[i * 3 + 2];
    for (int j = 0; j < 3; ++j)
        for (int i = 0; i < 3; ++i)
            d.localRot[j * 3 + i] = now.mat[j * 3] * fr[i * 3] + now.mat[j * 3 + 1] * fr[i * 3 + 1] +
                                    now.mat[j * 3 + 2] * fr[i * 3 + 2];
    d.roll0 = 0.0f;  // (in localRot now)
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

// ---- the level's vehicles, as the game's: a ZCar along its ZSHAPE ----
//
// Made with the map (inject.cpp / imports.cpp), then driven by the car's own
// mover through the vfuncs its scripts call (MoveOnSpline_if, 0x58CE30):
// 0x2AC SetPath(spline ref, start 0..1: the mover 0x595F30), 0x2B0
// SetMoveParams(speed, acceleration, top speed, +0xC4: 0x5963B0), 0x2CC
// MoveToSplineVal(place, flag: the mover's vfunc 0 0x596040).

std::string CarName(size_t i) { return "EditorCar_" + std::to_string(i + 1); }
std::string ShapeName(size_t i) { return "EditorPath_" + std::to_string(i + 1); }

struct LevelCar {
    engine::ZGEOM* car = nullptr;
    uint32_t path = 0;  // its spline's ref
    bool loop = false;
};
std::vector<LevelCar> g_levelCars;

// the route's knots for the spline: a Catmull-Rom curve through the points as cubic Bezier
// segments (each knot: the point, its control point out, the next one's control point in)
std::vector<float> Knots(const std::vector<std::array<float, 3>>& in, bool loop)
{
    std::vector<std::array<float, 3>> p = in;
    if (loop && p.size() > 2)
        p.push_back(p.front());  // closed: back to the start
    size_t n = p.size();
    std::vector<float> out;
    auto at = [&](long i) -> const std::array<float, 3>& {
        if (loop && in.size() > 2) {
            long m = long(in.size());
            return in[size_t(((i % m) + m) % m)];
        }
        return p[size_t(std::clamp(i, 0L, long(n) - 1))];
    };
    for (size_t i = 0; i < n; ++i) {
        const auto& a = p[i];
        const auto& prev = at(long(i) - 1);
        const auto& next = at(long(i) + 1);
        const auto& next2 = at(long(i) + 2);
        float k[9];
        for (int c = 0; c < 3; ++c) {
            k[c] = a[c];
            k[3 + c] = a[c] + (next[c] - prev[c]) / 6.0f;      // out of this one
            k[6 + c] = next[c] - (next2[c] - a[c]) / 6.0f;     // into the next one
        }
        out.insert(out.end(), k, k + 9);
    }
    return out;
}

void ApplyVehicles(const std::vector<LevelVehicle>& vehicles)
{
    StopDrives("level");
    g_levelCars.clear();
    for (size_t i = 0; i < vehicles.size(); ++i) {
        const LevelVehicle& v = vehicles[i];
        engine::ZGEOM* car = Resolve("ROOT/" + CarName(i));
        engine::ZGEOM* path = Resolve("ROOT/" + ShapeName(i));
        if (!car || !IsLive(car) || !path || !IsLive(path)) {
            if (v.model >= 0 && v.points.size() >= 2)
                Log("vehicles: %s: its car is made with the map (load it again)", v.object.c_str());
            continue;
        }
        if (v.spawnedId.empty())
            if (engine::ZGEOM* source = ResolveTarget(v.object); source && IsLive(source))
                SetHidden(source, true);  // the map's car: the copy drives instead
        OwnLightEntries(car);
        uint32_t ref = engine::RefOf(path);
        float speed = std::clamp(v.speed, kMinSpeed, kMaxSpeed);
        bool ok = CarCall(car, 0x2B0, Bits(speed), Bits(400.0f), Bits(speed), Bits(speed), 4) &&
                  CarCall(car, 0x2AC, ref, Bits(0.0f), 0, 0, 2) && CarCall(car, 0x2CC, Bits(1.0f), 0, 0, 0, 2);
        Log("vehicles: %s drives its path %s (%.0f cm/s%s)%s", CurrentLevel().name.c_str(), ShapeName(i).c_str(), speed,
            v.loop ? ", round and round" : "", ok ? "" : ": the car did not take it");
        if (ok)
            g_levelCars.push_back({car, ref, v.loop});
    }
}

// round and round: at its path's end (closed) back to its start, on again
void LoopCars()
{
    for (LevelCar& c : g_levelCars) {
        if (!c.loop || !IsLive(c.car))
            continue;
        float at = TrackPlace(c.car);
        if (at >= 0.999f) {
            CarCall(c.car, 0x2AC, c.path, Bits(0.0f), 0, 0, 2);
            CarCall(c.car, 0x2CC, Bits(1.0f), 0, 0, 0, 2);
        }
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

engine::ZGEOM* WholeVehicle(engine::ZGEOM* part)
{
    if (!part || !IsLive(part) || !engine::MeshOf(part) || !part->node)
        return nullptr;
    // up through its groups to the one in a room (C1-1's Ferrari: Karroseri ->
    // Carbody -> CarFerrari, in the garage's room)
    engine::ZGEOM* group = nullptr;
    engine::ZBaseGeom* n = part->node->parent;
    for (int depth = 0; n && depth < 6; ++depth, n = n->parent) {
        if (!engine::IsReadable(n, sizeof(*n)) || !n->geom || !IsLive(n->geom))
            return nullptr;
        std::string cls = engine::ClassName(n->geom);
        if (cls == "ZROOM")
            break;
        if (cls != "ZGROUP" || engine::MeshOf(n->geom))
            return nullptr;  // (a character's, an item's: not a car)
        group = n->geom;
    }
    if (!group || !n)
        return nullptr;  // (not in a room: a zip's group, the root)
    size_t parts = VisibleParts(group).size();
    return parts >= 2 && parts <= 64 ? group : nullptr;
}

void BakeVehicles(std::vector<LevelVehicle>& vehicles)
{
    const auto& records = engine::SceneRecords();
    for (LevelVehicle& v : vehicles) {
        engine::ZGEOM* object = ResolveTarget(v.object);
        if (!object || !IsLive(object))
            continue;  // (not on the map now: as it was baked)
        const SpawnedObject* s = FindSpawned(object);
        v.spawnedId = s ? s->id : "";
        engine::ZGEOM* model = s && s->group ? Resolve(s->sourcePath) : object;
        int rec = model ? engine::RecordOf(model) : -1;
        if (rec < 0 || size_t(rec) >= records.size() || !(records[rec].classId & 0x100000))
            rec = -1;
        v.model = rec;
        v.parts.clear();
        if (rec >= 0) {
            // the car's frame: where the model stands, its long side forward
            Transform f = ObjectFrame(model);
            float yaw, pitch, roll;
            MatrixToEuler(f.mat, yaw, pitch, roll);
            float fr[9];
            EulerToMatrix(yaw + LongSideTurn(model) + (v.reverse ? 180.0f : 0.0f), 0.0f, 0.0f, fr);
            // its plain models (records of the scene), the biggest (the body) first
            std::vector<std::pair<float, engine::ZGEOM*>> models;
            for (engine::ZGEOM* c : VisibleParts(model)) {
                float ctr[3], r = 0.0f;
                if (engine::RecordOf(c) >= 0 && engine::MeshOf(c))
                    models.push_back({GetBounds(c, ctr, r) ? r : 0.0f, c});
            }
            std::stable_sort(models.begin(), models.end(),
                             [](const auto& a, const auto& b) { return a.first > b.first; });
            for (const auto& [size, c] : models) {
                Transform w = GetTransform(c);
                std::array<float, 12> l{};
                float rel[3] = {w.pos[0] - f.pos[0], w.pos[1] - f.pos[1], w.pos[2] - f.pos[2]};
                // in the frame's x (right: row 2), y (up: row 1), z (forward: row 0)
                static const int kAxis[3] = {2, 1, 0};
                for (int a = 0; a < 3; ++a) {
                    const float* ax = fr + kAxis[a] * 3;
                    l[9 + a] = rel[0] * ax[0] + rel[1] * ax[1] + rel[2] * ax[2];
                }
                for (int j = 0; j < 3; ++j)
                    for (int a = 0; a < 3; ++a) {
                        const float* ax = fr + kAxis[a] * 3;
                        l[j * 3 + a] = w.mat[j * 3] * ax[0] + w.mat[j * 3 + 1] * ax[1] + w.mat[j * 3 + 2] * ax[2];
                    }
                v.parts.push_back({engine::RecordOf(c), l});
            }
            // its box in the frame (x right, y up, z forward): the wheels near its corners, low
            float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
            for (const auto& [size, c] : models) {
                Transform pt = GetTransform(c);
                const engine::ZBaseGeom* n = c->node;
                float ctr[3];
                for (int k = 0; k < 3; ++k)
                    ctr[k] = pt.pos[k] + pt.mat[6 + k] * n->boundCenter[0] + pt.mat[3 + k] * n->boundCenter[1] +
                             pt.mat[k] * n->boundCenter[2];
                static const int kAxis[3] = {2, 1, 0};
                for (int a = 0; a < 3; ++a) {
                    const float* ax = fr + kAxis[a] * 3;
                    float m = (ctr[0] - f.pos[0]) * ax[0] + (ctr[1] - f.pos[1]) * ax[1] + (ctr[2] - f.pos[2]) * ax[2];
                    float e = 0.0f;
                    for (int k = 0; k < 3; ++k) {
                        const float* axis = pt.mat + (k == 0 ? 6 : k == 1 ? 3 : 0);
                        e += std::fabs(axis[0] * ax[0] + axis[1] * ax[1] + axis[2] * ax[2]) * n->boundHalf[k];
                    }
                    lo[a] = std::min(lo[a], m - e);
                    hi[a] = std::max(hi[a], m + e);
                }
            }
            v.wheels.clear();
            if (hi[0] > lo[0]) {
                float w = hi[0] - lo[0], len = hi[2] - lo[2];
                float x = w * 0.5f - w * 0.12f, y = lo[1] + 30.0f, mid = (hi[0] + lo[0]) * 0.5f;
                float front = hi[2] - len * 0.2f, back = lo[2] + len * 0.2f;
                v.wheels = {{mid - x, y, front}, {mid + x, y, front}, {mid - x, y, back}, {mid + x, y, back}};
            }
        }
        std::vector<std::array<float, 3>> pts;
        for (const std::string& p : v.route)
            if (engine::ZGEOM* g = ResolveTarget(p); g && IsLive(g)) {
                Transform t = GetTransform(g);
                pts.push_back({t.pos[0], t.pos[1], t.pos[2]});
            }
        if (pts.size() >= 2 || v.route.empty())
            v.points = pts;
    }
}

std::vector<engine::SceneShape> VehicleShapes(const std::vector<LevelVehicle>& vehicles)
{
    std::vector<engine::SceneShape> out;
    for (size_t i = 0; i < vehicles.size(); ++i)
        if (vehicles[i].model >= 0 && vehicles[i].points.size() >= 2)
            out.push_back({ShapeName(i), Knots(vehicles[i].points, vehicles[i].loop)});
    return out;
}

std::vector<engine::SceneCar> VehicleCars(const std::vector<LevelVehicle>& vehicles)
{
    std::vector<engine::SceneCar> out;
    for (size_t i = 0; i < vehicles.size(); ++i) {
        const LevelVehicle& v = vehicles[i];
        if (v.model < 0 || v.parts.empty() || v.wheels.size() != 4 || v.points.size() < 2)
            continue;
        engine::SceneCar c;
        c.name = CarName(i);
        c.shape = ShapeName(i);
        c.model = v.model;
        c.parts = v.parts;
        c.wheels = v.wheels;
        memcpy(c.pos, v.points.front().data(), sizeof(c.pos));  // (the spline puts it on its start)
        out.push_back(c);
    }
    return out;
}

bool IsVehicleCopy(const std::vector<LevelVehicle>& vehicles, const std::string& spawnedId)
{
    if (spawnedId.empty())
        return false;
    for (const LevelVehicle& v : vehicles)
        if (v.spawnedId == spawnedId && v.model >= 0 && v.points.size() >= 2)
            return true;
    return false;
}

void DrawVehicleForSelection(engine::ZGEOM* geom)
{
    if (!geom || !IsLive(geom) || ActorOf(geom) || CurrentScene().empty() ||
        (!CurrentLevel().baseScene.empty() && CurrentLevel().baseScene != CurrentScene()))
        return;
    const SpawnedObject* spawned = FindSpawned(geom);
    if (spawned && spawned->point)
        return;  // a point: not a vehicle
    engine::ZGEOM* whole = WholeVehicle(geom);
    if (whole && !ActorOf(whole) && !FindSpawned(geom))
        geom = whole;  // (a car clicked in the world: one of its parts; the car drives)
    else
        whole = nullptr;
    std::string path = TargetFor(geom);
    if (path.empty())
        return;
    ImGui::SeparatorText("Vehicle");
    if (whole)
        ImGui::TextDisabled("the whole %s (%d models)", engine::SafeString(whole->name).c_str(),
                            int(VisibleParts(whole).size()));
    if (IsGameVehicle(geom)) {
        static float kmh = 18.0f;
        float at = TrackPlace(geom);
        if (at < 0.0f) {
            ImGui::TextDisabled("a vehicle of the game: it follows its own track");
            ImGui::TextDisabled("(a copy of its model can drive a route)");
            return;
        }
        ImGui::Text("Its own track: at %.0f%%", at * 100.0f);
        if (ImGui::SmallButton("To its start"))
            DriveTrack(geom, 0.0f, kmh / 0.036f);
        ImGui::SameLine();
        if (ImGui::SmallButton("To its end"))
            DriveTrack(geom, 1.0f, kmh / 0.036f);
        ImGui::SameLine();
        if (ImGui::SmallButton("Stop"))
            StopTrack(geom);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("It goes while the world runs (Freeze off, or the editor closed);\n"
                              "its own script may send it elsewhere too. Lua: h2.track");
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 7);
        ImGui::DragFloat("speed##track", &kmh, 0.5f, 1.0f, 200.0f, "%.0f km/h");
        ImGui::TextDisabled("a copy of its model can drive a route of its own");
        return;
    }
    LevelVehicle* v = EntryFor(path);
    if (!v) {
        if (ImGui::Button("Make it drive a route")) {
            CurrentLevel().vehicles.push_back({path});
            StartPoint(CurrentLevel().vehicles.back(), geom);
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
        StartPoint(*v, geom);  // (it starts where it stands)
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
    if (ImGui::SmallButton("Test now (save and reload)"))
        TestLevel();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("The car is made with the map as the game's are (a ZCar on a path of its own):\n"
                          "the map loads again and it drives (when the world runs)");
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
    script::Register("_track", LuaTrack);
    script::Register("_trackpos", LuaTrackPos);
    if (lua_State* L = script::State())
        if (luaL_dostring(L, kLuaDrive) != LUA_OK) {
            Log("vehicles: %s", lua_tostring(L, -1));
            lua_pop(L, 1);
        }
    OnFrameEnd(Update);
    OnFrameEnd(LoopCars);
    OnSceneLeaving([](const std::string&) {
        g_levelCars.clear();
        g_drives.clear();
        g_picking = false;
    });
    g_last = double(GetTickCount64());
}

bool OwnLightEntry(engine::ZGEOM* model)
{
    static bool hooked;
    if (!hooked) {
        hooked = true;
        OnScenePushed([](const std::string&) { g_lightEntriesMissing = 0; });
    }
    return OwnLightEntryOf(model);
}

int LightEntriesMissing() { return g_lightEntriesMissing; }

} // namespace h2::editor
