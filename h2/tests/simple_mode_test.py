"""Simple mode (docs/simple-mode.md, #31-#37) driven by its own controls (h2.dev_press): a level
made only through the guide on C1-1: a target from the selection card, "never be spotted", a
guard placed with a click in the world, an item, a hidden object of the map; Advanced mode and
back keep everything; saved and tried (the guard made with the map). Uses Mods/_SimpleTest
(removed after); a running game with dev commands, C1-1 loaded.

  python h2/tests/simple_mode_test.py
"""
import json
import os
import shutil
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "tools"))
import dev  # noqa: E402

LEVEL = "_SimpleTest"
FOLDER = os.path.join(dev.GAME, "Mods", LEVEL)
HIDE = "Tree_02_21"
results = []


def lua(code, timeout=30):
    out = dev.run(code, timeout)
    if out.startswith("error") or "timeout" in out:
        raise RuntimeError(out)
    return out


def check(name, ok, detail=""):
    results.append(bool(ok))
    print("%s %s %s" % ("PASS" if ok else "FAIL", name, detail))


def press(label, n=1, settle=0.8):
    lua('h2.dev_press(%s, %d) return 1' % (json.dumps(label), n))
    status = "looking"
    t = time.time()
    while time.time() - t < 5 and status == "looking":
        time.sleep(0.2)
        status = lua("return h2.dev_press_status()")
    time.sleep(settle)
    return status


def doc():
    with open(os.path.join(FOLDER, "level.json"), encoding="utf-8") as f:
        return json.load(f)


def shot(name):
    lua('h2.dev_shot("simple_%s") return 1' % name)
    time.sleep(0.5)


if os.path.isdir(FOLDER):
    shutil.rmtree(FOLDER)
try:
    lua(r'return h2.dev_loadscene([[C1-1\C1-1__MAIN]])')
    dev.wait_ready("C1-1__MAIN", 150)
    lua('return h2.dev_level("new", "%s")' % LEVEL)
    lua('h2.dev_editor(true) return 1')
    check("simple mode on", lua('return h2.dev_mode("simple")') == "simple")
    time.sleep(1)
    check("the guide's step bar", press("1 Mission").startswith("pressed"))
    check("Advanced mode's tabs are not drawn", press("Scene", settle=0.2) == "not found")
    shot("mission")

    # 1. Mission: a target from the selection card, never be spotted
    target = lua('for _, n in ipairs(h2.npcs()) do if n.object and n.object:alive() then return n.object:path() end end return ""')
    lua('h2.dev_select(%s) return 1' % json.dumps(target))
    time.sleep(0.5)
    shot("card")
    check("the card: Make it a target", press("Make it a target").startswith("pressed"))
    check("Never be spotted", press("Never be spotted").startswith("pressed"))

    # 2. People: a guard placed where the camera looks
    check("Next: People", press("Next: People >").startswith("pressed"))
    check("A guard", press("A guard").startswith("pressed"))
    check("Place it", press("Place it").startswith("pressed"))
    w, h = (float(v) for v in lua('local w, h = h2.dev_screen() return w .. " " .. h').split())
    for i in range(3):  # (the first frames look for the surface under the mouse)
        lua('h2.dev_hover(%f, %f) return 1' % (w / 2, h * 0.72))
        time.sleep(0.5)
    shot("placing")
    lua('h2.dev_click(%f, %f) return 1' % (w / 2, h * 0.72))
    time.sleep(1.5)
    info = lua('return h2.dev_level("info")')
    check("a guard placed", "npcs=1" in info, info)
    shot("people")

    # 3. Items: the first weapon of the map
    check("Next: Items", press("Next: Items >").startswith("pressed"))
    shot("items")

    # 4. The map: an object of the map hidden from its card
    check("Next: The map", press("Next: The map >").startswith("pressed"))
    lua('h2.dev_select(h2.find("%s"):path()) return 1' % HIDE)
    time.sleep(0.5)
    check("the card: Hide", press("Hide (H)").startswith("pressed"))
    check("the object hidden", lua('return tostring(h2.find("%s"):hidden())' % HIDE) == "true")

    # Advanced mode and back: nothing lost
    check("Advanced mode", press("Advanced mode").startswith("pressed") and lua("return h2.dev_mode()") == "advanced")
    check("Advanced mode's toolbar: Simple mode", press("Simple mode").startswith("pressed")
          and lua("return h2.dev_mode()") == "simple")

    # 6. Save
    check("step 6", press("6 Save & share").startswith("pressed"))
    check("Save", press("Save (Ctrl+S)").startswith("pressed"))
    d = doc()
    goals = [o.get("goal") for o in d.get("mission", {}).get("objectives", [])]
    check("saved: the target and never spotted", "kill" in goals and "unseen" in goals, str(goals))
    check("saved: the guard", len(d.get("npcs", [])) == 1)
    check("saved: the hidden object", any(o.get("hidden") for o in d.get("objects", [])), str(d.get("objects"))[:120])
    shot("share")

    # 5. Try it: the map again with the guard
    check("step 5", press("5 Try it").startswith("pressed"))
    check("Play from the start", press("Play from the start").startswith("pressed"))
    dev.wait_ready("C1-1__MAIN", 150)
    time.sleep(3)
    guard = d["npcs"][0].get("id", "") if d.get("npcs") else ""
    real = lua('local o = h2.find(%s) return tostring(o ~= nil and o:alive())' % json.dumps(guard))
    check("the guard made with the map", real == "true", guard + ": " + real)
finally:
    dev.run('h2.dev_mode("advanced") h2.dev_editor(false) return h2.dev_level("revert")', 10)
    shutil.rmtree(FOLDER, ignore_errors=True)
print("\n%d passed, %d failed" % (sum(results), len(results) - sum(results)))
sys.exit(0 if all(results) else 1)
