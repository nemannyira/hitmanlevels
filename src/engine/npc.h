#pragma once
// Creating NPCs (ZTRACKLNKHM2 actors) at runtime from the actor records of the
// loaded scene, with a behaviour (SDL script) of choice. See re/npc.md.
#include <map>
#include <string>
#include <vector>

#include "geoms.h"
#include "scene_data.h"

namespace h2::engine {

struct NpcSpawn {
    int record = -1;             // actor record the NPC is made from (body, animations, model)
    ZGEOM* parent = nullptr;     // group / room it is put in
    float mat[9] = {0, 0, 1, 0, 1, 0, 1, 0, 0};  // world orientation (rows forward, up, right)
    float pos[3] = {};           // world position
    std::vector<uint8_t> script; // behaviour block (BuildScriptBlock), empty: none
    std::string name = "Ground";
    uint32_t mesh = 0;           // another body (an imported character's), 0: the record's
    std::map<uint32_t, uint32_t> refSwap;  // references of its data block replaced (outfits)
    bool collides = true;        // one of the level's characters (SetActorCollides); false: a preview
};

ZGEOM* SpawnNpc(const NpcSpawn& spawn, std::string* error = nullptr);
// Characters bump into each other and Hitman, can be shot and seen through
// the scene's list "TrackLinkObjects" (every actor of the scene file; its
// events ZLIST_ActorCollision, ZLIST_CheckVisible, ZLIST_SelectNotify): an
// actor made at runtime is added to it (and gets its bounds), or taken out.
bool SetActorCollides(ZGEOM* actor, bool on);
// An item goes to a character (Hitman: into his inventory) as when it is
// picked up: ZITEM::SetOwner 0x4723F0 (owner ref, the owner's group, notify 1),
// the call Hitman's pickup makes (and that gives guards their weapons).
bool GiveItem(ZGEOM* item, ZGEOM* owner);
// Room left in the engine's buffers for new objects (sized while the scene
// loads; full, the game goes down): nodes (0x70 bytes each) and the objects'
// own memory (bytes; the largest free block too). -1: not known.
int FreeNodeSlots();
long long FreeObjectBytes(uint32_t* largestBlock = nullptr);
// Whether `bytes` and `nodes` more can be made (a reserve kept for the game).
bool RoomForObjects(size_t bytes, int nodes, std::string* error = nullptr);
// Hooks: room for actors added at runtime in the lists' events.
void InstallNpc();
// Removes an NPC made by SpawnNpc (its script stopped, the actor, its group).
// NOT safe at runtime: the engine keeps pointers to actors (crash right after).
bool DestroyNpc(ZGEOM* actor, std::string* error = nullptr);
// Moves a character: actors keep their own position (actor+0x57C, a little
// above the node) and write it into their node every frame. The room it is
// drawn with does not change (it stays invisible in another room).
bool TeleportActor(ZGEOM* actor, const float pos[3], const float* mat = nullptr);

// A behaviour starting may put its character elsewhere at once (on its
// route's first point, a start point of the level it was written for: the
// LONG WarpToPathPoint, start references, or its own code, like C1-1's
// DeliveryBoy), even while it is made. For `seconds` (and at least 120
// frames) from now, a jump of the actor further than walking (or being away
// from `pos` already) is undone: it is put back at `pos` (or where
// TeleportActor puts it meanwhile), and walks from there.
void KeepStartPlace(ZGEOM* actor, const float pos[3], const float mat[9], float seconds = 10.0f);

// Stops the geom's running script (if any) and, with `block` (a script block
// as BuildScriptBlock makes it), starts that one instead - the way PostInit
// starts it (ZGEOM::CreateScript 0x43CE30).
bool SetScript(ZGEOM* geom, const std::vector<uint8_t>* block, std::string* error = nullptr);

// Running script of a geom (null: none) and its class name.
void* ScriptInstance(ZGEOM* geom);
// Writes the parameters (LONG / FLOAT / ZGEOMREF values) into the running
// script instance by name: SDL instances keep their variables at
// instance[0x23] + offset, the offsets in the class's import list (see
// docs/npc.md). Returns how many were written.
int SetScriptVariables(ZGEOM* geom, const ScriptInfo& values, std::string* error = nullptr);
// Reads the current values of the running script into `values` (by name).
int GetScriptVariables(ZGEOM* geom, ScriptInfo& values);
// Name of the state the running script is in ("" when none).
std::string ScriptState(ZGEOM* geom);

// Actor records of the scene (class ZTRACKLNKHM2).
std::vector<int> ActorRecords();
// Model name from an actor's data block ("BodyguardMafia_01"), "" if unknown.
std::string ActorModelName(const SceneRecord& actor);

} // namespace h2::engine
