#include "inject.h"

#include <cstring>
#include <map>
#include <set>

#include "../loader.h"
#include "../log.h"
#include "geoms.h"
#include "imports.h"
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
std::function<std::vector<SceneList>(const std::string&)> g_listProvider;
std::function<std::vector<SceneCar>(const std::string&)> g_carProvider;
std::function<std::vector<SceneZone>(const std::string&)> g_zoneProvider;
std::map<std::string, int> g_zones;  // name -> its record (scene loaded last)
constexpr uint32_t kBoxClass = 0x2000E1;  // ZBoxPrimitive (0x44A300)
constexpr uint32_t kZoneInside = 0x88;    // CheckInside: Hitman inside (0x48ACF0 / 0x4FC480)
constexpr uint32_t kCarClass = 0x100BBF;    // ZCar (ZGROUP -> ZMoveOnSpline -> ZVehicle -> ZCar)
constexpr uint32_t kShapeClass = 0x4000022;  // ZSHAPE
// C1-1's van's data block (ZCar's settings; no references): for a scene without a car of its own
const uint8_t kCarData[64] = {0x40, 0x00, 0x00, 0x00, 0x83, 0x02, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x80,
                              0x3f, 0x03, 0x01, 0x00, 0x00, 0x00, 0x03, 0x01, 0x00, 0x00, 0x00, 0x03, 0x00,
                              0x00, 0x00, 0x00, 0x06, 0x88, 0xda, 0x0e, 0x00, 0x80, 0x08, 0x00, 0x00, 0x00,
                              0x00, 0x8b, 0x1c, 0x19, 0x01, 0x00, 0x06, 0x08, 0x00, 0x00, 0x00, 0x00, 0x06,
                              0x83, 0x00, 0x00, 0x00, 0x00, 0x82, 0x00, 0x00, 0x82, 0x42, 0x06, 0xff};
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

void SetSceneCarProvider(std::function<std::vector<SceneCar>(const std::string& scene)> provider)
{
    g_carProvider = std::move(provider);
}

// (a CheckInside routine cannot be attached while playing: a loaded save makes such a routine
// again (0x426070) and its load (0x48AC40) reads its data, which the save does not keep: 0x5C0100
// reads from null. The game attaches only routines without data so, e.g. a door's action.)
static std::vector<uint8_t> ZoneData(const float half[3])
{
    // ZBoxPrimitive: its half sizes; CheckInside (data 0x4FC060): Hitman (0: HitmanGround), the
    // receiver its load gives (0: TrackLinkObjects, +0x80 set), its enter / leave messages "0" (the
    // scene's first, remapped when it loads: the game's none, as its own boxes have it), the box
    // hidden (+0x7E: its vfunc 0xC0)
    std::vector<uint8_t> d(4);
    auto tag = [&](uint8_t t, uint32_t v) {
        d.push_back(t);
        d.insert(d.end(), reinterpret_cast<uint8_t*>(&v), reinterpret_cast<uint8_t*>(&v) + 4);
    };
    for (int k = 0; k < 3; ++k) {
        uint32_t bits;
        memcpy(&bits, &half[k], 4);
        tag(0x82, bits);
    }
    d.push_back(0x06);
    tag(0x08, 0);     // checked
    tag(0x08, 0);     // receiver
    tag(0x0A, 0);     // enter
    tag(0x0A, 0);     // leave
    tag(0x03, 1);     // +0x7E hidden
    tag(0x03, 0);     // +0x7F
    tag(0x03, 1);     // +0x80
    tag(0x03, 0);  // +0x81
    d.push_back(0x06);
    d.push_back(0xFF);
    Put(d, 0, uint32_t(d.size()));
    return d;
}

bool InZone(ZGEOM* box)
{
    uint8_t* routine = box ? GeomRoutine(box, "CheckInside") : nullptr;
    return routine && IsReadable(routine + kZoneInside, 1) && routine[kZoneInside] != 0;
}

void SetSceneZoneProvider(std::function<std::vector<SceneZone>(const std::string& scene)> provider)
{
    g_zoneProvider = std::move(provider);
}

