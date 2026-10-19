# The scripts' commands

The levels' compiled scripts (`scripts\...\<Class>.ppo`, in the scene ZIPs)
call the game through **interfaces**: objects whose methods are numbered.
This lists every interface's methods with the game's function and the names
the scripts call them by (2026-10-18: the 421 scripts of the 22 main scenes,
the tables read from the running game). The functions are named in the
Ghidra project the same way (`Scene_If_HideObject`, `Actor_If_m4` for one no
script names).

## How a script calls one

* A .ppo's symbol table names everything it uses (`tools/ppo_methods.py`).
  After the strings comes the table of its interface slots: a u32 count,
  then {u32 slot, u16 symbol} per interface (e.g. C1-1's LevelControl.ppo:
  0x0C `Sound_If`, 0x10 `Scene_If`, 0x08 `Geom_If`, ...).
* A call is `97 <slot u8> 00 <method name symbol u16> <number u16>`: the
  slot says the interface, the number is the method's index in it (the name
  is only for the reader). One name has one number on its interface in
  every script (the spellings `GetSceneRef` / `getSceneRef` too).
* The game makes the interfaces by name (0x567F00; 0x568040 takes the same names, for their end) and
  its script engine (`g_pScriptDll` 0x6A6C74; vfunc 0xA4 registers the
  interfaces, vfunc 0x40 the 557 event names, 0x568850) calls the
  interface's dispatcher with a call context: +0x04 the arguments (8 bytes
  each: value, type), +0x10 the method number. A number past the
  interface's own count goes to its base, minus that count: TrackLnk_HM2_if
  (88) -> Actor_If (37) -> Link_If (12) -> Geom_If (46); Boid_If,
  GuardArbiter_if, MoveOnSpline_if -> Geom_If; Car_If -> Vehicle_If ->
  MoveOnSpline_if.
* Each table is filled at startup by its adder (`Registered by` below: up to
  50 entries, 90 for TrackLnk_HM2_if).

## Used by the editor, or worth using

| Editor / Lua | The game's command |
|---|---|
| hide / unhide (`docs/editor.md`, B15) | Scene_If HideObject (30) / UnhideObject (32): geom vfunc 0xC0 (hidden), as the editor calls it |
| `npc:walkto`, `lookat`, `moveset`, `animation`, `faceto` | Actor_If MoveToPosition (8), LookAt (26), UseMoveSet (7), PlayAnim (43) / PlayAnimLoop (46), Geom_If FaceObject (4): the editor makes the same calls on the actor (`docs/npc.md`) |
| the mission's end (`"mission"` event, failed) | LevelControl_if MissionFailed (5): LevelControl vfunc 0xC0 |
| reported deaths (`"kill"` / `"harm"`) | LevelControl_if CharacterKilled (0) / CharacterHarmed (1): LevelControl vfunc 0xB4 / 0xB8 |
| the items a dying character lets go (Being2's death) | TrackLnk_HM2_if SlipLeftHandItem (25) / SlipRightHandItem (24) |
| `h2.sound` / `h2.stopsound` | Sound_If FindWave (0), Play2DSound (2), Play3DSound (3), StopSound (5), SetVolume (13), SetLoop (16) |
| (not yet) texts | Dialog_If ShowText (10), ShowText3D (11), OneLiner (12), RemoveText (13) |
| `obj:active` | Scene_If MakeActive (34) / MakeInactive (35) |
| `npc:draggable`, `npc:noticed` | TrackLnk_HM2_if EnableDrag (39) / DisableDrag (40), MakeActorVisible (67) / MakeActorInvisible (68): Msg_ObjectAdd / Del to TrackLinkObjects, whose ZLIST_CheckVisible routine keeps what the characters look out for (+0x7E: 200 {ref, flags}); SetBodyUnconscious (72, actor +0x7A4 bit 8) not yet |

A behaviour's parameters (Inspector > NPC) are written into the script
instance's data by the class's symbol entries (`engine/npc.cpp`
SetScriptVariables): where the script engine's own parameter parse (vfunc
0x48, when the scene makes the character) puts them. No interface method
sets them.

