#include "geoms.h"

#include <windows.h>
#include <cstring>
#include <unordered_map>
#include <unordered_set>

#include "../game.h"

namespace h2::engine {

namespace {

constexpr uintptr_t kSys_EngineDataBase = 0x98;  // ZSysInterface -> ZEngineDataBase*
constexpr uintptr_t kEngine_GeomTable = 0x04;    // ZEngineDataBase -> geom table
constexpr int kGeomTable_StaticBase = 0x4C;      // vtable offset: ZBaseGeom* StaticNodes()
constexpr uintptr_t kGeomTable_Hash = 0x08;      // -> object with the index mask at +0x10
constexpr uintptr_t kGeomTable_Entries = 0x0C;   // -> ZBaseGeom* entries[mask + 1]
constexpr int kMaxStaticNodes = 65535;           // 16-bit 1-based indices
constexpr uint32_t kMaxDynamicEntries = 1u << 20;

bool SafeCopy(void* dst, const void* src, size_t n)
{
    __try {
        memcpy(dst, src, n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

template <typename T>
bool Read(const void* p, T& out)
{
    return SafeCopy(&out, p, sizeof(T));
}

template <typename T>
bool ReadAt(const void* base, uintptr_t off, T& out)
{
    return Read(static_cast<const char*>(base) + off, out);
}

ZBaseGeom* CallStaticBase(void* table)
{
    __try {
        using Fn = ZBaseGeom*(__thiscall*)(void*);
        return (*reinterpret_cast<Fn**>(table))[kGeomTable_StaticBase / 4](table);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

// A node is live when its geom points back to it.
bool IsLiveNode(ZBaseGeom* node, ZGEOM*& geom)
{
    ZGEOM* g = nullptr;
    if (!node || !ReadAt(node, offsetof(ZBaseGeom, geom), g) || !g)
        return false;
    ZBaseGeom* back = nullptr;
    if (!ReadAt(g, offsetof(ZGEOM, node), back) || back != node)
        return false;
    geom = g;
    return true;
}

} // namespace

void* GeomTable()
{
    auto sys = reinterpret_cast<char*>(game::SysInterface());
    void* engine = nullptr;
    void* table = nullptr;
    if (!sys || !ReadAt(sys, kSys_EngineDataBase, engine) || !engine || !ReadAt(engine, kEngine_GeomTable, table))
        return nullptr;
    return table;
}

bool IsReadable(const void* p, size_t n)
{
    char buf[64];
    while (n) {
        size_t k = n < sizeof(buf) ? n : sizeof(buf);
        if (!SafeCopy(buf, p, k))
            return false;
        p = static_cast<const char*>(p) + k;
        n -= k;
    }
    return true;
}

std::string SafeString(const char* p, size_t max)
{
    std::string s;
    for (size_t i = 0; p && i < max; ++i) {
        char c;
        if (!Read(p + i, c) || !c)
            break;
        s += c;
    }
    return s;
}

std::string ClassName(const ZGEOM* geom)
{
    ZClassInfo* ci = nullptr;
    const char* name = nullptr;
    if (!geom || !ReadAt(geom, offsetof(ZGEOM, classInfo), ci) || !ci || !ReadAt(ci, 0, name))
        return "?";
    return SafeString(name, 64);
}

std::string ClassChain(const ZGEOM* geom)
{
    ZClassInfo* ci = nullptr;
    std::string chain;
    if (!geom || !ReadAt(geom, offsetof(ZGEOM, classInfo), ci))
        return "?";
    for (int depth = 0; ci && depth < 16; ++depth) {
        const char* name = nullptr;
        if (!ReadAt(ci, 0, name))
            break;
        if (!chain.empty())
            chain += " : ";
        chain += SafeString(name, 64);
        ZClassInfo* base = nullptr;
        if (!ReadAt(ci, offsetof(ZClassInfo, base), base) || base == ci)
            break;
        ci = base;
    }
    return chain;
}

bool IsA(const ZGEOM* geom, const char* className)
{
    ZClassInfo* ci = nullptr;
    if (!geom || !ReadAt(geom, offsetof(ZGEOM, classInfo), ci))
        return false;
    for (int depth = 0; ci && depth < 16; ++depth) {
        const char* name = nullptr;
        if (ReadAt(ci, 0, name) && SafeString(name, 64) == className)
            return true;
        ZClassInfo* base = nullptr;
        if (!ReadAt(ci, offsetof(ZClassInfo, base), base) || base == ci)
            break;
        ci = base;
    }
    return false;
}

void WorldPosition(ZGEOM* geom, float pos[3])
{
    float m[9];
    game::GetWorldTransform(geom, m, pos);
}

SceneSnapshot TakeSnapshot(const std::vector<ZBaseGeom*>& extra)
{
    SceneSnapshot snap;
    void* table = GeomTable();
    if (!table)
        return snap;

    std::vector<std::pair<ZBaseGeom*, ZGEOM*>> found;
    std::unordered_set<ZBaseGeom*> seen;
    auto add = [&](ZBaseGeom* node) {
        ZGEOM* geom;
        if (seen.count(node) || !IsLiveNode(node, geom))
            return false;
        seen.insert(node);
        found.push_back({node, geom});
        return true;
    };

    // static nodes: contiguous array; stop after a long run of dead slots
    if (ZBaseGeom* base = CallStaticBase(table)) {
        int dead = 0;
        for (int i = 0; i < kMaxStaticNodes && dead < 256; ++i)
            dead = add(base + i) ? 0 : dead + 1;
    }
    // dynamic nodes: hashed table of node pointers
    void* hash = nullptr;
    ZBaseGeom** entries = nullptr;
    uint32_t mask = 0;
    if (ReadAt(table, kGeomTable_Hash, hash) && hash && ReadAt(hash, 0x10, mask) &&
        ReadAt(table, kGeomTable_Entries, entries) && entries && mask < kMaxDynamicEntries) {
        for (uint32_t i = 0; i <= mask; ++i) {
            ZBaseGeom* node = nullptr;
            if (Read(entries + i, node) && node)
                add(node);
        }
    }

    // runtime created nodes the tables above may not reach
    for (ZBaseGeom* node : extra)
        add(node);

    std::unordered_map<ZBaseGeom*, int> index;
    snap.geoms.reserve(found.size());
    for (auto& [node, geom] : found) {
        GeomInfo gi;
        gi.node = node;
        gi.geom = geom;
        ReadAt(node, offsetof(ZBaseGeom, parent), gi.parent);
        const char* name = nullptr;
        ReadAt(geom, offsetof(ZGEOM, name), name);
        gi.name = SafeString(name);
        gi.className = ClassName(geom);
        index[node] = int(snap.geoms.size());
        snap.geoms.push_back(std::move(gi));
    }
    for (int i = 0; i < int(snap.geoms.size()); ++i) {
        auto it = index.find(snap.geoms[i].parent);
        if (it != index.end() && it->second != i)
            snap.geoms[it->second].children.push_back(i);
        else
            snap.roots.push_back(i);
    }
    // depths and paths (iterative, guards against cycles)
    auto segment = [&](const std::vector<int>& siblings, int i) {
        const std::string& name = snap.geoms[i].name;
        int k = 0;
        for (int s : siblings) {
            if (s == i)
                break;
            k += snap.geoms[s].name == name;
        }
        return k ? name + "#" + std::to_string(k) : name;
    };
    std::vector<int> stack;
    for (int r : snap.roots) {
        snap.geoms[r].path = segment(snap.roots, r);
        stack.push_back(r);
    }
    std::vector<char> visited(snap.geoms.size(), 0);
    while (!stack.empty()) {
        int i = stack.back();
        stack.pop_back();
        if (visited[i])
            continue;
        visited[i] = 1;
        for (int c : snap.geoms[i].children) {
            snap.geoms[c].depth = snap.geoms[i].depth + 1;
            snap.geoms[c].path = snap.geoms[i].path + "/" + segment(snap.geoms[i].children, c);
            stack.push_back(c);
        }
    }
    return snap;
}

int SceneSnapshot::Find(const ZGEOM* geom) const
{
    for (int i = 0; i < int(geoms.size()); ++i)
        if (geoms[i].geom == geom)
            return i;
    return -1;
}

int SceneSnapshot::FindPath(const std::string& path) const
{
    for (int i = 0; i < int(geoms.size()); ++i)
        if (geoms[i].path == path)
            return i;
    return -1;
}

std::vector<int> SceneSnapshot::FindName(const std::string& name) const
{
    std::vector<int> out;
    for (int i = 0; i < int(geoms.size()); ++i)
        if (geoms[i].name == name)
            out.push_back(i);
    return out;
}

} // namespace h2::engine
