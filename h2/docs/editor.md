# Making a level

A level is a folder in the game's `Mods\` directory: `level.json` (what is
changed on one map of the game) and its Lua scripts. It is played by loading
its map; the editor applies the changes when the map has loaded.

F8 opens and closes the editor (H2Fix.ini `[Editor] ToggleKey`), F1 lists
every key.

## The steps

The top of the Level tab shows them, green when done, amber when something
is missing; a click goes where the step is done.

**Map**: in the main menu, *Level Editor > New level*: a map, a name, and whether
the map is emptied (*Empty map*: its characters, items and objectives go;
*No buildings either*: only the sky stays, with a floor to stand on). Or, on
a map already loaded: *File > New level on this scene*. *Open a
level* / *File > Open level* loads a saved one.

**Place**:

* *Models* tab: the map's models (and *Other levels...*: models of the other
  maps, they come with the map when it loads again). Click a model, then
  click in the world: Shift+click places more, the wheel turns it, Ctrl puts
  it on the grid, N stands it upright or along the surface.
* Click an object in the world to select it (the *Scene* tab lists them all):
  the gizmo moves (W) or rotates (E) it, G puts it onto the surface under the
  mouse, H hides it, Ctrl+D copies it, Del removes a copy. Ctrl+Z / Ctrl+Y
  undo / redo.
* *NPCs* tab: choose a character and a behaviour, *Place character*, click in
  the world. *Onto the paths* (on by default) puts it, and new route points,
  on the nearest place characters can walk (2 m at most); a red ring means
  none is near: it would stand there and not walk. *Testing > Path network*
  shows where they can walk. A selected character shows its routes; *Add
  points...* adds points to one. A route several characters share (a change
  applies to all) becomes the selected one's own with *A route of its own*:
  a copy of the list, made with the map when it loads again (*Test now*),
  whose points then change for that character only.
* *Vehicles*: select a car (a car of the map, or a copy of one) and
  *Inspector > Vehicle > Make it drive a route*: the route starts where it
  stands; click the road for its points (Esc ends); speed, *round and round*,
  *its front is its back* if it drives backwards, then *Test now (save and
  reload)*. The car is made with the map as the game's are (a ZCar on a path
  of its own, driven by the game's own mover: speeding up, slowing down,
  drawn in the rooms it is in, kept in saved games); the map's car (or the
  copy) it stands for is not there while it drives. A car of the map's that is a group
  (the C1-1 Ferrari) is copied whole with Ctrl+D or Duplicate; the game's own
  vehicles that follow their tracks (the C1-1 van) drive only those: the
  Inspector's *To its start* / *To its end* / *Stop* and a speed (Lua
  `h2.track`); a copy of one drives a route of its own.
* A placed character is real (behaviour, collision, can be shot) only after
  the map loads again: *Test now (save and reload)*.
* Duplicate copies models, not characters: a character's body (Hitman's,
  or a character's model lying at the root of a map) is refused, as its copy
  took the game down; place a character from the NPCs tab instead.
* A level holds at most 2000 placed objects (copies, items, points), 200
  characters, 5000 changed objects of the map, 256 points shown on the map
  and 16 scripts (named `name.lua`); a level file from elsewhere is cut to
  this when it loads.
* The game keeps 510 moving objects in a map (characters, cars, dropped
  things, and every copy: copies are moving objects); the editor makes room
  for 4096, so a level's 2000 copies all fit. Should it ever run out, the
  copies past it are not drawn (H2Fix.log says how many) and *Duplicate* says
  the map has room for no more. The game's map draws at most 256 icons on a
  floor.

**Mission**:

* *Mission* tab: the laptop's briefing and objective lines, what completes
  each (the level's own logic, a character's death or being knocked out,
  Hitman reaching a place, taking an object or bringing it to a place, never
  being spotted), the mission's end and the music.
* Quicker from a selection: the Inspector's *Mission* row: *Make it a
  target* (a character), *Hitman has to take it* (an object), *Hitman has to
  get here*, *Make it the exit*.
* An object to take can be anything placed or on the map. An item (a key,
  a weapon: *Models > Pickup items*) is taken when Hitman picks it up the
  game's way. Any other model (a briefcase, a stack of books) is taken when
  Hitman presses E near it (*how near*, 1.5 m by default): it goes from the
  world and the game shows "Taken.". Goals done stay done in saved games.
