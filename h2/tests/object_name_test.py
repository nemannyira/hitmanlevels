"""An item's name in the mission (#40): an item placed and named ("the device"), then made a take objective:
the line says "Take the device" (the goal knows the item as "level:...", the name is kept under the item's
path); named again, the line follows. Uses Mods/_ObjectNameTest (removed); a running game with dev commands.

  python h2/tests/object_name_test.py
"""
import json
import os
import shutil
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "tools"))
import dev  # noqa: E402

LEVEL = "_ObjectNameTest"
FOLDER = os.path.join(dev.GAME, "Mods", LEVEL)
KEY = "ROOT/C1-1/C1-1_CHARACTERS.zip/AllLevels/Equipment.zip/AllLevels/Items.zip/Items/KeyRoom"
results = []


def lua(code, timeout=30):
    out = dev.run(code, timeout)
    if out.startswith("error") or "timeout" in out:
        raise RuntimeError(out)
    return out


def check(name, ok, detail=""):
    results.append(bool(ok))
    print("%s %s %s" % ("PASS" if ok else "FAIL", name, str(detail)[:200]))


def take_line():
    lua('return h2.dev_level("save")')
    doc = json.load(open(os.path.join(FOLDER, "level.json"), encoding="utf-8"))
    return [o.get("text") for o in doc["mission"]["objectives"] if o.get("goal") == "take"]


shutil.rmtree(FOLDER, ignore_errors=True)
try:
    lua(r'return h2.dev_loadscene([[C1-1\C1-1__MAIN]])', 10)
    dev.wait_ready("C1-1__MAIN", 180)
    lua('return h2.dev_level("new", "%s")' % LEVEL)
    x, y, z = (float(v) for v in lua('local x, y, z = h2.player():pos() return x .. " " .. y .. " " .. z').split())
    item = lua('return h2.dev_edit("item", [[%s]], %f, %f, %f)' % (KEY, x + 200, y + 3, z))
    check("an item placed", item.startswith("ROOT"), item)
    check("named", lua('return tostring(h2.dev_edit("name", [[%s]], "the device"))' % item) == "true")
    lua('return h2.dev_level("take", [[%s]])' % item)
    lines = take_line()
    check("the take line says its name", lines == ["Take the device"], lines)
    lua('return tostring(h2.dev_edit("name", [[%s]], "the ledger"))' % item)
    lines = take_line()
    check("named again: the line follows", lines == ["Take the ledger"], lines)
finally:
    try:
        lua('return h2.dev_level("revert")')
        lua(r'return h2.dev_loadscene([[C1-1\C1-1__MAIN]])', 10)
        dev.wait_ready("C1-1__MAIN", 180)
    except Exception:  # noqa: BLE001
        pass
    shutil.rmtree(FOLDER, ignore_errors=True)

print("\n%d passed, %d failed" % (results.count(True), results.count(False)))
sys.exit(0 if all(results) else 1)
