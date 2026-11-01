#include "npc_panel.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "imgui.h"

#include "../engine/lists.h"
#include "../engine/messages.h"
#include "../engine/npc.h"
#include "../engine/scene_data.h"
#include "../engine/script_files.h"
#include "../game.h"
#include "../log.h"
#include "../overlay/overlay.h"
#include "../scene.h"
#include "freecam.h"
#include "guides.h"
#include "highlight.h"
#include "history.h"
#include "level.h"
#include "mission.h"
#include "model.h"
#include "npc_docs.h"
#include "npcs.h"
#include "playtest.h"
#include "scene_panel.h"
#include "viewport.h"

namespace h2::editor {

namespace {

// a character's name in the lists: its model; a placed one with a body of
// another level by that body ("Vittorio (C0-1)", not the character lending it
// its behaviour)
std::string CharacterName(engine::ZGEOM* actor)
{
    std::string name = engine::SafeString(actor->name);
    for (const LevelNpc& n : CurrentLevel().npcs)
        if (n.id == name && !n.label.empty())
            return n.label;
    for (const LevelNpc& n : CurrentLevel().npcs)
        if (n.id == name && !n.importName.empty())
            return n.importName + " (" + n.importLevel + ")";
    int record = NpcRecordOf(actor);
    return record >= 0 ? NpcModel(record) : name;
}

// a kind of character: one actor record of the scene per model + behaviour
struct NpcType {
    int record = -1;
    std::string model;     // "BodyguardMafia_01"
    std::string group;     // name of the group it is in ("BodyguardMafia_Bedroom_00")
    std::string script;    // short behaviour name ("BaseGuard")
    int count = 0;
    // a character of another level: its body on `record` (same animations)
    uint32_t mesh = 0;
    std::string importLevel;
    uint32_t importMesh = 0;
    std::string importName;
    uint32_t outfitMesh = 0, nudeMesh = 0;
    std::string importBehaviour;  // its behaviour in its level ("Waiter")
};

bool g_visible = true;
std::string g_builtFor;
std::vector<NpcType> g_types;
std::vector<std::string> g_behaviourNames;  // script classes of the scene's characters
std::set<std::string> g_storyBehaviours;    // ... playing a part of the map's story (IsStoryBehaviour)
int g_type = -1;              // character chosen for placing
std::string g_newBehaviour;   // behaviour of the next one ("" = none)
engine::ScriptInfo g_new;     // its settings
std::vector<StartMove> g_newMoves;  // its settings that move it when it starts
bool g_startHere = true;            // ... are taken out: it starts where it is put
std::string g_message;
std::string g_info;           // "<table>:<parameter>" shown in the info box

// the character being edited (stays while other objects are selected, e.g.
// to point its settings at them)
engine::ZGEOM* g_target;
engine::ScriptInfo g_targetEdit;  // its settings (live values)
bool g_targetHasBehaviour;
double g_targetRead;              // when the live values were read
std::string g_changeTo;           // another behaviour for a placed character

// pick mode: the next click in the world sets this parameter
std::string g_pickParam;
std::string g_pickTable;          // "target" or "new"
bool g_pickPending;
int g_pickFrame;

// expected movement: the points and lists in an NPC's parameters
enum RoutesMode { kRoutesOff, kRoutesSelected, kRoutesAll };
int g_routes = kRoutesSelected;
struct Route {
    std::string name;   // parameter
    bool list = false;  // a list of points walked in order, or a single place
    std::vector<engine::ZGEOM*> points;
};
struct RouteCache {
    std::vector<Route> routes;
    double time = -1e9;
};
std::map<engine::ZGEOM*, RouteCache> g_routeCache;

// route editing
struct Marker {
    ImVec2 screen;
    engine::ZGEOM* point;
};
std::vector<Marker> g_markers;    // route points drawn this frame (a click selects them)
engine::ZGEOM* g_addList;         // "add points" mode: clicks add points to this list
std::string g_newPointParam;      // "new point" mode: the setting that gets the point
std::string g_newPointTable;
struct UsersCache {
    int count = 0;
    double time = -1e9;
};
std::map<engine::ZGEOM*, UsersCache> g_listUsers;

// placing
bool g_placing;
bool g_placePickPending;
int g_placePickFrame;
bool g_haveHit;
float g_point[3];
engine::ZGEOM* g_hitRoom;
// characters and new points go onto the path network (the nearest walkable place, 2 m at most)
bool g_snapNetwork = true;
constexpr float kSnapRadius = 200.0f;
float g_rawPoint[3] = {NAN, NAN, NAN};  // the last point snapped for the preview, and where it went
float g_snappedPoint[3];
bool g_pointOff;  // the preview's point has no walkable place near it

// the point moved onto the network when snapping is on; false: off the network (point unchanged)
bool SnapToNetwork(float p[3])
{
    if (!g_snapNetwork)
        return engine::OnPathNetwork(p) != 0;
    float out[3];
    if (!engine::NearestWalkable(p, kSnapRadius, out))
        return engine::OnPathNetwork(p) == -1;  // a map without a network: nothing to say
    memcpy(p, out, sizeof(out));
    return true;
}

std::string Lower(std::string s)
{
    for (char& c : s)
        c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// a behaviour (or a model's name) of someone armed who attacks trespassers
bool IsGuardBehaviour(const std::string& name)
{
    std::string n = Lower(name);
    for (const char* w : {"guard", "soldier", "police", "officer", "ninja", "sniper", "patrol", "sentry", "spetsnaz",
                          "kgb", "army", "commando", "security", "gunman", "militia"})
        if (n.find(w) != std::string::npos)
            return true;
    return false;
}

// what the list is made of: the scene and the characters of other levels the
// level has (its list changes with the level, e.g. another one opened)
std::string BuildKey()
{
    std::string key = CurrentScene();
    for (const ImportedBody& b : ImportedBodies())
        key += "|" + b.level + "/" + std::to_string(b.mesh);
    return key;
}

void Build()
{
    g_types.clear();
    g_type = -1;
    g_newBehaviour.clear();
    g_new = {};
    g_newMoves.clear();
    g_builtFor = BuildKey();
    g_behaviourNames = BehaviourNames();
    g_storyBehaviours.clear();
    for (const std::string& b : g_behaviourNames) {
        engine::ScriptInfo s;
        if (BehaviourTemplate(b, s) && IsStoryBehaviour(s))
            g_storyBehaviours.insert(b);
    }
    const auto& records = engine::SceneRecords();
    std::map<std::string, int> typeIndex;
    for (int index : NpcRecords()) {
        const engine::SceneRecord& r = records[index];
        NpcType t;
        t.record = index;
        t.model = NpcModel(index);
        t.group = r.parent >= 0 ? records[r.parent].name : "";
        engine::ScriptInfo info;
        if (r.script && engine::ParseScript(r.script, info))
            t.script = engine::ScriptShortName(info.name);
        std::string key = t.model + "|" + t.script;
        auto [it, added] = typeIndex.try_emplace(key, int(g_types.size()));
        if (added)
            g_types.push_back(t);
        g_types[it->second].count++;
    }
    std::sort(g_types.begin(), g_types.end(), [](const NpcType& a, const NpcType& b) {
        return a.script != b.script ? a.script < b.script : a.model < b.model;
    });
    // characters of other levels: their bodies (and animations) on a character
    // of this map (its behaviour, data and size): best one of the same kind
    // (a guard for a guard, someone harmless for a waiter: a guard's behaviour
    // attacks Hitman), then with the same animations, without moves of its own
    std::vector<int> actors = NpcRecords();
    for (const ImportedBody& b : ImportedBodies()) {
        int best = -1, bestScore = -1;
        bool guard = IsGuardBehaviour(b.behaviour.empty() ? b.name : b.behaviour);
        for (int index : actors) {
            std::string set = NpcAnimSet(index);
            if (set.empty())
                continue;
            std::string script;
            engine::ScriptInfo info;
            if (records[index].script && engine::ParseScript(records[index].script, info))
                script = engine::ScriptShortName(info.name);
            int score = (IsGuardBehaviour(script.empty() ? NpcModel(index) : script) == guard ? 8 : 0) +
                        (set == b.animSet ? 4 : set == "Reg_Male" ? 2 : 0) + (NpcHasExtraAnims(index) ? 0 : 1);
            if (score > bestScore) {
                best = index;
                bestScore = score;
            }
        }
        if (best < 0)
            continue;
        NpcType t;
        t.record = best;
        t.model = b.name + " (" + b.level + ")";
        t.group = "worn by " + NpcModel(best);
        engine::ScriptInfo info;
        if (records[t.record].script && engine::ParseScript(records[t.record].script, info))
            t.script = engine::ScriptShortName(info.name);
        t.mesh = b.mesh;
        t.importLevel = b.level;
        t.importMesh = b.sourceMesh;
        t.importName = b.name;
        t.outfitMesh = b.outfit;
        t.nudeMesh = b.nude;
        t.importBehaviour = b.behaviour;
        g_types.push_back(t);
    }
}

// a new behaviour for the next character: as authored, but starting where it is put
void SetNewBehaviour(const engine::ScriptInfo& s)
{
    g_new = s;
    g_newMoves = StartMoves(g_new);
    SetRouteWarps(g_new, g_newMoves, !g_startHere);
}

// The behaviours to choose from (the new character's, another one for a placed one): the map's own,
// its story parts, the other levels'; true when `chosen` was clicked.
bool BehaviourChoices(std::string& chosen)
{
    bool clicked = false;
    static char filter[32];
    if (ImGui::IsWindowAppearing()) {
        filter[0] = 0;
        ImGui::SetKeyboardFocusHere();
    }
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##bfilter", "filter (guard, patrol, C3-1...)", filter, sizeof(filter));
    std::string want = Lower(filter);
    auto matches = [&](const std::string& b) {
        return want.empty() || Lower(b).find(want) != std::string::npos ||
               Lower(DescribeBehaviour(b)).find(want) != std::string::npos;
    };
    std::vector<std::string> own, story, other;
    for (const std::string& b : g_behaviourNames)
        if (matches(b))
            (g_storyBehaviours.count(b) ? story : own).push_back(b);
    for (const std::string& b : ForeignBehaviourNames())
        if (matches(b))
            other.push_back(b);
    auto items = [&](const std::vector<std::string>& list) {
        for (const std::string& b : list) {
            if (ImGui::Selectable(b.c_str(), chosen == b)) {
                chosen = b;
                clicked = true;
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", DescribeBehaviour(b).c_str());
        }
    };
    items(own);
    if (!story.empty()) {
        ImGui::SeparatorText("parts of this map's story");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Behaviours that act out a part of the map's own mission: their settings point to\n"
                              "its characters or vehicles (a boss's limousine, the boss a bodyguard follows).\n"
                              "A copy plays that part; point those settings at your own ones.");
        items(story);
    }
    if (!other.empty()) {
        ImGui::SeparatorText("from other levels");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Behaviours of other levels' characters that work anywhere: their scripts are\n"
                              "loaded from that level. Routes they walk have to be set.");
        items(other);
    }
    if (own.empty() && story.empty() && other.empty())
        ImGui::TextDisabled("none with \"%s\"", filter);
    return clicked;
}

// A guard's point (GuardPoint) left empty gets a new point where the character stands: without one a
// guard goes to a place of its behaviour's own (BaseGuard (C1-1) on C2-2 walked away). How many made.
int GuardPointAt(engine::ScriptInfo& s, const Transform& at, engine::ZGEOM* room)
{
    for (engine::ScriptParam& p : s.params) {
        if (p.type != engine::ParamType::Ref || p.bits || Lower(p.name) != "guardpoint")
            continue;
        Transform t = at;
        t.pos[1] += 1.0f;
        std::string err;
        engine::ZGEOM* point = PlacePoint(t, room, &err);
        if (!point) {
            Log("npc: no guard point (%s)", err.c_str());
            return 0;
        }
        p.bits = engine::RefOf(point);
        return 1;
    }
    return 0;
}

void ChooseNewBehaviour(const std::string& name)
{
    g_newBehaviour = name;
    engine::ScriptInfo s;
    if (!name.empty())
        BehaviourTemplate(name, s);
    SetNewBehaviour(s);
}

// A behaviour that works anywhere for a character of that kind: its own when
// it does, else a guard's for a guard, the plain civilian for the others.
void ChooseType(int t)
{
    g_type = t;
    // a character of another level: its own behaviour there when it works here
    const NpcType& type = g_types[t];
    if (!type.importBehaviour.empty()) {
        std::string own = ForeignBehaviourName(type.importBehaviour, type.importLevel);
        const auto& offered = ForeignBehaviourNames();
        if (std::find(offered.begin(), offered.end(), own) != offered.end()) {
            ChooseNewBehaviour(own);
            return;
        }
    }
    engine::ScriptInfo info;
    bool authored = AuthoredBehaviour(g_types[t].record, info);
    if (authored && (type.importLevel.empty() || IsPortableBehaviour(info)) && !IsStoryBehaviour(info)) {
        g_newBehaviour = engine::ScriptShortName(info.name);
        SetNewBehaviour(info);  // this character's own settings
        return;
    }
    // a guard of the map's story (a limousine's, the boss's bodyguard): one of the map's other guards
    bool guard = IsGuardBehaviour(authored ? engine::ScriptShortName(info.name) : g_types[t].model) ||
                 (authored && IsGuardBehaviour(type.model));
    std::string pick;
    for (const std::string& b : g_behaviourNames)
        if (pick.empty() && guard && IsGuardBehaviour(b) && !g_storyBehaviours.count(b))
            pick = b;
    if (pick.empty() && !guard)
        pick = CivilianBehaviour();
    ChooseNewBehaviour(pick);
}

// The start point references of `moves` that `s` still has get a new point at
// `at` (undoable, one history entry each); how many were made.
int PointStartsAt(engine::ScriptInfo& s, const std::vector<StartMove>& moves, const Transform& at,
                  engine::ZGEOM* room)
{
    int made = 0;
    engine::ZGEOM* point = nullptr;
    for (const StartMove& m : moves) {
        if (!m.ref)
            continue;
        for (engine::ScriptParam& p : s.params) {
            if (p.name != m.name || p.type != engine::ParamType::Ref || p.bits != m.value)
                continue;
            if (!point) {
                Transform t = at;
                t.pos[1] += 1.0f;
                std::string err;
                point = PlacePoint(t, room, &err);
                if (!point) {
                    Log("npc: no start point for %s (%s), it starts at its own", m.name.c_str(), err.c_str());
                    return made;
                }
                ++made;
            }
            p.bits = engine::RefOf(point);
        }
    }
    return made;
}

// ---- routes ----

bool IsList(int record)
{
    const auto& records = engine::SceneRecords();
    return record >= 0 && size_t(record) < records.size() && (records[record].classId & 0x8000000) != 0;
}

// A place an NPC can go to: not a character, item, list or scripted object.
bool Positional(engine::ZGEOM* g)
{
    if (!g || !IsLive(g) || engine::IsA(g, "ZActor") || engine::IsA(g, "ZItem") ||
        engine::IsA(g, "ZItemTemplate"))
        return false;
    int rec = engine::RecordOf(g);
    if (rec < 0)
        return true;
    const engine::SceneRecord& r = engine::SceneRecords()[rec];
    return !r.script && !(r.classId & 0x8000000);
}

// Elements of a list object (ZLIST / ZConditionList) as the game has them
// now (the editor may have changed them); else its data block's array.
std::vector<engine::ZGEOM*> ListPoints(engine::ZGEOM* list, int record)
{
    std::vector<engine::ZGEOM*> out;
    if (engine::IsEditableList(list)) {
        for (uint32_t r : engine::ListRefs(list))
            if (engine::ZGEOM* g = engine::GeomOfRef(r); g && Positional(g))
                out.push_back(g);
        return out;
    }
    std::vector<uint8_t> b = engine::RemappedBlock(engine::SceneRecords()[record].data);
    size_t i = 4;
    while (i < b.size() && b[i] != 0xFF) {
        uint8_t tag = b[i] & 0x3F;
        uint32_t v = 0;
        if (tag == 9 || tag == 7 || tag == 0xC) {
            uint32_t n;
            memcpy(&n, &b[i + 1], 4);
            if (tag == 9)
                for (uint32_t k = 1; k < n / 4 && i + 5 + k * 4 <= b.size(); ++k) {
                    memcpy(&v, &b[i + 5 + (k - 1) * 4], 4);
                    if (engine::ZGEOM* g = engine::GeomOfRef(v))
                        out.push_back(g);
                }
            i += 1 + n;
        } else if (tag == 8) {
            memcpy(&v, &b[i + 1], 4);
            if (engine::ZGEOM* g = engine::GeomOfRef(v))
                out.push_back(g);
            i += 5;
        } else if (tag == 1) {
            i += 9;
        } else if (tag == 2 || tag == 3 || tag == 10 || tag == 11) {
            i += 5;
        } else if (tag == 4 || tag == 5) {
            i += 2 + strnlen(reinterpret_cast<const char*>(&b[i + 1]), b.size() - i - 1);
        } else {
            i += 1;
        }
    }
    out.erase(std::remove_if(out.begin(), out.end(), [](engine::ZGEOM* g) { return !Positional(g); }), out.end());
    return out;
}

std::vector<Route> RoutesOf(const engine::ScriptInfo& s)
{
    std::vector<Route> routes;
    for (const engine::ScriptParam& p : s.params) {
        if (p.type != engine::ParamType::Ref || !p.bits)
            continue;
        engine::ZGEOM* g = engine::GeomOfRef(p.bits);
        if (!g || !IsLive(g))
            continue;
        int rec = engine::RecordOf(g);
        if (IsList(rec)) {
            std::vector<engine::ZGEOM*> pts = ListPoints(g, rec);
            if (!pts.empty())
                routes.push_back({p.name, true, pts});
        } else if (Positional(g)) {
            routes.push_back({p.name, false, {g}});
        }
    }
    return routes;
}

// routes of a character from its current settings (read twice a second)
const std::vector<Route>& RoutesOfActor(engine::ZGEOM* actor)
{
    RouteCache& c = g_routeCache[actor];
    double now = NowMs();
    if (now - c.time > 500.0) {
        c.time = now;
        engine::ScriptInfo s;
        c.routes = CurrentBehaviour(actor, s) ? RoutesOf(s) : std::vector<Route>{};
    }
    return c.routes;
}

ImU32 RouteColor(size_t i)
{
    static const ImU32 colors[] = {IM_COL32(70, 220, 255, 255), IM_COL32(255, 200, 60, 255),
                                   IM_COL32(150, 255, 110, 255), IM_COL32(255, 120, 200, 255),
                                   IM_COL32(190, 150, 255, 255), IM_COL32(255, 140, 80, 255)};
    return colors[i % (sizeof(colors) / sizeof(colors[0]))];
}

ImU32 Faded(ImU32 c, float a)
{
    return (c & ~IM_COL32_A_MASK) | (ImU32(((c >> IM_COL32_A_SHIFT) & 0xFF) * a) << IM_COL32_A_SHIFT);
}

// From the NPC (at `from`) to the places of its behaviour; lists as numbered
// routes with arrows in walking order.
void DrawRoutes(const float from[3], const std::vector<Route>& routes, bool labels)
{
    for (size_t i = 0; i < routes.size(); ++i) {
        const Route& r = routes[i];
        ImU32 c = RouteColor(i);
        std::vector<Transform> pts;
        std::vector<engine::ZGEOM*> geoms;
        for (engine::ZGEOM* g : r.points) {
            if (IsLive(g)) {
                pts.push_back(GetTransform(g));
                geoms.push_back(g);
            }
        }
        if (pts.empty())
            continue;
        DrawLine3D(from, pts[0].pos, Faded(c, 0.45f), 1.5f);
        engine::ZGEOM* sel = Selected();
        for (size_t k = 0; k < pts.size(); ++k) {
            if (r.list && k + 1 < pts.size())
                DrawArrow3D(pts[k].pos, pts[k + 1].pos, c, 2.0f);
            if (geoms[k] == sel)
                DrawPoint3D(pts[k].pos, 8.0f, IM_COL32(255, 255, 255, 255));
            DrawPoint3D(pts[k].pos, r.list ? 4.0f : 5.0f, c);
            // a route the character walks: a point the path network does not
            // reach is never reached (the character stops)
            if (r.list && engine::OnPathNetwork(pts[k].pos) == 0) {
                DrawPoint3D(pts[k].pos, 9.0f, IM_COL32(255, 60, 50, 140));
                DrawLabel3D(pts[k].pos, "off the path network", IM_COL32(255, 90, 80, 255));
            }
            ImVec2 s;
            if (WorldToScreen(pts[k].pos, s))
                g_markers.push_back({s, geoms[k]});
            if (labels && r.list) {
                char n[80];
                if (k == 0)
                    snprintf(n, sizeof(n), "%s 1", r.name.c_str());
                else
                    snprintf(n, sizeof(n), "%d", int(k + 1));
                DrawLabel3D(pts[k].pos, n, k == 0 ? c : Faded(c, 0.8f));
            }
        }
        if (labels && !r.list)
            DrawLabel3D(pts[0].pos, r.name.c_str(), c);
    }
}

std::vector<engine::ZGEOM*> AllCharacters()
{
    std::vector<engine::ZGEOM*> out;
    for (int rec : NpcRecords())
        if (engine::ZGEOM* g = engine::RecordGeom(rec); g && IsLive(g))
            out.push_back(g);
    for (engine::ZGEOM* g : PlacedNpcs())
        out.push_back(g);
    return out;
}

void DrawAllRoutes()
{
    g_markers.clear();
    if (g_routes == kRoutesOff || engine::SceneRecords().empty() || !engine::SceneDataValid() || CurrentScene().empty())
        return;
    auto drawActor = [](engine::ZGEOM* actor) {
        if (!actor || !IsLive(actor) || IsRemovedNpc(actor))
            return;
        Transform t = GetTransform(actor);
        float from[3] = {t.pos[0], t.pos[1] + 100.0f, t.pos[2]};  // about the waist
        DrawRoutes(from, RoutesOfActor(actor), !overlay::SimpleMode());  // (setting names: Advanced mode)
    };
    if (g_routes == kRoutesSelected) {
        engine::ZGEOM* a = ActorOf(Selected());
        drawActor(a ? a : g_target);
        return;
    }
    for (engine::ZGEOM* g : AllCharacters())
        drawActor(g);
}

// ---- settings table ----

std::string RefLabel(uint32_t ref)
{
    if (!ref)
        return "(none)";
    engine::ZGEOM* g = engine::GeomOfRef(ref);
    if (!g || !IsLive(g))
        return "(missing)";
    return engine::SafeString(g->name);
}

void ParamTooltip(const engine::ScriptParam& p)
{
    ParamDoc doc = DescribeParam(p.name, p.typeName);
    ImGui::BeginTooltip();
    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 24.0f);
    ImGui::Text("%s  (%s)", p.name.c_str(), p.typeName.c_str());
    ImGui::TextDisabled("%s", doc.text.c_str());
    if (doc.inferred)
        ImGui::TextDisabled("(explained from the name)");
    ImGui::TextDisabled("click: keep this in the info box");
    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
}

// Settings table; returns true when the user changed a value (numbers: when
// the field is left). Hovering a name explains it, clicking it keeps the
// explanation in the info box below.
bool DrawParams(engine::ScriptInfo& s, bool editable, const char* table)
{
    bool changed = false;
    if (s.params.empty()) {
        ImGui::TextDisabled("no settings");
        return false;
    }
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding,
                        ImVec2(ImGui::GetStyle().CellPadding.x, ImGui::GetFontSize() * 0.2f));  // (#20: clicks)
    bool open = ImGui::BeginTable(table, 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp);
    ImGui::PopStyleVar();
    if (!open)
        return false;
    ImGui::TableSetupColumn("name", ImGuiTableColumnFlags_WidthStretch, 1.0f);
    ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch, 1.4f);
    engine::ZGEOM* sel = Selected();
    for (size_t i = 0; i < s.params.size(); ++i) {
        engine::ScriptParam& p = s.params[i];
        ImGui::PushID(int(i));
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        std::string key = std::string(table) + ":" + p.name;
        if (ImGui::Selectable((ParamLabel(p.name) + "##" + p.name).c_str(), g_info == key))
            g_info = g_info == key ? std::string() : key;
        if (ImGui::IsItemHovered())
            ParamTooltip(p);
        ImGui::TableSetColumnIndex(1);
        ImGui::SetNextItemWidth(-1);
        switch (p.type) {
        case engine::ParamType::Long: {
            int v = int(p.bits);
            if (!editable) {
                ImGui::Text("%d", v);
            } else {
                if (ImGui::InputInt("##v", &v, 0))
                    p.bits = uint32_t(v);
                changed |= ImGui::IsItemDeactivatedAfterEdit();
            }
            break;
        }
        case engine::ParamType::Float: {
            float v;
            memcpy(&v, &p.bits, 4);
            if (!editable) {
                ImGui::Text("%.2f", v);
            } else {
                if (ImGui::InputFloat("##v", &v, 0, 0, "%.2f"))
                    memcpy(&p.bits, &v, 4);
                changed |= ImGui::IsItemDeactivatedAfterEdit();
            }
            break;
        }
        case engine::ParamType::Ref: {
            std::string label = RefLabel(p.bits);
            bool picking = g_pickParam == p.name && g_pickTable == table;
            if (!editable) {
                ImGui::TextUnformatted(label.c_str());
                break;
            }
            // the value opens a menu: pick in the world / use the selection / none / go there
            if (picking)
                label = "click an object...";
            ImGui::PushStyleColor(ImGuiCol_Text, picking ? ImVec4(1.0f, 0.85f, 0.45f, 1.0f)
                                                         : ImGui::GetStyleColorVec4(ImGuiCol_Text));
            bool open = ImGui::Selectable((label + " ...##ref").c_str(), false);
            ImGui::PopStyleColor();
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s\nclick: choose another object",
                                  p.bits ? PathOf(engine::GeomOfRef(p.bits)).c_str() : "(none)");
            if (open)
                ImGui::OpenPopup("refmenu");
            if (ImGui::BeginPopup("refmenu")) {
                if (ImGui::MenuItem(picking ? "Stop picking" : "Pick in the world...")) {
                    if (picking) {
                        g_pickParam.clear();
                    } else {
                        g_pickParam = p.name;
                        g_pickTable = table;
                        g_pickPending = false;
                    }
                }
                std::string selName = sel && IsLive(sel) ? engine::SafeString(sel->name) : "";
                if (ImGui::MenuItem(("Use the selected object" + (selName.empty() ? "" : " (" + selName + ")")).c_str(),
                                    nullptr, false, !selName.empty())) {
                    p.bits = engine::RefOf(sel);
                    changed = true;
                }
                if (ImGui::MenuItem("New point (click in the world)...")) {
                    g_newPointParam = p.name;
                    g_newPointTable = table;
                    g_pickPending = false;
                    g_pickParam.clear();
                    g_addList = nullptr;
                }
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Makes an invisible point where you click and uses it here\n"
                                      "(move it later: click its marker, then drag it)");
                engine::ZGEOM* now = p.bits ? engine::GeomOfRef(p.bits) : nullptr;
                bool route = IsRouteRef(p.name) || (now && IsLive(now) && engine::IsEditableList(now));
                if (route && ImGui::MenuItem("New route (click its points)...")) {
                    std::string owner = "Route";
                    if (std::string(table) == "target" && g_target)
                        if (const LevelNpc* n = FindLevelNpc(g_target))
                            owner = n->id;
                    std::string err;
                    if (engine::ZGEOM* list = MakeNewRoute(owner + "_" + p.name, &err)) {
                        p.bits = engine::RefOf(list);
                        changed = true;
                        g_addList = list;
                        g_pickPending = false;
                        g_pickParam.clear();
                        g_newPointParam.clear();
                        g_message = p.name + " = new route " + engine::SafeString(list->name) +
                                    ": click its points in walking order, Esc ends";
                    } else {
                        g_message = "no route made: " + err;
                    }
                }
                if (route && ImGui::IsItemHovered())
                    ImGui::SetTooltip("A route of this level's own: click its points in the world in walking\n"
                                      "order (Esc ends); the map makes it when it loads again");
                if (ImGui::BeginMenu("A character")) {  // (also one not drawn yet, or far away)
                    for (engine::ZGEOM* c : AllCharacters()) {
                        if (IsRemovedNpc(c))
                            continue;
                        if (ImGui::MenuItem((CharacterName(c) + (IsPlacedNpc(c) ? " *" : "") + "##" +
                                             PathOf(c)).c_str(),
                                            nullptr, p.bits == engine::RefOf(c))) {
                            p.bits = engine::RefOf(c);
                            changed = true;
                        }
                    }
                    ImGui::EndMenu();
                }
                if (ImGui::MenuItem("None", nullptr, false, p.bits != 0)) {
                    p.bits = 0;
                    changed = true;
                }
                engine::ZGEOM* target = p.bits ? engine::GeomOfRef(p.bits) : nullptr;
                if (ImGui::MenuItem("Select it", nullptr, false, target && IsLive(target)))
                    Select(target);
                if (ImGui::MenuItem("Fly to it", nullptr, false, target && IsLive(target))) {
                    FlyCameraTo(GetTransform(target).pos);
                }
                ImGui::EndPopup();
            }
            break;
        }
        case engine::ParamType::String: {
            if (!editable) {
                ImGui::TextUnformatted(p.text.c_str());
                break;
            }
            char buf[256];
            snprintf(buf, sizeof(buf), "%s", p.text.c_str());
            if (ImGui::InputText("##v", buf, sizeof(buf)))
                p.text = buf;
            changed |= ImGui::IsItemDeactivatedAfterEdit();
            break;
        }
        default:
            ImGui::TextDisabled("(%s)", p.typeName.c_str());
            break;
        }
        ImGui::PopID();
    }
    ImGui::EndTable();
    // info box: the parameter clicked last
    std::string prefix = std::string(table) + ":";
    if (g_info.rfind(prefix, 0) == 0) {
        std::string name = g_info.substr(prefix.size());
        for (const engine::ScriptParam& p : s.params) {
            if (p.name != name)
                continue;
            ParamDoc doc = DescribeParam(p.name, p.typeName);
            ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGui::GetStyleColorVec4(ImGuiCol_FrameBg));
            if (ImGui::BeginChild((std::string(table) + "info").c_str(), ImVec2(0, 0),
                                  ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_Borders)) {
                ImGui::Text("%s  (%s)", p.name.c_str(), p.typeName.c_str());
                ImGui::TextWrapped("%s", doc.text.c_str());
                if (doc.inferred)
                    ImGui::TextDisabled("(explained from the name)");
            }
            ImGui::EndChild();
            ImGui::PopStyleColor();
        }
    }
    return changed;
}

