#pragma once
// Escalations (#23, h2/docs/escalation.md): a level played in stages (1 to 10), one after another,
// each the one before and more. The level document keeps every stage's things (LevelDocument:
// stages, rules; a StageSpan on its placed objects, characters, edits of the map's objects and
// objectives); the map is loaded for one stage: what is not there in it is not made with the map
// (CopiesFor, NpcsFor, ZonesFor, the in-game map's icons), not applied (ApplyLevel: an edited map
// object out of its span is hidden) and not checked (the objectives).
//
// The stage of a saved game is kept with it (the "__editor" part of Save\<slot>.h2lua) and set
// again when the game loads it (ZEngineDataBase::LoadGame 0x41AF70, before it pushes the map).
// The rules are checked by a Lua part run with the level's scripts (the game's events: spotted,
// kill, disguise; h2.time). The player's progress (the stages done, cleanly or not) is
// Save\escalations.json; the level's MissionCompleted (profile_guard) marks a stage done.
#include <string>

#include "level.h"

namespace h2::editor {

constexpr int kMaxStages = 10;

// The level has stages.
bool IsEscalation();
int StageCount();
// The stage the map is loaded / played for (1 for a plain level).
int CurrentStage();
// The stage the level's map is loaded for next (the stage bar, the launcher, a saved game's).
void SetCurrentStage(int stage);
// A thing of the level is there in the current stage / in `stage` (always in a plain level).
bool InStage(const StageSpan& span);
bool InStage(const StageSpan& span, int stage);
// What a thing placed now has: there from the current stage on.
StageSpan NewSpan();
// "stages 2-4", "from stage 3" ("" when in every stage).
std::string SpanText(const StageSpan& span);

// The laptop's briefing of the stage played: the stage's name and text and its rules before the
// level's briefing (`briefing`; the level's own when that is empty and the stage has nothing).
std::string StageBriefing(const std::string& briefing);
// Runs the stage's rules (with the level's scripts, owned by them).
void RunStageRules();
// The level was loaded (LoadLevel): its first stage, or the one asked for.
void OnLevelLoaded();
// The rules' state of a saved game ("" when the level has no stages; mission.cpp's saved goals).
int StageOfSavedGame(const std::string& extra);

// The player's progress of an escalation (Mods folder): the highest stage done (0: none), the
// stages done cleanly (no rule only noted as broken).
int StagesDone(const std::string& folder);
bool StageDoneCleanly(const std::string& folder, int stage);
// The stages the player can start: the next open one, and the ones done when the level allows it.
bool StagePlayable(const std::string& folder, int stage, int count, bool replay);

// Editor: the toolbar's stage bar, the Mission tab's Stages section, an Inspector row.
void DrawStageBar();
void DrawStagesSection();
// A thing's stages (from / gone from): true when changed.
bool DrawStageSpan(StageSpan& span, const char* id);

void InstallStages();

} // namespace h2::editor
