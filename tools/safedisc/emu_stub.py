"""Unicorn harness for the SafeDisc entry stub of VT3.exe (x86-32).

Usage: python emu_stub.py [VT3.exe path] [max_instructions]
Env:   CHILD_OK=1 (default) makes CreateProcessA report success (launcher not emulated)
       VT3_DUMP_DIR=... where overlay payloads the stub writes are saved
       TRACE=1 records the last instructions inside the stub region
Stops at the original entry point 0x654155 (or on ExitProcess). Reports how many
writes hit .text, which shows whether the stub itself decrypted anything.

Maps the PE image, gives it a minimal TEB/PEB, and stubs Win32 imports as
hook slots that log every call. Runs from the entry point until the original
entry point (OEP) is reached or emulation stops.
"""
import sys, struct, time
import pefile
from unicorn import *
from unicorn.x86_const import *

PATH = sys.argv[1] if len(sys.argv) > 1 else "/Users/heath/Virtua Tennis 3/dump/pc/VT3.exe"
MAX_INSN = int(sys.argv[2], 0) if len(sys.argv) > 2 else 5_000_000
pe = pefile.PE(PATH)
data = open(PATH, "rb").read()
IB = pe.OPTIONAL_HEADER.ImageBase
EP = IB + pe.OPTIONAL_HEADER.AddressOfEntryPoint
OEP_CANDIDATE = 0x654155
IMG_SIZE = pe.OPTIONAL_HEADER.SizeOfImage

STACK_BASE, STACK_SIZE = 0x00100000, 0x00200000
TEB = 0x7ffd0000
HOOK_BASE, HOOK_SIZE = 0x70000000, 0x00100000
HEAP_BASE, HEAP_SIZE = 0x20000000, 0x04000000

log_lines = []
def log(*a):
    s = " ".join(str(x) for x in a)
    log_lines.append(s)
    print(s, flush=True)

mu = Uc(UC_ARCH_X86, UC_MODE_32)
mu.mem_map(IB, IMG_SIZE, UC_PROT_ALL)
mu.mem_write(IB, data[:pe.OPTIONAL_HEADER.SizeOfHeaders])
for s in pe.sections:
    raw = data[s.PointerToRawData:s.PointerToRawData + min(s.SizeOfRawData, s.Misc_VirtualSize)]
    mu.mem_write(IB + s.VirtualAddress, raw)
mu.mem_map(STACK_BASE, STACK_SIZE, UC_PROT_READ | UC_PROT_WRITE)
mu.mem_map(TEB, 0x10000, UC_PROT_READ | UC_PROT_WRITE)
mu.mem_map(HOOK_BASE, HOOK_SIZE, UC_PROT_ALL)
mu.mem_map(HEAP_BASE, HEAP_SIZE, UC_PROT_READ | UC_PROT_WRITE)
# Hook slots: each is a single 'ret' byte; the handler runs in a code hook.
mu.mem_write(HOOK_BASE, b"\xc3" * HOOK_SIZE)

TEB_BASE = TEB + 0x0000   # TEB at TEB; PEB at TEB+0x1000
PEB = TEB + 0x1000
mu.mem_write(PEB, b"\x00" * 0x100)
struct.pack_into  # noqa
def w32(addr, val):
    mu.mem_write(addr, struct.pack("<I", val & 0xffffffff))
def r32(addr):
    return struct.unpack("<I", bytes(mu.mem_read(addr, 4)))[0]
# TEB: [0]=SEH chain, [4]=stack base, [8]=stack limit, [0x18]=self, [0x30]=PEB
w32(TEB + 0x00, 0xffffffff)
w32(TEB + 0x04, STACK_BASE + STACK_SIZE)
w32(TEB + 0x08, STACK_BASE)
w32(TEB + 0x18, TEB)
w32(TEB + 0x30, PEB)
w32(PEB + 0x08, IB)
GDT = 0x7ffe0000
mu.mem_map(GDT, 0x1000, UC_PROT_READ | UC_PROT_WRITE)
def gdt_entry(base, limit, access, flags):
    lo = (limit & 0xffff) | ((base & 0xffff) << 16)
    hi = ((base >> 16) & 0xff) | (access << 8) | (((limit >> 16) & 0xf) << 16) | (flags << 20) | (((base >> 24) & 0xff) << 24)
    return struct.pack("<II", lo, hi)
mu.mem_write(GDT + 0 * 8, b"\x00" * 8)
mu.mem_write(GDT + 1 * 8, gdt_entry(0, 0xfffff, 0x9b, 0xc))   # ring0 code32
mu.mem_write(GDT + 2 * 8, gdt_entry(0, 0xfffff, 0x93, 0xc))   # ring0 data32
mu.mem_write(GDT + 3 * 8, gdt_entry(0, 0xfffff, 0xfb, 0xc))   # ring3 code32
mu.mem_write(GDT + 4 * 8, gdt_entry(0, 0xfffff, 0xf3, 0xc))   # ring3 data32
mu.mem_write(GDT + 7 * 8, gdt_entry(TEB, 0xfff, 0xf3, 0x4))   # fs: ring3 data32 byte-granular
mu.reg_write(UC_X86_REG_GDTR, (0, GDT, 8 * 8 - 1, 0))
mu.reg_write(UC_X86_REG_CS, 0x08)
mu.reg_write(UC_X86_REG_DS, 0x10)
mu.reg_write(UC_X86_REG_ES, 0x10)
mu.reg_write(UC_X86_REG_SS, 0x10)
mu.reg_write(UC_X86_REG_FS, (7 << 3) | 3)

ESP_TOP = STACK_BASE + STACK_SIZE - 0x1000
RET_EXIT = HOOK_BASE + 0xFFF0   # return address for the initial call; handled as process exit
mu.reg_write(UC_X86_REG_ESP, ESP_TOP - 4)
w32(ESP_TOP - 4, RET_EXIT)
mu.reg_write(UC_X86_REG_EBP, 0)

# ---- hook slot allocation ----
slot_next = [HOOK_BASE + 0x100]
slot_name = {}      # addr -> "dll!name"
name_slot = {}      # "dll!name" -> addr
def slot_for(name):
    if name in name_slot:
        return name_slot[name]
    a = slot_next[0]
    slot_next[0] += 0x10
    name_slot[name] = a
    slot_name[a] = name
    return a

