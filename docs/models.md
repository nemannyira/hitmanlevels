# Models of other levels (reverse engineering notes, in progress)

Goal: place objects (and later characters) of any level in the loaded one.
Addresses: hitman2.exe (Steam), RenderD3D.dll (image base 0x10000000).

## Where a level's models are

A level package (`Scenes\<L>\<L>__MAIN.ZIP`) holds GMS/BUF (objects), **PRM**
(geometry), **TEX** (textures), ANM (animations), OCT/SGP/RMC/RMI (trees,
rooms), SND, LOC, ZGF (scripts), SUP.

`ZEngineDataBase::AllocSequence` (0x41DA90) loads them in this order:

* TEX: `GetTextureData` into an engine buffer, then **renderer vfunc 0x74
  (buffer, size)** (the renderer: `SysInterface + 0x18A0`, RenderD3D vtable
  0x100421E0).
* PRM: a buffer of **size + 0x60000**, `GetPrimsData` (a plain file read),
  later **renderer vfunc 0x68 (buffer, size + 0x60000)** = 0x10001140 ->
  0x100152D0: renderer +0x20 = buffer, +0x28 = buffer + header[1] (end of the
  file's prims: runtime prims are added after it), +0x24 = buffer end. The
  part from header[1] to +0x28 is written to save games (vfunc 0x3C), so the
  engine already appends prims at runtime.
* ANM after the sound data, the GMS last.

## PRM

Header `{u32 list, u32 table, u32 table, u32 count}`; `table` = `count` u32
offsets of **primitive descriptors**, ascending. All references inside the
file are **file offsets** (the renderer adds its buffer base), so a model
of another level has to be copied into this level's buffer with its offsets
moved.

Descriptor (0x40 bytes):

| off | |
|---|---|
| +0x00 | type / flags (0, 0x30000, 0x30084, 0x3C0C0, ...) |
| +0x04 | material / texture id (0x11..0xFC1; 0 for most) |
| +0x08 | offset of a linked descriptor or block (sometimes) |
| +0x14 | u16 ?, u16 **vertex count** |
| +0x18 | offset: bounding box (min / max) |
| +0x1C | offset: **vertices**, 36 bytes each (position, normal, colour, uv), then the indices |
| +0x28 | flags |
| +0x2C | offset of data after the descriptor (77 of 2173, e.g. bounds) |
| +0x38 | 0xFF |

* About 90% of the descriptors have their data between the previous
  descriptor and themselves (copyable as one span); the others share
  vertex data with earlier ones.
* The first part of every PRM (the AllLevels prims) is identical in all
  levels, at the same offsets.
* Objects' mesh references (GMS record +0x0C, node +0x68) are offsets of
  descriptors (or of a few larger objects after the table: lights, bounds).

## TEX

Header `{u32 table, u32 table2, ...}`; `table` = 2048 u32 offsets, index =
**texture id**. Entry: `u32 size, 'DXT1', 'DXT1', ..., u16 w, u16 h, ...,
name` (e.g. `H2_C2-1_Wood_02a`). Ids are per level and collide (C1-1 and
C2-1 share 20 of ~550): an imported texture needs a free id of this level
(about 600 of 2048 are used).

## Not yet known

* How a descriptor names its texture(s) (+0x04 is not a TEX id: up to 0xFC1),
  where the render materials are (the GMS `ZMaterialDesc` records, class
  0x2000F0, are sound materials: footsteps, shells).
* Skinned (character) meshes, their bones and the ANM data.
* How the renderer turns descriptors into its draw objects (0x10008CB0 reads
  vertex buffers of 36-byte vertices; its objects keep PRM offsets, e.g.
  +0x44 index list) and whether it can take descriptors added after loading.

## Plan

1. Static objects first: copy a model of another level (descriptor and its
   span, offsets moved) into the PRM buffer's free space before the renderer
   reads it (PRM size hook: a larger buffer), import its textures under free
   ids (TEX buffer before vfunc 0x74), make objects with that mesh.
2. A catalogue of every level's models (names from the GMS records using
   them) for the editor's Models tab.
3. Characters: skinned meshes, animations (ANM), the actor's data.
