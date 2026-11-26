/**
 * Robotech Battlecry (Xbox) - Recompiled Game Entry Point
 *
 * This is the Windows executable that hosts the recompiled game code.
 * It performs the following initialization sequence:
 *
 * 1. Load the original XBE file from disk
 * 2. Initialize the Xbox memory layout (map data sections to original VAs)
 * 3. Initialize the Xbox kernel replacement layer
 * 4. Initialize the kernel bridge (thunk table in Xbox memory)
 * 5. Set up game file paths for I/O redirection
 * 6. Initialize the stack pointer
 * 7. Install VEH crash handler for diagnostics
 * 8. Call the game's original entry point (recompiled)
 *
 * Customize this file for your game:
 *   - Set YOUR_GAME_ENTRY_POINT to the XBE entry point address
 *   - Set YOUR_GAME_XBE_PATH to where the XBE file lives
 *   - Set YOUR_GAME_DIR to the game data directory
 *   - Add any CRT global pre-initialization your game needs
 *   - Customize the VEH handler for game-specific crash diagnosis
 *
 * XBE Details (from analysis/default_analysis.json):
 *   Title:       Robotech: Battlecry
 *   Title ID:    0x544D0002
 *   Base addr:   0x00010000
 *   Entry point: 0x0001319F
 *   Sections:    26 (.text + D3D + DSOUND + BINK* + XPP + DOLBY)
 *   default.xbe SHA-256: 2fc8745a72d495626307d644e80f36ec48e216e0132c3c0394446f51f94400b3
 */

#include <windows.h>
#include <dbghelp.h>
#include <stdio.h>
#include <stdlib.h>
#include <io.h>
#include <stdbool.h>
#include <string.h>
#include <wchar.h>
#include <math.h>

/* xboxrecomp runtime headers */
#include <xbox/xboxrecomp.h>
#include <ohci.h>

/*
 * If xboxrecomp.h is not an umbrella header in your setup, include
 * the individual headers directly:
 *
 * #include "kernel.h"
 * #include "xbox_memory_layout.h"
 * #include "d3d8_xbox.h"
 * #include "dsound_xbox.h"
 * #include "xinput_xbox.h"
 */

/* Hybrid-graphics laptops (NVIDIA Optimus, AMD switchable): ask the driver to
 * run this process on the discrete GPU. Without these exports D3D11's default
 * adapter is the integrated one that drives the panel, so the game rendered
 * on Intel UHD -- slower, and with the Intel-only per-draw flush workaround
 * (RECOMP_GPU_FLUSH) -- while the dGPU sat idle. The drivers look the symbols
 * up by name in the exe; they do nothing on single-GPU machines. A per-app
 * choice in Windows Graphics settings still overrides them. */
__declspec(dllexport) DWORD NvOptimusEnablement = 0x00000001;
__declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;

/* ── Global register state (defined in xbox_memory_layout.c) ── */

/* RECOMP_TLS is not optional here. The runtime defines these thread-local, and
 * a plain `extern` referencing a __declspec(thread) variable does not resolve
 * to the calling thread's copy -- it resolves to the image's TLS template. The
 * host side then writes g_esp somewhere the generated code never reads, so the
 * guest starts with every register at zero and faults immediately, having
 * apparently ignored the setup that visibly ran. */
extern RECOMP_TLS uint32_t g_eax, g_ecx, g_edx, g_esp;
extern RECOMP_TLS uint32_t g_ebx, g_esi, g_edi;
extern RECOMP_TLS uint32_t g_seh_ebp;
/* x87 and SSE state. Global for the same reason the volatile GPRs are: one
 * guest routine can lift to several C functions, so a value written in one
 * body is read in the next. Defined in xbox_memory_layout.c like the rest of
 * the register file. */
extern RECOMP_TLS double g_fp_stack[8];
extern RECOMP_TLS int g_fp_top;
extern RECOMP_TLS uint16_t g_fp_control_word;
extern RECOMP_TLS uint32_t g_mxcsr;
extern RECOMP_TLS int g_fp_cmp;
extern RECOMP_TLS RecompXmm g_xmm0, g_xmm1, g_xmm2, g_xmm3;
extern RECOMP_TLS RecompXmm g_xmm4, g_xmm5, g_xmm6, g_xmm7;
extern ptrdiff_t g_xbox_mem_offset;

/* ── XBE Constants ─────────────────────────────────────────── */

/*
 * Paths are relative to the installation root selected at startup.
 */
#define YOUR_GAME_ENTRY_POINT   0x0001319F  /* XBE entry point VA (retail XOR decoded) */
#define YOUR_GAME_XBE_PATH      "game_files\\default.xbe"
#define YOUR_GAME_DIR            "game_files"

