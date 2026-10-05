# Models of other levels (reverse engineering notes)

Goal: place objects (and later characters) of any level in the loaded one.
Addresses: hitman2.exe (Steam), RenderD3D.dll (image base 0x10000000).
Implementation: `src/engine/imports.cpp` (loading), `src/editor/level.cpp`
(the level's list, the model library), `src/editor/models_panel.cpp` (UI).

## Where a level's models are

A level package (`Scenes\<L>\<L>__MAIN.ZIP`) holds GMS/BUF (objects), **PRM**
(geometry), **TEX** (textures), ANM (animations), **OCT** (bound trees,
collision), SGP, RMC/RMI (room trees), SND, LOC, ZGF (scripts), SUP. The
engine's file name extensions are at 0x682540 (rmc rmi oct buf prm gms tex
snd wav whd anm zgf sgp).

`ZEngineDataBase::AllocSequence` (0x41DA90) loads them in this order:

* TEX: `GetTextureData` into an engine buffer, then **renderer vfunc 0x74
  (buffer, size)** (the renderer: `SysInterface + 0x18A0`, RenderD3D vtable
  0x100421E0): 0x10015320 keeps the buffer (+0x2C), a texture manager reads it.
* PRM: a buffer of **size + 0x60000**, `GetPrimsData` (a plain file read),
  later **renderer vfunc 0x68 (buffer, size + 0x60000)** = 0x10001140 ->
  0x100152D0: renderer +0x20 = buffer, +0x28 = buffer + **header[1]** (where
  prims made at runtime are added), +0x24 = buffer end. The part from
  header[1] to +0x28 is written to save games (vfunc 0x3C).
* ANM after the sound data, then room and bound trees, the GMS last.
* `LoadBoundTrees` (0x41E7E0): OCT into engine +0xAE8 (size +0xAE4);
  `CreateBoundTrees` (0x41E6A0) gives each room its tree (room vfunc 0x310
  reads it from the stream), the entries after it to the collision manager
  (renderer +0x1C, vfunc 0x50 until -1) and the rest (vfunc 0x44 (data, size)).

## PRM

Header `{u32 list, u32 used (header[1]), u32 table, u32 count}`; `table` =
`count` u32 offsets of the **records**, ascending. All references are **file
offsets** (the renderer adds its buffer base), so a model of another level is
copied into this level's buffer with its offsets moved.

A model (GMS record +0x0C, node +0x68 = the offset of its first record) is a
chain of **0x84-byte records** linked by +0x08, one per material:

| off | |
|---|---|
| +0x00 | type (0 for plain static meshes) |
| +0x04 | **texture id** in the low 11 bits; above: sub-material index, 0x800 flag |
| +0x08 | next record of the model (offset) |
| +0x14 | u16 ?, u16 **vertex count** |
| +0x18 | offset: 0x3C bytes before the first record (markers, bounding box max / min at +0x24) |
| +0x1C | offset: **vertices**, 36 bytes each (position, normal, colour, uv) |
| +0x2C | offset (sometimes) |
| +0x30 | **collision**: OCT offset of the collision object + 1 (low bit: still an offset) |
| +0x34 | **collision mask**: what the triangles stop (queries test their mask: 0x20 characters walking; walls 0xFB) |
| +0x38 | 0xFF / 0x07 / 0xF8 |
| +0x40 | colour |
| +0x44 | offset: **indices** (after the vertices) |
| +0x48 | index count |

* Found by comparing the 1466 models C2-1 and C2-2 share at different
  offsets: the differences equal to the offset distance are the pointers
  (+0x08, +0x18, +0x1C, +0x44).
* A model's data (vertices, indices, bounds) mostly lies right before its
  records, but index strips (and some vertices) are shared between models
  and can be megabytes away: ~9% of the models span more than 4 MB. Sizes:
  vertices `count (u16 at +0x16) * 36`, strips `+0x48 * 2` (checked on every
  record of C2-1 and C6-1), bounds 0x3C. Imports copy these pieces (those
  closer than 4 KB together) instead of one span; `tools/check_imports.py`
  checks the rule on every level.
