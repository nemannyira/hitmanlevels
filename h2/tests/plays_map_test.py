"""The players' plays of the author's own level on the in-game map (#53, editor/ingame_map.h PlayMarkCopies):
plays.json from the site's author page in the level's folder; each place where Hitman died or was spotted gets
a map icon made with the map, on the floor of its place (C1-1: Outside, near Hitman's start), drawn when the map
is opened with M (a picture: dumps/shots/plays_map.png). A plays.json of another level is not used. Uses
Mods/_PlaysMapTest (removed); a running game with dev commands.

  python h2/tests/plays_map_test.py
"""
import json
import os
import shutil
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "tools"))
import dev  # noqa: E402

LEVEL = "_PlaysMapTest"
PACKAGE = "playsmaptest01"
FOLDER = os.path.join(dev.GAME, "Mods", LEVEL)
LOG = os.path.join(dev.GAME, "H2Fix.log")
DIK_M = 0x32
results = []


def lua(code, timeout=30):
    out = dev.run(code, timeout)
    if out.startswith("error") or "timeout" in out:
        raise RuntimeError(out)
    return out


def check(name, ok, detail=""):
    results.append(bool(ok))
    print("%s %s %s" % ("PASS" if ok else "FAIL", name, detail))


def load(level=None):
    if level:
        lua('return h2.dev_level("load", "%s")' % level)
    lua(r'return h2.dev_loadscene([[C1-1\C1-1__MAIN]])')
    dev.wait_ready("C1-1__MAIN", 150)


def size():
    return os.path.getsize(LOG)


def since(at):
    with open(LOG, encoding="latin1") as f:
        f.seek(at)
        return f.read()


def plays(level, at_died, at_spotted):
    with open(os.path.join(FOLDER, "plays.json"), "w", encoding="utf-8") as f:
        json.dump({"format": 1, "level": level, "title": "T", "base_scene": "C1-1\\C1-1__MAIN", "runs": [
            {"version": 1, "result": "failed", "time": 90, "counters": {"alerts": 1}, "events": [
                {"kind": "spotted", "t": 40, "at": at_spotted}, {"kind": "died", "t": 80, "at": at_died}]},
            {"version": 1, "result": "left", "time": 20, "counters": {"alerts": 0}, "events": []}]}, f)


if os.path.isdir(FOLDER):
    shutil.rmtree(FOLDER)
try:
    load()
    lua('return h2.dev_level("new", "%s")' % LEVEL)
    x, y, z = (float(v) for v in lua('local x, y, z = h2.player():pos() return x .. " " .. y .. " " .. z').split())
    lua('return h2.dev_level("save")')
    doc = json.load(open(os.path.join(FOLDER, "level.json"), encoding="utf-8"))
    doc["package"] = {"id": PACKAGE, "version": 1}  # (as an export stamps it: the plays name it)
    json.dump(doc, open(os.path.join(FOLDER, "level.json"), "w", encoding="utf-8"), indent=1)
    died, spotted = [round(x + 600), round(y), round(z)], [round(x - 600), round(y), round(z + 400)]
    plays(PACKAGE, died, spotted)
    mark = size()
    load(LEVEL)
    log = since(mark)
    check("plays.json read: 1 death, 1 time spotted", "1 deaths, 1 times spotted" in log,
          [l.strip()[-80:] for l in log.splitlines() if "plays" in l][:2])
    for name, icon in (("Plays_died0", "Icon_Target"), ("Plays_spotted0", "Icon_PointOfInterest")):
        out = lua('local o = h2.find("%s_MapIcon") if not o then return "missing|" end '
                  'local a, b = h2.dev_mapicon(o:path()) return a .. "|" .. b' % name)
        check("%s: %s on the floor of its place (Outside)" % (name, icon), out == icon + " shown|MAP_Outside", out)
    lua("h2.dev_key(%d, 0.1) return 1" % DIK_M)
    time.sleep(2.5)
    drawn = lua("return h2.dev_mapicons()")
    dev.shot("plays_map", 0.6)
    lua("h2.dev_key(%d, 0.1) return 1" % DIK_M)
    time.sleep(1.5)
    check("the map draws them", "Plays_died0_MapIcon" in drawn and "Plays_spotted0_MapIcon" in drawn,
          [l for l in drawn.splitlines() if "Plays" in l][:2])

    plays("someotherlevel1", died, spotted)
    mark = size()
    load(LEVEL)
    check("another level's plays.json: not used", "not this level's plays" in since(mark) and
          lua('return tostring(h2.find("Plays_died0_MapIcon") == nil)') == "true")

    # an escalation: the places of the stage the map is loaded for only (its things differ by stage)
    doc["stages"] = [{"name": "One", "text": ""}, {"name": "Two", "text": ""}]
    json.dump(doc, open(os.path.join(FOLDER, "level.json"), "w", encoding="utf-8"), indent=1)
    with open(os.path.join(FOLDER, "plays.json"), "w", encoding="utf-8") as f:
        json.dump({"format": 1, "level": PACKAGE, "runs": [
            {"version": 1, "stage": 1, "result": "failed", "time": 80, "counters": {}, "events": [
                {"kind": "died", "t": 80, "at": died}]},
            {"version": 1, "stage": 2, "result": "failed", "time": 40, "counters": {}, "events": [
                {"kind": "spotted", "t": 40, "at": spotted}]}]}, f)
    mark = size()
    load(LEVEL)
    check("an escalation's stage 1: only stage 1's places (1 death, not stage 2's spotting)",
          "1 deaths, 0 times spotted" in since(mark), [l.strip()[-60:] for l in since(mark).splitlines() if "plays" in l][:1])
finally:
    try:
        lua('return h2.dev_level("revert")')
        load()
    except Exception:  # noqa: BLE001
        pass
    shutil.rmtree(FOLDER, ignore_errors=True)

print("\n%d passed, %d failed" % (results.count(True), results.count(False)))
sys.exit(0 if all(results) else 1)
