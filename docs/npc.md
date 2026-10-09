# NPCs and their behaviour (reverse engineering notes)

Addresses are hitman2.exe (Steam build, unpacked dump `re/hitman2_dump.exe`).

## What an NPC is in a scene file

An NPC is a ZGROUP (class id 0x100001, e.g. `BodyguardMafia_Bedroom_00`)
holding one record of class **ZTRACKLNKHM2** (0x2003E9, derived from ZActor,
node name usually `Ground`). The body parts seen at runtime under it are made
by the actor itself; the record only carries:

| record field | NPC use |
|---|---|
| +0x0C mesh   | body mesh (e.g. 0x35D34A) |
| +0x18 flags  | 0x238082 typical |
| +0x14 events | optional event list |
| +0x20 data   | typed data block: animation set, model name, refs (`Worlds\Characters\_Anims\Reg_Male\Reg_Male.anm`, `BodyguardMafia_01`, ...) |
| +0x28 script | the **behaviour**: SDL script name + parameters |

Waypoint-like helpers are plain objects: `ZWaypointCover` (0x200420),
`ZCoverBlock`, `ZWaypointHelp`, `ZWaypointBeacon`, `ZWaypointAlarm`,
`ZPathfinder3`; most AI points are just named ZSTDOBJs (`ProtectPoint1`,
`SafePoint_*`, `GuardWaypoint`, ...) referenced from script parameters.

## Typed data blocks (DBLK)

Data, script and property blocks share one format: `u32 header` (size in the
low 24 bits, bit 31 = references already remapped) followed by tagged values
(tag & 0x3F):