# Import table: fill the IAT with slots so the stub can call them directly
imp_slots = {}
for e in pe.DIRECTORY_ENTRY_IMPORT:
    dll = e.dll.decode().lower()
    for imp in e.imports:
        nm = imp.name.decode() if imp.name else "ord%d" % imp.ordinal
        a = slot_for("%s!%s" % (dll, nm))
        w32(imp.address, a)
        imp_slots[imp.address] = "%s!%s" % (dll, nm)

# ---- API implementations: name -> (fn, stdcall_arg_bytes) ----
state = {"last_error": 0, "heap": HEAP_BASE + 0x1000, "handles": {}, "next_handle": 0x100,
         "exit": None, "ticks": 0, "ncalls": 0}
DLL_HANDLES = {}
def dll_handle(name):
    name = name.lower()
    if name not in DLL_HANDLES:
        DLL_HANDLES[name] = 0x10000000 + 0x10000 * (len(DLL_HANDLES) + 1)
    return DLL_HANDLES[name]
HANDLE_DLL = {}

def cstr(addr, maxlen=512):
    out = bytearray()
    while len(out) < maxlen:
        b = bytes(mu.mem_read(addr + len(out), 1))
        if b == b"\x00":
            break
        out += b
    return out.decode("latin-1")

def heap_alloc(size, align=16):
    size = max(size, 1)
    p = (state["heap"] + align - 1) & ~(align - 1)
    state["heap"] = p + size
    if state["heap"] > HEAP_BASE + HEAP_SIZE:
        raise RuntimeError("heap exhausted")
    mu.mem_write(p, b"\x00" * size)
    return p


# stdcall argument byte counts for APIs the stub resolves by name (cdecl = -1: caller pops)
STDCALL_BYTES = {
 "GetLastError": 0, "FormatMessageA": 28, "LocalFree": 4, "GetModuleHandleA": 4,
 "GetModuleFileNameA": 12, "VirtualProtect": 16, "CreateFileA": 28, "ReadFile": 20,
 "WriteFile": 20, "CloseHandle": 4, "DeleteFileA": 4, "SetFilePointer": 16,
 "GetFileSize": 8, "GetWindowsDirectoryA": 8, "GetTempPathA": 8, "GetTickCount": 0,
 "CreateDirectoryA": 8, "RemoveDirectoryA": 4, "CreateMutexA": 12, "GetDriveTypeA": 4,
 "GetFileAttributesA": 4, "GetCurrentProcessId": 0, "CreateProcessA": 40, "ReleaseMutex": 4,
 "GetCurrentThread": 0, "GetCurrentProcess": 0, "DuplicateHandle": 28, "CreateThread": 24,
 "WaitForSingleObject": 8, "LoadLibraryA": 4, "FreeLibrary": 4, "WriteProcessMemory": 20,
 "IsDBCSLeadByte": 4, "wsprintfA": -1, "GetProcAddress": 8, "GlobalAlloc": 8,
 "GlobalFree": 4, "ExitProcess": 4, "VirtualAlloc": 16, "VirtualFree": 12,
 "GetVersion": 0, "SetLastError": 4, "Sleep": 4, "DeviceIoControl": 32,
 "lstrlenA": 4, "RaiseException": 16, "IsDebuggerPresent": 0, "RegCreateKeyA": 20,
 "timeEndPeriod": 4, "GetCurrentProcess": 0, "MessageBoxA": 16, "CreateCompatibleDC": 4,
 "GetFileVersionInfoA": 16, "RegOpenKeyExA": 20, "RegQueryValueExA": 24, "RegCloseKey": 4,
 "OpenProcess": 12, "CreateEventA": 16, "SetEvent": 4, "ResetEvent": 4, "OpenMutexA": 12,
 "CreateFileMappingA": 24, "MapViewOfFile": 20, "UnmapViewOfFile": 4, "GetSystemInfo": 4,
 "GetCurrentThreadId": 0, "GetLogicalDriveStringsA": 8, "GetVolumeInformationA": 32,
 "GetSystemDirectoryA": 8, "GetEnvironmentVariableA": 12, "FindFirstFileA": 8,
 "FindClose": 4, "FindNextFileA": 8, "OutputDebugStringA": 4, "lstrcpyA": 8,
 "lstrcmpiA": 8, "lstrcmpA": 8, "lstrcatA": 8, "ExpandEnvironmentStringsA": 12,
 "MultiByteToWideChar": 24, "WideCharToMultiByte": 32, "GetACP": 0, "SetFilePointerEx": 20,
 "GetFileSizeEx": 8, "ReadProcessMemory": 20, "FlushFileBuffers": 4, "GetStartupInfoA": 4,
 "Sleep": 4, "QueryPerformanceFrequency": 4, "QueryPerformanceCounter": 4,
 "GetTimeZoneInformation": 4, "GetLocalTime": 4, "SystemTimeToFileTime": 8,
 "CreateToolhelp32Snapshot": 8, "Process32First": 8, "Process32Next": 8,
 "LoadResource": 8, "FindResourceA": 12, "SizeofResource": 8, "LockResource": 4,
 "VirtualQuery": 12, "IsBadReadPtr": 8, "IsBadWritePtr": 8, "GetProcessHeap": 0,
 "HeapAlloc": 12, "HeapFree": 12, "HeapCreate": 12, "HeapDestroy": 4, "GetModuleHandleW": 4,
 "LoadLibraryW": 4, "GetProcAddressW": 8, "GetLocaleInfoA": 16, "MulDiv": 12,
}


# ---- minimal virtual filesystem (reads go to host files; writes go to an overlay) ----
import os
GAME_HOST = "/Users/heath/Virtua Tennis 3/dump/pc"
VFS_DIRS = {"C:", "C:\\WINDOWS", "C:\\WINDOWS\\TEMP", "C:\\WINDOWS\\SYSTEM32", "C:\\GAMES",
            "C:\\GAMES\\VT3", "C:\\PROGRAM FILES", "C:\\TEMP"}
OVERLAY = {}   # normalised path -> bytearray (writes and created files)
VFS_FILES = {}
def norm(p):
    return p.replace("/", "\\").upper().rstrip("\\")
def vfs_lookup(path):
    n = norm(path)
    if n in OVERLAY:
        return ("mem", OVERLAY[n])
    if n in VFS_DIRS or n in [norm(d) for d in VFS_DIRS]:
        return ("dir", None)
    if n.startswith("C:\\GAMES\\VT3\\"):
        rel = path.replace("/", "\\")[len("C:\\Games\\VT3\\"):]
        hp = os.path.join(GAME_HOST, rel.replace("\\", "/"))
        if os.path.isdir(hp):
            return ("dir", None)
        if os.path.isfile(hp):
            return ("file", hp)
    return None
