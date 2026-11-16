"""Simple mode's launcher (#43): with "Keep the map's people" off the new level is an empty map (the map's
characters, items and objectives taken out: level.json "empty_map"), and the guide still offers people to
add (the map's looks) and a person can be placed. Uses Mods/_SimpleEmptyTest (removed); a running game with
dev commands.

  python h2/tests/simple_empty_test.py
"""
import json
import os
import shutil
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "tools"))
import dev  # noqa: E402

LEVEL = "_SimpleEmptyTest"
FOLDER = os.path.join(dev.GAME, "Mods", LEVEL)
results = []


def lua(code, timeout=30):
    out = dev.run(code, timeout)
    if out.startswith("error") or "timeout" in out:
        raise RuntimeError(out)
    return out


def check(name, ok, detail=""):
    results.append(bool(ok))
    print("%s %s %s" % ("PASS" if ok else "FAIL", name, str(detail)[:200]))


def press(label, n=1, settle=0.8):
    lua('h2.dev_press(%s, %d) return 1' % (json.dumps(label), n))
    status = "looking"
    t = time.time()
    while time.time() - t < 5 and status == "looking":
        time.sleep(0.2)
        status = lua("return h2.dev_press_status()")
    time.sleep(settle)
    return status


def shot(name):
    lua('h2.dev_shot("simple_empty_%s") return 1' % name)
    time.sleep(0.5)


shutil.rmtree(FOLDER, ignore_errors=True)
try:
    lua("h2.dev_launcher() return 1")
    time.sleep(1)
    check("New level", press("New level").startswith("pressed"))
    press("Simple: step by step")
    check("the name field", press("name").startswith("pressed"))
    lua('h2.dev_type("%s") return 1' % LEVEL)
    time.sleep(0.5)
    check("Keep the map's people: off", press("Keep the map's people").startswith("pressed"))
    shot("launcher")
    check("Create and start editing", press("Create and start editing").startswith("pressed"))
    time.sleep(3)
    check("the map loaded", dev.wait_ready("C1-1__MAIN", 180))
    time.sleep(2)
    with open(os.path.join(FOLDER, "level.json"), encoding="utf-8") as f:
        doc = json.load(f)
    check("level.json: an empty map", doc.get("empty_map") is True, str(doc)[:160])
    # (taken out: hidden, far under the map, ClearScene)
    left = lua('local n = 0 for _, c in ipairs(h2.npcs()) do if c.object then local _, y = c.object:pos() '
               'if y > -10000 then n = n + 1 end end end return n')
    check("no people of the map in the world", left == "0", left)

    lua('h2.dev_editor(true) return 1')
    time.sleep(1)
    check("the guide: Next: People", press("Next: People >").startswith("pressed"))
    check("A guard", press("A guard").startswith("pressed"))
    look = press("##look")
    check("a look to copy", look.startswith("pressed"), look)
    lua('h2.dev_uikey(526) return 1')  # (the list closed)
    time.sleep(0.5)
    check("Place it", press("Place it").startswith("pressed"))
    w, h = (float(v) for v in lua('local w, h = h2.dev_screen() return w .. " " .. h').split())
    for _ in range(3):
        lua('h2.dev_hover(%f, %f) return 1' % (w / 2, h * 0.72))
        time.sleep(0.5)
    lua('h2.dev_click(%f, %f) return 1' % (w / 2, h * 0.72))
    time.sleep(1.5)
    info = lua('return h2.dev_level("info")')
    check("a guard placed", "npcs=1" in info, info)
    shot("people")
finally:
    try:
        lua('h2.dev_mode("advanced") h2.dev_editor(false) return h2.dev_level("revert")')
        lua(r'return h2.dev_loadscene([[C1-1\C1-1__MAIN]])', 10)
        dev.wait_ready("C1-1__MAIN", 180)
    except Exception:  # noqa: BLE001
        pass
    shutil.rmtree(FOLDER, ignore_errors=True)

print("\n%d passed, %d failed" % (results.count(True), results.count(False)))
sys.exit(0 if all(results) else 1)
