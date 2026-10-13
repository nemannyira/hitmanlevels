"""Mission goals of placed objects, played: on a blank map (no buildings, the
C3-3 yard to stand on) a level gets a key (an item), a copy of one of the map's
own models and a point; "Hitman takes it" goals for the first two, "Hitman
gets here" for the point, renamed. The map is loaded with the level and the
goals are done with the game's own keys (E near the objects); a saved game
keeps them. Checks also that a copy of a model the blank map took out is
drawn (P17). Uses Mods/_GoalsTest and save slot 9 (removed / overwritten: back
up the saves first); a running game with dev commands.

  python tools/tests/goals_test.py
"""
import json
import os
import shutil
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, ".."))
import dev  # noqa: E402

LEVEL = "_GoalsTest"
FOLDER = os.path.join(dev.GAME, "Mods", LEVEL)
LOG = os.path.join(dev.GAME, "H2Fix.log")
SLOT = 9
START = (7285.0, 972.0, -7584.0)  # Hitman's start on C1-1 (the blank map's floor is put under him)
KEY_TEMPLATE = "ROOT/C1-1/C1-1_CHARACTERS.zip/AllLevels/Equipment.zip/AllLevels/Items.zip/Items/KeyRoom"
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


def watch():  # the editor's goals report themselves to scripts
    lua('GOALS = {} if not GOALS_WATCHED then GOALS_WATCHED = true '  # (console handlers outlive a map load)
        'h2.on("objective", function(slot) GOALS[#GOALS + 1] = slot end) end h2.god(true) return 1')


def done():
    return lua('table.sort(GOALS) return table.concat(GOALS, ",")')


def at(x, y, z):
    lua("h2.player():setpos(%f, %f, %f) return 1" % (x, y, z))
    time.sleep(1.5)


def press_e():
    lua("h2.dev_key(%d, 0.2) return 1" % DIK_E)
    time.sleep(1.5)


def log_size():
    return os.path.getsize(LOG) if os.path.exists(LOG) else 0


if os.path.isdir(FOLDER):
    shutil.rmtree(FOLDER)
try:
    lua('return h2.dev_level("new", "_GoalsTestBase")')  # (a level left in the editor would be put on the map)
    load()
    lua('return h2.dev_level("newblank", "%s")' % LEVEL)
    load(LEVEL)  # the floor comes with the map
    x, y, z = START
    key = lua('return h2.dev_edit("item", [[%s]], %f, %f, %f)' % (KEY_TEMPLATE, x + 200, y + 3, z))
    books = lua('return h2.dev_edit("dup", h2.findall("Books_01a_02")[1]:path(), %f, %f, %f)' % (x - 200, y + 10, z))
    point = lua('return h2.dev_edit("point", %f, %f, %f)' % (x, y, z + 300))
    check("objects placed", key.startswith("ROOT") and "+" in books and "Point+" in point, "%s | %s | %s" % (key, books, point))
    check("take: item", "Hitman takes Room key" in lua('return h2.dev_level("take", [[%s]])' % key))
    check("take: model", "Hitman takes Books_01a_02" in lua('return h2.dev_level("take", [[%s]])' % books))
    lua('return h2.dev_level("reach", [[%s]])' % point)
    renamed = lua('return h2.dev_edit("rename", [[%s]], "Hideout")' % point)
    check("point renamed", renamed.endswith("/Hideout"), renamed)
    check("no problems", lua('return h2.dev_level("problems")') == "", lua('return h2.dev_level("problems")'))
    lua('return h2.dev_level("save")')
    doc = json.load(open(os.path.join(FOLDER, "level.json"), encoding="utf-8"))
    goals = {o["slot"]: o for o in doc["mission"]["objectives"]}
    check("saved: item kept as the level's", goals[1]["target"].startswith("level:KeyRoom_item+"), goals[1]["target"])
    check("saved: goal texts", [goals[s]["text"] for s in (1, 2, 3)] ==
          ["Take Room key", "Take Books_01a_02", "Reach Hideout"], str([goals[s]["text"] for s in (1, 2, 3)]))
    check("saved: renamed target", goals[3]["target"].endswith("/Hideout"), goals[3]["target"])

    load(LEVEL)
    watch()
    check("loaded: Hitman on the floor", float(lua('local _, py = h2.player():pos() return py')) > y - 50)
    check("loaded: no problems", lua('return h2.dev_level("problems")') == "", lua('return h2.dev_level("problems")'))
    books = lua('return h2.findall("Books_01a_02+1")[1]:path()')
    # P17: the copy of a model the blank map took out is drawn
    lua('h2.freecam(true) h2.setcampos(%f, %f, %f) h2.camlookat(%f, %f, %f) return 1' % (x - 200, y + 150, z - 250, x - 200, y + 10, z))
    time.sleep(1.5)
    check("blank map: the copy is drawn", lua('return tostring(h2.dev_drawn([[%s]]))' % books) == "true")
    lua('h2.freecam(false) return 1')

    at(x + 140, y + 5, z)
    press_e()
    check("item picked up: objective 1", done() == "1", done())
    at(x - 260, y + 5, z)
    press_e()
    gone = lua('local b = h2.find([[%s]]) local _, by = b:pos() return tostring(b:hidden()) .. " " .. math.floor(by)' % books)
    check("model taken: objective 2", done() == "1,2", done())
    check("model taken: out of the world", gone.startswith("true") and int(gone.split()[1]) < -50000, gone)
    at(x + 400, y + 5, z + 400)
    press_e()
    check("E away from it takes nothing", done() == "1,2", done())
    at(x, y + 5, z + 300)
    time.sleep(1)
    check("place reached: objective 3", done() == "1,2,3", done())

    # a saved game keeps the goals done and the model taken
    save_file = os.path.join(dev.GAME, "Save", "%d.1" % SLOT)
    before = os.path.getmtime(save_file) if os.path.exists(save_file) else 0
    lua("return tostring(h2.dev_savegame(%d))" % SLOT)
    time.sleep(3)
    check("saved game", os.path.exists(save_file) and os.path.getmtime(save_file) > before)
    state = json.load(open(os.path.join(dev.GAME, "Save", "%d.h2lua" % SLOT), encoding="utf-8")).get("__editor", {})
    check("saved game: goals in its state", state.get("done") == [1, 2, 3] and len(state.get("taken", [])) == 1, str(state))
    mark = log_size()
    lua("return tostring(h2.dev_loadgame(%d))" % SLOT)
    wait_map()
    with open(LOG, "rb") as f:
        f.seek(mark)
        tail = f.read().decode("utf-8", "replace")
    check("loaded game: goals back", "goals of the saved game back" in tail and '"done":[1,2,3]' in tail)
    blanked = [int(l.split("blank map, ")[1].split()[0]) for l in tail.splitlines() if "blank map, " in l]
    check("loaded game: the buildings out again", blanked and blanked[-1] > 1000, str(blanked))
    check("loaded game: Hitman on the floor", float(lua('local _, py = h2.player():pos() return py')) > y - 50)
    gone = lua('local b = h2.find([[%s]]) local _, by = b:pos() return tostring(b:hidden()) .. " " .. math.floor(by)' % books)
    check("loaded game: model still taken", gone.startswith("true") and int(gone.split()[1]) < -50000, gone)
finally:
    dev.run('h2.freecam(false) return h2.dev_level("revert")', 10)
    shutil.rmtree(FOLDER, ignore_errors=True)
print("\n%d passed, %d failed" % (sum(results), len(results) - sum(results)))
