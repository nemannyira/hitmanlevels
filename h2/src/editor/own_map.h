#pragma once
// A map of its own: the level's base map copied under a new name
// (Scenes\<Name>\<Name>__MAIN.ZIP, its entries renamed SCENES/<Name>/...; the
// data itself copied as it is, still compressed; the map's sound streams
// <Name>__MAIN.WAV / .WHD and its loading music beside it). The level then
// is for that map: the launcher lists it under its own name, it loads as a
// scene of its own and its saved games are its own. The objects keep their
// paths (they are the scene's records: "ROOT/C1-1/..."), the base level's
// texts and scripts work as on the original.
#include <string>

namespace h2::editor {

// The scene name of a map of its own ("Custom1" -> "Custom1\Custom1__MAIN").
std::string OwnSceneName(const std::string& name);
// Makes the map (the level's base map copied) and makes the level its; saved.
bool MakeOwnMap(const std::string& name, std::string* error = nullptr);
// The level back on its original map; `removeFiles`: the copy deleted too.
bool BackToOriginalMap(bool removeFiles, std::string* error = nullptr);
// Level panel: the "A map of its own" section.
void DrawOwnMap();

} // namespace h2::editor
