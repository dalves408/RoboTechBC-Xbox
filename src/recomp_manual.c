/**
 * Manual function overrides and ICALL diagnostics
 *
 * This file provides:
 *   - recomp_lookup_manual()  : intercept specific Xbox VAs with hand-written code
 *   - recomp_icall_fail_log() : log when an indirect call target can't be resolved
 *   - ICALL trace ring buffer  : globals used by the RECOMP_ICALL macro
 *
 * The recomp pipeline generates an auto-dispatch table (recomp_lookup) that
 * resolves most function addresses. recomp_lookup_manual() is called FIRST,
 * giving you a chance to override any function with a custom implementation.
 *
 * Common reasons to add manual overrides:
 *   - Trace a function to understand call flow (wrap the generated version)
 *   - Fix a function the lifter translated incorrectly
 *   - Stub out a function that crashes (return early, set eax to a safe value)
 *   - Redirect a function to a native implementation (e.g., skip CRT init)
 *   - Intercept D3D/audio calls for custom rendering or sound
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stddef.h>   /* ptrdiff_t */
#include <windows.h>

#include "recomp/recomp_types.h"
#include "xbox_memory_layout.h"

/* ── ICALL trace ring buffer ───────────────────────────────── */

/*
 * These globals are written by the RECOMP_ICALL macro (defined in
 * recomp_types.h) every time an indirect call is dispatched. When a
 * crash occurs, the VEH handler or recomp_icall_fail_log() can dump
 * the last 16 call targets to help you trace what happened.
 *
 * The runtime owns them: xbox_kernel defines all three in
 * src/kernel/xbox_memory_layout.c, and recomp_types.h declares them extern.
 * Declare, do not define -- a definition here as well is a duplicate symbol,
 * and a project copied from this template failed to link on all three:
 *
 *   xbox_memory_layout.obj : error LNK2005: g_icall_count already defined
 *                            in recomp_manual.obj
 */
extern volatile uint32_t g_icall_trace[16];
extern volatile uint32_t g_icall_trace_idx;
extern volatile uint64_t g_icall_count;

/* ── Manual function overrides ─────────────────────────────── */

/*
 * Return a function pointer to override the given Xbox VA, or NULL
 * to fall through to the auto-generated dispatch table.
 *
 * This is called on every indirect call (RECOMP_ICALL) and every
 * direct call through the dispatch table, so keep it fast. A chain
 * of if-statements on uint32_t compiles to a simple comparison
 * sequence; for large override tables, consider a sorted array
 * with binary search.
 *
 * Examples of common override patterns:
 *
 *   // Trace wrapper: log entry/exit around the generated function
 *   extern void sub_00012345(void);
 *   static void traced_sub_00012345(void) {
 *       fprintf(stderr, "[TRACE] sub_00012345 entered, eax=0x%08X\n", g_eax);
 *       sub_00012345();
 *       fprintf(stderr, "[TRACE] sub_00012345 returned, eax=0x%08X\n", g_eax);
 *   }
 *
 *   // Stub: skip a function entirely (return 0 in eax)
 *   static void stub_00067890(void) {
 *       g_eax = 0;
 *   }
 *
 *   // Fix: replace a broken lifted function with correct C
 *   static void fixed_sub_000ABCDE(void) {
 *       // Read arguments from stack/registers per calling convention
 *       uint32_t arg1 = g_ecx;
 *       uint32_t arg2 = MEM32(g_esp + 4);
 *       // ... correct implementation ...
 *       g_eax = result;
 *   }
 */
/* D3D KickOff helpers (Xbox VA in the D3D section).
 *
 * KickOff @ 0x155730 sets PFB flush (inline hard-spin at loc_00155774), then
 * loops on 0x156840 until DMA_GET catches PUT. Sleep(1) on every busy poll
 * over-yielded (many kicks/frame) and still left GET stuck around flip ~1000
 * with MAD~0 / draws frozen. Strategy:
 *   1) Override full KickOff so the inline PFB spin cannot BusyLoop.
 *   2) Clear PFB ourselves via xbox_Nv2aAckBusyBits (D3D11 owns the work).
 *   3) Wait on PUT vs channel GET only (skip sticky 0x400700 secondary).
 *   4) After a short QPC budget, snap GET to PUT so the guest can leave
 *      KickOff; PB/DX11 continue asynchronously.
 * Callers PUSH a return address; every override path must g_esp += 4 (or
 * call the original, which does). */
