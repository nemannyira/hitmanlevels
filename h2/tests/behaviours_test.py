"""Behaviours for new characters on a map that is not C1-1 (issues #10, #11), on
C2-2 Kirov Park:

* the map's own behaviours are offered (EntranceGuard, Guard22, PatrolGuard22)
  and start on a character made now; the story parts (Boss, Bodyguard) are told;
* BaseGuard (C1-1) gets the map's guard group for the m_PanicArbiter C2-2
  does not have, and no other character's guard point (it walked 90 m to a
  limousine guard's): it starts;
* a placed patrol without its route is a Problem; a new route of the level
  (the panel's New route) is made at once, saved, made with the map again,
  and the patrol walks it.

Uses Mods/_BehTest (removed); a running game with dev commands.

  python h2/tests/behaviours_test.py
"""
import json
import os
import shutil
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "tools"))
import dev  # noqa: E402

LEVEL = "_BehTest"
FOLDER = os.path.join(dev.GAME, "Mods", LEVEL)
SCENE = r"C2-2\C2-2__MAIN"
LIMO_POINT = "ROOT/C2-2/C2-2_CHARACTERS.zip/Soldiers/Limo/PosGuard2"
results = []


def lua(code, timeout=30):
    out = dev.run(code, timeout)
    if out.startswith("error") or "timeout" in out:
        raise RuntimeError(out)
    return out


def check(name, ok, detail=""):
    results.append(bool(ok))
    print("%s %s %s" % ("PASS" if ok else "FAIL", name, detail))


def load():
    lua(r'return h2.dev_loadscene([[%s]])' % SCENE)
    dev.wait_ready("C2-2__MAIN", 150)
    time.sleep(3)


def state(expr):
    return lua('local o = %s return tostring(o and (o:state()))' % expr)


if os.path.isdir(FOLDER):
    shutil.rmtree(FOLDER)
try:
    load()
    lua('return h2.dev_level("new", "%s")' % LEVEL)
    names = lua('return table.concat(h2.behaviours(), ",")').split(",")
    check("the map's own guards offered", all(b in names for b in ("EntranceGuard", "Guard22", "PatrolGuard22")),
          ",".join(names))

    # made now (h2.npc takes behaviours as the panel does)
    lua('BT_E = h2.npc("SoldierRussian_01", 18000, 0, -5800, 0, "EntranceGuard") '
        'BT_B = h2.npc("BodyguardMafiaRussian_01", 18300, 0, -5800, 0, "BaseGuard (C1-1)") return 1')
    params = lua('local _, p = BT_B:behaviour() local function n(o) return o and o:path() or "none" end '
                 'return n(p.m_PanicArbiter) .. "|" .. n(p.GuardPoint)').split("|")
    check("BaseGuard: the map's guard group as its panic arbiter", params[0].endswith("Arbiter"), params[0])
    check("BaseGuard: a guard point where it stands, no other character's", params[1] != LIMO_POINT and
          "GuardPoint_npc" in params[1], params[1])
    time.sleep(6)
    check("EntranceGuard (own) starts", state("BT_E").startswith("Guard2."), state("BT_E"))
    check("BaseGuard (C1-1) starts", state("BT_B").startswith("BaseGuard_C11."), state("BT_B"))
    away = float(lua('local x, y, z = BT_B:pos() return math.sqrt((x - 18300)^2 + (z + 5800)^2)'))
    check("BaseGuard stays at its place", away < 300, "%.0f cm away" % away)

    # a placed patrol without a route, then a new route of the level for it
    record = lua('for _, n in ipairs(h2.npcs()) do if n.model == "BodyguardMafiaRussian_01" then '
                 'return tostring(n.record) end end return "-1"')
    patrol = lua('return h2.dev_place_npc(%s, 18600, 0, -5800, "afghanPatrolGuard (C5-3)")' % record)
    time.sleep(2.5)  # (the list is made once a second)
    problems = lua('return h2.dev_level("problems")')
    check("Problems: a patrol without its route", "does not start without WayPoints" in problems, problems[:200])
    route = lua('return h2.dev_level("newroute", "BT_Route")')
    check("a new route made now", lua('return tostring(h2.find([[%s]]) ~= nil)' % route) == "true", route)
    pts = [lua('return h2.dev_edit("point", %d, 0, %d)' % (x, z)) for x, z in ((18600, -5400), (18200, -6400))]
    lua('return h2.dev_level("listpoints", %s)' % json.dumps("|".join([route] + pts)))
    count = lua('return #h2.find([[%s]]):points()' % route)
    check("its points set now", count == "2", count)
    lua('return h2.dev_edit("npcref", h2.find(%s):path(), "WayPoints", [[%s]])' % (json.dumps(patrol), route))
finally:
    pass

saved = {}
try:
    lua('return h2.dev_level("save")')
    with open(os.path.join(FOLDER, "level.json"), encoding="utf-8") as f:
        saved = json.load(f)
    lists = [l["name"] for l in saved.get("new_lists", [])]
    check("saved: the route as a list of the level", "BT_Route" in lists, lists)
    entry = {l["path"]: l["points"] for l in saved.get("lists", [])}
    check("saved: its points", len(entry.get(route, [])) == 2, entry.get(route))
    load()
    check("made with the map again", lua('return tostring(h2.find([[%s]]) ~= nil)' % route) == "true")
    time.sleep(3)
    p = 'h2.find(%s)' % json.dumps(patrol)
    before = lua('local x, y, z = %s:pos() return string.format("%%.0f %%.0f", x, z)' % p)
    time.sleep(8)
    after = lua('local x, y, z = %s:pos() return string.format("%%.0f %%.0f", x, z)' % p)
    check("the patrol walks its route", state(p).startswith("PatrolGuard.") and before != after,
          "%s %s -> %s" % (state(p), before, after))
finally:
    try:
        lua('return h2.dev_level("revert")')
    except RuntimeError:
        pass
    if os.path.isdir(FOLDER):
        shutil.rmtree(FOLDER)

print("\n%d passed, %d failed" % (results.count(True), results.count(False)))
sys.exit(0 if all(results) else 1)
