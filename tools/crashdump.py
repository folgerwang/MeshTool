"""Reads a MeshTool crash dump without a debugger installed.

    python tools/crashdump.py %LOCALAPPDATA%\\CrashDumps\\MeshTool.exe.<pid>.dmp [build\\Release\\MeshTool.exe]

Prints the exception, the module it happened in, and every return address on
the crashing thread's stack that resolves into MeshTool.exe, with source
file and line from the PDB next to the executable (Release builds keep it).
Uses the `minidump` package (pip install minidump) and Windows' dbghelp.dll.
The stack is scanned rather than walked, so stale frames can appear; the
innermost MeshTool frames are the ones to read first.
"""
import ctypes
import ctypes.wintypes as wt
import os
import struct
import sys

from minidump.minidumpfile import MinidumpFile

dump = sys.argv[1]
exe = os.path.abspath(sys.argv[2] if len(sys.argv) > 2 else
                      os.path.join(os.path.dirname(__file__), "..", "build", "Release", "MeshTool.exe"))
mf = MinidumpFile.parse(dump)

exc = mf.exception.exception_records[0]
rec = exc.ExceptionRecord
print(f"exception {rec.ExceptionCode} at 0x{rec.ExceptionAddress:x}, thread {exc.ThreadId}")

mods = [(m.name, m.baseaddress, m.size) for m in mf.modules.modules]
exe_base = next((b for n, b, s in mods if n.lower().endswith("meshtool.exe")), 0)


def modof(a):
    for n, b, s in mods:
        if b <= a < b + s:
            return n, b
    return None, 0


th = next(t for t in mf.threads.threads if t.ThreadId == exc.ThreadId)
ctx = th.ContextObject
rsp = ctx.Rsp

dbg = ctypes.WinDLL("dbghelp.dll")
h = wt.HANDLE(0x1234)
dbg.SymSetOptions(0x10 | 0x2 | 0x4)   # SYMOPT_LOAD_LINES | UNDNAME | DEFERRED_LOADS
if not dbg.SymInitialize(h, None, False):
    raise SystemExit("SymInitialize failed")
dbg.SymLoadModuleEx.restype = ctypes.c_ulonglong
dbg.SymLoadModuleEx.argtypes = [wt.HANDLE, wt.HANDLE, ctypes.c_char_p, ctypes.c_char_p, ctypes.c_ulonglong,
                                wt.DWORD, ctypes.c_void_p, wt.DWORD]
dbg.SymLoadModuleEx(h, None, exe.encode(), None, exe_base, 0, None, 0)


class SYMBOL_INFO(ctypes.Structure):
    _fields_ = [("SizeOfStruct", wt.DWORD), ("TypeIndex", wt.DWORD), ("Reserved", ctypes.c_ulonglong * 2),
                ("Index", wt.DWORD), ("Size", wt.DWORD), ("ModBase", ctypes.c_ulonglong), ("Flags", wt.DWORD),
                ("Value", ctypes.c_ulonglong), ("Address", ctypes.c_ulonglong), ("Register", wt.DWORD),
                ("Scope", wt.DWORD), ("Tag", wt.DWORD), ("NameLen", wt.DWORD), ("MaxNameLen", wt.DWORD),
                ("Name", ctypes.c_char * 512)]


class IMAGEHLP_LINE64(ctypes.Structure):
    _fields_ = [("SizeOfStruct", wt.DWORD), ("Key", ctypes.c_void_p), ("LineNumber", wt.DWORD),
                ("FileName", ctypes.c_char_p), ("Address", ctypes.c_ulonglong)]


dbg.SymFromAddr.argtypes = [wt.HANDLE, ctypes.c_ulonglong, ctypes.POINTER(ctypes.c_ulonglong),
                            ctypes.POINTER(SYMBOL_INFO)]
dbg.SymGetLineFromAddr64.argtypes = [wt.HANDLE, ctypes.c_ulonglong, ctypes.POINTER(wt.DWORD),
                                     ctypes.POINTER(IMAGEHLP_LINE64)]


def sym(a):
    n, b = modof(a)
    if n is None:
        return None
    if not n.lower().endswith("meshtool.exe"):
        return f"{os.path.basename(n)}+0x{a - b:x}"
    si = SYMBOL_INFO()
    si.SizeOfStruct = 88
    si.MaxNameLen = 511
    disp = ctypes.c_ulonglong(0)
    if not dbg.SymFromAddr(h, a, ctypes.byref(disp), ctypes.byref(si)):
        return f"MeshTool.exe+0x{a - b:x}"
    line = IMAGEHLP_LINE64()
    line.SizeOfStruct = ctypes.sizeof(IMAGEHLP_LINE64)
    d2 = wt.DWORD(0)
    where = ""
    if dbg.SymGetLineFromAddr64(h, a, ctypes.byref(d2), ctypes.byref(line)):
        where = f"  {os.path.basename(line.FileName.decode(errors='replace'))}:{line.LineNumber}"
    return f"{si.Name.decode(errors='replace')}+0x{disp.value:x}{where}"


print("in:", sym(rec.ExceptionAddress))
reader = mf.get_reader()
start, size = th.Stack.StartOfMemoryRange, th.Stack.MemoryLocation.DataSize
stack = reader.read(start, size)
print(f"stack of {size} bytes, rsp at +0x{rsp - start:x}; return addresses found:")
for off in range(0, len(stack) - 8, 8):
    a = struct.unpack_from("<Q", stack, off)[0]
    n, b = modof(a)
    if n is None:
        continue
    s = sym(a)
    if n.lower().endswith("meshtool.exe") or "+0x0" not in s:
        if "MeshTool.exe" in s or ":" in s or any(k in n.lower() for k in ("nvoglv", "vulkan", "ntdll")):
            print(f"  +0x{off:05x}: {s}")
