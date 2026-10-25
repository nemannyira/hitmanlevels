"""The kill switch: a level whose scripts run out of time three times is
stopped (its handlers, timers and zones removed), the console's own handlers
go on, the Problems list says so. Uses Mods/_KillTest (removed at the end);
a running game with dev commands, on C1-1.

  python h2/tests/killswitch_test.py
"""
import json
import os
import shutil
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "tools"))
import dev  # noqa: E402

FOLDER = os.path.join(dev.GAME, "Mods", "_KillTest")
LEVEL = {"format": "h2level", "version": 1, "name": "_KillTest", "base_scene": "C1-1\\C1-1__MAIN",
         "scripts": ["main.lua"]}
MAIN = """
KS_LEVEL = 0
h2.on("frame", function() KS_LEVEL = KS_LEVEL + 1 end)
for i = 1, 3 do
  h2.after(0.5 * i, function() while true do end end)  -- each runs out of time (3 s)
end
"""

results = []


def check(name, ok, detail=""):
    results.append(ok)
    print("%s %s %s" % ("PASS" if ok else "FAIL", name, detail))


os.makedirs(FOLDER, exist_ok=True)
open(os.path.join(FOLDER, "level.json"), "w", encoding="utf-8").write(json.dumps(LEVEL))
open(os.path.join(FOLDER, "main.lua"), "w", encoding="utf-8").write(MAIN)
try:
    print(dev.run('KS_CONSOLE = 0 h2.on("frame", function() KS_CONSOLE = KS_CONSOLE + 1 end) return "console handler"'))
    print(dev.run('return h2.dev_level("load", "_KillTest")', 30))
    time.sleep(2)
    a = dev.run("return KS_LEVEL", 10)
    check("level handler runs", a.isdigit() and int(a) > 0, a)
    time.sleep(16)  # three timers, 3 s each
    problems = dev.run('return h2.dev_level("problems")', 10)
    check("problems name it", "ran too long three times" in problems, problems.replace("\n", " | ")[:200])
    l1, c1 = dev.run("return KS_LEVEL", 10), dev.run("return KS_CONSOLE", 10)
    time.sleep(2)
    l2, c2 = dev.run("return KS_LEVEL", 10), dev.run("return KS_CONSOLE", 10)
    check("level handler stopped", l1 == l2, "%s -> %s" % (l1, l2))
    check("console handler goes on", c1.isdigit() and c2.isdigit() and int(c2) > int(c1), "%s -> %s" % (c1, c2))
    check("game up", "C1-1" in dev.run("return h2.scene()", 10))
finally:
    dev.run('h2.off("frame") return h2.dev_level("revert")', 10)
    shutil.rmtree(FOLDER, ignore_errors=True)
print("\n%d passed, %d failed" % (sum(results), len(results) - sum(results)))
