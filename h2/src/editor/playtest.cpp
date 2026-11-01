#include "playtest.h"

#include <algorithm>

#include <cmath>
#include <cstring>
#include <string>

#include "imgui.h"

#include "../config.h"
#include "../display.h"
#include "../engine/inject.h"
#include "../engine/messages.h"
#include "../game.h"
#include "../log.h"
#include "../mainloop.h"
#include "../overlay/overlay.h"
#include "../scene.h"
#include "freecam.h"
#include "highlight.h"
#include "level.h"
#include "model.h"
#include "npcs.h"

namespace h2::editor {

namespace {

constexpr uintptr_t kPauseScene = 0x41B890;  // ZEngineDataBase::PauseScene(bool)
constexpr size_t kEngine_Paused = 0xB9C;      // ZEngineDataBase::IsPaused
constexpr float kStartAbove = 20.0f;          // Hitman's group is a little above the floor

enum StartAt { kAtHitman, kAtCamera, kAtLevelStart };
const char* const kStartNames[] = {"where Hitman is", "where the camera looks", "at the level's start"};

bool g_freeze = true;     // the world stands still while the editor is open
bool g_playFrozen;        // ... and while playing (Testing)
int g_startAt = kAtHitman;
bool g_wePaused;          // the pause is the editor's (not the game's own)
int g_stepFrames;         // frames the world runs although the editor holds it
bool g_askClose;          // the close question is to be opened
bool g_picking;           // waiting for the camera's point
std::string g_message;
// a test plays (Reload): the editor key goes back to the editor, the map loaded again as edited (not as
// played: moved characters, the dead, Hitman's place), the camera where it was
bool g_testing;
double g_testStart;       // when it began (the hint line fades)
bool g_back;              // back to the editor: opened once the map has loaded
bool g_backLeft;          // ... the old map has gone
int g_backFrames;         // ... frames on the new map
bool g_camFree;           // the camera before the test
float g_camPos[3], g_camYaw, g_camPitch;

char* Engine()
{
    char* sys = reinterpret_cast<char*>(game::SysInterface());
    return sys ? *reinterpret_cast<char**>(sys + 0x98) : nullptr;
}

bool EnginePaused()
{
    char* e = Engine();
    return e && e[kEngine_Paused] != 0;
}

void PauseScene(bool on)
{
    if (char* e = Engine())
        reinterpret_cast<void(__thiscall*)(char*, bool)>(kPauseScene)(e, on);
}

bool OnMap()
{
    const std::string& s = CurrentScene();
    return s.size() > 6 && _stricmp(s.c_str() + s.size() - 6, "__MAIN") == 0 && engine::SceneDataValid();
}

void OnFrame()
{
    if (overlay::IsOpen())
        g_testing = false;  // (opened some other way: editing the played map)
    if (g_back && g_backLeft && OnMap() && ++g_backFrames > 60) {  // (the game's camera behind Hitman by then)
        g_back = false;
        overlay::SetOpen(true);
        if (g_camFree) {
            SetFreeCamEnabled(true);
            SetFreeCamPosition(g_camPos);
            SetFreeCamAngles(g_camYaw, g_camPitch);
        }
        Log("test: back in the editor");
    }
    bool want = OnMap() && ((g_freeze && overlay::IsOpen()) || g_playFrozen);
    if (want && g_stepFrames > 0) {
        --g_stepFrames;
        want = false;
    }
    if (want && !g_wePaused) {
        if (!EnginePaused()) {  // the game's own pause stays the game's
            PauseScene(true);
            g_wePaused = true;
        }
    } else if (!want && g_wePaused) {
        if (EnginePaused())
            PauseScene(false);
        g_wePaused = false;
    }
}

void OnLeaving(const std::string&)
{
    if (g_wePaused && EnginePaused())
        PauseScene(false);  // loading pauses and resumes the scene itself
    g_wePaused = false;
    // the next map runs a moment before the editor holds it: the game's
    // camera starts in Hitman's head and moves behind him only while it runs
    g_stepFrames = 60;
    if (g_back) {
        g_backLeft = true;
        g_backFrames = 0;
    }
}

void Reload()
{
    g_playFrozen = false;  // testing plays
    g_camFree = FreeCamEnabled();
    FreeCamPosition(g_camPos);
    FreeCamAngles(g_camYaw, g_camPitch);
    std::string err;
    if (!ReloadLevelMap(&err)) {
        g_message = "not tested: " + err;
        return;
    }
    g_message.clear();
    g_testing = true;
    g_testStart = GetTickCount64() / 1000.0;
    overlay::SetOpen(false);  // playing
}

// The editor key while a test plays: the map loaded again as the level makes it, then the editor
bool OpenGuard()
{
    if (!g_testing)
        return true;
    g_testing = false;
    g_back = true;  // (before the request: the old map is left inside it, OnLeaving)
    g_backLeft = false;
    std::string err;
    if (!ReloadLevelMap(&err)) {
        g_back = false;
        Log("test: not loaded again (%s), the editor opens on the played map", err.c_str());
        return true;
    }
    return false;
}

// while a test plays: how to get back (and the routes key), at the top of the screen; faint after a while
void DrawTestHint()
{
    if (overlay::IsOpen() || (!g_testing && !g_back))
        return;
    std::string text = g_back ? "Back to the editor: loading the map..."
                              : "Testing the level     " + overlay::EditorKeyName() + ": back to the editor" +
                                    (g_config.routesKey ? "     " + overlay::KeyNameOf(g_config.routesKey) +
                                                              ": the people's routes"
                                                        : "");
    float age = float(GetTickCount64() / 1000.0 - g_testStart);
    float alpha = g_back || age < 8.0f ? 1.0f : 0.45f;
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    ImVec2 size = ImGui::CalcTextSize(text.c_str());
    ImVec2 at((ImGui::GetIO().DisplaySize.x - size.x) * 0.5f, 6.0f);
    dl->AddRectFilled(ImVec2(at.x - 10, at.y - 3), ImVec2(at.x + size.x + 10, at.y + size.y + 3),
                      IM_COL32(0, 0, 0, int(150 * alpha)), 3.0f);
    dl->AddText(at, IM_COL32(255, 220, 120, int(255 * alpha)), text.c_str());
}

void StartAt(const float pos[3], const float mat[9])
{
    float p[3] = {pos[0], pos[1] + kStartAbove, pos[2]};
    engine::SetPlayerStart(CurrentLevel().baseScene, p, mat);
}

bool StartAtHitman()
{
    engine::ZGEOM* hitman = engine::Player();
    if (!hitman || !IsLive(hitman))
        return false;
    Transform t = GetTransform(hitman);
    StartAt(t.pos, t.mat);
    return true;
}

void OnCameraPoint(engine::ZGEOM*, const float* point, const float*)
{
    g_picking = false;
    if (point) {
        // facing away from the camera, level
        float f[3] = {0, 0, 1};
        if (game::ZCAMERA* cam = game::MainCamera()) {
            float m[9], c[3];
            game::GetWorldTransform(cam, m, c);
            float dx = point[0] - c[0], dz = point[2] - c[2], len = std::sqrt(dx * dx + dz * dz);
            if (len > 1.0f)
                f[0] = dx / len, f[2] = dz / len;
        }
        float mat[9] = {f[0], 0, f[2], 0, 1, 0, f[2], 0, -f[0]};  // rows forward, up, right
        StartAt(point, mat);
    } else if (!StartAtHitman()) {
        Log("test: no point where the camera looks, Hitman starts at the level's start");
    }
    Reload();
}

void Test()
{
    // another map comes (a level of another map opened): Hitman's or the camera's place here means
    // nothing there, the level's start is used (#16: he stood at C1-1's coordinates on C2-2)
    if (!CurrentLevel().baseScene.empty() && _stricmp(CurrentLevel().baseScene.c_str(), CurrentScene().c_str()) != 0) {
        Reload();
        return;
    }
    if (g_startAt == kAtCamera) {
        g_picking = true;
        RequestPick(RenderWidth() * 0.5f, RenderHeight() * 0.5f, OnCameraPoint);
        return;
    }
    if (g_startAt == kAtHitman)
        StartAtHitman();
    Reload();
}

bool CloseGuard()
{
    if (PendingNpcCount() == 0 || !OnMap())
        return true;
    g_askClose = true;
    return false;
}

// the question CloseGuard asks (drawn whatever panels are shown)
void DrawClosePrompt()
{
    if (!overlay::IsOpen())
        return;
    int pending = PendingNpcCount();
    if (g_askClose) {
        g_askClose = false;
        ImGui::OpenPopup("Test the level?");
    }
    if (ImGui::BeginPopupModal("Test the level?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("%d placed character%s made when the map loads again.", pending,
                    pending == 1 ? " is" : "s are");
        ImGui::TextDisabled("Until then %s no behaviour, nothing bumps into %s and %s cannot be shot.",
                            pending == 1 ? "it has" : "they have", pending == 1 ? "it" : "them",
                            pending == 1 ? "it" : "they");
        if (ImGui::Button("Test now (save and reload)")) {
            ImGui::CloseCurrentPopup();
            Test();
        }
        ImGui::SameLine();
        if (ImGui::Button("Close the editor")) {
            ImGui::CloseCurrentPopup();
            overlay::SetOpen(false);
        }
        ImGui::SameLine();
        if (ImGui::Button("Keep editing"))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

} // namespace

bool WorldFrozen() { return g_wePaused; }
bool WorldFrozenWhilePlaying() { return g_playFrozen; }
void SetWorldFrozenWhilePlaying(bool on) { g_playFrozen = on; }

void TestLevel() { Test(); }

void TestLevelFrom(TestStart start)
{
    g_startAt = start == TestStart::Camera ? kAtCamera : start == TestStart::LevelStart ? kAtLevelStart : kAtHitman;
    Test();
}

void DrawPlayTestControl()
{
    if (!OnMap())
        return;
    int pending = PendingNpcCount();
    ImGui::BeginDisabled(g_picking);
    if (ImGui::Button(pending ? "Test *" : "Test"))
        Test();
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Saves the level, loads the map again and plays it (Hitman starts %s).%s\n"
                          "The editor key while playing: back to the editor (the map loads again as edited).",
                          kStartNames[g_startAt],
                          pending ? "\n* characters placed since are made then" : "");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(ImGui::CalcTextSize(kStartNames[kAtCamera]).x + ImGui::GetFrameHeight() * 1.5f);
    ImGui::Combo("##startat", &g_startAt, kStartNames, IM_ARRAYSIZE(kStartNames));
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Where Hitman starts when you test");
    ImGui::SameLine();
    ImGui::Checkbox("Freeze", &g_freeze);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("While the editor is open the world stands still: characters do not act until you\n"
                          "test (or close the editor). Off: the game runs under the editor.");
    if (!g_message.empty()) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.4f, 1.0f), "%s", g_message.c_str());
    }
}

void InstallPlayTest()
{
    OnFrameEnd(OnFrame);
    OnSceneLeaving(OnLeaving);
    overlay::SetCloseGuard(CloseGuard);
    overlay::SetOpenGuard(OpenGuard);
    overlay::AddAlwaysDraw(DrawClosePrompt);
    overlay::AddAlwaysDraw(DrawTestHint);
}

bool ScenePaused() { return EnginePaused(); }
void StepWorld(int frames) { g_stepFrames = std::max(g_stepFrames, std::min(frames, 30)); }

} // namespace h2::editor
