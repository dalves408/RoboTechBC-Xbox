"""Symbolized native stacks of every thread in a running process (x64).

    python scripts/diag/stackdump.py <pid> [--out file] [--max-frames N]

Uses dbghelp StackWalk64 with the exe's PDB, so lifted guest functions show up
as sub_XXXXXXXX frames: a hang dump reads like a guest call stack.
rbx_diag.py calls dump_stacks() automatically when it detects a frame hang.
"""
import argparse
import ctypes as C
import ctypes.wintypes as W
import sys

k32 = C.WinDLL("kernel32", use_last_error=True)
dbg = C.WinDLL("dbghelp", use_last_error=True)
psapi = C.WinDLL("psapi", use_last_error=True)

TH32CS_SNAPTHREAD = 0x4
THREAD_ALL = 0x0002 | 0x0008 | 0x0010 | 0x0040  # suspend, get ctx, query
PROCESS_ALL = 0x0400 | 0x0010  # query info, vm read
CONTEXT_FULL = 0x10000B
IMAGE_FILE_MACHINE_AMD64 = 0x8664
SYMOPT_UNDNAME, SYMOPT_DEFERRED, SYMOPT_LOAD_LINES = 0x2, 0x4, 0x10


class THREADENTRY32(C.Structure):
    _fields_ = [("dwSize", W.DWORD), ("cntUsage", W.DWORD), ("th32ThreadID", W.DWORD),
                ("th32OwnerProcessID", W.DWORD), ("tpBasePri", W.LONG),
                ("tpDeltaPri", W.LONG), ("dwFlags", W.DWORD)]


class M128A(C.Structure):
    _fields_ = [("Low", C.c_uint64), ("High", C.c_int64)]


class CONTEXT(C.Structure):
    _pack_ = 16
    _fields_ = ([(n, C.c_uint64) for n in ("P1Home", "P2Home", "P3Home", "P4Home", "P5Home", "P6Home")] +
                [("ContextFlags", W.DWORD), ("MxCsr", W.DWORD)] +
                [(n, W.WORD) for n in ("SegCs", "SegDs", "SegEs", "SegFs", "SegGs", "SegSs")] +
                [("EFlags", W.DWORD)] +
                [(n, C.c_uint64) for n in ("Dr0", "Dr1", "Dr2", "Dr3", "Dr6", "Dr7",
                                           "Rax", "Rcx", "Rdx", "Rbx", "Rsp", "Rbp", "Rsi", "Rdi",
                                           "R8", "R9", "R10", "R11", "R12", "R13", "R14", "R15", "Rip")] +
                [("FltSave", C.c_byte * 512), ("VectorRegister", M128A * 26),
                 ("VectorControl", C.c_uint64), ("DebugControl", C.c_uint64),
                 ("LastBranchToRip", C.c_uint64), ("LastBranchFromRip", C.c_uint64),
                 ("LastExceptionToRip", C.c_uint64), ("LastExceptionFromRip", C.c_uint64)])


class ADDRESS64(C.Structure):
    _fields_ = [("Offset", C.c_uint64), ("Segment", W.WORD), ("Mode", C.c_int)]


class KDHELP64(C.Structure):
    _fields_ = [("data", C.c_uint64 * 8), ("pad", W.DWORD * 4), ("Reserved", C.c_uint64 * 2)]


class STACKFRAME64(C.Structure):
    _fields_ = [("AddrPC", ADDRESS64), ("AddrReturn", ADDRESS64), ("AddrFrame", ADDRESS64),
                ("AddrStack", ADDRESS64), ("AddrBStore", ADDRESS64),
                ("FuncTableEntry", C.c_void_p), ("Params", C.c_uint64 * 4),
                ("Far", W.BOOL), ("Virtual", W.BOOL), ("Reserved", C.c_uint64 * 3),
                ("KdHelp", KDHELP64)]


class SYMBOL_INFO(C.Structure):
    _fields_ = [("SizeOfStruct", W.ULONG), ("TypeIndex", W.ULONG), ("Reserved", C.c_uint64 * 2),
                ("Index", W.ULONG), ("Size", W.ULONG), ("ModBase", C.c_uint64),
                ("Flags", W.ULONG), ("Value", C.c_uint64), ("Address", C.c_uint64),
                ("Register", W.ULONG), ("Scope", W.ULONG), ("Tag", W.ULONG),
                ("NameLen", W.ULONG), ("MaxNameLen", W.ULONG), ("Name", C.c_char * 512)]


class IMAGEHLP_LINE64(C.Structure):
    _fields_ = [("SizeOfStruct", W.DWORD), ("Key", C.c_void_p), ("LineNumber", W.DWORD),
                ("FileName", C.c_char_p), ("Address", C.c_uint64)]


