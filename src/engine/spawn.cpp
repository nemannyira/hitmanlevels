#include "spawn.h"

#include <windows.h>

#include "../game.h"
#include "../log.h"

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
// Only the flags of a plain static object are copied (as found over the
// meshed objects of the .GMS files): render bits (low byte), 0x100 (stairs,
// collision planes) and 0x20000 (common static bit). The others give the node
// a role in some engine system that the clone has no data for:
//   0x00400 helper/fragment   0x02000 sound/sight helpers   0x04000 fragments
//   0x08000 items/breakables (gets a handle from a manager at +0x6C)
//   0x10000 lights            0x40000 dynamic               0x80000 doors (crash)
constexpr uint32_t kCopyFlags = 0x000FF | 0x00100 | 0x20000;

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
        reinterpret_cast<void(__thiscall*)(ZBaseGeom*, bool)>(game::ZBaseGeom_SetDynamic)(node, true);
        a.step = 9;
        return true;
    } __except (Filter(GetExceptionInformation())) {
        return false;
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

ZGEOM* CloneGeom(ZGEOM* source, const std::string& name, std::string* error)
{
    auto fail = [&](const char* why) -> ZGEOM* {
        if (error)
            *error = why;
        Log("clone '%s': %s", name.c_str(), why);
        return nullptr;
    };
    void* table = GeomTable();
    if (!table)
        return fail("no scene loaded");
    if (!source || !IsReadable(source, sizeof(ZGEOM)) || !IsReadable(source->node, sizeof(ZBaseGeom)))
        return fail("source object is gone");
    ZBaseGeom* src = source->node;
    CloneArgs a{};
    a.table = table;
    a.name = name.c_str();  // copied by the engine
    a.mesh = MeshOf(source);
    if (!a.mesh)
        return fail("the source object has no mesh");
    a.flags = src->flags & kCopyFlags;
    a.parent = src->parent;
    a.parentGeom = a.parent && IsReadable(a.parent, sizeof(ZBaseGeom)) ? a.parent->geom : nullptr;
    memcpy(a.mat, src->mat, sizeof(a.mat));  // parent space, like the source
    memcpy(a.pos, src->pos, sizeof(a.pos));
    Log("clone '%s' from '%s': mesh %08X, flags %08X of %08X", name.c_str(), SafeString(source->name).c_str(),
        a.mesh, a.flags, src->flags);
    bool ok = DoClone(a);
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
