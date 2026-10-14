# Playtest log (2026-10-12)

Five levels were made with the editor from start to end (examples/missions)
and played through as a player: Hitman walked with the keys, steered with the
mouse, picked up and used items through the game's own input (tools/player.py
drives the game: the same keys and mouse a player uses). Every problem found
is listed here with what was done about it.

Legend: **fixed** (in the commit named), **worked around** (the cause stays;
users are guided around it), **open**.

## GUI review (before the levels)

| # | Finding | Status |
|---|---|---|
| G1 | Toolbar switches without explanations (Objects / NPCs, World / Local, Snap, Pick invisible); Undo / Redo did not say what they would undo | fixed: tooltips, "Undo move (Ctrl+Z)" |
| G2 | Two controls with near names: toolbar *Freeze* (world stands while editing) and Testing *Frozen* (world stands while playing) | fixed: *Freeze while playing* |
| G3 | The Inspector led with the engine's internals (class chain, pointers, flags) | fixed: folded under *details* (with *copy path* for scripts) |
| G4 | In the Level lists the destructive button came first (*delete* / *revert* before *select*): easy to hit by mistake | fixed: *select* first, *delete* / *revert* last with "Ctrl+Z undoes" |
| G5 | File menu jargon (*Apply to the scene*, *Revert the scene*, *Apply automatically*); taking the level off the map was not asked and not undoable | fixed: *Put the level on this map*, *Show the map without the level...* (asked), *Put the level on the map when it loads* |
| G6 | *Remove this objective* was immediate and not undoable | fixed: asked first |
| G7 | The ImGui demo window in the View menu (a developer's tool) | fixed: only with dev commands on |
| G8 | Snap step field had a fixed pixel width (cut with large fonts) | fixed: follows the font |

## Found while making and playing the levels

| # | Level | Finding | Status |
|---|---|---|---|
| P1 | all | A player could not *play* a level: the launcher's *Open* always opened the editor on it (world frozen, F8 to leave) | fixed: *Play* (editor stays closed) and *Edit* in the launcher |
| P2 | The Accountant | Problems listed the mission's exit point as "off the path network": a place Hitman goes to does not need the network | fixed: points used as the exit or a place to reach are not checked |
| P3 | The Accountant | *Fly camera here* (F) and the other *Fly to* buttons moved the camera but kept its old direction: it often looked away from the object (here at a cliff) | fixed: one FlyCameraTo, the object in the middle of the view |
| P4 | The Accountant | A long weapon laid on the sloping hilltop could not be picked up (no "Pick up" prompt; a key, ammo, and the same rifle on flat ground could) | explained (2026-10-13, P22): an item is offered only while its own point is above the ground's collision; placed items are put 3 cm above the surface, docs say to check the prompt |
| P5 | The Accountant | `h2.give(weapon)` while copies of that weapon lay on the map: drawing the given one crashed the game (RenderD3D, 0x474E82); reproduced 3 times, never without the copies | fixed (2026-10-13): the game's loop placing a weapon's parts (0x474E70) used a part reference without checking it; a dead one now sends the loop to its next part (the reproduction no longer crashes). `h2.give` still refuses then, as a precaution |
| P6 | The Accountant | `h2.give("AmmoSniper")` added no ammunition (the clip stays empty): the game counts ammo only when it is picked up | worked around: `h2.give` refuses ammo with an explanation; docs/lua.md |
| P7 | The Accountant | The editor's *exit* goal reported the previous status message | fixed |
| P8 | The Accountant | Hiding the base level's laptop lines (h2.objective(n, false)) leaves their space empty above the level's own line; the base level's *Target Photo* stays in the laptop | fixed (2026-10-13): the lines shown close up (a hidden line leaves no gap), "saves left" under the last; the *Target Photo* stays (the base level's file) |
| P9 | Dead Drop | On an emptied map the laptop lines a script gives (`h2.objective(1, "...")`) stayed empty: the editor replaced every line it did not own with an empty text, the script's own too | fixed: the editor's and scripts' lines are kept |
| P10 | Dead Drop | An emptied map showed empty bullets for the base level's own lines | fixed: those lines are hidden |
| P11 | Dead Drop | Placed BaseGuard guards shoot Hitman on sight in his suit: a level with them is hard (died twice) | as designed (the game's guards); README says how to approach |
| P12 | Estate Run | A loop over `h2.objects(...)` stopped with "object ... is not in the loaded scene" on `o:name()`: a sound object the game made for a moment was gone by then | fixed: `name()` reads the name from the path (no live object needed) |
| P13 | Estate Run | `h2.spawn(source, x, y, z)` copies stayed unseen: they went into the source's room (a garage lamp's copy on the road was in the garage room, drawn only from inside) | fixed: a copy goes into the room of its place |
| P14 | Estate Run | After `setpos` across the map Hitman sometimes fell through the ground (y -39009) and died; the game's pause menu then stops the clock | guarded (2026-10-13): not seen again in over 100 moves (standing, running, turning, right after a load); for 4 s after a move Hitman is put back on the place if he falls 5 m below it (logged) |
| P15 | Estate Run | Completing a level counts as completing its base map in the profile (here C1-1's Silent Assassin bonus weapon was awarded) | open (known: docs/roadmap.md section 3, a map of its own); fixed 2026-10-14: see below |
| P16 | Estate Run | A subtitle and an info line share one place on the screen: the first "seconds left" info hid the start subtitle | a script's care (docs/lua.md says so); the example fixed |
| P17 | Arena | On a map with *No buildings*, copies of the map's own models (Models tab, Ctrl+D, `h2.spawn`) are not drawn, though placed, not hidden, in the right room; the same copies on the normal map are | fixed (2026-10-13): a copy of a hidden object is never drawn, also when shown later; the blank map's models are shown while being copied (the copy keeps its model's collision as before) |
| P18 | Arena | A copy placed at a position (`h2.dev_edit("dup")`, the editor's duplicate at a place) went into its source's room: far from it, unseen | fixed: the room of its place, as points |
| P19 | Arena | Three targets of one model got the same laptop text ("Kill BodyguardMafia_01 (placed)" three times) | fixed: numbered (2), (3) |
| P20 | Arena | A guard standing on the floor from another level fell through it (it is made a moment after the level's characters) and stood 2 m below | fixed: shortly after the level is applied, its characters far below their place are put back |
| P21 | Arena | On the blank map the level's *MissionCompleted* stops on an assert (int3, caught) after the end screen came up; the editor's goal check then called it again every quarter second (49 times) | fixed: once per map; the exception is logged |
| P22 | Anathema Reworked | The ledger (an item placed on the road) sank 30 cm into the ground when the level loaded and was never offered to pick up, also after lifting it (the same as P4: items the level makes while the map loads) | fixed (2026-10-13): two causes. The script took the ledger's model for the item (`h2.findall` finds both; the examples now check `o:isa("ZItem")`), and the test player stopped 1.5 m away (the game offers items from about 1 m). The ledger is picked up the game's way now |
| P23 | Anathema Reworked | A goal's place (a point) was in another room after the map loaded again than when the goal took its path: "its place is missing", the goal could not be done | fixed: an object the level made is found by its own name ("Point+1") when its path changed |
| P24 | Anathema Reworked | The editor's own mission end counts only its goals, not a script's lines | by design: a level mixing both ends from its script (the `objective` event reports the editor's goals); the example shows how |

## How the levels were played

The test player (tools/player.py) uses the game's own input: W / Shift to
walk and run, the mouse to steer and aim, E to pick up, the number keys for
weapons, the left button to fire, B for the laptop. Its steering cannot find
its way around slopes and walls, so long ways were travelled by moving
Hitman (`setpos`): the last metres, picking up, entering zones and shooting
were done with the keys. Its aim was not good enough to hit a walking target:
those shots were stood in for with `npc:kill` (any death completes a kill
objective, as the player's shot does). Levels whose guards shoot on sight
were played once as they are (the guards won) and then with *Invisible
Hitman* to follow the story to its end.

## Regression after the fixes

All on the last build: API suite 74/74, security 41/41, stress 6/6, editor
test 34/34, kill switch 5/5, save / load 11/11, hostile level files 14/14
(the game survived every one). The launcher's *Play* was used from the main
menu to start The Informant: the editor stayed closed, the level ran.

## The levels

| Level | Map | What it tries | Played |
|---|---|---|---|
| The Accountant | C1-1 as it is | a walking target, editor goals (target, exit), the base level's guards | completed (Professional) |
| Dead Drop | C1-1 emptied | an item to deliver, guards, a time limit, being seen | completed; failed by time; killed by the guards |
| Estate Run | C1-1 emptied | checkpoints in order, markers, a clock | completed with 20 s to spare |
| The Informant | C1-1 as it is | an escort who follows Hitman | completed; failed (he died); failed (left behind) |
| Arena | C1-1, no buildings, C3-3 yard | a level built in the sky, three armed targets, an exit | completed (Silent Assassin) |
| Anathema Reworked | C1-1 reworked | six objectives of six kinds (item, disguise, rule, time, kill, place), start outfit, exit | completed (the ledger pickup stood in, P22; picked up the game's way after the fix, 2026-10-13) |

## 2026-10-13

P5 fixed in the engine (the weapon parts loop); the start outfit (Level tab >
*Hitman starts in*, `h2.outfit`) added and tested (postman, bodyguard); the
big level made and played through. API 74/74, security 41/41 on the last
build.

## 2026-10-13 (day): objects to take, the open findings

New: **Hitman takes an object** as a mission goal (Mission tab, or the
Inspector's *Hitman has to take it*). An item is taken when Hitman picks it
up; any other model (placed or the map's own) when he presses E near it
(it goes from the world, "Taken."). The editor's goals done (a target
dead, a place reached, an object taken) are kept with saved games (the
editor's part of `Save\<slot>.h2lua`). Points can be renamed in the
Inspector (routes, goals and characters' settings follow).

Played as a user: a key placed from *Models > Pickup items* on the road,
*Hitman has to take it*, saved, the map loaded, Hitman walked to it and
pressed E: objective done. The new test `tools/tests/goals_test.py` plays a
blank-map level with an item, a copy of a map model and a renamed point,
saves and loads (22 checks).

| # | Found | Status |
|---|---|---|
| P25 | An item placed from the Models tab landed kilometres from the click: its placement preview is made next to its source, under the item template; made again in the room under the mouse, the first preview counted as a second part of the template, and the item was placed at the middle of the two | fixed: the editor's own copies are never parts of a model |
| P26 | Loading a saved game of a *No buildings* level took out only 3 models (of 3617): the saved game has the map's models hidden, so they were taken for hidden by the map; their collision stayed | fixed: what the map itself hides is noted before a saved game's state is loaded |
| P27 | On a *No buildings* level Hitman sometimes fell through the floor from another level: taking the map's models out also took out the editor's library of other levels' models, whose collision is the floor's | fixed: the library is left alone |
| P28 | A new *No buildings* level made while Hitman was falling (a blank map without its floor) put the floor where he had fallen to | fixed: then under the place he came in at |
| P29 | The editor's "reach a place" goal done was lost by loading a saved game (its check mark too) | fixed: kept with the saved game (as above) |


## 2026-10-14: the last open findings

| # | Found | Status |
|---|---|---|
| P15 | Completing a level counted as completing its base map in the profile (C1-1's Silent Assassin bonus weapon), and *Continue* went on to the campaign's next mission | fixed: the profile is the global variables of CGlobalCom; while a level of the editor is on the map the statistics screen does not write Save\<n>.blap, the variables are put back when the map is left, and *Continue* goes to the main menu. Checked: the same completion without a level wrote the profile and awarded the weapon; with a level the profile stayed byte for byte (`tools/tests/profile_test.py`, 8 checks) |
| P8 | The base level's *Target Photo* stayed in the laptop | fixed: on an emptied map its files are hidden (their texts empty, a click on the entry opens nothing: BriefingMenu action 0xC9..); *Mission* tab > *The level's additional files* (level.json `base_files`) shows or hides them on any map |

## 2026-10-14: more goal types

New editor goals: *knocked out* (the character's behaviour state
`Being2.Unconscious`: the game reports knock-outs to the level only as a
script event without the victim), *brings an object to a place*, *never
spotted* (the "spotted" / "hostile" reactions; optionally fails the mission).
Played: never spotted checked at the start, lost when a pistol was drawn in
front of two guards ("Thread_Angry"); a model taken with E was put down at
its place when Hitman got there; a key held at its place showed "Drop it
here" and was not counted. Not played: a real knock-out (the test player
could not get chloroform to take: the guard turned and attacked), and an
item dropped at its place (no drop key found for the test player).

## 2026-10-14: vehicles

The game's vehicles move along splines (ZMoveOnSpline: a mover object at
+0x5C puts the vehicle on its spline every frame; the scripts drive it
through Car_If / Vehicle_If / MoveOnSpline_if methods SetPath,
SetMoveParams, DriveFwdToSplineVal, MoveToSplineVal). Those are not used:
the editor drives objects itself (src/editor/vehicles.h), and copies groups
whole (a car: a new group with copies of its parts).

| # | Found | Status |
|---|---|---|
| P30 | Groups (a car) could not be copied ("only objects with a mesh") | fixed: a group copy (level.json kind "group") |
| P31 | A group copy's parts went kilometres away: a map's group is at the world's origin, its parts where they stand | fixed: a group's place is its parts' middle, at the bottom of their boxes |
| P32 | Parts made dynamic under a new group crashed the engine's dynamic set (0x5BCC04) | fixed: only the group is dynamic (as the game's vehicles) |
| P33 | A driven car went sideways: the Ferrari's long side is its x | fixed: driven along its long side (`reverse` for the other end) |

Played: a Ferrari copy driving a loop of four points on the C1-1 road,
saved with the level and driving again when the map loaded (headlights
first); `h2.drive` with `speed` / `progress` / the "arrived" event.

## 2026-10-14: a map of its own

The car level (_Cars, a Ferrari copy driving a route) made a map of its own
(CarTown): loaded from the launcher's *Play*, saved and loaded again, back to
the original map.

| # | Found | Done |
|---|---|---|
| P34 | On its own map a level's placed objects were not made (0/5): each remembers the scene it was placed on | fixed: the original map counts as the copy's |
| P35 | The launcher showed an own map as its scene name | fixed: *CarTown (own map)* |

## 2026-10-14 (evening): the goals played for real, VS 18

The DLL built with VS 18 (MSVC 19.52) passed every suite. The two goals not
played before, played: a revolver picked up, carried to the place and dropped
with G (the game's DropItem key; close combat items are drawn with 1); the
C1-1 garage guard knocked out from behind with chloroform (Hitman invisible).

| # | Found | Done |
|---|---|---|
| P36 | A knocked out character never completed its goal: the engine counts it as not alive (Actor IsAlive) and the goal wanted it alive | fixed: knocked out = its unconscious state, alive or not for the engine |
| P37 | The same made a knock-out complete a "dies" goal, fire the scripts' death event and `npc:alive()` false | fixed: not a death; killed afterwards it is (tested: knocked out, then the instant death) |
| P38 | `npc:kill()` did nothing to a knocked out character | fixed: its behaviour gets the instant death || P39 | A saved game of a level with characters of other levels (CharTest: C1-1's maid and bodyguard, C3-2a's ninja on C2-1) did not load: the game stopped at an int3 in ZLoadGame::GetSafePointer. The outfit groups made for imported bodies (`<model>_HMas` / `_Nude`) keep their names in the engine's heap, and ZSaveGame::AddSafePointer writes such a pointer as kind 0 | fixed: their names are saved as the template's groups' (in the scene's static buffer); a pointer the save cannot place is logged ("save: a pointer of no kind"). Tested: saved, loaded, the ninja's clothes taken after loading, saved and loaded again in them |
| P40 | *No buildings* turned off: the buildings come back, the floor it put under Hitman stays and cuts through the map, with nothing saying it is an object of the level | fixed: the Level panel offers *Take the floor out* (level.json `floor_object`); undoable |
| P41 | A placed character with a body of another level was listed under the name of the character lending it its behaviour ("DeliveryBoy" for C0-1's Vittorio) | fixed: by its own body ("Vittorio (C0-1)") |

Also played through the GUI (mouse and keyboard, no dev commands): the
launcher's *No buildings either* with another floor (C2-2's snowy ground),
*Models* > *Other levels...* > *Characters* > Vittorio (C0-1) > *Load the
map again*, *NPCs* > Vittorio > *Place character* > a click on the floor >
*Test now*: a real character there (Neutral2).

| # | Found | Done |
|---|---|---|
| P42 | A map loaded with the editor open (*Load the map again* for models of other levels) showed the world from inside Hitman's head: the editor held the world from the first frame, before the game's camera moved behind him | fixed: a new map runs 60 frames before the editor holds it |

