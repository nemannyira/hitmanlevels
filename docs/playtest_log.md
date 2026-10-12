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
| P4 | The Accountant | A long weapon laid on the sloping hilltop could not be picked up (no "Pick up" prompt; a key, ammo, and the same rifle on flat ground could) | worked around: the game's own range check; docs/editor.md says to lay weapons on flat ground and to check the prompt when testing |
| P5 | The Accountant | `h2.give(weapon)` while copies of that weapon lay on the map: drawing the given one crashed the game (RenderD3D, 0x474E82); reproduced 3 times, never without the copies | fixed (2026-10-13): the game's loop placing a weapon's parts (0x474E70) used a part reference without checking it; a dead one now sends the loop to its next part (the reproduction no longer crashes). `h2.give` still refuses then, as a precaution |
| P6 | The Accountant | `h2.give("AmmoSniper")` added no ammunition (the clip stays empty): the game counts ammo only when it is picked up | worked around: `h2.give` refuses ammo with an explanation; docs/lua.md |
| P7 | The Accountant | The editor's *exit* goal reported the previous status message | fixed |
| P8 | The Accountant | Hiding the base level's laptop lines (h2.objective(n, false)) leaves their space empty above the level's own line; the base level's *Target Photo* stays in the laptop | open (cosmetic) |
| P9 | Dead Drop | On an emptied map the laptop lines a script gives (`h2.objective(1, "...")`) stayed empty: the editor replaced every line it did not own with an empty text, the script's own too | fixed: the editor's and scripts' lines are kept |
| P10 | Dead Drop | An emptied map showed empty bullets for the base level's own lines | fixed: those lines are hidden |
| P11 | Dead Drop | Placed BaseGuard guards shoot Hitman on sight in his suit: a level with them is hard (died twice) | as designed (the game's guards); README says how to approach |
| P12 | Estate Run | A loop over `h2.objects(...)` stopped with "object ... is not in the loaded scene" on `o:name()`: a sound object the game made for a moment was gone by then | fixed: `name()` reads the name from the path (no live object needed) |
| P13 | Estate Run | `h2.spawn(source, x, y, z)` copies stayed unseen: they went into the source's room (a garage lamp's copy on the road was in the garage room, drawn only from inside) | fixed: a copy goes into the room of its place |
| P14 | Estate Run | After `setpos` across the map Hitman sometimes fell through the ground (y -39009) and died; the game's pause menu then stops the clock | open: seen with the test player's long moves only; a story script should move him to a spot near him, or test it |
| P15 | Estate Run | Completing a level counts as completing its base map in the profile (here C1-1's Silent Assassin bonus weapon was awarded) | open (known: docs/roadmap.md section 3, a map of its own) |
| P16 | Estate Run | A subtitle and an info line share one place on the screen: the first "seconds left" info hid the start subtitle | a script's care (docs/lua.md says so); the example fixed |
| P17 | Arena | On a map with *No buildings*, copies of the map's own models (Models tab, Ctrl+D, `h2.spawn`) are not drawn, though placed, not hidden, in the right room; the same copies on the normal map are | open; partly: such a copy gets its model's collision back (taking a model out is one setting for all its objects: the map's own ones that stay out are moved under the map until it is restored). Build blank maps with models of other levels (as the floor) |
| P18 | Arena | A copy placed at a position (`h2.dev_edit("dup")`, the editor's duplicate at a place) went into its source's room: far from it, unseen | fixed: the room of its place, as points |
| P19 | Arena | Three targets of one model got the same laptop text ("Kill BodyguardMafia_01 (placed)" three times) | fixed: numbered (2), (3) |
| P20 | Arena | A guard standing on the floor from another level fell through it (it is made a moment after the level's characters) and stood 2 m below | fixed: shortly after the level is applied, its characters far below their place are put back |
| P21 | Arena | On the blank map the level's *MissionCompleted* stops on an assert (int3, caught) after the end screen came up; the editor's goal check then called it again every quarter second (49 times) | fixed: once per map; the exception is logged |

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
