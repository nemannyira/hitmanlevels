# Changes

What changed for level makers, newest first. The details (addresses, the
game's functions) are in `h2/docs/roadmap.md`, `h2/docs/mission.md`, `h2/docs/lua.md`.

## 2026-10-26: the briefing on every map (#12)

* The Mission tab's briefing is shown on maps whose laptop names its texts
  differently (C2-2: `Missionbriefing/Missionbriefing%d`, a text crawling up
  the box): before, the map's own briefing was shown there.

## 2026-10-26: items placed where they can be picked up (#13)

* An item placed with a click (*Models > Pickup items*) goes 3 cm above the
  ground's collision, not only above the drawn ground: on C2-2 the drawn snow
  lies 9 cm under it, and the game did not offer "Pick up".

## 2026-10-25 (evening): guards that work on every map (#10, #11)

* *NPCs > Place a character* offers the map's own behaviours: on C2-2 a new
  soldier is an EntranceGuard, Guard22 or PatrolGuard22 like the map's own.
  Behaviours acting out a part of the map's mission (a boss, its bodyguard,
  a limousine's guard) are listed apart, as *parts of this map's story*.
* A guard gets a guard point where it is put (before, another character's
  point: it walked away to it), and a guard group of the map when its
  behaviour comes from a level that had one the map lacks (BaseGuard (C1-1)
  on C2-2 now starts).
* A route setting (WayPoints, ...) has *New route (click its points)*: a
  route of the level's own, its points clicked at once.
* *Problems* lists a character whose behaviour cannot start (a route or a
  setting it needs is not set).

## 2026-10-25: one repository for the Hitman games

* The project is now **hitmanlevels** (github.com/nemannyira/hitmanlevels), made for Hitman 2 now and
  Contracts / Blood Money later: Hitman 2's editor in `h2/`, the shared parts in `shared/` and `tools/`,
  the site in `website/` (README.md).
* The package installer is `levelpack.exe` (it was `h2level_pack.exe`); a package names its game
  (`.h2lvl` = Hitman 2), and goes to that game's folder.
* The author key's folder is `%LOCALAPPDATA%\HitmanLevels` (a key made before moves there by itself).

## 2026-10-25: characters killed right after they were made

* A character made by a script (`h2.npc`) with its behaviour and killed at
  once could take the game down (seen on C5-3): fixed.

## 2026-10-24 (night): key backup, the Hub's client

* *Level > Share > Back up my key...* / *Restore a key...*: your author key in
  a password-locked file, so a new Windows or another PC keeps your
  fingerprint (`h2level_pack author backup|restore` too).
* Exported previews keep only the picture (no text or EXIF data); names and
  titles may not hold invisible or direction-changing characters; the export
  notes when a file mentions your Windows user name.
* The downloader for hitmanlevels.com is ready on the client side
  (`h2level_pack hub list|install`, docs/site_api.md): it stays off until the
  site and its signing key exist.

## 2026-10-24 (evening): levels passed on as one file

* *Level > Share > Export package*: the level as one signed file
  (`Mods\Packages\<level>_v<n>.h2lvl`), its scripts, README.md and
  preview.png with it. Your name, a license and your author key are asked
  once (h2/docs/editor.md, Share).
* `h2level_pack.exe` installs one (a double click): shows the author and the
  license, asks about authors not seen before, warns about another author's
  package of an installed level and about older versions, refuses changed or
  odd packages. Installed levels are downloaded levels (`Mods\Online\<id>`)
  and show their author in the launcher and the File menu.
* Very big levels could take the game down while the map loaded (or the
  load never ended): the level's objects used up the room the game keeps
  for the routines made while playing, such as the doors' actions. The
  editor now gives the map room for the level's own routines (found with
  about 1800 points shown on the map; h2/docs/roadmap.md, Engine limits).

## 2026-10-24: big levels, downloaded levels

* Big levels no longer take the game down (stress levels on C1-1, each limit
  found in the game's code; h2/docs/roadmap.md, Engine limits a level meets):
  * copies: the game keeps 510 moving objects in a map (it went down at about
    425 copies). The editor now makes room for 4096 and gives the light table
    room for the level's objects: a level's 2000 copies all move and are
    drawn, and a saved game of it loads. Past the room (never with copies
    alone) the others are not drawn, and *Duplicate* says so.
  * the game's map draws 256 icons on a floor (more took the game down when
    it was opened); a level shows at most 256 points on the map.
  * the level's records that would not fit what the engine makes while the
    map loads are left out.
* Reverting or loading a level with hundreds of objects ran the game out of
  memory (the editor took a snapshot of the scene for each object): fixed.
* A level downloaded later from the website will run untrusted
  (`Mods\Online\<id>`, docs/online_levels.md): less Lua memory and time, no
  dev tools or console while it runs, read-only in the editor (*Save as a
  level of your own* copies it), a strict check of its file. The check is
  also a program of its own: `h2level_check.exe [--strict] <level folder>`.

## 2026-10-23: tests, icons on the map

