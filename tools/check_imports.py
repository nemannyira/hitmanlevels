"""Offline check of the model import rules (src/engine/imports.cpp) on every
level: for each model of the catalogue (plain objects) and each character
body, the references a copy would follow, drop or not know.

Usage: python tools/check_imports.py [level ...]
"""
import collections
import glob
import os
import struct
import sys
import zipfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gms  # noqa: E402

GAME = r"C:\Program Files (x86)\Steam\steamapps\common\Hitman 2 Silent Assassin"
RECORD, SKIN_RECORD, SKINNED = 0x84, 0xA8, 0x10001


def u32(b, at):
    return struct.unpack_from("<I", b, at)[0] if at + 4 <= len(b) else 0


def plain(prm, mesh):
    """CopyChain: records, bounds, vertices and strips, pieces closer than
    4 KB copied together; pointers +08 +18 +1C +2C +44 moved, others dropped"""
    recs = []
    r = mesh
    while r and len(recs) < 64 and r + RECORD <= len(prm) and r not in recs:
        recs.append(r)
        r = u32(prm, r + 8)
    spans = []
    for r in recs:
        for at, size in ((r, RECORD), (u32(prm, r + 0x18), 0x3C), (u32(prm, r + 0x1C), (u32(prm, r + 0x14) >> 16) * 36),
                         (u32(prm, r + 0x44), u32(prm, r + 0x48) * 2)):
            if at and size and at + size <= len(prm):
                spans.append([at, at + size])
    spans.sort()
    merged = []
    for a, b in spans:
        if merged and a <= merged[-1][1] + 0x1000:
            merged[-1][1] = max(merged[-1][1], b)
        else:
            merged.append([a, b])
    dropped = sum(1 for r in recs for f in (0x08, 0x18, 0x1C, 0x2C, 0x44)
                  if u32(prm, r + f) and not any(a <= u32(prm, r + f) < b for a, b in merged))
    types = {u32(prm, r) for r in recs}
    return sum(b - a for a, b in merged), dropped, types


def skinned(prm, mesh):
    recs = []
    r = mesh
    while r and len(recs) < 64 and r + SKIN_RECORD <= len(prm) and u32(prm, r) == SKINNED and r not in recs:
        recs.append(r)
        r = u32(prm, r + 8)
    blocks = {}
    def want(at, size):
        if at and size and at + size <= len(prm) and size <= 0x400000:
            blocks[at] = max(blocks.get(at, 0), size)
            return True
        return False
    unknown = 0
    for r in recs:
        bones, count = u32(prm, r + 0x84), u32(prm, r + 0x14) >> 16
        want(u32(prm, r + 0x18), 0x3C)
        want(u32(prm, r + 0x1C), count * 52)
        want(u32(prm, r + 0x44), u32(prm, r + 0x48) * 2)
        for i, per in enumerate((48, 28, 48, 36, 48)):
            want(u32(prm, r + 0x88 + 4 * i), bones * per)
        want(u32(prm, r + 0xA4), u32(prm, r + 0xA0) * 8)
        sk = u32(prm, r + 0x9C)
        if sk and sk not in blocks:
            end = sk + 0x50
            while end + 0x88 <= len(prm) and end < sk + 0x8000 and not (
                    u32(prm, end) == SKINNED and u32(prm, end + 0x84) == bones):
                end += 2
            if end >= sk + 0x8000:
                unknown += 1
            want(sk, end - sk)
    size = sum(blocks.values()) + SKIN_RECORD * len(recs)
    return len(recs), size, unknown


def main():
    levels = sys.argv[1:] or [os.path.basename(z).split("__")[0]
                              for z in sorted(glob.glob(GAME + r"\Scenes\*\*__MAIN.ZIP"))]
    total = collections.Counter()
    for lv in levels:
        z = GAME + "\\Scenes\\%s\\%s__MAIN.ZIP" % (lv, lv)
        prm = zipfile.ZipFile(z).read("SCENES/%s/%s__MAIN.PRM" % (lv, lv))
        s = gms.load(z)
        a, b, c, n = struct.unpack_from("<4I", prm, 0)
        table = set(struct.unpack_from("<%dI" % n, prm, c))
        seen = set()
        big = drop = other = ok = 0
        bodies = bad_bodies = 0
        for g in s.geoms():
            m = g["mesh"]
            if not m or m in seen:
                continue
            seen.add(m)
            if g["cls"] == 0x2003E9 and u32(prm, m) == SKINNED:
                count, size, unknown = skinned(prm, m)
                bodies += 1
                bad_bodies += unknown > 0 or size > 0x400000
                continue
            if m not in table or u32(prm, m) != 0 or not (g["flags"] & 1):
                continue
            span, dropped, types = plain(prm, m)
            if span > 0x400000:
                big += 1
            elif dropped:
                drop += 1
            elif types != {0}:
                other += 1
            else:
                ok += 1
        print("%-6s models ok %4d, too big %3d, references dropped %3d, other record types %3d | bodies %3d, "
              "skeleton end not found / too big %d" % (lv, ok, big, drop, other, bodies, bad_bodies))
        total.update(ok=ok, big=big, drop=drop, other=other, bodies=bodies, bad=bad_bodies)
    print("all:", dict(total))


if __name__ == "__main__":
    main()