## The interfaces

### LevelControl_if (CLevelControl_if)

Registered by 0x56B990, table 0x6B38B8, 45 methods (18 named by the scripts).

| # | function | name (uses in the scripts) |
|---|---|---|
| 0 | 0x56BE40 | CharacterKilled (66) |
| 1 | 0x56BEE0 | CharacterHarmed (44) |
| 2 | 0x56BF80 | IncHeadshots (22) |
| 3 | 0x56BFA0 |  |
| 4 | 0x56BFC0 |  |
| 5 | 0x56BFE0 | MissionFailed (1) |
| 6 | 0x56C000 |  |
| 7 | 0x56C020 | getSuitName (88), GetSuitName (1) |
| 8 | 0x56C110 | isDeadBody (111), IsDeadBody (1) |
| 9 | 0x56C250 |  |
| 10 | 0x56C490 | IsChangingClothes (1) |
| 11 | 0x56C5E0 | IsDraggingBody (1) |
| 12 | 0x56C780 | IsRunning (2), isRunning (1) |
| 13 | 0x56C930 |  |
| 14 | 0x56CAE0 | IsSneaking (2), isSneaking (1) |
| 15 | 0x56CE80 |  |
| 16 | 0x56CFB0 | IsKilling (1) |
| 17 | 0x56D0F0 | IsPunching (1) |
| 18 | 0x56D220 |  |
| 19 | 0x56D350 |  |
| 20 | 0x56D480 | IsPickingLock (1) |
| 21 | 0x56D5B0 |  |
| 22 | 0x56D670 | IsMapMode (1) |
| 23 | 0x56CC90 |  |
| 24 | 0x56D750 |  |
| 25 | 0x56D850 |  |
| 26 | 0x56D9D0 | SendCommand (4) |
| 27 | 0x56D920 | SendCommand_i (44) |
| 28 | 0x56D920 | SendCommand_r (6) |
| 29 | 0x56DA60 |  |
| 30 | 0x56DB20 |  |
| 31 | 0x56DB40 |  |
| 32 | 0x56DC00 |  |
| 33 | 0x56DD50 |  |
| 34 | 0x56DED0 |  |
| 35 | 0x56DFD0 |  |
| 36 | 0x56DF20 |  |
| 37 | 0x56E230 |  |
| 38 | 0x56E2E0 |  |
| 39 | 0x56E5D0 |  |
| 40 | 0x56E680 |  |
| 41 | 0x56E690 | MapActionStrings (1) |
| 42 | 0x56EC70 |  |
| 43 | 0x56ECD0 |  |
| 44 | 0x56ED70 |  |

### Scene_If (ZScriptSceneInterface_t)

Registered by 0x57F110, table 0x6B3DC0, 44 methods (18 named by the scripts).