For those who work on the editor (h2/docs/debugging.md, Tests):

* `tools/tests/run_all.py` runs every test in one go: the saves backed up and
  put back, the dev commands on for the run only, the game started (and started
  again) as needed, a report in `dumps/test_runs`.
* `tools/tests/maps_gameway_test.py`: copies, points and character settings
  made with the map on other maps than C1-1.
* `tools/tests/gui_test.py` and `h2.dev_press(label)`: the editor's own buttons
  pressed by their labels.

* A point of the level can be shown on the game's map (M): Inspector > *On
  the map*, an icon of the map's legend; made with the map as the map's own
  icons are (h2/docs/roadmap.md, The in-game map).
* The game's map works on every level: with *No buildings* it showed
  nothing (its window and legend went with the buildings); now it keeps them,
  takes out only the old floor plans and draws the level's floor and models
  instead. On other maps the level's models can be drawn on it too (Level >
  *The level's models on the map*).
* *Empty map*: the characters taken out are off the game's map too (they were
  still drawn there at their old places).
* The game's map marks the level's mission: its targets (the map's own
  characters and the level's) as targets, its exit's point as an exit. The
  base level's targets are marked as enemies when the base level can no
  longer end the mission, and a character placed from a target is not a
  target unless the level's mission makes it one.

* A character's body can no longer be duplicated as a model (the copy took
  the game down: Hitman himself, some characters' bodies at the root).
* A level file is cut to what a level may hold (2000 placed objects, 5000
  object edits, 256 changed lists, 16 scripts named `name.lua`; the levels
  made so far hold far less). (A file with 2000 points with map icons took
  the game down while the map loaded: fixed 2026-10-24, above.)

Run in the game 2026-10-23: the map on C1-1, C2-1, C3-3, C4-1, C6-1, C7-1, a
blank map, a map of its own, saved games, and the Inspector's and Level
panel's controls through the GUI; the whole test set (run_all). Found and
then: a saved game of a level on C5-1 crashed when loaded (explained
2026-10-24: the test tools saved in C5-1's first seconds, while its start
camera moves; a save made later loads; h2/docs/roadmap.md). Levels online, a design:
docs/online_levels.md.

## 2026-10-17 .. 2026-10-22: the game's own way

The editor now does what the game itself does wherever the game has a way of
its own (found in hitman2.exe with Ghidra, checked in the running game).

### Made with the map

When a map loads with a level, these are made by the game as part of the
scene, like the map's own objects (before, the editor made them a moment after
the map had loaded):

* **Copies of the map's models**, of **groups of models** (a car model) and of
  **other levels' models**. The game treats them as its own: it offers its
  actions on them ("Take" for a take goal) and keeps them in saved games.
  A level saved before gets this from its next save on; until the map is
  loaded again, a copy made while editing is still a clone. The blank map's
  floor stays as it was.
* **Points** (routes, goals, the exit).
* **The settings of the map's own characters** (a weapon, a route): the game
  makes the character with them. Changed while playing, they are written into
  the running character until the map loads again.
* Characters, lists, zones and cars were made so already.

Paths of goals and routes set while an object was in a room still find it at
the map's root.

### Goals

* "Hitman has to take it" uses the game's own action (E, its "Take" line). The
  game offers such an action only around Hitman's chest height; an object on
  the floor is taken with E near it, with a hint.
* A taken object stays taken in a saved game.
* Kill / knock-out goals follow the game's reports of deaths.

### Lua

New, all calling what the levels' own scripts call:

| | |
|---|---|
| `obj:active([on])` | an object in or out of the world |
| `obj:send(message [, number])` | a message as the scripts send one (`"Msg_LockDoor"`, ...) |
| `obj:sound(name)`, `h2.sound(name)`, `h2.stopsound(id)` | the level's sounds (`"*/Male_Whistle1*"`) |
| `npc:draggable([on])` | whether Hitman can drag its body |
| `npc:noticed([on])` | whether the characters look out for it as for Hitman |
| `npc:unconsciousbody([on])` | the scripts' mark of an unconscious body |
| events `"zone"`, `"lock"`, `"button"`, `"elevator"` | Hitman in / out of the map's zones, doors locked, buttons, lifts |

`h2.give` now gives a weapon also when copies of it lie on the map.

### Other

* Characters are moved (setpos, the gizmo) with their own move, as the
  levels' scripts teleport them.
* Music zones get another track the game's way; hiding an object takes it out
  of its rooms as the game does.
* The Problems list offers *onto the paths* for a character or point off the
  path network when a place of it is within 8 m (undoable).
* Fixed: the Problems list no longer says a vehicle's route point is off the
  characters' path network (cars drive their own splines).
* Fixed: an object whose own name has a `#` (a copy of an imported
  "Truck_Body#0") was not found after it was made with the map.

### Checked

Every level in Mods (but My_level_8..10) loaded, saved and loaded again on a
copy; five also with a saved game: no exception, every pointer of the saves
placed. The test suites (`tools/tests`) all pass.
