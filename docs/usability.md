# Making levels easier to build (2026-10)

What gets in the way when a level is made with the editor, seen while
building and testing (the Courier example, the editor tests), and what is
proposed. Done items are marked.

## What is hard now

* **Where characters can walk is invisible.** A route point or a walk target
  off the path network is simply not reached; it shows only when testing.
  (Testing panel > Path network draws the network; h2.onpath tells.)
* **Mistakes show late.** A route that leads nowhere, an objective without
  its target, a script file missing, models of other levels waiting for the
  map to load again: each is found by testing and reading H2Fix.log.
* **Long, cut names.** Lists showed object paths
  (`ROOT/C1-1/C1-1_LOCATIONS.zip/...`), cut at the panel's edge. (done: names)
* **Narrow panels cut their rows** (the camera's view distance, the testing
  speeds). (done: the right column is at least 19 font sizes wide)
* **The camera started inside Hitman** after a map loaded. (done)
* **Scripts start from nothing.** A level's story is written in Lua with
  docs/lua.md beside it.

## Proposals, by value

1. **Off the network, in red** (done): route points and characters placed
   where the path network does not reach are drawn red with a warning; the
   Level panel's problem list names them.
2. **Problems** (done, Level panel): the level checked as it is edited -
   characters and route points off the path network, objectives without a
   target, level scripts missing, models of other levels not loaded yet,
   unsaved changes - each with a button that selects the object.
3. **Script templates**: "New script" in the Level panel writes main.lua from
   a template (an objective and its check, a timed event, a character that
   walks a round and drops an item), so a story starts from working code.
4. **Shortcuts sheet** (F1): the editor's keys (move / rotate / local /
   grab / snap / undo / test / freeze / speed) in one overlay.
5. **One workflow line** at the top of the Level panel: Map > Place >
   Mission > Test > Save, each step's state shown (what is still missing).
6. **Placing on the network**: an option that snaps route points and
   characters to the nearest walkable cell (engine::PathCellBelow).
7. **Mission goals from the selection**: "make this the target" / "make this
   the exit" on the selected object (the Mission panel asks for a pick now).
