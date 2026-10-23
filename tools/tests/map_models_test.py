"""The in-game map (M) of custom levels (editor/ingame_map.h):

* a level on C1-1 that asks for its models on the map: a copy of one of the
  map's models is drawn there (its "<id>_Map" plan model, made with the map,
  put onto the floor the map shows the copy on);
* a map without buildings (the launcher's New level): the map's window and its
  legend's icons stay, its floors' plans and fixed icons are taken out, the
  level's floor is drawn instead, and the map draws Hitman's icon.

Screenshots of the open map go to dumps/shots (map_models_*.png). Uses
Mods/_MapModelsTest and Mods/_MapModelsBlank (removed); a running game with
dev commands.

  python tools/tests/map_models_test.py
"""
import os
import re
import shutil
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, ".."))
import dev  # noqa: E402

LEVELS = ["_MapModelsTest", "_MapModelsBlank"]
LOG = os.path.join(dev.GAME, "H2Fix.log")
SOURCE = "Tree_02_21"  # C1-1's, near Hitman's start
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


def wait_map():
    time.sleep(4)
    t = time.time()
    while time.time() - t < 150:
        if "C1-1__MAIN" in dev.run("return h2.scene()", 5):
            break
        time.sleep(1)
    time.sleep(8)


def load(level=None):
    if level:
        lua('return h2.dev_level("load", "%s")' % level)
    lua(r'return h2.dev_loadscene([[C1-1\C1-1__MAIN]])')
    wait_map()


def size():
    return os.path.getsize(LOG)


def since(at):
    with open(LOG, encoding="latin1") as f:
        f.seek(at)
        return f.read()


def on_floor(name):
    """the map floor the object is on ("" none): its parent with a MapGroup routine"""
    return lua('local o = h2.find("%s") if not o or not o:parent() then return "" end '
               'local p = o:parent() if h2.dev_routine(p:path(), "MapGroup") == "none" then return "" end '
               'return p:name()' % name)


def hidden(name):
    return lua('local o = h2.find("%s") if not o then return "missing" end return tostring(o:hidden())' % name)


def open_map(shot):
    """the map opened with M: what it draws (a screenshot of it), closed again"""
    lua("h2.dev_key(%d, 0.1) return 1" % DIK_M)
    time.sleep(2.5)
    out = lua("return h2.dev_mapicons()")
    dev.shot(shot)
    lua("h2.dev_key(%d, 0.1) return 1" % DIK_M)
    time.sleep(1.5)
    return out


for level in LEVELS:
    shutil.rmtree(os.path.join(dev.GAME, "Mods", level), ignore_errors=True)
try:
    # an original map, the level's models asked for on the map
    load()
    lua('return h2.dev_level("new", "%s")' % LEVELS[0])
    src = lua('return h2.find("%s"):path()' % SOURCE)
    at = [float(v) for v in lua('local x, y, z = h2.find([[%s]]):pos() return x .. " " .. y .. " " .. z' % src).split()]
    copy = lua('return h2.dev_edit("dup", [[%s]], %f, %f, %f)' % (src, at[0] + 400, at[1], at[2])).rsplit("/", 1)[-1]
    lua('return h2.dev_level("mapmodels", "on")')
    lua('return h2.dev_level("save")')
    mark = size()
    load(LEVELS[0])
    log = since(mark)
    check("C1-1: no exception", "EXCEPTION" not in log)
    check("C1-1: the plan model made with the map", "scene map: 1 models" in log,
          [l.strip() for l in log.splitlines() if "scene map" in l][-1:])
    floor = on_floor(copy + "_Map")
    check("C1-1: on a floor of the map", floor != "", floor)
    check("C1-1: shown", hidden(copy + "_Map") == "false")
    check("C1-1: the map's own plan still shown", hidden("Text_Garage") == "false")
    drawn = open_map("map_models_c11")
    check("C1-1: the map draws its icons", drawn.startswith("floor ") and "Icon_Hero" in drawn, drawn.splitlines()[:2])
    lua('return h2.dev_level("revert")')
    check("C1-1: reverted: hidden", hidden(copy + "_Map") == "true")

    # a map without buildings (the launcher's New level), loaded twice (the floor is made the first time)
    load()
    lua('return h2.dev_level("newblank", "%s")' % LEVELS[1])
    load(LEVELS[1])
    lua('return h2.dev_level("save")')
    floor_id = re.search(r"floor=(\S+)", lua('return h2.dev_level("info")'))
    floor_id = floor_id.group(1) if floor_id else "-"
    check("blank: a floor placed", floor_id != "-", floor_id)
    mark = size()
    load(LEVELS[1])
    log = since(mark)
    check("blank: no exception", "EXCEPTION" not in log)
    check("blank: the map's legend stays", hidden("Icon_Hero") == "false", hidden("Icon_Hero"))
    check("blank: the map's frame stays", hidden("Frame_Outer") == "false", hidden("Frame_Outer"))
    check("blank: the floor plans taken out", hidden("Text_Garage") == "true", hidden("Text_Garage"))
    check("blank: the floors' fixed icons taken out", hidden("MapIcon_Exit") == "true", hidden("MapIcon_Exit"))
    check("blank: the floors stay", hidden("MAP_Basement") == "false")  # (the floor's group: the first of that name)
    floor = on_floor(floor_id + "_Map")
    check("blank: the level's floor drawn on a floor of the map", floor != "", floor)
    drawn = open_map("map_models_blank")
    check("blank: the map shows a floor", drawn.startswith("floor ") and drawn != "floor \n", drawn.splitlines()[:1])
    check("blank: Hitman's icon drawn", "Icon_Hero" in drawn, drawn.splitlines()[:3])
    check("blank: no fixed icon of the old plans", "MapIcon_" not in drawn, [l for l in drawn.splitlines() if "MapIcon_" in l][:2])
finally:
    dev.run('return h2.dev_level("revert")', 10)
    for level in LEVELS:
        shutil.rmtree(os.path.join(dev.GAME, "Mods", level), ignore_errors=True)
print("\n%d passed, %d failed" % (sum(results), len(results) - sum(results)))
