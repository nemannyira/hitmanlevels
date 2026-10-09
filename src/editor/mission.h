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
// A text the editor answers in the game's LOC tree (UTF-8), e.g. its main menu entry.
void SetLocText(const std::string& key, const std::string& text);
// Picking a target / place in the world for an objective: the viewport hands the mouse over.
bool MissionPicking();
void UpdateMissionPick();
// The level was applied to / taken back from the loaded scene.
void ApplyMission();
void RevertMission();

// Runtime (also for Lua): a line's text (UTF-8; "" = the level's own), its check mark.
bool SetObjectiveText(int slot, const std::string& text);
bool SetObjectiveDone(int slot, bool done);
// Hides a line (a level's own too) until a script gives it a text again.
bool HideObjectiveLine(int slot);
// A subtitle at the bottom of the screen (UTF-8; "" takes it away), as the
// game's speech shows them: MSG_WRITESUBTITLE to the GUI's OSD window, the
// param being the text (GUI main 0x534130 -> 0x561D50).
bool ShowSubtitle(const std::string& text);
// The game's info box (UTF-8, one paragraph): MSG_DISPLAYINFO / WARNING /
// HINT to the OSD (GUI main 0x534130 -> 0x561EA0.. -> its InfoDisplay
// routine, ZWINGROUP_InfoDisplay vtable 0x658D08 +0xC0 / C4 / C8 = 0x53C3C0..).
// The param is a LOC key (looked up by 0x53C8D0): the editor's own
// "H2Editor/Info/<n>" with the text. Not shown while Hitman's vfunc 0x63C
// says so (the GUI checks it).
enum class InfoKind { Info, Warning, Hint };
bool ShowInfo(InfoKind kind, const std::string& text);
// The laptop's mission briefing (UTF-8, "\n" new line; "" = as the level has
// it): a script's text, over the level document's until the map loads again.
bool SetBriefingText(const std::string& text);
bool CompleteMission();
bool FailMission();
// The level's own text of a line ("" when the level has none or it is not known yet).
std::string LevelObjectiveText(int slot);

} // namespace h2::editor
