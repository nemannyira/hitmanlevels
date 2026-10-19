#pragma once
// Engine messages and character life.
//
// Messages are registered by name (ZEngineDataBase::RegisterZMsg 0x41EED0,
// the same name gives the same id) and sent with the sender's ZGEOM vfunc
// 0x120 (receiver, id, param*): windows pass them to their routines (the laptop's
// briefing menu: SetObjective {slot, LOC key}, SetObjectiveCompleted {slot,
// done}), the level control object handles MissionCompleted / MissionFailed,
// characters' scripts get ScriptEvent with a message name (Msg_InstantDeath
// kills, as an explosion does).
#include <cstdint>

#include "geoms.h"

namespace h2::engine {

uint16_t RegisterMessage(const char* name);
bool SendMessage(ZGEOM* to, ZGEOM* from, uint16_t msg, void* param);

// An action Hitman can use on an object, as a door gets "Open door" while
// playing (0x488A70: the object's ZGEOM_Action routine found or attached
// (vfunc 0xE8 / 0xF4), its entry set (routine vfunc 0xAC: texts (copied), kind,
// message, receiver, distance), started (vfunc 0x50)). Kind 6, the scenes'
// "use" (Open, PlaceBomb, OpenSafe, EscapeFerrari...): used (vfunc 0x8C, within
// the distance of the object's box: vfunc 0x84, +0x80; the entry gets it as the
// scenes' data load gives it, 0x488A70 leaves it 0), it sends `msg` to the receiver (here the object
// itself, the user as param) and nothing else. Saved with the game (its texts
// as strings, its fields; its load sets the entry again).
bool AddUseAction(ZGEOM* geom, const char* text, uint16_t msg, int distance);
// Characters (ZActor): alive (vfunc 0x544; Hitman, ZHITMAN2: hit points
// +0xD72 above 0), killing one (its behaviour gets
// Msg_InstantDeath; without a behaviour: Actor_If's Die, vfunc 0x5F0).
bool IsAlive(ZGEOM* actor);
bool KillActor(ZGEOM* actor);
// Commands, as the scripts' Actor_If methods do them (call the actor's
// virtual functions the same way; the actor's own behaviour may give it
// other orders): MoveToPosition (Actor_If 8, 0x5842C0) walks to a point
// (of the level's path network: Actor_MoveTo 0x463DA0 asks the scene's
// rEnemyPathFinder3 (+0x513, set in PostInit 0x462E80); a point off it is
// not reached),
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
// Whether a point (where a character stands) is on the level's path network,
// as the pathfinder (ZPathfinder3, class 0x200075; its network at +0x64)
// takes it: a cell of it near the point (0x59A8E0 with the point twice; y
// lifted as RequestPath 0x599930 does). 1 yes, 0 no, -1 no network.
int OnPathNetwork(const float pos[3]);
// The network is a grid of cells over the map (world axes here; the nav
// object keeps min / max corners at +5 / +0x11 and the cells along x, z, y
// at +0x3B / +0x3F / +0x43). A character's point is looked up `lift` higher.
struct PathGrid {
    float min[3], max[3], size[3];
    int cells[3];
    float lift;
};
bool PathNetworkGrid(PathGrid& grid);
// The walkable cell the pathfinder finds for a point: within one cell across
// and 10 below it (0x59B610); `floor`: where a character on it stands (the
// cell's centre, less the lift).
bool PathCellBelow(const PathGrid& grid, const float pos[3], float floor[3]);
// The nearest point of the network to `pos` on about its floor (1.5 m below
// to 0.6 m above), at most `radius` away across; `pos` itself (its height
// kept) when it is on the network. False: none that near, or no network.
bool NearestWalkable(const float pos[3], float radius, float out[3]);
// Hitman takes the clothes of a character (a body, as when he takes them from
// one) or puts on a clothes item: MSG_CHANGECLOTHES to himself (ZHITMAN2
// handler 0x5142BC, id at +0xF90), the param pointing at {geom, 0} as he
// sends it himself (0x513E81).
bool HitmanChangeClothes(ZGEOM* source);
// An object the scene names (its com values: "rOSD", "rEnemyPathFinder3"...).
ZGEOM* SceneComGeom(const char* name);
// Hitman (the player's character, class 0x2003E8), the level control object.
ZGEOM* Player();
ZGEOM* LevelControl();

} // namespace h2::engine
