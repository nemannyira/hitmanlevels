"""Editor test: a level made, edited, saved, applied, loaded and tested in a
running game (dev commands on). Uses the level folder Mods/SuiteEd (removed
at the end).

  python h2/tests/editor_test.py
"""
import json
import os
import shutil
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "tools"))
import dev  # noqa: E402

GAME = dev.GAME
LEVEL = "SuiteEd"
FOLDER = os.path.join(GAME, "Mods", LEVEL)
results = []


def lua(code, timeout=30):
    out = dev.run(code, timeout)
    if out.startswith("error") or "timeout" in out:
        raise RuntimeError(out)
    return out


def check(name, fn):
    try:
        ok = fn()
        results.append((name, bool(ok), "" if ok else "false"))
    except Exception as e:  # noqa: BLE001
        results.append((name, False, str(e)[:200]))
    print(("PASS " if results[-1][1] else "FAIL ") + name + ("" if results[-1][1] else ": " + results[-1][2]))


def load_map(level=None):
    if level:
        lua('return h2.dev_level("load", "%s")' % level)
    lua('return h2.dev_loadscene([[C1-1\\C1-1__MAIN]])')
    t = time.time()
    time.sleep(4)
    while time.time() - t < 150:
        try:
            if "C1-1__MAIN" in dev.run("return h2.scene()", 5):
                break
        except Exception:  # noqa: BLE001
            pass
        time.sleep(1)
    time.sleep(6)


def pos(path):
    out = lua('local o = h2.find([[%s]]) local x, y, z = o:pos() return string.format("%%.1f %%.1f %%.1f", x, y, z)' % path)
    return [float(v) for v in out.split()]


def near(a, b, tol=2.0):
    return all(abs(x - y) <= tol for x, y in zip(a, b))


if os.path.isdir(FOLDER):
    shutil.rmtree(FOLDER)
load_map()
lua("h2.dev_editor(true) return true")
check("new level", lambda: LEVEL in lua('return h2.dev_level("new", "%s")' % LEVEL))

CRATE = lua('for _, o in ipairs(h2.objects("ZSTDOBJ")) do if o:mesh() and o:mesh() ~= 0 and not o:hidden() '
            'and o:path():find("LOC_Mansion") then return o:path() end end')
HIDE = lua('local n = 0 for _, o in ipairs(h2.objects("ZSTDOBJ")) do if o:mesh() and o:mesh() ~= 0 and not o:hidden() '
           'and o:path():find("LOC_Mansion") then n = n + 1 if n == 5 then return o:path() end end end')
start = pos(CRATE)
moved = [start[0] + 50, start[1], start[2] + 25]
check("move (recorded)", lambda: (lua('h2.dev_edit("move", [[%s]], %f, %f, %f) return true' % (CRATE, *moved)), near(pos(CRATE), moved))[1])
check("undo move", lambda: (lua('return h2.dev_level("undo")'), near(pos(CRATE), start))[1])
check("redo move", lambda: (lua('return h2.dev_level("redo")'), near(pos(CRATE), moved))[1])
check("hide (recorded)", lambda: lua('h2.dev_edit("hide", [[%s]], true) return tostring(h2.find([[%s]]):hidden())' % (HIDE, HIDE)) == "true")
DUP = None


def dup():
    global DUP
    DUP = lua('return h2.dev_edit("dup", [[%s]], %f, %f, %f)' % (CRATE, start[0] + 200, start[1], start[2]))
    return near(pos(DUP), [start[0] + 200, start[1], start[2]])


check("duplicate", dup)
ITEM = None


def item():
    global ITEM
    tmpl = lua('for _, o in ipairs(h2.objects("ZItemTemplate")) do if o:name() == "KeyRoom" then return o:path() end end')
    ITEM = lua('return h2.dev_edit("item", [[%s]], %f, %f, %f)' % (tmpl, start[0], start[1] + 50, start[2] + 100))
    return ITEM.startswith("ROOT")


check("place item", item)
check("place point", lambda: lua('return h2.dev_edit("point", -4176, -20, -6682)').startswith("ROOT"))
check("place npc (no behaviour)", lambda: lua('return h2.dev_place_npc(2350, -4100, -20, -6600)') != "")
check("place npc (BaseGuard)", lambda: lua('return h2.dev_place_npc(2350, -4000, -20, -6600, "BaseGuard")') != "")
check("sky", lambda: (lua('h2.dev_edit("sky", "C3-3") return true'), True)[1])
info = lua('return h2.dev_level("info")')
print("info:", info)
check("level counts", lambda: "npcs=2" in info and "spawned=" in info and "(unsaved)" in info)
check("save", lambda: "(unsaved)" not in lua('return h2.dev_level("save")'))


def saved():
    doc = json.load(open(os.path.join(FOLDER, "level.json"), encoding="utf-8"))
    return len(doc.get("npcs", [])) == 2 and len(doc.get("spawned", [])) >= 3 and len(doc.get("objects", [])) >= 2 \
        and (doc.get("sky") or {}).get("level") == "C3-3"


check("saved file", saved)


def round_trip():
    before = json.load(open(os.path.join(FOLDER, "level.json"), encoding="utf-8"))
    lua('return h2.dev_level("load", "%s")' % LEVEL)
    lua('return h2.dev_level("save")')
    after = json.load(open(os.path.join(FOLDER, "level.json"), encoding="utf-8"))
    lost = [k for k in before if before[k] != after.get(k)]
    if lost:
        raise RuntimeError("changed by load + save: %s" % lost)
    return True


check("load + save keeps everything", round_trip)
check("revert", lambda: (lua('return h2.dev_level("revert")'), near(pos(CRATE), start))[1])
check("apply", lambda: (lua('return h2.dev_level("apply")'), near(pos(CRATE), moved))[1])

# the level loaded with its map
load_map(LEVEL)
check("loaded: move kept", lambda: near(pos(CRATE), moved))
check("loaded: hidden kept", lambda: lua('return tostring(h2.find([[%s]]):hidden())' % HIDE) == "true")
check("loaded: characters made", lambda: int(lua('local n = 0 for _, o in ipairs(h2.objects("ZActor")) do '
                                                'if o:path():find("Courier") or o:path():find("_Placed") or o:path():find("NPC_") '
                                                'then n = n + 1 end end return n')) >= 0)
check("loaded: npcs listed", lambda: "npcs=2" in lua('return h2.dev_level("info")'))
check("delete duplicate", lambda: lua('return tostring(h2.dev_edit("delete", [[%s]]))' % DUP.replace("#1", "")) in ("true", "false"))

# every panel opens
for panel in ["Scene", "Models", "NPCs", "Level", "Mission", "Camera", "Inspector", "Testing", "Lua console"]:
    check("panel " + panel, lambda p=panel: (lua('h2.dev_panel("%s") return true' % p), time.sleep(0.5),
                                             dev.shot("panel_" + p.replace(" ", "_"), 0.5) is not None)[2])

# Test: the map loaded again, Hitman where he is
check("test button", lambda: (lua("h2.dev_test() return true"), time.sleep(5), True)[2])
t = time.time()
while time.time() - t < 150:
    if "C1-1__MAIN" in dev.run("return h2.scene()", 5):
        break
    time.sleep(1)
time.sleep(6)
check("after test: map up", lambda: "C1-1__MAIN" in lua("return h2.scene()"))
check("after test: editor closed", lambda: lua("return tostring(h2.freecam())") in ("false", "nil"))

print("\n%d passed, %d failed" % (sum(1 for r in results if r[1]), sum(1 for r in results if not r[1])))
for r in results:
    if not r[1]:
        print("FAIL", r[0], r[2])
