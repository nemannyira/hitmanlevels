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
#include <vector>

namespace h2::engine {
struct ZGEOM;
}

namespace h2::editor {

void InstallMission();
// The scene zone (engine/inject.h) made for a goal's place: a "reach" line's
// (slot 1..6), a "bring it" line's (100 + slot) or the mission's exit (0).
std::string GoalZoneName(int code);
// A text the editor answers in the game's LOC tree (UTF-8), e.g. its main menu entry.
void SetLocText(const std::string& key, const std::string& text);
// The inspector's Mission row for the selected object: make it a target to kill,
// an object Hitman has to take, a place Hitman has to reach or the mission's
// exit (or which line it is already).
void DrawMissionForSelection(engine::ZGEOM* geom);
// An object's name of the level maker's (a character, an item, any object an objective can be made for):
// the objectives' texts made for it use it ("Kill the courier"). A character placed by the level keeps it
// as its label (h2.find); any other object in the mission's names (level.json mission.names). Lines whose
// text the editor made follow the new name. "" takes the name off. Undoable.
bool SetObjectName(engine::ZGEOM* geom, const std::string& name, std::string* error = nullptr);
std::string ObjectName(engine::ZGEOM* geom);  // "" none of its own
// A row to name it: the Inspector's Mission row, Simple mode's card.
void DrawObjectNameRow(engine::ZGEOM* geom);
// Simple mode's Mission step (the guide), and whether it is done (no blank left)
void DrawSimpleMission();
bool MissionStepDone();
// The same from code (dev commands): "target" | "take" | "reach" | "exit"; what was done or why not.
std::string MissionGoalFor(engine::ZGEOM* geom, const std::string& goal);
// A "bring it to a place" line's place; a new "never spotted" line (dev commands).
std::string MissionPlaceFor(int slot, engine::ZGEOM* place);
std::string MissionUnseen(bool failsMission);
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
// The health bar (0..100 percent): MSG_SETHITPOINTS to the OSD.
bool ShowHitPoints(int percent);
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
// The loaded scene's level routine (LevelControl: the mission's statistics counters, runs.h); null before one.
void* LevelRoutine();
// The level routine's own end, as the base level's scripts call it (testing
// the "base_ends" setting: not let through when it is off).
bool BaseLevelEnd(bool completed);
// The level's own text of a line ("" when the level has none or it is not known yet).
std::string LevelObjectiveText(int slot);

// The objectives of the stage as they stand while it is played (the play debug's list).
struct GoalStatus {
    int slot = 0;
    std::string text;        // the laptop's line (or what the goal is)
    int state = 0;           // 1 done, 0 not yet, -1 lost (can no longer be done)
    std::string why;         // lost: why; not yet: how far Hitman is ("12 m"), or ""
    engine::ZGEOM* where = nullptr;  // its target or place (the camera can go there)
};
std::vector<GoalStatus> GoalStatuses();

} // namespace h2::editor
