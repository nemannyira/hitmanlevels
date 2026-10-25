"""Hitman 2's path network (ZPathfinder3, class 0x200075): its record's data
block holds the precomputed network (read by 0x5995A0 -> 0x597C00 ->
0x59F0B0 into the 0x229-byte object at pathfinder +0x64).

Data block (DBLK): tag 7 (u32 length + bytes) =
  u32 magic 0x1923483 (0x1923482: other flavour), u8 flag, then
  u32 ?, u16 A, u16 n1, n1 x 22 bytes, u16 n2, n2 x 12, u16 n3, n3*A x 4,
  u16 n4, n4 x 4, u16 n5, n5 x 10, u16 n6, n6 x 12, [n6 x 4 when the
  object's +4 flag], u16 n7, n7 x 4, u16 n8, n8 x 10 + n8 x 1, u16 n9, n9 x 1,
  17 x u32 (bounds, a matrix...)

  python nav.py <scene>__MAIN.ZIP
"""
import struct
import sys

import gms


def u16(b, o):
    return struct.unpack_from("<H", b, o)[0]


def u32(b, o):
    return struct.unpack_from("<I", b, o)[0]


def network(scene_zip):
    s = gms.load(scene_zip)
    for g in s.geoms():
        if g["cls"] == 0x200075:
            d = g["data"]
            hdr = u32(s.gms, d)
            p = d + 4
            tag = s.gms[p]
            assert tag & 0x3F == 7, hex(tag)
            n = u32(s.gms, p + 1)
            return g, s.gms[p + 5:p + 5 + n]
    return None, None


def parse(b, extra_flag=False):
    out = {}
    out["magic"] = u32(b, 0)
    out["flag"] = b[4]
    p = 5
    out["x"] = u32(b, p)
    out["A"] = u16(b, p + 4)
    p += 6

    def table(name, size):
        nonlocal p
        n = u16(b, p)
        p += 2
        out[name] = (n, p)
        p += n * size

    table("t1", 22)
    table("t2", 12)
    n3 = u16(b, p)
    p += 2
    out["t3"] = (n3, p)
    p += n3 * out["A"] * 4
    table("t4", 4)
    table("t5", 10)
    table("t6", 12)
    if extra_flag:
        p += out["t6"][0] * 4
    table("t7", 4)
    n8 = u16(b, p)
    p += 2
    out["t8"] = (n8, p)
    p += n8 * 10 + n8
    table("t9", 1)
    out["tail"] = struct.unpack_from("<17f", b, p)
    out["end"] = p + 68
    return out


if __name__ == "__main__":
    g, b = network(sys.argv[1])
    print(g["path"], len(b))
    n = parse(b)
    for k, v in n.items():
        print(k, v if not isinstance(v, tuple) or len(v) != 2 else "count=%d at %d" % v)
    print("total", len(b))