// ---- routes: lists of points ----

std::vector<engine::ZGEOM*> ListPointsLive(engine::ZGEOM* list)
{
    std::vector<engine::ZGEOM*> out;
    for (uint32_t r : engine::ListRefs(list))
        if (engine::ZGEOM* g = engine::GeomOfRef(r); g && IsLive(g))
            out.push_back(g);
    return out;
}

// characters whose behaviour uses the list (refreshed once a second)
int ListUsers(engine::ZGEOM* list)
{
    UsersCache& c = g_listUsers[list];
    double now = NowMs();
    if (now - c.time < 1000.0)
        return c.count;
    c.time = now;
    c.count = 0;
    uint32_t ref = engine::RefOf(list);
    for (engine::ZGEOM* a : AllCharacters()) {
        engine::ScriptInfo s;
        if (IsRemovedNpc(a) || !CurrentBehaviour(a, s))
            continue;
        for (const engine::ScriptParam& p : s.params)
            if (p.type == engine::ParamType::Ref && p.bits == ref) {
                ++c.count;
                break;
            }
    }
    return c.count;
}

void SetPoints(engine::ZGEOM* list, const std::vector<engine::ZGEOM*>& pts, const char* what)
{
    std::string err;
    if (SetListPoints(list, pts, &err)) {
        g_message = std::string(what) + " (Ctrl+Z undoes)";
        g_routeCache.clear();
    } else {
        g_message = std::string("route not changed: ") + err;
    }
}

