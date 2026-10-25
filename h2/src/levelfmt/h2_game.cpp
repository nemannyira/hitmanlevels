#include "h2_game.h"

#include "level_check.h"
#include "levelfmt/games.h"

namespace h2::levelfmt {

namespace {

bool CheckH2Level(const std::string& levelJson, std::string* why)
{
    Report r = CheckLevelText(levelJson, true);  // (a downloaded level: strict)
    if (!r.Ok() && why)
        *why = r.Summary();
    return r.Ok();
}

} // namespace

void RegisterH2Game()
{
    GameInfo g;
    g.id = "h2";
    g.name = "Hitman 2: Silent Assassin";
    g.extension = ".h2lvl";
    g.exe = "hitman2.exe";
    g.steamFolder = "Hitman 2 Silent Assassin";
    g.onlineDir = "Mods\\Online";
    g.checkLevel = &CheckH2Level;
    RegisterGame(g);
}

} // namespace h2::levelfmt
