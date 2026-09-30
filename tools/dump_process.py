"""Launch hitman2.exe, wait, then dump the main image, all loaded modules and
all private executable memory regions to an output directory.

Usage: python dump_process.py <out_dir> [wait_seconds] [--attach]
"""
import ctypes, ctypes.wintypes as wt, os, sys, time, subprocess, json

GAME_DIR = r"C:\Program Files (x86)\Steam\steamapps\common\Hitman 2 Silent Assassin"
EXE = os.path.join(GAME_DIR, "hitman2.exe")

k32 = ctypes.WinDLL("kernel32", use_last_error=True)
psapi = ctypes.WinDLL("psapi", use_last_error=True)

PROCESS_ALL_ACCESS = 0x1F0FFF
MEM_COMMIT = 0x1000
LIST_MODULES_32BIT = 1


class MBI(ctypes.Structure):
    _fields_ = [("BaseAddress", ctypes.c_uint64), ("AllocationBase", ctypes.c_uint64),
                ("AllocationProtect", wt.DWORD), ("__a1", wt.DWORD),
                ("RegionSize", ctypes.c_uint64), ("State", wt.DWORD),
                ("Protect", wt.DWORD), ("Type", wt.DWORD), ("__a2", wt.DWORD)]


k32.VirtualQueryEx.argtypes = [wt.HANDLE, ctypes.c_void_p, ctypes.POINTER(MBI), ctypes.c_size_t]
k32.VirtualQueryEx.restype = ctypes.c_size_t
k32.ReadProcessMemory.argtypes = [wt.HANDLE, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t,
                                  ctypes.POINTER(ctypes.c_size_t)]
k32.OpenProcess.restype = wt.HANDLE
psapi.EnumProcessModulesEx.argtypes = [wt.HANDLE, ctypes.POINTER(ctypes.c_void_p), wt.DWORD,
                                       ctypes.POINTER(wt.DWORD), wt.DWORD]
psapi.GetModuleFileNameExW.argtypes = [wt.HANDLE, ctypes.c_void_p, wt.LPWSTR, wt.DWORD]


class MODINFO(ctypes.Structure):
    _fields_ = [("base", ctypes.c_void_p), ("size", wt.DWORD), ("entry", ctypes.c_void_p)]


psapi.GetModuleInformation.argtypes = [wt.HANDLE, ctypes.c_void_p, ctypes.POINTER(MODINFO), wt.DWORD]


def read(h, addr, size):
    buf = (ctypes.c_ubyte * size)()
    out = bytearray(size)
    # read page by page so unreadable pages don't kill the whole read
    for off in range(0, size, 0x1000):
        n = ctypes.c_size_t(0)
        chunk = min(0x1000, size - off)
        if k32.ReadProcessMemory(h, addr + off, ctypes.byref(buf, off), chunk, ctypes.byref(n)):
            out[off:off + chunk] = bytes(buf[off:off + chunk])
    return bytes(out)


def main():
    out_dir = sys.argv[1]
    wait = float(sys.argv[2]) if len(sys.argv) > 2 else 8
    os.makedirs(out_dir, exist_ok=True)
    if "--attach" in sys.argv:
        pid = int(subprocess.check_output(
            ["powershell", "-c", "(Get-Process hitman2).Id"]).strip())
    else:
        p = subprocess.Popen([EXE], cwd=GAME_DIR)
        pid = p.pid
        time.sleep(wait)
    h = k32.OpenProcess(PROCESS_ALL_ACCESS, False, pid)
    if not h:
        raise SystemExit("OpenProcess failed %d" % ctypes.get_last_error())

    mods = (ctypes.c_void_p * 1024)()
    need = wt.DWORD()
    psapi.EnumProcessModulesEx(h, mods, ctypes.sizeof(mods), ctypes.byref(need), LIST_MODULES_32BIT)
    info = {"modules": [], "regions": []}
    mod_ranges = []
    for i in range(need.value // ctypes.sizeof(ctypes.c_void_p)):
        name = ctypes.create_unicode_buffer(520)
        psapi.GetModuleFileNameExW(h, mods[i], name, 520)
        mi = MODINFO()
        psapi.GetModuleInformation(h, mods[i], ctypes.byref(mi), ctypes.sizeof(mi))
        info["modules"].append({"name": name.value, "base": hex(mi.base), "size": hex(mi.size)})
        mod_ranges.append((mi.base, mi.base + mi.size))
        low = name.value.lower()
        if "windows\\sys" in low or "windows\\winsxs" in low:
            continue
        data = read(h, mi.base, mi.size)
        fn = "%08X_%s.bin" % (mi.base, os.path.basename(name.value))
        open(os.path.join(out_dir, fn), "wb").write(data)

    # private / non-module committed regions (unpacked DLLs mapped manually)
    addr = 0
    mbi = MBI()
    while addr < 0x80000000:
        if not k32.VirtualQueryEx(h, addr, ctypes.byref(mbi), ctypes.sizeof(mbi)):
            break
        base, size = mbi.BaseAddress, mbi.RegionSize
        if mbi.State == MEM_COMMIT and not any(a <= base < b for a, b in mod_ranges):
            info["regions"].append({"base": hex(base), "size": hex(size), "protect": hex(mbi.Protect),
                                    "alloc_base": hex(mbi.AllocationBase), "type": hex(mbi.Type)})
            if mbi.Protect & 0xF0 and size <= 64 * 1024 * 1024:  # executable
                open(os.path.join(out_dir, "region_%08X.bin" % base), "wb").write(read(h, base, size))
        addr = base + size
    json.dump(info, open(os.path.join(out_dir, "map.json"), "w"), indent=1)
    print(json.dumps(info["modules"], indent=1))
    print(len(info["regions"]), "regions")


if __name__ == "__main__":
    main()
