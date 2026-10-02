#pragma once
// NPCs of the editor: the characters of the scene (actor records), placing new
// ones and giving any NPC (scene or placed) another behaviour. Behaviours are
// the scene's SDL script classes with their parameters (see docs/npc.md);
// references in them are runtime object refs here.
#include <string>
#include <vector>

#include "../engine/scene_data.h"
#include "model.h"

namespace h2::editor {

// ---- the scene's characters and behaviours ----

// Actor records of the loaded scene (class ZTRACKLNKHM2).
std::vector<int> NpcRecords();
// "BodyguardMafia_01" (model name from the data block, else the record name)
std::string NpcModel(int record);
// The behaviour the record has in the scene file (runtime refs); false: none.
bool AuthoredBehaviour(int record, engine::ScriptInfo& out);
// Script classes used in the scene: short names ("BaseGuard"), sorted.
std::vector<std::string> BehaviourNames();
// The parameters a script class has at its first use in the scene (runtime refs).
bool BehaviourTemplate(const std::string& name, engine::ScriptInfo& out);
// Actor record by model name, group name ("BodyguardMafia_Bedroom_00"),
// record index as text, or an object path of a scene NPC; -1 if none.
int FindNpcRecord(const std::string& what);

// ---- NPC objects ----

// The character an object belongs to (the ZActor up its parents), or null.
engine::ZGEOM* ActorOf(engine::ZGEOM* geom);
// Record a scene NPC was made from, or the record a placed NPC copies; -1.
int NpcRecordOf(engine::ZGEOM* actor);

struct NpcSpec {
    int record = -1;          // body, animations and model come from this actor record
    Transform transform{};    // world
    bool hasBehaviour = false;
    engine::ScriptInfo behaviour;  // runtime refs
    std::string name;         // object name (made unique when empty)
    engine::ZGEOM* parentRoom = nullptr;  // room it starts in (default: the original character's)
};
engine::ZGEOM* CreateNpc(const NpcSpec& spec, std::string* error = nullptr);
bool IsPlacedNpc(engine::ZGEOM* actor);
const std::vector<engine::ZGEOM*>& PlacedNpcs();

// Changes an NPC's behaviour: the same kind of behaviour gets the new settings
// in place (any character); another kind (or none) makes a placed character
// again (LastReplacedNpc() is the new object). The level's own characters
// keep their kind of behaviour.
bool SetNpcBehaviour(engine::ZGEOM* actor, const engine::ScriptInfo* behaviour, std::string* error = nullptr);
engine::ZGEOM* LastReplacedNpc();
// Removes a character the editor placed (hidden and moved far away: actors
// cannot be destroyed while the scene runs).
bool DeleteNpc(engine::ZGEOM* actor, std::string* error = nullptr);
bool IsRemovedNpc(engine::ZGEOM* actor);

// Placing: a character of the record standing at `t` (no behaviour, not part
// of the level; one per model, used again), and putting it away.
engine::ZGEOM* ShowPreviewNpc(int record, const Transform& t, std::string* error = nullptr);
void HidePreviewNpc();
// The behaviour the NPC runs as far as the editor knows (set here, or as
// authored in the scene); false when it has none.
bool CurrentBehaviour(engine::ZGEOM* actor, engine::ScriptInfo& out);

// Serialisation of behaviours: references as object paths.
struct ParamValue {
    std::string name;
    std::string type;   // LONG / FLOAT / ZGEOMREF / STRING / other (raw hex)
    std::string value;  // number as text, object path, string, hex bytes
};
std::vector<ParamValue> SaveParams(const engine::ScriptInfo& s);
// Fills `s` (which must hold the class's declarations) from saved values.
void LoadParams(engine::ScriptInfo& s, const std::vector<ParamValue>& values);

void InstallNpcs();

} // namespace h2::editor
