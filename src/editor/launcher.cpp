#include "launcher.h"

#include <windows.h>
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "imgui.h"
#include "json.hpp"

#include "../display.h"
#include "../engine/scene_data.h"
#include "../engine/spawn.h"
#include "../game.h"
#include "../hook.h"
#include "../input/dinput.h"
#include "../log.h"
#include "../mainloop.h"
#include "../overlay/overlay.h"
#include "../scene.h"
#include "level.h"
#include "mission.h"
#include "model.h"

namespace h2::editor {

namespace {

struct Map {
    const char* scene;
    const char* name;
};
const Map kMaps[] = {
    {"C0-1\\C0-1__MAIN", "The Gontranno Sanctuary"},   {"C1-1\\C1-1__MAIN", "Anathema"},
    {"C2-1\\C2-1__MAIN", "St. Petersburg Stakeout"},   {"C2-2\\C2-2__MAIN", "Kirov Park Meeting"},
    {"C2-3\\C2-3__MAIN", "Tubeway Torpedo"},           {"C2-4\\C2-4__MAIN", "Invitation to a Party"},
    {"C3-1\\C3-1__MAIN", "Tracking Hayamoto"},         {"C3-2a\\C3-2a__MAIN", "Hidden Valley"},
    {"C3-2b\\C3-2b__MAIN", "At the Gates"},            {"C3-3\\C3-3__MAIN", "Shogun Showdown"},
    {"C4-1\\C4-1__MAIN", "Basement Killing"},          {"C4-2\\C4-2__MAIN", "The Graveyard Shift"},
    {"C4-3\\C4-3__MAIN", "The Jacuzzi Job"},           {"C5-1\\C5-1__MAIN", "Murder at the Bazaar"},
    {"C5-2\\C5-2__MAIN", "The Motorcade Interception"}, {"C5-3\\C5-3__MAIN", "Tunnel Rat"},
    {"C6-1\\C6-1__MAIN", "Temple City Ambush"},        {"C6-2\\C6-2__MAIN", "The Death of Hannelore"},
    {"C6-3\\C6-3__MAIN", "Terminal Hospitality"},      {"C7-1\\C7-1__MAIN", "St. Petersburg Revisited"},
    {"C8-1\\C8-1__MAIN", "Redemption at Gontranno"},
};
constexpr float kRow = 24.0f;            // the main menu's line height (GUI units)
constexpr float kTextSize = 20.5f;       // its letters' size (GUI units)
const char* const kEntry = "Level Editor";

// The entry is a button of the game's own (CHM2Button with its text, a
// ZCHAROBJ "Normal"), made from the Options button's records with an id the
// menu does not know: its handler (0x54BF40, thiscall (int id): 1 Start,
// 2 Options, 3 Quit, 4 Load, 5 Credits) is hooked for it.
constexpr uintptr_t kMainMenu_OnButton = 0x54BF40;
constexpr int kEntryId = 7;
constexpr uint32_t kClassText = 0x20002D;
constexpr size_t kButton_Up = 0x80, kButton_Down = 0x84;  // the neighbours' refs (arrow keys)
const char* const kEntryKey = "H2Editor/Menu/LevelEditor";
const char* const kTemplateKey = "AllLevels/Interface/Options";

// main menu entry
engine::ZGEOM* g_button;  // the game's button made for it (null: drawn by the overlay)
bool g_buttonTried;
bool g_hovered;

// launcher
bool g_launcherOpen;
int g_map = 1;
char g_name[64] = "My level";
bool g_empty = true;
bool g_blank = false;
// floors for a map without buildings: large flat models of other levels
// (single records with collision, in rooms)
struct Floor {
    const char* label;
    const char* level;
    uint32_t mesh;
    const char* name;
};
const Floor kFloors[] = {
    {"no floor", "", 0, ""},
    {"stone yard, 80 x 58 m (C3-3)", "C3-3", 0x60C472, "Yard_06-7-8"},
    {"stone paving, 70 x 51 m (C5-2)", "C5-2", 0x2951A6, "Ground_11"},
    {"snowy ground, 102 x 113 m (C2-2)", "C2-2", 0x2D2D2A, "Ground_03"},
};
int g_floor = 1;
struct Saved {
    std::string folder, name, scene, modified;
};
std::vector<Saved> g_saved;
int g_pick = -1;
std::string g_message;

// after the map is loaded: open the editor
std::string g_startScene;
bool g_editAfterStart = true;  // the editor opens when the map has started (Edit), not for Play

bool InMainMenu() { return _stricmp(CurrentScene().c_str(), "MainMenu") == 0; }

// The game's GUI cursor: the mouse window keeps its position at +0x11C / +0x120 (GUI units).
constexpr size_t kMouseWindow_Cursor = 0x11C;

// an object of the main menu (several windows have objects of the same name)
engine::ZGEOM* FindIn(const engine::SceneSnapshot& snap, const char* name, const char* within)
{
    for (int i : snap.FindName(name))
        if (snap.geoms[i].path.find(within) != std::string::npos && IsLive(snap.geoms[i].geom))
            return snap.geoms[i].geom;
    return nullptr;
}

// (every frame in the main menu: the snapshot is taken again only when the object is not in it)
engine::ZGEOM* Find(const char* name, const char* within = "MainMenu/")
{
    if (engine::ZGEOM* g = FindIn(SceneModel(), name, within))
        return g;
    if (engine::ZGEOM* g = FindIn(FresherSceneModel(), name, within))
        return g;
    return nullptr;
}

// the GUI's virtual screen (SysInterface +0x64 / +0x68, e.g. 1067 x 600) -> pixels
bool GuiToScreen(float x, float y, ImVec2& out, float& scale)
{
    auto sys = reinterpret_cast<char*>(game::SysInterface());
    if (!sys)
        return false;
    int vw = *reinterpret_cast<int*>(sys + 0x64), vh = *reinterpret_cast<int*>(sys + 0x68);
    if (vw <= 0 || vh <= 0 || RenderHeight() <= 0)
        return false;
    scale = float(RenderHeight()) / float(vh);
    float ox = (float(RenderWidth()) - float(vw) * scale) * 0.5f;
    out = ImVec2(ox + x * scale, y * scale);
    return true;
}

// The entry is the line under Load, the buttons under it move a line down.
// Kept relative to Load: the menu adds the screen's margins when it starts.
void Layout(engine::ZGEOM* load)
{
    Transform lt = GetTransform(load);
    engine::ZGEOM* lines[] = {g_button, Find("3Options"), Find("4Credits"), Find("6Quit")};
    for (int i = 0; i < 4; ++i) {
        engine::ZGEOM* b = lines[i];
        if (!b || !IsLive(b))
            continue;
        Transform t = GetTransform(b);
        float y = lt.pos[1] + kRow * float(i + 1);
        if (fabsf(t.pos[1] - y) > 0.01f) {
            t.pos[1] = y;
            SetTransform(b, t);
        }
    }
}

uint32_t* Field(engine::ZGEOM* geom, size_t offset)
{
    auto p = reinterpret_cast<uint32_t*>(reinterpret_cast<char*>(geom) + offset);
    return engine::IsReadable(p, 4) ? p : nullptr;
}

// The Options button made again under Load: its data block with our id and
// neighbours (Load above, Options below), its text with our LOC key.
engine::ZGEOM* MakeButton(engine::ZGEOM* load, engine::ZGEOM* options)
{
    auto fail = [](const char* why) -> engine::ZGEOM* {
        Log("launcher: menu button not made (%s), the overlay draws the entry", why);
        return nullptr;
    };
    if (!options || !IsLive(options))
        return fail("no Options button");
    const auto& records = engine::SceneRecords();
    int index = engine::RecordOf(options);
    const engine::SceneRecord* button = nullptr;
    const engine::SceneRecord* text = nullptr;
    for (const engine::SceneRecord& r : records)
        if (r.index == index)
            button = &r;
    for (const engine::SceneRecord& r : records)
        if (button && r.parent == button->index && r.classId == kClassText)
            text = &r;
    if (!button || !text || !button->data || !text->data)
        return fail("no records of the Options button");

    uint32_t refLoad = engine::RefOf(load), refOptions = engine::RefOf(options);
    std::vector<uint8_t> data = engine::RemappedBlock(button->data);
    std::vector<size_t> refs = engine::BlockRefOffsets(data);
    // ... LONG id, REF left, REF right, REF up, REF down
    if (refs.size() < 4 || refs[0] < 10)
        return fail("unknown button data");
    size_t id = refs[0] - 5;
    auto u32 = [&](size_t at) {
        uint32_t v;
        memcpy(&v, &data[at], 4);
        return v;
    };
    if ((data[id - 1] & 0x3F) != 3 || u32(id) != 2 || u32(refs[2]) != refLoad)
        return fail("unexpected button data");
    uint32_t v = kEntryId;
    memcpy(&data[id], &v, 4);
    memcpy(&data[refs[3]], &refOptions, 4);

    std::vector<uint8_t> label = engine::RemappedBlock(text->data);
    std::string from(kTemplateKey, strlen(kTemplateKey) + 1), to(kEntryKey, strlen(kEntryKey) + 1);
    auto at = std::search(label.begin(), label.end(), from.begin(), from.end());
    if (at == label.end())
        return fail("unknown text data");
    at = label.erase(at, at + from.size());
    label.insert(at, to.begin(), to.end());
    uint32_t header;
    memcpy(&header, label.data(), 4);
    header = (header & 0xFF000000) | uint32_t(label.size());
    memcpy(label.data(), &header, 4);

    engine::ZBaseGeom* parent = options->node->parent;
    if (!parent || !engine::IsReadable(parent, sizeof(*parent)) || !IsLive(parent->geom))
        return fail("no button group");
    float pos[3] = {load->node->pos[0], load->node->pos[1] + kRow, load->node->pos[2]};
    std::string err;
    engine::ZGEOM* made = engine::CreateObject("5LevelEditor", parent->geom, "CHM2Button", button->mesh,
                                               button->flags, button->mat, pos, data, &err, false);
    if (!made)
        return fail(err.c_str());
    if (!engine::CreateObject(text->name, made, "ZCHAROBJ", text->mesh, text->flags, text->mat, text->pos, label,
                              &err))
        Log("launcher: menu button text not made (%s)", err.c_str());
    if (!engine::InitObject(made))
        return fail("init failed");
    // the neighbours lead to it
    uint32_t ref = engine::RefOf(made);
    uint32_t* down = Field(load, kButton_Down);
    uint32_t* up = Field(options, kButton_Up);
    if (ref && down && *down == refOptions)
        *down = ref;
    if (ref && up && *up == refLoad)
        *up = ref;
    Log("launcher: menu button made (%p, ref %08X)", static_cast<void*>(made), ref);
    return made;
}

void LoadSavedList()
{
    g_saved.clear();
    for (const std::string& folder : ListLevels()) {
        Saved s;
        s.folder = folder;
        s.name = folder;
        std::ifstream f(GameDir() + "Mods\\" + folder + "\\level.json", std::ios::binary | std::ios::ate);
        if (!f || f.tellg() > std::streamoff(16 << 20))
            continue;  // missing, or too large to be a level (LoadLevel refuses it too)
        f.seekg(0);
        try {
            nlohmann::json j = nlohmann::json::parse(f);
            s.name = j.value("name", folder);
            s.scene = j.value("base_scene", "");
            s.modified = LevelModified(folder);
        } catch (...) {
            continue;
        }
        g_saved.push_back(s);
    }
    g_pick = g_saved.empty() ? -1 : 0;
}

const char* MapName(const std::string& scene)
{
    for (const Map& m : kMaps)
        if (_stricmp(m.scene, scene.c_str()) == 0)
            return m.name;
    return scene.c_str();
}

void StartOn(const std::string& scene)
{
    g_launcherOpen = false;
    overlay::SetExclusive(nullptr);
    overlay::SetOpen(false);
    g_startScene = scene;
    if (!RequestScene(scene)) {
        g_message = "the game did not start the map";
        g_startScene.clear();
    }
}

void DrawLauncher()
{
    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Always,
                            ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 34, ImGui::GetFontSize() * 26), ImGuiCond_Always);
    bool open = true;
    if (ImGui::Begin("Level editor##launcher", &open, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
                                                          ImGuiWindowFlags_NoMove)) {
        if (ImGui::BeginTabBar("launcher")) {
            if (ImGui::BeginTabItem("New level")) {
                ImGui::TextWrapped("A level of your own on one of the game's maps. The editor opens on the map: "
                                   "place characters, objects and items, give them routes, set the objectives and "
                                   "the music, then save (Save in the toolbar, or Ctrl+S).");
                ImGui::Spacing();
                ImGui::SetNextItemWidth(-ImGui::GetFontSize() * 4);
                if (ImGui::BeginCombo("map", kMaps[g_map].name)) {
                    for (int i = 0; i < int(sizeof(kMaps) / sizeof(kMaps[0])); ++i) {
                        char label[96];
                        snprintf(label, sizeof(label), "%s  (%s)", kMaps[i].name, kMaps[i].scene);
                        if (ImGui::Selectable(label, i == g_map))
                            g_map = i;
                    }
                    ImGui::EndCombo();
                }
                ImGui::SetNextItemWidth(-ImGui::GetFontSize() * 4);
                ImGui::InputText("name", g_name, sizeof(g_name));
                ImGui::Checkbox("Empty map", &g_empty);
                ImGui::SameLine();
                ImGui::TextDisabled("(no characters, items or objectives of the level)");
                ImGui::BeginDisabled(!g_empty);
                ImGui::Indent();
                ImGui::Checkbox("No buildings either", &g_blank);
                ImGui::SameLine();
                ImGui::TextDisabled("(only the sky: build with models)");
                ImGui::BeginDisabled(!g_blank);
                ImGui::SetNextItemWidth(-ImGui::GetFontSize() * 4);
                if (ImGui::BeginCombo("floor", kFloors[g_floor].label)) {
                    for (int i = 0; i < int(sizeof(kFloors) / sizeof(kFloors[0])); ++i)
                        if (ImGui::Selectable(kFloors[i].label, i == g_floor))
                            g_floor = i;
                    ImGui::EndCombo();
                }
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                    ImGui::SetTooltip("A model of another level put under Hitman where he starts: it is an object of "
                                      "your level\n(move it, copy it, delete it like the others)");
                ImGui::EndDisabled();
                ImGui::Unindent();
                ImGui::EndDisabled();
                ImGui::Spacing();
                if (ImGui::Button("Create and start editing", ImVec2(-1, 0))) {
                    NewLevel(g_name, kMaps[g_map].scene);
                    if (g_empty)
                        SetEmptyMap(true);
                    if (g_empty && g_blank) {
                        SetBlankMap(true);
                        if (kFloors[g_floor].mesh)
                            SetFloor(kFloors[g_floor].level, kFloors[g_floor].mesh, kFloors[g_floor].name);
                    }
                    std::string err;
                    if (!SaveLevel(&err))
                        g_message = "not saved: " + err;
                    StartOn(kMaps[g_map].scene);
                }
                ImGui::TextDisabled("saved to Mods\\<name>\\level.json");
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Open a level")) {
                if (g_saved.empty()) {
                    ImGui::TextDisabled("no levels in Mods\\ yet");
                }
                bool go = false;
                constexpr ImGuiTableFlags kFlags = ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg |
                                                   ImGuiTableFlags_BordersInnerV;
                if (!g_saved.empty() &&
                    ImGui::BeginTable("##saved", 3, kFlags, ImVec2(-1, ImGui::GetFontSize() * 14))) {
                    ImGui::TableSetupScrollFreeze(0, 1);
                    ImGui::TableSetupColumn("Level", ImGuiTableColumnFlags_WidthStretch);
                    ImGui::TableSetupColumn("Map", ImGuiTableColumnFlags_WidthStretch);
                    ImGui::TableSetupColumn("Last saved", ImGuiTableColumnFlags_WidthFixed,
                                            ImGui::CalcTextSize("0000-00-00 00:00").x);
                    ImGui::TableHeadersRow();
                    for (size_t i = 0; i < g_saved.size(); ++i) {  // the last saved first (ListLevels)
                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        char label[160];
                        snprintf(label, sizeof(label), "%s##%d", g_saved[i].name.c_str(), int(i));
                        if (ImGui::Selectable(label, int(i) == g_pick,
                                              ImGuiSelectableFlags_SpanAllColumns |
                                                  ImGuiSelectableFlags_AllowDoubleClick)) {
                            g_pick = int(i);
                            go = ImGui::IsMouseDoubleClicked(0);
                        }
                        ImGui::TableNextColumn();
                        ImGui::TextUnformatted(MapName(g_saved[i].scene));
                        ImGui::TableNextColumn();
                        ImGui::TextDisabled("%s", g_saved[i].modified.c_str());
                    }
                    ImGui::EndTable();
                }
                ImGui::BeginDisabled(g_pick < 0);
                float half = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
                bool play = ImGui::Button("Play", ImVec2(half, 0)) || go;
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                    ImGui::SetTooltip("Plays the level (double click too): the editor stays closed (%s opens it)",
                                      overlay::EditorKeyName().c_str());
                ImGui::SameLine();
                bool edit = ImGui::Button("Edit", ImVec2(-1, 0));
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                    ImGui::SetTooltip("Opens the level in the editor on its map");
                if ((play || edit) && g_pick >= 0) {
                    std::string err;
                    const Saved& s = g_saved[g_pick];
                    if (LoadLevel(s.folder, &err) && !s.scene.empty()) {
                        StartOn(s.scene);
                        g_editAfterStart = edit;
                    } else {
                        g_message = "not opened: " + err;
                    }
                }
                ImGui::EndDisabled();
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
        if (!g_message.empty())
            ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.4f, 1.0f), "%s", g_message.c_str());
    }
    ImGui::End();
    if (!open) {
        g_launcherOpen = false;
        overlay::SetExclusive(nullptr);
        overlay::SetOpen(false);
    }
}