def vfs_mkdir(path):
    VFS_DIRS.add(norm(path))
    return True
HANDLE_FILES = {}   # handle -> dict(kind, path, obj, pos, size, writable)
def new_handle(d):
    h = state["next_handle"]
    state["next_handle"] += 4
    HANDLE_FILES[h] = d
    return h

API = {}
def api(name, nbytes):
    def deco(fn):
        API[name] = (fn, nbytes)
        return fn
    return deco

ARG = lambda i: r32(mu.reg_read(UC_X86_REG_ESP) + 4 + 4 * i)

@api("kernel32.dll!GetModuleHandleA", 4)
def _(a):
    n = ARG(0)
    if n == 0:
        return IB
    s = cstr(n).lower()
    if s.endswith(".exe") or s == "vt3.exe":
        return IB
    h = dll_handle(s)
    HANDLE_DLL[h] = s
    return h

@api("kernel32.dll!GetProcAddress", 8)
def _(a):
    h, n = ARG(0), ARG(1)
    if n < 0x10000:
        nm = "ord%d" % n
    else:
        nm = cstr(n)
    dll = "kernel32.dll" if h == IB else HANDLE_DLL.get(h, "?")
    key = "%s!%s" % (dll, nm)
    slot = slot_for(key)
    log("  GetProcAddress(%s) -> %08x" % (key, slot))
    return slot

@api("kernel32.dll!LoadLibraryA", 4)
def _(a):
    s = cstr(ARG(0)).lower()
    h = dll_handle(s)
    HANDLE_DLL[h] = s
    log("  LoadLibraryA(%s) -> %08x" % (s, h))
    return h

@api("kernel32.dll!GlobalAlloc", 8)
def _(a):
    flags, size = ARG(0), ARG(1)
    return heap_alloc(size)

@api("kernel32.dll!GlobalFree", 4)
def _(a):
    return 0

@api("kernel32.dll!ExitProcess", 4)
def _(a):
    state["exit"] = ARG(0)
    return 0

@api("kernel32.dll!GetLastError", 0)
def _(a):
    return state["last_error"]

@api("kernel32.dll!SetLastError", 4)
def _(a):
    state["last_error"] = ARG(0)
    return 0

@api("kernel32.dll!GetVersion", 0)
def _(a):
    return 0x0A280105  # Windows XP (5.1, build 2600)

@api("kernel32.dll!VirtualAlloc", 16)
def _(a):
    addr, size = ARG(0), ARG(1)
    size = (size + 0xfff) & ~0xfff
    if addr:
        # Only map fresh memory if it is not already mapped
        try:
            mu.mem_read(addr, 1)
            return addr
        except UcError:
            pass
    p = heap_alloc(size, 0x1000)
    return p

@api("kernel32.dll!VirtualFree", 12)
def _(a):
    return 1

@api("kernel32.dll!VirtualProtect", 16)
def _(a):
    addr, size, newp, oldp = ARG(0), ARG(1), ARG(2), ARG(3)
    if oldp:
        w32(oldp, 0x04)
    return 1

@api("kernel32.dll!GetCurrentProcess", 0)
def _(a):
    return 0xffffffff

@api("kernel32.dll!GetCurrentProcessId", 0)
def _(a):
    return 0x1234

@api("kernel32.dll!GetCurrentThreadId", 0)
def _(a):
    return 0x1235

@api("kernel32.dll!GetTickCount", 0)
def _(a):
    state["ticks"] += 15
    return state["ticks"] & 0xffffffff

@api("kernel32.dll!Sleep", 4)
def _(a):
    return 0

@api("kernel32.dll!CloseHandle", 4)
def _(a):
    return 1

@api("kernel32.dll!CreateFileA", 28)
def _(a):
    name = cstr(ARG(0))
    log("  CreateFileA(%r)" % name)
    state["last_error"] = 2   # ERROR_FILE_NOT_FOUND
    return 0xffffffff          # INVALID_HANDLE_VALUE

@api("kernel32.dll!DeviceIoControl", 32)
def _(a):
    log("  DeviceIoControl(h=%08x code=%08x in=%d out=%d)" % (ARG(0), ARG(1), ARG(3), ARG(5)))
    state["last_error"] = 1
    return 0

@api("kernel32.dll!GetDriveTypeA", 4)
def _(a):
    root = cstr(ARG(0))
    log("  GetDriveTypeA(%r)" % root)
    return 5 if root[:1].upper() == "D" else 1  # DRIVE_CDROM for D:, else NO_ROOT

@api("kernel32.dll!lstrlenA", 4)
def _(a):
    return len(cstr(ARG(0), 1 << 16))

@api("kernel32.dll!GetModuleFileNameA", 12)
def _(a):
    h, buf, n = ARG(0), ARG(1), ARG(2)
    s = (r"C:\Games\VT3\VT3.exe").encode() + b"\x00"
    mu.mem_write(buf, s[:n])
    return len(s) - 1

@api("kernel32.dll!QueryPerformanceCounter", 4)
def _(a):
    w32(ARG(0), state["ticks"]); w32(ARG(0) + 4, 0)
    return 1

@api("kernel32.dll!RaiseException", 16)
def _(a):
    log("  RaiseException code=%08x" % ARG(0))
    return 0

@api("kernel32.dll!IsDebuggerPresent", 0)
def _(a):
    return 0

@api("advapi32.dll!RegCreateKeyA", 20)
def _(a):
    return 2

@api("winmm.dll!timeEndPeriod", 4)
def _(a):
    return 0

STRING_ARGS = {"CreateFileA": [0], "GetFileAttributesA": [0], "CreateDirectoryA": [0],
  "DeleteFileA": [0], "RemoveDirectoryA": [0], "GetDriveTypeA": [0], "CreateMutexA": [2],
  "RegCreateKeyA": [1], "LoadLibraryA": [0], "GetModuleHandleA": [0], "GetTempPathA": [],
  "lstrlenA": [0], "MessageBoxA": [1, 2]}
API_BY_NAME = {}
for _k, _v in list(API.items()):
    API_BY_NAME[_k.split("!", 1)[1]] = _v

def unknown(key):
    def fn(a):
        return 0
    return fn


