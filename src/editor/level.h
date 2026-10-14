#pragma once
// Level document: what a mod level changes relative to its base scene.
//
// Mods/<name>/level.json
// {
//   "format": "h2level", "version": 1,
//   "name": "My level",
//   "empty_map": true,                          // the base scene's characters, items and objectives taken out
//   "blank_map": true,                          // and its buildings (every model but the sky): hidden, stopping nothing
//   "place_floor": { "level": "C3-3", "mesh": 6341746 },  // a model of another level to put under Hitman (once)
//   "base_scene": "C1-1\\C1-1__MAIN",          // scene the level is built on
//   "own_map_of": "C1-1\\C1-1__MAIN",          // base_scene is a copy of it, the level's own (own_map.h)
//   "objects": [                                // edits of objects of the base scene
//     { "path": "ROOT/.../Crate#2", "class": "ZSTDOBJ",
//       "pos": [x, y, z], "matrix": [9 floats], "hidden": false } ],
//   "spawned": [                                // new objects: visual clones of a scene object
//     { "id": "Crate_01+1", "source_scene": "C1-1\C1-1__MAIN",
//       "source_path": "ROOT/.../Crate_01", "pos": [...], "matrix": [...], "hidden": false },
//     { "id": "Point+1", "kind": "point", "parent": "ROOT/.../Room", "pos": [...], ... } ],  // route point
//   "npcs": [                                   // characters the level places
//     { "id": "NPC_Maid_1", "record": 3803, "model": "Maid", "pos": [...], "matrix": [...],
//       "behaviour": "scripts\C1\C1_1\Maid.sdl",
//       "params": [ { "name": "GuardPoint", "type": "ZGEOMREF", "value": "ROOT/.../Lobby" }, ... ] } ],
//   "npc_edits": [                              // behaviour settings of the scene's characters
//     { "record": 2350, "path": "ROOT/.../Ground", "behaviour": "...", "params": [...] } ],
//   "lists": [                                  // changed lists (routes): their points in order
//     { "path": "ROOT/.../WPList", "points": ["ROOT/.../WP1Wait", "ROOT/.../Point+1", ...] } ],
//   "mission": {                                // objectives (the laptop's lines) and the mission's end
//     "objectives": [ { "slot": 1, "text": "Kill the lawyer", "goal": "kill",
//                       "target": "ROOT/.../MafiaLawyer/Ground", "replaces_original": true } ],
//     "end_when_done": true, "exit": "ROOT/.../Point+3", "exit_radius": 300,
//     "base_ends": false,                      // the base level's own logic does not end it
//     "base_files": false },                   // its laptop files (Target Photo); absent: hidden on an emptied map
//   "music": { "ROOT/.../MusicControl/InsideAndOutside/Location_01": "RussiaSlow" },
//   "vehicles": [ { "object": "ROOT/.../CarFerrari+1", "route": ["ROOT/.../Point+4", ...],
//                   "speed": 800, "loop": true, "reverse": false } ],  // driven along their points
//   "scripts": ["main.lua"]                     // level scripts, relative to the folder
// }
//
// A level built "from scratch" later is the same document on an empty base scene.
#include <map>
#include <string>
#include <vector>

#include "model.h"
#include "npcs.h"
#include "vehicles.h"

