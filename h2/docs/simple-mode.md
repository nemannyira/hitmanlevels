# Simple mode

The editor today shows everything it can do at once: five tabs on the left
(Scene, Models, NPCs, Level, Mission), three panels on the right (Camera,
Inspector, Testing), a toolbar of eleven controls, menus and the Lua console.
That is right for someone who wants to reach into the game, and it stays as
**Advanced mode**. Someone who only wants a small change of a mission (another
target, two more guards, a key to find, a door locked) is lost in it.

**Simple mode** is a second face of the same editor for that player: one
guided process, plain words, only what such a level needs. Both modes edit the
same `level.json`; switching never loses anything.

## What makes the editor hard now (the review)

1. **Too many ways in.** Nothing says where to start; the workflow line on the
   Level tab is the only guide and it points to other tabs.
2. **One task, many places.** A target is made in the Inspector, on the NPCs
   tab or on the Mission tab; the start outfit is on the Mission tab; a
   character's mission row is on the NPCs tab and in the Inspector.
3. **The engine's words.** Object paths, *copy path*, *select parent*, *show in
   tree*, behaviour names (EntranceGuard, Guard22, PatrolGuard22), setting names
   (WayPoints, ProtectPoint), model names (BodyguardMafia_01).
4. **The Level tab is a drawer of everything:** the level's state, problems,
   scripts, the map's options (empty, no buildings, sky), lists of new and
   edited objects, sharing, a map of its own.
5. **Modes nobody explained:** Objects / NPCs picking, World / Local, Snap,
   Pick invisible; characters that are "not real yet" until the map loads
   again.
6. **Text-heavy:** long tooltips and wrapped explanations instead of a next
   step.

## Principles

* **One question at a time.** Each step asks one thing ("Who are the
  targets?") and shows only the controls for it.
* **Sentences, not settings.** "Kill *Volkov*", "Get to *the boat*", "A guard
  who *patrols a route*": every choice is a sentence with blanks; a blank is
  filled by clicking in the world or from a short list.
* **The world is the canvas.** What is clicked is answered next to it (a small
  card with the actions that make sense for it), not in a panel across the
  screen.
* **Always a next step.** Every step ends with *Next*; the step bar shows what
  is done, what is missing and what is optional.
* **Nothing is lost.** What simple mode does not show (scripts, vehicles,
  routes of its own, sky, ...) stays in the level and is named ("This level
  also has 2 scripts: Advanced mode edits them").
* **The game's names only where they help.** Characters are named by what they
  are ("Mafia bodyguard", "Waiter"), with the model as a detail.

## The process

The left panel becomes one **guide** with a step bar on top:

```
 (1) Mission  >  (2) People  >  (3) Items  >  (4) The map  >  (5) Try it  >  (6) Save & share
```

Each step: a title, one line of what to do, its controls, *Back* / *Next* at
the bottom. A step is green when done, amber when something is missing,
grey when optional and untouched. A click on a step goes there.

### 0. Start (the launcher)

*Level Editor > New level*: the mission (map) as today, a name, and
**Simple / Advanced** (Simple first; the choice is remembered). Simple mode
starts from the map's own mission: its characters and objectives stay unless
the player changes them. (*Empty map* and *No buildings* are Advanced mode
choices.)

### 1. Mission: what the player has to do

* **Keep the original mission** (default) or **Make my own objectives**.
  The original's lines are shown (read only) so the player knows what is
  there.
* Own objectives: *Add an objective* offers sentences:
  * *Kill* [a character] / *Knock out* [a character]
  * *Get to* [a place] / *Take* [an object] / *Bring* [an object] *to* [a place]
  * *Never be spotted* (fails the mission: yes / no)
  A blank is filled with *Pick* (the next click in the world; the cursor says
  what it wants) or from the list of characters / objects already in the
  level. The laptop line's text is written from the sentence and can be
  changed.
* **The exit:** *Leave through* [a place] (optional; the map's own exit when
  kept).
* **Briefing:** the laptop's text (one box; the original's when empty).
* **Start outfit:** Hitman starts in [his suit / a disguise].

### 2. People: who is on the map

* **The people of the level:** a list of the characters placed and of the
  map's own characters changed, each with a plain name and what it does
  ("Guard: stands guard", "Civilian: walks a route", "Target: Volkov"). A click
  selects it and flies the camera there.
* **Add a person** asks three things, one under the other:
  1. *Who*: **Guard** (armed, attacks a trespasser), **Civilian** (harmless),
     **Target** (a person to kill: made a *Kill* objective at once).
  2. *Looks like*: the map's characters of that kind (and those imported
     from other levels), by a readable name; the most common one chosen.
  3. *Does*: **stands here** / **guards this spot** (a guard point where it
     stands) / **walks a route** (after placing: click the points, Esc ends) /
     *as the map's own [kind]* (its behaviour as on the map, for those that
     have one). The behaviour behind the choice is picked by the editor from
     the map's working behaviours (#20's filtered lists); its name is a detail
     in the tooltip.
  Then *Place* and a click in the world (Shift+click: one more of the same).