extern void sub_00155730(void);
extern void sub_001556F0(void);
extern void xbox_Nv2aAckBusyBits(void);

#define RBX_NV2A_USER_DMA_PUT 0x800040u
#define RBX_NV2A_USER_DMA_GET 0x800044u

static int rbx_d3d_dma_busy(uint32_t device)
{
    uint32_t pb, put, get;
    if (!device)
        return 0;
    pb = MEM32(device + 0x23BCu);
    if (!pb)
        return 0;
    put = MEM32(device + 0x2Cu) & 0x0FFFFFFFu;
    get = MEM32(pb + 0x44u) & 0x0FFFFFFFu;
    return put != get;
}

/* Perf counters, read by the [RBXPERF] stats line in rbx_d3d11.c. */
volatile LONG g_rbx_forced_catchups;   /* times GET was snapped to PUT before the walker read the commands */
volatile LONG g_rbx_fence_spurious;    /* BlockOnFence returned before its fence was reached */
volatile LONG g_rbx_fence_timeouts;    /* ...and the 2 s recheck gave up */

static void rbx_d3d_force_get_catchup(uint32_t device)
{
    uint32_t pb, put;
    volatile uint32_t *hw_put;
    volatile uint32_t *hw_get;
    static LONG s_log;

    if (!device)
        return;
    pb = MEM32(device + 0x23BCu);
    put = MEM32(device + 0x2Cu) & 0x0FFFFFFFu;
    if (pb)
        MEM32(pb + 0x44u) = put;
    InterlockedIncrement(&g_rbx_forced_catchups);
    hw_put = xbox_Nv2aRegPtr(RBX_NV2A_USER_DMA_PUT);
    hw_get = xbox_Nv2aRegPtr(RBX_NV2A_USER_DMA_GET);
    if (hw_put && hw_get)
        *hw_get = *hw_put;
    if (InterlockedIncrement(&s_log) <= 8) {
        fprintf(stderr, "[RBX] KickOff GET catch-up put=0x%08X (forced)\n", put);
        fflush(stderr);
    }
}

/* Called from gen KickOff / 0x1556F0 PFB hard-spins (direct calls, not ABI). */
void rbx_d3d_pfb_spin_yield(void)
{
    static LONG s_n;
    LONG n = InterlockedIncrement(&s_n);
    xbox_Nv2aKickHint();
    xbox_Nv2aAckBusyBits();
    if ((n % 4096) == 1) {
        fprintf(stderr, "[RBX] PFB spin yield #%ld\n", (long)n);
        fflush(stderr);
    }
    SwitchToThread();
}

/* Called from gen KickOff when 0x156840 still reports busy after one yield. */
void rbx_d3d_kick_get_yield(void)
{
    uint32_t device = MEM32(0x00166B48u);
    xbox_Nv2aKickHint();
    rbx_d3d_force_get_catchup(device);
    SwitchToThread();
}

static void rbx_d3d_wait_pfb(uint32_t regs)
{
    int spins = 0;
    xbox_Nv2aKickHint();
    MEM32(regs + 0x100410u) |= 0x00010000u;
    while (MEM32(regs + 0x100410u) & 0x00010000u) {
        xbox_Nv2aKickHint();
        if (++spins >= 16) {
            xbox_Nv2aAckBusyBits();
            break;
        }
        YieldProcessor();
    }
    if (MEM32(regs + 0x100410u) & 0x00010000u)
        xbox_Nv2aAckBusyBits();
}

/* Wait until GET catches PUT, or force catch-up after ~budget_ms. */
static void rbx_d3d_wait_get(uint32_t device, int budget_ms)
{
    LARGE_INTEGER t0, t, freq;
    int spins = 0;

    if (!rbx_d3d_dma_busy(device))
        return;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t0);
    for (;;) {
        xbox_Nv2aKickHint();
        if (!rbx_d3d_dma_busy(device))
            return;
        if (++spins < 512) {
            YieldProcessor();
            continue;
        }
        spins = 0;
        SwitchToThread();
        QueryPerformanceCounter(&t);
        if ((t.QuadPart - t0.QuadPart) * 1000 >= freq.QuadPart * (LONGLONG)budget_ms) {
            rbx_d3d_force_get_catchup(device);
            return;
        }
    }
}

