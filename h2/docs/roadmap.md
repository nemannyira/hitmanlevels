# Roadmap: research notes and plans (2026-10)

What was found while looking into the larger questions, what works now, and
the next steps. Addresses are hitman2.exe (dump) unless noted.

## 1. The NPC system (what is known)

* A character is a ZTRACKLNKHM2 actor (class 0x2003E9) in a group, with an
  aim helper (ZAIMTHINGY). Made at runtime it is bumped into by other
  characters but not by Hitman and cannot be shot: placed characters are
  added to the scene file while it loads (src/engine/inject.cpp, h2/docs/npc.md).
* Its behaviour is an SDL script class. The scene's **ZGF** is a chunk tree
  of compiled classes `scripts\...\<Class>.ppo` (tools/zgf.py lists and
  extracts it). It is mounted on ZSysFile only while the scene loads; the
  script engine (0x5D5BA0) loads a class by path, its base classes relative
  to it, and caches them for the scene. ZSysFile Exists / Size / Read
  (0x40E2C0 / 0x40E1B0 / 0x40E540) ask mounted archives first, then the
  disk.
* The editor serves other levels' .ppo files from memory through those three
  hooks (src/engine/script_files.cpp): **50 behaviours of other levels** can
  be given to placed characters ("Guard2_1 (C2-1)"), references filled from
  the scene's characters of the same parameter names. Characters of other
  levels get their own behaviour when it is portable.
* Script interfaces (registered in 0x5781E0): Engine_If, Dialog_If, Item_If,
  Scene_If, Sound_If, Geom_If, Link_If, Actor_If, List_If, ConditionList_If.
  Each is an object whose methods are called by index (Actor_If vtable
  0x65BC08; TrackLnk_HM2 dispatcher 0x56F2A0 adds methods in order with
  0x56F260). The method names (MoveToLocation, MoveToPosition, LookAt,
  FaceObject, MakeActorInvisible, GetDistanceToObject...) are only in the
  compiled .ppo files, which name them for the linker.
* Characters' scripts get **ScriptEvent** messages with an event name
  (SendMessage, vfunc 0x120): Lua obj:event(name).
* Required references: parameters never empty in any level (WayPoints,
  WayPointList, HelpPoints, SafePoints, WeaponTemplate, m_Arbiter...): a
  behaviour without one is not started (a null route list crashed the script
  engine). Furniture-bound behaviours (sitting, sleeping, patients) broke the
  scene unload when moved to another level: not offered.

Next steps:
1. Parse the .ppo format (string table, import table of interface methods)
   to map method names to indices: then Lua could call Actor_If methods
   (walk to a point, look at, play an animation) directly.
2. Done: new route lists made at load time (copies of a list record, at
   the root, a node of their own: inject.cpp SceneList, level.json
   "new_lists"): *A route of its own* in the NPCs tab gives a character a
   copy of the list it shares; tested on C1-1 (DeliveryBoy's HelpPoints, the
   copy's points changed and kept, the template's not).

## 2. Lua: what to add for full story missions

Now: objects (find, pos/rot, hide/show, spawn/destroy), characters (h2.npc,
behaviour, state, event, kill, alive), items, points, lists (setpoints),
mission (objective, briefing, complete/fail), music, sky, imports, camera,
testing (speed, god, invisible, freeze), events (h2.on: frame, objective,
...).

