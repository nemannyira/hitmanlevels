# Play statistics: what players' runs of a level tell, on the site

Decided with the owner 2026-11-14: two levels, built one after the other.

1. **Anonymous level statistics** (first): how often a level is played and finished, its ratings and times,
   and for its author where players fail, die and are spotted (heat maps on the map).
2. **Profiles and leaderboards** (after): the fastest runs per level (verified and unverified boards), a
   player's own page.

Sending is **off until the player turns it on** (opt-in). The game process stays off the network
(docs/online_levels.md section 1): the editor DLL writes a run to a local file, a separate program sends it.

## 1. A run

What one play of a downloaded level (`Mods\Online\<id>`) records; the player's own levels are left out (a
maker's test runs would skew them). Written by the DLL (h2/src/editor/runs.h, built 2026-11-14) at the run's
end to `Mods\Online\<id>\runs\<started>.json` (UTF-8, canonical; `<started>` like `20261114T100559Z`). One
run a map load: it starts when the level is on its map and the editor is closed.

| Field | From |
|---|---|
| `format` 1, `run`: a random id (32 hex), `level`, `version`, `dll`: HL_VERSION, `started` (UTC) | the installed level (its `package` id and version), the build |
| `result`: completed / failed / left | the statistics window (completed), the level routine's MissionFailed (`h2.mission`, the editor's goals, the level's own), leaving the map (another map, the main menu, a saved game loaded) or opening the editor (F8) |
| `time`: game seconds from the map's start | the statistics screen's time (the game clock, SysInterface +0x38 >> 10) |
| `counters`: shots, close encounters, headshots, alerts, enemies killed / harmed, innocents killed / harmed, saves | the statistics screen's lines; the same counters on the level routine and Hitman when it ends otherwise (h2/docs/roadmap.md section 8) |
| `rating` (the game's name: `SilentAssassin`...), `stealth`, `aggression` (0..100) | the statistics window, as it picks the rating (completed runs only) |
| `aids`: god mode, invisible Hitman, game speed, frozen | testtools AidsUsed (such a run counts for nothing) |
| `dev`: `DevCommands`, `LuaMemory` when on | H2Fix.ini |
| `stage`: the escalation's stage | stages.h (escalations only) |
| `alarm_highest` | the level routine's alarm level (the HUD's meter) at its highest |
| `events`: `objective_done` / `objective_lost` (slot), `spotted` (Hitman's place), `died` (place); each with `t`, game seconds | the editor's goals, the game's "spotted" (one a character in 10 s), Hitman's death; 500 at most |

The package's hash is not in the file: levelpack adds it when it sends a run (what it installed, its
`known.json`), and leaves out a run of a package changed since. A run with `aids` or `dev` is still written
(the player's own record) but the site leaves it out. A game closed without leaving the map writes nothing.

## 2. Sending

* `levelpack stats on | off | status | send`: the choice is kept in `%LOCALAPPDATA%\HitmanLevels\known.json`
  (default off). `send` posts every run not sent yet to `https://hitmanlevels.com/api/v1/runs` over HTTPS and
  marks it sent.
* When it is on, the DLL starts `levelpack stats send --quiet` as a detached process after it writes a run
  (only then: no process otherwise; the game process makes no connection).
* The editor's launcher and the end of a downloaded level ask once whether to share runs (plain words, a
  link to the privacy notice); the answer turns it on or off.

## 3. On the site (level 1)

* **Worker**: `POST /api/v1/runs` (no account): the run's level, version and package hash must be a listed
  one; the fields in range (time at least a level's plausible minimum, counters bounded); one run id once;
  a rate limit per client (Workers' rate limiting, nothing stored of the client). Stored: the run's numbers
  and events (D1), no IP, no account.
* **The level's page**: plays, finished %, Silent Assassin %, the rating's spread, the median time of the
  finished runs; the list sorts by most played and hardest (least finished).
* **The author's page** (signed in, their levels): where runs end (objective lost, Hitman died), how many
  alerts, and **heat maps** of deaths and of where Hitman was spotted, drawn on the map's own floor plans
  (the in-game map's projection: h2/docs/roadmap.md "The in-game map").
* **privacy.html**: the runs (anonymous numbers and places in a level), kept as long as the level is listed.

## 4. Level 2: profiles and leaderboards

* A player signs in in levelpack (OAuth device flow through the site; the token in Windows' Credential
  Manager), then picks a shown name; runs sent while signed in carry the account.
* **A run ticket**: before a ranked run levelpack asks the site for a ticket (random, bound to the account,
  the level's version and the DLL's version); the site notes the time; a reported time cannot be shorter
  than what the site saw between the ticket and the report (with a margin).
* **The run's log** as a hash chain of its events (each holding the hash of the one before), checked by the
  site for consistency (an objective done before its target died, a time shorter than walking there).
* **Boards**: per level the fastest Silent Assassin runs, "unverified" (passes the checks) and "verified"
  (the owner looked at a video the player linked). A player's page: levels played, ratings, best times.
* What the client cannot guarantee (docs/online_levels.md section 9): a forged result is always possible on
  the player's own PC; the checks make it costly and visible, the owner decides.

## 5. Steps (GitHub issues: #49 the epic, #50-#54)

1. The run in the DLL (the statistics screen's values found in the game's code; the local file).
2. Sending: levelpack stats, the DLL starting it, the one question.
3. The Worker's runs and the level's numbers on its page.
4. The author's page and the heat maps.
5. Profiles and leaderboards (level 2).
