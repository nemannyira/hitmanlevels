#include "ingame_map.h"

#include <windows.h>
#include <algorithm>
#include <cstring>

#include "messages.h"
#include "scene_data.h"

namespace h2::engine {

namespace {

constexpr uintptr_t kHandleToPtr = 0x40CAC0;  // (ecx: handle) -> the object (the engine's handle table)
constexpr uintptr_t kFloorOf = 0x540C50;      // window->(ZGEOM*) -> floor entry or null
constexpr uintptr_t kDefaultFloor = 0x53EDA0; // window->() -> floor entry
constexpr int kGroup_AddChild = 0x26C / 4;    // group->(ZGEOM* child, int, int)
constexpr int kGeom_GetWorld = 0x84 / 4;      // (float mat[9], float pos[3]): its world transform
constexpr int kGeom_SetWorld = 0x80 / 4;      // (const float mat[9], const float pos[3]): set (CopyObjectPosDir)
constexpr size_t kRoutine_Owner = 0x6E;       // a routine's object (ZEventBase)
constexpr size_t kMapObject_Icon = 0x72;      // ZGEOM_MapObject: the legend's icon's name (char*)
constexpr size_t kMapObject_Shown = 0x76;     //   its icon in the map's set (bool)
constexpr size_t kMap_IconDraw = 0x1AE;       // CMapIconDraw*: +0x18 count, +0x1C {icon, object} refs
constexpr size_t kMap_ShownFloor = 0x2F3;     // the floor entry shown last
constexpr size_t kFloor_Group = 0x04;         // a floor entry: its group's ref,
constexpr size_t kFloor_Name = 0x08;          //   its name (char*)
constexpr size_t kIconDraw_Count = 0x18, kIconDraw_Pairs = 0x1C;
constexpr uint32_t kMaxIcons = 256;

template <typename T>
T VFunc(void* obj, int index)
{
    return reinterpret_cast<T>((*reinterpret_cast<void***>(obj))[index]);
}

void* SafeHandleToPtr(uint32_t handle)
{
    __try {
        return reinterpret_cast<void*(__fastcall*)(uint32_t)>(kHandleToPtr)(handle);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

uint8_t* SafeFloorOf(void* map, ZGEOM* geom)
{
    __try {
        return reinterpret_cast<uint8_t*(__thiscall*)(void*, ZGEOM*)>(kFloorOf)(map, geom);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

uint8_t* SafeDefaultFloor(void* map)
{
    __try {
        return reinterpret_cast<uint8_t*(__thiscall*)(void*)>(kDefaultFloor)(map);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

// the child into the group, where it was in the world: the group's AddChild keeps its local transform
// (the floors of C2-1's and C7-1's mansion are moved and turned), so its world one is set again after
bool SafeAddChild(ZGEOM* group, ZGEOM* child)
{
    __try {
        float mat[9], pos[3];
        VFunc<void(__thiscall*)(ZGEOM*, float*, float*)>(child, kGeom_GetWorld)(child, mat, pos);
        VFunc<void(__thiscall*)(ZGEOM*, ZGEOM*, int, int)>(group, kGroup_AddChild)(group, child, 0, 1);
        VFunc<void(__thiscall*)(ZGEOM*, const float*, const float*)>(child, kGeom_SetWorld)(child, mat, pos);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool Usable(ZGEOM* g)
{
    return g && IsReadable(g, sizeof(ZGEOM)) && g->node && IsReadable(g->node, sizeof(ZBaseGeom)) &&
           g->node->geom == g;
}

// a floor entry's group
ZGEOM* FloorGroup(const uint8_t* entry)
{
    if (!entry || !IsReadable(entry + kFloor_Group, 4))
        return nullptr;
    uint32_t ref;
    memcpy(&ref, entry + kFloor_Group, 4);
    ZGEOM* g = ref ? GeomOfRef(ref) : nullptr;
    return Usable(g) ? g : nullptr;
}

// a field of the window (a pointer)
uint8_t* Field(void* map, size_t at)
{
    uint8_t* p = nullptr;
    if (map && IsReadable(static_cast<uint8_t*>(map) + at, 4))
        memcpy(&p, static_cast<uint8_t*>(map) + at, 4);
    return p;
}

} // namespace

void* IngameMap()
{
    uint32_t ref = SceneComRef("rIngameMap");
    void* map = ref ? SafeHandleToPtr(ref) : nullptr;
    return map && IsReadable(map, kMap_ShownFloor + 4) ? map : nullptr;
}

ZGEOM* IngameMapWindow()
{
    ZGEOM* window = reinterpret_cast<ZGEOM*>(Field(IngameMap(), kRoutine_Owner));
    return Usable(window) ? window : nullptr;
}

bool IsMapFloor(ZGEOM* geom)
{
    return Usable(geom) && GeomRoutine(geom, "MapGroup") != nullptr;
}

ZGEOM* MapFloorOf(ZGEOM* geom)
{
    void* map = IngameMap();
    if (!map)
        return nullptr;
    ZGEOM* floor = Usable(geom) ? FloorGroup(SafeFloorOf(map, geom)) : nullptr;
    return floor ? floor : FloorGroup(SafeDefaultFloor(map));
}

int MapIconShown(ZGEOM* geom)
{
    uint8_t* routine = Usable(geom) ? GeomRoutine(geom, "MapObject") : nullptr;
    if (!routine || !IsReadable(routine + kMapObject_Shown, 1))
        return -1;
    return routine[kMapObject_Shown] ? 1 : 0;
}

ZGEOM* MapIconOwner(ZGEOM* geom)
{
    if (MapIconShown(geom) >= 0)
        return geom;
    ZBaseGeom* parent = Usable(geom) ? geom->node->parent : nullptr;
    ZGEOM* group = parent && IsReadable(parent, sizeof(ZBaseGeom)) ? parent->geom : nullptr;
    return MapIconShown(group) >= 0 ? group : nullptr;
}

std::string MapIconName(ZGEOM* geom)
{
    uint8_t* routine = Usable(geom) ? GeomRoutine(geom, "MapObject") : nullptr;
    const char* name = nullptr;
    if (routine && IsReadable(routine + kMapObject_Icon, 4))
        memcpy(&name, routine + kMapObject_Icon, 4);
    return name ? SafeString(name) : std::string();
}

int MapIconShownInFile(ZGEOM* geom)
{
    int r = Usable(geom) ? RecordOf(geom) : -1;
    int section = RoutineSection(r, "ZGEOM_MapObject");
    if (!section)
        return -1;
    uint32_t shown = 0;  // (its values: the icon's name, shown)
    return RoutineValue(RawBlock(SceneRecords()[size_t(r)].data), section, 1, shown) ? (shown ? 1 : 0) : -1;
}

bool ShowMapIcon(ZGEOM* geom, bool on)
{
    return MapIconShown(geom) >= 0 && SendCommand(geom, "ShowMapIcon", on ? 1 : 0);
}

bool AddToMapFloor(ZGEOM* floor, ZGEOM* model)
{
    if (!IsMapFloor(floor) || !Usable(model) || model == floor)
        return false;
    if (model->node->parent == floor->node)
        return true;
    return SafeAddChild(floor, model) && model->node->parent == floor->node;
}

std::string MapShownFloor()
{
    const uint8_t* entry = Field(IngameMap(), kMap_ShownFloor);
    const char* name = nullptr;
    if (entry && IsReadable(entry + kFloor_Name, 4))
        memcpy(&name, entry + kFloor_Name, 4);
    return name ? SafeString(name) : std::string();
}

std::vector<MapIconDrawn> MapIconsDrawn()
{
    std::vector<MapIconDrawn> out;
    const uint8_t* draw = Field(IngameMap(), kMap_IconDraw);
    if (!draw || !IsReadable(draw + kIconDraw_Count, 4))
        return out;
    uint32_t n;
    memcpy(&n, draw + kIconDraw_Count, 4);
    n = std::min(n, kMaxIcons);
    if (!IsReadable(draw + kIconDraw_Pairs, size_t(n) * 8))
        return out;
    for (uint32_t i = 0; i < n; ++i) {
        uint32_t pair[2];
        memcpy(pair, draw + kIconDraw_Pairs + size_t(i) * 8, 8);
        out.push_back({GeomOfRef(pair[0]), GeomOfRef(pair[1])});
    }
    return out;
}

} // namespace h2::engine