static void rbx_d3d_busy_check_yield(void)
{
    uint32_t device = MEM32(0x00166B48u);
    /* PUT vs channel GET only — original also returns busy when
     * regs[0x400700] != 0 after GET catches, which can stick forever here. */
    if (rbx_d3d_dma_busy(device)) {
        g_eax = 1;
        xbox_Nv2aKickHint();
        SwitchToThread();
    } else {
        g_eax = 0;
    }
    g_esp += 4;
}

static void rbx_d3d_kick_pfb_yield(void)
{
    uint32_t device = MEM32(0x00166B48u);
    uint32_t regs;
    if (!device) {
        sub_001556F0();
        return;
    }
    regs = MEM32(device + 0x23c0u);
    if (!regs) {
        sub_001556F0();
        return;
    }
    rbx_d3d_wait_pfb(regs);
    g_esp += 4;
}

/* Full KickOff — replaces sub_00155730 so the inline PFB spin yields. */
static void rbx_d3d_kickoff_yield(void)
{
    uint32_t self = g_ecx;
    uint32_t put;
    uint32_t flags;
    uint32_t device;
    uint32_t regs;
    uint32_t pb;
    static LONG s_kick_n;
    LONG n0 = InterlockedIncrement(&s_kick_n);
    if (n0 <= 8 || (n0 % 512) == 0) {
        fprintf(stderr, "[RBX] KickOff enter #%ld ecx=0x%08X\n", (long)n0, self);
        fflush(stderr);
    }

    if ((MEM8(self + 8) & 4u) != 0)
        put = MEM32(self + 0x35Cu);
    else
        put = MEM32(self);
    MEM32(self + 0x2Cu) = put;

    flags = MEM32(self + 8);
    if (((flags >> 8) & 0x20u) != 0) {
        /* Rare channel-reset path — keep original (includes esp pop). */
        g_ecx = self;
        sub_00155730();
        return;
    }

    device = MEM32(0x00166B48u);
    regs = device ? MEM32(device + 0x23C0u) : 0;
    if (!regs) {
        g_ecx = self;
        sub_00155730();
        return;
    }
    rbx_d3d_wait_pfb(regs);

    pb = MEM32(self + 0x23BCu);
    MEM32(pb + 0x40u) = put & 0x0FFFFFFFu;

    if (MEM32(0x00166B4Cu) != 0) {
        uint32_t get0 = pb ? (MEM32(pb + 0x44u) & 0x0FFFFFFFu) : 0;
        int busy0 = rbx_d3d_dma_busy(self);
        /* ~4 ms: long enough for a hot ack/walker tick, short enough that a
         * stuck GET cannot freeze the Ch1 cinematic for minutes. */
        rbx_d3d_wait_get(self, 4);
        if ((n0 % 256) == 1) {
            uint32_t get1 = pb ? (MEM32(pb + 0x44u) & 0x0FFFFFFFu) : 0;
            fprintf(stderr,
                    "[RBX] KickOff #%ld put=0x%08X get0=0x%08X get1=0x%08X busy0=%d gate=0x%08X\n",
                    (long)n0, put & 0x0FFFFFFFu, get0, get1, busy0,
                    MEM32(0x00166B4Cu));
            fflush(stderr);
        }
        recomp_rmw_or32(self + 8, 0x2000u);
    } else if ((n0 % 256) == 1) {
        fprintf(stderr, "[RBX] KickOff gate 0x166B4C=0 (no GET wait) put=0x%08X\n",
                put & 0x0FFFFFFFu);
        fflush(stderr);
    }

    g_ecx = self;
    g_esp += 4;
}

