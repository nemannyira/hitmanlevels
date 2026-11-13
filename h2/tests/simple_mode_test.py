"""Simple mode (docs/simple-mode.md, #31-#37) driven by its own controls (h2.dev_press): a level
made only through the guide on C1-1: a target from the selection card, "never be spotted", a
guard placed with a click in the world, an item, a hidden object of the map; Advanced mode and
back keep everything; saved and tried (the guard made with the map). Uses Mods/_SimpleTest
(removed after); a running game with dev commands, C1-1 loaded.

  python h2/tests/simple_mode_test.py
"""
import json
import os
import shutil
import struct
import sys
import time
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "tools"))
import dev  # noqa: E402

LEVEL = "_SimpleTest"
FOLDER = os.path.join(dev.GAME, "Mods", LEVEL)
HIDE = "Tree_02_21"
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


def doc():
    with open(os.path.join(FOLDER, "level.json"), encoding="utf-8") as f:
        return json.load(f)


def shot(name):
    lua('h2.dev_shot("simple_%s") return 1' % name)
    time.sleep(0.5)


def drawn(name):
    """the world is drawn: the game's view (the editor closed) is not one plain colour"""
    from PIL import Image  # noqa: PLC0415
    import statistics  # noqa: PLC0415
    lua('h2.dev_editor(false) return 1')
    time.sleep(1)
    shot(name)
    time.sleep(0.5)
    im = Image.open(os.path.join(dev.GAME, "Screens", "simple_%s.bmp" % name)).convert("L").resize((60, 30))
    spread = statistics.pstdev(list(im.getdata()))
    lua('h2.dev_editor(true) return 1')
    time.sleep(0.5)
    return spread > 12, "spread %.1f" % spread


if os.path.isdir(FOLDER):
    shutil.rmtree(FOLDER)
