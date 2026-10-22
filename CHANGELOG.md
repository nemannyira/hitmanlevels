# Changes

What changed for level makers, newest first. The details (addresses, the
game's functions) are in `docs/roadmap.md`, `docs/mission.md`, `docs/lua.md`.

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
| `obj:sound(name)`, `h2.sound(name)`, `h2.stopsound(id)` | the level's sounds (`"*\\Male_Whistle1*"`) |
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
* Fixed: an object whose own name has a `#` (a copy of an imported
  "Truck_Body#0") was not found after it was made with the map.

### Checked

Every level in Mods (but My_level_8..10) loaded, saved and loaded again on a
copy; five also with a saved game: no exception, every pointer of the saves
placed. The test suites (`tools/tests`) all pass.
