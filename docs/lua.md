# Lua scripting

A level runs its scripts when its map is loaded: list them in the level's
`level.json`, files of the level's folder in `Mods\<level>\`:

```json
"scripts": ["main.lua"]
```

The editor's Lua console (`` ` `` / View > Lua console) runs lines in the
same state. Everything printed also goes to `H2Fix.log` (the run before:
`H2Fix.prev.log`).

**Sandbox**: Lua 5.4 with base, table, string, math, utf8, coroutine and
`os.clock` / `time` / `date`; no `io`, no loading native code, `load` takes
source text only. One call from the game (a handler, a timer, a chunk) may
run 3 seconds at most; the state may use 256 MB.

When the level's scripts run again (the level is loaded or applied again),
their event handlers, timers and zones and the objects they made go; the
ones made from the console stay. Timers, zones and `h2.state` belong to the
map: they go when it is left.

## Objects

Objects are handles to scene objects by their path; they stay valid while
the map is loaded (`obj:valid()`).

| call | |
|---|---|
| `h2.find(name)` | by name (`"Crate#2"`: the third of that name) or full path; nil: none |
| `h2.findall(name)` | all of that name |
| `h2.objects(class)` | all of a class, derived ones too (`"ZActor"`, `"ZSTDOBJ"`, `"ZItemTemplate"`) |
| `obj:name()`, `obj:path()`, `obj:class()`, `obj:isa(class)`, `obj:valid()` | |
| `obj:pos()`, `obj:setpos(x, y, z)` | world position, centimetres, +Y up |
| `obj:rot()`, `obj:setrot(yaw [, pitch, roll])` | degrees (yaw 0 = +Z, 90 = +X) |
| `obj:hide()`, `obj:show()`, `obj:hidden()` | |
| `obj:children()`, `obj:parent()` | |
| `obj:spawn([x, y, z [, yaw]])`, `h2.spawn(source, x, y, z [, yaw])` | a copy of an object's model (runtime only) |
| `obj:destroy()`, `obj:spawned()` | objects made by scripts |
| `h2.item(template, x, y, z [, yaw])` | a working item from a template of the map (`"GunHardballer"`, `"KeyDoor"`; see `h2.objects("ZItemTemplate")`) |
| `h2.point(x, y, z [, room])` | an invisible point (for routes, walking) |
| `list:points()`, `list:setpoints{ obj, ... }` | routes and point lists of the map |
| `h2.mesh_object(mesh, x, y, z [, parent [, flags]])` | a plain object showing a model of the map |
| `h2.import_models{ {"C2-1", mesh}, ... }`, `h2.imported_mesh(level, mesh)` | models of other levels (docs/models.md) |

Characters are moved with `setpos` / `setrot` (they are teleported),
anywhere, into other rooms too: one without a behaviour is sent to its new
spot (a walk of no length, which makes it find its room), so a walk it had
been given stops. Hitman can be moved anywhere too
(`h2.player():setpos(x, y, z)`: his group is moved).

## Characters

