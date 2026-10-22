"""The level's copies of a group of the map's models (a car model) and of a
model of another level, made with the map (engine/inject.h SceneCopy parts /
mesh): made while editing at first, then, the level saved and the map loaded
again, records of the scene at the root (the group with its models in it, the
other level's model as a model of the map) at their places; a saved game
loads; the group deleted is hidden away and undone back. Uses Mods/_Copy2Test
and save slot 9 (removed / overwritten: back up the saves first); a running
game with dev commands.

  python tools/tests/copies2_test.py
"""
import json
import os
import shutil
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, ".."))
import dev  # noqa: E402

LEVEL = "_Copy2Test"
FOLDER = os.path.join(dev.GAME, "Mods", LEVEL)
LOG = os.path.join(dev.GAME, "H2Fix.log")
SLOT = 9
IMPORT = {"level": "C3-3", "mesh": 6341746, "name": "Yard_06-7-8"}  # the blank maps' floor
LIBRARY = "ROOT/EditorImports/Yard_06-7-8.C3-3.60C472"
# a model whose name has a '#' of its own (its copies' names too)
IMPORT2 = {"level": "C2-1", "mesh": 3399678, "name": "Truck_Body#0"}
LIBRARY2 = "ROOT/EditorImports/Truck_Body#0.C2-1.33DFFE"
GROUP = "CarFerrari"  # C1-1's garage car: a group of models
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
    while time.time() - t < 200:
        if "C1-1__MAIN" in dev.run("return h2.scene()", 5):
            break
        time.sleep(1)
    time.sleep(10)


def load(level=None):
    if level:
        lua('return h2.dev_level("load", "%s")' % level)
    lua(r'return h2.dev_loadscene([[C1-1\C1-1__MAIN]])')
    wait_map()


def log_size():
    return os.path.getsize(LOG) if os.path.exists(LOG) else 0


def log_since(at):
    with open(LOG, encoding="latin1") as f:
        f.seek(at)
        return f.read()


def record(path):
    return lua('local r, k = h2.dev_record([[%s]]) return r .. " " .. k' % path).split()


def place(path):
    return [float(v) for v in lua('local x, y, z = h2.find([[%s]]):pos() return x .. " " .. y .. " " .. z' % path).split()]


def near(a, b, tol=2.0):
    return all(abs(x - y) <= tol for x, y in zip(a, b))


def parts(path):
    return int(lua('local n = 0 local function walk(o) for _, c in ipairs(o:children()) do '
                   'if c:mesh() and c:mesh() ~= 0 then n = n + 1 end walk(c) end end walk(h2.find([[%s]])) return n'
                   % path))


if os.path.isdir(FOLDER):
    shutil.rmtree(FOLDER)
