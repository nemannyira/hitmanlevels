#pragma once
// Behaviours of other levels (see h2/docs/npc.md, "Behaviours of other levels").
//
// A behaviour is a class of the SDL script engine, compiled into a file
// "<path>.ppo" of the scene's ZGF (fonts, particle palettes, scripts: a chunk
// tree, tools/zgf.py). The engine mounts the ZGF while the scene loads
// (ZSysFile vfunc 0x84) and unmounts it after (0x8C); the script engine loads
// a class by its path ("scripts\C7\C7_1\Car.sdl" -> ".ppo"), its base classes
// relative to it, through ZSysFile Exists / Size / Read (0x40E2C0, 0x40E1B0,
// 0x40E540: the mounted archives first, then the disk), and keeps every class
// it loaded by that path. So a class of another level can be given to a
// character at any time once its files can be read: the hooks here answer
// for the .ppo files of the levels asked for (from their ZIPs, in memory),
// when neither the mounted ZGF nor the disk has them.
#include <string>
#include <vector>

#include "scene_data.h"

namespace h2::engine {

void InstallScriptFiles();

// The compiled scripts of `level` ("C7-1") can be loaded from now on.
bool ProvideLevelScripts(const std::string& level);

// A behaviour of a level: a script class its characters use.
struct LevelBehaviour {
    std::string name;       // short name ("Pedestrian2")
    ScriptInfo script;      // path, the parameters of its first character (references 0), level
    int uses = 0;           // characters of the level with it
    std::vector<std::string> refs;  // its reference parameters
};
// The behaviours of a level's characters (cached; empty when it cannot be read).
const std::vector<LevelBehaviour>& LevelBehaviours(const std::string& level);

// Reference parameters a behaviour cannot start without (a route list it
// walks: the script engine stops the game on a missing one).
bool IsRequiredRef(const std::string& param);
// The first such parameter `s` has no value for ("" when none).
std::string MissingRequiredRef(const ScriptInfo& s);

} // namespace h2::engine
