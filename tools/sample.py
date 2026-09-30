"""Poor man's sampling profiler for the running 32-bit hitman2.exe.

Suspends each thread, reads EIP (plus a few return addresses from the stack),
resumes it. Prints the hottest addresses per thread, annotated with module.

Usage: python sample.py [seconds] [interval_ms] [stack_depth]
"""
import collections, ctypes, ctypes.wintypes as wt, ntpath, struct, subprocess, sys, time

k32 = ctypes.WinDLL("kernel32", use_last_error=True)
psapi = ctypes.WinDLL("psapi", use_last_error=True)
ntdll = ctypes.WinDLL("ntdll")

THREAD_ALL = 0x1FFFFF
WOW64_CONTEXT_CONTROL = 0x00010001
WOW64_CONTEXT_INTEGER = 0x00010002


class WOW64_CONTEXT(ctypes.Structure):
    _fields_ = [("ContextFlags", wt.DWORD), ("Dr", wt.DWORD * 6), ("FloatSave", ctypes.c_ubyte * 112),
                ("SegGs", wt.DWORD), ("SegFs", wt.DWORD), ("SegEs", wt.DWORD), ("SegDs", wt.DWORD),
                ("Edi", wt.DWORD), ("Esi", wt.DWORD), ("Ebx", wt.DWORD), ("Edx", wt.DWORD),
                ("Ecx", wt.DWORD), ("Eax", wt.DWORD), ("Ebp", wt.DWORD), ("Eip", wt.DWORD),
                ("SegCs", wt.DWORD), ("EFlags", wt.DWORD), ("Esp", wt.DWORD), ("SegSs", wt.DWORD),
                ("ExtendedRegisters", ctypes.c_ubyte * 512)]


class THREADENTRY32(ctypes.Structure):
    _fields_ = [("dwSize", wt.DWORD), ("cntUsage", wt.DWORD), ("th32ThreadID", wt.DWORD),
                ("th32OwnerProcessID", wt.DWORD), ("tpBasePri", ctypes.c_long),
                ("tpDeltaPri", ctypes.c_long), ("dwFlags", wt.DWORD)]


k32.OpenThread.restype = wt.HANDLE
k32.OpenProcess.restype = wt.HANDLE
k32.CreateToolhelp32Snapshot.restype = wt.HANDLE
k32.ReadProcessMemory.argtypes = [wt.HANDLE, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t,
                                  ctypes.POINTER(ctypes.c_size_t)]
k32.Wow64GetThreadContext.argtypes = [wt.HANDLE, ctypes.c_void_p]
k32.SuspendThread.argtypes = [wt.HANDLE]
k32.ResumeThread.argtypes = [wt.HANDLE]
psapi.EnumProcessModulesEx.argtypes = [wt.HANDLE, ctypes.POINTER(ctypes.c_void_p), wt.DWORD,
                                       ctypes.POINTER(wt.DWORD), wt.DWORD]
psapi.GetModuleFileNameExW.argtypes = [wt.HANDLE, ctypes.c_void_p, wt.LPWSTR, wt.DWORD]


class MODINFO(ctypes.Structure):
    _fields_ = [("base", ctypes.c_void_p), ("size", wt.DWORD), ("entry", ctypes.c_void_p)]


psapi.GetModuleInformation.argtypes = [wt.HANDLE, ctypes.c_void_p, ctypes.POINTER(MODINFO), wt.DWORD]


def pid_of(name="hitman2"):
    return int(subprocess.check_output(["powershell", "-c", "(Get-Process %s).Id" % name]).split()[0])


def threads(pid):
    snap = k32.CreateToolhelp32Snapshot(4, 0)
    te = THREADENTRY32(dwSize=ctypes.sizeof(THREADENTRY32))
    out = []
    ok = k32.Thread32First(snap, ctypes.byref(te))
    while ok:
        if te.th32OwnerProcessID == pid:
            out.append(te.th32ThreadID)
        ok = k32.Thread32Next(snap, ctypes.byref(te))
    k32.CloseHandle(snap)
    return out


