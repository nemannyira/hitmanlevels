"""Big levels on C1-1 against the engine's limits (docs/roadmap.md, Engine limits a level meets):
1999 copies of a tree made with the map (and one by Duplicate), all of them moving and drawn (the room lists' pool made with
4096 entries instead of 510; the game lives; a saved game keeps them; the level reverted at once), 2000
points with map icons (cut to 256; the map opens and draws at most 256 on a floor). Uses Mods/_LimitsTest
and save slot 9 (back up the saves first: run_all does); a running game with dev commands (C1-1 loaded).

  python tools/tests/limits_test.py
"""
import json
import os
import shutil
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, ".."))
import dev  # noqa: E402

LEVEL = "_LimitsTest"
FOLDER = os.path.join(dev.GAME, "Mods", LEVEL)
LOG = os.path.join(dev.GAME, "H2Fix.log")
SOURCE = "Tree_02_21"
DIK_M = 0x32
M = [0, 0, 1, 0, 1, 0, 1, 0, 0]
results = []


def check(name, ok, detail=""):
    results.append(bool(ok))
    print("%s %s %s" % ("PASS" if ok else "FAIL", name, detail))


def lua(code, timeout=30):
    out = dev.run(code, timeout)
    if out.startswith("error") or "timeout" in out:
        raise RuntimeError(out)
    return out


def alive():
    return dev.running() and "timeout" not in dev.run("return 1", 10)


def log_size():
    return os.path.getsize(LOG)


def log_since(at):
    with open(LOG, encoding="latin1") as f:
        f.seek(at)
        return f.read()


def load_from_menu(doc):
    """the level written, loaded from the main menu (made with the map's next load) -> log since"""
    shutil.rmtree(FOLDER, ignore_errors=True)
    os.makedirs(FOLDER)
    with open(os.path.join(FOLDER, "level.json"), "w", encoding="utf-8") as f:
        json.dump(doc, f)
    lua('return h2.dev_level("revert")')
    lua('return h2.dev_loadscene("MainMenu")')
    dev.wait_ready("MainMenu", 60)
    mark = log_size()
    lua('return h2.dev_level("load", "%s")' % LEVEL, 60)
    lua(r'return h2.dev_loadscene([[C1-1\C1-1__MAIN]])')
    dev.wait_ready("C1-1__MAIN", 200)
    return mark


def lines(text, *words):
    return [l.strip()[18:] for l in text.splitlines() if any(w in l for w in words)]


try:
    # a copy of the tree, as the editor saves one
    lua('return h2.dev_level("new", "%s")' % LEVEL)
    src = lua('return h2.find("%s"):path()' % SOURCE)
    x, y, z = [float(v) for v in lua('local x, y, z = h2.find([[%s]]):pos() return x .. " " .. y .. " " .. z' % src).split()]
    lua('return h2.dev_edit("dup", [[%s]], %f, %f, %f)' % (src, x + 350, y, z + 150))
    lua('return h2.dev_level("save")')
    with open(os.path.join(FOLDER, "level.json"), encoding="utf-8") as f:
        doc = json.load(f)
    one = doc["spawned"][0]

    # 2000 copies: as many as a level may hold, each a moving object
    copies = []
    for i in range(1999):  # (and one more by Duplicate: as many as a level may hold)
        s = dict(one, id="%s_%d" % (one["id"], i))
        s["pos"] = [one["pos"][0] + (i % 50) * 300, one["pos"][1], one["pos"][2] + (i // 50) * 300]
        copies.append(s)
    mark = load_from_menu(dict(doc, spawned=copies))
    out = log_since(mark)
    check("1999 copies: the game lives", alive())
    check("  the room lists' pool of 4096", "4096 room lists" in open(LOG, encoding="latin1").read())
    check("  made with the map", "scene copies: 1999 models" in out, lines(out, "scene copies")[-1:])
    check("  all of them moving and drawn", "not drawn" not in out, lines(out, "not drawn")[-1:])
    free, total = (int(v) for v in lua("return h2.dev_roomlists()").split())
    check("  room lists: 1999 and the scene's in use", total == 4096 and free < 4096 - 1999, "%d of %d free" % (free, total))
    out = dev.run('return h2.dev_edit("dup", [[%s]], %f, %f, %f)' % (src, x + 350, y, z - 150), 30)
    check("  Duplicate still makes moving copies", "+" in out and not out.startswith("error"), out[-60:])
    lua("return tostring(h2.dev_savegame(9))")
    time.sleep(3)
    lua("return tostring(h2.dev_loadgame(9))")
    loaded = dev.wait_ready("C1-1__MAIN", 200)
    check("  a saved game with them loads, the game lives", loaded and alive())
    if loaded:
        free2, _ = (int(v) for v in lua("return h2.dev_roomlists()").split())
        check("  the saved game's room lists as before", abs(free2 - free) < 64, "%d free (was %d)" % (free2, free))
    t = time.time()
    rev = dev.run('return h2.dev_level("revert")', 120)
    check("  reverted at once, the game lives", "timeout" not in rev and time.time() - t < 30 and alive(),
          "%.1f s" % (time.time() - t))
    check("  no loop took a snapshot of the scene for each object", "32 snapshots of the scene in one frame" not in log_since(mark))

    # 2000 points with icons
    points = [{"id": "P%d" % i, "kind": "point", "parent": "ROOT", "matrix": M, "hidden": False,
               "pos": [-4076.0 + (i % 50) * 20, -20.0, -6282.0 + (i // 50) * 20], "source_scene": "C1-1\\C1-1__MAIN",
               "source_path": "", "map_icon": "Icon_Exit"} for i in range(2000)]
    mark = load_from_menu(dict(doc, spawned=points))
    out = log_since(mark)
    check("2000 points with icons: the game lives", alive())
    check("  icons past 256 left out", "map icons beyond 256 left out" in out, lines(out, "map icons beyond")[-1:])
    time.sleep(20)  # (the map opens once the level's start is over)
    drawn = ""
    for attempt in range(4):
        lua("h2.dev_key(%d, 0.1) return 1" % DIK_M)
        time.sleep(3)
        drawn = dev.run("return h2.dev_mapicons()", 20)
        lua("h2.dev_key(%d, 0.1) return 1" % DIK_M)
        time.sleep(2)
        if drawn.startswith("floor"):
            break
        time.sleep(5)
    n = sum(1 for l in drawn.splitlines() if l.startswith("Icon_"))
    check("  the map opens, at most 256 icons on the floor", drawn.startswith("floor") and 0 < n <= 256, "%d drawn" % n)
    check("  the game lives after", alive())
    problems = dev.run('return h2.dev_level("problems")', 30)
    check("  the Problems list says so", "more than 256 icons on a floor" in problems, problems[:120])
    lua('return h2.dev_level("revert")', 120)
finally:
    if dev.running():
        dev.run('return h2.dev_level("revert")', 60)
    shutil.rmtree(FOLDER, ignore_errors=True)

print("\n%d passed, %d failed" % (results.count(True), results.count(False)))
sys.exit(0 if all(results) else 1)
