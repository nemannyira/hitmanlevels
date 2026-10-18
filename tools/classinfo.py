"""The game's registered classes and routines, from the unpacked image.

Every geom class and every routine (ZEventBase) is registered by a static
initialiser: routines by 0x4462C0 (ZCLASSINFO*, name, id, ?, short name, base
name, ..., factory = 13th argument), geom classes by 0x42B5E0 (ZCLASSINFO*,
name, size, base name, id, ?, ?, factory = 7th argument); ECX is the class
info. The factory allocates and calls the constructor, which writes the
class's vtable last. Lists each one with its class info, factory, constructor
and vtable, and (with --json) writes them for naming in Ghidra.

Usage: python classinfo.py [--json out.json] [image=re/hitman2_dump.exe]
"""
import json
import os
import struct
import sys
from collections import deque

import capstone
import pefile

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT = os.path.join(HERE, "..", "re", "hitman2_dump.exe")
REGISTER_ROUTINE, REGISTER_CLASS = 0x4462C0, 0x42B5E0


class Image:
    def __init__(self, path):
        self.pe = pefile.PE(path)
        self.base = self.pe.OPTIONAL_HEADER.ImageBase
        self.mem = self.pe.get_memory_mapped_image()
        # the unpacked dump is one section: its code ends where the import table opens .rdata
        iat = [i.address for e in getattr(self.pe, "DIRECTORY_ENTRY_IMPORT", []) for i in e.imports]
        start = self.base + self.pe.sections[0].VirtualAddress
        end = self.base + self.pe.OPTIONAL_HEADER.SizeOfImage
        split = min(iat) if iat else end
        self.sections = [(start, split, True), (split, end, False)]
        self.cs = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
        self.cs.detail = True

    def section(self, va):
        for lo, hi, x in self.sections:
            if lo <= va < hi:
                return x
        return None

    def is_code(self, va):
        return self.section(va) is True

    def is_data(self, va):
        return self.section(va) is False

    def dword(self, va):
        o = va - self.base
        return struct.unpack_from("<I", self.mem, o)[0] if 0 <= o <= len(self.mem) - 4 else 0

    def cstr(self, va):
        if not va or not self.is_data(va):
            return None
        o = va - self.base
        end = self.mem.find(b"\0", o, o + 96)
        s = self.mem[o:end] if end > o else b""
        return s.decode("latin1") if s and all(32 <= c < 127 for c in s) else None

    def code(self, va, size):
        o = va - self.base
        return self.cs.disasm(bytes(self.mem[o:o + size]), va)

    def function(self, va, limit=0x800):
        """Instructions from va to its first return (a constructor's straight-line body)."""
        out = []
        for i in self.code(va, limit):
            out.append(i)
            if i.mnemonic in ("ret", "retn") or (i.mnemonic == "jmp" and not out[:-1]):
                break
        return out

    def is_vtable(self, va):
        return self.is_data(va) and self.is_code(self.dword(va)) and self.is_code(self.dword(va + 4))


def imm(op):
    return op.imm & 0xFFFFFFFF if op.type == capstone.x86.X86_OP_IMM else None


def call_sites(img):
    """(call address, target) of every direct call to the two registrations."""
    out = []
    for lo, hi, x in img.sections:
        if not x:
            continue
        data = bytes(img.mem[lo - img.base:hi - img.base])
        at = 0
        while True:
            at = data.find(b"\xE8", at)
            if at < 0 or at + 5 > len(data):
                break
            target = (lo + at + 5 + struct.unpack_from("<i", data, at + 1)[0]) & 0xFFFFFFFF
            if target in (REGISTER_ROUTINE, REGISTER_CLASS):
                out.append((lo + at, target))
            at += 1
    return out


