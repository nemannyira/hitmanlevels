"""The goals that need the game's own moves, played with its keys:
"Hitman brings an object to a place" (a revolver picked up with E, carried
to a point, dropped with G) and "a character is knocked out" (C1-1's garage
guard, chloroform from behind: E, 1 draws it, the mouse button). Also that
a knock-out is not a death ("dies" goal, npc:alive) and that npc:kill() kills
a knocked out character. Uses Mods/_KOTest (removed); C1-1; a running game
with dev commands.

  python tools/tests/knockout_test.py
"""
import os
import shutil
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, ".."))
import dev  # noqa: E402

LEVEL = "_KOTest"
FOLDER = os.path.join(dev.GAME, "Mods", LEVEL)
WEAPONS = "ROOT/C1-1/C1-1_CHARACTERS.zip/AllLevels/Equipment.zip/AllLevels/Weapons.zip/Weapons/"
GUARD = "ROOT/C1-1/C1-1_CHARACTERS.zip/GarageGate/BodyguardMafia_GarageGate/Ground"
G = 'local g = h2.find([[%s]]) ' % GUARD
DIK_1, DIK_E, DIK_G = 0x02, 0x12, 0x22
results = []


def lua(code, timeout=30):
    out = dev.run(code, timeout)
    if out.startswith("error") or "timeout" in out:
        raise RuntimeError(out)
    return out


def check(name, ok, detail=""):
    results.append(bool(ok))
    print("%s %s %s" % ("PASS" if ok else "FAIL", name, detail))


def load():
    lua(r'return h2.dev_loadscene([[C1-1\C1-1__MAIN]])')
    time.sleep(4)
    t = time.time()
    while time.time() - t < 150 and "C1-1__MAIN" not in dev.run("return h2.scene()", 5):
        time.sleep(1)
    time.sleep(8)
    lua('GOALS = {} if not GOALS_WATCHED then GOALS_WATCHED = true '  # (console handlers outlive a map load; goals_test uses the same one)
        'h2.on("objective", function(slot) GOALS[#GOALS + 1] = slot end) end '
        'h2.god(true) h2.invisible(true) return 1')


def done():
    return lua('table.sort(GOALS) return table.concat(GOALS, ",")')


def key(dik, secs=0.2):
    lua("h2.dev_key(%d, %f) return 1" % (dik, secs))
    time.sleep(1.5)


def at(x, y, z, yaw=None):
    lua("h2.player():setpos(%f, %f, %f) return 1" % (x, y, z))
    if yaw is not None:
        lua("h2.player():setrot(%f) return 1" % yaw)
    time.sleep(1.5)


def guard_pos():
    return [float(v) for v in lua(G + 'local x, y, z = g:pos() return x .. "," .. y .. "," .. z').split(",")]


def knock_out():
    """Chloroform (placed beside the guard) picked up, drawn, used from behind."""
    gx, gy, gz = guard_pos()
    at(gx - 290, gy + 5, gz)
    key(DIK_E)
    key(DIK_1)
    lua(G + "g:faceto(%f, %f, %f) return 1" % (gx + 500, gy, gz))  # its back to Hitman
    at(gx - 70, gy + 5, gz, 270)  # (yaw 270: towards +x)
    lua("h2.dev_mouse(0, 3.0) return 1")
    time.sleep(5)


def make(goal):
    load()  # (a new level is for the map loaded)
    lua('return h2.dev_level("new", "%s")' % LEVEL)
    gx, gy, gz = guard_pos()
    check("%s: goal set" % goal, "objective" in lua('return h2.dev_level("%s", [[%s]])' % (goal, GUARD)))
    lua('return h2.dev_edit("item", [[%s]], %f, %f, %f)' % (WEAPONS + "CloseCombatChloroform", gx - 250, gy + 3, gz))
    return gx, gy, gz


try:
    shutil.rmtree(FOLDER, ignore_errors=True)
    # 1. knocked out: the goal done, the character alive
    make("knockout")
    lua('return h2.dev_level("save")')
    load()
    knock_out()
    state = lua(G + 'return g:state() .. " " .. tostring(g:alive()) .. " " .. tostring(g:knockedout())')
    check("knock-out: unconscious, alive", state == "Being2.Unconscious true true", state)
    check("knock-out: goal done", done() == "5", done())

    # 2. a "dies" goal: not by a knock-out, by a death afterwards
    shutil.rmtree(FOLDER, ignore_errors=True)
    make("target")
    lua('return h2.dev_level("save")')
    load()
    knock_out()
    time.sleep(2)
    check("dies: not by a knock-out", done() == "", done())
    check("dies: npc:kill() on the knocked out one", lua(G + "return tostring(g:kill())") == "true")
    time.sleep(4)
    state = lua(G + 'return g:state() .. " " .. tostring(g:alive())')
    check("dies: dead now", state == "Being2.Dead false", state)
    check("dies: goal done", done() == "5", done())

    # 3. a revolver brought to a point, dropped there with G
    shutil.rmtree(FOLDER, ignore_errors=True)
    load()
    lua('return h2.dev_level("new", "%s")' % LEVEL)
    gx, gy, gz = guard_pos()  # (the flat yard by the garage: points of the path network)
    x, y, z = [float(v) for v in lua('return table.concat({h2.walkable(%f, %f, %f, 400)}, ",")' % (gx - 500, gy, gz + 200)).split(",")]
    px, py, pz = [float(v) for v in lua('return table.concat({h2.walkable(%f, %f, %f, 400)}, ",")' % (gx - 500, gy, gz - 300)).split(",")]
    gun = lua('return h2.dev_edit("item", [[%s]], %f, %f, %f)' % (WEAPONS + "GunColtPython", x, y + 3, z))
    point = lua('return h2.dev_edit("point", %f, %f, %f)' % (px, py, pz))
    goal = lua('return h2.dev_level("deliver", [[%s]])' % gun)
    check("deliver: goal set", goal.startswith("objective "), goal)
    slot = int(goal.split()[1].rstrip(":"))
    check("deliver: place set", "to Point" in lua('return h2.dev_level("place", "%d|%s")' % (slot, point)))
    lua('return h2.dev_level("save")')
    load()
    at(x - 60, y + 5, z)
    key(DIK_E)
    at(px, py + 5, pz)
    time.sleep(1)
    check("deliver: held at the place, not done", done() == "", done())
    key(DIK_G, 0.3)
    time.sleep(2)
    check("deliver: dropped there, done", done() == str(slot), done())
finally:
    dev.run('h2.god(false) h2.invisible(false) return h2.dev_level("revert")', 10)
    shutil.rmtree(FOLDER, ignore_errors=True)
print("\n%d passed, %d failed" % (sum(results), len(results) - sum(results)))