static int portable_paths(void)
{
    WCHAR location[MAX_PATH], path[MAX_PATH];
    char save_utf8[MAX_PATH * 3];
    DWORD n = GetModuleFileNameW(NULL, location, MAX_PATH);
    if (n && n < MAX_PATH) {
        WCHAR *slash = wcsrchr(location, L'\\');
        if (slash) {
            *slash = L'\0';
            if (swprintf_s(path, MAX_PATH, L"%ls\\game_files\\default.xbe", location) > 0 &&
                GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES &&
                !SetCurrentDirectoryW(location))
                return 0;
        }
    }

    const char *override = getenv("RECOMP_SAVE_DIR");
    if (override && *override) {
        if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, override, -1, location, MAX_PATH))
            return 0;
    } else {
        wcscpy_s(location, MAX_PATH, L"SaveData");
    }
    n = GetFullPathNameW(location, MAX_PATH, path, NULL);
    /* xbox_path_init reserves room for UDATA, the title ID and filenames. */
    if (!n || n >= MAX_PATH - 96 ||
        !WideCharToMultiByte(CP_UTF8, 0, path, -1, save_utf8, sizeof save_utf8, NULL, NULL))
        return 0;
    DWORD attributes = GetFileAttributesW(path);
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        if (!CreateDirectoryW(path, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) {
            /* The runtime creates nested directories for explicit test overrides. */
            if (!override || !*override) return 0;
        }
    } else if (!(attributes & FILE_ATTRIBUTE_DIRECTORY)) {
        return 0;
    }
    const char *screenshots = getenv("RECOMP_FB_SHOT_DIR");
    int custom_screenshots = screenshots && *screenshots;
    return _putenv_s("RECOMP_SAVE_DIR", save_utf8) == 0 &&
        (custom_screenshots || _putenv_s("RECOMP_FB_SHOT_DIR", "screenshots") == 0);
}

/* ── Forward declarations ──────────────────────────────────── */

static BOOL load_xbe(const char *path, void **out_data, size_t *out_size);

/* Recompiled entry point (generated by recomp pipeline) */
extern void xbe_entry_point(void);

/* ── VEH crash handler ─────────────────────────────────────── */

/*
 * Vectored Exception Handler for crash diagnostics.
 *
 * When the recompiled game hits an access violation, this handler prints
 * the faulting address, all Xbox register values, and a native stack trace.
 * This is your primary debugging tool during bring-up.
 *
 * Customize this for your game:
 *   - Add game-specific address checks (GPU register probes, etc.)
 *   - Add dumps of game-specific globals (heap handles, state flags)
 *   - Add SEH simulation if your game uses __try/__except
 */
/* Name the guest function a fault happened in, and recover the call chain.
 *
 * Recompiled code faults as ordinary native code, so the exception record
 * carries a host RIP and nothing else -- there is no guest program counter to
 * report, and the host address changes every build. Two things recover the
 * guest view:
 *
 *   - every generated function is a real symbol in the image (sub_005A03C0
 *     and so on), so the linker's PDB already maps host address back to guest
 *     function. dbghelp turns an anonymous RIP into that name.
 *
 *   - every lifted call pushes its guest return address onto the guest stack
 *     before jumping, so the stack still holds the chain. Scanning up from esp
 *     for values inside the code sections recovers it.
 *
 * ponytail: the stack scan is a scan, not a frame walk -- these are FPO frames
 * with no reliable ebp chain, so there is nothing to walk. It over-reports,
 * since addresses from returned-from calls linger below esp, but naming the
 * guest function is the whole question at a fault.
 *
 * Requires linking dbghelp and keeping the .pdb beside the .exe.
 */