| # | function | name (uses in the scripts) |
|---|---|---|
| 0 | 0x5813A0 |  |
| 1 | 0x5814E0 | GetSceneRef (100), getSceneRef (8) |
| 2 | 0x5814E0 | GetSceneInt (67) |
| 3 | 0x5815C0 |  |
| 4 | 0x5816B0 |  |
| 5 | 0x581790 | getSceneBool (6) |
| 6 | 0x581870 | SetSceneInt (88) |
| 7 | 0x581870 | SetSceneRef (3) |
| 8 | 0x581920 |  |
| 9 | 0x581A00 |  |
| 10 | 0x581AB0 | SetSceneBool (22), setSceneBool (12) |
| 11 | 0x581260 |  |
| 12 | 0x581190 |  |
| 13 | 0x581240 | (none; PopScene, Ghidra) |
| 14 | 0x57F570 |  |
| 15 | 0x580F70 | GetParent (22) |
| 16 | 0x581080 |  |
| 17 | 0x57FF80 | NameForObject (10), nameforobject (1) |
| 18 | 0x580050 | NameForParentObject (26) |
| 19 | 0x580140 | FullNameForObject (2) |
| 20 | 0x57F730 | FindGeom (6) |
| 21 | 0x57F800 |  |
| 22 | 0x57F930 |  |
| 23 | 0x57FB00 |  |
| 24 | 0x57FCB0 | copyObjectPosDir (10), CopyObjectPosDir (3) |
| 25 | 0x57FD70 |  |
| 26 | 0x5806A0 |  |
| 27 | 0x580850 | GetObjectRelPos (44) |
| 28 | 0x580A80 |  |
| 29 | 0x580DB0 |  |
| 30 | 0x580370 | HideObject (5) |
| 31 | 0x5803D0 |  |
| 32 | 0x580430 | UnhideObject (5), UnHideObject (1) |
| 33 | 0x580490 |  |
| 34 | 0x5804F0 | makeActive (26) |
| 35 | 0x580540 | makeInactive (40), makeInActive (11), MakeinActive (3), MakeInActive (2), MakeInactive (2) |
| 36 | 0x580590 |  |
| 37 | 0x581E70 |  |
| 38 | 0x581FF0 |  |
| 39 | 0x5820B0 |  |
| 40 | 0x582110 |  |
| 41 | 0x581B70 |  |
| 42 | 0x581BD0 | PlaySoundAtObject (1) |
| 43 | 0x582760 |  |

### Sound_If (ZScriptSoundInterface_t)

Registered by 0x582540, table 0x6B3E90, 19 methods (15 named by the scripts).

| # | function | name (uses in the scripts) |
|---|---|---|
| 0 | 0x5827F0 | FindWave (418) |
| 1 | 0x582920 | FindInSoundSet (49) |
| 2 | 0x582A70 | Play2DSound (22) |
| 3 | 0x582B20 | Play3dSound (2), Play3DSound (1) |
| 4 | 0x582CB0 | Play3DSound2 (29) |
| 5 | 0x582ED0 | StopSound (115) |
| 6 | 0x582760 | CastToIdx (49) |
| 7 | 0x582760 |  |
| 8 | 0x582760 | CastToIdx2 (68) |
| 9 | 0x582760 | CastToRef2 (90) |
| 10 | 0x582F30 |  |
| 11 | 0x582FF0 | IsPlaying (22) |
| 12 | 0x583120 |  |
| 13 | 0x583260 | SetVolume (155) |
| 14 | 0x5832F0 | SetMinDist (89) |
| 15 | 0x5833B0 | SetGroupMusic (22) |
| 16 | 0x583400 | SetLoop (66) |
| 17 | 0x583490 |  |
| 18 | 0x583540 | SetNotifyTarget (88) |

### Dialog_If (ZScriptDialogInterface_t)

Registered by 0x579190, table 0x6B3B00, 19 methods (16 named by the scripts).

| # | function | name (uses in the scripts) |
|---|---|---|
| 0 | 0x579480 |  |
| 1 | 0x5793D0 | SetActorA (23) |
| 2 | 0x579420 | SetActorB (23) |
| 3 | 0x579510 | SetDlgController (23) |
| 4 | 0x579560 |  |
| 5 | 0x5795B0 | RequestEnter (23) |
| 6 | 0x579940 | Enter (23) |
| 7 | 0x57A290 | EnterToggle (23) |
| 8 | 0x579E50 | Leave (69) |
| 9 | 0x57A360 | ToggleCamera (46) |
| 10 | 0x57A3A0 | ShowText (74) |
| 11 | 0x57A820 | ShowText3D (22) |
| 12 | 0x57A480 | OneLiner (22) |
| 13 | 0x57B5B0 | RemoveText (94) |
| 14 | 0x57B390 | ResourceExists (115) |
| 15 | 0x57B5C0 |  |
| 16 | 0x57A9A0 | IsShowingText (135) |
| 17 | 0x57B1A0 | ShowSoundText (22) |
| 18 | 0x57B6B0 | DisableMapAndDrag (23) |

