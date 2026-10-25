"""Made with the map on other maps than C1-1: per map a level with a copy of
one of the map's plain models near Hitman, a point and (where the map has an
armed guard) another weapon for a guard (npc_edits) is saved; the map loaded
again with it, the copy and the point are records of the scene where they
were put, the guard is made with the weapon, no exception; a saved game
places every pointer and loads. A map without what is needed (no plain model
near Hitman, no point template) is reported, not failed. Uses
Mods/_MapsGameWayTest and save slot 9 (removed / overwritten: back up the
saves first); a running game with dev commands.

  python h2/tests/maps_gameway_test.py [C2-1 C3-1 ...]
"""
import json
import os
import shutil
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "tools"))
import dev  # noqa: E402

LEVEL = "_MapsGameWayTest"
FOLDER = os.path.join(dev.GAME, "Mods", LEVEL)
LOG = os.path.join(dev.GAME, "H2Fix.log")
SLOT = 9
MAPS = ["C2-1", "C3-1", "C4-1", "C5-1", "C6-1", "C7-1", "C8-1"]  # one of each chapter
results = []
notes = []

# the nearest plain model to Hitman (a mesh, no children, not a character or an item, shown)
# within 25 m -> its path
PLAIN_MODEL = r"""
local x, y, z = h2.player():pos()
local found = {}
for _, o in ipairs(h2.objects("ZSTDOBJ")) do
  pcall(function()  -- (an object gone from the scene meanwhile raises)
    if o:mesh() ~= 0 and not o:isa("ZActor") and not o:isa("ZItem") and not o:hidden() and #o:children() == 0
       and not o:path():find("HeroSetup") and not o:path():find("__MAP.zip") then
      local ox, oy, oz = o:pos()
      local d = (ox - x) ^ 2 + (oy - y) ^ 2 + (oz - z) ^ 2
      if d < 2500 * 2500 then found[#found + 1] = {d, o:path()} end
    end
  end)
end
table.sort(found, function(a, b) return a[1] < b[1] end)
local out = {}
for i = 1, math.min(#found, 40) do out[i] = found[i][2] end  -- (the nearest that are records of the scene file)
return table.concat(out, "\n")
"""


def copy_near(dx, dz):
    """a copy of the nearest of the map's models the editor copies (not a character's body) beside it
    -> (its name, its place) or (None, None)"""
    for source in [p for p in lua(PLAIN_MODEL).splitlines() if p]:
        # a record of the scene file (C6-1's arrival cutscene parts, moved to the root while it plays, are not
        # the records the editor knows: a copy of one is made while playing, as it should be)
        if not lua('local r, k = h2.dev_record([[%s]]) return k' % source).startswith("file"):
            continue
        x, y, z = (float(v) for v in lua('local x, y, z = h2.find([[%s]]):pos() return x .. " " .. y .. " " .. z'
                                         % source).split())
        out = dev.run('return h2.dev_edit("dup", [[%s]], %f, %f, %f)' % (source, x + dx, y, z + dz), 30)
        if not out.startswith("error") and "timeout" not in out:
            return out.rsplit("/", 1)[-1], [x + dx, y, z + dz]
    return None, None

# an armed guard of the map and another weapon template -> "record|its weapon|another" or "none"
ARMED_GUARD = r"""
local weapons = {}
for _, t in ipairs(h2.objects("ZItemTemplate")) do
  local n = t:name()
  if n:match("^Gun") or n:match("^Rifle") then weapons[#weapons + 1] = t end
end
for _, n in ipairs(h2.npcs()) do
  local w = n.object and h2.dev_setting(n.object:path(), "WeaponTemplate")
  if w and w ~= "" then
    for _, t in ipairs(weapons) do
      if t:path() ~= w then return n.record .. "|" .. w .. "|" .. t:path() end
    end
  end
end
return "none"
"""


def lua(code, timeout=30):
    out = dev.run(code, timeout)
    if out.startswith("error") or "timeout" in out:
        raise RuntimeError(out)
    return out


def check(name, ok, detail=""):
    results.append(bool(ok))
    print("%s %s %s" % ("PASS" if ok else "FAIL", name, detail))


def note(text):
    notes.append(text)
    print("NOTE " + text)


loaded_at = 0.0  # when the map came in last
START_OVER = 25.0  # a save in a level's first seconds: C5-1's (its start camera still moving) crashed when loaded


def wait_map(scene):
    global loaded_at
    dev.wait_ready(scene + "__MAIN", 180)
    loaded_at = time.time()


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


def place(path):
    return [float(v) for v in lua('local x, y, z = h2.find([[%s]]):pos() return x .. " " .. y .. " " .. z' % path).split()]


def near(a, b, tol=2.0):
    return all(abs(x - y) <= tol for x, y in zip(a, b))


def kind(name):
    return lua('local o = h2.find([[%s]]) if not o then return "missing" end '
               'local r, k = h2.dev_record(o:path()) return k' % name)