def modules(h):
    mods = (ctypes.c_void_p * 512)()
    need = wt.DWORD()
    psapi.EnumProcessModulesEx(h, mods, ctypes.sizeof(mods), ctypes.byref(need), 1)
    out = []
    for i in range(need.value // ctypes.sizeof(ctypes.c_void_p)):
        name = ctypes.create_unicode_buffer(520)
        psapi.GetModuleFileNameExW(h, mods[i], name, 520)
        mi = MODINFO()
        psapi.GetModuleInformation(h, mods[i], ctypes.byref(mi), ctypes.sizeof(mi))
        out.append((mi.base, mi.base + mi.size, ntpath.basename(name.value), name.value))
    return out


_exports = {}


def nearest_export(path, rva):
    if path not in _exports:
        try:
            import pefile
            pe = pefile.PE(path, fast_load=True)
            pe.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_EXPORT"]])
            _exports[path] = sorted((e.address, e.name.decode()) for e in
                                    getattr(pe, "DIRECTORY_ENTRY_EXPORT", None).symbols if e.name)
        except Exception:
            _exports[path] = []
    best = None
    for a, n in _exports[path]:
        if a <= rva:
            best = (a, n)
        else:
            break
    return "%s+%X" % (best[1], rva - best[0]) if best else "+%X" % rva


def sym(mods, a):
    for lo, hi, n, path in mods:
        if lo <= a < hi:
            if n.lower() == "hitman2.exe":
                return "%08X" % a
            return "%s!%s" % (n, nearest_export(path, a - lo))
    return "%08X" % a


def read_u32s(h, addr, n):
    buf = (ctypes.c_uint32 * n)()
    got = ctypes.c_size_t()
    if not k32.ReadProcessMemory(h, addr, buf, 4 * n, ctypes.byref(got)):
        return []
    return list(buf)


def main():
    secs = float(sys.argv[1]) if len(sys.argv) > 1 else 5
    interval = float(sys.argv[2]) / 1000 if len(sys.argv) > 2 else 0.01
    depth = int(sys.argv[3]) if len(sys.argv) > 3 else 0
    pid = pid_of()
    hp = k32.OpenProcess(0x1F0FFF, False, pid)
    mods = modules(hp)
    hits = collections.defaultdict(collections.Counter)
    callers = collections.defaultdict(collections.Counter)
    handles = {}
    end = time.time() + secs
    n = 0
    tids, refreshed = [], 0
    while time.time() < end:
        if time.time() - refreshed > 1:
            tids, refreshed = threads(pid), time.time()
        for tid in tids:
            h = handles.get(tid) or handles.setdefault(tid, k32.OpenThread(THREAD_ALL, False, tid))
            if k32.SuspendThread(h) == 0xFFFFFFFF:
                continue
            ctx = WOW64_CONTEXT(ContextFlags=WOW64_CONTEXT_CONTROL | WOW64_CONTEXT_INTEGER)
            if k32.Wow64GetThreadContext(h, ctypes.byref(ctx)):
                hits[tid][ctx.Eip] += 1
                if depth:
                    # heuristic: return addresses into hitman2.exe on the stack
                    rets = [v for v in read_u32s(hp, ctx.Esp, 4096) if 0x401000 <= v < 0x6DC000][:depth]
                    callers[tid][tuple(rets)] += 1
            k32.ResumeThread(h)
        n += 1
        time.sleep(interval)
    print("rounds:", n)
    for tid, c in sorted(hits.items(), key=lambda kv: -sum(kv[1].values())):
        busy = sum(v for a, v in c.items() if not sym(mods, a).lower().startswith(("ntdll", "win32u", "wow64")))
        print("\n== thread %d  samples=%d  outside-syscall=%d" % (tid, sum(c.values()), busy))
        for a, v in c.most_common(12):
            print("   %5d  %s" % (v, sym(mods, a)))
        for st, v in callers[tid].most_common(8):
            print("   stk %5d  %s" % (v, " <- ".join("%08X" % x for x in st)))


if __name__ == "__main__":
    main()