IMPL = {}
def impl(name):
    def deco(fn):
        IMPL[name] = fn
        return fn
    return deco

@impl("CreateMutexA")
def _i():
    return heap_alloc(16)

@impl("ReleaseMutex")
def _i():
    return 1


@impl("GetCurrentThread")
def _i():
    return 0xfffffffe

@impl("GetCurrentProcess")
def _i():
    return 0xffffffff

@impl("WaitForSingleObject")
def _i():
    return 0

@impl("CreateThread")
def _i():
    return 0

@impl("FormatMessageA")
def _i():
    return 0

@impl("LocalFree")
def _i():
    return 0

def _write_cstr_arg(buf_addr, buf_len, s):
    b = s.encode() + b"\x00"
    if buf_len < len(b):
        return len(b)
    mu.mem_write(buf_addr, b)
    return len(b) - 1

@impl("GetWindowsDirectoryA")
def _i():
    return _write_cstr_arg(ARG(0), ARG(1), "C:\\WINDOWS")

@impl("GetTempPathA")
def _i():
    return _write_cstr_arg(ARG(1), ARG(0), "C:\\WINDOWS\\Temp\\")

@impl("IsDBCSLeadByte")
def _i():
    return 0

@impl("DuplicateHandle")
def _i():
    return 1

@impl("wsprintfA")
def _i():
    return 0


@impl("RegCreateKeyA")
def _i():
    return 2

@impl("MessageBoxA")
def _i():
    return 1

@impl("CreateProcessA")
def _i():
    # CHILD_OK=1 (default): report the launcher as started and fill PROCESS_INFORMATION.
    # The launcher itself is not emulated.
    if os.environ.get("CHILD_OK", "1") == "1":
        pi = ARG(9)
        if pi:
            w32(pi + 0, 0x3000); w32(pi + 4, 0x3004); w32(pi + 8, 0x4444); w32(pi + 12, 0x4445)
        return 1
    return 0

@impl("OpenProcess")
def _i():
    return 0


def ret_addr_of(uc):
    return r32(uc.reg_read(UC_X86_REG_ESP))


@impl("GetFileAttributesA")
def _i():
    p = cstr(ARG(0))
    r = vfs_lookup(p)
    if r is None:
        state["last_error"] = 2
        return 0xffffffff
    return 0x10 if r[0] == "dir" else 0x80

@impl("CreateDirectoryA")
def _i():
    p = cstr(ARG(0))
    if vfs_lookup(p) is not None:
        state["last_error"] = 183
        return 0
    vfs_mkdir(p)
    return 1

@impl("RemoveDirectoryA")
def _i():
    VFS_DIRS.discard(norm(cstr(ARG(0))))
    return 1

@impl("DeleteFileA")
def _i():
    OVERLAY.pop(norm(cstr(ARG(0))), None)
    return 1

@impl("CreateFileA")
def _i():
    p = cstr(ARG(0))
    access, disp = ARG(1), ARG(4)
    r = vfs_lookup(p)
    writable = bool(access & 0x40000000)
    create = disp in (1, 2, 4, 5)   # CREATE_NEW, CREATE_ALWAYS, OPEN_ALWAYS, OPEN_ALWAYS-ish
    if r is None or (r[0] == "dir"):
        if writable and create and r is None and norm(os.path.dirname(p.replace("/", "\\"))) in [norm(d) for d in VFS_DIRS] or (writable and create and r is None and norm(p).rsplit("\\", 1)[0] in VFS_DIRS):
            OVERLAY[norm(p)] = bytearray()
            return new_handle({"path": norm(p), "kind": "mem", "pos": 0})
        state["last_error"] = 2
        return 0xffffffff
    if r[0] == "mem":
        return new_handle({"path": norm(p), "kind": "mem", "pos": 0})
    if writable and disp == 2:   # CREATE_ALWAYS -> overlay
        OVERLAY[norm(p)] = bytearray()
        return new_handle({"path": norm(p), "kind": "mem", "pos": 0})
    f = open(r[1], "rb")
    f.seek(0, 2); size = f.tell(); f.seek(0)
    return new_handle({"path": norm(p), "kind": "host", "obj": f, "pos": 0, "size": size})

def _fh(h):
    return HANDLE_FILES.get(h)

@impl("ReadFile")
def _i():
    h, buf, n, pr = ARG(0), ARG(1), ARG(2), ARG(3)
    d = _fh(h)
    if d is None:
        state["last_error"] = 6
        return 0
    if d["kind"] == "host":
        d["obj"].seek(d["pos"])
        chunk = d["obj"].read(n)
        if d.get("logged", 0) < 3:
            log("      ReadFile(%s) pos=%d n=%d got=%d" % (d["path"], d["pos"], n, len(chunk)))
            d["logged"] = d.get("logged", 0) + 1
    else:
        mem = OVERLAY.get(d["path"], bytearray())
        chunk = bytes(mem[d["pos"]:d["pos"] + n])
    mu.mem_write(buf, chunk)
    d["pos"] += len(chunk)
    if pr:
        w32(pr, len(chunk))
    return 1

@impl("WriteFile")
def _i():
    h, buf, n, pw = ARG(0), ARG(1), ARG(2), ARG(3)
    d = _fh(h)
    if d is None or d["kind"] != "mem":
        return 0
    chunk = bytes(mu.mem_read(buf, n))
    if d.get("wlogged", 0) < 2:
        log("      WriteFile(%s) pos=%d n=%d" % (d["path"], d["pos"], n))
        d["wlogged"] = d.get("wlogged", 0) + 1
    mem = OVERLAY.setdefault(d["path"], bytearray())
    mem[d["pos"]:d["pos"] + n] = chunk
    d["pos"] += n
    if pw:
        w32(pw, n)
    return 1

@impl("GetFileSize")
def _i():
    d = _fh(ARG(0))
    if d is None:
        return 0xffffffff
    if d["kind"] == "host":
        return d["size"]
    return len(OVERLAY.get(d["path"], b""))

@impl("SetFilePointer")
def _i():
    h, lo, hip, method = ARG(0), ARG(1), ARG(2), ARG(3)
    d = _fh(h)
    if d is None:
        return 0xffffffff
    off = lo if lo < 0x80000000 else lo - 0x100000000
    if hip:
        hi = r32(hip)
        if hi:
            off = ((hi << 32) | lo)
    size = d["size"] if d["kind"] == "host" else len(OVERLAY.get(d["path"], b""))
    base = {0: 0, 1: d["pos"], 2: size}.get(method, 0)
    d["pos"] = max(0, base + off)
    return d["pos"] & 0xffffffff