void DrawRouteList(const std::string& param, engine::ZGEOM* list, ImU32 color)
{
    ImGui::PushID(param.c_str());
    std::vector<engine::ZGEOM*> pts = ListPointsLive(list);
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(color), "%s", param.c_str());
    ImGui::SameLine();
    ImGui::TextDisabled("%s, %d points", engine::SafeString(list->name).c_str(), int(pts.size()));
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", PathOf(list).c_str());
    int users = ListUsers(list);
    if (users > 1)
        ImGui::TextDisabled("used by %d characters: a change applies to all of them", users);
    if (engine::RecordOf(list) >= 0) {  // (a list of the scene file: copied with the map)
        if (ImGui::SmallButton("A route of its own")) {
            std::string err, path = GiveOwnRoute(g_target, param, &err);
            g_message = path.empty() ? "not made: " + err
                                     : "its own " + param + " (" + path + ") comes with the map: Test now (save and reload)";
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("A copy of this list for this character only (its points as they are now),\n"
                              "made when the map loads again; then its points can change on their own");
    }
    engine::ZGEOM* sel = Selected();
    int selIndex = -1;
    for (size_t k = 0; k < pts.size(); ++k)
        if (pts[k] == sel)
            selIndex = int(k);
    float rowH = ImGui::GetTextLineHeightWithSpacing();
    float h = rowH * float(std::min<size_t>(std::max<size_t>(pts.size(), 1), 8)) + ImGui::GetStyle().FramePadding.y * 2;
    int moveFrom = -1, moveTo = -1, remove = -1;
    if (ImGui::BeginChild("points", ImVec2(0, h), ImGuiChildFlags_Borders)) {
        if (pts.empty())
            ImGui::TextDisabled("no points: add some");
        float buttons = ImGui::CalcTextSize("^ v x").x + ImGui::GetStyle().FramePadding.x * 6 +
                        ImGui::GetStyle().ItemSpacing.x * 2;
        for (size_t k = 0; k < pts.size(); ++k) {
            ImGui::PushID(int(k));
            char label[160];
            snprintf(label, sizeof(label), "%d. %s", int(k + 1), engine::SafeString(pts[k]->name).c_str());
            if (ImGui::Selectable(label, int(k) == selIndex, ImGuiSelectableFlags_AllowOverlap,
                                  ImVec2(ImGui::GetContentRegionAvail().x - buttons, 0)))
                Select(pts[k]);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s\nclick: select it (then drag it, or G to put it on a surface)",
                                  PathOf(pts[k]).c_str());
            ImGui::SameLine();
            ImGui::BeginDisabled(k == 0);
            if (ImGui::SmallButton("^"))
                moveFrom = int(k), moveTo = int(k) - 1;
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::BeginDisabled(k + 1 == pts.size());
            if (ImGui::SmallButton("v"))
                moveFrom = int(k), moveTo = int(k) + 1;
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::SmallButton("x"))
                remove = int(k);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("take it out of the route (the point stays)");
            ImGui::PopID();
        }
    }
    ImGui::EndChild();
    if (moveFrom >= 0) {
        std::swap(pts[moveFrom], pts[moveTo]);
        SetPoints(list, pts, "point moved in the route");
    } else if (remove >= 0) {
        pts.erase(pts.begin() + remove);
        SetPoints(list, pts, "point taken out of the route");
    }
    if (g_addList == list) {
        ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.45f, 1.0f), "click in the world to add points");
        ImGui::SameLine();
        if (ImGui::SmallButton("Done (Esc)"))
            g_addList = nullptr;
    } else {
        if (ImGui::Button("Add points..."))  {
            g_addList = list;
            g_pickPending = false;
            g_pickParam.clear();
            g_newPointParam.clear();
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Every click in the world adds a point after the selected one\n"
                              "(at the end when none of them is selected)");
        ImGui::SameLine();
        bool canAdd = sel && IsLive(sel) && selIndex < 0 && !engine::IsA(sel, "ZActor") &&
                      !engine::IsEditableList(sel) && !PathOf(sel).empty();
        ImGui::BeginDisabled(!canAdd);
        if (ImGui::Button("Add the selected object")) {
            pts.push_back(sel);
            SetPoints(list, pts, "object added to the route");
        }
        ImGui::EndDisabled();
    }
    ImGui::PopID();
}

