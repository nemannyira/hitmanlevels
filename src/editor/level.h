#pragma once
// Level document: what a mod level changes relative to its base scene.
//
// Mods/<name>/level.json
// {
//   "format": "h2level", "version": 1,
//   "name": "My level",
//   "empty_map": true,                          // the base scene's characters, items and objectives taken out
//   "base_scene": "C1-1\\C1-1__MAIN",          // scene the level is built on
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
//     "end_when_done": true, "exit": "ROOT/.../Point+3", "exit_radius": 300 },
//   "music": { "ROOT/.../MusicControl/InsideAndOutside/Location_01": "RussiaSlow" },
//   "scripts": ["main.lua"]                     // level scripts, relative to the folder
// }
//
// A level built "from scratch" later is the same document on an empty base scene.
#include <map>
#include <string>
#include <vector>

#include "model.h"
#include "npcs.h"

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
    Transform transform{};
    bool hasBehaviour = false;
    std::string behaviour;   // full script name
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
    enum Goal { Level, Kill, Reach };
    Goal goal = Level;             // Level: the level's own scripts decide
    std::string target;            // Kill: the character, Reach: the place (object paths)
    float radius = 250.0f;         // Reach: how near Hitman has to get
    bool replacesOriginal = true;  // the level's own completion of this line no longer counts
};

struct MissionDoc {
    std::vector<MissionObjective> objectives;
    bool endWhenDone = false;      // the mission is completed when every objective is done ...
    std::string exitPath;          // ... and Hitman reaches this place ("" = at once)
    float exitRadius = 300.0f;
};

struct LevelDocument {
    std::string name = "Untitled";
    std::string folder;  // Mods\<folder>
    std::string baseScene;
    bool emptyMap = false;  // the base scene's characters, items and objective lines are taken out
    std::map<std::string, ObjectEdit> objects;  // by path
    std::vector<SpawnedObject> spawned;
    std::vector<LevelNpc> npcs;
    std::map<int, NpcEditEntry> npcEdits;  // by actor record
    std::map<std::string, std::vector<std::string>> lists;  // changed lists: path -> element paths
    MissionDoc mission;
    std::map<std::string, std::string> music;  // music object path -> track name ("RussiaSlow")
    std::vector<std::string> scripts;
    bool dirty = false;
};

LevelDocument& CurrentLevel();

// Records the new state of an object into the current level (called by the
// editor history for every change).
void RecordEdit(engine::ZGEOM* geom, const ObjectState& before, const ObjectState& after);

bool SaveLevel(std::string* error = nullptr);
bool LoadLevel(const std::string& folder, std::string* error = nullptr);
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

// ---- characters (undoable, kept in the level) ----
engine::ZGEOM* PlaceNpc(const NpcSpec& spec, std::string* error = nullptr);
bool RemoveNpc(engine::ZGEOM* actor, std::string* error = nullptr);
// New behaviour settings for any character (or another behaviour for a placed one).
engine::ZGEOM* EditNpcBehaviour(engine::ZGEOM* actor, const engine::ScriptInfo* behaviour,
                                std::string* error = nullptr);
const LevelNpc* FindLevelNpc(engine::ZGEOM* actor);

// ---- route points and lists (undoable, kept in the level) ----
// A new point (an invisible helper characters can be sent to) at `at`, in `room`.
engine::ZGEOM* PlacePoint(const Transform& at, engine::ZGEOM* room, std::string* error = nullptr);
// New elements of a list (a route: the points in walking order).
bool SetListPoints(engine::ZGEOM* list, const std::vector<engine::ZGEOM*>& points, std::string* error = nullptr);

// Takes the scene's characters and items out (empty map) or puts them back.
void SetEmptyMap(bool empty);
// Applies the level's spawned objects and object edits to the loaded scene; returns how many applied.
int ApplyLevel();
// Undoes every edit of the level in the loaded scene (restores originals).
void RevertLevel();
std::vector<std::string> ListLevels();

// "Level" panel and automatic application when the base scene loads.
void InstallLevelPanel();

} // namespace h2::editor
