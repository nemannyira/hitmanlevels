#include "npcs.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <map>

#include "../engine/imports.h"
#include "../engine/lists.h"
#include "../engine/npc.h"
#include "../engine/spawn.h"
#include "../log.h"
#include "../scene.h"
#include "level.h"

namespace h2::editor {

namespace {

std::vector<engine::ZGEOM*> g_placed;
std::map<engine::ZGEOM*, engine::ScriptInfo> g_behaviour;  // set by the editor (runtime refs)
std::map<engine::ZGEOM*, bool> g_noBehaviour;               // stopped by the editor
std::map<engine::ZGEOM*, int> g_placedRecord;               // record a placed NPC copies
int g_counter;
engine::ZGEOM* g_replaced;  // the character SetNpcBehaviour made last
std::vector<engine::ZGEOM*> g_removed;  // placed characters removed (hidden, far below)

std::string Lower(std::string s)
{
    for (char& c : s)
        c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

engine::ScriptInfo Runtime(engine::ScriptInfo s)
{
    for (engine::ScriptParam& p : s.params)
        if (p.type == engine::ParamType::Ref)
            p.bits = p.bits ? engine::RemapRef(p.bits) : 0;
    return s;
}

std::map<std::pair<int, uint32_t>, engine::ZGEOM*> g_previews;  // (record, body) -> its preview character

// Outfits of characters of other levels: an actor's data block names its
// clothes on Hitman (a group HitmanAsGroup/<model>_HMas) and its body without
// them (<model>_Nude), each a group with a drawn child ("Ground") showing a
// body. For a body of another level, the template character's groups are
// made again with that level's bodies (one per template group and body).
std::map<std::pair<int, uint32_t>, engine::ZGEOM*> g_outfits;

engine::ZGEOM* OutfitGroup(int groupRecord, uint32_t body, const std::string& name)
{
    auto key = std::make_pair(groupRecord, body);
    if (auto it = g_outfits.find(key); it != g_outfits.end() && IsLive(it->second))
        return it->second;
    const auto& records = engine::SceneRecords();
    engine::ZGEOM* group = engine::RecordGeom(groupRecord);
    if (!group || !IsLive(group) || !group->node->parent)
        return nullptr;
    int child = -1;
    for (size_t k = size_t(groupRecord) + 1; k < records.size() && k < size_t(groupRecord) + 8; ++k)
        if (records[k].parent == groupRecord && records[k].mesh) {
            child = int(k);
            break;
        }
    engine::ZGEOM* shown = child >= 0 ? engine::RecordGeom(child) : nullptr;
    engine::ZGEOM* parent = group->node->parent->geom;
    if (!shown || !IsLive(shown) || !parent || !IsLive(parent))
        return nullptr;
    const engine::SceneRecord& g = records[groupRecord];
    const engine::SceneRecord& c = records[child];
    std::string err;
    engine::ZGEOM* made = engine::CreateObject(name, parent, engine::ClassName(group).c_str(), 0, g.flags, g.mat, g.pos,
                                               engine::RemappedBlock(g.data), &err, false);
    if (!made) {
        Log("npc: outfit %s not made (%s)", name.c_str(), err.c_str());
        return nullptr;
    }
    if (!engine::CreateObject(engine::SafeString(shown->name), made, engine::ClassName(shown).c_str(), body, c.flags,
                              c.mat, c.pos, engine::RemappedBlock(c.data), &err))
        Log("npc: outfit %s: body not made (%s)", name.c_str(), err.c_str());
    engine::InitObject(made);
    AdoptEngineOwned(made);
    g_outfits[key] = made;
    Log("npc: outfit %s made", name.c_str());
    return made;
}

// the template's outfit references -> the ones made with the imported bodies
std::map<uint32_t, uint32_t> OutfitSwap(int record, const std::string& name, uint32_t outfit, uint32_t nude)
{
    std::map<uint32_t, uint32_t> swap;
    const auto& records = engine::SceneRecords();
    if (record < 0 || size_t(record) >= records.size() || !records[record].data)
        return swap;
    std::vector<uint8_t> block = engine::RawBlock(records[record].data);
    for (size_t at : engine::BlockRefOffsets(block)) {
        uint32_t ref;
        memcpy(&ref, &block[at], 4);
        uint32_t idx = (ref & 0x7FFFFFFF) - 1;
        if (!(ref & 0x80000000) || idx >= records.size())
            continue;
        const std::string& g = records[idx].name;
        std::string suffix = g.size() > 5 ? g.substr(g.size() - 5) : "";
        uint32_t body = suffix == "_HMas" ? outfit : suffix == "_Nude" ? nude : 0;
        if (!body)
            continue;
        if (engine::ZGEOM* made = OutfitGroup(int(idx), body, name + suffix))
            if (uint32_t from = engine::RemapRef(ref))
                swap[from] = engine::RefOf(made);
    }
    return swap;
}
engine::ZGEOM* g_shownPreview;
constexpr float kPreviewAway = -50000.0f;

void PutAway(engine::ZGEOM* actor)
{
    if (!actor || !IsLive(actor))
        return;
    Transform t = GetTransform(actor);
    float away[3] = {t.pos[0], kPreviewAway, t.pos[2]};
    engine::TeleportActor(actor, away);
    SetHidden(actor, true);
}

void OnScene(const std::string&)
{
    g_previews.clear();
    g_outfits.clear();
    g_shownPreview = nullptr;
    g_placed.clear();
    g_removed.clear();
    g_behaviour.clear();
    g_noBehaviour.clear();
    g_placedRecord.clear();
}

} // namespace

std::vector<int> NpcRecords() { return engine::ActorRecords(); }

std::string NpcAnimSet(int record)
{
    const auto& records = engine::SceneRecords();
    if (record < 0 || size_t(record) >= records.size() || !records[record].data)
        return "";
    std::vector<uint8_t> block = engine::RawBlock(records[record].data);
    return block.empty() ? "" : engine::AnimSetOf(block.data(), block.size());
}

bool NpcHasExtraAnims(int record)
{
    const auto& records = engine::SceneRecords();
    if (record < 0 || size_t(record) >= records.size() || !records[record].data)
        return false;
    std::vector<uint8_t> block = engine::RawBlock(records[record].data);
    std::vector<std::string> files = engine::AnimFilesOf(block.data(), block.size());
    return files.size() == 2 && !files[1].empty();
}

std::string NpcModel(int record)
{
    const auto& records = engine::SceneRecords();
    if (record < 0 || size_t(record) >= records.size())
        return {};
    std::string m = engine::ActorModelName(records[record]);
    return m.empty() ? records[record].name : m;
}

bool AuthoredBehaviour(int record, engine::ScriptInfo& out)
{
    const auto& records = engine::SceneRecords();
    if (record < 0 || size_t(record) >= records.size() || !records[record].script)
        return false;
    engine::ScriptInfo info;
    if (!engine::ParseScript(records[record].script, info))
        return false;
    out = Runtime(info);
    return true;
}

std::vector<std::string> BehaviourNames()
{
    std::vector<std::string> names;
    for (const engine::SceneRecord& r : engine::SceneRecords()) {
        if (!r.script || r.classId != 0x2003E9)
            continue;
        engine::ScriptInfo info;
        if (!engine::ParseScript(r.script, info))
            continue;
        std::string n = engine::ScriptShortName(info.name);
        if (std::find(names.begin(), names.end(), n) == names.end())
            names.push_back(n);
    }
    std::sort(names.begin(), names.end(), [](const std::string& a, const std::string& b) { return Lower(a) < Lower(b); });
    return names;
}

bool BehaviourTemplate(const std::string& name, engine::ScriptInfo& out)
{
    std::string want = Lower(name);
    for (const engine::SceneRecord& r : engine::SceneRecords()) {
        if (!r.script)
            continue;
        engine::ScriptInfo info;
        if (!engine::ParseScript(r.script, info))
            continue;
        if (Lower(info.name) == want || Lower(engine::ScriptShortName(info.name)) == want) {
            out = Runtime(info);
            return true;
        }
    }
    return false;
}

int FindNpcRecord(const std::string& what)
{
    const auto& records = engine::SceneRecords();
    if (what.empty())
        return -1;
    if (std::all_of(what.begin(), what.end(), [](unsigned char c) { return std::isdigit(c); })) {
        int r = std::atoi(what.c_str());
        return r >= 0 && size_t(r) < records.size() && records[r].classId == 0x2003E9 ? r : -1;
    }
    if (engine::ZGEOM* g = Resolve(what))
        if (engine::ZGEOM* a = ActorOf(g))
            return NpcRecordOf(a);
    std::string want = Lower(what);
    for (int r : NpcRecords())
        if (Lower(NpcModel(r)) == want)
            return r;
    for (int r : NpcRecords())
        if (records[r].parent >= 0 && Lower(records[records[r].parent].name) == want)
            return r;
    return -1;
}

engine::ZGEOM* ActorOf(engine::ZGEOM* g)
{
    engine::ZBaseGeom* node = g && IsLive(g) ? g->node : nullptr;
    for (int depth = 0; node && depth < 64; ++depth) {
        if (!engine::IsReadable(node, sizeof(*node)))
            return nullptr;
        if (node->geom && IsLive(node->geom) && engine::IsA(node->geom, "ZActor"))
            return node->geom;
        node = node->parent;
    }
    return nullptr;
}

int NpcRecordOf(engine::ZGEOM* actor)
{
    if (!actor)
        return -1;
    auto it = g_placedRecord.find(actor);
    if (it != g_placedRecord.end())
        return it->second;
    return engine::RecordOf(actor);
}

engine::ZGEOM* CreateNpc(const NpcSpec& spec, std::string* error)
{
    auto fail = [&](const std::string& why) -> engine::ZGEOM* {
        if (error)
            *error = why;
        return nullptr;
    };
    if (spec.record < 0)
        return fail("no character chosen");
    // the room the position is in: the room of the nearest object below it would
    // be best; the room of the original character is used when nothing better
    engine::ZGEOM* parent = nullptr;
    if (engine::ZGEOM* original = engine::RecordGeom(spec.record))
        parent = RoomOf(original);
    engine::NpcSpawn s;
    s.record = spec.record;
    s.mesh = spec.mesh;
    if (spec.mesh && (spec.outfitMesh || spec.nudeMesh))
        s.refSwap = OutfitSwap(spec.record, spec.importName, spec.outfitMesh, spec.nudeMesh);
    s.parent = spec.parentRoom ? spec.parentRoom : parent;
    if (!s.parent)
        return fail("no room for the character");
    memcpy(s.mat, spec.transform.mat, sizeof(s.mat));
    memcpy(s.pos, spec.transform.pos, sizeof(s.pos));
    if (spec.hasBehaviour)
        s.script = engine::BuildScriptBlock(spec.behaviour);
    if (spec.name.empty()) {
        char name[96];
        snprintf(name, sizeof(name), "NPC_%s_%d",
                 (spec.importName.empty() ? NpcModel(spec.record) : spec.importName).c_str(), ++g_counter);
        s.name = name;
    } else {
        s.name = spec.name;
    }
    engine::ZGEOM* npc = engine::SpawnNpc(s, error);
    if (!npc)
        return nullptr;
    AdoptEngineOwned(npc);
    g_placed.push_back(npc);
    g_placedRecord[npc] = spec.record;
    if (spec.hasBehaviour)
        g_behaviour[npc] = spec.behaviour;
    else
        g_noBehaviour[npc] = true;
    return npc;
}

bool IsPlacedNpc(engine::ZGEOM* actor)
{
    return std::find(g_placed.begin(), g_placed.end(), actor) != g_placed.end();
}

const std::vector<engine::ZGEOM*>& PlacedNpcs()
{
    g_placed.erase(std::remove_if(g_placed.begin(), g_placed.end(), [](engine::ZGEOM* g) { return !IsLive(g); }),
                   g_placed.end());
    return g_placed;
}

bool DeleteNpc(engine::ZGEOM* actor, std::string* error)
{
    if (!IsPlacedNpc(actor)) {
        if (error)
            *error = "only characters placed by the editor can be removed";
        return false;
    }
    // Actors cannot be destroyed while the scene runs (the engine keeps
    // pointers to them): it is hidden and put far below the level, where it
    // stays (and the level no longer creates it next time).
    g_placed.erase(std::remove(g_placed.begin(), g_placed.end(), actor), g_placed.end());
    g_behaviour.erase(actor);
    g_noBehaviour.erase(actor);
    g_placedRecord.erase(actor);
    Transform t = GetTransform(actor);
    float away[3] = {t.pos[0], t.pos[1] - 50000.0f, t.pos[2]};
    engine::TeleportActor(actor, away);
    SetHidden(actor, true);
    g_removed.push_back(actor);
    return true;
}

engine::ZGEOM* ShowPreviewNpc(int record, const Transform& t, std::string* error, uint32_t mesh)
{
    engine::ZGEOM* actor = nullptr;
    auto it = g_previews.find({record, mesh});
    if (it != g_previews.end() && IsLive(it->second)) {
        actor = it->second;
    } else {
        engine::ZGEOM* original = engine::RecordGeom(record);
        engine::NpcSpawn s;
        s.record = record;
        s.mesh = mesh;
        s.parent = original ? RoomOf(original) : nullptr;
        if (!s.parent) {
            if (error)
                *error = "no room for the character";
            return nullptr;
        }
        memcpy(s.mat, t.mat, sizeof(s.mat));
        memcpy(s.pos, t.pos, sizeof(s.pos));
        s.name = "EditorPreview_" + NpcModel(record);
        actor = engine::SpawnNpc(s, error);
        if (!actor)
            return nullptr;
        AdoptEngineOwned(actor);
        g_previews[{record, mesh}] = actor;
    }
    if (g_shownPreview && g_shownPreview != actor)
        PutAway(g_shownPreview);
    engine::TeleportActor(actor, t.pos, t.mat);
    if (g_shownPreview != actor)
        SetHidden(actor, false);
    g_shownPreview = actor;
    return actor;
}

void HidePreviewNpc()
{
    if (!g_shownPreview)
        return;
    PutAway(g_shownPreview);
    g_shownPreview = nullptr;
}

bool IsRemovedNpc(engine::ZGEOM* actor)
{
    return std::find(g_removed.begin(), g_removed.end(), actor) != g_removed.end();
}

bool SetNpcBehaviour(engine::ZGEOM* actor, const engine::ScriptInfo* behaviour, std::string* error)
{
    auto fail = [&](const std::string& why) {
        if (error)
            *error = why;
        return false;
    };
    if (!actor || !IsLive(actor))
        return fail("no character");
    engine::ScriptInfo current;
    bool has = CurrentBehaviour(actor, current);
    // Same behaviour: its settings are written into the running script.
    // Replacing a running script leaves the guard groups and the level
    // control holding the old one (crash), so another behaviour means a new
    // character - only for the ones the editor placed.
    if (behaviour && has && engine::ScriptInstance(actor) &&
        Lower(engine::ScriptShortName(current.name)) == Lower(engine::ScriptShortName(behaviour->name))) {
        int n = engine::SetScriptVariables(actor, *behaviour, error);
        Log("npc %p: %d settings of %s written", static_cast<void*>(actor), n, current.name.c_str());
        g_behaviour[actor] = *behaviour;
        return true;
    }
    if (!IsPlacedNpc(actor))
        return fail("the level's own characters keep their kind of behaviour (only its settings can change); "
                    "place a new character for another behaviour");
    // a placed character: made again with the new behaviour
    NpcSpec spec;
    spec.record = NpcRecordOf(actor);
    spec.transform = GetTransform(actor);
    // a body of another level stays
    const auto& records = engine::SceneRecords();
    uint32_t body = engine::MeshOf(actor);
    if (spec.record >= 0 && size_t(spec.record) < records.size() && body && body != records[spec.record].mesh) {
        spec.mesh = body;
        for (const ImportedBody& b : ImportedBodies())  // and its outfits
            if (b.mesh == body) {
                spec.importLevel = b.level;
                spec.importMesh = b.sourceMesh;
                spec.importName = b.name;
                spec.outfitMesh = b.outfit;
                spec.nudeMesh = b.nude;
            }
    }
    spec.hasBehaviour = behaviour != nullptr;
    if (behaviour)
        spec.behaviour = *behaviour;
    spec.name = engine::SafeString(actor->name);
    std::string err;
    if (!DeleteNpc(actor, &err))
        return fail(err);
    engine::ZGEOM* again = CreateNpc(spec, &err);
    if (!again)
        return fail(err);
    g_replaced = again;
    return true;
}

bool CurrentBehaviour(engine::ZGEOM* actor, engine::ScriptInfo& out)
{
    if (!actor)
        return false;
    if (g_noBehaviour.count(actor))
        return false;
    auto it = g_behaviour.find(actor);
    if (it != g_behaviour.end()) {
        out = it->second;
    } else {
        int rec = engine::RecordOf(actor);
        if (rec < 0 || !AuthoredBehaviour(rec, out))
            return false;
    }
    engine::GetScriptVariables(actor, out);  // what the running script has now
    return true;
}

std::vector<StartMove> StartMoves(const engine::ScriptInfo& s)
{
    std::vector<StartMove> out;
    for (const engine::ScriptParam& p : s.params) {
        std::string n = Lower(p.name);
        if (p.type == engine::ParamType::Long) {
            if (p.bits && (n.find("warp") != std::string::npos || n.find("startatrandom") != std::string::npos))
                out.push_back({p.name, false, p.bits});
        } else if (p.type == engine::ParamType::Ref && p.bits) {
            bool named = n.find("start") != std::string::npos || n.find("spawn") != std::string::npos;
            bool many = n.find("list") != std::string::npos || n.find("positions") != std::string::npos ||
                        n.find("sound") != std::string::npos;
            engine::ZGEOM* g = engine::GeomOfRef(p.bits);
            if (named && !many && g && IsLive(g) && !engine::IsEditableList(g) && !engine::IsA(g, "ZLIST"))
                out.push_back({p.name, true, p.bits});
        }
    }
    return out;
}

void SetRouteWarps(engine::ScriptInfo& s, const std::vector<StartMove>& moves, bool on)
{
    for (const StartMove& m : moves)
        for (engine::ScriptParam& p : s.params)
            if (!m.ref && p.name == m.name && p.type == engine::ParamType::Long)
                p.bits = on ? m.value : 0;
}

std::vector<ParamValue> SaveParams(const engine::ScriptInfo& s)
{
    std::vector<ParamValue> out;
    for (const engine::ScriptParam& p : s.params) {
        ParamValue v;
        v.name = p.name;
        char buf[64];
        switch (p.type) {
        case engine::ParamType::Long:
            v.type = "LONG";
            v.value = std::to_string(int(p.bits));
            break;
        case engine::ParamType::Float: {
            float f;
            memcpy(&f, &p.bits, 4);
            snprintf(buf, sizeof(buf), "%.9g", f);
            v.type = "FLOAT";
            v.value = buf;
            break;
        }
        case engine::ParamType::Ref:
            v.type = "ZGEOMREF";
            v.value = p.bits ? PathOf(engine::GeomOfRef(p.bits)) : "";
            break;
        case engine::ParamType::String:
            v.type = "STRING";
            v.value = p.text;
            break;
        default:
            v.type = "RAW";
            for (uint8_t b : p.raw) {
                snprintf(buf, sizeof(buf), "%02X", b);
                v.value += buf;
            }
            break;
        }
        out.push_back(v);
    }
    return out;
}

void LoadParams(engine::ScriptInfo& s, const std::vector<ParamValue>& values)
{
    for (const ParamValue& v : values) {
        for (engine::ScriptParam& p : s.params) {
            if (p.name != v.name)
                continue;
            if (v.type == "LONG" && p.type == engine::ParamType::Long) {
                p.bits = uint32_t(std::atoi(v.value.c_str()));
            } else if (v.type == "FLOAT" && p.type == engine::ParamType::Float) {
                float f = float(std::atof(v.value.c_str()));
                memcpy(&p.bits, &f, 4);
            } else if (v.type == "ZGEOMREF" && p.type == engine::ParamType::Ref) {
                engine::ZGEOM* g = v.value.empty() ? nullptr : Resolve(v.value);
                p.bits = g ? engine::RefOf(g) : 0;
                if (!g && !v.value.empty())
                    Log("npc parameter %s: object '%s' not found", v.name.c_str(), v.value.c_str());
            } else if (v.type == "STRING" && p.type == engine::ParamType::String) {
                p.text = v.value;
            } else if (v.type == "RAW" && p.type == engine::ParamType::Other) {
                std::vector<uint8_t> raw;
                for (size_t i = 0; i + 1 < v.value.size(); i += 2)
                    raw.push_back(uint8_t(std::strtoul(v.value.substr(i, 2).c_str(), nullptr, 16)));
                if (!raw.empty())
                    p.raw = raw;
            }
        }
    }
}

engine::ZGEOM* LastReplacedNpc() { return g_replaced; }

void InstallNpcs() { OnScenePushed(OnScene); }

} // namespace h2::editor
