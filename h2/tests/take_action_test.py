"""A "Hitman takes it" goal of one of the scene's own models (not an item): the
game offers it as an action ("Take", kind 6: messages.h AddUseAction), Hitman
uses it with the action key facing it, the goal is done and the model gone; a
saved game keeps it so and loads. A "bring it to a place" goal of another
model: taken the same way, done when Hitman is in the place's zone (a box with
a CheckInside routine made with the scene). Uses Mods/_TakeActionTest and save slot 9
(removed / overwritten: back up the saves first); a running game with dev
commands.

  python h2/tests/take_action_test.py
"""
import os
import shutil
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "tools"))
import dev  # noqa: E402

LEVEL = "_TakeActionTest"
FOLDER = os.path.join(dev.GAME, "Mods", LEVEL)
LOG = os.path.join(dev.GAME, "H2Fix.log")
SLOT = 9
TREE = "Tree_02_21"  # C1-1's, near Hitman's start
TREE2 = "Tree_02_20"
DIK_E = 0x12
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
    dev.wait_ready("C1-1__MAIN", 150)


def load(level=None):
    if level:
        lua('return h2.dev_level("load", "%s")' % level)
    lua(r'return h2.dev_loadscene([[C1-1\C1-1__MAIN]])')
    wait_map()


def done():
    return lua('table.sort(GOALS) return table.concat(GOALS, ",")')


def face_tree_and_press(name=TREE):
    lua('local t = h2.find("%s") local tx, ty, tz = t:pos() local p = h2.player() local x, y, z = p:pos() '
        'local dx, dz = tx - x, tz - z local d = math.sqrt(dx * dx + dz * dz) '
        'p:setpos(tx - dx / d * 120, y, tz - dz / d * 120) p:faceto(tx, ty, tz) return 1' % name)
    time.sleep(1.5)
    lua("h2.dev_key(%d, 0.3) return 1" % DIK_E)
    time.sleep(1.5)


if os.path.isdir(FOLDER):
    shutil.rmtree(FOLDER)
try:
    lua('return h2.dev_level("new", "%s")' % LEVEL)
    load()
    tree = lua('return h2.find("%s"):path()' % TREE)
    tree2 = lua('return h2.find("%s"):path()' % TREE2)
    check("take: the scene's model", "Hitman takes" in lua('return h2.dev_level("take", [[%s]])' % tree))
    px, py, pz = [float(v) for v in lua('local x, y, z = h2.player():pos() return x .. "," .. y .. "," .. z').split(",")]
    point = lua('return h2.dev_edit("point", %f, %f, %f)' % (px, py, pz - 400))
    goal = lua('return h2.dev_level("deliver", [[%s]])' % tree2)
    slot = int(goal.split()[1].rstrip(":"))
    check("deliver: place set", "to Point" in lua('return h2.dev_level("place", "%d|%s")' % (slot, point)))
    lua('return h2.dev_level("save")')
    load(LEVEL)
    lua('GOALS = {} if not GOALS_WATCHED then GOALS_WATCHED = true '
        'h2.on("objective", function(slot) GOALS[#GOALS + 1] = slot end) end h2.god(true) h2.invisible(true) return 1')
    action = lua('return h2.dev_routine(h2.find("%s"):path(), "Action")' % TREE)
    check("the game's action on it (kind 6)", "+72 00000006" in action, action[:120])
    face_tree_and_press()
    check("used: objective done", done() != "", done())  # (its line: the first the map's own leave free)
    gone = lua('local t = h2.find([[%s]]) local _, ty = t:pos() return tostring(t:hidden()) .. " " .. math.floor(ty)' % tree)
    check("used: the model out of the world", gone.startswith("true") and int(gone.split()[1]) < -50000, gone)
    first = done()
    zone = lua('return h2.dev_routine("H2_Deliver_%d", "CheckInside")' % slot)
    check("deliver: the place's zone made with the scene", zone != "none", zone[:80])
    face_tree_and_press(TREE2)
    check("deliver: taken, not there yet", done() == first, done())
    lua("h2.player():setpos(%f, %f, %f) return 1" % (px, py, pz - 400))
    time.sleep(2)
    check("deliver: brought there, done", done() != first and str(slot) in done().split(","), done())
    save_file = os.path.join(dev.GAME, "Save", "%d.1" % SLOT)
    before = os.path.getmtime(save_file) if os.path.exists(save_file) else 0
    lua("return tostring(h2.dev_savegame(%d))" % SLOT)
    time.sleep(3)
    check("saved game", os.path.exists(save_file) and os.path.getmtime(save_file) > before)
    lua("return tostring(h2.dev_loadgame(%d))" % SLOT)
    wait_map()
    with open(LOG, encoding="latin1") as f:
        crashed = any("EXCEPTION" in line and "hitman2.exe" in line for line in f.readlines()[-400:])
    check("loaded game: no exception", not crashed)
    gone = lua('local t = h2.find([[%s]]) local _, ty = t:pos() return tostring(t:hidden()) .. " " .. math.floor(ty)' % tree)
    check("loaded game: the model still taken", gone.startswith("true") and int(gone.split()[1]) < -50000, gone)
finally:
    dev.run('return h2.dev_level("revert")', 10)
    shutil.rmtree(FOLDER, ignore_errors=True)
print("\n%d passed, %d failed" % (sum(results), len(results) - sum(results)))
