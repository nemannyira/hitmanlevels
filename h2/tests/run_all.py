"""All the tests in one go, with the game: the saves backed up (and put back
exactly as they were at the end), H2Fix.ini's DevCommands on for the run and
LuaMemory only for the tests that need it, LuaMemory last (put back after; the
security suite checks that h2.mem is not there without it), the game started
through Steam (h2/tools/start_game.ps1,
the intros skipped with Esc; why a start failed goes into the report), each test on a freshly loaded C1-1, the game
started again when a test took it down (and for the stress suite first, which
fills the map's buffers). The installed DLL is what is tested: build and copy
it first. A report goes to dumps/test_runs/<time>/report.md, each test's
output next to it.

  python h2/tests/run_all.py [--only api,gui,...] [--skip maps,fuzz] [--keep-game] [--list]

Start it from a shell outside any sandbox (PowerShell here): the files tests make outside the repo
and the game folder (the author key in %LOCALAPPDATA%) must be the ones the game sees.
"""
import argparse
import ctypes
import datetime
import os
import re
import shutil
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, os.path.join(HERE, "..", "tools"))
import dev  # noqa: E402

INI = os.path.join(dev.GAME, "H2Fix.ini")
SAVE = os.path.join(dev.GAME, "Save")
LOG = os.path.join(dev.GAME, "H2Fix.log")
START = os.path.join(ROOT, "h2", "tools", "start_game.ps1")
C11 = r"C1-1\C1-1__MAIN"

# (name, command after "python", timeout in seconds, a fresh game first[, LuaMemory on])
TESTS = [
    ("stress", ["run_suite.py", "stress_suite.lua", "240"], 400, True),
    ("api", ["run_suite.py", "api_suite.lua", "120"], 300, True),
    ("security", ["run_suite.py", "security_suite.lua", "90"], 200, False),
    ("editor", ["editor_test.py"], 600, False),
    ("goals", ["goals_test.py"], 600, False),
    ("saveload", ["saveload_test.py"], 600, False),
    ("door", ["door_test.py"], 600, False),  # (#35: doors locked by a level)
    ("bodies", ["bodies_test.py"], 600, False),  # (#30: an escalation's no bodies found)
    ("knockout", ["knockout_test.py"], 600, False),
    ("behaviours", ["behaviours_test.py"], 600, False),
    ("escalation", ["escalation_test.py"], 900, False),  # (#23: a level in stages)
    ("escalation_example", ["escalation_example_test.py"], 900, False),  # (#29: played through)
    ("take_action", ["take_action_test.py"], 600, False),
    ("copies", ["copies_test.py"], 600, False),
    ("copies2", ["copies2_test.py"], 600, False),
    ("imports_save", ["imports_save_test.py"], 900, False),
    ("gameway3", ["gameway3_test.py"], 600, False),
    ("map_icon", ["map_icon_test.py"], 600, False),
    ("map_models", ["map_models_test.py"], 900, False),
    ("map_maps", ["map_maps_test.py"], 2400, False),
    ("map_own", ["map_own_test.py"], 900, False),
    ("killswitch", ["killswitch_test.py"], 300, False),
    ("gui", ["gui_test.py"], 600, False),
    ("simple_mode", ["simple_mode_test.py"], 600, False),  # (#31: the guided editor)
    ("playdebug", ["playdebug_test.py"], 600, False),  # (the debug view while playing, the last test's events)
    ("route_order", ["route_order_test.py"], 600, False),  # (a placed walker goes along its points in order)
    ("laptop_photo", ["laptop_photo_test.py"], 600, False),  # (the level's photo where the map's is)
    ("maps_gameway", ["maps_gameway_test.py"], 3600, False),
    ("fuzz", ["fuzz_levels.py"], 1800, False),
    ("limits", ["limits_test.py"], 1500, True),  # (big levels: on a fresh game)
    ("run_maps", ["run_maps.py"], 3600, True),
    ("level_check", ["level_check_test.py"], 300, False),  # (the validator: no game needed)
    ("package", ["package_test.py"], 300, False),  # (packages and the installer: no game needed)
    ("package_game", ["package_game_test.py"], 600, False),  # (export in the game, install, load)
    ("kill_early", ["kill_early_test.py"], 900, False),  # (#2: scripted characters killed at once, C5-3)
    ("online", ["online_level_test.py"], 900, False),  # (starts the game itself, twice)
    ("runs", ["runs_test.py"], 900, False),  # (#50: a downloaded level's plays written down; starts the game)
    ("profile", ["profile_test.py"], 600, False, True),  # (reads the game's globals: h2.mem)
]


def log(text):
    print(time.strftime("%H:%M:%S ") + text, flush=True)


def alive():
    return dev.running() and "timeout" not in dev.run("return 1", 5)


def scene():
    return dev.run("return h2.scene()", 5)


def kill():
    subprocess.run(["taskkill", "/F", "/IM", "hitman2.exe"], capture_output=True)
    t = time.time()
    while dev.running() and time.time() - t < 20:
        time.sleep(0.5)
    time.sleep(2)


