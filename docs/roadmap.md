# Roadmap: research notes and plans (2026-10)

What was found while looking into the larger questions, what works now, and
the next steps. Addresses are hitman2.exe (dump) unless noted.

## 1. The NPC system (what is known)

* A character is a ZTRACKLNKHM2 actor (class 0x2003E9) in a group, with an
  aim helper (ZAIMTHINGY). Made at runtime it is bumped into by other
  characters but not by Hitman and cannot be shot: placed characters are
  added to the scene file while it loads (src/engine/inject.cpp, docs/npc.md).
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
2. New route lists made at load time (records added like the characters),
   so a patrol / pedestrian behaviour gets a route the user draws.

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
* Next steps: (1) the game's own vehicles on their own splines through their
  script interface (InitCarPosDir 0, MoveToRef 3, Stop 7, MoveToSplineVal 8,
  DriveFwdToSplineVal 10, SetMoveParams 14, SetPath 23); (2) the ZSHAPE
  record format, then new splines the game's vehicles can use; (3) drivers /
  passengers (C2-2 LimoGuard's rLimo + seat number); (4) what a driven car
  does to characters in its way (not checked yet).

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
instead of the map's (Level panel, h2.sky). Next: the sun / glow (Z2DOBJ)
and lens flare with them, the fog colour and light of the map to match.

## 10. Reviews (done this round)

Security: Lua sandbox (text-only load, time and memory limits), level files
from elsewhere (fields of the wrong type, NaN transforms, path names, level
names, character names and counts), game texts (control characters reach an
unchecked array), script inputs (model records, coordinates). Bugs: saved
games with the level's characters, the GMS copy leak, a freed camera during
loading with the editor open, the blank map keeping the clouds, the sky's
objects destroyed when it changes, testing values not fighting the game's
cheats.
