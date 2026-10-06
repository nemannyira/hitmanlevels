#include "npc.h"

#include <windows.h>
#include <algorithm>
#include <cstring>

#include "../game.h"
#include "../hook.h"
#include "../log.h"
#include "../mainloop.h"
#include "../scene.h"
#include "imports.h"
#include "inject.h"
#include "lists.h"
#include "spawn.h"

namespace h2::engine {

namespace {

constexpr uint32_t kActorClass = 0x2003E9;            // ZTRACKLNKHM2
constexpr uintptr_t kAlloc = 0x530490;                // engine allocation (cdecl size)
constexpr uintptr_t kScriptDll = 0x6A6C74;            // ?g_pScriptDll@@3PAVZDllBase@@A
constexpr uintptr_t kDblkVtable = 0x65D81C;
constexpr uintptr_t kNode_SetMesh = 0x5B9690;
constexpr uintptr_t kNode_SetTransform = 0x5BB240;
constexpr uintptr_t kNode_SetFlags = 0x5BAC80;
constexpr uintptr_t kEngine_InitList = 0x5D13;        // two phase init entries (InitializeGeoms)
constexpr int kTable_Create = 0x08 / 4;
constexpr int kGeom_EnsureExtra = 0x44 / 4;           // makes geom+0x10 (script list at +0x1C)
constexpr int kGeom_SetWorldTransform = 0x80 / 4;
constexpr int kGeom_ReadData = 0x1B8 / 4;             // (DBLK*)
constexpr int kGeom_ReadData2 = 0x130 / 4;            // (DBLK*)
constexpr int kGeom_InitPhase = 0x1D4 / 4;
constexpr int kGeom_Init = 0x148 / 4;
constexpr int kGeom_PostInit = 0x14C / 4;             // starts the script
constexpr int kGeom_AddChild = 0x270 / 4;
constexpr int kScript_NewParams = 0x54 / 4;
constexpr int kScript_ParseParams = 0x48 / 4;         // (params, record + 4)
constexpr int kInitEntry_Phase = 0x50 / 4;            // (phase, 0, 0)
constexpr int kScript_Destroy = 0x68 / 4;             // (instance)
constexpr uintptr_t kGeom_CreateScript = 0x43CE30;    // ZGEOM: creates and starts the instance of its script entry
constexpr uint32_t kFlagInitialized = 0x800000;
// ZLIST_ActorCollision (event, vtable 0x649930; +0x6E its list, +0x72 / +0x76
// arrays of 0x11 / 4 bytes per element, made by vfunc 0x1C for the elements
// the list has then; vfunc 0x2C pushes apart the elements close to each other)
constexpr uintptr_t kActorCollision_Init = 0x489900;
constexpr uintptr_t kItem_SetOwner = 0x4723F0;        // ZITEM: (owner ref, its group, notify)
constexpr uint32_t kActorCollisionRoom = 64;            // elements added at runtime it has room for
const char* const kActorList = "TrackLinkObjects";

template <typename T>
T VFunc(void* obj, int index)
{
    return reinterpret_cast<T>((*reinterpret_cast<void***>(obj))[index]);
}

void* Alloc(size_t n) { return reinterpret_cast<void*(__cdecl*)(size_t)>(kAlloc)(n); }

struct Dblk {
    uintptr_t vtable;
    uint32_t size;  // block size - 4
    uint8_t* cur;
    uint8_t* start;
};

struct NpcArgs {
    void* table;
    char* engine;
    const char* name;
    ZGEOM* parent;
    uint32_t mesh, flags;
    float recMat[9], recPos[3];
    float mat[9], pos[3];
    uint8_t* data;     // engine allocated, remapped
    uint8_t* script;   // engine allocated, remapped
    void* before[256]; // init list entries before Init
    int beforeCount;
    ZGEOM* result;
    int step;
};

void* g_faultAddress;
DWORD g_faultCode;

int Filter(EXCEPTION_POINTERS* ep)
{
    g_faultCode = ep->ExceptionRecord->ExceptionCode;
    g_faultAddress = ep->ExceptionRecord->ExceptionAddress;
    return EXCEPTION_EXECUTE_HANDLER;
}

void* InitListHead(char* engine) { return *reinterpret_cast<void**>(engine + kEngine_InitList); }
void* InitListNext(void* e) { return *reinterpret_cast<void**>(static_cast<char*>(e) + 0x56); }

bool WasBefore(const NpcArgs& a, void* e)
{
    for (int i = 0; i < a.beforeCount; ++i)
        if (a.before[i] == e)
            return true;
    return false;
}

// The steps of ZEngineDataBase::CreateGeoms and InitializeGeoms for one
// actor record. Only POD here (SEH).
bool DoSpawn(NpcArgs& a)
{
    __try {
        a.step = 1;
        using CreateFn = ZBaseGeom*(__thiscall*)(void*, const char*, const char*, ZBaseGeom*, int);
        ZBaseGeom* node = VFunc<CreateFn>(a.table, kTable_Create)(a.table, a.name, "ZTRACKLNKHM2", nullptr, 0);
        if (!node || !node->geom)
            return false;
        ZGEOM* geom = node->geom;
        a.result = geom;

        a.step = 2;
        reinterpret_cast<void(__thiscall*)(ZBaseGeom*, uint32_t)>(kNode_SetMesh)(node, a.mesh);
        reinterpret_cast<void(__thiscall*)(ZBaseGeom*, const float*, const float*)>(kNode_SetTransform)(
            node, a.recMat, a.recPos);

        a.step = 3;
        uint32_t f = (a.flags & 0xFFFFF) | node->flags;
        if (f & 0x8080)
            f |= 0x8080;
        bool dynamic = (f & 0x40000) != 0;
        if (dynamic)
            f &= 0xFFFBBFFF;  // made dynamic below (MakeDynamicGeomsDynamic)
        reinterpret_cast<void(__thiscall*)(ZBaseGeom*, uint32_t, uint32_t)>(kNode_SetFlags)(node, f, ~f);
        a.step = 4;
        VFunc<void(__thiscall*)(ZGEOM*, ZBaseGeom*, int, int)>(a.parent, kGeom_AddChild)(a.parent, node, 0, 0);
        uint32_t now = node->flags;
        reinterpret_cast<void(__thiscall*)(ZBaseGeom*, uint32_t, uint32_t)>(kNode_SetFlags)(node, now, ~now);

        if (a.script) {
            a.step = 6;
            void* dll = *reinterpret_cast<void**>(kScriptDll);
            if (dll) {
                VFunc<void(__thiscall*)(ZGEOM*)>(geom, kGeom_EnsureExtra)(geom);
                char* extra = *reinterpret_cast<char**>(reinterpret_cast<char*>(geom) + 0x10);
                uint32_t** list = reinterpret_cast<uint32_t**>(extra + 0x1C);
                if (!*list) {
                    *list = static_cast<uint32_t*>(Alloc(8));
                    (*list)[0] = (*list)[1] = 0;
                }
                uint32_t* entry = static_cast<uint32_t*>(Alloc(16));
                memset(entry, 0, 16);
                char* name = static_cast<char*>(Alloc(0x200));
                strncpy(name, reinterpret_cast<char*>(a.script + 5), 0x1FF);
                name[0x1FF] = 0;
                entry[2] = reinterpret_cast<uint32_t>(name);
                void* params = VFunc<void*(__thiscall*)(void*)>(dll, kScript_NewParams)(dll);
                entry[3] = reinterpret_cast<uint32_t>(params);
                VFunc<void(__thiscall*)(void*, void*, uint8_t*)>(dll, kScript_ParseParams)(dll, params, a.script + 4);
                (*list)[1] = reinterpret_cast<uint32_t>(entry);
                (*list)[0] += 1;
            }
        }

        if (a.data) {
            a.step = 7;
            Dblk d;
            d.vtable = kDblkVtable;
            d.size = (*reinterpret_cast<uint32_t*>(a.data) & 0xFFFFFF) - 4;
            d.cur = d.start = a.data + 4;
            VFunc<void(__thiscall*)(ZGEOM*, Dblk*)>(geom, kGeom_ReadData)(geom, &d);
            VFunc<void(__thiscall*)(ZGEOM*, Dblk*)>(geom, kGeom_ReadData2)(geom, &d);
        }

        a.step = 8;
        if (dynamic)
            reinterpret_cast<void(__thiscall*)(ZBaseGeom*, bool)>(game::ZBaseGeom_SetDynamic)(node, true);

        a.step = 9;
        a.beforeCount = 0;
        for (void* e = InitListHead(a.engine); e && a.beforeCount < 256; e = InitListNext(e))
            a.before[a.beforeCount++] = e;
        VFunc<void(__thiscall*)(ZGEOM*)>(geom, kGeom_Init)(geom);

        a.step = 10;  // as InitializeGeoms, for the entries the actor added
        for (int phase = 1; phase <= 2; ++phase) {
            for (void* e = InitListHead(a.engine); e; e = InitListNext(e)) {
                if (WasBefore(a, e))
                    continue;
                uint32_t eflags = *reinterpret_cast<uint32_t*>(static_cast<char*>(e) + 0x20);
                if (!(eflags & phase))
                    continue;
                uint32_t ref = *reinterpret_cast<uint32_t*>(static_cast<char*>(e) + 0x48);
                ZGEOM* g = GeomOfRef(ref);
                if (!g)
                    continue;
                if ((eflags & 0x200) || !((g->node->flags >> 10) & 1))
                    VFunc<void(__thiscall*)(void*, int, int, int)>(e, kInitEntry_Phase)(e, phase, 0, 0);
                if (phase == 1)
                    VFunc<void(__thiscall*)(ZGEOM*)>(g, kGeom_InitPhase)(g);
            }
        }

        a.step = 11;
        reinterpret_cast<void(__thiscall*)(ZBaseGeom*, uint32_t, uint32_t)>(kNode_SetFlags)(node, kFlagInitialized, 0);
        VFunc<void(__thiscall*)(ZGEOM*)>(geom, kGeom_PostInit)(geom);
        a.step = 12;
        return true;
    } __except (Filter(GetExceptionInformation())) {
        return false;
    }
}

// How many actors reference each record from their data block (per-actor
// helpers such as ZAIMTHINGY are referenced by one actor only).
const std::vector<int>& ActorRefCounts()
{
    static std::vector<int> counts;
    static const void* builtFor;
    static size_t builtSize;
    const auto& records = SceneRecords();
    if (builtFor == records.data() && builtSize == records.size())
        return counts;
    builtFor = records.data();
    builtSize = records.size();
    counts.assign(records.size(), 0);
    for (const SceneRecord& r : records) {
        if (r.classId != kActorClass || !r.data)
            continue;
        std::vector<uint8_t> raw = RawBlock(r.data);
        for (size_t off : BlockRefOffsets(raw)) {
            uint32_t v;
            memcpy(&v, &raw[off], 4);
            uint32_t idx = v & 0x7FFFFFFF;
            if (idx && idx <= counts.size())
                counts[idx - 1]++;
        }
    }
    return counts;
}

// Data block of a new actor: the remapped block of the record it is made
// from, with its own copies of the original's per-actor helper objects.
std::vector<uint8_t> ActorDataBlock(const SceneRecord& r, const std::string& name)
{
    std::vector<uint8_t> raw = RawBlock(r.data);
    std::vector<uint8_t> block = RemappedBlock(r.data);
    if (raw.size() != block.size())
        return block;
    const auto& records = SceneRecords();
    const auto& counts = ActorRefCounts();
    for (size_t off : BlockRefOffsets(raw)) {
        uint32_t v;
        memcpy(&v, &raw[off], 4);
        uint32_t idx = v & 0x7FFFFFFF;
        if (!idx || idx > records.size() || counts[idx - 1] != 1)
            continue;
        const SceneRecord& target = records[idx - 1];
        if (target.classId != 0x200002 || target.mesh || target.script || target.data)
            continue;  // only plain helpers are copied
        ZGEOM* original = RecordGeom(int(idx - 1));
        ZGEOM* parent = original && original->node && original->node->parent ? original->node->parent->geom : nullptr;
        float mat[9], pos[3];
        memcpy(mat, target.mat, sizeof(mat));
        memcpy(pos, target.pos, sizeof(pos));
        std::string err;
        ZGEOM* copy = CreateHelper(target.name, parent, target.flags, mat, pos, &err);
        if (!copy) {
            Log("npc '%s': own copy of '%s' failed (%s), sharing the original's", name.c_str(), target.name.c_str(),
                err.c_str());
            continue;
        }
        uint32_t ref = RefOf(copy);
        memcpy(&block[off], &ref, 4);
        Log("npc '%s': own '%s' %p", name.c_str(), target.name.c_str(), static_cast<void*>(copy));
    }
    return block;
}

struct ScriptArgs {
    ZGEOM* geom;
    uint8_t* block;  // null: only stop
    int step;
};

bool DoSetScript(ScriptArgs& a)
{
    __try {
        a.step = 1;
        void* dll = *reinterpret_cast<void**>(kScriptDll);
        if (!dll)
            return false;
        ZGEOM* geom = a.geom;
        VFunc<void(__thiscall*)(ZGEOM*)>(geom, kGeom_EnsureExtra)(geom);
        char* extra = *reinterpret_cast<char**>(reinterpret_cast<char*>(geom) + 0x10);
        if (!extra)
            return false;
        uint32_t** list = reinterpret_cast<uint32_t**>(extra + 0x1C);
        if (!*list) {
            *list = static_cast<uint32_t*>(Alloc(8));
            (*list)[0] = (*list)[1] = 0;
        }
        uint32_t* entry = reinterpret_cast<uint32_t*>((*list)[1]);
        a.step = 2;
        if (entry && entry[1]) {  // the running instance
            VFunc<void(__thiscall*)(void*, uint32_t)>(dll, kScript_Destroy)(dll, entry[1]);
            entry[1] = 0;
        }
        if (!a.block)
            return true;
        a.step = 3;
        if (!entry) {
            entry = static_cast<uint32_t*>(Alloc(16));
            memset(entry, 0, 16);
            (*list)[1] = reinterpret_cast<uint32_t>(entry);
            (*list)[0] += 1;
        }
        char* name = static_cast<char*>(Alloc(0x200));
        strncpy(name, reinterpret_cast<char*>(a.block + 5), 0x1FF);
        name[0x1FF] = 0;
        entry[2] = reinterpret_cast<uint32_t>(name);
        void* params = VFunc<void*(__thiscall*)(void*)>(dll, kScript_NewParams)(dll);
        entry[3] = reinterpret_cast<uint32_t>(params);
        VFunc<void(__thiscall*)(void*, void*, uint8_t*)>(dll, kScript_ParseParams)(dll, params, a.block + 4);
        a.step = 4;
        reinterpret_cast<void(__thiscall*)(ZGEOM*)>(kGeom_CreateScript)(geom);
        a.step = 5;
        return entry[1] != 0;
    } __except (Filter(GetExceptionInformation())) {
        return false;
    }
}

uint8_t* EngineCopy(const std::vector<uint8_t>& b)
{
    if (b.empty())
        return nullptr;
    auto* p = static_cast<uint8_t*>(Alloc(b.size() + 4));
    memcpy(p, b.data(), b.size());
    p[b.size()] = 0xFF;  // end marker in case the block has none
    return p;
}

} // namespace

std::vector<int> ActorRecords()
{
    std::vector<int> out;
    for (const SceneRecord& r : SceneRecords())
        if (r.classId == kActorClass && !IsSceneNpcRecord(r.index))  // the level's characters are not the scene's
            out.push_back(r.index);
    return out;
}

std::string ActorModelName(const SceneRecord& actor)
{
    std::vector<uint8_t> b = RemappedBlock(actor.data);
    // first plain string value after the animation file
    for (size_t i = 4; i + 1 < b.size();) {
        uint8_t tag = b[i] & 0x3F;
        if (tag == 4 || tag == 5) {
            const char* s = reinterpret_cast<const char*>(&b[i + 1]);
            size_t n = strnlen(s, b.size() - i - 1);
            std::string str(s, n);
            if (!str.empty() && str.find('\\') == std::string::npos && str.find('.') == std::string::npos)
                return str;
            i += n + 2;
        } else if (tag == 1) {
            i += 9;
        } else if (tag == 2 || tag == 3 || tag == 8 || tag == 10 || tag == 11) {
            i += 5;
        } else if (b[i] == 0xFF) {
            break;
        } else {
            i += 1;
        }
    }
    return {};
}

namespace {

bool DoStateName(void* instance, char* out, size_t n)
{
    __try {
        const char* s = reinterpret_cast<const char*>(static_cast<uint32_t*>(instance)[0x1C]);
        if (!s)
            return false;
        strncpy(out, s, n - 1);
        out[n - 1] = 0;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

struct VarArgs {
    void* instance;
    const char* name;
    uint32_t bits;
    bool found;
    bool read;  // read instead of write
};

bool DoSetVariable(VarArgs& a)
{
    __try {
        auto inst = static_cast<uint32_t*>(a.instance);
        auto cls = reinterpret_cast<char*>(inst[0x21] - 0x3C);
        auto imports = *reinterpret_cast<uint32_t**>(cls + 0x30);
        if (!imports)
            return false;
        auto entries = reinterpret_cast<uint32_t**>(imports[0]);
        uint32_t count = imports[2];
        char* data = reinterpret_cast<char*>(inst[0x23]);
        for (uint32_t i = 0; i < count && i < 1024; ++i) {
            uint32_t* e = entries[i];
            if (!e || !e[5] || strcmp(reinterpret_cast<const char*>(e[5]), a.name) != 0)
                continue;
            if (e[0] != 0x14 && e[0] != 0x24)
                return false;  // strings and user types are not written here
            if (a.read)
                a.bits = *reinterpret_cast<uint32_t*>(data + e[2]);
            else
                *reinterpret_cast<uint32_t*>(data + e[2]) = a.bits;
            a.found = true;
            return true;
        }
        return true;
    } __except (Filter(GetExceptionInformation())) {
        return false;
    }
}

void* DoScriptInstance(ZGEOM* geom)
{
    __try {
        char* extra = *reinterpret_cast<char**>(reinterpret_cast<char*>(geom) + 0x10);
        if (!extra)
            return nullptr;
        uint32_t* list = *reinterpret_cast<uint32_t**>(extra + 0x1C);
        if (!list || !list[1])
            return nullptr;
        return reinterpret_cast<void*>(reinterpret_cast<uint32_t*>(list[1])[1]);
    } __except (Filter(GetExceptionInformation())) {
        return nullptr;
    }
}

// Characters kept where they were put while their behaviour starts (KeepStartPlace).
struct StartGuard {
    ZGEOM* actor;
    float home[3], mat[9];  // where it was put
    float last[3];          // where it was the frame before
    ULONGLONG until;        // GetTickCount64
    int frames = 0;
    int putBack = 0;
};
std::vector<StartGuard> g_guards;
constexpr float kJump = 250.0f;     // further in one frame: put there, not walked or pushed
constexpr int kGuardFrames = 120;   // at least this many frames of the game
constexpr int kMaxPutBack = 10;     // a behaviour that keeps putting it elsewhere wins

bool ActorAt(ZGEOM* actor, float mat[9], float pos[3])
{
    if (!actor || !IsReadable(actor, 0x588) || !IsA(actor, "ZActor"))
        return false;
    game::GetWorldTransform(actor, mat, pos);
    return true;
}

void GuardFrame()
{
    static ULONGLONG last;
    ULONGLONG now = GetTickCount64(), passed = last ? now - last : 0;
    last = now;
    if (g_guards.empty())
        return;
    // the scene paused (the editor's edit mode, the game's menu): its time does not count
    char* sys = *reinterpret_cast<char**>(game::g_pSysInterface);
    char* engine = sys ? *reinterpret_cast<char**>(sys + 0x98) : nullptr;
    if (engine && engine[0xB9C]) {
        for (StartGuard& g : g_guards)
            g.until += passed;
        return;
    }
    for (size_t i = 0; i < g_guards.size();) {
        StartGuard& g = g_guards[i];
        float mat[9], pos[3];
        if (!ActorAt(g.actor, mat, pos) || (++g.frames > kGuardFrames && now > g.until)) {
            g_guards.erase(g_guards.begin() + ptrdiff_t(i));
            continue;
        }
        float dx = pos[0] - g.last[0], dy = pos[1] - g.last[1], dz = pos[2] - g.last[2];
        float hx = pos[0] - g.home[0], hz = pos[2] - g.home[2];
        bool away = g.frames == 1 && hx * hx + hz * hz > kJump * kJump;  // put elsewhere while it was made
        if ((away || dx * dx + dy * dy + dz * dz > kJump * kJump) && g.putBack >= kMaxPutBack) {
            Log("npc %p: its behaviour keeps putting it elsewhere: left there", static_cast<void*>(g.actor));
            g_guards.erase(g_guards.begin() + ptrdiff_t(i));
            continue;
        }
        if (away || dx * dx + dy * dy + dz * dz > kJump * kJump) {
            ++g.putBack;
            Log("npc %p: its behaviour put it at (%.0f %.0f %.0f): back where it was put", static_cast<void*>(g.actor),
                pos[0], pos[1], pos[2]);
            float home[3], homeMat[9];  // (TeleportActor writes them into g too)
            memcpy(home, g.home, sizeof(home));
            memcpy(homeMat, g.mat, sizeof(homeMat));
            TeleportActor(g.actor, home, homeMat);
        } else {
            memcpy(g.last, pos, sizeof(pos));
        }
        ++i;
    }
}

} // namespace

void KeepStartPlace(ZGEOM* actor, const float pos[3], const float mat[9], float seconds)
{
    StartGuard g{actor};
    if (!ActorAt(actor, g.mat, g.last))  // g.last: where it is now (already moved, the first frame tells)
        return;
    memcpy(g.home, pos, sizeof(g.home));
    memcpy(g.mat, mat, sizeof(g.mat));
    g.until = GetTickCount64() + ULONGLONG(seconds * 1000.0f);
    for (StartGuard& had : g_guards)
        if (had.actor == actor) {
            had = g;
            return;
        }
    g_guards.push_back(g);
}

void* ScriptInstance(ZGEOM* geom)
{
    if (!geom || !IsReadable(geom, sizeof(ZGEOM)))
        return nullptr;
    return DoScriptInstance(geom);
}

int GetScriptVariables(ZGEOM* geom, ScriptInfo& values)
{
    void* inst = ScriptInstance(geom);
    if (!inst)
        return 0;
    int n = 0;
    for (ScriptParam& p : values.params) {
        if (p.type != ParamType::Long && p.type != ParamType::Float && p.type != ParamType::Ref)
            continue;
        VarArgs a{inst, p.name.c_str(), 0, false, true};
        if (DoSetVariable(a) && a.found) {
            p.bits = a.bits;
            ++n;
        }
    }
    return n;
}

std::string ScriptState(ZGEOM* geom)
{
    void* inst = ScriptInstance(geom);
    char buf[128];
    if (!inst || !DoStateName(inst, buf, sizeof(buf)))
        return {};
    return buf;
}

int SetScriptVariables(ZGEOM* geom, const ScriptInfo& values, std::string* error)
{
    void* inst = ScriptInstance(geom);
    if (!inst) {
        if (error)
            *error = "no running behaviour";
        return 0;
    }
    int written = 0;
    for (const ScriptParam& p : values.params) {
        if (p.type != ParamType::Long && p.type != ParamType::Float && p.type != ParamType::Ref)
            continue;
        VarArgs a{inst, p.name.c_str(), p.bits, false, false};
        if (!DoSetVariable(a)) {
            if (error)
                *error = "cannot write " + p.name;
            continue;
        }
        written += a.found;
    }
    return written;
}

bool TeleportActor(ZGEOM* actor, const float pos[3], const float* newMat)
{
    constexpr size_t kActorPos = 0x57C;
    if (!actor || !IsReadable(actor, kActorPos + 12) || !IsA(actor, "ZActor"))
        return false;
    auto p = reinterpret_cast<float*>(reinterpret_cast<char*>(actor) + kActorPos);
    float mat[9], cur[3];
    game::GetWorldTransform(actor, mat, cur);
    float above = p[1] - cur[1];  // the actor's point is a little above its node
    if (!(above > -500.0f && above < 500.0f))
        above = 0.0f;
    p[0] = pos[0];
    p[1] = pos[1] + above;
    p[2] = pos[2];
    game::SetWorldTransform(actor, newMat ? newMat : mat, pos);
    for (StartGuard& g : g_guards)  // moved by the editor: kept there
        if (g.actor == actor) {
            memcpy(g.home, pos, sizeof(g.home));
            memcpy(g.last, pos, sizeof(g.last));
            if (newMat)
                memcpy(g.mat, newMat, sizeof(g.mat));
        }
    return true;
}

bool DestroyNpc(ZGEOM* actor, std::string* error)
{
    if (!actor || !IsReadable(actor, sizeof(ZGEOM)) || !IsReadable(actor->node, sizeof(ZBaseGeom))) {
        if (error)
            *error = "no such character";
        return false;
    }
    ZBaseGeom* parent = actor->node->parent;
    ZGEOM* group = parent && IsReadable(parent, sizeof(ZBaseGeom)) ? parent->geom : nullptr;
    bool ok = DestroyGeom(actor);
    if (ok && group && IsA(group, "ZGROUP") && SafeString(group->name).find("_Group") != std::string::npos)
        DestroyGeom(group);
    if (!ok && error)
        *error = "the engine could not remove it";
    return ok;
}

bool SetScript(ZGEOM* geom, const std::vector<uint8_t>* block, std::string* error)
{
    if (!geom || !IsReadable(geom, sizeof(ZGEOM))) {
        if (error)
            *error = "no such object";
        return false;
    }
    ScriptArgs a{};
    a.geom = geom;
    a.block = block && !block->empty() ? EngineCopy(*block) : nullptr;
    bool ok;
    {
        game::RefAllocUnlock unlock;
        ok = DoSetScript(a);
    }
    if (!ok) {
        char buf[160];
        snprintf(buf, sizeof(buf), "script change failed (step %d, exception %08lX at %p)", a.step, g_faultCode,
                 g_faultAddress);
        if (error)
            *error = buf;
        Log("npc %p: %s", static_cast<void*>(geom), buf);
        return false;
    }
    Log("npc %p: script %s", static_cast<void*>(geom),
        a.block ? reinterpret_cast<const char*>(a.block + 5) : "(stopped)");
    return true;
}

namespace {

using InitFn = void(__fastcall*)(char* self, void* edx);
InitFn oActorCollisionInit;

// as the game's, with room for the characters the editor adds
void __fastcall hkActorCollisionInit(char* self, void* edx)
{
    __try {
        char* list = *reinterpret_cast<char**>(self + 0x6E);
        void* container = *reinterpret_cast<void**>(list + 0x18);
        uint32_t n = VFunc<uint32_t(__thiscall*)(void*)>(container, 0x14 / 4)(container) + kActorCollisionRoom;
        *reinterpret_cast<void**>(self + 0x72) = Alloc(n * 0x11);
        *reinterpret_cast<void**>(self + 0x76) = Alloc(n * 4);
        Log("npc: actor collision %p of list %p: room for %u", static_cast<void*>(self), static_cast<void*>(list), n);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        oActorCollisionInit(self, edx);
    }
}

// A placed copy of a character runs the original's behaviour, and some
// behaviours take an item of the level (C1-1 PostMan: its Flowers) that the
// original already has. An item held (node flag 0x200000) going to another
// owner is a stop of the game (int3): the item stays with its owner instead.
using SetOwnerFn = uint32_t(__fastcall*)(char* item, void* edx, uint32_t owner, void* group, char notify);
SetOwnerFn oItemSetOwner;

bool ItemHeld(char* item, uint32_t& owner)
{
    __try {
        owner = *reinterpret_cast<uint32_t*>(item + 0x64);
        return (reinterpret_cast<ZGEOM*>(item)->node->flags & 0x200000) != 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

uint32_t __fastcall hkItemSetOwner(char* item, void* edx, uint32_t owner, void* group, char notify)
{
    uint32_t had = 0;
    if (ItemHeld(item, had) && had != owner) {
        Log("npc: item %s is %08X's, not given to %08X", SafeString(reinterpret_cast<ZGEOM*>(item)->name).c_str(), had,
            owner);
        return had;
    }
    return oItemSetOwner(item, edx, owner, group, notify);
}

// the scene's actor lists (one in each scene: AllLevels_AnimSound)
std::vector<ZGEOM*> ActorLists()
{
    std::vector<ZGEOM*> out;
    const auto& records = SceneRecords();
    for (size_t i = 0; i < records.size(); ++i)
        if (records[i].name == kActorList)
            if (ZGEOM* g = RecordGeom(int(i)); g && IsEditableList(g))
                out.push_back(g);
    return out;
}

bool CalcActorBounds(ZGEOM* actor)
{
    __try {
        VFunc<void(__thiscall*)(ZGEOM*)>(actor, 0)(actor);  // ZActor: its box (0x47A380)
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

} // namespace

bool SetActorCollides(ZGEOM* actor, bool on)
{
    if (!actor)
        return false;
    if (on)
        CalcActorBounds(actor);  // made at runtime: no bounds, nothing finds it
    uint32_t ref = RefOf(actor);
    bool changed = false;
    for (ZGEOM* list : ActorLists()) {
        std::vector<uint32_t> refs = ListRefs(list);
        auto it = std::find(refs.begin(), refs.end(), ref);
        if (on == (it != refs.end()))
            continue;
        if (on)
            refs.push_back(ref);
        else
            refs.erase(it);
        std::string err;
        if (SetListRefs(list, refs, &err))
            changed = true;
        else
            Log("npc: %s: %s", kActorList, err.c_str());
    }
    return changed;
}

void InstallNpc()
{
    Hook("ZLIST_ActorCollision init", kActorCollision_Init, &hkActorCollisionInit, &oActorCollisionInit);
    Hook("ZITEM::SetOwner", kItem_SetOwner, &hkItemSetOwner, &oItemSetOwner);
    OnFrameEnd(GuardFrame);
    OnSceneLeaving([](const std::string&) { g_guards.clear(); });
}

ZGEOM* SpawnNpc(const NpcSpawn& s, std::string* error)
{
    auto fail = [&](const std::string& why) -> ZGEOM* {
        if (error)
            *error = why;
        Log("npc: %s", why.c_str());
        return nullptr;
    };
    const auto& records = SceneRecords();
    if (s.record < 0 || size_t(s.record) >= records.size())
        return fail("no such actor record (scene data not captured?)");
    const SceneRecord& r = records[s.record];
    if (r.classId != kActorClass)
        return fail("record is not an actor");
    void* table = GeomTable();
    auto sys = reinterpret_cast<char*>(game::SysInterface());
    char* engine = sys ? *reinterpret_cast<char**>(sys + 0x98) : nullptr;
    if (!table || !engine)
        return fail("no scene loaded");
    if (!s.parent || !IsReadable(s.parent, sizeof(ZGEOM)))
        return fail("no parent group");
    // Like the scene's NPCs: a group placed in the world holds the actor at its
    // own (local) spot; the actor moves itself within the group's space.
    std::string groupName = s.name + "_Group";
    std::string err;
    ZGEOM* group = CreateHelper(groupName, s.parent, 0x4082, s.mat, s.pos, &err, "ZGROUP");
    if (!group)
        return fail("group: " + err);
    // the scene's NPC groups are dynamic (flag 0x40000): they move
    reinterpret_cast<void(__thiscall*)(ZBaseGeom*, bool)>(game::ZBaseGeom_SetDynamic)(group->node, true);

    NpcArgs a{};
    a.table = table;
    a.engine = engine;
    a.name = s.name.c_str();
    a.parent = group;
    a.mesh = s.mesh ? s.mesh : r.mesh;
    a.flags = r.flags;
    memcpy(a.recMat, r.mat, sizeof(a.recMat));
    memcpy(a.recPos, r.pos, sizeof(a.recPos));
    memcpy(a.mat, s.mat, sizeof(a.mat));
    memcpy(a.pos, s.pos, sizeof(a.pos));
    std::vector<uint8_t> block = ActorDataBlock(r, s.name);
    if (!s.refSwap.empty() && !block.empty())
        for (size_t at : BlockRefOffsets(block)) {
            uint32_t v;
            memcpy(&v, &block[at], 4);
            auto it = s.refSwap.find(v);
            if (it != s.refSwap.end())
                memcpy(&block[at], &it->second, 4);
        }
    // a body of another level: its own animation sets (they come with it)
    if (std::vector<std::string> anims = ImportedAnimFiles(s.mesh); !anims.empty() && !block.empty())
        block = WithStrings(block, anims);
    a.data = EngineCopy(block);  // kept: the actor may point into it
    a.script = s.script.empty() ? nullptr : EngineCopy(s.script);
    Log("npc: spawning '%s' from record %d (%s, mesh %08X, flags %08X, data %zu bytes, script %zu bytes)",
        s.name.c_str(), s.record, r.name.c_str(), r.mesh, r.flags, a.data ? size_t(*reinterpret_cast<uint32_t*>(a.data) & 0xFFFFFF) : 0,
        s.script.size());
    bool ok;
    {
        game::RefAllocUnlock unlock;
        ok = DoSpawn(a);
    }
    if (!ok) {
        char buf[160];
        snprintf(buf, sizeof(buf), "engine call failed (step %d, exception %08lX at %p)", a.step, g_faultCode,
                 g_faultAddress);
        DestroyGeom(group);
        return fail(buf);
    }
    Log("npc: spawned '%s' geom %p node %p", s.name.c_str(), static_cast<void*>(a.result),
        static_cast<void*>(a.result->node));
    if (s.collides)
        SetActorCollides(a.result, true);
    return a.result;
}

} // namespace h2::engine
