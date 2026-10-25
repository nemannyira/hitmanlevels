"""A downloaded level (Mods\\Online\\<id>, docs/online_levels.md section 6) in the game: it runs
untrusted. Part 1 (H2Fix.ini DevCommandsOnline=0, as players have it): its script finds no
h2.dev_* and no h2.mem, gets 64 MB of Lua memory and 1 s for a call (read from H2Fix.log: the dev
commands stop answering once it runs, as they should); the game is closed after. Part 2
(DevCommandsOnline=1): the level is not saved, Save as makes a level of the user's own (its scripts
along, trusted again), its h2.state file is its own (Save\\9.<id>.h2lua). Starts the game itself
(h2/tests/run_all.py's start); uses save slot 9 (back up the saves first: run_all does).

  python h2/tests/online_level_test.py
"""
import os
import re
import shutil
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "tools"))
sys.path.insert(0, HERE)
import dev  # noqa: E402
import run_all  # noqa: E402

ID = "h2testonline01"
FOLDER = os.path.join(dev.GAME, "Mods", "Online", ID)
COPY = "_OnlineCopy"
LOG = os.path.join(dev.GAME, "H2Fix.log")
INI = os.path.join(dev.GAME, "H2Fix.ini")
SLOT = 9
results = []

LEVEL = """{"format": "h2level", "version": 1, "name": "Online test", "base_scene": "C1-1\\\\C1-1__MAIN",
"objects": [], "spawned": [], "npcs": [], "lists": [], "scripts": ["main.lua"]}"""
SCRIPT = r'''
print("ONLINE tools dev=" .. tostring(h2.dev_level ~= nil) .. " mem=" .. tostring(h2.mem ~= nil))
h2.state.online = 42
local ok = pcall(function()
    local t = {}
    for i = 1, 3000000 do t[i] = string.rep("x", 40) .. i end
end)
collectgarbage()
print("ONLINE memory " .. (ok and "allowed" or "stopped"))
h2.after(0.1, function()
    print("ONLINE time start")
    while true do end
end)
'''


def check(name, ok, detail=""):
    results.append(bool(ok))
    print("%s %s %s" % ("PASS" if ok else "FAIL", name, detail))


def set_online_dev(on):
    with open(INI, encoding="latin1", newline="") as f:
        text = f.read()
    text = re.sub(r"(?m)^DevCommandsOnline=[^\r\n]*\r?\n?", "", text)
    if on:
        text = re.sub(r"(?m)^(\[Debug\]\r?\n)", lambda m: m.group(1) + "DevCommandsOnline=1\r\n", text, count=1)
    with open(INI, "w", encoding="latin1", newline="") as f:
        f.write(text)


def log_size():
    return os.path.getsize(LOG) if os.path.exists(LOG) else 0


def log_since(at):
    with open(LOG, encoding="latin1") as f:
        f.seek(at)
        return f.read()


def load_online():
    """the level loaded from the main menu, the map loaded with it -> the log since"""
    dev.run('return h2.dev_level("revert")', 30)
    dev.run('return h2.dev_loadscene("MainMenu")', 10)
    dev.wait_ready("MainMenu", 60)
    mark = log_size()
    # (in one command: once a downloaded level is the editor's, Mods\_cmd.lua is not run any more)
    print(dev.run('local info = h2.dev_level("load", [[Online\\%s]]) h2.dev_loadscene([[C1-1\\C1-1__MAIN]]) '
                  'return info' % ID, 30)[:100])
    t = time.time()
    while time.time() - t < 150:
        if "ONLINE time start" in log_since(mark) or not dev.running():
            break
        time.sleep(1)
    time.sleep(4)
    return mark


def make_level():
    shutil.rmtree(FOLDER, ignore_errors=True)
    os.makedirs(FOLDER)
    with open(os.path.join(FOLDER, "level.json"), "w", encoding="utf-8") as f:
        f.write(LEVEL)
    with open(os.path.join(FOLDER, "main.lua"), "w", encoding="utf-8") as f:
        f.write(SCRIPT)


try:
    make_level()
    # ---- part 1: as players have it
    set_online_dev(False)
    check("the game started", run_all.start())
    mark = load_online()
    out = log_since(mark)
    check("the level loaded (strictly checked)", "level loaded: Online\\" + ID in out,
          [l for l in out.splitlines() if "level" in l][:2])
    check("untrusted: 64 MB, 1 s", "a downloaded level runs: 64 MB, 1 s" in out,
          [l.strip()[-80:] for l in out.splitlines() if "downloaded level runs" in l][:1])
    check("no h2.dev_*, no h2.mem in its script", "ONLINE tools dev=false mem=false" in out,
          [l.strip()[-60:] for l in out.splitlines() if "ONLINE tools" in l][:1])
    check("64 MB of Lua memory", "ONLINE memory stopped" in out)
    check("1 s for a call", "ran longer than 1 s" in out)
    answer = dev.run("return 1", 8)
    check("Mods\\_cmd.lua not run while it runs", "timeout" in answer, answer[:40])
    run_all.kill()
    # ---- part 2: the dev commands kept for testing
    set_online_dev(True)
    check("the game started again", run_all.start())
    mark = load_online()
    out = log_since(mark)
    check("untrusted again", "a downloaded level runs" in out)
    check("its dev tools kept (DevCommandsOnline=1), h2.mem out",
          "ONLINE tools dev=true mem=false" in out)
    saved = dev.run('return h2.dev_level("save")', 20)
    check("a downloaded level is not saved", "not changed" in saved, saved[:100])
    dev.run("return h2.dev_savegame(%d)" % SLOT, 30)
    time.sleep(8)
    own = os.path.join(dev.GAME, "Save", "%d.%s.h2lua" % (SLOT, ID))
    check("its h2.state file is its own", os.path.exists(own), own)
    if os.path.exists(own):
        with open(own, encoding="utf-8", errors="replace") as f:
            check("  holding its state", "42" in f.read())
    shutil.rmtree(os.path.join(dev.GAME, "Mods", COPY), ignore_errors=True)
    mark = log_size()
    out = dev.run('return h2.dev_level("saveas", "%s")' % COPY, 30)
    info = dev.run('return h2.dev_level("info")', 20)
    check("Save as: a level of the user's own", "folder=%s " % COPY in info, info[:80])
    check("  its script along", os.path.exists(os.path.join(dev.GAME, "Mods", COPY, "main.lua")))
    time.sleep(2)
    check("  trusted again", "the user's own again" in log_since(mark))
    check("  h2.dev_level back", "true" in dev.run("return tostring(h2.dev_level ~= nil)", 10))
finally:
    run_all.kill()
    set_online_dev(False)
    run_all.start()  # (as it was: the game running with the user's settings)
    shutil.rmtree(FOLDER, ignore_errors=True)
    try:
        os.rmdir(os.path.dirname(FOLDER))
    except OSError:
        pass
    shutil.rmtree(os.path.join(dev.GAME, "Mods", COPY), ignore_errors=True)
    for name in os.listdir(os.path.join(dev.GAME, "Save")):
        if name.endswith(".%s.h2lua" % ID):
            os.remove(os.path.join(dev.GAME, "Save", name))

print("\n%d passed, %d failed" % (results.count(True), results.count(False)))
sys.exit(0 if all(results) else 1)