static void print_guest_context(void *rip)
{
    /* SYMBOL_INFO is variable-length: the name is written past the struct, so
     * it must be over-allocated with MaxNameLen set to the slack. */
    char buf[sizeof(SYMBOL_INFO) + 256];
    SYMBOL_INFO *sym = (SYMBOL_INFO *)buf;
    DWORD64 disp = 0;

    memset(buf, 0, sizeof(buf));
    sym->SizeOfStruct = sizeof(SYMBOL_INFO);
    sym->MaxNameLen = 255;
    if (SymFromAddr(GetCurrentProcess(), (DWORD64)(uintptr_t)rip, &disp, sym))
        fprintf(stderr, "  in %s+0x%llX\n",
                sym->Name, (unsigned long long)disp);

    if (g_xbox_mem_offset && g_esp) {
        const uint32_t *sp =
            (const uint32_t *)((uintptr_t)g_xbox_mem_offset + g_esp);
        int shown = 0, i;
        fprintf(stderr, "  guest stack (return addresses, innermost first):\n");
        for (i = 0; i < 256 && shown < 24; i++) {
            uint32_t v = sp[i];
            if (v > g_xbox_code_lo && v < g_xbox_code_hi) {
                fprintf(stderr, "    [esp+%-4d] 0x%08X\n", i * 4, v);
                shown++;
            }
        }
        /* When the frame is gone, say so, and fall back on what is left.
         *
         * A fault inside a block copy leaves no return address in range: the
         * copy is a host memmove and its caller's frame is further up than
         * this reaches. Worse, a copy that ran past its buffer overwrites the
         * stack itself, so the list comes out empty exactly when it is most
         * needed -- and an empty list under a promising heading reads like
         * "nothing to see" rather than "the evidence is destroyed". */
        if (!shown)
            fprintf(stderr, "    (no return addresses in range)\n");

        /* Everything below only pays for itself when the scan came back
         * empty. A crash where the chain was recovered already says what
         * happened, and burying that under another twenty lines makes the
         * common report worse to read. */
        if (!shown) {
            uint32_t k;

            /* The indirect-call history, which the stack cannot overwrite.
             * recomp_manual.c prints this too, but only from
             * recomp_icall_fail_log -- a page fault inside a block copy
             * never reaches that path, and this is the same information by
             * the route the fault actually took.
             *
             * Declared here rather than included: main.c does not pull in
             * recomp_types.h, and recomp_manual.c spells it the same way. */
            extern volatile uint32_t g_icall_trace[16];
            extern volatile uint32_t g_icall_trace_idx;

            fprintf(stderr, "  recent ICALL targets:");
            for (k = 0; k < 16; k++)
                fprintf(stderr, " %08X",
                        g_icall_trace[(g_icall_trace_idx + k) & 15]);
            fprintf(stderr, "\n");

            /* The frame unfiltered. The pointers and lengths handed to
             * whatever faulted live here and look nothing like code, so the
             * filter above drops exactly what is wanted. On the crash this
             * was written for, one of these words was the end of the mapped
             * RAM mirror -- which is what identified the fault as a copy
             * running off its buffer rather than a stray pointer. */
            for (i = 0; i < 20; i++)
                fprintf(stderr, "    raw[esp+%-4d] 0x%08X\n", i * 4, sp[i]);
        }
    }
}

/* The emulated APU.
 *
 * Two entry points that had no callers anywhere in the tree. apu_mmio_hook.c
 * documents apu_hook_handle_mmio as "called from VEH in main.c" and no main.c
 * called it; apu_decode_and_handle's first line is `if (!g_apu_state) return
 * false` and nothing assigned g_apu_state. So with RECOMP_AC97_READY set the
 * APU's registers were unmapped to be trapped, and every trap was then
 * declined and surfaced as an access violation on the first register
 * DirectSound touched. The VEH below now reaches apu_hook_handle_mmio
 * through the toolkit's xbox_VehMmioFault.
 *
 * Declared rather than included so this does not depend on src/apu being on
 * the include path. */
typedef struct MCPXAPUState MCPXAPUState;
extern MCPXAPUState *mcpx_apu_init_standalone(uint8_t *ram_ptr);
extern MCPXAPUState *g_apu_state;

/* Hardware-register accesses handled by trapping (each costs an exception
 * round trip); read by the [RBXPERF] stats line in rbx_d3d11.c. */
volatile LONG g_rbx_mmio_traps;