try:
    os.makedirs(FOLDER)
    json.dump({"format": "h2level", "version": 1, "name": LEVEL, "base_scene": "C1-1\\C1-1__MAIN",
               "imports": [IMPORT, IMPORT2]}, open(os.path.join(FOLDER, "level.json"), "w", encoding="utf-8"))
    load(LEVEL)
    px, py, pz = place(lua("return h2.player():path()"))
    car = lua('return h2.find("%s"):path()' % GROUP)
    car_parts = parts(car)
    car_at = [px + 600.0, py, pz + 300.0]
    car_copy = lua('return h2.dev_edit("dup", [[%s]], %f, %f, %f)' % (car, *car_at))
    yard_at = [px + 5000.0, py - 3000.0, pz]
    yard_copy = lua('return h2.dev_edit("dup", [[%s]], %f, %f, %f)' % (LIBRARY, *yard_at))
    truck_at = [px + 900.0, py, pz - 400.0]
    truck_copy = lua('return h2.dev_edit("dup", [[%s]], %f, %f, %f)' % (LIBRARY2, *truck_at))
    truck_id = truck_copy.rsplit("/", 1)[-1]
    check("copies made while editing", record(car_copy)[1] == "none" and record(yard_copy)[1] == "none",
          "%s | %s" % (car_copy, yard_copy))
    lua('return h2.dev_level("save")')
    doc = json.load(open(os.path.join(FOLDER, "level.json"), encoding="utf-8"))
    by_id = {s["id"]: s for s in doc["spawned"]}
    car_id, yard_id = car_copy.rsplit("/", 1)[-1], yard_copy.rsplit("/", 1)[-1]
    check("level.json: the group's models", len(by_id[car_id].get("parts", [])) == car_parts,
          "%d of %d" % (len(by_id[car_id].get("parts", [])), car_parts))
    check("level.json: the other level's model", by_id[yard_id].get("import", {}).get("mesh") == IMPORT["mesh"],
          str(by_id[yard_id].get("import")))
    mark = log_size()
    load(LEVEL)
    made = log_since(mark)
    check("made with the map", "scene copies: 3 models" in made,
          [line for line in made.splitlines() if "scene copies" in line][-1:] or "no line")
    car_copy = lua('return h2.find("%s"):path()' % car_id)
    yard_copy = lua('return h2.find("%s"):path()' % yard_id)
    check("the group copy: a record of the scene", record(car_copy)[1] == "copy" and car_copy == "ROOT/" + car_id,
          "%s %s" % (car_copy, " ".join(record(car_copy))))
    check("the group copy: its models", parts(car_copy) == car_parts, "%d of %d" % (parts(car_copy), car_parts))
    check("the group copy: at its place", near(place(car_copy), car_at), str(place(car_copy)))
    check("the other level's model: a record of the scene",
          record(yard_copy)[1] == "copy" and yard_copy == "ROOT/" + yard_id, "%s %s" % (yard_copy, " ".join(record(yard_copy))))
    check("the other level's model: its model",
          lua('return tostring(h2.find([[%s]]):mesh() == h2.find([[%s]]):mesh())' % (yard_copy, LIBRARY)) == "true")
    check("the other level's model: at its place", near(place(yard_copy), yard_at), str(place(yard_copy)))
    found = lua('local a = h2.find("%s") local b = h2.find([[%s]]) return tostring(a and a:path()) .. " | " .. tostring(b and b:path())' % (truck_id, truck_copy))
    check("a name with its own '#': found by its name and by its path while editing", found.count("ROOT/" + truck_id) == 2, found)
    save_file = os.path.join(dev.GAME, "Save", "%d.1" % SLOT)
    before = os.path.getmtime(save_file) if os.path.exists(save_file) else 0
    mark = log_size()
    lua("return tostring(h2.dev_savegame(%d))" % SLOT)
    time.sleep(3)
    check("saved game", os.path.exists(save_file) and os.path.getmtime(save_file) > before)
    audit = [line.strip() for line in log_since(mark).splitlines() if "save audit: slot %d:" % SLOT in line]
    check("save audit: every pointer placed", audit and audit[-1].endswith("unplaced 0"), audit[-1] if audit else "no line")
    mark = log_size()
    lua("return tostring(h2.dev_loadgame(%d))" % SLOT)
    wait_map()
    check("loaded game: no exception", not any("EXCEPTION" in line and "hitman2.exe" in line
                                               for line in log_since(mark).splitlines()))
    check("loaded game: the copies there", lua('return tostring(h2.find("%s") ~= nil and h2.find("%s") ~= nil)'
                                               % (car_id, yard_id)) == "true")
    load(LEVEL)
    car_copy = lua('return h2.find("%s"):path()' % car_id)
    check("delete the group copy", lua('return tostring(h2.dev_edit("delete", [[%s]]))' % car_copy) == "true")
    check("deleted: hidden away", lua('return tostring(h2.find("%s"):hidden())' % car_id) == "true")
    lua('return h2.dev_level("undo")')
    time.sleep(1)
    check("undone: back", lua('return tostring(h2.find("%s"):hidden())' % car_id) == "false" and
          near(place(car_copy), car_at), str(place(car_copy)))
finally:
    dev.run('return h2.dev_level("revert")', 10)
    shutil.rmtree(FOLDER, ignore_errors=True)
print("\n%d passed, %d failed" % (sum(results), len(results) - sum(results)))