### Engine_If (ZScriptEngineInterface_t)

Registered by 0x57BB90, table 0x6B3BD8, 19 methods (5 named by the scripts).

| # | function | name (uses in the scripts) |
|---|---|---|
| 0 | 0x57BDB0 |  |
| 1 | 0x57C070 | getTime (59), GetTime (3) |
| 2 | 0x57BF70 | random (124), Random (2) |
| 3 | 0x57BFF0 |  |
| 4 | 0x57C0E0 | GetGlobalInt (22) |
| 5 | 0x57C1C0 |  |
| 6 | 0x57C2A0 | Activate (1) |
| 7 | 0x57C390 |  |
| 8 | 0x57C4F0 |  |
| 9 | 0x57C440 |  |
| 10 | 0x57C580 |  |
| 11 | 0x582760 |  |
| 12 | 0x582760 | RefToInt (1) |
| 13 | 0x582760 |  |
| 14 | 0x582760 |  |
| 15 | 0x57BF30 |  |
| 16 | 0x4813C0 |  |
| 17 | 0x4813C0 |  |
| 18 | 0x57C650 |  |

### Item_If (ZScriptItemInterface_t)

Registered by 0x57CA40, table 0x6B3CA8, 21 methods (5 named by the scripts).

| # | function | name (uses in the scripts) |
|---|---|---|
| 0 | 0x57CE30 |  |
| 1 | 0x57D0E0 |  |
| 2 | 0x57D200 | GetRef (2) |
| 3 | 0x57D450 |  |
| 4 | 0x57D530 |  |
| 5 | 0x57D6E0 |  |
| 6 | 0x57D8F0 | GetOwner (6) |
| 7 | 0x57DA00 |  |
| 8 | 0x57DF50 |  |
| 9 | 0x57E270 |  |
| 10 | 0x57E0E0 |  |
| 11 | 0x57E430 |  |
| 12 | 0x57E590 |  |
| 13 | 0x57E890 | TransferItem (2) |
| 14 | 0x57EB10 |  |
| 15 | 0x57DD50 |  |
| 16 | 0x57DE50 |  |
| 17 | 0x57CC90 |  |
| 18 | 0x57CD20 |  |
| 19 | 0x57EBC0 | EnablePickup (5) |
| 20 | 0x57EC60 | PlayItemSound (3) |

### GUI_if (CGUI_if)

Registered by 0x5664A0, table 0x6B37E0, 6 methods (4 named by the scripts).

| # | function | name (uses in the scripts) |
|---|---|---|
| 0 | 0x5665E0 | DisplayWarning (2) |
| 1 | 0x566660 | DisplayWarningTimed (1) |
| 2 | 0x566770 | DisplayInfo (6), displayInfo (2) |
| 3 | 0x5667F0 | FrameEnd (1), displayHint (1) |
| 4 | 0x566870 |  |
| 5 | 0x5666E0 |  |

### Geom_If (ZGeomIf)

Registered by 0x5892B0, table 0x6B42B0, 46 methods (19 named by the scripts).

