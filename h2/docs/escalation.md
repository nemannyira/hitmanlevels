# Escalations (design)

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
`level.json` gets `stages` (1 to 10): what each stage adds on top of the
level, and `"replay"`: whether the player may play the stages done again (the
author's choice, on by default).

```json
"stages": [
  { "name": "The Courier", "text": "Kill the courier.",
    "objectives": [1], "rules": [] },
  { "name": "Quietly", "objectives": [1],
    "rules": [ { "kind": "unseen", "broken": "fail" },
               { "kind": "no_innocents", "broken": "note" } ] },
  { "name": "On the clock", "objectives": [1, 2],
    "rules": [ { "kind": "time", "seconds": 300 },
               { "kind": "kill_with", "object": "FiberWire" } ] }
]
```

* **The world grows with the stages.** Everything placed (characters,
  objects, points, routes) has the stage it comes in (`"stage": 2`; none =
  every stage). Stage 3 has what stages 1-3 placed. A thing can also be
  *gone from stage N* (a door opened in stage 1 is locked from stage 3).
* **Objectives** are the level's objectives; a stage names the ones it uses
  (later stages usually keep the earlier ones and add one).
* **Rules** are checked while playing. What breaking one does is the
  author's choice per rule (`"broken"`): `"fail"` fails the stage (with the
  reason on screen), `"note"` only says so on screen and marks the stage's
  result (done, but not clean: shown in the launcher; the rating as the game
  counts it). Each rule writes its own laptop line and is shown on the screen
  when the stage starts.

### Rules (a first set)

| rule | checked by | there now? |
|---|---|---|
| never spotted | the *unseen* goal | yes |
| time limit | a timer (`h2.after`), the time left on screen | yes (Courier) |
| no innocents killed | the `kill` event's `kind` | yes |
| no disguise / keep the start clothes | the `disguise` event | yes (event seen in scripts; check in game) |
| one disguise only (no change once taken) | the `disguise` event | as above |
| kill a target only with X (an item, fiber wire, a firearm kind, an accident) | the `kill` event's `type` + the weapon Hitman holds | needs work: what `type` holds, the weapon in his hands |
| no bodies found | the characters' "body found" reaction / the map's alarm | needs reverse engineering |
| no firearm drawn | Hitman's drawn weapon | needs work |
| extra target | an objective of the stage | yes |
| bring an object to a place | the *deliver* goal | yes |

The ones marked yes are the first version; the others follow.

## In the editor

* **Stage bar** (toolbar, only for an escalation): `Stage 1 2 3 [+]` (up to
  10). The
  world shows the chosen stage (what stages up to it placed). What is placed
  while a stage is chosen comes in that stage; the Inspector shows *comes in
  stage 2* (changeable) and *gone from stage …*.
* **Mission tab** gets a *Stages* section: per stage its name, text, which
  objectives it uses and its rules (picked from the list above, each with its
  setting: seconds, the item, *fails the stage* or *only noted*); for the
  whole escalation *the player may replay stages done*.
* **New level** offers *Escalation (stages)* besides a plain level; a plain
  level can be turned into one later (it becomes stage 1).
* **Test** plays the chosen stage. *Problems* lists stages that cannot be
  finished (no objective, a rule that contradicts another: "never spotted" +
  "kill with a firearm in sight" is allowed, but e.g. "keep the start
  clothes" with an objective to take a disguise is not).

## Playing it

* The launcher's level list shows an escalation with its stages:
  *Stage 2 of 3*, a mark when all are done (and whether cleanly); the next
  stage is open, the stages done can be played again when the author allows
  it (`"replay"`).
* At the start the stage's name and rules show on the screen and in the
  laptop. Breaking a rule fails the stage with the reason ("You were seen").
* Finishing a stage: the statistics screen as now, then "Stage 3 opened";
  the last one: "Escalation complete".
* Progress is the player's own, per level (next to the level's saves; not in
  the package, so a shared level starts at stage 1).

## Order of work

1. Format (`stages`, `stage` on placed things), the validator, loading the
   chosen stage's world.
2. Mission tab *Stages*, the stage bar, Test per stage.
3. Rules of the first set, their laptop lines and on-screen notes, failing
   a stage.
4. Launcher: stages, progress, the next stage opened.
5. Rules that need reverse engineering (kill method, bodies found, drawn
   weapon).
6. An example escalation in `examples/missions` and the docs.
