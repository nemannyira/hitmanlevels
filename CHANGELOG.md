# Changes

What changed for level makers, newest first. The details (addresses, the
game's functions) are in `h2/docs/roadmap.md`, `h2/docs/mission.md`, `h2/docs/lua.md`.

## 2026-11-14: version 0.1.0, sharing on the site

* A version: 0.1.0 (H2Fix.log's first line, the files' properties,
  `levelpack --version`).
* *Upload it on hitmanlevels.com* next to the export opens the site's
  upload page in the browser.
* `levelpack register` (the release's *Open .h2lvl files with
  levelpack.cmd*): a double click on a `.h2lvl` installs it;
  `levelpack unregister` takes it back.
* A downloaded level's plays are written down on the PC: each play of a
  level in `Mods\Online` leaves `runs\<started>.json` in its folder (the
  result, the time, the statistics screen's counters and rating, the
  testing aids used, the objectives done or lost, where Hitman was spotted
  or died). Nothing is sent; that comes with levelpack (docs/play_stats.md).
* Sharing those plays is up to the player: the launcher asks once when a
  downloaded level is picked (*Share them* / *Don't share*); `levelpack
  stats on | off | status | send` changes it and shows what was sent. When
  on, the game starts levelpack after a play to send it (the game itself
  still makes no connection). Plays with testing aids or dev settings, and
  plays of a level changed since it was installed, are never sent.
* levelpack.exe now goes into the game's folder too (README's install step).

## 2026-11-13: the level's picture is the game's view

* *Take a picture of this view* (Save & share) takes the game's own frame.
  It read the whole screen before: in a window, other programs' windows
  over the game or around it went into preview.png (#48). Take the picture
  again for a level made before this.

## 2026-11-09: the laptop's photos in packages

* The laptop's photos go with a shared level: `photo1.png` .. `photo5.png`
  of its folder, checked as its preview (a PNG of its picture only); a JPG
  or BMP chosen is written as a PNG. The picture is drawn exactly over the
  map's photo: the photo view no longer gets the widescreen field of view
  (the map's photos were drawn at 0.75 of their size on 16:9).
* A person's route in Simple mode starts where it stands (*start*): the
  game's walkers take a route's first point as their own place and go to
  the second first, so the first point clicked was skipped.
* knockout_test holds the attack until the guard drops, as a player does
  (the game's attack goes on only while the button is held); whether this
  was #38's cause is not seen in the game yet.

## 2026-11-02: route order, names, the laptop's photos, quicker lists

* People walk their route's points in order: the walkers that pick a point
  at random (WayPointList with `isWPListPath` 0, `bRandomPath`) are set to
  walk in order when placed, older ones get *Walk the points in order*;
  *Walks a route* no longer chooses a behaviour that always walks at random.
  Simple mode does not add a point people cannot reach (it is moved onto the
  paths within 6 m, or refused).
* The debug view's routes say which points a person reached, in which
  order, where it heads and why it walks out of order or stands still.
* Objects of the mission can be named (Inspector / card: *Name it*): the
  objective lines follow ("Kill the courier").
* The laptop's photos of the level's own: a picture file or the photo of a
  person or item as the lists show it, in place of the map's photo.
* The behaviours of other levels are read in the background at the start
  (the People step opened with a 0.4 s wait before); editor stalls over
  40 ms are logged with their place (`slow: ...` in H2Fix.log).

## 2026-11-01: the debug view while playing

* F9 while playing is a debug view now (Try it / *Testing*: *Debug view
  while playing*), its parts each switched on or off: the routes (as
  before); what each person does, how it reacts, the suspicion it reports
  and whether it sees Hitman; how far and how wide each one sees (the
  game's own sight values, red while Hitman is seen); the objectives with
  the alarm level and the latest events in the corner; people with a route
  standing still for 15 s.
* *What happened in the last test*: back in the editor, the test's events
  with their time; a click takes the camera there (Advanced: *Last test*).

## 2026-11-01: back from a test, routes while playing

* F8 while a test plays goes back to the editor: the map loads again as
  the level makes it (not as it was played), the editor opens on it, the
  free camera where it was. A line at the top of the screen says so.
* F9 while playing (Try it: *Show the people's routes while playing*;
  Advanced: *Testing > Routes while playing*): the routes the people walk,
  each in its colour, points numbered, arrows halfway along each leg, the
  name above the head; a point off the paths red. `[Editor] RoutesKey`.
* Characters' pictures no longer squeezed (their draws keep the screen's
  shape: the width stretched back); pictures in the middle of the tooltip.

## 2026-11-01: routes walked, characters' pictures

* Routes seen while they are made: a new route was not drawn at all (it has
  no record of the scene file), so its clicked points left no trace. Now its
  points are numbered with arrows in walking order; Simple mode leaves out
  the shared help and safe points' lines across the map.
* Simple mode's *Route of ...* box while its points are clicked: how many,
  *Take back the last one*, *Clear*, *Done (Esc)*; *Route* beside a new
  person who walks (the level's people, its card) changes it later.
* Advanced mode: a route setting without a route has *New route...* in the
  character's *Routes* (it was only in the setting's menu).
* *Walks a route* did nothing: the route was looked for in the placed
  character's settings, which a character not made yet has not. The route
  is now made before it is placed, its points clicked right after.
* New routes copy a ZConditionList of the map first (a list the pedestrians
  walk; a guard walks one too): a pedestrian of another level walking a copy
  of a plain list (C1-1's Cricket_Volume_Control) took the game down.
* The characters' pictures: in front of the camera, facing it, the part of
  the picture they stand in enlarged (a character's draws keep the camera's
  view); pictures 512 pixels. The People step's looks: the same model with
  more behaviours says which ("Bodyguard Mafia - Door Guard").

## 2026-10-31: Simple mode: the original objectives, the camera, pictures

* The original mission's objectives can be changed (*Change*: an objective
  of yours on the line) or removed (off the laptop, not counted; the mission
  then ends with your objectives); *Put back*. Level files: an objective's
  `"hidden": true` (Advanced mode: *taken out*).
* The toolbar's *Free camera* and *To Hitman*.
* *Walks a route* offers a behaviour of another level's that works anywhere
  when the map's own people walk no routes (most maps), and says why when
  there is none.
* The mouse over a character (People step, NPCs tab) or an item (Items
  step, Models tab) shows a picture of it; item pictures were empty (the
  templates' parts are inactive in the scene: their clones are made
  active). The Items step has a search field.

## 2026-10-31: fixes from a walkthrough of every part

* A placed character removed (the People step's Remove, the NPCs tab's,
  a level reverted) left the whole world undrawn: it was taken out of the
  actors' collision lists. Now it is made inactive as the levels' scripts
  take a character out.
* Simple mode: the map's own hidden characters (C2-2's limousine's people)
  were listed as taken out, with a *Put back* that would have shown them;
  an objective's buttons ran off the panel (its *Remove* unreachable);
  Hitman selected was offered as an object to take, hide or copy (and H hid
  him); with no level open (the intro, the menu) the guide offered to edit
  objectives; a failed save said nothing; an escalation's stage shown is
  named; items keep their numbers ("Ammo Cal 50") and ammunition is a group
  of its own; the launcher's text follows the mode.

## 2026-10-31: no bodies found (#30)

* One more rule of an escalation's stages: *no bodies found*: a body (dead
  or knocked out) seen by a character, as the game's own sight tells it.

## 2026-10-31: Simple mode, more (#35, #36)

* Doors of the map can be locked (Simple mode's card and The map step,
  Advanced mode's Inspector: *door locked*): a locked door opens only for the
  holder of its key, as the game's own; kept in saved games. Scripts:
  `obj:locked([on])`.
* The map step lists what was changed, each with *Undo this*; a line's
  laptop text can be written by hand; the Problems list has *fix it there*.
* Step 6: a description (README.md) and a picture of the view (preview.png)
  for the package.

## 2026-10-31: Simple mode (#31)

* The editor's second face for small changes of a mission: a guide of six
  steps (Mission, People, Items, The map, Try it, Save & share), objectives
  as sentences, people added by who they are and what they do, the map's
  items by kind, a card beside the selected object with what can be done
  with it. *New level* asks Simple or Advanced; the toolbar switches.
* Advanced mode is the editor as before (plus the *Simple mode* switch).

## 2026-10-31: rules of the weapon in hand (#28)

* Two more rules of an escalation's stages: *targets killed only with...*
  (the fiber wire, a knife, close combat, a pistol, a firearm, or one weapon
  of the map) and *no firearm drawn*.
* Scripts: `h2.hand()`, the item in Hitman's hands and its kind.

## 2026-10-30: escalations, levels in stages (#23-#27)

* A level can be an escalation: played in stages (1 to 10), one after
  another, each the one before and more (Mission tab > *Stages*). The
  toolbar's *Stage* buttons show and edit a stage; what is placed comes in the
  stage shown; the Inspector's *stages ... to ...* row says in which stages an
  object, character, map object or objective is there.
* Rules of the stages: never be seen, a time limit, no innocents killed, no
  disguise, one disguise at most; each fails the stage or is only noted. The
  laptop's briefing and the screen show the stage's name and rules.
* The launcher shows an escalation's progress and the stage to play; a stage
  done opens the next; replaying the stages done is the author's choice. A
  saved game keeps its stage.

## 2026-10-26: the map from above (#22)

* The free camera above the map shows it from above (before: grey): the open
  places and the roofs; *From above: inside the buildings* in the Camera
  panel shows the floors inside the buildings instead.

## 2026-10-26: smaller things from the walkthrough (#21)

* Level tab: the map's settings under *The map*; *Share* and *A map of its
  own* at the end; *Hitman starts in* moved to the Mission tab; a script has
  *open* (the program .lua files open with, Notepad else).
* Launcher: the floor is offered only with *No buildings either*; the folder
  the level is saved in is shown as it will be.
* A long briefing line is shown wrapped under its box; texts that ran out of
  the panel wrap.
* The *Test* step is not green while there is nothing to test.
* No *Vehicle* section for items.
* The free camera is turned off with the editor (the game's camera again).
* On an emptied map the laptop's empty *Additional files* boxes are hidden.

## 2026-10-26: the NPCs tab easier to read (#20)

* Settings have plain names (*Hit points*, *Weapon*, *Guard point*, *Guard
  group*, *Route*, ...; the game's own name in the tooltip).
* The behaviour lists are taller and have a filter (a name, a word of the
  description, a level: "patrol", "C3-1").
* *Place another <model>* at the top places the next character of the kind
  placed last, without scrolling down.
* Setting rows have more room; the last "placed ..." message goes with the
  map.

## 2026-10-26: finding models of every level, characters with names (#19)

* *Models > Other levels...* has *every level* in its level list: three
  letters of a name ("case", "book") list the models of all the other levels
  with it, each with its level.
* A placed character can have a name of its own (*NPCs*, under its buttons:
  *Name it*): the lists and the objectives made for it use it ("Kill
  Volkov"), and scripts find it with `h2.find("Volkov")`.

## 2026-10-26: no bonus weapon popup after an editor level (#18)

* A level of the editor completed with Silent Assassin no longer shows the
  campaign's "awarded a bonus weapon" box: nothing was awarded (the profile
  is kept from editor levels).

## 2026-10-26: opening a level of another map (#16)

* *File > Open level* lists each level's map, loads the map of a level made
  on another one (as the launcher does), and asks first when the open level
  has unsaved changes.
* *Test* of a level whose map is not the loaded one starts Hitman at the
  level's start (before: at his place on the other map).

## 2026-10-26: an emptied map's mission is the level's (#17)

* A map emptied (launcher's *Empty map*, or the Level tab's) starts with
  *The base level can end the mission* off and *Completed when every
  objective is done* on, so the exit can be chosen at once.
* While the base level cannot end the mission, its EXIT is not on the game's
  map (M).

## 2026-10-26: characters in settings, placed characters shown (#15)

* A setting's *Pick in the world* takes the character clicked (before: the
  ground behind it), and its menu has *A character* (every character, also
  far away): a bodyguard protects your own target, kept when the map loads.
* A character placed while the world stands (the editor open) is drawn at
  once (before: only its marker when selected).

## 2026-10-26: scripts and the Mission tab's laptop lines (#14)

* *New script from a template* uses the first laptop line the Mission tab
  does not (before: line 1, which its start zone ticked off: the tab's
  objective 1 showed done at once).
* *Problems* says when a script sets a line the Mission tab uses.

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
