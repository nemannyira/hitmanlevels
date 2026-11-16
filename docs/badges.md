# Badges: what a player earns on a level and on their profile

GitHub: #58 (the site's badges, built 2026-11-16), #59 (the client's new data), #60 (later).

Decided with the owner 2026-11-16 (docs/play_stats.md is the data they are made from):

* Badges are **public**, like the player page's *Levels played* (the play history's switch is for the plays
  one by one only).
* Only **ranked runs** count (sent signed in with their ticket, docs/play_stats.md section 4): the site has
  checked their time. Runs with testing aids or dev settings are never sent.
* **Not retroactive**: a badge is earned when a run arrives (or the event that gives it happens) after the
  badges went live; the runs sent before give none.
* **Under Par**: the par time is the site's (below), not the author's.

## 1. A level's badges (a player earns them on one level; an escalation: on one stage)

Earned by one ranked run (as it arrives; kept when the board or the par changes later):

| Badge | The run |
|---|---|
| Finished | completed |
| Silent Assassin | rated SilentAssassin |
| Ghost | completed, 0 alerts, never spotted (no `spotted` event) |
| No Saves | completed, 0 saves |
| Clean Hands | completed, 0 innocents killed or harmed |
| No Shots Fired | completed, 0 shots |
| Under Par | completed in the par time or less: the 25th percentile of the level's (stage's) completed ranked runs, once it has 10 of them (before that, none) |
| First to Finish | the level's (stage's) first completed ranked run |
| Board Top 3 | in the first three of the level's (stage's) Silent Assassin board as it arrives |

With the client's new data (section 4), from the runs of an editor that sends it:

| Badge | The run |
|---|---|
| Suit Only | completed without changing clothes (the level's start outfit is not a change) |
| Precision | completed, no kill the game counts (it does not count its targets, nor the editor's mission targets) and a target of the editor's mission killed |
| No Bodies Found | completed, no body found by anyone |

Only from a run that says it tracks these events (`tracked`): an older editor's run has none of them without
none having happened.

**Mastery** of a level: how many of its badges the player has, as a level 1..5 (1, 3, 5, 7, 9 badges). An
escalation's stages are counted together (a badge of any stage counts once).

**Rarity**: the level's page shows each badge with the share of the players with a ranked run on the level
(stage) who have it.

## 2. An escalation's badges

* Escalation Complete: every stage finished. Clean Sweep: every stage Silent Assassin.
* The stage medals are the stage's badges above: bronze Finished, silver Ghost, gold Silent Assassin.

## 3. A profile's badges

From the level badges the player earned (so not retroactive either):

| Badge | |
|---|---|
| Finished 1 / 10 / 50 | levels finished |
| Silent Assassin 5 / 25 | levels with Silent Assassin |
| Map Tourist 5 / 10 | levels finished on that many of the game's maps |
| Escalation Master 3 | escalations complete |
| Verified Runner | a run on a verified board (the owner verified its video) |
| Board Holder | first on a level's board (as the run arrived) |

An author's (their account's levels):

| Badge | |
|---|---|
| First Level | a level uploaded |
| Played by 10 / 100 | that many different players (not the author) have a ranked run of one of their levels |
| Tough Nut | a level 20 players (not the author) played, fewer than a fifth of them finished it |

The author's own runs of their level earn them the level's badges as anyone's; they never count for the
author's badges.

## Built (2026-11-16, #58)

* `website/migrations/0008_badges.sql`: `badges` (user, badge, level, stage, the run, when); a `played` row a
  player and level (stage) for the rarity.
* `website/src/badges.js`: `awardRun` when a ranked run arrives (POST /api/v1/runs; the run is kept if it fails),
  `awardProfile` (an upload: First Level; the owner verifying a video: Verified Runner), `playerBadges`
  (GET /api/players/<id> `badges`), `levelBadges` (GET /api/levels/<id> `badges`: players per badge, all
  stages and per stage). An account deleted takes its badges and those on its levels; a level taken down
  hides its badges.
* The pages: the player page's *Badges* (the profile's; each level with its mastery marks), the level page's
  *Badges* (the share of its players, with the stage picker). Names and texts: `BADGES` in site.js.
* Tests: `website/test/badges_test.mjs` (the rules on the migrations, node:sqlite), `worker_test.mjs` (a
  ranked run's badges end to end).

## 4. The client's new data (the runs, docs/play_stats.md section 1)

The DLL adds to a run's `events` (the site takes these kinds once it knows them: the site goes first):

* `disguise`: Hitman's clothes changed (from the game's own change of outfit), with the outfit's name.
* `kill`: a character killed by Hitman, with how (the game's own kind of death: weapon, its kind, wire,
  poison, an accident...) and whether it was a target of the mission.
* `body_found`: a body found by a character (the game's own reaction to it).

Each from the game's code (Ghidra, the engine's messages), not guessed.

Built 2026-11-16 (h2/src/editor/runs.cpp; docs/play_stats.md section 1), from what the escalations' rules
already read: the clothes notification's text is the clothes (`BodyguardMafia_01_HMas`: the model), seen 2026-11-16
coming about 2 s after the change (the level's start outfit's too: left out by its model); the weapon kind
(engine::WeaponKind, the template's Type) in Hitman's hands at the kill; ObjectVisible's param the body's
geom reference (seen: both characters who saw a patrolling guard's body got it). The editor listens to a
message by its name (`msg:<name>`, script/events.h). The site takes the events and `tracked`
(website/src/worker.js), gives Suit Only, Precision and No Bodies Found (badges.js). Tested: runs_test (a
run with the clothes, the target's kill with the pistol, the body found; the start outfit not counted),
badges_test, worker_test.

## 5. Later (noted, not built)

* **The author's own challenges** for a level, with the escalations' rules engine (3-5 optional ones with
  their own names and texts): the DLL writes the ones done into the run, the site gives badges.
* **The author's par time** (the editor, the package, the site) beside the site's.
* **Crowd Favourite** (among the most played levels): a ranking that changes, unlike earned badges.
* Badges of the kill's method ("Fiber Wire Only", "Accidents Only") once section 4's data comes in.
