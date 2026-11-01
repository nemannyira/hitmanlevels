#pragma once
#include <string>
#include <windows.h>

struct ImFont;

namespace h2::overlay {

// Editor overlay (Dear ImGui on the game's D3D8 device).
void OnDeviceCreated(void* device, HWND window);
// Called right before IDirect3DDevice8::Present: draws the overlay.
void OnPresent(void* device);

// References the overlay holds on `device` (its font texture), and releasing
// them once the game has let go of the device.
unsigned long DeviceRefsHeld(void* device);
void OnDeviceReleased();

bool IsOpen();
void SetOpen(bool open);
// Asked when the editor key would close the editor: false keeps it open
// (e.g. the guard asks the user first).
void SetCloseGuard(bool (*guard)());
// Asked when the editor key would open the editor: false keeps it closed
// (the guard opens it its own way, e.g. after loading the map again).
void SetOpenGuard(bool (*guard)());
// Keys pressed while the editor is closed (not repeats), asked in the order
// added: true takes the key from the game (the later ones do not get it).
void AddKeyHandler(bool (*handler)(int vk));

// Fixed editor layout:
//   main menu bar (File / Edit / View / Help)
//   toolbar                              (Dock::Toolbar panels, one row each)
//   left sidebar: tabs | game view | right sidebar: sections
//                      | bottom     |               (Dock::Bottom, e.g. console)
// Docked panels draw only their contents; the layout owns the windows.
// Floating panels open their own window, Dock::None ones draw no window
// (gizmos, markers). A panel without a name is always Dock::None.
enum class Dock { None, Floating, Toolbar, Left, Right, Bottom };

// The editor's two faces (docs/simple-mode.md): a panel is drawn in the modes it is for.
constexpr int kAdvanced = 1, kSimple = 2, kBothModes = kAdvanced | kSimple;

struct Panel {
    const char* name;
    bool* visible;
    void (*draw)();
    Dock dock = Dock::Floating;
    int modes = kAdvanced;  // (a panel without a name: both)
};
void AddPanel(const Panel& panel);

// Simple mode: the guide instead of the tabs and sections; H2Fix.ini [Editor] Mode=simple|advanced
// (none: simple), saved when switched.
bool SimpleMode();
void SetSimpleMode(bool on, bool save = true);

// Brings a panel forward (its left tab selected, made visible).
void ShowPanel(const char* name);
// The key that opens and closes the editor, by its name ("F8").
std::string EditorKeyName();
// The game's window (HWND; null before the overlay starts).
void* GameWindow();
// A key's name ("F9"; "none" for 0).
std::string KeyNameOf(int vk);

// Items of a main menu ("File", "Edit", "View", "Help"): `items` is called
// inside the open menu. Menus appear in the order of their first registration.
void AddMenu(const char* menu, void (*items)());
// Drawn every frame, also while the editor is closed (e.g. the main menu's entry).
void AddAlwaysDraw(void (*draw)());
// While set and the editor is open, only this window is drawn (the launcher).
void SetExclusive(void (*draw)());
// The next presented frame without the overlay; at the frame after it `fn` gets the device (its front
// buffer holds that frame: a picture of the game's view, e.g. a package's preview).
void RequestCleanFrame(void (*fn)(void* device));
// The game's menu font (Copperplate Gothic Bold, loaded at 48 px), null when missing.
ImFont* MenuFont();

} // namespace h2::overlay