* The first part of every PRM (the AllLevels prims) is identical in all
  levels, at the same offsets.

## TEX

Header `{u32 table, u32 table2, ...}`; `table` = 2048 u32 offsets, index =
**texture id**. Entry: `u32 size (the whole entry), 'DXT1', 'DXT1', u32 id,
u16 w, u16 h, u32 mips, ..., name at +0x24` (e.g. `H2_C2-1_Wood_02a`). Ids are
per level and collide (C1-1 and C2-1 share 20 of ~550): an imported texture
gets a free id (about 600 of 2048 are used), or this level's texture of the
same name is used.

## Collision (OCT)

* `LoadBoundTrees` reads the OCT into engine +0xAE8; `CreateBoundTrees`
  gives each room its tree (room vfunc 0x310), then walks the **collision
  entries** `{u32 size, object, ..., +0x20 data}` with the collision
  manager's vfunc 0x50 (renderer +0x1C, vtable 0x100435B8) until a u32 -1;
  the rest is the **triangle pool** (8-byte items), vfunc 0x44 keeps it
  (+0x9E start, +0xA2 count).
* vfunc 0x50 only initialises the entry in place: object = entry + 4,
  vtable 0x100436F4, +0x14 = data pointer, +0x10 = [entry+0x30] (scale),
  +4..+0xC = [entry+0x24..] (origin, mesh space).
