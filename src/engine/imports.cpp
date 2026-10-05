#include "imports.h"

#include <windows.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <set>

#include "../game.h"
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
constexpr uintptr_t kGetTreesSize = 0x41BD80;    // OCT: bound trees and collision data
constexpr uintptr_t kGetTreesData = 0x41BE10;
constexpr uintptr_t kGetSceneName = 0x41B880;    // ZEngineDataBase::(void) -> const char*
// AllocSequence keeps 0x60000 bytes after the file's prims for the ones made
// at runtime (from header[1], saved in save games): the copies go after it,
// header[1] stays, so save games are the same with or without them.
constexpr uint32_t kRuntimeReserve = 0x60000;
constexpr uint32_t kRecord = 0x84;               // a PRM record (one material of a model)
constexpr int kTexIds = 2048;

using SizeFn = uint32_t(__fastcall*)(void* self, void* edx);
using DataFn = void(__fastcall*)(void* self, void* edx, void* buf, uint32_t size);
SizeFn oPrimsSize, oTexSize, oTreesSize;
DataFn oPrimsData, oTexData, oTreesData;

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
    std::vector<uint8_t> prm, tex, oct;
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
    if (!ReadZipEntry(zip, base + "PRM", s.prm, &err) || !ReadZipEntry(zip, base + "TEX", s.tex, &err) ||
        !ReadZipEntry(zip, base + "OCT", s.oct, &err)) {
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
// Collision: a copied record's +0x30 points into its level's OCT, whose
// references are relative to themselves: the whole OCT of that level is
// added after this level's, and the field moved by where it went.
struct CollisionUse {
    size_t at;  // in the blob
    std::string level;
    uint32_t offset;  // in that level's OCT
};
std::vector<CollisionUse> g_collisions;
std::map<std::pair<std::string, uint32_t>, uint32_t> g_recordMap;  // (level, record) -> its copy here
uint8_t* g_primBuf;
bool g_treesPending;
uint32_t g_treesReal;
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
        return false;  // menus: the level's models stay as they were
    g_meshes.clear();  // a level: nothing of the one before
    if (g_provider)
        g_load = g_provider(n);
    g_load.insert(g_load.end(), g_requests.begin(), g_requests.end());
    Log("imports: %s gets %zu models of other levels", n.c_str(), g_load.size());
    return !g_load.empty();
}

struct Chain {
    size_t pos = 0;                // where its span went in the blob
    std::vector<size_t> records;   // its records in the blob
    uint32_t mesh = 0;             // the first record's offset here
};

// A model's records and data copied into the blob, offsets moved. `main`:
// its collision is used and its records answer for the level's triangle lists.
bool CopyChain(const std::string& level, uint32_t first, bool main, Chain& out)
{
    Source* s = LoadSource(level);
    if (!s)
        return false;
    const std::vector<uint8_t>& prm = s->prm;
    ModelImport m{level, first};
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
    out.pos = pos;
    out.records.clear();
    for (uint32_t r : records) {
        size_t q = pos + (r - lo);
        out.records.push_back(q);
        if (main)
            g_recordMap[{m.level, r}] = g_base + uint32_t(q);
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
        uint32_t coll = U32(g_blob, q + 0x30);
        Put(g_blob, q + 0x30, 0);  // set when the OCT is read (GetTreesData)
        if (coll && coll < s->oct.size() && main)
            g_collisions.push_back({q + 0x30, m.level, coll});
        // +0x34: what the record's triangles stop, tested against a query's
        // mask (0x20: characters walking). Models often leave walking to
        // invisible boxes of their level: a placed copy is solid like a wall
        // (0xFB) itself.
        if (U32(prm, records[0] + 0x30) && main)
            Put(g_blob, q + 0x34, U32(g_blob, q + 0x34) | 0x68);
        uint32_t tex = U32(g_blob, q + 0x04) & 0x7FF;
        if (s->HasTexture(tex))
            g_texUses.push_back({q + 0x04, 0x7FF, m.level, tex});
    }
    out.mesh = g_base + uint32_t(pos) + (m.mesh - lo);
    Log("imports: %s %08X -> %08X (%zu records, %u bytes%s)", m.level.c_str(), m.mesh, out.mesh, records.size(),
        hi - lo, outside ? ", some references outside dropped" : "");
    return true;
}