k32.OpenProcess.restype = W.HANDLE
k32.OpenThread.restype = W.HANDLE
k32.CreateToolhelp32Snapshot.restype = W.HANDLE
k32.GetThreadContext.argtypes = [W.HANDLE, C.c_void_p]
k32.GetThreadDescription.argtypes = [W.HANDLE, C.POINTER(C.c_wchar_p)]
dbg.SymInitialize.argtypes = [W.HANDLE, C.c_char_p, W.BOOL]
dbg.SymFromAddr.argtypes = [W.HANDLE, C.c_uint64, C.POINTER(C.c_uint64), C.POINTER(SYMBOL_INFO)]
dbg.SymGetLineFromAddr64.argtypes = [W.HANDLE, C.c_uint64, C.POINTER(W.DWORD), C.POINTER(IMAGEHLP_LINE64)]
dbg.SymGetModuleBase64.restype = C.c_uint64
dbg.SymGetModuleBase64.argtypes = [W.HANDLE, C.c_uint64]
psapi.GetModuleBaseNameW.argtypes = [W.HANDLE, C.c_void_p, C.c_wchar_p, W.DWORD]
dbg.StackWalk64.argtypes = [W.DWORD, W.HANDLE, W.HANDLE, C.POINTER(STACKFRAME64), C.c_void_p,
                            C.c_void_p, C.c_void_p, C.c_void_p, C.c_void_p]
_fta = C.cast(dbg.SymFunctionTableAccess64, C.c_void_p)
_gmb = C.cast(dbg.SymGetModuleBase64, C.c_void_p)


def _sym(hp, addr):
    si = SYMBOL_INFO()
    si.SizeOfStruct = SYMBOL_INFO.Name.offset + 4  # sizeof(SYMBOL_INFO) == 88
    si.MaxNameLen = 511
    disp = C.c_uint64()
    name = f"0x{addr:X}"
    base = dbg.SymGetModuleBase64(hp, C.c_uint64(addr))
    if base:
        mb = C.create_unicode_buffer(260)
        if psapi.GetModuleBaseNameW(hp, C.c_void_p(base), mb, 260):
            name = f"{mb.value}+0x{addr - base:X}"
    if dbg.SymFromAddr(hp, addr, C.byref(disp), C.byref(si)):
        name = f"{si.Name.decode(errors='replace')}+0x{disp.value:X}"
    ln = IMAGEHLP_LINE64()
    ln.SizeOfStruct = C.sizeof(IMAGEHLP_LINE64)
    d2 = W.DWORD()
    if dbg.SymGetLineFromAddr64(hp, addr, C.byref(d2), C.byref(ln)) and ln.FileName:
        f = ln.FileName.decode(errors="replace").replace("\\", "/").rsplit("/", 1)[-1]
        name += f"  [{f}:{ln.LineNumber}]"
    return name


def _threads(pid):
    snap = k32.CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0)
    te = THREADENTRY32()
    te.dwSize = C.sizeof(te)
    out = []
    ok = k32.Thread32First(snap, C.byref(te))
    while ok:
        if te.th32OwnerProcessID == pid:
            out.append(te.th32ThreadID)
        ok = k32.Thread32Next(snap, C.byref(te))
    k32.CloseHandle(snap)
    return out


def dump_stacks(pid, max_frames=40):
    """Return a text report of every thread's symbolized stack."""
    hp = k32.OpenProcess(PROCESS_ALL, False, pid)
    if not hp:
        return f"OpenProcess({pid}) failed: {C.get_last_error()}\n"
    dbg.SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED | SYMOPT_LOAD_LINES)
    dbg.SymInitialize(hp, None, True)
    lines = []
    for tid in _threads(pid):
        ht = k32.OpenThread(THREAD_ALL, False, tid)
        if not ht:
            continue
        desc = C.c_wchar_p()
        tname = ""
        if k32.GetThreadDescription(ht, C.byref(desc)) >= 0 and desc.value:
            tname = desc.value
        k32.SuspendThread(ht)
        try:
            buf = C.create_string_buffer(C.sizeof(CONTEXT) + 16)
            addr = (C.addressof(buf) + 15) & ~15
            ctx = CONTEXT.from_address(addr)
            ctx.ContextFlags = CONTEXT_FULL
            if not k32.GetThreadContext(ht, addr):
                lines.append(f"--- thread {tid} {tname}: GetThreadContext failed")
                continue
            sf = STACKFRAME64()
            sf.AddrPC.Offset, sf.AddrPC.Mode = ctx.Rip, 3
            sf.AddrFrame.Offset, sf.AddrFrame.Mode = ctx.Rbp, 3
            sf.AddrStack.Offset, sf.AddrStack.Mode = ctx.Rsp, 3
            frames = []
            for _ in range(max_frames):
                if not dbg.StackWalk64(IMAGE_FILE_MACHINE_AMD64, hp, ht, C.byref(sf), addr,
                                       None, _fta, _gmb, None):
                    break
                if not sf.AddrPC.Offset:
                    break
                frames.append(_sym(hp, sf.AddrPC.Offset))
            lines.append(f"--- thread {tid} {tname} ({len(frames)} frames)")
            lines += ["    " + f for f in frames]
        finally:
            k32.ResumeThread(ht)
            k32.CloseHandle(ht)
    dbg.SymCleanup(hp)
    k32.CloseHandle(hp)
    return "\n".join(lines) + "\n"


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("pid", type=int)
    ap.add_argument("--out")
    ap.add_argument("--max-frames", type=int, default=40)
    a = ap.parse_args()
    txt = dump_stacks(a.pid, a.max_frames)
    if a.out:
        open(a.out, "w").write(txt)
    else:
        sys.stdout.write(txt)