static LONG CALLBACK veh_handler(PEXCEPTION_POINTERS ep)
{
    if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) {
        uintptr_t fault_addr = ep->ExceptionRecord->ExceptionInformation[1];

        /*
         * GPU register probe at 0xFD000000 range.
         * Some games probe NV2A registers directly. On real hardware this
         * returns GPU state; here we just skip the instruction.
         * TODO: Implement mini x86-64 decoder for instruction skipping,
         * or connect to the xbox_nv2a library for proper handling.
         */
        if (fault_addr >= 0xFD000000 && fault_addr < 0xFE000000) {
            return EXCEPTION_CONTINUE_SEARCH;
        }

        /* Trapped hardware registers (APU, USB OHCI) -> their models.
         *
         * The other half of RECOMP_AC97_READY: reporting the codec ready is
         * what lets DirectSoundCreate past its first gate, and from there it
         * drives the APU directly, so the registers have to fault to be seen.
         * "Handled" means the access was decoded and the instruction stepped
         * over, so execution resumes instead of unwinding. */
        if (xbox_VehMmioFault(ep)) {
            InterlockedIncrement(&g_rbx_mmio_traps);
            return EXCEPTION_CONTINUE_EXECUTION;
        }

        /* A fault inside ntdll (its unwinder under a profiler's stack
         * sampling, for instance) is handled by ntdll's own SEH; printing it
         * as [CRASH] misled. Faults in the game image or the CRT (a guest
         * block copy running off its buffer) are still reported, and an
         * unhandled one of any kind gets the crash dump (crash_filter). */
        {
            static uintptr_t nt_lo, nt_hi;
            uintptr_t rip = (uintptr_t)ep->ContextRecord->Rip;
            if (!nt_lo) {
                HMODULE m = GetModuleHandleW(L"ntdll.dll");
                if (m) {
                    PIMAGE_NT_HEADERS nt = (PIMAGE_NT_HEADERS)((uint8_t *)m + ((PIMAGE_DOS_HEADER)m)->e_lfanew);
                    nt_hi = (uintptr_t)m + nt->OptionalHeader.SizeOfImage;
                    nt_lo = (uintptr_t)m;
                }
            }
            if (rip >= nt_lo && rip < nt_hi)
                return EXCEPTION_CONTINUE_SEARCH;
        }
        fprintf(stderr, "[CRASH] Access violation at RIP=0x%llX, fault addr=0x%llX (%s)\n",
            (unsigned long long)ep->ContextRecord->Rip,
            (unsigned long long)fault_addr,
            ep->ExceptionRecord->ExceptionInformation[0] ? "write" : "read");
        fprintf(stderr, "  Xbox regs: eax=0x%08X ecx=0x%08X edx=0x%08X esp=0x%08X\n",
            g_eax, g_ecx, g_edx, g_esp);
        fprintf(stderr, "  Xbox regs: ebx=0x%08X esi=0x%08X edi=0x%08X\n",
            g_ebx, g_esi, g_edi);
        fprintf(stderr, "  Xbox VA of fault: 0x%08X\n",
            (uint32_t)(fault_addr - (uintptr_t)g_xbox_mem_offset));
        print_guest_context((void *)ep->ContextRecord->Rip);

        /*
         * TODO: Add game-specific diagnostics here. Examples:
         *
         * Dump CRT heap handle:
         *   uint32_t heap = *(uint32_t *)((uint8_t *)g_xbox_mem_offset + HEAP_HANDLE_VA);
         *   fprintf(stderr, "  CRT heap handle: 0x%08X\n", heap);
         *
         * Dump game state:
         *   uint32_t state = *(uint32_t *)((uint8_t *)g_xbox_mem_offset + GAME_STATE_VA);
         *   fprintf(stderr, "  Game state: %u\n", state);
         */

        /* Print native stack return addresses for debugging */
        {
            /* The handler runs on the faulting thread, so clamp the scan to
             * this thread's stack: a fault near the top of the stack (or a
             * corrupt Rsp) must not fault again in here and lose the log. */
            uintptr_t *sp = (uintptr_t *)ep->ContextRecord->Rsp;
            ULONG_PTR lo = 0, hi = 0;
            int n = 0;
            GetCurrentThreadStackLimits(&lo, &hi);
            if ((uintptr_t)sp >= lo && (uintptr_t)sp < hi && !((uintptr_t)sp & 7))
                n = (int)(((uintptr_t)hi - (uintptr_t)sp) / sizeof *sp);
            if (n > 64) n = 64;
            fprintf(stderr, "  Native stack (return addrs in the first %d slots):\n", n);
            for (int i = 0; i < n && sp[i]; i++) {
                if (sp[i] >= 0x140000000ULL && sp[i] < 0x150000000ULL) {
                    fprintf(stderr, "    [%d] 0x%llX\n", i, (unsigned long long)sp[i]);
                }
            }
        }
        fflush(stderr);
    }

    return EXCEPTION_CONTINUE_SEARCH;
}

/* Crash dumps for players.
 *
 * The VEH above prints the guest view of a fault to stderr, which a player
 * running the release never sees. An exception nothing handles also writes
 * crashes\RoboTechBC_<date>_<time>.dmp beside the exe: threads, their stacks
 * and the memory they point at, a few MB, enough to open in WinDbg or Visual
 * Studio with the matching .pdb. The dump is written from its own thread so a
 * stack overflow still gets one. RECOMP_MINIDUMP=0 turns it off;
 * RECOMP_MINIDUMP=full adds all process memory (large). */
static PEXCEPTION_POINTERS s_crash_ep;
static DWORD s_crash_tid;
static WCHAR s_crash_path[MAX_PATH];

static DWORD WINAPI crash_dump_thread(LPVOID arg)
{
    const char *mode = getenv("RECOMP_MINIDUMP");
    MINIDUMP_EXCEPTION_INFORMATION mei;
    MINIDUMP_TYPE type = (MINIDUMP_TYPE)(MiniDumpWithThreadInfo | MiniDumpWithIndirectlyReferencedMemory |
                                         MiniDumpWithUnloadedModules | MiniDumpWithHandleData);
    SYSTEMTIME st;
    HANDLE f;
    BOOL ok;
    (void)arg;
    if (mode && strcmp(mode, "full") == 0)
        type = (MINIDUMP_TYPE)(type | MiniDumpWithFullMemory);
    CreateDirectoryW(L"crashes", NULL);
    GetLocalTime(&st);
    swprintf_s(s_crash_path, MAX_PATH, L"crashes\\RoboTechBC_%04u%02u%02u_%02u%02u%02u.dmp",
               st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    f = CreateFileW(s_crash_path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE)
        return 1;
    mei.ThreadId = s_crash_tid;
    mei.ExceptionPointers = s_crash_ep;
    mei.ClientPointers = FALSE;
    ok = MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), f, type, &mei, NULL, NULL);
    CloseHandle(f);
    if (!ok)
        DeleteFileW(s_crash_path);
    return ok ? 0 : 1;
}

