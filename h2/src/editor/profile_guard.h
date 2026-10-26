#pragma once
// A level of the editor does not count in the game's profile (P15).
//
// The profile (Save\<n>.blap: the scenes completed, each mission's rating
// and statistics, bonus weapons) is kept in the global variables of
// CGlobalCom (the static object at 0x6A40EC: vtable, capacity, data, used
// size, then the variables in a buffer of its own). Completing a mission
// changes them (strLastCompletedScene / strHighestCompletedScene: the
// level's MissionCompleted 0x5058B0; the statistics record 0x552F70) and the
// statistics screen writes the file (0x551420, called from 0x55C8A0).
//
// While a level of the editor is on the map the file is not written, and
// when the map is left the variables are put back as they were when the
// level came on it. The statistics screen does not show the bonus weapon the
// rating chose (0x55CF00 sets its +0xC0; its show 0x55CC00 opens the box).
namespace h2::editor {

void InstallProfileGuard();
// The level was put on the loaded map (its variables noted, once a map).
void ProtectProfile();
// Whether the profile is kept from the level now.
bool ProfileProtected();

} // namespace h2::editor