// the editor's convention: a local vector (x, y, z) is x * row2 + y * row1 + z * row0
void ToParent(const float mat[9], const float pos[3], const float v[3], float out[3], bool point)
{
    for (int k = 0; k < 3; ++k)
        out[k] = mat[6 + k] * v[0] + mat[3 + k] * v[1] + mat[k] * v[2] + (point ? pos[k] : 0.0f);
}
void FromParent(const float mat[9], const float pos[3], const float v[3], float out[3], bool point)
{
    float d[3];
    for (int k = 0; k < 3; ++k)
        d[k] = v[k] - (point ? pos[k] : 0.0f);
    out[0] = d[0] * mat[6] + d[1] * mat[7] + d[2] * mat[8];
    out[1] = d[0] * mat[3] + d[1] * mat[4] + d[2] * mat[5];
    out[2] = d[0] * mat[0] + d[1] * mat[1] + d[2] * mat[2];
}

constexpr size_t kVertex = 36;  // position, normal, colour, uv

// a group: its parts copied, the others' vertices moved into the main
// part's frame, their records chained after the main part's, bounds of all
bool CopyGroup(const ModelImport& m)
{
    const LevelModel* model = nullptr;
    for (const LevelModel& lm : LevelModels(m.level))
        if (lm.mesh == m.mesh)
            model = &lm;
    if (!model || model->pieces.empty()) {
        Log("imports: %s %08X: no such group", m.level.c_str(), m.mesh);
        return false;
    }
    const LevelModelPart& main = model->pieces[0];
    std::vector<Chain> chains(model->pieces.size());
    if (!CopyChain(m.level, main.mesh, true, chains[0]))
        return false;
    float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
    std::set<uint32_t> moved;
    auto bounds = [&](const Chain& c, const LevelModelPart* part) {
        for (size_t q : c.records) {
            uint32_t verts = U32(g_blob, q + 0x1C), count = U32(g_blob, q + 0x14) >> 16;
            if (verts < g_base)
                continue;
            size_t at = verts - g_base;
            if (at + count * kVertex > g_blob.size())
                continue;
            for (uint32_t i = 0; i < count; ++i) {
                float* v = reinterpret_cast<float*>(&g_blob[at + i * kVertex]);
                // records may share (overlapping) vertex data: each vertex once
                if (part && moved.insert(uint32_t(at + i * kVertex)).second) {
                    float g[3], n[3];
                    ToParent(part->mat, part->pos, v, g, true);
                    FromParent(main.mat, main.pos, g, v, true);
                    ToParent(part->mat, part->pos, v + 3, n, false);
                    FromParent(main.mat, main.pos, n, v + 3, false);
                }
                for (int k = 0; k < 3; ++k) {
                    lo[k] = std::min(lo[k], v[k]);
                    hi[k] = std::max(hi[k], v[k]);
                }
            }
        }
    };
    if (main.visible)
        bounds(chains[0], nullptr);
    for (size_t i = 1; i < model->pieces.size(); ++i) {
        if (!CopyChain(m.level, model->pieces[i].mesh, false, chains[i]))
            continue;
        bounds(chains[i], &model->pieces[i]);  // moves an invisible part's vertices too (unused)
    }
    // One new model of the drawn parts, laid out as the level files do it (the
    // renderer keeps a model's vertices together: parts far apart in the
    // buffer draw wrong): each record's vertices and indices, the bounds
    // block, the records (the first one last); the first record gets
    // pieces[0]'s collision.
    std::vector<size_t> drawn;  // records (blob offsets) in drawing order
    for (size_t i = 0; i < chains.size(); ++i)
        if (model->pieces[i].visible)
            drawn.insert(drawn.end(), chains[i].records.begin(), chains[i].records.end());
    if (drawn.empty())
        return false;
    size_t region = (g_blob.size() + 15) & ~size_t(15);
    std::vector<uint8_t> out;
    std::vector<uint32_t> verts(drawn.size()), indices(drawn.size());
    for (size_t k = 0; k < drawn.size(); ++k) {
        size_t q = drawn[k];
        uint32_t v = U32(g_blob, q + 0x1C), count = U32(g_blob, q + 0x14) >> 16;
        uint32_t ix = U32(g_blob, q + 0x44), words = U32(g_blob, q + 0x48);
        verts[k] = indices[k] = 0;
        if (v >= g_base && v - g_base + size_t(count) * kVertex <= g_blob.size()) {
            verts[k] = g_base + uint32_t(region + out.size());
            out.insert(out.end(), g_blob.begin() + (v - g_base), g_blob.begin() + (v - g_base) + count * kVertex);
        }
        if (ix >= g_base && ix - g_base + size_t(words) * 2 <= g_blob.size()) {
            indices[k] = g_base + uint32_t(region + out.size());
            out.insert(out.end(), g_blob.begin() + (ix - g_base), g_blob.begin() + (ix - g_base) + words * 2);
        }
        while (out.size() & 3)
            out.push_back(0);
    }
    uint32_t box = 0, oldBox = U32(g_blob, drawn[0] + 0x18);
    if (oldBox >= g_base && oldBox - g_base + 0x3C <= g_blob.size()) {
        box = g_base + uint32_t(region + out.size());
        out.insert(out.end(), g_blob.begin() + (oldBox - g_base), g_blob.begin() + (oldBox - g_base) + 0x3C);
        memcpy(&out[box - g_base - region + 0x24], hi, 12);
        memcpy(&out[box - g_base - region + 0x30], lo, 12);
    }
    while (out.size() & 3)
        out.push_back(0);
    std::vector<uint32_t> at(drawn.size());  // the new records, the first one last
    for (size_t k = drawn.size(); k-- > 0;) {
        at[k] = g_base + uint32_t(region + out.size());
        out.insert(out.end(), g_blob.begin() + drawn[k], g_blob.begin() + drawn[k] + kRecord);
    }
    std::vector<TexUse> newUses;
    for (size_t k = 0; k < drawn.size(); ++k) {
        size_t q = at[k] - g_base - region;
        auto put = [&](size_t f, uint32_t v) { memcpy(&out[q + f], &v, 4); };
        put(0x08, k + 1 < drawn.size() ? at[k + 1] : 0);
        put(0x18, k == 0 ? box : 0);
        put(0x1C, verts[k]);
        put(0x2C, 0);
        put(0x30, 0);
        put(0x44, indices[k]);
        // +0x04 above 16 bits: the record's number in its model (the renderer
        // keeps a model's parts by it): numbered again through the parts
        uint32_t tex;
        memcpy(&tex, &out[q + 0x04], 4);
        put(0x04, (tex & 0xFFFF) | uint32_t(k) << 16);
        for (const TexUse& t : g_texUses)
            if (t.at == drawn[k] + 0x04)
                newUses.push_back({at[k] - g_base + 0x04, t.mask, t.level, t.id});
    }
    g_texUses.insert(g_texUses.end(), newUses.begin(), newUses.end());
    size_t from = chains[0].records[0] + 0x30;
    for (CollisionUse& c : g_collisions)
        if (c.at == from)
            c.at = at[0] - g_base + 0x30;
    g_blob.resize(region);
    g_blob.insert(g_blob.end(), out.begin(), out.end());
    g_pending[{m.level, m.mesh}] = at[0];
    Log("imports: %s group %s: %zu parts", m.level.c_str(), model->name.c_str(), model->pieces.size());
    return true;
}

