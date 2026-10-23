"""Icons of a level on the in-game map (engine/inject.h SceneCopy::mapIcon,
SceneNpc::mapIcon, SceneMapIcon): on C1-1 a point near Hitman's start gets
"Exit"; the level saved and the map loaded again, its icon record
("<point>_MapIcon", a copy of one of the map's own icon records) is made with
the map, and the map opened with M draws it on the floor it shows (Outside:
Hitman stands there), with the map's own icons; a saved game keeps it. A
point without an icon gets none, unless it is the mission's exit (Exit); the
mission's targets, one of the map's guards and a character the level places,
are marked as targets (their groups' map routines). Uses Mods/_MapIconTest
and save slot 9 (removed / overwritten: back up the saves first); a running
game with dev commands.

  python tools/tests/map_icon_test.py
"""
import os
import shutil
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, ".."))
import dev  # noqa: E402

LEVEL = "_MapIconTest"
FOLDER = os.path.join(dev.GAME, "Mods", LEVEL)
LOG = os.path.join(dev.GAME, "H2Fix.log")
SLOT = 9
DIK_M = 0x32
GUARD = 2350  # C1-1's BodyguardMafia_Bedroom_00
GUARD_LUA = 'local g for _, n in ipairs(h2.npcs()) do if n.record == %d then g = n.object end end ' % GUARD
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


def map_icons():
    """the map opened with M, what it draws, closed again"""
    lua("h2.dev_key(%d, 0.1) return 1" % DIK_M)
    time.sleep(2.5)
    out = lua("return h2.dev_mapicons()")
    lua("h2.dev_key(%d, 0.1) return 1" % DIK_M)
    time.sleep(1.5)
    return out


if os.path.isdir(FOLDER):
    shutil.rmtree(FOLDER)
try:
    load()
    before = map_icons()
    check("the map's own: a floor and icons drawn", before.startswith("floor ") and len(before.splitlines()) > 1,
          before.splitlines()[:3])
    lua('return h2.dev_level("new", "%s")' % LEVEL)
    at = lua('local x, y, z = h2.player():pos() return string.format("%.0f, %.0f, %.0f", x + 300, y, z)')
    point = lua('return h2.dev_edit("point", %s)' % at)
    other = lua('return h2.dev_edit("point", %s)' % at)  # (no icon)
    exit_point = lua('return h2.dev_edit("point", %s)' % at)  # (no icon of its own: the mission's exit)
    lua('return h2.dev_level("mapicon", [[%s|Icon_Exit]])' % point)
    lua('return h2.dev_level("exit", [[%s]])' % exit_point)
    guard = lua(GUARD_LUA + 'return g:path()')
    lua('return h2.dev_level("target", [[%s]])' % guard)
    placed = lua('return h2.dev_place_npc(%d, %s)' % (GUARD, at))
    lua('return h2.dev_level("target", h2.find("%s"):path())' % placed)
    try:
        lua('return h2.dev_level("mapicon", [[%s|Icon_Nonsense]])' % other)
        check("an unknown icon refused", False)
    except RuntimeError as e:
        check("an unknown icon refused", "no map icon" in str(e), str(e)[:80])
    lua('return h2.dev_level("save")')
    saved = open(os.path.join(FOLDER, "level.json"), encoding="utf-8").read()
    check("level.json keeps the icon", '"map_icon": "Icon_Exit"' in saved)
    name = point.rsplit("/", 1)[-1]
    other_name = other.rsplit("/", 1)[-1]
    mark = size()
    load(LEVEL)
    log = since(mark)
    check("no exception", "EXCEPTION" not in log)
    check("the scene has a map icon to copy", "no map icon record" not in log)
    rec = lua('local o = h2.find("%s_MapIcon") if not o then return "missing" end '
              'local r, k = h2.dev_record(o:path()) return k' % name)
    check("the icon made with the map", rec == "copy", rec)
    check("none for the point without an icon",
          lua('return tostring(h2.find("%s_MapIcon") == nil)' % other_name) == "true")
    exit_icon = lua('return h2.dev_mapicon("%s_MapIcon")' % exit_point.rsplit("/", 1)[-1])
    check("the mission's exit: Exit", exit_icon == "Icon_Exit shown", exit_icon)
    target = lua(GUARD_LUA + 'return h2.dev_mapicon(g:path())')
    check("the map's guard to kill: Target", target == "Icon_Target shown", target)
    target = lua('return h2.dev_mapicon("%s")' % placed)
    check("the placed character to kill: Target", target == "Icon_Target shown", target)
    other_guard = lua('for _, n in ipairs(h2.npcs()) do if n.record ~= %d and n.object then '
                      'local i = h2.dev_mapicon(n.object:path()) '
                      'if i ~= "none" and i:sub(1, 11) ~= "Icon_Target" then return i end end end return "none"' % GUARD)
    check("the others keep their own (not all targets)", other_guard != "none", other_guard)
    routine = lua('return h2.dev_routine(h2.find("%s_MapIcon"):path(), "MapObject")' % name)
    check("its ZGEOM_MapObject routine", routine != "none", routine[:60])
    drawn = map_icons()
    check("the map shows the Outside floor", drawn.startswith("floor C1-1/IngameMap/Outside"), drawn.splitlines()[:1])
    mine = [l for l in drawn.splitlines() if l.endswith("/" + name + "_MapIcon")]
    check("the map draws the point's icon", mine and mine[0].startswith("Icon_Exit "), mine or drawn[-200:])
    own = len(before.splitlines())
    check("and the map's own icons still", len(drawn.splitlines()) >= own, "%d / %d lines" % (len(drawn.splitlines()), own))
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
    wait_map()
    check("loaded game: no exception", "EXCEPTION" not in since(mark))
    drawn = map_icons()
    check("loaded game: the icon on the map", any(l.endswith("/" + name + "_MapIcon") for l in drawn.splitlines()),
          drawn.splitlines()[:2])
finally:
    dev.run('return h2.dev_level("revert")', 10)
    shutil.rmtree(FOLDER, ignore_errors=True)
print("\n%d passed, %d failed" % (sum(results), len(results) - sum(results)))