| # | function | name (uses in the scripts) |
|---|---|---|
| 0 | 0x58A170 | getPosition (110) |
| 1 | 0x58A2C0 |  |
| 2 | 0x58A8D0 |  |
| 3 | 0x58A9D0 |  |
| 4 | 0x58AB10 | FaceObject (22) |
| 5 | 0x58A390 | IsInsideList (2) |
| 6 | 0x58BEC0 | CheckPointInsideBox (1) |
| 7 | 0x58C090 |  |
| 8 | 0x58BBD0 | GetDistanceToObject (22), getDistanceToObject (14) |
| 9 | 0x58BD60 |  |
| 10 | 0x58AE10 |  |
| 11 | 0x58AEC0 |  |
| 12 | 0x58A730 |  |
| 13 | 0x58A610 |  |
| 14 | 0x58A6A0 | rotateZ (3) |
| 15 | 0x58A7C0 |  |
| 16 | 0x58B030 | GetSceneRef (65), getSceneRef (16) |
| 17 | 0x58B0B0 |  |
| 18 | 0x58B0C0 |  |
| 19 | 0x58BEA0 |  |
| 20 | 0x58BEB0 |  |
| 21 | 0x58B0D0 | SendCommand (28), sendCommand (22) |
| 22 | 0x58B190 | SendCommand_i (79) |
| 23 | 0x58B190 | SendCommand_r (32), sendCommand_r (3) |
| 24 | 0x58B280 | SendCommand_ii (66) |
| 25 | 0x58B280 | SendCommand_rr (1) |
| 26 | 0x58B280 |  |
| 27 | 0x58B3B0 | SendCommand_rf (12) |
| 28 | 0x58B4E0 |  |
| 29 | 0x58B870 |  |
| 30 | 0x58B960 |  |
| 31 | 0x58BAB0 | SendCommand_s (32) |
| 32 | 0x58B630 | SendCommand_is (44) |
| 33 | 0x58B780 | sendCommandRecursive_i (5), SendCommandRecursive_i (4) |
| 34 | 0x589740 | sendScriptCommand (20), SendScriptCommand (3) |
| 35 | 0x589820 | sendScriptCommand_i (4) |
| 36 | 0x589820 | SendScriptCommand_r (8), sendScriptCommand_r (2) |
| 37 | 0x589920 |  |
| 38 | 0x589A00 |  |
| 39 | 0x589A00 |  |
| 40 | 0x589A00 |  |
| 41 | 0x589B30 |  |
| 42 | 0x589C90 |  |
| 43 | 0x589E20 |  |
| 44 | 0x589FE0 |  |
| 45 | 0x58AC30 |  |

### Link_If (ZLinkIf)

Registered by 0x5878E0, table 0x6B41D0, 12 methods (7 named by the scripts), then its base Geom_If.

| # | function | name (uses in the scripts) |
|---|---|---|
| 0 | 0x588EC0 |  |
| 1 | 0x5887F0 |  |
| 2 | 0x588E80 | PlaySpeech (22) |
| 3 | 0x588EA0 |  |
| 4 | 0x5887F0 | StopAllAnimsSound (22) |
| 5 | 0x587A70 | getAnimHandle (924) |
| 6 | 0x588E90 | PlayAnim (154) |
| 7 | 0x588E60 | PlayAnimSegment (66) |
| 8 | 0x588E70 | CanPlayAnim (88) |
| 9 | 0x588EB0 | PlayAnimLoop (22) |
| 10 | 0x587F40 |  |
| 11 | 0x5884D0 |  |

### Actor_If (ZActorIf)

Registered by 0x5839E0, table 0x6B3F60, 37 methods (13 named by the scripts), then its base Link_If.

| # | function | name (uses in the scripts) |
|---|---|---|
| 0 | 0x5841F0 |  |
| 1 | 0x584260 |  |
| 2 | 0x585000 | Die (66) |
| 3 | 0x5850B0 | DieByForce (22) |
| 4 | 0x585260 |  |
| 5 | 0x4813C0 |  |
| 6 | 0x583DB0 |  |
| 7 | 0x583F70 | UseMoveSet (44) |
| 8 | 0x5842C0 | MoveToPosition (176) |
| 9 | 0x5843B0 |  |
| 10 | 0x584530 |  |
| 11 | 0x584690 |  |
| 12 | 0x5849A0 | MoveToLocation (88) |
| 13 | 0x584A70 | MoveToLocationForced (88) |
| 14 | 0x584BD0 |  |
| 15 | 0x584C90 |  |
| 16 | 0x584CE0 |  |
| 17 | 0x584D30 |  |
| 18 | 0x584D80 |  |
| 19 | 0x584E00 | Stop (88) |
| 20 | 0x584E70 | shootIntoGround (66) |
| 21 | 0x584F50 |  |
| 22 | 0x584EE0 |  |
| 23 | 0x584FB0 |  |
| 24 | 0x585340 |  |
| 25 | 0x585360 | DisableMovePool (44) |
| 26 | 0x584E20 | LookAt (22) |
| 27 | 0x5856E0 | GetActorSoundSet (22) |
| 28 | 0x585750 | GetActorVoicePitch (22) |
| 29 | 0x5857C0 |  |
| 30 | 0x583FF0 |  |
| 31 | 0x584090 | SetVisRangeSpeed (22) |
| 32 | 0x584140 |  |
| 33 | 0x585380 |  |
| 34 | 0x585520 |  |
| 35 | 0x585010 |  |
| 36 | 0x585270 |  |

