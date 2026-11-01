#include "mission.h"

#include <windows.h>
#include <commdlg.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <map>
#include <ranges>
#include <set>
#include <string>
#include <vector>

#include "imgui.h"
#include "json.hpp"

#include "../engine/game_events.h"
#include "../engine/ingame_map.h"
#include "../engine/inject.h"
#include "../engine/messages.h"
#include "../engine/npc.h"
#include "../engine/scene_data.h"
#include "../hook.h"
#include "../input/dinput.h"
#include "../log.h"
#include "../mainloop.h"
#include "../overlay/overlay.h"
#include "../game.h"
#include "../scene.h"
#include "../script/events.h"
#include "../script/state.h"
#include "freecam.h"
#include "../ui.h"
#include "history.h"
#include "level.h"
#include "model.h"
#include "music.h"
#include "npcs.h"
#include "scene_panel.h"
#include "stages.h"
#include "viewport.h"
#include "highlight.h"

namespace h2::editor {

namespace {

constexpr uintptr_t kLocFind = 0x428FF0;          // stdcall (const char* path, const uint8_t* root) -> node
constexpr uintptr_t kSetLocaleFolder = 0x52D340;  // BriefingMenu::(const char* folder)
constexpr uintptr_t kSetObjective = 0x52E2E0;     // BriefingMenu::(int slot, const char* key)
constexpr uintptr_t kSetCompleted = 0x52E350;     // BriefingMenu::(int* {slot, done})
constexpr uintptr_t kBriefingAction = 0x52CED0;   // BriefingMenu::(int action) -> handled; 0xC9.. its files
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
std::string g_scriptBriefing;              // h2.briefing (this load), over the document's
std::string g_lineKeys[kSlots + 1];         // the level's LOC key of each line (as it set them)
bool g_ours;                               // the editor itself checks a line
std::vector<engine::ZGEOM*> g_exitIcons;   // the base level's exit icons taken off the map (BaseExitIcons)
void BaseExitIcons(bool shown);
bool g_restoring;                          // lines go back to the level's texts
bool g_panelVisible = true;
std::string g_status;

// runtime state of the editor's goals
bool g_done[kSlots + 1];
bool g_ended;
double g_lastCheck;
bool g_applied;                            // the mission was applied to this scene
// "Hitman takes it" goals: an item is taken when he holds it (its object is
// kept: its path changes when he picks it up); anything else when he presses
// the game's pickup key (E) near it: it is taken out of the world.
engine::ZGEOM* g_takeObject[kSlots + 1];
std::map<std::string, ObjectState> g_taken;  // objects taken out (target -> as they were)
std::string g_pendingGoals;                  // goals done in a loaded saved game (applied when the mission runs)
std::set<engine::ZGEOM*> g_knockedOut;       // characters reported unconscious (KnockOut goals)
bool g_spotted;                              // a guard spotted Hitman (Unseen goals)
bool g_spottedFailed;                        // ... and the mission was failed for it
bool g_nearPlace[kSlots + 1];                // Hitman at a Deliver goal's place (the hint was shown)
constexpr float kAway = -100000.0f;          // a taken object's place: 1 km under the map

MissionDoc& Doc() { return CurrentLevel().mission; }

// the objectives of the stage played or edited (an escalation's: stages.h; all of a plain level's)
auto StageObjectives()
{
    return Doc().objectives | std::views::filter([](const MissionObjective& o) { return InStage(o.span); });
}

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
        // Control characters are the game's markup: the briefing menu writes
        // a byte per 0x02 (page break) into a 16-byte array of its own
        // (SetLocaleFolder 0x52D340, BriefingMenu+0x5DC) unchecked. A text
        // keeps new lines only.
        if (cp == 0x09)  // tab
            cp = ' ';
        if ((cp < 0x20 && cp != 0x0A) || cp == 0x7F)  // new line kept
            continue;
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

// A level's own briefing keys: "<folder>/formatstring" names them when the level has one (C2-2:
// "Missionbriefing/Missionbriefing%d"; the default "MissionBriefing/%da", SetLocaleFolder 0x52D340
// reads it as a node's data string, 0x428F30). The level's briefing (BuildTexts' "1a") goes under
// its first key and the further ones are answered "not there", as with the default keys.
void BriefingFormat(const uint8_t* node)
{
    auto first = g_texts.find(Lower(g_folder + "/MissionBriefing/1a"));
    if (first == g_texts.end() || !engine::IsReadable(node, 1) || !(node[0] & 2))
        return;
    const char* data = reinterpret_cast<const char*>(node + 1);
    if (node[0] & 1)
        data += strnlen(data, 4096) + 1;  // (the text first, then the data)
    std::string format(data, strnlen(data, 64));
    size_t d = format.find("%d");
    if (d == std::string::npos || format.find('%', d + 1) != std::string::npos)
        return;  // one number, nothing else to format
    std::string text = first->second;
    for (int i = 1; i <= 40; ++i) {
        std::string key = format.substr(0, d) + std::to_string(i) + format.substr(d + 2);
        g_texts[Lower(g_folder + "/" + key)] = i == 1 ? text : "";
    }
}

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
        if (!g_folder.empty() && Lower(g_folder + "/formatstring") == key)
            if (uint8_t* node = oLocFind(path, root)) {
                BriefingFormat(node);
                return node;
            }
        auto it = g_texts.find(key);
        if (it != g_texts.end())
            return it->second.empty() ? nullptr : reinterpret_cast<uint8_t*>(it->second.data());
    }
    uint8_t* node = oLocFind(path, root);
    if (node && !g_locRoot && path && !g_folder.empty() && _strnicmp(path, g_folder.c_str(), g_folder.size()) == 0)
        g_locRoot = root;
    return node;
}

void BuildTexts();
bool BaseFilesHidden() { return Doc().baseFiles == 0 || (Doc().baseFiles < 0 && CurrentLevel().emptyMap); }

void __fastcall hkSetFolder(void* self, void* edx, const char* folder)
{
    g_briefing = self;
    g_folder = folder ? folder : "";
    g_locRoot = nullptr;
    BuildTexts();  // the level's briefing text is read now
    oSetFolder(self, edx, folder);
}

// The base level's files hidden: a click on their (empty) entries opens nothing (the window's
// buttons send the photo itself, whatever their text)
using BriefingAction_t = bool(__fastcall*)(void* self, void* edx, uint32_t action);
BriefingAction_t oBriefingAction;

bool __fastcall hkBriefingAction(void* self, void* edx, uint32_t action)
{
    bool own = action >= 0xC9 && size_t(action - 0xC9) < Doc().files.size() && !Doc().files[action - 0xC9].name.empty();
    if (action >= 0xC9 && action <= 0xCD && self == g_briefing && ForThisScene() && BaseFilesHidden() && !own)
        return true;
    return oBriefingAction(self, edx, action);
}

void __fastcall hkLevelInit(void* self, void* edx)
{
    g_level = self;
    oLevelInit(self, edx);
    engine::WatchLevel(self);
}