@impl("CloseHandle")
def _i():
    d = HANDLE_FILES.pop(ARG(0), None)
    if d and d.get("kind") == "host":
        d["obj"].close()
    return 1

# ===================== extension: module loading, devices, disc reads, IOCTL logging =====================
BS = chr(92)
DEVPFX = BS + BS + "." + BS
ISO_PATH = os.environ.get("VT3_ISO", "/Users/heath/Virtua Tennis 3/Virtua Tennis 3 (Europe) (En,Fr,De,Es,It).iso")
ISO_SIZE = os.path.getsize(ISO_PATH) if os.path.exists(ISO_PATH) else 0
_ISO_FH = [None]
DISC_READS = []      # (how, offset, size, got)
IOCTL_LOG = []       # (handle kind, code, in_size, out_size, in_prefix hex)
MODULES = {}         # lower basename -> record
MODULE_BY_BASE = {}
_next_module_base = [0x30000000]

def iso_read(off, n):
    if _ISO_FH[0] is None:
        _ISO_FH[0] = open(ISO_PATH, "rb")   # read only
    _ISO_FH[0].seek(off)
    return _ISO_FH[0].read(n)

def _align(x, a=0x1000):
    return (x + a - 1) & ~(a - 1)

def load_pe_module(path, data):
    pe2 = pefile.PE(data=data)
    pref = pe2.OPTIONAL_HEADER.ImageBase
    size = _align(pe2.OPTIONAL_HEADER.SizeOfImage)
    base = pref
    try:
        mu.mem_map(base, size, UC_PROT_ALL)
    except UcError:
        base = _next_module_base[0]
        _next_module_base[0] += 0x01000000
        mu.mem_map(base, size, UC_PROT_ALL)
    mu.mem_write(base, data[:pe2.OPTIONAL_HEADER.SizeOfHeaders])
    for s in pe2.sections:
        vsz = _align(max(s.Misc_VirtualSize, s.SizeOfRawData))
        raw = data[s.PointerToRawData:s.PointerToRawData + min(s.SizeOfRawData, vsz)]
        if raw:
            mu.mem_write(base + s.VirtualAddress, raw)
    pe2.parse_data_directories()
    delta = (base - pref) & 0xffffffff
    if delta and hasattr(pe2, "DIRECTORY_ENTRY_BASERELOC"):
        for blk in pe2.DIRECTORY_ENTRY_BASERELOC:
            for ent in blk.entries:
                if ent.type == 3:   # IMAGE_REL_BASED_HIGHLOW
                    a = base + ent.rva
                    w32(a, (r32(a) + delta) & 0xffffffff)
    rec = {"name": os.path.basename(path).lower(), "base": base, "pref": pref, "exports": {}, "ords": {}, "path": path}
    if hasattr(pe2, "DIRECTORY_ENTRY_EXPORT"):
        for sym in pe2.DIRECTORY_ENTRY_EXPORT.symbols:
            if sym.address:
                if sym.name:
                    rec["exports"][sym.name.decode()] = base + sym.address
                rec["ords"][sym.ordinal] = base + sym.address
    MODULES[rec["name"]] = rec
    MODULE_BY_BASE[base] = rec
    if hasattr(pe2, "DIRECTORY_ENTRY_IMPORT"):
        for e in pe2.DIRECTORY_ENTRY_IMPORT:
            dll = e.dll.decode().lower()
            for imp in e.imports:
                nm = imp.name.decode() if imp.name else "ord%d" % imp.ordinal
                target = None
                if dll in MODULES and nm in MODULES[dll]["exports"]:
                    target = MODULES[dll]["exports"][nm]
                if target is None:
                    target = slot_for("%s!%s" % (dll, nm))
                w32(base + (imp.address - pref), target)
    log("  [module] loaded %s at %08x (pref %08x, exports %d, imports %d)" % (
        rec["name"], base, pref, len(rec["exports"]), sum(len(e.imports) for e in getattr(pe2, "DIRECTORY_ENTRY_IMPORT", []))))
    return rec

def _format_c(fmt_addr, args_addr):
    fmt = cstr(fmt_addr, 4096)
    out = []
    ai = 0
    i = 0
    while i < len(fmt):
        c = fmt[i]
        if c != "%":
            out.append(c); i += 1; continue
        i += 1
        spec = ""
        while i < len(fmt) and fmt[i] in "-+ #0123456789.lh":
            spec += fmt[i]; i += 1
        if i >= len(fmt):
            break
        t = fmt[i]; i += 1
        if t == "%":
            out.append("%"); continue
        v = r32(args_addr + 4 * ai); ai += 1
        width = 0
        zero = spec.startswith("0")
        digits = "".join(ch for ch in spec if ch.isdigit())
        if digits:
            width = int(digits.split(".")[0] or 0)
        if t == "s":
            s = cstr(v, 4096) if v else "(null)"
        elif t in "di":
            s = str(v - (1 << 32) if v & 0x80000000 else v)
        elif t == "u":
            s = str(v)
        elif t in "xX":
            s = ("%x" % v) if t == "x" else ("%X" % v)
        elif t == "c":
            s = chr(v & 0xff)
        else:
            s = "?"
        if len(s) < width:
            s = s.rjust(width, "0" if zero and t not in "s" else " ")
        out.append(s)
    return "".join(out)

_CreateFileA_prev = IMPL.get("CreateFileA")
_ReadFile_prev = IMPL.get("ReadFile")
_SetFilePointer_prev = IMPL.get("SetFilePointer")
_GetFileSize_prev = IMPL.get("GetFileSize")
_CloseHandle_prev = IMPL.get("CloseHandle")

@impl("CreateFileA")
def _i():
    p = cstr(ARG(0))
    up = p.upper()
    if up.startswith(DEVPFX):
        dev = up[len(DEVPFX):]
        if dev.startswith("GLOBAL" + BS) or "SECDRV" in dev or dev.startswith("SECSERV"):
            log("  CreateFileA(device %r) -> driver handle" % p)
            return new_handle({"kind": "dev", "path": p})
        if len(dev) == 2 and dev[1] == ":" or dev.startswith("CDROM"):
            log("  CreateFileA(device %r) -> CD handle" % p)
            return new_handle({"kind": "cd", "path": p, "pos": 0})
        log("  CreateFileA(device %r) -> not found" % p)
        state["last_error"] = 2
        return 0xffffffff
    if up[:3] == "D:" + BS or up == "D:":
        log("  CreateFileA(%r) -> CD file (not served yet)" % p)
        state["last_error"] = 2
        return 0xffffffff
    return _CreateFileA_prev()

