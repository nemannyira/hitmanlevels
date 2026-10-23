# Changes

What changed for level makers, newest first. The details (addresses, the
game's functions) are in `docs/roadmap.md`, `docs/mission.md`, `docs/lua.md`.

## 2026-10-23: tests, icons on the map

For those who work on the editor (docs/debugging.md, Tests):

* `tools/tests/run_all.py` runs every test in one go: the saves backed up and
  put back, the dev commands on for the run only, the game started (and started
  again) as needed, a report in `dumps/test_runs`.
* `tools/tests/maps_gameway_test.py`: copies, points and character settings
  made with the map on other maps than C1-1.
* `tools/tests/gui_test.py` and `h2.dev_press(label)`: the editor's own buttons
  pressed by their labels.

* A point of the level can be shown on the game's map (M): Inspector > *On
  the map*, an icon of the map's legend; made with the map as the map's own
  icons are (docs/roadmap.md, The in-game map).
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
  made so far hold far less). Still open: a file with 2000 points with map
  icons takes the game down while the map loads; the engine's capacity is to
  be measured (docs/online_levels.md, section 5).

Run in the game 2026-10-23: the map on C1-1, C2-1, C3-3, C4-1, C6-1, C7-1, a
blank map, a map of its own, saved games, and the Inspector's and Level
panel's controls through the GUI; the whole test set (run_all). Found and
open: a saved game of a level on C5-1 crashes when it is loaded (older than
these changes; docs/roadmap.md). Levels online, a design:
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