void DrawRouteLists(const engine::ScriptInfo& s, engine::ZGEOM* actor)
{
    const std::vector<Route>& routes = RoutesOfActor(actor);
    bool any = false;
    for (const engine::ScriptParam& p : s.params) {
        if (p.type != engine::ParamType::Ref || !p.bits)
            continue;
        engine::ZGEOM* list = engine::GeomOfRef(p.bits);
        if (!list || !IsLive(list) || !engine::IsEditableList(list))
            continue;
        if (!any) {
            ImGui::SeparatorText("Routes");
            any = true;
        }
        ImU32 color = IM_COL32(200, 200, 200, 255);
        for (size_t i = 0; i < routes.size(); ++i)
            if (routes[i].name == p.name)
                color = RouteColor(i);
        DrawRouteList(p.name, list, color);
    }
}

// ---- the edited character ----

void SetTarget(engine::ZGEOM* actor)
{
    if (actor == g_target)
        return;
    g_target = actor;
    g_targetRead = -1e9;
    g_changeTo.clear();
    g_pickParam.clear();
}

void RefreshTarget(bool force)
{
    if (!g_target)
        return;
    double now = NowMs();
    if (!force && (now - g_targetRead < 500.0 || ImGui::IsAnyItemActive() || !g_pickParam.empty()))
        return;
    g_targetRead = now;
    g_targetHasBehaviour = CurrentBehaviour(g_target, g_targetEdit);
}

bool ApplyTarget()
{
    std::string err;
    engine::ZGEOM* now = EditNpcBehaviour(g_target, g_targetHasBehaviour ? &g_targetEdit : nullptr, &err);
    if (!now) {
        g_message = "not changed: " + err;
        RefreshTarget(true);
        return false;
    }
    g_message = "changed (Ctrl+Z undoes)";
    g_routeCache.erase(g_target);
    if (now != g_target) {
        g_target = now;
        Select(now);
    }
    g_targetRead = NowMs();
    return true;
}

std::string BehaviourLabel(const std::string& shortName) { return shortName.empty() ? "none" : shortName; }

void DrawTarget()
{
    engine::ZGEOM* actor = g_target;
    if (!actor || !IsLive(actor) || IsRemovedNpc(actor)) {
        g_target = nullptr;
        return;
    }
    RefreshTarget(false);
    bool placed = IsPlacedNpc(actor);
    ImGui::SeparatorText("Character");
    ImGui::Text("%s", CharacterName(actor).c_str());
    ImGui::SameLine();
    ImGui::TextDisabled(placed ? "(placed by the level)" : "(of the scene)");
    std::string state = engine::ScriptState(actor);
    if (!state.empty())
        ImGui::TextDisabled("now: %s", state.c_str());
    if (ImGui::SmallButton("Select"))
        Select(actor);
    ImGui::SameLine();
    if (ImGui::SmallButton("Fly to")) {
        FlyCameraTo(GetTransform(actor).pos);
    }
    if (placed) {
        ImGui::SameLine();
        if (ImGui::SmallButton("Remove")) {
            std::string err;
            g_message = RemoveNpc(actor, &err) ? "removed (Ctrl+Z undoes)" : "not removed: " + err;
            g_target = nullptr;
            return;
        }
        // its name of the level maker's: lists, goals' texts, h2.find("Volkov")
        static char name[41];
        static engine::ZGEOM* nameOf;
        const LevelNpc* entry = FindLevelNpc(actor);
        if (nameOf != actor) {
            snprintf(name, sizeof(name), "%s", entry ? entry->label.c_str() : "");
            nameOf = actor;
        }
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 9);
        bool enter = ImGui::InputTextWithHint("##npcname", "a name (Volkov)", name, sizeof(name),
                                              ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::SameLine();
        ImGui::BeginDisabled(entry && entry->label == name);
        if (ImGui::SmallButton("Name it") || (enter && entry && entry->label != name)) {
            std::string err;
            g_message = SetNpcLabel(actor, name, &err)
                            ? (name[0] ? std::string("named ") + name + " (scripts: h2.find(\"" + name + "\"))"
                                       : std::string("its name taken off"))
                            : "not named: " + err;
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("A name of its own (letters, digits, spaces, _ and -): shown in the lists and in\n"
                              "the objectives made for it; scripts find it with h2.find(\"name\").");
    }

    if (IsPendingNpc(actor)) {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.45f, 1.0f),
                           "Not real yet: it gets its behaviour, can be bumped into and shot when the map "
                           "loads again (Test).");
        ImGui::PopTextWrapPos();
    }
    std::string current = g_targetHasBehaviour ? engine::ScriptShortName(g_targetEdit.name) : "";
    ImGui::Text("Behaviour: %s", BehaviourLabel(current).c_str());
    if (placed && g_targetHasBehaviour && IsStoryBehaviour(g_targetEdit)) {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.4f, 1.0f),
                           "A part of this map's story: its settings point to the map's characters or vehicles, "
                           "and it acts that part (point them at your own ones, or choose another behaviour).");
        ImGui::PopTextWrapPos();
    }
    if (!current.empty()) {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextDisabled("%s", DescribeBehaviour(current).c_str());
        ImGui::PopTextWrapPos();
    }
    if (placed) {
        // another kind of behaviour: the character is made again
        ImGui::SetNextItemWidth(-ImGui::CalcTextSize("Change").x - ImGui::GetStyle().ItemSpacing.x * 2 - 8);
        if (ImGui::BeginCombo("##changeto", g_changeTo.empty() ? "another behaviour..." : g_changeTo.c_str(),
                              ImGuiComboFlags_HeightLargest)) {
            if (ImGui::Selectable("none (stands still)", g_changeTo == "-"))
                g_changeTo = "-";
            BehaviourChoices(g_changeTo);
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(g_changeTo.empty());
        bool change = ImGui::Button("Change");
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("The character gets it when the map loads again");
        if (change) {
            int points = 0;
            if (g_changeTo == "-") {
                g_targetHasBehaviour = false;
            } else {
                engine::ScriptInfo s;
                if (BehaviourTemplate(g_changeTo, s)) {
                    // made again where it stands, not at the behaviour's start
                    std::vector<StartMove> moves = StartMoves(s);
                    SetRouteWarps(s, moves, false);
                    points = PointStartsAt(s, moves, GetTransform(actor), RoomOf(actor));
                    points += GuardPointAt(s, GetTransform(actor), RoomOf(actor));
                    g_targetEdit = s;
                    g_targetHasBehaviour = true;
                }
            }
            g_changeTo.clear();
            if (ApplyTarget()) {
                JoinLast(points + 1, "character behaviour");
            } else {
                for (int i = 0; i < points; ++i)
                    Undo();
            }
        }
        ImGui::EndDisabled();
    } else {
        ImGui::TextDisabled("(its kind of behaviour stays; its settings can change)");
    }
    if (g_targetHasBehaviour) {
        if (std::string missing = engine::MissingRequiredRef(g_targetEdit); !missing.empty())
            ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.4f, 1.0f), "Needs %s: without it the behaviour does not start.",
                               missing.c_str());
        ImGui::TextDisabled("Settings (live; a change applies when you leave the field):");
        if (DrawParams(g_targetEdit, true, "target"))
            ApplyTarget();
        DrawRouteLists(g_targetEdit, actor);
    }
}