STARTS = []  # why starting / loading failed (issue #5: the report says it)


def why(text):
    STARTS.append(time.strftime("%H:%M:%S ") + text)
    log(text)


def set_debug_key(key, value):
    """H2Fix.ini's [Debug] key set (added when missing); the whole file is put back at the end"""
    with open(INI, encoding="latin1", newline="") as f:
        text = f.read()
    if re.search(r"(?m)^%s=" % key, text):
        text = re.sub(r"(?m)^%s=[^\r\n]*" % key, "%s=%s" % (key, value), text)
    elif re.search(r"(?m)^\[Debug\]", text):
        text = re.sub(r"(?m)^\[Debug\][^\r\n]*(\r?\n)", lambda m: m.group(0) + "%s=%s" % (key, value) + m.group(1),
                      text, 1)
    else:
        text += "\r\n[Debug]\r\n%s=%s\r\n" % (key, value)
    with open(INI, "w", encoding="latin1", newline="") as f:
        f.write(text)


def start(wait=90):
    """the game through Steam, the intros skipped (Esc until the main menu) -> True when up. It runs behind the
    user's windows (the window in front now gets the foreground back: [Debug] Background, display.cpp)"""
    kill()
    set_debug_key("Background", ctypes.windll.user32.GetForegroundWindow() or 0)
    try:
        out = subprocess.run(["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", START],
                             capture_output=True, text=True, timeout=120).stdout
    except subprocess.TimeoutExpired:
        why("start: start_game.ps1 did not finish in 120 s")
        return False
    if "game up" not in out:
        why("start: no game window (start_game.ps1: %s)" % (out.strip()[-160:] or "nothing"))
        return False
    t = time.time()
    s = ""
    while time.time() - t < wait:
        s = scene()
        if "MainMenu" in s:
            return True
        if "timeout" not in s:
            dev.run("return h2.dev_key(1, 0.1)", 5)  # (Esc: the intro videos)
        elif not dev.running():
            why("start: the game went away before the main menu")
            return False
        time.sleep(0.5)
    why("start: no main menu in %d s (the game answered: %s; DevCommands on? Mods\\_cmd.lua answered?)"
        % (wait, s.strip()[:60] or "nothing"))
    return False


def load_c11():
    """C1-1 loaded freshly, the editor closed, no level -> True when there"""
    dev.run("h2.dev_editor(false) return h2.dev_level('revert')", 10)
    dev.run("return h2.dev_loadscene([[%s]])" % C11, 10)
    if dev.wait_ready("C1-1__MAIN", 180):
        return True
    why("load: C1-1 not ready in 180 s (dev_ready: %s, game running: %s)"
        % (dev.run("return h2.dev_ready()", 5).strip()[:60], dev.running()))
    return False


def ready(fresh):
    # (a third try waits longer: Steam starting up, or a game closing slowly, took the first ones)
    for attempt in range(3):
        if attempt == 2:
            kill()
            time.sleep(20)
        if (fresh or not alive()) and not start(90 if attempt < 2 else 180):
            fresh = True
            continue
        if load_c11():
            return True
        fresh = True
    return False


def summary(name, out):
    """(passed, failed, the line it was read from)"""
    m = re.findall(r"(\d+) passed, (\d+) failed", out)
    if m:
        return int(m[-1][0]), int(m[-1][1]), "%s passed, %s failed" % m[-1]
    m = re.findall(r"pass (\d+) fail (\d+)(?: pending (\d+))?", out)
    if m and name != "run_maps":
        p, f, pend = m[-1]
        return int(p), int(f) + int(pend or 0), "pass %s fail %s pending %s" % (p, f, pend or 0)
    if name == "run_maps":
        lines = out.split("==== summary")[-1].strip().splitlines()
        bad = [l for l in lines if " fail 0" not in l]
        return len(lines) - len(bad), len(bad), "%d maps, %d not clean" % (len(lines), len(bad))
    m = re.findall(r"survived (\d+) / (\d+)", out)
    if m:
        s, n = int(m[-1][0]), int(m[-1][1])
        return s, n - s, "survived %d / %d" % (s, n)
    return 0, 1, "no result line"


def log_size():
    return os.path.getsize(LOG) if os.path.exists(LOG) else 0


def log_since(at):
    if not os.path.exists(LOG):
        return ""
    with open(LOG, encoding="latin1") as f:
        if os.path.getsize(LOG) >= at:
            f.seek(at)
        return f.read()