/* D3D BlockOnFence (0x1558D0, args: fence, flags; ret 8). When the GPU is far
 * from the fence it patches software method 5 into the pushbuffer after the
 * fence and sleeps on the PGRAPH event -- and returns as soon as that event is
 * set, without looking at the fence again. The runtime also sets the event
 * outside method 5 (IdleWake after each consumed kick, RBX-17), which then
 * released D3D before the walk had reached the fence: MakeSpace took the
 * unread ring as free and overwrote commands the walk had not read (menu
 * flashes, "[PB] bad target" resyncs). Re-check the fence before returning;
 * a spurious wake now only costs a short spin. RECOMP_FENCE_RECHECK=0 = off. */
extern void sub_001558D0_orig(void);
void sub_001558D0(void)
{
    static int on = -1;
    uint32_t target = MEM32(g_esp + 4);
    uint32_t dev, fence_va, counter;
    DWORD t0;
    if (on < 0) {
        const char *e = getenv("RECOMP_FENCE_RECHECK");
        on = !(e && e[0] == '0');
    }
    sub_001558D0_orig();
    if (!on)
        return;
    dev = MEM32(0x00166B48u);
    if (!dev)
        return;
    fence_va = MEM32(dev + 0x34u);
    counter = MEM32(dev + 0x30u);
    if (counter - target >= counter - MEM32(fence_va))
        return;
    {
        LONG n = InterlockedIncrement(&g_rbx_fence_spurious);
        if (n <= 4 || (n & (n - 1)) == 0) {
            fprintf(stderr, "[RBX] BlockOnFence woke early: fence=%08X target=%08X counter=%08X (#%ld)\n",
                    MEM32(fence_va), target, counter, (long)n);
            fflush(stderr);
        }
    }
    t0 = GetTickCount();
    while (counter - target < counter - MEM32(fence_va)) {
        if (GetTickCount() - t0 > 2000u) {
            LONG n = InterlockedIncrement(&g_rbx_fence_timeouts);
            if (n <= 8) {
                fprintf(stderr, "[RBX] BlockOnFence recheck timeout fence=%08X target=%08X\n",
                        MEM32(fence_va), target);
                fflush(stderr);
            }
            break;
        }
        Sleep(0);
    }
}

/* 0x1166C is the shared tail of 0x11370 (the mission's per-frame callback,
 * third argument of the fixed-step loop 0x10FC80) and 0x11424:
 *
 *     cmp  [0x1BC410], edi
 *     pop  edi
 *     jne  0x12440        ; tail call
 *     ret
 *
 * The lifter did not see it as an entry point and left an unresolved stub
 * that only pops 4 bytes. That never restored edi (0x11370 zeroes it), so the
 * fixed-step loop's update pointer became NULL and the mission froze on its
 * first frame; esp also drifted by 4 per frame and 0x12440 never ran.
 * scripts/patch_kickoff_yield.ps1 renames the generated stub out of the way. */
void sub_00012440(void);
void sub_0001166C(void)
{
    int same = MEM32(0x1BC410u) == g_edi;
    g_edi = MEM32(g_esp);
    g_esp += 4;
    if (same) {
        g_esp += 4;
        return;
    }
    sub_00012440();
}

/* RECOMP_XPP_TRACE=1: log the title's calls into XAPI's peripheral (XPP)
 * entry points -- args off the guest stack and eax on return -- so input
 * bring-up can see which of XGetDevices / XInputOpen / XInputGetState the
 * title actually reaches and what each one hands back.
 *
 * Generated code calls these directly, so the hook is link-time: build with
 * -DRBX_XPP_TRACE after renaming each listed definition in src/recomp/gen to
 * sub_00XXXXXX_orig. Known: 187F4F XInputOpen, 18825A XGetDevices,
 * 18827C XGetDeviceChanges, 188233 XdReportDeviceInsertionRemoval,
 * 18AF89 USBD enumeration-stage completion (DPC). */
#ifdef RBX_XPP_TRACE
#define XPP_LIST(X) X(187F4F) X(187FA5) X(187FB1) X(18818E) X(1881FD) \
                    X(188255) X(18825A) X(18827C) X(18879E) X(1888D0) X(18AF89) X(188233) X(188320) X(189376)
