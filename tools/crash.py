"""The exceptions of H2Fix.log with the functions they happened in.

src/crashlog.cpp logs every exception outside the editor's own guarded probes
("EXCEPTION <code> at <addr> <module>+<offset>", the registers, then the code
addresses found on the stack). This prints those blocks with the name of each
hitman2.exe address's function from the running Ghidra project (its HTTP
server, 127.0.0.1:8089, program hitman2_dump.exe), so a crash reads as a call
chain.

Usage: python crash.py [H2Fix.log] [--last N]   (default: the game's log, the last 3)
"""
import json
import os
import re
import sys
import urllib.parse
import urllib.request

GAME = r"C:\Program Files (x86)\Steam\steamapps\common\Hitman 2 Silent Assassin"
GHIDRA = "http://127.0.0.1:8089"
PROGRAM = "hitman2_dump.exe"
_names = {}


def function_name(addr):
    if addr in _names:
        return _names[addr]
    name = None
    try:
        q = urllib.parse.urlencode({"address": "0x%08x" % addr, "program": PROGRAM})
        with urllib.request.urlopen("%s/get_function_by_address?%s" % (GHIDRA, q), timeout=10) as r:
            d = json.loads(r.read())
            if isinstance(d, dict) and d.get("name"):
                name = "%s+%X" % (d["name"], addr - int(d.get("entry_point", "%x" % addr), 16))
    except Exception:
        pass
    _names[addr] = name
    return name


def blocks(lines):
    cur = None
    for line in lines:
        if "EXCEPTION " in line:
            if cur:
                yield cur
            cur = [line.rstrip()]
        elif cur is not None and re.search(r"\]\s+(eax=|\[esp\+)", line):
            cur.append(line.rstrip())
        elif cur is not None:
            yield cur
            cur = None
    if cur:
        yield cur


def annotate(line):
    m = re.search(r"([0-9A-F]{8}) hitman2\.exe\+[0-9A-F]+", line)
    if not m:
        return line
    name = function_name(int(m.group(1), 16))
    return line + ("   <- " + name if name else "")


def main():
    args = sys.argv[1:]
    last = 3
    if "--last" in args:
        i = args.index("--last")
        last = int(args[i + 1])
        del args[i:i + 2]
    path = args[0] if args else os.path.join(GAME, "H2Fix.log")
    with open(path, encoding="latin1") as f:
        found = list(blocks(f))
    if not found:
        print("no exceptions in", path)
        return
    for b in found[-last:]:
        for line in b:
            print(annotate(line))
        print()


if __name__ == "__main__":
    main()
