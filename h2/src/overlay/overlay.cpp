#include "overlay.h"

#include "../devcmd.h"

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
std::vector<void (*)()> g_always;
ImFont* g_menuFont;
void (*g_exclusive)();
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

// drawn now: visible and of the mode the editor is in
bool Shown(const Panel& p)
{
    return *p.visible && (p.modes & (g_config.simpleMode ? kSimple : kAdvanced));
}
std::string g_selectTab;      // left tab to select on the next frame
bool (*g_closeGuard)();       // SetCloseGuard
bool (*g_openGuard)();        // SetOpenGuard
std::vector<bool (*)(int)> g_keyHandlers;  // AddKeyHandler

constexpr ImGuiWindowFlags kDockedFlags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove |
                                          ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBringToFrontOnFocus;

bool IsInputMessage(UINT msg)
{
    return (msg >= WM_KEYFIRST && msg <= WM_KEYLAST) || (msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST);
}

LRESULT CALLBACK WndProc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_KEYDOWN && wp == static_cast<WPARAM>(g_config.editorKey) && !(lp & (1 << 30))) {
        if (g_open ? !g_closeGuard || g_closeGuard() : !g_openGuard || g_openGuard())
            SetOpen(!g_open);
        return 0;
    }
    if (!g_open && msg == WM_KEYDOWN && !(lp & (1 << 30)))
        for (auto handler : g_keyHandlers)
            if (handler(int(wp)))
                return 0;
    if (g_ready && g_open) {
        if (msg == WM_CHAR && g_config.logRender)
            Log("WM_CHAR %04X", unsigned(wp));
        if (ImGui_ImplWin32_WndProcHandler(wnd, msg, wp, lp))
            return 1;
        if (IsInputMessage(msg))
            return 0;  // the editor owns keyboard and mouse
    }
    // automated testing: the game pauses when its window loses focus, keep it running
    if (g_config.devCommands && ((msg == WM_ACTIVATEAPP && !wp) || (msg == WM_ACTIVATE && LOWORD(wp) == WA_INACTIVE) ||
                                 msg == WM_KILLFOCUS))
        return 0;
    // DirectInput runs non-exclusive (see dinput.cpp): keep the OS cursor off the game
    if (msg == WM_SETCURSOR && LOWORD(lp) == HTCLIENT) {
        SetCursor(nullptr);
        return TRUE;
    }
    return CallWindowProcA(g_gameWndProc, wnd, msg, wp, lp);
}