// ---- placing ----

void OnPlacePick(engine::ZGEOM* hit, const float* point, const float*)
{
    g_placePickPending = false;
    g_haveHit = point != nullptr;
    if (point) {
        float dx = point[0] - g_rawPoint[0], dy = point[1] - g_rawPoint[1], dz = point[2] - g_rawPoint[2];
        if (!(dx * dx + dy * dy + dz * dz < 5.0f * 5.0f)) {  // moved (also NaN: the first): look again
            memcpy(g_rawPoint, point, sizeof(g_rawPoint));
            memcpy(g_snappedPoint, point, sizeof(g_snappedPoint));
            g_pointOff = !SnapToNetwork(g_snappedPoint);
        }
        memcpy(g_point, g_snappedPoint, sizeof(g_point));
    }
    if (hit)
        if (engine::ZGEOM* room = RoomOf(hit))
            g_hitRoom = room;
}

// where the character goes: on the surface point, facing the camera
Transform PlacingTransform()
{
    game::ZCAMERA* cam = game::MainCamera();
    float f[3] = {0, 0, 1};
    if (cam) {
        float m[9], c[3];
        game::GetWorldTransform(cam, m, c);
        f[0] = c[0] - g_point[0];
        f[2] = c[2] - g_point[2];
        float len = std::sqrt(f[0] * f[0] + f[2] * f[2]);
        if (len > 1.0f) {
            f[0] /= len;
            f[2] /= len;
        } else {
            f[0] = 0;
            f[2] = 1;
        }
    }
    Transform t;
    float m[9] = {f[0], 0, f[2], 0, 1, 0, f[2], 0, -f[0]};  // rows forward, up, right
    memcpy(t.mat, m, sizeof(m));
    memcpy(t.pos, g_point, sizeof(g_point));
    return t;
}

void AfterGuidePlace(engine::ZGEOM* npc);
void TypeTooltip(int t, const char* more = nullptr);
engine::ZGEOM* g_guideRoute;  // the route made for the character the guide places (its points clicked next)
bool GuideWalks();

void Place()
{
    if (g_type < 0 || !g_haveHit)
        return;
    NpcSpec spec;
    const NpcType& type = g_types[g_type];
    spec.record = type.record;
    spec.mesh = type.mesh;
    spec.importLevel = type.importLevel;
    spec.importMesh = type.importMesh;
    spec.importName = type.importName;
    spec.outfitMesh = type.outfitMesh;
    spec.nudeMesh = type.nudeMesh;
    spec.transform = PlacingTransform();
    spec.hasBehaviour = !g_newBehaviour.empty();
    int points = 0;
    if (spec.hasBehaviour) {
        spec.behaviour = g_new;
        engine::ZGEOM* room = g_hitRoom ? g_hitRoom : RoomOf(engine::RecordGeom(spec.record));
        if (g_startHere)
            points = PointStartsAt(spec.behaviour, g_newMoves, spec.transform, room);
        points += GuardPointAt(spec.behaviour, spec.transform, room);
        // Simple mode's "walks a route": a new route in its behaviour's route setting before it is placed (a
        // placed character is not real until the map loads again: its settings cannot be read back then)
        g_guideRoute = nullptr;
        if (GuideWalks())
            for (engine::ScriptParam& p : spec.behaviour.params)
                if (p.type == engine::ParamType::Ref && IsRouteRef(p.name)) {
                    std::string rerr;
                    if (engine::ZGEOM* list = MakeNewRoute("Route_" + type.model + "_" + p.name, &rerr)) {
                        p.bits = engine::RefOf(list);
                        g_guideRoute = list;
                    } else {
                        Log("npc: no route for the guide's character: %s", rerr.c_str());
                    }
                    break;
                }
    }
    std::string err;
    engine::ZGEOM* npc = PlaceNpc(spec, &err);
    if (!npc) {
        for (int i = 0; i < points; ++i)
            Undo();
        g_message = "placing failed: " + err;
        return;
    }
    JoinLast(points + 1, "place character");
    Select(npc);
    SetTarget(npc);
    g_message = std::string("placed ") + engine::SafeString(npc->name) + " (" + BehaviourLabel(g_newBehaviour) +
                "), Ctrl+Z undoes";
    AfterGuidePlace(npc);  // (its own message)
}

void DrawPlace()
{
    ImGui::SeparatorText("Place a character");
    // as many rows as the panel has room for (at least 7, at most every type)
    float line = ImGui::GetTextLineHeightWithSpacing();
    float listH = std::clamp(line * (float(g_types.size()) + 0.5f), line * 7.0f,
                             std::max(line * 7.0f, ImGui::GetContentRegionAvail().y * 0.45f));
    if (ImGui::BeginChild("types", ImVec2(0, listH), ImGuiChildFlags_Borders)) {
        for (size_t i = 0; i < g_types.size(); ++i) {
            const NpcType& t = g_types[i];
            char label[256];
            snprintf(label, sizeof(label), "%s##t%d", t.model.c_str(), int(i));
            if (ImGui::Selectable(label, g_type == int(i), ImGuiSelectableFlags_AllowOverlap))
                ChooseType(int(i));
            if (ImGui::IsItemHovered()) {
                std::string more = "behaviour: " + (t.script.empty() ? std::string("none") : t.script) + " (e.g. " +
                                   t.group + ")";
                TypeTooltip(int(i), more.c_str());
            }
            ImGui::SameLine(ImGui::GetContentRegionMax().x - ImGui::CalcTextSize(t.script.c_str()).x);
            ImGui::TextDisabled("%s", t.script.c_str());
        }
    }
    ImGui::EndChild();
    if (g_type < 0) {
        ImGui::TextDisabled("choose a character to place one");
        return;
    }
    ImGui::SetNextItemWidth(-1);
    if (ImGui::BeginCombo("##behaviour", BehaviourLabel(g_newBehaviour).c_str(), ImGuiComboFlags_HeightLargest)) {
        if (ImGui::Selectable("none (stands still)", g_newBehaviour.empty()))
            ChooseNewBehaviour("");
        std::string chosen = g_newBehaviour;
        if (BehaviourChoices(chosen))
            ChooseNewBehaviour(chosen);
        ImGui::EndCombo();
    }
    if (!g_newBehaviour.empty()) {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextDisabled("%s", DescribeBehaviour(g_newBehaviour).c_str());
        if (std::string missing = engine::MissingRequiredRef(g_new); !missing.empty())
            ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.4f, 1.0f),
                               "Needs %s (pick a list of the map below): without it the character stands still.",
                               missing.c_str());
        ImGui::PopTextWrapPos();
        if (!g_newMoves.empty()) {
            if (ImGui::Checkbox("Starts where you put it", &g_startHere))
                SetRouteWarps(g_new, g_newMoves, !g_startHere);
            if (ImGui::IsItemHovered()) {
                std::string names;
                for (const StartMove& m : g_newMoves)
                    names += "\n  " + m.name + (m.ref ? " (its start point)" : " (route)");
                ImGui::SetTooltip("This behaviour moves the character when it starts:%s\n\n"
                                  "On: it starts where you click, then walks its route.\n"
                                  "Off: it jumps to its start as in the original level.",
                                  names.c_str());
            }
        }
        DrawParams(g_new, true, "new");
    }
    if (g_placing) {
        ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.45f, 1.0f), "click in the world to place it");
        ImGui::SameLine();
        if (ImGui::SmallButton("Cancel (Esc)"))
            CancelNpcPlacement();
    } else if (ImGui::Button("Place character", ImVec2(-1, 0))) {
        g_placing = true;
        g_placePickPending = false;
        g_haveHit = false;
        g_rawPoint[0] = NAN;
        g_message.clear();
    }
}

