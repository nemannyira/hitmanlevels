# The editor's cost per frame

Measured 2026-10-18 on C1-1 (Hitman standing, the editor's panels closed,
FpsLimit=60, the dev session's Lua handlers on) with Frida timers on the
editor's own functions (QueryPerformanceCounter around each call; Frida
adds about 1 µs to every call it times, so the small numbers are upper
bounds).

## Frame end (OnFrameEnd callbacks, after ZEngineDataBase::MainLoop)

| Callback | per frame |
|---|---|
| frame limiter (`framelimit.cpp`: waits for the next 1/60 s) | 12.7 ms (waiting: the game's whole frame took about 4 ms) |
| Lua: game events, triggers, story state, `"frame"` handlers (`dllmain.cpp`) | 0.21 ms |
| diagnostics' frame count | 0.01 ms |
| the other eight (vehicles, mission, play test, test tools, level, NPC guard, ...) | 3 to 8 µs each (Frida's own cost) |

## Hooks called while drawing

| Hook | calls | per call |
|---|---|---|
| IDirect3DDevice8::Present (the overlay, then Steam's overlay and the driver) | 60 / s | 141 µs, most of it the original Present |
| SetTransform (the highlight's world matrix, `highlight.cpp`) | 52 800 / s | ~1 µs (Frida's floor: a compare and the original call) |
| the draw calls (the highlight's node check) | 41 800 / s | ~1.7 µs (the same) |

So with its panels closed the editor takes well under 1 ms of a 16.7 ms
frame (the Lua frame and Present's overlay the largest). A level's own Lua
handlers add to the 0.21 ms: a `"message"` handler runs about 70 times a
second (`h2/docs/messages.md`).

## How to measure again

Frida (`mcp__frida__*`), one script per measurement (a script is unloaded
when its call returns: attach, `Thread.sleep(seconds)`, detach, return the
sums):

* the frame-end callbacks: `hkMainLoop` is where the game's MainLoop
  (0x422920) jumps; it walks a vector of function pointers (its begin / end
  are the two globals it reads after the original call) - attach to each;
* the device hooks: the device's vtable entries point into Steam's
  gameoverlayrenderer.dll, whose functions jump into the editor (E9);
  Present is the one called 60 times a second.

Never attach to a game function the editor hooks itself (`h2/docs/hooks.md`),
and do not write code into the game's process from Frida (a test that did
closed the game).