// Look of Hitman 2: Silent Assassin's menus: black and graphite panels,
// crimson accents, warm white text, square corners, a narrow typeface.
void ApplyTheme(float scale)
{
    ImGuiStyle& st = ImGui::GetStyle();
    st = ImGuiStyle();
    st.WindowRounding = st.ChildRounding = st.FrameRounding = st.PopupRounding = 0.0f;
    st.ScrollbarRounding = st.GrabRounding = st.TabRounding = 0.0f;
    st.WindowBorderSize = 1.0f;
    st.FrameBorderSize = 0.0f;
    st.TabBorderSize = 0.0f;
    st.WindowPadding = ImVec2(10, 10);
    st.FramePadding = ImVec2(8, 4);
    st.ItemSpacing = ImVec2(8, 6);
    st.ScrollbarSize = 12.0f;
    st.GrabMinSize = 10.0f;
    st.SeparatorTextBorderSize = 2.0f;

    auto rgb = [](int r, int g, int b, float a = 1.0f) { return ImVec4(r / 255.0f, g / 255.0f, b / 255.0f, a); };
    const ImVec4 red = rgb(168, 18, 28), redHover = rgb(200, 32, 42), redActive = rgb(226, 48, 56);
    const ImVec4 black = rgb(8, 8, 9), graphite = rgb(24, 24, 26), steel = rgb(40, 40, 44), line = rgb(58, 58, 62);
    ImVec4* c = st.Colors;
    c[ImGuiCol_Text] = rgb(232, 228, 220);
    c[ImGuiCol_TextDisabled] = rgb(138, 133, 128);
    c[ImGuiCol_WindowBg] = rgb(14, 14, 15, 0.94f);
    c[ImGuiCol_ChildBg] = rgb(0, 0, 0, 0.0f);
    c[ImGuiCol_PopupBg] = rgb(12, 12, 13, 0.98f);
    c[ImGuiCol_Border] = line;
    c[ImGuiCol_BorderShadow] = rgb(0, 0, 0, 0.0f);
    c[ImGuiCol_FrameBg] = graphite;
    c[ImGuiCol_FrameBgHovered] = steel;
    c[ImGuiCol_FrameBgActive] = rgb(60, 24, 26);
    c[ImGuiCol_TitleBg] = black;
    c[ImGuiCol_TitleBgActive] = rgb(40, 8, 10);
    c[ImGuiCol_TitleBgCollapsed] = black;
    c[ImGuiCol_MenuBarBg] = black;
    c[ImGuiCol_ScrollbarBg] = rgb(10, 10, 11, 0.6f);
    c[ImGuiCol_ScrollbarGrab] = steel;
    c[ImGuiCol_ScrollbarGrabHovered] = line;
    c[ImGuiCol_ScrollbarGrabActive] = red;
    c[ImGuiCol_CheckMark] = redActive;
    c[ImGuiCol_SliderGrab] = red;
    c[ImGuiCol_SliderGrabActive] = redActive;
    c[ImGuiCol_Button] = steel;
    c[ImGuiCol_ButtonHovered] = red;
    c[ImGuiCol_ButtonActive] = redActive;
    c[ImGuiCol_Header] = rgb(168, 18, 28, 0.55f);
    c[ImGuiCol_HeaderHovered] = rgb(200, 32, 42, 0.70f);
    c[ImGuiCol_HeaderActive] = redActive;
    c[ImGuiCol_Separator] = line;
    c[ImGuiCol_SeparatorHovered] = redHover;
    c[ImGuiCol_SeparatorActive] = redActive;
    c[ImGuiCol_ResizeGrip] = rgb(168, 18, 28, 0.25f);
    c[ImGuiCol_ResizeGripHovered] = redHover;
    c[ImGuiCol_ResizeGripActive] = redActive;
    c[ImGuiCol_Tab] = graphite;
    c[ImGuiCol_TabHovered] = redHover;
    c[ImGuiCol_TabSelected] = red;
    c[ImGuiCol_TabSelectedOverline] = redActive;
    c[ImGuiCol_TabDimmed] = graphite;
    c[ImGuiCol_TabDimmedSelected] = rgb(110, 16, 22);
    c[ImGuiCol_TableHeaderBg] = graphite;
    c[ImGuiCol_TableBorderStrong] = line;
    c[ImGuiCol_TableBorderLight] = steel;
    c[ImGuiCol_TableRowBg] = rgb(0, 0, 0, 0.0f);
    c[ImGuiCol_TableRowBgAlt] = rgb(255, 255, 255, 0.025f);
    c[ImGuiCol_TextSelectedBg] = rgb(168, 18, 28, 0.45f);
    c[ImGuiCol_NavCursor] = redActive;
    c[ImGuiCol_ModalWindowDimBg] = rgb(0, 0, 0, 0.6f);
    st.ScaleAllSizes(scale);

    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->Clear();
    const char* fonts[] = {"C:\\Windows\\Fonts\\bahnschrift.ttf", "C:\\Windows\\Fonts\\segoeui.ttf"};
    bool loaded = false;
    for (const char* f : fonts) {
        if (GetFileAttributesA(f) != INVALID_FILE_ATTRIBUTES && io.Fonts->AddFontFromFileTTF(f, 16.0f * scale)) {
            loaded = true;
            break;
        }
    }
    if (!loaded) {
        io.Fonts->AddFontDefault();
        io.FontGlobalScale = scale;
    }
    // the game's menus are set in Copperplate Gothic (if Windows has it)
    const char* copperplate = "C:\\Windows\\Fonts\\COPRGTB.TTF";
    g_menuFont = GetFileAttributesA(copperplate) != INVALID_FILE_ATTRIBUTES
                     ? io.Fonts->AddFontFromFileTTF(copperplate, 48.0f)
                     : nullptr;
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
            if (p.dock == dock && p.name && (p.modes & (g_config.simpleMode ? kSimple : kAdvanced)))
                ImGui::MenuItem(p.name, nullptr, p.visible);
    ImGui::Separator();
    if (ImGui::MenuItem("Simple mode", nullptr, g_config.simpleMode))
        SetSimpleMode(!g_config.simpleMode);
    ImGui::Separator();
    ImGui::MenuItem("Hide panels", "Tab", &g_panelsHidden);
    if (g_config.devCommands)  // a developer's tool
        ImGui::MenuItem("ImGui demo", nullptr, &g_showDemo);
}

