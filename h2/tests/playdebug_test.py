"""The debug view while playing (editor/playdebug.cpp): a Simple mode level on C1-1 with a target and
"never be spotted", played from the start; the view switched on (h2.dev_playdebug), the game's sight
table read as the view reads it (h2.dev_seers: ranges, view angles, eyes), the objectives listed,
back in the editor with F8, the last test's events (h2.dev_lasttest) and their list in the Try it step.
The suspicion the scripts report (who, value) is in the game's log: printed as a NOTE (whether `who`
is the character's reference). Uses Mods/_PlayDebugTest (removed after); a running game with dev
commands.

  python h2/tests/playdebug_test.py
"""
import json
import os
import re
import shutil
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "tools"))
import dev  # noqa: E402

LEVEL = "_PlayDebugTest"
FOLDER = os.path.join(dev.GAME, "Mods", LEVEL)
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


def shot(name):
    lua('h2.dev_shot("playdebug_%s") return 1' % name)
    time.sleep(0.5)


def log_text():
    with open(os.path.join(dev.GAME, "H2Fix.log"), encoding="latin1", errors="replace") as f:
        return f.read()


if os.path.isdir(FOLDER):
    shutil.rmtree(FOLDER)
try:
    lua(r'return h2.dev_loadscene([[C1-1\C1-1__MAIN]])')
    dev.wait_ready("C1-1__MAIN", 150)
    lua('return h2.dev_level("new", "%s")' % LEVEL)
    lua('h2.dev_editor(true) return 1')
    lua('return h2.dev_mode("simple")')
    time.sleep(1)
    press("1 Mission")
    target = lua('for _, n in ipairs(h2.npcs()) do if n.object and n.object:alive() then return n.object:path() end end return ""')
    lua('h2.dev_select(%s) return 1' % json.dumps(target))
    time.sleep(0.5)
    check("a target", press("Make it a target").startswith("pressed"))
    check("never be spotted", press("Never be spotted").startswith("pressed"))
    # the target named for the mission: its objective's text follows (mission.h SetObjectName)
    check("the target named", lua('return h2.dev_edit("name", %s, "the courier")' % json.dumps(target)) == "true")
    lua('return h2.dev_level("save")')
    with open(os.path.join(FOLDER, "level.json"), encoding="utf-8") as f:
        mission = json.load(f).get("mission", {})
    check("its objective says the name", any(o.get("text") == "Kill the courier" for o in mission.get("objectives", [])),
          str(mission.get("objectives"))[:160])
    press("5 Try it")
    check("the debug view's settings in Try it", press("What they see", settle=0.2).startswith("pressed"))
    press("What they see")  # (back on)
    check("Play from the start", press("Play from the start").startswith("pressed"))
    dev.wait_ready("C1-1__MAIN", 150)
    time.sleep(4)
    check("the view switched on", lua('return tostring(h2.dev_playdebug(true))') == "true")
    time.sleep(1)
    shot("view")

    seers = lua("return h2.dev_seers()").splitlines()
    print("\n".join(seers[:12]))
    rows = [re.search(r"range (\d+) angle (\d+) actor (\d) sees (\d)", s) for s in seers[1:]]
    rows = [m for m in rows if m]
    check("the sight table read (seers)", len(rows) > 0, "%d seers" % len(rows))
    check("ranges and view angles as the game keeps them",
          rows and all(0 < int(m.group(1)) <= 100000 and 0 <= int(m.group(2)) <= 360 for m in rows),
          ", ".join("%s/%s" % (m.group(1), m.group(2)) for m in rows[:6]))
    check("characters among them (their eyes: their heads)", any(m.group(3) == "1" for m in rows))
    check("the alarm level line", seers and seers[0].startswith("stress "), seers[0] if seers else "")

    time.sleep(6)  # (events, the objectives polled)
    playing = lua("return h2.dev_lasttest()")
    check("the test's log while playing", "The test starts" in playing, playing[:200])
    reports = re.findall(r"stress reported: who ([0-9A-F]+), value (-?\d+)", log_text())
    print("NOTE suspicion reports: %s" % (reports[:6] or "none yet (nobody suspicious)"))

    before = log_text().count("test: back in the editor")
    lua('return h2.dev_wmkey(0x77)')  # (F8)
    for _ in range(90):
        time.sleep(1)
        if log_text().count("test: back in the editor") > before:
            break
    check("F8: back in the editor", log_text().count("test: back in the editor") > before)
    dev.wait_ready("C1-1__MAIN", 60)
    time.sleep(1)
    events = lua("return h2.dev_lasttest()")
    print(events)
    check("the last test's events kept after the map loaded again", "The test starts" in events)
    lua('h2.dev_editor(true) h2.dev_mode("simple") return 1')
    time.sleep(1)
    press("5 Try it")
    check("Try it: What happened in the last test",
          press("What happened in the last test", settle=0.3).startswith("pressed"))
    shot("lasttest")
finally:
    dev.run('h2.dev_playdebug(false) h2.dev_mode("advanced") h2.dev_editor(false) return h2.dev_level("revert")', 10)
    shutil.rmtree(FOLDER, ignore_errors=True)
print("\n%d passed, %d failed" % (sum(results), len(results) - sum(results)))
sys.exit(0 if all(results) else 1)
