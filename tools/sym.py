"""Symbolize DINPUT8.dll+offset addresses from H2Fix.log with the linker map.

  python tools/sym.py C0AC D5E64 ...      offsets (hex) from "DINPUT8.dll+XXXX"
  python tools/sym.py --log                the DINPUT8 frames of the last exception in H2Fix.log
"""
import bisect
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MAP = os.path.join(ROOT, "build", "Release", "dinput8.map")
LOG = r"C:\Program Files (x86)\Steam\steamapps\common\Hitman 2 Silent Assassin\H2Fix.log"


def load():
    base = 0x10000000
    syms = []
    for line in open(MAP, encoding="latin1"):
        m = re.match(r"\s*Preferred load address is ([0-9a-fA-F]+)", line)
        if m:
            base = int(m.group(1), 16)
        m = re.match(r"\s*[0-9a-fA-F]{4}:[0-9a-fA-F]{8}\s+(\S+)\s+([0-9a-fA-F]{8})\s+(f\s+)?(\S+)", line)
        if m:
            syms.append((int(m.group(2), 16), m.group(1), m.group(4)))
    syms.sort()
    return base, syms


def undecorate(name):
    m = re.match(r"\?([^@]+)@(.*)", name)
    if not m:
        return name
    parts = [m.group(1)]
    for p in m.group(2).split("@"):
        if not p or p[0] in "0123456789$" or p.startswith("Y") or p.startswith("Q") or p.startswith("A"):
            break
        parts.append(p)
    return "::".join(reversed(parts))


def main():
    base, syms = load()
    addrs = [s[0] for s in syms]
    offs = []
    if sys.argv[1:] == ["--log"]:
        lines = open(LOG, encoding="latin1").read().splitlines()
        last = max(i for i, l in enumerate(lines) if "EXCEPTION" in l)
        start = last
        # the first exception of the last burst
        for i in range(last, -1, -1):
            if "EXCEPTION" in lines[i]:
                start = i
            elif "devcmd" in lines[i] or "PushScene" in lines[i]:
                break
        print(lines[start])
        for l in lines[start + 1:start + 30]:
            m = re.search(r"DINPUT8\.dll\+([0-9A-Fa-f]+)", l)
            if m:
                offs.append(int(m.group(1), 16))
            elif "EXCEPTION" in l:
                break
    else:
        offs = [int(a, 16) for a in sys.argv[1:]]
    for off in offs:
        va = base + off
        i = bisect.bisect_right(addrs, va) - 1
        if i < 0:
            print("%08X ?" % off)
            continue
        a, name, obj = syms[i]
        print("%08X %s+0x%X  (%s)" % (off, undecorate(name), va - a, obj))


main()
