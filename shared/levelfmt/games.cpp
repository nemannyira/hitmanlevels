#include "games.h"

#include <algorithm>
#include <cctype>

namespace h2::levelfmt {

namespace {

std::vector<GameInfo>& Registry()
{
    static std::vector<GameInfo> games;
    return games;
}

} // namespace

void RegisterGame(const GameInfo& game)
{
    auto& games = Registry();
    auto it = std::find_if(games.begin(), games.end(), [&](const GameInfo& g) { return g.id == game.id; });
    if (it != games.end())
        *it = game;
    else
        games.push_back(game);
}

const GameInfo* FindGame(const std::string& id)
{
    for (const GameInfo& g : Registry())
        if (g.id == id)
            return &g;
    return nullptr;
}

const GameInfo* GameOfExtension(const std::string& path)
{
    std::string low(path);
    std::transform(low.begin(), low.end(), low.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    for (const GameInfo& g : Registry())
        if (low.size() > g.extension.size() && low.compare(low.size() - g.extension.size(), g.extension.size(),
                                                           g.extension) == 0)
            return &g;
    return nullptr;
}

const std::vector<GameInfo>& Games() { return Registry(); }

} // namespace h2::levelfmt