static LONG WINAPI crash_filter(PEXCEPTION_POINTERS ep)
{
    static volatile LONG once;
    HANDLE t;
    DWORD rc = 1;
    if (InterlockedExchange(&once, 1))
        return EXCEPTION_CONTINUE_SEARCH;
    s_crash_ep = ep;
    s_crash_tid = GetCurrentThreadId();
    fprintf(stderr, "[CRASH] unhandled exception 0x%08lX at 0x%p\n",
            (unsigned long)ep->ExceptionRecord->ExceptionCode, ep->ExceptionRecord->ExceptionAddress);
    t = CreateThread(NULL, 0, crash_dump_thread, NULL, 0, NULL);
    if (t) {
        WaitForSingleObject(t, 60000);
        GetExitCodeThread(t, &rc);
        CloseHandle(t);
    }
    if (rc == 0)
        fprintf(stderr, "[CRASH] dump written to %ls\n", s_crash_path);
    else
        fprintf(stderr, "[CRASH] could not write a crash dump\n");
    fflush(stderr);
    return EXCEPTION_CONTINUE_SEARCH;
}

/* Started from a shortcut (Setup's Start menu entry) nothing catches stderr, so
 * the log goes to logs\run_<date>_<time>.log beside the exe, as Play.bat did.
 * A launcher or console that already redirects stderr keeps it. The newest 20
 * logs are kept. RECOMP_LOG_FILE=0 turns this off. */