* **Remove a person of the map:** select one of the map's characters and
  *Remove from the level* (hidden, as Advanced mode's H).
* A note while placed people are waiting for the map to load again: "New
  people come to life when you try the level (step 5)."

### 3. Items: what Hitman can find

* **Add an item:** a short list by kind: weapons, keys and tools, other
  pickup items (*Models > Pickup items* of the map and of the level's
  imports), each by a readable name. *Place* and a click in the world.
* The level's items listed: select, *Remove*.
* An item can be made an objective from its card (*Hitman has to take it*).

### 4. The map: change what is there (optional)

* Click any object of the map: its card offers **Move**, **Turn**, **Hide**,
  **Copy**, and for a door **Lock / Unlock** when the game allows it.
* **Add an object:** the map's models by category (as the Models tab, with
  its thumbnails), for cover, a blocked way, a hiding place.
* The changes listed, each with *Undo this*.

### 5. Try it

* **Play from the start** / **Play from here** (the camera's point): save, load
  the map again, play (F8 back to the editor).
* **Problems** in plain words, each with a fix where one exists: "The guard
  near the gate cannot walk there: *Move onto the paths*", "Objective 2 has no
  target: *Pick one*".
* What testing changed (god mode, speed) is not offered here: Advanced mode's
  Testing panel.

### 6. Save & share

* The level's name, a short description (README.md), a picture (preview.png
  from the current view).
* **Save** and **Export package** (the author key questions as today).

## The world: the selection card

In Simple mode a click selects as today; instead of the Inspector a small card
stands next to the object on screen:

* its plain name and kind ("Mafia bodyguard: guard, stands guard");
* the actions that make sense for it and for the current step: *Move* (the
  gizmo), *Turn*, *Remove* / *Hide*, *Copy*, *Make it a target*, *Hitman has to
  take it*, *Make it the exit*, *Walk a route...*;
* *More...* opens it in Advanced mode's Inspector.

The gizmo keeps W / E; World / Local, Snap and Pick invisible are Advanced
mode's (Simple: world axes, snapping to the surface).

## Layout in Simple mode

| | |
|---|---|
| top | menu (File, Help), toolbar: Undo, Redo, Save, *Try it*, **Advanced mode** switch |
| left | the guide (steps) |
| world | the selection card, the placing cursor with what it places |
| bottom | one line of camera help ("Right mouse + W A S D: fly, wheel: speed") |
| hidden | Scene tree, Models / NPCs / Level / Mission tabs, Camera, Inspector, Testing, Lua console |

F1 (keys) and Tab (hide the panels) work in both modes.

## Advanced mode

Today's editor, unchanged, plus the **Simple mode** switch in the toolbar. A
level made in Advanced mode opens in Simple mode too: what the guide cannot
show is listed on its first step ("This level also has: 2 scripts, 1 car
route, an emptied map: Advanced mode edits them").

## Building it

Everything the guide does exists in the editor: placing characters with a
behaviour, guard points, new routes, mission goals from a selection, items,
hiding, the gizmo, Test, Problems, export. Simple mode is a new face over
those functions, not new engine work.

| issue | |
|---|---|
| #32 | the mode: H2Fix.ini `[Editor] Mode`, the launcher's choice, the toolbar switch, the panels of each mode, the "also has" note |
| #33 | the guide: step bar, Back / Next, step states; step 1 Mission (objectives as sentences, Pick, exit, briefing, outfit) |
| #34 | step 2 People: add a guard / civilian / target with plain choices, the list, removing the map's people |
| #35 | steps 3-4 Items and The map: items by kind, the selection card in the world |
| #36 | steps 5-6 Try it and Save & share: play buttons, problems in plain words with fixes, description and picture |
| #37 | docs (editor.md starts with Simple mode) and the GUI tests of a level made in Simple mode |

Escalations stay in Advanced mode for now; a later step may give Simple mode
"a stage more" (the stage bar and the rule list).

## Built (first version, 2026-10-31)

* The mode (#32): `[Editor] Mode`, the launcher's choice, both toolbars'
  switch, the panels of each mode, the "also has" note, the camera's line.
* The guide and Mission (#33): the step bar with states, Back / Next;
  objectives as sentences picked in the world, the end and the exit, the
  briefing, the start outfit.
* People (#34): a guard / civilian / target, its look (readable names, the
  most common first), guards / stays or walks a route (its points clicked
  after placing; offered when a behaviour of the map walks one), the list
  with *Remove* / *Put back*, taking the map's people out. A click in the
  world picks a person before the wall behind.
* Items, The map and the card (#35): items by kind; move / turn / hide /
  copy / onto the surface, the map's models; the card beside the selection.
* Try it, Save & share (#36): play from the start / the camera's point, the
  Problems list, save, the package.
* Docs and the test (#37): editor.md's *Simple mode*, `simple_mode_test.py`
  (in run_all).

Then (#35, #36): a line's laptop text by hand (*Change the text*); doors'
*Lock / Unlock* (the game's MSG_LOCKDOOR / MSG_UNLOCKDOOR, `"doors"` in
level.json, `obj:locked()`; a saved game keeps the door's own state); the map
step's *What you changed* with *Undo this*; problems in plainer words with
*fix it there* (the guide's step, Advanced mode's tab); step 6's description
(README.md) and picture (preview.png: the next frame without the editor, 640
wide). *Add an object* is the Models tab's list (its thumbnails on hover).
