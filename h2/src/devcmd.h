#pragma once
// Developer automation (H2Fix.ini [Debug] DevCommands=1): the game runs
// Mods\_cmd.lua as soon as it appears (and deletes it), writing the results
// to Mods\_cmd.out; Lua gets h2.dev_* functions to load scenes, take
// screenshots and press keys / mouse buttons inside the game's DirectInput.
// Used to test builds without anybody at the keyboard.

namespace h2 {

void InstallDevCommands();

// Called by the DirectInput hooks: injected keys / buttons / mouse motion.
// `keys` is the 256 byte DIK state, `mouse` a DIMOUSESTATE.
void DevInjectKeyboard(unsigned char* keys);
void DevInjectMouse(long* dx, long* dy, unsigned char* buttons, long* wheel = nullptr);
bool DevInjecting();

// Called on Present: pending screenshot.
void DevOnPresent(void* device);
// Called before the editor's ImGui frame: injected editor mouse / key events.
void DevUiStep();

} // namespace h2