def arguments(img, call, count):
    """The pushed arguments (first first) and ECX before a call, from the 0x80 bytes before it."""
    for back in range(0x80, 0, -1):
        ins = list(img.code(call - back, back + 5))
        if not ins or ins[-1].address != call or ins[-1].mnemonic != "call":
            continue
        pushes, ecx = [], None
        for i in reversed(ins[:-1]):
            if i.mnemonic == "push" and len(pushes) < count:
                pushes.append(imm(i.operands[0]))
            elif i.mnemonic == "sub" and i.op_str.startswith("esp,"):  # a struct argument, copied
                pushes.extend([None] * (imm(i.operands[1]) // 4))
            elif i.mnemonic == "mov" and i.op_str.startswith("ecx,") and ecx is None:
                ecx = imm(i.operands[1])
            elif i.mnemonic in ("call", "ret"):
                break
        if len(pushes) >= count:
            return pushes[:count], ecx
    return None, None


def constructor(img, factory):
    """(constructor, vtable): the first function (breadth first from the factory) writing a vtable to [reg]."""
    queue, seen = deque([(factory, 0)]), set()
    while queue:
        va, depth = queue.popleft()
        if va in seen or depth > 3 or not img.is_code(va):
            continue
        seen.add(va)
        vt, calls = None, []
        for i in img.function(va):
            if i.mnemonic == "mov" and len(i.operands) == 2:
                d, s = i.operands
                if (d.type == capstone.x86.X86_OP_MEM and d.mem.disp == 0 and d.mem.index == 0 and d.size == 4
                        and s.type == capstone.x86.X86_OP_IMM and img.is_vtable(imm(s))):
                    vt = imm(s)
            elif i.mnemonic == "call" and i.operands[0].type == capstone.x86.X86_OP_IMM:
                calls.append(imm(i.operands[0]))
        if vt:
            return va, vt
        queue.extend((c, depth + 1) for c in calls)
    return None, None


def registrations(img):
    out = []
    for call, target in sorted(call_sites(img)):
        routine = target == REGISTER_ROUTINE
        args, ecx = arguments(img, call, 13 if routine else 7)
        if not args:
            continue
        name = img.cstr(args[0])
        if not name:
            continue
        factory = args[12] if routine else args[6]
        ctor, vt = constructor(img, factory) if factory else (None, None)
        out.append({
            "kind": "routine" if routine else "class",
            "name": name,
            "id": args[1] if routine else args[3],
            "short": img.cstr(args[3]) if routine else None,
            "base": img.cstr(args[4]) if routine else img.cstr(args[2]),
            "size": None if routine else args[1],
            "register_call": call,
            "classinfo": ecx,
            "factory": factory,
            "ctor": ctor,
            "vtable": vt,
        })
    return out


def vfuncs(img, regs):
    """Functions only one registered vtable has: {address: "<class>_vf<offset>"}."""
    starts = {r["vtable"] for r in regs if r["vtable"]}
    users = {}
    for r in regs:
        vt = r["vtable"]
        if not vt:
            continue
        for k in range(400):
            slot = vt + k * 4
            if k and slot in starts:
                break
            f = img.dword(slot)
            if not img.is_code(f):
                break
            users.setdefault(f, set()).add("%s_vf%X" % (r["name"], k * 4))
    return {f: next(iter(n)) for f, n in users.items() if len(n) == 1}


def main():
    args = sys.argv[1:]
    out_json = None
    if args[:1] == ["--json"]:
        out_json, args = args[1], args[2:]
    img = Image(args[0] if args else DEFAULT)
    regs = registrations(img)
    h = lambda v: "%08X" % v if v else "-"
    for r in regs:
        print("%-7s %-40s id=%-6s base=%-14s ci=%s factory=%s ctor=%s vtable=%s" % (
            r["kind"], r["name"], "0x%X" % r["id"] if r["id"] is not None else "?", r["base"] or "-",
            h(r["classinfo"]), h(r["factory"]), h(r["ctor"]), h(r["vtable"])))
    vf = vfuncs(img, regs)
    print("%d registrations (%d with a vtable), %d vfuncs of one class" % (
        len(regs), sum(1 for r in regs if r["vtable"]), len(vf)), file=sys.stderr)
    if out_json:
        with open(out_json, "w") as f:
            json.dump({"registrations": regs, "vfuncs": {"%08X" % k: v for k, v in vf.items()}}, f, indent=1)


if __name__ == "__main__":
    main()
