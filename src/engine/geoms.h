#pragma once
// Scene object model of the Glacier (Hitman 2) engine, as reverse engineered:
//
//   ZGEOM (scene object)            ZBaseGeom (transform node, 0x70 bytes)
//   +0x00 vtable                    +0x00 float mat[9]   rows forward, up, right (parent space)
//   +0x04 char* name                +0x24 float pos[3]   (parent space, cm, +Y up)
//   +0x08 ZBaseGeom* node   ---->   +0x3C uint32 flags
//   +0x0C ZClassInfo* class         +0x30 float boundCenter[3], +0x40 float boundHalf[3]
//                                   (box in node space), +0x4C float boundRadius
//                                   +0x50 ZBaseGeom* parent
//                                   +0x54 ZGEOM* geom
//                                   +0x5C uint16 first child  (1-based index, static nodes)
//                                   +0x5E uint16 next sibling (1-based index, static nodes)
//
// Nodes live in a static array (loaded with the scene) or in a hashed table of
// dynamic nodes, both owned by the geom table at ZEngineDataBase+4.
#include <cstdint>
#include <string>
#include <vector>

namespace h2::engine {

#pragma pack(push, 1)
struct ZClassInfo {
    const char* name;          // 0x00
    uint32_t _04, _08;
    ZClassInfo* base;          // 0x0C
    ZClassInfo* prev;          // 0x10
    ZClassInfo* next;          // 0x14
    uint32_t objectSize;       // 0x18
    uint8_t _1c[7];
    uint32_t id;               // 0x23
};
#pragma pack(pop)

struct ZBaseGeom;

struct ZGEOM {
    void* vtable;
    const char* name;
    ZBaseGeom* node;
    ZClassInfo* classInfo;
};

struct ZBaseGeom {
    float mat[9];              // 0x00
    float pos[3];              // 0x24
    float boundCenter[3];      // 0x30  bounding sphere centre (node space)
    uint32_t flags;            // 0x3C
    float boundHalf[3];        // 0x40  bounding box half extents (node space, around boundCenter)
    float boundRadius;         // 0x4C  |boundHalf| (+ epsilon)
    ZBaseGeom* parent;         // 0x50
    ZGEOM* geom;               // 0x54
    uint8_t _58[0x5C - 0x58];
    uint16_t firstChild;       // 0x5C
    uint16_t nextSibling;      // 0x5E
    uint8_t _60[0x70 - 0x60];
};
static_assert(sizeof(ZBaseGeom) == 0x70);

// Safe (SEH guarded) accessors: engine memory may change under us.
bool IsReadable(const void* p, size_t n);
std::string SafeString(const char* p, size_t max = 128);

struct GeomInfo {
    ZBaseGeom* node;
    ZGEOM* geom;
    ZBaseGeom* parent;
    std::string name;
    std::string className;
    int depth = 0;
    std::vector<int> children;  // indices into the snapshot
    // Stable identity across sessions: names from the root, "#k" for the k-th
    // (k > 0) sibling with the same name, e.g. "ROOT/Garden/Crate#2".
    std::string path;
};

// Snapshot of every scene object currently loaded (static + dynamic nodes),
// arranged as a tree (index 0 = ROOT when present).
struct SceneSnapshot {
    std::vector<GeomInfo> geoms;
    std::vector<int> roots;

    int Find(const ZGEOM* geom) const;
    int FindPath(const std::string& path) const;
    std::vector<int> FindName(const std::string& name) const;
};
// `extra`: nodes created at runtime (see spawn.h), included when live.
SceneSnapshot TakeSnapshot(const std::vector<ZBaseGeom*>& extra = {});

// The geom table (ZEngineDataBase+4) of the loaded scene, or null.
void* GeomTable();

std::string ClassName(const ZGEOM* geom);
// "ZCAMERA : ZGEOM : ..." (most derived first)
std::string ClassChain(const ZGEOM* geom);
// The same for a class id (e.g. of a scene file's record); `known`: any live
// object (its class info leads to the others). "" when unknown.
std::string ClassChainOfId(uint32_t id, const ZGEOM* known);
bool IsA(const ZGEOM* geom, const char* className);

void WorldPosition(ZGEOM* geom, float pos[3]);

} // namespace h2::engine