| call | |
|---|---|
| `h2.player()` | Hitman |
| `h2.give(item | template [, character])` | the item goes to the character (default Hitman: into his inventory, as if picked up; a weapon can be drawn, a key is carried out of sight); a template name makes the item first. A character keeps it (it is not dropped when it dies: for that `npc:carry`) |
| `h2.disguise(character | clothes item)` | Hitman takes its clothes (as from a body: the character is left in underwear) |
| `h2.health([points])` | Hitman's hit points (and the health bar); a new value above 0 (C1-1 on this difficulty: 60 at most) |
| `h2.npcs()` | `{ {record, model, behaviour, object}, ... }` the map's characters |
| `h2.npc(who, x, y, z [, yaw [, behaviour [, params]]])` | a character now (`who`: model `"BodyguardMafia_01"`, group name, record or a character; `behaviour`: `"BaseGuard"`, `false` none, nil its own; `params`: `{ GuardPoint = obj, IsSentry = 1 }`) |
| `h2.behaviours([others])` | behaviour names (`others`: of other levels too, `"Guard2_1 (C2-1)"`) |
| `npc:behaviour()`, `npc:behaviour(name | false [, params])` | read / change |
| `npc:state()` | the state its behaviour is in (`"Neutral2.normal"`) |
| `npc:event(name)` | a script event for its behaviour (`"Msg_InstantDeath"`, ...) |
| `npc:alive()`, `npc:kill()` | `kill`: a character's behaviour gets the instant death; one without a behaviour dies at once (Actor_If Die). `h2.player():alive()`: Hitman's hit points above 0 |
| `npc:walkto(x, y, z)`, `npc:walkto(obj)` | walks there: points of the map's path network only (else it stays) |
| `h2.walkable(x, y, z [, radius])`, `h2.walkable(obj [, radius])` | the nearest point of the path network on about that floor (1.5 m below to 0.6 m above), at most `radius` across (default 200 cm, up to 800): `x, y, z`, or nil. E.g. a walk target: `npc:walkto(h2.walkable(x, y, z))` |
| `h2.onpath(x, y, z)`, `h2.onpath(obj)` | whether a point is on the path network (a cell of it within 25 cm across, 2.5 m down on C1-1); nil: the map has none. A point just off it may still be walked towards (the character stops at the edge). The editor shows the network: Testing panel > Path network |
| `npc:moveset("Run")` | how it moves (`"Walk"`, `"Run"`) |
| `npc:lookat(obj)`, `npc:lookat(nil)` | turns its head / stops |
| `npc:faceto(obj)`, `npc:faceto(x, y, z)` | turns it at once |
| `npc:animation(name [, loop])` | plays one of its animations (names: docs/animations.md); false: it has none of that name |
| `npc:carry(template [, fn])` | it carries an item: when it dies, a working item of the template (as `h2.item`) lands by its body and `fn(item, npc)` is called; false: it is dead already. `npc:carry(nil)` takes them back, `npc:carrying()` lists the templates |
| `params = { WeaponTemplate = h2.find("GunBeretta92") }` | (in `h2.npc` / `npc:behaviour`, guard behaviours) the weapon a guard holds: the game's own way of arming one (C1-1's guards: GunBeretta92, RifleShotgun, GunDesertEagle) |

A character's own behaviour may give it other orders: for a scripted scene,
place it without one (`h2.npc(..., false)` or in the editor).

## Mission