int SceneZoneRecord(const std::string& name)
{
    auto it = g_zones.find(name);
    return it == g_zones.end() ? -1 : it->second;
}

bool InSceneZone(const std::string& name)
{
    int record = SceneZoneRecord(name);
    return record >= 0 && InZone(RecordGeom(record));
}

void SetSceneListProvider(std::function<std::vector<SceneList>(const std::string& scene)> provider)
{
    g_listProvider = std::move(provider);
}

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
    g_zones.clear();
    std::vector<SceneNpc> npcs = g_provider ? g_provider(scene) : std::vector<SceneNpc>{};
    std::vector<SceneList> lists = g_listProvider ? g_listProvider(scene) : std::vector<SceneList>{};
    std::vector<SceneCar> cars = g_carProvider ? g_carProvider(scene) : std::vector<SceneCar>{};
    std::vector<SceneZone> zones = g_zoneProvider ? g_zoneProvider(scene) : std::vector<SceneZone>{};
    if (npcs.empty() && lists.empty() && cars.empty() && zones.empty())
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
    auto copyBlock = [&](uint32_t at) -> uint32_t {
        std::vector<uint8_t> b = Block(gms, at);
        if (b.empty())
            return 0;
        Put(b, 0, U32(b, 0) & 0x7FFFFFFF);  // not remapped yet
        return Append(gms, b);
    };
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
    // Outfits of other bodies, as the game's own (HitmanAs/<model>_HMas, a
    // group at the origin with a drawn child): a copy of the template's group
    // and its child at the root, the child showing the body. One for each
    // template group and body; its record index, or -1.
    std::map<std::pair<uint32_t, uint32_t>, int> outfits;
    std::set<std::string> outfitNames;
    auto outfitGroup = [&](uint32_t idx, uint32_t body, const std::string& prefix, const std::string& suffix) -> int {
        if (auto it = outfits.find({idx, body}); it != outfits.end())
            return it->second;
        int child = -1;
        for (uint32_t k = idx + 1; k < count && k < idx + 8; ++k)
            if (recs[k].parent == int(idx) && recs[k].mesh) {
                child = int(k);
                break;
            }
        uint32_t pops = added == 0 ? uint32_t(depth) : 1;
        if (child < 0 || !(recs[idx].classId & 0x100000) || pops > 127)
            return -1;
        std::string name = prefix + suffix;  // (unique: another template group, another name)
        for (int n = 2; outfitNames.count(LowerCase(name)); ++n)
            name = prefix + "_" + std::to_string(n) + suffix;
        outfitNames.insert(LowerCase(name));
        std::vector<uint8_t> g(gms.begin() + recordAt(int(idx)), gms.begin() + recordAt(int(idx)) + kRecord);
        std::vector<uint8_t> c(gms.begin() + recordAt(child), gms.begin() + recordAt(child) + kRecord);
        Put(g, 0x00, AppendString(buf, name));
        Put(g, 0x20, recs[idx].data ? copyBlock(recs[idx].data) : 0);
        Put(c, 0x0C, body);
        Put(c, 0x20, recs[child].data ? copyBlock(recs[child].data) : 0);
        int index = int(count) + added;
        groupBlocks.emplace_back(kPools);
        g_groups.insert(uint32_t((gms.size() + 3) & ~size_t(3)));
        add(g, kOpens | (pops << 25), recs[idx].extra, rootNodes);
        add(c, 0, recs[child].extra, groupBlocks.back());
        outfits[{idx, body}] = index;
        Log("scene npcs: outfit %s (%s with another body)", name.c_str(), recs[idx].name.c_str());
        return index;
    };
    for (const SceneNpc& npc : npcs) {
        if (npc.record < 0 || uint32_t(npc.record) >= count || recs[npc.record].classId != kActorClass ||
            recs[npc.record].parent < 0) {
            Log("scene npcs: %s: no template character (record %d)", npc.name.c_str(), npc.record);
            continue;
        }
        const SceneRecord& actor = recs[npc.record];
        const SceneRecord& group = recs[actor.parent];
        std::vector<uint8_t> data = Block(gms, actor.data);
        // its outfits (before its group: at the root)
        if (npc.mesh && (npc.outfitMesh || npc.nudeMesh))
            for (size_t at : BlockRefOffsets(data)) {
                uint32_t ref = U32(data, at), idx = (ref & 0x7FFFFFFF) - 1;
                if (!(ref & 0x80000000) || idx >= count)
                    continue;
                const std::string& n = recs[idx].name;
                std::string suffix = n.size() > 5 ? n.substr(n.size() - 5) : "";
                uint32_t body = suffix == "_HMas" ? npc.outfitMesh : suffix == "_Nude" ? npc.nudeMesh : 0;
                if (!body)
                    continue;
                int made = outfitGroup(idx, body, npc.importName.empty() ? npc.name : npc.importName, suffix);
                if (made >= 0)
                    Put(data, at, 0x80000000 | uint32_t(made + 1));
            }
        int groupIndex = int(count) + added, actorIndex = groupIndex + 1, helperIndex = groupIndex + 2;
        // its data: its own aim helper (the template's is its only), its body's animations
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
        groupBlocks.emplace_back(kPools);
        g_groups.insert(uint32_t((gms.size() + 3) & ~size_t(3)));
        add(g, kOpens | (pops << 25), group.extra, rootNodes);
        add(a, 0, actor.extra, groupBlocks.back());
        if (helper >= 0)
            add(h, 0, recs[helper].extra, groupBlocks.back());
        listed.push_back(0x80000000 | uint32_t(actorIndex + 1));
        g_made[npc.name] = actorIndex;
    }
    // new lists: copies of list records, at the root after the groups (a node
    // of their own: TakeNode), their data blocks copied
    bool inGroup = added > 0;  // (the last entry was a group's child)
    int listsAdded = 0;
    for (const SceneList& l : lists) {
        if (l.record < 0 || uint32_t(l.record) >= count || l.name.empty() || l.name.size() > 64 ||
            !recs[l.record].data || recs[l.record].mesh || recs[l.record].script ||
            (recs[l.record].classId & 0x100000)) {  // (a list: no model, no script, not a group)
            Log("scene lists: %s: no list to copy (record %d)", l.name.c_str(), l.record);
            continue;
        }
        std::vector<uint8_t> data = Block(gms, recs[l.record].data);
        size_t at = 0;
        uint32_t bytes = 0;
        if (data.empty() || !FindArray(data, at, bytes)) {
            Log("scene lists: %s: record %d is not a list", l.name.c_str(), l.record);
            continue;
        }
        Put(data, 0, U32(data, 0) & 0x7FFFFFFF);  // not remapped yet
        std::vector<uint8_t> r(gms.begin() + recordAt(l.record), gms.begin() + recordAt(l.record) + kRecord);
        Put(r, 0x00, AppendString(buf, l.name));
        Put(r, 0x20, Append(gms, data));
        uint32_t pops = added == 0 ? uint32_t(depth) : inGroup ? 1u : 0u;
        if (pops > 127)
            break;
        g_groups.insert(uint32_t((gms.size() + 3) & ~size_t(3)));  // (its node: TakeNode)
        uint32_t off = Append(gms, r);
        uint32_t e[2] = {(off / 4) | (pops << 25), recs[l.record].extra};
        table.insert(table.end(), reinterpret_cast<uint8_t*>(e), reinterpret_cast<uint8_t*>(e) + 8);
        ++moreOfClass[U32(r, 0x10)];
        ++added;
        ++listsAdded;
        inGroup = false;
    }
    // zones: copies of one of the scene's CheckInside boxes (its events: that
    // routine only), at the root (a node of their own: TakeNode), with data
    // blocks of their own in the game's format
    if (!zones.empty()) {
        int box = -1;
        for (uint32_t i = 0; i < count && box < 0; ++i) {
            if (recs[i].classId != kBoxClass || !recs[i].events || recs[i].script || recs[i].mesh)
                continue;
            uint32_t ev = recs[i].events, n = U32(gms, ev);
            uint32_t name = U32(gms, ev + 4);
            if (n == 1 && name < gms.size() &&
                strncmp(reinterpret_cast<const char*>(&gms[name]), "ZGEOM_CheckInside", gms.size() - name) == 0)
                box = int(i);
        }
        if (box < 0)
            Log("scene zones: %s has no CheckInside box to copy (goals check distances)", scene.c_str());
        for (const SceneZone& z : box < 0 ? std::vector<SceneZone>{} : zones) {
            uint32_t pops = added == 0 ? uint32_t(depth) : inGroup ? 1u : 0u;
            if (pops > 127 || z.name.empty())
                break;
            int index = int(count) + added;
            std::vector<uint8_t> d = ZoneData(z.half);  // (remapped when the scene loads: its messages)
            const float mat[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
            std::vector<uint8_t> r(gms.begin() + recordAt(box), gms.begin() + recordAt(box) + kRecord);
            Put(r, 0x00, AppendString(buf, z.name));
            Put(r, 0x04, Append(gms, mat, sizeof(mat)));
            Put(r, 0x08, Append(gms, z.pos, sizeof(z.pos)));
            Put(r, 0x1C, 0);
            Put(r, 0x20, Append(gms, d));
            g_groups.insert(uint32_t((gms.size() + 3) & ~size_t(3)));  // (its node: TakeNode)
            uint32_t off = Append(gms, r);
            uint32_t e[2] = {(off / 4) | (pops << 25), recs[box].extra};
            table.insert(table.end(), reinterpret_cast<uint8_t*>(e), reinterpret_cast<uint8_t*>(e) + 8);
            ++moreOfClass[U32(r, 0x10)];
            ++added;
            inGroup = false;
            g_zones[z.name] = index;
            Log("scene zones: %s at (%.0f %.0f %.0f), %.0f x %.0f x %.0f", z.name.c_str(), z.pos[0], z.pos[1], z.pos[2],
                z.half[0] * 2, z.half[1] * 2, z.half[2] * 2);
        }
    }
    // vehicles: a ZCar with copies of the model's records, and its spline
    int carsAdded = 0;
    if (!cars.empty()) {
        int car = -1, shape = -1;  // the scene's own as templates
        for (uint32_t i = 0; i < count && (car < 0 || shape < 0); ++i) {
            if (recs[i].classId == kCarClass && car < 0)
                car = int(i);
            if (recs[i].classId == kShapeClass && shape < 0)
                shape = int(i);
        }
        int open = added == 0 ? depth : inGroup ? 1 : 0;  // groups open before the next entry
        auto emit = [&](std::vector<uint8_t>& rec, bool opens, int level, uint32_t extra, std::vector<uint32_t>* nodes,
                        bool ownNode) {
            uint32_t pops = uint32_t(open - level);
            open = level + (opens ? 1 : 0);
            if (ownNode)
                g_groups.insert(uint32_t((gms.size() + 3) & ~size_t(3)));
            uint32_t off = Append(gms, rec);
            uint32_t e[2] = {(off / 4) | (opens ? kOpens : 0) | (pops << 25), extra};
            table.insert(table.end(), reinterpret_cast<uint8_t*>(e), reinterpret_cast<uint8_t*>(e) + 8);
            ++moreOfClass[U32(rec, 0x10)];
            int pool = rec[0x2D] * 8 + NodeKind(U32(rec, 0x10));
            if (nodes && pool < kPools)
                ++(*nodes)[pool];
            ++added;
        };
        std::vector<std::vector<int>> children(count);
        for (uint32_t i = 0; i < count; ++i)
            if (recs[i].parent >= 0 && uint32_t(recs[i].parent) < count)
                children[recs[i].parent].push_back(int(i));
        // a record of the model and what is under it, a level deeper each group
        // record flags the copies get: a car's parts 0x200000, as every part of the game's vehicles (the van's,
        // the trucks', the limos', the escape cutscene's Ferrari): CreateGeoms (0x41D078) makes a plain mesh
        // record without it a lightweight static node, not to be moved (moved, the world around it was drawn torn)
        uint32_t copyFlags = 0;
        // ... and their light table handle (a list entry's second u32, node +0x6C): the scene's light table
        // (GMS +0x18, engine +0x5D1F, made by 0x428880) keeps for each handle the lights on that node; a node
        // that moves marks those lights changed (0x428180). The game never shares one: a node it copies, or
        // registers at runtime, gets an entry of its own (0x5BB2C0, SetFlags 0x5BAC80: vfunc 0x24). A car's
        // parts get, while loading, the handle of an empty entry ({0, 1}, as the game's moving parts have:
        // C1-1's van) and one of their own once the level runs (OwnLightEntries)
        uint32_t copyExtra = 0;
        uint32_t emptyLights = 0;
        if (uint32_t lt = U32(gms, 0x18); lt && lt + 8 <= gms.size()) {
            size_t data = lt + 4;
            uint32_t words = U32(gms, data);
            for (uint32_t i = 0; i < count && !emptyLights; ++i) {
                uint32_t h = recs[i].extra;
                if (h > 3 && h + 1 < words && data + (h + 2) * 4 <= gms.size() && U32(gms, data + h * 4) == 0 &&
                    U32(gms, data + h * 4 + 4) == 1)
                    emptyLights = h;
            }
        }
        std::function<void(int, int, size_t, const std::array<float, 12>*, int, const char*)> copy =
            [&](int r, int level, size_t block, const std::array<float, 12>* local, int guard, const char* name) {
                if (guard > 8)
                    return;
                std::vector<uint8_t> rec(gms.begin() + recordAt(r), gms.begin() + recordAt(r) + kRecord);
                if (name)
                    Put(rec, 0x00, AppendString(buf, name));
                if (local) {
                    Put(rec, 0x04, Append(gms, local->data(), 36));
                    Put(rec, 0x08, Append(gms, local->data() + 9, 12));
                }
                Put(rec, 0x20, recs[r].data ? copyBlock(recs[r].data) : 0);
                Put(rec, 0x28, 0);  // (no scripts on the copy)
                Put(rec, 0x18, U32(rec, 0x18) | copyFlags);
                bool opens = (U32(gms, entry(r)) & kOpens) != 0;
                emit(rec, opens, level, copyExtra ? copyExtra : recs[r].extra, &groupBlocks[block], false);
                if (recs[r].classId & 0x100000) {
                    groupBlocks.emplace_back(kPools);
                    size_t mine = groupBlocks.size() - 1;
                    for (int c : children[r])
                        copy(c, level + 1, mine, nullptr, guard + 1, nullptr);
                }
            };
        for (const SceneCar& c : cars) {
            if (c.parts.empty() || c.parts.size() > 64 || shape < 0 ||
                c.name.empty() || c.name.size() > 64 || c.shape.empty() || c.shape.size() > 64) {
                Log("scene cars: %s: no models (of record %d) or no ZSHAPE to copy", c.name.c_str(), c.model);
                continue;
            }
            uint32_t mesh = ShapeMesh(c.shape);
            if (!mesh) {
                Log("scene cars: %s: its spline %s was not made", c.name.c_str(), c.shape.c_str());
                continue;
            }
            // the spline: a ZSHAPE at the origin, hidden (as the game's paths)
            std::vector<uint8_t> s(gms.begin() + recordAt(shape), gms.begin() + recordAt(shape) + kRecord);
            const float ident[9] = {0, 0, 1, 0, 1, 0, 1, 0, 0}, zero[3] = {};
            Put(s, 0x00, AppendString(buf, c.shape));
            Put(s, 0x04, Append(gms, ident, sizeof(ident)));
            Put(s, 0x08, Append(gms, zero, sizeof(zero)));
            Put(s, 0x0C, mesh);
            Put(s, 0x18, (U32(s, 0x18) & ~0xFFFFFu) | 0x200000);
            Put(s, 0x20, recs[shape].data ? copyBlock(recs[shape].data) : 0);
            Put(s, 0x28, 0);
            emit(s, false, 0, recs[shape].extra, nullptr, true);
            // the car: a ZCar group (the scene's own as the template, else the model's with the van's settings)
            int tmpl = car >= 0 ? car : c.parts.front().first;
            std::vector<uint8_t> g(gms.begin() + recordAt(tmpl), gms.begin() + recordAt(tmpl) + kRecord);
            Put(g, 0x00, AppendString(buf, c.name));
            Put(g, 0x04, Append(gms, c.mat, sizeof(c.mat)));
            Put(g, 0x08, Append(gms, c.pos, sizeof(c.pos)));
            Put(g, 0x0C, 0);
            Put(g, 0x10, kCarClass);
            if (car >= 0) {
                Put(g, 0x20, recs[car].data ? copyBlock(recs[car].data) : 0);
            } else {
                std::vector<uint8_t> d(kCarData, kCarData + sizeof(kCarData));
                Put(g, 0x18, (U32(g, 0x18) & ~0xFFFFFu) | 0x44000);  // dynamic, auto-assign (as the van)
                g[0x2D] = gms[recordAt(recs[c.parts.front().first].parent >= 0 ? recs[c.parts.front().first].parent
                                                                              : c.parts.front().first) + 0x2D];
                Put(g, 0x20, Append(gms, d));
            }
            Put(g, 0x28, 0);  // (the editor drives it: no script)
            emit(g, true, 0, recs[tmpl].extra, nullptr, true);
            groupBlocks.emplace_back(kPools);
            size_t block = groupBlocks.size() - 1;
            bool body = false;  // ZMoveOnSpline's init (0x5912A0) takes its "*Body*" child as the body (+0xDE)
            copyFlags = 0x200000;
            copyExtra = emptyLights;
            if (!emptyLights)
                Log("scene cars: %s: no empty light table entry in this scene (its parts keep their models')",
                    c.name.c_str());
            for (const auto& [r, local] : c.parts)
                if (r >= 0 && uint32_t(r) < count && !(recs[r].classId & 0x100000) && recs[r].mesh) {
                    copy(r, 1, block, &local, 0, body ? nullptr : "Body");
                    body = true;
                }
            copyFlags = 0;
            copyExtra = 0;
            static const char* const kWheels[4] = {"FrontLeft_01", "FrontRight_01", "BackLeft_01", "BackRight_01"};
            for (size_t k = 0; k < c.wheels.size() && k < 4; ++k) {
                std::vector<uint8_t> w(gms.begin() + recordAt(c.model), gms.begin() + recordAt(c.model) + kRecord);
                Put(w, 0x00, AppendString(buf, kWheels[k]));
                Put(w, 0x04, Append(gms, ident, sizeof(ident)));
                Put(w, 0x08, Append(gms, c.wheels[k].data(), 12));
                Put(w, 0x0C, 0);
                Put(w, 0x10, 0x100001);  // ZGROUP, empty
                Put(w, 0x14, 0);
                Put(w, 0x20, 0);
                Put(w, 0x28, 0);
                bool front = k < 2;  // a front wheel steers: its "Wheel*" child turns (0x5958E0)
                emit(w, front, 1, recs[c.model].extra, &groupBlocks[block], false);
                groupBlocks.emplace_back(kPools);
                if (front) {
                    size_t mine = groupBlocks.size() - 1;
                    std::vector<uint8_t> h = w;
                    Put(h, 0x00, AppendString(buf, "Wheel"));
                    Put(h, 0x08, Append(gms, zero, sizeof(zero)));
                    emit(h, false, 2, recs[c.model].extra, &groupBlocks[mine], false);
                    groupBlocks.emplace_back(kPools);
                }
            }
            ++carsAdded;
        }
        inGroup = open > 0;
    }
    if (!added)
        return false;
    if (carsAdded)
        Log("scene cars: %d vehicles (ZCar) with their splines", carsAdded);
    if (listsAdded)
        Log("scene lists: %d new lists", listsAdded);
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
    for (uint32_t i = 0; i < count && !listed.empty(); ++i) {
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
