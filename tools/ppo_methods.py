"""Interface method numbers of Hitman 2's compiled scripts (.ppo).

A .ppo has a symbol table (u32 offsets of identifier strings, relative to
the table's end) followed by the pool of the strings (sorted). Byte code calls an interface method as

    97 <operand u8> 00 <symbol u16> <method u16> ...

(e.g. Being2.ppo: 97 14 00 8d 00 0c 00 = MoveToLocation, method 12), the
method number being what the interface's dispatcher gets (call context
+0x10, see re/npc.md). Which interface a call goes to depends on what it is
called on: one name can have other numbers on other interfaces (the list
marks them); calls on the script's own `this` are the sure ones.

After the string pool comes the table of the script's interface slots (a
u32 count, then {u32 slot, u16 symbol}); the call's operand is that slot, so
it says which interface the method is on (docs/script_commands.md).

  python ppo_methods.py <dir with extracted .ppo files (tools/zgf.py)>
  python ppo_methods.py --interfaces <dir>   per interface: name, numbers
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


def interface_slots(b, names):
    """{slot: interface name} from the table after the string pool; {} when
    the script uses no interface itself"""
    best = {}
    for m in re.finditer(rb"(?=(.)\x00\x00\x00)", b, re.S):
        o, n = m.start(), b[m.start()]
        if not 1 <= n <= 60 or o + 4 + 6 * n > len(b):
            continue
        ents = [struct.unpack_from("<IH", b, o + 4 + 6 * i) for i in range(n)]
        if (n > len(best) and all(s < len(names) and slot < 0x400 and slot % 4 == 0 for slot, s in ents)
                and any(names[s].lower().endswith("_if") for _, s in ents)):
            best = {slot: names[s] for slot, s in ents}
    return best


def interfaces(root):
    seen = collections.defaultdict(collections.Counter)
    for dirpath, _, fns in os.walk(root):
        for fn in fns:
            if not fn.lower().endswith(".ppo"):
                continue
            b = open(os.path.join(dirpath, fn), "rb").read()
            names = symbol_table(b, strings(b))
            if len(names) < 5:
                continue
            slots = interface_slots(b, names)
            for m in re.finditer(rb"\x97(.)\x00(..)(..)", b, re.S):
                slot = m.group(1)[0]
                sym, num = struct.unpack("<HH", m.group(2) + m.group(3))
                if sym < len(names) and slots.get(slot, "").lower().endswith("_if"):
                    seen[(slots[slot], names[sym])][num] += 1
    for (iface, name), c in sorted(seen.items()):
        print("%-18s %-34s %s" % (iface, name, " ".join("%d:x%d" % kv for kv in sorted(c.items()))))


def main():
    if sys.argv[1] == "--interfaces":
        interfaces(sys.argv[2])
        return
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
