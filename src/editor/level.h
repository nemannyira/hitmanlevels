#pragma once
// Level document: what a mod level changes relative to its base scene.
//
// Mods/<name>/level.json
// {
//   "format": "h2level", "version": 1,
//   "name": "My level",
//   "base_scene": "C1-1\\C1-1__MAIN",          // scene the level is built on
//   "objects": [                                // edits of objects of the base scene
//     { "path": "ROOT/.../Crate#2", "class": "ZSTDOBJ",
//       "pos": [x, y, z], "matrix": [9 floats], "hidden": false } ],
//   "spawned": [],                              // new objects (cloned from a source scene
//                                               // object; filled once cloning exists)
//   "scripts": ["main.lua"]                     // level scripts, relative to the folder
// }
//
// A level built "from scratch" later is the same document on an empty base scene.
#include <map>
#include <string>
#include <vector>

#include "model.h"

namespace h2::editor {

struct ObjectEdit {
    std::string className;
    ObjectState original;  // state before the level changed it (this session)
    ObjectState state;
    bool hasOriginal = false;
};

struct SpawnedObject {
    std::string id;
    std::string sourceScene;
    std::string sourcePath;
    Transform transform;
};

struct LevelDocument {
    std::string name = "Untitled";
    std::string folder;  // Mods\<folder>
    std::string baseScene;
    std::map<std::string, ObjectEdit> objects;  // by path
    std::vector<SpawnedObject> spawned;
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
// Applies the level's object edits to the loaded scene; returns how many applied.
int ApplyLevel();
// Undoes every edit of the level in the loaded scene (restores originals).
void RevertLevel();
std::vector<std::string> ListLevels();

// "Level" panel and automatic application when the base scene loads.
void InstallLevelPanel();

} // namespace h2::editor
