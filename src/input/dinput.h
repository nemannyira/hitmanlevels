#pragma once
#include <windows.h>

namespace h2 {

// Wraps the game's DirectInput devices (we are its dinput8.dll):
//  - devices are forced to non-exclusive so window messages keep flowing to
//    the editor UI (ImGui) even while the game reads DirectInput;
//  - while input is captured the game reads "nothing pressed / no movement".
void HookDirectInput(void* directInput8);  // IDirectInput8A* from DirectInput8Create

void SetGameInputBlocked(bool blocked);
bool GameInputBlocked();

// Raw state of the game's devices as last read (valid whether blocked or not),
// for the editor's own controls (free camera).
struct MouseDelta {
    long dx, dy, wheel;
};
MouseDelta TakeMouseDelta();
// Presses of the left mouse button the game has seen since the last call
// (its own menus' clicks).
int TakeGameClicks();

} // namespace h2
