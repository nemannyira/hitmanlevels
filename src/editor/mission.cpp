#include "mission.h"

#include <windows.h>
#include <algorithm>
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
#include "../game.h"
#include "../scene.h"
#include "../script/events.h"
#include "freecam.h"
#include "level.h"
#include "model.h"
#include "music.h"
#include "npcs.h"
#include "scene_panel.h"
#include "viewport.h"
#include "highlight.h"

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
std::map<std::string, std::string> g_fixedTexts;   // ... of the editor itself (main menu entry)
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

// An element shown: the window file hides the lines a level does not use
// with node flag 0x400; vfunc 0xC0 (0x5BAEF0) is SetHidden(bool), flag 0x800.
constexpr uintptr_t kNode_SetFlags = 0x5BAC80;  // ZBaseGeom::(set, clear)

bool SetVisible(void* element, bool visible)
{
    if (!element)
        return false;
    __try {
        void* node = static_cast<engine::ZGEOM*>(element)->node;
        if (visible)
            reinterpret_cast<void(__thiscall*)(void*, uint32_t, uint32_t)>(kNode_SetFlags)(node, 0, 0x400);
        VFunc<void(__thiscall*)(void*, int)>(element, 0xC0 / 4)(element, visible ? 0 : 1);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

int g_shownLines;          // the last line shown beyond the level's own (0: none)
float g_savesY = -1e30f;   // the "saves left" line's own place (local y); -1e30: not known

// The "saves left" line goes below the last line shown (in a level with 4
// lines it is where line 5 is). Lines are siblings in "Objectives", one
// line height apart.
engine::ZGEOM* LocalY(const char* name, float& y)
{
    auto* e = static_cast<engine::ZGEOM*>(Element(name));
    if (!e || !engine::IsReadable(e->node, sizeof(engine::ZBaseGeom)))
        return nullptr;
    y = e->node->pos[1];
    return e;
}

void PlaceSavesLeft(int lastLine)
{
    char name[64];
    snprintf(name, sizeof(name), "Objectives\\%d", lastLine);
    float y1, y2, yLast, ySaves;
    engine::ZGEOM* saves = LocalY("Objectives\\SavesLeft", ySaves);
    if (!saves || !LocalY("Objectives\\1", y1) || !LocalY("Objectives\\2", y2) || !LocalY(name, yLast))
        return;
    if (g_savesY < -1e29f)
        g_savesY = ySaves;
    float want = std::max(g_savesY, yLast + (y2 - y1));
    if (want == ySaves)
        return;
    engine::ZBaseGeom* n = saves->node;
    float pos[3] = {n->pos[0], want, n->pos[2]};
    float mat[9];
    memcpy(mat, n->mat, sizeof(mat));
    __try {
        game::SetLocalTransform(saves, mat, pos);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
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
    g_shownLines = std::max(g_shownLines, slot);
    PlaceSavesLeft(g_shownLines);
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
    for (auto& [k, v] : g_fixedTexts)
        g_texts[k] = v;
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
    g_shownLines = 0;
    g_savesY = -1e30f;
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

std::string Label(const std::string& path)
{
    if (path.empty())
        return "";
    engine::ZGEOM* g = Resolve(path);
    if (!g || !IsLive(g))
        return "(missing) " + path.substr(path.rfind('/') + 1);
    if (engine::ZGEOM* a = ActorOf(g)) {
        int rec = NpcRecordOf(a);
        if (rec >= 0)
            return NpcModel(rec) + (IsPlacedNpc(a) ? " (placed)" : "");
    }
    return engine::SafeString(g->name);
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

MissionObjective* ObjectiveAt(int slot)
{
    for (MissionObjective& o : Doc().objectives)
        if (o.slot == slot)
            return &o;
    return nullptr;
}

// a line of the level becomes editable: an entry of the document
MissionObjective& Editable(int slot)
{
    if (MissionObjective* o = ObjectiveAt(slot))
        return *o;
    MissionObjective o;
    o.slot = slot;
    Doc().objectives.push_back(o);
    return Doc().objectives.back();
}

// the line's text as the laptop shows it
std::string LineText(int slot)
{
    MissionObjective* o = ObjectiveAt(slot);
    if (o && !o->text.empty())
        return o->text;
    return OwnText(slot);
}

std::string Summary(const MissionObjective* o)
{
    if (!o || o->goal == MissionObjective::Level)
        return "the level decides";
    std::string what = Label(o->target);
    if (o->goal == MissionObjective::Kill)
        return what.empty() ? "kill: choose who" : what + " dies";
    return what.empty() ? "reach: choose where" : "Hitman reaches " + what;
}

bool LineDone(int slot)
{
    MissionObjective* o = ObjectiveAt(slot);
    return o && o->goal != MissionObjective::Level ? g_done[slot] : LineChecked(slot);
}

// text that follows the goal while the user has not written their own
bool AutoText(const std::string& text)
{
    return text.empty() || text.rfind("Kill ", 0) == 0 || text.rfind("Reach ", 0) == 0 || text == "New objective";
}

void SetTarget(MissionObjective& o, const std::string& path)
{
    o.target = path;
    if (AutoText(o.text) && !path.empty())  // the laptop's text follows the new target
        o.text = (o.goal == MissionObjective::Kill ? "Kill " : "Reach ") + Label(path);
    Changed();
}

// ---- picking in the world ----

enum class PickKind { None, Character, Place };
PickKind g_pick = PickKind::None;
int g_pickSlot;          // the objective's line; 0 = the mission's exit
bool g_pickPending;
int g_pickFrame;
int g_selSlot = 1;       // the objective being edited

void StartPick(PickKind kind, int slot)
{
    g_pick = kind;
    g_pickSlot = slot;
    g_pickPending = false;
}

void OnPlacePick(engine::ZGEOM* hit, const float* point, const float*)
{
    g_pickPending = false;
    if (g_pick != PickKind::Place)
        return;
    if (!point) {
        g_status = "no surface there: click the ground";
        return;
    }
    engine::ZGEOM* room = hit ? RoomOf(hit) : nullptr;
    if (!room)
        room = Resolve("ROOT");
    Transform t = {{0, 0, 1, 0, 1, 0, 1, 0, 0}, {point[0], point[1], point[2]}};
    std::string err;
    engine::ZGEOM* p = PlacePoint(t, room, &err);
    g_pick = PickKind::None;
    if (!p) {
        g_status = "no point made: " + err;
        return;
    }
    std::string path = PathOf(p);
    if (g_pickSlot == 0) {
        Doc().exitPath = path;
        Changed();
    } else {
        SetTarget(Editable(g_pickSlot), path);
    }
    g_status = "place set (an invisible point: move it later from the Scene tab)";
}

void DrawHint(const char* text)
{
    ImGuiIO& io = ImGui::GetIO();
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    ImVec2 p(io.MousePos.x + 18, io.MousePos.y + 18);
    ImVec2 size = ImGui::CalcTextSize(text);
    dl->AddRectFilled(ImVec2(p.x - 6, p.y - 4), ImVec2(p.x + size.x + 6, p.y + size.y + 4), IM_COL32(0, 0, 0, 170), 4);
    dl->AddText(p, IM_COL32(255, 220, 120, 255), text);
}

// ---- the editor of one line ----

bool PlaceButtons(int slot, const std::string& current)
{
    bool changed = false;
    bool picking = g_pick == PickKind::Place && g_pickSlot == slot;
    if (ImGui::SmallButton(picking ? "click the ground... (Esc)" : "Pick in the world"))
        picking ? (void)(g_pick = PickKind::None) : StartPick(PickKind::Place, slot);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Click the ground where Hitman has to get: an invisible point is made there");
    ImGui::SameLine();
    if (ImGui::SmallButton("Where Hitman stands")) {
        engine::ZGEOM* hitman = engine::Player();
        engine::ZGEOM* room = hitman ? RoomOf(hitman) : nullptr;
        if (!room)
            room = Resolve("ROOT");
        std::string err;
        engine::ZGEOM* p = hitman ? PlacePoint(GetTransform(hitman), room, &err) : nullptr;
        if (p) {
            if (slot == 0) {
                Doc().exitPath = PathOf(p);
                Changed();
            } else {
                SetTarget(Editable(slot), PathOf(p));
            }
            changed = true;
        } else {
            g_status = "no point made: " + (hitman ? err : std::string("Hitman not found"));
        }
    }
    if (!current.empty()) {
        ImGui::SameLine();
        if (ImGui::SmallButton("Fly to")) {
            if (engine::ZGEOM* g = Resolve(current)) {
                Transform t = GetTransform(g);
                float cam[3] = {t.pos[0] - 300.0f, t.pos[1] + 250.0f, t.pos[2] - 300.0f};
                SetFreeCamEnabled(true);
                SetFreeCamPosition(cam);
                SetFreeCamAngles(45.0f, -30.0f);
            }
        }
    }
    return changed;
}

void DrawLineEditor(int slot)
{
    MissionObjective* o = ObjectiveAt(slot);
    std::string own = OwnText(slot);
    ImGui::PushID(slot);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGui::GetStyleColorVec4(ImGuiCol_FrameBg));
    ImGui::BeginChild("line", ImVec2(0, 0), ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_Borders);
    ImGui::Text("Objective %d", slot);
    if (!own.empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled("(the level's line)");
    } else if (LevelLines() > 0 && slot > LevelLines()) {
        ImGui::SameLine();
        ImGui::TextDisabled("(a new line: the laptop lists it below the level's %d)", LevelLines());
    }

    // text
    ImGui::TextUnformatted("Text in the laptop");
    char buf[256];
    snprintf(buf, sizeof(buf), "%s", o ? o->text.c_str() : "");
    ImGui::SetNextItemWidth(-1);
    if (ImGui::InputTextWithHint("##text", own.empty() ? "e.g. Kill the lawyer" : own.c_str(), buf, sizeof(buf)))
        Editable(slot).text = buf;
    if (ImGui::IsItemDeactivatedAfterEdit())
        Changed();

    // goal
    ImGui::Spacing();
    ImGui::TextUnformatted("Done when");
    int goal = o ? int(o->goal) : int(MissionObjective::Level);
    bool goalChanged = false;
    if (!own.empty() || goal == int(MissionObjective::Level))
        goalChanged |= ImGui::RadioButton("the level decides (as in the game)", &goal,
                                          int(MissionObjective::Level));
    goalChanged |= ImGui::RadioButton("a character dies", &goal, int(MissionObjective::Kill));
    goalChanged |= ImGui::RadioButton("Hitman reaches a place", &goal, int(MissionObjective::Reach));
    if (goalChanged) {
        MissionObjective& e = Editable(slot);
        if (e.goal != MissionObjective::Goal(goal)) {
            e.goal = MissionObjective::Goal(goal);
            e.target.clear();
            Changed();
        }
        o = &e;
    }

    if (o && o->goal == MissionObjective::Kill) {
        ImGui::Indent();
        std::string label = Label(o->target);
        ImGui::SetNextItemWidth(-1);
        if (ImGui::BeginCombo("##target", label.empty() ? "choose the character..." : label.c_str())) {
            std::vector<engine::ZGEOM*> all = Characters();
            for (size_t i = 0; i < all.size(); ++i) {
                std::string p = PathOf(all[i]);
                std::string name = Label(p);
                char item[200];
                snprintf(item, sizeof(item), "%s##%d", name.c_str(), int(i));
                if (ImGui::Selectable(item, p == o->target))
                    SetTarget(*o, p);
            }
            ImGui::EndCombo();
        }
        bool picking = g_pick == PickKind::Character && g_pickSlot == slot;
        if (ImGui::SmallButton(picking ? "click a character... (Esc)" : "Pick in the world"))
            picking ? (void)(g_pick = PickKind::None) : StartPick(PickKind::Character, slot);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Click the character in the world");
        if (engine::ZGEOM* t = o->target.empty() ? nullptr : Resolve(o->target)) {
            ImGui::SameLine();
            if (ImGui::SmallButton("Select"))
                Select(t);
            ImGui::SameLine();
            ImGui::TextDisabled(engine::IsAlive(t) ? "(alive)" : "(dead)");
        }
        ImGui::Unindent();
    } else if (o && o->goal == MissionObjective::Reach) {
        ImGui::Indent();
        std::string label = Label(o->target);
        ImGui::TextUnformatted(label.empty() ? "no place yet" : label.c_str());
        PlaceButtons(slot, o->target);
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 7);
        if (ImGui::DragFloat("how near", &o->radius, 5.0f, 50.0f, 5000.0f, "%.0f cm"))
            CurrentLevel().dirty = true;
        ImGui::Unindent();
    }
    if (o && o->goal != MissionObjective::Level && !own.empty()) {
        if (ImGui::Checkbox("the original no longer counts", &o->replacesOriginal))
            Changed();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("E.g. the original target's death no longer checks this line: only the goal above");
    }

    // remove / back to the level's
    ImGui::Spacing();
    if (o) {
        if (ImGui::SmallButton(own.empty() ? "Remove this objective" : "Back to the level's own")) {
            Doc().objectives.erase(Doc().objectives.begin() + (o - Doc().objectives.data()));
            CurrentLevel().dirty = true;
            ShowLines({slot});
        }
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::PopID();
}

int FreeSlot()
{
    for (int slot = 1; slot <= kSlots; ++slot)
        if (OwnText(slot).empty() && !ObjectiveAt(slot))
            return slot;
    return 0;
}

void DrawObjectives()
{
    // every line: the level's and the editor's
    std::vector<int> slots;
    for (int slot = 1; slot <= kSlots; ++slot)
        if (!OwnText(slot).empty() || ObjectiveAt(slot))
            slots.push_back(slot);
    if (slots.empty())
        ImGui::TextDisabled("This map has no objectives (yet).");
    float line = ImGui::GetTextLineHeight();
    for (int slot : slots) {
        ImGui::PushID(slot);
        std::string text = LineText(slot);
        bool done = LineDone(slot);
        ImVec2 at = ImGui::GetCursorScreenPos();
        if (ImGui::Selectable("##row", g_selSlot == slot, ImGuiSelectableFlags_AllowOverlap,
                              ImVec2(0, line * 2 + ImGui::GetStyle().ItemSpacing.y)))
            g_selSlot = slot;
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s\nclick: edit it below", text.c_str());
        // two lines: the text, what completes it (and done)
        ImDrawList* dl = ImGui::GetWindowDrawList();
        float w = ImGui::GetContentRegionAvail().x;
        char first[300];
        snprintf(first, sizeof(first), "%d. %s", slot, text.empty() ? "(no text)" : text.c_str());
        ImVec4 clip(at.x, at.y, at.x + w - line * 1.5f, at.y + line * 2);
        dl->AddText(nullptr, 0.0f, at, ImGui::GetColorU32(ImGuiCol_Text), first, nullptr, 0.0f, &clip);
        std::string second = "   " + Summary(ObjectiveAt(slot));
        dl->AddText(nullptr, 0.0f, ImVec2(at.x, at.y + line), ImGui::GetColorU32(ImGuiCol_TextDisabled), second.c_str(),
                    nullptr, 0.0f, &clip);
        if (done)
            dl->AddText(ImVec2(at.x + w - line * 1.4f, at.y + line * 0.5f), IM_COL32(120, 255, 120, 255), "ok");
        ImGui::PopID();
    }
    int free = FreeSlot();
    ImGui::BeginDisabled(!free);
    if (ImGui::Button("+ New objective")) {
        MissionObjective o;
        o.slot = free;
        o.goal = MissionObjective::Kill;
        Doc().objectives.push_back(o);
        g_selSlot = free;
        Changed();
        StartPick(PickKind::Character, free);  // straight to choosing the target
        g_status = "new objective: click its target in the world (or choose below)";
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip(free ? "A new line in the laptop: a character to kill (or a place to reach)"
                               : "The laptop has 6 lines, all in use");
    if (std::find(slots.begin(), slots.end(), g_selSlot) == slots.end())
        g_selSlot = slots.empty() ? 0 : slots.front();
    if (g_selSlot)
        DrawLineEditor(g_selSlot);
}

void DrawEnd()
{
    MissionDoc& m = Doc();
    if (ImGui::Checkbox("Completed when every objective is done", &m.endWhenDone))
        Changed();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Without this the level's own scripts end the mission (as in the game)");
    if (!m.endWhenDone)
        return;
    ImGui::Indent();
    std::string label = Label(m.exitPath);
    ImGui::Text("and Hitman reaches: %s", label.empty() ? "nothing more (ends at once)" : label.c_str());
    PlaceButtons(0, m.exitPath);
    if (!m.exitPath.empty()) {
        ImGui::SameLine();
        if (ImGui::SmallButton("No exit")) {
            m.exitPath.clear();
            Changed();
        }
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 7);
        if (ImGui::DragFloat("how near##exit", &m.exitRadius, 5.0f, 50.0f, 5000.0f, "%.0f cm"))
            CurrentLevel().dirty = true;
    }
    ImGui::Unindent();
}

void DrawPanel()
{
    if (CurrentScene().empty()) {
        ImGui::TextDisabled("no scene loaded");
        return;
    }
    if (!CurrentLevel().baseScene.empty() && CurrentLevel().baseScene != CurrentScene()) {
        ImGui::TextWrapped("The level being edited is for %s.", CurrentLevel().baseScene.c_str());
        return;
    }
    // saving: these settings are part of the level
    DrawSaveRow();
    if (!g_status.empty())
        ImGui::TextWrapped("%s", g_status.c_str());

    ImGui::SeparatorText("Objectives (the laptop's lines)");
    if (!g_briefing)
        ImGui::TextDisabled("(this map has no laptop briefing yet)");
    DrawObjectives();

    ImGui::SeparatorText("End of the mission");
    DrawEnd();

    ImGui::SeparatorText("Music");
    DrawMusicSection();

    if (ImGui::CollapsingHeader("Test")) {
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
    }
}

} // namespace

void SetLocText(const std::string& key, const std::string& text)
{
    g_fixedTexts[Lower(key)] = Node(text);
    g_texts[Lower(key)] = Node(text);
}

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
    bool ok = CallSetObjective(slot, key.c_str());
    if (ok && OwnText(slot).empty()) {  // a line the level's laptop does not show
        ShowLineElements(slot);
        CallSetCompleted(slot, g_done[slot]);
    }
    return ok;
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
    for (auto& [k, v] : g_fixedTexts)
        g_texts[k] = v;
    g_texts[Lower(kEmptyKey)] = Node("");
    memset(g_done, 0, sizeof(g_done));
    g_ended = false;
    g_restoring = true;
    for (int slot : slots)
        RestoreLine(slot);
    g_restoring = false;
    g_applied = false;
}

bool MissionPicking() { return g_pick != PickKind::None && engine::SceneDataValid(); }

void UpdateMissionPick()
{
    ImGuiIO& io = ImGui::GetIO();
    if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        g_pick = PickKind::None;
        return;
    }
    bool overWorld = !io.WantCaptureMouse && !ImGui::IsMouseDown(ImGuiMouseButton_Right);
    if (g_pick == PickKind::Character) {
        engine::ZGEOM* hover = overWorld ? CharacterAt(io.MousePos.x, io.MousePos.y) : nullptr;
        if (hover && (hover == engine::Player() || ActorOf(hover) == engine::Player()))
            hover = nullptr;  // not Hitman himself
        std::string name = hover ? Label(PathOf(hover)) : "";
        std::string hint = hover ? "click: " + name + " is the target   Esc: cancel"
                                 : std::string("click a character: the target   Esc: cancel");
        DrawHint(hint.c_str());
        if (hover) {  // a ring over its head
            Transform t = GetTransform(hover);
            float head[3] = {t.pos[0], t.pos[1] + 200.0f, t.pos[2]};
            float sx, sy;
            if (WorldToScreen(head, sx, sy))
                ImGui::GetForegroundDrawList()->AddCircle(ImVec2(sx, sy), 12.0f, IM_COL32(255, 220, 120, 255), 0, 3.0f);
        }
        if (overWorld && hover && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            g_pick = PickKind::None;
            SetTarget(Editable(g_pickSlot), PathOf(hover));
            g_status = "target: " + name;
        }
    } else if (g_pick == PickKind::Place) {
        DrawHint(g_pickSlot ? "click the ground: the place Hitman has to reach   Esc: cancel"
                            : "click the ground: the mission's exit   Esc: cancel");
        if (g_pickPending && ImGui::GetFrameCount() - g_pickFrame > 10)
            g_pickPending = false;
        if (overWorld && !g_pickPending && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            g_pickPending = true;
            g_pickFrame = ImGui::GetFrameCount();
            RequestPick(io.MousePos.x, io.MousePos.y, OnPlacePick);
        }
    }
}

void InstallMission()
{
    OnScenePushed([](const std::string&) { g_pick = PickKind::None; });
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
