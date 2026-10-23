"""The in-game map (M) of levels on other maps than C1-1 (editor/ingame_map.h):
per map a level with its models on the map, a copy of a plain model near
Hitman and a point beside him with an icon; the map loaded again with it, the
copy's plan model is on a floor of the map, the point's icon on Hitman's
floor, the open map draws Hitman and the point's icon. Then a map without
buildings on C2-1: the map's window works, the old plans' fixed icons and the
map's characters are off it, the level's floor is drawn. Screenshots of the
open map: dumps/shots/map_maps_*.png. Uses Mods/_MapMapsTest (removed); a
running game with dev commands.

  python tools/tests/map_maps_test.py [C2-1 C3-3 ...]
"""
import os
import shutil
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, ".."))
import dev  # noqa: E402

LEVEL = "_MapMapsTest"
FOLDER = os.path.join(dev.GAME, "Mods", LEVEL)
LOG = os.path.join(dev.GAME, "H2Fix.log")
MAPS = ["C2-1", "C3-3", "C4-1", "C6-1", "C7-1"]  # 11, 10, 3, 1 and 11 floors
BLANK_MAP = "C2-1"
# a place on a floor whose group is moved and turned (the mansion of C2-1): a model of its location
TURNED = {"C2-1": ("C2-1_LOC_Mansion_01_1stFloor", "MAP_Mansion_1stFloor")}
DIK_M = 0x32
results = []

# the nearest plain model to Hitman within 25 m -> its path (as maps_gameway_test)
PLAIN_MODEL = r"""
local x, y, z = h2.player():pos()
local found = {}
for _, o in ipairs(h2.objects("ZSTDOBJ")) do
  if o:mesh() ~= 0 and not o:isa("ZActor") and not o:isa("ZItem") and not o:hidden() and #o:children() == 0
     and not o:path():find("HeroSetup") and not o:path():find("__MAP.zip") then
    local ox, oy, oz = o:pos()
    local d = (ox - x) ^ 2 + (oy - y) ^ 2 + (oz - z) ^ 2
    if d < 2500 * 2500 then found[#found + 1] = {d, o:path()} end
  end
end
table.sort(found, function(a, b) return a[1] < b[1] end)
local out = {}
for i = 1, math.min(#found, 10) do out[i] = found[i][2] end
return table.concat(out, "\n")
"""


def copy_near(dx, dz):
    """a copy of the nearest of the map's models the editor copies (not a character's body) beside it
    -> (its name, its place) or (None, None)"""
    for source in [p for p in lua(PLAIN_MODEL).splitlines() if p]:
        x, y, z = (float(v) for v in lua('local x, y, z = h2.find([[%s]]):pos() return x .. " " .. y .. " " .. z'
                                         % source).split())
        out = dev.run('return h2.dev_edit("dup", [[%s]], %f, %f, %f)' % (source, x + dx, y, z + dz), 30)
        if not out.startswith("error") and "timeout" not in out:
            return out.rsplit("/", 1)[-1], [x + dx, y, z + dz]
    return None, None


def lua(code, timeout=30):
    out = dev.run(code, timeout)
    if out.startswith("error") or "timeout" in out:
        raise RuntimeError(out)
    return out


def check(name, ok, detail=""):
    results.append(bool(ok))
    print("%s %s %s" % ("PASS" if ok else "FAIL", name, detail))


def wait_map(scene):
    time.sleep(4)
    t = time.time()
    while time.time() - t < 180:
        if (scene + "__MAIN").lower() in dev.run("return h2.scene()", 5).lower():
            break
        time.sleep(1)
    time.sleep(8)


def load(scene, level=None):
    if level:
        lua('return h2.dev_level("load", "%s")' % level)
    lua(r'return h2.dev_loadscene([[%s\%s__MAIN]])' % (scene, scene))
    wait_map(scene)


def size():
    return os.path.getsize(LOG)


def since(at):
    with open(LOG, encoding="latin1") as f:
        f.seek(at)
        return f.read()


def icon_of(find):
    """h2.dev_mapicon of the object found by that Lua expression: (icon, floor)"""
    out = lua('local o = %s if not o then return "missing|" end local a, b = h2.dev_mapicon(o:path()) '
              'return a .. "|" .. b' % find)
    return tuple(out.split("|", 1))


def floor_of(name):
    return lua('local o = h2.find("%s") if not o or not o:parent() then return "" end '
               'local p = o:parent() if h2.dev_routine(p:path(), "MapGroup") == "none" then return "" end '
               'return p:name()' % name)


def open_map(shot):
    """the map opened with M (a level's start may not let it open yet: tried for 30 s), what it draws, closed"""
    for attempt in range(7):
        lua("h2.dev_key(%d, 0.1) return 1" % DIK_M)
        time.sleep(2.5)
        out = lua("return (h2.dev_mapicons())")
        if out.splitlines()[0] != "floor":  # (open: a floor shown)
            dev.shot(shot)
            lua("h2.dev_key(%d, 0.1) return 1" % DIK_M)
            time.sleep(1.5)
            return out
        time.sleep(2.5)
    return out


