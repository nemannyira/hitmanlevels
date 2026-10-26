# Lua scripting

A level runs its scripts when its map is loaded: list them in the level's
`level.json`, files of the level's folder in `Mods\<level>\`:

```json
"scripts": ["main.lua"]
```

The editor's Lua console (`` ` `` / View > Lua console) runs lines in the
same state: Tab completes a name (`h2.pl` -> `h2.player(`, `obj:po` ->
`obj:pos(`; several: they are listed), Up / Down bring back earlier lines.
Everything printed also goes to `H2Fix.log` (the run before:
`H2Fix.prev.log`). The level scripts' errors also show in the Level panel's
Problems list (since they last ran).

A new level's script can start from a template: Level panel > Scripts >
New script from a template.

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
| `h2.find("Volkov")` | also a character the level named (NPCs tab > *Name it*) |
| `h2.findall(name)` | all of that name (an item's model has its name too: `o:isa("ZItem")` is the item itself) |
| `h2.objects(class)` | all of a class, derived ones too (`"ZActor"`, `"ZSTDOBJ"`, `"ZItemTemplate"`) |
| `obj:name()`, `obj:path()`, `obj:class()`, `obj:isa(class)`, `obj:valid()` | |
| `obj:pos()`, `obj:setpos(x, y, z)` | world position, centimetres, +Y up |
| `obj:rot()`, `obj:setrot(yaw [, pitch, roll])` | degrees (yaw 0 = +Z, 90 = +X) |
| `obj:hide()`, `obj:show()`, `obj:hidden()` | |
| `obj:send(message [, number])` | a message as the levels' scripts send one (`"Msg_LockDoor"`, `"Msg_UnlockDoor"`, ...: their SendCommand) |
| `obj:active([on])` | in the world or not, as the levels' scripts switch objects (MakeActive / MakeInactive: out of its rooms, its routines stopped, its children too) |
| `obj:children()`, `obj:parent()` | |
| `obj:spawn([x, y, z [, yaw]])`, `h2.spawn(source, x, y, z [, yaw])` | a copy of an object's model (runtime only) |
| `obj:destroy()`, `obj:spawned()` | objects made by scripts |
| `h2.item(template, x, y, z [, yaw])` | a working item from a template of the map (`"GunHardballer"`, `"KeyDoor"`; see `h2.objects("ZItemTemplate")`). The game offers it to pick up only while (x, y, z) is above the ground's collision: a few centimetres over the floor |
| `h2.point(x, y, z [, room])` | an invisible point (for routes, walking) |
| `list:points()`, `list:setpoints{ obj, ... }` | routes and point lists of the map |
| `h2.mesh_object(mesh, x, y, z [, parent [, flags]])` | a plain object showing a model of the map |
| `h2.import_models{ {"C2-1", mesh}, ... }`, `h2.imported_mesh(level, mesh)` | models of other levels (h2/docs/models.md) |

Characters are moved with `setpos` / `setrot` (they are teleported),
anywhere, into other rooms too: one without a behaviour is sent to its new
spot (a walk of no length, which makes it find its room), so a walk it had
been given stops. Hitman can be moved anywhere too
(`h2.player():setpos(x, y, z)`: his group is moved).

## Characters

| call | |
|---|---|
| `h2.player()` | Hitman |
| `h2.give(item | template [, character])` | the item goes to the character (default Hitman: into his inventory, as if picked up; a weapon can be drawn, a key is carried out of sight); a template name makes the item first. A character keeps it (it is not dropped when it dies: for that `npc:carry`). Not ammunition (it counts only when picked up: lay it down with `h2.item`); A given weapon has an empty clip |
| `h2.outfit(model)` | Hitman in the clothes of a character model of the map (`"Postman"`, `"BodyguardMafia_01"`): one is made under the map, gives its clothes and stays out of sight. A level's own: Level tab > *Hitman starts in* (`"start_outfit"` in level.json) |
| `h2.disguise(character | clothes item)` | Hitman takes its clothes (as from a body: the character is left in underwear) |
| `h2.health([points])` | Hitman's hit points (and the health bar); a new value above 0 (C1-1 on this difficulty: 60 at most) |
| `h2.npcs()` | `{ {record, model, behaviour, object}, ... }` the map's characters |
| `h2.npc(who, x, y, z [, yaw [, behaviour [, params]]])` | a character now (`who`: model `"BodyguardMafia_01"`, group name, record or a character; `behaviour`: `"BaseGuard"`, `false` none, nil its own; `params`: `{ GuardPoint = obj, IsSentry = 1 }`; a guard point not given is a point where it stands, made for the map's time) |
| `h2.behaviours([others])` | behaviour names: the map's characters' (`"EntranceGuard"`), the plain civilian, and with `others` the other levels' that work anywhere (`"Guard2_1 (C2-1)"`) |
| `npc:behaviour()`, `npc:behaviour(name | false [, params])` | read / change |
| `npc:state()` | the state its behaviour is in (`"Neutral2.normal"`) |
| `npc:event(name)` | a script event for its behaviour (`"Msg_InstantDeath"`, ...) |
| `npc:alive()`, `npc:kill()`, `npc:knockedout()` | `kill`: a character's behaviour gets the instant death (a knocked out one too); one without a behaviour dies at once (Actor_If Die). A knocked out character (chloroform: state `Being2.Unconscious`) is alive and `knockedout()`; the engine itself counts it as not alive. `h2.player():alive()`: Hitman's hit points above 0 |
| `npc:walkto(x, y, z)`, `npc:walkto(obj)` | walks there: points of the map's path network only (else it stays) |
| `h2.walkable(x, y, z [, radius])`, `h2.walkable(obj [, radius])` | the nearest point of the path network on about that floor (1.5 m below to 0.6 m above), at most `radius` across (default 200 cm, up to 800): `x, y, z`, or nil. E.g. a walk target: `npc:walkto(h2.walkable(x, y, z))` |
| `h2.onpath(x, y, z)`, `h2.onpath(obj)` | whether a point is on the path network (a cell of it within 25 cm across, 2.5 m down on C1-1); nil: the map has none. A point just off it may still be walked towards (the character stops at the edge). The editor shows the network: Testing panel > Path network |
| `npc:moveset("Run")` | how it moves (`"Walk"`, `"Run"`) |
| `npc:lookat(obj)`, `npc:lookat(nil)` | turns its head / stops |
| `npc:faceto(obj)`, `npc:faceto(x, y, z)` | turns it at once |
| `npc:draggable([on])` | whether Hitman can drag its body (the scripts' EnableDrag / DisableDrag) |
| `npc:unconsciousbody([on])` | the scripts' mark that its body is an unconscious one (SetBodyUnconscious): the characters' scripts ask it when they find a body |
| `npc:noticed([on])` | whether the characters look out for it as they look out for Hitman (the scripts' MakeActorVisible / MakeActorInvisible) |
| `npc:animation(name [, loop])` | plays one of its animations (names: h2/docs/animations.md); false: it has none of that name |
| `npc:carry(template [, fn])` | it carries an item: when it dies (the game's report of it, as the `"death"` event), a working item of the template (as `h2.item`) lands by its body and `fn(item, npc)` is called, as a level's own script gives one (the game itself drops only what a dead character holds in its hands); false: it is dead already. `npc:carry(nil)` takes them back, `npc:carrying()` lists the templates |
| `params = { WeaponTemplate = h2.find("GunBeretta92") }` | (in `h2.npc` / `npc:behaviour`, guard behaviours) the weapon a guard holds: the game's own way of arming one (C1-1's guards: GunBeretta92, RifleShotgun, GunDesertEagle) |

A character's own behaviour may give it other orders: for a scripted scene,
place it without one (`h2.npc(..., false)` or in the editor).

## Mission

| call | |
|---|---|
| `h2.objective(slot [, text [, done]])` | a laptop objective line (1..6); returns the level's own text |
| `h2.objective(slot, false)` | hides a line (the level's own too) until it gets a text again |
| `h2.sound(name)` -> id, `h2.stopsound(id)` | one of the level's sounds, as its scripts play one: a part of its name between stars, the first sound whose name has it (`"*/Male_Whistle1*"`: the names are paths, `Streams/FX/Male_Whistle1.wav`; `"*Male_Whistle1*"` finds Female_Whistle1 first) |
| `obj:sound(name)` -> id | the same played at the object (the scripts' Play3dSound) |
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
| `"objective"` | the slot of an objective the editor's mission marked done (a target dead, a place reached, an object taken) |
| `"suspicious"`, `"recognized"`, `"hostile"`, `"spotted"`, `"help"` | a guard's reaction; `e.to`: the guard (once a second each) |
| `"alarm"` | `e.on`; levels whose scripts have an alarm state (not C1-1) |
| `"disguise"`, `"pickup"`, `"useitem"`, `"drop"` | Hitman's clothes, items (`e.object`: the item, not its model) |
| `"shot"` | `e.silent`; once a second per place (everyone near hears it) |
| `"hit"`, `"damage"`, `"explosion"`, `"unconscious"` | |
| `"door"` | `e.open`, `e.to` the door, `e.object` who |
| `"zone"` | `{object, name, inside}`: Hitman in or out of one of the map's zones (an object with a CheckInside routine: the game's own check), the level's goal places too |
| `"lock"` | `e.locked`, `e.to` the door: a level's script locked / unlocked it |
| `"button"`, `"elevator"` | a button pressed, an elevator sent (the levels that have them) |
| `"drag"` | `{body, by, started}` |
| `"laptop"`, `"map"`, `"menu"`, `"inventory"` | `e.open` |
| `"cutscene"` | `e.started` |
| `"restart"` | |
| `"save"`, `"load"` | `{slot}`: see Story state |
| `"message"` | every message of the game (about 100 a second): `{name, to, from, object, value, words, param, text}` |
| `"msg:<name>"` | one of them (`"msg:hitmanspotted"`, any case); `h2.messages()` lists the names |

## Vehicles

A level's vehicles (Inspector > Vehicle) are the game's: a ZCar made with the
map on a path of its own (`ROOT/EditorCar_1`, `ROOT/EditorPath_1`, ...);
`h2.track(car, place, kmh)` drives one along its path as the game's own
vehicles. `h2.drive` below moves any object along points by itself (a
script's effect, not a vehicle of the game).

```lua
local car = h2.find("CarFerrari+1")                 -- a car the level placed (a group copy)
local v = h2.drive(car, h2.find("CarRoute"), {speed = 800, loop = true})
-- route: a list of points (its :points()), or { object, ... }, or { {x, y, z}, ... }
v:stop()  v:go()  v:speed(400)  -- cm/s
print(v:running(), v:progress())                     -- progress 0..1
h2.on("arrived", function(e) print("arrived", e.vehicle, e.object:name()) end)
```

The object drives a smooth curve through the points, turned along it (its
long side forward; `reverse = true` if it goes backwards), uphill and
downhill with the points' heights, in the game's time (it stands while the
world is frozen). One drive an object: a new one replaces it. The game's own
vehicles (ZMoveOnSpline: the C1-1 van) are kept on their tracks by the game:
refused. They drive their own track instead:

```lua
local van = h2.find("TruckDeliveryVan")
h2.track(van, 0, 60)      -- to its track's start (0; 1 its end), at most 60 km/h
print(h2.trackpos(van))   -- where it is on it, 0..1
h2.track(van)             -- stops it where it is
```

Front first, speeding up and slowing down as the game's vehicles do; its
own script (the level's) may send it elsewhere too (C1-1's van goes back to
its place when it stops, C7-1's bus keeps driving its round).

## Story state

`h2.state` is a table kept with the game's saves: strings, numbers,
booleans and tables (lists 1..n or string keys; number keys of other tables
come back as strings; functions are not kept). A new map starts with it
empty; a saved game gives it back before the level's scripts run, so they
see it:

The key `__editor` is the editor's own (its mission goals done).

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