* *Hitman brings an object to a place*: an item is done when it lies at
  the place (dropped there: G, the game's Drop key); any other object is
  taken with E and put down at the place when Hitman gets there with it.
* *a character is knocked out*: done when its behaviour is unconscious
  (chloroform) and it lives. *Hitman is never spotted*: checked from the
  start, lost for good when a guard spots him (drawn weapon, trespassing,
  seen through the disguise); optionally that fails the mission.
* Points have names of their own: select one, type a name in the Inspector
  (e.g. *Hideout*) and *Rename*. Routes, goals and characters' settings
  follow it; scripts find it with `h2.find("Hideout")`.
* The game's map (M) marks the mission as the game's missions are: its
  characters to kill or knock out as targets, its exit's point as an exit.
* A point can be shown on the game's map (M): Inspector > *On the map* takes
  an icon of the map's legend (Exit, Point of interest, Pickup, Target, ...).
  It is shown from the map's next load (*Test*), on the floor the map shows
  for the point's room. The map's picture itself (the floor plans, their
  texts) is part of the map and does not change with the level; characters
  have their icons as before (a level's own characters too).
* The game's map draws the level's models from above (copies, other levels'
  models, the floor) on the floor of their place: Level > *The level's
  models on the map*, always so with *No buildings*, whose map loses its old
  floor plans with the buildings (its window, legend and the characters'
  icons stay). From the map's next load.
* A story of your own: Lua scripts. *Level > Scripts > New script from a
  template* writes one with the usual parts; *Run now* runs the scripts again
  without loading the map. The whole API: [lua.md](lua.md). The console
  (`` ` ``) runs Lua lines; Tab completes names.

**Test**: the toolbar's *Test* saves, loads the map again and starts it
with Hitman where he is, where the camera looks or at the level's start. The
*Testing* panel: game speed, freeze, god mode, invisible Hitman, the path
network (a mission completed after god mode, invisible Hitman or another
speed was on says so over the statistics: not a real rating). The Level tab's *Problems* list shows what would go wrong: characters
and route points off the path network, objectives whose target is gone,
missing script files, the scripts' errors since they last ran, and what the
game could not take of the level when the map loaded (objects left out or not
drawn, a full light table, more than 256 icons on a floor of the game's map). A character
or point off the path network with a place of it within 8 m gets an *onto the
paths* button: it moves there (undoable, kept in the level). A vehicle's route
points are not listed (cars drive their own splines).

**Save**: Ctrl+S or *Save* in the toolbar.

**A map of its own**: *Level > A map of its own* copies the level's map
under a new name (`Scenes\<Name>`, about 15 MB: the map's archive with its
entries renamed, its sounds) and makes the level that map's. The launcher
lists it as *<Name> (own map)*; it loads as a scene of its own, its saved
games are its own and the campaign's map is untouched. Everything placed
before stays (objects keep their paths), the laptop, texts and scripts work
as on the original. *Back to the original map* returns (optionally deleting
the copy's files, from another map).

**Play**: main menu > *Level Editor* > *Open a level* > *Play* (the editor
stays closed; *Edit* opens it on the map). Five example levels, played
through: `examples/missions` (copy a folder into `Mods`).

**Share**: *Level > Share > Export package* saves the level and writes one
file, `Mods\Packages\<level>_v<n>.h2lvl`, to pass on (its scripts go with it,
and `README.md` / `preview.png` when the level's folder has them). The first
time it asks your name and a license and makes your author key: the
packages are signed with it, so players see that a level and its updates come
from you (your name and a fingerprint such as `D046-8AD8-164A-AA81`). The key
stays on this PC, readable by this Windows user only
(`%LOCALAPPDATA%\HitmanLevels`); a new Windows or another PC makes a new key,
and players then see your next version as from another author.

Installing: a double click on the `.h2lvl` with `levelpack.exe` (or
`levelpack install <file>`). It shows the level, the author and the
license; an author not seen on the PC before is asked about, later levels of
the same author install without a question. Another key on a level that is
installed, or an older version, are warned about. A package that was changed
after it was signed, or holds anything else than a level, is refused. The
level goes to `Mods\Online\<id>` and plays like a downloaded one: its scripts
with less memory and time, no console; *Save as a level of your own* makes an
editable copy (with a new id: shared, it is your level).

*Back up my key...* writes your key, locked with a password, to
`Mods\Packages\author_<fingerprint>.h2key`; keep it and the password somewhere else too. *Restore a
key...* (shown while there is no key, or with *Change...*) brings it back on a new Windows or another
PC, so your next packages keep your fingerprint. The exported `preview.png` keeps only the picture
(its text and EXIF data stay home); names and titles may not hold invisible or direction-changing
characters; the export says so when a file mentions your Windows user name.

## Good to know (from playing levels through)

* The game offers "Pick up" for an item only while its own point is above
  the ground's collision: the editor puts placed items 3 cm above the
  surface. Moved by hand into the ground (or a long weapon on a slope) it is
  not offered: check the prompt when testing. Hitman picks up the first item
  of a pile: keep items apart.
* *No buildings* turned off again: the buildings come back; the floor it put
  under Hitman stays an object of the level (*Level* > *Take the floor out*).
* A level with *No buildings*: the map's own models and models of other
  levels (*Models* > *Other levels...*, as the floor) can be placed on it.
* Guards (BaseGuard) shoot a trespasser on sight: give the player cover, a
  way behind them or a disguise.
* Playing a level does not count in the game's profile (the campaign's
  saved progress, ratings, bonus weapons): the statistics screen still shows
  the rating, and its *Continue* goes to the main menu.
* On an emptied map the base level's laptop files (its *Target Photo*) are
  hidden; *Mission* tab > *The level's additional files* switches them.

## Where things are

| | |
|---|---|
| left | tabs: Scene (every object), Models, NPCs, Level, Mission |
| right | Camera (free camera, speed, view distance), Inspector (the selection), Testing |
| bottom | Lua console (`` ` ``) |
| toolbar | Objects / NPCs picking, Move / Rotate, World / Local, Grab, Snap, Undo / Redo, Save, Test, Freeze |

Tab hides the panels (the game's view only). The free camera: hold the right
mouse button and use W A S D, Q / E, Shift faster, the wheel for its speed.
