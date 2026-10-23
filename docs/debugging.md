# Debugging and reverse engineering the game

The tools used on hitman2.exe and how they fit together (2026-10-18). Only one
debugger can be attached to the game at a time.

## Ghidra

Project `Hitman2` (repo root): `hitman2_dump.exe` (the unpacked image,
`re/`), `RenderD3D.dll`. The game's 342 registered classes and routines are
named (`tools/classinfo.py`, `docs/classes.md`): `<name>_ctor`, `_vtable`,
`_Create`, `Register_<name>` and `<name>_vf<offset>` for a function only one
class's vtable has.

Its MCP server's HTTP endpoint (127.0.0.1:8089) takes bulk requests too, e.g.
`POST /create_label?program=hitman2_dump.exe` with `{"labels": [{address,
name}, ...]}`. The program must be a query parameter: without it a request goes
to the active program. Inline scripts are off (`GHIDRA_MCP_ALLOW_SCRIPTS`).

## Crashes

* `src/crashlog.cpp` logs every exception outside the editor's own guarded
  probes to H2Fix.log: code, address (`module+offset`), registers and the code
  addresses on the stack.
* `python tools/crash.py [log] [--last N]` prints those blocks with each
  hitman2.exe address's function from the Ghidra project (a call chain).
* To stop at the crash itself: x32dbg (`x32dbg/release/x32`), attached to the
  running game. Its settings for this game: `[Exceptions]
  IgnoreRange=00000000-FFFFFFFF:first` (first-chance exceptions go to the game:
  the editor's probes fault on purpose, and so does the game's own code), no
  TLS callback or entry breakpoints. It then stops only at an exception nothing
  handles; the call stack and registers are there.

## x32dbg

MCP plugin (x64dbg-MCP Server, 127.0.0.1:9094), as tools `mcp__x64dbg__*`
through `x32dbg/mcp_stdio_bridge.py` (which starts x32dbg when it is not
running). Attach to the game's pid; `detach` leaves the game running. Logging
breakpoints did not resume on their own (fast resume): read the arguments at
the stop, delete it, run on. `LogRedirect "<file>"` / `LogRedirectStop` keep
its log (the file is locked until stopped).

## Frida, Cheat Engine

* Frida (`mcp__frida__*`): Interceptor hooks that log arguments without
  stopping the game, for functions called often.
* Cheat Engine (`mcp__cheatengine__*`): memory scans, "what accesses / writes
  this address", structure dissection. Use it without its debugger while
  x32dbg is attached.
* Neither goes on a function the editor hooks itself (`docs/hooks.md`; such a
  function starts with a jmp, 0xE9): use the editor's own hook and its log.

## apitrace

`third_party/apitrace-14.0-win32` (32-bit, as the game). The renderer
(RenderD3D.dll) uses d3d8.dll: put apitrace's d3d8.dll wrapper next to
hitman2.exe (the game starts through Steam), trace, then remove the wrapper.
A trace replays frame by frame with every call, buffer and state; two traces'
frames can be compared (images, state).

## Tests

`tools/tests`, against the installed DLL in a running game with
`DevCommands=1` (H2Fix.ini [Debug]; `tools/dev.py` sends Lua to it).

* `python tools/tests/run_all.py`: all of them in one go. It backs up Save
  (into `dumps/test_runs/<time>/save_backup`, put back as it was at the end),
  turns DevCommands and LuaMemory on for the run (H2Fix.ini put back after),
  starts the game through Steam (`tools/start_game.ps1`: Steam's launch dialog
  and a "Steam Service Error" box answered; the intros skipped with Esc), runs
  each test on a freshly loaded C1-1, starts the game again when a test took it
  down and before the stress suite (which fills the map). Report:
  `dumps/test_runs/<time>/report.md`, each test's output next to it.
  `--only api,gui`, `--skip maps_gameway,fuzz,run_maps`, `--keep-game`,
  `--list`.
* The Lua suites (`api_suite.lua`, `security_suite.lua`, `stress_suite.lua`)
  run through `run_suite.py`; the others are Python scripts ending with
  "N passed, M failed".
* `maps_gameway_test.py [C2-1 ...]`: a level's copy of a plain model, a point
  and another weapon for an armed guard on other maps (one of each chapter by
  default), made with the map, kept by a saved game. What a map does not have
  (no plain model near Hitman, no point template) is a NOTE, not a failure.
* `gui_test.py`: the editor's own controls pressed by their labels.
  `h2.dev_press(label [, n])` finds the control through ImGui's test engine
  hooks (`IMGUI_ENABLE_TEST_ENGINE`; on only while a press is asked for) and
  clicks its middle (the n-th control of that label; "##id" not part of the
  label); a panel is scrolled to a control out of sight. `h2.dev_press_status()`:
  `"looking"`, `"pressed x y"` or `"not found"` (after about 120 frames). The
  editor must be open and the control's panel shown (`h2.dev_editor(true)`,
  `h2.dev_panel(name)`). `h2.dev_selected()`: the selection's path (a copy
  just made by Duplicate is selected).

## Reference made with these tools

`docs/classes.md` (classes), `docs/structures.md` (fields), `docs/messages.md`
(messages), `docs/script_commands.md` (the scripts' interfaces),
`docs/performance.md` (the editor's frame cost), `docs/hooks.md` (the
editor's hooks).

## Sweep of 2026-10-20

* Exceptions: H2Fix.log (crash.py) none; Frida's exception handler counted 0
  first-chance exceptions in 10 s of editing a level with every kind of object
  and in 20 s of the API suite.
* Hooks: every one of `docs/hooks.md` starts with a jmp into the editor (Frida,
  39 / 39).
* Memory: 8 loads of C1-1 without a level 259 -> 260 MB private, 8 with a
  level of copies, an imported model, points and a character 268 -> 272 MB,
  two runs of imports_save_test leave nothing: no leak per load. A game that
  ran a whole test session before the stress suite ran out of its 32-bit
  address space (RenderD3D's abort at 1.6 GB); the suite fills the engine on
  purpose and wants a freshly started game.
* The paths inject.cpp builds for the scene's records (a character's settings'
  references while the scene loads: h2.dev_recordpaths) are the editor's for
  every object of the world; 602 of 6105 differ, all the interface's windows
  (put elsewhere at init), which no behaviour setting names.
