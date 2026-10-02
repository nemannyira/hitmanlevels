#include "imports.h"

#include <windows.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <set>

#include "../hook.h"
#include "../log.h"
#include "geoms.h"
#include "scene_data.h"
#include "zip.h"

namespace h2::engine {

namespace {

constexpr uintptr_t kGetPrimsSize = 0x41BFF0;    // ZEngineDataBase::(void) -> size
constexpr uintptr_t kGetPrimsData = 0x41C080;    // ZEngineDataBase::(void* buf, uint size)
constexpr uintptr_t kGetTextureSize = 0x41C270;
constexpr uintptr_t kGetTextureData = 0x41C300;
constexpr uintptr_t kGetSceneName = 0x41B880;    // ZEngineDataBase::(void) -> const char*
constexpr uint32_t kRecord = 0x84;               // a PRM record (one material of a model)
constexpr int kTexIds = 2048;

using SizeFn = uint32_t(__fastcall*)(void* self, void* edx);
using DataFn = void(__fastcall*)(void* self, void* edx, void* buf, uint32_t size);
SizeFn oPrimsSize, oTexSize;
DataFn oPrimsData, oTexData;

uint32_t U32(const std::vector<uint8_t>& b, size_t at)
{
    uint32_t v = 0;
    if (at + 4 <= b.size())
        memcpy(&v, &b[at], 4);
    return v;
}
void Put(std::vector<uint8_t>& b, size_t at, uint32_t v) { memcpy(&b[at], &v, 4); }

struct Source {
    bool loaded = false;
    std::vector<uint8_t> prm, tex;
    std::set<uint32_t> records;  // offsets of the PRM's records (its table)
    uint32_t texTable = 0;
    bool HasTexture(uint32_t id) const { return id && id < kTexIds && U32(tex, texTable + id * 4) != 0; }
    uint32_t TextureAt(uint32_t id) const { return U32(tex, texTable + id * 4); }
    uint32_t TextureSize(uint32_t id) const { return U32(tex, TextureAt(id)); }
    std::string TextureName(uint32_t id) const
    {
        uint32_t o = TextureAt(id) + 0x24;
        std::string n;
        while (o < tex.size() && tex[o] && n.size() < 96)
            n += char(tex[o++]);
        return n;
    }
};
std::map<std::string, Source> g_sources;

Source* LoadSource(const std::string& level)
{
    Source& s = g_sources[level];
    if (s.loaded)
        return s.prm.empty() ? nullptr : &s;
    s.loaded = true;
    std::string zip = GameDir() + "Scenes\\" + level + "\\" + level + "__MAIN.ZIP";
    std::string base = "SCENES/" + level + "/" + level + "__MAIN.";
    std::string err;
    if (!ReadZipEntry(zip, base + "PRM", s.prm, &err) || !ReadZipEntry(zip, base + "TEX", s.tex, &err)) {
        Log("imports: %s: %s", level.c_str(), err.c_str());
        s.prm.clear();
        return nullptr;
    }
    s.texTable = U32(s.tex, 0);
    uint32_t table = U32(s.prm, 8), count = U32(s.prm, 12);
    for (uint32_t i = 0; i < count && table + i * 4 + 4 <= s.prm.size(); ++i)
        s.records.insert(U32(s.prm, table + i * 4));
    Log("imports: read %s (PRM %zu, TEX %zu bytes)", level.c_str(), s.prm.size(), s.tex.size());
    return &s;
}

std::vector<ModelImport> g_requests;
std::function<std::vector<ModelImport>(const std::string&)> g_provider;
std::vector<ModelImport> g_load;  // what the scene being loaded gets

// the load in progress
struct TexUse {  // a texture id field of a copied record
    size_t at;      // in the blob
    uint32_t mask;  // bits of the id in the field
    std::string level;
    uint32_t id;
};
bool g_active;
uint32_t g_primReal, g_texReal, g_base;
std::vector<uint8_t> g_blob;
std::vector<TexUse> g_texUses;
std::map<std::pair<std::string, uint32_t>, uint32_t> g_texMap;  // (level, id) -> id here
std::map<std::pair<std::string, uint32_t>, uint32_t> g_meshes;  // (level, mesh) -> mesh here
std::map<std::pair<std::string, uint32_t>, uint32_t> g_pending; // the same, being loaded

bool Wanted(void* engine)
{
    const char* name = reinterpret_cast<const char*(__thiscall*)(void*)>(kGetSceneName)(engine);
    std::string n = name ? name : "";
    g_load.clear();
    if (n.find("__MAIN") == std::string::npos)
        return false;
    if (g_provider)
        g_load = g_provider(n);
    g_load.insert(g_load.end(), g_requests.begin(), g_requests.end());
    Log("imports: %s gets %zu models of other levels", n.c_str(), g_load.size());
    return !g_load.empty();
}

// one model: its records and data copied into the blob, offsets moved
bool Copy(const ModelImport& m)
{
    Source* s = LoadSource(m.level);
    if (!s)
        return false;
    const std::vector<uint8_t>& prm = s->prm;
    std::vector<uint32_t> records;
    for (uint32_t r = m.mesh; r && records.size() < 64; r = U32(prm, r + 8)) {
        if (r + kRecord > prm.size() || std::find(records.begin(), records.end(), r) != records.end())
            break;
        records.push_back(r);
    }
    if (records.empty()) {
        Log("imports: %s %08X: no such model", m.level.c_str(), m.mesh);
        return false;
    }
    uint32_t lo = UINT32_MAX, hi = 0;
    for (uint32_t r : records) {
        lo = std::min(lo, r);
        hi = std::max(hi, r + kRecord);
        for (size_t f : {0x18, 0x1C, 0x44}) {
            uint32_t v = U32(prm, r + f);
            if (v && v < r)
                lo = std::min(lo, v);
        }
    }
    if (hi - lo > 0x400000) {
        Log("imports: %s %08X: model spans %u bytes, not copied", m.level.c_str(), m.mesh, hi - lo);
        return false;
    }
    size_t pos = (g_blob.size() + 15) & ~size_t(15);
    g_blob.resize(pos + (hi - lo));
    memcpy(&g_blob[pos], &prm[lo], hi - lo);
    uint32_t delta = g_base + uint32_t(pos) - lo;
    int outside = 0;
    for (uint32_t r : records) {
        size_t q = pos + (r - lo);
        for (size_t f : {0x08, 0x18, 0x1C, 0x2C, 0x44}) {
            uint32_t v = U32(g_blob, q + f);
            if (!v)
                continue;
            if (v >= lo && v < hi) {
                Put(g_blob, q + f, v + delta);
            } else {
                Put(g_blob, q + f, 0);
                ++outside;
            }
        }
        Put(g_blob, q + 0x30, 0);  // collision data elsewhere in the file: not copied yet
        uint32_t tex = U32(g_blob, q + 0x04) & 0x7FF;
        if (s->HasTexture(tex))
            g_texUses.push_back({q + 0x04, 0x7FF, m.level, tex});
        uint32_t tex2 = U32(g_blob, q + 0x34) & 0xFFFF;
        if (s->HasTexture(tex2))
            g_texUses.push_back({q + 0x34, 0xFFFF, m.level, tex2});
    }
    uint32_t mesh = g_base + uint32_t(pos) + (m.mesh - lo);
    g_pending[{m.level, m.mesh}] = mesh;
    Log("imports: %s %08X -> %08X (%zu records, %u bytes%s)", m.level.c_str(), m.mesh, mesh, records.size(), hi - lo,
        outside ? ", some references outside dropped" : "");
    return true;
}

uint32_t __fastcall hkPrimsSize(void* self, void* edx)
{
    uint32_t real = oPrimsSize(self, edx);
    g_active = false;
    if (!Wanted(self) || int(real) <= 0)
        return real;
    g_primReal = real;
    g_base = (real + 15) & ~15u;
    g_blob.clear();
    g_texUses.clear();
    g_texMap.clear();
    g_pending.clear();
    std::set<std::pair<std::string, uint32_t>> done;
    for (const ModelImport& m : g_load)
        if (done.insert({m.level, m.mesh}).second)
            Copy(m);
    if (g_blob.empty())
        return real;
    g_active = true;
    return g_base + ((uint32_t(g_blob.size()) + 15) & ~15u);
}

uint32_t __fastcall hkTexSize(void* self, void* edx)
{
    uint32_t real = oTexSize(self, edx);
    if (!g_active || int(real) <= 0)
        return real;
    g_texReal = real;
    uint32_t extra = 0;
    std::set<std::pair<std::string, uint32_t>> seen;
    for (const TexUse& u : g_texUses)
        if (seen.insert({u.level, u.id}).second)
            extra += g_sources[u.level].TextureSize(u.id) + 16;
    return real + extra;
}

void __fastcall hkTexData(void* self, void* edx, void* buf, uint32_t size)
{
    if (!g_active) {
        oTexData(self, edx, buf, size);
        return;
    }
    oTexData(self, edx, buf, g_texReal);
    auto b = static_cast<uint8_t*>(buf);
    uint32_t table;
    memcpy(&table, b, 4);
    auto slot = [&](uint32_t id) { return reinterpret_cast<uint32_t*>(b + table + id * 4); };
    std::map<std::string, uint32_t> names;  // this level's textures by name
    for (uint32_t id = 1; id < kTexIds; ++id) {
        uint32_t o = *slot(id);
        if (o && o + 0x24 < g_texReal)
            names[reinterpret_cast<const char*>(b + o + 0x24)] = id;
    }
    uint32_t cursor = g_texReal;
    uint32_t free = kTexIds - 1;
    int added = 0, shared = 0;
    for (const TexUse& u : g_texUses) {
        auto key = std::make_pair(u.level, u.id);
        if (g_texMap.count(key))
            continue;
        const Source& s = g_sources[u.level];
        std::string name = s.TextureName(u.id);
        auto same = names.find(name);
        if (same != names.end()) {
            g_texMap[key] = same->second;
            ++shared;
            continue;
        }
        while (free > 0 && *slot(free))
            --free;
        uint32_t n = s.TextureSize(u.id);
        if (!free || cursor + n > size) {
            Log("imports: no room for texture %s", name.c_str());
            g_texMap[key] = 0;
            continue;
        }
        memcpy(b + cursor, &s.tex[s.TextureAt(u.id)], n);
        memcpy(b + cursor + 0x0C, &free, 4);  // the entry's id
        *slot(free) = cursor;
        names[name] = free;
        g_texMap[key] = free;
        cursor += n;
        ++added;
        --free;
    }
    Log("imports: textures: %d added, %d shared with this level", added, shared);
}

void __fastcall hkPrimsData(void* self, void* edx, void* buf, uint32_t size)
{
    if (!g_active) {
        oPrimsData(self, edx, buf, size);
        return;
    }
    oPrimsData(self, edx, buf, g_primReal);
    for (const TexUse& u : g_texUses) {
        auto it = g_texMap.find({u.level, u.id});
        uint32_t id = it != g_texMap.end() ? it->second : 0;
        uint32_t v = U32(g_blob, u.at);
        Put(g_blob, u.at, (v & ~u.mask) | (id & u.mask));
    }
    auto b = static_cast<uint8_t*>(buf);
    memcpy(b + g_base, g_blob.data(), g_blob.size());
    // header[1]: where the engine adds prims while the level runs (header[2] is the record table)
    uint32_t used = g_base + ((uint32_t(g_blob.size()) + 15) & ~15u);
    memcpy(b + 4, &used, 4);
    g_meshes = g_pending;
    g_active = false;
    Log("imports: %zu models placed at %08X (%zu bytes)", g_meshes.size(), g_base, g_blob.size());
}

// ---- catalogue of another level ----

struct Catalogue {
    bool built = false;
    std::vector<LevelModel> models;
};
std::map<std::string, Catalogue> g_catalogues;

// any object of the loaded scene: its class info leads to every class
ZGEOM* KnownGeom()
{
    for (int i = 0; i < 64; ++i)
        if (ZGEOM* g = RecordGeom(i))
            return g;
    return nullptr;
}

// rotation of a record in the world: its matrix and its groups' (the editor's
// convention: a local vector (x, y, z) is x * row2 + y * row1 + z * row0)
void Compose(const float parent[9], const float local[9], float out[9])
{
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c)
            out[r * 3 + c] = local[r * 3 + 0] * parent[6 + c] + local[r * 3 + 1] * parent[3 + c] +
                             local[r * 3 + 2] * parent[0 + c];
}

void BuildCatalogue(const std::string& level, Catalogue& cat)
{
    cat.built = true;
    Source* src = LoadSource(level);
    if (!src)
        return;
    std::string zip = GameDir() + "Scenes\\" + level + "\\" + level + "__MAIN.ZIP";
    std::string base = "SCENES/" + level + "/" + level + "__MAIN.";
    std::vector<uint8_t> packed, gms, buf;
    std::string err;
    if (!ReadZipEntry(zip, base + "GMS", packed, &err) || !ReadZipEntry(zip, base + "BUF", buf, &err) ||
        packed.size() < 9 || !Inflate(packed.data() + 9, packed.size() - 9, gms, U32(packed, 0))) {
        Log("imports: %s: scene file not read (%s)", level.c_str(), err.c_str());
        return;
    }
    std::vector<SceneRecord> records = ParseSceneRecords(gms, buf);
    ZGEOM* known = KnownGeom();
    std::map<uint32_t, std::string> chains;
    auto chain = [&](uint32_t id) -> const std::string& {
        auto it = chains.find(id);
        if (it == chains.end())
            it = chains.emplace(id, ClassChainOfId(id, known)).first;
        return it->second;
    };
    std::map<uint32_t, size_t> byMesh;
    for (const SceneRecord& r : records) {
        if (!r.mesh || (r.flags & (0x2000 | 0x10000)) || !src->records.count(r.mesh) || U32(src->prm, r.mesh) != 0)
            continue;
        // in a room, not part of a character or an item
        bool inRoom = false, excluded = false;
        for (int p = r.parent; p >= 0 && size_t(p) < records.size(); p = records[p].parent) {
            const std::string& c = chain(records[p].classId);
            inRoom |= c.find("ZROOM") != std::string::npos;
            excluded |= c.find("ZActor") != std::string::npos || c.find("ZItem") != std::string::npos;
        }
        const std::string& own = chain(r.classId);
        if (!inRoom || excluded || own.find("ZActor") != std::string::npos || own.find("ZItem") != std::string::npos)
            continue;
        auto [it, added] = byMesh.emplace(r.mesh, cat.models.size());
        if (!added) {
            cat.models[it->second].uses++;
            continue;
        }
        LevelModel m;
        m.mesh = r.mesh;
        m.name = r.name;
        m.group = r.parent >= 0 ? records[r.parent].name : "";
        m.classId = r.classId;
        m.flags = r.flags;
        memcpy(m.mat, r.mat, sizeof(m.mat));
        for (int p = r.parent; p >= 0 && size_t(p) < records.size(); p = records[p].parent) {
            float world[9];
            Compose(records[p].mat, m.mat, world);
            memcpy(m.mat, world, sizeof(world));
        }
        m.uses = 1;
        for (uint32_t q = r.mesh; q && m.parts < 64 && q + kRecord <= src->prm.size(); q = U32(src->prm, q + 8))
            ++m.parts;
        uint32_t bounds = U32(src->prm, r.mesh + 0x18);
        if (bounds && bounds + 0x3C <= src->prm.size()) {
            float b[6];
            memcpy(b, &src->prm[bounds + 0x24], sizeof(b));
            float dx = b[0] - b[3], dy = b[1] - b[4], dz = b[2] - b[5];
            float d2 = dx * dx + dy * dy + dz * dz;
            if (d2 > 0 && d2 < 1e12f)
                m.radius = 0.5f * sqrtf(d2);
        }
        cat.models.push_back(std::move(m));
    }
    Log("imports: %s: %zu models", level.c_str(), cat.models.size());
}

} // namespace

