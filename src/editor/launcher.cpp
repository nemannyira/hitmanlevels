#include "launcher.h"

#include <windows.h>
#include <cfloat>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "imgui.h"
#include "json.hpp"

#include "../display.h"
#include "../engine/scene_data.h"
#include "../game.h"
#include "../input/dinput.h"
#include "../log.h"
#include "../mainloop.h"
#include "../overlay/overlay.h"
#include "../scene.h"
#include "level.h"
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
const char* const kEntry = "Level editor";

// main menu entry
bool g_layoutDone;
bool g_hovered;

// launcher
bool g_launcherOpen;
int g_map = 1;
char g_name[64] = "My level";
bool g_empty = true;
struct Saved {
    std::string folder, name, scene;
};
std::vector<Saved> g_saved;
int g_pick = -1;
std::string g_message;

// after the map is loaded: open the editor
std::string g_startScene;

bool InMainMenu() { return _stricmp(CurrentScene().c_str(), "MainMenu") == 0; }

// The game's GUI cursor: the mouse window keeps its position at +0x11C / +0x120 (GUI units).
constexpr size_t kMouseWindow_Cursor = 0x11C;

// an object of the main menu (several windows have objects of the same name)
engine::ZGEOM* Find(const char* name, const char* within = "MainMenu/")
{
    const auto& snap = SceneModel(0);
    for (int i : snap.FindName(name))
        if (snap.geoms[i].path.find(within) != std::string::npos)
            return snap.geoms[i].geom;
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

// the buttons under Load move a line down: the entry takes their place
void Layout()
{
    engine::ZGEOM* load = Find("2Load");
    if (!load || !IsLive(load))
        return;
    for (const char* name : {"3Options", "4Credits", "6Quit"}) {
        engine::ZGEOM* b = Find(name);
        if (!b || !IsLive(b))
            continue;
        Transform t = GetTransform(b);
        t.pos[1] += kRow;
        SetTransform(b, t);
    }
    g_layoutDone = true;
}

void LoadSavedList()
{
    g_saved.clear();
    for (const std::string& folder : ListLevels()) {
        Saved s;
        s.folder = folder;
        s.name = folder;
        std::ifstream f(GameDir() + "Mods\\" + folder + "\\level.json", std::ios::binary);
        try {
            nlohmann::json j = nlohmann::json::parse(f);
            s.name = j.value("name", folder);
            s.scene = j.value("base_scene", "");
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
                                   "the music, then save (File > Save level).");
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
                ImGui::Spacing();
                if (ImGui::Button("Create and start editing", ImVec2(-1, 0))) {
                    NewLevel(g_name, kMaps[g_map].scene);
                    if (g_empty)
                        SetEmptyMap(true);
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
                if (!g_saved.empty() && ImGui::BeginListBox("##saved", ImVec2(-1, ImGui::GetFontSize() * 14))) {
                    for (size_t i = 0; i < g_saved.size(); ++i) {
                        char label[256];
                        snprintf(label, sizeof(label), "%s  -  %s##%d", g_saved[i].name.c_str(),
                                 MapName(g_saved[i].scene), int(i));
                        if (ImGui::Selectable(label, int(i) == g_pick, ImGuiSelectableFlags_AllowDoubleClick)) {
                            g_pick = int(i);
                            go = ImGui::IsMouseDoubleClicked(0);
                        }
                    }
                    ImGui::EndListBox();
                }
                ImGui::BeginDisabled(g_pick < 0);
                if ((ImGui::Button("Open", ImVec2(-1, 0)) || go) && g_pick >= 0) {
                    std::string err;
                    const Saved& s = g_saved[g_pick];
                    if (LoadLevel(s.folder, &err) && !s.scene.empty())
                        StartOn(s.scene);
                    else
                        g_message = "not opened: " + err;
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

// the entry, drawn over the main menu while the editor is closed
void DrawEntry()
{
    if (!InMainMenu() || !engine::SceneDataValid()) {
        TakeGameClicks();
        return;
    }
    if (!g_layoutDone)
        Layout();
    if (!g_layoutDone || overlay::IsOpen()) {
        TakeGameClicks();
        return;
    }
    engine::ZGEOM* load = Find("2Load");
    if (!load || !IsLive(load))
        return;
    Transform lt = GetTransform(load);  // the menu positions its buttons when it starts
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
    int clicks = TakeGameClicks();
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
    overlay::SetOpen(true);
    overlay::ShowPanel("Level");
}

} // namespace

void InstallLauncher()
{
    overlay::AddAlwaysDraw(DrawEntry);
    OnFrameEnd(AfterStart);
    OnScenePushed([](const std::string&) {
        g_layoutDone = false;
        g_hovered = false;
        if (g_launcherOpen) {
            g_launcherOpen = false;
            overlay::SetExclusive(nullptr);
        }
    });
}

} // namespace h2::editor