| tag | value |
|---|---|
| 1 | 8 bytes (double) |
| 2 | FLOAT, 4 bytes |
| 3 | LONG, 4 bytes |
| 4, 5 | NUL-terminated string |
| 7, 9, 0xC | u32 length + bytes (9: array, 0xC: nested block) |
| 8 | ZGEOMREF: in the file `0x80000000 | record index (1-based)` |
| 10 | message id (index into the scene's message name list) |
| 11 | sound reference |
| other | tag only (6 = separator) |
| 0xFF | end |

`ZEngineDataBase::RemapCreatedDataBlock` (0x41CD80 -> 0x5BE7C0) rewrites the
refs with the table of created geoms (`refs[index-1]`) and message ids;
DBLK reader object: `{vtbl 0x65D81C, size-4, cur, start}` set up by 0x5BE550.

## Behaviour = SDL script class

Script records: `u32 size | 0x84 name\0 | 0x84 declaration\0 | values... | 06 FF`.
Example (C1-1 bodyguard):

```
scripts\C1\C1_1\BaseGuard.sdl
LONG m_iHitpoints=20; LONG Mute=0; ZGEOMREF WeaponTemplate; LONG MayHolsterWeapon=1;
FLOAT ReactionTime; ZGEOMREF GuardPoint; ZGEOMREF m_Arbiter; LONG IsSentry=0;
FLOAT SentryRadius=500; FLOAT RunAnnoyance=1; FLOAT SneakAnnoyance=1;
FLOAT TrespassAnnoyance=1; FLOAT Xenophobia=0; LONG PostManAllowed=1;
LONG AtKitchenGate=0; ZGEOMREF ProtectPoint; ZGEOMREF m_rDistractPoint;
ZGEOMREF m_PanicArbiter;
```

The compiled scripts are in the scene's **.ZGF** (next to the fonts), as an
inheritance chain per class, e.g.
`Being2 -> Man2 -> Armed2 -> Guard2 -> BaseGuard -> DoorGuard/GarageGuard/...`,
`Neutral2 -> PostMan/Maid/DeliveryBoy`, `GenericVehicle -> GenericCar -> Van`,
with threads (`Thread_Protective`, `Thread_GoPiss`, `Thread_TooClose`, ...).
The engine ("SDL_Engine.dll", statically linked; object at
`g_pScriptDll` 0x6A6C74, vtable 0x65F1C0) runs them:

| vfunc | |
|---|---|
| 0x3C | create instance (name, params, this geom ref) |
| 0x40 | register message name |
| 0x44 | start instance |
| 0x48 | parse parameters (params, record+4) into a parameter object |
| 0x54 | new parameter object |
| 0x58 | free parameter object |
| 0x68 | destroy instance |
| 0xA4 | register interface type (`TrackLnk_HM2_if`, `GuardArbiter_if`, `Vehicle_If`, ...) |

Native messages scripts react to are registered in 0x568850 (`guardwaypoint`,
`alarm`, `talkto`, `msg_walktoref`, `hitmanspotted`, ~550 names).

### How a geom gets its script (ZEngineDataBase::CreateGeoms 0x41CDA0)

After all records exist, for each record with a script:

```
DBLK d(rec); if (!(rec[0] & 0x80000000)) { rec[0] |= 0x80000000; RemapCreatedDataBlock(&d, refs, n, msgs); }
geom->vfunc44();                                // makes geom+0x10 (extra data)
list  = geom[4]+0x1C ?: new {count, entry}      // engine alloc 0x530490
entry = new {0, instance, name, params}         // 16 bytes
entry.name   = new char[0x200], strcpy(rec+5)
entry.params = g_pScriptDll->vfunc54()
g_pScriptDll->vfunc48(entry.params, rec+4)
list.entry = entry; list.count++
```

The instance is created in **PostInit** (ZGEOM vfunc 0x14C = 0x43CE30 for
plain geoms; for ZTRACKLNKHM2 0x508010 -> 0x462E80 -> 0x47EDA0 -> 0x46FF40 ->
0x43CE30):
`g_pScriptIfDll->vfunc40(thisRef, "Geom_If", "<ThisGeomRef>")`, instance =
`g_pScriptDll->vfunc3C(name, params, thisRef)`, `vfunc44(instance)`; name and
params are freed afterwards.

Data blocks: `geom->vfunc1B8(&dblk)` then `geom->vfunc130(&dblk)`.

### Level start order (InitializeGeoms 0x41E920)

1. Init (0x148) for every geom
2. two passes over the list at engine+0x5D13: `vfunc50(1,0,0)`, `geom->vfunc1D4()`; `vfunc50(2,0,0)`
3. `SetFlags(0x800000)` and PostInit (0x14C) -> scripts start

## The GMS is freed after loading

`AllocSequence` (0x41DA90): `GetGeomsData` (0x41C1C0) reads the packed GMS
(header: unpacked size, packed size, flag), it is inflated, CreateGeoms runs
and the buffer is freed. The BUF stays (engine+0x1C47). To spawn NPCs later
the editor keeps a copy of the unpacked GMS (CreateGeoms hook) and of the
record -> geom ref table (RemapCreatedDataBlock hook).

## Checked offline (tools/gms.py over every `*__MAIN.ZIP`)

* 891 script records, all parse as declaration + one value per declared
  parameter + `06 FF`; parameter types: ZGEOMREF (4452), LONG (3477),
  FLOAT (1438), SndSetRef / SndRef (tag 11, 72).
* 191 different NPC behaviours, e.g. PatrolGuard32, SewerRat,
  IndianPedestrian, EntranceGuard, GuardRandom, SimpleSniper, CastleNinja,
  BaseGuard, Pedestrian2, Guest, Merchant, Maid, Nurse, SleepingGuard...
* 717 actor data blocks, all walk to their end marker with the tag rules.

## Editor implementation (first version)

`src/engine/scene_data.cpp` keeps the GMS/BUF and the ref tables,
`src/engine/npc.cpp` builds an NPC from an actor record (Create
"ZTRACKLNKHM2", mesh, flags, AddChild, world transform, script entry, data
block, Init + the init list phases, SetFlags 0x800000, PostInit) and logs the
step it fails at. Not yet: events (+0x14), saving NPCs in the level, removing
them, the parent ZGROUP of the original.

## Behaviours that move the character when they start

Checked offline over every level's scripts:

* `LONG WarpToPathPoint` (28 patrol scripts, e.g. C2-1 PingPongGuard,
  PatrolGuard2_1; authored 1, sometimes 0) with `LONG StartPathPos`: the
  script puts the character on that point of its route (`WayPoints`) when it
  starts; 0 = it walks there from where it stands.
* `LONG startAtRandomPathPoint` (Pedestrian2, Pedestrian2_1, IndianPedestrian;
  afghanPedestrian has 0): a random point of `WayPointList`.
* Start point references to one positional object (class 0x200002):
  Mother `StartPos`, Sergei `StartPoint`, Chef `startPos`, K4Sniper / K5Guard
  `SpawnPosition`, SewerGuards / SubwayPatrolGuards / PatrolBodyGuards
  `m_rStartPos`, C8-1 sniper `rGardenSpawn`. Lists of start points:
  RandomPetronaGuest / FoodDeliveryBoy `startPointList`, Mr17
  `m_rStartPositions`.

A placed copy of such a character would jump to the original's start, so
the editor turns the LONGs off and points a single start reference at a new
point where the character is put (`StartMoves` in src/editor/npcs.cpp).

Behaviours also move their character by their own code (C1-1 DeliveryBoy:
crates, van and exit references, no start parameter), so the editor also
watches a placed character while its behaviour starts (`KeepStartPlace` in
src/engine/npc.cpp): for 10 s (at least 120 frames) a jump of more than 250
units in one frame, or being away from where it was put on the first frame,
puts it back there (up to 10 times); it walks its route from there.

## Placed characters are made with the scene (2026-10)

A character made while the game runs (SpawnNpc) is bumped into by other
characters but not by Hitman, and cannot be shot. The level's characters
are therefore added to the scene file while it loads (src/engine/inject.cpp):
the editor's Place shows a character without behaviour or collision until
the map is loaded again (Test in the toolbar); then it is
one of the scene's and gets its behaviour. Another kind of behaviour for a
placed character also waits for the next load (a running script cannot be
replaced). Lua's h2.npc still makes a character at once (NpcSpec::now).

Only behaviours that work anywhere are offered (IsPortableBehaviour: their
references are guard / protect / distract points, routes, help and safe
points, weapon template, guard group, alarm and level control), and the
game's plain civilian `scripts\AllLevels\Std2\Neutral2.sdl` (HelpPoints,
SafePoints) when the scene's ZGF has the class (its state Neutral2.normal).
Checked offline: C1-1 offers BaseGuard and Neutral2; behaviours like Chef,
PostMan (its Flowers), DeliveryBoy (crates, van) are not offered.

