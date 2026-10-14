#include "spawn.h"

#include <windows.h>

#include "../game.h"
#include "../log.h"
#include "npc.h"

namespace h2::engine {

namespace {

constexpr int kTable_Create = 0x08 / 4;         // ZBaseGeom* (name, className, node, extra)
constexpr uintptr_t kNode_SetMesh = 0x5B9690;   // (uint32 mesh)
constexpr uintptr_t kNode_SetTransform = 0x5BB240;  // (float mat[9], float pos[3])
constexpr uintptr_t kNode_SetFlags = 0x5BAC80;  // (uint32 set, uint32 clear)
constexpr uintptr_t kNode_CalcBounds = 0x5BA7C0;  // (bool)
constexpr int kGeom_AddChild = 0x270 / 4;       // (ZBaseGeom* child, 0, 0), groups
constexpr int kGeom_Init = 0x148 / 4;
constexpr int kGeom_PostInit = 0x14C / 4;
constexpr int kGeom_DeletingDtor = 0x1FC / 4;   // (int flags), 1 = free
constexpr uint32_t kFlagInitialized = 0x800000;  // set by InitializeGeoms between Init and PostInit
// Node flags (low 20 bits, as CreateGeoms takes them from the record) are
// copied except the ones that give the node a role in an engine system the
// clone has no data for (meaning found over the meshed objects of the .GMS
// files):
//   0x02000 sound/sight helpers   0x04000 fragments   0x10000 lights
//   0x40000 dynamic (set separately)                  0x80000 doors (crash)
// 0x08000 must stay: with it the node gets its render instance (SetFlags asks
// the manager at engine+0x5D1F for a handle into +0x6C); without it the clone
// is invisible.
constexpr uint32_t kCopyFlags = 0xFFFFF & ~(0x02000 | 0x04000 | 0x10000 | 0x40000 | 0x80000);

template <typename T>
T VFunc(void* obj, int index)
{
    return reinterpret_cast<T>((*reinterpret_cast<void***>(obj))[index]);
}

struct CloneArgs {
    void* table;
    const char* name;
    ZBaseGeom* parent;
    ZGEOM* parentGeom;
    uint32_t mesh;
    uint32_t flags;
    float mat[9];
    float pos[3];
    bool dynamic;
    ZBaseGeom* result;
    int step;  // how far it got, for the log
};

void* g_faultAddress;
DWORD g_faultCode;

int Filter(EXCEPTION_POINTERS* ep)
{
    g_faultCode = ep->ExceptionRecord->ExceptionCode;
    g_faultAddress = ep->ExceptionRecord->ExceptionAddress;
    return EXCEPTION_EXECUTE_HANDLER;
}

// Only POD here: SEH and C++ unwinding do not mix.
bool DoClone(CloneArgs& a)
{
    __try {
        a.step = 1;
        using CreateFn = ZBaseGeom*(__thiscall*)(void*, const char*, const char*, ZBaseGeom*, int);
        ZBaseGeom* node = VFunc<CreateFn>(a.table, kTable_Create)(a.table, a.name, "ZSTDOBJ", nullptr, 0);
        if (!node || !node->geom)
            return false;
        a.result = node;
        ZGEOM* geom = node->geom;

        a.step = 2;
        reinterpret_cast<void(__thiscall*)(ZBaseGeom*, uint32_t)>(kNode_SetMesh)(node, a.mesh);
        a.step = 3;
        reinterpret_cast<void(__thiscall*)(ZBaseGeom*, const float*, const float*)>(kNode_SetTransform)(
            node, a.mat, a.pos);
        a.step = 4;
        uint32_t f = node->flags | a.flags;
        reinterpret_cast<void(__thiscall*)(ZBaseGeom*, uint32_t, uint32_t)>(kNode_SetFlags)(node, f, ~f);
        a.step = 5;
        if (a.parentGeom)
            VFunc<void(__thiscall*)(ZGEOM*, ZBaseGeom*, int, int)>(a.parentGeom, kGeom_AddChild)(a.parentGeom, node,
                                                                                               0, 0);
        a.step = 6;
        reinterpret_cast<void(__thiscall*)(ZBaseGeom*, bool)>(kNode_CalcBounds)(node, false);
        a.step = 7;
        VFunc<void(__thiscall*)(ZGEOM*)>(geom, kGeom_Init)(geom);
        reinterpret_cast<void(__thiscall*)(ZBaseGeom*, uint32_t, uint32_t)>(kNode_SetFlags)(node, kFlagInitialized, 0);
        VFunc<void(__thiscall*)(ZGEOM*)>(geom, kGeom_PostInit)(geom);
        a.step = 8;
        // moving objects are tracked by the dynamic set (collision, culling)
        if (a.dynamic)
            reinterpret_cast<void(__thiscall*)(ZBaseGeom*, bool)>(game::ZBaseGeom_SetDynamic)(node, true);
        a.step = 9;
        return true;
    } __except (Filter(GetExceptionInformation())) {
        return false;
    }
}

struct HelperArgs {
    void* table;
    const char* name;
    const char* className;
    ZGEOM* parent;
    uint32_t flags;
    float mat[9];
    float pos[3];
    uint32_t mesh;   // 0: none
    uint8_t* data;   // typed data block read by the object (null: none)
    bool noInit;     // initialised later (InitObject), e.g. after its children were made
    ZBaseGeom* result;
    int step;
};

constexpr uintptr_t kDblkVtable = 0x65D81C;
constexpr int kGeom_ReadData = 0x1B8 / 4;   // (DBLK*)
constexpr int kGeom_ReadData2 = 0x130 / 4;  // (DBLK*)
struct Dblk {
    uintptr_t vtable;
    uint32_t size;  // block size - 4
    uint8_t* cur;
    uint8_t* start;
};

bool DoHelper(HelperArgs& a)
{
    __try {
        a.step = 1;
        using CreateFn = ZBaseGeom*(__thiscall*)(void*, const char*, const char*, ZBaseGeom*, int);
        ZBaseGeom* node = VFunc<CreateFn>(a.table, kTable_Create)(a.table, a.name, a.className, nullptr, 0);
        if (!node || !node->geom)
            return false;
        a.result = node;
        ZGEOM* geom = node->geom;
        a.step = 2;
        if (a.mesh)
            reinterpret_cast<void(__thiscall*)(ZBaseGeom*, uint32_t)>(kNode_SetMesh)(node, a.mesh);
        reinterpret_cast<void(__thiscall*)(ZBaseGeom*, const float*, const float*)>(kNode_SetTransform)(node, a.mat,
                                                                                                     a.pos);
        uint32_t f = node->flags | (a.flags & 0xFFFFF & ~0x40000u);
        reinterpret_cast<void(__thiscall*)(ZBaseGeom*, uint32_t, uint32_t)>(kNode_SetFlags)(node, f, ~f);
        a.step = 3;
        if (a.parent)
            VFunc<void(__thiscall*)(ZGEOM*, ZBaseGeom*, int, int)>(a.parent, kGeom_AddChild)(a.parent, node, 0, 0);
        if (a.mesh)
            reinterpret_cast<void(__thiscall*)(ZBaseGeom*, bool)>(kNode_CalcBounds)(node, false);
        if (a.data) {
            a.step = 6;
            Dblk d;
            d.vtable = kDblkVtable;
            d.size = (*reinterpret_cast<uint32_t*>(a.data) & 0xFFFFFF) - 4;
            d.cur = d.start = a.data + 4;
            VFunc<void(__thiscall*)(ZGEOM*, Dblk*)>(geom, kGeom_ReadData)(geom, &d);
            VFunc<void(__thiscall*)(ZGEOM*, Dblk*)>(geom, kGeom_ReadData2)(geom, &d);
        }
        a.step = 4;
        if (!a.noInit) {
            VFunc<void(__thiscall*)(ZGEOM*)>(geom, kGeom_Init)(geom);
            reinterpret_cast<void(__thiscall*)(ZBaseGeom*, uint32_t, uint32_t)>(kNode_SetFlags)(node, kFlagInitialized,
                                                                                               0);
            VFunc<void(__thiscall*)(ZGEOM*)>(geom, kGeom_PostInit)(geom);
        }
        a.step = 5;
        return true;
    } __except (Filter(GetExceptionInformation())) {
        return false;
    }
}

bool DoInit(ZGEOM* geom)
{
    __try {
        VFunc<void(__thiscall*)(ZGEOM*)>(geom, kGeom_Init)(geom);
        reinterpret_cast<void(__thiscall*)(ZBaseGeom*, uint32_t, uint32_t)>(kNode_SetFlags)(geom->node,
                                                                                           kFlagInitialized, 0);
        VFunc<void(__thiscall*)(ZGEOM*)>(geom, kGeom_PostInit)(geom);
        return true;
    } __except (Filter(GetExceptionInformation())) {
        return false;
    }
}

constexpr int kTemplate_CreateItem = 0x2A8 / 4;  // ZGEOM* (ZGROUP* parent)
constexpr uintptr_t kEngine_RootGroup = 0xAD8;

ZGEOM* DoCreateItem(ZGEOM* tmpl, void* root)
{
    __try {
        return VFunc<ZGEOM*(__thiscall*)(ZGEOM*, void*)>(tmpl, kTemplate_CreateItem)(tmpl, root);
    } __except (Filter(GetExceptionInformation())) {
        return nullptr;
    }
}

bool DoDestroy(ZGEOM* geom)
{
    __try {
        VFunc<void*(__thiscall*)(ZGEOM*, int)>(geom, kGeom_DeletingDtor)(geom, 1);
        return true;
    } __except (Filter(GetExceptionInformation())) {
        return false;
    }
}

} // namespace

uint32_t MeshOf(const ZGEOM* geom)
{
    uint32_t mesh = 0;
    if (geom && IsReadable(geom, sizeof(ZGEOM)) && IsReadable(geom->node, sizeof(ZBaseGeom)))
        mesh = *reinterpret_cast<const uint32_t*>(reinterpret_cast<const char*>(geom->node) + 0x68);
    return mesh;
}

ZGEOM* CloneGeom(ZGEOM* source, const std::string& name, std::string* error, ZGEOM* parent, bool dynamic)
{
    auto fail = [&](const char* why) -> ZGEOM* {
        if (error)
            *error = why;
        Log("clone '%s': %s", name.c_str(), why);
        return nullptr;
    };
    if (std::string room; !RoomForObjects(512, 4, &room))
        return fail(room.c_str());
    void* table = GeomTable();
    if (!table)
        return fail("no scene loaded");
    if (!source || !IsReadable(source, sizeof(ZGEOM)) || !IsReadable(source->node, sizeof(ZBaseGeom)))
        return fail("source object is gone");
    ZBaseGeom* src = source->node;
    CloneArgs a{};
    a.table = table;
    a.name = name.c_str();  // copied by the engine
    a.dynamic = dynamic;
    a.mesh = MeshOf(source);
    if (!a.mesh)
        return fail("the source object has no mesh");
    a.flags = src->flags & kCopyFlags;
    if (parent && IsReadable(parent, sizeof(ZGEOM))) {
        a.parent = parent->node;
        a.parentGeom = parent;
    } else {
        a.parent = src->parent;
        a.parentGeom = a.parent && IsReadable(a.parent, sizeof(ZBaseGeom)) ? a.parent->geom : nullptr;
    }
    memcpy(a.mat, src->mat, sizeof(a.mat));  // parent space, like the source
    memcpy(a.pos, src->pos, sizeof(a.pos));
    Log("clone '%s' from '%s': mesh %08X, flags %08X of %08X", name.c_str(), SafeString(source->name).c_str(),
        a.mesh, a.flags, src->flags);
    bool ok;
    {
        game::RefAllocUnlock unlock;
        ok = DoClone(a);
    }
    if (!ok) {
        if (a.result && a.result->geom && a.step > 1)
            DoDestroy(a.result->geom);
        char buf[128];
        snprintf(buf, sizeof(buf), "engine call failed (step %d, exception %08lX at %p)", a.step, g_faultCode,
                 g_faultAddress);
        return fail(buf);
    }
    ZBaseGeom* n = a.result;
    Log("cloned '%s' as '%s': node %p geom %p mesh %08X flags %08X bound %.0f (source bound %.0f)",
        SafeString(source->name).c_str(), name.c_str(), static_cast<void*>(n), static_cast<void*>(n->geom), a.mesh,
        n->flags, n->boundRadius, src->boundRadius);
    return n->geom;
}

ZGEOM* CreateHelper(const std::string& name, ZGEOM* parent, uint32_t flags, const float mat[9], const float pos[3],
                    std::string* error, const char* className)
{
    return CreateObject(name, parent, className ? className : "ZSTDOBJ", 0, flags, mat, pos, {}, error);
}

bool InitObject(ZGEOM* geom)
{
    if (!geom || !IsReadable(geom, sizeof(ZGEOM)) || !IsReadable(geom->node, sizeof(ZBaseGeom)))
        return false;
    game::RefAllocUnlock unlock;
    if (DoInit(geom))
        return true;
    Log("init '%s': exception %08lX at %p", SafeString(geom->name).c_str(), g_faultCode, g_faultAddress);
    return false;
}

ZGEOM* CreateObject(const std::string& name, ZGEOM* parent, const char* className, uint32_t mesh, uint32_t flags,
                    const float mat[9], const float pos[3], const std::vector<uint8_t>& data, std::string* error,
                    bool init)
{
    if (!RoomForObjects(512, 4, error))
        return nullptr;
    void* table = GeomTable();
    if (!table) {
        if (error)
            *error = "no scene loaded";
        return nullptr;
    }
    HelperArgs a{};
    a.table = table;
    a.name = name.c_str();
    a.className = className;
    a.mesh = mesh;
    a.noInit = !init;
    // the engine's memory: objects may keep pointers into their data
    if (!data.empty()) {
        a.data = static_cast<uint8_t*>(reinterpret_cast<void*(__cdecl*)(size_t)>(0x530490)(data.size() + 1));
        if (!a.data) {
            if (error)
                *error = "out of memory";
            return nullptr;
        }
        memcpy(a.data, data.data(), data.size());
        a.data[data.size()] = 0xFF;
    }
    a.parent = parent && IsReadable(parent, sizeof(ZGEOM)) ? parent : nullptr;
    a.flags = flags;
    memcpy(a.mat, mat, sizeof(a.mat));
    memcpy(a.pos, pos, sizeof(a.pos));
    bool ok;
    {
        game::RefAllocUnlock unlock;
        ok = DoHelper(a);
    }
    if (!ok) {
        if (a.result && a.result->geom && a.step > 1)
            DoDestroy(a.result->geom);
        char buf[128];
        snprintf(buf, sizeof(buf), "engine call failed (step %d, exception %08lX at %p)", a.step, g_faultCode,
                 g_faultAddress);
        if (error)
            *error = buf;
        Log("helper '%s': %s", name.c_str(), buf);
        return nullptr;
    }
    return a.result->geom;
}

ZGEOM* CreateItem(ZGEOM* itemTemplate, std::string* error)
{
    if (!RoomForObjects(1024, 8, error))  // an item takes about 200 bytes and 3 nodes
        return nullptr;
    auto sys = reinterpret_cast<char*>(game::SysInterface());
    char* engine = sys ? *reinterpret_cast<char**>(sys + 0x98) : nullptr;
    void* root = engine ? *reinterpret_cast<void**>(engine + kEngine_RootGroup) : nullptr;
    if (!root || !itemTemplate || !IsReadable(itemTemplate, sizeof(ZGEOM)) || !IsA(itemTemplate, "ZItemTemplate")) {
        if (error)
            *error = "not an item template (or no scene)";
        return nullptr;
    }
    g_faultCode = 0;
    ZGEOM* item;
    {
        game::RefAllocUnlock unlock;
        item = DoCreateItem(itemTemplate, root);
    }
    if (!item || !IsReadable(item, sizeof(ZGEOM))) {
        char buf[128];
        snprintf(buf, sizeof(buf), "CreateItem failed (exception %08lX at %p)", g_faultCode, g_faultAddress);
        if (error)
            *error = buf;
        Log("item from '%s': %s", SafeString(itemTemplate->name).c_str(), buf);
        return nullptr;
    }
    Log("item from '%s': %s %p (%s)", SafeString(itemTemplate->name).c_str(), SafeString(item->name).c_str(),
        static_cast<void*>(item), ClassName(item).c_str());
    return item;
}

bool DestroyGeom(ZGEOM* geom)
{
    if (!geom || !IsReadable(geom, sizeof(ZGEOM)))
        return false;
    bool ok = DoDestroy(geom);
    if (ok)
        Log("destroyed geom %p", static_cast<void*>(geom));
    else
        Log("destroying geom %p failed: exception %08lX at %p", static_cast<void*>(geom), g_faultCode, g_faultAddress);
    return ok;
}

} // namespace h2::engine
