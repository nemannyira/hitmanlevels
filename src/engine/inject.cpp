#include "inject.h"

#include <cstring>
#include <map>
#include <set>

#include "../loader.h"
#include "../log.h"
#include "scene_data.h"

namespace h2::engine {

namespace {

// GMS: header u32s at 0x00 (record list: count, then {u32 offset / 4 |
// pops << 25, u32 light table handle} per record), 0x10 (geom counts: count,
// then {class id, count, count when ForceExtraGeom is off} per class), 0x14
// (nodes: u32 blocks, then per block 24 counts, one block for the root
// group's children and one for each record of a group class (0x100000), in
// list order: the nodes of that group's children, 8 counts per pool: a
// record's node comes from pool byte +0x2D * 8 + NodeKind(class), 0x5BE500).
// 0x28: links (count, then pairs of static node references - 0x70: offsets in
// the node buffer after the root group's node, made with those counts). The
// level's other files (OCT, RMC, RMI, SGP) hold such references too, so the
// nodes made from the scene file must stay where they are: the root's block
// is the buffer's first, and the new groups (root children) get nodes of
// their own after all the others instead (TakeNode).
// A record is 0x30 bytes: name (BUF), matrix, position, mesh, class, events,
// node flags, properties (BUF), data, ?, script, pool.
constexpr uint32_t kActorClass = 0x2003E9;   // ZTRACKLNKHM2
constexpr uint32_t kHelperClass = 0x200002;  // ZSTDOBJ
constexpr uint32_t kOpens = 0x1000000;       // list entry: the next records are its children
constexpr size_t kRecord = 0x30;
constexpr int kPools = 24;
const char* const kActorList = "TrackLinkObjects";
const char* const kAimHelper = "ZAIMTHINGY";

// ZEngineDataBase's node kind of a class (0x5BE500)
int NodeKind(uint32_t cls)
{
    if (cls & 0x200000)
        return cls == 0x200015 || cls == 0x20001C || cls == 0x200012 ? 3 : 1;
    if (cls & 0x100000)
        return 0;
    return int(((~cls & 0x800000) | 0x1000000) >> 23);
}

std::function<std::vector<SceneNpc>(const std::string&)> g_provider;
std::map<std::string, int> g_made;  // name -> actor record (scene loaded last)
std::set<uint32_t> g_groups;        // offsets of the new groups' records in the GMS
struct PlayerStart {
    std::string scene;  // lower case
    float pos[3], mat[9];
};
std::vector<PlayerStart> g_start;   // for the next load (SetPlayerStart)
constexpr uint32_t kHitmanClass = 0x2003E8;  // ZHITMAN2
const uint8_t* g_gms = nullptr;     // the GMS CreateGeoms reads (while it does)

constexpr uintptr_t kTakeNodeCall = 0x41D056;  // CreateGeoms: kind = 0x5BE500(class, flags)
constexpr uintptr_t kNodeKind = 0x5BE500;
constexpr uintptr_t kAllocNodes = 0x426730;    // geom table: (count) -> first of count new static nodes
constexpr uintptr_t kSysInterface = 0x6A6C5C;

// CreateGeoms takes a record's node from its parent's block: cursors[pool * 8
// + kind], then moves that cursor on. A new group gets a node made for it
// instead, the last of the buffer so far.
void __cdecl TakeNode(int kind, const uint8_t* record, uintptr_t* cursors)
{
    if (!g_gms || record < g_gms || !g_groups.count(uint32_t(record - g_gms)))
        return;
    int at = record[0x2D] * 8 + kind;
    uintptr_t engine = *reinterpret_cast<uintptr_t*>(*reinterpret_cast<uintptr_t*>(kSysInterface) + 0x98);
    void* table = *reinterpret_cast<void**>(engine + 4);
    if (at < kPools && table)
        cursors[at] = reinterpret_cast<uintptr_t(__thiscall*)(void*, int)>(kAllocNodes)(table, 1);
}

__declspec(naked) void TakeNodeThunk()
{
    __asm {
        mov eax, kNodeKind
        call eax
        push eax
        push dword ptr [ebp - 0x24]  // the parent's cursors
        push ebx                     // the record
        push eax                     // its kind
        call TakeNode
        add esp, 12
        pop eax
        ret
    }
}

uint32_t U32(const std::vector<uint8_t>& b, size_t at)
{
    uint32_t v = 0;
    if (at + 4 <= b.size())
        memcpy(&v, &b[at], 4);
    return v;
}
void Put(std::vector<uint8_t>& b, size_t at, uint32_t v) { memcpy(&b[at], &v, 4); }

uint32_t Append(std::vector<uint8_t>& b, const void* p, size_t n)
{
    size_t at = (b.size() + 3) & ~size_t(3);
    b.resize(at);
    b.insert(b.end(), static_cast<const uint8_t*>(p), static_cast<const uint8_t*>(p) + n);
    return uint32_t(at);
}
uint32_t Append(std::vector<uint8_t>& b, const std::vector<uint8_t>& v) { return Append(b, v.data(), v.size()); }

uint32_t AppendString(std::vector<uint8_t>& buf, const std::string& s)
{
    uint32_t at = uint32_t(buf.size());
    buf.insert(buf.end(), s.begin(), s.end());
    buf.push_back(0);
    return at;
}

std::vector<uint8_t> Block(const std::vector<uint8_t>& gms, uint32_t at)
{
    uint32_t size = U32(gms, at) & 0xFFFFFF;
    if (!at || size < 4 || at + size > gms.size())
        return {};
    return std::vector<uint8_t>(gms.begin() + at, gms.begin() + at + size);
}

// the first reference array (tag 9: u32 bytes, then references) of a block
bool FindArray(const std::vector<uint8_t>& b, size_t& at, uint32_t& bytes)
{
    size_t i = 4;
    while (i < b.size() && b[i] != 0xFF) {
        uint8_t tag = b[i] & 0x3F;
        if (tag == 9 || tag == 7 || tag == 0xC) {
            uint32_t n = U32(b, i + 1);
            if (tag == 9) {
                at = i;
                bytes = n;
                return i + 1 + n <= b.size() && n >= 4;
            }
            i += 1 + n;
        } else if (tag == 1) {
            i += 9;
        } else if (tag == 2 || tag == 3 || tag == 8 || tag == 10 || tag == 11) {
            i += 5;
        } else if (tag == 4 || tag == 5) {
            for (++i; i < b.size() && b[i]; ++i) {
            }
            ++i;
        } else {
            ++i;
        }
    }
    return false;
}

} // namespace

void SetSceneNpcProvider(std::function<std::vector<SceneNpc>(const std::string& scene)> provider)
{
    g_provider = std::move(provider);
}

int SceneNpcRecord(const std::string& name)
{
    auto it = g_made.find(name);
    return it == g_made.end() ? -1 : it->second;
}

bool IsSceneNpcRecord(int index)
{
    for (const auto& [name, record] : g_made)
        if (record == index || record - 1 == index || record + 1 == index)  // its group, actor, aim helper
            return true;
    return false;
}

void InstallSceneNpcs()
{
    uint8_t original[5], call[5] = {0xE8};
    int32_t rel = int32_t(kNodeKind - (kTakeNodeCall + 5));
    original[0] = 0xE8;
    memcpy(original + 1, &rel, 4);
    rel = int32_t(reinterpret_cast<uintptr_t>(&TakeNodeThunk) - (kTakeNodeCall + 5));
    memcpy(call + 1, &rel, 4);
    PatchBytes(reinterpret_cast<void*>(kTakeNodeCall), original, call, 5, "CreateGeoms node of a scene npc group");
}

void SetSceneNpcGms(const void* gms) { g_gms = static_cast<const uint8_t*>(gms); }

namespace {
std::string LowerCase(std::string s)
{
    for (char& c : s)
        c = char(tolower(static_cast<unsigned char>(c)));
    return s;
}
} // namespace

void SetPlayerStart(const std::string& scene, const float pos[3], const float mat[9])
{
    PlayerStart s;
    s.scene = LowerCase(scene);
    memcpy(s.pos, pos, sizeof(s.pos));
    memcpy(s.mat, mat, sizeof(s.mat));
    g_start.assign(1, s);
}

bool MovePlayerStart(const std::string& scene, std::vector<uint8_t>& gms, const std::vector<uint8_t>& buf)
{
    if (g_start.empty())
        return false;
    PlayerStart s = g_start.front();
    if (s.scene.empty() || LowerCase(scene).find(s.scene) == std::string::npos)
        return false;
    g_start.clear();  // once
    std::vector<SceneRecord> recs = ParseSceneRecords(gms, buf);
    uint32_t list = U32(gms, 0);
    for (size_t i = 0; i < recs.size(); ++i) {
        if (recs[i].classId != kHitmanClass || recs[i].parent < 0)
            continue;
        size_t group = size_t(U32(gms, list + 4 + size_t(recs[i].parent) * 8) & 0xFFFFFF) * 4;
        uint32_t mat = Append(gms, s.mat, sizeof(s.mat));
        uint32_t pos = Append(gms, s.pos, sizeof(s.pos));
        Put(gms, group + 0x04, mat);
        Put(gms, group + 0x08, pos);
        Log("scene: Hitman starts at (%.0f %.0f %.0f)", s.pos[0], s.pos[1], s.pos[2]);
        return true;
    }
    Log("scene: no Hitman in %s, he starts where the level has him", scene.c_str());
    return false;
}

bool AddSceneNpcs(const std::string& scene, std::vector<uint8_t>& gms, std::vector<uint8_t>& buf)
{
    g_made.clear();
    g_groups.clear();
    if (!g_provider)
        return false;
    std::vector<SceneNpc> npcs = g_provider(scene);
    if (npcs.empty())
        return false;
    std::vector<SceneRecord> recs = ParseSceneRecords(gms, buf);
    uint32_t list = U32(gms, 0), count = U32(gms, list);
    if (recs.size() != count || !count) {
        Log("scene npcs: the record list could not be read");
        return false;
    }
    auto entry = [&](int i) { return list + 4 + size_t(i) * 8; };
    auto recordAt = [&](int i) { return size_t(U32(gms, entry(i)) & 0xFFFFFF) * 4; };
    // a list entry: record offset / 4, kOpens, steps up before it (<< 25).
    // The groups open at the end of the list (the root group is the engine's,
    // not a record): the new ones go into the root, all of them closed first.
    int depth = 0;
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t e = U32(gms, entry(int(i)));
        depth -= int(e >> 25);
        if (e & kOpens)
            ++depth;
    }
    if (depth < 0)
        depth = 0;
    std::vector<uint8_t> table(gms.begin() + entry(0), gms.begin() + entry(int(count)));
    std::map<uint32_t, int> moreOfClass;
    std::vector<uint32_t> rootNodes(kPools);          // not in the root's block (TakeNode)
    std::vector<std::vector<uint32_t>> groupBlocks;    // node counts of the new groups' children
    std::vector<uint32_t> listed;  // actor references for TrackLinkObjects
    int added = 0;
    for (const SceneNpc& npc : npcs) {
        if (npc.record < 0 || uint32_t(npc.record) >= count || recs[npc.record].classId != kActorClass ||
            recs[npc.record].parent < 0) {
            Log("scene npcs: %s: no template character (record %d)", npc.name.c_str(), npc.record);
            continue;
        }
        const SceneRecord& actor = recs[npc.record];
        const SceneRecord& group = recs[actor.parent];
        int groupIndex = int(count) + added, actorIndex = groupIndex + 1, helperIndex = groupIndex + 2;
        // its data: its own aim helper (the template's is its only), its body's animations
        std::vector<uint8_t> data = Block(gms, actor.data);
        int helper = -1;
        for (size_t at : BlockRefOffsets(data)) {
            uint32_t ref = U32(data, at), idx = (ref & 0x7FFFFFFF) - 1;
            if ((ref & 0x80000000) && idx < count && recs[idx].name == kAimHelper &&
                recs[idx].classId == kHelperClass) {
                helper = int(idx);
                Put(data, at, 0x80000000 | uint32_t(helperIndex + 1));
                break;
            }
        }
        if (!data.empty() && !npc.anims.empty())
            data = WithStrings(data, npc.anims);
        if (!data.empty())
            Put(data, 0, U32(data, 0) & 0x7FFFFFFF);  // not remapped yet
        // the records: copies of the template's, the level's name, place and body
        std::vector<uint8_t> g(gms.begin() + recordAt(actor.parent), gms.begin() + recordAt(actor.parent) + kRecord);
        std::vector<uint8_t> a(gms.begin() + recordAt(npc.record), gms.begin() + recordAt(npc.record) + kRecord);
        std::vector<uint8_t> h;
        if (helper >= 0)
            h.assign(gms.begin() + recordAt(helper), gms.begin() + recordAt(helper) + kRecord);
        Put(g, 0x00, AppendString(buf, npc.name + "_Group"));
        Put(g, 0x04, Append(gms, npc.mat, sizeof(npc.mat)));
        Put(g, 0x08, Append(gms, npc.pos, sizeof(npc.pos)));
        Put(a, 0x00, AppendString(buf, npc.name));
        if (npc.mesh)
            Put(a, 0x0C, npc.mesh);
        Put(a, 0x20, data.empty() ? 0 : Append(gms, data));
        Put(a, 0x28, 0);  // its behaviour is set after loading
        uint32_t pops = added == 0 ? uint32_t(depth) : 1;
        if (pops > 127) {
            Log("scene npcs: the record list is too deep");
            break;
        }
        auto add = [&](const std::vector<uint8_t>& rec, uint32_t flags, uint32_t extra, std::vector<uint32_t>& nodes) {
            uint32_t off = Append(gms, rec);
            uint32_t e[2] = {(off / 4) | flags, extra};
            table.insert(table.end(), reinterpret_cast<uint8_t*>(e), reinterpret_cast<uint8_t*>(e) + 8);
            ++moreOfClass[U32(rec, 0x10)];
            int pool = rec[0x2D] * 8 + NodeKind(U32(rec, 0x10));
            if (pool < kPools)
                ++nodes[pool];
            ++added;
        };
        groupBlocks.emplace_back(kPools);
        g_groups.insert(uint32_t((gms.size() + 3) & ~size_t(3)));
        add(g, kOpens | (pops << 25), group.extra, rootNodes);
        add(a, 0, actor.extra, groupBlocks.back());
        if (helper >= 0)
            add(h, 0, recs[helper].extra, groupBlocks.back());
        listed.push_back(0x80000000 | uint32_t(actorIndex + 1));
        g_made[npc.name] = actorIndex;
    }
    if (!added)
        return false;
    // geom counts (the engine allocates the geoms of each class at once)
    uint32_t counts = U32(gms, 0x10), classes = U32(gms, counts);
    std::vector<uint8_t> ct(gms.begin() + counts + 4, gms.begin() + counts + 4 + size_t(classes) * 12);
    for (auto& [cls, n] : moreOfClass) {
        bool found = false;
        for (uint32_t k = 0; k < classes && !found; ++k)
            if (U32(ct, k * 12) == cls) {
                Put(ct, k * 12 + 4, U32(ct, k * 12 + 4) + n);
                found = true;
            }
        if (!found) {
            uint32_t e[3] = {cls, uint32_t(n), 0};
            ct.insert(ct.end(), reinterpret_cast<uint8_t*>(e), reinterpret_cast<uint8_t*>(e) + 12);
            ++classes;
        }
    }
    std::vector<uint8_t> countsBlock(4);
    Put(countsBlock, 0, classes);
    countsBlock.insert(countsBlock.end(), ct.begin(), ct.end());
    Put(gms, 0x10, Append(gms, countsBlock));
    // node counts: a block for each group (the groups' own nodes: TakeNode)
    uint32_t nodes = U32(gms, 0x14), blocks = U32(gms, nodes);
    std::vector<uint8_t> nb(gms.begin() + nodes + 4, gms.begin() + nodes + 4 + size_t(blocks) * kPools * 4);
    for (const std::vector<uint32_t>& block : groupBlocks)
        nb.insert(nb.end(), reinterpret_cast<const uint8_t*>(block.data()),
                  reinterpret_cast<const uint8_t*>(block.data()) + kPools * 4);
    std::vector<uint8_t> nodesBlock(4);
    Put(nodesBlock, 0, blocks + uint32_t(groupBlocks.size()));
    nodesBlock.insert(nodesBlock.end(), nb.begin(), nb.end());
    Put(gms, 0x14, Append(gms, nodesBlock));
    // the scene's actor list gets them
    for (uint32_t i = 0; i < count; ++i) {
        if (recs[i].name != kActorList || !recs[i].data)
            continue;
        std::vector<uint8_t> b = Block(gms, recs[i].data);
        size_t at = 0;
        uint32_t bytes = 0;
        if (b.empty() || !FindArray(b, at, bytes)) {
            Log("scene npcs: %s not changed (no reference array)", kActorList);
            continue;
        }
        std::vector<uint8_t> more(listed.size() * 4);
        memcpy(more.data(), listed.data(), more.size());
        b.insert(b.begin() + at + 1 + bytes, more.begin(), more.end());
        Put(b, at + 1, bytes + uint32_t(more.size()));
        Put(b, 0, (U32(b, 0) & 0x7F000000) | uint32_t(b.size()));
        Put(gms, recordAt(int(i)) + 0x20, Append(gms, b));
    }
    // the record list, longer
    std::vector<uint8_t> newList(4);
    Put(newList, 0, count + uint32_t(added));
    newList.insert(newList.end(), table.begin(), table.end());
    Put(gms, 0, Append(gms, newList));
    Log("scene npcs: %d records of %zu characters added to %s", added, listed.size(), scene.c_str());
    return true;
}

} // namespace h2::engine
