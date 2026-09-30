#include "overlay.h"

#include <cstdio>
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
struct Menu {
    std::string name;
    std::vector<void (*)()> items;
};
std::vector<Menu> g_menus;
bool g_showDemo = false;
bool g_panelsHidden = false;  // Tab: game view only (toolbar stays)
std::string g_selectTab;      // left tab to select on the next frame

constexpr ImGuiWindowFlags kDockedFlags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove |
                                          ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBringToFrontOnFocus;

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

Menu& MenuNamed(const char* name)
{
    for (Menu& m : g_menus)
        if (m.name == name)
            return m;
    g_menus.push_back({name, {}});
    return g_menus.back();
}

void ViewMenu()
{
    for (Dock dock : {Dock::Left, Dock::Right, Dock::Bottom, Dock::Floating, Dock::Toolbar})
        for (Panel& p : g_panels)
            if (p.dock == dock && p.name)
                ImGui::MenuItem(p.name, nullptr, p.visible);
    ImGui::Separator();
    ImGui::MenuItem("Hide panels", "Tab", &g_panelsHidden);
    ImGui::MenuItem("ImGui demo", nullptr, &g_showDemo);
}

void HelpMenu()
{
    ImGui::TextDisabled("F8           editor on / off");
    ImGui::TextDisabled("Tab          hide / show the panels");
    ImGui::TextDisabled("`            Lua console");
    ImGui::Separator();
    ImGui::TextDisabled("click        select");
    ImGui::TextDisabled("W / E / Q    move / rotate / local-world");
    ImGui::TextDisabled("H  F         hide  focus camera");
    ImGui::TextDisabled("Ctrl+D  Del  duplicate  delete");
    ImGui::TextDisabled("Ctrl+Z / Y   undo / redo");
    ImGui::Separator();
    ImGui::TextDisabled("right mouse  look (free camera)");
    ImGui::TextDisabled("  + WASD QE  move, Shift fast, wheel speed");
}

void DrawMenuBar()
{
    if (!ImGui::BeginMainMenuBar())
        return;
    for (Menu& m : g_menus) {
        if (ImGui::BeginMenu(m.name.c_str())) {
            for (auto items : m.items)
                items();
            ImGui::EndMenu();
        }
    }
    ImGuiIO& io = ImGui::GetIO();
    char right[160];
    snprintf(right, sizeof(right), "%s   %.0f fps   F8: back to game",
             CurrentScene().empty() ? "no scene" : CurrentScene().c_str(), io.Framerate);
    ImGui::SameLine(ImGui::GetWindowWidth() - ImGui::CalcTextSize(right).x - ImGui::GetStyle().ItemSpacing.x * 2);
    ImGui::TextDisabled("%s", right);
    ImGui::EndMainMenuBar();
}

bool AnyVisible(Dock dock)
{
    for (Panel& p : g_panels)
        if (p.dock == dock && *p.visible)
            return true;
    return false;
}

// Toolbar strip under the menu; returns its bottom edge.
float DrawToolbar(float top)
{
    if (!AnyVisible(Dock::Toolbar))
        return top;
    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(0, top));
    ImGui::SetNextWindowSize(ImVec2(io.DisplaySize.x, 0));
    ImGui::Begin("##toolbar", nullptr,
                 kDockedFlags | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoScrollbar |
                     ImGuiWindowFlags_NoSavedSettings);
    for (Panel& p : g_panels)
        if (p.dock == Dock::Toolbar && *p.visible)
            p.draw();
    float bottom = top + ImGui::GetWindowHeight();
    ImGui::End();
    return bottom;
}

