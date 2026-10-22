"""Made with the map, third round (2026-10-20): a level's settings of one of the
map's characters (npc_edits) go into its record's script block while the map
loads, so the game makes the character with them; a level's point is a record
of the scene; a character moved by the editor goes with its own move (it
stays where it was put). A saved game keeps them and loads. Uses
Mods/_GameWay3Test and save slot 9 (removed / overwritten: back up the saves
first); a running game with dev commands.

  python tools/tests/gameway3_test.py
"""
import json
import os
import shutil
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, ".."))
import dev  # noqa: E402

LEVEL = "_GameWay3Test"
FOLDER = os.path.join(dev.GAME, "Mods", LEVEL)
LOG = os.path.join(dev.GAME, "H2Fix.log")
SLOT = 9
GUARD = 2350  # C1-1's BodyguardMafia_Bedroom_00 (BaseGuard, WeaponTemplate GunBeretta92)
POINT = (-4076.0, -20.0, -6282.0)
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


GUARD_LUA = 'local g for _, n in ipairs(h2.npcs()) do if n.record == %d then g = n.object end end ' % GUARD

if os.path.isdir(FOLDER):
    shutil.rmtree(FOLDER)
try:
    load()
    deagle = lua('return h2.find("GunDesertEagle"):path()')
    beretta = lua(GUARD_LUA + 'return h2.dev_setting(g:path(), "WeaponTemplate")')
    check("the map's own setting", beretta.endswith("/GunBeretta92"), beretta)
    lua('return h2.dev_level("new", "%s")' % LEVEL)
    point = lua('return h2.dev_edit("point", %f, %f, %f)' % POINT)
    lua('return h2.dev_level("save")')
    doc = json.load(open(os.path.join(FOLDER, "level.json"), encoding="utf-8"))
    doc["npc_edits"] = [{"record": GUARD, "path": "", "behaviour": "BaseGuard",
                         "params": [{"name": "WeaponTemplate", "type": "ZGEOMREF", "value": deagle}]}]
    json.dump(doc, open(os.path.join(FOLDER, "level.json"), "w", encoding="utf-8"), indent=1)
    mark = size()
    load(LEVEL)
    log = since(mark)
    check("settings made with the map", "scene scripts: settings of 1 characters" in log)
    setting = lua(GUARD_LUA + 'return h2.dev_setting(g:path(), "WeaponTemplate")')
    check("the game made the character with them", setting.endswith("/GunDesertEagle"), setting)
    written = [l for l in log.split("scene scripts: settings")[-1].splitlines() if "settings of" in l and "written" in l]
    check("not written into the running script after the load", not written, written[:1])
    name = point.rsplit("/", 1)[-1]
    rec = lua('local r, k = h2.dev_record(h2.find("%s"):path()) return k' % name)
    check("the level's point: a record of the scene", rec == "copy", rec)
    check("the point found by its path from before", lua('return tostring(h2.find([[%s]]) ~= nil)' % point) == "true")
    # a character moved by the editor: with its own move, stays there and walks on from there
    lua(GUARD_LUA + 'h2.dev_edit("move", g:path(), -4034, 7, -6309) return 1')
    time.sleep(3)
    pos = [float(v) for v in lua(GUARD_LUA + 'local x, y, z = g:pos() return x .. " " .. y .. " " .. z').split()]
    check("a character moved: where it was put (within its walk)", abs(pos[0] + 4034) < 400 and abs(pos[2] + 6309) < 400, str(pos))
    save_file = os.path.join(dev.GAME, "Save", "%d.1" % SLOT)
    before = os.path.getmtime(save_file) if os.path.exists(save_file) else 0
    mark = size()
    lua("return tostring(h2.dev_savegame(%d))" % SLOT)
    time.sleep(3)
    check("saved game", os.path.exists(save_file) and os.path.getmtime(save_file) > before)
    audit = [l.strip() for l in since(mark).splitlines() if "save audit: slot %d:" % SLOT in l]
    check("save audit: every pointer placed", audit and audit[-1].endswith("unplaced 0"), audit[-1] if audit else "no line")
    mark = size()
    lua("return tostring(h2.dev_loadgame(%d))" % SLOT)
    wait_map()
    check("loaded game: no exception", not any("EXCEPTION" in l for l in since(mark).splitlines()))
    setting = lua(GUARD_LUA + 'return h2.dev_setting(g:path(), "WeaponTemplate")')
    check("loaded game: the setting kept", setting.endswith("/GunDesertEagle"), setting)
    check("loaded game: the point there", lua('return tostring(h2.find("%s") ~= nil)' % name) == "true")
finally:
    dev.run('return h2.dev_level("revert")', 10)
    shutil.rmtree(FOLDER, ignore_errors=True)
print("\n%d passed, %d failed" % (sum(results), len(results) - sum(results)))
