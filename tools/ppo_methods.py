"""Interface method numbers of Hitman 2's compiled scripts (.ppo).

A .ppo has a symbol table (u32 offsets of identifier strings, relative to
the table's end) followed by the pool of the strings (sorted). Byte code calls an interface method as

    97 <operand u8> 00 <symbol u16> <method u16> ...

(e.g. Being2.ppo: 97 14 00 8d 00 0c 00 = MoveToLocation, method 12), the
method number being what the interface's dispatcher gets (call context
+0x10, see re/npc.md). Which interface a call goes to depends on what it is
called on: one name can have other numbers on other interfaces (the list
marks them); calls on the script's own `this` are the sure ones.

  python ppo_methods.py <dir with extracted .ppo files (tools/zgf.py)>
"""
import collections
import os
import re
import struct
import sys


def strings(b):
    return {m.start(): m.group()[:-1].decode() for m in re.finditer(rb"[\x20-\x7e]{1,}\x00", b)}


def symbol_table(b, starts):
    """the symbol table: u32 offsets of identifier strings, relative to the
    table's end (the string pool follows it). Returns the names in order."""
    best = []
    for end in range(4, len(b) + 1):
        names = []
        p = end - 4
        while p >= 0:
            v = struct.unpack_from("<I", b, p)[0]
            if (end + v) not in starts:
                break
            names.append(starts[end + v])
            p -= 4
        if len(names) > len(best):
            best = names[::-1]
    return best


def calls(b, names):
    out = []
    for m in re.finditer(rb"\x97(.)\x00(..)(..)", b, re.S):
        sym = struct.unpack("<H", m.group(2))[0]
        num = struct.unpack("<H", m.group(3))[0]
        if sym < len(names):
            out.append((names[sym], num, m.group(1)[0]))
    return out


def main():
    root = sys.argv[1]
    seen = collections.defaultdict(collections.Counter)
    files = 0
    for dirpath, _, fns in os.walk(root):
        for fn in fns:
            if not fn.lower().endswith(".ppo"):
                continue
            b = open(os.path.join(dirpath, fn), "rb").read()
            st = strings(b)
            names = symbol_table(b, st)
            if len(names) < 5:
                continue
            files += 1
            for name, num, op in calls(b, names):
                seen[name][num] += 1
    print("files", files)
    for name in sorted(seen, key=lambda n: (min(seen[n]), n)):
        c = seen[name]
        flag = "" if len(c) == 1 else "   <- differs"
        print("%-32s %s%s" % (name, ", ".join("%d x%d" % (k, v) for k, v in sorted(c.items())), flag))


if __name__ == "__main__":
    main()
