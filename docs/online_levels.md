# Levels online: sharing, play records, rankings (client side)

How the editor's levels could be shared on a website (upload, download, who
played a level, rankings by completion time and score) as safely as possible,
seen from the client: the game, the editor DLL and a helper program on the
player's PC. The server is out of scope here except where the client depends
on it. Written 2026-10-23 from the code as it is (hardening already in place
is noted; what is missing is listed under each part and in the plan at the
end).

## 1. What is at stake

| Asset | Threat |
|---|---|
| The player's PC | A level is content from strangers: its files are parsed by the editor and its scripts run in the game. A crafted file that corrupts memory gets code execution. |
| The game install, saves, other levels | A package that writes outside its own folder (Mods\autoexec.lua runs at every start; with DevCommands, Mods\_cmd.lua is executed), or deletes / overwrites saves. |
| Accounts | Tokens stolen from files or logs; a phished login. |
| Rankings | Forged results: the player controls the game, the DLL and the scripts. |
| Privacy | Who played what is personal data (the players are in the EU: GDPR). |
| The game's copyright | Packages must not carry the game's own files. |

**The game process is the weakest point.** hitman2.exe and RenderD3D.dll are
built without ASLR, DEP (NX) or CFG (their PE headers: DllCharacteristics
none of 0x40 / 0x100 / 0x4000): addresses are fixed and data is executable,
so any memory corruption in the game process is easy to turn into code
execution. Everything below follows from that: **no network code and no
first parsing of downloaded data inside the game process.**

## 2. Architecture

```
  website (HTTPS API)
        |
  H2 Level Hub  (a separate 64-bit program: ASLR, DEP, CFG; no rights beyond
        |        the user's; optionally an AppContainer / low integrity)
        |  downloads, checks signatures and hashes, validates, installs;
        |  packs uploads; signs in; sends play records
        v
  Mods\Online\<id>\   (written only by the Hub; read-only for the game)
        |
  game + editor DLL  (no sockets; loads only installed, validated levels;
                      untrusted mode for downloaded ones; writes run
                      records to a local file the Hub picks up)
```

* The DLL never talks to the network. The Hub is the only networked part and
  is a modern, separately built program, so the game's missing mitigations
  are not exposed to the internet.
* The Hub and the DLL share one level validator (a static library built from
  the loader's checks, `editor/level.cpp` LoadLevel and the engine providers'
  limits), so what the Hub accepts is exactly what the DLL would load.
* The DLL validates again when it loads (a file may change between the Hub's
  check and the load; and levels can still come from elsewhere).

## 3. The package

A `.h2lvl` file: a ZIP (stored or deflate only) with

| File | Content |
|---|---|
| `manifest.json` | format version, level id, version, title, author id, base scene, editor version needed, the list of files with their sizes and SHA-256 |
| `manifest.sig` | Ed25519 signature of `manifest.json` by the site's key (see 4) |
| `level.json` | the level as the editor saves it |
| `scripts/*.lua` | its scripts (source text only) |
| `preview.png` | shown by the Hub only, never by the game |

Rules the Hub enforces before anything is written:

