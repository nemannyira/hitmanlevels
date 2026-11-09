# CLAUDE.md

Working rules for this repository (hitmanlevels: a level editor and level packages for Hitman 2: Silent
Assassin, built as the game's `dinput8.dll`). Talk to the user in Hungarian; code, comments, docs, commit
messages and Ghidra names in English.

## 1. Think before coding

Don't assume. Don't hide confusion. Surface tradeoffs.

* State your assumptions explicitly. If uncertain, ask.
* If a request can be read several ways, present them; don't pick one silently.
* If a simpler approach exists, say so. Push back when warranted.
* If something is unclear, stop, name what is confusing, and ask.

## 2. Start from the game's own code (reverse engineering first)

Everything that touches the engine starts from what we can read back from the existing code, not from
guesses or trial and error.

* Before implementing or fixing engine-facing behaviour, find how the game itself does it and use that path
  (its functions, its data, its messages). Test in the game only to confirm.
* Never ship a speculative fix. Trace a bug through the engine before changing code or blaming something.
* Use the MCP tools whenever they are available (the ones used so far):
  * **Ghidra** (`mcp__ghidra__*`): project `Hitman2.gpr` at the repo root: `hitman2_dump.exe` (the
    unpacked game) and `RenderD3D.dll`. Decompile, follow xrefs, read data. Name every function or field
    you work out (`Module_Function`, in English), add a plate comment saying what it does and where this
    repo uses it, then save the programs.
  * **x32dbg** (`mcp__x64dbg__*`): attach to the running game for breakpoints, call stacks, registers.
  * **Frida** (`mcp__frida__*`): hooks and memory reads without pausing the game.
  * **Cheat Engine** (`mcp__cheatengine__*`): memory reads, scans, structures.
  * **apitrace** (`mcp__apitrace__*`): Direct3D 8 traces (install the d3d8 wrapper, always uninstall it after).
  * Only one debugger may be attached to the game at a time.
* Write what you find into `h2/docs/` (addresses, structures, what the editor does with them).

## 3. Simplicity first

The minimum code that solves the problem. Nothing speculative.

* No features beyond what was asked.
* No abstractions for single-use code.
* No "flexibility" or "configurability" that was not requested.
* No error handling for impossible cases (engine memory is still read through the guarded helpers, as the
  surrounding code does).
* If you write 200 lines and it could be 50, rewrite it. Would a senior engineer call it overcomplicated?
  Then simplify.

## 4. Surgical changes

Touch only what you must. Clean up only your own mess.

* Don't "improve" adjacent code, comments or formatting; don't refactor what isn't broken.
* Match the existing style (naming, comment density, the docs' plain English), even if you would do it
  differently.
* Unrelated dead code: mention it, don't delete it.
* Remove the imports, variables and functions that *your* change made unused; leave pre-existing dead code
  unless asked.
* Every changed line should trace directly to the user's request.

## 5. Goal-driven execution

Define success criteria and loop until they are verified.

* "Add validation" → write tests for invalid inputs, then make them pass.
* "Fix the bug" → write a test that reproduces it, then make it pass.
* "Refactor X" → the tests pass before and after.

For multi-step tasks state a brief plan:

```
1. [Step] → verify: [check]
2. [Step] → verify: [check]
```

## The repository

* `h2/` Hitman 2: `src/` (the DLL: `engine/`, `editor/`, `script/`, `overlay/`, `levelfmt/`), `docs/`,
  `tests/`, `tools/` (`dev.py`, `start_game.ps1`, RE scripts), `examples/`, `H2Fix.ini`.
* `shared/levelfmt` (packages, signing, sharing), `tools/levelpack`, `website/`, `docs/`.
* Not tracked: `re/`, `dumps/`, `x32dbg/`, `build/`, the Ghidra project. Never commit game files.

## Build, install, test

* Build everything: `build.bat Release` (x86, MSVC, Ninja → `build\Release`). `build.bat Release install`
  also copies `dinput8.dll` into the game folder (the game must not be running).
* The game runs only when the user allows it. Before a run that saves games, back up `Save`; afterwards put
  `H2Fix.ini` back to `DevCommands=0` (`LogFiles`, `LuaMemory` stay 0), kill the game, remove test levels.
* Start the game with `h2/tools/start_game.ps1`, skip the intros (Esc until the main menu), drive it with
  `h2/tools/dev.py` (`h2.dev_*` commands). Don't send keys while the user is playing.
* Run game tests from PowerShell, not Bash (Bash's sandbox hides files the game must see).
  Everything: `python h2/tests/run_all.py`; a part: `--only name,name`. Reports go to `dumps/test_runs/`.
* A glitch the user reports comes from their own play: read their log and level first.

## Git

* Commit as `Pintér Tamás <106834442+nemannyira@users.noreply.github.com>`, never the global identity.
* Push only when the user explicitly approves:
  `git -c credential.helper= -c "credential.helper=!gh auth git-credential" push`.
* Open defects and planned work are GitHub issues (`nemannyira/hitmanlevels`); close them with "Fixes #n".
