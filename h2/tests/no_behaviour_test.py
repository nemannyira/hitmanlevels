"""A character without a behaviour (#57): Hitman cannot kill or knock it out (the game leaves a character's death
to its behaviour: h2/docs/npc.md), so Problems lists it, and says the mission cannot be finished when it is a
target; one with a behaviour (the plain civilian) is not listed. Uses Mods/_NoBehaviourTest (removed); a running
game with dev commands.

  python h2/tests/no_behaviour_test.py
"""
import os
import shutil
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "tools"))
import dev  # noqa: E402

LEVEL = "_NoBehaviourTest"
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


shutil.rmtree(FOLDER, ignore_errors=True)
try:
    lua(r'return h2.dev_loadscene([[C1-1\C1-1__MAIN]])', 10)
    dev.wait_ready("C1-1__MAIN", 180)
    lua('return h2.dev_level("new", "%s")' % LEVEL)
    bare = lua("return h2.dev_place_npc(2350, -4176, -20, -6682)")  # (no behaviour)
    civilian = lua('return h2.dev_place_npc(2350, -4637, -20, -6353, "Neutral2")')
    problems = lua('return h2.dev_level("problems")')
    check("a character without a behaviour is listed", "character %s has no behaviour" % bare in problems, problems)
    check("  not as a target yet", "mission cannot be finished" not in problems)
    check("one with the plain civilian's is not", "character %s has no" % civilian not in problems)
    lua('return h2.dev_level("target", h2.find("%s"):path())' % bare)
    problems = lua('return h2.dev_level("problems")')
    check("made a target: the mission cannot be finished", "character %s has no behaviour" % bare in problems and
          "it is a target: the mission cannot be finished" in problems, problems)
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
