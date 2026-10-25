#pragma once
// Names every game's levels and packages share: a level's script files and a downloaded level's id.
#include <string>

namespace h2::levelfmt {

// A script of the level: a file of its folder by a plain name ("main.lua"; no
// folders, drives, device names such as CON, at most 32 characters before .lua).
bool ValidScriptName(const std::string& name);
// A downloaded level's id, its folder <game>\Mods\Online\<id>: [a-z0-9]{8,32}.
bool ValidOnlineId(const std::string& id);

} // namespace h2::levelfmt
