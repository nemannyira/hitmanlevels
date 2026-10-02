#pragma once
// Mission editor: the objectives (lines of the laptop's mission objectives),
// goals the editor checks itself (kill a character, reach a place) and the
// mission's end. Kept in the level document (LevelDocument::mission).
//
// The laptop's briefing menu (a routine of the LaptopBriefing window) shows
// up to 6 objectives: their texts are LOC keys (<level>/Laptop/<n>, set when
// the level starts: SetLocaleFolder 0x52D340), SetObjective 0x52E2E0 sets a
// line's key, SetObjectiveCompleted 0x52E350 its check mark. The texts are
// found in the LOC tree by 0x428FF0 (path, root): the editor answers its own
// texts there. The level control object ends the mission (MissionCompleted).
#include <string>

namespace h2::editor {

void InstallMission();
// Picking a target / place in the world for an objective: the viewport hands the mouse over.
bool MissionPicking();
void UpdateMissionPick();
// The level was applied to / taken back from the loaded scene.
void ApplyMission();
void RevertMission();

// Runtime (also for Lua): a line's text (UTF-8; "" = the level's own), its check mark.
bool SetObjectiveText(int slot, const std::string& text);
bool SetObjectiveDone(int slot, bool done);
bool CompleteMission();
bool FailMission();
// The level's own text of a line ("" when the level has none or it is not known yet).
std::string LevelObjectiveText(int slot);

} // namespace h2::editor