Proposed, in order of value:
1. **Triggers**: h2.zone(x, y, z, radius | box) with on_enter / on_leave for
   Hitman or any character (the editor's Reach goal generalised), timers
   (h2.after(seconds, fn), h2.every).
2. **Game events to Lua**: character killed (by whom, how: CharacterKilled /
   ActorDead), body found, alarm raised / ended (guard arbiters), Hitman
   spotted, disguise changed (Hitman_ChangedClothes), item picked up
   (SCR_PICKUPITEM), shot fired, door opened. Most are messages the level
   routine already gets (0x504410 registers them).
3. **Characters**: npc:walkto(point), npc:lookat(obj), npc:say(dialog),
   npc:animation(name) through Actor_If (needs the .ppo method map, see 1.);
   until then: behaviours with route lists and events.
4. **Hitman**: inventory (give / take items), disguise, teleport at load
   (done for the start), health, set the mission's rating counters.
5. **Text** (done): subtitles (h2.subtitle: MSG_WRITESUBTITLE), the info
   box (h2.info: MSG_DISPLAYINFO / WARNING / HINT with the editor's own LOC
   keys), laptop pages (briefing, objectives).
6. **Story flow**: states saved with the game (a Lua table written into the
   save game next to the level's state), checkpoints, cut-scene camera paths
   (the free camera's keyframes), fade in / out (ShowBlack).
7. **Sound and music**: play a sound / dialogue line at an object (Sound_If),
   music moods (done: tracks per zone).
8. **Safety** (done): text-only load, 3 s per call, 256 MB, no io / os.

### Game events for Lua (where to hook them)

Now: scene, frame, death (polled 4 times a second), objective.

* **Every message** goes through `ZGEOM::SendMessage` (vfunc 0x120 =
  0x43CBE0, the same function in 119 geom vtables, the characters' too):
  `this` = sender, args (receiver, msg id, param*). It calls the receiver's
  vfunc 0x15C (HandleMessage: 0x43CFE0 hands it to the geom's script,
  `g_pScriptDll` vfunc 0x5C = 0x5D4CE0, **by name**) and vfunc 0x10C (the
  geom's events / routines, 0x43C6E0). Names: `GetZMsgName` 0x41EEB0
  (engine +0xC37 + id*4); ~250 engine messages (MSG_DAMAGE, ProjectileHit,
  HitObject, MSG_DRAGSTART / DRAGEND, MSG_PICKUPITEM, MSG_DROPITEM,
  MSG_EXPLODE, msg_sound_shot / silentshot / footstep / die / door ...) and
  ~550 script messages registered in 0x568850 (hitmanspotted,
  HitmanDetected, alarm, alarmraised, enteralarmstate, leavealarmstate,
  characterkilled, CharacterUnconscious, ActorDead, hitman_changedclothes,
  hitmanaiming, weapondetected, dooropen / doorclose, cutsequence_start /
  end, msg_lightswitchtoggled, windowbroken, restrictedzoneenter ...).
* **The level routine** (handler 0x504960, vtable slot 0x34 of the level
  routine; per level classes call it): CharacterKilled -> vfunc 0xB0
  classifies the victim, vfunc 0xB4 (base 0x505530: type 2 enemy, 1
  innocent, as the statistics 0x5058B0 list them; the level's scripts call
  it too, through LevelControl_if 0x56BEB0, with 0 for a death not by
  Hitman; then it sends ScriptCharacterKilled),
  CharacterHarmed -> 0xB8 (base 0x505600), MissionFailed -> 0xC0,
  MissionCompleted -> 0xC4, pick up / use item / change clothes
  (levelcontrolnotify*) -> 0x84 / 0x88 / 0x8C, BriefingMenuOpened,
  IngameMenuOpened, IngameMapOpened, IngameInventoryOpened, RestartLevel.
* Characters (ZTRACKLNKHM2 HandleMessage 0x508950) handle MSG_DAMAGE,
  ProjectileHit, HitObject, MSG_DRAGSTART / DRAGEND, MSG_CHANGECLOTHES,
  Hitman_ChangedClothes natively, the rest goes on to their script.

Done (src/engine/game_events.cpp, Lua names in src/script/events.h): the
hook on 0x43CBE0 keeps only the messages Lua has handlers for (a flag per
id, the names mapped again when the engine registers more), the level
routine's vtable entries 0xB4 / 0xB8 / 0xC0 / 0xC4 are routed through the
editor (per level class, patched at its init 0x504410), Hitman's damage
0x519390 is hooked. Everything is queued and fired after the frame (no Lua
inside the engine's dispatch; at most 4096 a frame). Lua: "message",
"msg:<name>", "kill" / "harm" (victim, enemy / innocent), "mission",
"hitman_damage", and names of their own for messages ("spotted", "alarm",
"disguise", "pickup", "shot", "door", "drag", "laptop", ...); h2.messages()
lists the names. Objects in a message: the param itself or the dword it
points to, as a pointer or a geom ref, when it is an object of the scene.
To check in the game: how many messages a second, which messages each
action really sends (the aliases are by name), what the params carry.
Zones, timers and keys: src/script/triggers.h (game time that stands with
the world, h2.after / every, h2.zone, the "key" event). Story state kept
with the saves: src/script/state.h (h2.state, "save" / "load" events,
Save\<slot>.h2lua next to the game's files).

## 3. Maps of their own

* The engine loads a scene by path: a renamed copy of a map
  (tools/clone_scene.py: `Scenes\Custom1\Custom1__MAIN.ZIP`, entries renamed)
  loads with its characters, laptop and texts. A map of one's own can be its
  own scene package, its levels and save progress apart from the original.
* Done: *Level > A map of its own* (src/editor/own_map.cpp) copies the
  level's map as `Scenes\<Name>\<Name>__MAIN.ZIP` (entries renamed, data
  copied as it is) with its WAV / WHD; the level's base scene becomes
  `<Name>\<Name>__MAIN`, `own_map_of` keeps the original. Objects keep their
  paths (ROOT/C1-1/...); placed objects made on the original are applied on
  the copy. Tested: placed objects, a driven car, a saved game loaded, the
  launcher's Play.
* A scene package: GMS (records: known, the editor writes it), BUF (strings),
  PRM (models: known, imports copy them), TEX (textures: known), OCT
  (collision: mostly known), RMC / RMI (room trees), SGP (sound graph), SND,
  ANM (animations: known), LOC (texts), ZGF (scripts, fonts: known), SUP,
  WAV / WHD (sound).
* Steps to a map from nothing:
  1. "Template" maps: a small level copied under a new name with its world
     taken out (the blank map, with the sky kept) and a floor: works now as
     a level setting, next as its own package.
  2. A package writer (tool): GMS with the root room, one room, Hitman,
     HeroSetup / GuiSetup / MenuSetup / LevelControl records copied from a
     host level; PRM / TEX with only the models used; OCT with their
     collision; RMC / RMI for one room (format to be read); ANM with
     Hitman's sets; ZGF with the host's fonts and LevelControl.
  3. Launcher entries for such maps (their own name, briefing, picture).

## 4. Cars

* Class chain: ZGROUP -> ZMoveOnSpline (0x100BBD) -> ZVehicle (0x100BBE) ->
  ZCar (0x100BBF). Vehicles move along **ZSHAPE** splines (class 0x04000022:
  the curve is a PRM shape record, the data block is empty), driven by their
  scripts (C1-1 Van: rExplode; C2-1 Truck: Track1; C7-1 Car / OmniBus:
  m_rTrackList; C2-2 Limo: tracks to and from a meeting; C5-2 Car2 with a
  follower car). AllLevels has GenericMoveOnSpline / GenericVehicle /
  GenericCar classes.
* Possible now: a level's own vehicles with their own tracks; a vehicle of
  another level imported as models (looks only), its script as a behaviour
  of another level (as characters) once its group is made at load time.
* Done (b2dc9ba): a copied group (a car: kind "group" in level.json) is
  driven kinematically along a route of points the user clicks (a
  Catmull-Rom curve; Inspector > Vehicle, level.json "vehicles"; Lua
  `h2.drive` with stop / go / speed). The game's own vehicles are refused:
  their mover (+0x5C) puts them back on their spline every frame.
* Done: the game's own vehicles along their own tracks (h2.track, the
  Inspector): their mover (+0x5C, vtable 0x65C884) takes a place on the track
  with its vfunc 0 (0x596040, MoveToSplineVal); speed +0x5B, top speeds +0x63
  and the vehicle's +0xC4. Tested: C1-1's van, C7-1's bus.
* Done (6fd2fdf): a level's vehicles the game's way. The route is a ZSHAPE:
  its model record in the PRM (type 0x40000; +0x84 knots of 9 floats: the
  point, its control point out, the next one's in; +0x88 their count; +0x18
  bounds; +0x1C / +0x44 a polyline), which ZSHAPE builds cubic segments from
  (0x44DEB0; the renderer's resource manager vfunc 0xD4 / 0xD8 give the knots
  and the count). The car is a ZCar made at load with the model's meshes as
  its children: its "*Body*" (ZMoveOnSpline's init 0x5912A0, +0xDE: ZCar's
  placing 0x58FC20 needs it), wheels named *FrontLeft_01* / *FrontRight_01*
  (each with a "Wheel*" child: they steer, 0x5958E0) / *BackLeft_01* /
  *BackRight_01* (ZCar's init 0x592EC0 counts them and divides by the counts).
  Driven through the vfuncs MoveOnSpline_if calls (dispatcher 0x58D280,
  methods 0x58CE30): 0x2AC SetPath(ref, start) -> mover 0x595F30, 0x2B0
  SetMoveParams(speed, acceleration, top speed, +0xC4) -> 0x5963B0, 0x2CC
  MoveToSplineVal(place, flag) -> mover vfunc 0. Its frame (0x591610, ZMoveOnSpline
  vfunc 0x154) steps the mover and places the car.
* Next steps: (1) drivers / passengers (C2-2 LimoGuard's rLimo + seat
  number); (2) what a driven car does to characters in its way (not checked
  yet); (3) a map car's records left out at load (now it is hidden).

## 8. The rating (Silent Assassin, ...)

* The end of a mission (the level routine's MissionCompleted, 0x5058B0;
  also the editor's goals and h2.mission("complete")) saves progress
  (GlobalCom strLastCompletedScene / strHighestCompletedScene) and shows the
  statistics window (MSG_SHOWSTATS). The profile (Save\<n>.blap) is the
  global variables of CGlobalCom (0x6A40EC: a buffer of 0x2000 bytes); the
  statistics screen writes it (0x551420). While a level of the editor is on
  the map that write is skipped and the variables are put back when the map
  is left; its Continue goes to the main menu (src/editor/profile_guard.h).
* The counters come from the game's events: shots, kills (CharacterKilled /
  ActorDead), headshots (scripts call IncHeadshots), alerts (guard scripts).
* Where they are (2026-11-14, for the play statistics: docs/play_stats.md,
  #50): LevelControl_MissionCompleted 0x5058B0 builds a 0x38-byte block and
  sends it with MSG_SHOWSTATS to the statistics window (SceneCom ref
  "rSTATISTICS"): +0x00 the scene's name (8 bytes), +0x08 Hitman +0x11C7,
  +0x0C LevelControl +0x220 + Hitman +0x11CB, +0x10 LC +0x208, +0x14 LC
  +0x21C, +0x18 LC +0x210, +0x1C LC +0x20C, +0x20 LC +0x218, +0x24 LC +0x214,
  +0x30 SysInterface +0x38 >> 10, +0x34 LC +0x249. So the counters are on
  LevelControl and Hitman during the whole mission (a failed or left run has
  them too). The window (handler 0x55C590) copies the block to 0x6B3418;
  0x55C8A0 makes stealth = exp(k x ((+0x24 + 2 x +0x20 + +0x18) x 3 + +0x1C +
  2 x +0x08 + +0x10 + +0x0C)) (this +0xBC, 0x6B3444) and aggression = 100
  when +0x14 and +0x0C are 0, else from their sum (this +0xB8, 0x6B3440);
  StatisticsLevel_FillLines 0x55BED0 bands both and reads the rating from the
  8 x 8 table this +0xC6 (names at 0x691238). Which field is which line of the
  screen (shots, alerts, kills...) is to be confirmed with a run of known
  numbers.
  Characters the level places are made with the scene and count like the
  level's own: a guard behaviour as an enemy, a civilian (Neutral2 based) as
  an innocent.
* The game does not note cheats: EnableCheats (SysInterface +0x7B6) only
  turns on the cheat keys. The editor's testing aids (god mode, invisible
  Hitman, game speed, frozen) are not seen by the statistics either: a run
  tested with them can still be rated Silent Assassin. The editor marks such
  runs: a mission of a level of the editor completed after god mode,
  invisible Hitman or another game speed was on (since the map loaded)
  shows "TESTED WITH ...: NOT A REAL RATING" over the statistics
  (testtools.h AidsUsed / NoteMissionEnd).

## 9. The sky (done)

The backdrop group's plain models (dome and cloud planes) of another level
instead of the map's (Level panel, h2.sky), with its sun and the sun's glow
(Z2DOBJ: their plain data blocks copied; put in their direction as far as
the map's own sun, whose sky has another scale; checked: C3-3's pale sun
over C1-1). Next: the lens flare (ZLightOverlay), the fog colour and light of
the map to match.

## 9b. The map from above (done, #22)

A camera draws from the rooms it is in (its visibility, `Camera_FindVisibleRooms`
0x433170): the collision manager (renderer +0x1C) vfunc 0x24 (RenderD3D
+0x27D60: rooms, end, matrix, position, scale) gives the rooms whose inside
trees hold the point (vfunc 0x18), else those a box of that size meets (vfunc
0x14), the deepest of nested ones; each is kept if `ZROOM_HoldsPoint` (vfunc
0x16C, 0x43DC50: its box and inside trees) holds the camera; each gets an entry
of 0xF00 bytes (0x20000 of stack: 34) with the whole view, and rooms seen
through portals are added clipped to the portal (`Camera_WalkRoomPortals`
0x432120, `Camera_AddRoomThroughPortal` 0x431A10). Camera +0x18: the room it
was in last; +0x1C: a room of its own (geom ref; 0: the root), drawn with the
whole view (its own nodes only) when it is in none, if it is a room (vfunc
0x1CC = IsA, ZROOM 0x100021). Above a map the camera is in no room: grey.

The editor gives the free camera, when it is in no room, the rooms the same
query finds with a box as large as its far plane, those in front, nearest
first (32), and `ZROOM_HoldsPoint` answers yes at the camera's call
(0x4333C9). Rooms under the open sky have the map's backdrop (room +0xB3);
inside rooms have none. Both kinds at once overflow the renderer's lit colour
vertex buffer (one of 0x20000, filled in RenderD3D 0x10010050; C1-1 with all
its rooms crashed in 0x100090C0 on a buffer index past it): the view shows one
kind (*From above: inside the buildings*). A room of no size
(EmptyGroupForBlankScreen) is left out (it crashed in `LightTable_LightsOf`).

## 10. Reviews (done this round)

Security: Lua sandbox (text-only load, time and memory limits), level files
from elsewhere (fields of the wrong type, NaN transforms, path names, level
names, character names and counts), game texts (control characters reach an
unchecked array), script inputs (model records, coordinates). Bugs: saved
games with the level's characters, the GMS copy leak, a freed camera during
loading with the editor open, the blank map keeping the clouds, the sky's
objects destroyed when it changes, testing values not fighting the game's
cheats.

## 11. The game's own way: what is left (found, not built yet)

* **Taking an object that is not an item**: done 2026-10-18 for the scene's
  models (a kind-6 "Take" action, h2/docs/mission.md); a copy made while playing
  still uses E (the game offers no action of it: copies as records made with
  the scene would, B16). What was found before: the game
  gives objects actions with a ZGEOM_Action routine (ctor 0x487680, vtable
  0x649770; messages Action_GetActionList, CanOperateObject, EnableAction,
  RemoveNamedAction...). Doors add theirs at runtime with 0x488A70 (fastcall:
  geom, LOC node of the text, then kind, message, 0, distance, 0: "OpenDoor"
  kind 2 at 400, "PeekHole" kind 0xB at 250; items' "_Pickup" is kind 0x10):
  the routine is found or added (geom vfunc 0xE8 / 0xF4 "ZGEOM_Action"), then
  its vfuncs 0x50 (1, geom) / 0xAC (the entry) / 0x50 (2). What each kind
  makes Hitman do (and which one only sends the message) has to be found in
  the game running before a "take" action can be added this way.
  Seen in the game (x32dbg, C1-1 loading, 2026-10-18): 0x488A70 gets ecx = the
  door, edx = the text itself ("Open door", not a LOC node), and on the stack
  (text, kind 2, message 0x57 = "dooropen", 0, distance 400, 0), RET 0x18. The
  entry (vfunc 0xAC = 0x488A10: text, text, kind, message, 0, distance, ?)
  keeps kind at +0x72 (1 by the ctor), the message at +0x76 (short), distance
  at +0x80 (also vfunc 0xA4); used, it sends that message to its geom. Who
  reads the kind (Hitman's side of Action_GetActionList) is still to be found.
* **npc:carry**: the item could be given to the character when the script
  says so (GiveItem, ZITEM::SetOwner as guards get their weapons) and dropped
  by the game when it dies (MSG_DROPITEM 0x47EAF0 / 0x5333B0, Being2's
  DropItem); whether a dead character's inventory (not its held weapon) is
  dropped has to be seen in the game first.
* **Characters added while playing** (h2.npc, NpcSpec::now) stay runtime
  objects: the scene's are records (inject.cpp); a runtime one is shown until
  the map loads again.
* **Not possible as records**: leaving the scene's records out (an emptied
  map, a deleted character): their indices are what scripts and data blocks
  point to; the sky of another level: it has to be in a ZROOM (a root record
  is not drawn as the room's backdrop); a teleport: ZLIST_TeleportList
  (vtable 0x64C550) is a list of places, the actor's own SetWorldTransform
  (vfunc 0x80 = 0x463AC0) is what moves it, already used.
* **Teleports (B12, looked at again 2026-10-18)**: ZLIST_TeleportList keeps
  three fields only (+0x72, +0x76, +0x7A; no engine code reads it by name:
  the scripts drive it); ZGEOM_CutSequenceMove only reads its data. A
  character's room is found by the game when a path starts (the editor's walk
  of no length after a teleport: the game's own way; calling the room setter
  0x463230 directly hung the game). Hitman's floor contacts (a list around
  +0x44C, written while walking by 0x60F860 from the character physics step
  0x610640: velocity, contacts, the floor) are left as they were by a
  teleport; the game has no "put on the floor" call of its own seen, so the
  fall guard (P14) stays.
  Done 2026-10-19: the levels' scripts teleport with Scene_If
  CopyObjectPosDir (0x57FCB0: the other object's world transform, geom vfunc
  0x84, set on the target with vfunc 0x80). Done so to Hitman himself
  (h2.dev_copyposdir) it moves his group with him and he stands on the floor
  where he lands (C1-1, from the start hill into the mansion: y 500, still
  there 6 s on); the editor's h2.setpos / player:setpos sets his group's world
  transform with the same virtual. The floor is the physics' (no call of its
  own).

### Game way, third round (2026-10-20)

* **Characters moved** (npc:setpos, the editor's gizmo, the start guard):
  ZActor's own SetWorldTransform (vfunc 0x80 = 0x463B10: its mover +0x54B and
  its group), as the levels' scripts' CopyObjectPosDir; the editor no longer
  writes the actor's point (+0x57C), which follows the mover (seen: a C1-1
  guard sent into the library walks on from there).
* **The level's points** are made with the map (inject.h SceneCopy::point: a
  copy of the characters' aim helper record, ZAIMTHINGY, at the root), as the
  maps' own waypoints are records. Paths of goals and routes set while the
  point was in a room find it at the root by its name (Resolve); h2.find does
  the same, and the Problems list and the Mission row compare objects, not
  paths.

* **The scene's characters' settings** (npc_edits) are made part of their
  records' script blocks while the scene loads (inject.h SceneScriptEdit: the
  references by the objects' paths, the level's own by their names), so the
  script engine reads them as the scene's own when it makes the character
  (seen: a C1-1 guard made with WeaponTemplate GunDesertEagle instead of
  GunBeretta92). Changed while playing they are still written into the running
  script until the map loads again: replacing a running script leaves the
  guard groups and the level control holding the old one (a crash), and the
  game has no setter of a running script's members (its scripts write them in
  place too).

### The in-game map (M) (2026-10-23)

* The map is the scene's own: `<map>__MAP.zip` has a group per floor
  (`MAP_Outside`, `MAP_1stFloor`, ...: a ZGROUP_MapGroup routine with the
  floor's name, `C1-1/IngameMap/Outside`, and the location group of its
  rooms) with the floor plan as flat models, the texts as models and the
  fixed icons (stairs, exits: ZBoxPrimitive with ZGEOM_MapObject); the window
  (`AllLevels/IngameMap.zip`, ZWINDOW_IngameMap) shows a floor through a
  camera, its legend (`AllLevels/Interface/Map/Legend`) has the icon models.
* ZGEOM_MapObject (data 0x541320: an icon name, shown; ShowMapIcon switches
  it) adds its object to the window's set at init (0x541370; on the hardest
  difficulty not icon_enemy / icon_innocent / icon_police). The characters'
  and some doors' records have it.
* Opened (vfAC), the map shows the floor of Hitman's room (0x540C50: the
  room's parents, or the object's parent, up to a floor's group or location)
  and draws, for each object of the set on that floor and not hidden
  (0x540920), the legend's icon of its name at its place (CMapIconDraw:
  {icon, object} pairs, vfDC).