void DrawLayout()
{
    ImGuiIO& io = ImGui::GetIO();
    float W = io.DisplaySize.x, H = io.DisplaySize.y;
    DrawMenuBar();
    float top = DrawToolbar(ImGui::GetFrameHeight());
    float height = H - top;
    float leftW = 0, rightW = 0;
    if (!g_panelsHidden && AnyVisible(Dock::Left)) {
        ImGui::SetNextWindowPos(ImVec2(0, top));
        ImGui::SetNextWindowSize(ImVec2(W * 0.2f, height), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSizeConstraints(ImVec2(200, height), ImVec2(W * 0.45f, height));
        ImGui::Begin("Left panel", nullptr, kDockedFlags);
        leftW = ImGui::GetWindowWidth();
        if (ImGui::BeginTabBar("left")) {
            for (Panel& p : g_panels) {
                if (p.dock != Dock::Left || !*p.visible)
                    continue;
                ImGuiTabItemFlags flags = g_selectTab == p.name ? ImGuiTabItemFlags_SetSelected : 0;
                if (ImGui::BeginTabItem(p.name, nullptr, flags)) {
                    p.draw();
                    ImGui::EndTabItem();
                }
            }
            g_selectTab.clear();
            ImGui::EndTabBar();
        }
        ImGui::End();
    }
    if (!g_panelsHidden && AnyVisible(Dock::Right)) {
        float w = W * 0.2f;
        ImGui::SetNextWindowSize(ImVec2(w, height), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSizeConstraints(ImVec2(220, height), ImVec2(W * 0.45f, height));
        ImGui::Begin("Right panel", nullptr, kDockedFlags);
        rightW = ImGui::GetWindowWidth();
        ImGui::SetWindowPos(ImVec2(W - rightW, top));
        bool first = true;
        for (Panel& p : g_panels) {
            if (p.dock != Dock::Right || !*p.visible)
                continue;
            if (!first)
                ImGui::Spacing();
            first = false;
            ImGui::PushID(p.name);
            if (ImGui::CollapsingHeader(p.name, ImGuiTreeNodeFlags_DefaultOpen))
                p.draw();
            ImGui::PopID();
        }
        ImGui::End();
    }
    if (!g_panelsHidden && AnyVisible(Dock::Bottom)) {
        float w = W - leftW - rightW;
        ImGui::SetNextWindowSize(ImVec2(w, H * 0.25f), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSizeConstraints(ImVec2(w, 80), ImVec2(w, H * 0.7f));
        ImGui::Begin("Bottom panel", nullptr, kDockedFlags);
        ImGui::SetWindowPos(ImVec2(leftW, H - ImGui::GetWindowHeight()));
        for (Panel& p : g_panels)
            if (p.dock == Dock::Bottom && *p.visible)
                p.draw();
        ImGui::End();
    }
    for (Panel& p : g_panels)
        if ((p.dock == Dock::Floating && *p.visible) || p.dock == Dock::None)
            if (*p.visible)
                p.draw();
    if (g_showDemo)
        ImGui::ShowDemoWindow(&g_showDemo);

    if (!io.WantCaptureKeyboard && ImGui::IsKeyPressed(ImGuiKey_Tab, false) && !io.KeyCtrl)
        g_panelsHidden = !g_panelsHidden;
    if (!io.WantTextInput && ImGui::IsKeyPressed(ImGuiKey_GraveAccent, false)) {
        *ConsoleVisible() = !*ConsoleVisible();
        if (*ConsoleVisible()) {
            g_panelsHidden = false;
            FocusConsoleInput();
        }
    }
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
    AddPanel({"Lua console", ConsoleVisible(), DrawConsole, Dock::Bottom});
    AddMenu("View", ViewMenu);
    AddMenu("Help", HelpMenu);
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
    if (g_open)
        DrawLayout();
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
    if (g_ready)
        ImGui::GetIO().MouseDrawCursor = open;
}

void AddPanel(const Panel& panel)
{
    g_panels.push_back(panel);
    if (!panel.name)
        g_panels.back().dock = Dock::None;
}

void ShowPanel(const char* name)
{
    for (Panel& p : g_panels) {
        if (p.name && std::string(p.name) == name) {
            *p.visible = true;
            g_panelsHidden = false;
            if (p.dock == Dock::Left)
                g_selectTab = name;
        }
    }
}

void AddMenu(const char* menu, void (*items)())
{
    // keep the conventional order whoever registers first
    if (g_menus.empty())
        for (const char* m : {"File", "Edit", "View", "Help"})
            g_menus.push_back({m, {}});
    MenuNamed(menu).items.push_back(items);
}

} // namespace h2::overlay