static void log_to_file(void)
{
    HANDLE h = GetStdHandle(STD_ERROR_HANDLE);
    const char *e = getenv("RECOMP_LOG_FILE");
    char path[MAX_PATH];
    SYSTEMTIME t;
    if (e && e[0] == '0') return;
    if (h && h != INVALID_HANDLE_VALUE && GetFileType(h) != FILE_TYPE_UNKNOWN) return;
    CreateDirectoryA("logs", NULL);
    {   /* prune: keep the 19 newest, so 20 with this one */
        WIN32_FIND_DATAA fd;
        HANDLE f = FindFirstFileA("logs\\run_*.log", &fd);
        char names[256][64];
        int n = 0, i, j;
        if (f != INVALID_HANDLE_VALUE) {
            do { if (n < 256 && strlen(fd.cFileName) < 64) strcpy(names[n++], fd.cFileName); } while (FindNextFileA(f, &fd));
            FindClose(f);
        }
        for (i = 1; i < n; i++)          /* names sort by time: run_YYYYMMDD_HHMMSS.log */
            for (j = i; j > 0 && strcmp(names[j - 1], names[j]) > 0; j--) {
                char tmp[64]; strcpy(tmp, names[j]); strcpy(names[j], names[j - 1]); strcpy(names[j - 1], tmp);
            }
        for (i = 0; i + 19 < n; i++) {
            snprintf(path, sizeof path, "logs\\%s", names[i]);
            DeleteFileA(path);
        }
    }
    GetLocalTime(&t);
    snprintf(path, sizeof path, "logs\\run_%04u%02u%02u_%02u%02u%02u.log",
             t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    if (freopen(path, "w", stderr)) {
        setvbuf(stderr, NULL, _IONBF, 0);
        SetStdHandle(STD_ERROR_HANDLE, (HANDLE)_get_osfhandle(_fileno(stderr)));
    }
}

#ifdef RBX_HAVE_IMPORT
int rbx_prepare_main(void);   /* src/rbx_prepare_import.c: --prepare, RecompLauncher's setup step */
#endif

/* ── WinMain ───────────────────────────────────────────────── */

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance,
                   LPSTR lpCmdLine, int nCmdShow)
{
    void *xbe_data = NULL;
    size_t xbe_size = 0;

    (void)hInstance;
    (void)hPrevInstance;
    (void)lpCmdLine;
    (void)nCmdShow;

    /* Unbuffered output for immediate visibility during debugging */
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

#ifdef RBX_HAVE_IMPORT
    {   /* --prepare: RecompLauncher's setup step; the game does not start */
        int prepared = rbx_prepare_main();
        if (prepared >= 0) return prepared;
    }
#endif

    if (!portable_paths()) {
        fprintf(stderr, "Could not initialize portable game paths. Check the installation and save folder paths.\n");
        return 1;
    }
    log_to_file();
    if (GetFileAttributesA(YOUR_GAME_XBE_PATH) == INVALID_FILE_ATTRIBUTES) {
        fprintf(stderr, "game_files\\default.xbe is missing beside the exe.\n");
        MessageBoxA(NULL, "The game files are missing (game_files\\default.xbe).\n\n"
                    "Run Setup again with this folder (or Set Up Again in RecompLauncher) and select your\n"
                    "Robotech: Battlecry disc image or folder.",
                    "Robotech: Battlecry Recomp", MB_OK | MB_ICONERROR);
        return 1;
    }

    printf("=== Robotech Battlecry (Xbox) - Static Recompilation ===\n");
    printf("Loading XBE...\n");

    /* Bring-up defaults (same set RedCard / NHL Hitz use). Without
     * RECOMP_PB_EXEC the title's D3D pushbuffers never rasterize, so the
     * framebuffer window stays black after CreateDevice even though KickOff
     * and fence waits complete (fence mirror alone is not enough). Only fill
     * gaps the launcher did not already set. */
    {
        struct { const char *name; const char *value; } defs[] = {
            { "RECOMP_PB_EXEC", "1" },
            /* dma GET publish is required for ring fidelity; segment scan
             * AV'd mid-mission (pb_walk_segment @ 0x8F800000). BlockOnTime
             * multi-second sleeps are capped by RECOMP_SWM5_PACE_MS instead. */
            { "RECOMP_PB_WALK", "dma" },
            /* Mid-walk IdleWake early-signals the D3D fence and races menu
             * UI pushbuffers (white glyph boxes). Off for interactive/bat;
             * Ch1 probes set RECOMP_SWM5_PACE_MS=16 (+ PACE_THREAD) and gate
             * with RECOMP_SWM5_PACE_ENABLE so menus stay clean until m101. */
            { "RECOMP_SWM5_PACE_MS", "0" },
            { "RECOMP_FB_WINDOW", "1" },
            { "RECOMP_KEYBOARD", "1" },
            { "RECOMP_USB", "1" },
            /* Pad 2 is plugged when a second host controller appears and then
             * stays plugged, so Versus keeps both ports while SDL reconnects
             * a pad, but one controller never shows the game a phantom
             * player 2 (it waited for input from a pad that was not there). */
            { "RECOMP_USB_PADS", "sticky" },
            /* Off for Ch1 bring-up: FENCE_EXEC held the guest on unpublished
             * fences while the cinematic walker crawled (~draws freeze at
             * 331). Opt in once HUD flight is solid. */
            { "RECOMP_FENCE_EXEC", "0" },
            /* One-CPU DISPATCH_LEVEL semantics: a raised thread and a DPC
             * never run side by side. Without it XAPI's USB enumeration DPC
             * reports the pad inserted, the title thread calls XInputOpen
             * before that DPC has finished, gets DEVICE_NOT_CONNECTED and
             * never retries: no controller input at all. The old
             * RECOMP_IRQL_UNSTICK=1 default zeroed the depth under the gate
             * and is no longer set. */
            { "RECOMP_DISPATCH_GATE", "1" },
            /* VBLANK_CATCHUP stays off by default (menu timing); probe with =1. */
            { "RECOMP_TITLE_KEVENTS", "1" },
            { "RECOMP_VBLANK", "paced" },
            /* VBLANK_THREAD opt-in (mission probes only) — default off so
             * title menus keep their paced-timer timing. */
            /* Cap timer-thread DPC drain slices (ms) after IRQL unstick floods. */
            { "RECOMP_TIMER_DPC_MS", "3" },
            /* Keep the paced-vblank timer above raster workers so mission
             * frames that burn all cores cannot open multi-second vblank gaps. */
            { "RECOMP_TIMER_PRIO", "2" },
            /* edge + slow mission frames held FLIP_STALL (no PFB ack) so
             * KickOff BusyLoop spun for the whole frame — Ch1 soft hang.
             * Off unless the launcher opts in. */
            { "RECOMP_FLIP_PACING", "0" },
            /* Present vsync wait stacked with keyed-mutex contention on the
             * FB thread; guest pacing is enough for bring-up. */
            { "RECOMP_HEAP_RECLAIM", "1" },
            { "RECOMP_AC97_READY", "1" },
            /* Keep MmAllocateContiguousMemory out of the loaded image range so
             * XAPI's USB interrupt EDs resolve; without this the pad can
             * enumerate (setups OK) but never open interrupt-IN reports. */
            { "RECOMP_CONTIG_AVOID_IMAGE", "1" },
            /* Transform DrawPrimitiveUP / INLINE_ARRAY world geometry through
             * the FFP composite matrix (mission meshes). =0 reproduces the
             * black post-briefing present with huge off-surface coords. */
            { "RECOMP_FFP_INLINE", "1" },
            /* GPU raster path (src/rbx_d3d11.c). =0 falls back to CPU PB_EXEC. */
            { "RECOMP_D3D11", "1" },
            /* D3D11 swap-chain present beside GDI (gpu_present.c). */
            { "RECOMP_D3D_PRESENT", "1" },
            /* oD0 default is write-aware in nv2a_pb_exec (only when VSH never
             * wrote oD0.rgb), so DX11 can keep it on without washing cel
             * outlines that deliberately write black diffuse. */
        };
        if (!getenv("RECOMP_VSH_DEFAULT_DIFFUSE"))
            _putenv_s("RECOMP_VSH_DEFAULT_DIFFUSE", "1");
        size_t i;
        for (i = 0; i < sizeof(defs) / sizeof(defs[0]); i++) {
            if (!getenv(defs[i].name))
                _putenv_s(defs[i].name, defs[i].value);
        }
        /* The toolkit reads RECOMP_IRQL_UNSTICK as present/absent. */
        {
            const char *v = getenv("RECOMP_IRQL_UNSTICK");
            if (v && strcmp(v, "0") == 0)
                _putenv_s("RECOMP_IRQL_UNSTICK", "");
        }
        /* RECOMP_AC97_READY=0 means "force off": presence gates the APU. */
        {
            const char *ac97 = getenv("RECOMP_AC97_READY");
            if (ac97 && ac97[0] == '0' && ac97[1] == '\0')
                _putenv_s("RECOMP_AC97_READY", "");
        }
        {
            const char *fe = getenv("RECOMP_FENCE_EXEC");
            fprintf(stderr, "[BOOT] PB_EXEC=%s FB_WINDOW=%s FENCE_EXEC=%s\n",
                    getenv("RECOMP_PB_EXEC") ? getenv("RECOMP_PB_EXEC") : "(off)",
                    getenv("RECOMP_FB_WINDOW") ? "1" : "(off)",
                    (fe && *fe && strcmp(fe, "0")) ? fe : "(off)");
        }
    }

    /* Install VEH handler (first handler in chain) */
    /* Load symbols up front rather than from inside the handler: at fault
     * time the process is already in a bad way, and SymInitialize
     * allocates. Failure is not fatal -- the handler prints no name. */
    SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME);
    SymInitialize(GetCurrentProcess(), NULL, TRUE);
    AddVectoredExceptionHandler(1, veh_handler);
    {
        const char *md = getenv("RECOMP_MINIDUMP");
        if (!(md && strcmp(md, "0") == 0))
            SetUnhandledExceptionFilter(crash_filter);
    }

    /* Step 1: Load XBE */
    if (!load_xbe(YOUR_GAME_XBE_PATH, &xbe_data, &xbe_size)) {
        MessageBoxA(NULL, "Failed to load default.xbe.\n"
                    "Place the game files in the 'game' subdirectory.",
                    "Recomp", MB_ICONERROR);
        return 1;
    }
    printf("XBE loaded: %zu bytes\n", xbe_size);

    /* Step 2: Initialize Xbox memory layout */
    printf("Initializing Xbox memory layout...\n");
    if (!xbox_MemoryLayoutInit(xbe_data, xbe_size)) {
        MessageBoxA(NULL, "Failed to initialize Xbox memory layout.\n"
                    "The required virtual address range may be unavailable.",
                    "Recomp", MB_ICONERROR);
        free(xbe_data);
        return 1;
    }

    g_xbox_mem_offset = xbox_GetMemoryOffset();
    printf("Xbox memory mapped. Offset: 0x%llX\n", (unsigned long long)g_xbox_mem_offset);

    /* XAPI talks to its controller through the MCPX OHCI registers. The
     * runtime keeps this model opt-in via RECOMP_USB — without xbox_OhciInit
     * the MMIO traps never install, so PAD_SCRIPT / PAD_LIVE / keyboard-as-pad
     * never reach the title (menu stays on A CONFIRM with no presses). */
    xbox_OhciInit();

    /* D3D8 (XDK 4721) stores the device pointer at 0x00166B48. KickOff /
     * BlockOn-style waits compare PUT at +0x30 against *(GET ptr at +0x34).
     * Mirror submitted -> completed so those waits can finish without a real
     * GPU interrupt (same bring-up hook RedCard / Hitz use). */
    xbox_Nv2aMirrorFence(0x00166B48u, 0x30u, 0x34u);

    /* Bring up the emulated APU.
     *
     * Gated on the same variable that unmaps its registers, because the two
     * halves are useless apart: a trap with no model declines every access,
     * and a model nothing traps into never sees a register. The APU walks
     * Xbox physical RAM to find voice buffers, so it gets the guest RAM
     * base. */
    if (getenv("RECOMP_AC97_READY")) {
        g_apu_state = mcpx_apu_init_standalone((uint8_t *)xbox_GetMemoryBase());
        fprintf(stderr, "[BOOT] emulated APU %s\n",
                g_apu_state ? "up" : "FAILED to initialise");
    }

    /* Step 3: Initialize Xbox kernel */
    {
        extern void rbx_settings_install(void);
        rbx_settings_install();
    }
    printf("Initializing Xbox kernel replacement...\n");
    xbox_kernel_init();

    /* Step 4: Set game directory for file I/O path translation */
    {
        extern void xbox_path_init(const char *game_dir, const char *save_dir);
        xbox_path_init(YOUR_GAME_DIR, getenv("RECOMP_SAVE_DIR"));
    }

    /* Step 5: Initialize kernel bridge (thunk table in Xbox memory) */
    printf("Initializing kernel bridge...\n");
    xbox_kernel_bridge_init();

    /* Step 6: Initialize stack */
    g_esp = XBOX_STACK_TOP;

    /*
     * TODO: Pre-initialize CRT globals if needed.
     *
     * Many Xbox games use the MSVC CRT. The CRT's __heap_init sets up a
     * heap descriptor at a game-specific address. You may need to
     * pre-initialize __active_heap to avoid small-block heap issues:
     *
     *   uint32_t *active_heap = (uint32_t *)((uint8_t *)g_xbox_mem_offset + ACTIVE_HEAP_VA);
     *   *active_heap = 1;  // 1 = system heap (HeapAlloc), avoids SBH init
     *
     * Find ACTIVE_HEAP_VA by searching the disassembly for __heap_init
     * or by looking for the CRT's __active_heap global in the data section.
     */

    printf("\n=== Initialization complete ===\n");
    printf("Entry point: 0x%08X\n", YOUR_GAME_ENTRY_POINT);
    printf("ESP: 0x%08X\n", g_esp);

    /* Build the flat dispatch table before the guest runs.
     *
     * Without this recomp_lookup falls back to a binary search over the
     * whole function table -- roughly log2(n) branches on *every*
     * indirect call, which for a 45,000-function C++ title is about 16
     * every time the game goes through a vtable. Half-Life 2 spent most
     * of its static initialisation inside recomp_lookup for exactly this
     * reason, and it read as a hang.
     *
     * Optional by design: if the allocation fails the search still works,
     * so a failure is worth one line and not a fatal error. */
    if (!recomp_dispatch_init())
        fprintf(stderr, "[BOOT] flat dispatch unavailable; "
                        "indirect calls will use the binary search\n");

    /* Arm the hang watchdog. Does nothing unless RECOMP_WATCHDOG_SECS is set,
     * and must be called from this thread -- the guest registers it samples are
     * thread-local, so it has to be handed the copies belonging to the thread
     * that runs guest code.
     *
     * Not optional boilerplate: without this call RECOMP_WATCHDOG_SECS is
     * silently inert, and the one diagnostic that tells a hang from slowness
     * does nothing while appearing to be set. */
    xbox_WatchdogStart();

    /* D3D11 Nv2aBackend: GPU-fills decoded batches, readback into guest FB.
     * Must register before the guest's first KickOff. RECOMP_D3D11=0 skips. */
    {
        extern void rbx_d3d11_register(void);
        rbx_d3d11_register();
    }

    /* Step 7: Call the recompiled entry point */
    printf("\nStarting game...\n");
    fflush(stdout);

    xbe_entry_point();

    printf("\nGame returned. Cleaning up...\n");

    /* Cleanup */
    xbox_kernel_shutdown();
    xbox_MemoryLayoutShutdown();
    free(xbe_data);

    return 0;
}