def guard_setting(record):
    return lua('for _, n in ipairs(h2.npcs()) do if n.record == %d then '
               'return h2.dev_setting(n.object:path(), "WeaponTemplate") or "" end end return "none"' % record)


def run_map(scene):
    print("\n== " + scene)
    load(scene)
    lua('return h2.dev_level("new", "%s")' % LEVEL)
    copy, at = copy_near(300.0, 100.0)
    if not copy:
        note("%s: no plain model within 25 m of Hitman" % scene)
    point = point_at = None
    px, py, pz = (float(v) for v in lua('local x, y, z = h2.player():pos() return x .. " " .. y .. " " .. z').split())
    point_at = [px + 150.0, py, pz]
    try:
        point = lua('return h2.dev_edit("point", %f, %f, %f)' % tuple(point_at)).rsplit("/", 1)[-1]
    except RuntimeError as e:
        note("%s: no point made (%s)" % (scene, str(e)[:100]))
    guard = lua(ARMED_GUARD)
    lua('return h2.dev_level("save")')
    edit = None
    if guard == "none":
        note("%s: no armed guard / other weapon template" % scene)
    else:
        record, had, other = guard.split("|")
        doc = json.load(open(os.path.join(FOLDER, "level.json"), encoding="utf-8"))
        behaviour = lua('for _, n in ipairs(h2.npcs()) do if n.record == %s then return n.behaviour or "" end end '
                        'return ""' % record)
        doc["npc_edits"] = [{"record": int(record), "path": "", "behaviour": behaviour,
                             "params": [{"name": "WeaponTemplate", "type": "ZGEOMREF", "value": other}]}]
        json.dump(doc, open(os.path.join(FOLDER, "level.json"), "w", encoding="utf-8"), indent=1)
        edit = (int(record), had, other)
    mark = size()
    load(scene, LEVEL)
    log = since(mark)
    check(scene + ": loaded with the level, no exception", "EXCEPTION" not in log)
    lacks = [l.split("scene copies: ", 1)[1].strip() for l in log.splitlines()
             if "scene copies: " in l and ("has no" in l or "made while playing instead" in l)]
    for l in lacks:  # (made while playing then: a clone, as before)
        note("%s: %s" % (scene, l))
    if copy and any(l.startswith(copy) and "plain model" in l for l in lacks):
        copy = None
    if point and any(l.startswith(point) and "aim helper" in l for l in lacks):
        point = None
    if copy:
        check(scene + ": copies made with the map", "scene copies:" in log,
              [l.strip() for l in log.splitlines() if "scene copies" in l][-1:] or "no line")
        check(scene + ": the copy a record of the scene", kind(copy) == "copy", kind(copy))
        if kind(copy) == "copy":
            check(scene + ": the copy at its place", near(place(copy), at), str(place(copy)))
    if point:
        check(scene + ": the point a record of the scene", kind(point) == "copy", kind(point))
        if kind(point) == "copy":
            check(scene + ": the point at its place", near(place(point), point_at), str(place(point)))
    if edit:
        check(scene + ": settings made with the map", "scene scripts: settings of 1 characters" in log)
        setting = guard_setting(edit[0])
        check(scene + ": the guard made with the other weapon", setting == edit[2], "%s (was %s)" % (setting, edit[1]))
    save_file = os.path.join(dev.GAME, "Save", "%d.1" % SLOT)
    before = os.path.getmtime(save_file) if os.path.exists(save_file) else 0
    mark = size()
    time.sleep(max(0.0, START_OVER - (time.time() - loaded_at)))  # (as a player saves: once the level has started)
    lua("return tostring(h2.dev_savegame(%d))" % SLOT)
    time.sleep(3)
    check(scene + ": saved game", os.path.exists(save_file) and os.path.getmtime(save_file) > before)
    audit = [l.strip() for l in since(mark).splitlines() if "save audit: slot %d:" % SLOT in l]
    check(scene + ": save audit: every pointer placed", audit and audit[-1].endswith("unplaced 0"),
          audit[-1] if audit else "no line")
    mark = size()
    lua("return tostring(h2.dev_loadgame(%d))" % SLOT)
    wait_map(scene)
    check(scene + ": loaded game: no exception", "EXCEPTION" not in since(mark))
    if copy:
        check(scene + ": loaded game: the copy there", kind(copy) != "missing", kind(copy))
    if edit:
        check(scene + ": loaded game: the weapon kept", guard_setting(edit[0]) == edit[2], guard_setting(edit[0]))
    dev.run('return h2.dev_level("revert")', 10)


try:
    for m in sys.argv[1:] or MAPS:
        if os.path.isdir(FOLDER):
            shutil.rmtree(FOLDER)
        try:
            run_map(m)
        except RuntimeError as e:
            check(m + ": ran through", False, str(e)[:200])
            if not dev.running():
                note("%s: the game is down; the other maps not run" % m)
                break
finally:
    dev.run('return h2.dev_level("revert")', 10)
    shutil.rmtree(FOLDER, ignore_errors=True)
for n in notes:
    print("NOTE " + n)
print("\n%d passed, %d failed" % (sum(results), len(results) - sum(results)))
