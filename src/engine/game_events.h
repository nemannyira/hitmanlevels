#pragma once
// The game's events for level scripts.
//
// Every message goes through ZGEOM::SendMessage (vfunc 0x120 = 0x43CBE0, the
// same function in every geom class): `this` sends (receiver, msg id,
// param*); the receiver's HandleMessage (vfunc 0x15C) hands it to its script
// by name and vfunc 0x10C to its routines. Names: engine +0xC37 + id*4
// (GetZMsgName 0x41EEB0), ids 1..engine +0x1C43.
//
// The level routine (LevelControl) gets a killed / harmed character with its
// kind (vfunc 0xB4 / 0xB8 (victim, kind): 2 enemy, 1 innocent, as the
// statistics 0x5058B0 count them; 0 not counted), called by the level's
// scripts (LevelControl_if 0x56BEB0) or its CharacterKilled message (kind
// from vfunc 0xB0); the end of a mission vfunc 0xC4 (completed) / 0xC0
// (failed).
// Hitman's damage: ZHITMAN2 0x519390 (hitman, float amount), hit points at
// +0xD72. A body dragged: ZTRACKLNKHM2 vfunc 0x658 = 0x50A750 (dragger ref,
// kept at +0x762) and 0x65C = 0x50A820 (let go), as MSG_DRAGSTART / DRAGEND
// call them.
//
// Scripts also message each other by name (the script engine's delivery
// 0x5E13A0 (instance, name, param*), ScriptEvent unpacked: the alarm, Hitman
// spotted...): kept as messages without a sender.
//
// Only the messages asked for are kept (WatchMessages); everything is
// queued and taken after the frame (TakeGameEvents): scripts never run inside
// the engine's dispatch.
#include <cstdint>
#include <set>
#include <string>
#include <vector>

#include "geoms.h"

namespace h2::engine {

struct GameEvent {
    enum Kind { Message, Kill, Harm, MissionCompleted, MissionFailed, HitmanDamage, Drag } kind = Message;
    std::string name;        // Message: the message's name
    ZGEOM* to = nullptr;     // Message: receiver; Kill / Harm: the level object; Drag: the body
    ZGEOM* from = nullptr;   // Message: sender
    uintptr_t param = 0;     // Message: the param pointer itself (some messages pass a value: 0 / 1)
    bool hasValue = false;   // the dword the param points to was readable
    uint32_t value = 0;      //   (Kill / Harm: the victim, a geom ref or pointer)
    uint32_t words[4] = {};  // the first dwords the param points to (words[0] = value)
    int wordCount = 0;
    std::string text;        // a text the param (or what it points to) is
    int type = 0;            // Kill / Harm: the kind (2 enemy, 1 innocent, 0 not counted); Drag: 1 started, 0 ended;
                             //   MissionCompleted / Failed: 1 the base level's, stopped
                             //   (Drag: value = the dragger's geom ref)
    float amount = 0.0f;     // HitmanDamage: damage, health after it
    float health = 0.0f;
    bool script = false;     // Message: one script sent to another (to: ScriptGeom(instance))
    void* instance = nullptr;
};

// The object whose script is `instance` (null: none of the snapshot's).
ZGEOM* ScriptGeom(void* instance, const SceneSnapshot& scene);

void InstallGameEvents();
// Lower case message names to keep; `all`: every message.
void WatchMessages(const std::set<std::string>& names, bool all);
// The level routine of the loaded scene (its vtable's kill / harm / mission
// entries are routed through the editor).
void WatchLevel(void* level);
// Whether the base level's own logic may end the mission (its level routine's
// MissionCompleted / MissionFailed): when not, such an end is only reported
// (GameEvent type 1); the editor's own end (AllowLevelEnd before it) and a
// failure after Hitman died always go through. Back to allowed when a scene
// is left.
void SetLevelEndsAllowed(bool allowed);
bool LevelEndsAllowed();
void AllowLevelEnd();  // the next end is the editor's (h2.mission, its goals)

// The characters the level routine was told were killed / harmed (knocked out) in this scene, as
// the game's own scripts tell it (Being2: CharacterKilled / CharacterHarmed -> LevelControl 0x504960
// -> Kill vfunc 0xB4 / Harm vfunc 0xB8 with the victim). The actor (ZActor) is the victim.
bool WasKilled(ZGEOM* character);
bool WasHarmed(ZGEOM* character);
std::vector<ZGEOM*> TakeKilled();  // killed since the last call

// The events since the last call (at most a few thousand a frame).
std::vector<GameEvent> TakeGameEvents();
// Every message name the engine knows now (lower case: as WatchMessages).
std::vector<std::string> MessageNames();

} // namespace h2::engine