### TrackLnk_HM2_if (ZTrackLnkHM2If)

Registered by 0x56F260, table 0x6B3990, 88 methods (16 named by the scripts), then its base Actor_If.

| # | function | name (uses in the scripts) |
|---|---|---|
| 0 | 0x570200 |  |
| 1 | 0x56FED0 |  |
| 2 | 0x570160 |  |
| 3 | 0x570190 |  |
| 4 | 0x570390 |  |
| 5 | 0x570270 |  |
| 6 | 0x570570 |  |
| 7 | 0x571740 |  |
| 8 | 0x570510 |  |
| 9 | 0x570550 |  |
| 10 | 0x5705F0 |  |
| 11 | 0x570930 |  |
| 12 | 0x570A00 |  |
| 13 | 0x5713E0 |  |
| 14 | 0x571750 |  |
| 15 | 0x573260 |  |
| 16 | 0x571990 |  |
| 17 | 0x573320 |  |
| 18 | 0x573620 |  |
| 19 | 0x573740 |  |
| 20 | 0x573860 |  |
| 21 | 0x5738C0 |  |
| 22 | 0x573A10 |  |
| 23 | 0x573980 |  |
| 24 | 0x573AC0 | SlipRightHandItem (66) |
| 25 | 0x573E10 | SlipLeftHandItem (66) |
| 26 | 0x574160 |  |
| 27 | 0x574700 | broadCastHelpRequestRanged (44) |
| 28 | 0x574CB0 |  |
| 29 | 0x520520 |  |
| 30 | 0x574F00 |  |
| 31 | 0x574F70 |  |
| 32 | 0x5750B0 | EnablePickupClothes (66) |
| 33 | 0x5750C0 | DisablePickupClothes (44) |
| 34 | 0x5750D0 |  |
| 35 | 0x576FC0 |  |
| 36 | 0x5751D0 |  |
| 37 | 0x575410 | ActivateRagdoll (22) |
| 38 | 0x575470 | DeactivateRagdoll (44) |
| 39 | 0x575580 | EnableDrag (66) |
| 40 | 0x5755A0 | DisableDrag (44) |
| 41 | 0x575640 | ShootIntoGroundForced (22) |
| 42 | 0x575690 |  |
| 43 | 0x575800 |  |
| 44 | 0x575A10 |  |
| 45 | 0x575CC0 | isOnBack (22) |
| 46 | 0x577920 |  |
| 47 | 0x575DD0 |  |
| 48 | 0x575EC0 |  |
| 49 | 0x576A30 |  |
| 50 | 0x576B90 |  |
| 51 | 0x575ED0 |  |
| 52 | 0x575FF0 |  |
| 53 | 0x576100 |  |
| 54 | 0x5762C0 |  |
| 55 | 0x5764B0 |  |
| 56 | 0x576570 |  |
| 57 | 0x5765D0 |  |
| 58 | 0x576640 |  |
| 59 | 0x576750 |  |
| 60 | 0x576850 |  |
| 61 | 0x576910 |  |
| 62 | 0x576970 |  |
| 63 | 0x56FE60 | Breathe (22) |
| 64 | 0x576D40 |  |
| 65 | 0x576E50 | lockUbAnim (110) |
| 66 | 0x576DB0 |  |
| 67 | 0x576EC0 | MakeActorVisible (22) |
| 68 | 0x576F40 | MakeActorInvisible (44) |
| 69 | 0x577030 |  |
| 70 | 0x577050 |  |
| 71 | 0x56FAD0 |  |
| 72 | 0x56FC90 | SetBodyUnconscious (44) |
| 73 | 0x56FD10 |  |
| 74 | 0x570F10 |  |
| 75 | 0x570CB0 |  |
| 76 | 0x570A30 |  |
| 77 | 0x570AA0 |  |
| 78 | 0x570B50 |  |
| 79 | 0x570C00 |  |
| 80 | 0x577070 |  |
| 81 | 0x5772A0 |  |
| 82 | 0x5772D0 |  |
| 83 | 0x577360 |  |
| 84 | 0x5755D0 |  |
| 85 | 0x5774E0 |  |
| 86 | 0x5775E0 |  |
| 87 | 0x577650 |  |

