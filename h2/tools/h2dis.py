"""Quick disassembler for the unpacked hitman2 image with import/export names.

Usage: python h2dis.py <hex addr> [count=40] [image=re/hitman2_dump.exe]
"""
import os, re, sys

import capstone
import pefile

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT = os.path.join(HERE, "..", "..", "re", "hitman2_dump.exe")


def load(path):
    pe = pefile.PE(path)
    base = pe.OPTIONAL_HEADER.ImageBase
    names = {}
    for e in getattr(pe, "DIRECTORY_ENTRY_IMPORT", []):
        for i in e.imports:
            names[i.address] = "%s!%s" % (e.dll.decode().split(".")[0], i.name.decode() if i.name else i.ordinal)
    exports = pe.DIRECTORY_ENTRY_EXPORT.symbols if hasattr(pe, "DIRECTORY_ENTRY_EXPORT") else []
    for e in exports:
        if e.name:
            names[base + e.address] = e.name.decode()
    return pe.get_memory_mapped_image(), base, names


def main():
    addr = int(sys.argv[1], 16)
    count = int(sys.argv[2]) if len(sys.argv) > 2 else 40
    img, base, names = load(sys.argv[3] if len(sys.argv) > 3 else DEFAULT)
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    code = img[addr - base: addr - base + count * 16]
    for n, ins in enumerate(md.disasm(code, addr)):
        if n >= count:
            break
        ops = ins.op_str
        notes = [names[int(h, 16)] for h in re.findall(r"0x[0-9a-f]+", ops) if int(h, 16) in names]
        if ins.address in names:
            print("%s:" % names[ins.address])
        print("%08X  %-7s %s%s" % (ins.address, ins.mnemonic, ops, ("   ; " + ", ".join(notes)) if notes else ""))


if __name__ == "__main__":
    main()
