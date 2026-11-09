"""A placed walker goes along its route's points in order (the night of 2026-11-01: the WayPointList walkers
picked their next point at random unless isWPListPath is 1). On C1-1: a civilian with the walker the
Simple mode's "Walks a route" takes (afghanPedestrian of C5-2), a new route of the level with 5 points on the
path network around the map's Delivery Boy, saved, the map loaded again; then the points it reaches (within
1.3 m) are followed for a minute: each one must be the next of the one before (in a loop). Uses
Mods/_RouteOrder (removed); a running game with dev commands.

  python h2/tests/route_order_test.py
"""
import json
import math
import os
import shutil
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "tools"))
import dev  # noqa: E402

LEVEL = "_RouteOrder"
FOLDER = os.path.join(dev.GAME, "Mods", LEVEL)
WALKER = "afghanPedestrian (C5-2)"
REACHED = 130.0  # cm, flat (as the debug view's route tracking)
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
    lua(r'return h2.dev_loadscene([[C1-1\C1-1__MAIN]])')
    dev.wait_ready("C1-1__MAIN", 150)
    time.sleep(3)


def xyz(text):
    return [float(v) for v in text.split(",")]


if os.path.isdir(FOLDER):
    shutil.rmtree(FOLDER)
try:
    load()
    lua('return h2.dev_level("new", "%s")' % LEVEL)
    # around the map's Delivery Boy (he walks there): 5 places of the path network on a ring of 8 m
    center = xyz(lua('for _, n in ipairs(h2.npcs()) do if n.model == "DeliveryBoy" and n.object then '
                     'local x, y, z = n.object:pos() return x .. "," .. y .. "," .. z end end return ""'))
    record = lua('for _, n in ipairs(h2.npcs()) do if n.model == "DeliveryBoy" then return tostring(n.record) end end '
                 'return "-1"')
    places = []
    for k in range(5):
        a = 2 * math.pi * k / 5
        x, z = center[0] + 800 * math.sin(a), center[2] + 800 * math.cos(a)
        p = lua('local x, y, z = h2.walkable(%f, %f, %f, 800) return x and (x .. "," .. y .. "," .. z) or ""'
                % (x, center[1], z))
        if p:
            places.append(xyz(p))
    check("5 places on the path network", len(places) == 5, str(len(places)))

    # the walker in the middle, its route as Simple mode makes it: a start point where it stands (the game's walker
    # takes the first point as its own place, ZConditionList_GetNextLocation), then the 5 places
    home = xyz(lua('local x, y, z = h2.walkable(%f, %f, %f, 800) return x .. "," .. y .. "," .. z' % tuple(center)))
    walker = lua('return h2.dev_place_npc(%s, %f, %f, %f, %s)' % (record, home[0], home[1], home[2], json.dumps(WALKER)))
    route = lua('return h2.dev_level("newroute", "RO_Route")')
    start = lua('return h2.dev_edit("point", %f, %f, %f)' % tuple(home))
    start = lua('return h2.dev_edit("rename", %s, "RO_Route_start")' % json.dumps(start))
    pts = [lua('return h2.dev_edit("point", %f, %f, %f)' % tuple(p)) for p in places]
    lua('return h2.dev_level("listpoints", %s)' % json.dumps("|".join([route, start] + pts)))
    lua('return h2.dev_edit("npcref", h2.find(%s):path(), "WayPointList", [[%s]])' % (json.dumps(walker), route))
    lua('return h2.dev_level("save")')
    with open(os.path.join(FOLDER, "level.json"), encoding="utf-8") as f:
        npc = (json.load(f).get("npcs") or [{}])[0]
    longs = {p["name"]: p["value"] for p in npc.get("params", []) if p.get("type") == "LONG"}
    check("placed to walk its points in order", longs.get("isWPListPath") == "1" and
          longs.get("startAtRandomPathPoint", "0") == "0", str(longs))

    load()
    w = 'h2.find(%s)' % json.dumps(walker)
    reached = []
    t0 = time.time()
    while time.time() - t0 < 60:
        pos = xyz(lua('local x, y, z = %s:pos() return x .. "," .. y .. "," .. z' % w))
        for k, p in enumerate(places):
            if math.hypot(pos[0] - p[0], pos[2] - p[2]) < REACHED and (not reached or reached[-1] != k):
                reached.append(k)
        time.sleep(0.25)
    order = " ".join(str(k + 1) for k in reached)
    check("it reached at least 4 points", len(reached) >= 4, order)
    check("the first point clicked is the first it walks to", bool(reached) and reached[0] == 0, order)
    # (in a loop the start point, where it stood, comes between point 5 and point 1: passing near it counts)
    wrong = [(a + 1, b + 1) for a, b in zip(reached, reached[1:]) if b != (a + 1) % len(places)]
    check("each point the next of the one before", not wrong, "%s (out of order: %s)" % (order, wrong))
finally:
    try:
        lua('return h2.dev_level("revert")')
    except RuntimeError:
        pass
    shutil.rmtree(FOLDER, ignore_errors=True)
print("\n%d passed, %d failed" % (sum(results), len(results) - sum(results)))
sys.exit(0 if all(results) else 1)