### GuardArbiter_if (CGuardArbiter_if)

Registered by 0x564EF0, table 0x6B3710, 19 methods (0 named by the scripts), then its base Geom_If.

| # | function | name (uses in the scripts) |
|---|---|---|
| 0 | 0x565120 |  |
| 1 | 0x565130 |  |
| 2 | 0x565220 |  |
| 3 | 0x565600 |  |
| 4 | 0x565340 |  |
| 5 | 0x565600 |  |
| 6 | 0x565660 |  |
| 7 | 0x565830 |  |
| 8 | 0x565FA0 |  |
| 9 | 0x5658C0 |  |
| 10 | 0x565EC0 |  |
| 11 | 0x565A50 |  |
| 12 | 0x565B30 |  |
| 13 | 0x565C60 |  |
| 14 | 0x565DE0 |  |
| 15 | 0x565E50 |  |
| 16 | 0x565540 |  |
| 17 | 0x565430 |  |
| 18 | 0x565520 |  |

### Boid_If (ZBoidIf)

Registered by 0x562FC0, table 0x6B3640, 31 methods (24 named by the scripts), then its base Geom_If.

| # | function | name (uses in the scripts) |
|---|---|---|
| 0 | 0x5633A0 |  |
| 1 | 0x563400 | setMinMaxForce (29) |
| 2 | 0x5634B0 | setMinMaxSpeed (47) |
| 3 | 0x563560 |  |
| 4 | 0x5636E0 | setForceConstraint (6), SetForceConstraint (4) |
| 5 | 0x5637E0 |  |
| 6 | 0x563A30 | setVisionRangeAndAngle (3) |
| 7 | 0x5635D0 |  |
| 8 | 0x5638F0 | ApplyConstantForce (12), applyConstantForce (3) |
| 9 | 0x563B00 | enableBanking (16) |
| 10 | 0x563B70 | updateLocalSpace (18), UpdateLocalSpace (5) |
| 11 | 0x563BE0 | shootIntoGround (12) |
| 12 | 0x563BF0 |  |
| 13 | 0x563380 | DisableBoid (7) |
| 14 | 0x563390 | IgnoreBoid (4) |
| 15 | 0x563310 | GetBoidSoundSet (2) |
| 16 | 0x5643D0 | GetAnimHandle (39) |
| 17 | 0x564500 | PlayAnim (10), playAnim (8) |
| 18 | 0x5645C0 | playAnimLoop (39) |
| 19 | 0x564680 |  |
| 20 | 0x563C60 | disableAllBehaviors (23), disableallbehaviors (4) |
| 21 | 0x563C70 | enableFleeBehavior (6), EnableFleeBehavior (3) |
| 22 | 0x563D50 | enableWander2DBehavior (16), EnableWander2DBehavior (2) |
| 23 | 0x563EF0 | enableSeparationBehavior (21) |
| 24 | 0x563FA0 | enableCohesionBehavior (14) |
| 25 | 0x564000 | enableAllignmentBehavior (6) |
| 26 | 0x564060 | enableFallBehavior (6) |
| 27 | 0x5641B0 | EnableStayInBoxBehavior (13) |
| 28 | 0x564240 | EnableFollowWaypointsBehavior (6) |
| 29 | 0x564920 |  |
| 30 | 0x564990 | PlayBoidSound (10) |