@impl("ReadFile")
def _i():
    h, buf, n, pr = ARG(0), ARG(1), ARG(2), ARG(3)
    d = HANDLE_FILES.get(h)
    if d is not None and d.get("kind") == "cd":
        chunk = iso_read(d["pos"], n) if d["pos"] < ISO_SIZE else b""
        DISC_READS.append(("ReadFile", d["pos"], n, len(chunk)))
        log("  ReadFile(%s) off=%d n=%d got=%d" % (d["path"], d["pos"], n, len(chunk)))
        if chunk:
            mu.mem_write(buf, chunk)
        d["pos"] += len(chunk)
        if pr:
            w32(pr, len(chunk))
        return 1
    return _ReadFile_prev()

@impl("SetFilePointer")
def _i():
    d = HANDLE_FILES.get(ARG(0))
    if d is not None and d.get("kind") == "cd":
        lo, hip, method = ARG(1), ARG(2), ARG(3)
        hi = r32(hip) if hip else 0
        off = ((hi << 32) | lo) if hip else (lo - (1 << 32) if lo & 0x80000000 else lo)
        base = {0: 0, 1: d["pos"], 2: ISO_SIZE}.get(method, 0)
        d["pos"] = base + off
        log("  SetFilePointer(cd) -> %d" % d["pos"])
        return d["pos"] & 0xffffffff
    return _SetFilePointer_prev()

@impl("SetFilePointerEx")
def _i():
    d = HANDLE_FILES.get(ARG(0))
    lo, hi, newp, method = ARG(1), ARG(2), ARG(3), ARG(4)
    off = ((hi << 32) | lo) & 0xffffffffffffffff
    if off >= 1 << 63:
        off -= 1 << 64
    if d is None:
        return 0
    if d.get("kind") == "cd":
        base = {0: 0, 1: d["pos"], 2: ISO_SIZE}.get(method, 0)
        d["pos"] = base + off
        if newp:
            w32(newp, d["pos"] & 0xffffffff); w32(newp + 4, (d["pos"] >> 32) & 0xffffffff)
        return 1
    return 0

@impl("GetFileSize")
def _i():
    d = HANDLE_FILES.get(ARG(0))
    if d is not None and d.get("kind") == "cd":
        return ISO_SIZE & 0xffffffff
    return _GetFileSize_prev()

@impl("CloseHandle")
def _i():
    d = HANDLE_FILES.get(ARG(0))
    if d is not None and d.get("kind") in ("cd", "dev"):
        HANDLE_FILES.pop(ARG(0), None)
        return 1
    return _CloseHandle_prev()

@impl("LoadLibraryA")
def _i():
    s = cstr(ARG(0))
    n = norm(s)
    if n in OVERLAY and bytes(OVERLAY[n][:2]) == b"MZ":
        bn = n.rsplit(BS, 1)[-1].lower()
        if bn in MODULES:
            return MODULES[bn]["base"]
        return load_pe_module(n, bytes(OVERLAY[n]))["base"]
    h = dll_handle(s.lower())
    HANDLE_DLL[h] = s.lower()
    log("  LoadLibraryA(%s) -> %08x (no module)" % (s, h))
    return h

@impl("GetProcAddress")
def _i():
    h, n = ARG(0), ARG(1)
    nm = ("ord%d" % n) if n < 0x10000 else cstr(n)
    rec = MODULE_BY_BASE.get(h)
    if rec is not None:
        a = rec["ords"].get(n) if n < 0x10000 else rec["exports"].get(nm)
        log("  GetProcAddress(%s!%s) -> %08x" % (rec["name"], nm, a or 0))
        return a or 0
    dll = "kernel32.dll" if h == IB else HANDLE_DLL.get(h, "?")
    key = "%s!%s" % (dll, nm)
    log("  GetProcAddress(%s) -> slot" % key)
    return slot_for(key)

@impl("DeviceIoControl")
def _i():
    h, code, inp, insz, outp, outsz = ARG(0), ARG(1), ARG(2), ARG(3), ARG(4), ARG(5)
    d = HANDLE_FILES.get(h) or {}
    kind = d.get("kind", "none")
    inb = bytes(mu.mem_read(inp, min(insz, 0x600))) if inp and insz else b""
    IOCTL_LOG.append((kind, code, insz, outsz, inb[:0x20].hex()))
    log("  DeviceIoControl kind=%s code=%08x in=%d out=%d in[:32]=%s" % (kind, code, insz, outsz, inb[:0x20].hex()))
    if kind == "dev" and code == 0xef002407:
        return _secdrv_reply(inp, insz, outp, outsz)
    if kind == "cd":
        DISC_READS.append(("IOCTL%08x" % code, -1, insz, outsz))
    state["last_error"] = 1
    return 0

def _secdrv_reply(inp, insz, outp, outsz):
    # Mode "log" only records the request and fails it. Mode "answers" returns the values
    # the driver is known to hand out (verified against the constants present in SECDRV.SYS).
    mode = os.environ.get("SECDRV_MODE", "log")
    if mode != "answers":
        state["last_error"] = 1
        return 0
    cmd = r32(inp + 12) if inp and insz >= 16 else 0
    ans = {0x3c: 0x400, 0x3d: 0x2c8, 0x3e: 0x5278d11b}
    log("      secdrv command %#x mode=answers" % cmd)
    if cmd in ans and outp:
        w32(outp + 0, 4); w32(outp + 4, 3); w32(outp + 8, 86)
        w32(outp + 12 + 0x400, 4)          # ExtraDataSize (illustrative; layout not verified)
        w32(outp + 12 + 0x404 + 0, ans[cmd])
        return 1
    state["last_error"] = 1
    return 0

@impl("ReadProcessMemory")
def _i():
    h, src, dst, n, pr = ARG(0), ARG(1), ARG(2), ARG(3), ARG(4)
    try:
        mu.mem_write(dst, bytes(mu.mem_read(src, n)))
    except UcError:
        return 0
    if pr:
        w32(pr, n)
    return 1