void DrawList()
{
    std::vector<engine::ZGEOM*> all = AllCharacters();
    char header[64];
    snprintf(header, sizeof(header), "Characters (%d)###chars", int(all.size()));
    if (!ImGui::CollapsingHeader(header))
        return;
    for (size_t i = 0; i < all.size(); ++i) {
        engine::ZGEOM* g = all[i];
        ImGui::PushID(int(i));
        engine::ScriptInfo s;
        std::string b = CurrentBehaviour(g, s) ? engine::ScriptShortName(s.name) : "-";
        char label[192];
        snprintf(label, sizeof(label), "%s%s", CharacterName(g).c_str(), IsPlacedNpc(g) ? " *" : "");
        if (ImGui::Selectable(label, g == g_target, ImGuiSelectableFlags_AllowOverlap)) {
            Select(g);
            SetTarget(g);
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s\nnow: %s%s", PathOf(g).c_str(), engine::ScriptState(g).c_str(),
                              IsPlacedNpc(g) ? "\n* placed by the level" : "");
        ImGui::SameLine(ImGui::GetContentRegionMax().x - ImGui::CalcTextSize(b.c_str()).x);
        ImGui::TextDisabled("%s", b.c_str());
        ImGui::PopID();
    }
}

// ---- Simple mode's People step (docs/simple-mode.md, #34) ----

enum class Who { Guard, Civilian, Target };
Who g_who = Who::Guard;
bool g_walks = false;          // walks a route (else: stays around where it is put)
int g_look = -1;               // the type chosen for the guide (index in g_types)
bool g_guidePlacing = false;   // the placing was started by the guide: its steps after placing
Who g_guideWho = Who::Guard;
bool g_guideWalks = false;
std::map<std::string, bool> g_walksCache;  // behaviour -> has a route setting

// a model's name to read: "BodyguardMafia_01" -> "Bodyguard Mafia" (an import keeps its "(C3-1)")
std::string Readable(const std::string& model)
{
    std::string base = model, tail;
    size_t paren = base.find(" (");
    if (paren != std::string::npos) {
        tail = base.substr(paren);
        base = base.substr(0, paren);
    }
    while (!base.empty() && (std::isdigit(static_cast<unsigned char>(base.back())) || base.back() == '_'))
        base.pop_back();
    std::string out;
    for (size_t i = 0; i < base.size(); ++i) {
        char c = base[i];
        if (c == '_') {
            if (!out.empty() && out.back() != ' ')
                out += ' ';
            continue;
        }
        bool upper = std::isupper(static_cast<unsigned char>(c));
        bool afterLower = i && std::islower(static_cast<unsigned char>(base[i - 1]));
        bool acronymEnd = i && i + 1 < base.size() && std::isupper(static_cast<unsigned char>(base[i - 1])) &&
                          std::islower(static_cast<unsigned char>(base[i + 1]));
        if (upper && (afterLower || acronymEnd) && !out.empty() && out.back() != ' ')
            out += ' ';
        out += c;
    }
    return (out.empty() ? model : out) + tail;
}

// a person's name to read: its name in the level ("Volkov"), else its model's
std::string ReadableName(engine::ZGEOM* actor)
{
    std::string name = engine::SafeString(actor->name);
    for (const LevelNpc& n : CurrentLevel().npcs)
        if (n.id == name && !n.label.empty())
            return n.label;
    return Readable(CharacterName(actor));
}

bool TypeIsGuard(const NpcType& t)
{
    return IsGuardBehaviour(t.script.empty() ? t.model : t.script) || IsGuardBehaviour(t.model);
}

bool HasRoute(const engine::ScriptInfo& s)
{
    for (const engine::ScriptParam& p : s.params)
        if (p.type == engine::ParamType::Ref && IsRouteRef(p.name))
            return true;
    return false;
}

bool BehaviourWalks(const std::string& name)
{
    auto it = g_walksCache.find(name);
    if (it != g_walksCache.end())
        return it->second;
    engine::ScriptInfo s;
    bool walks = BehaviourTemplate(name, s) && HasRoute(s);
    g_walksCache[name] = walks;
    return walks;
}

// a behaviour of that kind (guard or not) that walks a route: the map's own first, then one of another
// level's that works anywhere (the map's own seldom have a route: C1-1's and C2-2's guards stand or go
// between points) ("" none)
std::string WalkingBehaviour(bool guard)
{
    for (const std::string& b : g_behaviourNames)
        if (!g_storyBehaviours.count(b) && IsGuardBehaviour(b) == guard && BehaviourWalks(b))
            return b;
    for (const std::string& b : ForeignBehaviourNames())
        if (IsGuardBehaviour(b) == guard && BehaviourWalks(b))
            return b;
    return "";
}

// ---- the hovered character's picture (People step, NPCs tab): a preview of it stands in front of the
// camera, its draws go into the thumbnail (highlight.h), as the Models tab's models ----
int g_hoverType = -1;      // hovered this frame (index in g_types)
int g_hoverCandidate = -1; // hovered since g_hoverSince
int g_thumbType = -1;      // the one in the thumbnail
double g_hoverSince, g_lastTypeHover, g_thumbPlaced;
constexpr double kTypeHoverDelay = 0.15, kTypeHoverLinger = 0.5;

// in front of the camera (to be drawn), its front towards it
Transform InFrontOfCamera()
{
    Transform t;
    game::ZCAMERA* cam = game::MainCamera();
    if (!cam)
        return t;
    float m[9], c[3];
    game::GetWorldTransform(cam, m, c);
    // a character's draws (skinned) take the game camera's view, not the thumbnail's turntable: what the
    // camera sees of it is the picture. Near, facing the camera, its middle (its place: about its hips) at the
    // camera's height
    float fx = m[0], fz = m[2], len = std::sqrt(fx * fx + fz * fz);  // (its front then shows)
    if (len < 0.01f) {
        fx = 0;
        fz = 1;
    } else {
        fx /= len;
        fz /= len;
    }
    float mat[9] = {fx, 0, fz, 0, 1, 0, fz, 0, -fx};  // rows forward, up, right
    memcpy(t.mat, mat, sizeof(mat));
    for (int i = 0; i < 3; ++i)
        t.pos[i] = c[i] + m[i] * 350.0f;
    t.pos[1] = c[1];
    return t;
}

void TypeThumbTick()
{
    double now = ImGui::GetTime();
    int hovered = g_hoverType;
    g_hoverType = -1;
    if (g_placing) {  // (the placing's own preview: not taken over)
        if (g_thumbType >= 0)
            SetThumbnailGeoms({});
        g_thumbType = -1;
        return;
    }
    if (hovered >= 0 && hovered < int(g_types.size())) {
        g_lastTypeHover = now;
        if (hovered != g_hoverCandidate) {
            g_hoverCandidate = hovered;
            g_hoverSince = now;
        }
        if (now - g_hoverSince >= kTypeHoverDelay && g_thumbType != hovered) {
            const NpcType& type = g_types[hovered];
            std::string err;
            if (engine::ZGEOM* a = ShowPreviewNpc(type.record, InFrontOfCamera(), &err, type.mesh)) {
                // its room follows its place only as the world runs: a few frames of it while the editor
                // holds the world (as a character moved across rooms, level.cpp)
                if (ScenePaused())
                    StepWorld(2);
                std::vector<engine::ZGEOM*> parts = VisibleParts(a);  // (the character's own draws too: its body)
                parts.push_back(a);
                SetThumbnailGeoms(parts);
                Transform at = GetTransform(a);  // framed on its body (a character's place is about its hips)
                SetThumbnailFrame(at.pos, 92.0f, 0.10f);
                g_thumbType = hovered;
                g_thumbPlaced = now;
            }
        }
    } else if (g_thumbType >= 0 && now - g_lastTypeHover > kTypeHoverLinger) {
        HidePreviewNpc();
        SetThumbnailGeoms({});
        ReleaseThumbnail();
        g_thumbType = -1;
    }
}

// the tooltip of a character of the list: its name, a picture of it
void TypeTooltip(int t, const char* more)
{
    g_hoverType = t;
    const NpcType& type = g_types[t];
    ImGui::BeginTooltip();
    ImGui::TextUnformatted(Readable(type.model).c_str());
    ImGui::TextDisabled("%s, %d on the map", type.model.c_str(), type.count);
    if (more)
        ImGui::TextDisabled("%s", more);
    // the character stands in the same part of the picture whatever the camera (its skinned draws keep the
    // camera's view): that part, enlarged
    const ImVec2 uv0(0.30f, 0.33f), uv1(0.70f, 0.95f);
    float h = 260.0f * (ImGui::GetFontSize() / 16.0f), w = h * (uv1.x - uv0.x) / (uv1.y - uv0.y);
    void* tex = g_thumbType == t ? ThumbnailTexture() : nullptr;
    if (tex)
        ImGui::Image(reinterpret_cast<ImTextureID>(tex), ImVec2(w, h), uv0, uv1);
    else
        ImGui::Dummy(ImVec2(w, h));  // (the picture comes a moment later)
    ImGui::EndTooltip();
}

// the guide's choice as the NPCs tab's: the type, then its behaviour (one that walks when asked)
bool ChooseForGuide()
{
    if (g_look < 0 || g_look >= int(g_types.size()))
        return false;
    ChooseType(g_look);
    if (!g_walks || HasRoute(g_new))
        return true;
    std::string b = WalkingBehaviour(TypeIsGuard(g_types[g_look]));
    if (b.empty())
        return false;
    ChooseNewBehaviour(b);
    return true;
}

// after a character placed by the guide: a target's objective, a route's points
void AfterGuidePlace(engine::ZGEOM* npc)
{
    if (!g_guidePlacing || !npc)
        return;
    if (g_guideWho == Who::Target)
        MissionGoalFor(npc, "target");
    g_message = "Placed: " + ReadableName(npc) + (g_guideWho == Who::Target ? " (a target now)" : "") +
                ". Ctrl+Z takes it back.";
    if (!g_guideWalks)
        return;
    if (!g_guideRoute || !IsLive(g_guideRoute)) {
        g_message += " Its behaviour has no route to walk: it stays where it stands.";
        return;
    }
    Select(npc);
    g_addList = g_guideRoute;  // its points next: each click in the world adds one
    g_guideRoute = nullptr;
    g_placing = false;
    g_message = "Placed: " + ReadableName(npc) + ". Now click the route's points in the world, in walking order "
                "(Esc: done). It walks them when you try the level.";
}

bool GuideWalks() { return g_guidePlacing && g_guideWalks; }

