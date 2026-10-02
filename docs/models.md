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
| +0x30 | **collision**: offset in the level's OCT data |
| +0x34 | a second texture id (low 16 bits; LOD / glow textures) |
| +0x38 | 0xFF / 0x07 / 0xF8 |
| +0x40 | colour |
| +0x44 | offset: **indices** (after the vertices) |
| +0x48 | index count |

* Found by comparing the 1466 models C2-1 and C2-2 share at different
  offsets: the differences equal to the offset distance are the pointers
  (+0x08, +0x18, +0x1C, +0x44).
* A model's data (vertices, indices, bounds) lies right before its records:
  one span from the lowest pointer to the last record's end.
* The first part of every PRM (the AllLevels prims) is identical in all
  levels, at the same offsets.

## TEX

Header `{u32 table, u32 table2, ...}`; `table` = 2048 u32 offsets, index =
**texture id**. Entry: `u32 size (the whole entry), 'DXT1', 'DXT1', u32 id,
u16 w, u16 h, u32 mips, ..., name at +0x24` (e.g. `H2_C2-1_Wood_02a`). Ids are
per level and collide (C1-1 and C2-1 share 20 of ~550): an imported texture
gets a free id (about 600 of 2048 are used), or this level's texture of the
same name is used.

## Importing (what the editor does)

While a scene loads (GetPrimsSize / Data, GetTextureSize / Data hooked):

1. The level's list (`imports` in level.json) is asked for that scene.
2. Each model's span is copied after the PRM's data, pointers moved, +0x30
   cleared; header[1] is set after the copies (runtime prims go after them).
3. Their textures are added to the TEX buffer (free ids, the entry's id field
   changed, the table entry set) and the records' texture ids changed.
4. After loading, `ROOT/EditorImports/<name>.<level>.<mesh>` objects (far
   below the world, in the source object's world rotation) show the models;
   the Models tab lists them with their level and places copies of them.

Checked: C2-1's truck and street lamp in C1-1 with their textures, placed,
saved, the game restarted, loaded again.

## Not yet

* **Collision**: record +0x30 points into the OCT's collision data (packed
  trees, format unknown); copies have none.
* Objects made of several models (a truck's wheels are models of their own).
* Records of other types (+0x00 != 0), skinned meshes, characters (bodies,
  bones, ANM animations).
