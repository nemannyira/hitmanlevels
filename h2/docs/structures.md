# The game's structures

The fields of hitman2.exe's objects that the editor reads or writes, in one
place (2026-10-18). Each comes from Ghidra (the function that reads or writes
it); the ones marked "seen" were also read in the running game (Cheat Engine,
C1-1). The code that uses a field has the details next to it. The Ghidra
project has `ZBaseGeom`, `ZClassInfo` and `ZGEOM_Header` as types
(hitman2_dump.exe).

## Engine globals

| Address / field | What |
|---|---|
| 0x6A6C5C | `g_pSysInterface` (ZSysInterface*) |
| sys +0x57 | fullscreen (bool) |
| sys +0x64 / +0x68 | the resolution (Hitman2.ini), also the GUI's virtual screen |
| sys +0x6C / +0x70 | the window size (RenderD3D copies it) |
| sys +0x98 | ZEngineDataBase* (the scene) |
| sys +0x7B4 | set while the game logic runs: reference tables made while loading are locked (`game.h`) |
| sys +0x193C | game speed: the factor of every frame's time (`testtools.h`) |
| sys +0x18A8 | the save slot while a game loads (LoadGame2 0x41B1C0) |
| 0x6A6C74 | `g_pScriptDll` |
| engine +0x04 | the geom table (vtable 0x63D540): +0x14 node buffer, +0x18 object buffer |
| engine +0xC37 + id*4 | message names (GetZMsgName 0x41EEB0); engine +0x1C43 the last id |
| engine +0x5D13 | two-phase init entries (InitializeGeoms) |
| engine +0x5D1F | the scene's light table |

## ZGEOM (every scene object)

Seen: Hitman (ZHITMAN2) `08 6B 65 00 | name | node | 0x6ADD40 | extra`.

| Field | What |
|---|---|
| +0x00 | vtable |
| +0x04 | char* name |
| +0x08 | ZBaseGeom* node |
| +0x0C | ZClassInfo* |
| +0x10 | extra data (vfunc 0x44 makes it); +0x1C in it: the script list |

