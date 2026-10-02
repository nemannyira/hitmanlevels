#include "mission.h"

#include <windows.h>
#include <cctype>
#include <cmath>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "imgui.h"

#include "../engine/messages.h"
#include "../engine/scene_data.h"
#include "../hook.h"
#include "../log.h"
#include "../mainloop.h"
#include "../overlay/overlay.h"
#include "../scene.h"
#include "../script/events.h"
#include "freecam.h"
#include "level.h"
#include "model.h"
#include "music.h"
#include "npcs.h"
#include "scene_panel.h"

namespace h2::editor {

namespace {

constexpr uintptr_t kLocFind = 0x428FF0;          // stdcall (const char* path, const uint8_t* root) -> node
constexpr uintptr_t kSetLocaleFolder = 0x52D340;  // BriefingMenu::(const char* folder)
constexpr uintptr_t kSetObjective = 0x52E2E0;     // BriefingMenu::(int slot, const char* key)
constexpr uintptr_t kSetCompleted = 0x52E350;     // BriefingMenu::(int* {slot, done})
constexpr size_t kBriefingDoneBits = 0x610;       // BriefingMenu: bit n = line n checked
constexpr uintptr_t kLevelInit = 0x504410;        // level routine (of LevelControl): registers its messages
constexpr int kLevel_MissionFailed = 0xC0 / 4;    // what its MissionFailed / MissionCompleted messages call
constexpr int kLevel_MissionCompleted = 0xC4 / 4;
constexpr int kSlots = 6;
const char* const kEmptyKey = "H2Editor/Empty";

using LocFind_t = uint8_t*(__stdcall*)(const char* path, const uint8_t* root);
using SetFolder_t = void(__fastcall*)(void* self, void* edx, const char* folder);
using SetObjective_t = void(__fastcall*)(void* self, void* edx, int slot, const char* key);
using SetCompleted_t = void(__fastcall*)(void* self, void* edx, int* param);
LocFind_t oLocFind;
SetFolder_t oSetFolder;
SetObjective_t oSetObjective;
using LevelInit_t = void(__fastcall*)(void* self, void* edx);
LevelInit_t oLevelInit;
void* g_level;                             // the level routine (mission end)
SetCompleted_t oSetCompleted;

void* g_briefing;                          // the level's briefing menu (this scene)
std::string g_folder;                      // its LOC folder ("C1-1")
const uint8_t* g_locRoot;                  // LOC tree the folder's texts were found in
std::map<std::string, std::string> g_texts;  // lower-case LOC path -> node: 0x01, Latin-1 text, NUL
std::map<std::string, std::string> g_scriptTexts;  // ... set by scripts (h2.objective)
std::string g_lineKeys[kSlots + 1];         // the level's LOC key of each line (as it set them)
bool g_ours;                               // the editor itself checks a line
bool g_restoring;                          // lines go back to the level's texts
bool g_panelVisible = true;
std::string g_status;

// runtime state of the editor's goals
bool g_done[kSlots + 1];
bool g_ended;
double g_lastCheck;
bool g_applied;                            // the mission was applied to this scene
std::map<engine::ZGEOM*, bool> g_alive;    // characters' life (the "death" event)

MissionDoc& Doc() { return CurrentLevel().mission; }

template <typename T>
T VFunc(void* obj, int index)
{
    return reinterpret_cast<T>((*reinterpret_cast<void***>(obj))[index]);
}

bool ForThisScene()
{
    const LevelDocument& L = CurrentLevel();
    return !CurrentScene().empty() && (L.baseScene.empty() || L.baseScene == CurrentScene());
}

std::string Lower(std::string s)
{
    for (char& c : s)
        c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// the editor's own LOC key of a line (its text answered by hkLocFind)
const char* EditorKey(int slot)
{
    static std::string keys[kSlots + 1];
    if (keys[slot].empty())
        keys[slot] = "H2Editor/Line/" + std::to_string(slot);
    return keys[slot].c_str();
}

// the text the level document gives a line ("" = the level's own)
std::string EditorText(int slot);
bool InDocument(int slot);
bool EmptyMap();

// UTF-8 -> the game's 8-bit text (Latin-1; letters it lacks lose their accent)
std::string ToGameText(const std::string& utf8)
{
    std::string out;
    for (size_t i = 0; i < utf8.size();) {
        unsigned char c = utf8[i];
        uint32_t cp = c;
        int n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 1;
        if (n > 1 && i + n <= utf8.size()) {
            cp = c & (0xFF >> (n + 1));
            for (int k = 1; k < n; ++k)
                cp = (cp << 6) | (utf8[i + k] & 0x3F);
        }
        i += n;
        if (cp == 0x151) cp = 0xF6;       // ő -> ö
        else if (cp == 0x150) cp = 0xD6;  // Ő -> Ö
        else if (cp == 0x171) cp = 0xFC;  // ű -> ü
        else if (cp == 0x170) cp = 0xDC;  // Ű -> Ü
        out += cp < 0x100 ? char(cp) : '?';
    }
    return out;
}

std::string FromGameText(const char* s)
{
    std::string out;
    for (; *s; ++s) {
        unsigned char c = *s;
        if (c < 0x80) {
            out += char(c);
        } else {
            out += char(0xC0 | (c >> 6));
            out += char(0x80 | (c & 0x3F));
        }
    }
    return out;
}

// ---- hooks ----

uint8_t* __stdcall hkLocFind(const char* path, const uint8_t* root)
{
    if (path && !g_texts.empty()) {
        const char* p = path;
        while (*p == '/')
            ++p;
        char key[160];
        size_t n = 0;
        for (; p[n] && n + 1 < sizeof(key); ++n)
            key[n] = char(std::tolower(static_cast<unsigned char>(p[n])));
        key[n] = 0;
        auto it = g_texts.find(key);
        if (it != g_texts.end())
            return reinterpret_cast<uint8_t*>(it->second.data());
    }
    uint8_t* node = oLocFind(path, root);
    if (node && !g_locRoot && path && !g_folder.empty() && _strnicmp(path, g_folder.c_str(), g_folder.size()) == 0)
        g_locRoot = root;
    return node;
}

void __fastcall hkSetFolder(void* self, void* edx, const char* folder)
{
    g_briefing = self;
    g_folder = folder ? folder : "";
    g_locRoot = nullptr;
    oSetFolder(self, edx, folder);
}

void __fastcall hkLevelInit(void* self, void* edx)
{
    g_level = self;
    oLevelInit(self, edx);
}

bool CallLevel(int index)
{
    if (!g_level)
        return false;
    __try {
        VFunc<void(__thiscall*)(void*)>(g_level, index)(g_level);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// a line gets a text: the level's keys are noted; lines the editor gives a
// text keep the editor's key
void __fastcall hkSetObjective(void* self, void* edx, int slot, const char* key)
{
    g_briefing = self;
    if (slot >= 1 && slot <= kSlots) {
        if (key && _strnicmp(key, "H2Editor/", 9) != 0)
            g_lineKeys[slot] = key;
        if (!g_restoring && !EditorText(slot).empty())
            key = EditorKey(slot);
        else if (!g_restoring && EmptyMap() && !InDocument(slot))
            key = kEmptyKey;  // an empty map has no objectives of the level
    }
    oSetObjective(self, edx, slot, key);
}

bool Owned(int slot)
{
    if (!ForThisScene())
        return false;
    for (const MissionObjective& o : Doc().objectives)
        if (o.slot == slot && o.goal != MissionObjective::Level && o.replacesOriginal)
            return true;
    return false;
}

void __fastcall hkSetCompleted(void* self, void* edx, int* param)
{
    g_briefing = self;
    if (!g_ours && param && Owned(param[0])) {
        Log("mission: the level's check of objective %d ignored (the editor decides it)", param[0]);
        return;
    }
    oSetCompleted(self, edx, param);
}

// ---- the laptop's lines ----

bool CallSetObjective(int slot, const char* key)
{
    if (!g_briefing)
        return false;
    __try {
        hkSetObjective(g_briefing, nullptr, slot, key);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// the briefing window's element by name (e.g. "Objectives\\5")
void* Element(const char* name)
{
    if (!g_briefing)
        return nullptr;
    void* window = *reinterpret_cast<void**>(static_cast<char*>(g_briefing) + 0x6E);
    if (!window || !engine::IsReadable(window, 4))
        return nullptr;
    __try {
        return VFunc<void*(__thiscall*)(void*, const char*, int)>(window, 0x240 / 4)(window, name, 0);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

bool SetVisible(void* element, bool visible)
{
    if (!element)
        return false;
    __try {
        VFunc<void(__thiscall*)(void*, int)>(element, 0xC0 / 4)(element, visible ? 1 : 0);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// lines the level does not use are hidden in its laptop
void ShowLineElements(int slot)
{
    char name[64];
    for (const char* part : {"", "\\Text", "\\Check"}) {
        snprintf(name, sizeof(name), "Objectives\\%d%s", slot, part);
        SetVisible(Element(name), true);
    }
}

bool CallSetCompleted(int slot, bool done)
{
    if (!g_briefing)
        return false;
    int param[2] = {slot, done ? 1 : 0};
    g_ours = true;
    bool ok = true;
    __try {
        oSetCompleted(g_briefing, nullptr, param);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = false;
    }
    g_ours = false;
    return ok;
}

bool LineChecked(int slot)
{
    if (!g_briefing || !engine::IsReadable(static_cast<char*>(g_briefing) + kBriefingDoneBits, 4))
        return false;
    uint32_t bits = *reinterpret_cast<uint32_t*>(static_cast<char*>(g_briefing) + kBriefingDoneBits);
    return (bits >> slot) & 1;
}

std::string Node(const std::string& text) { return std::string("\x01", 1) + ToGameText(text) + std::string(1, '\0'); }

// texts the editor answers in the LOC tree
void BuildTexts()
{
    g_texts = g_scriptTexts;
    g_texts[Lower(kEmptyKey)] = Node("");
    if (!ForThisScene())
        return;
    for (const MissionObjective& o : Doc().objectives)
        if (!o.text.empty())
            g_texts[Lower(EditorKey(o.slot))] = Node(o.text);
}

bool InDocument(int slot)
{
    if (!ForThisScene())
        return false;
    for (const MissionObjective& o : Doc().objectives)
        if (o.slot == slot)
            return true;
    return false;
}

bool EmptyMap() { return ForThisScene() && CurrentLevel().emptyMap; }

std::string EditorText(int slot)
{
    if (!ForThisScene())
        return "";
    for (const MissionObjective& o : Doc().objectives)
        if (o.slot == slot)
            return o.text;
    return "";
}

const uint8_t* SafeLocFind(const char* key, const uint8_t* root)
{
    __try {
        return oLocFind(key, root);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

std::string OwnText(int slot)
{
    if (!g_locRoot || !oLocFind || slot < 1 || slot > kSlots || g_lineKeys[slot].empty() || EmptyMap())
        return "";
    const uint8_t* node = SafeLocFind(g_lineKeys[slot].c_str(), g_locRoot);
    if (!node || !(node[0] & 1))
        return "";
    return FromGameText(reinterpret_cast<const char*>(node + 1));
}

// lines the level's laptop shows (the last one it gave a text)
int LevelLines()
{
    int n = 0;
    for (int slot = 1; slot <= kSlots; ++slot)
        if (!g_lineKeys[slot].empty())
            n = slot;
    return n;
}

// a line back to the level's text (or empty)
void RestoreLine(int slot)
{
    std::string key = g_lineKeys[slot];
    CallSetObjective(slot, key.empty() ? kEmptyKey : key.c_str());
}

// shows the document's lines in the laptop
void ShowLines(const std::vector<int>& removed = {})
{
    BuildTexts();
    for (int slot : removed)
        RestoreLine(slot);
    for (int slot = 1; slot <= kSlots; ++slot)  // the level's lines (taken out on an empty map)
        if (!InDocument(slot) && !g_lineKeys[slot].empty())
            RestoreLine(slot);
    for (const MissionObjective& o : Doc().objectives) {
        bool own = !OwnText(o.slot).empty();
        if (!o.text.empty())
            CallSetObjective(o.slot, EditorKey(o.slot));
        else if (own)
            CallSetObjective(o.slot, g_lineKeys[o.slot].c_str());
        if (!own) {  // a new line: shown, its check mark
            ShowLineElements(o.slot);
            CallSetCompleted(o.slot, g_done[o.slot]);
        }
    }
}

// ---- goals ----

engine::ZGEOM* Target(const MissionObjective& o)
{
    engine::ZGEOM* g = o.target.empty() ? nullptr : Resolve(o.target);
    return g && IsLive(g) ? g : nullptr;
}

float DistanceToPlayer(engine::ZGEOM* place)
{
    engine::ZGEOM* hitman = engine::Player();
    if (!hitman || !place)
        return 1e30f;
    Transform a = GetTransform(hitman), b = GetTransform(place);
    float d[3] = {a.pos[0] - b.pos[0], a.pos[1] - b.pos[1], a.pos[2] - b.pos[2]};
    return std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
}

bool Reached(const MissionObjective& o)
{
    switch (o.goal) {
    case MissionObjective::Kill: {
        engine::ZGEOM* t = Target(o);
        return t && !engine::IsAlive(t);
    }
    case MissionObjective::Reach:
        return DistanceToPlayer(Target(o)) <= o.radius;
    default:
        return LineChecked(o.slot);
    }
}

std::vector<engine::ZGEOM*> Characters();

// the "death" event for level scripts
void WatchDeaths()
{
    for (engine::ZGEOM* a : Characters()) {
        bool alive = engine::IsAlive(a);
        auto it = g_alive.find(a);
        if (it != g_alive.end() && it->second && !alive) {
            std::string path = PathOf(a);
            if (!path.empty())
                script::FireDeath(path);
        }
        g_alive[a] = alive;
    }
}

void Check()
{
    double now = double(GetTickCount64());
    if (now - g_lastCheck < 250.0)
        return;
    g_lastCheck = now;
    if (engine::SceneDataValid() && !CurrentScene().empty())
        WatchDeaths();
    if (!ForThisScene() || !engine::SceneDataValid() || (Doc().objectives.empty() && !Doc().endWhenDone))
        return;
    if (!g_applied) {  // the level was not applied (e.g. a mission edited on a scene without a level yet)
        g_applied = true;
        ShowLines();
    }
    bool all = true;
    for (const MissionObjective& o : Doc().objectives) {
        if (o.goal == MissionObjective::Level) {
            all &= LineChecked(o.slot);
            continue;
        }
        if (!g_done[o.slot] && Reached(o)) {
            g_done[o.slot] = true;
            CallSetCompleted(o.slot, true);
            Log("mission: objective %d done", o.slot);
            script::FireObjective(o.slot);
        }
        all &= g_done[o.slot];
    }
    if (!Doc().endWhenDone || g_ended || !all || Doc().objectives.empty())
        return;
    if (!Doc().exitPath.empty()) {
        engine::ZGEOM* exit = Resolve(Doc().exitPath);
        if (!exit || DistanceToPlayer(exit) > Doc().exitRadius)
            return;
    }
    g_ended = CompleteMission();
}

void OnScene(const std::string&)
{
    g_briefing = nullptr;
    g_level = nullptr;
    g_folder.clear();
    g_locRoot = nullptr;
    g_ended = false;
    g_applied = false;
    g_alive.clear();
    memset(g_done, 0, sizeof(g_done));
    for (std::string& k : g_lineKeys)
        k.clear();
    g_scriptTexts.clear();
    BuildTexts();
}

// ---- panel ----

const char* GoalName(MissionObjective::Goal g)
{
    switch (g) {
    case MissionObjective::Kill: return "kill a character";
    case MissionObjective::Reach: return "reach a place";
    default: return "the level's own";
    }
}

std::string Label(const std::string& path)
{
    if (path.empty())
        return "(none)";
    engine::ZGEOM* g = Resolve(path);
    if (!g || !IsLive(g))
        return "(missing) " + path.substr(path.rfind('/') + 1);
    std::string name = engine::SafeString(g->name);
    if (engine::ZGEOM* a = ActorOf(g)) {
        int rec = NpcRecordOf(a);
        if (rec >= 0)
            return NpcModel(rec) + (IsPlacedNpc(a) ? " (placed)" : "");
    }
    return name;
}

std::vector<engine::ZGEOM*> Characters()
{
    std::vector<engine::ZGEOM*> out;
    for (int rec : NpcRecords())
        if (engine::ZGEOM* g = engine::RecordGeom(rec); g && IsLive(g))
            out.push_back(g);
    for (engine::ZGEOM* g : PlacedNpcs())
        if (IsLive(g) && !IsRemovedNpc(g))
            out.push_back(g);
    return out;
}

void Changed()
{
    CurrentLevel().dirty = true;
    if (CurrentLevel().baseScene.empty())
        CurrentLevel().baseScene = CurrentScene();
    ShowLines();
}

// a place: the selected object or a new point where Hitman stands
bool PlaceChooser(std::string& path, const char* id)
{
    bool changed = false;
    ImGui::PushID(id);
    ImGui::TextUnformatted(Label(path).c_str());
    if (!path.empty() && ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", path.c_str());
    engine::ZGEOM* sel = Selected();
    ImGui::BeginDisabled(!sel || !IsLive(sel) || PathOf(sel).empty());
    if (ImGui::SmallButton("use selected")) {
        path = PathOf(sel);
        changed = true;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::SmallButton("point at Hitman")) {
        engine::ZGEOM* hitman = engine::Player();
        engine::ZGEOM* room = hitman ? RoomOf(hitman) : nullptr;
        if (!room)
            room = Resolve("ROOT");
        std::string err;
        Transform t = hitman ? GetTransform(hitman) : Transform{};
        engine::ZGEOM* p = hitman ? PlacePoint(t, room, &err) : nullptr;
        if (p) {
            path = PathOf(p);
            changed = true;
        } else {
            g_status = "no point made: " + (hitman ? err : std::string("Hitman not found"));
        }
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Makes an invisible point where Hitman stands now (walk there first)");
    if (!path.empty()) {
        ImGui::SameLine();
        if (ImGui::SmallButton("fly to")) {
            if (engine::ZGEOM* g = Resolve(path)) {
                Transform t = GetTransform(g);
                float cam[3] = {t.pos[0] - 300.0f, t.pos[1] + 250.0f, t.pos[2] - 300.0f};
                SetFreeCamEnabled(true);
                SetFreeCamPosition(cam);
                SetFreeCamAngles(45.0f, -30.0f);
            }
        }
    }
    ImGui::PopID();
    return changed;
}

bool TargetChooser(std::string& path)
{
    bool changed = false;
    std::string label = Label(path);
    ImGui::SetNextItemWidth(-1);
    if (ImGui::BeginCombo("##target", label.c_str())) {
        std::vector<engine::ZGEOM*> all = Characters();
        for (size_t i = 0; i < all.size(); ++i) {
            std::string p = PathOf(all[i]);
            int rec = NpcRecordOf(all[i]);
            char item[200];
            snprintf(item, sizeof(item), "%s%s##%d", rec >= 0 ? NpcModel(rec).c_str() : "?",
                     IsPlacedNpc(all[i]) ? " (placed)" : "", int(i));
            if (ImGui::Selectable(item, p == path)) {
                path = p;
                changed = true;
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", p.c_str());
        }
        ImGui::EndCombo();
    }
    engine::ZGEOM* a = ActorOf(Selected());
    ImGui::BeginDisabled(!a);
    if (ImGui::SmallButton("use the selected character")) {
        path = PathOf(a);
        changed = true;
    }
    ImGui::EndDisabled();
    if (engine::ZGEOM* t = path.empty() ? nullptr : Resolve(path)) {
        ImGui::SameLine();
        if (ImGui::SmallButton("select"))
            Select(t);
        ImGui::SameLine();
        ImGui::TextDisabled(engine::IsAlive(t) ? "alive" : "dead");
    }
    return changed;
}

void DrawObjective(MissionObjective& o, int index, int& remove)
{
    ImGui::PushID(index);
    std::string own = OwnText(o.slot);
    bool done = o.goal == MissionObjective::Level ? LineChecked(o.slot) : g_done[o.slot];
    ImGui::Separator();
    ImGui::Text("Objective %d", o.slot);
    ImGui::SameLine();
    ImGui::TextColored(done ? ImVec4(0.5f, 1.0f, 0.5f, 1.0f) : ImVec4(0.7f, 0.7f, 0.7f, 1.0f),
                       done ? "done" : "not done");
    ImGui::SameLine(ImGui::GetContentRegionMax().x - ImGui::CalcTextSize("remove").x - 8);
    if (ImGui::SmallButton("remove"))
        remove = index;
    if (!own.empty())
        ImGui::TextDisabled("level: %s", own.c_str());
    else if (o.slot > LevelLines() && LevelLines() > 0)
        ImGui::TextWrapped("(this level's laptop shows %d lines: this one is not shown there, "
                           "the editor still checks it)", LevelLines());
    char buf[256];
    snprintf(buf, sizeof(buf), "%s", o.text.c_str());
    ImGui::SetNextItemWidth(-1);
    if (ImGui::InputTextWithHint("##text", own.empty() ? "text in the laptop" : "new text (empty: the level's)", buf,
                                 sizeof(buf)))
        o.text = buf;
    if (ImGui::IsItemDeactivatedAfterEdit())
        Changed();
    ImGui::SetNextItemWidth(-1);
    if (ImGui::BeginCombo("##goal", GoalName(o.goal))) {
        for (int g = 0; g < 3; ++g)
            if (ImGui::Selectable(GoalName(MissionObjective::Goal(g)), o.goal == g)) {
                o.goal = MissionObjective::Goal(g);
                Changed();
            }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("How it is done:\n- the level's own: the level's scripts check it (as in the game)\n"
                          "- kill a character: done when that character dies\n"
                          "- reach a place: done when Hitman gets near it");
    if (o.goal == MissionObjective::Kill) {
        if (TargetChooser(o.target))
            Changed();
    } else if (o.goal == MissionObjective::Reach) {
        if (PlaceChooser(o.target, "reach"))
            Changed();
        ImGui::SetNextItemWidth(120);
        if (ImGui::DragFloat("within", &o.radius, 5.0f, 50.0f, 5000.0f, "%.0f cm"))
            CurrentLevel().dirty = true;
    }
    if (o.goal != MissionObjective::Level && !own.empty()) {
        if (ImGui::Checkbox("the level's own check no longer counts", &o.replacesOriginal))
            Changed();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("E.g. the original target's death no longer checks this line:\n"
                              "only the goal above does");
    }
    ImGui::PopID();
}

void DrawPanel()
{
    if (CurrentScene().empty()) {
        ImGui::TextDisabled("no scene loaded");
        return;
    }
    MissionDoc& m = Doc();
    if (!CurrentLevel().baseScene.empty() && CurrentLevel().baseScene != CurrentScene()) {
        ImGui::TextWrapped("The level being edited is for %s.", CurrentLevel().baseScene.c_str());
        return;
    }
    ImGui::TextWrapped("The mission's objectives are the lines of the laptop's \"Mission Objectives\". "
                       "Change their texts, give them a target or a place to reach, or add new ones.");
    if (!g_briefing)
        ImGui::TextDisabled("(this scene has no laptop briefing yet)");
    // the level's own lines first (when not in the document)
    for (int slot = 1; slot <= kSlots; ++slot) {
        bool listed = false;
        for (const MissionObjective& o : m.objectives)
            listed |= o.slot == slot;
        std::string own = listed ? "" : OwnText(slot);
        if (own.empty())
            continue;
        ImGui::PushID(slot + 100);
        if (ImGui::SmallButton("change")) {
            MissionObjective o;
            o.slot = slot;
            m.objectives.push_back(o);
            Changed();
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Change this objective's text or what completes it");
        ImGui::SameLine();
        ImGui::TextWrapped("%d. %s%s", slot, own.c_str(), LineChecked(slot) ? "  (done)" : "");
        ImGui::PopID();
    }
    int remove = -1;
    for (size_t i = 0; i < m.objectives.size(); ++i)
        DrawObjective(m.objectives[i], int(i), remove);
    if (remove >= 0) {
        int slot = m.objectives[remove].slot;
        m.objectives.erase(m.objectives.begin() + remove);
        CurrentLevel().dirty = true;
        ShowLines({slot});
    }
    int free = 0;
    for (int slot = 1; slot <= kSlots && !free; ++slot) {
        bool used = !OwnText(slot).empty();
        for (const MissionObjective& o : m.objectives)
            used |= o.slot == slot;
        if (!used)
            free = slot;
    }
    ImGui::Spacing();
    ImGui::BeginDisabled(!free);
    if (ImGui::Button("Add an objective")) {
        MissionObjective o;
        o.slot = free;
        o.goal = MissionObjective::Kill;
        o.text = "New objective";
        m.objectives.push_back(o);
        Changed();
    }
    ImGui::EndDisabled();
    if (!free) {
        ImGui::SameLine();
        ImGui::TextDisabled("(the laptop has 6 lines)");
    }

    ImGui::SeparatorText("End of the mission");
    if (ImGui::Checkbox("Completed when every objective is done", &m.endWhenDone))
        Changed();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Without this the level's own scripts end the mission (as in the game)");
    if (m.endWhenDone) {
        ImGui::TextDisabled("and Hitman reaches (none: at once):");
        if (PlaceChooser(m.exitPath, "exit"))
            Changed();
        if (!m.exitPath.empty()) {
            ImGui::SameLine();
            if (ImGui::SmallButton("clear")) {
                m.exitPath.clear();
                Changed();
            }
            ImGui::SetNextItemWidth(120);
            if (ImGui::DragFloat("within##exit", &m.exitRadius, 5.0f, 50.0f, 5000.0f, "%.0f cm"))
                CurrentLevel().dirty = true;
        }
    }
    ImGui::SeparatorText("Music");
    DrawMusicSection();
    ImGui::SeparatorText("Test");
    if (ImGui::SmallButton("Complete mission now"))
        g_status = CompleteMission() ? "mission completed" : "the level control was not found";
    ImGui::SameLine();
    if (ImGui::SmallButton("Fail mission"))
        g_status = FailMission() ? "mission failed" : "the level control was not found";
    engine::ZGEOM* a = ActorOf(Selected());
    ImGui::BeginDisabled(!a || !engine::IsAlive(a));
    if (ImGui::SmallButton("Kill the selected character"))
        g_status = engine::KillActor(a) ? "killed" : "not killed";
    ImGui::EndDisabled();
    if (!g_status.empty())
        ImGui::TextDisabled("%s", g_status.c_str());
}

} // namespace

bool SetObjectiveText(int slot, const std::string& text)
{
    if (slot < 1 || slot > kSlots)
        return false;
    std::string key = Lower("H2Editor/Script/" + std::to_string(slot));
    g_scriptTexts[key] = Node(text);
    BuildTexts();
    if (text.empty()) {
        RestoreLine(slot);
        return g_briefing != nullptr;
    }
    return CallSetObjective(slot, key.c_str());
}

bool SetObjectiveDone(int slot, bool done)
{
    if (slot < 1 || slot > kSlots)
        return false;
    g_done[slot] = done;
    return CallSetCompleted(slot, done);
}

bool CompleteMission()
{
    Log("mission: completed by the editor");
    return CallLevel(kLevel_MissionCompleted);
}

bool FailMission()
{
    Log("mission: failed by the editor");
    return CallLevel(kLevel_MissionFailed);
}

std::string LevelObjectiveText(int slot) { return OwnText(slot); }

void ApplyMission()
{
    g_applied = true;
    ShowLines();
}

void RevertMission()
{
    std::vector<int> slots;
    for (const MissionObjective& o : Doc().objectives)
        slots.push_back(o.slot);
    g_texts = g_scriptTexts;
    g_texts[Lower(kEmptyKey)] = Node("");
    memset(g_done, 0, sizeof(g_done));
    g_ended = false;
    g_restoring = true;
    for (int slot : slots)
        RestoreLine(slot);
    g_restoring = false;
    g_applied = false;
}

void InstallMission()
{
    Hook("LOC find", kLocFind, hkLocFind, &oLocFind);
    Hook("BriefingMenu::SetLocaleFolder", kSetLocaleFolder, hkSetFolder, &oSetFolder);
    Hook("BriefingMenu::SetObjective", kSetObjective, hkSetObjective, &oSetObjective);
    Hook("Level::InitMessages", kLevelInit, hkLevelInit, &oLevelInit);
    Hook("BriefingMenu::SetObjectiveCompleted", kSetCompleted, hkSetCompleted, &oSetCompleted);
    overlay::AddPanel({"Mission", &g_panelVisible, DrawPanel, overlay::Dock::Left});
    OnScenePushed(OnScene);
    OnFrameEnd(Check);
}

} // namespace h2::editor