void SetImportProvider(std::function<std::vector<ModelImport>(const std::string&)> provider)
{
    g_provider = std::move(provider);
}

const std::vector<LevelModel>& LevelModels(const std::string& level)
{
    Catalogue& c = g_catalogues[level];
    if (!c.built)
        BuildCatalogue(level, c);
    return c.models;
}

const std::vector<std::string>& GameLevels()
{
    static std::vector<std::string> levels;
    if (!levels.empty())
        return levels;
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((GameDir() + "Scenes\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE)
        return levels;
    do {
        std::string d = fd.cFileName;
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || d == "." || d == "..")
            continue;
        if (GetFileAttributesA((GameDir() + "Scenes\\" + d + "\\" + d + "__MAIN.ZIP").c_str()) !=
            INVALID_FILE_ATTRIBUTES)
            levels.push_back(d);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    std::sort(levels.begin(), levels.end());
    return levels;
}

void SetModelImports(const std::vector<ModelImport>& list) { g_requests = list; }

uint32_t ImportedMesh(const std::string& level, uint32_t mesh)
{
    auto it = g_meshes.find({level, mesh});
    return it == g_meshes.end() ? 0 : it->second;
}

void InstallImports()
{
    Hook("GetPrimsSize", kGetPrimsSize, &hkPrimsSize, &oPrimsSize);
    Hook("GetPrimsData", kGetPrimsData, &hkPrimsData, &oPrimsData);
    Hook("GetTextureSize", kGetTextureSize, &hkTexSize, &oTexSize);
    Hook("GetTextureData", kGetTextureData, &hkTexData, &oTexData);
}

} // namespace h2::engine