#define XPP_EXT(v) extern void sub_00##v##_orig(void);
XPP_LIST(XPP_EXT)
static void xpp_log(uint32_t va, recomp_func_t fn, volatile LONG *cnt)
{
    uint32_t sp = g_esp, ra = MEM32(sp);
    uint32_t a1 = MEM32(sp + 4), a2 = MEM32(sp + 8), a3 = MEM32(sp + 12), a4 = MEM32(sp + 16);
    uint32_t c1 = g_ecx;
    uint32_t pre[8] = {0};
    if (c1 >= 0x10000u && c1 < 0x04000000u) {
        for (int k = 0; k < 6; k++) pre[k] = MEM32(c1 + 4 * k);
        if (pre[4] >= 0x10000u && pre[4] < 0x04000000u) {
            pre[6] = MEM32(pre[4] + 0x14);
            if (pre[6] >= 0x10000u && pre[6] < 0x04000000u)
                pre[7] = MEM32(pre[6] + 4 * ((pre[1] >> 24) & 0xFF));
        }
    }
    LONG n = InterlockedIncrement(cnt);
    fn();
    if (n <= 12 || (n & (n - 1)) == 0) {
        fprintf(stderr, "[XPP] %08X #%ld ra=%08X ecx=%08X args %08X %08X %08X %08X -> eax=%08X\n",
                va, (long)n, ra, c1, a1, a2, a3, a4, g_eax);
        if (a1 >= 0x10000u && a1 < 0x04000000u)
            fprintf(stderr, "[XPP]    *a1 = %08X %08X %08X %08X %08X %08X\n",
                    MEM32(a1), MEM32(a1 + 4), MEM32(a1 + 8), MEM32(a1 + 12),
                    MEM32(a1 + 16), MEM32(a1 + 20));
        if (c1 >= 0x10000u && c1 < 0x04000000u)
            fprintf(stderr, "[XPP]    pre *ecx = %08X %08X %08X %08X %08X %08X tbl=%08X ent=%08X\n",
                    pre[0], pre[1], pre[2], pre[3], pre[4], pre[5], pre[6], pre[7]);
        fflush(stderr);
    }
}
#define XPP_WRAP(v) static volatile LONG xpp_n_##v; \
    void sub_00##v(void) { xpp_log(0x00##v##u, sub_00##v##_orig, &xpp_n_##v); }
XPP_LIST(XPP_WRAP)
#endif /* RBX_XPP_TRACE */

recomp_func_t recomp_lookup_manual(uint32_t xbox_va)
{
    if (xbox_va == 0x00155730u) {
        static LONG s_look;
        LONG n = InterlockedIncrement(&s_look);
        if (n <= 16 || (n % 1024) == 0) {
            fprintf(stderr, "[RBX] lookup 0x155730 -> kickoff_yield (#%ld)\n", (long)n);
            fflush(stderr);
        }
        return rbx_d3d_kickoff_yield;
    }
    if (xbox_va == 0x00156840u)
        return rbx_d3d_busy_check_yield;
    if (xbox_va == 0x001556F0u)
        return rbx_d3d_kick_pfb_yield;
    return (recomp_func_t)0;
}

/* ── ICALL failure logging ─────────────────────────────────── */

/*
 * Called when RECOMP_ICALL cannot resolve a target address.
 * This usually means one of:
 *   - A vtable dispatch to an address not in the dispatch table
 *   - A function pointer loaded from uninitialized or corrupt memory
 *   - A kernel thunk address that the bridge doesn't handle
 *
 * During early bring-up you will see many of these. Most are harmless
 * (the ICALL macro pops the dummy return address and continues).
 * Focus on the ones that cause crashes or incorrect behavior.
 */
void recomp_icall_fail_log(uint32_t va)
{
    fprintf(stderr, "[ICALL] Failed to resolve VA 0x%08X (total calls: %llu)\n",
            va, (unsigned long long)g_icall_count);

    /* Dump last 16 call targets from the ring buffer */
    fprintf(stderr, "  Recent ICALL targets:\n");
    for (int i = 0; i < 16; i++) {
        int idx = (g_icall_trace_idx - 16 + i) & 15;
        if (g_icall_trace[idx])
            fprintf(stderr, "    [%2d] 0x%08X\n", i, g_icall_trace[idx]);
    }
    fflush(stderr);
}

