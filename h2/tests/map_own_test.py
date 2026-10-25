"""The in-game map (M) of a level on a map of its own (editor/own_map.h: the
level's base map copied as Scenes\\<Name>, its floors' names LOC keys of the
original): a level on C1-1 with its models on the map, a copy of a tree and a
point with an icon is moved onto a map of its own; loaded there, the map has
its floors and window, Hitman, the point's icon and the copy's plan model are
on its floors, the map opens; a saved game keeps them on the map. Uses
Mods/_MapOwnTest, Scenes\\MapOwnTest (removed after) and save slot 9
(overwritten: back up the saves first); a running game with dev commands.

  python h2/tests/map_own_test.py
"""
import os
import shutil
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "tools"))
import dev  # noqa: E402

LEVEL = "_MapOwnTest"
OWN = "MapOwnTest"
FOLDER = os.path.join(dev.GAME, "Mods", LEVEL)
SCENES = os.path.join(dev.GAME, "Scenes", OWN)
LOG = os.path.join(dev.GAME, "H2Fix.log")
SOURCE = "Tree_02_21"  # C1-1's, near Hitman's start
SLOT = 9
DIK_M = 0x32
results = []


def lua(code, timeout=30):
    out = dev.run(code, timeout)
    if out.startswith("error") or "timeout" in out:
        raise RuntimeError(out)
    return out


def check(name, ok, detail=""):
    results.append(bool(ok))
    print("%s %s %s" % ("PASS" if ok else "FAIL", name, detail))


def wait_map(scene):
    dev.wait_ready(scene, 180)


def size():
    return os.path.getsize(LOG)


def since(at):
    with open(LOG, encoding="latin1") as f:
        f.seek(at)
        return f.read()


def floor_of(find):
    """the map floor the object found by that Lua expression is in ("" none)"""
    return lua('local o = %s if not o or not o:parent() then return "" end local p = o:parent() '
               'if h2.dev_routine(p:path(), "MapGroup") == "none" then return "" end return p:name()' % find)


def mapicon(find):
    return lua('local o = %s if not o then return "missing|" end local a, b = h2.dev_mapicon(o:path()) '
               'return a .. "|" .. b' % find)


def open_map():
    for attempt in range(7):  # (a level's start may not let it open yet)
        lua("h2.dev_key(%d, 0.1) return 1" % DIK_M)
        time.sleep(2.5)
        out = lua("return (h2.dev_mapicons())")
        if out.splitlines()[0] != "floor":
            dev.shot("map_own")
            lua("h2.dev_key(%d, 0.1) return 1" % DIK_M)
            time.sleep(1.5)
            return out
        time.sleep(2.5)
    return out


shutil.rmtree(FOLDER, ignore_errors=True)
shutil.rmtree(SCENES, ignore_errors=True)
try:
    lua(r'return h2.dev_loadscene([[C1-1\C1-1__MAIN]])')
    wait_map("C1-1__MAIN")
    lua('return h2.dev_level("new", "%s")' % LEVEL)
    lua('return h2.dev_level("mapmodels", "on")')
    src = lua('return h2.find("%s"):path()' % SOURCE)
    x, y, z = (float(v) for v in lua('local x, y, z = h2.find([[%s]]):pos() return x .. " " .. y .. " " .. z' % src).split())
    copy = lua('return h2.dev_edit("dup", [[%s]], %f, %f, %f)' % (src, x + 400, y, z)).rsplit("/", 1)[-1]
    at = lua('local x, y, z = h2.player():pos() return string.format("%.0f, %.0f, %.0f", x + 200, y, z)')
    point = lua('return h2.dev_edit("point", %s)' % at).rsplit("/", 1)[-1]
    lua('return h2.dev_level("mapicon", [[ROOT/%s|Icon_PointOfInterest]])' % point)
    lua('return h2.dev_level("save")')
    scene = lua('return h2.dev_level("ownmap", "%s")' % OWN)
    check("a map of its own made", OWN.lower() in scene.lower() and os.path.isdir(SCENES), scene)
    mark = size()
    lua('return h2.dev_level("load", "%s")' % LEVEL)
    lua('return h2.dev_loadscene([[%s]])' % scene)
    wait_map(OWN + "__MAIN")
    check("loaded: the map of its own", OWN.lower() in lua("return h2.scene()").lower())
    check("no exception", "EXCEPTION" not in since(mark))
    check("its map's window", lua("local a, b = h2.dev_mapicons() return b") != "no window")
    hero = mapicon("h2.player()")
    check("Hitman on a floor", hero.split("|")[1] == "MAP_Outside", hero)
    check("the point's icon on the floor", mapicon('h2.find("%s_MapIcon")' % point) == "Icon_PointOfInterest shown|MAP_Outside")
    check("the copy drawn on the floor", floor_of('h2.find("%s_Map")' % copy) == "MAP_Outside")
    drawn = open_map()
    check("the map opens on its floor", drawn.startswith("floor C1-1/IngameMap/Outside"), drawn.splitlines()[:1])
    check("it draws the point's icon", ("/%s_MapIcon" % point) in drawn)
    save_file = os.path.join(dev.GAME, "Save", "%d.1" % SLOT)
    t0 = os.path.getmtime(save_file) if os.path.exists(save_file) else 0
    mark = size()
    lua("return tostring(h2.dev_savegame(%d))" % SLOT)
    time.sleep(3)
    check("saved game", os.path.exists(save_file) and os.path.getmtime(save_file) > t0)
    audit = [l.strip() for l in since(mark).splitlines() if "save audit: slot %d:" % SLOT in l]
    check("save audit: every pointer placed", audit and audit[-1].endswith("unplaced 0"), audit[-1] if audit else "no line")
    mark = size()
    lua("return tostring(h2.dev_loadgame(%d))" % SLOT)
    wait_map(OWN + "__MAIN")
    check("loaded game: no exception", "EXCEPTION" not in since(mark))
    check("loaded game: the point's icon on the floor",
          mapicon('h2.find("%s_MapIcon")' % point) == "Icon_PointOfInterest shown|MAP_Outside")
    check("loaded game: the copy drawn on the floor", floor_of('h2.find("%s_Map")' % copy) == "MAP_Outside")
finally:
    dev.run('return h2.dev_level("revert")', 10)
    lua(r'return h2.dev_loadscene([[C1-1\C1-1__MAIN]])')  # (off the map of its own: its files can go)
    wait_map("C1-1__MAIN")
    shutil.rmtree(FOLDER, ignore_errors=True)
    shutil.rmtree(SCENES, ignore_errors=True)
print("\n%d passed, %d failed" % (sum(results), len(results) - sum(results)))
