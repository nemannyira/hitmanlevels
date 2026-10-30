"""Escalations (#23-#27, docs/escalation.md), on C1-1: a level of 3 stages.

* what is placed has its stages: a point of every stage, one from stage 2, one gone from stage 3, a map
  object gone from stage 2 (hidden), objectives of their stages; the map loads for each stage
  (h2.dev_stage) with what that stage has;
* the rules of the stage (h2._stagerules), h2.stage();
* a saved game keeps its stage: saved in stage 2, loaded while stage 1 is on: stage 2 again;
* a stage done (its mission completed) is the player's progress (Save\\escalations.json), cleanly or not.

Uses Mods/_EscTest and save slot 7 (removed / run_all puts the saves back); a running game with dev
commands.

  python h2/tests/escalation_test.py
"""
import json
import os
import shutil
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "tools"))
import dev  # noqa: E402

LEVEL = "_EscTest"
FOLDER = os.path.join(dev.GAME, "Mods", LEVEL)
PROGRESS = os.path.join(dev.GAME, "Save", "escalations.json")
SLOT = 7
results = []


def lua(code, timeout=30):
    out = dev.run(code, timeout)
    if out.startswith("error") or "timeout" in out:
        raise RuntimeError(out)
    return out


def check(name, ok, detail=""):
    results.append(bool(ok))
    print("%s %s %s" % ("PASS" if ok else "FAIL", name, detail))


def ready():
    if not dev.wait_ready("C1-1__MAIN", 180):
        raise RuntimeError("the map did not come in")
    time.sleep(3)


def found(name):
    return lua('return tostring(h2.find("%s") ~= nil)' % name) == "true"


def hidden(path):
    return lua('return tostring(h2.find([[%s]]):hidden())' % path) == "true"


def stage(n):
    out = lua("return h2.dev_stage(%d)" % n)
    if out.startswith("false"):
        raise RuntimeError(out)
    ready()


def progress():
    try:
        with open(PROGRESS, encoding="utf-8") as f:
            return json.load(f).get(LEVEL, {})
    except (OSError, ValueError):
        return {}


if os.path.isdir(FOLDER):
    shutil.rmtree(FOLDER)
try:
    lua(r"return h2.dev_loadscene([[C1-1\C1-1__MAIN]])")
    ready()
    lua('return h2.dev_level("new", "%s")' % LEVEL)
    x, y, z = [float(v) for v in lua('local x, y, z = h2.player():pos() return x .. " " .. y .. " " .. z').split()]
    a = lua('return h2.dev_edit("point", %f, %f, %f)' % (x + 200, y, z))
    b = lua('return h2.dev_edit("point", %f, %f, %f)' % (x + 300, y, z))
    c = lua('return h2.dev_edit("point", %f, %f, %f)' % (x + 400, y, z))
    npc = lua('return h2.dev_place_npc(2350, %f, %f, %f)' % (x + 500, y, z))
    crate = lua('for _, o in ipairs(h2.objects("ZSTDOBJ")) do if o:mesh() and o:mesh() ~= 0 and not o:hidden() '
                'and o:path():find("LOC_Mansion") then return o:path() end end')
    lua('local o = h2.find([[%s]]) local x, y, z = o:pos() h2.dev_edit("move", [[%s]], x + 10, y, z) return 1'
        % (crate, crate))  # (an edit of it: one that changes nothing is not kept)
    lua('return h2.dev_level("save")')

    # the stages written as the editor writes them
    path = os.path.join(FOLDER, "level.json")
    with open(path, encoding="utf-8") as f:
        level = json.load(f)
    ids = {s["id"]: s for s in level["spawned"]}
    name = {p: p.split("/")[-1] for p in (a, b, c)}
    ids[name[b]]["stage"] = 2
    ids[name[c]]["gone_from"] = 3
    for n in level["npcs"]:
        n["stage"] = 2
    for o in level["objects"]:
        if o["path"] == crate:
            o["gone_from"] = 2
    level["stages"] = [{"name": "One", "text": "The first."}, {"name": "Two", "text": ""},
                       {"name": "Three", "text": ""}]
    level["rules"] = [{"kind": "time", "seconds": 600, "stage": 2, "broken": "note"},
                      {"kind": "unseen", "stage": 3, "broken": "fail"},
                      {"kind": "time", "seconds": 15, "stage": 3, "broken": "fail"}]
    level["replay"] = False
    level["mission"] = {"objectives": [
        {"slot": 1, "text": "Get to A", "goal": "reach", "target": a, "radius": 250, "replaces_original": True},
        {"slot": 2, "text": "Get to B", "goal": "reach", "target": b, "radius": 250, "replaces_original": True,
         "stage": 2}], "end_when_done": False, "exit": "", "exit_radius": 300}
    with open(path, "w", encoding="utf-8") as f:
        json.dump(level, f, indent=2)
    if os.path.exists(PROGRESS):
        p = json.load(open(PROGRESS, encoding="utf-8"))
        p.pop(LEVEL, None)
        json.dump(p, open(PROGRESS, "w", encoding="utf-8"))

    lua('return h2.dev_level("load", "%s")' % LEVEL)
    stage(1)
    check("stage 1: h2.stage()", lua("local s, n = h2.stage() return s .. '/' .. n") == "1/3")
    check("stage 1: its points (A, C; not B)", found(name[a]) and not found(name[b]) and found(name[c]))
    check("stage 1: the map object there", not hidden(crate))
    check("stage 1: the character of stage 2 not there", not found(npc))
    check("stage 1: no rules", lua("return #h2._stagerules()") == "0")

    stage(2)
    check("stage 2: B comes", found(name[a]) and found(name[b]) and found(name[c]))
    check("stage 2: the map object gone (hidden)", hidden(crate))
    check("stage 2: its character", found(npc))
    check("stage 2: its rule (time)", lua("local r = h2._stagerules() return #r .. r[1].kind") == "1time")

    stage(3)
    check("stage 3: C gone", found(name[a]) and found(name[b]) and not found(name[c]))
    check("stage 3: its rules and stage 2's", lua("return #h2._stagerules()") == "3")
    lua('ESC_RESULT = nil h2.on("mission", function(e) ESC_RESULT = e.result end) return 1')
    time.sleep(25)  # (its 15 s, game time)
    check("stage 3: the time up fails it", lua("return tostring(ESC_RESULT)") == "failed")

    # a saved game keeps its stage
    stage(2)
    lua("return h2.dev_savegame(%d)" % SLOT)
    time.sleep(2)
    stage(1)
    lua("return h2.dev_loadgame(%d)" % SLOT)
    ready()
    check("saved in stage 2, loaded: stage 2", lua("local s = h2.stage() return s") == "2" and found(name[b]))

    # done: the player's progress; a rule only noted: not clean
    lua("h2._stagebroken(1, 'test') return 1")
    lua('return h2.mission("complete")')
    time.sleep(3)
    p = progress()
    check("stage 2 done (not cleanly)", p.get("done") == 2 and 2 not in p.get("clean", []), str(p))
finally:
    try:
        lua(r"return h2.dev_loadscene([[C1-1\C1-1__MAIN]])")
        dev.wait_ready("C1-1__MAIN", 180)
    except Exception:  # noqa: BLE001
        pass
    shutil.rmtree(FOLDER, ignore_errors=True)
    for ext in (".1", ".2", ".h2lua"):
        try:
            os.remove(os.path.join(dev.GAME, "Save", "%d%s" % (SLOT, ext)))
        except OSError:
            pass

print("%d passed, %d failed" % (results.count(True), results.count(False)))
sys.exit(0 if all(results) else 1)