// A character's body: a chain of skinned records (type 0x10001, 0xA8 bytes;
// +0x84 bones) referring to bounds (+0x18), vertices (+0x1C, 52 bytes),
// index strips (+0x44, +0x48 words), per-bone blocks (+0x88..+0x98: 48 / 28 /
// 48 / 36 / 48 bytes a bone), the skeleton (+0x9C: a bone map and matrices,
// up to the next model's records), a size table (+0xA4, +0xA0 pairs); the
// first record's +0x3C: a LOD table (8 lists {u32 count, records}).
constexpr uint32_t kSkinned = 0x10001;
constexpr uint32_t kSkinRecord = 0xA8;

bool CopySkinned(const ModelImport& m)
{
    Source* s = LoadSource(m.level);
    if (!s)
        return false;
    const std::vector<uint8_t>& prm = s->prm;
    std::vector<uint32_t> records;
    for (uint32_t r = m.mesh; r && records.size() < 64; r = U32(prm, r + 8)) {
        if (r + kSkinRecord > prm.size() || U32(prm, r) != kSkinned ||
            std::find(records.begin(), records.end(), r) != records.end())
            break;
        records.push_back(r);
    }
    if (records.empty()) {
        Log("imports: %s %08X: no such body", m.level.c_str(), m.mesh);
        return false;
    }
    // the blocks the records refer to and their sizes (the shared ones far
    // away in the file, e.g. index lists, are copied on their own)
    std::map<uint32_t, uint32_t> blocks;  // file offset -> size
    auto want = [&](uint32_t at, uint32_t size) {
        if (!at || !size || at + size > prm.size() || size > 0x400000)
            return false;
        uint32_t& b = blocks[at];
        b = std::max(b, size);
        return true;
    };
    for (uint32_t r : records) {
        uint32_t bones = U32(prm, r + 0x84), count = U32(prm, r + 0x14) >> 16;
        want(U32(prm, r + 0x18), 0x3C);              // bounds
        want(U32(prm, r + 0x1C), count * 52);        // vertices
        want(U32(prm, r + 0x44), U32(prm, r + 0x48) * 2);  // index strips
        static const uint32_t kPerBone[] = {48, 28, 48, 36, 48};  // +0x88 .. +0x98
        for (int i = 0; i < 5; ++i)
            want(U32(prm, r + 0x88 + i * 4), bones * kPerBone[i]);
        want(U32(prm, r + 0xA4), U32(prm, r + 0xA0) * 8);  // size table
        // the skeleton: up to the next skinned record after it
        if (uint32_t sk = U32(prm, r + 0x9C); sk && !blocks.count(sk)) {
            uint32_t end = sk + 0x50;
            while (end + 0x88 <= prm.size() && end < sk + 0x8000 &&
                   !(U32(prm, end) == kSkinned && U32(prm, end + 0x84) == bones))
                end += 2;
            want(sk, end - sk);
        }
    }
    uint32_t lods = U32(prm, records[0] + 0x3C);
    std::vector<std::pair<uint32_t, uint32_t>> lists;  // file offset, count
    if (lods && lods + 0x20 <= prm.size())
        for (int i = 0; i < 8; ++i) {
            uint32_t list = U32(prm, lods + i * 4), count = list && list + 4 <= prm.size() ? U32(prm, list) : 0;
            if (list && count <= 64 && list + 4 + count * 4 <= prm.size())
                lists.push_back({list, count});
        }
    // the blob: blocks, records, LOD table and lists; offsets moved by a map
    std::map<uint32_t, uint32_t> moved;  // file offset -> offset here
    auto place = [&](uint32_t from, uint32_t size) {
        size_t pos = (g_blob.size() + 3) & ~size_t(3);
        g_blob.resize(pos + size);
        memcpy(&g_blob[pos], &prm[from], size);
        moved[from] = g_base + uint32_t(pos);
        return pos;
    };
    for (auto& [at, size] : blocks)
        place(at, size);
    std::vector<size_t> copies;
    for (uint32_t r : records)
        copies.push_back(place(r, kSkinRecord));
    size_t lodAt = 0;
    std::map<uint32_t, size_t> listAt;
    if (lods) {
        lodAt = place(lods, 0x20);
        for (auto& [list, count] : lists)
            if (!listAt.count(list))
                listAt[list] = place(list, 4 + count * 4);
    }
    int outside = 0;
    auto move = [&](size_t at) {
        uint32_t v = U32(g_blob, at);
        if (!v)
            return;
        auto it = moved.find(v);
        Put(g_blob, at, it != moved.end() ? it->second : 0);
        outside += it == moved.end();
    };
    for (size_t k = 0; k < records.size(); ++k) {
        size_t q = copies[k];
        for (size_t f : {0x08, 0x18, 0x1C, 0x3C, 0x44, 0x88, 0x8C, 0x90, 0x94, 0x98, 0x9C, 0xA4})
            move(q + f);
        uint32_t tex = U32(g_blob, q + 0x04) & 0x7FF;
        if (s->HasTexture(tex))
            g_texUses.push_back({q + 0x04, 0x7FF, m.level, tex});
    }
    if (lods) {
        for (int i = 0; i < 8; ++i)
            move(lodAt + i * 4);
        for (auto& [list, at] : listAt)
            for (uint32_t k = 0, n = U32(g_blob, at); k < n && k < 64; ++k)
                move(at + 4 + k * 4);
    }
    uint32_t mesh = moved[m.mesh];
    g_pending[{m.level, m.mesh}] = mesh;
    Log("imports: %s body %08X -> %08X (%zu records, %zu blocks%s)", m.level.c_str(), m.mesh, mesh, records.size(),
        blocks.size(), outside ? ", some references not followed" : "");
    return true;
}