/* An indirect call whose target is not code: a null or wild function pointer.
 *
 * Skipping these is right -- calling a data address is worse -- but skipping
 * them *silently* is not. They almost always arrive inside a loop, so the
 * symptom is a hang with no output rather than a diagnosable null vtable call.
 *
 * Rate-limited per address: a spin can produce millions of these, and the
 * useful information is which addresses occur, not how often.
 */
void recomp_icall_not_code_log(uint32_t va)
{
    enum { SLOTS = 32 };
    static uint32_t seen[SLOTS], seen_ret[SLOTS];
    static uint64_t hits[SLOTS];
    static int count;
    int i;
    /* Every caller pushes the guest return address just before this check,
     * so it identifies the call site (the target alone is usually just 0). */
    uint32_t ret = MEM32(g_esp);

    for (i = 0; i < count; i++)
        if (seen[i] == va && seen_ret[i] == ret)
            break;
    if (i == count) {
        if (count == SLOTS)
            return;
        seen[count] = va;
        seen_ret[count] = ret;
        hits[count] = 0;
        count++;
    }
    hits[i]++;
    /* Report at 1, 10, 100, 1000 ... rather than once. A single line says a
     * wild pointer was skipped; the progression says it is being skipped in a
     * loop, which is the difference between a curiosity and the reason the
     * title is hung. */
    {
        uint64_t n = hits[i];
        while (n >= 10 && n % 10 == 0)
            n /= 10;
        if (n != 1)
            return;
    }
    fprintf(stderr, "[ICALL] target 0x%08X is not code -- skipped %llu time(s) "
                    "(null or wild function pointer, ret 0x%08X, at call #%llu)\n",
            va, (unsigned long long)hits[i], ret,
            (unsigned long long)g_icall_count);
    fflush(stderr);
}

#ifdef RBX_ABI_CHECK
/* Built with -DRBX_ABI_CHECK=ON: a lifted callee returned without restoring
 * ebx/esi/edi (or popped too little). Reported per callee at 1, 10, 100...
 * hits, with the caller's return address, so the routine corrupting a hot
 * loop stands out. */
void recomp_abi_violation_log(uint32_t va, uint32_t ebx0, uint32_t esi0,
                              uint32_t edi0, uint32_t esp0)
{
    enum { SLOTS = 1024 };
    static uint32_t seen[SLOTS];
    static uint64_t hits[SLOTS];
    static int count;
    uint64_t n;
    int i;

    for (i = 0; i < count; i++)
        if (seen[i] == va)
            break;
    if (i == count) {
        if (count == SLOTS)
            return;
        seen[count++] = va;
    }
    n = ++hits[i];
    while (n >= 10 && n % 10 == 0)
        n /= 10;
    if (n != 1)
        return;
    fprintf(stderr, "[ABI] sub_%08X x%llu:%s%s%s%s ebx %08X->%08X esi %08X->%08X "
                    "edi %08X->%08X esp %08X->%08X ret %08X\n",
            va, (unsigned long long)hits[i],
            g_ebx != ebx0 ? " ebx" : "", g_esi != esi0 ? " esi" : "",
            g_edi != edi0 ? " edi" : "", g_esp < esp0 + 4 ? " esp" : "",
            ebx0, g_ebx, esi0, g_esi, edi0, g_edi, esp0, g_esp, MEM32(esp0));
    fflush(stderr);
}
#endif

/* ── Untranslated instructions ───────────────────────────────────────────
 *
 * The lifter emits RECOMP_UNIMPL(text, va) at every instruction it has no
 * translation for, in place of the bare comment it used to leave. The
 * instruction is still a no-op; this only stops the omission being silent.
 * RECOMP_UNIMPL_TRAP=1 aborts at the first hit, at the guest address of the
 * cause rather than wherever the damage surfaces. */
#include <stdlib.h>

void recomp_unimpl(const char *text, uint32_t va)
{
    static int printed;
    const char *trap = getenv("RECOMP_UNIMPL_TRAP");
    int stop = trap && *trap && *trap != '0';

    if (printed < 50 || stop) {
        printed++;
        fprintf(stderr,
                "[UNIMPL] untranslated instruction REACHED: `%s` at 0x%08X"
                " (a no-op; set RECOMP_UNIMPL_TRAP=1 to stop here)\n",
                text, va);
        fflush(stderr);
    }
    if (stop) abort();
}
