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

Built 2026-10-24 (`shared/levelfmt/package.h`, `share.h`; the editor's *Level >
Share > Export package*, the installer `levelpack.exe`). A `.h2lvl` file
is a ZIP of **stored** files (no compression: a level is small, and without
inflating there is no "zip bomb" and no decompressor to attack):

| File | Content |
|---|---|
| `manifest.json` | format 1, the game (`h2`; `.h2lvl` files, later `h3` `.h3lvl`, `h4` `.h4lvl`), the level's id and version, title, base scene, author (name, Ed25519 public key), license, and every other file's name, size and SHA-256; in one canonical form (compact, keys sorted: the text must equal its own re-serialization, so no key twice, no second spelling a parser could read differently) |
| `manifest.sig` | the author's Ed25519 signature (64 bytes) of `"H2LVL manifest v1\n"` + `manifest.json` (the prefix keeps the key's signatures of other things apart) |
| `level.json` | the level as the editor saves it, with `package: {id, version}` |
| `scripts/<name>.lua` | the scripts its level.json lists (exactly those) |
| `README.md` | optional, plain text, shown by the installer only |
| `preview.png` | optional, shown by the installer only, never by the game |

What the reader (`OpenPackage`) checks before any of it is used:

* **The layout**: the end record last with no comment, the directory right
  before it, each file's two headers alike and the files one after another
  from the start: nothing before, between or after them (no polyglots, no
  hidden data); no encryption, ZIP64, data descriptors or links.
* **Only these names**, as above (a script's name as the editor allows:
  `[A-Za-z0-9_-]{1,32}.lua`, no device names); no name twice, also not in
  another case. Names from the archive are never used as paths: each allowed
  name maps to a fixed path in the install folder (this also rules out "zip
  slip": `..`, absolute paths, drive letters, `\\?\`, alternate data streams
  `x:y`, device names, links).
* **Sizes**: the archive ≤ 2 MB; level.json ≤ 1 MB, a script ≤ 256 KB,
  README ≤ 64 KB, the preview ≤ 1 MB and 2048 x 2048, the manifest ≤ 64 KB.
* **The signature** over the manifest with the key it names, S below the
  group's order (one form of a signature); then **the hashes**: the files are
  exactly the manifest's list, in its order, with its sizes and SHA-256.
* **The content**: level.json passes the strict check (section 5) and is the
  manifest's (id, version, name, base scene); the scripts are UTF-8 without
  NUL, the README plain text, the preview a PNG.
* **No game data**: a level refers to the game's files, it does not carry
  them. Models of other levels are references (`imports`: a game level's
  name and a mesh there); a map of its own (`own_map_of`, editor/own_map.h)
  is made again on the player's PC from his own game files (the copy is
  deterministic), never downloaded. This keeps the game's copyrighted data
  off the site and keeps binary game formats (GMS, PRM, TEX) out of the
  download path altogether.
