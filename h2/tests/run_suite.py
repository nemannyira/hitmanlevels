"""Runs an in-game Lua test suite (a running game with dev commands, the map
loaded, the editor closed) and prints its results.

  python h2/tests/run_suite.py [h2/tests/api_suite.lua] [timeout seconds]
"""
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "tools"))
import dev  # noqa: E402

path = sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, "api_suite.lua")
timeout = float(sys.argv[2]) if len(sys.argv) > 2 else 90
print(dev.run(open(path, encoding="utf-8").read(), 60))
t = time.time()
while time.time() - t < timeout:
    out = dev.run("return tostring(SUITE and SUITE.done)", 10)
    if out.strip() == "true":
        break
    time.sleep(1)
print(dev.run('return string.format("pass %d fail %d pending %d", SUITE.pass, SUITE.fail, SUITE.pending)', 10))
print(dev.run('return table.concat(SUITE.log, string.char(10))', 10))