@impl("WriteProcessMemory")
def _i():
    h, dst, src, n, pw = ARG(0), ARG(1), ARG(2), ARG(3), ARG(4)
    try:
        mu.mem_write(dst, bytes(mu.mem_read(src, n)))
    except UcError:
        return 0
    if IB + 0x1000 <= dst < IB + 0x2cb000:
        log("  WriteProcessMemory into .text %08x n=%d" % (dst, n))
    if pw:
        w32(pw, n)
    return 1

@impl("wsprintfA")
def _i():
    buf, fmt = ARG(0), ARG(1)
    s = _format_c(fmt, uc_esp() + 12)
    b = s.encode("latin-1", "replace") + b"\x00"
    mu.mem_write(buf, b)
    return len(b) - 1

def uc_esp():
    return CUR_UC[0].reg_read(UC_X86_REG_ESP)
CUR_UC = [mu]

@impl("CreateEventA")
def _i():
    return heap_alloc(16)

@impl("SetEvent")
def _i():
    return 1

@impl("ResetEvent")
def _i():
    return 1

@impl("OpenEventA")
def _i():
    state["last_error"] = 2
    return 0

@impl("OpenMutexA")
def _i():
    state["last_error"] = 2
    return 0

@impl("WaitForMultipleObjects")
def _i():
    return 0

@impl("CreateFileMappingA")
def _i():
    state["last_error"] = 5
    return 0

@impl("OpenFileMappingA")
def _i():
    state["last_error"] = 2
    return 0

@impl("MapViewOfFile")
def _i():
    return 0

@impl("UnmapViewOfFile")
def _i():
    return 1

@impl("CopyFileA")
def _i():
    state["last_error"] = 5
    return 0

@impl("GetHandleInformation")
def _i():
    if ARG(1):
        w32(ARG(1), 0)
    return 1

@impl("HeapDestroy")
def _i():
    return 1

@impl("HeapSize")
def _i():
    return 0x1000

@impl("EnterCriticalSection")
def _i():
    return 0

@impl("LeaveCriticalSection")
def _i():
    return 0

@impl("InitializeCriticalSection")
def _i():
    return 0

@impl("DeleteCriticalSection")
def _i():
    return 0

@impl("ReportEventA")
def _i():
    return 0

@impl("RegisterEventSourceA")
def _i():
    return 0

@impl("DeregisterEventSource")
def _i():
    return 1

@impl("RegOpenKeyA")
def _i():
    return 2

@impl("RegOpenKeyExA")
def _i():
    return 2

@impl("RegQueryValueExA")
def _i():
    return 2

@impl("RegEnumKeyExA")
def _i():
    return 259

@impl("RegCloseKey")
def _i():
    return 0

@impl("OpenSCManagerA")
def _i():
    log("  OpenSCManagerA -> fake handle")
    return 0x5000

@impl("OpenServiceA")
def _i():
    log("  OpenServiceA(%r) -> not found" % cstr(ARG(1)))
    state["last_error"] = 1060
    return 0

@impl("CreateServiceA")
def _i():
    log("  CreateServiceA(%r) -> denied" % cstr(ARG(2)))
    state["last_error"] = 5
    return 0

@impl("StartServiceA")
def _i():
    state["last_error"] = 1060
    return 0

@impl("CloseServiceHandle")
def _i():
    return 1

@impl("IsValidSecurityDescriptor")
def _i():
    return 1

@impl("InitializeSecurityDescriptor")
def _i():
    return 1

@impl("SetSecurityDescriptorDacl")
def _i():
    return 1

@impl("GetKeyboardType")
def _i():
    return 0

@impl("GetCommandLineA")
def _i():
    return heap_alloc(0x200)

@impl("OpenProcess")
def _i():
    log("  OpenProcess(pid=%d) -> parent handle" % ARG(2))
    return 0x3000

@impl("FindResourceA")
def _i():
    return 0

STDCALL_BYTES.update({
 "CreateFileMappingA": 24, "MapViewOfFile": 20, "CopyFileA": 12, "GetHandleInformation": 8,
 "OpenFileMappingA": 12, "HeapDestroy": 4, "EnterCriticalSection": 4, "LeaveCriticalSection": 4,
 "InitializeCriticalSection": 4, "DeleteCriticalSection": 4, "OpenEventA": 12, "OpenMutexA": 12,
 "UnmapViewOfFile": 4, "ReadProcessMemory": 20, "WriteProcessMemory": 20,
 "WaitForMultipleObjects": 16, "ReportEventA": 36, "RegisterEventSourceA": 12,
 "DeregisterEventSource": 4, "RegEnumKeyExA": 32, "RegQueryValueExA": 24, "RegOpenKeyA": 12,
 "RegOpenKeyExA": 20, "RegCloseKey": 4, "CloseServiceHandle": 4, "OpenServiceA": 12,
 "OpenSCManagerA": 12, "IsValidSecurityDescriptor": 4, "InitializeSecurityDescriptor": 8,
 "SetSecurityDescriptorDacl": 16, "GetKeyboardType": 4, "CreateEventA": 16, "SetEvent": 4,
 "ResetEvent": 4, "SetFilePointerEx": 20, "GetFileSizeEx": 8, "CreateServiceA": 52,
 "StartServiceA": 12, "OpenProcess": 12, "GetCommandLineA": 0, "HeapSize": 12,
 "FindResourceA": 12, "WaitForMultipleObjects": 16, "wsprintfA": -1,
})
# ===================== end extension =====================


# ---- heap family (the helper uses HeapAlloc through the process heap) ----
HEAP_ALLOCS = {}
@impl("GetProcessHeap")
def _i():
    return 0x1000

@impl("HeapCreate")
def _i():
    return 0x1001

@impl("HeapAlloc")
def _i():
    flags, size = ARG(1), ARG(2)
    p = heap_alloc(size if size else 1)
    HEAP_ALLOCS[p] = size
    return p

@impl("HeapFree")
def _i():
    return 1

@impl("HeapReAlloc")
def _i():
    old, size = ARG(2), ARG(3)
    p = heap_alloc(size if size else 1)
    if old and old in HEAP_ALLOCS:
        n = min(HEAP_ALLOCS[old], size)
        mu.mem_write(p, bytes(mu.mem_read(old, n)))
    HEAP_ALLOCS[p] = size
    return p

STDCALL_BYTES.update({"GetProcessHeap": 0, "HeapCreate": 12, "HeapAlloc": 12, "HeapFree": 12, "HeapReAlloc": 16})