Virtuals used (ZGEOM's own numbering, bytes): 0x78 / 0x80 / 0x84 local /
world transform set / get, 0x44 extra data, 0x80 data load of a routine,
0xC0 hide, 0xE8 / 0xEC find a routine, 0xF0 add a routine, 0xF4 add a
runtime routine, 0x10C send to routines, 0x130 ReadData2, 0x148 Init, 0x14C
PostInit, 0x16C IsInside (boxes), 0x1A4 clone, 0x1A8 instantiate, 0x1B8
ReadData, 0x1D4 init phase, 0x270 add child. `h2/docs/classes.md` has each
class's vtable.

## ZClassInfo

Seen: ZHITMAN2's at 0x6ADD40, size 0x1263, id 0x2003E8.

| Field | What |
|---|---|
| +0x00 | char* name |
| +0x0C | base class |
| +0x10 / +0x14 | prev / next in the class list |
| +0x18 | object size |
| +0x23 | class id (unaligned) |

## ZBaseGeom (transform node, 0x70 bytes)

| Field | What |
|---|---|
| +0x00 | float mat[9]: rows forward, up, right (parent space) |
| +0x24 | float pos[3] (parent space, cm, +Y up) |
| +0x30 | bounding centre (node space) |
| +0x3C | flags: 0x400, 0x800 hidden (SetFlags 0x5BAC80; the game's hide 0x5BAEF0), 0x4000 auto-assign rooms, 0x40000 dynamic, 0x200000 held item, 0x800000 initialized |
| +0x40 | bounding box half sizes, +0x4C radius |
| +0x50 | parent node |
| +0x54 | its ZGEOM |
| +0x5C / +0x5E | first child / next sibling (1-based, static nodes) |
| +0x68 | mesh (the renderer's) |
| +0x6C | light table entry |

Hitman's node is a child placed at 0 (seen): his world place is his parent's.

## Characters

ZActor and its classes (ZTRACKLNKHM2 0x2003E9 the scene's NPCs, ZHITMAN2
0x2003E8 Hitman).

| Field | What |
|---|---|
| +0x564 | the look-at target (vfunc 0x610) |
| +0x574 | 0 with the death animation (vfunc 0x540) |
| +0x54B | its mover: position (+0x00), matrix (+0x0C), the last place (+0x38); set by the actor's SetWorldTransform (vfunc 0x80 = 0x463B10, which moves its group too) |
| +0x57C | float pos[3]: the actor's own point, a little above the node, following the mover (seen: 1511.5, 502.8, -449.9 with the node at y 500) |
| +0x67E | the room it is drawn with |
| +0x762 | the dragger's reference while dragged |
| Hitman +0x490 | his physics contacts (0x60F860, in the step 0x610640) |
| Hitman +0xD72 / +0xD76 | hit points / their maximum, floats (seen: 60 / 60) |
| Hitman +0xF90 | the id of the action he uses (handler 0x5142BC) |
| NPC +0x513 | rEnemyPathFinder3 (PostInit 0x462E80) |

## ZITEM

| Field | What |
|---|---|
| +0x64 | the owner's reference (SetOwner 0x4723F0) |
| +0xBB | the parts' references (0x474E70) |

## Routines (ZEventBase)

A geom's routines are objects of their own, found by name (vfunc 0xEC).

| Field | What |
|---|---|
| +0x38 | the data section (ReadData2 copies it); the routine's state machine is vfunc 0x50 (0x42ABB0: 1 init, 2 start) |
| flag 2 | added at runtime (vfunc 0xF4): the data load leaves it out, a loaded save makes it anew without data |

### ZGEOM_Action (ctor 0x487680, vtable 0x649770)

| Field | What |
|---|---|
| +0x72 | kind: 6 use (a message to the receiver), 7 place an item, 13 / 14 climb, 15 _Place, 16 _Pickup |
| +0x76 | message |
| +0x78 | receiver |
| +0x7C | range (vfunc 0xA4; Hitman's choice among near actions weighs it) |
| +0x80 | reach: a use action (kind 6) can be used while Hitman is within it of the object's box (vfunc 0x84) |
| +0x84 | flags (1 / 4: off; 2: out of reach) |
| +0x88 | on |
| +0x94 / +0x98 | the geoms told they are in range (MSG_ENTERITEMRANGE: Hitman's near list +0x5AD / +0x5B1) |
| +0xA0 / +0xA4 | the texts |

Set by vfunc 0xAC (0x488A10: texts, kind, message, receiver, range, reach, +0x130);
the scenes' data load (vfunc 0x80) gives range and reach the same value, the
runtime add 0x488A70 leaves the reach 0. Every frame vfunc 0x2C tells Hitman
(HitmanGround) when he comes within reach; Hitman picks among his near list's
actions (0x511C50) those his vfunc 0x638 allows (kind 6: the same room and
within the object's box widened by 90 / 40 / 90, about his chest). Use vfunc
0x8C, can-use 0x84.

### CheckInside (ctor 0x48A830, data load 0x4FC060)

| Field | What |
|---|---|
| +0x72 | the geom checked (0: Hitman) |
| +0x76 | receiver |
| +0x7A / +0x7C | enter / leave messages |
| +0x7E..+0x81 | flags (+0x80 with receiver 0: TrackLinkObjects) |
| +0x88 | inside now (update 0x48ACA0) |

The box (ZBoxPrimitive 0x2000E1) keeps its half sizes at +0x18.

### MusicSegment (vtable 0x64B858, data load 0x4A7F10)

| Field | What |
|---|---|
| +0x72 | mood |
| +0x7A | the track's sound reference (seen: 0x564 Exploration, 0x4A6 Chopin on C1-1) |

MusicControl (ctor 0x4A81B0): +0x1E4 makes the next check send the segments
again.

### ZMoveOnSpline (vehicles)

The mover at vehicle +0x5C (vtable 0x65C884): +0x04 place on the track
(0..1), +0x20 way (1 / -1), +0x4A target, +0x5B speed, +0x5F acceleration,
+0x63 top speed; the vehicle's +0xC4 top speed, +0xD0 / +0xD1 arrived flags.

## Other

| Object | Field | What |
|---|---|---|
| ZCAMERA | +0x44 | flags (1 orthographic, 0x8000 overlay) |
| ZCAMERA | +0x4C / +0x50 | window size, +0x54 focal, +0x58 fov (radians), +0x60 / +0x64 near / far |
| ZLIST_ActorCollision (vtable 0x649930) | +0x6E | its list; +0x72 / +0x76 per element arrays |
| a list | +0x18 | its reference container (`lists.h`) |
| ZPathfinder3 (0x200075) | +0x64 | the path network |
| BriefingMenu | +0x610 | the checked lines (bit n); +0x6E its window |
| the GUI mouse window | +0x11C / +0x120 | the cursor (GUI units) |
| a PRM model record | +0x30 collision, +0x34 query mask; ZSHAPE (0x40000): +0x84 knots, +0x88 their count |
