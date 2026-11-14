Hitman Levels {VERSION} for Hitman 2: Silent Assassin
=====================================================

A level editor inside the game, and the way to share levels. Make new missions on the game's own
maps (targets, people and their routes, items, objectives, the laptop's briefing) and play the
levels other players made.

Source, issues and changes: https://github.com/nemannyira/hitmanlevels


WHAT YOU NEED

  * Hitman 2: Silent Assassin, the Steam version (1.02). Other versions are recognised and left alone:
    the editor then does not start, the game runs as it is.
  * Windows.


INSTALL

  1. Find the game's folder. With Steam:
       C:\Program Files (x86)\Steam\steamapps\common\Hitman 2 Silent Assassin
     (Steam: right-click the game > Manage > Browse local files.)
  2. Copy dinput8.dll and H2Fix.ini next to hitman2.exe.
  3. Optional: copy the folders in examples\ into the game's Mods folder (make it if it is missing).

  Back up the game's Save folder first: a game saved while playing a level takes one of the game's
  own save slots.

  To remove it: delete dinput8.dll and H2Fix.ini.


FIRST STEPS

  * Main menu > Level Editor: New level (pick a map and Simple: a guide takes you step by step) or
    Open a level (Play, or Edit).
  * F8 opens and closes the editor while you play; F1 lists the keys.
  * H2Fix.ini holds the settings (display, the editor's key, Simple or Advanced mode). It also brings
    fixes for today's PCs: a frame limit, borderless window, widescreen field of view, larger menus.


SHARING LEVELS

  * Save & share (Simple mode) or Level > Share > Export package writes your level as one file:
    Mods\Packages\<level>_v<n>.h2lvl. It is signed with an author key made on your PC the first time.
  * Then "Upload it on hitmanlevels.com" (next to Export) opens the site's upload page.
  * To install levels with a double click: run "Open .h2lvl files with levelpack.cmd" once (it runs
    "levelpack register"; "levelpack unregister" takes it back). Then a double click on a .h2lvl
    installs it (or run "levelpack install <file>"). It shows the level, its author and license, and
    refuses a package that was changed after its author signed it. Installed levels go to
    Mods\Online and run their scripts in a stricter sandbox.
  * levelpack --help lists the rest (inspect, author key backup and restore).


UNSIGNED FILES

  These files are not code-signed yet, so Windows SmartScreen or an antivirus may warn about them.
  Check that the zip is the one published: its SHA-256 is next to the download
  (PowerShell: Get-FileHash hitmanlevels-h2-{VERSION}.zip).


PRIVACY

  The editor (dinput8.dll) never connects to the internet. levelpack.exe connects only when you ask it
  to list or install levels from the site (levelpack hub ...).


KNOWN ISSUES

  The open ones are on https://github.com/nemannyira/hitmanlevels/issues


LICENSE

  MIT (LICENSE.txt). The libraries it is built with are under their own licenses
  (THIRD_PARTY_LICENSES.txt). A fan project, not affiliated with or endorsed by IO Interactive;
  Hitman is a trademark of IO Interactive A/S. You need your own copy of the game.
