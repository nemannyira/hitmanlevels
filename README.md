# hitmanlevels

Level editors for the classic Hitman games, and the way their levels are shared:

| Game | Folder | State |
|---|---|---|
| Hitman 2: Silent Assassin (2002) | [`h2/`](h2/) | the editor works: [h2/docs/editor.md](h2/docs/editor.md) |
| Hitman: Contracts (2004) | `h3/` | to come |
| Hitman: Blood Money (2006) | `h4/` | to come |

Each editor is a DLL the game loads (Hitman 2: `dinput8.dll` next to `hitman2.exe`): an in-game editor
(objects, characters, routes, mission goals, Lua scripts), its levels kept in `Mods\<level>\level.json`.
Levels travel as signed packages (`.h2lvl`, later `.h3lvl`, `.h4lvl`): [docs/online_levels.md](docs/online_levels.md);
the site for them, hitmanlevels.com, is planned: [docs/site_api.md](docs/site_api.md).

## Layout

```
h2/            Hitman 2: the editor DLL's code (src/), its docs, tests, tools, example levels, H2Fix.ini
shared/        what every game uses: levelfmt/ (packages, signatures, installer, the Hub's client)
tools/         tools for every game: levelpack (installs, exports, inspects packages; signs the site's list)
website/       hitmanlevels.com (Cloudflare; not built by CMake)
third_party/   imgui, ImGuizmo, json, Lua, MinHook, Monocypher
docs/          the shared docs: level packages and their sharing, the site's API
```

Code shared by the games moves to `shared/` once a second game needs it (the overlay, the Lua sandbox,
the hooks are Hitman 2's for now).

## Build

Windows, Visual Studio (x86 tools) and CMake: `build.bat` (or `build.bat Release install`, which copies
Hitman 2's DLL into its Steam folder). Everything lands in `build\Release`: `dinput8.dll`, `levelpack.exe`,
`h2level_check.exe`.

## Tests

`python h2/tests/run_all.py` runs every Hitman 2 test with the game (about 30 minutes; it backs up the
saves and puts them back); `python h2/tests/package_test.py` checks the packages without the game.

## License

MIT ([LICENSE](LICENSE)); the third-party code under its own licenses.