* A record's +0x30 = entry + 5; the engine takes `octBase + (v & ~1)`.
* The entry's octree: nodes at data + 0x14 (6 bytes: `u16 items << 3 |
  0x8000 last | octant`, `u16 first child`, `u16 first item`), items at
  `data + [data]` (16 bytes: `u32 pool offset`, six u16 quantised bounds).
  Traversal 0x10036000 (line), 0x10035D30 (box).
* Pool item: `{u16 count, u16 first index | 0x8000, u32 PRM record}`: the
  triangles are the record's own vertices (+0x1C) and indices (+0x44).
  Collectors (0x10021CE0 line, 0x100217A0 box) first test the query's mask
  against the record's +0x34.
* Node flag 0x8000 at runtime registers the object with the collision world
  (SetFlags 0x5BAC80 -> engine +0x5D1F vfunc 0x24, handle at node +0x6C).

Imported collision: the source level's whole OCT is added after this
level's; the used entries are initialised (vfunc 0x50), their items' pool
offsets moved to the copied pool (relative to this level's pool, found by
walking entries to the -1), the copied pool items moved to the copied PRM
records; records get mask | 0x68 (solid like a wall) and the model objects
node flags 0x8008. Checked: Hitman stops at C2-1's truck in C1-1.

## Importing (what the editor does)

While a scene loads (GetPrimsSize / Data, GetTextureSize / Data hooked):

1. The level's list (`imports` in level.json) is asked for that scene.
2. Each model is copied after the PRM's data and the 0x60000 bytes the engine
   keeps for prims made at runtime (from header[1], which stays: save games
   are the same with or without imports), its references moved.
3. Their textures are added to the TEX buffer (free ids, the entry's id field
   changed, the table entry set) and the records' texture ids changed.
4. After loading, `ROOT/EditorImports/<name>.<level>.<mesh>` objects (far
   below the world, in the source object's world rotation) show the models;
   the Models tab lists them with their level and places copies of them.

Checked: C2-1's truck and street lamp in C1-1 with their textures, placed,
saved, the game restarted, loaded again.

## Characters (skinned bodies)

An actor record's mesh (+0x0C) is a chain of **skinned records** (type
0x00010001, 0xA8 bytes: the 0x84 record and an extension shared by the
model's records):

| off | |
|---|---|
| +0x04, +0x08, +0x14, +0x18, +0x1C, +0x44, +0x48 | as in the plain record (texture, next, vertex count, bounds 0x3C, vertices, index strips, words) |
| +0x3C | first record: LOD table, 8 lists `{u32 count, record offsets}` |
| +0x40 | colour |
| +0x80 | ? (per record) |
| +0x84 | bones (0x45 for Reg_Male) |
| +0x88, +0x8C, +0x90, +0x94, +0x98 | per-bone blocks: 48, 28, 48, 36, 48 bytes a bone |
| +0x9C | skeleton: bone map and matrices (0xA14 bytes for Reg_Male), up to the next model's records; shared by several models |
| +0xA0, +0xA4 | size table: `+0xA0` pairs of u32 |

Vertices are 52 bytes. Index strips may be shared and lie megabytes away.
Found by comparing the 31 characters several levels have (e.g. SoldierRussian_01
in C2-1 / C2-2); the renderer reads +0x88.. through the prim manager (vfunc 0xC).

Importing a character (not run in the game yet): its body's blocks are copied
one by one with their sizes, the references moved by a map; the NPCs tab
offers it worn by a character of this map (NpcSpawn.mesh): that character's
data, size and behaviour, the outfits and animations of its own level.

## ANM (animations)

Chunks `{u32 tag, u32 size | flags}` (size with the header, low 30 bits).
Flag bit 31: a chunk with children: `+8` offset of its own data (from the
chunk), `+0xC` child count, children at `+0x10` (bit 30: after a u32 count and
that many u32s). Without flags the data is at `+8`. The file is one chunk
(tag `MNA\0`) whose children are:

| tag | |
|---|---|
| 0xA | an animation **set**, one each: data = its name (`Worlds\Characters\_Anims\Reg_Male\Reg_Male.anm`); one child (tag = animation count) with the offsets of its animations in chunk 5 (0x84-byte records: name, source file, frames, bones, rate...) |
| 0xE | `{u32 set index, u32 body mesh (PRM offset), u32 offset + 1 in chunk 0xF or 0}` |
| 0xF | per body data for a set: 28 bytes a bone (Reg_Male bodies, 0x78C each; Hitman and most special sets: none) |
| 7, 8, 6 | key frames (4 streams each, sizes in the header) |
| 0xB, 0xD | float tables |
| 4 | bone names (GROUND, PELVIS, SPINE, ...) |
| 5 | animations: track tables and the 0x84 records |

The buffer: `ZEngineDataBase+0x40`, allocated with exactly
`GetAnimsSize` (0x41C760) bytes and filled by `GetAnimsData` (0x41C7F0).
Pointers into it are saved in save games as offsets from it
(`ZSaveGame::AddSafePointer`, `CheckInAnimBuffer` 0x401510 uses the
allocation's size).

An actor (0x47AB10) reads the first two strings of its data block: its set
and an extra set (`""` when none, e.g. `Kbg_Officer.anm`, `Ninja.anm`,
cutscene sets), and loads each with the actor's vfunc 0x2D0 (0x47AD80) into
+0x5C / +0x60; animations are looked up by name in the extra set first
(vfunc 0x208, 0x47AD00). Loading is an animation set object (0x2C bytes,
vtable 0x65D848), vfunc 0x10 (0x5C8860) `(out, name, body mesh, ANM)`: it
keeps the data of chunks 4, 5, 6, 7, 8, 0xB, 0xD, 0xF of that ANM, finds the
set through chunk 0xE (set name and body mesh; `out` = the body's 0xF data,
to the actor's +0x64 / +0x68) or by name; false when the ANM has no such set.

Imports: the ANM of each level a character comes from is added after the
level's own (offsets of the level's part stay). The actor of an imported body
gets that character's two set names; vfunc 0x10 is hooked: a set the level
lacks is loaded from the body's level (with the body's mesh there); a set it
has gets the body's 0xF data from the body's level.

## Not yet

* Objects made of several models (a truck's wheels are models of their own).
* Characters checked in the game (bodies, outfits, animations).
