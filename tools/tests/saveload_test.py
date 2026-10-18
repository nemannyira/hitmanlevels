"""Save / load with a level: the game saved in slot 9 (the user's saves must be
backed up first), the world and the story changed, the save loaded: the
level's edits are back, h2.state is the saved one, the level's scripts ran
again, the "save" / "load" events came. Uses Mods/_SaveTest (removed at the
end); a running game with dev commands.

  python tools/tests/saveload_test.py
"""
import os
import shutil
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, ".."))
import dev  # noqa: E402

LEVEL = "_SaveTest"
FOLDER = os.path.join(dev.GAME, "Mods", LEVEL)
SLOT = 9
MAIN = """
h2.state.runs = (h2.state.runs or 0) + 1
SAVETEST_RAN = (SAVETEST_RAN or 0) + 1
h2.on("save", function(e) h2.state.saved_slot = e.slot end)
h2.on("load", function(e) SAVETEST_LOADED = e.slot end)
"""
results = []


def lua(code, timeout=30):
    out = dev.run(code, timeout)
    if out.startswith("error") or "timeout" in out:
        raise RuntimeError(out)
    return out


def check(name, ok, detail=""):
    results.append(bool(ok))
    print("%s %s %s" % ("PASS" if ok else "FAIL", name, detail))


def wait_map():
    time.sleep(4)
    t = time.time()
    while time.time() - t < 150:
        if "C1-1__MAIN" in dev.run("return h2.scene()", 5):
            break
        time.sleep(1)
    time.sleep(8)


def pos(path):
    return [float(v) for v in lua('local x, y, z = h2.find([[%s]]):pos() return x .. " " .. y .. " " .. z' % path).split()]


def near(a, b, tol=2.0):
    return all(abs(x - y) <= tol for x, y in zip(a, b))


if os.path.isdir(FOLDER):
    shutil.rmtree(FOLDER)
try:
    lua(r'return h2.dev_loadscene([[C1-1\C1-1__MAIN]])')
    wait_map()
    lua('return h2.dev_level("new", "%s")' % LEVEL)
    open(os.path.join(FOLDER, "main.lua"), "w", encoding="utf-8").write(MAIN)
    CRATE = lua('for _, o in ipairs(h2.objects("ZSTDOBJ")) do if o:mesh() and o:mesh() ~= 0 and not o:hidden() '
                'and o:path():find("LOC_Mansion") then return o:path() end end')
    start = pos(CRATE)
    moved = [start[0] + 60, start[1], start[2] + 30]
    lua('h2.dev_edit("move", [[%s]], %f, %f, %f) return true' % (CRATE, *moved))
    POINT = lua('return h2.dev_edit("point", -4176, -20, -6682)')
    lua('return h2.dev_level("save")')
    lua(r'return h2.dev_loadscene([[C1-1\C1-1__MAIN]])')  # the level applied and its scripts run by the map
    wait_map()
    check("level applied on load", near(pos(CRATE), moved), str(pos(CRATE)))
    check("script ran", lua("return tostring(SAVETEST_RAN)") != "nil")
    lua('h2.state.stage = 5 h2.state.note = "saved" return true')
    save_file = os.path.join(dev.GAME, "Save", "%d.1" % SLOT)
    before = os.path.getmtime(save_file) if os.path.exists(save_file) else 0
    lua("return tostring(h2.dev_savegame(%d))" % SLOT)  # (the game's own result says nothing)
    time.sleep(3)
    check("save", os.path.exists(save_file) and os.path.getmtime(save_file) > before)
    with open(os.path.join(dev.GAME, "H2Fix.log"), encoding="latin1") as f:
        audit = [line.strip() for line in f if "save audit: slot %d:" % SLOT in line]
    check("save audit: every pointer placed", audit and audit[-1].endswith("unplaced 0"), audit[-1] if audit else "no line")
    check("save event",lua("return tostring(h2.state.saved_slot)") == str(SLOT),
          lua("return tostring(h2.state.saved_slot)"))
    # change the story and the world
    lua('h2.state.stage = 9 h2.state.note = "changed" SAVETEST_RAN = nil SAVETEST_LOADED = nil return true')
    lua('local p = h2.player() local x, y, z = p:pos() p:setpos(x + 300, y, z) return true')
    check("load", lua("return tostring(h2.dev_loadgame(%d))" % SLOT) == "true")
    wait_map()
    check("state restored", lua('return h2.state.stage .. " " .. h2.state.note') == "5 saved",
          lua('return tostring(h2.state.stage) .. " " .. tostring(h2.state.note)'))
    check("edit back after load", near(pos(CRATE), moved), str(pos(CRATE)))
    check("point back after load", lua('return tostring(h2.find([[%s]]) ~= nil)' % POINT) == "true")
    check("scripts ran again", lua("return tostring(SAVETEST_RAN)") != "nil")
    check("load event", lua("return tostring(SAVETEST_LOADED)") == str(SLOT), lua("return tostring(SAVETEST_LOADED)"))
    check("no problems", lua('return h2.dev_level("problems")') == "", lua('return h2.dev_level("problems")'))
finally:
    dev.run('return h2.dev_level("revert")', 10)
    shutil.rmtree(FOLDER, ignore_errors=True)
print("\n%d passed, %d failed" % (sum(results), len(results) - sum(results)))
