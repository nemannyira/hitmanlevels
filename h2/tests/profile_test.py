"""A level of the editor does not count in the game's profile (P15): a level
on C1-1 completed (statistics screen, bonus weapon) leaves Save\\*.blap as it
was, the game's global variables are put back when its map is left, and the
statistics screen's Continue goes to the main menu, not to the next mission.
Uses Mods/_ProfileTest (removed at the end); a running game with dev
commands and LuaMemory (the global variables are read).

  python h2/tests/profile_test.py
"""
import hashlib
import glob
import os
import shutil
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "tools"))
import dev  # noqa: E402

LEVEL = "_ProfileTest"
FOLDER = os.path.join(dev.GAME, "Mods", LEVEL)
LOG = os.path.join(dev.GAME, "H2Fix.log")
DIK_RETURN = 0x1C
results = []
# the global variables (CGlobalCom 0x6A40EC: capacity, data, used) as one text
VARIABLES = """local M = h2.mem local base = 0x6a40ec local data, used = M.u32(base + 8), M.u32(base + 12)
local t = {} for i = 0, used - 1 do t[#t + 1] = string.format("%02x", M.u8(data + i)) end return table.concat(t)"""


def lua(code, timeout=30):
    out = dev.run(code, timeout)
    if out.startswith("error") or "timeout" in out:
        raise RuntimeError(out)
    return out


def check(name, ok, detail=""):
    results.append(bool(ok))
    print("%s %s %s" % ("PASS" if ok else "FAIL", name, detail))


def wait_scene(part, timeout=150):
    time.sleep(4)
    t = time.time()
    while time.time() - t < timeout:
        if part in dev.run("return h2.scene()", 5):
            return True
        time.sleep(1)
    return False


def profiles():
    return {os.path.basename(p): hashlib.md5(open(p, "rb").read()).hexdigest()
            for p in glob.glob(os.path.join(dev.GAME, "Save", "*.blap"))}


def log_size():
    return os.path.getsize(LOG) if os.path.exists(LOG) else 0


def log_since(mark):
    with open(LOG, "rb") as f:
        f.seek(mark)
        return f.read().decode("utf-8", "replace")


if os.path.isdir(FOLDER):
    shutil.rmtree(FOLDER)
try:
    before = profiles()
    lua('return h2.dev_level("new", "_ProfileTestBase")')  # (no level on the first map)
    lua(r'return h2.dev_loadscene([[C1-1\C1-1__MAIN]])')
    check("map up", wait_scene("C1-1__MAIN"))
    time.sleep(6)
    lua('return h2.dev_level("new", "%s")' % LEVEL)
    lua('return h2.dev_level("save")')
    mark = log_size()
    lua(r'return h2.dev_loadscene([[C1-1\C1-1__MAIN]])')  # the level on its map
    check("level map up", wait_scene("C1-1__MAIN"))
    time.sleep(8)
    variables = lua(VARIABLES)
    check("profile kept from the level", "profile: kept from the level" in log_since(mark))
    lua('h2.god(true) h2.mission("complete") return true')
    time.sleep(10)
    check("completed: profile not written", "profile: not written" in log_since(mark))
    check("completed: variables changed (statistics)", lua(VARIABLES) != variables)
    for _ in range(4):  # bonus weapon, statistics: Continue
        lua("h2.dev_key(%d, 0.15) return 1" % DIK_RETURN)
        time.sleep(4)
        if "MainMenu" in dev.run("return h2.scene()", 5):
            break
    check("Continue: the main menu", wait_scene("MainMenu", 60), dev.run("return h2.scene()", 5))
    tail = log_since(mark)
    check("left: variables put back", "global variables put back (changed by the level" in tail)
    check("profiles unchanged", profiles() == before)
finally:
    shutil.rmtree(FOLDER, ignore_errors=True)
print("\n%d passed, %d failed" % (sum(results), len(results) - sum(results)))