* **Export** refuses a level a strict check refuses, missing scripts, and a
  file holding a path of the PC (`C:\Users\...`, the user's profile path):
  the Windows user name would go with it.

**Install** (`InstallPackage`): into a new folder `Mods\Online\.new-<random>`,
then swapped in for `Mods\Online\<id>\` (the old version moved aside first and
removed after; put back if the swap fails); the id comes from the signed
manifest, checked against `[a-z0-9]{8,32}`; the files are read-only, with the
manifest kept as `package.json` (the launcher and the File menu show the
author from it). Links in the way (`Mods`, `Online`, the level's folder) are
refused. The installer never writes anywhere else under the game.

**What the PC knows** (`%LOCALAPPDATA%\HitmanLevels\known.json`): the author
keys accepted (with the name each had then) and each installed level's key,
the version installed and the highest ever installed. The installer asks
about an author not seen before (`--yes`), warns when another key signs an
installed level's id (someone else's level passed off as its update:
`--allow-other-author`) and about an older version (`--allow-older`); the same
package again is "installed already". Exit codes: 0 done, 1 refused, 2
usage, 3 a confirmation missing.

## 4. Trust: signatures and transport

* **Transport**: HTTPS only (WinHTTP / Schannel, TLS 1.2+, the system's
  certificate checks; no HTTP fallback, no "ignore certificate errors").
* **Signatures**: the site signs each published package's manifest with an
  Ed25519 key (Monocypher 4.0.3 in the client, third_party/monocypher); the Hub has the public
  key built in (two keys, so one can be rotated). A package without a valid
  signature is refused even when it came over HTTPS: a compromised web
  server or CDN cannot serve altered levels unless it also has the signing
  key, which should live offline / in the server's signing service.
* **Authors' keys** (done 2026-10-24 for packages passed on by hand,
  section 3): the editor makes an Ed25519 key per author once (DPAPI, the
  PC's own LocalAppData) and signs each package with it; the installer shows
  the author's name with the key's fingerprint, asks about a key it has not
  seen, and warns when another key signs a level id it has installed. The
  site can take the key over as the author's (uploads signed with it show
  that version 3 comes from the author of version 1); a lost key (a new
  Windows) means a new key: a backup of the key (password-protected file) is
  left for later.
* **Versions**: a manifest carries a version that only grows; the installer
  keeps the highest it has installed per level and installs an older one
  only when asked (`--allow-older`; no silent downgrade to a vulnerable
  version).

## 5. The level file: what the client checks today and what to add

Already in the client (tested by `h2/tests/fuzz_levels.py` and
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
  settings, 16 scripts, 64 vehicles, 256 points shown on the map; the editor
  makes no more than that. The 38 levels made so far hold 15 placed objects
  at most.
* **What the engine can take while the map loads** (measured 2026-10-24,
  h2/docs/roadmap.md "Engine limits a level meets"): the moving objects' room
  lists (510 in a scene), the dynamic objects' array at load (1000), the
  records with data (0x2000), the map's icons on a floor (256) and the
  routine list (a level of 1800 points with icons never finished loading).
  The DLL keeps within each: the level's moving records are made dynamic
  after the scene's own and only while 128 room lists stay free, objects
  whose records would not fit are left out, the icons past 256 on a floor
  are not drawn, a level shows at most 256 points on the map. A hostile file
  of 2000 placed objects, all of them with icons, loads (and is cut).
* **Script names as a whitelist**: `[A-Za-z0-9_-]{1,32}.lua`, not Windows'
  device names (`CON`, `NUL`, `COM1`... with any extension), instead of the
  blacklist of `..`, `:` and slashes.

Added 2026-10-24: **the validator as a library** (`src/levelfmt/level_check`):
what a level.json may hold, in one place, with no knowledge of the game, built
into the DLL, into `h2level_check.exe` (the Hub's installer starts from it)
and into a libFuzzer target (`-DH2_FUZZ=ON`, clang-cl). Strict for a
downloaded level (the DLL's LoadLevel for `Mods\Online\<id>`, the Hub): a
field the editor does not write, a value of the wrong kind, out of range or
of the wrong characters, more than a level may hold, ids or scripts twice
refuse the level; lenient for the user's own (warnings in H2Fix.log, the
loader leaves those out). `h2/tests/level_check_test.py` (no game): 30
hostile cases both ways, and every level of Mods and examples accepted
strictly.

## 6. In the game: an untrusted mode for downloaded levels

The Lua sandbox (`script/lua_vm.cpp`) already has: no `io`, `debug`,
`package` / `require`, `dofile` / `loadfile`; `load` of source text only (a
precompiled chunk can corrupt the VM); no `string.dump`; `os` cut to
harmless functions; a memory limit; a time budget per call (3 s) and the
kill switch (a level's scripts that run out of time three times are
stopped); `h2.mem` only with LuaMemory=1; dev commands only with
DevCommands=1; long and strange text (`%n`, `%s`, NUL) checked.

For levels under `Mods\Online\<id>` (done 2026-10-24, `script::SetUntrusted`,
`editor::IsOnlineLevel`):

* **Memory**: 64 MB of Lua memory instead of 256 (a 32-bit game).
* **Time**: 1 s for a call instead of 3.
* **The user's own tools out while one runs**: `h2.dev_*` and `h2.mem` are
  taken out of `h2` (kept in the registry, back when the level is the user's
  own again), `Mods\_cmd.lua` is not run, the console takes no input (the
  level's globals share the state). `[Debug] DevCommandsOnline=1` keeps the
  dev commands for testing such a level; it is off by default.
* **The editor**: a downloaded level is not saved; *Save as a level of your
  own* makes a copy in `Mods\` (its scripts, by their names only), which is
  the user's from then on. The launcher lists `Online\<id>` levels.
* **Saved games**: `h2.state` stays limited (4 MB, depth 32, checked when
  read back); a downloaded level's file is `Save\<slot>.<id>.h2lua`, apart
  from local levels'.

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
   engine's load-time limits kept (done 2026-10-24); the validator as a
   library the DLL and the Hub share (done 2026-10-24: `h2/src/levelfmt`,
   `h2level_check.exe`).
2. **The untrusted mode** (section 6) for levels under Mods\Online\ (done
   2026-10-24; to test in the game once the Hub installs a level there: by
   hand, a folder `Mods\Online\<id>` works the same).
3. **The package format offline** (done 2026-10-24): export in the editor
   (Level > Share), `levelpack.exe` installs (author keys, manifest,
   hashes; section 3), so levels can be passed on without the site; the
   installer becomes the Hub's. `h2/tests/package_test.py` (no game: the
   RFC 8032 vectors, Python's Ed25519 both ways, hostile packages) and a
   libFuzzer target (`package_fuzz`, `-DH2_FUZZ=ON`).
4. **The Hub**: browsing and downloading with signatures, installing. The client side is built
   (2026-10-24, issue #6: `shared/levelfmt/hub.h`, `levelpack hub list|install`, `site keygen|sign`;
   the site's contract in [site_api.md](site_api.md)); the site (hitmanlevels.com) and its key are
   to come: until a site key is built in, the Hub is off.
5. **Accounts and uploads.**
6. **Play records and rankings** (section 9), after the server's checks are
   designed with them.

Each step can be tested as the editor is now: the validator with the fuzz
cases (a libFuzzer target too), the installer with hostile archives (zip slip,
bombs, wrong hashes and signatures, old versions), the untrusted mode with
`security_suite.lua` run as a downloaded level.
