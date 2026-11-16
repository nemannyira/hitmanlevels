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
| `events` (2026-11-16, #59): `disguise` (`outfit`: the character's model), `kill` (`victim`: enemy / innocent / target, `weapon`: the kind in Hitman's hands, `item` a thing that is not a weapon, `none`; `at`), `body_found` (`at`: the body) | the game's clothes notification (levelcontrolnotifychangeclothes, its text `<model>_HMas`; the level's own start outfit as the map loads left out), the statistics' kills (2 enemy, 1 innocent) and the death of a target of the editor's mission (the objectives; the original mission's own targets are not known: not written), the game's sight telling a living character it sees a dead or knocked out one (ObjectVisible, once a body) |
| `tracked`: `["disguise", "kill", "body_found"]` | the events this editor writes (an older one's run: none of them, not "none happened") |

The package's hash is not in the file: levelpack adds it when it sends a run (what it installed, its
`known.json`), and leaves out a run of a package changed since. A run with `aids` or `dev` is still written
(the player's own record) but never sent. A game closed without leaving the map writes nothing.

## 2. Sending

Built 2026-11-14 (#51; shared/levelfmt/stats.h, `levelpack stats`):

* `levelpack stats on | off | status | send [--quiet]`: the choice is kept in
  `%LOCALAPPDATA%\HitmanLevels\known.json` (`"stats"`; off until chosen). `send` posts each run not sent yet to
  `https://hitmanlevels.com/api/v1/runs` over HTTPS (docs/site_api.md "Runs") and moves it to `runs\sent`
  (`runs\refused` when the site refuses it). `status` counts them.
* Never sent: a run with `aids` or `dev`, one of another version than the installed one, one of a level whose
  files changed since levelpack installed it (package.json's hashes) or that levelpack did not install. The
  installer notes the package's SHA-256 in known.json (sent with the run); an update keeps the `runs` folder.
* When it is on, the DLL starts the game folder's `levelpack.exe stats send --quiet` as a detached process
  after it writes a run (only then; the game process makes no connection). levelpack.exe is installed next
  to hitman2.exe (release/README.txt).
* The editor's launcher asks once, when a downloaded level is picked in *Open a level* (plain words, a link to
  the privacy notice): *Share them* / *Don't share*.

## 3. On the site (level 1)

Built 2026-11-14 (#52; website/src/worker.js `postRun`, migration 0004_runs.sql):

* **Worker**: `POST /api/v1/runs` (no account, no Origin: levelpack posts it): the run's game, level, version
  and package hash must be a listed package (not taken down, its author not blocked); every field in range
  (time up to a day, counters up to 100000, at most 500 events, no aids or dev settings); one run id once
  (409); at most 30 runs a minute from one address (Workers' rate limiting: the address is not stored).
  Stored: the run's numbers and events, when it arrived; no IP, no account, not when it was played. Deleted
  with the level (an account deleted takes its levels' runs). A plausible minimum time per level is not
  checked yet (nothing to base it on before runs come in).
* **The level's page**: plays, finished %, Silent Assassin %, the ratings' spread, the median time of the
  finished runs (every version together); the list shows each level's plays and finished % and sorts by
  most played and hardest (least finished, of the levels played at least 5 times).
* **Escalations** (2026-11-15, h2/docs/escalation.md "Shared on the site"): the level's numbers and boards per
  stage (a stage picker), its stages shown, the author's page per stage, the game's map for the stage loaded.
* **The author's page** (built 2026-11-14, #53: website/public/author.html, `GET /api/author/levels/<id>/plays`;
  signed in, their levels): plays, finished, failed, left, alerts a play, how the runs not finished end (Hitman
  died, objective n lost, left), and the places where Hitman died or was spotted from above (x across, z up, a
  metre grid: no game art on the site, decided with the owner 2026-11-14: the floor plans are IO Interactive's).
* **The same places on the game's own map** (#53, h2/src/editor/ingame_map.h PlayMarkCopies): the author page's
  *Download for the editor* gives `plays.json`; saved in the level's folder (`Mods\<level>\plays.json`, the
  author's own level: its `package` id must be the file's `level`), the map loaded with the level gets a map
  icon at each place, on the floor of its place (Hitman died: Icon_Target, red; spotted: Icon_PointOfInterest,
  "!"; at most 100 of each, the map draws 256 icons a floor). The in-game map's x and z run as the site's plot.
  The file is not packaged (ExportLevelFolder takes the level's own files only).
* **privacy.html** (done with #52): the runs (anonymous numbers and places in a level), kept as long as the
  level is on the site.

## 4. Level 2: profiles and leaderboards

Built 2026-11-14 (#54; website/src/players.js, migration 0005_players.sql, `levelpack signin`, `stats ticket`).
Decided with the owner then: no hash chain of the events (the editor is open source: a forger computes the
same chain; the site checks what it can itself), the ticket taken as a run starts, the site account's name.

* **Signing in levelpack**: `levelpack signin` asks the site for a device code (`POST /api/v1/device`), shows
  the 8-letter code and the site's device page (opened in the browser when run at a console); the player,
  signed in on the site, types it there (`device.html`, `POST /api/device/approve`); levelpack asks for its
  token every 5 s (`POST /api/v1/device/token`: 428 until approved, then the token once) and keeps it in
  Windows' Credential Manager (`HitmanLevels/<site>`). The site keeps its SHA-256 only. `levelpack signout`
  ends it on both sides; `stats status` names the account. The name shown is the site account's (Discord /
  GitHub / Google's at first), changed in the site's account box (`POST /api/me/name`).
* **A run ticket**: as a downloaded level's run starts, the DLL starts `levelpack stats ticket <level>
  <version> <run file>` (sharing on; levelpack does nothing when not signed in): the site notes the time
  (`POST /api/v1/tickets`, a listed level's version) and levelpack writes `runs\<run>.ticket`; `stats send`
  sends it with the run (the run's `ticket`) and moves it along. The site takes a ticket once, for that
  account, level and version.
* **Ranked**: a run sent signed in with its ticket, when the site saw at least its time (less 30 s) pass
  between the ticket and the report and its events are in order within its time. Others still count for the
  level's numbers and the player's page.
* **Boards** (the level's page): each player's fastest ranked Silent Assassin run, "unverified" (ranked) and
  "verified" (the player linked a video on their player page, `POST /api/me/video`, https only; the owner
  watched it: admin *Videos of runs*, verify / unverify).
* **A player's page** (`player.html?id=`, `GET /api/players/<id>`): the listed levels played, plays,
  finished, the best Silent Assassin time (verified or not); the player's own view lists their ranked
  runs to link videos (`GET /api/me/runs`).
* **A player's play history** (2026-11-16, migration 0007_history.sql; decided with the owner then): the
  plays the player sent signed in, one by one, newest first (the level, its stage, finished / failed / left,
  the time, the rating, the day the site got it: when it was played is not kept), and the totals (plays,
  levels, finished, Silent Assassin, time played); `GET /api/players/<id>/history`. The player always sees
  it on their page; others only when the player turns it on there (*Show my play history to everyone*,
  `POST /api/me/history`, `users.history_public`: off at first). The listed levels' plays only, as the
  page's table.
* An account deleted: its runs stay as anonymous ones (no account, not ranked, no video); its levelpack
  tokens, tickets and device codes go. The daily clean-up deletes device codes that ran out and tickets two
  days old.
* What the client cannot guarantee (docs/online_levels.md section 9): a forged result is always possible on
  the player's own PC; the checks make it costly and visible, the owner decides.

## 5. Steps (GitHub issues: #49 the epic, #50-#54)

1. The run in the DLL (the statistics screen's values found in the game's code; the local file).
2. Sending: levelpack stats, the DLL starting it, the one question.
3. The Worker's runs and the level's numbers on its page.
4. The author's page and the heat maps.
5. Profiles and leaderboards (level 2).
