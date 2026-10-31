"""The level validator (src/levelfmt, build/Release/h2level_check.exe) without the game: hostile and
odd level files, strict (a downloaded level: refused) and lenient (the user's own: accepted, cut), and
every level of the game's Mods folder and of examples/ accepted strictly (the editor writes nothing the
validator refuses). Build first.

  python h2/tests/level_check_test.py
"""
import glob
import json
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
EXE = os.path.join(ROOT, "build", "Release", "h2level_check.exe")
sys.path.insert(0, os.path.join(HERE, "..", "tools"))
import dev  # noqa: E402

results = []


def check(name, ok, detail=""):
    results.append(bool(ok))
    print("%s %s %s" % ("PASS" if ok else "FAIL", name, detail))


def run(text, strict):
    with tempfile.TemporaryDirectory() as d:
        path = os.path.join(d, "level.json")
        with open(path, "w", encoding="utf-8", newline="") as f:
            f.write(text)
        p = subprocess.run([EXE] + (["--strict"] if strict else []) + [path], capture_output=True, text=True)
        return p.returncode, p.stdout


def base(**extra):
    doc = {"format": "h2level", "version": 1, "name": "T", "base_scene": "C1-1\\C1-1__MAIN", "objects": [],
           "spawned": [], "npcs": [], "lists": [], "scripts": ["main.lua"]}
    doc.update(extra)
    return doc


M = [0, 0, 1, 0, 1, 0, 1, 0, 0]
P = {"pos": [1.0, 2.0, 3.0], "matrix": M, "hidden": False}