/* Periodic FB focus reclaim for bat launches (console often stays foreground). */
void rbx_fb_focus_tick(void)
{
    extern void xbox_FramebufferRaiseFocus(void);
    static DWORD s_focus_t0;
    static DWORD s_last;
    DWORD now = GetTickCount();
    if (!s_focus_t0)
        s_focus_t0 = now;
    if (now - s_focus_t0 >= 120000u)
        return;
    if (now - s_last < 3000u)
        return;
    s_last = now;
    xbox_FramebufferRaiseFocus();
}

/* ── XBE Loading ───────────────────────────────────────────── */

static BOOL load_xbe(const char *path, void **out_data, size_t *out_size)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "Cannot open XBE: %s\n", path);
        return FALSE;
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (size <= 0) {
        fclose(f);
        return FALSE;
    }

    void *data = malloc((size_t)size);
    if (!data) {
        fclose(f);
        return FALSE;
    }

    if (fread(data, 1, (size_t)size, f) != (size_t)size) {
        free(data);
        fclose(f);
        return FALSE;
    }

    fclose(f);
    *out_data = data;
    *out_size = (size_t)size;
    return TRUE;
}

/* Console entry point (for debugging -- lets you see printf output) */
int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    return WinMain(GetModuleHandle(NULL), NULL, GetCommandLineA(), SW_SHOW);
}