bool CallLevel(int index)
{
    if (!g_level)
        return false;
    __try {
        VFunc<void(__thiscall*)(void*)>(g_level, index)(g_level);
        return true;
    } __except (Log("mission: level routine %d raised %08X", index, GetExceptionCode()), EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// a line gets a text: the level's keys are noted; lines the editor gives a
// text keep the editor's key
void HideLineElements(int slot);

void __fastcall hkSetObjective(void* self, void* edx, int slot, const char* key)
{
    g_briefing = self;
    if (slot >= 1 && slot <= kSlots) {
        bool editors = key && _strnicmp(key, "H2Editor/", 9) == 0;  // the editor's own (a script's line too)
        if (key && !editors)
            g_lineKeys[slot] = key;
        if (!g_restoring && !EditorText(slot).empty())
            key = EditorKey(slot);
        else if (!g_restoring && EmptyMap() && !InDocument(slot) && !editors) {
            key = kEmptyKey;  // an empty map has no objectives of the level (a script's lines it has)
            oSetObjective(self, edx, slot, key);
            HideLineElements(slot);  // no empty bullet either
            return;
        }
    }
    oSetObjective(self, edx, slot, key);
}

bool Owned(int slot)
{
    if (!ForThisScene())
        return false;
    for (const MissionObjective& o : StageObjectives())
        if (o.slot == slot && ((o.goal != MissionObjective::Level && o.replacesOriginal) || o.hidden))
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
bool g_hiddenLines[8];     // hidden by scripts (h2.objective(slot, false))
float g_savesY = -1e30f;   // the "saves left" line's own place (local y); -1e30: not known
float g_lineY[kSlots + 1];  // each line's own place (local y)
bool g_lineYKnown;

// Lines are siblings in "Objectives", one line height apart.
engine::ZGEOM* LocalY(const char* name, float& y)
{
    auto* e = static_cast<engine::ZGEOM*>(Element(name));
    if (!e || !engine::IsReadable(e->node, sizeof(engine::ZBaseGeom)))
        return nullptr;
    y = e->node->pos[1];
    return e;
}

void SetLocalY(engine::ZGEOM* e, float y)
{
    engine::ZBaseGeom* n = e->node;
    if (n->pos[1] == y)
        return;
    float pos[3] = {n->pos[0], y, n->pos[2]};
    float mat[9];
    memcpy(mat, n->mat, sizeof(mat));
    __try {
        game::SetLocalTransform(e, mat, pos);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

// The lines shown close up (a line hidden above leaves no gap: the level's
// lines a script hid above the editor's, P8) and the "saves left" line goes
// below the last one (in a level with 4 lines it is where line 5 is).
void Relayout()
{
    char name[64];
    if (!g_lineYKnown) {
        for (int slot = 1; slot <= kSlots; ++slot) {
            snprintf(name, sizeof(name), "Objectives\\%d", slot);
            if (!LocalY(name, g_lineY[slot]))
                return;
        }
        g_lineYKnown = true;
    }
    int shown = 0;
    for (int slot = 1; slot <= kSlots; ++slot) {
        snprintf(name, sizeof(name), "Objectives\\%d", slot);
        float y;
        engine::ZGEOM* e = LocalY(name, y);
        if (!e)
            continue;
        bool hidden = (e->node->flags & 0xC00) != 0;  // the window file's 0x400, SetHidden's 0x800
        SetLocalY(e, hidden ? g_lineY[slot] : g_lineY[++shown]);
    }
    float ySaves;
    engine::ZGEOM* saves = LocalY("Objectives\\SavesLeft", ySaves);
    if (!saves)
        return;
    if (g_savesY < -1e29f)
        g_savesY = ySaves;
    float step = g_lineY[2] - g_lineY[1];
    SetLocalY(saves, shown > 0 ? g_lineY[shown] + step : g_savesY);
}

// lines the level does not use are hidden in its laptop
void HideLineElements(int slot)
{
    char name[64];
    for (const char* part : {"", "\\Text", "\\Check"}) {
        snprintf(name, sizeof(name), "Objectives\\%d%s", slot, part);
        SetVisible(Element(name), false);
    }
    Relayout();
}

void ShowLineElements(int slot)
{
    char name[64];
    for (const char* part : {"", "\\Text", "\\Check"}) {
        snprintf(name, sizeof(name), "Objectives\\%d%s", slot, part);
        SetVisible(Element(name), true);
    }
    g_shownLines = std::max(g_shownLines, slot);
    Relayout();
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

// a LOC node (0x01, the text, NUL); at most 4096 characters (the briefing;
// the game's own are under 2000): texts from scripts and level files are
// not trusted to fit whatever the GUI copies them into
std::string Node(const std::string& text, size_t max = 4096)
{
    std::string s = ToGameText(text);
    if (s.size() > max)
        s.resize(max);
    return std::string("\x01", 1) + s + std::string(1, '\0');
}

// texts the editor answers in the LOC tree
void BuildTexts()
{
    g_texts = g_scriptTexts;
    for (auto& [k, v] : g_fixedTexts)
        g_texts[k] = v;
    g_texts[Lower(kEmptyKey)] = Node("");
    if (!ForThisScene())
        return;
    // The briefing is the texts of "<folder>/MissionBriefing/1a", "2a", ...
    // as long as they exist (SetLocaleFolder 0x52D340): the level's own
    // further parts are answered "not there".
    const std::string briefing = StageBriefing(g_scriptBriefing.empty() ? Doc().briefing : g_scriptBriefing);
    if (!briefing.empty() && !g_folder.empty()) {
        g_texts[Lower(g_folder + "/MissionBriefing/1a")] = Node(briefing);
        for (int i = 2; i <= 40; ++i)
            g_texts[Lower(g_folder + "/MissionBriefing/" + std::to_string(i) + "a")] = "";
    }
    for (const MissionObjective& o : StageObjectives())
        if (!o.text.empty())
            g_texts[Lower(EditorKey(o.slot))] = Node(o.text, 256);
    // the base level's additional files (its target's photo: "<folder>/Laptop/Files/Photo1".. 5, read by
    // SetLocaleFolder 0x52D340): an empty text (not there, the key itself would show), as the unused entries
    if (BaseFilesHidden() && !g_folder.empty())
        for (int i = 1; i <= 5; ++i)
            g_texts[Lower(g_folder + "/Laptop/Files/Photo" + std::to_string(i))] = Node("");
    // the level's own files in their places (the map's photos': shown with its picture, LaptopPhotoTick)
    if (!g_folder.empty())
        for (size_t i = 0; i < Doc().files.size() && i < 5; ++i)
            if (!Doc().files[i].name.empty())
                g_texts[Lower(g_folder + "/Laptop/Files/Photo" + std::to_string(i + 1))] = Node(Doc().files[i].name, 64);
}

bool InDocument(int slot)
{
    if (!ForThisScene())
        return false;
    for (const MissionObjective& o : StageObjectives())
        if (o.slot == slot)
            return true;
    return false;
}

bool EmptyMap() { return ForThisScene() && CurrentLevel().emptyMap; }

std::string EditorText(int slot)
{
    if (!ForThisScene())
        return "";
    for (const MissionObjective& o : StageObjectives())
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
        if (!InDocument(slot) && !g_lineKeys[slot].empty() && !g_hiddenLines[slot])
            RestoreLine(slot);
    for (const MissionObjective& o : StageObjectives()) {
        if (o.hidden) {  // an original line removed
            CallSetObjective(o.slot, kEmptyKey);
            HideLineElements(o.slot);
            continue;
        }
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
    for (int slot = 1; slot <= kSlots; ++slot)
        if (g_hiddenLines[slot])
            HideLineElements(slot);
    // the base level's files hidden: their six buttons and title too (#21: empty boxes); shown again with
    // the map's next load (the window shows the ones the level has)
    if (ForThisScene() && BaseFilesHidden()) {
        for (int i = 1; i <= 6; ++i)
            SetVisible(Element(("Files\\" + std::to_string(i)).c_str()), false);
        SetVisible(Element("FilesTitle"), false);
    }
    Relayout();
}

bool CallSetFolder(void* briefing, const char* folder)
{
    __try {
        oSetFolder(briefing, nullptr, folder);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The briefing is read when the folder is set: it is set again (the lines
// it resets get their texts and check marks back).
void RefreshBriefing()
{
    BuildTexts();
    if (!g_briefing || g_folder.empty())
        return;
    uint32_t done = 0;
    if (engine::IsReadable(static_cast<char*>(g_briefing) + kBriefingDoneBits, 4))
        done = *reinterpret_cast<uint32_t*>(static_cast<char*>(g_briefing) + kBriefingDoneBits);
    if (!CallSetFolder(g_briefing, g_folder.c_str()))
        Log("mission: the briefing was not set again");
    ShowLines();
    for (int slot = 1; slot <= kSlots; ++slot)
        if ((done >> slot) & 1)
            CallSetCompleted(slot, true);
}

void DrawBriefingEditor()
{
    static char buf[4096];
    static bool editing;
    MissionDoc& m = Doc();
    if (!editing)
        snprintf(buf, sizeof(buf), "%s", m.briefing.c_str());
    ImGui::InputTextMultiline("##briefing", buf, sizeof(buf), ImVec2(-1, ImGui::GetTextLineHeight() * 6));
    editing = ImGui::IsItemActive();
    if (ImGui::IsItemDeactivatedAfterEdit() && m.briefing != buf) {
        m.briefing = buf;
        CurrentLevel().dirty = true;
        RefreshBriefing();
    }
    if (ImGui::IsItemHovered() && m.briefing.empty())
        ImGui::SetTooltip("Empty: the level's own briefing. The laptop shows it scrolled, the way it is written here.");
    // the box does not wrap its lines (ImGui's multiline text): a long line read wrapped below it (#21)
    float boxWidth = ImGui::GetItemRectSize().x - ImGui::GetStyle().FramePadding.x * 2;
    bool longLine = false;
    for (size_t start = 0; start < m.briefing.size() && !longLine;) {
        size_t end = m.briefing.find('\n', start);
        if (end == std::string::npos)
            end = m.briefing.size();
        longLine = ImGui::CalcTextSize(m.briefing.c_str() + start, m.briefing.c_str() + end).x > boxWidth;
        start = end + 1;
    }
    if (!editing && longLine) {
        float h = ImGui::GetTextLineHeightWithSpacing() * 5;
        if (ImGui::BeginChild("##briefingwrapped", ImVec2(-1, h), ImGuiChildFlags_Borders)) {
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextDisabled("%s", m.briefing.c_str());
            ImGui::PopTextWrapPos();
        }
        ImGui::EndChild();
    }
    ImGui::BeginDisabled(m.briefing.empty());
    if (ImGui::SmallButton("The level's own briefing")) {
        m.briefing.clear();
        CurrentLevel().dirty = true;
        RefreshBriefing();
    }
    ImGui::EndDisabled();
}

// ---- goals ----

bool IsItem(engine::ZGEOM* g) { return g && engine::IsA(g, "ZItem") && !engine::IsA(g, "ZItemTemplate"); }

// the item an object is a part of (its model), or the object itself
engine::ZGEOM* ItemOrSelf(engine::ZGEOM* geom)
{
    for (engine::ZBaseGeom* n = geom && geom->node ? geom->node : nullptr; n; n = n->parent)
        if (IsItem(n->geom))
            return n->geom;
    return geom;
}

engine::ZGEOM* Target(const MissionObjective& o)
{
    if ((o.goal == MissionObjective::Take || o.goal == MissionObjective::Deliver) && o.slot >= 1 && o.slot <= kSlots) {
        engine::ZGEOM*& kept = g_takeObject[o.slot];
        if (kept && IsLive(kept))
            return kept;
        kept = nullptr;
    }
    engine::ZGEOM* g = o.target.empty() ? nullptr : ResolveTarget(o.target);
    g = g && IsLive(g) ? g : nullptr;
    if (g && (o.goal == MissionObjective::Take || o.goal == MissionObjective::Deliver) && o.slot >= 1 && o.slot <= kSlots)
        g_takeObject[o.slot] = g;
    return g;
}

bool HeldByHitman(engine::ZGEOM* item)
{
    engine::ZGEOM* hitman = engine::Player();
    uint32_t owner = 0;
    return hitman && item && engine::ItemHolder(item, owner) && owner == engine::RefOf(hitman);
}

// an object (not an item) taken: out of sight and out of the way
void TakeOut(const std::string& target, engine::ZGEOM* g)
{
    if (!g || !IsLive(g) || g_taken.count(target))
        return;
    ObjectState st = GetState(g);
    g_taken[target] = st;
    st.transform.pos[1] = kAway;
    st.hidden = true;
    SetState(g, st);
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

float Distance(engine::ZGEOM* a, engine::ZGEOM* b)
{
    if (!a || !b)
        return 1e30f;
    Transform ta = GetTransform(a), tb = GetTransform(b);
    float d[3] = {ta.pos[0] - tb.pos[0], ta.pos[1] - tb.pos[1], ta.pos[2] - tb.pos[2]};
    return std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
}

// Hitman at a goal's place: as its zone's CheckInside routine found him (a box
// made with the scene, engine/inject.h), or near it while it has no zone (the
// place was made or moved since the map loaded, or is not one of the level's points)
bool AtPlace(int code, engine::ZGEOM* place, float radius)
{
    if (!place)
        return false;
    std::string zone = GoalZoneName(code);
    if (engine::ZGEOM* box = engine::RecordGeom(engine::SceneZoneRecord(zone)); box && IsLive(box)) {
        Transform a = GetTransform(box), b = GetTransform(place);
        float d[3] = {a.pos[0] - b.pos[0], a.pos[1] - b.pos[1], a.pos[2] - b.pos[2]};
        if (d[0] * d[0] + d[1] * d[1] + d[2] * d[2] < 1.0f)
            return engine::InSceneZone(zone);
    }
    return DistanceToPlayer(place) <= radius;
}

// "Bring it to a place": an item laid down there (held by no one), any other
// object taken (E) and Hitman there with it: it is put down at the place
bool Delivered(const MissionObjective& o)
{
    engine::ZGEOM* t = Target(o);
    engine::ZGEOM* place = o.place.empty() ? nullptr : ResolveTarget(o.place);
    if (!t || !place || !IsLive(place))
        return false;
    if (IsItem(t)) {
        uint32_t owner = 0;
        return !engine::ItemHolder(t, owner) && Distance(t, place) <= o.radius;
    }
    if (!g_taken.count(o.target) || !AtPlace(100 + o.slot, place, o.radius))
        return false;
    ObjectState st = g_taken[o.target];  // as it was, at the place now
    st.transform.pos[0] = GetTransform(place).pos[0];
    st.transform.pos[1] = GetTransform(place).pos[1];
    st.transform.pos[2] = GetTransform(place).pos[2];
    SetState(t, st);
    return true;
}

bool Reached(const MissionObjective& o)
{
    switch (o.goal) {
    case MissionObjective::Kill: {  // the level routine told it was killed (Being2: CharacterKilled), or
        engine::ZGEOM* t = Target(o);  // the game counts it dead (a death it does not report: one from
        engine::ZGEOM* actor = ActorOf(t);  // unconsciousness, the scripts' instant death)
        return t && (engine::WasKilled(actor ? actor : t) || engine::WasKilled(t) ||
                     (!engine::IsAlive(t) && !KnockedOut(t)));  // (the engine counts a knocked out one dead)
    }
    case MissionObjective::Reach:
        return AtPlace(o.slot, Target(o), o.radius);
    case MissionObjective::Take: {
        engine::ZGEOM* t = Target(o);
        return t && (IsItem(t) ? HeldByHitman(t) : g_taken.count(o.target) > 0);
    }
    case MissionObjective::KnockOut: {  // its behaviour in the base class's state (Being2.Unconscious: a
        engine::ZGEOM* t = Target(o);      // knock-out is not reported to the level routine, seen in
        if (!t)                            // the game 2026-10-18), or the level routine told it was
            return false;                  // harmed and not killed
        engine::ZGEOM* actor = ActorOf(t);
        engine::ZGEOM* who = actor ? actor : t;
        if (g_knockedOut.count(who))
            return true;
        if (!KnockedOut(t) && (!engine::WasHarmed(who) || engine::WasKilled(who)))
            return false;
        g_knockedOut.insert(who);  // (it may wake up: the goal stays done)
        return true;
    }
    case MissionObjective::Deliver:
        return Delivered(o);
    case MissionObjective::Unseen:
        return !g_spotted;
    default:
        return LineChecked(o.slot);
    }
}

std::vector<engine::ZGEOM*> Characters();

// the "death" event for level scripts, once a character: the ones the level routine was told were
// killed, and the ones the game counts dead it was not told of (from unconsciousness; the scripts'
// instant death of one: seen in the game 2026-10-18)
std::set<engine::ZGEOM*> g_deathFired;
std::map<engine::ZGEOM*, bool> g_alive;  // characters' life as the game last said it

void WatchDeaths()
{
    auto fire = [](engine::ZGEOM* a) {
        if (!IsLive(a) || !g_deathFired.insert(a).second)
            return;
        std::string path = PathOf(a);
        if (!path.empty())
            script::FireDeath(path);
    };
    for (engine::ZGEOM* a : engine::TakeKilled())
        fire(a);
    for (engine::ZGEOM* a : Characters()) {
        bool alive = engine::IsAlive(a) || KnockedOut(a);  // (knocked out: not a death)
        auto it = g_alive.find(a);
        if (it != g_alive.end() && it->second && !alive)
            fire(a);
        g_alive[a] = alive;
    }
}

void ApplyPendingGoals();

// Take goals of objects that are not items. One the scene made gets a "use" action, as the
// scenes' own (Open, PlaceBomb: kind 6, messages.h AddUseAction); used, it tells the object. The
// game offers actions only of objects the scene made (a copy made while playing is not among
// them, found in the game 2026-10-18): a copy is taken with the pickup key near it.
std::set<engine::ZGEOM*> g_takeActions;  // objects given the action in this scene
bool g_nearTake[kSlots + 1];             // Hitman is near a copy (the hint was shown)
constexpr int kDikE = 0x12;              // the game's pickup / action key (its default binding)

void CheckTakes()
{
    int presses = TakeGameKeyPresses(kDikE);
    if (!ForThisScene() || !engine::SceneDataValid())
        return;
    static uint16_t msg;
    if (!msg && (msg = engine::RegisterMessage("H2Editor_Take")) != 0)
        engine::WatchUse(msg);
    for (const MissionObjective& o : StageObjectives()) {
        if ((o.goal != MissionObjective::Take && o.goal != MissionObjective::Deliver) || g_done[o.slot] ||
            g_taken.count(o.target))
            continue;
        engine::ZGEOM* t = Target(o);
        if (!t || IsItem(t)) {
            g_nearTake[o.slot] = false;
            continue;
        }
        float reach = o.goal == MissionObjective::Take ? o.radius : 150.0f;  // (Deliver: radius is the place's)
        bool taken = false, action = false;
        if (engine::RecordOf(t) >= 0 && msg) {  // the scene's (the level's copies made with the map too): the game's action
            if (!g_takeActions.count(t)) {
                if (engine::AddUseAction(t, "Take", msg, int(reach)))
                    g_takeActions.insert(t);
                else
                    Log("mission: objective %d: no action on %s", o.slot, o.target.c_str());
            }
            action = g_takeActions.count(t) > 0;
            taken = engine::TakeUsed(t);
        }
        // The key near it: a copy made while playing has no action, and the game offers a
        // "use" action (kind 6) only within a band around Hitman's chest (ZHITMAN2 vfunc 0x638:
        // the object's box widened by 90 / 40 / 90, the same room): nothing on the floor
        // (the game's own floor things are items, picked up). Hinted where the action is not.
        engine::ZGEOM* hitman = engine::Player();
        bool inReach = hitman && engine::IsAlive(hitman) && DistanceToPlayer(t) <= reach;
        bool low = hitman && GetTransform(t).pos[1] < GetTransform(hitman).pos[1] + 60.0f;
        if (inReach && !g_nearTake[o.slot] && (!action || low))
            ShowInfo(InfoKind::Hint, "E: take it (" +
                                         (o.text.empty() ? "objective " + std::to_string(o.slot) : o.text) + ")");
        g_nearTake[o.slot] = inReach;
        if (!taken && inReach && presses > 0) {
            taken = true;
            presses = 0;  // one object a press
        }
        if (taken) {
            TakeOut(o.target, t);
            ShowInfo(InfoKind::Info, "Taken.");
            Log("mission: objective %d: Hitman took %s", o.slot, o.target.c_str());
        }
    }
}

void Check()
{
    CheckTakes();
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
    if (!LevelApplyPending())  // (the level's copies made with the map are put at their places then)
        ApplyPendingGoals();
    bool all = true;
    int counted = 0;
    for (const MissionObjective& o : StageObjectives()) {
        if (o.hidden)
            continue;  // (an original line removed)
        ++counted;
        if (o.goal == MissionObjective::Level) {
            all &= LineChecked(o.slot);
            continue;
        }
        if (o.goal == MissionObjective::Unseen) {  // done from the start, lost for good when spotted
            bool ok = !g_spotted;
            if (ok != g_done[o.slot]) {
                g_done[o.slot] = ok;
                CallSetCompleted(o.slot, ok);
                if (!ok) {
                    Log("mission: objective %d lost: Hitman was spotted", o.slot);
                    ShowInfo(InfoKind::Warning, "Spotted: " + (o.text.empty() ? std::string("objective lost") : o.text));
                    if (o.failsMission && !g_spottedFailed && !g_ended) {
                        g_spottedFailed = true;
                        g_ended = true;
                        FailMission();
                    }
                }
            }
            all &= ok;
            continue;
        }
        if (o.goal == MissionObjective::Deliver && !g_done[o.slot]) {  // at the place with the item: a hint
            engine::ZGEOM* t = Target(o);
            engine::ZGEOM* place = o.place.empty() ? nullptr : ResolveTarget(o.place);
            bool there = t && place && IsItem(t) && HeldByHitman(t) && DistanceToPlayer(place) <= o.radius;
            if (there && !g_nearPlace[o.slot])
                ShowInfo(InfoKind::Hint, "Drop it here (" + (o.text.empty() ? std::string("the objective") : o.text) + ")");
            g_nearPlace[o.slot] = there;
        }
        if (!g_done[o.slot] && Reached(o)) {
            g_done[o.slot] = true;
            CallSetCompleted(o.slot, true);
            Log("mission: objective %d done", o.slot);
            script::FireObjective(o.slot);
        }
        all &= g_done[o.slot];
    }
    if (!Doc().endWhenDone || g_ended || !all || !counted)
        return;
    if (!Doc().exitPath.empty()) {
        engine::ZGEOM* exit = ResolveTarget(Doc().exitPath);
        if (!AtPlace(0, exit, Doc().exitRadius))
            return;
    }
    // once: on a blank map the level's MissionCompleted got the end screen up but stopped with an
    // exception (caught: false), and the check called it again every quarter second (Arena, 2026-10-12)
    g_ended = true;
    if (!CompleteMission())
        Log("mission: the level's end did not return (its end screen may still have come)");
}

void OnScene(const std::string&)
{
    g_exitIcons.clear();  // (gone with the scene)
    g_shownLines = 0;
    g_savesY = -1e30f;
    g_lineYKnown = false;
    g_briefing = nullptr;
    g_level = nullptr;
    g_folder.clear();
    g_locRoot = nullptr;
    g_ended = false;
    g_applied = false;
    memset(g_done, 0, sizeof(g_done));
    memset(g_takeObject, 0, sizeof(g_takeObject));
    g_takeActions.clear();
    g_deathFired.clear();
    g_alive.clear();
    memset(g_nearTake, 0, sizeof(g_nearTake));
    TakeGameKeyPresses(kDikE);
    g_taken.clear();
    g_knockedOut.clear();
    g_spotted = g_spottedFailed = false;
    memset(g_nearPlace, 0, sizeof(g_nearPlace));
    for (std::string& k : g_lineKeys)
        k.clear();
    g_scriptTexts.clear();
    g_scriptBriefing.clear();
    memset(g_hiddenLines, 0, sizeof(g_hiddenLines));
    BuildTexts();
}

// ---- panel ----

// an object's name for texts: an item's as the game shows it ("Room key"), a
// copy's without the editor's "+1"
std::string NiceName(engine::ZGEOM* g)
{
    std::string name = engine::SafeString(g->name);
    if (IsItem(g)) {
        std::string tmpl = name;
        if (const SpawnedObject* s = FindSpawned(g))
            tmpl = s->sourcePath.substr(s->sourcePath.rfind('/') + 1);
        const uint8_t* node = g_locRoot && oLocFind
                                  ? SafeLocFind(("AllLevels/Items/ShortNames/" + tmpl).c_str(), g_locRoot)
                                  : nullptr;
        if (node && (node[0] & 1)) {
            std::string shown = FromGameText(reinterpret_cast<const char*>(node + 1));
            if (!shown.empty() && shown.size() < 64)
                return shown;
        }
    }
    size_t plus = name.find('+');
    if (plus != std::string::npos && plus > 0)
        name.resize(plus);
    return name;
}

std::string Label(const std::string& path)
{
    if (path.empty())
        return "";
    engine::ZGEOM* g = ResolveTarget(path);
    if (!g || !IsLive(g))
        return "(missing) " + path.substr(path.rfind('/') + 1);
    if (auto it = Doc().names.find(path); it != Doc().names.end() && !it->second.empty())
        return it->second;
    if (engine::ZGEOM* a = ActorOf(g)) {
        if (const LevelNpc* n = FindLevelNpc(a); n && !n->label.empty())
            return n->label;
        int rec = NpcRecordOf(a);
        if (rec >= 0)
            return NpcModel(rec) + (IsPlacedNpc(a) ? " (placed)" : "");
    }
    return NiceName(g);
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
    for (MissionObjective& o : StageObjectives())
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
    o.span = NewSpan();
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
    if (o->goal == MissionObjective::Take)
        return what.empty() ? "take: choose what" : "Hitman takes " + what;
    if (o->goal == MissionObjective::KnockOut)
        return what.empty() ? "knock out: choose who" : what + " knocked out";
    if (o->goal == MissionObjective::Deliver) {
        std::string where = Label(o->place);
        return what.empty() ? "bring: choose what" : where.empty() ? what + ": choose where" : what + " to " + where;
    }
    if (o->goal == MissionObjective::Unseen)
        return std::string("never spotted") + (o->failsMission ? " (or the mission fails)" : "");
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
    return text.empty() || text.rfind("Kill ", 0) == 0 || text.rfind("Reach ", 0) == 0 ||
           text.rfind("Take ", 0) == 0 || text.rfind("Knock out ", 0) == 0 || text.rfind("Bring ", 0) == 0 ||
           text == "Do not be spotted" || text == "New objective";
}

void SetTarget(MissionObjective& o, const std::string& path)
{
    o.target = path;
    if (o.slot >= 1 && o.slot <= kSlots)
        g_takeObject[o.slot] = nullptr;
    if (AutoText(o.text) && !path.empty()) {  // the laptop's text follows the new target
        std::string text = (o.goal == MissionObjective::Kill       ? "Kill "
                            : o.goal == MissionObjective::Take     ? "Take "
                            : o.goal == MissionObjective::KnockOut ? "Knock out "
                            : o.goal == MissionObjective::Deliver  ? "Bring "
                                                                   : "Reach ") +
                           Label(path);
        if (o.goal == MissionObjective::Deliver && !o.place.empty())
            text += " to " + Label(o.place);
        // three bodyguards of one model are three lines of one text: numbered
        int same = 1;
        for (const MissionObjective& other : Doc().objectives)
            if (&other != &o && other.text.rfind(text, 0) == 0)
                ++same;
        o.text = same > 1 ? text + " (" + std::to_string(same) + ")" : text;
    }
    Changed();
}

// A place picked: 0 the mission's exit, 1..6 a "reach" line's place, 101..106 a
// "bring it" line's place
void SetPlace(int code, const std::string& path)
{
    if (code == 0) {
        Doc().exitPath = path;
        Changed();
    } else if (code > 100) {
        MissionObjective& o = Editable(code - 100);
        o.place = path;
        if (AutoText(o.text) && !o.target.empty())
            o.text = "Bring " + Label(o.target) + (path.empty() ? "" : " to " + Label(path));
        Changed();
    } else {
        SetTarget(Editable(code), path);
    }
}

// ---- picking in the world ----

enum class PickKind { None, Character, Place, Object };
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
    SetPlace(g_pickSlot, path);
    g_status = "place set (an invisible point: move it later from the Scene tab)";
}

// the object to take: an item (also when its model was clicked) or any model; not characters
engine::ZGEOM* TakeableAt(engine::ZGEOM* hit)
{
    if (!hit || !IsLive(hit) || ActorOf(hit))
        return nullptr;
    engine::ZGEOM* g = ItemOrSelf(hit);
    return TargetFor(g).empty() ? nullptr : g;
}

void OnObjectPick(engine::ZGEOM* hit, const float*, const float*)
{
    g_pickPending = false;
    if (g_pick != PickKind::Object)
        return;
    engine::ZGEOM* g = TakeableAt(hit);
    if (!g) {
        g_status = hit ? "a character cannot be taken: click an object" : "nothing there: click an object";
        return;
    }
    g_pick = PickKind::None;
    SetTarget(Editable(g_pickSlot), TargetFor(g));
    g_status = "Hitman has to take " + Label(TargetFor(g));
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
            SetPlace(slot, PathOf(p));
            changed = true;
        } else {
            g_status = "no point made: " + (hitman ? err : std::string("Hitman not found"));
        }
    }
    if (!current.empty()) {
        ImGui::SameLine();
        if (ImGui::SmallButton("Fly to")) {
            if (engine::ZGEOM* g = ResolveTarget(current)) {
                FlyCameraTo(GetTransform(g).pos);
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
    if (o && IsEscalation() && DrawStageSpan(o->span, "##objstages"))  // (stages.h: its stages)
        Changed();
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
    goalChanged |= ImGui::RadioButton("a character is knocked out", &goal, int(MissionObjective::KnockOut));
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Knocked out (chloroform, a blow), not killed: done while it lives");
    goalChanged |= ImGui::RadioButton("Hitman takes an object", &goal, int(MissionObjective::Take));
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("An item (a weapon, a key...): done when Hitman picks it up.\n"
                          "Any other object (a model you placed or one of the map's): done when Hitman\n"
                          "presses E near it; it goes from the world.");
    goalChanged |= ImGui::RadioButton("Hitman brings an object to a place", &goal, int(MissionObjective::Deliver));
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("An item: done when it lies at the place (Hitman drops it there).\n"
                          "Any other object: taken with E, done when Hitman gets to the place (it is put down there).");
    goalChanged |= ImGui::RadioButton("Hitman is never spotted", &goal, int(MissionObjective::Unseen));
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Checked from the start; lost for good when a guard spots Hitman");
    if (goalChanged) {
        MissionObjective& e = Editable(slot);
        if (e.goal != MissionObjective::Goal(goal)) {
            e.goal = MissionObjective::Goal(goal);
            e.target.clear();
            e.radius = e.goal == MissionObjective::Take ? 150.0f : 250.0f;
            e.place.clear();
            if (e.goal == MissionObjective::Unseen && AutoText(e.text))
                e.text = "Do not be spotted";
            g_takeObject[slot] = nullptr;
            Changed();
        }
        o = &e;
    }

    if (o && (o->goal == MissionObjective::Kill || o->goal == MissionObjective::KnockOut)) {
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
        if (engine::ZGEOM* t = o->target.empty() ? nullptr : ResolveTarget(o->target)) {
            ImGui::SameLine();
            if (ImGui::SmallButton("Select"))
                Select(t);
            ImGui::SameLine();
            ImGui::TextDisabled(engine::IsAlive(t) ? "(alive)" : KnockedOut(t) ? "(knocked out)" : "(dead)");
        }
        ImGui::Unindent();
    } else if (o && o->goal == MissionObjective::Unseen) {
        ImGui::Indent();
        if (ImGui::Checkbox("Being spotted fails the mission", &o->failsMission))
            Changed();
        ImGui::TextDisabled(g_spotted ? "(spotted on this map)" : "(not spotted yet)");
        ImGui::Unindent();
    } else if (o && (o->goal == MissionObjective::Take || o->goal == MissionObjective::Deliver)) {
        ImGui::Indent();
        std::string label = Label(o->target);
        engine::ZGEOM* t = o->target.empty() ? nullptr : ResolveTarget(o->target);
        ImGui::TextUnformatted(label.empty() ? "no object yet" : label.c_str());
        if (t) {
            ImGui::SameLine();
            ImGui::TextDisabled(IsItem(t) ? "(an item: picked up)" : "(taken with E)");
        }
        bool picking = g_pick == PickKind::Object && g_pickSlot == slot;
        if (ImGui::SmallButton(picking ? "click an object... (Esc)" : "Pick in the world"))
            picking ? (void)(g_pick = PickKind::None) : StartPick(PickKind::Object, slot);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Click the object Hitman has to take (an item or any model)");
        ImGui::SameLine();
        engine::ZGEOM* sel = TakeableAt(Selected());
        ImGui::BeginDisabled(!sel);
        if (ImGui::SmallButton("The selected object"))
            SetTarget(*o, TargetFor(sel));
        ImGui::EndDisabled();
        if (t) {
            ImGui::SameLine();
            if (ImGui::SmallButton("Select"))
                Select(t);
            ImGui::SameLine();
            if (ImGui::SmallButton("Fly to"))
                FlyCameraTo(GetTransform(t).pos);
        }
        if (o->goal == MissionObjective::Deliver) {
            std::string where = Label(o->place);
            ImGui::Text("to: %s", where.empty() ? "choose the place" : where.c_str());
            PlaceButtons(100 + slot, o->place);
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 7);
            if (ImGui::DragFloat("how near##place", &o->radius, 5.0f, 50.0f, 5000.0f, "%.0f cm"))
                CurrentLevel().dirty = true;
        } else if (t && !IsItem(t)) {
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 7);
            if (ImGui::DragFloat("how near", &o->radius, 5.0f, 50.0f, 1000.0f, "%.0f cm"))
                CurrentLevel().dirty = true;
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
    if (!own.empty()) {  // an original line taken out of the laptop (Simple mode's Remove)
        bool hidden = o && o->hidden;
        if (ImGui::Checkbox("taken out (not on the laptop, not counted)", &hidden)) {
            Editable(slot).hidden = hidden;
            Changed();
            if (!hidden)
                ShowLines({slot});
        }
        o = ObjectiveAt(slot);
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
        // not in the undo history: asked first
        if (ImGui::SmallButton(own.empty() ? "Remove this objective..." : "Back to the level's own..."))
            ImGui::OpenPopup("remove objective");
        if (ImGui::BeginPopup("remove objective")) {
            ImGui::Text(own.empty() ? "Remove objective %d? (Undo does not bring it back)"
                                    : "Objective %d back to the level's own line and goal?",
                        slot);
            if (ImGui::Button(own.empty() ? "Remove" : "Back to the level's own")) {
                Doc().objectives.erase(Doc().objectives.begin() + (o - Doc().objectives.data()));
                CurrentLevel().dirty = true;
                ShowLines({slot});
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel"))
                ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
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
        o.span = NewSpan();
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
    if (ImGui::Checkbox("The base level can end the mission", &m.baseEnds)) {
        Changed();
        if (ForThisScene())
            engine::SetLevelEndsAllowed(m.baseEnds);
            BaseExitIcons(m.baseEnds);
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Off: the base level's own scripts (its target killed, ...) no longer complete or fail\n"
                          "the mission; your goals and scripts (h2.mission) do. Hitman's death still fails it.\n"
                          "The game's map (M) then marks its targets as enemies, from the map's next load.");
    if (ImGui::Checkbox("Completed when every objective is done", &m.endWhenDone))
        Changed();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Without this the level's own scripts end the mission (as in the game)");
    if (!m.endWhenDone) {
        ImGui::TextDisabled("(ticked: an exit Hitman has to reach can be chosen too)");
        return;
    }
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

// ---- Simple mode's Mission step (docs/simple-mode.md, #33) ----

int g_simpleTextSlot;  // the line whose laptop text is being written (Simple mode; 0 none)

// the next small button on the line of the one before when it fits there, else under it
void Flow(const char* label)
{
    float w = ImGui::CalcTextSize(label).x + ImGui::GetStyle().FramePadding.x * 2;
    float right = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;
    if (ImGui::GetItemRectMax().x + ImGui::GetStyle().ItemSpacing.x + w <= right)
        ImGui::SameLine();
}

// the blank of a sentence still to fill: what the next click picks (flow: after the button before)
void PickButton(const char* label, PickKind kind, int slot, bool flow = false)
{
    bool picking = g_pick == kind && g_pickSlot == slot;
    const char* text = picking ? "click in the world... (Esc)" : label;
    if (flow)
        Flow(text);
    if (ImGui::SmallButton(text))
        picking ? (void)(g_pick = PickKind::None) : StartPick(kind, slot);
}

// a new line of the laptop, its blank picked next (none free: false)
bool NewSentence(MissionObjective::Goal goal)
{
    int slot = FreeSlot();
    if (!slot)
        return false;
    MissionObjective o;
    o.slot = slot;
    o.goal = goal;
    o.radius = goal == MissionObjective::Take ? 150.0f : 250.0f;
    o.span = NewSpan();
    Doc().objectives.push_back(o);
    g_selSlot = slot;
    Changed();
    StartPick(goal == MissionObjective::Kill || goal == MissionObjective::KnockOut ? PickKind::Character
              : goal == MissionObjective::Reach                                   ? PickKind::Place
                                                                                   : PickKind::Object,
              slot);
    return true;
}

// ---- the level's own laptop files: its pictures shown in place of the map's photos ----

// The GUI's BriefingPhotos/Photo<n> (a ZWINPIC: SetLocaleFolder 0x52D340 gives it its entry; shown, its node
// flag 0x800 is cleared, seen in the game 2026-11-01 on C1-1); null: the map's laptop has no photo there
engine::ZGEOM* PhotoGeom(int n)
{
    static const void* builtFor;
    static size_t builtSize;
    static engine::ZGEOM* found[6];
    const engine::SceneSnapshot& snap = SceneModel();
    if (builtFor != snap.geoms.data() || builtSize != snap.geoms.size()) {
        builtFor = snap.geoms.data();
        builtSize = snap.geoms.size();
        memset(found, 0, sizeof(found));
        for (const engine::GeomInfo& g : snap.geoms)
            for (int k = 1; k <= 5; ++k) {
                std::string tail = "/BriefingPhotos/Photo" + std::to_string(k);
                if (g.path.size() > tail.size() && g.path.compare(g.path.size() - tail.size(), tail.size(), tail) == 0)
                    found[k] = g.geom;
            }
    }
    return n >= 1 && n <= 5 && found[n] && IsLive(found[n]) ? found[n] : nullptr;
}

int MapPhotoCount()
{
    int n = 0;
    while (n < 5 && PhotoGeom(n + 1))
        ++n;
    return n;
}

std::string LevelFolderFile(const std::string& name)
{
    return GameDir() + "Mods\\" + CurrentLevel().folder + "\\" + name;
}

bool PhotoShown(engine::ZGEOM* g)
{
    __try {
        return g->node && !(g->node->flags & 0x800);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// while the laptop shows a photo of the level's own files: its picture over it (fitted, the rest dark)
void DrawLaptopPhoto()
{
    if (overlay::IsOpen() || CurrentScene().empty() || !engine::SceneDataValid() || !ForThisScene() ||
        Doc().files.empty())
        return;
    for (size_t i = 0; i < Doc().files.size() && i < 5; ++i) {
        const LaptopFile& f = Doc().files[i];
        engine::ZGEOM* g = f.image.empty() ? nullptr : PhotoGeom(int(i) + 1);
        if (!g || !PhotoShown(g))
            continue;
        int w = 0, h = 0;
        float u = 1, v = 1;
        void* tex = ImageTexture(LevelFolderFile(f.image), &w, &h, &u, &v);
        if (!tex || w <= 0 || h <= 0)
            continue;
        // the photo's place: the middle of the screen, its half extents in the virtual screen (ui.h)
        ImVec2 size = ImGui::GetIO().DisplaySize;
        float s = UiScale();
        float hw = g->node->boundHalf[0] * s, hh = g->node->boundHalf[1] * s;
        ImVec2 c(size.x * 0.5f, size.y * 0.5f);
        ImDrawList* dl = ImGui::GetForegroundDrawList();
        dl->AddRectFilled(ImVec2(c.x - hw, c.y - hh), ImVec2(c.x + hw, c.y + hh), IM_COL32(12, 12, 12, 255));
        float fit = std::min(2.0f * hw / float(w), 2.0f * hh / float(h));
        float iw = float(w) * fit * 0.5f, ih = float(h) * fit * 0.5f;
        dl->AddImage(reinterpret_cast<ImTextureID>(tex), ImVec2(c.x - iw, c.y - ih), ImVec2(c.x + iw, c.y + ih),
                     ImVec2(0, 0), ImVec2(u, v));
    }
}

// a picture chosen on the disk: copied into the level's folder as photo<n>.<ext>; its file name ("" none)
std::string PickPicture(int n)
{
    char file[MAX_PATH] = "";
    OPENFILENAMEA ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = static_cast<HWND>(overlay::GameWindow());
    ofn.lpstrFilter = "Pictures (*.png, *.jpg, *.bmp)\0*.png;*.jpg;*.jpeg;*.bmp\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = sizeof(file);
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameA(&ofn))
        return "";
    std::string src = file;
    size_t dot = src.find_last_of('.');
    std::string ext = dot == std::string::npos ? "" : src.substr(dot);
    for (char& ch : ext)
        ch = char(std::tolower(static_cast<unsigned char>(ch)));
    if (ext == ".jpeg")
        ext = ".jpg";
    if (ext != ".png" && ext != ".jpg" && ext != ".bmp")
        return "";
    std::string name = "photo" + std::to_string(n) + ext;
    if (!CopyFileA(src.c_str(), LevelFolderFile(name).c_str(), FALSE))
        return "";
    ReleaseImages();  // (a picture of that name read before)
    return name;
}

// the laptop's files of the level: name, picture (Mission tab, Simple mode's Mission step)
void DrawLaptopFiles()
{
    int places = MapPhotoCount();
    ImGui::PushTextWrapPos(0.0f);
    if (!places) {
        ImGui::TextDisabled("This map's laptop has no place for photos (its Additional files).");
        ImGui::PopTextWrapPos();
        return;
    }
    if (places == 1)
        ImGui::TextDisabled("The laptop's Additional files: this map has 1 place for a photo (the map's own photo "
                            "is shown there unless you give one).");
    else
        ImGui::TextDisabled("The laptop's Additional files: this map has %d places for photos.", places);
    ImGui::PopTextWrapPos();
    std::vector<LaptopFile>& files = Doc().files;
    for (int i = 0; i < places; ++i) {
        ImGui::PushID(i);
        if (i >= int(files.size())) {
            if (ImGui::SmallButton("Add a photo")) {
                files.push_back({"Target Photo", ""});
                Changed();
            }
            ImGui::PopID();
            break;
        }
        LaptopFile& f = files[i];
        char name[65];
        snprintf(name, sizeof(name), "%s", f.name.c_str());
        ImGui::Text("%d", i + 1);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(-ImGui::GetFontSize() * 2);
        ImGui::InputText("##fname", name, sizeof(name));
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("The entry's text in the laptop's Additional files");
        if (name != f.name) {
            f.name = name;
            Changed();
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("x")) {
            files.erase(files.begin() + i);
            Changed();
            ImGui::PopID();
            break;
        }
        ImGui::Indent();
        ImGui::TextDisabled("picture: %s", f.image.empty() ? "none (the map's own photo)" : f.image.c_str());
        if (ImGui::SmallButton("A file...")) {
            std::string picked = PickPicture(i + 1);
            g_status = picked.empty() ? "no picture taken (PNG, JPG or BMP)" : "picture " + picked + " in the level's folder";
            if (!picked.empty()) {
                f.image = picked;
                Changed();
            }
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("A picture of yours (PNG, JPG or BMP): copied into the level's folder, shown in the\n"
                              "laptop when this file is opened.");
        std::string last = LastPictureWhat();
        ImGui::BeginDisabled(last.empty());
        ImGui::SameLine();
        std::string label = last.empty() ? std::string("The last picture shown") : "Photo of " + last;
        if (ImGui::SmallButton(label.c_str())) {
            std::string err, shot = "photo" + std::to_string(i + 1) + ".png";
            if (SaveLastPicturePng(LevelFolderFile(shot), &err)) {
                ReleaseImages();
                f.image = shot;
                Changed();
                g_status = "photo of " + last + ": " + shot;
            } else {
                g_status = "no photo: " + err;
            }
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("The picture the lists show of a person (People) or an item (Items): hover one\n"
                              "until its picture shows, then this saves it as the file's photo.");
        ImGui::Unindent();
        ImGui::PopID();
    }
    ImGui::TextDisabled("Shown when the map loads again (Try it): the laptop is B while playing.");
}

void DrawSimpleMissionStep()
{
    if (!ForThisScene()) {
        ImGui::TextWrapped("The level is for %s: load its map first.", CurrentLevel().baseScene.c_str());
        return;
    }
    ImGui::SeparatorText("What the player has to do");
    int lines = 0;
    for (int slot = 1; slot <= kSlots; ++slot) {
        MissionObjective* o = ObjectiveAt(slot);
        std::string original = OwnText(slot);
        if (original.empty() && !o)
            continue;
        ++lines;
        ImGui::PushID(slot);
        std::string text = LineText(slot);
        bool own = o && o->goal != MissionObjective::Level;
        if (o && o->hidden) {  // an original line removed: shown greyed, it can come back
            ImGui::Bullet();
            ImGui::TextDisabled("%s (removed)", original.empty() ? "a line" : original.c_str());
            ImGui::Indent();
            if (ImGui::SmallButton("Put back")) {
                Doc().objectives.erase(Doc().objectives.begin() + (o - Doc().objectives.data()));
                CurrentLevel().dirty = true;
                ShowLines({slot});
            }
            ImGui::Unindent();
            ImGui::PopID();
            continue;
        }
        ImGui::Bullet();
        if (own && g_simpleTextSlot == slot) {  // its laptop text, written by hand
            char buf[256];
            snprintf(buf, sizeof(buf), "%s", o->text.c_str());
            ImGui::SetNextItemWidth(-1);
            if (ImGui::IsWindowAppearing() || !ImGui::IsAnyItemActive())
                ImGui::SetKeyboardFocusHere();
            if (ImGui::InputText("##linetext", buf, sizeof(buf), ImGuiInputTextFlags_EnterReturnsTrue))
                g_simpleTextSlot = 0;
            if (buf != o->text) {
                o->text = buf;
                CurrentLevel().dirty = true;
            }
            if (ImGui::IsItemDeactivated()) {
                g_simpleTextSlot = 0;
                Changed();
            }
        } else {
            ImGui::TextWrapped("%s", text.empty() ? "(a line without text yet)" : text.c_str());
            if (own && ImGui::IsItemHovered())
                ImGui::SetTooltip("The laptop's line: Change the text writes your own");
        }
        ImGui::Indent();
        if (!own) {
            ImGui::TextDisabled("the original mission's");
            // change it (a goal of the editor's on its line: the original's no longer counts) or remove it
            if (ImGui::SmallButton("Change"))
                ImGui::OpenPopup("change");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Another objective on this line: the original's no longer counts");
            if (ImGui::BeginPopup("change")) {
                const std::pair<const char*, MissionObjective::Goal> goals[] = {
                    {"Kill someone", MissionObjective::Kill},       {"Knock someone out", MissionObjective::KnockOut},
                    {"Get to a place", MissionObjective::Reach},    {"Take an object", MissionObjective::Take},
                    {"Bring an object somewhere", MissionObjective::Deliver}, {"Never be spotted", MissionObjective::Unseen}};
                for (const auto& [label, goal] : goals)
                    if (ImGui::Selectable(label)) {
                        MissionObjective& e = Editable(slot);
                        e.goal = goal;
                        e.replacesOriginal = true;
                        e.target.clear();
                        e.place.clear();
                        e.text = goal == MissionObjective::Unseen ? "Do not be spotted" : "";
                        e.radius = goal == MissionObjective::Take ? 150.0f : 250.0f;
                        Changed();
                        if (goal != MissionObjective::Unseen)
                            StartPick(goal == MissionObjective::Kill || goal == MissionObjective::KnockOut
                                          ? PickKind::Character
                                      : goal == MissionObjective::Reach ? PickKind::Place
                                                                         : PickKind::Object,
                                      slot);
                    }
                ImGui::EndPopup();
            }
            Flow("Remove");
            if (ImGui::SmallButton("Remove"))
                ImGui::OpenPopup("remove original");
            if (ImGui::BeginPopup("remove original")) {
                ImGui::Text("Remove \"%s\" from the mission?", text.c_str());
                ImGui::PushTextWrapPos(ImGui::GetFontSize() * 24);
                ImGui::TextDisabled("It goes from the laptop and no longer counts. The original mission would still "
                                    "want it done, so the mission then ends when the objectives you added are done.");
                ImGui::PopTextWrapPos();
                if (ImGui::Button("Remove it")) {
                    MissionObjective& e = Editable(slot);
                    e.goal = MissionObjective::Level;
                    e.hidden = true;
                    Doc().endWhenDone = true;
                    Changed();
                    ImGui::CloseCurrentPopup();
                }
                ImGui::SameLine();
                if (ImGui::Button("Keep it"))
                    ImGui::CloseCurrentPopup();
                ImGui::EndPopup();
            }
        } else {
            ImGui::TextDisabled("%s", Summary(o).c_str());
            bool button = true;
            if (o->goal == MissionObjective::Kill || o->goal == MissionObjective::KnockOut)
                PickButton(o->target.empty() ? "Pick who" : "Someone else", PickKind::Character, slot);
            else if (o->goal == MissionObjective::Take || o->goal == MissionObjective::Deliver)
                PickButton(o->target.empty() ? "Pick what" : "Something else", PickKind::Object, slot);
            else if (o->goal == MissionObjective::Reach)
                PickButton(o->target.empty() ? "Pick where" : "Another place", PickKind::Place, slot);
            else
                button = false;
            if (o->goal == MissionObjective::Deliver && !o->target.empty())
                PickButton(o->place.empty() ? "Pick where to" : "Another place to", PickKind::Place, 100 + slot, true);
            if (o->goal == MissionObjective::Unseen &&
                ImGui::Checkbox("being spotted fails the mission", &o->failsMission))
                Changed();
            if (button || o->goal == MissionObjective::Unseen)
                Flow("Change the text");
            if (ImGui::SmallButton("Change the text"))
                g_simpleTextSlot = slot;
            Flow("Remove");
            if (ImGui::SmallButton("Remove"))
                ImGui::OpenPopup("remove");
            if (ImGui::BeginPopup("remove")) {
                ImGui::Text("Remove \"%s\"?", text.c_str());
                if (ImGui::Button(original.empty() ? "Remove" : "Remove (the original's line comes back)")) {
                    Doc().objectives.erase(Doc().objectives.begin() + (o - Doc().objectives.data()));
                    CurrentLevel().dirty = true;
                    ShowLines({slot});
                    ImGui::CloseCurrentPopup();
                }
                ImGui::SameLine();
                if (ImGui::Button("Keep it"))
                    ImGui::CloseCurrentPopup();
                ImGui::EndPopup();
            }
        }
        ImGui::Unindent();
        ImGui::PopID();
    }
    if (!lines)
        ImGui::TextDisabled("No objectives yet: add one below.");

    ImGui::Spacing();
    bool free = FreeSlot() != 0;
    ImGui::BeginDisabled(!free);
    ImGui::TextUnformatted("Add an objective:");
    struct Sentence {
        const char* label;
        MissionObjective::Goal goal;
        const char* tip;
    };
    const Sentence sentences[] = {
        {"Kill someone", MissionObjective::Kill, "Then click the character in the world"},
        {"Knock someone out", MissionObjective::KnockOut, "Then click the character: done when knocked out and alive"},
        {"Get to a place", MissionObjective::Reach, "Then click the ground where Hitman has to get"},
        {"Take an object", MissionObjective::Take, "Then click an item (a key, a weapon) or any object"},
        {"Bring an object somewhere", MissionObjective::Deliver, "Then click the object; then where it has to go"},
    };
    float avail = ImGui::GetContentRegionAvail().x, x = 0;
    for (const Sentence& st : sentences) {
        float w = ImGui::CalcTextSize(st.label).x + ImGui::GetStyle().FramePadding.x * 2;
        if (x > 0 && x + w < avail)
            ImGui::SameLine();
        else
            x = 0;
        x += w + ImGui::GetStyle().ItemSpacing.x;
        if (ImGui::Button(st.label))
            NewSentence(st.goal);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", st.tip);
    }
    if (ImGui::Button("Never be spotted"))
        g_status = MissionUnseen(false);
    ImGui::EndDisabled();
    if (!free)
        ImGui::TextDisabled("The laptop has 6 lines, all in use: remove one first.");
    if (g_pick != PickKind::None)
        ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.45f, 1.0f), "Click in the world (Esc: cancel)");

    ImGui::SeparatorText("When the mission ends");
    MissionDoc& m = Doc();
    if (ImGui::RadioButton("As the original mission ends", !m.endWhenDone)) {
        m.endWhenDone = false;
        Changed();
    }
    if (ImGui::RadioButton("When the objectives you added are done", m.endWhenDone)) {
        m.endWhenDone = true;
        Changed();
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("The original mission's lines no longer end it then (its own targets, its exit)");
    if (m.endWhenDone && std::none_of(m.objectives.begin(), m.objectives.end(), [](const MissionObjective& o) {
            return !o.hidden && o.goal != MissionObjective::Level;
        })) {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.4f, 1.0f), "Add an objective above: there is none of yours to end it.");
        ImGui::PopTextWrapPos();
    }
    if (m.endWhenDone) {
        ImGui::Indent();
        std::string exit = Label(m.exitPath);
        ImGui::TextWrapped("and Hitman leaves through: %s", exit.empty() ? "anywhere (it ends at once)" : exit.c_str());
        PickButton(exit.empty() ? "Pick the exit" : "Another exit", PickKind::Place, 0);
        if (!exit.empty()) {
            ImGui::SameLine();
            if (ImGui::SmallButton("No exit")) {
                m.exitPath.clear();
                Changed();
            }
        }
        ImGui::Unindent();
    }

    ImGui::SeparatorText("Briefing (the laptop's text)");
    DrawBriefingEditor();
    ImGui::SeparatorText("The laptop's photos");
    DrawLaptopFiles();
    ImGui::SeparatorText("Hitman starts");
    DrawStartOutfitControl();
    if (!g_status.empty())
        ImGui::TextDisabled("%s", g_status.c_str());
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

    DrawStagesSection();
    ImGui::SeparatorText("Briefing (the laptop's text)");
    DrawBriefingEditor();
    bool files = !BaseFilesHidden();
    if (ImGui::Checkbox("The level's additional files (Target Photo)", &files)) {
        Doc().baseFiles = files ? 1 : 0;
        Changed();
        g_status = "the laptop's files change when the map loads again";
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("The base level's files in the laptop, e.g. its target's photo.\n"
                          "Hidden on an emptied map unless switched on. Takes effect when the map loads.");
    if (ImGui::TreeNode("The laptop's photos (the level's own)")) {
        DrawLaptopFiles();
        ImGui::TreePop();
    }
    DrawStartOutfitControl();  // (#21: from the Level tab)
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
    g_scriptTexts[key] = Node(text, 256);  // one line of the laptop
    BuildTexts();
    bool wasHidden = g_hiddenLines[slot];
    g_hiddenLines[slot] = false;
    if (text.empty()) {
        RestoreLine(slot);
        if (wasHidden && !OwnText(slot).empty())
            ShowLineElements(slot);
        return g_briefing != nullptr;
    }
    bool ok = CallSetObjective(slot, key.c_str());
    if (ok && (OwnText(slot).empty() || wasHidden)) {  // a line the level's laptop does not show
        ShowLineElements(slot);
        CallSetCompleted(slot, g_done[slot]);
    }
    return ok;
}

// the GUI's OSD window (its routine is the GUI main: 0x534130)
static engine::ZGEOM* Osd()
{
    const engine::SceneSnapshot& snap = SceneModel();
    const std::string tail = "ZWINDOWS/MainWindow/OSD";
    for (int i : snap.FindName("OSD")) {
        const std::string& p = snap.geoms[i].path;
        if (p.size() >= tail.size() && p.compare(p.size() - tail.size(), tail.size(), tail) == 0)
            return snap.geoms[i].geom;
    }
    return nullptr;
}

bool ShowInfo(InfoKind kind, const std::string& text)
{
    engine::ZGEOM* osd = Osd();
    if (!osd || text.empty())
        return false;
    // a key of its own each time (the GUI looks the text up when it shows it)
    static int next;
    static std::string keys[8];
    std::string& key = keys[next];
    next = (next + 1) % 8;
    key = "H2Editor/Info/" + std::to_string(&key - keys);
    std::string shown = text;
    for (char& c : shown)
        if (c == '\n' || c == '\r')
            c = ' ';
    if (shown.size() > 300)
        shown.resize(300);
    g_scriptTexts[Lower(key)] = Node(shown);
    BuildTexts();
    const char* msg = kind == InfoKind::Warning ? "MSG_DISPLAYWARNING"
                      : kind == InfoKind::Hint  ? "MSG_DISPLAYHINT"
                                                : "MSG_DISPLAYINFO";
    return engine::SendMessage(osd, osd, engine::RegisterMessage(msg), const_cast<char*>(key.c_str()));
}

bool ShowHitPoints(int percent)
{
    engine::ZGEOM* osd = engine::SceneComGeom("rOSD");  // not the OSD window: the scene's rOSD
    engine::ZGEOM* hitman = engine::Player();
    if (!osd || !hitman)
        return false;
    // as Hitman's damage 0x519390 tells it: MSG_SETHITPOINTS, the param the value itself
    return engine::SendMessage(osd, hitman, engine::RegisterMessage("MSG_SETHITPOINTS"),
                               reinterpret_cast<void*>(static_cast<intptr_t>(percent)));
}

bool ShowSubtitle(const std::string& text)
{
    static std::string shown;  // the GUI keeps the pointer: alive until the next one
    engine::ZGEOM* osd = Osd();
    if (!osd)
        return false;
    std::string next = ToGameText(text);
    for (char& c : next)
        if (c == '\n')
            c = ' ';  // one line
    if (next.size() > 200)
        next.resize(200);
    shown = next;
    return engine::SendMessage(osd, osd, engine::RegisterMessage("MSG_WRITESUBTITLE"),
                               const_cast<char*>(shown.c_str()));
}

bool HideObjectiveLine(int slot)
{
    if (slot < 1 || slot > kSlots)
        return false;
    g_hiddenLines[slot] = true;
    bool ok = CallSetObjective(slot, kEmptyKey);
    HideLineElements(slot);
    return ok;
}

bool SetBriefingText(const std::string& text)
{
    g_scriptBriefing = text;
    RefreshBriefing();
    return g_briefing != nullptr;
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
    engine::AllowLevelEnd();
    return CallLevel(kLevel_MissionCompleted);
}

bool FailMission()
{
    Log("mission: failed by the editor");
    engine::AllowLevelEnd();
    return CallLevel(kLevel_MissionFailed);
}

bool BaseLevelEnd(bool completed)
{
    return CallLevel(completed ? kLevel_MissionCompleted : kLevel_MissionFailed);
}

std::string LevelObjectiveText(int slot) { return OwnText(slot); }

namespace {
engine::ZGEOM* CharacterOf(engine::ZGEOM* geom);

// the objectives made for `path` (its target or its place) whose text the editor made: the new name
void RefreshTexts(const std::string& path)
{
    for (MissionObjective& o : Doc().objectives)
        if ((o.target == path || o.place == path) && AutoText(o.text)) {
            o.text.clear();
            SetTarget(o, o.target);
        }
    ShowLines();
}

// the path the objectives know an object by (a character: its actor's)
std::string NamePath(engine::ZGEOM* geom)
{
    engine::ZGEOM* c = geom ? CharacterOf(geom) : nullptr;
    return PathOf(c ? c : geom);
}
} // namespace

std::string ObjectName(engine::ZGEOM* geom)
{
    if (!geom || !IsLive(geom))
        return "";
    if (engine::ZGEOM* a = ActorOf(geom))
        if (const LevelNpc* n = FindLevelNpc(a); n && !n->label.empty())
            return n->label;
    auto it = Doc().names.find(NamePath(geom));
    return it == Doc().names.end() ? "" : it->second;
}

bool SetObjectName(engine::ZGEOM* geom, const std::string& name, std::string* error)
{
    auto fail = [&](const std::string& why) {
        if (error)
            *error = why;
        return false;
    };
    if (!geom || !IsLive(geom))
        return fail("no object");
    if (name.size() > 40 || std::any_of(name.begin(), name.end(), [](char c) { return unsigned(c) < 0x20; }))
        return fail("40 letters at most, on one line");
    std::string path = NamePath(geom);
    if (path.empty())
        return fail("the object has no path the level can keep");
    engine::ZGEOM* a = ActorOf(geom);
    if (a && FindLevelNpc(a)) {  // placed by the level: its label (scripts find it by it)
        if (!SetNpcLabel(a, name, error))
            return false;
    } else {
        std::string before = ObjectName(geom);
        auto set = [path](const std::string& value) {
            if (value.empty())
                Doc().names.erase(path);
            else
                Doc().names[path] = value;
            CurrentLevel().dirty = true;
            RefreshTexts(path);
            return true;
        };
        set(name);
        PushAction("name an object", [set, before] { return set(before); }, [set, name] { return set(name); });
    }
    RefreshTexts(path);
    Changed();
    return true;
}

void DrawObjectNameRow(engine::ZGEOM* geom)
{
    static char name[41];
    static engine::ZGEOM* nameOf;
    if (!geom || !IsLive(geom))
        return;
    std::string current = ObjectName(geom);
    if (nameOf != geom) {
        snprintf(name, sizeof(name), "%s", current.c_str());
        nameOf = geom;
    }
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10);
    bool enter = ImGui::InputTextWithHint("##objname", Label(NamePath(geom)).c_str(), name, sizeof(name),
                                          ImGuiInputTextFlags_EnterReturnsTrue);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Its name in the mission: the objectives made for it say it (\"Kill the courier\",\n"
                          "\"Take the ledger\"); empty: the model's name.");
    ImGui::SameLine();
    ImGui::BeginDisabled(current == name);
    if (ImGui::SmallButton("Name it") || (enter && current != name)) {
        std::string err;
        g_status = SetObjectName(geom, name, &err) ? (name[0] ? std::string("named: ") + name : "its own name taken off")
                                                    : "not named: " + err;
    }
    ImGui::EndDisabled();
}

std::vector<GoalStatus> GoalStatuses()
{
    std::vector<GoalStatus> out;
    if (!ForThisScene() || !engine::SceneDataValid())
        return out;
    static const char* const kWhat[] = {"the level's own", "kill", "reach", "take", "knock out", "bring", "never spotted"};
    for (const MissionObjective& o : StageObjectives()) {
        if (o.hidden)
            continue;
        GoalStatus g;
        g.slot = o.slot;
        g.text = o.text.empty() ? OwnText(o.slot) : o.text;
        if (g.text.empty())
            g.text = std::string(kWhat[o.goal]) + (o.target.empty() ? "" : " " + o.target);
        engine::ZGEOM* t = o.goal == MissionObjective::Level || o.goal == MissionObjective::Unseen ? nullptr : Target(o);
        g.where = o.goal == MissionObjective::Deliver && !o.place.empty() ? ResolveTarget(o.place) : t;
        bool done = o.goal == MissionObjective::Level ? LineChecked(o.slot) : g_done[o.slot];
        if (o.goal == MissionObjective::Unseen)
            done = !g_spotted;
        if (done) {
            g.state = 1;
        } else if (o.goal == MissionObjective::Unseen) {
            g.state = -1;
            g.why = "Hitman was spotted";
        } else if (o.goal != MissionObjective::Level && !t) {
            g.state = -1;
            g.why = (o.goal == MissionObjective::Kill || o.goal == MissionObjective::KnockOut ? "the character "
                                                                                               : "the object ") +
                    o.target + " is not there";
        } else if (o.goal == MissionObjective::KnockOut) {
            engine::ZGEOM* actor = ActorOf(t);
            engine::ZGEOM* who = actor ? actor : t;
            if (engine::WasKilled(who) || (!engine::IsAlive(t) && !KnockedOut(t))) {
                g.state = -1;
                g.why = "killed, not knocked out";
            }
        }
        if (g.state == 0 && g.where) {
            char d[32];
            snprintf(d, sizeof(d), "%.0f m", DistanceToPlayer(g.where) / 100.0f);
            g.why = d;
        }
        out.push_back(std::move(g));
    }
    return out;
}

namespace {

// The base level's exit off the in-game map while its scripts do not end the mission (#17: C2-2's
// EXIT at the pier showed on an emptied map whose mission ends elsewhere); back as the scene file has it.
void BaseExitIcons(bool shown)
{
    if (shown) {
        for (engine::ZGEOM* g : g_exitIcons)
            if (IsLive(g) && engine::MapIconShownInFile(g) == 1)
                engine::ShowMapIcon(g, true);
        g_exitIcons.clear();
        return;
    }
    const auto& records = engine::SceneRecords();
    for (size_t i = 0; i < records.size(); ++i) {
        engine::ZGEOM* g = engine::RecordGeom(int(i));
        if (!g || !IsLive(g) || engine::MapIconShown(g) != 1 || engine::MapIconName(g) != "Icon_Exit")
            continue;
        if (std::find(g_exitIcons.begin(), g_exitIcons.end(), g) == g_exitIcons.end())
            g_exitIcons.push_back(g);
        engine::ShowMapIcon(g, false);
    }
}

} // namespace

void ApplyMission()
{
    g_applied = true;
    engine::SetLevelEndsAllowed(Doc().baseEnds);
    BaseExitIcons(Doc().baseEnds);
    ShowLines();
}

void RevertMission()
{
    engine::SetLevelEndsAllowed(true);
    BaseExitIcons(true);
    for (auto& [target, st] : g_taken)  // taken objects come back
        if (engine::ZGEOM* g = ResolveTarget(target); g && IsLive(g))
            SetState(g, st);
    g_taken.clear();
    memset(g_takeObject, 0, sizeof(g_takeObject));
    std::vector<int> slots;
    for (const MissionObjective& o : StageObjectives())
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

// ---- goals from the selection (the inspector) ----

namespace {

// the character (as the objectives keep it) the object belongs to; null: not a character, or Hitman
engine::ZGEOM* CharacterOf(engine::ZGEOM* geom)
{
    engine::ZGEOM* actor = geom ? ActorOf(geom) : nullptr;
    if (!actor || actor == engine::Player())
        return nullptr;
    for (engine::ZGEOM* c : Characters())
        if (c == geom || c == actor || ActorOf(c) == actor)
            return c;
    return nullptr;
}

// the same object (a point or a copy of the level made with the map is at the root, its path
// when the goal was set had its room)
bool SameTarget(const std::string& a, const std::string& b)
{
    if (a == b)
        return true;
    engine::ZGEOM* g = a.empty() ? nullptr : ResolveTarget(a);
    return g && g == ResolveTarget(b);
}

// the line already having this target with this goal, 0 if none
int LineWith(MissionObjective::Goal goal, const std::string& path)
{
    for (const MissionObjective& o : StageObjectives())
        if (o.goal == goal && SameTarget(o.target, path))
            return o.slot;
    return 0;
}

void NewGoal(MissionObjective::Goal goal, const std::string& path)
{
    int slot = FreeSlot();
    if (!slot) {
        g_status = "the laptop has 6 lines, all in use";
        return;
    }
    MissionObjective o;
    o.slot = slot;
    o.goal = goal;
    if (goal == MissionObjective::Take)
        o.radius = 150.0f;
    o.span = NewSpan();
    Doc().objectives.push_back(o);
    SetTarget(Doc().objectives.back(), path);
    g_selSlot = slot;
    g_status = "objective " + std::to_string(slot) + ": " + Summary(&Doc().objectives.back());
    overlay::ShowPanel("Mission");
}

} // namespace

std::string MissionGoalFor(engine::ZGEOM* geom, const std::string& goal)
{
    if (!geom || !IsLive(geom))
        return "no such object";
    if (goal == "target") {
        engine::ZGEOM* c = CharacterOf(geom);
        if (!c)
            return "not a character";
        NewGoal(MissionObjective::Kill, PathOf(c));
    } else if (goal == "reach" || goal == "exit") {
        std::string path = PathOf(geom);
        if (path.empty() || ActorOf(geom))
            return "not a place";
        if (goal == "reach") {
            NewGoal(MissionObjective::Reach, path);
        } else {
            Doc().endWhenDone = true;
            Doc().exitPath = path;
            Changed();
            g_status = "the mission ends when every objective is done and Hitman reaches " + Label(path);
        }
    } else if (goal == "take") {
        engine::ZGEOM* g = TakeableAt(geom);
        if (!g)
            return "not an object to take";
        NewGoal(MissionObjective::Take, TargetFor(g));
    } else if (goal == "knockout") {
        engine::ZGEOM* c = CharacterOf(geom);
        if (!c)
            return "not a character";
        NewGoal(MissionObjective::KnockOut, PathOf(c));
    } else if (goal == "deliver") {  // the object; its place: MissionPlaceFor
        engine::ZGEOM* g = TakeableAt(geom);
        if (!g)
            return "not an object to take";
        NewGoal(MissionObjective::Deliver, TargetFor(g));
    } else {
        return "goal: target, knockout, reach, take, deliver or exit";
    }
    return g_status.empty() ? "ok" : g_status;
}

std::string MissionPlaceFor(int slot, engine::ZGEOM* place)
{
    MissionObjective* o = slot >= 1 && slot <= kSlots ? ObjectiveAt(slot) : nullptr;
    if (!o || o->goal != MissionObjective::Deliver)
        return "objective " + std::to_string(slot) + " is not a \"bring it to a place\" one";
    if (!place || !IsLive(place) || PathOf(place).empty())
        return "no such place";
    SetPlace(100 + slot, PathOf(place));
    return "objective " + std::to_string(slot) + ": " + Summary(o);
}

std::string MissionUnseen(bool failsMission)
{
    int slot = FreeSlot();
    if (!slot)
        return "the laptop has 6 lines, all in use";
    MissionObjective o;
    o.slot = slot;
    o.goal = MissionObjective::Unseen;
    o.text = "Do not be spotted";
    o.failsMission = failsMission;
    o.span = NewSpan();
    Doc().objectives.push_back(o);
    Changed();
    return "objective " + std::to_string(slot) + ": " + Summary(&Doc().objectives.back());
}

void DrawMissionForSelection(engine::ZGEOM* geom)
{
    if (!geom || !IsLive(geom) || CurrentScene().empty() ||
        (!CurrentLevel().baseScene.empty() && CurrentLevel().baseScene != CurrentScene()))
        return;
    ImGui::SeparatorText("Mission");
    DrawObjectNameRow(geom);
    bool full = FreeSlot() == 0;
    if (engine::ZGEOM* c = CharacterOf(geom)) {
        std::string path = PathOf(c);
        if (int slot = LineWith(MissionObjective::Kill, path)) {
            ImGui::TextDisabled("the target of objective %d", slot);
            ImGui::SameLine();
            if (ImGui::SmallButton("edit")) {
                g_selSlot = slot;
                overlay::ShowPanel("Mission");
            }
            return;
        }
        if (int slot = LineWith(MissionObjective::KnockOut, path)) {
            ImGui::TextDisabled("to be knocked out: objective %d", slot);
            ImGui::SameLine();
            if (ImGui::SmallButton("edit##ko")) {
                g_selSlot = slot;
                overlay::ShowPanel("Mission");
            }
            return;
        }
        ImGui::BeginDisabled(full);
        if (ImGui::Button("Make it a target"))
            NewGoal(MissionObjective::Kill, path);
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip(full ? "The laptop has 6 lines, all in use"
                                   : "A new objective: \"Kill ...\", done when this character dies");
        ImGui::SameLine();
        ImGui::BeginDisabled(full);
        if (ImGui::Button("Knock it out"))
            NewGoal(MissionObjective::KnockOut, path);
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip(full ? "The laptop has 6 lines, all in use"
                                   : "A new objective: \"Knock out ...\", done when it is knocked out and lives");
        return;
    }
    if (ActorOf(geom))  // Hitman
        return;
    std::string path = PathOf(geom);
    if (path.empty())
        return;
    // an object to take (an item: only this row, it is not a place)
    if (engine::ZGEOM* take = TakeableAt(geom)) {
        std::string what = TargetFor(take);
        if (int slot = LineWith(MissionObjective::Take, what)) {
            ImGui::TextDisabled("Hitman takes it: objective %d", slot);
            ImGui::SameLine();
            if (ImGui::SmallButton("edit##take")) {
                g_selSlot = slot;
                overlay::ShowPanel("Mission");
            }
        } else {
            ImGui::BeginDisabled(full);
            if (ImGui::Button("Hitman has to take it"))
                NewGoal(MissionObjective::Take, what);
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::BeginDisabled(full || LineWith(MissionObjective::Deliver, what) != 0);
            if (ImGui::Button("...bring it to a place")) {
                NewGoal(MissionObjective::Deliver, what);
                g_status += ": choose the place in the Mission tab";
            }
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("%s", full           ? "The laptop has 6 lines, all in use"
                                        : IsItem(take) ? "A new objective: \"Take ...\", done when Hitman picks this item up"
                                                       : "A new objective: \"Take ...\", done when Hitman presses E near it\n"
                                                         "(it goes from the world)");
        }
        if (IsItem(take))
            return;
    }
    int line = LineWith(MissionObjective::Reach, path);
    bool exit = Doc().endWhenDone && SameTarget(Doc().exitPath, path);
    if (line) {
        ImGui::TextDisabled("the place of objective %d", line);
        ImGui::SameLine();
        if (ImGui::SmallButton("edit")) {
            g_selSlot = line;
            overlay::ShowPanel("Mission");
        }
    } else {
        ImGui::BeginDisabled(full);
        if (ImGui::Button("Hitman has to get here"))
            NewGoal(MissionObjective::Reach, path);
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip(full ? "The laptop has 6 lines, all in use"
                                   : "A new objective: \"Reach ...\", done when Hitman gets near it");
        ImGui::SameLine();
        if (ImGui::GetContentRegionAvail().x <
            ImGui::CalcTextSize("Make it the exit").x + ImGui::GetStyle().FramePadding.x * 2)
            ImGui::NewLine();  // no room: on the next line
    }
    if (exit) {
        ImGui::TextDisabled("the mission's exit");
    } else if (ImGui::Button("Make it the exit")) {
        Doc().endWhenDone = true;
        Doc().exitPath = path;
        Changed();
        g_status = "the mission ends when every objective is done and Hitman reaches " + Label(path);
        overlay::ShowPanel("Mission");
    }
    if (!exit && ImGui::IsItemHovered())
        ImGui::SetTooltip("The mission is completed when every objective is done and Hitman gets here");
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
    } else if (g_pick == PickKind::Object) {
        DrawHint("click the object Hitman has to take   Esc: cancel");
        if (g_pickPending && ImGui::GetFrameCount() - g_pickFrame > 10)
            g_pickPending = false;
        if (overWorld && !g_pickPending && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            g_pickPending = true;
            g_pickFrame = ImGui::GetFrameCount();
            RequestPick(io.MousePos.x, io.MousePos.y, OnObjectPick);
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

namespace {

// the editor's goals done (and objects taken) in a saved game
std::string SaveGoals()
{
    if (ForThisScene() && IsEscalation() && Doc().objectives.empty())  // (its stage: stages.h)
        return nlohmann::json{{"scene", CurrentScene()}, {"level", CurrentLevel().name}, {"stage", CurrentStage()}}
            .dump();
    if (!ForThisScene() || Doc().objectives.empty())
        return "";
    nlohmann::json done = nlohmann::json::array(), taken = nlohmann::json::array();
    for (const MissionObjective& o : StageObjectives())
        if (o.goal != MissionObjective::Level && g_done[o.slot])
            done.push_back(o.slot);
    for (const auto& [target, st] : g_taken)
        taken.push_back(target);
    if (done.empty() && taken.empty() && !g_spotted && !IsEscalation())
        return "";
    nlohmann::json j{{"scene", CurrentScene()}, {"level", CurrentLevel().name}, {"done", done},
                     {"taken", taken},          {"spotted", g_spotted}};
    if (IsEscalation())
        j["stage"] = CurrentStage();
    return j.dump();
}

void LoadGoals(const std::string& text) { g_pendingGoals = text; }

// after a saved game was loaded: its goals done again, its taken objects out
void ApplyPendingGoals()
{
    if (g_pendingGoals.empty())
        return;
    std::string text;
    text.swap(g_pendingGoals);
    nlohmann::json j = nlohmann::json::parse(text, nullptr, false);
    if (!j.is_object() || j.value("scene", "") != CurrentScene() || j.value("level", "") != CurrentLevel().name)
        return;
    for (const auto& t : j.value("taken", nlohmann::json::array()))
        if (t.is_string())
            for (const MissionObjective& o : StageObjectives())
                if ((o.goal == MissionObjective::Take || o.goal == MissionObjective::Deliver) &&
                    o.target == t.get<std::string>())
                    TakeOut(o.target, Target(o));
    if (j.value("spotted", false))
        g_spotted = g_spottedFailed = true;  // (the failure was the saved game's own)
    for (const auto& s : j.value("done", nlohmann::json::array()))
        if (s.is_number_integer()) {
            int slot = s.get<int>();
            const MissionObjective* o = slot >= 1 && slot <= kSlots ? ObjectiveAt(slot) : nullptr;
            if (o && o->goal != MissionObjective::Level && !g_done[slot]) {
                g_done[slot] = true;
                CallSetCompleted(slot, true);
                if (o->goal == MissionObjective::Deliver && g_taken.count(o->target))  // put down at its place
                    if (engine::ZGEOM* t = Target(*o), *place = ResolveTarget(o->place); t && place) {
                        ObjectState st = g_taken[o->target];
                        memcpy(st.transform.pos, GetTransform(place).pos, sizeof(st.transform.pos));
                        SetState(t, st);
                    }
            }
        }
    Log("mission: goals of the saved game back (%s)", text.c_str());
}

// the game's events the goals need: Hitman spotted (a character knocked out: its state, Reached)
void OnEditorEvent(const char* /*event*/, const engine::GameEvent& e)
{
    if (!ForThisScene())
        return;
    bool watched = false;  // only a level with an "unseen" goal counts it
    for (const MissionObjective& o : StageObjectives())
        watched |= o.goal == MissionObjective::Unseen;
    if (watched && !g_spotted) {
        g_spotted = true;
        Log("mission: Hitman spotted (%s)", e.name.c_str());
    }
}

} // namespace

std::string GoalZoneName(int code)
{
    if (code == 0)
        return "H2_Exit";
    return code > 100 ? "H2_Deliver_" + std::to_string(code - 100) : "H2_Reach_" + std::to_string(code);
}

void InstallMission()
{
    script::SetEditorEvents({"spotted", "hostile"}, OnEditorEvent);
    script::SetSaveExtra(SaveGoals, LoadGoals);
    OnScenePushed([](const std::string&) { g_pick = PickKind::None; });
    Hook("LOC find", kLocFind, hkLocFind, &oLocFind);
    Hook("BriefingMenu::SetLocaleFolder", kSetLocaleFolder, hkSetFolder, &oSetFolder);
    Hook("BriefingMenu::SetObjective", kSetObjective, hkSetObjective, &oSetObjective);
    Hook("Level::InitMessages", kLevelInit, hkLevelInit, &oLevelInit);
    Hook("BriefingMenu::SetObjectiveCompleted", kSetCompleted, hkSetCompleted, &oSetCompleted);
    Hook("BriefingMenu::Action", kBriefingAction, hkBriefingAction, &oBriefingAction);
    overlay::AddPanel({"Mission", &g_panelVisible, DrawPanel, overlay::Dock::Left});
    overlay::AddAlwaysDraw(DrawLaptopPhoto);
    OnScenePushed(OnScene);
    OnFrameEnd(Check);
}

void DrawSimpleMission() { DrawSimpleMissionStep(); }

bool MissionStepDone()
{
    const LevelDocument& L = CurrentLevel();
    int own = 0;
    for (const MissionObjective& o : L.mission.objectives) {
        if (o.hidden)
            continue;
        own += o.goal != MissionObjective::Level;
        if (o.goal != MissionObjective::Level && o.goal != MissionObjective::Unseen &&
            (o.target.empty() || (o.goal == MissionObjective::Deliver && o.place.empty())))
            return false;  // a blank not filled
    }
    if (L.mission.endWhenDone && !own)
        return false;  // the objectives end it, and there are none
    return own > 0 || !L.emptyMap;  // own objectives, or the original's
}

} // namespace h2::editor
