#pragma once
// Engine messages and character life.
//
// Messages are registered by name (ZEngineDataBase::RegisterZMsg 0x41EED0,
// the same name gives the same id) and sent to an object with ZGEOM vfunc
// 0x120 (from, id, param*): windows pass them to their routines (the laptop's
// briefing menu: SetObjective {slot, LOC key}, SetObjectiveCompleted {slot,
// done}), the level control object handles MissionCompleted / MissionFailed,
// characters' scripts get ScriptEvent with a message name (Msg_InstantDeath
// kills, as an explosion does).
#include <cstdint>

#include "geoms.h"

namespace h2::engine {

uint16_t RegisterMessage(const char* name);
bool SendMessage(ZGEOM* to, ZGEOM* from, uint16_t msg, void* param);
// Characters (ZActor): alive (vfunc 0x544), killing one (instant death).
bool IsAlive(ZGEOM* actor);
bool KillActor(ZGEOM* actor);
// Hitman (the player's character, class 0x2003E8), the level control object.
ZGEOM* Player();
ZGEOM* LevelControl();

} // namespace h2::engine
