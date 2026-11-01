#include "testtools.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_set>
#include <vector>

#include "imgui.h"

#include "../config.h"
#include "../engine/messages.h"
#include "../engine/scene_data.h"
#include "../game.h"
#include "../log.h"
#include "../mainloop.h"
#include "../overlay/overlay.h"
#include "../scene.h"
#include "freecam.h"
#include "guides.h"
#include "npc_panel.h"
#include "playdebug.h"
#include "playtest.h"

namespace h2::editor {

namespace {

constexpr size_t kSys_TimeScale = 0x193C;     // float
constexpr uintptr_t kGodMode = 0x6A960C;      // float, the GodMode cheat
constexpr uintptr_t kInvisible = 0x6AA05C;    // float, CheckVisible's debug value
constexpr float kSpeeds[] = {0.25f, 0.5f, 1.0f, 2.0f, 4.0f};

bool g_visible = true;
float g_speed = 1.0f;
bool g_god, g_invisible;
unsigned g_aids;  // used on this map: 1 god mode, 2 invisible Hitman, 4 game speed
std::string g_endNote;  // the mission completed with them: shown over the statistics

bool OnMap()
{
    const std::string& s = CurrentScene();
    return s.size() > 6 && _stricmp(s.c_str() + s.size() - 6, "__MAIN") == 0 && engine::SceneDataValid();
}

// The path network around where the camera looks: columns of the network's grid are
// looked up from above the camera downwards (the pathfinder's own lookup:
// the walkable cell within 10 below), a few milliseconds a frame; the cells
// found are drawn as dots. A new scan starts when the camera has moved away.
constexpr float kPathRadius = 2500.0f;   // cm around the scan's centre
constexpr float kPathSpacing = 50.0f;    // cm between the columns looked at, at least
constexpr float kPathAbove = 300.0f;     // the height range (from the camera: one lookup
constexpr float kPathBelow = 4000.0f;    //   covers 11 cells, ~2.75 m on C1-1, ~2 us)
constexpr size_t kPathMaxPoints = 40000;
constexpr double kPathBudgetMs = 2.0;

using Point3 = std::array<float, 3>;

struct PathScan {
    bool active = false;
    engine::PathGrid grid{};
    float center[3] = {};          // the camera when it started
    int cell[2] = {};              // its column (x, z)
    int stride = 1;                // cells between the columns looked at
    std::vector<std::array<int, 2>> columns;  // offsets, nearest first
    size_t next = 0;               // the column looked at
    float y = NAN;                 // the next height in it (NaN: from the top)
    std::vector<Point3> points;
    std::unordered_set<uint64_t> seen;
};

bool g_showPaths;
PathScan g_scan;
std::vector<Point3> g_pathPoints;  // the last whole scan
float g_pathCenter[3];
bool g_pathShown;                  // g_pathPoints is a scan of this map
int g_pathState;                   // 0 nothing yet, 1 shown, -1 the map has no network
std::string g_pathScene;           // the map and grid the points are of
engine::PathGrid g_pathGrid;

bool SameGrid(const engine::PathGrid& a, const engine::PathGrid& b)
{
    return memcmp(a.min, b.min, sizeof(a.min)) == 0 && memcmp(a.cells, b.cells, sizeof(a.cells)) == 0;
}

void StartPathScan(const engine::PathGrid& grid, const float cam[3])
{
    PathScan& s = g_scan;
    s.active = true;
    s.grid = grid;
    memcpy(s.center, cam, sizeof(s.center));
    s.cell[0] = int(std::floor((cam[0] - grid.min[0]) / grid.size[0]));
    s.cell[1] = int(std::floor((cam[2] - grid.min[2]) / grid.size[2]));
    float cellSize = std::min(grid.size[0], grid.size[2]);
    s.stride = std::max(1, int(std::ceil(kPathSpacing / cellSize)));
    int half = std::min(200, int(std::ceil(kPathRadius / (s.stride * cellSize))));
    s.columns.clear();
    for (int z = -half; z <= half; ++z)
        for (int x = -half; x <= half; ++x)
            if (x * x + z * z <= half * half)
                s.columns.push_back({x, z});
    std::sort(s.columns.begin(), s.columns.end(), [](const auto& a, const auto& b) {
        return a[0] * a[0] + a[1] * a[1] < b[0] * b[0] + b[1] * b[1];
    });
    s.next = 0;
    s.y = NAN;
    s.points.clear();
    s.seen.clear();
}

// Looks at columns until the frame's time is used up; true when done.
bool ContinuePathScan()
{
    PathScan& s = g_scan;
    auto start = std::chrono::steady_clock::now();
    const engine::PathGrid& g = s.grid;
    const float top = s.center[1] + kPathAbove, bottom = s.center[1] - kPathBelow;
    int n = 0;
    while (s.next < s.columns.size()) {
        if ((++n & 15) == 0 &&
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count() > kPathBudgetMs)
            return false;
        const auto& c = s.columns[s.next];
        float p[3] = {g.min[0] + (float(s.cell[0] + c[0] * s.stride) + 0.5f) * g.size[0], std::isnan(s.y) ? top : s.y,
                      g.min[2] + (float(s.cell[1] + c[1] * s.stride) + 0.5f) * g.size[2]};
        float floor[3];
        if (engine::PathCellBelow(g, p, floor)) {
            uint64_t key = 0;
            for (int i = 0; i < 3; ++i)
                key = key << 16 | uint16_t((floor[i] + (i == 1 ? g.lift : 0.0f) - g.min[i]) / g.size[i]);
            if (s.points.size() < kPathMaxPoints && s.seen.insert(key).second)
                s.points.push_back({floor[0], floor[1], floor[2]});
            s.y = std::min(p[1], floor[1]) - g.size[1];  // on below it (another floor)
        } else {
            s.y = p[1] - 11.0f * g.size[1];  // the lookup went 10 cells down
        }
        if (s.y < bottom) {
            ++s.next;
            s.y = NAN;
        }
    }
    return true;
}

// The point of the network the free camera looks at (along its view, at
// most 60 m; the camera itself when it looks at none or is not free).
void ViewFocus(const engine::PathGrid& grid, const float cam[3], float out[3])
{
    memcpy(out, cam, 3 * sizeof(float));
    if (!FreeCamEnabled())
        return;
    float yaw, pitch;
    FreeCamAngles(yaw, pitch);
    if (pitch > -3.0f)
        return;  // looking along the ground or up: around the camera
    const float r = 3.14159265f / 180.0f;
    float d[3] = {std::sin(yaw * r) * std::cos(pitch * r), std::sin(pitch * r), std::cos(yaw * r) * std::cos(pitch * r)};
    for (float s = 100.0f; s <= 6000.0f; s += 100.0f) {
        float p[3] = {cam[0] + d[0] * s, cam[1] + d[1] * s, cam[2] + d[2] * s};
        float floor[3];
        // a cell of the network at most 2.5 m under the ray
        if (engine::PathCellBelow(grid, p, floor) && floor[1] <= p[1] + 1.0f && floor[1] >= p[1] - 250.0f) {
            memcpy(out, floor, sizeof(floor));
            return;
        }
    }
}

void UpdatePathView()
{
    if (g_showPaths && OnMap() && !overlay::IsOpen())
        return;  // kept for when the editor opens again
    if (!g_showPaths || !OnMap()) {
        g_scan = PathScan();
        g_pathPoints.clear();
        g_pathShown = false;
        g_pathState = 0;
        return;
    }
    engine::PathGrid grid;
    if (!engine::PathNetworkGrid(grid)) {
        g_scan = PathScan();
        g_pathPoints.clear();
        g_pathShown = false;
        g_pathState = -1;
        return;
    }
    float cam[3];
    FreeCamPosition(cam);
    // another map: what was found is not of it
    if (CurrentScene() != g_pathScene || !SameGrid(grid, g_pathGrid)) {
        g_pathScene = CurrentScene();
        g_pathGrid = grid;
        g_pathPoints.clear();
        g_pathShown = false;
        g_scan = PathScan();
    }
    // around where the camera looks (x, z), heights from the camera's
    float focus[3];
    ViewFocus(grid, cam, focus);
    focus[1] = cam[1];
    const float* from = g_scan.active ? g_scan.center : g_pathCenter;
    float dx = focus[0] - from[0], dz = focus[2] - from[2], dy = focus[1] - from[1];
    bool moved = dx * dx + dz * dz > (kPathRadius * 0.25f) * (kPathRadius * 0.25f) || std::fabs(dy) > 250.0f;
    if ((!g_scan.active && (!g_pathShown || moved)) || (g_scan.active && moved))
        StartPathScan(grid, focus);
    if (g_scan.active && ContinuePathScan()) {
        g_pathPoints.swap(g_scan.points);
        memcpy(g_pathCenter, g_scan.center, sizeof(g_pathCenter));
        g_pathShown = true;
        g_pathState = 1;
        g_scan = PathScan();
    }
}

void DrawPathView()
{
    // with the editor open only: drawn over the game, its menus and laptop too
    if (!g_showPaths || !OnMap() || !overlay::IsOpen())
        return;
    // the last whole scan; the first one while it goes
    const std::vector<Point3>& points = g_pathShown ? g_pathPoints : g_scan.points;
    if (points.empty())
        return;
    // fading out towards the edge of the scan (around where the camera looked)
    const float* c = g_pathShown ? g_pathCenter : g_scan.center;
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    const float r2 = kPathRadius * kPathRadius * 1.1f;
    for (const Point3& p : points) {
        float dx = p[0] - c[0], dz = p[2] - c[2];
        float d2 = dx * dx + dz * dz;
        if (d2 > r2)
            continue;
        ImVec2 s;
        if (!WorldToScreen(p.data(), s))
            continue;
        float close = 1.0f - std::sqrt(d2 / r2);
        int alpha = int(110 + 145 * close);
        float h = 1.5f + 1.5f * close;
        dl->AddRectFilled(ImVec2(s.x - h, s.y - h), ImVec2(s.x + h, s.y + h), IM_COL32(80, 230, 120, alpha));
    }
}

void WriteFloat(uintptr_t at, float v)
{
    __try {
        if (*reinterpret_cast<float*>(at) != v)
            *reinterpret_cast<float*>(at) = v;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

// The values are written when the editor's setting or the map changes, so
// the game's own cheats (EnableCheats: GodMode) still work in between.
void OnFrame()
{
    static bool wasMap;
    static float lastSpeed = 1.0f;
    static bool lastGod, lastInvisible;
    bool map = OnMap();
    bool changed = map != wasMap;
    wasMap = map;
    if (changed) {
        g_aids = 0;  // (a map loaded: a new run)
        g_endNote.clear();
    }
    if (map)
        g_aids |= (g_god ? 1u : 0u) | (g_invisible ? 2u : 0u) | (g_speed != 1.0f ? 4u : 0u);
    float speed = map ? g_speed : 1.0f;
    bool god = map && g_god, invisible = map && g_invisible;
    char* sys = reinterpret_cast<char*>(game::SysInterface());
    if (sys && (changed || speed != lastSpeed))
        WriteFloat(reinterpret_cast<uintptr_t>(sys + kSys_TimeScale), speed);
    if (changed || god != lastGod)
        WriteFloat(kGodMode, god ? 1.0f : 0.0f);
    if (changed || invisible != lastInvisible)
        WriteFloat(kInvisible, invisible ? 1.0f : 0.0f);
    lastSpeed = speed;
    lastGod = god;
    lastInvisible = invisible;
    UpdatePathView();
}

std::string ActiveText()
{
    std::string s;
    auto add = [&](const std::string& t) { s += (s.empty() ? "" : "  |  ") + t; };
    if (WorldFrozenWhilePlaying())
        add("FROZEN");
    if (g_speed != 1.0f) {
        char b[32];
        snprintf(b, sizeof(b), "SPEED x%g", g_speed);
        add(b);
    }
    if (g_god)
        add("GOD MODE");
    if (g_invisible)
        add("INVISIBLE");
    if (!g_endNote.empty())
        add(g_endNote);
    return s;
}

// while playing: what is on, in a corner (testing should not be mistaken for the game)
void DrawIndicator()
{
    if (overlay::IsOpen() || !OnMap())
        return;
    std::string s = ActiveText();
    if (s.empty())
        return;
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    ImVec2 size = ImGui::CalcTextSize(s.c_str());
    ImVec2 p((ImGui::GetIO().DisplaySize.x - size.x) * 0.5f, 10);  // top centre: the HUD is in the corners
    dl->AddRectFilled(ImVec2(p.x - 6, p.y - 4), ImVec2(p.x + size.x + 6, p.y + size.y + 4), IM_COL32(0, 0, 0, 160), 4);
    dl->AddText(p, IM_COL32(255, 210, 90, 255), s.c_str());
}

void NextSpeed()
{
    size_t i = 0;
    while (i < std::size(kSpeeds) && kSpeeds[i] <= g_speed + 1e-3f)
        ++i;
    SetGameSpeed(i < std::size(kSpeeds) ? kSpeeds[i] : kSpeeds[0]);
}

// keys while the editor is closed
bool OnKey(int vk)
{
    if (!OnMap())
        return false;
    if (vk == g_config.freezeKey && vk) {
        SetWorldFrozenWhilePlaying(!WorldFrozenWhilePlaying());
        return true;
    }
    if (vk == g_config.speedKey && vk) {
        NextSpeed();
        return true;
    }
    return false;
}

void DrawPanel()
{
    ImGui::TextDisabled("Trying out (not saved)");
    ImGui::SetNextItemWidth(-ImGui::CalcTextSize("speed").x - ImGui::GetStyle().ItemSpacing.x * 2);
    float speed = g_speed;
    if (ImGui::SliderFloat("speed", &speed, 0.1f, 8.0f, "x%.2f", ImGuiSliderFlags_Logarithmic))
        SetGameSpeed(speed);
    for (float s : kSpeeds) {
        char label[16];
        snprintf(label, sizeof(label), "x%g", s);
        if (ImGui::SmallButton(label))
            SetGameSpeed(s);
        ImGui::SameLine();
    }
    ImGui::NewLine();
    bool frozen = WorldFrozenWhilePlaying();
    if (ImGui::Checkbox("Freeze while playing", &frozen))
        SetWorldFrozenWhilePlaying(frozen);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("The world stands still also with the editor closed (Hitman too).%s",
                          g_config.freezeKey ? "\nWhile playing: the freeze key toggles it." : "");
    bool god = g_god;
    if (ImGui::Checkbox("God mode", &god))
        SetGodMode(god);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Hitman cannot be hurt");
    bool invisible = g_invisible;
    if (ImGui::Checkbox("Invisible Hitman", &invisible))
        SetHitmanInvisible(invisible);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Characters do not see anyone: they still hear shots and noise,\n"
                          "and bump into Hitman.");
    DrawPlayDebugSettings();
    ImGui::Checkbox("Path network", &g_showPaths);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Green dots (editor open) where characters can walk (npc:walkto, h2.onpath):\n"
                          "the map's path network, %.0f m around where the camera looks,\n"
                          "from %.0f m above the camera to %.0f m below.",
                          kPathRadius / 100.0f, kPathAbove / 100.0f, kPathBelow / 100.0f);
    if (g_showPaths) {
        ImGui::SameLine();
        if (!OnMap())
            ImGui::TextDisabled("(on a map)");
        else if (g_pathState < 0)
            ImGui::TextDisabled("this map has none");
        else if (g_scan.active && !g_pathShown)
            ImGui::TextDisabled("looking %d%%", int(100 * g_scan.next / std::max<size_t>(1, g_scan.columns.size())));
        else
            ImGui::TextDisabled("%d cells, %.0f cm", int(g_pathPoints.size()), g_pathGrid.size[0]);
    }
    char keys[96] = "";
    auto keyName = [](int vk) {
        static char n[32];
        if (vk == VK_PAUSE)
            return std::string("Pause");  // no scan code name for it
        UINT sc = MapVirtualKeyA(UINT(vk), MAPVK_VK_TO_VSC);
        if (!sc || !GetKeyNameTextA(LONG(sc << 16), n, sizeof(n)))
            snprintf(n, sizeof(n), "key 0x%02X", vk);
        return std::string(n);
    };
    if (g_config.freezeKey)
        snprintf(keys, sizeof(keys), "freeze: %s", keyName(g_config.freezeKey).c_str());
    if (g_config.speedKey)
        snprintf(keys + strlen(keys), sizeof(keys) - strlen(keys), "%sspeed: %s", *keys ? ", " : "",
                 keyName(g_config.speedKey).c_str());
    if (*keys)
        ImGui::TextDisabled("keys: %s", keys);
}

} // namespace

float GameSpeed() { return g_speed; }
void SetGameSpeed(float factor) { g_speed = std::isfinite(factor) ? std::clamp(factor, 0.1f, 8.0f) : 1.0f; }
bool GodMode() { return g_god; }
void SetGodMode(bool on) { g_god = on; }
bool HitmanInvisible() { return g_invisible; }
void NoteMissionEnd()
{
    std::string aids = AidsUsed();
    g_endNote = aids.empty() ? "" : "TESTED WITH " + aids + ": NOT A REAL RATING";
    for (char& c : g_endNote)
        c = char(toupper(static_cast<unsigned char>(c)));
}

std::string AidsUsed()
{
    std::string s;
    for (auto [bit, name] : {std::pair<unsigned, const char*>{1, "god mode"}, {2, "invisible Hitman"}, {4, "game speed"}})
        if (g_aids & bit)
            s += (s.empty() ? "" : ", ") + std::string(name);
    return s;
}
void SetHitmanInvisible(bool on) { g_invisible = on; }

void InstallTestTools()
{
    overlay::AddPanel({"Testing", &g_visible, DrawPanel, overlay::Dock::Right});
    overlay::AddAlwaysDraw(DrawIndicator);
    overlay::AddAlwaysDraw(DrawPathView);
    overlay::AddKeyHandler(OnKey);
    OnFrameEnd(OnFrame);
}

} // namespace h2::editor
