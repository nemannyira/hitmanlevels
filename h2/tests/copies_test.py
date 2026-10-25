"""The level's copies of the map's models made with the map (engine/inject.h
SceneCopy): a copy made while editing is a clone at first; the level saved and
the map loaded again, it is a record of the scene at the root (its name, its
place), the game offers its action on it (a "take" goal: kind 6), Hitman takes
it, a saved game keeps it so and loads; deleted it is hidden away, undone it is
back. Uses Mods/_CopyTest and save slot 9 (removed / overwritten: back up the
saves first); a running game with dev commands.

  python h2/tests/copies_test.py
"""
import os
import shutil
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "tools"))
import dev  # noqa: E402

LEVEL = "_CopyTest"
FOLDER = os.path.join(dev.GAME, "Mods", LEVEL)
LOG = os.path.join(dev.GAME, "H2Fix.log")
SLOT = 9
SOURCE = "Tree_02_21"  # C1-1's, near Hitman's start
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


if os.path.isdir(FOLDER):
    shutil.rmtree(FOLDER)
try:
    lua('return h2.dev_level("new", "%s")' % LEVEL)
    load()
    src = lua('return h2.find("%s"):path()' % SOURCE)
    sx, sy, sz = place(src)
    at = [sx + 350.0, sy, sz + 150.0]
    copy = lua('return h2.dev_edit("dup", [[%s]], %f, %f, %f)' % (src, *at))
    name = copy.rsplit("/", 1)[-1]
    check("copy made while editing: a clone", record(copy)[1] == "none", " ".join(record(copy)))
    check("take goal on the copy", "Hitman takes" in lua('return h2.dev_level("take", [[%s]])' % copy))
    lua('return h2.dev_level("save")')
    with open(os.path.join(FOLDER, "level.json"), encoding="utf-8") as f:
        saved = f.read()
    check("level.json keeps the copy's record", '"record"' in saved)
    mark = log_size()
    load(LEVEL)
    made = log_since(mark)
    check("made with the map", "scene copies: 1 models" in made,
          [line for line in made.splitlines() if "scene copies" in line][-1:] or "no line")
    copy = lua('return h2.find("%s"):path()' % name)
    rec = record(copy)
    check("the copy is a record of the scene (at the root)", rec[1] == "copy" and copy == "ROOT/" + name,
          "%s %s" % (copy, " ".join(rec)))
    check("at its place", near(place(copy), at), str(place(copy)))
    check("shown", lua('return tostring(h2.find([[%s]]):hidden())' % copy) == "false")
    check("drawn with the rooms around it", lua('local f = h2.find([[%s]]):flags() return tostring(f & 0x4000 ~= 0)'
                                               % copy) == "true")
    action = lua('return h2.dev_routine([[%s]], "Action")' % copy)
    check("the game's action on it (kind 6)", "+72 00000006" in action, action[:120])
    lua('GOALS = {} if not GOALS_WATCHED then GOALS_WATCHED = true '
        'h2.on("objective", function(slot) GOALS[#GOALS + 1] = slot end) end h2.god(true) h2.invisible(true) return 1')
    lua('local t = h2.find([[%s]]) local tx, ty, tz = t:pos() local p = h2.player() local x, y, z = p:pos() '
        'local dx, dz = tx - x, tz - z local d = math.sqrt(dx * dx + dz * dz) '
        'p:setpos(tx - dx / d * 120, y, tz - dz / d * 120) p:faceto(tx, ty, tz) return 1' % copy)
    time.sleep(1.5)
    lua("h2.dev_key(%d, 0.3) return 1" % DIK_E)
    time.sleep(1.5)
    check("taken with the action: objective done", lua("return #GOALS") != "0")
    gone = lua('local t = h2.find([[%s]]) local _, ty = t:pos() return tostring(t:hidden()) .. " " .. math.floor(ty)' % copy)
    check("taken: out of the world", gone.startswith("true"), gone)
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
    gone = lua('local t = h2.find("%s") return tostring(t ~= nil and t:hidden())' % name)
    check("loaded game: the copy still taken", gone == "true", gone)
    # delete and undo (the map loaded again with the level)
    load(LEVEL)
    copy = lua('return h2.find("%s"):path()' % name)
    check("delete", lua('return tostring(h2.dev_edit("delete", [[%s]]))' % copy) == "true")
    gone = lua('local t = h2.find("%s") return tostring(t ~= nil and t:hidden())' % name)
    check("deleted: hidden away (the map's own object)", gone == "true", gone)
    lua('return h2.dev_level("undo")')
    time.sleep(1)
    back = lua('local t = h2.find("%s") return tostring(t ~= nil and not t:hidden())' % name)
    check("undone: back", back == "true" and near(place(copy), at), "%s %s" % (back, place(copy)))
finally:
    dev.run('return h2.dev_level("revert")', 10)
    shutil.rmtree(FOLDER, ignore_errors=True)
print("\n%d passed, %d failed" % (sum(results), len(results) - sum(results)))
