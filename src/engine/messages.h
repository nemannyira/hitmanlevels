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
// Commands, as the scripts' Actor_If methods do them (call the actor's
// virtual functions the same way; the actor's own behaviour may give it
// other orders): MoveToPosition (Actor_If 8, 0x5842C0) walks to a point
// (on the level's path network; characters made while playing do not walk),
// LookAt (26, 0x584E20) keeps a target for the head, which turns to it at
// once (0x46F630) unless it is looking at something already (null: stops),
// UseMoveSet (7, 0x583F70) chooses how it moves ("Walk", "Run", ...).
bool ActorWalkTo(ZGEOM* actor, const float pos[3]);
bool ActorLookAt(ZGEOM* actor, ZGEOM* target);
bool ActorUseMoveSet(ZGEOM* actor, const char* name);
// PlayAnim / PlayAnimLoop (Link_If 0x587CC0 / 0x588110 -> 0x588B80): the
// animation by name from the character's sets (getAnimHandle 0x587A70 ->
// 0x588DF0: vfunc 0x2A0's set, then 0x29C's, set vfunc 0x18(name)), played
// with vfunc 0x214(anim, 1 | 5 looping, 0, -1.0, 1.0). False: no such
// animation for it.
bool ActorPlayAnimation(ZGEOM* actor, const char* name, bool loop);
// Hitman (the player's character, class 0x2003E8), the level control object.
ZGEOM* Player();
ZGEOM* LevelControl();

} // namespace h2::engine