def hook_code(uc, addr, size, user):
    if not (HOOK_BASE <= addr < HOOK_BASE + HOOK_SIZE):
        return
    if addr == RET_EXIT:
        log("[exit] initial return reached")
        uc.emu_stop()
        return
    key = slot_name.get(addr)
    if key is None:
        return
    short = key.split("!", 1)[-1]
    esp = uc.reg_read(UC_X86_REG_ESP)
    ret = r32(esp)
    state["ncalls"] += 1
    log("  call %s ret=%08x esp=%08x" % (key, ret, esp))
    _sa = STRING_ARGS.get(short)
    if _sa is not None:
        for _i in _sa:
            _p = ARG(_i)
            try:
                log("      arg%d=%r" % (_i, cstr(_p)) if _p > 0x10000 else "      arg%d=%08x" % (_i, _p))
            except Exception as _e:
                log("      arg%d=?? %s" % (_i, _e))
    nbytes = STDCALL_BYTES.get(short, 0)
    if nbytes < 0:
        nbytes = 0
    if short == "CreateProcessA":
        log("      CreateProcessA app=%r cmd=%r" % (cstr(ARG(0)) if ARG(0) > 0x10000 else None, cstr(ARG(1)) if ARG(1) > 0x10000 else None))
    if short in IMPL:
        ret_val = IMPL[short]()
    elif key in API:
        ret_val = API[key][0](None)
        nbytes = API[key][1]
    else:
        ret_val = 0
        log("  [unimpl] %s  args=%s" % (key, ",".join("%08x" % ARG(i) for i in range(4))))
    if ret_val is None:
        ret_val = 0
    if short == "ExitProcess" or key == "kernel32.dll!ExitProcess":
        uc.emu_stop()
        return
    uc.reg_write(UC_X86_REG_EAX, ret_val & 0xffffffff)
    uc.reg_write(UC_X86_REG_ESP, esp + 4 + nbytes)
    uc.reg_write(UC_X86_REG_EIP, ret)

def mem_unmapped(uc, access, addr, size, value, user):
    eip = uc.reg_read(UC_X86_REG_EIP)
    log("[unmapped] access=%d addr=%08x size=%d eip=%08x" % (access, addr, size, eip))
    return False

text_writes = {}
def mem_write_text(uc, access, addr, size, value, user):
    page = addr & ~0xfff
    text_writes[page] = text_writes.get(page, 0) + 1

mu.hook_add(UC_HOOK_CODE, hook_code, None, HOOK_BASE, HOOK_BASE + HOOK_SIZE - 1)
from collections import deque
TRACE = deque(maxlen=int(__import__("os").environ.get("TRACE_N", "120")))
def trace_code(uc, addr, size, user):
    TRACE.append((addr, uc.reg_read(UC_X86_REG_EAX), uc.reg_read(UC_X86_REG_ECX), uc.reg_read(UC_X86_REG_ESP)))
if __import__("os").environ.get("TRACE"):
    mu.hook_add(UC_HOOK_CODE, trace_code, None, IB + 0x7e0000, IB + 0x7e5000)
mu.hook_add(UC_HOOK_MEM_UNMAPPED, mem_unmapped)
mu.hook_add(UC_HOOK_MEM_WRITE, mem_write_text, None, IB + 0x1000, IB + 0x2cb000 - 1)

mu.reg_write(UC_X86_REG_EAX, 0)
mu.reg_write(UC_X86_REG_EBX, 0)

t0 = time.time()
stop_reason = None
try:
    mu.emu_start(EP, OEP_CANDIDATE, timeout=0, count=MAX_INSN)
    stop_reason = "reached until"
except UcError as e:
    stop_reason = "UcError: %s eip=%08x" % (e, mu.reg_read(UC_X86_REG_EIP))
print("stop:", stop_reason, "insns-limit", MAX_INSN, "time %.1fs" % (time.time() - t0))
print("eip=%08x esp=%08x eax=%08x" % (mu.reg_read(UC_X86_REG_EIP), mu.reg_read(UC_X86_REG_ESP), mu.reg_read(UC_X86_REG_EAX)))
print("api calls:", state["ncalls"], "exit:", state["exit"])
if __import__("os").environ.get("TRACE"):
    for a, eax, ecx, esp in TRACE:
        print("  trace %08x eax=%08x ecx=%08x esp=%08x" % (a, eax, ecx, esp))
print("text page writes:", len(text_writes), sorted(text_writes.items())[:20])

# dump any files the stub created in the overlay (temp payloads)
import os as _os
_outdir = _os.environ.get("VT3_DUMP_DIR", "/Users/heath/Virtua Tennis 3/dump/pc/safedisc")
_os.makedirs(_outdir, exist_ok=True)  # payloads the stub wrote, dumped for analysis
for _k, _v in OVERLAY.items():
    _fn = _k.replace("\\", "_").replace(":", "")
    open(_os.path.join(_outdir, _fn), "wb").write(bytes(_v))
    print("overlay file", _k, len(_v))

# ---- end-of-run report: how much of the parent image changed, plus IOCTL and disc-read logs ----
_img_now = bytes(mu.mem_read(IB, IMG_SIZE))
_img_file = data[:0]
_changed_text = 0
_text_secs = [s for s in pe.sections if s.Name.rstrip(b"\0") == b".text"]
for _s in _text_secs:
    _raw = data[_s.PointerToRawData:_s.PointerToRawData + min(_s.SizeOfRawData, _s.Misc_VirtualSize)]
    _mem = _img_now[_s.VirtualAddress:_s.VirtualAddress + len(_raw)]
    _changed_text += sum(1 for a, b in zip(_raw, _mem) if a != b)
print("parent .text bytes changed vs file: %d" % _changed_text)
_dump_dir = os.environ.get("VT3_DUMP_DIR", "/Users/heath/Virtua Tennis 3/dump/pc/safedisc")
os.makedirs(_dump_dir, exist_ok=True)
open(os.path.join(_dump_dir, "parent_image_at_stop.bin"), "wb").write(_img_now)
print("modules:", {k: hex(v["base"]) for k, v in MODULES.items()})
print("IOCTLs seen: %d" % len(IOCTL_LOG))
for _k in IOCTL_LOG[:40]:
    print("  ioctl kind=%s code=%08x in=%d out=%d in[:32]=%s" % _k)
print("disc reads: %d" % len(DISC_READS))
for _r in DISC_READS[:40]:
    print("  disc", _r)
