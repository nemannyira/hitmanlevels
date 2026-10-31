"""An escalation's "no bodies found" rule (#30): a stage of C1-1 with the rule; nothing broken while the
guards live; a guard killed in a bedroom (two others by him): one of them sees the body (the game's
TrackLinkObjects sends it ObjectVisible) and the rule is broken. Uses Mods/_BodyTest (removed); a running
game with dev commands.

  python h2/tests/bodies_test.py
"""
import json
import os
import shutil
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "tools"))
import dev  # noqa: E402

LEVEL = "_BodyTest"
FOLDER = os.path.join(dev.GAME, "Mods", LEVEL)
VICTIM = "BodyguardMafia_Bedroom_01/Ground"  # (C1-1: two other bodyguards stand by him)
results = []


def lua(code, timeout=30):
    out = dev.run(code, timeout)
    if out.startswith("error") or "timeout" in out:
        raise RuntimeError(out)
    return out


def check(name, ok, detail=""):
    results.append(bool(ok))
    print("%s %s %s" % ("PASS" if ok else "FAIL", name, detail))


def broken():
    return lua("return tostring(h2.state.__stage and h2.state.__stage.broken['1'])")


if os.path.isdir(FOLDER):
    shutil.rmtree(FOLDER)
os.makedirs(FOLDER)
with open(os.path.join(FOLDER, "level.json"), "w", encoding="utf-8") as f:
    json.dump({"format": "h2level", "version": 1, "name": LEVEL, "base_scene": "C1-1\\C1-1__MAIN",
               "objects": [], "spawned": [], "npcs": [], "lists": [], "scripts": [],
               "stages": [{"name": "Unseen bodies"}],
               "rules": [{"kind": "no_bodies", "broken": "note"}]}, f)
try:
    lua('return h2.dev_level("load", "%s")' % LEVEL)
    lua("return h2.dev_stage(1)")
    if not dev.wait_ready("C1-1__MAIN", 180):
        raise RuntimeError("the map did not come in")
    time.sleep(5)
    check("the stage's rule", lua("return (h2.stage())") == "1")
    time.sleep(3)
    check("nothing found while they live", broken() != "true", broken())
    # a guard taken to Hitman's start on the hill (nobody else there) and killed: his body is not seen
    lone = lua('local hx, hy, hz = h2.player():pos() for _, n in ipairs(h2.npcs()) do '
               'if n.object and n.object:alive() and (n.behaviour or ""):lower():find("guard") '
               'and not n.object:path():find("Bedrooms", 1, true) then '
               'n.object:setpos(hx + 250, hy + 5, hz) return n.object:path() end end return ""')
    time.sleep(2)
    if lone:
        lua('h2.find([[%s]]):kill() return 1' % lone)
        time.sleep(8)
        check("a body nobody sees: not broken", broken() != "true", lone[-50:])
    else:
        print("(no guard alone on the map: that part left out)")
    victim = lua('for _, n in ipairs(h2.npcs()) do if n.object and n.object:path():find(%s, 1, true) then '
                 'return n.object:path() end end return ""' % json.dumps(VICTIM))
    check("the guard to kill", victim != "", victim[-50:])
    lua('h2.find([[%s]]):kill() return 1' % victim)
    t = time.time()
    while time.time() - t < 20 and broken() != "true":
        time.sleep(1)
    check("his body seen: the rule broken", broken() == "true", "%.0f s" % (time.time() - t))
finally:
    try:
        lua(r"return h2.dev_loadscene([[C1-1\C1-1__MAIN]])")
        dev.wait_ready("C1-1__MAIN", 180)
    except Exception:  # noqa: BLE001
        pass
    dev.run('return h2.dev_level("new", "_Nothing")', 10)
    shutil.rmtree(FOLDER, ignore_errors=True)
print("\n%d passed, %d failed" % (sum(results), len(results) - sum(results)))
sys.exit(0 if all(results) else 1)
