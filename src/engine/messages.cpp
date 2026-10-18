#include "messages.h"

#include <windows.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <set>
#include <string>

#include "../game.h"
#include "npc.h"
#include "scene_data.h"

namespace h2::engine {

namespace {

constexpr uintptr_t kRegisterZMsg = 0x41EED0;  // ZEngineDataBase::RegisterZMsg(name, 0, file, line) -> ushort
constexpr int kGeom_SendMessage = 0x120 / 4;   // sender->(ZGEOM* receiver, int msg, void* param)
constexpr int kActor_IsAlive = 0x544 / 4;
// Actor_If 2 (Die, 0x585000): drops what it holds, then dies (0x468D80: alive
// +0x574 = 0 through vfunc 0x540, the death animation) - with or without a
// behaviour
constexpr int kActor_Die = 0x5F0 / 4;
// Actor_If's commands (docs/npc.md): MoveToPosition 0x5B4(1.0), 0x5C0(&path),
// 0x594(0), 0x58C(&path, pos); LookAt 0x610(ref); UseMoveSet 0x564(0x574(name))
constexpr int kActor_MoveFactor = 0x5B4 / 4;
constexpr int kActor_NewPath = 0x5C0 / 4;
constexpr int kActor_594 = 0x594 / 4;
constexpr int kActor_MoveTo = 0x58C / 4;
constexpr int kActor_LookAt = 0x610 / 4;      // (ref) keeps the target (+0x564); turns now only while looking
constexpr int kActor_StartLook = 0x37C / 4;   // 0x46F630 (ref): the head turns to it (0: stops), +0x105 bit 0 set
constexpr size_t kActor_Looking = 0x105;
constexpr int kActor_MoveSetId = 0x574 / 4;
constexpr int kActor_UseMoveSet = 0x564 / 4;
constexpr int kActor_AnimSet = 0x29C / 4;     // the character's animation sets
constexpr int kActor_AnimSet2 = 0x2A0 / 4;    //   (looked in first)
constexpr int kAnimSet_Find = 0x18 / 4;       // (name) -> animation
constexpr int kActor_PlayAnim = 0x214 / 4;    // (anim, mode, 0, -1.0, 1.0) -> playing animation
constexpr uint32_t kHitmanClass = 0x2003E8;
constexpr size_t kHitman_Health = 0xD72;         // float hit points (ZHITMAN2 is not a ZActor)
constexpr uint32_t kLevelControlClass = 0x2000E1;
constexpr uint32_t kPathfinderClass = 0x200075;  // ZPathfinder3 (rEnemyPathFinder3)
constexpr size_t kPathfinder_Nav = 0x64;         // the network (its data block, 0x5995A0)
constexpr size_t kPathfinder_YOffset = 0x20;     // float: RequestPath 0x599930 adds it and 30 to y
constexpr float kPathfinder_YLift = 30.0f;       // (0x639850)
// nav (from*, to*, char line): false when either point has no cell of the
// network near it (0x59B4A0: its cell, 0x59B4E0, then the nearest walkable one
// 0x59B610 within one cell across and 10 down); line 0 and from == to: true
// for a point on it
constexpr uintptr_t kNav_Connected = 0x59A8E0;
// nav (u16 out[3], const u16 in[3], int down, int across, float): the nearest
// walkable cell in the box (in - (across, across, down) .. in + (across, across, 0))
constexpr uintptr_t kNav_NearestCell = 0x59B610;
// The grid (0x59B530: world -> cell): min / max corners (float[3]) and cells
// along x, z, y (cell u16 [x, z, y], y up)
constexpr size_t kNav_Min = 0x05, kNav_Max = 0x11, kNav_CellsX = 0x3B, kNav_CellsZ = 0x3F, kNav_CellsY = 0x43;
const char* const kInstantDeath = "Msg_InstantDeath";

template <typename T>
T VFunc(void* obj, int index)
{
    return reinterpret_cast<T>((*reinterpret_cast<void***>(obj))[index]);
}

void* Engine()
{
    auto sys = reinterpret_cast<char*>(game::SysInterface());
    return sys ? *reinterpret_cast<void**>(sys + 0x98) : nullptr;
}

int SafeRegister(void* engine, const char* name)
{
    __try {
        return reinterpret_cast<uint16_t(__thiscall*)(void*, const char*, int, const char*, int)>(kRegisterZMsg)(
            engine, name, 0, "h2editor", 0);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

constexpr uintptr_t kAddAction = 0x488A70;  // __fastcall (geom, text; text, kind, msg, receiver ref, distance, 0)
constexpr int kActionUse = 6;

bool SafeAddAction(ZGEOM* geom, const char* text, uint16_t msg, uint32_t receiver, int distance)
{
    __try {
        reinterpret_cast<void*(__fastcall*)(ZGEOM*, const char*, const char*, int, int, uint32_t, int, int)>(
            kAddAction)(geom, text, text, kActionUse, msg, receiver, distance, 0);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool SafeSend(ZGEOM* to, ZGEOM* from, int msg, void* param)
{
    __try {
        // the sender is `this`, the receiver the first argument (0x43CC20)
        VFunc<void(__thiscall*)(ZGEOM*, ZGEOM*, int, void*)>(from, kGeom_SendMessage)(from, to, msg, param);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

int SafeIsAlive(ZGEOM* actor)
{
    __try {
        return VFunc<int(__thiscall*)(ZGEOM*)>(actor, kActor_IsAlive)(actor) != 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

bool SafeDie(ZGEOM* actor)
{
    __try {
        VFunc<void(__thiscall*)(ZGEOM*)>(actor, kActor_Die)(actor);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool SafeWalkTo(ZGEOM* actor, const float pos[3])
{
    __try {
        char path[64] = {};  // MoveToPosition: 12 bytes of its frame
        float p[3] = {pos[0], pos[1], pos[2]};
        VFunc<void(__thiscall*)(ZGEOM*, float)>(actor, kActor_MoveFactor)(actor, 1.0f);
        VFunc<void(__thiscall*)(ZGEOM*, void*)>(actor, kActor_NewPath)(actor, path);
        VFunc<void(__thiscall*)(ZGEOM*, int)>(actor, kActor_594)(actor, 0);
        VFunc<void(__thiscall*)(ZGEOM*, void*, float*)>(actor, kActor_MoveTo)(actor, path, p);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool SafeLookAt(ZGEOM* actor, uint32_t ref)
{
    __try {
        VFunc<void(__thiscall*)(ZGEOM*, uint32_t)>(actor, kActor_LookAt)(actor, ref);
        bool looking = (reinterpret_cast<const uint8_t*>(actor)[kActor_Looking] & 1) != 0;
        if (!looking || !ref)
            VFunc<void(__thiscall*)(ZGEOM*, uint32_t)>(actor, kActor_StartLook)(actor, ref);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool SafeUseMoveSet(ZGEOM* actor, const char* name)
{
    __try {
        int id = VFunc<int(__thiscall*)(ZGEOM*, const char*)>(actor, kActor_MoveSetId)(actor, name);
        VFunc<void(__thiscall*)(ZGEOM*, int)>(actor, kActor_UseMoveSet)(actor, id);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void* SafeFindAnim(ZGEOM* actor, const char* name)
{
    __try {
        void* set = VFunc<void*(__thiscall*)(ZGEOM*)>(actor, kActor_AnimSet)(actor);
        if (!set)
            return nullptr;
        if (void* set2 = VFunc<void*(__thiscall*)(ZGEOM*)>(actor, kActor_AnimSet2)(actor))
            if (void* a = VFunc<void*(__thiscall*)(void*, const char*)>(set2, kAnimSet_Find)(set2, name))
                return a;
        return VFunc<void*(__thiscall*)(void*, const char*)>(set, kAnimSet_Find)(set, name);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

bool SafePlayAnim(ZGEOM* actor, void* anim, int mode)
{
    __try {
        using Play = void*(__thiscall*)(ZGEOM*, void*, int, int, float, float);
        return VFunc<Play>(actor, kActor_PlayAnim)(actor, anim, mode, 0, -1.0f, 1.0f) != nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// 1: on the network, 0: not, -1: failed
int SafeOnNav(void* nav, const float pos[3])
{
    __try {
        float p[3] = {pos[0], pos[1], pos[2]};
        float q[3] = {pos[0], pos[1], pos[2]};
        int r = reinterpret_cast<int(__thiscall*)(void*, float*, float*, char)>(kNav_Connected)(nav, p, q, 0);
        return (r & 0xFF) != 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

bool SafeNearestCell(void* nav, uint16_t out[3], const uint16_t in[3])
{
    __try {
        using Nearest = int(__thiscall*)(void*, uint16_t*, const uint16_t*, int, int, float);
        return (reinterpret_cast<Nearest>(kNav_NearestCell)(nav, out, in, 10, 1, 1.0f) & 0xFF) != 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool Usable(ZGEOM* g)
{
    return g && IsReadable(g, sizeof(ZGEOM)) && g->node && IsReadable(g->node, sizeof(ZBaseGeom)) &&
           g->node->geom == g && SceneDataValid();
}

ZGEOM* FirstRecord(bool (*match)(const SceneRecord&))
{
    for (const SceneRecord& r : SceneRecords())
        if (match(r))
            if (ZGEOM* g = RecordGeom(r.index); Usable(g))
                return g;
    return nullptr;
}

} // namespace

uint16_t RegisterMessage(const char* name)
{
    // the engine keeps the name pointer: names live as long as the game
    static std::set<std::string> names;
    void* engine = Engine();
    if (!engine || !name)
        return 0;
    const std::string& kept = *names.insert(name).first;
    int id = SafeRegister(engine, kept.c_str());
    return id < 0 ? 0 : uint16_t(id);
}

bool SendMessage(ZGEOM* to, ZGEOM* from, uint16_t msg, void* param)
{
    if (!Usable(to) || !msg)
        return false;
    return SafeSend(to, from && Usable(from) ? from : to, msg, param);  // the sender's vfunc is called
}

// a living ZActor (the commands below call its virtual functions)
static bool ActorAlive(ZGEOM* actor)
{
    return Usable(actor) && IsA(actor, "ZActor") && SafeIsAlive(actor) == 1;
}

bool IsAlive(ZGEOM* actor)
{
    if (!Usable(actor))
        return false;
    if (IsA(actor, "ZHITMAN2")) {  // not a ZActor: its hit points
        const char* hp = reinterpret_cast<const char*>(actor) + kHitman_Health;
        return IsReadable(hp, 4) && *reinterpret_cast<const float*>(hp) > 0.0f;
    }
    if (!IsA(actor, "ZActor"))
        return false;
    return SafeIsAlive(actor) == 1;
}

bool KillActor(ZGEOM* actor)
{
    if (!ActorAlive(actor))
        return false;
    // a behaviour dies its own way (as from an explosion); without one the
    // message goes nowhere: Die itself
    if (ScriptInstance(actor)) {
        static const char* param = kInstantDeath;
        return SendMessage(actor, actor, RegisterMessage("ScriptEvent"), &param);
    }
    return SafeDie(actor);
}

bool ActorWalkTo(ZGEOM* actor, const float pos[3])
{
    return ActorAlive(actor) && SafeWalkTo(actor, pos);
}

bool ActorLookAt(ZGEOM* actor, ZGEOM* target)
{
    uint32_t ref = target && Usable(target) ? RefOf(target) : 0;
    if (target && !ref)
        return false;
    return ActorAlive(actor) && SafeLookAt(actor, ref);  // no target: stops looking
}

bool ActorUseMoveSet(ZGEOM* actor, const char* name)
{
    return name && *name && ActorAlive(actor) && SafeUseMoveSet(actor, name);
}

bool ActorPlayAnimation(ZGEOM* actor, const char* name, bool loop)
{
    if (!name || !*name || !ActorAlive(actor))
        return false;
    void* anim = SafeFindAnim(actor, name);
    return anim && SafePlayAnim(actor, anim, loop ? 5 : 1);
}

ZGEOM* Player()
{
    // asked for often (zones, events, every frame): kept while the scene's
    // records and the object stay the same
    static ZGEOM* hitman;
    static const SceneRecord* records;
    static size_t count;
    const std::vector<SceneRecord>& all = SceneRecords();
    if (hitman && all.data() == records && all.size() == count && Usable(hitman) && IsA(hitman, "ZHITMAN2"))
        return hitman;
    hitman = FirstRecord([](const SceneRecord& r) { return r.classId == kHitmanClass; });
    records = all.data();
    count = all.size();
    return hitman;
}

// The loaded map's network (null: none) and how much the pathfinder lifts a
// character's point before looking it up.
static void* Network(float& lift)
{
    // looked up again when the scene's records change (another map, loaded again)
    static ZGEOM* pf;
    static const SceneRecord* records;
    static size_t count;
    static uint32_t ref;
    if (!SceneDataValid())
        return nullptr;
    const std::vector<SceneRecord>& all = SceneRecords();
    if (all.data() != records || all.size() != count || (pf && (!Usable(pf) || GeomOfRef(ref) != pf))) {
        pf = FirstRecord([](const SceneRecord& r) { return r.classId == kPathfinderClass; });
        ref = pf ? RefOf(pf) : 0;
        records = all.data();
        count = all.size();
    }
    const char* base = reinterpret_cast<const char*>(pf);
    if (!pf || !IsReadable(base + kPathfinder_YOffset, kPathfinder_Nav + 4 - kPathfinder_YOffset))
        return nullptr;
    void* nav = *reinterpret_cast<void* const*>(base + kPathfinder_Nav);
    if (!nav || !IsReadable(nav, kNav_CellsY + 4))
        return nullptr;
    lift = *reinterpret_cast<const float*>(base + kPathfinder_YOffset) + kPathfinder_YLift;
    if (!std::isfinite(lift) || std::fabs(lift) > 1000.0f)
        lift = kPathfinder_YLift;
    return nav;
}

int OnPathNetwork(const float pos[3])
{
    float lift;
    void* nav = Network(lift);
    if (!nav)
        return -1;
    float p[3] = {pos[0], pos[1] + lift, pos[2]};
    return SafeOnNav(nav, p);
}

bool PathNetworkGrid(PathGrid& grid)
{
    float lift;
    const char* nav = static_cast<const char*>(Network(lift));
    if (!nav)
        return false;
    memcpy(grid.min, nav + kNav_Min, sizeof(grid.min));
    memcpy(grid.max, nav + kNav_Max, sizeof(grid.max));
    memcpy(&grid.cells[0], nav + kNav_CellsX, 4);
    memcpy(&grid.cells[1], nav + kNav_CellsY, 4);
    memcpy(&grid.cells[2], nav + kNav_CellsZ, 4);
    grid.lift = lift;
    for (int i = 0; i < 3; ++i) {
        if (grid.cells[i] <= 0 || grid.cells[i] > 0x10000 || !std::isfinite(grid.min[i]) ||  // u16 cells (C1-1: 65536 x 8192 x 65536 of 25 cm)
            !std::isfinite(grid.max[i]) || grid.max[i] <= grid.min[i])
            return false;
        grid.size[i] = (grid.max[i] - grid.min[i]) / float(grid.cells[i]);
    }
    return true;
}

bool PathCellBelow(const PathGrid& grid, const float pos[3], float floor[3])
{
    float lift;
    void* nav = Network(lift);
    if (!nav)
        return false;
    float p[3] = {pos[0], pos[1] + grid.lift, pos[2]};
    int c[3];  // world axes
    for (int i = 0; i < 3; ++i) {
        float f = (p[i] - grid.min[i]) / grid.size[i];
        if (!(f >= 0.0f) || f >= float(grid.cells[i]))
            return false;
        c[i] = int(f);
    }
    const uint16_t in[3] = {uint16_t(c[0]), uint16_t(c[2]), uint16_t(c[1])};  // the network's x, z, y
    uint16_t out[3] = {};
    if (!SafeNearestCell(nav, out, in))
        return false;
    const int found[3] = {out[0], out[2], out[1]};
    for (int i = 0; i < 3; ++i)
        floor[i] = grid.min[i] + (float(found[i]) + 0.5f) * grid.size[i];
    floor[1] -= grid.lift;
    return true;
}

bool NearestWalkable(const float pos[3], float radius, float out[3])
{
    if (!std::isfinite(pos[0]) || !std::isfinite(pos[1]) || !std::isfinite(pos[2]) || !(radius > 0.0f))
        return false;
    if (OnPathNetwork(pos) == 1) {
        memcpy(out, pos, 3 * sizeof(float));
        return true;
    }
    PathGrid g;
    if (!PathNetworkGrid(g))
        return false;
    // a lookup finds a cell within one cell across: every second column is enough
    const float step[2] = {g.size[0] * 2.0f, g.size[2] * 2.0f};
    const int n[2] = {std::min(32, int(radius / step[0]) + 1), std::min(32, int(radius / step[1]) + 1)};
    float best = radius * radius;
    bool found = false;
    for (int i = -n[0]; i <= n[0]; ++i) {
        for (int k = -n[1]; k <= n[1]; ++k) {
            float p[3] = {pos[0] + float(i) * step[0], pos[1] + 60.0f, pos[2] + float(k) * step[1]};
            float f[3];
            if (!PathCellBelow(g, p, f) || f[1] > pos[1] + 60.0f || f[1] < pos[1] - 150.0f)
                continue;
            float dx = f[0] - pos[0], dz = f[2] - pos[2];
            float d = dx * dx + dz * dz;
            if (d >= best)
                continue;
            if (std::fabs(f[1] - pos[1]) < 30.0f)
                f[1] = pos[1];  // the same surface: where it was clicked, not the cell's middle
            if (OnPathNetwork(f) != 1)
                continue;
            best = d;
            memcpy(out, f, sizeof(f));
            found = true;
        }
    }
    return found;
}

// ZEngineDataBase::GetSceneCom 0x420A60, then its vfunc 0x88 (name, &ref)
static uint32_t SafeSceneComRef(void* engine, const char* name)
{
    __try {
        void* com = reinterpret_cast<void*(__thiscall*)(void*)>(0x420A60)(engine);
        if (!com)
            return 0;
        uint32_t ref = 0;
        VFunc<void(__thiscall*)(void*, const char*, uint32_t*)>(com, 0x88 / 4)(com, name, &ref);
        return ref;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

ZGEOM* SceneComGeom(const char* name)
{
    void* engine = Engine();
    if (!engine || !name || !SceneDataValid())
        return nullptr;
    uint32_t ref = SafeSceneComRef(engine, name);
    ZGEOM* g = ref ? GeomOfRef(ref) : nullptr;
    return Usable(g) ? g : nullptr;
}

bool HitmanChangeClothes(ZGEOM* source)
{
    ZGEOM* hitman = Player();
    if (!hitman || !Usable(source))
        return false;
    static ZGEOM* param[2];  // kept: the handler may look at it later in the frame
    param[0] = source;
    param[1] = nullptr;
    return SendMessage(hitman, hitman, RegisterMessage("MSG_CHANGECLOTHES"), param);
}

ZGEOM* LevelControl()
{
    return FirstRecord([](const SceneRecord& r) {
        if (r.classId != kLevelControlClass || !r.script)
            return false;
        std::string n;
        for (char c : r.name)
            n += char(std::tolower(static_cast<unsigned char>(c)));
        return n.rfind("levelcontrol", 0) == 0;
    });
}

bool AddUseAction(ZGEOM* geom, const char* text, uint16_t msg, int distance)
{
    if (!geom || !text || !msg || !IsReadable(geom, sizeof(ZGEOM)))
        return false;
    uint32_t ref = RefOf(geom);
    return ref && SafeAddAction(geom, text, msg, ref, distance);
}

} // namespace h2::engine
