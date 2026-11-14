#pragma once
// A play of a downloaded level (Mods\Online\<id>) written down at its end: the play statistics' run
// (docs/play_stats.md section 1, #50). Nothing is sent from here: the file stays on the PC
// (levelpack sends it, if the player turned that on).
//
// Mods\Online\<id>\runs\<started>.json (canonical JSON): the level's id and version, the DLL's version,
// the result (completed / failed / left: the map left, the editor opened), the time and the counters of
// the game's statistics screen, the rating (completed), the testing aids and dev settings it ran with,
// the escalation's stage, the alarm level's highest, the events (objective done / lost, Hitman spotted,
// Hitman died: game time and place).
//
// The counters are the game's own (h2/docs/roadmap.md "The mission statistics"): LevelControl
// +0x208..+0x220 and +0x249, Hitman +0x11C7 / +0x11CB, the time SysInterface +0x38 >> 10, as the level
// routine's MissionCompleted (0x5058B0) puts them in the statistics block; a completed run takes the
// block, the scores and the rating as the statistics window has them (its handler 0x55C590).
namespace h2::editor {

void InstallRuns();

} // namespace h2::editor