### MoveOnSpline_if (ZMoveOnSplineIf)

Registered by 0x58D000, table 0x6B4478, 25 methods (9 named by the scripts), then its base Geom_If.

| # | function | name (uses in the scripts) |
|---|---|---|
| 0 | 0x58D2C0 | SetMoveParams (1) |
| 1 | 0x58D3A0 |  |
| 2 | 0x58D3F0 |  |
| 3 | 0x58D500 | MoveToRef (12) |
| 4 | 0x58D5F0 |  |
| 5 | 0x58D660 |  |
| 6 | 0x58D6D0 |  |
| 7 | 0x58D760 | Stop (12) |
| 8 | 0x58D7F0 | MoveToSplineVal (12) |
| 9 | 0x58D870 | SetPath (1) |
| 10 | 0x58D8F0 |  |
| 11 | 0x58D990 |  |
| 12 | 0x58D9E0 |  |
| 13 | 0x58DA50 |  |
| 14 | 0x58DAF0 |  |
| 15 | 0x58DB40 |  |
| 16 | 0x58DBB0 |  |
| 17 | 0x58DC20 |  |
| 18 | 0x58DC90 | SetMoveSoundParams (1) |
| 19 | 0x58DD10 | SetRollingSoundParams (1) |
| 20 | 0x58DD90 | StartMoveSound (2) |
| 21 | 0x58DE00 |  |
| 22 | 0x58DE70 | StopMoveSound (1) |
| 23 | 0x58DE80 |  |
| 24 | 0x58DE90 |  |

### Vehicle_If (ZVehicleIf)

Registered by 0x58E3E0, table 0x6B4548, 4 methods (1 named by the scripts), then its base MoveOnSpline_if.

| # | function | name (uses in the scripts) |
|---|---|---|
| 0 | 0x58E4D0 | DriveFwdToSplineVal (1) |
| 1 | 0x58E550 |  |
| 2 | 0x58E5D0 |  |
| 3 | 0x58E630 |  |

### Car_If (ZCarIf)

Registered by 0x58C6B0, table 0x6B43A8, 10 methods (8 named by the scripts), then its base Vehicle_If.

| # | function | name (uses in the scripts) |
|---|---|---|
| 0 | 0x58C820 | InitCarPosDir (1) |
| 1 | 0x58C870 |  |
| 2 | 0x58C8F0 | SoundEngineStart (2) |
| 3 | 0x58C960 | SoundEngineStop (1) |
| 4 | 0x58C9D0 | SoundSpeedUp (6) |
| 5 | 0x58CA40 | SoundAirBreak (5) |
| 6 | 0x58CAB0 | SoundGearShift (1) |
| 7 | 0x58CB20 |  |
| 8 | 0x58CB30 | SetSkidPitch (1) |
| 9 | 0x58CB80 | SetSoundScramble (1) |

### ConditionList_If (ZConditionListIf)

Registered by 0x585C50, table 0x6B4030, 18 methods (0 named by the scripts).

| # | function | name (uses in the scripts) |
|---|---|---|
| 0 | 0x585E50 |  |
| 1 | 0x585F70 |  |
| 2 | 0x585FC0 |  |
| 3 | 0x586070 |  |
| 4 | 0x586120 |  |
| 5 | 0x5861D0 |  |
| 6 | 0x586280 |  |
| 7 | 0x586370 |  |
| 8 | 0x5863C0 |  |
| 9 | 0x586440 |  |
| 10 | 0x586540 |  |
| 11 | 0x586640 |  |
| 12 | 0x586720 |  |
| 13 | 0x586800 |  |
| 14 | 0x5868D0 |  |
| 15 | 0x586A40 |  |
| 16 | 0x587250 |  |
| 17 | 0x4813C0 |  |

### List_If (ZListIf)

Registered by 0x586E90, table 0x6B4100, 0 methods (0 named by the scripts).