## Edit mode and Test (src/editor/playtest.cpp)

While the editor is open on a map the world stands still: the engine's own
`ZEngineDataBase::PauseScene(true)` (0x41B890, flag engine+0xB9C). MainLoop
then skips the scripts (g_pScriptDll vfunc 0x9C) and TimerUpdate runs only
the timers flagged 0x100; FrameUpdate (the editor's camera) still runs. The
game's own pause is left alone (only a pause the editor made is undone).

Test saves the level and loads its map again, then closes the editor.
Hitman starts where he is, where the camera looks (pick at the screen
centre) or at the level's start: his start is his group's record (parent
of the ZHITMAN2 record, class 0x2003E8; its ancestors are at the origin on
every level, checked offline), whose matrix and position are replaced in the
GMS copy while the scene loads (inject.cpp MovePlayerStart), so the engine
puts him there itself (rooms included). Closing the editor while placed
characters are not made yet asks whether to test first.

## Behaviours of other levels (src/engine/script_files.cpp)

The scene's **ZGF** is a chunk tree (tools/zgf.py): fonts, particle palettes
and the compiled script classes `scripts\<dir>\<Class>.ppo`, base classes
under paths relative to their subclass (`scripts\C1\C1_1\..\..\AllLevels\
Std2\Neutral2.ppo`; one class can be there several times). AllocSequence
reads it (GetGeomFilesData), mounts it on ZSysFile (vtable 0x639A18, vfunc
0x84 -> archive object 0x5B5F40, lookup 0x5B6280) and unmounts it after
InitializeGeoms (0x8C). The script engine loads a class by path
(0x5D5BA0: cached by name, else "Cannot load script" + int3) through
SysInterface vfunc 200 (".sdl" -> ".ppo") and ZSysFile Exists 0x40E2C0 /
Size 0x40E1B0 / Read 0x40E540, which ask the mounted archives first and then
the disk. Loaded classes stay cached for the scene: that is why a class of
the scene can be given at runtime, and why Neutral2 is only found under the
name its first subclass loaded it as.

The editor hooks those three: when neither answers, the .ppo files of the
levels it was asked for (ProvideLevelScripts, read from their ZIPs, keyed by
the normalised path) are served from memory. A behaviour of another level
("Guard2_1 (C2-1)") is its class path with that level's declarations; its
references are empty and filled from this scene's characters' parameters of
the same name (FillSharedRefs: weapon template, guard group, help and safe
points, alarm list, ...). Checked in C1-1: C2-1's Guard2_1 runs (Guard2.Guard)
across reloads.

Parameters never empty in any level's characters (offline count over every
scene) are required: a behaviour without one (WayPointList, WayPoints,
HelpPoints, ...) is not started (C7-1's Pedestrian2 without a list crashed the
script engine 2 s later, a call on a null reference in 0x5D67A0). Behaviours
tied to their level's furniture without a reference (sitting, sleeping,
patients...) are not offered: C6-3's DoctorSitting on C1-1 made the game free
a room twice when the map was left.