def set_ini(text, values):
    for key, value in values.items():
        text = re.sub(r"(?m)^%s=[^\r\n]*" % key, "%s=%s" % (key, value), text)
    # the tests drive Advanced mode's panels (the first Mode of [Editor] counts; [Display] has a Mode too)
    if re.search(r"(?m)^\[Editor\]", text):
        return re.sub(r"(?m)^\[Editor\][^\r\n]*(\r?\n)", lambda m: m.group(0) + "Mode=advanced" + m.group(1), text, 1)
    return text + "\r\n[Editor]\r\nMode=advanced\r\n"


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--only", default="", help="these tests (comma separated names)")
    ap.add_argument("--skip", default="", help="not these")
    ap.add_argument("--keep-game", action="store_true", help="leave the game running at the end")
    ap.add_argument("--list", action="store_true", help="the tests' names")
    args = ap.parse_args()
    if args.list:
        for name, cmd, *_ in TESTS:
            print("%-14s %s" % (name, " ".join(cmd)))
        return 0
    only = [n for n in args.only.split(",") if n]
    skip = [n for n in args.skip.split(",") if n]
    names = [t[0] for t in TESTS]
    for n in only + skip:
        if n not in names:
            print("no test %s (--list)" % n)
            return 2
    tests = [t for t in TESTS if (not only or t[0] in only) and t[0] not in skip]

    stamp = datetime.datetime.now().strftime("%Y%m%d_%H%M%S")
    out_dir = os.path.join(ROOT, "dumps", "test_runs", stamp)
    os.makedirs(out_dir)
    save_backup = os.path.join(out_dir, "save_backup")
    shutil.copytree(SAVE, save_backup)
    with open(INI, encoding="latin1", newline="") as f:
        ini = f.read()
    log("saves backed up to " + save_backup)
    rows = []
    t0 = time.time()
    try:
        memory = None  # LuaMemory as the game was started with it
        restart = True
        for name, cmd, timeout, fresh, *rest in tests:
            log("== " + name)
            wanted = bool(rest and rest[0])
            if wanted != memory:  # (the ini is read when the game starts)
                with open(INI, "w", encoding="latin1", newline="") as f:
                    f.write(set_ini(ini, {"DevCommands": 1, "LuaMemory": int(wanted)}))
                memory, restart = wanted, True
            STARTS.clear()
            if not ready(fresh or restart):
                rows.append((name, 0, 1, "the game did not start / load C1-1", 0, "\n".join(STARTS[-6:])))
                log("   the game did not start / load C1-1")
                restart = True
                continue
            restart = False
            mark = log_size()
            t = time.time()
            try:
                argv = [os.path.join(HERE, c) if c.endswith((".py", ".lua")) else c for c in cmd]
                p = subprocess.run([sys.executable] + argv, cwd=ROOT,
                                   capture_output=True, text=True, timeout=timeout, encoding="utf-8", errors="replace")
                out = p.stdout + ("\n--- stderr\n" + p.stderr if p.stderr.strip() else "")
            except subprocess.TimeoutExpired as e:
                out = (e.stdout or "") if isinstance(e.stdout, str) else (e.stdout or b"").decode("utf-8", "replace")
                out += "\n(timed out after %d s)" % timeout
            secs = time.time() - t
            with open(os.path.join(out_dir, name + ".txt"), "w", encoding="utf-8") as f:
                f.write(out)
            passed, failed, line = summary(name, out)
            if "(timed out" in out:
                failed, line = failed + 1, line + ", timed out"
            down = not alive()
            if down:
                failed, line, restart = failed + 1, line + ", GAME DOWN after it", True
            exceptions = [l.strip() for l in log_since(mark).splitlines() if "EXCEPTION" in l]
            fails = [l.strip() for l in out.splitlines() if l.startswith("FAIL") or "GAME DOWN" in l][:8]
            rows.append((name, passed, failed, line, secs, "\n".join(fails + exceptions[:3])))
            log("   %s (%d s)" % (line, secs))
            if name == "stress":
                restart = True  # (its map is full: the others on a fresh game)
    finally:
        if not args.keep_game:
            kill()
        with open(INI, "w", encoding="latin1", newline="") as f:
            f.write(ini)
        # (a game left running writes saves only when asked; the files are put back now, not after it)
        for name in os.listdir(SAVE):
            os.remove(os.path.join(SAVE, name))
        for name in os.listdir(save_backup):
            shutil.copy2(os.path.join(save_backup, name), os.path.join(SAVE, name))
        log("saves and H2Fix.ini put back" + (" (the game still runs: H2Fix.ini counts from its next start)"
                                               if dev.running() else ""))

    total_p = sum(r[1] for r in rows)
    total_f = sum(r[2] for r in rows)
    report = ["# Test run %s" % stamp, "",
              "%d tests, %d checks passed, %d failed, %d min" % (len(rows), total_p, total_f, (time.time() - t0) / 60),
              "", "| test | result | time |", "|---|---|---|"]
    for name, p, f, line, secs, _ in rows:
        report.append("| %s | %s%s | %d s |" % (name, "" if f == 0 else "**FAIL** ", line, secs))
    for name, p, f, line, secs, details in rows:
        if details:
            report += ["", "## " + name, "", "```", details, "```"]
    path = os.path.join(out_dir, "report.md")
    with open(path, "w", encoding="utf-8") as f:
        f.write("\n".join(report) + "\n")
    print("\n".join(report))
    print("\nreport: " + path)
    return 1 if total_f else 0


if __name__ == "__main__":
    sys.exit(main())
