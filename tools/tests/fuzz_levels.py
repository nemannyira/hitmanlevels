"""Hostile level files: each is written to Mods/_Fuzz_<n>, loaded (with its
map, C1-1) and the game checked; the folders are removed at the end.

  python tools/tests/fuzz_levels.py
"""
import json
import os
import shutil
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, ".."))
import dev  # noqa: E402

MODS = os.path.join(dev.GAME, "Mods")
BASE = {
    "format": "h2level", "version": 1, "name": "Fuzz", "base_scene": "C1-1\\C1-1__MAIN",
    "npcs": [{"id": "A", "record": 2350, "model": "BodyguardMafia_01", "pos": [-4176.0, -20.0, -6682.0],
              "matrix": [0, 0, 1, 0, 1, 0, 1, 0, 0]}],
    "scripts": ["main.lua"],
}
M = [0, 0, 1, 0, 1, 0, 1, 0, 0]


def case(**changes):
    d = json.loads(json.dumps(BASE))
    d.update(changes)
    return d


CASES = {
    "wrong types": case(npcs="x", objects=5, scripts={}, imports="y", spawned=7, lists=[1, 2], mission=3, sky=4),
    "wrong types inside": case(npcs=[1, "a", None, {"record": "x", "pos": "y"}],
                               objects=[{"path": 5, "pos": [1, 2], "matrix": "m"}],
                               spawned=[{"kind": 7, "pos": [0, 0, 0], "matrix": M}]),
    "bad records": case(npcs=[{"id": "A", "record": r, "model": "", "pos": [0, 0, 0], "matrix": M}
                              for r in (-1, 0, 999999, 2 ** 31 - 1, 1)]),
    "huge numbers": case(npcs=[{"id": "A", "record": 2350, "pos": [1e308, -1e308, 1e38], "matrix": [1e300] * 9}],
                         objects=[{"path": "ROOT", "pos": [1e39, 0, 0], "matrix": M, "class": "ZROOM"}]),
    "paths": case(objects=[{"path": p, "pos": [0, 0, 0], "matrix": M, "class": "ZSTDOBJ", "hidden": True}
                           for p in ("ROOT", "", "ROOT/..", "ROOT/" + "x" * 5000, "..\\..\\x", "ROOT/C1-1/#999")],
                  scripts=["../../evil.lua", "C:/Windows/win.ini", "\\x.lua", "/x.lua", "", "main.lua"]),
    "spawned bogus": case(spawned=[{"id": i, "source_scene": "C1-1\\C1-1__MAIN", "source_path": p,
                                    "kind": k, "pos": [0, 0, 0], "matrix": M, "parent": p, "hidden": False}
                                   for i, (p, k) in enumerate([("ROOT/nothing", "item"), ("ROOT", "point"),
                                                               ("", None), ("ROOT/C1-1", "item")])]),
    "imports bogus": case(imports=[{"level": lv, "mesh": m, "name": "x", "character": c}
                                   for lv, m, c in (("..\\..\\Windows", 1, False), ("C9-9", 2, True),
                                                    ("C2-1", 0xFFFFFFFF, False), ("C2-1", 0, True), ("", 5, False))],
                          sky={"level": "..\\.."}, place_floor={"level": "C3-3", "mesh": 123456789}),
    "many": case(objects=[{"path": "ROOT/x%d" % i, "pos": [0, 0, 0], "matrix": M, "class": "ZSTDOBJ"}
                          for i in range(10000)],
                 lists=[{"path": "ROOT", "points": ["ROOT"] * 10000}]),
    "mission bogus": case(mission={"objectives": [{"slot": s, "text": "t" * 1000000, "kind": "kill", "target": "ROOT"}
                                                  for s in (0, 1, 99, -5)],
                                   "briefing": "b" * 2000000, "exit": "ROOT", "exit_radius": -1e30, "base_ends": "no"}),
    "take goals bogus": case(mission={"objectives": [
        {"slot": s, "goal": "take", "target": tg, "radius": r}
        for s, tg, r in [(1, "level:", -0.0), (2, "level:../../x", -5), (3, "level:" + "A" * 5000, 1e30),
                         (4, "ROOT", 0), (5, "ROOT/C1-1", 1e-30), (6, "level:level:x", 150)]]},
        spawned=[{"id": "level:x", "kind": "point", "parent": "ROOT", "pos": [0, 0, 0], "matrix": M}]),
    "take goals wrong types": case(mission={"objectives": [{"slot": 1, "goal": "take", "target": None, "radius": "x"},
                                                           {"slot": 2, "goal": ["take"], "target": 5}]}),
    "scene bogus": case(base_scene=r"MainMenu\MainMenu"),
    "scene outside": case(base_scene=r"..\..\..\Windows\win"),
    "behaviour bogus": case(npcs=[{"id": "A", "record": 2350, "pos": [-4176, -20, -6682], "matrix": M,
                                   "behaviour": {"name": n, "level": lv, "params": [{"name": "WayPoints", "type": t,
                                                                                    "value": v}]}}
                                  for n, lv, t, v in (("..\\..\\x.sdl", "", "ZGEOMREF", "ROOT/nothing"),
                                                      ("scripts\\C1\\C1_1\\BaseGuard.sdl", "..\\..", "LONG", "x"),
                                                      ("x" * 10000, "C2-1", "FLOAT", 1e308))]),
}

DEEP = '{"format":"h2level","version":1,"name":"Deep","base_scene":"C1-1\\\\C1-1__MAIN","x":' + "[" * 100000 + "]" * 100000 + "}"


def run_case(name, text, script="h2.log('fuzz main ran')"):
    folder = "_Fuzz_%d" % abs(hash(name) % 100000)
    path = os.path.join(MODS, folder)
    os.makedirs(path, exist_ok=True)
    open(os.path.join(path, "level.json"), "w", encoding="utf-8").write(text)
    open(os.path.join(path, "main.lua"), "w", encoding="utf-8").write(script)
    out = dev.run('local ok, e = pcall(h2.dev_level, "load", "%s") return tostring(ok) .. " " .. tostring(e)' % folder, 60)
    loaded = out.startswith("true")
    if loaded:
        dev.run(r'return h2.dev_loadscene([[C1-1\C1-1__MAIN]])', 10)
        time.sleep(4)
        t = time.time()
        while time.time() - t < 150:
            s = dev.run("return h2.scene()", 5)
            if "C1-1__MAIN" in s:
                break
            if not dev.running():
                break
            time.sleep(1)
        time.sleep(6)
    alive = "C1-1" in dev.run("return h2.scene()", 10) or "Logos" in dev.run("return h2.scene()", 5)
    print("%-20s load: %-60s game: %s" % (name, out[:60], "alive" if alive else "DOWN"))
    shutil.rmtree(path, ignore_errors=True)
    return alive


# a scene the game does not have is not loaded (the engine crashes on one)
print("dev_loadscene of no scene:", dev.run(r'return h2.dev_loadscene([[MainMenu\MainMenu]])', 10))
results = {}
for name, d in CASES.items():
    if not dev.running():
        os.system('python "%s" --start' % os.path.join(HERE, "..", "dev.py"))
    results[name] = run_case(name, json.dumps(d))
results["deep nesting"] = run_case("deep nesting", DEEP)
results["16 MB+"] = run_case("16 MB+", json.dumps(case(pad="x" * (17 << 20))))
print("\nsurvived %d / %d" % (sum(results.values()), len(results)))
for k, v in results.items():
    if not v:
        print("GAME DOWN:", k)
