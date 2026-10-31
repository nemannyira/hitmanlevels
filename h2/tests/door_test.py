"""Doors locked by a level (#35; engine::SetDoorLocked: the game's MSG_LOCKDOOR / MSG_UNLOCKDOOR as the
levels' scripts send them): one of C1-1's doors found by its routine (obj:locked() not nil), locked
from the Inspector's "door locked", saved ("doors" of level.json), locked again when the map loads
(the level applied), kept in a saved game; the level reverted: open again. Uses Mods/_DoorTest
(removed); C1-1; a running game with dev commands.

  python h2/tests/door_test.py
"""
import json
import os
import shutil
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "tools"))
import dev  # noqa: E402

LEVEL = "_DoorTest"
FOLDER = os.path.join(dev.GAME, "Mods", LEVEL)
SLOT = 9
results = []


def lua(code, timeout=30):
    out = dev.run(code, timeout)
    if out.startswith("error") or "timeout" in out:
        raise RuntimeError(out)
    return out


def check(name, ok, detail=""):
    results.append(bool(ok))
    print("%s %s %s" % ("PASS" if ok else "FAIL", name, detail))


def press(label, settle=0.8):
    lua('h2.dev_press(%s, 1) return 1' % json.dumps(label))
    status = "looking"
    t = time.time()
    while time.time() - t < 5 and status == "looking":
        time.sleep(0.2)
        status = lua("return h2.dev_press_status()")
    time.sleep(settle)
    return status


def load():
    lua(r'return h2.dev_loadscene([[C1-1\C1-1__MAIN]])')
    dev.wait_ready("C1-1__MAIN", 150)
    time.sleep(2)


# a door of the map, open now, nearest Hitman (its path)
FIND_DOOR = r"""
local hx, hy, hz = h2.player():pos()
local best, bd = nil, 1e18
for _, o in ipairs(h2.objects("ZGEOM")) do
  local l = o:locked()
  if l == false then
    local x, y, z = o:pos()
    local d = (x - hx)^2 + (y - hy)^2 + (z - hz)^2
    if d < bd then best, bd = o, d end
  end
end
return best and best:path() or "none"
"""

if os.path.isdir(FOLDER):
    shutil.rmtree(FOLDER)
try:
    load()
    door = lua(FIND_DOOR, 60)
    check("a door of C1-1 (its routine)", door != "none", door)
    locked = 'return tostring(h2.find([[%s]]):locked())' % door
    if door != "none":
        lua('h2.find([[%s]]):locked(true) return 1' % door)
        check("obj:locked(true)", lua(locked) == "true")
        lua('h2.find([[%s]]):locked(false) return 1' % door)
        check("obj:locked(false)", lua(locked) == "false")

        lua('return h2.dev_level("new", "%s")' % LEVEL)
        lua('h2.dev_mode("advanced") h2.dev_editor(true) h2.dev_panel("Inspector") h2.dev_select([[%s]]) return 1' % door)
        time.sleep(0.8)
        check("Inspector: door locked", press("door locked").startswith("pressed") and lua(locked) == "true")
        lua('return h2.dev_level("save")')
        with open(os.path.join(FOLDER, "level.json"), encoding="utf-8") as f:
            doc = json.load(f)
        check("saved: doors", doc.get("doors") == [{"path": door, "locked": True}], str(doc.get("doors")))
        lua('h2.dev_editor(false) return 1')

        load()  # the level applied with the map
        check("locked when the map loads", lua(locked) == "true")
        # unlocked while playing (its key), then saved: the saved game keeps it open (the door's own saved state
        # comes after the level is applied)
        lua('h2.find([[%s]]):locked(false) return 1' % door)
        lua("return tostring(h2.dev_savegame(%d))" % SLOT)
        time.sleep(3)
        lua("return tostring(h2.dev_loadgame(%d))" % SLOT)
        dev.wait_ready("C1-1__MAIN", 150)
        time.sleep(3)
        check("a saved game: open as saved", lua(locked) == "false")

        lua('h2.find([[%s]]):locked(true) return 1' % door)  # (as the level has it)
        lua('return h2.dev_level("revert")')
        time.sleep(1)
        check("the level reverted: open", lua(locked) == "false")
finally:
    dev.run('h2.dev_editor(false) return h2.dev_level("revert")', 10)
    shutil.rmtree(FOLDER, ignore_errors=True)
print("\n%d passed, %d failed" % (sum(results), len(results) - sum(results)))
sys.exit(0 if all(results) else 1)
