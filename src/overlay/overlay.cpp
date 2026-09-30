#include "overlay.h"

#include <string>
#include <vector>

#include "imgui.h"
#include "backends/imgui_impl_win32.h"

#include "../config.h"
#include "../d3d8types.h"
#include "../display.h"
#include "../input/dinput.h"
#include "../log.h"
#include "../scene.h"
#include "../script/lua_vm.h"
#include "../ui.h"
#include "console.h"
#include "imgui_impl_dx8.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace h2::overlay {

namespace {

HWND g_window;
void* g_device;  // device the DX8 backend is bound to (its font texture holds a reference)
WNDPROC g_gameWndProc;
bool g_ready;
bool g_open;
std::string g_iniPath;
std::vector<Panel> g_panels;
bool g_showStats = true;
bool g_showDemo = false;

bool IsInputMessage(UINT msg)
{
    return (msg >= WM_KEYFIRST && msg <= WM_KEYLAST) || (msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST);
}

LRESULT CALLBACK WndProc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_KEYDOWN && wp == static_cast<WPARAM>(g_config.editorKey) && !(lp & (1 << 30))) {
        SetOpen(!g_open);
        return 0;
    }
    if (g_ready && g_open) {
        if (msg == WM_CHAR && g_config.logRender)
            Log("WM_CHAR %04X", unsigned(wp));
        if (ImGui_ImplWin32_WndProcHandler(wnd, msg, wp, lp))
            return 1;
        if (IsInputMessage(msg))
            return 0;  // the editor owns keyboard and mouse
    }
    // DirectInput runs non-exclusive (see dinput.cpp): keep the OS cursor off the game
    if (msg == WM_SETCURSOR && LOWORD(lp) == HTCLIENT) {
        SetCursor(nullptr);
        return TRUE;
    }
    return CallWindowProcA(g_gameWndProc, wnd, msg, wp, lp);
}

void DrawStats()
{
    ImGui::SetNextWindowPos(ImVec2(10, 40), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Stats", &g_showStats, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::End();
        return;
    }
    ImGuiIO& io = ImGui::GetIO();
    ImGui::Text("%.1f fps (%.2f ms)", io.Framerate, 1000.0f / io.Framerate);
    ImGui::Text("scene: %s", CurrentScene().c_str());
    ImGui::Text("render: %d x %d, UI scale %.2f", RenderWidth(), RenderHeight(), UiScale());
    ImGui::End();
}

void DrawMenuBar()
{
    if (!ImGui::BeginMainMenuBar())
        return;
    ImGui::TextDisabled("H2 Editor");
    if (ImGui::BeginMenu("Windows")) {
        for (Panel& p : g_panels)
            ImGui::MenuItem(p.name, nullptr, p.visible);
        ImGui::MenuItem("Stats", nullptr, &g_showStats);
        ImGui::Separator();
        ImGui::MenuItem("ImGui demo", nullptr, &g_showDemo);
        ImGui::EndMenu();
    }
    ImGui::SameLine(ImGui::GetWindowWidth() - 220);
    ImGui::TextDisabled("F8: back to game");
    ImGui::EndMainMenuBar();
}

} // namespace

void OnDeviceCreated(void* device, HWND window)
{
    if (!window)
        return;
    if (g_ready) {  // a new device after the previous one was released
        if (!g_device) {
            ImGui_ImplDX8_Init(device);
            g_device = device;
        }
        return;
    }
    g_window = window;
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    g_iniPath = GameDir() + "H2Editor.ini";
    io.IniFilename = g_iniPath.c_str();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui::StyleColorsDark();
    // readable at high resolutions
    float scale = RenderHeight() > 0 ? RenderHeight() / 1080.0f : 1.0f;
    scale = scale < 1.0f ? 1.0f : scale;
    ImGui::GetStyle().ScaleAllSizes(scale);
    io.FontGlobalScale = scale;

    ImGui_ImplWin32_Init(window);
    ImGui_ImplDX8_Init(device);
    g_device = device;
    g_gameWndProc = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrA(window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&WndProc)));
    AddPanel({"Lua console", ConsoleVisible(), DrawConsole});
    g_ready = true;
    Log("overlay ready (toggle key %02X), UI scale %.3f", g_config.editorKey, UiScale());
    script::ExecFile(GameDir() + "Mods\\autoexec.lua");
}

void OnPresent(void* device)
{
    UpdateCursorClip(g_window);
    if (!g_ready || device != g_device)
        return;
    ImGui_ImplDX8_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    if (g_open) {
        DrawMenuBar();
        for (Panel& p : g_panels)
            if (*p.visible)
                p.draw();
        if (g_showStats)
            DrawStats();
        if (g_showDemo)
            ImGui::ShowDemoWindow(&g_showDemo);
    }
    ImGui::Render();
    ImDrawData* dd = ImGui::GetDrawData();
    if (dd->CmdListsCount > 0 && d3d8::Call<HRESULT>(device, d3d8::Dev_BeginScene) >= 0) {
        ImGui_ImplDX8_RenderDrawData(dd);
        d3d8::Call<HRESULT>(device, d3d8::Dev_EndScene);
    }
}

unsigned long DeviceRefsHeld(void* device)
{
    return device == g_device && ImGui_ImplDX8_HasDeviceObjects() ? 1 : 0;
}

void OnDeviceReleased()
{
    if (!g_device)
        return;
    g_device = nullptr;
    ImGui_ImplDX8_Shutdown();
}

bool IsOpen() { return g_open; }

void SetOpen(bool open)
{
    g_open = open;
    SetGameInputBlocked(open);
    if (open)
        FocusConsoleInput();
    if (g_ready)
        ImGui::GetIO().MouseDrawCursor = open;
}

void AddPanel(const Panel& panel) { g_panels.push_back(panel); }

} // namespace h2::overlay
