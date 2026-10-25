"""Saved games of a level with characters of other levels (P39): on C2-1 a
level places C1-1's maid and bodyguard and C3-2a's ninja (their bodies and
animations imported). A game saved there loads (it stopped at an int3 in
ZLoadGame::GetSafePointer: the outfit groups made for the imported bodies
keep their names in the engine's heap); the ninja's clothes are taken after
loading and the game is saved and loaded again in them. Uses Mods/_ImportSave
(removed) and save slot 9 (overwritten: back up the saves first); a running
game with dev commands.

  python h2/tests/imports_save_test.py
"""
import json
import os
import shutil
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "tools"))
import dev  # noqa: E402

LEVEL = "_ImportSave"
FOLDER = os.path.join(dev.GAME, "Mods", LEVEL)
LOG = os.path.join(dev.GAME, "H2Fix.log")
SLOT = 9
DIK_E = 0x12
MATRIX = [-1, 0, 0, 0, 1, 0, 0, 0, -1]
IMPORTS = [
    {"level": "C1-1", "mesh": 8361570, "name": "Maid", "character": True, "anim_set": "Female_Reg",
     "outfit_mesh": 0, "nude_mesh": 0},
    {"level": "C3-2a", "mesh": 5143338, "name": "SoldierNinjaSniperOutside_01", "character": True,
     "anim_set": "Reg_Male", "outfit_mesh": 5793782, "nude_mesh": 6167222},
    {"level": "C1-1", "mesh": 3527498, "name": "BodyguardMafia_01", "character": True, "anim_set": "Reg_Male",
     "outfit_mesh": 7416772, "nude_mesh": 8075670},
]
NPCS = [  # on C2-1's soldier record (5800), in a row
    {"id": "NPC_Maid_1", "z": -2671.0, "import": IMPORTS[0]},
    {"id": "NPC_Ninja_1", "z": -2586.0, "import": IMPORTS[1]},
    {"id": "NPC_Bodyguard_1", "z": -2501.0, "import": IMPORTS[2]},
]
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
    dev.wait_ready("C2-1__MAIN", 300)  # three imported bodies and their animations load slowly


def log_size():
    return os.path.getsize(LOG) if os.path.exists(LOG) else 0


def log_since(mark):
    with open(LOG, "rb") as f:
        f.seek(mark)
        return f.read().decode("utf-8", "replace")


def characters():
    return lua('local t = {} for _, n in ipairs({"NPC_Maid_1", "NPC_Ninja_1", "NPC_Bodyguard_1"}) do '
               'local o = h2.find(n) t[#t + 1] = o and tostring(o:alive()) or "-" end return table.concat(t, " ")')


def save_and_load(what):
    mark = log_size()
    save_file = os.path.join(dev.GAME, "Save", "%d.1" % SLOT)
    before = os.path.getmtime(save_file) if os.path.exists(save_file) else 0
    lua("return tostring(h2.dev_savegame(%d))" % SLOT)
    time.sleep(4)
    check("%s: saved" % what, os.path.exists(save_file) and os.path.getmtime(save_file) > before)
    check("%s: every pointer placed" % what, "a pointer of no kind" not in log_since(mark))
    lua("return tostring(h2.dev_loadgame(%d))" % SLOT)
    wait_map()
    alive = dev.run("return h2.scene()", 10)
    check("%s: loaded (the game answers)" % what, "C2-1__MAIN" in alive, alive)
    check("%s: no exception" % what, "EXCEPTION" not in log_since(mark))


shutil.rmtree(FOLDER, ignore_errors=True)
os.makedirs(FOLDER)
doc = {"format": "h2level", "version": 1, "name": LEVEL, "base_scene": "C2-1\\C2-1__MAIN", "imports": IMPORTS,
       "objects": [], "spawned": [], "npc_edits": [], "lists": [], "scripts": [],
       "npcs": [{"id": n["id"], "record": 5800, "model": "SoldierRussian_01", "pos": [2643.0, -2659.0, n["z"]],
                 "matrix": MATRIX, "import_level": n["import"]["level"], "import_mesh": n["import"]["mesh"],
                 "import_name": n["import"]["name"]} for n in NPCS]}
json.dump(doc, open(os.path.join(FOLDER, "level.json"), "w", encoding="utf-8"), indent=2)
try:
    lua('return h2.dev_level("load", "%s")' % LEVEL)
    lua(r'return h2.dev_loadscene([[C2-1\C2-1__MAIN]])')
    wait_map()
    check("level on the map: the three characters", characters() == "true true true", characters())
    save_and_load("saved game")
    check("saved game: the three characters back", characters() == "true true true", characters())

    # the ninja's clothes (its outfit group made for the imported body)
    lua('h2.god(true) return tostring(h2.find("NPC_Ninja_1"):kill())')
    time.sleep(4)
    lua('local n = h2.find("NPC_Ninja_1") local x, y, z = n:pos() h2.player():setpos(x + 60, y + 5, z) return 1')
    time.sleep(2)
    mark = log_size()
    lua("h2.dev_key(%d, 0.3) return 1" % DIK_E)
    time.sleep(6)
    check("clothes taken", "EXCEPTION" not in log_since(mark) and "C2-1__MAIN" in dev.run("return h2.scene()", 10))
    save_and_load("in the ninja's clothes")
finally:
    dev.run('h2.god(false) return h2.dev_level("revert")', 10)
    shutil.rmtree(FOLDER, ignore_errors=True)
print("\n%d passed, %d failed" % (sum(results), len(results) - sum(results)))