# (name, document or text, refused when strict, refused when lenient)
CASES = [
    ("a plain level", base(), False, False),
    ("not JSON", "{\"format\": \"h2level\",", True, True),
    ("not an h2level file", {"format": "other"}, True, True),
    ("a field the editor does not write", base(autoexec="x"), True, False),
    ("unknown field in a placed object", base(spawned=[dict(P, id="a", kind="point", evil=1)]), True, False),
    ("script outside the folder", base(scripts=["..\\..\\autoexec.lua"]), True, False),
    ("script device name", base(scripts=["CON.lua"]), True, False),
    ("script COM1", base(scripts=["com1.lua"]), True, False),
    ("script not .lua", base(scripts=["main.dll"]), True, False),
    ("script twice", base(scripts=["main.lua", "main.lua"]), True, False),
    ("17 scripts", base(scripts=["s%d.lua" % i for i in range(17)]), True, False),
    ("position out of range", base(spawned=[dict(P, id="a", pos=[1e9, 0, 0])]), True, False),
    ("matrix of 8", base(spawned=[dict(P, id="a", matrix=M[:8])]), True, False),
    ("a number as text", base(spawned=[dict(P, id="a", pos=["1", 0, 0])]), True, False),
    ("a control character in a name", base(name="a\u0007b"), True, False),
    ("a briefing with new lines", base(mission={"briefing": "line 1\nline 2"}), False, False),
    ("a goal not of the editor's", base(mission={"objectives": [{"slot": 1, "goal": "format_c"}]}), True, False),
    ("a laptop line 7", base(mission={"objectives": [{"slot": 7, "goal": "level"}]}), True, False),
    ("an icon not of the legend", base(spawned=[dict(P, id="a", kind="point", map_icon="Icon_Evil")]), True, False),
    ("257 points on the map",
     base(spawned=[dict(P, id="p%d" % i, kind="point", map_icon="Icon_Exit") for i in range(257)]), True, False),
    ("2001 placed objects", base(spawned=[dict(P, id="p%d" % i, kind="point") for i in range(2001)]), True, False),
    ("ids twice", base(spawned=[dict(P, id="a", kind="point"), dict(P, id="a", kind="point")]), True, False),
    ("a base scene with ..", base(base_scene="..\\..\\Windows"), True, False),
    ("a map of its own with a slash", base(own_map_of="a/b"), True, False),
    ("a vehicle's 5 wheels", base(vehicles=[{"object": "x", "wheels": [[0, 0, 0]] * 5}]), True, False),
    ("a vehicle's speed NaN-like", base(vehicles=[{"object": "x", "speed": 1e300}]), True, False),
    ("deep nesting", '{"format":"h2level","x":' + "[" * 20000 + "]" * 20000 + "}", True, False),
    ("a huge string", base(name="x" * 100000), True, False),
    ("music not an object", base(music=["a"]), True, False),
    ("a list of 257 points", base(lists=[{"path": "ROOT/x", "points": ["ROOT/p"] * 257}]), True, False),
    # an escalation (#24): stages, rules, the stages of what is placed
    ("an escalation", base(stages=[{"name": "One", "text": "a\nb"}, {"name": "Two"}],
                           rules=[{"kind": "time", "seconds": 60, "stage": 2, "broken": "note"},
                                  {"kind": "kill_with", "weapon": "fiberwire"}, {"kind": "no_firearm"},
                                  {"kind": "kill_with", "weapon": "GunHardballer"}], replay=False,
                           spawned=[dict(P, id="a", kind="point", stage=2)],
                           npcs=[{"pos": [1.0, 2.0, 3.0], "matrix": M, "id": "n", "record": 1, "model": "M", "gone_from": 2}],
                           mission={"objectives": [{"slot": 1, "goal": "level", "stage": 1, "gone_from": 2}]}),
     False, False),
    ("11 stages", base(stages=[{"name": "S"}] * 11), True, False),
    ("no stages ([])", base(stages=[]), True, False),
    ("a stage of a level without stages", base(spawned=[dict(P, id="a", kind="point", stage=2)]), True, False),
    ("a stage past the last", base(stages=[{"name": "S"}], spawned=[dict(P, id="a", kind="point", stage=2)]),
     True, False),
    ("gone before it comes", base(stages=[{"name": "S"}] * 3, spawned=[dict(P, id="a", kind="point", stage=3,
                                                                          gone_from=2)]), True, False),
    ("a rule not of the editor's", base(stages=[{"name": "S"}], rules=[{"kind": "format_c"}]), True, False),
    ("a rule broken neither fail nor note", base(stages=[{"name": "S"}], rules=[{"kind": "unseen", "broken": "x"}]),
     True, False),
    ("rules without stages", base(rules=[{"kind": "unseen"}]), True, False),
    ("a weapon name with a path", base(stages=[{"name": "S"}], rules=[{"kind": "kill_with", "weapon": "../x"}]),
     True, False),
    ("a time limit of 1 s", base(stages=[{"name": "S"}], rules=[{"kind": "time", "seconds": 1}]), True, False),
    # doors locked by the level (#35)
    ("a door locked", base(doors=[{"path": "ROOT/a/Door_01", "locked": True}]), False, False),
    ("a door locked \"yes\"", base(doors=[{"path": "ROOT/a/Door_01", "locked": "yes"}]), True, False),
    ("a door with a key it may not have", base(doors=[{"path": "ROOT/a/Door_01", "key": "x"}]), True, False),
]

if not os.path.exists(EXE):
    print("FAIL build first: %s" % EXE)
    sys.exit(1)
for name, doc, strictRefused, lenientRefused in CASES:
    text = doc if isinstance(doc, str) else json.dumps(doc)
    sc, sout = run(text, True)
    lc, lout = run(text, False)
    check(name, (sc == 1) == strictRefused and (lc == 1) == lenientRefused,
          "strict %s, lenient %s | %s" % ("refused" if sc else "accepted", "refused" if lc else "accepted",
                                          (sout.splitlines()[1:2] or [""])[0][:100]))

# the levels the editor made: all accepted strictly
levels = [d for d in glob.glob(os.path.join(dev.GAME, "Mods", "*")) + glob.glob(os.path.join(ROOT, "h2", "examples", "missions", "*"))
          if os.path.exists(os.path.join(d, "level.json")) and not os.path.basename(d).startswith("_Fuzz")]
refused = []
for d in levels:
    p = subprocess.run([EXE, "--strict", d], capture_output=True, text=True)
    if p.returncode != 0:
        refused.append(os.path.basename(d) + ": " + (p.stdout.splitlines()[1:2] or [""])[0])
check("%d levels of Mods and examples accepted strictly" % len(levels), not refused, "; ".join(refused[:3]))

print("\n%d passed, %d failed" % (results.count(True), results.count(False)))
sys.exit(0 if all(results) else 1)
