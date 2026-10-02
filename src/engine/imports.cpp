#include "imports.h"

#include <windows.h>
#include <algorithm>
#include <cstring>
#include <map>
#include <set>

#include "../hook.h"
#include "../log.h"
#include "zip.h"

namespace h2::engine {

namespace {

constexpr uintptr_t kGetPrimsSize = 0x41BFF0;    // ZEngineDataBase::(void) -> size
constexpr uintptr_t kGetPrimsData = 0x41C080;    // ZEngineDataBase::(void* buf, uint size)
constexpr uintptr_t kGetTextureSize = 0x41C270;
constexpr uintptr_t kGetTextureData = 0x41C300;
constexpr uintptr_t kGetSceneName = 0x41B880;    // ZEngineDataBase::(void) -> const char*
constexpr uint32_t kRuntimeReserve = 0x60000;    // AllocSequence's room for runtime prims
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
    Log("imports: read %s (PRM %zu, TEX %zu bytes)", level.c_str(), s.prm.size(), s.tex.size());
    return &s;
}

std::vector<ModelImport> g_requests;

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
    return !g_requests.empty() && n.find("__MAIN") != std::string::npos;
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
    g_base = (real + kRuntimeReserve + 15) & ~15u;
    g_blob.clear();
    g_texUses.clear();
    g_texMap.clear();
    g_pending.clear();
    for (const ModelImport& m : g_requests)
        Copy(m);
    if (g_blob.empty())
        return real;
    g_active = true;
    return real + (g_base - real - kRuntimeReserve) + uint32_t(g_blob.size());
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
    memcpy(static_cast<uint8_t*>(buf) + g_base, g_blob.data(), g_blob.size());
    g_meshes = g_pending;
    g_active = false;
    Log("imports: %zu models placed at %08X (%zu bytes)", g_meshes.size(), g_base, g_blob.size());
}

} // namespace

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
