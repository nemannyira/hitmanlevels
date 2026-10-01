#pragma once
// Creating NPCs (ZTRACKLNKHM2 actors) at runtime from the actor records of the
// loaded scene, with a behaviour (SDL script) of choice. See re/npc.md.
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
};

ZGEOM* SpawnNpc(const NpcSpawn& spawn, std::string* error = nullptr);

// Actor records of the scene (class ZTRACKLNKHM2).
std::vector<int> ActorRecords();
// Model name from an actor's data block ("BodyguardMafia_01"), "" if unknown.
std::string ActorModelName(const SceneRecord& actor);

} // namespace h2::engine