* A floor's data: the group's own, then the routine's name (`0x84`, a LOC
  key: `C1-1/IngameMap/Outside`) and location (`0x88`: a 1-based record
  reference, e.g. `C1-1_LOC_Outside.zip`; C6-1's one floor "City" has the
  whole `C6-1_LOCATIONS.zip`). Every scene has its map under one
  `<map>__MAP.zip` group (C6-1's inside its CHARACTERS group, C0-2 uses
  C0-1's); 1 to 11 floors, 4 to 42 fixed icons.
* The window's camera (ZCAMERA +0x1C: the geom it draws, 0: the world)
  draws the shown floor's group; the window puts its icon drawer onto that
  floor with the group's AddChild (vfunc 0x26C (child, 0, 1) -> 0x43E6C0).
  The floor plans are models at the world's places (the camera frames the
  floor's bounds), not in rooms: no world camera draws them and nothing
  collides with them (static models collide through the rooms' trees).
* Without a floor for Hitman's room the window shows an outside floor of
  the game's maps by name (0x53EDA0), else its first: a map copied as a map
  of its own keeps its floors (their names are LOC keys) and works as the
  original.
* Done (2026-10-23, editor/ingame_map.h): a level's point gets an icon
  (inject.h SceneCopy::mapIcon: a copy of a map icon record at the point,
  static, put onto the floor of its place once the map has loaded;
  level.json `"map_icon"`).
  The level's models are drawn on the map (Level > *The level's models on
  the map*, `"map_models"`; always on a blank map): copies of one of the
  map's floor plan records with the level's models (SceneMapModel, made with
  the map at the root, flattened to stay under the icons), put onto the floor
  the window's own lookup gives for their objects (0x540C50, else 0x53EDA0)
  with the floor group's AddChild. A blank map keeps the map's group (its
  window and legend: before, it took them out with the buildings, so the map
  showed nothing) and takes out only its floors' plans and fixed icons.
  `h2.dev_mapicons()` reads what the map draws.
* An icon goes off the map with the routine's own message ShowMapIcon 0
  (its handler 0x541520: out of the window's set; 1 back; the flag +0x76 is
  saved with the game): the map leaves out only inactive objects (node flag
  0x400), not hidden ones (0x800), so an empty map's characters (their
  icons are on their groups' records) and a blank map's fixed icons are taken
  off it so, and shown again when the level is reverted.
* The mission's icons, made with the map as the game's missions have theirs:
  a character to kill or knock out gets `Icon_Target` (its group's
  ZGEOM_MapObject data with that icon: the map's characters' groups by their
  paths, SceneMapIcon; the level's characters as they are made,
  SceneNpc::mapIcon), the exit's point `Icon_Exit` unless it has an icon of
  its own. `h2.dev_mapicon(path)`: an object's icon and whether it is on the
  map.
* The game's other hostile important characters are `Icon_Enemy` (C1-1's
  MafiaSon, MafiaBrother, MafiaLawyer; `Icon_VIP` marks allies and people to
  meet: Vittorio, Sergei, the Ambassador): the base level's targets get it when
  its scripts no longer end the mission (Mission > *The base level can end the
  mission* off: their deaths count no more; SceneMapIcon `from`), and so does
  a character the level places from a target's template unless the level's
  mission makes it one.
