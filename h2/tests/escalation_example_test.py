"""The example escalation (h2/examples/missions/TheAccountantEscalation, #29) played through: each of
its three stages started as the launcher starts it, the accountant killed, Hitman at the exit: the
mission completed, the stage done (cleanly), the next one open; the bodyguard of stage 2 on.

Uses Mods/_EscExample (removed; run_all puts the saves back); a running game with dev commands.

  python h2/tests/escalation_example_test.py
"""
import json
import os
import shutil
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "tools"))
import dev  # noqa: E402

LEVEL = "_EscExample"
SOURCE = os.path.join(HERE, "..", "examples", "missions", "TheAccountantEscalation")
FOLDER = os.path.join(dev.GAME, "Mods", LEVEL)
PROGRESS = os.path.join(dev.GAME, "Save", "escalations.json")
results = []


def lua(code, timeout=30):
    out = dev.run(code, timeout)
    if out.startswith("error") or "timeout" in out:
        raise RuntimeError(out)
    return out


def check(name, ok, detail=""):
    results.append(bool(ok))
    print("%s %s %s" % ("PASS" if ok else "FAIL", name, detail))


def progress():
    try:
        with open(PROGRESS, encoding="utf-8") as f:
            return json.load(f).get(LEVEL, {})
    except (OSError, ValueError):
        return {}


def guards_near_accountant():  # (the script's: not C1-1's own, whose paths have the map's)
    return int(lua('local a = h2.find("NPC_MafiaLawyer_1") local x, _, z = a:pos() local n = 0 '
                   'for _, o in ipairs(h2.objects("ZActor")) do local ox, _, oz = o:pos() '
                   'if o:path():find("BodyguardMafia") and not o:path():find("C1-1", 1, true) and (ox - x)^2 + (oz - z)^2 < 600^2 then n = n + 1 end end '
                   'return n'))


if os.path.isdir(FOLDER):
    shutil.rmtree(FOLDER)
shutil.copytree(SOURCE, FOLDER)
exit_point = json.load(open(os.path.join(FOLDER, "level.json"), encoding="utf-8"))["spawned"][0]["pos"]
try:
    if os.path.exists(PROGRESS):
        p = json.load(open(PROGRESS, encoding="utf-8"))
        p.pop(LEVEL, None)
        json.dump(p, open(PROGRESS, "w", encoding="utf-8"))
    lua('return h2.dev_level("load", "%s")' % LEVEL)
    for stage in (1, 2, 3):
        lua("return h2.dev_stage(%d)" % stage)
        if not dev.wait_ready("C1-1__MAIN", 180):
            raise RuntimeError("the map did not come in")
        time.sleep(6)
        check("stage %d: h2.stage()" % stage, lua("return (h2.stage())") == str(stage))
        guards = guards_near_accountant()
        check("stage %d: the bodyguard %s" % (stage, "there" if stage >= 2 else "not there"),
              (guards > 0) == (stage >= 2), "%d near him" % guards)
        lua('ESC_RESULT = nil h2.on("mission", function(e) ESC_RESULT = e.result end) return 1')
        lua('h2.find("NPC_MafiaLawyer_1"):kill() return 1')
        time.sleep(2)
        lua("h2.player():setpos(%f, %f, %f) return 1" % tuple(exit_point))
        t = time.time()
        while time.time() - t < 20 and lua("return tostring(ESC_RESULT)") == "nil":
            time.sleep(1)
        check("stage %d: completed" % stage, lua("return tostring(ESC_RESULT)") == "completed")
        time.sleep(2)
        p = progress()
        check("stage %d: done, cleanly" % stage, p.get("done") == stage and stage in p.get("clean", []), str(p))
finally:
    try:
        lua(r"return h2.dev_loadscene([[C1-1\C1-1__MAIN]])")
        dev.wait_ready("C1-1__MAIN", 180)
    except Exception:  # noqa: BLE001
        pass
    shutil.rmtree(FOLDER, ignore_errors=True)

print("%d passed, %d failed" % (results.count(True), results.count(False)))
sys.exit(0 if all(results) else 1)
