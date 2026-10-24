"""The editor's own controls pressed by their labels (h2.dev_press: ImGui's test
engine hooks, a click in the control's middle), the way a user makes a level:
select a model, Duplicate, "Hitman has to take it", Save, Undo / Redo, the
Problems list's "onto the paths" for a point beside the path network, Test
(save, the map again, the editor closed): the copy is then made with the map
and the game offers its action on it. Uses Mods/_GuiTest (removed after); a
running game with dev commands, C1-1 loaded.

  python tools/tests/gui_test.py
"""
import json
import os
import shutil
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, ".."))
import dev  # noqa: E402

LEVEL = "_GuiTest"
FOLDER = os.path.join(dev.GAME, "Mods", LEVEL)
SOURCE = "Tree_02_21"
results = []


def lua(code, timeout=30):
    out = dev.run(code, timeout)
    if out.startswith("error") or "timeout" in out:
        raise RuntimeError(out)
    return out


def check(name, ok, detail=""):
    results.append(bool(ok))
    print("%s %s %s" % ("PASS" if ok else "FAIL", name, detail))


def press(label, n=1, settle=1.0):
    """presses the control of that label; its status ("pressed x y" / "not found")"""
    lua('h2.dev_press(%s, %d) return 1' % (json.dumps(label), n))
    status = "looking"
    t = time.time()
    while time.time() - t < 5 and status == "looking":
        time.sleep(0.2)
        status = lua("return h2.dev_press_status()")
    time.sleep(settle)  # (the click itself: four frames)
    return status


def info():
    return lua('return h2.dev_level("info")')


def wait_map():
    dev.wait_ready("C1-1__MAIN", 150)


# a place off the path network near one of the map's characters (they stand on it) with a place
# of it within 7 m (the Problems list then offers "onto the paths")
OFF_NETWORK = r"""
for _, n in ipairs(h2.npcs()) do
  local o = n.object
  if o and h2.onpath(o) then
    local x, y, z = o:pos()
    for r = 200, 900, 100 do
      for a = 0, 330, 30 do
        local px, pz = x + r * math.cos(math.rad(a)), z + r * math.sin(math.rad(a))
        if h2.onpath(px, y, pz) == false and h2.walkable(px, y, pz, 700) then
          return string.format("%.0f %.0f %.0f", px, y, pz)
        end
      end
    end
  end
end
return "none"
"""

if os.path.isdir(FOLDER):
    shutil.rmtree(FOLDER)
try:
    lua(r'return h2.dev_loadscene([[C1-1\C1-1__MAIN]])')
    wait_map()
    lua('return h2.dev_level("new", "%s")' % LEVEL)
    lua('h2.dev_editor(true) h2.dev_panel("Scene") h2.dev_panel("Inspector") h2.dev_select(h2.find("%s"):path()) return 1' % SOURCE)
    time.sleep(1)
    check("unknown label: not found", press("No such button 123", settle=0.2) == "not found")
    status = press("Duplicate (Ctrl+D)")
    check("Duplicate pressed", status.startswith("pressed"), status)
    check("a copy in the level", "spawned=1" in info(), info())
    copy = lua("return h2.dev_selected() or 'none'")  # (the copy is selected)
    name = copy.rsplit("/", 1)[-1]
    check("the copy selected", name.startswith(SOURCE + "+"), copy)
    gone = 'local o = h2.find(%s) return tostring(o == nil or o:hidden())' % json.dumps(name)
    status = press("Undo")
    check("Undo: the copy gone", status.startswith("pressed") and lua(gone) == "true", status)
    status = press("Redo")
    check("Redo: the copy back", status.startswith("pressed") and lua(gone) == "false", status)
    lua('h2.dev_select(h2.find(%s):path()) return 1' % json.dumps(name))
    status = press("Hitman has to take it")
    check("take goal pressed", status.startswith("pressed"), status)
    # a point beside the path network: the Problems list offers to put it on the paths
    spot = lua(OFF_NETWORK)
    check("a place off the path network near a character", spot != "none", spot)
    if spot != "none":
        point = lua('return h2.dev_edit("point", %s)' % spot.replace(" ", ", "))
        lua('h2.dev_panel("Level") return 1')
        time.sleep(2.5)  # (the list is made once a second)
        status = press("onto the paths")
        check("onto the paths pressed", status.startswith("pressed"), status)
        time.sleep(1.5)
        on = lua('return tostring(h2.onpath(h2.find(%s)))' % json.dumps(point))
        check("the point on the path network", on == "true", on)
        status = press("Undo")
        on = lua('return tostring(h2.onpath(h2.find(%s)))' % json.dumps(point))
        check("Undo: the point off it again", status.startswith("pressed") and on == "false", on)
        press("Redo")
    # the in-game map's controls: a point's icon (Inspector > On the map), the level's models (Level panel)
    near = lua('local x, y, z = h2.player():pos() return string.format("%.0f, %.0f, %.0f", x + 200, y, z)')
    map_point = lua('return h2.dev_edit("point", %s)' % near)
    lua('h2.dev_select(%s) h2.dev_panel("Inspector") return 1' % json.dumps(map_point))
    time.sleep(0.5)
    check("On the map: the menu opened", press("On the map (M)").startswith("pressed"))
    check("On the map: PointOfInterest picked", press("PointOfInterest").startswith("pressed"))
    lua('h2.dev_panel("Level") return 1')
    time.sleep(0.5)
    check("the level's models on the map: ticked", press("The level's models on the map (M)").startswith("pressed"))
    status = press("Save (Ctrl+S)")
    check("Save pressed", status.startswith("pressed") and "(unsaved)" not in info(), status + " / " + info())
    doc = json.load(open(os.path.join(FOLDER, "level.json"), encoding="utf-8"))
    goals = doc.get("mission", {}).get("objectives", [])
    map_name = map_point.rsplit("/", 1)[-1]
    check("saved: the point's icon", any(e.get("id") == map_name and e.get("map_icon") == "Icon_PointOfInterest"
                                         for e in doc.get("spawned", [])))
    check("saved: the level's models on the map", doc.get("map_models") is True)
    check("saved: the copy, its record and its take goal",
          any(s.get("record", -1) >= 0 for s in doc.get("spawned", [])) and any(o.get("goal") == "take" for o in goals),
          "%d objects, goals %s" % (len(doc.get("spawned", [])), [o.get("goal") for o in goals]))
    status = press("Test")
    check("Test pressed (save, the map again, the editor closed)", status.startswith("pressed"), status)
    wait_map()
    rec = lua('local o = h2.find(%s) if not o then return "none" end local r, k = h2.dev_record(o:path()) return k' % json.dumps(name))
    check("the copy made with the map", rec == "copy", rec)
    action = lua('return h2.dev_routine(h2.find(%s):path(), "Action")' % json.dumps(name))
    check("the game's action on it", "+72 00000006" in action, action[:80])
    icon = lua('local a, b = h2.dev_mapicon(h2.find("%s_MapIcon"):path()) return a .. "|" .. b' % map_name)
    check("the point's icon on the map", icon == "Icon_PointOfInterest shown|MAP_Outside", icon)
    plan = lua('local o = h2.find(%s) return o and o:parent() and o:parent():name() or ""' % json.dumps(name + "_Map"))
    check("the copy drawn on the map", plan == "MAP_Outside", plan)
finally:
    dev.run('h2.dev_editor(false) return h2.dev_level("revert")', 10)
    shutil.rmtree(FOLDER, ignore_errors=True)
print("\n%d passed, %d failed" % (sum(results), len(results) - sum(results)))