void DrawSimplePeopleStep()
{
    if (CurrentScene().empty() || engine::SceneRecords().empty() || !engine::SceneDataValid()) {
        ImGui::TextWrapped("Load the level's map first.");
        return;
    }
    if (g_builtFor != BuildKey()) {
        Build();
        g_look = -1;
        g_walksCache.clear();
    }
    if (!g_message.empty()) {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.45f, 1.0f), "%s", g_message.c_str());
        ImGui::PopTextWrapPos();
    }

    ImGui::SeparatorText("Add a person");
    ImGui::TextUnformatted("Who?");
    Who before = g_who;
    if (ImGui::RadioButton("A guard", g_who == Who::Guard))
        g_who = Who::Guard;
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Armed; attacks Hitman when it sees him where he may not be");
    ImGui::SameLine();
    if (ImGui::RadioButton("A civilian", g_who == Who::Civilian))
        g_who = Who::Civilian;
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Harmless; may run away or call the guards");
    ImGui::SameLine();
    if (ImGui::RadioButton("A target", g_who == Who::Target))
        g_who = Who::Target;
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("A person Hitman has to kill: it becomes an objective (step 1)");
    if (g_who != before)
        g_look = -1;

    // looks like: the map's characters of that kind, the most common first
    std::vector<int> looks;
    for (size_t i = 0; i < g_types.size(); ++i)
        if (g_who == Who::Target || TypeIsGuard(g_types[i]) == (g_who == Who::Guard))
            looks.push_back(int(i));
    std::stable_sort(looks.begin(), looks.end(),
                     [](int a, int b) { return g_types[a].count > g_types[b].count; });
    if (looks.empty()) {
        ImGui::TextDisabled("This map has no one of that kind to copy.");
        return;
    }
    if (std::find(looks.begin(), looks.end(), g_look) == looks.end())
        g_look = looks.front();
    ImGui::TextUnformatted("Looks like:");
    ImGui::SetNextItemWidth(-1);
    // a look of the map's: its model, and what it does there when the model is there with more behaviours
    auto lookName = [&](int i) {
        std::string name = Readable(g_types[i].model);
        int same = 0;
        for (int j : looks)
            same += g_types[j].model == g_types[i].model;
        if (same > 1 && !g_types[i].script.empty())
            name += " - " + Readable(g_types[i].script);
        return name;
    };
    if (ImGui::BeginCombo("##look", lookName(g_look).c_str(), ImGuiComboFlags_HeightLarge)) {
        for (int i : looks) {
            char label[200];
            snprintf(label, sizeof(label), "%s##%d", lookName(i).c_str(), i);
            if (ImGui::Selectable(label, g_look == i))
                g_look = i;
            if (ImGui::IsItemHovered())
                TypeTooltip(i);
        }
        ImGui::EndCombo();
    }

    ImGui::TextUnformatted("What does it do?");
    bool canWalk = BehaviourWalks(g_types[g_look].script) || !WalkingBehaviour(TypeIsGuard(g_types[g_look])).empty();
    if (ImGui::RadioButton(g_who == Who::Guard ? "Guards the spot where it stands" : "Stays around where it stands",
                           !g_walks))
        g_walks = false;
    ImGui::BeginDisabled(!canWalk);
    if (ImGui::RadioButton("Walks a route", g_walks && canWalk))
        g_walks = true;
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) && canWalk)
        ImGui::SetTooltip("After placing it, click the route's points in walking order (Esc ends)");
    if (!canWalk) {
        g_walks = false;
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextDisabled("(no behaviour of this map or of the other levels walks a route for %s)",
                            TypeIsGuard(g_types[g_look]) ? "a guard" : "someone of this kind");
        ImGui::PopTextWrapPos();
    }

    if (g_placing && g_guidePlacing) {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.45f, 1.0f), "Click in the world to place it (Shift: more, Esc: stop)");
        ImGui::PopTextWrapPos();
    } else if (ImGui::Button("Place it", ImVec2(-1, 0))) {
        if (ChooseForGuide()) {
            g_guidePlacing = true;
            g_guideWho = g_who;
            g_guideWalks = g_walks;
            g_placing = true;
            g_placePickPending = false;
            g_haveHit = false;
            g_rawPoint[0] = NAN;
            g_message.clear();
        } else {
            g_message = "No behaviour of this map fits: try another look";
        }
    }
    if (!g_placing && g_addList == nullptr)
        g_guidePlacing = false;

    if (int pending = PendingNpcCount()) {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextDisabled(pending == 1 ? "The new person comes to life when you try the level (step 5)."
                                         : "The %d new people come to life when you try the level (step 5).",
                            pending);
        ImGui::PopTextWrapPos();
    }

    // the level's people
    ImGui::SeparatorText("The level's people");
    int shown = 0;
    for (engine::ZGEOM* a : AllCharacters()) {
        bool placed = IsPlacedNpc(a);
        bool hidden = !placed && HiddenByLevel(a);  // (not the map's own hidden ones)
        if (!placed && !hidden)
            continue;
        ++shown;
        ImGui::PushID(a);
        std::string name = ReadableName(a) + (hidden ? " (taken out)" : " (new)");
        const char* button = hidden ? "Put back" : "Remove";
        float bw = ImGui::CalcTextSize(button).x + ImGui::GetStyle().FramePadding.x * 2;
        if (ImGui::Selectable(name.c_str(), Selected() == a, ImGuiSelectableFlags_AllowOverlap,
                              ImVec2(ImGui::GetContentRegionAvail().x - bw - ImGui::GetStyle().ItemSpacing.x, 0))) {
            Select(a);
            FlyCameraTo(GetTransform(a).pos);
        }
        ImGui::SameLine(ImGui::GetContentRegionMax().x - bw);
        if (hidden) {
            if (ImGui::SmallButton(button)) {
                ObjectState b = GetState(a), after = b;
                after.hidden = false;
                Change(a, b, after, "show");
            }
        } else {
            if (ImGui::SmallButton(button)) {
                std::string err;
                g_message = RemoveNpc(a, &err) ? "removed (Ctrl+Z undoes)" : "not removed: " + err;
                if (g_target == a)
                    g_target = nullptr;
                ImGui::PopID();
                break;
            }
        }
        ImGui::PopID();
    }
    if (!shown)
        ImGui::TextDisabled("No one added or removed yet.");
    engine::ZGEOM* sel = ActorOf(Selected());
    if (sel && !IsPlacedNpc(sel) && !IsHidden(sel) && sel != engine::Player()) {  // (shown: one to take out)
        if (ImGui::Button(("Take " + ReadableName(sel) + " out of the map").c_str())) {
            ObjectState b = GetState(sel), after = b;
            after.hidden = true;
            Change(sel, b, after, "hide");
        }
    } else {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextDisabled("To take one of the map's people out: click it in the world.");
        ImGui::PopTextWrapPos();
    }
}

void DrawPanel()
{
    if (CurrentScene().empty()) {
        ImGui::TextDisabled("no scene loaded");
        return;
    }
    if (engine::SceneRecords().empty() || !engine::SceneDataValid()) {
        ImGui::TextWrapped("The scene's data was not captured: load the level again (it is read while loading).");
        return;
    }
    if (g_builtFor != BuildKey())
        Build();
    if (engine::ZGEOM* a = ActorOf(Selected()))
        SetTarget(a);

    ImGui::SetNextItemWidth(ImGui::CalcTextSize("selected character").x + ImGui::GetFrameHeight() +
                            ImGui::GetStyle().FramePadding.x * 2);  // the longest choice fits
    ImGui::Combo("Routes", &g_routes, "off\0selected character\0all characters\0");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Where the characters will go: their routes (numbered, with arrows in walking order)\n"
                          "and the places in their behaviour (guard point, protect point, ...)");
    ImGui::SameLine();
    if (ImGui::GetContentRegionAvail().x < ImGui::GetFrameHeight() + ImGui::GetStyle().ItemInnerSpacing.x +
                                               ImGui::CalcTextSize("Onto the paths").x)
        ImGui::NewLine();  // no room: on the next line
    if (ImGui::Checkbox("Onto the paths", &g_snapNetwork))
        g_rawPoint[0] = NAN;  // the preview looks again
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Placed characters and new route points go to the nearest place characters can walk\n"
                          "(the map's path network, %.0f m at most); off it they do not walk, the point is red.\n"
                          "Testing > Path network shows it.",
                          kSnapRadius / 100.0f);
    if (!g_message.empty())
        ImGui::TextWrapped("%s", g_message.c_str());
    // the next one of the kind placed last, without scrolling down to the list (#20)
    if (g_type >= 0 && g_type < int(g_types.size()) && !g_placing && g_target) {
        if (ImGui::SmallButton(("Place another " + g_types[g_type].model).c_str())) {
            g_placing = true;
            g_placePickPending = false;
            g_haveHit = false;
            g_rawPoint[0] = NAN;
            g_message.clear();
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Click in the world to place one more, with the behaviour and settings below in\n"
                              "Place a character (%s)", BehaviourLabel(g_newBehaviour).c_str());
    }
    if (int pending = PendingNpcCount()) {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.45f, 1.0f),
                           "%d placed character%s not real yet (no behaviour, nothing bumps into %s, %s cannot be "
                           "shot): the map makes %s when it loads again (Test).",
                           pending, pending == 1 ? " is" : "s are", pending == 1 ? "it" : "them",
                           pending == 1 ? "it" : "they", pending == 1 ? "it" : "them");
        ImGui::PopTextWrapPos();
        if (ImGui::Button("Test now (save and reload)", ImVec2(-1, 0))) {
            g_target = nullptr;
            TestLevel();
        }
    }

    if (g_target) {
        DrawTarget();
        if (g_target)
            DrawMissionForSelection(g_target);
            DrawStagesForSelection(g_target);
    } else
        ImGui::TextDisabled("Select a character (click it, or in the list below) to see and change its behaviour.");
    DrawList();
    DrawPlace();
}

// pick mode result
void OnParamPick(engine::ZGEOM* hit, const float*, const float*)
{
    g_pickPending = false;
    if (g_pickParam.empty() || !hit)
        return;
    engine::ScriptInfo& s = g_pickTable == "new" ? g_new : g_targetEdit;
    for (engine::ScriptParam& p : s.params) {
        if (p.name != g_pickParam || p.type != engine::ParamType::Ref)
            continue;
        p.bits = engine::RefOf(hit);
        g_message = p.name + " = " + engine::SafeString(hit->name);
    }
    bool target = g_pickTable == "target";
    g_pickParam.clear();
    if (target && g_target)
        ApplyTarget();
}