* **Only these names**: `level.json`, `scripts/[a-z0-9_-]{1,32}\.lua`,
  `preview.png`; no other file, no folders beyond `scripts/`, no
  duplicates differing in case only. Names from the archive are never used
  as paths: each allowed name maps to a fixed path in the install folder
  (this also rules out "zip slip": `..`, absolute paths, drive letters,
  `\\?\`, alternate data streams `x:y`, device names `CON`, `NUL`, `COM1`,
  links).
* **Sizes**: the archive ≤ 2 MB, each file ≤ 1 MB unpacked (level.json,
  scripts), ≤ 4 MB in all, at most 32 files; the unpacked size is counted
  while inflating, so a "zip bomb" stops at the limit (also a ratio limit,
  e.g. 100:1).
* **Hashes**: each file's SHA-256 matches the signed manifest.
* **No game data**: a level refers to the game's files, it does not carry
  them. Models of other levels are references (`imports`: a game level's
  name and a mesh there); a map of its own (`own_map_of`, editor/own_map.h)
  is made again on the player's PC from his own game files (the copy is
  deterministic), never downloaded. This keeps the game's copyrighted data
  off the site and keeps binary game formats (GMS, PRM, TEX) out of the
  download path altogether.
* **Install**: into a new temporary folder, then renamed to
  `Mods\Online\<id>\` (the id comes from the signed manifest, checked
  against `[a-z0-9]{8,32}`); the files are marked read-only. The Hub never
  writes anywhere else under the game.

## 4. Trust: signatures and transport

* **Transport**: HTTPS only (WinHTTP / Schannel, TLS 1.2+, the system's
  certificate checks; no HTTP fallback, no "ignore certificate errors").
* **Signatures**: the site signs each published package's manifest with an
  Ed25519 key (libsodium or Monocypher in the Hub); the Hub has the public
  key built in (two keys, so one can be rotated). A package without a valid
  signature is refused even when it came over HTTPS: a compromised web
  server or CDN cannot serve altered levels unless it also has the signing
  key, which should live offline / in the server's signing service.
* **Authors' keys** (later): the Hub can make an Ed25519 key per author
  (kept with DPAPI) and sign uploads with it, so the site can show that
  version 3 of a level comes from the same author as version 1.
* **Versions**: a manifest carries a version that only grows; the Hub keeps
  the highest it has seen per level and refuses an older one (no silent
  downgrade to a vulnerable version).

## 5. The level file: what the client checks today and what to add

Already in the client (tested by `tools/tests/fuzz_levels.py` and
`security_suite.lua`):

* level.json: wrong types, deep nesting (100000 levels), huge strings and
  numbers are refused or clamped; floats must be finite and in range
  (`std::isfinite`, ±1e7 positions, ±1e3 matrices; vehicles ±1e6); at most
  200 characters (`kMaxLevelNpcs`).
* Paths: scripts only from the level's folder (by a whitelist of names,
  below); other levels' names only of the installed game
  (`engine::IsGameLevel`, `LevelZipPath`); a map of its own's name
  `[A-Za-z][A-Za-z0-9_-]{2,23}`; map icon names from a fixed list.
* Scene injection (engine/inject.cpp): record indices range-checked, names
  ≤ 64 characters, list depth ≤ 127, models only where they are plain models
  of the scene.

Added 2026-10-23 (fuzz_levels "many placed", "script names"):

* **Counts**: a level file is cut to at most 2000 placed objects, 5000
  object edits, 256 changed lists of 256 points, 200 characters' new
  settings, 16 scripts (vehicles 64, new lists 256 already); the editor makes
  no more placed objects than that. The 38 levels made so far hold 15 placed
  objects at most. **Not enough yet**: each placed object becomes records of
  the scene while it loads (a point with a map icon two, a group's copy one
  per model), and 2000 points with icons (about 4000 records added to C1-1's
  4264) took the game down while the map loaded (fuzz_levels "many placed").
  The engine's capacity for records added at load is to be measured and
  inject.cpp given a budget for them (it knows how many it adds); until then
  a hostile file can still crash the game this way (not run code).
* **Script names as a whitelist**: `[A-Za-z0-9_-]{1,32}.lua`, not Windows'
  device names (`CON`, `NUL`, `COM1`... with any extension), instead of the
  blacklist of `..`, `:` and slashes.

To add for downloaded levels:

* **Strict schema**: unknown fields refused for downloaded levels (the
  editor's own files may keep being lenient).

## 6. In the game: an untrusted mode for downloaded levels

The Lua sandbox (`script/lua_vm.cpp`) already has: no `io`, `debug`,
`package` / `require`, `dofile` / `loadfile`; `load` of source text only (a
precompiled chunk can corrupt the VM); no `string.dump`; `os` cut to
harmless functions; a memory limit; a time budget per call (3 s) and the
kill switch (a level's scripts that run out of time three times are
stopped); `h2.mem` only with LuaMemory=1; dev commands only with
DevCommands=1; long and strange text (`%n`, `%s`, NUL) checked.

For levels under `Mods\Online\`:

* **Memory**: 256 MB of Lua memory is a lot in a 32-bit game that ran out of
  address space at 1.6 GB in a long test session; 32–64 MB for downloaded
  levels.
* **Time**: a shorter budget per call (e.g. 1 s).
* **Debug features off while one runs**, whatever H2Fix.ini says:
  DevCommands (it executes Mods\_cmd.lua and offers memory and script
  commands), LuaMemory (`h2.mem`), the editor's scripts console. These are
  the user's own tools, not for content from others.
* **The editor**: a downloaded level opens read-only; "Save as" makes a
  local copy (in Mods\, the user's own from then on).
* **Saved games**: `h2.state` stays limited (4 MB, depth 32, checked when
  read back), its file per save slot; a downloaded level's state is kept
  apart from local levels' (its id in the file name).

## 7. Accounts on the client

* Sign-in through the system browser (OAuth 2.0 authorization code with
  PKCE, or the device code flow); the Hub never sees the password and
  never shows its own password field.
* Tokens: a short-lived access token in memory, the refresh token in the
  Windows Credential Manager (DPAPI); never in H2Fix.ini, level files, the
  log or crash logs. Separate scopes for downloading, uploading and
  submitting play records; sign-out revokes the refresh token.
* The DLL never holds tokens: it writes play records to a local file, the
  Hub signs in and sends them.

## 8. Uploading a level

* The Hub packs only what the format allows (section 3) from the level's
  folder; it shows the list of files before sending.
* It checks the level with the same validator first, and that its scripts
  are text.
* level.json can hold names from the user's PC only where the editor puts
  them (none today); the Hub should still scan for `C:\Users\` and similar
  and strip / refuse, so that a Windows user name is not published.
* The author agrees to the site's terms (it is his own work, the license it
  is shared under); the Hub records which version he uploaded.

## 9. Who played it, rankings

**What the client cannot guarantee.** The score and the time are measured
on the player's PC, where the player controls the game, the DLL, its memory
and the level's scripts. No client-side design makes a forged result
impossible; obfuscation or a key built into the DLL only stops casual
cheating (the key can be read out). The design therefore makes cheating
costly and visible and leaves the decision to the server:

* **A run ticket**: when a ranked run starts, the Hub asks the server for a
  ticket (a random id bound to the user, the level's id and version and the
  DLL's version). The server notes the time; the time the player reports
  cannot be shorter than the time the server saw between the ticket and the
  report (minus a tolerance).
* **The game's own numbers**: the completion time, the rating (Silent
  Assassin...) and its statistics (shots, kills, alerts, bodies found) are
  the game's own values from its statistics screen (the profile code knows
  where they are: P15, CGlobalCom), not values the level's scripts compute.
* **A record of the run**: the events the editor already sees (kills,
  knock-outs, alarms, goals done, saves / loads, Hitman's health) with game
  times, in a hash chain, sent with the result. The server can check it for
  consistency (a goal done before its target died, a time too short to walk
  there: the path network gives a lower bound).
* **Conditions of a ranked run**: a released DLL build (its hash), the
  level's package hash unchanged, DevCommands, LuaMemory and the editor off
  for the whole run, no saved-game loads (or a separate board with saves).
  All reported, none provable: the server treats them as claims.
* **Server side** (for completeness): plausibility limits per level (from
  the author's and moderators' runs), statistics on outliers, a video for
  the top entries (as speedrunning sites do), reports from players,
  "verified" and "unverified" boards.

**Who played it** is personal data: a pseudonym chosen by the player, shown
only if he agrees (the Hub asks once, can be changed); no hardware ids, no
IP addresses on the site; the player can have his records deleted.

## 10. The client's own updates

The Hub and the DLL are code: they must never update themselves silently
from the site. An update is signed (Authenticode for the Hub; the DLL with
the same Ed25519 key scheme as packages), shown to the player, and its
version must be newer than the installed one.

## 11. A plan in steps

1. **Harden the DLL's loader now** (useful for levels shared today by hand):
   count limits, script name whitelist, fuzz cases (done 2026-10-23); the
   validator as a library the DLL and the Hub share.
2. **The untrusted mode** (section 6) for levels under Mods\Online\.
3. **The package format offline**: export / import `.h2lvl` in the editor
   (author key, manifest, hashes), so levels can be passed on without the
   site; the importer is the Hub's installer.
4. **The Hub**: browsing and downloading with signatures, installing.
5. **Accounts and uploads.**
6. **Play records and rankings** (section 9), after the server's checks are
   designed with them.

Each step can be tested as the editor is now: the validator with the fuzz
cases (a libFuzzer target too), the installer with hostile archives (zip slip,
bombs, wrong hashes and signatures, old versions), the untrusted mode with
`security_suite.lua` run as a downloaded level.
