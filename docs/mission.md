# Routes, missions, music and the main menu (reverse engineering notes)

Addresses are hitman2.exe (Steam build).

## Lists (routes, safe points)

* ZLIST / ZConditionList (class 0x080000C8; plain ZLIST 0x0800001A) keep their
  elements in a reference container at **+0x18** (vtable 0x639510: chunked
  array `{prev, next, used dwords, ?, data...}`, `stride` dwords per element
  (ZLIST 1, ZConditionList 10), the reference first; vfunc 0x04 Add,
  0x0C Clear, 0x14 Count).
* Filled by ReadData (vfunc 0x1B8): ZLIST 0x4437C0 (array tag 9 -> Add for
  every reference that resolves), ZConditionList 0x46CCB0 (also a LONG, a
  nested condition block and its per-element arrays at +0x56 count, +0x5A
  entries {?, ?, ref, ?}, +0x5E pointers).
* Changing the elements: clear the container, read a data block with the new
  array (template: the record's block). Guards follow the new route at once.
* Data block of a patrol list: `89 <len> refs... 06 03 <LONG> 07 04000000 06 FF`.

## Characters

* Alive: actor vfunc **0x544** (0 when dead; script state then `Being2.Dead`).
* Instant death (as explosions do): `vfunc 0x120(actor, RegisterZMsg("ScriptEvent"), &"Msg_InstantDeath")`.
* Messages: `ZEngineDataBase::RegisterZMsg` 0x41EED0 (name, 0, file, line) -> id,
  `ZGEOM vfunc 0x120 (from, id, param*)` sends.
* Hitman: the record of class 0x2003E8. Level control: class 0x2000E1 named
  `LevelControl*` (with LevelControl.sdl).

## Mission objectives (the laptop)

* The briefing menu is a routine of the LaptopBriefing window
  (`ZWINDOW_BriefingMenu`). Lines 1..6 are LOC keys:
  * SetLocaleFolder 0x52D340 (this, folder): sets the lines whose
    `<folder>/Laptop/<n>` exists;
  * SetObjective 0x52E2E0 (this, slot, key) — thiscall, RET 8;
  * SetObjectiveCompleted 0x52E350 (this, {slot, done}): bit `slot` of
    +0x610, check mark `AllLevels/Symbols` Complete / Incomplete, info
    "Objective status changed".
  * Lines are laid out by the level's GUI file: a level shows only its own
    number of lines (C1-1: 4).
* LOC tree lookup 0x428FF0 (**stdcall** path, root) -> node `{flags, text}`
  (flag 1: text follows). The editor answers its own keys there.
* The completion bits are display only; the mission's logic is in the level
  routine (characters\level.cpp, per level c1_1.cpp ... with hard coded names,
  e.g. C2-1's target "KGBOfficer_01" at +0x271) and LevelControl.sdl.
* Level routine: messages registered by 0x504410 (CharacterKilled,
  MissionFailed, MissionCompleted, ...); handler 0x504960; **vfunc 0xC4**
  completes the mission (rating / statistics), 0xC0 fails it.

## Music

* MusicControl (ZGROUP_MusicControl routine, vtable 0x64B8E0, ctor 0x4A81B0)
  checks every 0x400 ms which of its zones Hitman is in (+0x1E3 inside flag,
  +0x1F1 last check) and starts that zone's music.
* Zones are ZBoxPrimitive objects (`Location_01`, `PreAction_01`, `Action_01`)
  with MusicSegment routines (type 0/1/2 at +0x72) and a sound reference
  (tag 0x0B) = offset of an entry's fields in the level's .SND table:
  `name\0 {1, name offset, ?, float}`.
* Every level's .SND / .WHD lists all music streams (Streams\Music\*.wav, 22
  tracks), played from streams.wav. WHD entry: `name\0 {6, name offset,
  0x80001000, 44100, 16, ?, Ogg length, 2 channels, data offset, PCM size, 4,
  1}`. The tracks are Ogg Vorbis with **byte-identical headers** (id, comment,
  code books).
* The game decodes with its own libogg / libvorbis loop (stream open around
  0x5EBE40: three header packets once, then only pages of that serial), so a
  mid-stream jump into another stream is silence. The editor serves the chosen
  track's audio pages rewritten as the continuation of the playing stream
  (its serial, continuing page numbers, growing granules, no EOS, new CRC);
  when the game starts the track from its beginning, the chosen one's headers
  come first.

## Main menu

* Scene `MainMenu`; buttons `MainMenu/Buttons/1Start, 2Load, 3Options,
  4Credits, 6Quit` 24 GUI units apart (the menu's init adds the GUI screen's
  margins to their positions). GUI screen: SysInterface +0x64 / +0x68
  (1067 x 600 at 16:9), renderer vfunc 0xB0 / 0xB4 report the same.
* The game's cursor: MouseWindow +0x11C / +0x120 (GUI units).
* Scene switch as the menus do: PopAllScenes 0x41B570, PushScene 0x41B460.