bool Copy(const ModelImport& m)
{
    if (m.mesh & kGroupModel)
        return CopyGroup(m);
    if (Source* s = LoadSource(m.level); s && U32(s->prm, m.mesh) == kSkinned)
        return CopySkinned(m);
    Chain c;
    if (!CopyChain(m.level, m.mesh, true, c))
        return false;
    g_pending[{m.level, m.mesh}] = c.mesh;
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
    g_collisions.clear();
    g_recordMap.clear();
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

// OCT: room trees, then collision entries {u32 size, object, ..., +0x20 data}
// up to a u32 -1, then the level's triangle pool (8-byte items): the
// collision manager keeps its start (vfunc 0x44). An entry's octree (data
// + 0x14: 6-byte nodes {u16 items << 3 | flags (0x8000 last child, 7
// octant), u16 first child, u16 first item}) has 16-byte items (data +
// [data]) whose first u32 is an offset in the pool.
uint32_t PoolStart(const uint8_t* oct, uint32_t size, uint32_t entry)
{
    for (int i = 0; i < 400000 && entry + 4 <= size; ++i) {
        uint32_t s;
        memcpy(&s, oct + entry, 4);
        if (s == 0xFFFFFFFF)
            return entry + 4;
        if (s < 4 || entry + s > size)
            return 0;
        entry += s;
    }
    return 0;
}

// The pool offsets of an entry's items moved by `delta`; the offsets they had.
std::vector<uint32_t> ShiftLeaves(uint8_t* buf, uint32_t size, uint32_t entry, uint32_t delta)
{
    auto u16 = [&](uint32_t at) {
        uint16_t v = 0;
        if (at + 2 <= size)
            memcpy(&v, buf + at, 2);
        return v;
    };
    std::vector<uint32_t> had;
    uint32_t data = entry + 0x20, rel = 0;
    if (data + 0x1A > size)
        return had;
    memcpy(&rel, buf + data, 4);
    uint32_t items = data + rel, nodes = data + 0x14;
    std::set<uint32_t> leaves;
    std::vector<uint32_t> stack{nodes};
    std::set<uint32_t> seen;
    while (!stack.empty() && seen.size() < 100000) {
        uint32_t n = stack.back();
        stack.pop_back();
        if (!seen.insert(n).second || n + 6 > size)
            continue;
        uint16_t a = u16(n), child = u16(n + 2), first = u16(n + 4);
        for (uint32_t k = 0; k < uint32_t((a & 0x7FF8) >> 3); ++k)
            leaves.insert(first + k);
        if (child)
            for (uint32_t c = nodes + child * 6u; c + 6 <= size; c += 6) {
                stack.push_back(c);
                if (u16(c) & 0x8000)
                    break;
            }
    }
    for (uint32_t i : leaves) {
        uint32_t at = items + i * 16, v;
        if (at + 4 > size)
            continue;
        memcpy(&v, buf + at, 4);
        had.push_back(v);
        v += delta;
        memcpy(buf + at, &v, 4);
    }
    return had;
}

// A pool item {u16 count, u16 first index (| 0x8000 flag), u32 PRM record}:
// the triangles of a record's index list. Moved to the record's copy, or
// made empty (count 2 from 0: the test's loop ends at once).
bool MoveItem(uint8_t* item, const std::string& level)
{
    uint32_t head, record;
    memcpy(&head, item, 4);
    memcpy(&record, item + 4, 4);
    auto it = g_recordMap.find({level, record});
    if (it != g_recordMap.end()) {
        memcpy(item + 4, &it->second, 4);
        return true;
    }
    head = (head & 0x80000000) | 2;
    memcpy(item, &head, 4);
    if (!g_recordMap.empty())
        memcpy(item + 4, &g_recordMap.begin()->second, 4);
    return false;
}

bool InitCollisionEntry(void* manager, uint8_t* entry)
{
    __try {
        using Fn = uint8_t*(__thiscall*)(void*, uint8_t*);
        (*reinterpret_cast<Fn**>(manager))[0x50 / 4](manager, entry);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

uint32_t __fastcall hkTreesSize(void* self, void* edx)
{
    uint32_t real = oTreesSize(self, edx);
    if (!g_treesPending || int(real) <= 0)
        return real;
    g_treesReal = real;
    uint32_t extra = 0;
    std::set<std::string> levels;
    for (const CollisionUse& c : g_collisions)
        if (levels.insert(c.level).second)
            extra += uint32_t(g_sources[c.level].oct.size()) + 16;
    return real + extra;
}

void __fastcall hkTreesData(void* self, void* edx, void* buf, uint32_t size)
{
    if (!g_treesPending) {
        oTreesData(self, edx, buf, size);
        return;
    }
    g_treesPending = false;
    oTreesData(self, edx, buf, g_treesReal);
    // the size stays the whole buffer: collision objects outside it are not used
    auto b = static_cast<uint8_t*>(buf);
    std::map<std::string, uint32_t> at;
    uint32_t cursor = (g_treesReal + 15) & ~15u;
    for (const CollisionUse& c : g_collisions) {
        if (at.count(c.level))
            continue;
        const std::vector<uint8_t>& oct = g_sources[c.level].oct;
        if (cursor + oct.size() > size)
            break;
        memcpy(b + cursor, oct.data(), oct.size());
        at[c.level] = cursor;
        cursor = (cursor + uint32_t(oct.size()) + 15) & ~15u;
    }
    // A record's +0x30 is its collision object + 1 (the object is an entry's
    // body: {u32 size, object (vtable written when loading), ...}); the
    // collision manager's vfunc 0x50 (entry) does that for this level's
    // entries in CreateBoundTrees, for the copies it is done here.
    void* manager = nullptr;
    if (auto sys = reinterpret_cast<char*>(game::SysInterface()))
        if (auto renderer = *reinterpret_cast<char**>(sys + 0x18A0))
            manager = *reinterpret_cast<void**>(renderer + 0x1C);
    // this level's pool: found from an entry of one of its own records
    uint32_t pool = 0;
    if (g_primBuf) {
        uint32_t table, count;
        memcpy(&table, g_primBuf + 8, 4);
        memcpy(&count, g_primBuf + 12, 4);
        for (uint32_t i = 0; i < count && !pool; ++i) {
            uint32_t r, v;
            memcpy(&r, g_primBuf + table + i * 4, 4);
            memcpy(&v, g_primBuf + r + 0x30, 4);
            if (v >= 5 && v < g_treesReal)
                pool = PoolStart(b, g_treesReal, v - 5);
        }
    }
    std::map<std::string, uint32_t> delta;    // level -> pool offsets moved by
    std::map<std::string, uint32_t> srcPools;  // level -> its pool in this buffer
    std::set<uint32_t> movedItems;
    int set = 0, ready = 0, leaves = 0, foreign = 0;
    std::set<uint32_t> entries;
    for (const CollisionUse& c : g_collisions) {
        auto it = at.find(c.level);
        if (it == at.end() || !g_primBuf || !pool)
            continue;
        uint32_t v = it->second + c.offset;
        if (!delta.count(c.level)) {
            const std::vector<uint8_t>& oct = g_sources[c.level].oct;
            uint32_t srcPool = PoolStart(oct.data(), uint32_t(oct.size()), c.offset - 5);
            if (!srcPool) {
                Log("imports: %s: collision data not understood", c.level.c_str());
                delta[c.level] = 0;
            } else {
                delta[c.level] = it->second + srcPool - pool;
                srcPools[c.level] = it->second + srcPool;
            }
        }
        if (!delta[c.level] && it->second)
            continue;  // no collision rather than a wrong one
        memcpy(g_primBuf + g_base + c.at, &v, 4);
        ++set;
        if (entries.insert(v - 5).second) {
            for (uint32_t off : ShiftLeaves(b, size, v - 5, delta[c.level])) {
                ++leaves;
                uint32_t item = srcPools[c.level] + off;
                if (item + 8 <= size && movedItems.insert(item).second && !MoveItem(b + item, c.level))
                    ++foreign;
            }
            if (manager && InitCollisionEntry(manager, b + v - 5))
                ++ready;
        }
    }
    if (!manager)
        Log("imports: no collision manager, the copies have no collision objects");
    Log("imports: collision of %d records, %d objects, %d leaves, %zu triangle lists (%d of other models left out); "
        "%zu levels' OCT added after %u bytes, pool at %X",
        set, ready, leaves, movedItems.size(), foreign, at.size(), g_treesReal, pool);
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
    // free ids among the level's own (16..1998 in every level; the ones above
    // are the engine's at runtime: a texture there draws as an effect)
    constexpr uint32_t kFirstId = 16, kLastId = 1998;
    uint32_t free = kFirstId;
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
        while (free <= kLastId && *slot(free))
            ++free;
        uint32_t n = s.TextureSize(u.id);
        if (free > kLastId || cursor + n > size) {
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
        ++free;
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
    g_primBuf = b;
    g_treesPending = !g_collisions.empty();
    g_meshes = g_pending;
    g_active = false;
    Log("imports: %zu models placed at %08X (%zu bytes)", g_meshes.size(), g_base, g_blob.size());
}

// ---- catalogue of another level ----

struct Catalogue {
    bool built = false;
    std::vector<LevelModel> models;
    std::vector<LevelCharacter> characters;
};

// the string values of a typed data block, in order (tag 4 / 5)
std::vector<std::string> BlockStrings(const uint8_t* b, size_t n)
{
    std::vector<std::string> out;
    size_t p = 4;
    while (p < n && out.size() < 16) {
        uint8_t t = b[p];
        if (t == 0xFF)
            break;
        switch (t & 0x3F) {
        case 1:
            p += 9;
            break;
        case 2: case 3: case 8: case 10: case 11:
            p += 5;
            break;
        case 4: case 5: {
            size_t e = p + 1;
            while (e < n && b[e])
                ++e;
            out.emplace_back(reinterpret_cast<const char*>(b + p + 1), e - p - 1);
            p = e + 1;
            break;
        }
        case 7: case 9: case 0xC: {
            uint32_t len = 0;
            if (p + 5 <= n)
                memcpy(&len, b + p + 1, 4);
            p += 5 + len;
            break;
        }
        default:
            p += 1;
        }
    }
    return out;
}
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

// a record's transform in the frame of its group `group` (an ancestor)
void InGroup(const std::vector<SceneRecord>& records, size_t k, int group, float mat[9], float pos[3])
{
    memcpy(mat, records[k].mat, 36);
    memcpy(pos, records[k].pos, 12);
    for (int p = records[k].parent; p >= 0 && p != group && size_t(p) < records.size(); p = records[p].parent) {
        const SceneRecord& g = records[p];
        float m[9], v[3];
        Compose(g.mat, mat, m);
        for (int i = 0; i < 3; ++i)
            v[i] = g.mat[6 + i] * pos[0] + g.mat[3 + i] * pos[1] + g.mat[i] * pos[2] + g.pos[i];
        memcpy(mat, m, 36);
        memcpy(pos, v, 12);
    }
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
    std::map<uint32_t, size_t> bodies;
    for (const SceneRecord& r : records) {
        if (r.classId != 0x2003E9 || !r.mesh || r.mesh + 4 > src->prm.size() || U32(src->prm, r.mesh) != kSkinned ||
            !r.data || r.data + 4 > gms.size())
            continue;
        size_t n = std::min<size_t>(U32(gms, r.data) & 0xFFFFFF, gms.size() - r.data);
        std::string model = ModelNameOf(&gms[r.data], n), set = AnimSetOf(&gms[r.data], n);
        if (model.empty() || set.empty())
            continue;
        auto [it, added] = bodies.emplace(r.mesh, cat.characters.size());
        if (!added) {
            cat.characters[it->second].uses++;
            continue;
        }
        LevelCharacter c;
        c.mesh = r.mesh;
        c.model = model;
        c.animSet = set;
        if (r.parent >= 0)
            c.group = records[r.parent].name;
        c.uses = 1;
        cat.characters.push_back(c);
    }
    std::map<uint32_t, size_t> byMesh;
    std::map<int, std::vector<size_t>> groupParts;  // group record -> its models' records
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
        bool visible = (r.flags & 1) != 0;  // drawn (invisible: collision bodies, emitters)
        bool collides = U32(src->prm, r.mesh + 0x30) != 0;
        if (visible || collides)  // a part of each group above it (a wheel in its own group too)
            for (int p = r.parent; p >= 0 && size_t(p) < records.size(); p = records[p].parent) {
                if (chain(records[p].classId).find("ZROOM") != std::string::npos)
                    break;
                groupParts[p].push_back(size_t(r.index));
            }
        if (!visible)
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
        m.collision = U32(src->prm, r.mesh + 0x30) != 0;
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
    // groups of 2..24 models (a vehicle and its wheels): one model of them all
    size_t singles = cat.models.size();
    std::set<std::vector<uint32_t>> seenGroups;
    auto radiusOf = [&](uint32_t mesh) {
        uint32_t bounds = U32(src->prm, mesh + 0x18);
        if (!bounds || bounds + 0x3C > src->prm.size())
            return 0.0f;
        float b[6];
        memcpy(b, &src->prm[bounds + 0x24], sizeof(b));
        float dx = b[0] - b[3], dy = b[1] - b[4], dz = b[2] - b[5];
        float d2 = dx * dx + dy * dy + dz * dz;
        return d2 > 0 && d2 < 1e12f ? 0.5f * sqrtf(d2) : 0.0f;
    };
    for (auto& [g, parts] : groupParts) {
        int drawn = 0;
        for (size_t k : parts)
            drawn += (records[k].flags & 1) != 0;
        if (drawn < 2 || parts.size() > 24)
            continue;
        // the frame: an invisible body with collision, else the largest drawn part
        size_t mainPart = SIZE_MAX;
        float best = -1;
        for (size_t k = 0; k < parts.size(); ++k) {
            const SceneRecord& r = records[parts[k]];
            if (!(r.flags & 1) && radiusOf(r.mesh) > best) {
                best = radiusOf(r.mesh);
                mainPart = k;
            }
        }
        if (mainPart == SIZE_MAX)
            for (size_t k = 0; k < parts.size(); ++k)
                if ((records[parts[k]].flags & 1) && radiusOf(records[parts[k]].mesh) > best) {
                    best = radiusOf(records[parts[k]].mesh);
                    mainPart = k;
                }
        std::swap(parts[0], parts[mainPart]);
        std::vector<uint32_t> meshes;
        for (size_t k : parts)
            meshes.push_back(records[k].mesh);
        if (!seenGroups.insert(meshes).second)
            continue;  // the same group again (another instance)
        LevelModel m;
        m.mesh = kGroupModel | uint32_t(g);
        m.name = records[g].name;
        m.group = records[g].parent >= 0 ? records[records[g].parent].name : "";
        const SceneRecord& main = records[parts[0]];
        const SceneRecord* drawnMain = &main;
        for (size_t k : parts)
            if (!(drawnMain->flags & 1) && (records[k].flags & 1))
                drawnMain = &records[k];
        m.classId = drawnMain->classId;
        m.flags = drawnMain->flags;
        m.collision = U32(src->prm, main.mesh + 0x30) != 0;
        memcpy(m.mat, main.mat, sizeof(m.mat));
        for (int p = main.parent; p >= 0 && size_t(p) < records.size(); p = records[p].parent) {
            float world[9];
            Compose(records[p].mat, m.mat, world);
            memcpy(m.mat, world, sizeof(world));
        }
        m.uses = 1;
        float reach = 0;
        for (size_t k : parts) {
            const SceneRecord& r = records[k];
            LevelModelPart part;
            part.mesh = r.mesh;
            part.visible = (r.flags & 1) != 0;
            InGroup(records, k, g, part.mat, part.pos);
            m.pieces.push_back(part);
            const float* o = m.pieces[0].pos;
            float d[3] = {part.pos[0] - o[0], part.pos[1] - o[1], part.pos[2] - o[2]};
            reach = std::max(reach, sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]) + radiusOf(r.mesh));
        }
        m.radius = reach;
        cat.models.push_back(std::move(m));
    }
    Log("imports: %s: %zu models, %zu groups, %zu characters", level.c_str(), singles, cat.models.size() - singles,
        cat.characters.size());
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

const std::vector<LevelCharacter>& LevelCharacters(const std::string& level)
{
    Catalogue& c = g_catalogues[level];
    if (!c.built)
        BuildCatalogue(level, c);
    return c.characters;
}

std::string ModelNameOf(const uint8_t* block, size_t size)
{
    for (const std::string& s : BlockStrings(block, size))
        if (!s.empty() && s.find('\\') == std::string::npos)
            return s;
    return "";
}

std::string AnimSetOf(const uint8_t* block, size_t size)
{
    for (const std::string& s : BlockStrings(block, size)) {
        size_t at = s.find("_Anims\\");
        if (at == std::string::npos)
            continue;
        std::string rest = s.substr(at + 7);
        return rest.substr(0, rest.find('\\'));
    }
    return "";
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
    Hook("GetTreesSize", kGetTreesSize, &hkTreesSize, &oTreesSize);
    Hook("GetTreesData", kGetTreesData, &hkTreesData, &oTreesData);
}

} // namespace h2::engine
