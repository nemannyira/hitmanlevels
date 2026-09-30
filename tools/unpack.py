"""Unpack hitman2.exe: run it under a minimal debugger, dump the image as soon
as the stub has decompressed .text (before its Steam check), and rebuild a
loadable PE (raw == virtual layout, original import table restored).

Usage: python unpack.py <out.exe>
"""
import ctypes, ctypes.wintypes as wt, ntpath, os, struct, sys

import pefile

GAME_DIR = r"C:\Program Files (x86)\Steam\steamapps\common\Hitman 2 Silent Assassin"
EXE = os.path.join(GAME_DIR, "hitman2.exe")
IMAGE_BASE = 0x400000
IMAGE_SIZE = 0x2EC000
OEP = 0x5A184E
OEP_BYTES = b"\x55\x8B\xEC\x6A\xFF"
IMPORT_DIR = (0x2739F0, 0x8C)
EXPORT_DIR = 0x274760   # 679 decorated C++ exports used by RenderD3D.dll etc.

k32 = ctypes.WinDLL("kernel32", use_last_error=True)

DEBUG_ONLY_THIS_PROCESS = 2
DBG_CONTINUE = 0x00010002
DBG_EXCEPTION_NOT_HANDLED = 0x80010001
EXCEPTION_DEBUG_EVENT = 1
EXIT_PROCESS_DEBUG_EVENT = 5
LOAD_DLL_DEBUG_EVENT = 6
EXCEPTION_SINGLE_STEP = 0x80000004
EXCEPTION_BREAKPOINT = 0x80000003
STATUS_WX86_SINGLE_STEP = 0x4000001E
STATUS_WX86_BREAKPOINT = 0x4000001F
BENIGN = (EXCEPTION_BREAKPOINT, EXCEPTION_SINGLE_STEP, STATUS_WX86_BREAKPOINT, STATUS_WX86_SINGLE_STEP)


class STARTUPINFO(ctypes.Structure):
    _fields_ = [("cb", wt.DWORD), ("lpReserved", wt.LPWSTR), ("lpDesktop", wt.LPWSTR),
                ("lpTitle", wt.LPWSTR), ("dwX", wt.DWORD), ("dwY", wt.DWORD), ("dwXSize", wt.DWORD),
                ("dwYSize", wt.DWORD), ("dwXCountChars", wt.DWORD), ("dwYCountChars", wt.DWORD),
                ("dwFillAttribute", wt.DWORD), ("dwFlags", wt.DWORD), ("wShowWindow", wt.WORD),
                ("cbReserved2", wt.WORD), ("lpReserved2", ctypes.c_void_p), ("hStdInput", wt.HANDLE),
                ("hStdOutput", wt.HANDLE), ("hStdError", wt.HANDLE)]


class PROCESS_INFORMATION(ctypes.Structure):
    _fields_ = [("hProcess", wt.HANDLE), ("hThread", wt.HANDLE), ("dwProcessId", wt.DWORD),
                ("dwThreadId", wt.DWORD)]


class DEBUG_EVENT(ctypes.Structure):
    # 64-bit layout: the union is 8-byte aligned
    _fields_ = [("dwDebugEventCode", wt.DWORD), ("dwProcessId", wt.DWORD), ("dwThreadId", wt.DWORD),
                ("_pad", wt.DWORD), ("u", ctypes.c_ubyte * 160)]


psapi = ctypes.WinDLL("psapi", use_last_error=True)
psapi.EnumProcessModulesEx.argtypes = [wt.HANDLE, ctypes.POINTER(ctypes.c_void_p), wt.DWORD,
                                       ctypes.POINTER(wt.DWORD), wt.DWORD]
psapi.GetModuleFileNameExW.argtypes = [wt.HANDLE, ctypes.c_void_p, wt.LPWSTR, wt.DWORD]
k32.CreateProcessW.argtypes = [wt.LPCWSTR, wt.LPWSTR, ctypes.c_void_p, ctypes.c_void_p, wt.BOOL,
                               wt.DWORD, ctypes.c_void_p, wt.LPCWSTR, ctypes.c_void_p, ctypes.c_void_p]
