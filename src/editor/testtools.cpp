#include "testtools.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>

#include "imgui.h"

#include "../config.h"
#include "../engine/scene_data.h"
#include "../game.h"
#include "../log.h"
#include "../mainloop.h"
#include "../overlay/overlay.h"
#include "../scene.h"
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

bool OnMap()
{
    const std::string& s = CurrentScene();
    return s.size() > 6 && _stricmp(s.c_str() + s.size() - 6, "__MAIN") == 0 && engine::SceneDataValid();
}

void WriteFloat(uintptr_t at, float v)
{
    __try {
        if (*reinterpret_cast<float*>(at) != v)
            *reinterpret_cast<float*>(at) = v;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

// every frame: the values hold on a map (the game's own cheats would change them back)
void OnFrame()
{
    bool map = OnMap();
    char* sys = reinterpret_cast<char*>(game::SysInterface());
    if (sys)
        WriteFloat(reinterpret_cast<uintptr_t>(sys + kSys_TimeScale), map ? g_speed : 1.0f);
    WriteFloat(kGodMode, map && g_god ? 1.0f : 0.0f);
    WriteFloat(kInvisible, map && g_invisible ? 1.0f : 0.0f);
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
    if (ImGui::Checkbox("Frozen", &frozen))
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
    char keys[96] = "";
    auto keyName = [](int vk) {
        static char n[32];
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
void SetGameSpeed(float factor) { g_speed = std::clamp(factor, 0.1f, 8.0f); }
bool GodMode() { return g_god; }
void SetGodMode(bool on) { g_god = on; }
bool HitmanInvisible() { return g_invisible; }
void SetHitmanInvisible(bool on) { g_invisible = on; }

void InstallTestTools()
{
    overlay::AddPanel({"Testing", &g_visible, DrawPanel, overlay::Dock::Right});
    overlay::AddAlwaysDraw(DrawIndicator);
    overlay::SetKeyHandler(OnKey);
    OnFrameEnd(OnFrame);
}

} // namespace h2::editor