| call | |
|---|---|
| `h2.objective(slot [, text [, done]])` | a laptop objective line (1..6); returns the level's own text |
| `h2.objective(slot, false)` | hides a line (the level's own too) until it gets a text again |
| `h2.subtitle(text [, seconds])` | a subtitle line at the bottom of the screen (default 5 game seconds; nil / `""` takes it away) |
| `h2.info(text [, kind])` | the game's own info line ("Info: ...", "Warning:", "Hint:" in its colour, bottom left); `kind`: `"info"` (default), `"warning"`, `"hint"`; one paragraph. A subtitle takes the same place |
| `h2.briefing(text)` | the laptop's briefing (`""`: the level's own) |
| `h2.mission("complete" | "fail")` | ends the mission (statistics, saved progress) |
| `h2.basemission([on])` | whether the base level's own logic (its targets, its exit) may end the mission; the level's setting: `"mission": { "base_ends": false }` or the Mission panel. Off: such an end is only reported (`"mission"` event, `e.stopped`); Hitman's death still fails it |
| `h2.music()`, `h2.music(zone, track)` | music zones and tracks |
| `h2.sky([level])` | the sky of another level (`"C3-3"`; `""`: the map's own) |

## Time, timers, zones, keys

| call | |
|---|---|
| `h2.time()` | game seconds on this map: stands while the world stands (editor open, Freeze, the game's pause), runs with the game speed |
| `h2.after(s, fn)` | once, in `s` game seconds; returns a timer |
| `h2.every(s, fn [, n])` | every `s` seconds (`n` times) |
| `t:cancel()`, `t:active()` | |
| `h2.zone{ x, y, z, radius, ... }` | a sphere |
| `h2.zone{ min = {x, y, z}, max = {x, y, z}, ... }` | a box |
| `h2.zone{ object = obj, radius = r, ... }` | around an object, moving with it |
| `who = "hitman"` (default) `| "characters" | obj | { obj, ... }` | whom it watches |
| `on_enter = function(z, who) end`, `on_leave = ...` | checked 10 times a game second |
| `z:inside(obj)`, `z:contains(x, y, z)`, `z:remove()` | |

## Events

`h2.on(name, fn)` adds a handler, `h2.off(name)` removes all of that name.
Handlers run after the frame (never inside the game's own code); one that
fails prints its error and the others still run.

| event | the handler gets |
|---|---|
| `"frame"` | seconds since the last frame |
| `"scene"` | the scene's name, after a map loaded |
| `"key"` | `{key = "K" / "F5" / "Numpad1" / "Space", vk}`: a key while playing (the game gets it too) |
| `"kill"`, `"harm"` | `{victim, kind = "enemy" / "innocent" / "other", type}`: as the rating counts it; `"other"`: not counted (not by Hitman) |
| `"death"` | a character (checked 4 times a second, any cause) |
| `"hitman_damage"` | `{amount, health, dead}` |
| `"mission"` | `{result = "completed" / "failed", stopped}`: `stopped`, the base level's own end not let through |
| `"objective"` | the slot of an objective the editor's mission marked done |
| `"suspicious"`, `"recognized"`, `"hostile"`, `"spotted"`, `"help"` | a guard's reaction; `e.to`: the guard (once a second each) |
| `"alarm"` | `e.on`; levels whose scripts have an alarm state (not C1-1) |
| `"disguise"`, `"pickup"`, `"useitem"`, `"drop"` | Hitman's clothes, items (`e.object`) |
| `"shot"` | `e.silent`; once a second per place (everyone near hears it) |
| `"hit"`, `"damage"`, `"explosion"`, `"unconscious"` | |
| `"door"` | `e.open`, `e.to` the door, `e.object` who |
| `"drag"` | `{body, by, started}` |
| `"laptop"`, `"map"`, `"menu"`, `"inventory"` | `e.open` |
| `"cutscene"` | `e.started` |
| `"restart"` | |
| `"save"`, `"load"` | `{slot}`: see Story state |
| `"message"` | every message of the game (about 100 a second): `{name, to, from, object, value, words, param, text}` |
| `"msg:<name>"` | one of them (`"msg:hitmanspotted"`, any case); `h2.messages()` lists the names |

## Story state

`h2.state` is a table kept with the game's saves: strings, numbers,
booleans and tables (lists 1..n or string keys; number keys of other tables
come back as strings; functions are not kept). A new map starts with it
empty; a saved game gives it back before the level's scripts run, so they
see it:

```lua
h2.state.stage = h2.state.stage or 1
h2.on("save", function(e) h2.state.saved_at = h2.time() end)
h2.on("load", function(e) print("back at stage", h2.state.stage) end)
```

It is written as `Save\<slot>.h2lua` next to the game's files of the slot.

## Testing aids and camera

`h2.speed([factor])`, `h2.god([on])`, `h2.invisible([on])`,
`h2.freeze([on])`; `h2.freecam([on])`, `h2.campos()`, `h2.setcampos(x, y,
z)`, `h2.camrot()`, `h2.setcamrot(yaw, pitch)`, `h2.camlookat(x, y, z)`;
`h2.scene()`, `h2.fps()`, `h2.resolution()`, `h2.log(...)`.

## Example: a small story

A whole mission: `examples/missions/Courier` (copy the folder into `Mods`).

```lua
-- main.lua (C1-1): a guard must reach the front gate; if Hitman is spotted
-- before, the mission fails
local gate = h2.find("BodyguardMafia_FrontGate_00")
local courier = h2.npc("BodyguardMafia_01", -4176, -20, -6682, 0, false)
h2.state.stage = h2.state.stage or 1

h2.objective(1, "Let the courier reach the gate")

if h2.state.stage == 1 then
  h2.after(3, function()
    courier:moveset("Walk")
    courier:walkto(gate)
  end)
end

h2.zone{ object = gate, radius = 200, who = { courier },
  on_enter = function()
    if h2.state.stage ~= 1 then return end
    h2.state.stage = 2
    courier:faceto(gate)
    courier:animation("Greeting_Russian01")
    h2.objective(1, nil, true)
    h2.after(4, function() h2.mission("complete") end)
  end }

h2.on("spotted", function(e)
  if h2.state.stage == 1 then h2.mission("fail") end
end)
```