* Found in the game (2026-10-23, all checked by map_icon_test, map_models_test,
  map_maps_test, map_own_test, gui_test):
  * The engine's rooms are large boxes: a dynamic object at the root put into
    a room by its place got a room of C1-1's 2nd floor for a point on the hill
    outside. The floor of a place is the map's floor of the editor's room
    there (RoomAt, which skips the in-game map's own models: its floor plans
    lie flat at the world's places), and icons and plan models are put onto
    it after loading, as the map's own icons are in their floors' groups.
  * The window is put among the interface's at init (GuiSetup / MenuSetup /
    ZWINDOWS): the map's group is found from its record's group (MapSetup.zip
    in `<map>__MAP.zip`), not from the window object.
  * The window hides the floors it does not show, and the map draws hidden
    icons (it leaves out inactive ones only): a blank map looks at the map's
    parts before it skips hidden objects.
  * A floor's AddChild keeps the child's local transform, and C2-1's and
    C7-1's mansion floors are moved (-18333, 7007) and turned (45 degrees,
    mirrored): the world place is set again after it (geom vfunc 0x84 / 0x80).
  * A copy whose source has no record the editor knows (C6-1's cutscene parts
    moved to the root) is made while playing: its plan models are made while
    playing too (engine::CreateObject in the floor's group, the flags of the
    floor's plan models).
  * The map opens only once a level's start is over (C4-1: about 20 s after
    loading; C6-1: after its arrival cutscene).
  * Not the map's, found with it: a character's body could be duplicated as a
    model (Hitman's object is his body; C6-1's RegularMaleIndian at the root)
    and the copy took the renderer down. DuplicateObject refuses models that
    are not plain (engine::IsPlainModel), InActor counts Hitman (ZPlayer).
