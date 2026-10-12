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
| P5 | The Accountant | `h2.give(weapon)` while copies of that weapon lay on the map: drawing the given one crashed the game (RenderD3D, 0x474E82); reproduced 3 times, never without the copies | worked around: `h2.give` refuses then, with an explanation (the cause in the engine is open) |
| P6 | The Accountant | `h2.give("AmmoSniper")` added no ammunition (the clip stays empty): the game counts ammo only when it is picked up | worked around: `h2.give` refuses ammo with an explanation; docs/lua.md |
| P7 | The Accountant | The editor's *exit* goal reported the previous status message | fixed |
| P8 | The Accountant | Hiding the base level's laptop lines (h2.objective(n, false)) leaves their space empty above the level's own line; the base level's *Target Photo* stays in the laptop | open (cosmetic) |
