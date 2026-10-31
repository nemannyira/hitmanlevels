#pragma once
// Editing and testing a level.
//
// Edit mode: while the editor is open on a map, the world stands still (the
// engine's own pause, ZEngineDataBase::PauseScene: scripts and characters
// stop, the camera and the editor work), so placed characters and the
// level's own ones are alike: they act when the level is tested.
//
// Test: the level is saved and its map loaded again (the characters placed
// since are made with the scene then, see npcs.h CreateNpc), Hitman starting
// where he is, where the camera looks or at the level's start, and the
// editor closes. Closing the editor with characters not made yet asks first.
namespace h2::editor {

void InstallPlayTest();
// Toolbar: Test, where Hitman starts, Freeze.
void DrawPlayTestControl();
// Test (the toolbar's button): saves, loads the map again, plays.
void TestLevel();
// Test with Hitman starting there (Simple mode's guide: the level's start, the camera's point)
enum class TestStart { Hitman, Camera, LevelStart };
void TestLevelFrom(TestStart start);
// Whether the editor holds the world still now.
bool WorldFrozen();
// The world also stands still with the editor closed (testtools.h).
bool WorldFrozenWhilePlaying();
// The engine's pause is on (the editor's, the Freeze toggle's, the game's own).
bool ScenePaused();
// While the editor holds the world still: let it run `frames` frames (a
// character moved meanwhile is only drawn in its new room once its own
// update has run; the paused engine does not run it).
void StepWorld(int frames);
void SetWorldFrozenWhilePlaying(bool on);

} // namespace h2::editor
