#pragma once
// "Level editor" in the game's main menu (under Load): the menu's buttons
// below Load move down a line and the entry is drawn there in the menu's
// style; it opens the launcher: a new level on one of the game's maps
// (optionally an empty map: no characters, items or objectives of the
// level) or a level saved before. The chosen map is loaded and the editor
// opens on it.

namespace h2::editor {

void InstallLauncher();

} // namespace h2::editor