try:
    lua(r'return h2.dev_loadscene([[C1-1\C1-1__MAIN]])')
    dev.wait_ready("C1-1__MAIN", 150)
    lua('return h2.dev_level("new", "%s")' % LEVEL)
    lua('h2.dev_editor(true) return 1')
    check("simple mode on", lua('return h2.dev_mode("simple")') == "simple")
    time.sleep(1)
    check("the guide's step bar", press("1 Mission").startswith("pressed"))
    check("Advanced mode's tabs are not drawn", press("Scene", settle=0.2) == "not found")
    shot("mission")

    # an objective without its target: the Problems list's "fix it there" goes to step 1 (#36); removed then
    press("Kill someone")
    lua('h2.dev_uikey(526) return 1')  # (Escape: no target picked)
    time.sleep(0.5)
    check("step 5", press("5 Try it").startswith("pressed"))
    time.sleep(2)  # (the list is made once a second)
    check("Problems: fix it there", press("fix it there").startswith("pressed"))
    check("... the Mission step shown", press("As the original mission ends", settle=0.2).startswith("pressed"))
    # (C1-1's 4 original lines have their Remove first: the new line's is the 5th, its question's the 6th)
    check("the objective removed", press("Remove", 5).startswith("pressed") and press("Remove", 6).startswith("pressed"))

    # an original line removed (gone from the laptop, not counted), then put back
    check("an original line: Remove", press("Remove").startswith("pressed") and press("Remove it").startswith("pressed"))
    lua('return h2.dev_level("save")')
    goals = doc().get("mission", {})
    check("saved: the line taken out, the mission ends with the objectives added",
          any(o.get("hidden") and o.get("slot") == 1 for o in goals.get("objectives", [])) and goals.get("end_when_done"),
          str(goals)[:160])
    check("Put back", press("Put back").startswith("pressed"))
    lua('return h2.dev_level("save")')
    check("saved: the line back", not any(o.get("hidden") for o in doc().get("mission", {}).get("objectives", [])))
    press("As the original mission ends")

    # 1. Mission: a target from the selection card, never be spotted
    target = lua('for _, n in ipairs(h2.npcs()) do if n.object and n.object:alive() then return n.object:path() end end return ""')
    lua('h2.dev_select(%s) return 1' % json.dumps(target))
    time.sleep(0.5)
    shot("card")
    check("the card: Make it a target", press("Make it a target").startswith("pressed"))
    check("Never be spotted", press("Never be spotted").startswith("pressed"))

    # 2. People: a guard placed where the camera looks
    check("Next: People", press("Next: People >").startswith("pressed"))
    check("A civilian", press("A civilian").startswith("pressed"))
    walks = press("Walks a route")
    check("a civilian walks a route (a behaviour of another level's when the map has none)",
          walks.startswith("pressed") and "disabled" not in walks, walks)
    check("A guard", press("A guard").startswith("pressed"))
    press("Guards the spot where it stands")
    check("Place it", press("Place it").startswith("pressed"))
    w, h = (float(v) for v in lua('local w, h = h2.dev_screen() return w .. " " .. h').split())
    for i in range(3):  # (the first frames look for the surface under the mouse)
        lua('h2.dev_hover(%f, %f) return 1' % (w / 2, h * 0.72))
        time.sleep(0.5)
    shot("placing")
    lua('h2.dev_click(%f, %f) return 1' % (w / 2, h * 0.72))
    time.sleep(1.5)
    info = lua('return h2.dev_level("info")')
    check("a guard placed", "npcs=1" in info, info)
    shot("people")
    # a civilian who walks a route: placed, then its route's points clicked
    press("A civilian")
    press("Walks a route")
    check("Place it (walks a route)", press("Place it").startswith("pressed"))
    for i in range(3):
        lua('h2.dev_hover(%f, %f) return 1' % (w * 0.40, h * 0.72))
        time.sleep(0.4)
    lua('h2.dev_click(%f, %f) return 1' % (w * 0.40, h * 0.72))
    time.sleep(1.5)
    def route_point(fx):
        for i in range(2):
            lua('h2.dev_hover(%f, %f) return 1' % (w * fx, h * 0.74))
            time.sleep(0.3)
        lua('h2.dev_click(%f, %f) return 1' % (w * fx, h * 0.74))
        time.sleep(1)

    def route_list():
        lua('return h2.dev_level("save")')
        lists = [lst for lst in doc().get("lists", []) if lst.get("path", "").startswith("ROOT/Route_")]
        return lists[0].get("points", []) if lists else None

    def route_points():
        """the points clicked (the start point, where the person stands, not counted)"""
        pts = route_list()
        return -1 if pts is None else len([p for p in pts if not str(p).lower().endswith("_start")])

    start = route_list() or []
    check("the route starts where the person stands (a walker goes from its first point to the next)",
          len(start) == 1 and str(start[0]).lower().endswith("_start"), str(start))
    for fx in (0.45, 0.50, 0.55):
        route_point(fx)
    shot("route")
    n = route_points()
    check("the route's 3 points", n == 3, str(n))
    check("Take back the last one", press("Take back the last one").startswith("pressed"))
    n = route_points()
    check("... 2 points", n == 2, str(n))
    check("the route done", press("Done (Esc)").startswith("pressed"))
    info = lua('return h2.dev_level("info")')
    check("the civilian placed with a route of its own", "npcs=2" in info and "lists=1" in info, info)
    # its route changed later: the level's people's "Route"
    check("the people's Route", press("Route").startswith("pressed") and
          press("Take back the last one", settle=0.1).startswith("pressed"))  # (its box shown: one taken back)
    route_point(0.50)
    route_point(0.55)
    n = route_points()
    check("... points added", n == 3, str(n))
    lua('h2.dev_uikey(526) return 1')  # (Esc: done)
    time.sleep(0.5)

    # 3. Items: searched; a hovered one's picture (its clone drawn into the thumbnail)
    check("Next: Items", press("Next: Items >").startswith("pressed"))
    press("##itemfilter", settle=0.2)
    lua('h2.dev_type("key") return 1')
    time.sleep(0.6)
    check("the search: no weapon with key", press("Gun Hardballer", settle=0.2) == "not found")
    found = press("Key Door", settle=0.1)
    lua('h2.dev_uikey(526) return 1')  # (the click started placing it)
    check("the search: Key Door", found.startswith("pressed"), found)
    if found.startswith("pressed"):
        _, kx, ky = found.split()
        for _ in range(12):
            lua('h2.dev_hover(%d, %s) return 1' % (int(kx) - 60, ky))
            time.sleep(0.2)
        check("its picture", lua("return tostring(h2.dev_thumbnail())") == "true")
    for _ in range(3):
        lua('h2.dev_uikey(523) return 1')  # (the search emptied)
    shot("items")

    # 4. The map: an object of the map hidden from its card
    check("Next: The map", press("Next: The map >").startswith("pressed"))
    lua('h2.dev_select(h2.find("%s"):path()) return 1' % HIDE)
    time.sleep(0.5)
    check("the card: Hide", press("Hide (H)").startswith("pressed"))
    check("the object hidden", lua('return tostring(h2.find("%s"):hidden())' % HIDE) == "true")
    # a door of the map locked from its card (#35), listed with its undo
    door = lua('local hx, hy, hz = h2.player():pos() local best, bd = nil, 1e18 for _, o in ipairs(h2.objects("ZGEOM")) do '
               'if o:locked() == false then local x, y, z = o:pos() local d = (x-hx)^2 + (y-hy)^2 + (z-hz)^2 '
               'if d < bd then best, bd = o, d end end end return best and best:path() or "none"', 60)
    lua('h2.dev_select([[%s]]) return 1' % door)
    time.sleep(0.5)
    check("the card: Lock the door", press("Lock the door").startswith("pressed")
          and lua('return tostring(h2.find([[%s]]):locked())' % door) == "true", door[-40:])
    shot("map")

    # Advanced mode and back: nothing lost
    check("Advanced mode", press("Advanced mode").startswith("pressed") and lua("return h2.dev_mode()") == "advanced")
    check("Advanced mode's toolbar: Simple mode", press("Simple mode").startswith("pressed")
          and lua("return h2.dev_mode()") == "simple")

    # 6. Save
    check("step 6", press("6 Save & share").startswith("pressed"))
    check("Save", press("Save (Ctrl+S)").startswith("pressed"))
    d = doc()
    goals = [o.get("goal") for o in d.get("mission", {}).get("objectives", [])]
    check("saved: the target and never spotted", "kill" in goals and "unseen" in goals, str(goals))
    check("saved: the guard and the civilian", len(d.get("npcs", [])) == 2)
    route = [lst for lst in d.get("lists", []) if lst.get("path", "").startswith("ROOT/Route_")]
    check("saved: the civilian's route and its points", route and len(route[0].get("points", [])) >= 1,
          str(route)[:120])
    check("saved: the hidden object", any(o.get("hidden") for o in d.get("objects", [])), str(d.get("objects"))[:120])
    check("saved: the locked door", d.get("doors") == [{"path": door, "locked": True}], str(d.get("doors"))[:120])
    # the picture of the view (#36): a PNG a package takes
    check("Take a picture", press("Take a picture of this view").startswith("pressed"))
    time.sleep(1.5)
    png_ok, png_detail = False, "none"
    try:
        with open(os.path.join(FOLDER, "preview.png"), "rb") as f:
            png = f.read()
        w_, h_ = struct.unpack(">II", png[16:24])
        at, idat = 8, b""
        while at < len(png):
            n, kind = struct.unpack(">I4s", png[at:at + 8])
            if kind == b"IDAT":
                idat += png[at + 8:at + 8 + n]
            at += 12 + n
        png_ok = png[:8] == b"\x89PNG\r\n\x1a\n" and w_ == 640 and len(zlib.decompress(idat)) == h_ * (w_ * 3 + 1)
        png_detail = "%d x %d, %d bytes" % (w_, h_, len(png))
    except (OSError, ValueError, struct.error, zlib.error) as e:
        png_detail = str(e)
    check("preview.png: 640 wide, its pixels", png_ok, png_detail)
    # the picture is the game's view, not the screen (#48: the front buffer, read before, is the whole screen
    # in a window; the game runs behind other windows here): close to the same view shot from the back buffer
    if png_ok:
        from PIL import Image, ImageChops, ImageStat  # noqa: PLC0415
        lua('h2.dev_editor(false) return 1')
        time.sleep(1)
        shot("view")
        time.sleep(0.5)
        view = Image.open(os.path.join(dev.GAME, "Screens", "simple_view.bmp")).convert("RGB").resize((w_, h_))
        pic = Image.open(os.path.join(FOLDER, "preview.png")).convert("RGB")
        diff = sum(ImageStat.Stat(ImageChops.difference(view, pic)).mean) / 3
        check("preview.png is the game's view, not the screen", diff < 30, "mean difference %.1f" % diff)
        lua('h2.dev_editor(true) return 1')
        time.sleep(0.5)
    shot("share")

    # 5. Try it: the map again with the guard
    check("step 5", press("5 Try it").startswith("pressed"))
    check("Play from the start", press("Play from the start").startswith("pressed"))
    dev.wait_ready("C1-1__MAIN", 150)
    time.sleep(3)
    guard = d["npcs"][0].get("id", "") if d.get("npcs") else ""
    real = lua('local o = h2.find(%s) return tostring(o ~= nil and o:alive())' % json.dumps(guard))
    check("the guard made with the map", real == "true", guard + ": " + real)
    walker = d["npcs"][1].get("id", "") if len(d.get("npcs", [])) > 1 else ""
    states = []
    for _ in range(4):  # (the world runs: Try it closed the editor)
        states.append(lua('local o = h2.find(%s) return o and o:state() or "none"' % json.dumps(walker)))
        time.sleep(1.5)
    check("the civilian walks its route", any("walk" in st.lower() for st in states), walker + ": " + ", ".join(states))
    shot("playing")

    def backs():
        with open(os.path.join(dev.GAME, "H2Fix.log"), encoding="latin1", errors="replace") as f:
            return f.read().count("test: back in the editor")

    # the editor key while playing: the map loaded again as edited, the editor open on it
    before = backs()
    lua('return h2.dev_wmkey(0x77)')  # (F8)
    for _ in range(90):
        time.sleep(1)
        if backs() > before:
            break
    check("F8 while playing: back in the editor", backs() > before)
    dev.wait_ready("C1-1__MAIN", 60)
    # removed again (People step), then the level reverted: the world still drawn (a removed character
    # taken out of the actors' collision lists left it undrawn)
    lua('h2.dev_editor(true) h2.dev_mode("simple") return 1')
    time.sleep(1)
    press("2 People")
    check("the guard removed", press("Remove").startswith("pressed"))
    ok, detail = drawn("removed")
    check("the world drawn after the removal", ok, detail)
    lua('return h2.dev_level("revert")')
    time.sleep(1)
    ok, detail = drawn("reverted")
    check("the world drawn after the level reverted", ok, detail)
finally:
    dev.run('h2.dev_mode("advanced") h2.dev_editor(false) return h2.dev_level("revert")', 10)
    shutil.rmtree(FOLDER, ignore_errors=True)
print("\n%d passed, %d failed" % (sum(results), len(results) - sum(results)))
sys.exit(0 if all(results) else 1)