* An icon taken off the map (empty map, blank map) is shown again on revert
  as the scene file has it (its routine's data, `MapIconShownInFile`), also
  when a saved game had taken it off already.

### Engine limits a level meets (measured 2026-10-24)

Stress levels on C1-1 (scratch tests: N copies of a tree, N points with map
icons) found what took the game down, each traced to the engine's own code:

| Limit | Where | Past it | The editor now |
|---|---|---|---|
| 510 room lists of moving objects (4096 since 2026-10-24) | geom table pool (made by 0x427C90: 0x1FE entries of 0x20 bytes at table +0x51, a stack of free u16 indices at +0x55; vfunc 0x50 = 0x427CE0 takes one, its only limit "cmp edx, 0x1FE" at 0x427CE3; count at +0x59; a node keeps its own index at +0x5C, 16-bit). The maker is hooked (4096 entries, the engine's allocation as its own) and the taker's 0x1FE patched to 4096: a level of 2000 copies, all of them moving and drawn, a saved game of it loads. Every dynamic node in a room and every auto-assign one takes one (SetDynamic 0x5BAA00 -> 0x5BD1B0, auto-assign 0x5BDBD0 -> 0x5BD760) | the allocation returns 0 and the caller reads through null (0x5BD714): 425 copies of a tree on C1-1 (its own moving objects take about 100) | `engine::RoomListsFree`; ZBaseGeom::SetDynamic and the auto-assign hooked (refused when none is left); a level's moving records made dynamic after the scene's own, only while more than 128 are free (`kRoomListsKept`, the game's own characters, bodies, thrown things); Duplicate / place item refused past that; the editor moves a static object only when it can be made dynamic |
| 1000 dynamic objects at load | CreateGeoms' ZStackArray<1000, SMakeGeomDynamic> on AllocSequence's stack (MakeDynamicGeomsDynamic 0x41E410, MakeAutoAssignGeomsAutoAssign 0x41E470) | INT3, then written past the array over the stack | the level's records with 0x40000 are made without it (0x40000 and 0x4000 off, as CreateGeoms does: 0xFFFBBFFF) and made dynamic / auto-assign after the scene's (hooks on both): they take no room there (C1-1 has 102 of its own, C8-1 336) |
| 0x2000 records with a data block at load | CreateGeoms' stack of blocks to remap | INT3 | inject.cpp counts the scene's (C6-3: 4413) and leaves out the level's objects that would not fit |
| 256 icons on a floor of the in-game map | CMapIconDraw (0x53D1E0: 0x800 bytes of pairs, count +0x18) | INT3 when the map is opened | the add hooked: the icons past 256 are not drawn (`engine::InstallIngameMap`) |
| 2000 light table entries made while playing | every scene file's light table (GMS +0x18; 0x428880: words[2] the pool of 8-byte entries, words[1] the 16-byte nodes) has room for 2000 entries that nodes take while playing (vfunc 0x24 -> 0x4286F0 / 0x5C0870: none left, 0). The editor gives each copy one of its own (OwnLightEntry) | 1999 copies took them all; the copies left sharing the empty entry broke the table (an exception in 0x428220 when one was destroyed) | inject.cpp makes both pools larger in its GMS copy by the level's records + 512 (MoreLightEntries; the handles point below them); "no entry left" is logged |
| the routines' arena (about 1800 points with map icons on C1-1: a load that never ended or crashed) | GeomTable's ctor (0x424150) makes it [GMS +0x1C] + 0x46000 bytes (GeomTable_MakeRoutineArena 0x427270): the scene file's routines and 0x46000 for those made while playing; every routine takes align16(class size + 4) of it (GeomTable_AllocRoutineInArena 0x427380, unchecked). The level's routines (a map icon's ZGEOM_MapObject: 0x80) ate the 0x46000, then a door's ZGEOM_Action was built below the arena: no handle, NULL from 0x488A70, a crash in the door routine (0x4CC4C0 -> 0x488F80), or memory overwritten (the routine list 0x41A9B0 going round in a circle in InitializeGeoms) | the load never ends, or a crash (found with Frida, 2026-10-24, issue #1) | fixed: the level's records add their routines' bytes to GMS +0x1C (inject.cpp `RoutineArenaBytes`, the class size from the game's GetClassInfo 0x4213F0); 1800 icon points load, the arena keeps the free bytes it has without a level (193 KB on C1-1). Measured (issue #9): C1-1 builds 1580 routines, 516 880 bytes while CreateGeoms runs (the formula gives exactly that, with ZLIST_CheckVisible's 88 174, the largest class) and 385 more, ~100 KB, at InitializeGeoms; the scene file's +0x1C (501 192) is the exporter's estimate, 15 KB short of what loading takes: the 0x46000 reserve covers it. The 256 points on the map stay (the floor draws 256) |

And two in the editor itself, found with them:

* The editor's snapshot of the scene (editor/model.cpp SceneModel): a
  replaced one lives until the frame ends (a reference somebody iterates
  stays valid). Loops that took one for each object kept them all: reverting
  600 copies (RetireCopy -> VisibleParts) and putting 450 points' icons onto
  the map's floors (RoomAt used SceneModel(0)) ran the 32-bit game out of
  address space (std::bad_alloc in TakeSnapshot, the game at 1.6 GB). The
  parts are found before anything is retired, RoomAt and SpawnClone use the
  last snapshot (`RecentSceneModel`), and 32 snapshots in one frame are
  logged.
* Placing a copy made with the map where it already is no longer makes it
  dynamic (SetTransform with the same place).

### C5-1: a saved game made in the level's first seconds crashes when loaded (found 2026-10-23, explained 2026-10-24)

* Found by maps_gameway_test (its first run on C5-1): a saved game made and
  loaded took the game down in its first frame drawn (RenderD3D 0x1001F8C0,
  the characters' bone animation: a packed array of 29-byte bone entries,
  0x1001EDA0, read through a pointer the saved game gave back wrong; eip the
  float 314.53).
* Not the level's: with no level at all, and with the DLL of the morning of
  2026-10-23 (master), the same. The test saved 8 s after C5-1 came in, while
  its start camera still moves; a save made 20 s or 60 s in loads (an empty
  level, a level with a copy). The game's own way to save is its in-game menu
  (0x555C60 -> ZEngineDataBase::SaveGame 0x41AAD0); h2.dev_savegame calls
  SaveGame directly, at any moment. maps_gameway_test now saves 25 s after a
  map came in, as a player would.
* Whether the in-game menu can be opened that early on C5-1 is not checked
  (h2.dev_key's Esc did not open it there in the first minute either).

### Direct writes into the game's objects (audit, 2026-10-18)

Every place the editor writes a game object's memory itself instead of calling
the game (the source scanned for writes through casts, `->` and `memcpy` into
game pointers; each one looked up in Ghidra):

| Where | What | The game's own way | Verdict |
|---|---|---|---|
| editor/model.cpp `SetHidden` | node flag 0x800 set / cleared | **ZGEOM vfunc 0xC0** (0x43B9C0 -> ZBaseGeom 0x5BAEF0 (node, hide)): besides 0x800 (through SetFlags 0x5BAC80) it takes a dynamic node out of its room and the bound / collision lists (0x5B99F0, room vfunc 0x304) and puts it back when shown (room vfunc 0x2FC, 0x5BD1B0, 0x5B96F0); used by ZLIST_CutSequenceShowHide, ZGROUP_HideGroupObjects, ZMoveOnSpline, CheckInside (+0x7E) | **to change** (B15): a hidden object would then stop colliding as the game's do |
| editor/vehicles.cpp `OwnLightEntries` | light table handle (+0x6C), flag 0x1000000 | SetFlags' 0x8000 branch does the same (light table vfunc 0x24, +0x6C, 0x1000000), but its "clear" half unregisters the shared handle first | as the game's set branch; kept |
| script/objects_api.cpp `h2.health` | Hitman's hit points +0xD72 | the game writes the field itself (difficulty 0x511BB0, refill 0x5150E1, level boxes 0x4F8B74...); no setter | kept |
| editor/music.cpp | MusicSegment +0x7A, MusicControl +0x1E4 | the routines' own data load / ctor write them; no setter | kept (B7) |
| engine/npc.cpp | a script instance's variables; the actor collision's buffers (its init hooked) | settings made with the map since 2026-10-20 (the scene's script blocks); changed while playing: written in place, as the scripts write their members (no setter; a new instance crashes the guard groups) | done / kept |
| editor/testtools.cpp | the game's cheat / debug values | the game reads them as they are | kept |
| fov.cpp, display.cpp | camera fov / far plane around SetupProjection (vfunc 0x258); SysInterface width / height | through the game's SetupProjection; values the game reads | kept (display fixes) |

### Routines attached while playing (2026-10-18)

* A geom's routines: vfunc 0xE8 / 0xEC find one by class name (0x43C330);
  0xF0 (0x43BC00) makes one (ZEngineDataBase::AllocRout 0x421000 by class
  name), sets its owner (routine vfunc 0x7C), adds it to the geom's list and
  tells the geom (vfunc 0x104); 0xF4 (0x43C1F0) does the same and marks it
  made while playing (routine flag 2). ReadData2 (geom vfunc 0x130) copies each
  routine's section of the data block to routine +0x38. A routine's state
  machine (ZEventBase vfunc 0x50 = 0x42ABB0): 1 init (its data read from
  +0x38, flag 4), 2 start (flag 8); a door's action is attached this way
  (0x488A70: vfunc 0xF4, then 0x50 (1, geom, 0), its entry, 0x50 (2)).
* Seen in the game: a ZBoxPrimitive made with CreateObject with a CheckInside
  attached by 0xF4 and started by 0x50 (1) / (2) works (Hitman in / out), but a
  saved game with it does not load: the load makes flag-2 routines again
  (0x426070) and CheckInside's load (0x48AC40 -> 0x4FC060) reads its data from
  +0x38, which the save does not keep (0x5C0100 reads from null). The game
  attaches only routines without data so. **Places are zones only as records
  made with the scene** (D11): a place made while playing is checked by
  distance until the map loads again.
* **h2.drive (D14, 2026-10-18)**: stays a script tool (any object along
  points, by the editor). The game moves objects on paths with ZMoveOnSpline
  (its vehicles: the level's vehicles are ZCar records already, h2.track
  drives them); its follow routines are hardly used (ZGEOM_WaypointFollow:
  C0-1's two pigeons; ZGEOM_FollowShape, ZGEOM_FollowGeom: no scene), and a
  routine with data cannot be attached while playing (a loaded save).
* **B4, characters made while playing (D15, 2026-10-18)**: the game makes
  none: ZGEOM_GeomSpawn (1848 records) spawns effects (bullet impacts),
  ZGEOM_CreateInstance is in no scene; every character of the game is a
  record. h2.npc's characters stay runtime objects (shown until the map
  loads again); the level's characters are records already.
* **B16 done (2026-10-19)**: a copy of a plain model of the map keeps its
  record and is made with the map from then on (engine/inject.h SceneCopy:
  a copy of the record at the root, as a car's parts), so the game offers its
  actions and saves it as its own (copies_test). Copies of groups (a car
  model: an empty group with copies of its models' records, as a level's car)
  and of other levels' models (a plain model record of the map with that
  model) are made with the map too (copies2_test, 2026-10-19); a blank map's
  floor stays made while playing (put into the room it is in). Found with it: the game
  offers a "use" action only around Hitman's chest (ZHITMAN2 vfunc 0x638: the
  object's box widened by 90 / 40 / 90, the same room), so a take goal's
  object on the floor is taken with E near it.
* **B16, the level's copies of models (found 2026-10-18)**: the editor
  builds a copy itself (CloneGeom: the geom table's create, mesh, transform,
  flags, the parent's AddChild, init); the game offers no action of such a
  copy. The game copies an object while playing with geom vfunc 0x1A8
  (0x43B0F0, as GeomSpawn does: (parent, world matrix, world position, 0, 0,
  0, 0) -> the clone vfunc 0x1A4, Init 0x148, its routines' states 1 / 2
  (vfunc 0x10C), the initialized flag, PostInit 0x14C; groups are not
  copied): such a copy gets the action offered (h2.dev_instantiate, seen in
  C1-1's library). It keeps the source's name (the editor's paths show it as
  "#1"), while the level finds its copies by their own names ("+1"), and a
  name in the engine's heap breaks a saved game (P39): the copies' names (or
  copies as records made with the scene, as the characters are) come first.
* **B2 / B3 (D16, 2026-10-18)**: the game removes an object with its
  deleting destructor (geom vfunc 0x1FC (1): ZGEOM_TimeOutDelete; cut
  sequences send MSG_DELETEOBJECT), as the editor removes its own objects. A
  character the level placed and the user deleted is not made with the scene
  any more (the next load: as the game, no record); until then it is hidden.
  An emptied map hides the scene's characters and items: leaving their
  records out or making them inert would hand the level's own code (it finds
  characters by name, e.g. C2-1's target "KGBOfficer_01") something that is
  not a character. The hiding is the game's own (B15: out of the rooms too).
* **B6, the sky (D17, 2026-10-18)**: giving the map's own backdrop records
  the other level's models at load would fit only as many as the map has
  (C1-1: 8 models, C3-1: 42, C4-3: 98; C2-1, C5-x have no backdrop group),
  and new records cannot go into that group (the indices after it would
  move). The other level's sky stays made in the map's backdrop group when the
  level is applied.
