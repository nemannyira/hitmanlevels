#pragma once
// The games whose levels the packages carry (Hitman 2 now; Contracts and Blood Money later): what the shared
// package code needs of each, registered by the game's own code (h2/src/levelfmt/h2_game.cpp) so this part
// knows no game's level fields.
#include <string>
#include <vector>

namespace h2::levelfmt {

struct GameInfo {
    std::string id;           // in a package's manifest: "h2"
    std::string name;         // "Hitman 2: Silent Assassin"
    std::string extension;    // its packages' file extension: ".h2lvl"
    std::string exe;          // the game's executable, its folder told by it: "hitman2.exe"
    std::string steamFolder;  // under steamapps\common: "Hitman 2 Silent Assassin"
    std::string onlineDir;    // installed levels, under the game's folder: "Mods\\Online"
    // a level.json as an installed (downloaded) level is checked; false with the first reasons
    bool (*checkLevel)(const std::string& levelJson, std::string* why) = nullptr;
};

void RegisterGame(const GameInfo& game);  // (again for the same id: replaced)
const GameInfo* FindGame(const std::string& id);
const GameInfo* GameOfExtension(const std::string& path);  // by a file's extension (case-insensitive)
const std::vector<GameInfo>& Games();

} // namespace h2::levelfmt