// a virtual key's name as the keyboard shows it ("F8", "Pause", "Scroll Lock")
std::string KeyName(int vk)
{
    if (!vk)
        return "none";
    if (vk == VK_PAUSE)
        return "Pause";  // no scan code name for it
    char n[32];
    UINT sc = MapVirtualKeyA(UINT(vk), MAPVK_VK_TO_VSC);
    if (!sc || !GetKeyNameTextA(LONG(sc << 16), n, sizeof(n)))
        snprintf(n, sizeof(n), "key 0x%02X", vk);
    return n;
}

// Every key of the editor, by what is being done (Help menu and the F1 sheet).
void DrawKeys()
{
    std::string editor = KeyName(g_config.editorKey);
    struct Row {
        const char* key;
        const char* what;
    };
    auto section = [](const char* title, std::initializer_list<Row> rows) {
        ImGui::SeparatorText(title);
        if (!ImGui::BeginTable(title, 2, ImGuiTableFlags_SizingFixedFit))
            return;
        for (const Row& r : rows) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(r.key);
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%s", r.what);
        }
        ImGui::EndTable();
    };
    std::string freeze = KeyName(g_config.freezeKey), speed = KeyName(g_config.speedKey);
    section("Editor", {{editor.c_str(), "editor on / off, back to the game"},
                       {"F1", "this sheet"},
                       {"Tab", "hide / show the panels"},
                       {"`", "Lua console (in it: Tab completes a name, Up / Down earlier lines)"},
                       {"Ctrl+S", "save the level"},
                       {"Ctrl+Z / Ctrl+Y", "undo / redo"}});
    section("Camera", {{"right mouse", "look around (free camera)"},
                       {"  + W A S D", "forward, left, back, right"},
                       {"  + Q / E", "down / up"},
                       {"  + Shift / Ctrl", "fast / slow"},
                       {"  + wheel", "camera speed"}});
    section("Selection", {{"click", "select (an object, a character, a route point)"},
                          {"Esc", "nothing selected"},
                          {"W / E", "gizmo: move / rotate"},
                          {"Q", "gizmo: world / local axes"},
                          {"G", "grab: onto the surface under the mouse"},
                          {"H", "hide / show"},
                          {"F", "camera to it"},
                          {"Ctrl+D", "duplicate"},
                          {"Del", "delete"}});
    section("Placing and grabbing", {{"click", "place (on a surface)"},
                                     {"Shift+click", "place, and another one"},
                                     {"wheel", "turn"},
                                     {"Alt+wheel", "lift off the surface"},
                                     {"Ctrl", "on the grid"},
                                     {"N", "align to the surface / stand upright"},
                                     {"Esc", "cancel (also picking on the map)"}});
    section("While playing", {{freeze.c_str(), "world stands still / goes on (FreezeKey)"},
                              {speed.c_str(), "next game speed (SpeedKey)"}});
    ImGui::TextDisabled("Keys: H2Fix.ini [Editor]. Typing in a field: the keys go to the field.");
}

bool g_showKeys = false;

void HelpMenu()
{
    ImGui::MenuItem("Keys", "F1", &g_showKeys);
    ImGui::Separator();
    DrawKeys();
}

void DrawKeysWindow()
{
    if (!g_showKeys)
        return;
    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Appearing,
                            ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(0, 0), ImVec2(io.DisplaySize.x * 0.9f, io.DisplaySize.y * 0.9f));
    if (ImGui::Begin("Keys (F1)", &g_showKeys,
                     ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings))
        DrawKeys();
    ImGui::End();
}

void DrawMenuBar()
{
    if (!ImGui::BeginMainMenuBar())
        return;
    ImGui::TextColored(ImVec4(0.88f, 0.19f, 0.22f, 1.0f), "HITMAN 2");
    ImGui::SameLine(0, 4);
    ImGui::TextDisabled("EDITOR");
    ImGui::SameLine(0, 16);
    for (Menu& m : g_menus) {
        if (ImGui::BeginMenu(m.name.c_str())) {
            for (auto items : m.items)
                items();
            ImGui::EndMenu();
        }
    }
    ImGuiIO& io = ImGui::GetIO();
    char right[160];
    snprintf(right, sizeof(right), "%s   %.0f fps   F1: keys   %s: back to game",
             CurrentScene().empty() ? "no scene" : CurrentScene().c_str(), io.Framerate,
             KeyName(g_config.editorKey).c_str());
    ImGui::SameLine(ImGui::GetWindowWidth() - ImGui::CalcTextSize(right).x - ImGui::GetStyle().ItemSpacing.x * 2);
    ImGui::TextDisabled("%s", right);
    ImGui::EndMainMenuBar();
}