Transform PointAt(const float p[3])
{
    Transform t = {{0, 0, 1, 0, 1, 0, 1, 0, 0}, {p[0], p[1], p[2]}};
    return t;
}

// "add points" mode: a new point after the selected one of the list
void OnAddPoint(engine::ZGEOM* hit, const float* point, const float*)
{
    g_pickPending = false;
    engine::ZGEOM* list = g_addList;
    if (!list || !IsLive(list))
        return;
    if (!point) {
        g_message = "no surface there";
        return;
    }
    std::vector<engine::ZGEOM*> pts = ListPointsLive(list);
    // with the list's other points (their group), else in the room clicked
    engine::ZGEOM* parent = nullptr;
    if (!pts.empty() && pts[0]->node && pts[0]->node->parent && pts[0]->node->parent->geom)
        parent = pts[0]->node->parent->geom;
    if (!parent || PathOf(parent).empty())
        parent = hit ? RoomOf(hit) : nullptr;
    if (!parent)
        parent = Resolve("ROOT");
    float at[3] = {point[0], point[1], point[2]};
    bool on = SnapToNetwork(at);
    std::string err;
    engine::ZGEOM* np = PlacePoint(PointAt(at), parent, &err);
    if (!np) {
        g_message = "no point made: " + err;
        return;
    }
    g_message = on ? "" : "the point is off the path network: the character will not get there";
    auto it = std::find(pts.begin(), pts.end(), Selected());
    pts.insert(it == pts.end() ? pts.end() : it + 1, np);
    SetPoints(list, pts, "point added");
    Select(np);  // the next one comes after it
}

// "new point" mode: a new point for a setting
void OnNewPoint(engine::ZGEOM* hit, const float* point, const float*)
{
    g_pickPending = false;
    std::string param = g_newPointParam, table = g_newPointTable;
    g_newPointParam.clear();
    if (param.empty())
        return;
    if (!point) {
        g_message = "no surface there";
        return;
    }
    engine::ZGEOM* room = hit ? RoomOf(hit) : nullptr;
    if (!room)
        room = Resolve("ROOT");
    float at[3] = {point[0], point[1], point[2]};
    bool on = SnapToNetwork(at);
    std::string err;
    engine::ZGEOM* np = PlacePoint(PointAt(at), room, &err);
    if (!np) {
        g_message = "no point made: " + err;
        return;
    }
    engine::ScriptInfo& s = table == "new" ? g_new : g_targetEdit;
    for (engine::ScriptParam& p : s.params)
        if (p.name == param && p.type == engine::ParamType::Ref)
            p.bits = engine::RefOf(np);
    g_message = param + " = new point " + engine::SafeString(np->name) +
                (on ? "" : " (off the path network: the character will not get there)");
    if (table == "target" && g_target)
        ApplyTarget();
}

} // namespace

void DrawSimplePeople() { DrawSimplePeopleStep(); }
std::string PersonName(engine::ZGEOM* actor) { return actor ? ReadableName(actor) : ""; }

bool NpcPlacing() { return g_placing || !g_pickParam.empty() || g_addList || !g_newPointParam.empty(); }

void CancelNpcPlacement()
{
    if (g_placing) {
        HidePreviewNpc();
        SetPickIgnore(nullptr);
    }
    g_placing = false;
    g_placePickPending = false;
    g_pickParam.clear();
    g_addList = nullptr;
    g_newPointParam.clear();
}

engine::ZGEOM* RoutePointAt(float x, float y)
{
    engine::ZGEOM* best = nullptr;
    float bestD = 10.0f * 10.0f;
    for (const Marker& m : g_markers) {
        float dx = m.screen.x - x, dy = m.screen.y - y;
        float d = dx * dx + dy * dy;
        if (d < bestD && IsLive(m.point)) {
            bestD = d;
            best = m.point;
        }
    }
    return best;
}

void UpdateNpcPlacement()
{
    ImGuiIO& io = ImGui::GetIO();
    if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        CancelNpcPlacement();
        return;
    }
    bool overWorld = !io.WantCaptureMouse && !ImGui::IsMouseDown(ImGuiMouseButton_Right);
    if (g_addList || !g_newPointParam.empty()) {  // new points where the user clicks
        if (g_addList && !IsLive(g_addList)) {
            g_addList = nullptr;
            return;
        }
        ImDrawList* dl = ImGui::GetForegroundDrawList();
        std::string text = g_addList ? "click: add a point to " + engine::SafeString(g_addList->name) +
                                           " (after the selected point)   Esc: done"
                                     : "click: new point for " + g_newPointParam + "   Esc: cancel";
        ImVec2 p(io.MousePos.x + 18, io.MousePos.y + 18);
        ImVec2 size = ImGui::CalcTextSize(text.c_str());
        dl->AddRectFilled(ImVec2(p.x - 6, p.y - 4), ImVec2(p.x + size.x + 6, p.y + size.y + 4), IM_COL32(0, 0, 0, 170),
                          4);
        dl->AddText(p, IM_COL32(255, 220, 120, 255), text.c_str());
        if (g_pickPending && ImGui::GetFrameCount() - g_pickFrame > 10)
            g_pickPending = false;
        if (overWorld && !g_pickPending && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            g_pickPending = true;
            g_pickFrame = ImGui::GetFrameCount();
            RequestPick(io.MousePos.x, io.MousePos.y, g_addList ? OnAddPoint : OnNewPoint);
        }
        return;
    }
    if (!g_pickParam.empty()) {  // pick an object for a setting
        ImDrawList* dl = ImGui::GetForegroundDrawList();
        std::string text = "click the object to use as " + g_pickParam + "   Esc: cancel";
        ImVec2 p(io.MousePos.x + 18, io.MousePos.y + 18);
        ImVec2 size = ImGui::CalcTextSize(text.c_str());
        dl->AddRectFilled(ImVec2(p.x - 6, p.y - 4), ImVec2(p.x + size.x + 6, p.y + size.y + 4), IM_COL32(0, 0, 0, 170),
                          4);
        dl->AddText(p, IM_COL32(255, 220, 120, 255), text.c_str());
        // characters by their place (the picking draws see no skinned body: the ground behind it was
        // taken, #15), anything else by the picking
        engine::ZGEOM* who = overWorld ? CharacterAt(io.MousePos.x, io.MousePos.y) : nullptr;
        if (who)
            dl->AddText(ImVec2(p.x, p.y + size.y + 6), IM_COL32(255, 220, 120, 255),
                        ("click: " + CharacterName(who)).c_str());
        if (overWorld && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            if (who) {
                OnParamPick(who, nullptr, nullptr);
            } else {
                g_pickPending = true;
                g_pickFrame = ImGui::GetFrameCount();
                RequestPick(io.MousePos.x, io.MousePos.y, OnParamPick);
            }
        }
        return;
    }
    if (!g_placing)
        return;
    if (g_type < 0) {
        CancelNpcPlacement();
        return;
    }
    if (!overWorld)
        return;
    if (g_placePickPending && ImGui::GetFrameCount() - g_placePickFrame > 10)
        g_placePickPending = false;
    if (!g_placePickPending) {
        g_placePickPending = true;
        g_placePickFrame = ImGui::GetFrameCount();
        RequestPick(io.MousePos.x, io.MousePos.y, OnPlacePick);
    }
    if (g_haveHit) {
        if (g_routes != kRoutesOff && !g_newBehaviour.empty()) {
            float from[3] = {g_point[0], g_point[1] + 100.0f, g_point[2]};
            DrawRoutes(from, RoutesOf(g_new), !overlay::SimpleMode());
        }
        DrawSnapTarget(g_point, g_pointOff ? IM_COL32(255, 80, 64, 255) : IM_COL32(64, 224, 255, 255));
        if (!g_pointOff && (g_point[0] != g_rawPoint[0] || g_point[2] != g_rawPoint[2]))
            DrawLine3D(g_rawPoint, g_point, IM_COL32(64, 224, 255, 140), 1.5f);  // where it was pulled from
        // the character itself where it will stand
        std::string err;
        if (engine::ZGEOM* preview =
                ShowPreviewNpc(g_types[g_type].record, PlacingTransform(), &err, g_types[g_type].mesh)) {
            SetPickIgnore(preview);
        } else {
            float top[3] = {g_point[0], g_point[1] + 180.0f, g_point[2]};
            DrawLine3D(g_point, top, IM_COL32(64, 224, 255, 200), 2.0f);  // about a person's height
        }
    }
    char text[256];
    snprintf(text, sizeof(text), "%s (%s)\nclick: place   Shift+click: place more   Esc: cancel%s",
             g_types[g_type].model.c_str(), BehaviourLabel(g_newBehaviour).c_str(),
             !g_haveHit  ? "\nno surface under the mouse"
             : g_pointOff ? "\noff the path network: it will not walk from there"
                          : "");
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    ImVec2 p(io.MousePos.x + 18, io.MousePos.y + 18);
    ImVec2 size = ImGui::CalcTextSize(text);
    dl->AddRectFilled(ImVec2(p.x - 6, p.y - 4), ImVec2(p.x + size.x + 6, p.y + size.y + 4), IM_COL32(0, 0, 0, 170), 4);
    dl->AddText(p, IM_COL32(255, 220, 120, 255), text);
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && g_haveHit) {
        HidePreviewNpc();
        Place();
        if (!io.KeyShift) {
            g_placing = false;
            SetPickIgnore(nullptr);
        }
    }
}

void InstallNpcPanel()
{
    engine::InstallSceneData();
    overlay::AddPanel({"NPCs", &g_visible, DrawPanel, overlay::Dock::Left});
    static bool always = true;
    overlay::AddPanel({nullptr, &always, [] {
                           if (!g_placing)
                               DrawAllRoutes();
                           TypeThumbTick();
                       }});
    // placing ended some other way (also with the editor closed): the preview goes (not while it is the
    // picture of a hovered character)
    overlay::AddAlwaysDraw([] {
        if ((!g_placing && g_thumbType < 0) || !overlay::IsOpen())
            HidePreviewNpc();
        if (!overlay::IsOpen())
            g_thumbType = -1;
    });
    OnScenePushed([](const std::string&) {
        g_builtFor.clear();
        g_types.clear();
        g_behaviourNames.clear();
        g_routeCache.clear();
        g_info.clear();
        g_message.clear();  // (#20: "placed ..." of the map before)
        g_type = -1;
        g_target = nullptr;
        g_hitRoom = nullptr;
        CancelNpcPlacement();
    });
}

} // namespace h2::editor
