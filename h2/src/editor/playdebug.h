#pragma once
// The debug view while a level is played (Try it; the routes key, [Editor] RoutesKey, F9): the routes the
// characters walk, what each of them does, how suspicious it is and whether it sees Hitman, how far and how
// wide each one sees, the objectives as they stand with the level's alarm level, and characters with a route
// standing still. What happened in a test is listed when it is over (back in the editor: a click takes the
// camera to where it happened).
#include <string>

namespace h2::editor {

void InstallPlayDebug();

// What the view shows (each can be switched off; the view itself with the routes key).
struct PlayDebugLayers {
    bool routes = true;  // the routes walked (npc_panel.cpp DrawPlayRoutes)
    bool doing = true;   // what each one does, its suspicion, whether it sees Hitman
    bool sight = true;   // how far and how wide each one sees
    bool goals = true;   // the objectives, the alarm level and the latest events in the corner
    bool stuck = true;   // a character with a route standing still
};
bool* PlayDebugShown();
PlayDebugLayers& PlayDebugView();
// The settings (the guide's Try it step, the Testing panel).
void DrawPlayDebugSettings();
// What happened in the last test (a click takes the camera there).
void DrawLastTest();
bool LastTestHasEvents();
// The same as text, a line an event ("1:05  Guard spotted Hitman"): dev commands.
std::string LastTestText();

// playtest.cpp: a test's map is loading / the test is over, the editor back.
void PlayDebugTestStarted();
void PlayDebugBackInEditor();

} // namespace h2::editor