bool AnyVisible(Dock dock)
{
    for (Panel& p : g_panels)
        if (p.dock == dock && Shown(p))
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
        if (p.dock == Dock::Toolbar && Shown(p))
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
        ImGui::SetNextWindowSize(ImVec2(W * 0.26f, height), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSizeConstraints(ImVec2(ImGui::GetFontSize() * 22.0f, height), ImVec2(W * 0.45f, height));  // its tabs fit
        ImGui::Begin("Left panel", nullptr, kDockedFlags);
        leftW = ImGui::GetWindowWidth();
        if (ImGui::BeginTabBar("left")) {
            for (Panel& p : g_panels) {
                if (p.dock != Dock::Left || !Shown(p))
                    continue;
                ImGuiTabItemFlags flags = g_selectTab == p.name ? ImGuiTabItemFlags_SetSelected : 0;
                if (ImGui::BeginTabItem(p.name, nullptr, flags)) {
                    // the tab's own scroll region: the tabs stay at the top, whole (not narrowed by a scrollbar)
                    if (ImGui::BeginChild("content"))
                        p.draw();
                    ImGui::EndChild();
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
        ImGui::SetNextWindowSizeConstraints(ImVec2(std::max(220.0f, ImGui::GetFontSize() * 19.0f), height),
                                            ImVec2(W * 0.45f, height));  // its rows (camera, testing) fit
        ImGui::Begin("Right panel", nullptr, kDockedFlags);
        rightW = ImGui::GetWindowWidth();
        ImGui::SetWindowPos(ImVec2(W - rightW, top));
        bool first = true;
        for (Panel& p : g_panels) {
            if (p.dock != Dock::Right || !Shown(p))
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
            if (p.dock == Dock::Bottom && Shown(p))
                p.draw();
        ImGui::End();
    }
    for (Panel& p : g_panels)
        if ((p.dock == Dock::Floating && Shown(p)) || (p.dock == Dock::None && *p.visible))
            p.draw();
    if (g_showDemo)
        ImGui::ShowDemoWindow(&g_showDemo);
    DrawKeysWindow();
    if (!io.WantTextInput && ImGui::IsKeyPressed(ImGuiKey_F1, false))
        g_showKeys = !g_showKeys;

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

std::string EditorKeyName() { return KeyName(g_config.editorKey); }
std::string KeyNameOf(int vk) { return KeyName(vk); }
void* GameWindow() { return g_window; }

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
    // readable at high resolutions
    float scale = RenderHeight() > 0 ? RenderHeight() / 1080.0f : 1.0f;
    scale = scale < 1.0f ? 1.0f : scale;
    ApplyTheme(scale);

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

static void (*g_cleanFrame)(void*);  // RequestCleanFrame
static int g_cleanState;      // 1: this frame without the overlay, 2: then `g_cleanFrame`

void OnPresent(void* device)
{
    UpdateCursorClip(g_window);
    if (!g_ready || device != g_device)
        return;
    if (g_cleanState == 1) {
        g_cleanState = 2;
        return;  // (nothing drawn: the game's frame alone)
    }
    if (g_cleanState == 2) {
        g_cleanState = 0;
        if (g_cleanFrame)
            g_cleanFrame(device);
        g_cleanFrame = nullptr;
    }
    ImGui_ImplDX8_NewFrame();
    ImGui_ImplWin32_NewFrame();
    DevUiStep();
    ImGui::NewFrame();
    for (auto draw : g_always)
        draw();
    if (g_open) {
        if (g_exclusive)
            g_exclusive();
        else
            DrawLayout();
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

void AddAlwaysDraw(void (*draw)()) { g_always.push_back(draw); }

void RequestCleanFrame(void (*fn)(void* device))
{
    g_cleanFrame = fn;
    g_cleanState = 1;
}
ImFont* MenuFont() { return g_menuFont; }
void SetExclusive(void (*draw)()) { g_exclusive = draw; }

void SetCloseGuard(bool (*guard)()) { g_closeGuard = guard; }
void SetOpenGuard(bool (*guard)()) { g_openGuard = guard; }
void AddKeyHandler(bool (*handler)(int vk)) { g_keyHandlers.push_back(handler); }

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
    if (!panel.name) {
        g_panels.back().dock = Dock::None;
        g_panels.back().modes = kBothModes;
    }
}

bool SimpleMode() { return g_config.simpleMode; }

void SetSimpleMode(bool on, bool save)
{
    g_config.simpleMode = on;
    if (save)
        WritePrivateProfileStringA("Editor", "Mode", on ? "simple" : "advanced", (GameDir() + "H2Fix.ini").c_str());
    Log("editor: %s mode", on ? "simple" : "advanced");
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
