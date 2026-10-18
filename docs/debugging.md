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

## Reference made with these tools

`docs/classes.md` (classes), `docs/structures.md` (fields), `docs/messages.md`
(messages), `docs/script_commands.md` (the scripts' interfaces),
`docs/performance.md` (the editor's frame cost), `docs/hooks.md` (the
editor's hooks).