k32.ReadProcessMemory.argtypes = [wt.HANDLE, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t,
                                  ctypes.POINTER(ctypes.c_size_t)]


def read(h, addr, size):
    out = bytearray(size)
    buf = (ctypes.c_ubyte * 0x1000)()
    for off in range(0, size, 0x1000):
        n = ctypes.c_size_t()
        if k32.ReadProcessMemory(h, addr + off, buf, 0x1000, ctypes.byref(n)):
            out[off:off + 0x1000] = bytes(buf)
    return bytes(out)


def module_bases(h):
    """{lowercase dll name: base} of the 32-bit modules loaded in the debuggee."""
    mods = (ctypes.c_void_p * 512)()
    need = wt.DWORD()
    psapi.EnumProcessModulesEx(h, mods, ctypes.sizeof(mods), ctypes.byref(need), 1)  # LIST_MODULES_32BIT
    bases = {}
    for i in range(need.value // ctypes.sizeof(ctypes.c_void_p)):
        name = ctypes.create_unicode_buffer(520)
        psapi.GetModuleFileNameExW(h, mods[i], name, 520)
        bases[ntpath.basename(name.value).lower()] = mods[i]
    return bases


def export_names(bases, dll):
    """Map runtime address -> export name of `dll`, following forwarders into
    already-loaded modules so e.g. HeapAlloc (-> ntdll) still maps to HeapAlloc."""
    sysdir = os.path.join(os.environ["WINDIR"], "SysWOW64")
    pes = {}

    def load(n):
        if n not in pes:
            pes[n] = pefile.PE(os.path.join(sysdir, n), fast_load=True)
            pes[n].parse_data_directories(directories=[pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_EXPORT"]])
        return pes[n]

    def resolve(n, sym):
        pe = load(n)
        for e in pe.DIRECTORY_ENTRY_EXPORT.symbols:
            if e.name and e.name.decode() == sym:
                if e.forwarder:
                    tgt, tsym = e.forwarder.decode().split(".", 1)
                    tgt = tgt.lower() + ".dll"
                    if tgt.startswith("api-ms-") or tgt not in bases:
                        tgt = "kernelbase.dll"
                    return resolve(tgt, tsym)
                return bases[n] + e.address
        return None

    out = {}
    for e in load(dll).DIRECTORY_ENTRY_EXPORT.symbols:
        if e.name:
            a = resolve(dll, e.name.decode())
            if a:
                out.setdefault(a, e.name.decode())
    return out


def dump_at_oep():
    si = STARTUPINFO(cb=ctypes.sizeof(STARTUPINFO))
    pi = PROCESS_INFORMATION()
    if not k32.CreateProcessW(EXE, None, None, None, False, DEBUG_ONLY_THIS_PROCESS, None, GAME_DIR,
                              ctypes.byref(si), ctypes.byref(pi)):
        raise SystemExit("CreateProcess failed %d" % ctypes.get_last_error())
    ev = DEBUG_EVENT()
    image = exports = None
    armed = False
    while k32.WaitForDebugEvent(ctypes.byref(ev), 30000):
        status = DBG_CONTINUE
        code = ev.dwDebugEventCode
        if code == LOAD_DLL_DEBUG_EVENT and not armed:
            # detect decompression by the known CRT prologue at the OEP
            page = read(pi.hProcess, OEP & ~0xFFF, 0x1000)
            if page[OEP & 0xFFF:(OEP & 0xFFF) + len(OEP_BYTES)] == OEP_BYTES:
                # .text is decompressed and the stub is resolving imports. The
                # Steam DRM check runs after this and exits unless launched by
                # the Steam client, so dump right here (globals still pristine).
                image = read(pi.hProcess, IMAGE_BASE, IMAGE_SIZE)
                exports = export_names(module_bases(pi.hProcess), "kernel32.dll")
                print("code unpacked, dumped")
                k32.TerminateProcess(pi.hProcess, 0)
                armed = True
        elif code == EXCEPTION_DEBUG_EVENT:
            exc_code = struct.unpack_from("<I", bytes(ev.u), 0)[0]
            exc_addr = struct.unpack_from("<Q", bytes(ev.u), 16)[0]
            if exc_code not in BENIGN:
                status = DBG_EXCEPTION_NOT_HANDLED
                print("exception %08X at %08X" % (exc_code, exc_addr))
        elif code == EXIT_PROCESS_DEBUG_EVENT:
            k32.ContinueDebugEvent(ev.dwProcessId, ev.dwThreadId, status)
            break
        k32.ContinueDebugEvent(ev.dwProcessId, ev.dwThreadId, status)
    return image, exports


def rebuild_pe(img, exports):
    img = bytearray(img)
    e_lfanew = struct.unpack_from("<I", img, 0x3C)[0]
    nsec = struct.unpack_from("<H", img, e_lfanew + 6)[0]
    opt = e_lfanew + 24
    struct.pack_into("<I", img, opt + 16, OEP - IMAGE_BASE)          # AddressOfEntryPoint
    dd = opt + 96
    struct.pack_into("<II", img, dd + 8, *IMPORT_DIR)                # import directory
    struct.pack_into("<II", img, dd + 12 * 8, 0, 0)                  # clear IAT dir
    nnames, names = struct.unpack_from("<I4xI", img, EXPORT_DIR + 24)
    last = struct.unpack_from("<I", img, names + 4 * (nnames - 1))[0]
    exp_end = img.index(b"\0", last) + 1
    struct.pack_into("<II", img, dd, EXPORT_DIR, exp_end - EXPORT_DIR)  # export directory
    sec = opt + struct.unpack_from("<H", img, e_lfanew + 20)[0]
    for i in range(nsec):
        s = sec + i * 40
        vsize, va = struct.unpack_from("<II", img, s + 8)
        struct.pack_into("<II", img, s + 16, vsize, va)              # raw = virtual

    # The stub reuses KERNEL32's OriginalFirstThunk array as scratch, so its
    # thunks hold resolved addresses. Rebuild name thunks in a new section.
    new_va = IMAGE_SIZE
    extra = bytearray()
    o = IMPORT_DIR[0]
    while True:
        oft, _, _, name, ft = struct.unpack_from("<5I", img, o)
        if name == 0:
            break
        thunks = []
        i = 0
        while True:
            v = struct.unpack_from("<I", img, oft + i)[0]
            if v == 0:
                break
            if IMAGE_SIZE <= v < 0x80000000:                         # resolved address
                sym = exports.get(v)
                if sym is None:
                    raise SystemExit("unresolved import %08X" % v)
                hint_rva = new_va + len(extra)
                extra += b"\0\0" + sym.encode() + b"\0"
                extra += b"\0" * (len(extra) & 1)
                v = hint_rva
            thunks.append(v)
            i += 4
        if any(new_va <= t < new_va + len(extra) for t in thunks):
            while len(extra) % 4:
                extra += b"\0"
            oft = new_va + len(extra)
            extra += struct.pack("<%dI" % (len(thunks) + 1), *thunks, 0)
            struct.pack_into("<I", img, o, oft)
        # FirstThunk := name thunks so loaders see import names
        struct.pack_into("<%dI" % (len(thunks) + 1), img, ft, *thunks, 0)
        o += 20

    if extra:
        raw = (len(extra) + 0xFFF) & ~0xFFF
        extra += b"\0" * (raw - len(extra))
        s = sec + nsec * 40
        assert s + 40 <= struct.unpack_from("<I", img, opt + 60)[0], "no room for section header"
        struct.pack_into("<8sIIIIIIHHI", img, s, b".idata2", len(extra), new_va, raw, new_va,
                         0, 0, 0, 0, 0xC0000040)
        struct.pack_into("<H", img, e_lfanew + 6, nsec + 1)
        struct.pack_into("<I", img, opt + 56, new_va + raw)           # SizeOfImage
        img += extra
    return bytes(img)


if __name__ == "__main__":
    image, exports = dump_at_oep()
    if image is None:
        raise SystemExit("unpacked code not detected")
    open(sys.argv[1], "wb").write(rebuild_pe(image, exports))
    print("written", sys.argv[1])
