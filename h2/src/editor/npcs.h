#pragma once
// NPCs of the editor: the characters of the scene (actor records), placing new
// ones and giving any NPC (scene or placed) another behaviour. Behaviours are
// the scene's SDL script classes with their parameters (see h2/docs/npc.md);
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
// The behaviours of the map's characters (on their own map whatever they point
// to is there; IsStoryBehaviour tells the ones playing a part of its story),
// and the game's plain civilian (Neutral2) when the scene has it. Short names
// ("EntranceGuard"), sorted.
std::vector<std::string> BehaviourNames();
// A behaviour playing a part of its map's story: its settings point to the
// map's characters or vehicles (C2-2's Boss: its counterpart and limousine;
// its Bodyguard: the boss), or it uses furniture. A copy acts that part.
bool IsStoryBehaviour(const engine::ScriptInfo& s);
// A setting that is one character's own place or route (guard point, protect
// point, waypoints, ...): never taken from another character (FillSharedRefs).
bool IsOwnPlaceRef(const std::string& param);
// Of those, the routes (lists of points walked: WayPoints, PatrolPointList, ...).
bool IsRouteRef(const std::string& param);
// The parameters a script class has at its first use in the scene (runtime
// refs); Neutral2's as the game's characters declare them.
bool BehaviourTemplate(const std::string& name, engine::ScriptInfo& out);
// Whether a behaviour works anywhere: its references are only places and
// lists the editor can give it (guard point, route, help and safe points,
// weapon, guard group, alarm, level control), none to an object of its level
// (an item it takes, a door, another character, a stove).
bool IsPortableBehaviour(const engine::ScriptInfo& s);
// A behaviour of another level's characters by name: "Pedestrian2 (C7-1)"
// (BehaviourTemplate takes it; its compiled scripts are loaded from that level).
std::string ForeignBehaviourName(const std::string& script, const std::string& level);
// The portable behaviours of the game's other levels, as such names (sorted).
const std::vector<std::string>& ForeignBehaviourNames();
// Its zero references filled from the scene's characters' settings of the same
// name (help and safe points, weapon template, guard group, alarm list, ...;
// not a character's own place or route: IsOwnPlaceRef), an arbiter the map's
// characters do not name from the map's guard group; how many.
// requiredOnly: only those it cannot start without (engine/script_files.h).
int FillSharedRefs(engine::ScriptInfo& s, bool requiredOnly = false);
// The game's plain civilian (AllLevels/Std2/Neutral2: stands, panics and runs
// for help at trouble), "" when the scene's scripts do not have it.
std::string CivilianBehaviour();
// Actor record by model name, group name ("BodyguardMafia_Bedroom_00"),
// record index as text, or an object path of a scene NPC; -1 if none.
int FindNpcRecord(const std::string& what);

// ---- NPC objects ----

// The character an object belongs to (the ZActor up its parents), or null.
engine::ZGEOM* ActorOf(engine::ZGEOM* geom);
// A character knocked out (chloroform, a blow): its behaviour in the base
// class's unconscious state ("Being2.Unconscious"). The engine counts it as
// not alive (Actor IsAlive): it is not dead.
bool KnockedOut(engine::ZGEOM* geom);
// Record a scene NPC was made from, or the record a placed NPC copies; -1.
int NpcRecordOf(engine::ZGEOM* actor);

struct NpcSpec {
    int record = -1;          // body, animations and model come from this actor record
    Transform transform{};    // world
    bool hasBehaviour = false;
    engine::ScriptInfo behaviour;  // runtime refs
    std::string name;         // object name (made unique when empty)
    engine::ZGEOM* parentRoom = nullptr;  // room it starts in (default: the original character's)
    uint32_t mesh = 0;        // another body (a character of another level), 0: the record's
    std::string importLevel;  // that body's level, its mesh there and model name (saved)
    uint32_t importMesh = 0;
    std::string importName;
    uint32_t outfitMesh = 0;  // that character's clothes on Hitman / body without them, here
    uint32_t nudeMesh = 0;
    // made now with its behaviour (a script's character while the game runs:
    // nothing but other characters bump into it), not shown until the map
    // loads again
    bool now = false;
};
// A character placed now is not yet one of the scene's: it only shows where
// it will be (no behaviour, nothing bumps into it, it cannot be shot). The
// level's characters are made with the scene when it loads (engine/inject.h):
// it is a real one after the map is loaded again, with its behaviour.
engine::ZGEOM* CreateNpc(const NpcSpec& spec, std::string* error = nullptr);
bool IsPlacedNpc(engine::ZGEOM* actor);
// A placed character that is as it should be only after the map is loaded
// again (placed now, or its behaviour changed), and how many there are.
bool IsPendingNpc(engine::ZGEOM* actor);
int PendingNpcCount();
const std::vector<engine::ZGEOM*>& PlacedNpcs();

// Changes an NPC's behaviour: the same kind of behaviour gets the new settings
// in place (any character); a placed character's other kind (or none) is
// kept for the next load (IsPendingNpc). The level's own characters keep
// their kind of behaviour.
bool SetNpcBehaviour(engine::ZGEOM* actor, const engine::ScriptInfo* behaviour, std::string* error = nullptr);
// Removes a character the editor placed (hidden and moved far away: actors
// cannot be destroyed while the scene runs).
bool DeleteNpc(engine::ZGEOM* actor, std::string* error = nullptr);
bool IsRemovedNpc(engine::ZGEOM* actor);

// Placing: a character of the record standing at `t` (no behaviour, not part
// of the level; one per model, used again), and putting it away.
engine::ZGEOM* ShowPreviewNpc(int record, const Transform& t, std::string* error = nullptr, uint32_t mesh = 0);
// The animation set of a character record of the scene ("Reg_Male"; "" when none).
std::string NpcAnimSet(int record);
// Whether that character has an extra animation set (its own moves).
bool NpcHasExtraAnims(int record);
void HidePreviewNpc();
// The behaviour the NPC runs as far as the editor knows (set here, or as
// authored in the scene); false when it has none.
bool CurrentBehaviour(engine::ZGEOM* actor, engine::ScriptInfo& out);

// Settings of a behaviour that move the character when its script starts:
// to a point of its route (LONG WarpToPathPoint / startAtRandomPathPoint, on
// when not 0) or to its own start point (a reference to one point:
// StartPos, SpawnPosition, m_rStartPos, ...). Lists of start points are not
// among them (the script picks one).
struct StartMove {
    std::string name;
    bool ref = false;
    uint32_t value = 0;  // as the behaviour has it (LONG / runtime ref)
};
std::vector<StartMove> StartMoves(const engine::ScriptInfo& s);
// Sets the LONG ones of `moves` in `s`: 0 (off) or back to their value.
void SetRouteWarps(engine::ScriptInfo& s, const std::vector<StartMove>& moves, bool on);

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