void OpenLauncher()
{
    LoadSavedList();
    g_message.clear();
    g_launcherOpen = true;
    overlay::SetExclusive(DrawLauncher);
    overlay::SetOpen(true);
}

using OnButton_t = uint32_t(__fastcall*)(void* self, void* edx, int id);
OnButton_t oOnButton;

uint32_t __fastcall hkOnButton(void* self, void* edx, int id)
{
    if (id == kEntryId) {
        if (!g_launcherOpen)
            OpenLauncher();
        return 0;
    }
    return oOnButton(self, edx, id);
}

// The entry under Load: the game's own button, or drawn over the main menu
// when that could not be made.
void DrawEntry()
{
    int clicks = TakeGameClicks();  // taken every frame: only fresh ones count
    if (!InMainMenu() || !engine::SceneDataValid())
        return;
    engine::ZGEOM* load = Find("2Load");
    if (!load || !IsLive(load))
        return;
    if (!g_buttonTried) {
        g_buttonTried = true;
        g_button = MakeButton(load, Find("3Options"));
    }
    if (g_button && !IsLive(g_button))
        g_button = nullptr;
    Layout(load);
    if (g_button || overlay::IsOpen())
        return;
    Transform lt = GetTransform(load);
    ImVec2 at;
    float scale;
    if (!GuiToScreen(lt.pos[0], lt.pos[1] + kRow, at, scale))
        return;
    ImFont* font = overlay::MenuFont() ? overlay::MenuFont() : ImGui::GetFont();
    float size = kTextSize * scale;
    ImVec2 extent = font->CalcTextSizeA(size, FLT_MAX, 0.0f, kEntry);
    // the game's cursor: its mouse window follows it (GUI units)
    g_hovered = false;
    engine::ZGEOM* mouse = Find("MouseWindow", "ZWINDOWS/");
    const float* c = mouse && IsLive(mouse)
                         ? reinterpret_cast<const float*>(reinterpret_cast<const char*>(mouse) + kMouseWindow_Cursor)
                         : nullptr;
    if (c && engine::IsReadable(c, 8)) {
        ImVec2 cursor;
        float s;
        if (GuiToScreen(c[0], c[1], cursor, s))
            g_hovered = cursor.x >= at.x - 4 && cursor.x <= at.x + extent.x + 4 && cursor.y >= at.y - 2 &&
                        cursor.y <= at.y + extent.y + 2;
    }
    ImU32 color = g_hovered ? IM_COL32(10, 10, 10, 255) : IM_COL32(108, 127, 142, 255);
    ImGui::GetBackgroundDrawList()->AddText(font, size, at, color, kEntry);
    if (clicks > 0)
        Log("launcher: click (%s)", g_hovered ? "on the entry" : "elsewhere");
    if (clicks > 0 && g_hovered)
        OpenLauncher();
}

// the map was loaded from the launcher: the editor opens on it
void AfterStart()
{
    if (g_startScene.empty() || CurrentScene() != g_startScene || !engine::SceneDataValid())
        return;
    static int frames;
    if (++frames < 30)  // the level is applied first
        return;
    frames = 0;
    g_startScene.clear();
    if (!g_editAfterStart) {  // Play
        g_editAfterStart = true;
        return;
    }
    overlay::SetOpen(true);
    overlay::ShowPanel("Level");
}

} // namespace

void InstallLauncher()
{
    SetLocText(kEntryKey, kEntry);
    Hook("MainMenu::OnButton", kMainMenu_OnButton, &hkOnButton, &oOnButton);
    overlay::AddAlwaysDraw(DrawEntry);
    OnFrameEnd(AfterStart);
    OnScenePushed([](const std::string&) {
        g_button = nullptr;
        g_buttonTried = false;
        g_hovered = false;
        if (g_launcherOpen) {
            g_launcherOpen = false;
            overlay::SetExclusive(nullptr);
        }
    });
}

} // namespace h2::editor
