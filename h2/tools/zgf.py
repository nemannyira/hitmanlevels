"""Hitman 2 ZGF (compiled scripts + fonts of a scene): a chunk tree.

A chunk: u32 tag, u32 size | flags (bit 31: it has child chunks, bit 30: a
table of u32 after the count; size = the whole chunk), then
  with children:  u32 ?, u32 count, ([u32 n, n x u32] if bit 30), children
  without:        u32 offset of its name or data (bits 30/31 of the size set
                  in the reader's test) or the data at +8.
The ZGF's top chunk (tag 'TFGZ') has one child per file; a file's name is
at child + u32[child + 8] when its flags say so, its data is its first
child's (same rule). ZSysFile looks a path up there (0x5B6280, case- and
slash-insensitive) before the disk.

  python zgf.py <scene>.ZIP|<file.zgf.bin>          list the files
  python zgf.py <zip> <out dir>                       extract them
"""
import os
import struct
import sys
import zipfile
import zlib


def u32(b, o):
    return struct.unpack_from("<I", b, o)[0]


def name_or_data(b, c):
    f = u32(b, c + 4) & 0xC0000000
    return c + u32(b, c + 8) if f else c + 8


def first_child(b, c):
    f = u32(b, c + 4)
    if not f & 0x80000000:
        return None
    if f & 0x40000000:
        return c + 0x14 + u32(b, c + 0x10) * 4
    return c + 0x10


def files(b):
    top = 0
    count = u32(b, top + 0xC)
    c = first_child(b, top)
    out = []
    for _ in range(count):
        size = u32(b, c + 4) & 0x3FFFFFFF
        n = name_or_data(b, c)
        name = b[n:b.index(b"\0", n)].decode("latin1")
        d = first_child(b, c)
        if d is not None:
            dsize = u32(b, d + 4) & 0x3FFFFFFF
            data_at = name_or_data(b, d)
            data = b[data_at:d + dsize]
        else:
            data = b""
        out.append((name, c, size, data))
        c += size
    return out


def load(path):
    if path.lower().endswith(".zip"):
        z = zipfile.ZipFile(path)
        n = [x for x in z.namelist() if x.upper().endswith(".ZGF")][0]
        raw = z.read(n)
        size = u32(raw, 0)
        return zlib.decompressobj(-15).decompress(raw[9:])
    return open(path, "rb").read()


if __name__ == "__main__":
    b = load(sys.argv[1])
    fs = files(b)
    for name, c, size, data in fs:
        print("%08X %7d %7d %s" % (c, size, len(data), name))
    if len(sys.argv) > 2:
        for name, c, size, data in fs:
            p = os.path.join(sys.argv[2], name.replace("\\", os.sep))
            os.makedirs(os.path.dirname(p) or ".", exist_ok=True)
            open(p, "wb").write(data)