## Interface methods (calling them from the editor)

Script interfaces are registered with the script engine by name (0x5781E0:
Engine_If, Dialog_If, Item_If, Scene_If, Sound_If, Geom_If, Link_If,
Actor_If, List_If, ConditionList_If; 0x568850: LevelControl_if, GUI_if,
TrackLnk_HM2_if, GuardArbiter_if, Boid_If, Vehicle_If, Car_If,
MoveOnSpline_if). Their methods have **no names in the exe**: each
interface keeps a table of functions in registration order and a dispatcher
that gets a call context (`+0x04` argument list `{slots**, first, count}`,
slots of 8 bytes `{value, type byte}`, `+0x08` the result list, `+0x10`
the method number u16; ECX = the interface object, `+0x18` the actor). A
number past the interface's own goes on to its base, minus its count:

| interface | table | count | registered in | dispatcher | base |
|---|---|---|---|---|---|
| TrackLnk_HM2_if | 0x6B3990 | 88 | 0x56F2A0 | 0x56FA90 | Actor_If |
| Actor_If | 0x6B3F60 | 37 | 0x583A20 | 0x583D70 | Link_If |
| Link_If | 0x6B41D0 | 12 (thunks to the interface object's vtable) | 0x587920 | 0x587A30 | 0x589710 |

The **names are in the .ppo**: a symbol table (u32 offsets of identifier
strings, relative to the table's end, the sorted string pool right after
it); byte code calls a method as `97 14 00 <symbol u16> <number u16>`
(tools/ppo_methods.py lists them). Calls on `this` (`2f ff ff 97 14 00`) in
Being2 (its `this` is Actor_If): UseMoveSet 7, MoveToPosition 8,
MoveToLocation 12, MoveToLocationForced 13, LookAt 26, PlayAnim 43,
PlayAnimSegment 44, CanPlayAnim 45, PlayAnimLoop 46, FaceObject 53 (43 and
up: past Actor_If's 37).

What the Actor_If methods do with the actor (its vtable):

| method | Actor_If | the actor's calls |
|---|---|---|
| UseMoveSet(name) | 7: 0x583F70 | id = 0x574(name), 0x564(id) |
| MoveToPosition(x, y, z) | 8: 0x5842C0 | 0x5B4(1.0), 0x5C0(&path), 0x594(0), 0x58C(&path, pos) |
| MoveToLocation(ref) | 12: 0x5849A0 | the object's world mat / pos (0x84), 0x5B4(1.0), 0x5C4(&pos, &mat) |
| LookAt(ref) | 26: 0x584E20 | 0x610(ref) = 0x4687C0: keeps the target (+0x564), turns at once only while looking (+0x105 bit 0); 0x37C = 0x46F630 starts the head turn (0: stops) |
| (19) | 0x584E00 | 0x5B8(1) |
| PlayAnim(handle) / PlayAnimLoop | Link_If 6 / 9 (43 / 46 from Actor_If): interface vfunc 0x70 / 0x74 = 0x587CC0 / 0x588110 -> vfunc 0x58 = 0x588B80 | the actor's 0x214(anim, 1 (5: loop), 0, -1.0, 1.0) |
| getAnimHandle(name) | Link_If 5 (130 from TrackLnk_HM2_if): 0x587A70 -> 0x588DF0 | anim = 0x2A0()->0x18(name), else 0x29C()->0x18(name); handle = anim - engine+0x40 |
| FaceObject(ref) | base 4 (53 from Actor_If; table 0x6B42B0, registered 0x5892F0): 0x58AB10 | sets the world matrix towards the target, level (0x80) |

The editor's Lua npc:walkto / lookat / moveset make the same calls
(engine/messages.cpp; lookat also 0x37C when the character is not looking
at anything yet). Seen in the game: walkto walks a level's character (with
or without a behaviour) and a character placed in the level (made with the
scene) to a point of the level's path network (pathrequest, pathfinished;
a point off it: patherror, e.g. Hitman's start hill on C1-1); characters
made while playing (h2.npc) walk as well (Actor_MoveTo 0x463DA0 asks the
scene's rEnemyPathFinder3 at actor +0x513, set in PostInit 0x462E80 for
every actor). moveset("Run"):
about 3 times the walking speed. lookat turns the head. npc:animation(name
[, loop]) plays an animation of the character's sets (e.g. Stand_Scared_Alt01
looping); npc:faceto turns it at once (a character's face is opposite its
node's forward row).

## The path network

ZPathfinder3 (class 0x200075, ctor 0x5992B0, vtable 0x65C8C0; the scene's
com value rEnemyPathFinder3) loads the network from its record's data block
(tag 7, magic 0x1923483: 0x5995A0 -> 0x597C00 -> 0x59F0B0)
into the object at pathfinder +0x64 ("nav"). It is a grid
of cells over the map (0x59B530, world -> cell): min / max corners as
float[3] at nav +5 / +0x11, the cells along x, z, y at +0x3B / +0x3F / +0x43;
a cell is u16 [x, z, y] (y up).

RequestPath (vfunc 0x200 = 0x599930 (ref, from, to); pathfinder +0x78 must
be set) lifts both points by pathfinder +0x20 + 30.0 (0x639850) and looks up
their cells: 0x59B4A0 takes the point's cell (0x59B4E0) and the nearest
walkable one 0x59B610 (nav; out, in, 10, 1, 1.0) in the box one cell across
and 10 down. A point with no such cell is off the network (patherror).
0x59A8E0 (nav; from, to, char line) does it for two points: with the same
point twice and line 0 it is true for a point on the network.

The editor: engine::OnPathNetwork / PathNetworkGrid / PathCellBelow
(engine/messages.cpp), Lua h2.onpath(x, y, z | obj), the Testing panel's
"Path network" (the cells found column by column around the camera, drawn as
dots).

## Death, Hitman's life, moving Hitman

Actor_If 2 is Die (0x585000 -> the actor's vfunc 0x5F0 = 0x5092C0): it
drops what the actor holds, then 0x468D80 kills it (vfunc 0x5DC(0), alive
+0x574 = 0 through vfunc 0x540, 0x5B8(1), 0x5BC, 0x3CC). The editor's kill
uses it for a character without a behaviour (Msg_InstantDeath only reaches
a behaviour). IsAlive is vfunc 0x544 (+0x574). Hitman (ZHITMAN2) is not a
ZActor: his hit points are the float at +0xD72.

Moving Hitman: his "Ground" node (ZHITMAN2) sits at the origin of its group
(HeroSetup.zip/Hitman2), and the group is where he is. Moving the node itself
(SetWorldTransform) gave it an offset from the group and flung him out of
the world, even 2 m away. The editor moves the group (editor::SetTransform,
so setpos / setrot and the gizmo): tested on C1-1 from the start hill to the
front gate, he stands, turns and walks. His object keeps the floor triangle
he stands on (+0x45C, +0x468, +0x474 corners, +0x480 normal, ids at +0x44C /
+0x490).

## Characters and rooms

A character is drawn with the room its own per-frame update finds it in:
teleported (TeleportActor: +0x57C and the group's transform) it shows in
the new room as soon as that update runs; the engine paused (PauseScene,
the editor's Freeze) does not run it, so the editor lets the world run 3
frames after moving one (editor::StepWorld). Checked on C1-1: the front
gate guard into a first-floor bedroom and back, paused and running.
Drawn this frame: its node's +0x58 high word is set (a render slot) and
flag 0x02000000 is clear (0x1000 set).

Not solved: a character made while playing (h2.npc / SpawnNpc) moved into
another room is not drawn there, even with the world running (C1-1: made at
the front gate, moved into the bedroom). Ruled out: actor +0x124 (the floor
material's +0x30, footsteps, from 0x464B83), the rooms' +0x97 lists (their
ZGATE portals, tested for visibility 0x432320 -> node flag 0x1000), node
flag 0x08000000 (a node whose ref is not from the static pool:
GeomPtrToRef 0x426AF0 / 0x4269E0; setting it crashes).
