#pragma once

namespace h2::overlay {

// Lua console panel: output log and an input line with history.
bool* ConsoleVisible();
void DrawConsole();
// Give the input line keyboard focus on the next frame (overlay opened).
void FocusConsoleInput();

} // namespace h2::overlay
