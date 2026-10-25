"""References to an address in the unpacked hitman2 image: calls / jumps
(rel32) and absolute 32-bit values (pushes, movs, vtables), with the
function each one is in (the nearest export before it).

Usage: python h2xref.py <hex addr> [image=re/hitman2_dump.exe]
"""
import bisect
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from h2dis import DEFAULT, load  # noqa: E402


def main():
    target = int(sys.argv[1], 16)
    img, base, names = load(sys.argv[2] if len(sys.argv) > 2 else DEFAULT)
    funcs = sorted(a for a in names if a >= base)
    def where(a):
        i = bisect.bisect_right(funcs, a) - 1
        return "%s+%X" % (names[funcs[i]], a - funcs[i]) if i >= 0 else "?"
    for at in range(len(img) - 5):
        op = img[at]
        if op in (0xE8, 0xE9):
            if base + at + 5 + struct.unpack_from("<i", img, at + 1)[0] == target:
                print("%08X  %s  %s" % (base + at, "call" if op == 0xE8 else "jmp ", where(base + at)))
        if struct.unpack_from("<I", img, at)[0] == target:
            print("%08X  abs   %s" % (base + at, where(base + at)))


if __name__ == "__main__":
    main()
