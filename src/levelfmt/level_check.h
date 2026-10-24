#pragma once
// What a level file (Mods\<level>\level.json) may hold: the checks the editor
// DLL makes when it loads one and the Level Hub (docs/online_levels.md) makes
// before it installs a downloaded one, in one place. Nothing here knows the
// game: it reads JSON only, so it builds into the DLL, into h2level_check.exe
// and into a fuzz target alike.
#include <cstddef>
#include <string>
#include <vector>

#include "json.hpp"

namespace h2::levelfmt {

// What a level may hold; a level file from elsewhere is cut to it when it
// loads (CheckLevel reports it), and the editor makes no more.
constexpr size_t kMaxSpawned = 2000;   // placed objects (copies, items, points)
constexpr size_t kMaxEdits = 5000;     // objects of the map changed
constexpr size_t kMaxNpcs = 200;       // characters added; also characters' new settings
constexpr size_t kMaxLists = 256;      // lists changed, new lists
constexpr size_t kMaxListPoints = 256; //   points of one
constexpr size_t kMaxScripts = 16;
constexpr size_t kMaxVehicles = 64;
constexpr size_t kMaxObjectives = 6;   // the laptop's lines
constexpr size_t kMaxImports = 256;    // models of other levels
// points shown on the in-game map: the map draws 256 icons on a floor (engine::InstallIngameMap), and a
// level of 1800 of them on C1-1 never finished loading (the scene's routine list went round in a circle
// in InitializeGeoms, 0x41E9F6; 1500 loaded)
constexpr size_t kMaxMapIcons = 256;
constexpr size_t kMaxFileBytes = 16u << 20;  // level.json

// A script of the level: a file of its folder by a plain name ("main.lua"; no
// folders, drives, device names such as CON, at most 32 characters before .lua).
bool ValidScriptName(const std::string& name);
// The in-game map's legend: the icons a point may have ("Icon_Exit", ...).
const std::vector<std::string>& MapIconNames();
// A downloaded level's id, its folder Mods\Online\<id>: [a-z0-9]{8,32}.
bool ValidOnlineId(const std::string& id);

struct Issue {
    bool error;         // the level is refused (strict) / the field is left out (otherwise)
    std::string where;  // "spawned[3].pos"
    std::string what;
};

struct Report {
    std::vector<Issue> issues;
    bool Ok() const;                       // no errors
    std::string Summary(size_t max = 5) const;  // the first errors (then warnings), one a line
};

// Checks a parsed level.json. `strict` (a downloaded level): a field the
// editor does not write, a value of the wrong kind or out of range, more than
// a level may hold is an error; otherwise (the user's own files, older
// versions) those are warnings and the loader leaves them out.
Report CheckLevel(const nlohmann::json& level, bool strict);
// The same from the file's text (a parse error is an error).
Report CheckLevelText(const std::string& text, bool strict);

} // namespace h2::levelfmt
