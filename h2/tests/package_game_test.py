"""A level package made in the game and played as installed (src/levelfmt/share.h): the editor's
Export package (h2.dev_export, Level > Share) writes Mods\\Packages\\<level>_v1.h2lvl, levelpack
installs it into Mods\\Online\\<id>, and the game loads it as a downloaded level (strictly checked,
its script untrusted). The author files in %LOCALAPPDATA%\\HitmanLevels are put aside for the test
and back after. Starts the game itself (h2/tests/run_all.py's start). Run it from a shell outside any
sandbox: the author key it makes in %LOCALAPPDATA% must be the one the game (started through Steam) sees.

  python h2/tests/package_game_test.py
"""
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, os.path.join(HERE, "..", "tools"))
sys.path.insert(0, HERE)
import dev  # noqa: E402
import run_all  # noqa: E402

PACK = os.environ.get("LEVELPACK") or os.path.join(ROOT, "build", "Release", "levelpack.exe")
NAME = "_PkgTest"
FOLDER = os.path.join(dev.GAME, "Mods", NAME)
LOG = os.path.join(dev.GAME, "H2Fix.log")
HOME = os.path.join(os.environ["LOCALAPPDATA"], "HitmanLevels")
results = []

LEVEL = {"format": "h2level", "version": 1, "name": "Package test", "base_scene": "C1-1\\C1-1__MAIN",
         "objects": [], "spawned": [], "npcs": [], "lists": [], "scripts": ["main.lua"]}
SCRIPT = 'print("PKGTEST running, dev tools " .. tostring(h2.dev_level ~= nil))\n'


def check(name, ok, detail=""):
    results.append(bool(ok))
    print("%s %s %s" % ("PASS" if ok else "FAIL", name, str(detail)[:160]))


def log_size():
    return os.path.getsize(LOG) if os.path.exists(LOG) else 0


def log_since(at):
    with open(LOG, encoding="latin1") as f:
        f.seek(at)
        return f.read()


aside = tempfile.mkdtemp(prefix="h2author_")
had_home = os.path.isdir(HOME)
level_id = None
package = None
try:
    if had_home:  # the user's own author files: put aside, back after
        shutil.move(HOME, os.path.join(aside, "HitmanLevels"))
    code = subprocess.run([PACK, "author", "Game Test", "CC0 1.0"], capture_output=True, text=True).returncode
    check("a test author key made", code == 0)
    shutil.rmtree(FOLDER, ignore_errors=True)
    os.makedirs(FOLDER)
    with open(os.path.join(FOLDER, "level.json"), "w", encoding="utf-8") as f:
        json.dump(LEVEL, f)
    with open(os.path.join(FOLDER, "main.lua"), "w", encoding="utf-8") as f:
        f.write(SCRIPT)

    check("the game started", run_all.start())
    print(dev.run('return h2.dev_level("load", "%s")' % NAME, 30)[:100])
    out = dev.run("local p, e = h2.dev_export() return tostring(p) .. '|' .. tostring(e)", 30)
    package = out.split("|")[0].strip()
    check("Export package in the game", package.endswith("_PkgTest_v1.h2lvl") and os.path.exists(package), out)
    with open(os.path.join(FOLDER, "level.json"), encoding="utf-8") as f:
        stamp = json.load(f).get("package", {})
    level_id = stamp.get("id")
    check("the level saved with its package id and version 1", stamp.get("version") == 1 and level_id, stamp)
    out = dev.run("local p, e = h2.dev_export() return tostring(p) .. '|' .. tostring(e)", 30)
    check("a second export: version 2", out.split("|")[0].strip().endswith("_PkgTest_v2.h2lvl"), out)

    if not level_id or not os.path.exists(package):
        raise RuntimeError("no package exported: the rest cannot run")
    p = subprocess.run([PACK, "install", package, "--game", dev.GAME, "--yes"], capture_output=True, text=True)
    online = os.path.join(dev.GAME, "Mods", "Online", level_id or "-")
    check("levelpack installs the game's package", p.returncode == 0 and os.path.isdir(online),
          (p.stdout + p.stderr).strip().splitlines()[-1:])

    dev.run('return h2.dev_level("revert")', 30)
    dev.run('return h2.dev_loadscene("MainMenu")', 10)
    dev.wait_ready("MainMenu", 60)
    mark = log_size()
    # (in one command: once a downloaded level is the editor's, Mods\_cmd.lua is not run any more)
    dev.run('local i = h2.dev_level("load", [[Online\\%s]]) h2.dev_loadscene([[C1-1\\C1-1__MAIN]]) return i'
            % level_id, 30)
    t = time.time()
    while time.time() - t < 150 and "PKGTEST running" not in log_since(mark) and dev.running():
        time.sleep(1)
    time.sleep(3)
    out = log_since(mark)
    check("the installed level loads (strictly checked)", "level loaded: Online\\" + level_id in out,
          [l for l in out.splitlines() if "level" in l][:2])
    check("  untrusted", "a downloaded level runs" in out)
    check("  its script runs without the dev tools", "PKGTEST running, dev tools false" in out,
          [l.strip()[-60:] for l in out.splitlines() if "PKGTEST" in l][:1])
except RuntimeError as ex:
    check(str(ex), False)
finally:
    run_all.kill()
    shutil.rmtree(FOLDER, ignore_errors=True)
    if level_id:
        online = os.path.join(dev.GAME, "Mods", "Online", level_id)
        for f in os.listdir(online) if os.path.isdir(online) else []:
            os.chmod(os.path.join(online, f), 0o666)
        shutil.rmtree(online, ignore_errors=True)
        try:
            os.rmdir(os.path.dirname(online))
        except OSError:
            pass
    packages = os.path.join(dev.GAME, "Mods", "Packages")
    for f in os.listdir(packages) if os.path.isdir(packages) else []:
        if f.startswith(NAME + "_v"):
            os.remove(os.path.join(packages, f))
    try:
        os.rmdir(packages)
    except OSError:
        pass
    shutil.rmtree(HOME, ignore_errors=True)
    if had_home:
        shutil.move(os.path.join(aside, "HitmanLevels"), HOME)
    shutil.rmtree(aside, ignore_errors=True)
    run_all.start()  # (as it was: the game running with the user's settings)

print("\n%d passed, %d failed" % (results.count(True), results.count(False)))
sys.exit(0 if all(results) else 1)
