#pragma once
// The game's events for level scripts.
//
// Every message goes through ZGEOM::SendMessage (vfunc 0x120 = 0x43CBE0, the
// same function in every geom class): `this` sends (receiver, msg id,
// param*); the receiver's HandleMessage (vfunc 0x15C) hands it to its script
// by name and vfunc 0x10C to its routines. Names: engine +0xC37 + id*4
// (GetZMsgName 0x41EEB0), ids 1..engine +0x1C43.
//
// The level routine (LevelControl) classifies a killed / harmed character
// (vfunc 0xB0) and calls vfunc 0xB4 / 0xB8 (victim, kind: 1 enemy, 2
// innocent), the end of a mission vfunc 0xC4 (completed) / 0xC0 (failed).
// Hitman's damage: ZHITMAN2 0x519390 (hitman, float amount), hit points at
// +0xD72.
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
    enum Kind { Message, Kill, Harm, MissionCompleted, MissionFailed, HitmanDamage } kind = Message;
    std::string name;        // Message: the message's name
    ZGEOM* to = nullptr;     // Message: receiver; Kill / Harm: the level object
    ZGEOM* from = nullptr;   // Message: sender
    uintptr_t param = 0;     // Message: the param pointer itself (some messages pass a value: 0 / 1)
    bool hasValue = false;   // the dword the param points to was readable
    uint32_t value = 0;      //   (Kill / Harm: the victim, a geom ref or pointer)
    std::string text;        // a text the param (or what it points to) is
    int type = 0;            // Kill / Harm: the level's classification (1 enemy, 2 innocent)
    float amount = 0.0f;     // HitmanDamage: damage, health after it
    float health = 0.0f;
};

void InstallGameEvents();
// Lower case message names to keep; `all`: every message.
void WatchMessages(const std::set<std::string>& names, bool all);
// The level routine of the loaded scene (its vtable's kill / harm / mission
// entries are routed through the editor).
void WatchLevel(void* level);
// The events since the last call (at most a few thousand a frame).
std::vector<GameEvent> TakeGameEvents();
// Every message name the engine knows now (lower case: as WatchMessages).
std::vector<std::string> MessageNames();

} // namespace h2::engine
