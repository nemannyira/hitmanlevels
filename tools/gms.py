"""Hitman 2 scene reader: <scene>.GMS (compiled geoms) + <scene>.BUF (names/data).

GMS (after the 9 byte header: u32 unpacked size, u32 packed size, u8 flag,
raw deflate) is a blob addressed by byte offsets from its start:
  +0x00 -> geom list: u32 count, count x {u32 rec, u32 extra}
           rec & 0xffffff = record offset / 4, rec >> 25 = groups to pop first
  +0x04 -> message names: u32 count, count x u32 offset (to C strings)
  +0x08 -> ZDefines string
  +0x10 -> type counts, +0x14 -> node pool sizes (24 x u32)

SCompiledGeom record (ZEngineDataBase::CreateGeoms 0x41CDA0):
  +0x00 name (offset in BUF)      +0x04 matrix 3x3 (offset in GMS)
  +0x08 position (offset in GMS)  +0x0C mesh reference (-> node+0x68)
  +0x10 class id (& 0x100000: group, children follow)
  +0x14 events                    +0x18 node flags (low 20 bits)
  +0x1C property data (offset in BUF)  +0x20 data block  +0x28 script
  +0x2D node pool index
"""
import struct
import sys
import zlib
import zipfile


def unpack(data):
    size = struct.unpack_from("<I", data)[0]
    out = zlib.decompressobj(-15).decompress(data[9:])
    assert len(out) == size, (len(out), size)
    return out


class Scene:
    def __init__(self, gms, buf):
        self.gms, self.buf = gms, buf

    def u32(self, o):
        return struct.unpack_from("<I", self.gms, o)[0]

    def floats(self, o, n):
        return struct.unpack_from("<%df" % n, self.gms, o)

    def bufstr(self, o):
        return self.buf[o:self.buf.index(b"\0", o)].decode("latin1")

    def geoms(self):
        table = self.u32(0)
        stack = []  # paths of open groups
        counts = {}
        for i in range(self.u32(table)):
            rec, extra = struct.unpack_from("<II", self.gms, table + 4 + i * 8)
            for _ in range(rec >> 25):
                stack.pop()
            off = (rec & 0xFFFFFF) * 4
            f = struct.unpack_from("<11I", self.gms, off)
            name = self.bufstr(f[0])
            parent = stack[-1] if stack else ""
            key = (parent, name)
            k = counts.get(key, 0)
            counts[key] = k + 1
            path = (parent + "/" if parent else "") + name + ("#%d" % k if k else "")
            g = dict(index=i, path=path, name=name, cls=f[4], mesh=f[3], flags=f[6],
                     pos=self.floats(f[2], 3), mat=self.floats(f[1], 9),
                     props=f[7], events=f[5], data=f[8], script=f[10],
                     pool=self.gms[off + 0x2D], extra=extra)
            yield g
            if f[4] & 0x100000:
                stack.append(path)


def load(zip_path, stem=None):
    z = zipfile.ZipFile(zip_path)
    names = {n.upper().rsplit(".", 1)[1]: n for n in z.namelist()}
    return Scene(unpack(z.read(names["GMS"])), z.read(names["BUF"]))


if __name__ == "__main__":
    s = load(sys.argv[1])
    for g in s.geoms():
        print("%5d %08X mesh=%08X fl=%06X pool=%2d %-60s (%.0f %.0f %.0f)" % (
            g["index"], g["cls"], g["mesh"], g["flags"], g["pool"], g["path"][-60:], *g["pos"]))
