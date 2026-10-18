# The game's messages

Objects talk to each other with messages (SendMessage 0x43CBE0: receiver,
message id, sender, param); a geom passes them to its routines (vfunc 0x10C).
The names are registered at load in the scene's engine (engine +0xC37 +
id*4, GetZMsgName 0x41EEB0; engine +0x1C43 the last id). Lua sees every one
as the `"message"` event and one by name as `"msg:<name>"` (`docs/lua.md`);
`h2.messages()` lists the names.

## How often (C1-1, 30 seconds, Hitman standing)

`msg_stresssetval` 1817, `lnkanimend` 232, `msg_sound_door` 52,
`dooropen` 8, `pathopendoor` 8, `msg_animsoundstart` 8, `pathrequest` 5,
`pathfinished` 5, `scriptevent` 1. A `"message"` handler sees about 70 a
second: it must be cheap, or filter with `"msg:<name>"`.

## Used by the editor

Lua events made of messages (`kAliases`, `src/script/events.cpp`). Some of
them are the scripts' own events, not engine messages (not in the id list
below): the scripts deliver them to each other by name (0x5E13A0, ScriptEvent
unpacked: `hitmanspotted`, `thread_*`, the alarm's); the editor keeps them as
messages without a sender.

| Messages | Lua event |
|---|---|
| `thread_examinehitman`, `thread_actingstrange` | `"suspicious"` |
| `thread_recognized` | `"recognized"` |
| `thread_angry` | `"hostile"` |
| `hitmanspotted`, `hitmandetected`, `reportnewtarget` | `"spotted"` |
| `helprequest` | `"help"` |
| `enteralarmstate`, `alarmraised`, `leavealarmstate`, `alarmend` | `"alarm"` |
| `characterunconscious` | `"unconscious"` |
| `levelcontrolnotifychangeclothes` | `"disguise"` |
| `levelcontrolnotifypickupitem`, `levelcontrolnotifyuseitem`, `msg_dropitem` | `"pickup"`, `"useitem"`, `"drop"` |
| `msg_damage`, `projectilehit`, `msg_explode` | `"damage"`, `"hit"`, `"explosion"` |
| `msg_sound_shot`, `msg_sound_silentshot` | `"shot"` |
| `dooropen`, `doorclose` | `"door"` |
| `briefingmenuopened`, `ingamemapopened`, `ingamemenuopened`, `ingameinventoryopened` | `"laptop"`, `"map"`, `"menu"`, `"inventory"` |
| `cutsequence_start`, `cutsequence_end` | `"cutscene"` |
| `restartlevel` | `"restart"` |

Sent by the editor: `msg_displayhint` / `msg_displayinfo` /
`msg_displaywarning` (mission texts), `msg_sethitpoints` (Hitman's health
on screen), `msg_instantdeath` (`npc:kill`); `obj:event(name)` sends any
script event (`hitman_dead`, `msg_changemusictoactionmode`, ...). The zones
(`checkinsideenter` / `checkinsideleave`) are read from the routine's +0x88
instead.

## Candidates for Lua events

Messages the game has whose events would be useful for level scripts (none
made yet; each would be a name in the `"message"` event's table first):

| Messages | A possible event |
|---|---|
| `checkinsideenter`, `checkinsideleave` | `"zone"` `{zone, inside}`: Hitman entering / leaving a level's zone, as the game's own CheckInside routine reports it, instead of polling |
| `msg_picklockstart`, `msg_picklockstop`, `msg_picklockdoorunlocked`, `msg_lockdoor`, `msg_unlockdoor` | `"lock"` `{door, locked}` |
| `msg_choicedisplay`, `msg_choicemade`, `msg_choicecancel` | `"choice"`: the game's dialog choices |
| `pickup_startoffer`, `pickup_accept`, `pickup_deny` | `"offer"`: an item offered by a character |
| `elevmoveto`, `elevstop` | `"elevator"` |
| `msg_buttonpressed`, `pushbutton` | `"button"` |
| `nearnotifynear`, `nearnotifyfar`, `characterenterrange`, `characterleaverange` | `"near"` |
| `musicenter`, `musicleave`, `musicactionmode`, `musiclocationmode` | `"music"` `{mood}` |
| `targetinsidelof`, `targetoutsidelof` | `"aim"` (the sniper's line of fire) |

## The names by id (C1-1)

The ids are the scene's (registered while it loads): the last ones come from
its own scripts (`msg_sound_door`, ...) and may differ on another map. Id 4's
name is "0" (a scene's message list starts with it); 387 has none.

| id | name | id | name | id | name | id | name |
|---|---|---|---|---|---|---|---|
| 1 | `msg_soundnotifystopped` | 2 | `msg_soundevent` | 3 | `scriptevent` | 4 | `0` |
| 5 | `actionhide` | 6 | `actionshow` | 7 | `activate` | 8 | `activateobject` |
| 9 | `camfollowgeom` | 10 | `camera_add` | 11 | `camera_position` | 12 | `camera_remove` |
| 13 | `camera_requestlist` | 14 | `camera_requestquality` | 15 | `camera_sendlist` | 16 | `camera_sendquality` |
| 17 | `camera_update` | 18 | `checkinsideenter` | 19 | `checkinsideleave` | 20 | `createinstance` |
| 21 | `cutsequence_start` | 22 | `destroyinstance` | 23 | `endstate` | 24 | `enterview` |
| 25 | `falsenow` | 26 | `ikbomb_explode` | 27 | `leaveview` | 28 | `lort` |
| 29 | `msg_soundtrigger` | 30 | `msg_subtitlescleared` | 31 | `msg_subtitleserase` | 32 | `msg_subtitleswrite` |
| 33 | `matposanimend` | 34 | `projectilehit` | 35 | `sideright` | 36 | `sidewrong` |
| 37 | `startscroll` | 38 | `startstate` | 39 | `stopscroll` | 40 | `truenow` |
| 41 | `vertanimend` | 42 | `commands` | 43 | `d_nativedraw` | 44 | `ip_debug` |
| 45 | `ip_timemultiplier` | 46 | `ip_timers` | 47 | `msginvisible` | 48 | `msgvisible` |
| 49 | `msg_xcrossedless` | 50 | `msg_xcrossedmore` | 51 | `r_fog` | 52 | `activatesound` |
| 53 | `deactivatesound` | 54 | `framenotify` | 55 | `msg_soundsegmentstart` | 56 | `msg_soundsegmentstop` |
| 57 | `msg_changemusictoactionmode` | 58 | `msg_changemusictolocationmode` | 59 | `msg_changemusictopreactionmode` | 60 | `musicactionmode` |
| 61 | `musicenter` | 62 | `musicleave` | 63 | `musiclocationmode` | 64 | `musicpreactionmode` |
| 65 | `explodebomb` | 66 | `fastcollide` | 67 | `msg_writesubtitle` | 68 | `oldcode` |
| 69 | `rigidbodyenable` | 70 | `setmaterial` | 71 | `setposition` | 72 | `setsndinfo` |
| 73 | `setvelocity` | 74 | `subtitlesclear` | 75 | `actionchange` | 76 | `actionremove` |
| 77 | `action_getactionlist` | 78 | `blinkinglightsoff` | 79 | `blinkinglightson` | 80 | `canoperateobject` |
| 81 | `characterenterrange` | 82 | `characterleaverange` | 83 | `cutsequence_end` | 84 | `disableaction` |
| 85 | `doorclose` | 86 | `doorforceclose` | 87 | `dooropen` | 88 | `doorprotect` |
| 89 | `doorstatus` | 90 | `doorunprotect` | 91 | `elevmoveto` | 92 | `elevstop` |
| 93 | `enableaction` | 94 | `getnearobjects` | 95 | `msg_activateexplosive` | 96 | `msg_animsoundstart` |
| 97 | `msg_enteritemrange` | 98 | `msg_getinventorylist` | 99 | `msg_getinventorylist2` | 100 | `msg_leaveitemrange` |
| 101 | `msg_lnkobjoutofcutseq` | 102 | `msg_lockdoor` | 103 | `msg_peekholerequest` | 104 | `msg_picklockdoorunlocked` |
| 105 | `msg_picklockprogessreset` | 106 | `msg_picklockrequest` | 107 | `msg_picklockstart` | 108 | `msg_picklockstop` |
| 109 | `msg_requestgraying` | 110 | `msg_requestkeyholepos` | 111 | `msg_setlockability` | 112 | `msg_unlockdoor` |
| 113 | `msg_buttonpressed` | 114 | `objectvisibleadd` | 115 | `objectvisibleremove` | 116 | `removenamedaction` |
| 117 | `msg_openboxes` | 118 | `ambientbehaviorgreet` | 119 | `ambientbehaviormeet` | 120 | `bombforceactivate` |
| 121 | `canpenetrate` | 122 | `hitobject` | 123 | `msg_activatepianowire` | 124 | `msg_contextpointer` |
| 125 | `msg_damage` | 126 | `msg_deactivatepianowire` | 127 | `msg_explode` | 128 | `msg_explosivesetdamage` |
| 129 | `msg_explosiveseteffecttime` | 130 | `msg_explosivesetrange` | 131 | `msg_explosivestopeffect` | 132 | `msg_gettarget` |
| 133 | `msg_itemgetanimname` | 134 | `msg_itemsetstate` | 135 | `msg_itemstatecreate` | 136 | `msg_updatecrosshair` |
| 137 | `msg_beacon` | 138 | `msg_instantdeath` | 139 | `msg_itemdroppedinchute` | 140 | `setammotemplate` |
| 141 | `shootattarget` | 142 | `snipermaincamera_setorgmatrix` | 143 | `snipermaincamera_setorgposition` | 144 | `snipermaincamera_setoverlay` |
| 145 | `snipermaincamera_setquality` | 146 | `targetinsidelof` | 147 | `targetoutsidelof` | 148 | `actionoff` |
| 149 | `actionon` | 150 | `closedoor` | 151 | `closedoor2` | 152 | `comrequest` |
| 153 | `cutsequencerequestdata` | 154 | `cutsequencesenddata` | 155 | `fireshot` | 156 | `hero_enter` |
| 157 | `hero_leave` | 158 | `inventory_add` | 159 | `lnkanimend` | 160 | `msg_action` |
| 161 | `msg_additem` | 162 | `msg_changeweapon` | 163 | `msg_choicecancel` | 164 | `msg_choicedisplay` |
| 165 | `msg_choicemade` | 166 | `msg_firetraning` | 167 | `msg_getwrapper` | 168 | `msg_lnkobjaction` |
| 169 | `msg_removeall` | 170 | `msg_selectorref` | 171 | `msg_setstate` | 172 | `msg_suspend` |
| 173 | `nearnotifyfar` | 174 | `nearnotifynear` | 175 | `offertalkmsg` | 176 | `opendoor` |
| 177 | `opendoor2` | 178 | `pickup_accept` | 179 | `pickup_deny` | 180 | `pickup_endoffer` |
| 181 | `pickup_pickup` | 182 | `pickup_request` | 183 | `pickup_startoffer` | 184 | `placeitem` |
| 185 | `protectend` | 186 | `protectstart` | 187 | `pushbutton` | 188 | `shootsound` |
| 189 | `staticcamera_activate` | 190 | `staticcamera_deactivate` | 191 | `stopoffertalkmsg` | 192 | `subtitlesnotifyclear` |
| 193 | `subtitleswrite` | 194 | `talk_accept` | 195 | `talk_deny` | 196 | `talk_end` |
| 197 | `talk_endoffer` | 198 | `talk_request` | 199 | `talk_start` | 200 | `talk_startoffer` |
| 201 | `transferitem` | 202 | `walksound` | 203 | `tisserlort` | 204 | `empty` |
| 205 | `msg_getavailablestates` | 206 | `msg_setitem` | 207 | `write` | 208 | `loadsavesetdifficulty` |
| 209 | `msg_setsize` | 210 | `hitman_dead` | 211 | `msg_disableinfos` | 212 | `msg_displayhint` |
| 213 | `msg_displayinfo` | 214 | `msg_displaywarning` | 215 | `msg_enableinfos` | 216 | `msg_missionfailed` |
| 217 | `msg_scrolllistaddentry` | 218 | `msg_scrolllistclear` | 219 | `msg_scrolllistclose` | 220 | `msg_scrolllistcount` |
| 221 | `msg_scrolllistdown` | 222 | `msg_scrolllistfocusnum` | 223 | `msg_scrolllistgetselected` | 224 | `msg_scrolllisthide` |
| 225 | `msg_scrolllisthighlight` | 226 | `msg_scrolllistopen` | 227 | `msg_scrolllistremoveentry` | 228 | `msg_scrolllistsetabs` |
| 229 | `msg_scrolllistup` | 230 | `msg_sethitpoints` | 231 | `msg_stresssetval` | 232 | `canopen` |
| 233 | `disableas_open` | 234 | `disablecui` | 235 | `enablecui` | 236 | `hideinventory` |
| 237 | `msg_addaction` | 238 | `msg_disableframeupd` | 239 | `msg_dropitem` | 240 | `msg_firstsaveinv` |
| 241 | `msg_hideosd` | 242 | `msg_initsaveinv` | 243 | `msg_isosdvisible` | 244 | `msg_lastaccomplished` |
| 245 | `msg_loadallinv` | 246 | `msg_loadselectedinv` | 247 | `msg_nightvisionmode` | 248 | `msg_osdaddowner` |
| 249 | `msg_removeaction` | 250 | `msg_savecurrentinv` | 251 | `msg_setlevelstats` | 252 | `msg_snipermode` |
| 253 | `msg_statshideosd` | 254 | `msg_stresssetfreq` | 255 | `msg_weaponammoclear` | 256 | `msg_weaponammohidename` |
| 257 | `msg_weaponammosetweapon` | 258 | `msg_weaponammoupdatespare` | 259 | `msg_weaponsdisplayshow` | 260 | `msg_writesubtitletemp` |
| 261 | `mapmenuisopened` | 262 | `showblack` | 263 | `msg_osdcancel` | 264 | `msg_setnumber` |
| 265 | `activateopenclosedoor` | 266 | `addseeableobject` | 267 | `animalbite` | 268 | `autoaimtargetref` |
| 269 | `camerahitman2direction` | 270 | `carrydeadbody` | 271 | `changevertices` | 272 | `checkinsidewaterenter` |
| 273 | `checkinsidewaterexit` | 274 | `cutsequence_lastframe` | 275 | `deactivateopenclosedoor` | 276 | `disablecamera` |
| 277 | `disablehitman` | 278 | `doorlocked` | 279 | `dragdeadbody` | 280 | `enablecamera` |
| 281 | `enablepidcontroller` | 282 | `firstpersonmodeforce` | 283 | `firstpersonmoderelease` | 284 | `forceautoaimdisableoff` |
| 285 | `forceautoaimdisableon` | 286 | `forcehitmandeath` | 287 | `hitmanaiming` | 288 | `hitmancontrolmode` |
| 289 | `hitmancontrolmodeprevious` | 290 | `msg_binoculars` | 291 | `msg_calcmats` | 292 | `msg_cameraplaneend` |
| 293 | `msg_cameraplanestart` | 294 | `msg_canpickupitem` | 295 | `msg_changeclothes` | 296 | `msg_climbwall` |
| 297 | `msg_climbwindow` | 298 | `msg_destroydrag` | 299 | `msg_fireweaponatpos` | 300 | `msg_getcameraplane` |
| 301 | `msg_hitmanreleasedrag` | 302 | `msg_inventorysetactiveleft` | 303 | `msg_inventorysetactiveright` | 304 | `msg_invertorysetactiveleft` |
| 305 | `msg_invertorysetactiveright` | 306 | `msg_lockhitmanintruck` | 307 | `msg_movemodify` | 308 | `msg_operateobject` |
| 309 | `msg_pickupitem` | 310 | `msg_pushbutton` | 311 | `msg_reloadweapon` | 312 | `msg_setarmorpoints` |
| 313 | `msg_standardshoeleft` | 314 | `msg_standardshoeright` | 315 | `msg_useitem` | 316 | `msg_sendvisibleping` |
| 317 | `operateontarget` | 318 | `placeitem2` | 319 | `place_bomb` | 320 | `reloaddone` |
| 321 | `removeseeableobject` | 322 | `setbreathtime` | 323 | `setdialogmaster` | 324 | `sethitmanorientation` |
| 325 | `sigtecon_crosshairmode` | 326 | `storehitmanglobalvalues` | 327 | `usebinoculars` | 328 | `coolioin` |
| 329 | `hitmandamagenotify` | 330 | `msg_disablecontrols` | 331 | `msg_enablecontrols` | 332 | `frameend` |
| 333 | `framestart` | 334 | `adddynobstacle` | 335 | `closedoorendframeno` | 336 | `closevehicledoor` |
| 337 | `damageinfo` | 338 | `detectweapon` | 339 | `findpath` | 340 | `helprequest` |
| 341 | `hitman_changedclothes` | 342 | `isopen` | 343 | `mos_animdone` | 344 | `msg_additemtoinventory` |
| 345 | `msg_changemesh` | 346 | `msg_changenude` | 347 | `msg_dispinfo` | 348 | `msg_dispwarning` |
| 349 | `msg_dragend` | 350 | `msg_dragstart` | 351 | `msg_editorcalctable` | 352 | `msg_hitmanallowdrag` |
| 353 | `msg_removeitemfrominventory` | 354 | `msg_requestweaponsallowed` | 355 | `maincameracontrol_getfollowgeom` | 356 | `msgprereacheddest` |
| 357 | `msgreacheddest` | 358 | `musicstacksize` | 359 | `objectnotvisible` | 360 | `objectnotvisibleinternal` |
| 361 | `objectvisible` | 362 | `objectvisibleinternal` | 363 | `opendoorstartframeno` | 364 | `openvehicledoor` |
| 365 | `pathcanceled` | 366 | `pathcanceledlockeddoor` | 367 | `patherror` | 368 | `pathfinished` |
| 369 | `pathopendoor` | 370 | `pathrequest` | 371 | `pathfinderopen` | 372 | `pathresponse` |
| 373 | `preparepathfinder` | 374 | `reportnewtarget` | 375 | `se_projectilehit` | 376 | `se_rockethit` |
| 377 | `se_vehiclehit` | 378 | `setbomb` | 379 | `setmusicaction` | 380 | `setmusiclocation` |
| 381 | `setmusicpreaction` | 382 | `vehiclecontinue` | 383 | `vehicletempstop` | 384 | `weapondetected` |
| 385 | `hitmandisabledrop` | 386 | `hitmanenabledrop` | 387 | (none) | 388 | `levelcontrolnotifyuseitem` |
| 389 | `levelcontrolnotifypickupitem` | 390 | `levelcontrolnotifychangeclothes` | 391 | `msg_seerobjectadd` | 392 | `msg_seeradd` |
| 393 | `msg_seerdel` | 394 | `msg_objectadd` | 395 | `msg_objectdel` | 396 | `msg_setvisrange` |
| 397 | `msg_setvisfov` | 398 | `msg_setvisoffset` | 399 | `msg_setvislightmultiplier` | 400 | `msg_forcevisible` |
| 401 | `msg_getallvisible` | 402 | `msg_checkvisibleto` | 403 | `msg_connectcui` | 404 | `msg_disconnectcui` |
| 405 | `msg_removeweaponsfrominventory` | 406 | `msg_addtoinventoryselection` | 407 | `msg_removefrominventoryselection` | 408 | `msg_giveallitems` |
| 409 | `inventoryownerdied` | 410 | `inventoryownerresurrected` | 411 | `msg_startanimation` | 412 | `msg_stopanimation` |
| 413 | `showmapicon` | 414 | `pause` | 415 | `unpause` | 416 | `cutsequence_begin` |
| 417 | `cutsequencerequestinfo` | 418 | `cutsequence_setdestpos` | 419 | `msg_enableinfodisplay` | 420 | `msg_deleteobject` |
| 421 | `camera_addwithprio` | 422 | `maincameracontrol_addforcecamera` | 423 | `maincameracontrol_removeforcecamera` | 424 | `msg_getmatposframenr` |
| 425 | `stopanim` | 426 | `forcestopanim` | 427 | `continueanim` | 428 | `runoffscreen` |
| 429 | `characterkilled` | 430 | `characterharmed` | 431 | `missionfailed` | 432 | `missioncompleted` |
| 433 | `briefingmenuopened` | 434 | `ingamemenuopened` | 435 | `ingamemapopened` | 436 | `msg_statdone` |
| 437 | `restartlevel` | 438 | `togglebriefing` | 439 | `togglemap` | 440 | `ingameinventoryopened` |
| 441 | `setloadsavemenucancelenabled` | 442 | `maincameracontrol_activatecamera` | 443 | `maincameracontrol_deactivatecamera` | 444 | `coolioout` |
| 445 | `cooliostart` | 446 | `cooliofire` | 447 | `peekkeyhole` | 448 | `usepolaroid` |
| 449 | `cooliofov` | 450 | `cooliozoomready` | 451 | `msg_requestkeyholeactive` | 452 | `msg_resetcamtobestpos` |
| 453 | `setquality` | 454 | `updateconfiguration` | 455 | `dialogcamera_setup` | 456 | `dialogcamera_stop` |
| 457 | `dialogcamera_switch` | 458 | `msg_showstats` | 459 | `msg_storestats` | 460 | `msg_showbriefingmenu` |
| 461 | `setobjective` | 462 | `setlocalefolder` | 463 | `setobjectivecompleted` | 464 | `briefingavailable` |
| 465 | `msg_showingamemenu` | 466 | `msg_enablecontinue` | 467 | `msg_enablerestart` | 468 | `missingcontrolleropen` |
| 469 | `missingcontrollerclose` | 470 | `ismissingcontrolleropen` | 471 | `msgadjustsoundgraph` | 472 | `deactivate` |
| 473 | `msg_sound_door` |  |  |  |  |  |  |