def run_map(scene):
    print("\n== " + scene)
    shutil.rmtree(FOLDER, ignore_errors=True)
    load(scene)
    lua('return h2.dev_level("new", "%s")' % LEVEL)
    lua('return h2.dev_level("mapmodels", "on")')
    copy, _ = copy_near(200.0, 100.0)
    at = lua('local x, y, z = h2.player():pos() return string.format("%.0f, %.0f, %.0f", x + 150, y, z)')
    point = lua('return h2.dev_edit("point", %s)' % at).rsplit("/", 1)[-1]
    lua('return h2.dev_level("mapicon", [[ROOT/%s|Icon_PointOfInterest]])' % point)
    turned = None
    if scene in TURNED:
        at2 = lua('for _, o in ipairs(h2.objects("ZSTDOBJ")) do if o:mesh() ~= 0 and o:path():find("%s", 1, true) then '
                  'local x, y, z = o:pos() return string.format("%%.0f, %%.0f, %%.0f", x, y + 50, z) end end return ""'
                  % TURNED[scene][0])
        check(scene + ": a model of the mansion found", at2 != "", TURNED[scene][0])
        if at2:
            turned = lua('return h2.dev_edit("point", %s)' % at2).rsplit("/", 1)[-1]
            lua('return h2.dev_level("mapicon", [[ROOT/%s|Icon_Exit]])' % turned)
    lua('return h2.dev_level("save")')
    mark = size()
    load(scene, LEVEL)
    log = since(mark)
    check(scene + ": no exception", "EXCEPTION" not in log)
    hero = icon_of("h2.player()")
    check(scene + ": Hitman on a floor", hero[1] != "", hero)
    mine = icon_of('h2.find("%s_MapIcon")' % point)
    check(scene + ": the point's icon on Hitman's floor", mine == ("Icon_PointOfInterest shown", hero[1]), mine)
    places = lua('local a, b = h2.find("%s_MapIcon"), h2.find("ROOT/%s") local ax, _, az = a:pos() local bx, _, bz = b:pos() '
                 'return string.format("%%.0f %%.0f %%.0f %%.0f", ax, az, bx, bz)' % (point, point)).split()
    check(scene + ": the icon where the point is (x, z)", places[:2] == places[2:], places)
    if turned:
        icon = icon_of('h2.find("%s_MapIcon")' % turned)
        check(scene + ": a point in the mansion: its floor (moved and turned)", icon[1] == TURNED[scene][1], icon)
        where = lua('local a, b = h2.find("%s_MapIcon"), h2.find("ROOT/%s") local ax, _, az = a:pos() '
                    'local bx, _, bz = b:pos() return string.format("%%.0f %%.0f %%.0f %%.0f", ax, az, bx, bz)'
                    % (turned, turned)).split()
        check(scene + ": its icon where the point is (x, z)", where[:2] == where[2:], where)
    if copy:
        plan = floor_of(copy + "_Map")
        check(scene + ": the copy drawn on a floor", plan != "", plan)
    drawn = open_map("map_maps_" + scene)
    check(scene + ": the map draws Hitman", "Icon_Hero" in drawn, drawn.splitlines()[:2])
    check(scene + ": the map draws the point's icon", ("/%s_MapIcon" % point) in drawn,
          [l for l in drawn.splitlines() if "MapIcon" in l][:3])
    lua('return h2.dev_level("revert")')


def run_blank(scene):
    print("\n== blank " + scene)
    shutil.rmtree(FOLDER, ignore_errors=True)
    load(scene)
    plain = lua('local o = h2.find("Icon_Hero") return tostring(o and o:hidden())')
    lua('return h2.dev_level("newblank", "%s")' % LEVEL)
    load(scene, LEVEL)
    lua('return h2.dev_level("save")')  # (its floor placed)
    mark = size()
    load(scene, LEVEL)
    check("blank " + scene + ": no exception", "EXCEPTION" not in since(mark))
    check("blank " + scene + ": the legend as on the plain map",
          lua('local o = h2.find("Icon_Hero") return tostring(o and o:hidden())') == plain)
    drawn = open_map("map_maps_blank_" + scene)
    check("blank " + scene + ": the map draws Hitman", "Icon_Hero" in drawn, drawn.splitlines()[:2])
    check("blank " + scene + ": none of the old plans' fixed icons", "/MapIcon_" not in drawn,
          [l for l in drawn.splitlines() if "/MapIcon_" in l][:2])
    others = [l for l in drawn.splitlines()[1:] if l and "Icon_Hero" not in l]
    check("blank " + scene + ": none of the map's characters", not others, others[:3])
    lua('return h2.dev_level("revert")')


def refuse_character_copy():
    """a character's model (Hitman's body) cannot be duplicated: its copy as a model crashed the renderer"""
    body = lua("return h2.player():path()")  # (Hitman's object is his body: "Hitman2/Ground")
    out = dev.run('return h2.dev_edit("dup", [[%s]], 0, 0, 0)' % body, 15)
    check("a character's model is not copied", "parts of characters cannot be copied" in out, out[:100])


try:
    load("C1-1")
    lua('return h2.dev_level("new", "%s")' % LEVEL)
    refuse_character_copy()
    lua('return h2.dev_level("revert")')
    for m in sys.argv[1:] or MAPS:
        try:
            run_map(m)
        except RuntimeError as e:
            check(m + ": ran through", False, str(e)[:200])
            if not dev.running():
                break
    if dev.running() and not sys.argv[1:]:
        run_blank(BLANK_MAP)
finally:
    dev.run('return h2.dev_level("revert")', 10)
    shutil.rmtree(FOLDER, ignore_errors=True)
print("\n%d passed, %d failed" % (sum(results), len(results) - sum(results)))
