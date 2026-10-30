# Escalations

The new games' *Escalation* mode, as a kind of level the editor makes: a
mission on a map in stages, each stage the one before plus a complication.

## What it is in the new games

* A short mission of its own on a map (new targets, not the map's story), in
  3-5 stages. Finishing a stage opens the next; a failed stage is played
  again.
* Each stage keeps everything of the one before and adds something:
  a rule (a kill method, a time limit, a disguise rule, no bodies found, never
  spotted), another target or task, or more security (guards, cameras, locked
  ways).
* The player sees the stage's rules before it starts and while playing.

## As a level of ours

An escalation is one level (`Mods\<folder>`), shared as one package. Its
`level.json` gets `stages` (1 to 10, each a name and the laptop's text of it),
`rules`, and `"replay"`: whether the player may play the stages done again (the
author's choice, on by default).

```json
"stages": [ { "name": "The Courier", "text": "Kill the courier." },
            { "name": "Quietly", "text": "He has a bodyguard now." },
            { "name": "On the clock", "text": "" } ],
"rules": [ { "kind": "unseen", "stage": 2, "broken": "fail" },
           { "kind": "no_innocents", "stage": 2, "broken": "note" },
           { "kind": "time", "seconds": 300, "stage": 3, "broken": "fail" } ],
"replay": true
```

* **Everything has its stages.** A placed object, item, point or character,
  an edit of a map object, an objective and a rule have `"stage": N` (there
  from stage N on; none: from the first) and `"gone_from": M` (not there from
  stage M on; none: never gone). Stage 3 has what is there in stage 3. A map
  object *gone* in a stage is hidden in it (a door, a crate); one that only
  *comes* in a stage is hidden before it.
* **Objectives** are the laptop's lines as in a plain level; a line can be one
  objective in stages 1-2 and another from stage 3 (the same slot, other
  stages).
* **Rules** are checked while playing. What breaking one does is the
  author's choice per rule (`"broken"`): `"fail"` fails the stage (the mission
  ends failed, the reason on screen), `"note"` only says so on screen and
  marks the stage's result (done, but not cleanly: shown in the launcher).
  The stage's name and rules are in the laptop's briefing and on screen when
  the stage starts; a time limit tells the time left (10, 5, 2, 1 minutes,
  30 and 10 seconds).

### Rules (a first set)

The weapon in Hitman's hands (#28): `ZHITMAN2 +0x3C5` (unaligned) is the
item's ref (0: empty hands), the item's template is `ZITEM +0x60`, a weapon
template's *Type* `ZItemTemplateWeapon +0xB8`: pistol 0, revolver 1, SMG 2,
machine gun 3, rifle 4, pump gun 5, shotgun 6, rocket launcher 7, knife 8,
fiber wire 9 (*Piano*), other 10 (chloroform), grenade 11. Characters with no
behaviour script report no kill to the level's statistics (no `kill` event),
so a target's rule listens to its `death`.

| rule | checked by | there now? |
|---|---|---|
| never spotted | the *unseen* goal | yes |
| time limit | game time played (`h2.time`; kept in saved games), the time left on screen | yes |
| no innocents killed | the `kill` event's `kind` | yes |
| no disguise / keep the start clothes | the `disguise` event | yes |
| one disguise only (no change once taken) | the `disguise` event | as above |
| kill a target only with X (fiber wire, a knife, close combat, a pistol, a firearm, one weapon of the map) | the stage's *Kill* targets' `death` (none: Hitman's `kill`), the weapon in his hands then (`h2.hand`) | yes (`kill_with`, #28) |
| no firearm drawn | the weapon in Hitman's hands, twice a second | yes (`no_firearm`, #28) |
| no bodies found | — | not yet: the game keeps no count of bodies found (the reaction is inside the characters' behaviour scripts); a TODO, #30 |
| extra target | an objective of the stage | yes |
| bring an object to a place | the *deliver* goal | yes |

The ones marked yes are the first version; the others follow.

## In the editor

* **Stage bar** (toolbar, only for an escalation): `Stage 1 2 3 [+]` (up to
  10). The map shows the chosen stage (its button loads the map for it,
  saved first). What is placed while a stage is shown comes in it; the
  Inspector's *stages [2] to [the last]* row changes a selected thing's
  stages (a map object's too: hidden out of them).
* **Mission tab**, *Stages (escalation)*: *Make it an escalation*; per stage
  its name, text and the rules coming in it (each with its setting and
  *fails the stage* / *only noted*), *Take this stage away* (its things come
  in the stage before); *Stages done can be played again*. Each objective
  line has its stages row.
* **Test** plays the stage shown. *Problems* lists a stage without an
  objective (when the objectives end the mission).
* Dev commands: `h2.dev_stage([n])` (the map for stage n), `h2.dev_launcher()`.

## Playing it

* The launcher's level list shows an escalation: *Escalation: stage 2 of 3*,
  *all done* (and whether cleanly), and the stage to start: the next open
  one, the stages done too when the author allows it (`"replay"`).
* At the start the stage's name and rules show on screen (a subtitle) and in
  the laptop. A rule broken: the stage fails with the reason ("You were
  seen"), or it is only noted.
* Finishing a stage (its mission completed): "Stage 3 opened"; the last one:
  "Escalation complete".
* Progress is the player's own, per level: `Save\escalations.json` (not in
  the package: a shared level starts at stage 1). A saved game keeps its
  stage (with its goals, `Save\<slot>.h2lua`; set again before the game
  loads the map, `ZEngineDataBase::LoadGame` 0x41AF70).

## Order of work

1. Format (`stages`, `stage` on placed things), the validator, loading the
   chosen stage's world. (done, #24)
2. Mission tab *Stages*, the stage bar, Test per stage. (done, #25)
3. Rules of the first set, their laptop lines and on-screen notes, failing
   a stage. (done, #26)
4. Launcher: stages, progress, the next stage opened. (done, #27)
5. Rules that need reverse engineering: kill method, drawn weapon (done,
   #28); bodies found (later, #30).
6. An example escalation in `examples/missions` and the docs.
