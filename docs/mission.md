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
  * The window (AllLevels/LaptopBriefing) always has `Objectives/1..6`
    (ZWINGROUP with `Check` and `Text`) and `Objectives/SavesLeft`; a level
    hides the lines it does not use with node flag 0x400 and has SavesLeft
    where its next line is (C1-1: 4 lines, SavesLeft at line 5). The editor
    clears 0x400 (SetFlags 0x5BAC80) and unhides (vfunc 0xC0 = SetHidden,
    flag 0x800: 0 shows) the lines it uses, and moves SavesLeft below the
    last one (local y + one line height). The laptop opens with B.
  * The briefing (scroll area) is the texts of `<folder>/MissionBriefing/1a`,
    `2a`, ... while they exist (the format can be overridden by
    `<folder>/formatstring`), read in SetLocaleFolder. The editor answers 1a
    with the level document's `mission.briefing` (or h2.briefing's) and the
    level's further parts with "not there"; a change calls SetLocaleFolder
    again (it clears the check marks: they are set again).
* LOC tree lookup 0x428FF0 (**stdcall** path, root) -> node `{flags, text}`
  (flag 1: text follows). The editor answers its own keys there.
* The completion bits are display only; the mission's logic is in the level
  routine (characters\level.cpp, per level c1_1.cpp ... with hard coded names,
  e.g. C2-1's target "KGBOfficer_01" at +0x271) and LevelControl.sdl.
* Level routine: messages registered by 0x504410 (CharacterKilled,
  MissionFailed, MissionCompleted, ...); handler 0x504960; **vfunc 0xC4**
  completes the mission (rating / statistics), 0xC0 fails it.

## Places (zones)

* The game's levels watch places with boxes: ZBoxPrimitive (class 0x2000E1,
  0x44A300; its data: half sizes +0x18..+0x20, inside test vfunc 0x16C =
  0x44A430 on a point in its frame) with a **CheckInside** routine (record
  events `{count, {name offset, float}}` naming "ZGEOM_CheckInside"; ctor
  0x48A830, vtable 0x649A48). Its data (0x4FC060): checked geom +0x72 (0:
  HitmanGround), receiver +0x76, enter / leave messages +0x7A / +0x7C (scene
  message indices; "0", the list's first, is the game's none), flags
  +0x7E..+0x81. Every frame (0x48ACA0 -> 0x4FC480) it tests the checked geom
  and keeps whether it is inside at **+0x88**, sending enter / leave through
  the box's vfunc 0x11C (-> SendMessage) when that changes.
* A level's "reach" places and exit (its points) are such boxes made with the
  scene (engine/inject.h SceneZone: a copy of one of the scene's CheckInside
  boxes, half sizes = the radius, its own messages "0"); the goal is reached
  when the routine has Hitman inside. A place made or moved after the map
  loaded is checked by distance until the map loads again.

## Actions (taking an object)

* An object's actions are ZGEOM_Action routines (ctor 0x487680, vtable
  0x649770). Entry (vfunc 0xAC = 0x488A10): texts (copied: +0xA4, +0xA0),
  kind +0x72, message +0x76, receiver +0x78, distance +0x80; its data load
  (vfunc 0x80) reads nothing for one attached while playing (flag 2). A door
  attaches "Open door" while playing with 0x488A70 (fastcall: geom, text;
  then text, kind 2, message "dooropen", receiver 0, distance 400, 0). Saved
  with the game (texts as strings, the fields); its load sets the entry again.
* Used (vfunc 0x8C = 0x488860, the user's ref): kind 6 sends its message to
  its receiver (the user as param) and nothing else; kind 7 also sends
  PlaceItem2 to the user; other kinds are done by the user (character vfunc
  0x410). Usable (vfunc 0x84): kind 6 within the distance. The scenes' kinds:
  6 "use" (Open, PlaceBomb, PoisonWater, DeactivateLaser, OpenSafe,
  EscapeFerrari...), 13 / 14 climbing, 15 _Place, 16 _Pickup (items), 1.
* The game offers (HUD line, action key E) the actions of an object Hitman
  faces, among the objects the scene made: a copy made while playing gets no
  offer (seen 2026-10-18).
* A "Hitman takes it" goal of a model the scene made (not an item) gives it a
  kind-6 "Take" action (engine/messages.h AddUseAction, message
  "H2Editor_Take", the object its own receiver); the SendMessage hook sees the
  message reach it (game_events.h TakeUsed) and the model is taken. A copy
  the level made is taken with E near it.

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
* A zone's MusicSegment routine (vtable 0x64B858, ctor 0x4A7D10) reads its
  data at 0x4A7F10: mood +0x72 (0 calm, 1 tension, 2 combat), +0x76, the sound
  reference **+0x7A**, +0x7E, a flag +0x82, three floats +0x83..; saved and
  loaded with the game (0x4A80B0 / 0x4A8030). Its vfunc 0x84 (0x4A7FC0) copies
  that state for the MusicControl.
* MusicControl's data (0x4A8400): Hitman +0x72, its rooms +0x76, the level's
  LevelControl +0x122, its segments +0x126, message ids +0x1D2 / +0x1D4.
  Entering a room (0x4A8720) it sends every segment's state (0x4A8470) to the
  LevelControl (geom vfunc 0x118: message, state); the level's script plays
  them. **+0x1E4**: set by the ctor, cleared after a check: the next check
  takes the room as entered anew. A geom's routine by name: vfunc 0xEC
  ("MusicSegment", "MusicControl").
* The editor gives a zone another track as the game's load would: the
  segment's +0x7A, then MusicControl +0x1E4 (it sends the segments again).
  (Before 2026-10-17 it served the chosen track's Ogg pages in place of the
  original's reads of streams.wav.)

## Main menu

* Scene `MainMenu`; buttons `MainMenu/Buttons/1Start, 2Load, 3Options,
  4Credits, 6Quit` 24 GUI units apart (the menu's init adds the GUI screen's
  margins to their positions). GUI screen: SysInterface +0x64 / +0x68
  (1067 x 600 at 16:9), renderer vfunc 0xB0 / 0xB4 report the same.
* The game's cursor: MouseWindow +0x11C / +0x120 (GUI units).
* Scene switch as the menus do: PopAllScenes 0x41B570, PushScene 0x41B460.
* Buttons: CHM2Button (class 0x10042B, base ZBUTTON) with a ZCHAROBJ child
  `Normal` (mesh 0x11426) holding the LOC key of its text. Button data
  (ReadData 0x532A00 -> 0x4BCFE0 -> 0x4BD530): window values, two LONGs
  (+0x6C bits), LONG **id** (+0x68), four refs left / right / **up** (+0x80) /
  **down** (+0x84) for the arrow keys, then sound refs and two LONGs (300, 12).
  PostInit 0x4BD850 looks at the text children, so a button made at runtime
  is initialised after its text.
* The menu's handler of a pressed button: 0x54BF40 (thiscall, int id):
  1 Start, 2 Options, 3 Quit (confirm), 4 Load, 5 Credits; other ids do nothing.
* The editor's entry: the Options records made again under Load with id 7,
  neighbours Load / Options (theirs patched to it), text key
  `H2Editor/Menu/LevelEditor` (answered by the LOC hook); 0x54BF40 is hooked
  for id 7. Mouse focus, highlight and Enter work as for the game's buttons.
