"""Loads every map in turn and runs tools/tests/map_smoke.lua on it (a game
with dev commands running). A map that takes the game down is reported and
the game started again.

  python tools/tests/run_maps.py [C2-1 C3-3 ...]
"""
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, ".."))
import dev  # noqa: E402

MAPS = ["C0-1", "C1-1", "C2-1", "C2-2", "C2-3", "C2-4", "C3-1", "C3-2a", "C3-2b", "C3-3", "C4-1", "C4-2", "C4-3",
        "C5-1", "C5-2", "C5-3", "C6-1", "C6-2", "C6-3", "C7-1", "C8-1"]
SMOKE = open(os.path.join(HERE, "map_smoke.lua"), encoding="utf-8").read()


def alive():
    return "timeout" not in dev.run("return 1", 5)


def ensure_game():
    if not dev.running() or not alive():
        os.system('python "%s" --kill' % os.path.join(HERE, "..", "dev.py"))
        time.sleep(3)
        os.system('python "%s" --start' % os.path.join(HERE, "..", "dev.py"))


def load(m):
    dev.run(r'return h2.dev_loadscene([[%s\%s__MAIN]])' % (m, m), 10)
    t = time.time()
    time.sleep(4)
    while time.time() - t < 180:
        s = dev.run("return h2.scene()", 5)
        if (m + "__MAIN").lower() in s.lower():
            return True
        if not dev.running():
            return False
        time.sleep(1)
    return False


summary = []
for m in sys.argv[1:] or MAPS:
    ensure_game()
    if not load(m):
        summary.append((m, "did not load"))
        print(m, "did not load")
        continue
    time.sleep(6)
    dev.run("h2.dev_editor(false) return true", 5)
    print(m, dev.run(SMOKE, 60))
    t = time.time()
    done = False
    while time.time() - t < 40:
        out = dev.run("return tostring(SUITE and SUITE.done)", 5)
        if out.strip() == "true":
            done = True
            break
        if "timeout" in out and not dev.running():
            break
        time.sleep(1)
    if not done:
        summary.append((m, "GAME DOWN or hung"))
        print(m, "GAME DOWN or hung")
        continue
    res = dev.run('return string.format("pass %d fail %d", SUITE.pass, SUITE.fail)', 5)
    log = dev.run("return table.concat(SUITE.log, string.char(10))", 5)
    summary.append((m, res + (" | " + log.replace("\n", " | ") if log.strip() else "")))
    print(m, res, log)
print("\n==== summary")
for m, r in summary:
    print(m, r)
