"""The play statistics' run (docs/play_stats.md section 1, #50): a downloaded level's plays written down by
the DLL in Mods\\Online\\<id>\\runs\\<started>.json. Three plays of a level installed as a downloaded one:
completed after two shots (the statistics screen's counters, time and rating), failed (h2.mission), left
(the main menu loaded) with god mode on (its aids). Starts the game itself with DevCommandsOnline=1 (the dev
commands kept while a downloaded level runs: such runs are marked "dev"), and again with the settings as
they were at the end.

  python h2/tests/runs_test.py
"""
import glob
import json
import os
import shutil
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "tools"))
sys.path.insert(0, HERE)
import dev  # noqa: E402
import run_all  # noqa: E402

ID = "h2testruns01"
FOLDER = os.path.join(dev.GAME, "Mods", "Online", ID)
RUNS = os.path.join(FOLDER, "runs")
INI = os.path.join(dev.GAME, "H2Fix.ini")
C11 = r"C1-1\C1-1__MAIN"
LEVEL = {"format": "h2level", "version": 1, "name": "Runs test", "base_scene": "C1-1\\C1-1__MAIN",
         "objects": [], "spawned": [], "npcs": [], "lists": [], "scripts": [],
         "package": {"id": ID, "version": 3}}
results = []


def check(name, ok, detail=""):
    results.append(bool(ok))
    print("%s %s %s" % ("PASS" if ok else "FAIL", name, detail))


def lua(code, timeout=15):
    return dev.run(code, timeout)


def runs():
    return sorted(glob.glob(os.path.join(RUNS, "*.json")))


def new_run(before, timeout=15):
    """the run written since `before` (the list of files then) -> its JSON, or None"""
    t = time.time()
    while time.time() - t < timeout:
        new = [f for f in runs() if f not in before]
        if new:
            with open(new[-1], encoding="utf-8") as f:
                return json.load(f)
        time.sleep(0.5)
    return None


def load_map():
    lua("return h2.dev_loadscene([[%s]])" % C11, 10)
    ok = dev.wait_ready("C1-1__MAIN", 180)
    time.sleep(3)
    return ok


with open(INI, encoding="latin1", newline="") as f:
    ini = f.read()
try:
    shutil.rmtree(FOLDER, ignore_errors=True)
    os.makedirs(FOLDER)
    with open(os.path.join(FOLDER, "level.json"), "w", encoding="utf-8") as f:
        json.dump(LEVEL, f)
    run_all.set_debug_key("DevCommandsOnline", 1)
    check("the game started", run_all.start())
    # (in one command: the level the editor's, its map loaded)
    lua('local info = h2.dev_level("load", [[Online\\%s]]) h2.dev_loadscene([[%s]]) return info' % (ID, C11), 30)
    check("the downloaded level's map loaded", dev.wait_ready("C1-1__MAIN", 180))
    time.sleep(3)

    # ---- 1: completed after two shots
    before = runs()
    for _ in range(3):
        if lua("local _, k = h2.hand() return k or ''") == "pistol":
            break
        lua("h2.dev_key(3, 0.3) return 1")  # (his pistols)
        time.sleep(4)
    for _ in range(2):
        lua("h2.dev_mouse(0, 0.1) return 1")
        time.sleep(1.5)
    time.sleep(2)
    lua('return h2.mission("complete")')
    r = new_run(before)
    check("completed: a run written", r is not None, str(runs()))
    r = r or {}
    check("  the level, its version, the DLL's", r.get("level") == ID and r.get("version") == 3 and r.get("dll"),
          "%s %s %s" % (r.get("level"), r.get("version"), r.get("dll")))
    check("  result completed", r.get("result") == "completed", r.get("result"))
    check("  two shots (the statistics screen's)", r.get("counters", {}).get("shots") == 2, str(r.get("counters")))
    check("  its time", 5 <= r.get("time", 0) < 600, str(r.get("time")))
    check("  its rating", r.get("rating") in ("SilentAssassin", "Ghost", "Shadow", "Professional"),
          "%s (stealth %s, aggression %s)" % (r.get("rating"), r.get("stealth"), r.get("aggression")))
    check("  marked: the dev commands on", r.get("dev") == ["DevCommands"] and r.get("aids") == [], str(r.get("dev")))
    check("  a run id, when it started", len(r.get("run", "")) == 32 and r.get("started", "").endswith("Z"))

    # ---- 2: failed
    check("the map again", load_map())
    before = runs()
    lua('return h2.mission("fail")')
    r = new_run(before) or {}
    check("failed: a run written, result failed", r.get("result") == "failed", r.get("result"))
    check("  no rating", "rating" not in r)

    # ---- 3: left, with god mode
    check("the map again", load_map())
    before = runs()
    lua("h2.god(true) return 1")
    time.sleep(1)
    lua('return h2.dev_loadscene("MainMenu")', 10)
    r = new_run(before) or {}
    check("left: a run written, result left", r.get("result") == "left", r.get("result"))
    check("  its aids: god mode", r.get("aids") == ["god mode"], str(r.get("aids")))
    dev.wait_ready("MainMenu", 60)
    time.sleep(2)
    check("one run a play (3 files)", len(runs()) == 3, str([os.path.basename(f) for f in runs()]))
finally:
    run_all.kill()
    with open(INI, "w", encoding="latin1", newline="") as f:
        f.write(ini)
    shutil.rmtree(FOLDER, ignore_errors=True)
    try:
        os.rmdir(os.path.dirname(FOLDER))
    except OSError:
        pass
    run_all.start()  # (as it was: the game running with the user's settings)

print("\n%d passed, %d failed" % (results.count(True), results.count(False)))
sys.exit(0 if all(results) else 1)