namespace h2::editor {

struct ObjectEdit {
    std::string className;
    ObjectState original;  // state before the level changed it (this session)
    ObjectState state;
    bool hasOriginal = false;
};

struct SpawnedObject {
    std::string id;           // also the object's name in the scene (copies)
    bool item = false;        // a working item made from the item template at sourcePath
    bool point = false;       // an invisible point (characters walk to points), no source
    bool group = false;       // a copy of a group (a car: body, doors, wheels): a new group with copies of
                              // the source's visible parts in it
    std::string sourceScene;  // clones currently come from the base scene itself
    std::string sourcePath;
    std::string parentPath;   // room it is placed in ("" = the source's parent)
    Transform transform;
    bool hidden = false;
};

struct LevelNpc {
    std::string id;          // object name of the character
    int record = -1;         // actor record of the base scene it copies (body, animations)
    std::string model;       // that record's model name (checked when loading)
    std::string importLevel; // its body from a character of another level (ImportedModel)
    uint32_t importMesh = 0;
    std::string importName;  // that character's model name
    Transform transform{};
    bool hasBehaviour = false;
    std::string behaviour;   // full script name
    std::string behaviourLevel;  // the level whose scripts have it ("": this map's; engine/script_files.h)
    std::vector<ParamValue> params;
};

struct NpcEditEntry {        // changed behaviour settings of a character of the scene
    int record = -1;
    std::string path;        // its object (information)
    std::string behaviour;
    std::vector<ParamValue> params;
};

struct MissionObjective {
    int slot = 1;                  // line of the laptop's objectives (1..6)
    std::string text;              // shown there ("" = the level's own text)
    enum Goal { Level, Kill, Reach, Take, KnockOut, Deliver, Unseen };
    Goal goal = Level;             // Level: the level's own scripts decide
    std::string target;            // Kill / KnockOut: the character, Reach: the place, Take / Deliver: the object
    std::string place;             // Deliver: where the object has to go (Unseen: no target)
    bool failsMission = false;     // Unseen: being spotted fails the mission too
    float radius = 250.0f;         // Reach: how near Hitman has to get; Take (not an item): to take it
    bool replacesOriginal = true;  // the level's own completion of this line no longer counts
};

struct MissionDoc {
    std::vector<MissionObjective> objectives;
    bool endWhenDone = false;      // the mission is completed when every objective is done ...
    std::string exitPath;          // ... and Hitman reaches this place ("" = at once)
    float exitRadius = 300.0f;
    std::string briefing;          // the laptop's mission briefing ("" = the level's own)
    bool baseEnds = true;          // the base level's own logic may end the mission
    int baseFiles = -1;            // the laptop's additional files of the base level (Target Photo):
                                   // 1 shown, 0 hidden, -1 hidden on an emptied map
};

struct ImportedModel {        // a model of another level the map is loaded with
    std::string level;        // "C2-1"
    uint32_t mesh = 0;        // its mesh reference there
    std::string name;         // its name there (information)
    bool character = false;   // a character's body (worn by a character of this map)
    std::string animSet;      // a body's animations ("Reg_Male")
    uint32_t outfitMesh = 0;  // its clothes on Hitman and its body without them (loaded with it)
    uint32_t nudeMesh = 0;
};

struct LevelDocument {
    std::string name = "Untitled";
    std::string folder;  // Mods\<folder>
    std::string baseScene;
    std::string ownMapOf;   // the game's map a map of its own was copied from ("": base_scene is the game's)
    bool emptyMap = false;  // the base scene's characters, items and objective lines are taken out
    std::string startOutfit;  // Hitman starts in the clothes of this model ("" his own suit)
    bool blankMap = false;  // its buildings too: every model of the map but the sky (hidden, stopping nothing)
    // an imported model to put under Hitman when the map is next loaded (a new
    // blank map's floor); then it is an object of the level like the others
    std::string floorLevel;
    uint32_t floorMesh = 0;
    // the sky of another level instead of the map's own (its backdrop's models, imported)
    std::string skyLevel;
    std::map<std::string, ObjectEdit> objects;  // by path
    std::vector<SpawnedObject> spawned;
    std::vector<LevelNpc> npcs;
    std::map<int, NpcEditEntry> npcEdits;  // by actor record
    std::map<std::string, std::vector<std::string>> lists;  // changed lists: path -> element paths
    MissionDoc mission;
    std::map<std::string, std::string> music;  // music object path -> track name ("RussiaSlow")
    std::vector<std::string> scripts;
    std::vector<ImportedModel> imports;
    std::vector<LevelVehicle> vehicles;  // objects driving routes when the level is played
    bool dirty = false;
};

LevelDocument& CurrentLevel();

// Records the new state of an object into the current level (called by the
// editor history for every change).
void RecordEdit(engine::ZGEOM* geom, const ObjectState& before, const ObjectState& after);

bool SaveLevel(std::string* error = nullptr);
// Saves the level and loads its map again: the characters placed since are
// made with the scene then (npcs.h CreateNpc). False when it is not saved.
bool ReloadLevelMap(std::string* error = nullptr);
// The edits so far become a level of that name (its own folder in Mods) and are saved.
bool SaveLevelAs(const std::string& name, std::string* error = nullptr);
// Toolbar: the level's name, unsaved mark and Save / Save as; Ctrl+S.
void DrawSaveControl();
// The same as a row of a panel (full width button).
void DrawSaveRow();
void SaveShortcut();
// What the Level panel's Problems list shows now.
std::vector<std::string> LevelProblems();
bool LoadLevel(const std::string& folder, std::string* error = nullptr);

// ---- models of other levels ----
// The level's list (the map gets them when it is loaded: see engine/imports.h);
// each is an object of ROOT/EditorImports in the loaded map, far below the
// world, which the Models tab lists and places like the map's own models.
bool IsImported(const std::string& level, uint32_t mesh);
void SetImported(const std::string& level, uint32_t mesh, const std::string& name, bool on, bool character = false,
                 const std::string& animSet = "", uint32_t outfitMesh = 0, uint32_t nudeMesh = 0);
// The characters' bodies of other levels the loaded map has (mesh: here).
struct ImportedBody {
    std::string level, name, animSet;
    uint32_t sourceMesh = 0, mesh = 0;
    uint32_t outfit = 0, nude = 0;  // here (0: none)
    std::string behaviour;          // its behaviour there ("Waiter")
};
std::vector<ImportedBody> ImportedBodies();
// Objects of the level made of it (it cannot leave the list while used).
int ImportUses(const std::string& level, uint32_t mesh);
// The list differs from what the loaded map has: it has to be loaded again.
bool ImportsPending();
void ReloadMapForImports();
// The level a model of ROOT/EditorImports comes from ("": not one of them).
std::string ImportLevelOf(const std::string& path);
// Changes when models of other levels were made in the loaded map.
int ImportLibraryVersion();
void NewLevel(const std::string& name, const std::string& baseScene);
// Creates a copy of the object next to it (or at `at`, world space) and adds
// it to the level (undoable).
engine::ZGEOM* DuplicateObject(engine::ZGEOM* source, std::string* error = nullptr, const Transform* at = nullptr,
                               engine::ZGEOM* parent = nullptr);
// Places a working item made from an item template, its world transform `at`
// (undoable).
engine::ZGEOM* PlaceItem(engine::ZGEOM* itemTemplate, const Transform& at, std::string* error = nullptr);
// Deletes an object the level created (undoable); false for scene objects.
bool DeleteObject(engine::ZGEOM* geom);
const SpawnedObject* FindSpawned(engine::ZGEOM* geom);
// A mission goal's object: an item the level made is kept as "level:<its id>"
// (its path changes when Hitman picks it up, and "KeyRoom#2" is a count of
// same-named objects), anything else as its path.
std::string TargetFor(engine::ZGEOM* geom);
engine::ZGEOM* ResolveTarget(const std::string& target);

// ---- characters (undoable, kept in the level) ----
engine::ZGEOM* PlaceNpc(const NpcSpec& spec, std::string* error = nullptr);
bool RemoveNpc(engine::ZGEOM* actor, std::string* error = nullptr);
// New behaviour settings for any character (or another behaviour for a placed one).
engine::ZGEOM* EditNpcBehaviour(engine::ZGEOM* actor, const engine::ScriptInfo* behaviour,
                                std::string* error = nullptr);
const LevelNpc* FindLevelNpc(engine::ZGEOM* actor);

// ---- route points and lists (undoable, kept in the level) ----
// A new point (an invisible helper characters can be sent to) at `at`, in `room`.
// The room of the map a point is in (the smallest map model around it).
engine::ZGEOM* RoomAt(const float p[3]);
engine::ZGEOM* PlacePoint(const Transform& at, engine::ZGEOM* room, std::string* error = nullptr);
// A point of the level under a name of its own ("Garage"; undoable): it is made
// again under it and the level's routes, goals and behaviour settings follow it
// (scripts naming it by its old name do not). Returns the renamed point.
engine::ZGEOM* RenamePoint(engine::ZGEOM* point, const std::string& name, std::string* error = nullptr);
// New elements of a list (a route: the points in walking order).
bool SetListPoints(engine::ZGEOM* list, const std::vector<engine::ZGEOM*>& points, std::string* error = nullptr);

// Takes the scene's characters and items out (empty map) or puts them back.
void SetEmptyMap(bool empty);
// Takes the scene's buildings out too (blank map) or puts them back.
void SetBlankMap(bool blank);
// A copy of one of the map's models was made (shown also when the blank map took the model out).
void UseBlankedMesh(uint32_t mesh);
// The level gets a model of another level as its floor: under Hitman, once
// the map is loaded with it.
void SetFloor(const std::string& level, uint32_t mesh, const std::string& name);
// The sky: another level's sky ("" = the map's own): its backdrop's models
// (sky dome, cloud planes) instead of the map's. The map is loaded again for
// a sky not imported yet (ImportsPending).
// The levels that have a sky (read once).
const std::vector<std::string>& SkyLevels();
void SetSky(const std::string& level);
// The scene changed: the sky made for the previous one is gone.
void ForgetSky();
// Applies the level's spawned objects and object edits to the loaded scene; returns how many applied.
int ApplyLevel();
// Undoes every edit of the level in the loaded scene (restores originals).
void RevertLevel();
// The levels in Mods (folders), the last saved first.
std::vector<std::string> ListLevels();
// When a level was last saved: its level.json's time, local ("2026-10-06 14:32"; "" unknown).
std::string LevelModified(const std::string& folder);

// "Level" panel and automatic application when the base scene loads.
void InstallLevelPanel();

} // namespace h2::editor
