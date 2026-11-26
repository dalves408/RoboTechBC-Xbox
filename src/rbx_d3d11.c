/*
 * Robotech Battlecry - D3D11 Nv2aBackend (adapted from MvC2 gpu_backend +
 * Redcard register-combiner HLSL). RECOMP_D3D11=1 (default) registers GPU
 * rasterization; RECOMP_D3D11=0 = CPU. RECOMP_D3D_PRESENT drives the FB window
 * via a D3D11 swap chain. Combiner batches from the executor use rbx_rc_hlsl.h.
 */
#define COBJMACROS
#include <windows.h>
extern unsigned long long g_pb_scan_ticks; extern unsigned g_pb_scan_segs;
extern unsigned long long g_pb_t_batch, g_pb_t_xf, g_pb_t_vsh, g_pb_t_clip, g_pb_t_flush; extern unsigned g_pb_n_xfverts;
static unsigned long long gb_qf(void) { static LARGE_INTEGER f; if (!f.QuadPart) QueryPerformanceFrequency(&f); return (unsigned long long)f.QuadPart; }
#include <d3d11.h>
#include <dxgi.h>
#include <d3dcompiler.h>
#include <wincodec.h>     /* hi-res texture pack */
#include <objbase.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>
#include <stddef.h>
#include "nv2a_backend.h"
#include "rbx_rc_hlsl.h"
#include "rbx_settings.h"
#include "../patches/rbx_gvs.h"   /* GPU vertex programs: hand-off from the executor */
#include "d3d/d3d8_swizzle.h"

/* Sprite filter / overlay (MvC2-only) — no-ops for Robotech. */
static int sprite_filter_init(ID3D11Device *d, ID3D11DeviceContext *c) { (void)d; (void)c; return 0; }
static void sprite_filter_run(ID3D11ShaderResourceView *srv, uint32_t sw, uint32_t sh,
    ID3D11RenderTargetView *rtv, uint32_t dw, uint32_t dh, int on, int smooth, int sharpen)
{ (void)srv; (void)sw; (void)sh; (void)rtv; (void)dw; (void)dh; (void)on; (void)smooth; (void)sharpen; }
#define MVC2_DEBUG_ON_FLIP(va, w, h, pitch, bpp) ((void)0)

extern ptrdiff_t xbox_GetMemoryOffset(void);

/* ---- [RBXPERF] counters (printed with RECOMP_GPU_STATS=1, parsed by scripts/diag/rbx_diag.py) ---- */
static unsigned qpc_us(void);
extern volatile LONG g_rbx_forced_catchups, g_rbx_fence_spurious, g_rbx_fence_timeouts; /* recomp_manual.c */
extern volatile LONG g_rbx_mmio_traps;                                                    /* main.c */
extern volatile LONG g_rbx_fence_wait_us;                                                 /* recomp_manual.c */
extern volatile unsigned g_rbx_pb_drops, g_rbx_pb_midwalk_get;                            /* patches/nv2a_pb_scan_rbx.c */
#ifdef RBX_HAVE_APU_VP
extern unsigned rbx_apu_lock_waits(void), rbx_apu_lock_wait_timeouts(void);              /* patches/apu_vp_rbx.c */
#else
static unsigned rbx_apu_lock_waits(void) { return 0; }
static unsigned rbx_apu_lock_wait_timeouts(void) { return 0; }
#endif
static unsigned st_rb_us, st_rb_n;            /* readback: time writing staging copies into guest memory, and count */
static unsigned st_sh_us, st_sh_n, s_sh_total; /* combiner pixel shaders compiled (time, count, since boot) */
static unsigned st_pub;                       /* frames handed to the window */
static unsigned st_rt_new, st_rt_evict, st_rt_alias; /* render targets created, evicted (LRU, MAX_RT), created for a VA that already had one */
static unsigned st_pub_nort, st_pub_multi, st_rb_defer, st_rb_evict, st_pace_us, st_pace_n, st_pace_calls, st_pace_late;   /* flips whose surface had no GPU target (window fell back to guest memory) / several */
static unsigned long long st_ft_sum, st_ft_max, s_ft_last; /* guest flip-to-flip time, QPC ticks */
static unsigned st_ft_n, st_hitch33, st_hitch50, st_hitch100;  /* flip gaps over 33 / 50 / 100 ms */
/* RECOMP_RBX_SIMRATE=1: how often the game's state actually changes, as opposed to how often it flips. Each flip hashes
 * the world (the constants of every GPU vertex-program draw: camera and object matrices) and everything else (the
 * transformed vertices of CPU-path draws: HUD, sprites, fixed-function world). A flip whose hashes equal the last
 * flip's showed the same picture again. One [SIMRATE] line every 2 s: changes per window, how many identical flips
 * each change was followed by, and the time between changes. If the world changes on every flip the game already
 * steps per frame and interpolation has nothing to add; if it changes every Nth flip at a steady interval, that
 * interval is the simulation tick. */
static int s_sr = -1;
static uint64_t s_sr_world, s_sr_scr, s_sr_world_prev, s_sr_scr_prev;
static unsigned st_sr_flips, st_sr_world_new, st_sr_scr_new, st_sr_any_new, st_sr_run[8], s_sr_run;
static unsigned long long s_sr_last, st_sr_gap_sum, st_sr_gap_max, st_sr_gap_min; static unsigned st_sr_gap_n;
#define SR_SEED 1469598103934665603ULL
static uint64_t sr_fold(uint64_t h, const void *p, size_t n)
{
    const uint8_t *b = (const uint8_t *)p; size_t i;
    for (i = 0; i < n; i++) h = (h ^ b[i]) * 1099511628211ULL;
    return h;
}
static int sr_on(void) { if (s_sr < 0) { const char *e = getenv("RECOMP_RBX_SIMRATE"); s_sr = e && e[0] && e[0] != '0'; } return s_sr; }
static void sr_flip(unsigned long long now)
{
    static DWORD last_print;
    int wn = s_sr_world != s_sr_world_prev, sn = s_sr_scr != s_sr_scr_prev;
    DWORD t = GetTickCount();
    st_sr_flips++;
    st_sr_world_new += wn; st_sr_scr_new += sn;
    if (wn || sn) {
        st_sr_any_new++;
        st_sr_run[s_sr_run < 7 ? s_sr_run : 7]++;   /* identical flips that followed the previous change */
        s_sr_run = 0;
        if (s_sr_last) {
            unsigned long long d = now - s_sr_last;
            st_sr_gap_sum += d; st_sr_gap_n++;
            if (d > st_sr_gap_max) st_sr_gap_max = d;
            if (!st_sr_gap_min || d < st_sr_gap_min) st_sr_gap_min = d;
        }
        s_sr_last = now;
    } else s_sr_run++;
    s_sr_world_prev = s_sr_world; s_sr_scr_prev = s_sr_scr;
    s_sr_world = s_sr_scr = SR_SEED;
    if (!last_print) last_print = t;
    if (t - last_print >= 2000) {
        unsigned long long qf = gb_qf();
        fprintf(stderr, "[SIMRATE] ms=%lu flips=%u changes=%u world=%u other=%u repeats_after_change 0:%u 1:%u 2:%u 3:%u 4:%u 5:%u 6:%u 7+:%u "
                        "gap_us avg=%u min=%u max=%u\n",
                (unsigned long)(t - last_print), st_sr_flips, st_sr_any_new, st_sr_world_new, st_sr_scr_new,
                st_sr_run[0], st_sr_run[1], st_sr_run[2], st_sr_run[3], st_sr_run[4], st_sr_run[5], st_sr_run[6], st_sr_run[7],
                st_sr_gap_n ? (unsigned)(st_sr_gap_sum * 1000000ULL / qf / st_sr_gap_n) : 0u,
                (unsigned)(st_sr_gap_min * 1000000ULL / qf), (unsigned)(st_sr_gap_max * 1000000ULL / qf));
        st_sr_flips = st_sr_world_new = st_sr_scr_new = st_sr_any_new = 0;
        memset(st_sr_run, 0, sizeof st_sr_run);
        st_sr_gap_sum = st_sr_gap_max = st_sr_gap_min = 0; st_sr_gap_n = 0;
        last_print = t;
    }
}

#define MAX_RT   8
#define MAX_TEX  768
#define VB_BYTES (8u * 1024u * 1024u)

typedef struct { uint32_t va, w, h, bpp, pitch, dirty, used, aa_sx, aa_sy;
                 ID3D11Texture2D *tex, *stage, *msaa; ID3D11RenderTargetView *rtv; ID3D11ShaderResourceView *srv; uint32_t sampled;
                 int resolve_pending;
                 /* deferred readback: the copy queued at the last flip (pstage, one of stage/stage2) and where it goes */
                 ID3D11Texture2D *stage2, *pstage; int pend; uint32_t pva, pw, ph, ppitch, pbpp;
                 /* down is the guest-sized copy written back to guest memory when the render scale is above 1 */
                 ID3D11Texture2D *down; ID3D11RenderTargetView *down_rtv; uint32_t shown_dirty;
                 /* widescreen: guest pixels added on each side of a full-screen target (0 = 4:3). The texture is
                  * (w + 2 ex) x h at the render scale; w, h and guest memory stay the game's size. */
                 uint32_t ex; } Rt;
typedef struct { uint32_t va, w, h, ex;
                 ID3D11Texture2D *tex; ID3D11DepthStencilView *dsv; } Ds;
typedef struct { uint32_t offset, w, h, color, pitch; uint64_t hash; uint32_t used;
                 uint64_t thash, sig; uint32_t vframe, redecodes, from_pack; /* texture-bytes hash, sample signature, s_frame+1 of the last full verify (get_tex fast path) */
                 ID3D11Texture2D *tex; ID3D11ShaderResourceView *srv; } TexEnt;
typedef struct { uint32_t key[12]; void *obj; } StateEnt;

typedef struct { float sw, sh, tw, th; uint32_t textured, afunc; float aref; uint32_t pad;
                 float wsq, uofs, clipz, nearz; /* wsq/uofs: widescreen (wide_wsq, wide_uv); clipz/nearz: GPU clipping (s_clip_hlsl), offsets 40/44 as in RcDrawCb */ } Cb;
/* Matches rbx_rc_hlsl.h cbuffer C (b0) for combiner draws. */
typedef struct {
    float surf[2], zscale, aref;
    uint32_t atest, afunc, textured, dbg; /* dbg: 1=V0, 2=solid, 3=T0 */
    float wsq, isz1, clipz, nearz; /* wsq: widescreen squeeze (wide_wsq); clipz/nearz: GPU clipping only (s_clip_hlsl) */
} RcDrawCb;
typedef struct {
    uint32_t hdr[4], hdr2[4];
    float fog[4], uvs[4][4];
    uint32_t stages[16][4];
} RcParams;

static ID3D11Device *s_dev;
static ID3D11DeviceContext *s_ctx;
static ID3D11VertexShader *s_vs;
static ID3D11PixelShader *s_ps;
static ID3D11InputLayout *s_il;
static ID3D11VertexShader *s_rc_vs;
static ID3D11InputLayout *s_rc_il;
/* GPU clipping (executor, default on; RECOMP_GPU_CLIP=0 off): vertex shaders that also write the near planes as clip distances,
 * and rasterizer states that cull ([front CCW][none, front, back]) with depth clip off. */
static ID3D11VertexShader *s_vs_clip, *s_rc_vs_clip;
static ID3D11RasterizerState *s_rs_clip[2][3][2];   /* [ccw][cull][scissor: Versus split] */
static int s_clip_failed;
/* GPU vertex programs (executor, default on; RECOMP_GPU_VSH=0 off): one vertex shader and input layout per known program, the
 * constant buffer ring (RbxGvsConst at b2), and for RECOMP_GPU_VSH_VERIFY the stream-out readback. */
static ID3D11VertexShader *s_gvs_vs[RBX_GVS_NPROG];
static ID3D11InputLayout *s_gvs_il[RBX_GVS_NPROG];
static ID3D11GeometryShader *s_gvs_so[RBX_GVS_NPROG];
#define GVS_CB_RING 512
static ID3D11Buffer *s_gvs_cbr[GVS_CB_RING], *s_gvs_sob, *s_gvs_sost;
static unsigned s_gvs_cb_i;
static int s_gvs_ready;
static ID3D11Buffer *s_rc_cb;
static ID3D11PixelShader *s_rc_ps;
static struct { Nv2aCombiner key; ID3D11PixelShader *shader; } s_rc_cache[128];
static unsigned s_rc_next;
#define CB_RING 1024
#define VB_RING 3
/* NHLHitz-style ext stream for RC: float V0 + d1/tex/fog (COLOR UNORM on
 * stream 0 was unread on mission VSH batches while TEXCOORD floats worked). */
typedef struct {
    float v0[4];
    float d1[4];
    float tc[4][4];
    float fog;
    float pad[3]; /* 112-byte stride (16B align); fog @96 */
} RcVertExt;
#define XVB_BYTES ((VB_BYTES / (uint32_t)sizeof(Nv2aVertex)) * (uint32_t)sizeof(RcVertExt))
static ID3D11Buffer *s_vb, *s_cb;                /* current vertex buffer / first constant buffer */
static ID3D11Buffer *s_vbr[VB_RING], *s_cbr[CB_RING];  /* rings: no buffer is DISCARDed twice within a ring cycle (Intel drivers corrupt queued draws otherwise) */
static ID3D11Buffer *s_xvb, *s_xvbr[VB_RING];
static unsigned s_vb_i, s_cb_i;
#define IB_BYTES (4u * 1024u * 1024u)
static ID3D11Buffer *s_ibr[VB_RING];   /* index ring for the executor's indexed batches */
static unsigned s_ib_i, s_ib_off;
static uint32_t s_xvb_off;
static unsigned s_vendor, s_flush_mode, s_flush_tex; /* RECOMP_GPU_FLUSH_TEX=1: flush before and after every texture (re)creation */        /* RECOMP_GPU_FLUSH: 0 none, 1 P8 512-wide glyph draws, 2 all textured draws, 3 every draw; default 0 everywhere (on an Intel UHD laptop: no corruption with 0, and 3 cost ~3.5x fps) */
static unsigned long s_defer_n; static struct { IUnknown *o; uint32_t f; } s_grave[4096];
static ID3D11RasterizerState *s_rs, *s_rs_split;
static uint32_t s_split_va;
static unsigned s_split_top, s_split_bottom;

/* Each Xbox Versus camera starts with a depth/stencil clear over its half
 * of the 640x480 back buffer. Constrain its draws to that half so the next
 * camera cannot overwrite it. Offscreen passes leave the split intact. */
void rbx_d3d11_clear_rect(uint32_t va, unsigned width, unsigned height, unsigned aa_x,
                          uint32_t flags, uint32_t horizontal, uint32_t vertical)
{
    unsigned top = vertical & 0xffffu;
    unsigned bottom = (vertical >> 16) + 1;
    if ((flags & 3u) && height == 480 && width == 640 * aa_x &&
        horizontal == 0x027f0000u && bottom == top + height / 2 &&
        (top == 0 || bottom == height)) {
        s_split_va = va;
        s_split_top = top;
        s_split_bottom = bottom;
    } else if (va == s_split_va) {
        s_split_va = 0;
    }
}
static ID3D11Texture2D *s_white;
static ID3D11ShaderResourceView *s_white_srv;
static uint32_t s_vb_off;
/* F1 options, requested from the window's thread and applied at a flip. */
static int s_scale = 1, s_resets;
static unsigned s_msaa = 1, s_msaa_mask = 1, s_aniso = 1, s_fps_limit;
static int s_cap_drop;   /* RECOMP_FPS_CAP_DROP=1: the FPS limit only drops frames on their way to the window (old) */
static ID3D11VertexShader *s_bl_vs;
static ID3D11PixelShader *s_bl_ps;
static ID3D11Buffer *s_bl_cb;
static ID3D11SamplerState *s_bl_smp;
static Rt s_rt[MAX_RT];
static Ds s_ds[4];

/* ---- widescreen ---------------------------------------------------------------
 * F1 "Widescreen" (RECOMP_RBX_WIDESCREEN): the game's full-screen surfaces (640x480 logical) become wider render
 * targets, (w + 2 ex) x h, while the game keeps drawing in its own 640x480 coordinates. Every vertex shader scales
 * clip-space x by (1 - wsq):
 *  - 3D draws are squeezed into the middle 4:3 at their own shape, and what lies left and right of the 4:3 view
 *    (geometry the game sends past the screen edge) lands in the new side areas: Hor+, the vertical view unchanged;
 *  - 2D (HUD, menus, movies, text) is squeezed the same way, so it keeps its shape in a centred 4:3 area;
 *  - a flat full-width quad that is untextured or samples a render target (fades, flashes, cinematic bars, full-
 *    screen copies and post passes) is stretched over the whole target instead (wsq = 0).
 * Squeezed draws that sample a wide target have their u remapped to the same middle area (wide_uv), so screen-
 * space effects line up. Guest memory keeps the game's size: readback squeezes the wide frame into it, and seeding
 * a target from guest memory stretches it back. The window gets the wide frame and its aspect.
 * s_wide = target aspect / (4:3); 1 = off. Changed only between frames (video_apply), which rebuilds the targets. */
static float s_wide = 1.0f;
/* For the game's frustum culling (sub_0012B670 in recomp_manual.c): how much wider than 4:3 the view is. */
float rbx_wide_factor(void) { return s_wide; }

/* Guest pixels to add each side of a surface: only the game's full-screen size gets wider. */
static uint32_t wide_extra(uint32_t w, uint32_t h, uint32_t ax, uint32_t ay)
{
    uint32_t e;
    if (s_wide <= 1.001f) return 0;
    if (!ax) ax = 1;
    if (!ay) ay = 1;
    if (w != 640u * ax || h != 480u * ay) return 0;
    /* whole logical pixels, so the centred 4:3 starts on a pixel edge */
    e = (uint32_t)(640.0f * (s_wide - 1.0f) * 0.5f + 0.5f);
    return e * ax;
}
static uint32_t rt_full_w(const Rt *rt) { return rt->w + 2u * rt->ex; }
/* The clip-space x factor's complement for a draw into rt (0 = no squeeze). */
static float wide_wsq(const Rt *rt, int stretch)
{
    if (!rt || !rt->ex || stretch) return 0.0f;
    return 1.0f - (float)rt->w / (float)rt_full_w(rt);
}
static Rt *rt_of_srv(ID3D11ShaderResourceView *v)
{
    int i;
    if (!v) return NULL;
    for (i = 0; i < MAX_RT; i++)
        if (s_rt[i].tex && s_rt[i].srv == v) return &s_rt[i];
    return NULL;
}
/* A squeezed draw that samples a wide target: normalised u -> u * *k + *o, the middle 4:3 of that target. */
static void wide_uv(ID3D11ShaderResourceView *v, float wsq, float *k, float *o)
{
    Rt *src = wsq > 0.0f ? rt_of_srv(v) : NULL;
    *k = 1.0f; *o = 0.0f;
    if (!src || !src->ex) return;
    *k = (float)src->w / (float)rt_full_w(src);
    *o = (float)src->ex / (float)rt_full_w(src);
}
/* Screen-flat (one w for every vertex) and spanning the surface's full width: a full-screen pass, not 3D. */
static int flat_full_width(const Nv2aVertex *v, uint32_t n, uint32_t w)
{
    uint32_t i;
    float lo, hi, r0;
    if (!n) return 0;
    lo = hi = v[0].x; r0 = v[0].rhw;
    for (i = 1; i < n; i++) {
        if (fabsf(v[i].rhw - r0) > 1e-6f * fabsf(r0) + 1e-12f) return 0;
        if (v[i].x < lo) lo = v[i].x;
        if (v[i].x > hi) hi = v[i].x;
    }
    return lo <= 1.0f && hi >= (float)w - 1.0f;
}
/* Every bound texture is absent or a render target (the frame itself): a full-width pass of it may stretch. */
static int only_rt_textures(ID3D11ShaderResourceView *const *v, unsigned n)
{
    unsigned i;
    for (i = 0; i < n; i++)
        if (v[i] && v[i] != s_white_srv && !rt_of_srv(v[i])) return 0;
    return 1;
}
static TexEnt s_tx[MAX_TEX];
/* Blend, depth-stencil and sampler state caches: open-addressed hash tables.
 * The old 96/48-entry arrays were searched linearly and, once full, created a
 * state on every call without storing it, so each draw with an uncached state
 * took a new D3D reference that was never released. A full table now keeps
 * only the most recent extra object (the previous one goes to the deferred
 * release queue), and the [RBXPERF] line counts these as state_spill. */
#define STATE_CAP 1024u /* power of two; filled to 3/4 at most */
typedef struct { StateEnt e[STATE_CAP]; unsigned n; void *spill; } StateTab;
static StateTab s_bst, s_dsst, s_sst;
static volatile LONG st_state_spill;
static int s_failed, s_inited, s_noalpha;
static uint32_t s_frame, s_tick;
static uint32_t st_th_fast, st_th_full, st_th_stale;
static uint32_t st_rtsamples, st_draws, st_tris, st_culled, st_decodes, st_clears, st_hash_ns_us, st_flips;
static int s_cull_on = 1, s_verbose, s_trace, s_rb_every = 8, s_trace_all, s_trace_lines, s_rb_sync;
static unsigned s_ta_lo, s_ta_hi;
static int s_ta_sig;
static uint64_t s_ta_seen[512];
static int s_ta_n;
static int ta_new(uint64_t sig)
{
    int i;
    if (!s_ta_sig) return 1;
    for (i = 0; i < s_ta_n; i++) if (s_ta_seen[i] == sig) return 0;
    if (s_ta_n < 512) s_ta_seen[s_ta_n++] = sig;
    return 1;
}

/* Copy between render targets of different sizes: a box over the source pixels each output pixel covers. */
static const char s_bl_hlsl[] =
"Texture2D t : register(t0); SamplerState s : register(s0);\n"
"cbuffer C : register(b0) { float4 src; };\n"
"struct V { float4 p : SV_Position; float2 uv : TEXCOORD0; };\n"
"V vs(uint i : SV_VertexID) { V o; float2 q = float2((i << 1) & 2, i & 2);\n"
"  o.uv = q; o.p = float4(q * float2(2, -2) + float2(-1, 1), 0, 1); return o; }\n"
"float4 ps(V i) : SV_Target {\n"
"  float2 r = max(src.zw, 1.0); int nx = (int)min(ceil(r.x), 8.0), ny = (int)min(ceil(r.y), 8.0);\n"
"  float4 sum = 0; float2 c = i.uv * src.xy;\n"
"  for (int y = 0; y < ny; y++) for (int x = 0; x < nx; x++) {\n"
"    float2 o = ((float2(x, y) + 0.5) / float2(nx, ny) - 0.5) * r; sum += t.Sample(s, (c + o) / src.xy); }\n"
"  return sum / (nx * ny); }\n";

static const char s_hlsl[] =
"cbuffer C : register(b0) { float2 surf; float2 tdim; uint textured; uint afunc; float aref; uint pad; float wsq; float uofs; };\n"
"Texture2D t0 : register(t0); SamplerState s0 : register(s0);\n"
"struct VI { float4 p : POSITION; float4 c : COLOR0; float2 uv : TEXCOORD0; };\n"
"struct VO { float4 pos : SV_Position; float4 c : COLOR0; float2 uv : TEXCOORD0; };\n"
"VO vs(VI i) {\n"
"  VO o; float w = 1.0 / max(i.p.w, 1e-20);\n"
"  float nx = i.p.x / surf.x * 2.0 - 1.0; float ny = 1.0 - i.p.y / surf.y * 2.0;\n"
"  float nz = saturate(i.p.z / 16777215.0);\n"
"  o.pos = float4(nx * (1.0 - wsq) * w, ny * w, nz * w, w); o.c = i.c; o.uv = i.uv; return o; }\n"
"float4 ps(VO i) : SV_Target {\n"
"  float4 c = i.c;\n"
"  if (textured != 0) { float4 t = t0.Sample(s0, i.uv / tdim + float2(uofs, 0.0)); if ((pad & 5) != 0) t.a = 1.0; if ((pad & 2) != 0) t.rgb = float3(1.0, 1.0, 1.0); c *= t; }\n"
"  float a = c.a * 255.0; float r = aref;\n"
"  bool ok = true;\n"
"  if (afunc == 0x200) ok = false; else if (afunc == 0x201) ok = a < r - 0.5;\n"
"  else if (afunc == 0x202) ok = abs(a - r) < 0.5; else if (afunc == 0x203) ok = a < r + 0.5;\n"
"  else if (afunc == 0x204) ok = a > r + 0.5; else if (afunc == 0x205) ok = abs(a - r) >= 0.5;\n"
"  else if (afunc == 0x206) ok = a > r - 0.5;\n"
"  if (!ok) discard;\n"
"  return c; }\n";

/* GPU clipping: the executor hands program and fixed-function batches over unclipped (default; RECOMP_GPU_CLIP=0 turns it off). Same
 * mapping as vs / vs_rc, but w keeps its sign and z is not clamped per vertex (the rasterizer clamps per pixel,
 * depth clip off), and the CPU clipper's two planes become clip distances: w >= 1e-3, and z >= clipz in screen z.
 * The inputs match s_il / s_rc_il; each output adds the clip distances after the fields its pixel shader reads. */
static const char s_clip_hlsl[] =
"cbuffer C : register(b0) { float2 surf; float zs_rc; float pad0; uint4 pad1; float wsq; float pad2; float clipz; float nearz; };\n"
"float4 clip_pos(float3 p, float rhw, float zs, out float2 cd) {\n"
"  float w = 1.0 / rhw;\n"
"  cd = float2(w - 1e-3, nearz > 0.5 ? (p.z - clipz) * w : 1.0);\n"
"  return float4((p.x / surf.x * 2.0 - 1.0) * (1.0 - wsq) * w, (1.0 - p.y / surf.y * 2.0) * w, p.z * zs * w, w); }\n"
"struct VI { float4 p : POSITION; float4 c : COLOR0; float2 uv : TEXCOORD0; };\n"
"struct VO { float4 pos : SV_Position; float4 c : COLOR0; float2 uv : TEXCOORD0; float2 cd : SV_ClipDistance0; };\n"
"VO vs_clip(VI i) { VO o; o.pos = clip_pos(i.p.xyz, i.p.w, 1.0 / 16777215.0, o.cd); o.c = i.c; o.uv = i.uv; return o; }\n"
"struct VIR { float3 p : POSITION; float rhw : RHW; float4 c : COLOR; float2 uv : TEXCOORD0;\n"
"             float4 v0 : TEXCOORD7; float4 d1 : TEXCOORD1; float4 t0 : TEXCOORD2;\n"
"             float4 t1 : TEXCOORD3; float4 t2 : TEXCOORD4; float4 t3 : TEXCOORD5;\n"
"             float fog : TEXCOORD6; };\n"
"struct VOR { float4 pos : SV_Position; float4 c : COLOR; float4 d1 : TEXCOORD1;\n"
"             float4 t0 : TEXCOORD2; float4 t1 : TEXCOORD3; float4 t2 : TEXCOORD4;\n"
"             float4 t3 : TEXCOORD5; float fog : TEXCOORD6; float2 cd : SV_ClipDistance0; };\n"
"VOR vs_clip_rc(VIR i) { VOR o; o.pos = clip_pos(i.p, i.rhw, zs_rc, o.cd);\n"
"  o.c = i.v0; o.d1 = i.d1; o.t0 = i.t0; o.t1 = i.t1; o.t2 = i.t2; o.t3 = i.t3; o.fog = i.fog; return o; }\n";

/* GPU vertex programs (patches/rbx_gvs.h): the title's known vertex programs written out in HLSL, one entry point
 * each, with the CPU path's semantics end to end: the program (nv2a_vsh_interp's rules: outputs not written keep
 * oD0.w = oD1.w = 1, oTn = 0,0,0,1, the rest 0; colours clamp; rcc clamps; rcp(0) = +inf), the executor's
 * post-fixes (w = +0 -> 1, anti-aliasing scale), backend_from_xf (diffuse packed to 8 bits, fog factor),
 * rc_vert_fix (t1 <- t0) and vs_clip_rc (clip-space mapping, near planes as clip distances). Keep in step with the
 * vsh_fast_* functions in the executor. Outputs match ps_rc's input. */
static const char s_gvs_hlsl[] =
"cbuffer C : register(b0) { float2 surf; float zs_rc; float pad0; uint4 pad1; float wsq; float pad2; float clipz; float nearz; };\n"
"cbuffer V : register(b2) { float4 c0; float4 c1; float4 c58; float4 c59; float4 cu[15]; float4 aa; float4 fogp; uint4 fogm; };\n"
"#define C(n) cu[(n) - 96]\n"
"struct VOR { float4 pos : SV_Position; float4 c : COLOR; float4 d1 : TEXCOORD1;\n"
"             float4 t0 : TEXCOORD2; float4 t1 : TEXCOORD3; float4 t2 : TEXCOORD4;\n"
"             float4 t3 : TEXCOORD5; float fog : TEXCOORD6; float2 cd : SV_ClipDistance0; };\n"
"struct X { float4 pos; float4 d0; float4 d1; float4 t0; float4 t1; float4 t2; float4 t3; float fog; };\n"
"X xdef() { X x; x.pos = 0; x.d0 = float4(0, 0, 0, 1); x.d1 = float4(0, 0, 0, 1);\n"
"  x.t0 = float4(0, 0, 0, 1); x.t1 = x.t0; x.t2 = x.t0; x.t3 = x.t0; x.fog = 0; return x; }\n"
"float dp4(float4 a, float4 b) { return ((a.x * b.x + a.y * b.y) + a.z * b.z) + a.w * b.w; }\n"
"float dp3(float4 a, float4 b) { return (a.x * b.x + a.y * b.y) + a.z * b.z; }\n"
"float4 m4(float4 v) { return float4(dp4(v, C(96)), dp4(v, C(97)), dp4(v, C(98)), dp4(v, C(99))); }\n"
"float rcc(float x) { float r = 1.0 / x; float m = min(max(abs(r), 5.42101e-20), 1.884467e19); return r < 0.0 ? -m : m; }\n"
"void epi(inout X x) { float r1 = rcc(x.pos.w); x.pos.xyz = (x.pos.xyz * c58.xyz) * r1 + c59.xyz; }\n"
"float fogf(float d) {\n"
"  float f, px = fogp.x, py = fogp.y; uint m = fogm.x;\n"
"  if (fogm.y == 0u) return 1.0;\n"
"  if (m == 0x800u || m == 0x802u) f = px + exp2(d * py * 16.0) - 1.5;\n"
"  else if (m == 0x801u || m == 0x803u) f = px + exp2(-d * d * py * py * 32.0) - 1.5;\n"
"  else f = px + d * py - 1.0;\n"
"  if (m == 0x802u || m == 0x803u || m == 0x804u) f = abs(f);\n"
"  if ((asuint(f) & 0x7FFFFFFFu) > 0x7F800000u) f = 1.0;\n"
"  return saturate(f); }\n"
"VOR fin(X x) {\n"
"  VOR o; float w;\n"
"  if (asuint(x.pos.w) == 0u) x.pos.w = 1.0;\n"
"  x.pos.xy *= aa.xy; w = x.pos.w;\n"
"  o.cd = float2(w - 1e-3, nearz > 0.5 ? (x.pos.z - clipz) * w : 1.0);\n"
"  o.pos = float4((x.pos.x / surf.x * 2.0 - 1.0) * (1.0 - wsq) * w, (1.0 - x.pos.y / surf.y * 2.0) * w, x.pos.z * zs_rc * w, w);\n"
"  o.c = floor(saturate(x.d0) * 255.0 + 0.5) / 255.0;\n"
"  o.d1 = x.d1;\n"
"  if (abs(x.t1.x) + abs(x.t1.y) + abs(x.t1.z) < 1e-6 && abs(x.t0.x) + abs(x.t0.y) + abs(x.t0.z) > 1e-6) x.t1 = x.t0;\n"
"  o.t0 = x.t0; o.t1 = x.t1; o.t2 = x.t2; o.t3 = x.t3;\n"
"  o.fog = fogf(x.fog);\n"
"  return o; }\n"
"struct I1 { float4 v0 : ATTR0; float4 v1 : ATTR1; float4 v2 : ATTR2; };\n"
"VOR vs_g1(I1 i) { X x = xdef();\n"
"  x.pos = m4(i.v0); x.d0 = saturate(C(100)); x.t0 = i.v2;\n"
"  x.fog = dp4(i.v0, C(110)) * C(106).x + C(106).y;\n"
"  x.t1.x = dp3(i.v1, -C(101)); x.t1.y = C(102).w;\n"
"  epi(x); return fin(x); }\n"
"struct I2 { float4 v0 : ATTR0; float4 v1 : ATTR1; };\n"
"VOR vs_g2(I2 i) { X x = xdef(); float4 r3 = i.v0 + i.v1 * C(101).xxxy;\n"
"  x.d0 = saturate(C(100)); x.pos = m4(r3);\n"
"  x.fog = dp4(i.v0, C(110)) * C(106).x + C(106).y;\n"
"  epi(x); return fin(x); }\n"
"struct I3 { float4 v0 : ATTR0; float4 v3 : ATTR1; float4 v4 : ATTR2; float4 v9 : ATTR3;\n"
"            float4 v10 : ATTR4; float4 v11 : ATTR5; float4 v12 : ATTR6; };\n"
"VOR vs_g3(I3 i) { X x = xdef(); float4 r1 = i.v0;\n"
"  r1.w = i.v0.w == 0.0 ? asfloat(0x7F800000u) : 1.0 / i.v0.w;\n"
"  x.d0 = saturate(i.v3); x.d1 = saturate(i.v4); x.fog = r1.w;\n"
"  x.pos = r1 * c0 + c1;\n"
"  x.t0 = i.v9; x.t1 = i.v10; x.t2 = i.v11; x.t3 = i.v12;\n"
"  return fin(x); }\n"
"struct I4 { float4 v0 : ATTR0; float4 v2 : ATTR1; float4 v4 : ATTR2; float4 v5 : ATTR3; };\n"
"VOR vs_g4(I4 i) { X x = xdef(); float4 r3 = i.v0 * C(100).y + i.v2 * C(100).x;\n"
"  x.d0 = saturate(i.v4); x.t0 = i.v5; x.pos = m4(r3);\n"
"  x.fog = dp4(r3, C(110)) * C(106).x + C(106).y;\n"
"  epi(x); return fin(x); }\n";

#define SAFE_REL(p) do { if (p) { IUnknown_Release((IUnknown *)(p)); (p) = NULL; } } while (0)
/* Texture-cache entries replaced mid-frame are released two flips later, not while draws that sampled them are still queued. */
static uint32_t s_frame_for_grave;
static void gb_defer_rel(void *o) { if (!o) return; if (s_defer_n >= sizeof(s_grave)/sizeof(s_grave[0])) { IUnknown_Release(s_grave[0].o); memmove(&s_grave[0], &s_grave[1], (s_defer_n - 1) * sizeof(s_grave[0])); s_defer_n--; } s_grave[s_defer_n].o = (IUnknown *)o; s_grave[s_defer_n].f = s_frame_for_grave; s_defer_n++; }
#define DEFER_REL(p) do { if (p) { gb_defer_rel(p); (p) = NULL; } } while (0)

/* ---- shader bytecode cache ----------------------------------------------
 * Compiling a combiner variant takes ~30 ms on the pushbuffer thread (9 per
 * Story mission run on the test bench: ~260 ms of hitches). Compiled DXBC is
 * vendor-neutral, so it is kept on disk keyed by a hash of the exact source,
 * entry point, target and flags, and later runs load it instead of compiling.
 * RECOMP_SHADER_CACHE=<dir> moves the cache (default shader_cache\ beside the
 * game); RECOMP_SHADER_CACHE=0 turns it off. A stale or damaged file is just a
 * miss: the key covers the source, and the header and DXBC tag are checked. */
static unsigned st_sh_hits;
static const char *shader_cache_dir(void)
{
    static char dir[260];
    static int state = -1;   /* -1 unknown, 0 off, 1 on */
    if (state < 0) {
        const char *e = getenv("RECOMP_SHADER_CACHE");
        state = 0;
        if (!(e && e[0] == '0' && !e[1]) &&
            snprintf(dir, sizeof dir, "%s", (e && *e) ? e : "shader_cache") < (int)sizeof dir) {
            CreateDirectoryA(dir, NULL);
            state = 1;
        }
    }
    return state ? dir : NULL;
}

static uint64_t shader_key(const void *src, SIZE_T n, LPCSTR entry, LPCSTR target, UINT f1, UINT f2)
{
    uint64_t h = 1469598103934665603ULL;
    const unsigned char *p;
    UINT fl[2]; SIZE_T i;
#define SK_MIX(b) (h = (h ^ (unsigned char)(b)) * 1099511628211ULL)
    for (p = (const unsigned char *)"rbxs1|"; *p; p++) SK_MIX(*p);
    for (p = (const unsigned char *)entry; *p; p++) SK_MIX(*p);
    SK_MIX('|');
    for (p = (const unsigned char *)target; *p; p++) SK_MIX(*p);
    SK_MIX('|');
    fl[0] = f1; fl[1] = f2;
    for (i = 0; i < sizeof fl; i++) SK_MIX(((const unsigned char *)fl)[i]);
    for (p = (const unsigned char *)src, i = 0; i < n; i++) SK_MIX(p[i]);
#undef SK_MIX
    return h;
}

typedef struct { char magic[4]; uint32_t version; uint64_t key; uint32_t size; uint32_t pad; } ShaderCacheHdr;

/* D3DCompile with the cache in front (no macros or includes are cached). */
static HRESULT rbx_compile(const void *src, SIZE_T n, LPCSTR name, const D3D_SHADER_MACRO *defs, ID3DInclude *inc,
                           LPCSTR entry, LPCSTR target, UINT f1, UINT f2, ID3DBlob **code, ID3DBlob **err)
{
    const char *dir = (defs || inc) ? NULL : shader_cache_dir();
    char path[400];
    uint64_t key = 0;
    HRESULT hr;
    FILE *f;
    if (dir) {
        key = shader_key(src, n, entry, target, f1, f2);
        snprintf(path, sizeof path, "%s\\%016llx.dxbc", dir, (unsigned long long)key);
        if ((f = fopen(path, "rb")) != NULL) {
            ShaderCacheHdr h;
            ID3DBlob *b = NULL;
            if (fread(&h, sizeof h, 1, f) == 1 && !memcmp(h.magic, "RBXS", 4) && h.version == 1 && h.key == key &&
                h.size >= 4 && h.size < (1u << 22) && SUCCEEDED(D3DCreateBlob(h.size, &b))) {
                unsigned char *d = (unsigned char *)ID3D10Blob_GetBufferPointer(b);
                if (fread(d, 1, h.size, f) == h.size && !memcmp(d, "DXBC", 4)) {
                    fclose(f);
                    *code = b;
                    if (err) *err = NULL;
                    st_sh_hits++;
                    return S_OK;
                }
                SAFE_REL(b);
            }
            fclose(f);
        }
    }
    {
        unsigned t0 = qpc_us();
        hr = D3DCompile(src, n, name, defs, inc, entry, target, f1, f2, code, err);
        st_sh_us += qpc_us() - t0; st_sh_n++; s_sh_total++;
    }
    if (dir && SUCCEEDED(hr) && *code) {
        char tmp[420];
        ShaderCacheHdr h;
        memset(&h, 0, sizeof h);
        memcpy(h.magic, "RBXS", 4); h.version = 1; h.key = key;
        h.size = (uint32_t)ID3D10Blob_GetBufferSize(*code);
        snprintf(tmp, sizeof tmp, "%s.%lu.tmp", path, (unsigned long)GetCurrentProcessId());
        if ((f = fopen(tmp, "wb")) != NULL) {
            int ok = fwrite(&h, sizeof h, 1, f) == 1 &&
                     fwrite(ID3D10Blob_GetBufferPointer(*code), 1, h.size, f) == h.size;
            ok = (fclose(f) == 0) && ok;
            if (!ok || !MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING)) DeleteFileA(tmp);
        }
    }
    return hr;
}

static int gb_init(void)
{
    D3D_FEATURE_LEVEL fl;
    ID3DBlob *vsb = NULL, *psb = NULL, *err = NULL;
    D3D11_INPUT_ELEMENT_DESC el[3] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0,  D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "COLOR",    0, DXGI_FORMAT_B8G8R8A8_UNORM,     0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,       0, 20, D3D11_INPUT_PER_VERTEX_DATA, 0 } };
    D3D11_BUFFER_DESC bd;
    D3D11_RASTERIZER_DESC rd;
    D3D11_TEXTURE2D_DESC td;
    D3D11_SUBRESOURCE_DATA sd;
    uint32_t white = 0xFFFFFFFFu;
    HRESULT hr;

    { const char *ad = getenv("RECOMP_GPU_ADAPTER");   /* "warp" = Microsoft software rasteriser, for A/B against a vendor driver */
      int warp = ad && (ad[0] == 'w' || ad[0] == 'W');
      const char *fls = getenv("RECOMP_GPU_FL");   /* "11_0" / "10_1" / "10_0": cap the feature level (driver A/B) */
      D3D_FEATURE_LEVEL want[1]; UINT nwant = 0;
      if (fls && !strcmp(fls, "11_0")) { want[0] = D3D_FEATURE_LEVEL_11_0; nwant = 1; }
      else if (fls && !strcmp(fls, "10_1")) { want[0] = D3D_FEATURE_LEVEL_10_1; nwant = 1; }
      else if (fls && !strcmp(fls, "10_0")) { want[0] = D3D_FEATURE_LEVEL_10_0; nwant = 1; }
      hr = D3D11CreateDevice(NULL, warp ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_HARDWARE, NULL, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                             nwant ? want : NULL, nwant, D3D11_SDK_VERSION, &s_dev, &fl, &s_ctx); }
    if (FAILED(hr)) { fprintf(stderr, "[RBX DX11] D3D11CreateDevice failed 0x%08lX\n", (unsigned long)hr); return 0; }
    for (unsigned i = 1; i <= 3; i++) {
        UINT color = 0, depth = 0;
        if (SUCCEEDED(ID3D11Device_CheckMultisampleQualityLevels(s_dev, DXGI_FORMAT_B8G8R8A8_UNORM, 1u << i, &color)) &&
            SUCCEEDED(ID3D11Device_CheckMultisampleQualityLevels(s_dev, DXGI_FORMAT_D24_UNORM_S8_UINT, 1u << i, &depth)) &&
            color && depth)
            s_msaa_mask |= 1u << i;
    }
    rbx_settings_msaa_supported(s_msaa_mask);
    hr = rbx_compile(s_hlsl, sizeof s_hlsl - 1, "gb", NULL, NULL, "vs", "vs_4_0", 0, 0, &vsb, &err);
    if (FAILED(hr)) { fprintf(stderr, "[RBX DX11] vs: %s\n", err ? (char *)ID3D10Blob_GetBufferPointer(err) : "?"); return 0; }
    hr = rbx_compile(s_hlsl, sizeof s_hlsl - 1, "gb", NULL, NULL, "ps", "ps_4_0", 0, 0, &psb, &err);
    if (FAILED(hr)) { fprintf(stderr, "[RBX DX11] ps: %s\n", err ? (char *)ID3D10Blob_GetBufferPointer(err) : "?"); return 0; }
    ID3D11Device_CreateVertexShader(s_dev, ID3D10Blob_GetBufferPointer(vsb), ID3D10Blob_GetBufferSize(vsb), NULL, &s_vs);
    ID3D11Device_CreatePixelShader(s_dev, ID3D10Blob_GetBufferPointer(psb), ID3D10Blob_GetBufferSize(psb), NULL, &s_ps);
    ID3D11Device_CreateInputLayout(s_dev, el, 3, ID3D10Blob_GetBufferPointer(vsb), ID3D10Blob_GetBufferSize(vsb), &s_il);
    {   /* GPU clipping's simple-path vertex shader; without it the executor keeps clipping on the CPU */
        ID3DBlob *cb = NULL, *ce = NULL;
        if (SUCCEEDED(rbx_compile(s_clip_hlsl, sizeof s_clip_hlsl - 1, "gbclip", NULL, NULL, "vs_clip", "vs_4_0", 0, 0, &cb, &ce)))
            ID3D11Device_CreateVertexShader(s_dev, ID3D10Blob_GetBufferPointer(cb), ID3D10Blob_GetBufferSize(cb), NULL, &s_vs_clip);
        else
            fprintf(stderr, "[RBX DX11] clip vs: %s\n", ce ? (char *)ID3D10Blob_GetBufferPointer(ce) : "?");
        SAFE_REL(cb); SAFE_REL(ce);
    }
    memset(&bd, 0, sizeof bd);
    bd.ByteWidth = VB_BYTES; bd.Usage = D3D11_USAGE_DYNAMIC; bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    for (unsigned vi = 0; vi < VB_RING; vi++) ID3D11Device_CreateBuffer(s_dev, &bd, NULL, &s_vbr[vi]); s_vb = s_vbr[0];
    bd.ByteWidth = IB_BYTES; bd.BindFlags = D3D11_BIND_INDEX_BUFFER;
    for (unsigned vi = 0; vi < VB_RING; vi++)
        if (FAILED(ID3D11Device_CreateBuffer(s_dev, &bd, NULL, &s_ibr[vi]))) { s_ibr[0] = NULL; break; }   /* indexed batches then fall back */
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    /* RC dual-VB ext ring (float V0 + d1/tex/fog); IL/VS still lazy in initialize_combiners. */
    bd.ByteWidth = XVB_BYTES;
    for (unsigned vi = 0; vi < VB_RING; vi++) ID3D11Device_CreateBuffer(s_dev, &bd, NULL, &s_xvbr[vi]);
    s_xvb = s_xvbr[0]; s_xvb_off = 0;
    /* Room for either simple Cb (32) or RcDrawCb (48); D3D CBs are 16-byte aligned. */
    bd.ByteWidth = sizeof(RcDrawCb); bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    for (unsigned ci = 0; ci < CB_RING; ci++) ID3D11Device_CreateBuffer(s_dev, &bd, NULL, &s_cbr[ci]); s_cb = s_cbr[0];
    memset(&rd, 0, sizeof rd);
    rd.FillMode = D3D11_FILL_SOLID; rd.CullMode = D3D11_CULL_NONE; rd.DepthClipEnable = TRUE; rd.ScissorEnable = FALSE;
    rd.MultisampleEnable = TRUE;
    ID3D11Device_CreateRasterizerState(s_dev, &rd, &s_rs);
    rd.ScissorEnable = TRUE;
    ID3D11Device_CreateRasterizerState(s_dev, &rd, &s_rs_split);
    memset(&td, 0, sizeof td);
    td.Width = td.Height = 1; td.MipLevels = td.ArraySize = 1; td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_IMMUTABLE; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    sd.pSysMem = &white; sd.SysMemPitch = 4; sd.SysMemSlicePitch = 4;
    ID3D11Device_CreateTexture2D(s_dev, &td, &sd, &s_white);
    ID3D11Device_CreateShaderResourceView(s_dev, (ID3D11Resource *)s_white, NULL, &s_white_srv);
    SAFE_REL(vsb); SAFE_REL(psb);
    if (!s_vs || !s_ps || !s_il || !s_vb || !s_cb || !s_rs || !s_white_srv) return 0;
    if (SUCCEEDED(rbx_compile(s_bl_hlsl, sizeof s_bl_hlsl - 1, "bl", NULL, NULL, "vs", "vs_4_0", 0, 0, &vsb, &err))
        && SUCCEEDED(rbx_compile(s_bl_hlsl, sizeof s_bl_hlsl - 1, "bl", NULL, NULL, "ps", "ps_4_0", 0, 0, &psb, &err))) {
        D3D11_SAMPLER_DESC sm;
        ID3D11Device_CreateVertexShader(s_dev, ID3D10Blob_GetBufferPointer(vsb), ID3D10Blob_GetBufferSize(vsb), NULL, &s_bl_vs);
        ID3D11Device_CreatePixelShader(s_dev, ID3D10Blob_GetBufferPointer(psb), ID3D10Blob_GetBufferSize(psb), NULL, &s_bl_ps);
        bd.ByteWidth = 16;
        ID3D11Device_CreateBuffer(s_dev, &bd, NULL, &s_bl_cb);
        memset(&sm, 0, sizeof sm);
        sm.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sm.AddressU = sm.AddressV = sm.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sm.MaxLOD = D3D11_FLOAT32_MAX;
        ID3D11Device_CreateSamplerState(s_dev, &sm, &s_bl_smp);
    }
    SAFE_REL(vsb); SAFE_REL(psb);
    if (!s_bl_vs || !s_bl_ps || !s_bl_cb || !s_bl_smp) { fprintf(stderr, "[RBX DX11] copy shader failed\n"); return 0; }
    {
        IDXGIDevice *dd = NULL; IDXGIAdapter *ad = NULL; DXGI_ADAPTER_DESC desc;
        if (SUCCEEDED(ID3D11Device_QueryInterface(s_dev, &IID_IDXGIDevice, (void **)&dd)) &&
            SUCCEEDED(IDXGIDevice_GetAdapter(dd, &ad)) && SUCCEEDED(IDXGIAdapter_GetDesc(ad, &desc)))
            s_vendor = desc.VendorId; { const char *fm = getenv("RECOMP_GPU_FLUSH"); s_flush_mode = fm ? (unsigned)atoi(fm) : 0u; { const char *ft = getenv("RECOMP_GPU_FLUSH_TEX"); s_flush_tex = ft ? (unsigned)atoi(ft) : 0u; } }
            fprintf(stderr, "[RBX DX11] vendor 0x%04X flush_mode %u flush_tex %u\n", s_vendor, s_flush_mode, s_flush_tex);
            fprintf(stderr, "[RBX DX11] D3D11 back end up on '%ls' (feature level 0x%X)\n", desc.Description, (unsigned)fl);
        SAFE_REL(ad); SAFE_REL(dd);
    }
    return 1;
}

static int gb_ready(void)
{
    if (s_failed) return 0;
    if (!s_inited) {
        s_inited = 1;
        s_verbose = getenv("RECOMP_GPU_STATS") != NULL; s_trace = getenv("RECOMP_GPU_TRACE") != NULL; { const char *ta = getenv("RECOMP_GPU_TRACEALL"); s_trace_all = ta != NULL; if (ta && sscanf(ta, "%u-%u", &s_ta_lo, &s_ta_hi) != 2) { s_ta_lo = 0; s_ta_hi = 0xFFFFFFFFu; s_ta_sig = (ta[0] == (char)115); } } { const char *re = getenv("RECOMP_GPU_RBEVERY"); s_rb_every = re ? atoi(re) : 8; if (s_rb_every < 0) s_rb_every = 1; if (getenv("RECOMP_GPU_SKIPRB")) s_rb_every = 0; } /* targets the GPU samples are read back every Nth flip (1 = every flip, 0 = never) */ s_rb_sync = getenv("RECOMP_GPU_SYNC_RB") != NULL; s_cap_drop = getenv("RECOMP_FPS_CAP_DROP") != NULL;
        { const char *c = getenv("RECOMP_GPU_CULL"); if (c && c[0] == '0') s_cull_on = 0; }
        if (!gb_init()) { s_failed = 1; fprintf(stderr, "[RBX DX11] init failed\n"); return 0; }
    }
    return 1;
}

/* Redcard-style NV2A register-combiner shaders (lazy; needs FL 11.0 / SM 5.0). */
static int initialize_combiners(void)
{
    ID3DBlob *v = NULL, *err = NULL;
    D3D11_BUFFER_DESC bd;
    /* Stream 0: Nv2aVertex head (pos/rhw/COLOR/uv). Stream 1: RcVertExt
     * (float V0 + d1/tex/fog) — NHLHitz dual-VB pattern so mission batches
     * actually fetch V0 (UNORM COLOR @16 alone stayed unread). */
    D3D11_INPUT_ELEMENT_DESC e[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT,       0,  0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "RHW",      0, DXGI_FORMAT_R32_FLOAT,             0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "COLOR",    0, DXGI_FORMAT_B8G8R8A8_UNORM,        0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,          0, 20, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 7, DXGI_FORMAT_R32G32B32A32_FLOAT,    1,  0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 1, DXGI_FORMAT_R32G32B32A32_FLOAT,    1, 16, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 2, DXGI_FORMAT_R32G32B32A32_FLOAT,    1, 32, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 3, DXGI_FORMAT_R32G32B32A32_FLOAT,    1, 48, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 4, DXGI_FORMAT_R32G32B32A32_FLOAT,    1, 64, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 5, DXGI_FORMAT_R32G32B32A32_FLOAT,    1, 80, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 6, DXGI_FORMAT_R32_FLOAT,             1, 96, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };
    HRESULT hr;
    unsigned vi;
    if (s_rc_vs) return 1;
    hr = rbx_compile(k_hlsl_rc, sizeof(k_hlsl_rc) - 1, "rbx_rc", NULL, NULL, "vs_rc", "vs_5_0",
                    D3DCOMPILE_ENABLE_STRICTNESS, 0, &v, &err);
    if (FAILED(hr)) {
        fprintf(stderr, "[RBX DX11] rc vs: %s\n", err ? (char *)ID3D10Blob_GetBufferPointer(err) : "?");
        SAFE_REL(err); return 0;
    }
    if (FAILED(ID3D11Device_CreateVertexShader(s_dev, ID3D10Blob_GetBufferPointer(v),
               ID3D10Blob_GetBufferSize(v), NULL, &s_rc_vs))
     || FAILED(ID3D11Device_CreateInputLayout(s_dev, e, 11, ID3D10Blob_GetBufferPointer(v),
               ID3D10Blob_GetBufferSize(v), &s_rc_il))) {
        SAFE_REL(v); SAFE_REL(s_rc_vs); SAFE_REL(s_rc_il); return 0;
    }
    SAFE_REL(v);
    {   /* GPU clipping's combiner-path vertex shader; if it fails, clipping goes back to the CPU */
        ID3DBlob *cb = NULL, *ce = NULL;
        if (SUCCEEDED(rbx_compile(s_clip_hlsl, sizeof s_clip_hlsl - 1, "gbclip", NULL, NULL, "vs_clip_rc", "vs_5_0", 0, 0, &cb, &ce)))
            ID3D11Device_CreateVertexShader(s_dev, ID3D10Blob_GetBufferPointer(cb), ID3D10Blob_GetBufferSize(cb), NULL, &s_rc_vs_clip);
        if (!s_rc_vs_clip) {
            fprintf(stderr, "[RBX DX11] clip rc vs: %s\n", ce ? (char *)ID3D10Blob_GetBufferPointer(ce) : "?");
            s_clip_failed = 1;
        }
        SAFE_REL(cb); SAFE_REL(ce);
    }
    {   /* GPU vertex programs: every program's shader and layout, or none (the executor then transforms on the CPU) */
        static const char *const entry[RBX_GVS_NPROG] = { NULL, "vs_g1", "vs_g2", "vs_g3", "vs_g4" };
        static const unsigned nin[RBX_GVS_NPROG] = { 0, 3, 2, 7, 4 };
        static const D3D11_SO_DECLARATION_ENTRY so[] = {
            { 0, "SV_Position", 0, 0, 4, 0 }, { 0, "COLOR", 0, 0, 4, 0 }, { 0, "TEXCOORD", 1, 0, 4, 0 },
            { 0, "TEXCOORD", 2, 0, 4, 0 }, { 0, "TEXCOORD", 3, 0, 4, 0 }, { 0, "TEXCOORD", 4, 0, 4, 0 },
            { 0, "TEXCOORD", 5, 0, 4, 0 }, { 0, "TEXCOORD", 6, 0, 1, 0 } };
        UINT so_stride = 29 * 4;
        int p, ok = 1, verify = getenv("RECOMP_GPU_VSH_VERIFY") != NULL;
        for (p = 1; p < RBX_GVS_NPROG && ok; p++) {
            ID3DBlob *gb = NULL, *ge = NULL;
            D3D11_INPUT_ELEMENT_DESC ie[RBX_GVS_MAXIN];
            unsigned k;
            for (k = 0; k < nin[p]; k++) {
                ie[k].SemanticName = "ATTR"; ie[k].SemanticIndex = k; ie[k].Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
                ie[k].InputSlot = 0; ie[k].AlignedByteOffset = 16 * k; ie[k].InputSlotClass = D3D11_INPUT_PER_VERTEX_DATA;
                ie[k].InstanceDataStepRate = 0;
            }
            if (FAILED(rbx_compile(s_gvs_hlsl, sizeof s_gvs_hlsl - 1, "gvs", NULL, NULL, entry[p], "vs_5_0", 0, 0, &gb, &ge))
                || FAILED(ID3D11Device_CreateVertexShader(s_dev, ID3D10Blob_GetBufferPointer(gb), ID3D10Blob_GetBufferSize(gb), NULL, &s_gvs_vs[p]))
                || FAILED(ID3D11Device_CreateInputLayout(s_dev, ie, nin[p], ID3D10Blob_GetBufferPointer(gb), ID3D10Blob_GetBufferSize(gb), &s_gvs_il[p]))) {
                fprintf(stderr, "[RBX DX11] GPU vertex program %s: %s\n", entry[p], ge ? (char *)ID3D10Blob_GetBufferPointer(ge) : "create failed");
                ok = 0;
            } else if (verify
                       && FAILED(ID3D11Device_CreateGeometryShaderWithStreamOutput(s_dev, ID3D10Blob_GetBufferPointer(gb),
                                 ID3D10Blob_GetBufferSize(gb), so, (UINT)(sizeof so / sizeof so[0]), &so_stride, 1,
                                 D3D11_SO_NO_RASTERIZED_STREAM, NULL, &s_gvs_so[p]))) {
                fprintf(stderr, "[RBX DX11] GPU vertex program %s: no stream-out, verify off\n", entry[p]);
            }
            SAFE_REL(gb); SAFE_REL(ge);
        }
        if (ok) {
            D3D11_BUFFER_DESC gd;
            unsigned i;
            memset(&gd, 0, sizeof gd);
            gd.ByteWidth = sizeof(RbxGvsConst); gd.Usage = D3D11_USAGE_DYNAMIC;
            gd.BindFlags = D3D11_BIND_CONSTANT_BUFFER; gd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            for (i = 0; i < GVS_CB_RING && ok; i++)
                if (FAILED(ID3D11Device_CreateBuffer(s_dev, &gd, NULL, &s_gvs_cbr[i]))) ok = 0;
            if (verify) {
                memset(&gd, 0, sizeof gd);
                gd.ByteWidth = RBX_GVS_VERIFY_N * so_stride; gd.Usage = D3D11_USAGE_DEFAULT;
                gd.BindFlags = D3D11_BIND_STREAM_OUTPUT;
                if (SUCCEEDED(ID3D11Device_CreateBuffer(s_dev, &gd, NULL, &s_gvs_sob))) {
                    gd.Usage = D3D11_USAGE_STAGING; gd.BindFlags = 0; gd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                    ID3D11Device_CreateBuffer(s_dev, &gd, NULL, &s_gvs_sost);
                }
            }
        }
        s_gvs_ready = ok;
        if (ok) fprintf(stderr, "[RBX DX11] GPU vertex programs ready (%d)\n", RBX_GVS_NPROG - 1);
    }
    if (!s_xvb) {
        memset(&bd, 0, sizeof bd);
        bd.ByteWidth = XVB_BYTES;
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        for (vi = 0; vi < VB_RING; vi++) {
            if (FAILED(ID3D11Device_CreateBuffer(s_dev, &bd, NULL, &s_xvbr[vi]))) {
                SAFE_REL(s_rc_vs); SAFE_REL(s_rc_il); return 0;
            }
        }
        s_xvb = s_xvbr[s_vb_i % VB_RING];
        s_xvb_off = 0;
    }
    memset(&bd, 0, sizeof bd);
    bd.ByteWidth = sizeof(RcParams);
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(ID3D11Device_CreateBuffer(s_dev, &bd, NULL, &s_rc_cb))) {
        SAFE_REL(s_rc_vs); SAFE_REL(s_rc_il); return 0;
    }
    fprintf(stderr, "[RBX DX11] register combiners enabled (dual-VB V0 float + four stages)\n");
    return 1;
}

static ID3D11PixelShader *combiner_shader(const Nv2aCombiner *c)
{
    Nv2aCombiner key = *c;
    unsigned i, n;
    size_t used;
    char prefix[2048], *source;
    ID3DBlob *blob = NULL, *err = NULL;
    HRESULT hr;

    memset(key.factor0, 0, sizeof key.factor0);
    memset(key.factor1, 0, sizeof key.factor1);
    key.final_c0 = key.final_c1 = 0;
    for (i = 0; i < 128; i++)
        if (s_rc_cache[i].shader && !memcmp(&key, &s_rc_cache[i].key, sizeof key))
            return s_rc_cache[i].shader;
    n = c->control & 255u;
    if (n > 8) n = 8;
    used = (size_t)snprintf(prefix, sizeof prefix,
        "#define SPEC 1\n#define SPEC_H0 %uu\n#define SPEC_H1 %uu\n#define SPEC_H2 %uu\n"
        "#define SPEC_H3 %uu\n#define SPEC_N %u\n#define SPEC_WORDS ",
        c->control, c->stage_program, c->final0, c->final1, n);
    for (i = 0; i < 8; i++)
        used += (size_t)snprintf(prefix + used, sizeof(prefix) - used,
            "%suint4(%uu,%uu,%uu,%uu)", i ? "," : "",
            c->color_icw[i], c->alpha_icw[i], c->color_ocw[i], c->alpha_ocw[i]);
    prefix[used++] = '\n';
    prefix[used] = 0;
    source = (char *)malloc(used + sizeof k_hlsl_rc);
    if (!source) return NULL;
    memcpy(source, prefix, used);
    memcpy(source + used, k_hlsl_rc, sizeof k_hlsl_rc);
    hr = rbx_compile(source, used + sizeof(k_hlsl_rc) - 1, "rbx_rc", NULL, NULL, "ps_rc", "ps_5_0",
                     D3DCOMPILE_ENABLE_STRICTNESS, 0, &blob, &err);
    free(source);
    if (FAILED(hr)) {
        fprintf(stderr, "[RBX DX11] rc ps: %s\n", err ? (char *)ID3D10Blob_GetBufferPointer(err) : "?");
        SAFE_REL(err); return NULL;
    }
    i = s_rc_next++ % 128;
    SAFE_REL(s_rc_cache[i].shader);
    if (FAILED(ID3D11Device_CreatePixelShader(s_dev, ID3D10Blob_GetBufferPointer(blob),
               ID3D10Blob_GetBufferSize(blob), NULL, &s_rc_cache[i].shader))) {
        SAFE_REL(blob); return NULL;
    }
    SAFE_REL(blob);
    s_rc_cache[i].key = key;
    fprintf(stderr, "[RBX DX11] compiled combiner variant %u (%u stages)\n", s_rc_next, n);
    return s_rc_cache[i].shader;
}

/* ---- render targets ---------------------------------------------------- */

/* Draw srv into rtv. Every draw sets all the state it needs, so nothing is restored but the bindings. */
static void blit(ID3D11ShaderResourceView *srv, ID3D11RenderTargetView *rtv, uint32_t dw, uint32_t dh, uint32_t sw, uint32_t sh)
{
    D3D11_MAPPED_SUBRESOURCE m;
    D3D11_VIEWPORT vp;
    ID3D11ShaderResourceView *nosrv = NULL;
    if (SUCCEEDED(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)s_bl_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        float *c = (float *)m.pData;
        c[0] = (float)sw; c[1] = (float)sh; c[2] = (float)sw / (float)dw; c[3] = (float)sh / (float)dh;
        ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)s_bl_cb, 0);
    }
    ID3D11DeviceContext_PSSetShaderResources(s_ctx, 0, 1, &nosrv);
    ID3D11DeviceContext_OMSetRenderTargets(s_ctx, 1, &rtv, NULL);
    ID3D11DeviceContext_OMSetBlendState(s_ctx, NULL, NULL, 0xFFFFFFFFu);
    ID3D11DeviceContext_OMSetDepthStencilState(s_ctx, NULL, 0);
    ID3D11DeviceContext_RSSetState(s_ctx, s_rs);
    vp.TopLeftX = vp.TopLeftY = 0; vp.Width = (float)dw; vp.Height = (float)dh; vp.MinDepth = 0; vp.MaxDepth = 1;
    ID3D11DeviceContext_RSSetViewports(s_ctx, 1, &vp);
    ID3D11DeviceContext_IASetInputLayout(s_ctx, NULL);
    ID3D11DeviceContext_IASetPrimitiveTopology(s_ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_VSSetShader(s_ctx, s_bl_vs, NULL, 0);
    ID3D11DeviceContext_PSSetShader(s_ctx, s_bl_ps, NULL, 0);
    ID3D11DeviceContext_PSSetConstantBuffers(s_ctx, 0, 1, &s_bl_cb);
    ID3D11DeviceContext_PSSetShaderResources(s_ctx, 0, 1, &srv);
    ID3D11DeviceContext_PSSetSamplers(s_ctx, 0, 1, &s_bl_smp);
    ID3D11DeviceContext_Draw(s_ctx, 3, 0);
    ID3D11DeviceContext_PSSetShaderResources(s_ctx, 0, 1, &nosrv);
    ID3D11DeviceContext_OMSetRenderTargets(s_ctx, 0, NULL, NULL);
}

static void rt_resolve(Rt *rt)
{
    if (!rt->resolve_pending) return;
    ID3D11ShaderResourceView *none[4] = {0};
    ID3D11DeviceContext_OMSetRenderTargets(s_ctx, 0, NULL, NULL);
    ID3D11DeviceContext_PSSetShaderResources(s_ctx, 0, 4, none);
    ID3D11DeviceContext_ResolveSubresource(s_ctx, (ID3D11Resource *)rt->tex, 0,
        (ID3D11Resource *)rt->msaa, 0, DXGI_FORMAT_B8G8R8A8_UNORM);
    rt->resolve_pending = 0;
}

/* What a staging copy reads: the guest-sized image of this target. */
static ID3D11Resource *rt_guest_image(Rt *rt)
{
    rt_resolve(rt);
    if (!rt->down) return (ID3D11Resource *)rt->tex;
    blit(rt->srv, rt->down_rtv, rt->w, rt->h, rt_full_w(rt) * (uint32_t)s_scale, rt->h * (uint32_t)s_scale);
    return (ID3D11Resource *)rt->down;
}

static void rt_release(Rt *r)
{
    SAFE_REL(r->rtv); SAFE_REL(r->srv); SAFE_REL(r->tex); SAFE_REL(r->stage); SAFE_REL(r->stage2);
    SAFE_REL(r->down); SAFE_REL(r->down_rtv);
    SAFE_REL(r->msaa);
    r->resolve_pending = 0;
    r->pend = 0; r->pstage = NULL; r->shown_dirty = 0; r->dirty = 0;
}

/* After a change of size, a target starts from what the game last saw in guest memory, not from nothing:
 * some are drawn once and sampled for a long time. */
static void rt_seed_from_guest(Rt *rt, const Nv2aSurface *s)
{
    extern int tex_range_ok_public(uint32_t va, uint32_t n);
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
    uint32_t bpp = s->bytes_per_pixel, y, x;
    uint32_t *px;
    D3D11_TEXTURE2D_DESC td;
    D3D11_SUBRESOURCE_DATA sd;
    ID3D11Texture2D *t = NULL;
    ID3D11ShaderResourceView *v = NULL;
    if ((bpp != 4 && bpp != 2) || s->pitch < s->width * bpp || !tex_range_ok_public(s->color_va, s->pitch * s->height)) return;
    px = (uint32_t *)malloc((size_t)s->width * s->height * 4);
    if (!px) return;
    for (y = 0; y < s->height; y++) {
        const uint8_t *row = mem + s->color_va + (size_t)y * s->pitch;
        if (bpp == 4) memcpy(px + (size_t)y * s->width, row, (size_t)s->width * 4);
        else for (x = 0; x < s->width; x++) {
            uint32_t c = ((const uint16_t *)row)[x];
            px[(size_t)y * s->width + x] = 0xFF000000u | (((c >> 11) & 31) * 255 / 31) << 16 | (((c >> 5) & 63) * 255 / 63) << 8 | ((c & 31) * 255 / 31);
        }
    }
    memset(&td, 0, sizeof td);
    td.Width = s->width; td.Height = s->height; td.MipLevels = td.ArraySize = 1; td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_IMMUTABLE; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    sd.pSysMem = px; sd.SysMemPitch = s->width * 4; sd.SysMemSlicePitch = 0;
    if (SUCCEEDED(ID3D11Device_CreateTexture2D(s_dev, &td, &sd, &t)) &&
        SUCCEEDED(ID3D11Device_CreateShaderResourceView(s_dev, (ID3D11Resource *)t, NULL, &v))) {
        blit(v, rt->rtv, rt_full_w(rt) * (uint32_t)s_scale, s->height * (uint32_t)s_scale, s->width, s->height);
        rt->resolve_pending = rt->msaa != NULL;
    }
    SAFE_REL(v); SAFE_REL(t);
    free(px);
}

static void readback_finish(Rt *rt);
static void readback(Rt *rt);

/* ---- partial clears ------------------------------------------------------
 * NV2A CLEAR_SURFACE has a per-channel colour mask (R 0x10, G 0x20, B 0x40,
 * A 0x80) and is limited to CLEAR_RECT. ClearRenderTargetView does neither, so
 * an alpha-only clear wiped the picture to the clear colour and a clear of
 * one region (a loading bar, a menu panel) wiped the whole target -- a frame
 * of solid colour, i.e. a flash. A partial clear is drawn instead: a full-
 * target triangle through a scissor, with the write mask set from the
 * clear's channels. RECOMP_GPU_CLEARFULL=1 = old behaviour. */
extern uint32_t g_rbx_clear_rect_h, g_rbx_clear_rect_v;
extern int g_rbx_clear_rect_set;
static unsigned st_clear_partial;
static ID3D11PixelShader *s_cl_ps;
static ID3D11Buffer *s_cl_cb;
static ID3D11RasterizerState *s_cl_rs;
static ID3D11BlendState *s_cl_bs[16];
static const char s_cl_hlsl[] =
"cbuffer C : register(b0) { float4 col; };\n"
"float4 ps(float4 p : SV_Position) : SV_Target { return col; }\n";

static int clear_partial_draw(Rt *rt, unsigned wmask, const float c[4], const D3D11_RECT *r)
{
    D3D11_MAPPED_SUBRESOURCE m;
    D3D11_VIEWPORT vp;
    ID3D11ShaderResourceView *nosrv = NULL;
    wmask &= 15;
    if (!s_cl_ps) {
        ID3DBlob *b = NULL, *err = NULL;
        D3D11_BUFFER_DESC bd;
        D3D11_RASTERIZER_DESC rd;
        if (FAILED(rbx_compile(s_cl_hlsl, sizeof s_cl_hlsl - 1, "cl", NULL, NULL, "ps", "ps_4_0", 0, 0, &b, &err))) {
            SAFE_REL(err); return 0;
        }
        ID3D11Device_CreatePixelShader(s_dev, ID3D10Blob_GetBufferPointer(b), ID3D10Blob_GetBufferSize(b), NULL, &s_cl_ps);
        SAFE_REL(b);
        memset(&bd, 0, sizeof bd);
        bd.ByteWidth = 16; bd.Usage = D3D11_USAGE_DYNAMIC; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        ID3D11Device_CreateBuffer(s_dev, &bd, NULL, &s_cl_cb);
        memset(&rd, 0, sizeof rd);
        rd.FillMode = D3D11_FILL_SOLID; rd.CullMode = D3D11_CULL_NONE; rd.DepthClipEnable = TRUE;
        rd.ScissorEnable = TRUE; rd.MultisampleEnable = TRUE;
        ID3D11Device_CreateRasterizerState(s_dev, &rd, &s_cl_rs);
    }
    if (!s_cl_bs[wmask]) {
        D3D11_BLEND_DESC d;
        memset(&d, 0, sizeof d);
        d.RenderTarget[0].SrcBlend = d.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
        d.RenderTarget[0].DestBlend = d.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
        d.RenderTarget[0].BlendOp = d.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        d.RenderTarget[0].RenderTargetWriteMask = (UINT8)wmask;
        ID3D11Device_CreateBlendState(s_dev, &d, &s_cl_bs[wmask]);
    }
    if (!s_cl_ps || !s_cl_cb || !s_cl_rs || !s_cl_bs[wmask] || !s_bl_vs) return 0;
    if (FAILED(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)s_cl_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return 0;
    memcpy(m.pData, c, 16);
    ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)s_cl_cb, 0);
    ID3D11DeviceContext_PSSetShaderResources(s_ctx, 0, 1, &nosrv);
    ID3D11DeviceContext_OMSetRenderTargets(s_ctx, 1, &rt->rtv, NULL);
    ID3D11DeviceContext_OMSetBlendState(s_ctx, s_cl_bs[wmask], NULL, 0xFFFFFFFFu);
    ID3D11DeviceContext_OMSetDepthStencilState(s_ctx, NULL, 0);
    ID3D11DeviceContext_RSSetState(s_ctx, s_cl_rs);
    vp.TopLeftX = vp.TopLeftY = 0; vp.MinDepth = 0; vp.MaxDepth = 1;
    vp.Width = (float)(rt_full_w(rt) * (uint32_t)s_scale); vp.Height = (float)(rt->h * (uint32_t)s_scale);
    ID3D11DeviceContext_RSSetViewports(s_ctx, 1, &vp);
    ID3D11DeviceContext_RSSetScissorRects(s_ctx, 1, r);
    ID3D11DeviceContext_IASetInputLayout(s_ctx, NULL);
    ID3D11DeviceContext_IASetPrimitiveTopology(s_ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_VSSetShader(s_ctx, s_bl_vs, NULL, 0);
    ID3D11DeviceContext_PSSetShader(s_ctx, s_cl_ps, NULL, 0);
    ID3D11DeviceContext_PSSetConstantBuffers(s_ctx, 0, 1, &s_cl_cb);
    ID3D11DeviceContext_Draw(s_ctx, 3, 0);
    ID3D11DeviceContext_OMSetRenderTargets(s_ctx, 0, NULL, NULL);
    return 1;
}
static uint32_t s_evicted[8]; static int s_evicted_n;   /* VAs of recently evicted targets */

static Rt *get_rt(const Nv2aSurface *s)
{
    int i, free_i = -1, lru = 0;
    D3D11_TEXTURE2D_DESC td;
    uint32_t w = s->width, h = s->height, ex = wide_extra(s->width, s->height, s->aa_sx, s->aa_sy);

    for (i = 0; i < MAX_RT; i++) {
        if (s_rt[i].tex && s_rt[i].va == s->color_va && s_rt[i].w == w && s_rt[i].h == h) {
            s_rt[i].used = s_tick; s_rt[i].pitch = s->pitch; s_rt[i].bpp = s->bytes_per_pixel;
            s_rt[i].aa_sx = s->aa_sx ? s->aa_sx : 1; s_rt[i].aa_sy = s->aa_sy ? s->aa_sy : 1;
            return &s_rt[i];
        }
        if (!s_rt[i].tex && free_i < 0) free_i = i;
        if (s_rt[i].tex && s_rt[i].used < s_rt[lru].used) lru = i;
    }
    if (free_i < 0) {
        free_i = lru;
        /* Write its last queued copy to guest memory first: dropping it left guest memory a frame stale, and a
         * target re-created from it (or the window's guest-memory fallback) then showed an older frame. */
        if (s_rt[free_i].pend) readback_finish(&s_rt[free_i]);
        /* A sampled target whose readback was deferred (RECOMP_GPU_RBEVERY) still holds newer pixels than guest
         * memory: write them now, once, so a re-created target seeds from the right image. */
        if (s_rt[free_i].dirty && s_rt[free_i].stage && s_rb_every != 0) { readback(&s_rt[free_i]); st_rb_evict++; }
        if (s_evicted_n < 8) s_evicted[s_evicted_n++] = s_rt[free_i].va;
        else { memmove(s_evicted, s_evicted + 1, 7 * sizeof s_evicted[0]); s_evicted[7] = s_rt[free_i].va; }
        rt_release(&s_rt[free_i]);
        st_rt_evict++;
    }
    s_rt[free_i].pend = 0; s_rt[free_i].pstage = NULL;
    memset(&td, 0, sizeof td);
    td.Width = (w + 2u * ex) * (uint32_t)s_scale; td.Height = h * (uint32_t)s_scale; td.MipLevels = td.ArraySize = 1; td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    s_rt[free_i].ex = ex;
    if (FAILED(ID3D11Device_CreateTexture2D(s_dev, &td, NULL, &s_rt[free_i].tex))) { rt_release(&s_rt[free_i]); return NULL; }
    ID3D11Device_CreateShaderResourceView(s_dev, (ID3D11Resource *)s_rt[free_i].tex, NULL, &s_rt[free_i].srv);
    if (s_msaa > 1) {
        td.SampleDesc.Count = s_msaa;
        td.BindFlags = D3D11_BIND_RENDER_TARGET;
        if (FAILED(ID3D11Device_CreateTexture2D(s_dev, &td, NULL, &s_rt[free_i].msaa))) {
            rt_release(&s_rt[free_i]);
            return NULL;
        }
        td.SampleDesc.Count = 1;
    }
    if (FAILED(ID3D11Device_CreateRenderTargetView(s_dev,
            (ID3D11Resource *)(s_rt[free_i].msaa ? s_rt[free_i].msaa : s_rt[free_i].tex),
            NULL, &s_rt[free_i].rtv))) {
        rt_release(&s_rt[free_i]);
        return NULL;
    }
    td.Width = w; td.Height = h;
    if (s_scale > 1 || ex) {   /* the guest-sized copy for readback: smaller, or a wide target squeezed to the game's size */
        td.BindFlags = D3D11_BIND_RENDER_TARGET;
        if (FAILED(ID3D11Device_CreateTexture2D(s_dev, &td, NULL, &s_rt[free_i].down))) { rt_release(&s_rt[free_i]); return NULL; }
        ID3D11Device_CreateRenderTargetView(s_dev, (ID3D11Resource *)s_rt[free_i].down, NULL, &s_rt[free_i].down_rtv);
    }
    s_rt[free_i].sampled = 0;
    td.Usage = D3D11_USAGE_STAGING; td.BindFlags = 0; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Device_CreateTexture2D(s_dev, &td, NULL, &s_rt[free_i].stage);
    if (!s_rb_sync) ID3D11Device_CreateTexture2D(s_dev, &td, NULL, &s_rt[free_i].stage2);
    s_rt[free_i].va = s->color_va; s_rt[free_i].w = w; s_rt[free_i].h = h;
    s_rt[free_i].pitch = s->pitch; s_rt[free_i].bpp = s->bytes_per_pixel; s_rt[free_i].used = s_tick; s_rt[free_i].dirty = 0;
    s_rt[free_i].aa_sx = s->aa_sx ? s->aa_sx : 1; s_rt[free_i].aa_sy = s->aa_sy ? s->aa_sy : 1;
    s_rt[free_i].shown_dirty = 0;
    st_rt_new++;
    {
        /* A new target must not start black when the game already has an image there (menus and loading screens
         * change the clip on the same buffer, which is a new target here; a target evicted from the 8 slots comes
         * back). Black for one frame is a flash. RECOMP_GPU_RTSEED=0 = old behaviour (seed only after a reset). */
        static int seed = -1;
        int j, alias = -1, was_evicted = 0;
        if (seed < 0) { const char *e = getenv("RECOMP_GPU_RTSEED"); seed = !(e && e[0] == '0'); }
        for (j = 0; j < MAX_RT; j++)
            if (j != free_i && s_rt[j].tex && s_rt[j].va == s->color_va && s_rt[j].bpp == s->bytes_per_pixel &&
                (alias < 0 || s_rt[j].used > s_rt[alias].used))
                alias = j;
        for (j = 0; j < s_evicted_n; j++)
            if (s_evicted[j] == s->color_va) was_evicted = 1;
        if (alias >= 0) st_rt_alias++;
        if (s_resets) rt_seed_from_guest(&s_rt[free_i], s);
        else if (seed && alias >= 0 && !s_rt[free_i].msaa && s_rt[alias].ex == ex) {
            /* Same pixels, 1:1, over the overlap (both are at the current render scale). */
            D3D11_BOX box;
            rt_resolve(&s_rt[alias]);
            box.left = box.top = box.front = 0; box.back = 1;
            box.right = (rt_full_w(&s_rt[alias]) < w + 2u * ex ? rt_full_w(&s_rt[alias]) : w + 2u * ex) * (uint32_t)s_scale;
            box.bottom = (s_rt[alias].h < h ? s_rt[alias].h : h) * (uint32_t)s_scale;
            ID3D11DeviceContext_CopySubresourceRegion(s_ctx, (ID3D11Resource *)s_rt[free_i].tex, 0, 0, 0, 0,
                                                      (ID3D11Resource *)s_rt[alias].tex, 0, &box);
        } else if (seed && (alias >= 0 || was_evicted))
            rt_seed_from_guest(&s_rt[free_i], s);
    }
    if (s_verbose || getenv("RECOMP_GPU_LOGRT"))
        fprintf(stderr, "[RBX DX11] render target va=%08X %ux%u bpp=%u pitch=%u aa=%ux%u wide+%u\n", s->color_va, w, h,
                s->bytes_per_pixel, s->pitch, s->aa_sx, s->aa_sy, ex);
    return &s_rt[free_i];
}

static Ds *get_ds(const Nv2aSurface *s, uint32_t zva)
{
    int i, slot = -1;
    D3D11_TEXTURE2D_DESC td;
    uint32_t ex = wide_extra(s->width, s->height, s->aa_sx, s->aa_sy);
    if (!zva) return NULL;
    for (i = 0; i < 4; i++)
        if (s_ds[i].tex && s_ds[i].va == zva && s_ds[i].w == s->width && s_ds[i].h == s->height && s_ds[i].ex == ex) return &s_ds[i];
    for (i = 0; i < 4; i++) if (!s_ds[i].tex) { slot = i; break; }
    if (slot < 0) { slot = (int)(s_tick & 3); SAFE_REL(s_ds[slot].dsv); SAFE_REL(s_ds[slot].tex); }
    memset(&td, 0, sizeof td);
    td.Width = (s->width + 2u * ex) * (uint32_t)s_scale; td.Height = s->height * (uint32_t)s_scale; td.MipLevels = td.ArraySize = 1;
    td.Format = DXGI_FORMAT_D24_UNORM_S8_UINT; td.SampleDesc.Count = s_msaa; td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    if (FAILED(ID3D11Device_CreateTexture2D(s_dev, &td, NULL, &s_ds[slot].tex))) return NULL;
    ID3D11Device_CreateDepthStencilView(s_dev, (ID3D11Resource *)s_ds[slot].tex, NULL, &s_ds[slot].dsv);
    s_ds[slot].va = zva; s_ds[slot].w = s->width; s_ds[slot].h = s->height; s_ds[slot].ex = ex;
    return &s_ds[slot];
}

/* ---- sprite layer ------------------------------------------------------------
 * With the F1 sprite filter on, a run of character-sprite draws goes to its own transparent layer at 384x224, where one
 * layer pixel is one sprite pixel and tiles have joined up into poses: the game stretches this art to 640x480, 5/3 across
 * and 15/7 down, and at that size a filter only sees uneven blocks. Any other draw, a clear or a flip ends the run: the layer is filtered
 * and laid over the picture. Depth is kept among the sprites only: the game draws them after the stage and over it. */
static int s_tex_from_pack;
static int s_sf, s_smooth, s_sharpen, s_sf_dirty;
static Rt *s_sf_rt;
static ID3D11Texture2D *s_sf_tex, *s_sf_dtex;
static ID3D11RenderTargetView *s_sf_rtv;
static ID3D11ShaderResourceView *s_sf_srv;
static ID3D11DepthStencilView *s_sf_dsv;
static ID3D11BlendState *s_sf_blend;
static uint32_t s_sf_w, s_sf_h;
#define SF_W 384u
#define SF_H 224u

static int sf_layer_ready(uint32_t w, uint32_t h)
{
    D3D11_TEXTURE2D_DESC td;
    if (s_sf_tex && s_sf_w == w && s_sf_h == h) return 1;
    SAFE_REL(s_sf_srv); SAFE_REL(s_sf_rtv); SAFE_REL(s_sf_tex); SAFE_REL(s_sf_dsv); SAFE_REL(s_sf_dtex);
    if (!sprite_filter_init(s_dev, s_ctx)) return 0;
    if (!s_sf_blend) {
        D3D11_BLEND_DESC d;
        memset(&d, 0, sizeof d);
        d.RenderTarget[0].BlendEnable = TRUE;
        d.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA; d.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
        d.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE; d.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
        d.RenderTarget[0].BlendOp = d.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        d.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        if (FAILED(ID3D11Device_CreateBlendState(s_dev, &d, &s_sf_blend))) return 0;
    }
    memset(&td, 0, sizeof td);
    td.Width = w; td.Height = h; td.MipLevels = td.ArraySize = 1; td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(ID3D11Device_CreateTexture2D(s_dev, &td, NULL, &s_sf_tex))) return 0;
    ID3D11Device_CreateRenderTargetView(s_dev, (ID3D11Resource *)s_sf_tex, NULL, &s_sf_rtv);
    ID3D11Device_CreateShaderResourceView(s_dev, (ID3D11Resource *)s_sf_tex, NULL, &s_sf_srv);
    td.Format = DXGI_FORMAT_D24_UNORM_S8_UINT; td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    if (FAILED(ID3D11Device_CreateTexture2D(s_dev, &td, NULL, &s_sf_dtex))) return 0;
    ID3D11Device_CreateDepthStencilView(s_dev, (ID3D11Resource *)s_sf_dtex, NULL, &s_sf_dsv);
    s_sf_w = w; s_sf_h = h;
    return s_sf_rtv && s_sf_srv && s_sf_dsv;
}

/* Is this batch drawn at the sprite scale? A sheet also holds bar and effect parts drawn at other sizes, which would lose
 * detail in the layer: they stay on the picture. */
static int sf_at_sprite_scale(const Nv2aBatch *b)
{
    uint32_t i, j, n = b->count < 6 ? b->count : 6;
    int seen = 0;
    for (i = 0; i < n; i++)
        for (j = i + 1; j < n; j++) {
            float dx = fabsf(b->vertices[i].x - b->vertices[j].x), dy = fabsf(b->vertices[i].y - b->vertices[j].y);
            float du = fabsf(b->vertices[i].u - b->vertices[j].u), dv = fabsf(b->vertices[i].v - b->vertices[j].v);
            if (du > 1.0f && dx > 1.0f) { float r = dx / du; if (r < 1.60f || r > 1.73f) return 0; seen = 1; }
            if (dv > 1.0f && dy > 1.0f) { float r = dy / dv; if (r < 2.07f || r > 2.21f) return 0; seen = 1; }
        }
    return seen;
}

/* End a run of sprites: filter the layer and lay it over the target it belongs to. */
static void sf_flush(void)
{
    if (!s_sf_dirty) return;
    s_sf_dirty = 0;
    if (s_sf_rt && s_sf_rt->tex)
        sprite_filter_run(s_sf_srv, s_sf_w, s_sf_h, s_sf_rt->rtv, rt_full_w(s_sf_rt) * (uint32_t)s_scale, s_sf_rt->h * (uint32_t)s_scale, s_sf, s_smooth, s_sharpen);
}

/* ---- state objects ------------------------------------------------------ */

static D3D11_BLEND blend_factor(uint32_t f, int alpha)
{
    switch (f) {
    case 0: return D3D11_BLEND_ZERO;
    case 1: return D3D11_BLEND_ONE;
    case 0x300: return alpha ? D3D11_BLEND_SRC_ALPHA : D3D11_BLEND_SRC_COLOR;
    case 0x301: return alpha ? D3D11_BLEND_INV_SRC_ALPHA : D3D11_BLEND_INV_SRC_COLOR;
    case 0x302: return D3D11_BLEND_SRC_ALPHA;
    case 0x303: return D3D11_BLEND_INV_SRC_ALPHA;
    case 0x304: return D3D11_BLEND_DEST_ALPHA;
    case 0x305: return D3D11_BLEND_INV_DEST_ALPHA;
    case 0x306: return alpha ? D3D11_BLEND_DEST_ALPHA : D3D11_BLEND_DEST_COLOR;
    case 0x307: return alpha ? D3D11_BLEND_INV_DEST_ALPHA : D3D11_BLEND_INV_DEST_COLOR;
    case 0x308: return D3D11_BLEND_SRC_ALPHA_SAT;
    case 0x8001: return alpha ? D3D11_BLEND_BLEND_FACTOR : D3D11_BLEND_BLEND_FACTOR;
    case 0x8002: return D3D11_BLEND_INV_BLEND_FACTOR;
    case 0x8003: return D3D11_BLEND_BLEND_FACTOR;
    case 0x8004: return D3D11_BLEND_INV_BLEND_FACTOR;
    }
    return D3D11_BLEND_ONE;
}

static D3D11_BLEND_OP blend_op(uint32_t e)
{
    switch (e) {
    case 0x8007: return D3D11_BLEND_OP_MIN;
    case 0x8008: return D3D11_BLEND_OP_MAX;
    case 0x800A: return D3D11_BLEND_OP_SUBTRACT;
    case 0x800B: return D3D11_BLEND_OP_REV_SUBTRACT;
    }
    return D3D11_BLEND_OP_ADD;
}

static D3D11_COMPARISON_FUNC cmp_func(uint32_t f)
{
    switch (f) {
    case 0x200: return D3D11_COMPARISON_NEVER;
    case 0x201: return D3D11_COMPARISON_LESS;
    case 0x202: return D3D11_COMPARISON_EQUAL;
    case 0x203: return D3D11_COMPARISON_LESS_EQUAL;
    case 0x204: return D3D11_COMPARISON_GREATER;
    case 0x205: return D3D11_COMPARISON_NOT_EQUAL;
    case 0x206: return D3D11_COMPARISON_GREATER_EQUAL;
    }
    return D3D11_COMPARISON_ALWAYS;
}

static D3D11_STENCIL_OP sten_op(uint32_t o)
{
    switch (o) {
    case 0: return D3D11_STENCIL_OP_ZERO;
    case 0x1E01: return D3D11_STENCIL_OP_REPLACE;
    case 0x1E02: return D3D11_STENCIL_OP_INCR_SAT;
    case 0x1E03: return D3D11_STENCIL_OP_DECR_SAT;
    case 0x150A: return D3D11_STENCIL_OP_INVERT;
    case 0x8507: return D3D11_STENCIL_OP_INCR;
    case 0x8508: return D3D11_STENCIL_OP_DECR;
    }
    return D3D11_STENCIL_OP_KEEP;
}

static unsigned state_slot(const StateTab *t, const uint32_t *key)
{
    uint32_t h = 2166136261u;
    unsigned i, j;
    for (i = 0; i < 12; i++) { h ^= key[i]; h *= 16777619u; h ^= h >> 15; }
    for (j = h & (STATE_CAP - 1); ; j = (j + 1) & (STATE_CAP - 1))
        if (!t->e[j].obj || !memcmp(t->e[j].key, key, 12 * 4)) return j;
}
static void *find_state(StateTab *t, const uint32_t *key)
{
    return t->e[state_slot(t, key)].obj;
}
static void store_state(StateTab *t, const uint32_t *key, void *obj)
{
    if (t->n < STATE_CAP / 4 * 3) {
        StateEnt *e = &t->e[state_slot(t, key)];
        memcpy(e->key, key, 12 * 4);
        e->obj = obj;
        t->n++;
        return;
    }
    if (InterlockedIncrement(&st_state_spill) == 1)
        fprintf(stderr, "[RBX DX11] state cache full (%u entries); extra states are no longer cached\n", t->n);
    if (t->spill) gb_defer_rel(t->spill);
    t->spill = obj;
}
static void clear_states(StateTab *t)
{
    unsigned i;
    for (i = 0; i < STATE_CAP; i++) if (t->e[i].obj) gb_defer_rel(t->e[i].obj);
    if (t->spill) gb_defer_rel(t->spill);
    memset(t, 0, sizeof *t);
}

static ID3D11BlendState *get_blend(const Nv2aRenderState *r)
{
    uint32_t key[12] = { r->blend_enable ? 1 : 0, r->blend_src, r->blend_dst, r->blend_eq, r->color_mask };
    ID3D11BlendState *b = (ID3D11BlendState *)find_state(&s_bst, key);
    D3D11_BLEND_DESC d;
    if (b) return b;
    memset(&d, 0, sizeof d);
    d.RenderTarget[0].BlendEnable = r->blend_enable ? TRUE : FALSE;
    d.RenderTarget[0].SrcBlend = blend_factor(r->blend_src, 0);
    d.RenderTarget[0].DestBlend = blend_factor(r->blend_dst, 0);
    d.RenderTarget[0].BlendOp = blend_op(r->blend_eq);
    d.RenderTarget[0].SrcBlendAlpha = blend_factor(r->blend_src, 1);
    d.RenderTarget[0].DestBlendAlpha = blend_factor(r->blend_dst, 1);
    d.RenderTarget[0].BlendOpAlpha = blend_op(r->blend_eq);
    /* Presentation treats the target's alpha as opacity, so alpha blending must not write it (stray alpha showed as white panels). */
    { static int ka = -1; if (ka < 0) ka = getenv("RECOMP_GPU_RAWALPHA") == NULL;
      if (ka) { d.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ZERO; d.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE; d.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD; } }
    d.RenderTarget[0].RenderTargetWriteMask =
        (uint8_t)(((r->color_mask & 0x00010000) ? 1 : 0) | ((r->color_mask & 0x100) ? 2 : 0) |
                  ((r->color_mask & 1) ? 4 : 0) | ((r->color_mask & 0x01000000) ? 8 : 0));
    if (FAILED(ID3D11Device_CreateBlendState(s_dev, &d, &b))) return NULL;
    store_state(&s_bst, key, b);
    return b;
}

static ID3D11DepthStencilState *get_dss(const Nv2aRenderState *r, int have_ds)
{
    uint32_t key[12] = { have_ds, r->depth_test_enable ? 1 : 0, r->depth_func, r->depth_write ? 1 : 0,
                         r->stencil_enable ? 1 : 0, r->stencil_func, r->stencil_func_mask & 0xFF,
                         r->stencil_write_mask & 0xFF, r->stencil_fail, r->stencil_zfail, r->stencil_zpass,
                         (r->blend_enable && r->blend_src == 0x302 && r->blend_dst == 0x303) ? 1u : 0u };
    ID3D11DepthStencilState *d = (ID3D11DepthStencilState *)find_state(&s_dsst, key);
    D3D11_DEPTH_STENCIL_DESC dd;
    D3D11_DEPTH_STENCILOP_DESC op;
    if (d) return d;
    memset(&dd, 0, sizeof dd);
    dd.DepthEnable = (have_ds && r->depth_test_enable) ? TRUE : FALSE;
    dd.DepthWriteMask = r->depth_write ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
    {   /* RECOMP_GPU_BLEND_NOZWRITE=1 (experiment): alpha-blended draws (SRC_ALPHA / INV_SRC_ALPHA) do not write depth,
         * as the Dreamcast's translucent list never does. Ryu's Shinku Hadoken: 12 overlapping segment sprites. */
        static int nz = -1;
        if (nz < 0) nz = getenv("RECOMP_GPU_BLEND_NOZWRITE") != NULL;
        if (nz && r->blend_enable && r->blend_src == 0x302 && r->blend_dst == 0x303) dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    }
    dd.DepthFunc = cmp_func(r->depth_func);
    dd.StencilEnable = (have_ds && r->stencil_enable) ? TRUE : FALSE;
    dd.StencilReadMask = (UINT8)(r->stencil_func_mask & 0xFF);
    dd.StencilWriteMask = (UINT8)(r->stencil_write_mask & 0xFF);
    op.StencilFailOp = sten_op(r->stencil_fail);
    op.StencilDepthFailOp = sten_op(r->stencil_zfail);
    op.StencilPassOp = sten_op(r->stencil_zpass);
    op.StencilFunc = cmp_func(r->stencil_func);
    dd.FrontFace = op; dd.BackFace = op;
    if (FAILED(ID3D11Device_CreateDepthStencilState(s_dev, &dd, &d))) return NULL;
    store_state(&s_dsst, key, d);
    return d;
}

static D3D11_TEXTURE_ADDRESS_MODE addr_mode(uint32_t m)
{
    switch (m) {
    case 2: return D3D11_TEXTURE_ADDRESS_MIRROR;
    case 3: return D3D11_TEXTURE_ADDRESS_CLAMP;
    case 4: return D3D11_TEXTURE_ADDRESS_BORDER;
    case 5: return D3D11_TEXTURE_ADDRESS_CLAMP;
    }
    return D3D11_TEXTURE_ADDRESS_WRAP;
}

static ID3D11SamplerState *get_sampler(const Nv2aTexture *t)
{
    uint32_t minf = (t->filter >> 16) & 0xFF, magf = (t->filter >> 24) & 0xF;
    uint32_t key[12] = { minf == 1 ? 1u : 0u, magf == 1 ? 1u : 0u, t->addr_u, t->addr_v, s_aniso };
    ID3D11SamplerState *s = (ID3D11SamplerState *)find_state(&s_sst, key);
    D3D11_SAMPLER_DESC d;
    if (s) return s;
    memset(&d, 0, sizeof d);
    if (key[0] && key[1]) d.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    else if (key[0]) d.Filter = D3D11_FILTER_MIN_POINT_MAG_MIP_LINEAR;
    else if (key[1]) d.Filter = D3D11_FILTER_MIN_LINEAR_MAG_POINT_MIP_LINEAR;
    else d.Filter = D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
    if (!key[0] && !key[1] && s_aniso > 1) d.Filter = D3D11_FILTER_ANISOTROPIC;
    d.AddressU = addr_mode(t->addr_u); d.AddressV = addr_mode(t->addr_v); d.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    d.MaxLOD = D3D11_FLOAT32_MAX;
    d.MaxAnisotropy = d.Filter == D3D11_FILTER_ANISOTROPIC ? s_aniso : 1;
    d.ComparisonFunc = D3D11_COMPARISON_NEVER;
    if (FAILED(ID3D11Device_CreateSamplerState(s_dev, &d, &s))) return NULL;
    store_state(&s_sst, key, s);
    return s;
}

/* ---- textures ----------------------------------------------------------- */

static uint32_t tex_bytes(const Nv2aTexture *t)
{
    uint32_t px = t->width * t->height;
    switch (t->color) {
    case 0x0C: return px / 2 < 8 ? 8 : px / 2;                 /* DXT1 */
    case 0x0E: case 0x0F: return px < 16 ? 16 : px;            /* DXT3/5 */
    case 0x00: case 0x01: case 0x0B: case 0x19: return px;
    case 0x02: case 0x03: case 0x04: case 0x05: case 0x10: case 0x11: case 0x1A: case 0x1B: return px * 2;
    }
    return px * 4;
}

static uint64_t hash_bytes(const uint8_t *p, uint32_t n)
{
    /* Four independent multiply lanes: the old single FNV-style chain ran at ~2 GB/s and was
     * ~10% of the executor thread; this is limited by memory bandwidth instead. */
    const uint64_t K = 0x9E3779B97F4A7C15ull;
    uint64_t a = 0x243F6A8885A308D3ull, b = 0x13198A2E03707344ull, c = 0xA4093822299F31D0ull, d = 0x082EFA98EC4E6C89ull;
    const uint64_t *w = (const uint64_t *)p;
    uint32_t i, q = n / 32;
    for (i = 0; i < q; i++, w += 4) {
        a = (a ^ w[0]) * K; a ^= a >> 29;
        b = (b ^ w[1]) * K; b ^= b >> 29;
        c = (c ^ w[2]) * K; c ^= c >> 29;
        d = (d ^ w[3]) * K; d ^= d >> 29;
    }
    for (i = q * 32; i < n; i++) { a = (a ^ p[i]) * K; a ^= a >> 29; }
    a ^= (b << 17) | (b >> 47); a *= K;
    a ^= (c << 31) | (c >> 33); a *= K;
    a ^= (d << 7) | (d >> 57); a *= K;
    return a ^ (a >> 32);
}
static unsigned st_dec_us, st_crt_us;
static unsigned qpc_us(void);
/* Cheap content signature. Textures up to 32 KB (portraits, small P8 sheets: changed mid-frame in practice) get the full hash;
 * bigger ones: n/1024 spread words (64..512) + the last word, i.e. one word per KB, so any rewritten run >= 1 KB is seen. */
static uint64_t sample_sig(const uint8_t *p, uint32_t n)
{
    const uint64_t K = 0x9E3779B97F4A7C15ull;
    const uint64_t *w = (const uint64_t *)p;
    uint32_t nw = n / 8, ns, step, i;
    uint64_t a = 0x452821E638D01377ull;
    if (n <= 32768) return hash_bytes(p, n);
    ns = n / 1024; if (ns < 64) ns = 64; if (ns > 512) ns = 512;
    step = nw / ns;
    for (i = 0; i < ns; i++) { a = (a ^ w[(size_t)i * step]) * K; a ^= a >> 29; }
    a = (a ^ w[nw - 1]) * K; a ^= a >> 29;
    return a;
}
/* A texture read must land in mapped guest memory. A garbage offset (seen once during a stage load: 0xEED36461) faults here,
 * where the real GPU would only sample rubbish for a frame. Windows: 64 MB of RAM, its contiguous and tiled views. */
static int tex_range_ok(uint32_t va, uint32_t n)
{
    static const uint32_t base[3] = { 0x00000000u, 0x80000000u, 0xF0000000u };
    const uint32_t size = 0x04000000u;
    int i;
    for (i = 0; i < 3; i++)
        if (va - base[i] < size && n <= size - (va - base[i])) return 1;
    return 0;
}
int tex_range_ok_public(uint32_t va, uint32_t n) { return tex_range_ok(va, n); }
static int tex_ok(const Nv2aTexture *t)
{
    static LONG nlog;
    if (t->width && t->width <= 4096 && t->height && t->height <= 4096 && tex_range_ok(t->offset, tex_bytes(t))
        && (t->color != 0x0B || !t->palette || tex_range_ok(t->palette, 1024))) return 1;
    if (InterlockedIncrement(&nlog) <= 20)
        fprintf(stderr, "[RBX DX11] bad texture skipped: offset %08X palette %08X %ux%u fmt %02X\n", t->offset, t->palette, t->width, t->height, t->color);
    return 0;
}
/* ---- hi-res texture pack ---------------------------------------------------
 * <16 hex digits of pixel_hash>_<16 hex digits of alpha_hash>*.dds or .png in the installation's texture_pack folder
 * next to the exe (subfolders allowed) replace the decoded texture of that hash, any size. The hash covers the decoded
 * colours only, so a pack can be made from the disc without running the game. The game can draw the same colours with
 * other alpha than the disc holds, so a replacement whose alpha_hash differs is not used: the original is drawn instead. The pack only holds stages and menus:
 * character sprites are packed into sheets that change every frame, so no hash of theirs ever repeats. */
typedef struct { uint64_t hash, alpha; char path[MAX_PATH]; } PackEnt;
static PackEnt *s_pack;
static int s_npack, s_pack_cap, s_pack_scanned;
static int s_hires = 1;

static void pack_scan(const char *dir, int depth)
{
    char pat[MAX_PATH], sub[MAX_PATH];
    WIN32_FIND_DATAA fd;
    HANDLE h;
    if (snprintf(pat, sizeof pat, "%s\\*", dir) >= (int)sizeof pat) return;
    if ((h = FindFirstFileA(pat, &fd)) == INVALID_HANDLE_VALUE) return;
    do {
        size_t n = strlen(fd.cFileName);
        if (fd.cFileName[0] == '.') continue;
        if (snprintf(sub, sizeof sub, "%s\\%s", dir, fd.cFileName) >= (int)sizeof sub) continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) { if (depth < 4) pack_scan(sub, depth + 1); continue; }
        if (n >= 37 && (!_stricmp(fd.cFileName + n - 4, ".png") || !_stricmp(fd.cFileName + n - 4, ".dds"))
            && strspn(fd.cFileName, "0123456789abcdefABCDEF") == 16 && fd.cFileName[16] == '_'
            && strspn(fd.cFileName + 17, "0123456789abcdefABCDEF") == 16) {
            char hex[17];
            if (s_npack == s_pack_cap) {
                int cap = s_pack_cap ? s_pack_cap * 2 : 1024;
                PackEnt *p = (PackEnt *)realloc(s_pack, (size_t)cap * sizeof *p);
                if (!p) break;
                s_pack = p; s_pack_cap = cap;
            }
            memcpy(hex, fd.cFileName, 16); hex[16] = 0;
            s_pack[s_npack].hash = strtoull(hex, NULL, 16);
            memcpy(hex, fd.cFileName + 17, 16);
            s_pack[s_npack].alpha = strtoull(hex, NULL, 16);
            strcpy(s_pack[s_npack].path, sub);
            s_npack++;
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

static int pack_cmp(const void *a, const void *b)
{
    const PackEnt *left = a, *right = b;
    if (left->hash != right->hash) return left->hash < right->hash ? -1 : 1;
    if (left->alpha != right->alpha) return left->alpha < right->alpha ? -1 : 1;
    /* Prefer the DDS when both files describe the same texture. */
    {
        const char *p = ((const PackEnt *)a)->path, *q = ((const PackEnt *)b)->path;
        int pd = !_stricmp(p + strlen(p) - 4, ".dds"), qd = !_stricmp(q + strlen(q) - 4, ".dds");
        return qd - pd;
    }
}

int rbx_pack_count(void)
{
    if (!s_pack_scanned) {
        char dir[MAX_PATH], working_pack[MAX_PATH], *sl;
        s_pack_scanned = 1;
        DWORD len = GetFullPathNameA("texture_pack", MAX_PATH, working_pack, NULL);
        pack_scan("texture_pack", 0);
        if (GetModuleFileNameA(NULL, dir, MAX_PATH) && (sl = strrchr(dir, '\\')) != NULL && (size_t)(sl - dir) < MAX_PATH - 20) {
            strcpy(sl, "\\texture_pack");
            if (!len || len >= MAX_PATH || _stricmp(dir, working_pack))
                pack_scan(dir, 0);
        }
        if (s_npack) {
            int r, wr = 1;
            qsort(s_pack, (size_t)s_npack, sizeof *s_pack, pack_cmp);
            for (r = 1; r < s_npack; r++)
                if (s_pack[r].hash != s_pack[wr - 1].hash ||
                    s_pack[r].alpha != s_pack[wr - 1].alpha) s_pack[wr++] = s_pack[r];
            s_npack = wr;
        }
        fprintf(stderr, "[RBX DX11] hi-res texture pack: %d files\n", s_npack);
    }
    return s_npack;
}

/* Every pixel's colour, position-dependent. Hot only while textures change, and quick next to decoding them. */
static uint64_t pixel_hash(const uint32_t *argb, uint32_t w, uint32_t h)
{
    const uint64_t K = 0x9E3779B97F4A7C15ull;
    uint64_t x = ((uint64_t)w << 32) | h, mul = K, step = K * 2;
    size_t i, n = (size_t)w * h;
    for (i = 0; i < n; i++, mul += step) x += (uint64_t)(argb[i] & 0xFFFFFFu) * mul;
    x ^= x >> 29;
    x *= K;
    return x ^ (x >> 32);
}

/* Alpha in 32 steps, position-dependent: a small rounding difference is not a difference. */
static uint64_t alpha_hash(const uint32_t *argb, uint32_t w, uint32_t h)
{
    const uint64_t K = 0x9E3779B97F4A7C15ull;
    uint64_t x = ((uint64_t)w << 32) | h, mul = K, step = K * 2;
    size_t i, n = (size_t)w * h;
    for (i = 0; i < n; i++, mul += step) x += (uint64_t)(argb[i] >> 27) * mul;
    x ^= x >> 29;
    x *= K;
    return x ^ (x >> 32);
}

/* A .dds with a DX10 header, mips as stored: no decoding and no mip generation. 0: not usable. */
static int pack_make_dds(const char *path, TexEnt *e)
{
    enum { HDR = 4 + 124 + 20 };
    FILE *f = fopen(path, "rb");
    long size;
    uint8_t *buf = NULL;
    int ok = 0;
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size > HDR && (buf = (uint8_t *)malloc((size_t)size)) != NULL && fread(buf, 1, (size_t)size, f) == (size_t)size
        && !memcmp(buf, "DDS ", 4) && !memcmp(buf + 84, "DX10", 4)) {
        uint32_t h = *(uint32_t *)(buf + 12), w = *(uint32_t *)(buf + 16), mips = *(uint32_t *)(buf + 28);
        uint32_t fmt = *(uint32_t *)(buf + 128), bx = fmt == DXGI_FORMAT_BC7_UNORM, m;
        D3D11_SUBRESOURCE_DATA sd[16];
        const uint8_t *q = buf + HDR, *end = buf + size;
        if (!mips) mips = 1;
        if ((bx || fmt == DXGI_FORMAT_B8G8R8A8_UNORM) && mips <= 16 && w && h && w <= 16384 && h <= 16384) {
            uint32_t mw = w, mh = h;
            D3D11_TEXTURE2D_DESC td;
            for (m = 0; m < mips; m++) {
                uint32_t pitch = bx ? ((mw + 3) / 4) * 16 : mw * 4, rows = bx ? (mh + 3) / 4 : mh;
                sd[m].pSysMem = q; sd[m].SysMemPitch = pitch; sd[m].SysMemSlicePitch = 0;
                if ((size_t)(end - q) < (size_t)pitch * rows) break;
                q += (size_t)pitch * rows;
                mw = mw > 1 ? mw / 2 : 1; mh = mh > 1 ? mh / 2 : 1;
            }
            if (m == mips) {
                memset(&td, 0, sizeof td);
                td.Width = w; td.Height = h; td.MipLevels = mips; td.ArraySize = 1; td.Format = (DXGI_FORMAT)fmt;
                td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_IMMUTABLE; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                ok = SUCCEEDED(ID3D11Device_CreateTexture2D(s_dev, &td, sd, &e->tex));
            }
        }
    }
    free(buf);
    fclose(f);
    return ok;
}

/* Make e's texture from the pack image of this hash. 0: none, so the guest's own is used. */
static int s_gen_mips;
static int pack_make(const uint32_t *argb, uint32_t width, uint32_t height, TexEnt *e)
{
    static IWICImagingFactory *fac;
    static int fac_tried;
    uint64_t hash = pixel_hash(argb, width, height), alpha;
    int lo = 0, hi = s_npack, found, ok = 0;
    IWICBitmapDecoder *dec = NULL;
    IWICBitmapFrameDecode *fr = NULL;
    IWICFormatConverter *cv = NULL;
    WCHAR wp[MAX_PATH];
    UINT w = 0, h = 0;
    uint8_t *px = NULL;

    while (lo < hi) {
        int m = (lo + hi) / 2;
        if (s_pack[m].hash < hash) lo = m + 1; else hi = m;
    }
    if (lo == s_npack || s_pack[lo].hash != hash) return 0;

    alpha = alpha_hash(argb, width, height);
    for (found = lo; found < s_npack && s_pack[found].hash == hash; found++) {
        if (s_pack[found].alpha == alpha) break;
    }
    if (found == s_npack || s_pack[found].hash != hash) {
        static unsigned nd;
        if (nd++ < 40) fprintf(stderr, "[RBX DX11] pack alpha differs, keeping the original: %s\n", s_pack[lo].path);
        return 0;
    }
    {
        size_t len = strlen(s_pack[found].path);
        if (len > 4 && !_stricmp(s_pack[found].path + len - 4, ".dds")) {
            ok = pack_make_dds(s_pack[found].path, e);
            { static unsigned n; if (n++ < 40 || (n % 100) == 0) fprintf(stderr, "[RBX DX11] pack #%u dds ok=%d %016llX\n", n, ok, (unsigned long long)hash); }
            return ok;
        }
    }
    if (!fac_tried) {
        fac_tried = 1;
        CoInitializeEx(NULL, COINIT_MULTITHREADED);
        if (FAILED(CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, &IID_IWICImagingFactory, (void **)&fac))) fac = NULL;
    }
    if (!fac) return 0;
    MultiByteToWideChar(CP_ACP, 0, s_pack[found].path, -1, wp, MAX_PATH);
    if (SUCCEEDED(IWICImagingFactory_CreateDecoderFromFilename(fac, wp, NULL, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec))
        && SUCCEEDED(IWICBitmapDecoder_GetFrame(dec, 0, &fr))
        && SUCCEEDED(IWICBitmapFrameDecode_GetSize(fr, &w, &h)) && w && h && w <= 16384 && h <= 16384
        && SUCCEEDED(IWICImagingFactory_CreateFormatConverter(fac, &cv))
        && SUCCEEDED(IWICFormatConverter_Initialize(cv, (IWICBitmapSource *)fr, &GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, NULL, 0.0, WICBitmapPaletteTypeCustom))
        && (px = (uint8_t *)malloc((size_t)w * h * 4)) != NULL
        && SUCCEEDED(IWICFormatConverter_CopyPixels(cv, NULL, w * 4, w * h * 4, px))) {
        D3D11_TEXTURE2D_DESC td;
        memset(&td, 0, sizeof td);
        td.Width = w; td.Height = h; td.MipLevels = 0; td.ArraySize = 1; td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        td.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
        if (SUCCEEDED(ID3D11Device_CreateTexture2D(s_dev, &td, NULL, &e->tex))) {
            ID3D11DeviceContext_UpdateSubresource(s_ctx, (ID3D11Resource *)e->tex, 0, NULL, px, w * 4, 0);
            s_gen_mips = 1;
            ok = 1;
            { static unsigned n; if (n++ < 40 || (n % 100) == 0) fprintf(stderr, "[RBX DX11] pack #%u %ux%u %016llX\n", n, w, h, (unsigned long long)hash); }
        }
    }
    free(px);
    if (cv) IWICFormatConverter_Release(cv);
    if (fr) IWICBitmapFrameDecode_Release(fr);
    if (dec) IWICBitmapDecoder_Release(dec);
    return ok;
}

/* Texture-cache index: bucket -> slot of the last entry seen with that key. A hint only (an entry is always
 * checked against the texture before use), so a stale or colliding bucket just falls back to the scan. It was
 * 256 buckets in get_tex only; the slow path scanned all 768 slots on every call. */
#define TEX_IDX_BITS 12
static uint16_t s_tex_idx[1u << TEX_IDX_BITS];
static unsigned tex_bucket(const Nv2aTexture *t)
{
    return (((t->offset >> 4) * 2654435761u) ^ (t->width * 40503u) ^ (t->height * 9973u) ^ (t->color * 2246822519u) ^
            (t->pitch * 3266489917u)) >> (32 - TEX_IDX_BITS);
}
static int tex_ent_is(const TexEnt *c, const Nv2aTexture *t)
{
    return c->tex && c->offset == t->offset && c->w == t->width && c->h == t->height && c->color == t->color && c->pitch == t->pitch;
}

static ID3D11ShaderResourceView *get_tex_slow(const Nv2aTexture *t)
{
    int i, free_i = -1, lru = 0, first_i = -1;
    uint64_t h, th;
    uint32_t n = tex_bytes(t);
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
    LARGE_INTEGER hash_start, hash_end;

    if (!tex_ok(t)) return s_white_srv;
    s_noalpha = (t->color == 0x1E || t->color == 0x1F || t->color == 0x07) ? 1 : 0;
    for (i = 0; i < MAX_RT; i++)
        if (s_rt[i].tex && s_rt[i].srv && s_rt[i].va == t->offset && s_rt[i].w == t->width && s_rt[i].h == t->height) {
            rt_resolve(&s_rt[i]);
            s_rt[i].sampled = s_frame + 1; st_rtsamples++;
            return s_rt[i].srv;
        }
    QueryPerformanceCounter(&hash_start);
    th = hash_bytes(mem + t->offset, n);
    if (t->color == 0x0B && t->palette) h = th ^ (hash_bytes(mem + t->palette, 1024) * 31u);
    else h = th;
    st_th_full++;
    QueryPerformanceCounter(&hash_end);
    st_hash_ns_us += (uint32_t)((hash_end.QuadPart - hash_start.QuadPart) * 1000000ULL / gb_qf());
    {   /* Indexed hit: start the scan at that slot, so the matching entry is found at once. */
        unsigned hint = s_tex_idx[tex_bucket(t)];
        if (hint < MAX_TEX && tex_ent_is(&s_tx[hint], t)) first_i = (int)hint;
    }
    for (int k = 0; k < MAX_TEX; k++) {
        TexEnt *e;
        i = first_i >= 0 ? (k == 0 ? first_i : (k <= first_i ? k - 1 : k)) : k;
        e = &s_tx[i];
        if (!e->tex) { if (free_i < 0) free_i = i; continue; }
        if (e->offset == t->offset && e->w == t->width && e->h == t->height && e->color == t->color && e->pitch == t->pitch) {
            s_tex_idx[tex_bucket(t)] = (uint16_t)i;
            e->used = s_tick;
            { static int nc = -1; if (nc < 0) nc = getenv("RECOMP_GPU_NOTEXCACHE") ? 1 : 0; /* A/B switch: re-upload every draw */
              if (!nc && e->hash == h) { e->thash = th; e->sig = sample_sig(mem + t->offset, n); e->vframe = s_frame + 1; return e->srv; } }
            free_i = i;                    /* same slot, new content */
            e->redecodes++;
            DEFER_REL(e->srv); DEFER_REL(e->tex);
            goto decode;
        }
        if (e->used < s_tx[lru].used) lru = i;
    }
    if (free_i < 0) { free_i = lru; DEFER_REL(s_tx[free_i].srv); DEFER_REL(s_tx[free_i].tex); }
    s_tx[free_i].redecodes = 0;
decode:
    {
        uint32_t *argb = (uint32_t *)malloc((size_t)t->width * t->height * 4);
        D3D11_TEXTURE2D_DESC td;
        D3D11_SUBRESOURCE_DATA sd;
        TexEnt *e = &s_tx[free_i];
        if (!argb) return s_white_srv;
        unsigned q0 = qpc_us(), q1;
        if (!argb) return s_white_srv;
        if (!nv2a_backend_decode_texture(t, argb)) { fprintf(stderr, "[GPUTR] decode FAILED fmt=%02X %ux%u\n", t->color, t->width, t->height); free(argb); e->tex = NULL; return s_white_srv; }
        { static int tm = -1; static unsigned tn;
          if (tm < 0) tm = getenv("RECOMP_GPU_TEXMEAN") != NULL;
          if (tm && tn < 80 && t->width >= 32 && t->color != 0x1E) {
              size_t np = (size_t)t->width * t->height, i; unsigned long long sum = 0;
              unsigned a0 = 0;
              for (i = 0; i < np; i++) {
                  uint32_t p = argb[i];
                  sum += (p & 255u) + ((p >> 8) & 255u) + ((p >> 16) & 255u);
                  if ((p >> 24) == 0) a0++;
              }
              tn++;
              fprintf(stderr, "[TEXMEAN] f%u off=%08X %ux%u fmt=%02X meanRGB=%.1f a0=%u%%\n",
                      s_frame, t->offset, t->width, t->height, t->color,
                      (double)sum / (double)(np * 3ull), (unsigned)(a0 * 100ull / np));
          } }
        q1 = qpc_us(); st_dec_us += q1 - q0;
        /* A slot rewritten this often is a sprite sheet, which no pack can hold: not worth hashing every version. */
        if (s_hires && e->redecodes < 64 && rbx_pack_count() &&
            pack_make(argb, t->width, t->height, e)) {
            free(argb);
            e->from_pack = 1;
            goto made;
        }
        e->from_pack = 0;
        memset(&td, 0, sizeof td);
        td.Width = t->width; td.Height = t->height; td.MipLevels = td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_IMMUTABLE;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        sd.pSysMem = argb; sd.SysMemPitch = t->width * 4; sd.SysMemSlicePitch = 0;
        if (s_flush_tex) ID3D11DeviceContext_Flush(s_ctx);
        if (s_aniso > 1 && (t->width > 1 || t->height > 1)) {
            D3D11_TEXTURE2D_DESC mip_td = td;
            mip_td.MipLevels = 0;
            mip_td.Usage = D3D11_USAGE_DEFAULT;
            mip_td.BindFlags |= D3D11_BIND_RENDER_TARGET;
            mip_td.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
            if (SUCCEEDED(ID3D11Device_CreateTexture2D(s_dev, &mip_td, NULL, &e->tex))) {
                ID3D11DeviceContext_UpdateSubresource(s_ctx, (ID3D11Resource *)e->tex, 0, NULL, argb, sd.SysMemPitch, 0);
                s_gen_mips = 1;
            }
        }
        if (!e->tex && FAILED(ID3D11Device_CreateTexture2D(s_dev, &td, &sd, &e->tex))) { fprintf(stderr, "[RBX DX11] CreateTexture2D failed %ux%u fmt=%02X\n", t->width, t->height, t->color); free(argb); e->tex = NULL; return s_white_srv; }
        { static int tl = -1; static unsigned nlog;
          if (tl < 0) tl = getenv("RECOMP_GPU_TEXLOG") ? 1 : 0;
          if (tl && nlog < 400 && t->width < 512) { nlog++;
              fprintf(stderr, "[TEXLOG] f%u new tex off=%08X %ux%u fmt=%02X pitch=%u pal=%08X hash=%016llX", s_frame, t->offset, t->width, t->height, t->color, t->pitch, t->palette, (unsigned long long)h);
              if (t->color == 0x0B && t->palette) { const uint32_t *pp = (const uint32_t *)(mem + t->palette); unsigned a0 = 0, aw = 0, k; for (k = 0; k < 256; k++) { if ((pp[k] >> 24) == 0) a0++; if (pp[k] == 0xFFFFFFFFu) aw++; } fprintf(stderr, " pal: alpha0=%u white=%u e0=%08X e1=%08X e255=%08X palhash=%016llX", a0, aw, pp[0], pp[1], pp[255], (unsigned long long)hash_bytes(mem + t->palette, 1024)); }
              { unsigned a0 = 0, aw = 0; size_t k, np = (size_t)t->width * t->height; for (k = 0; k < np; k++) { if ((argb[k] >> 24) == 0) a0++; else if (argb[k] == 0xFFFFFFFFu) aw++; } fprintf(stderr, " decoded: alpha0=%u%% white=%u%%\n", (unsigned)(a0 * 100ull / np), (unsigned)(aw * 100ull / np)); }
          } }
        free(argb);
made:
        ID3D11Device_CreateShaderResourceView(s_dev, (ID3D11Resource *)e->tex, NULL, &e->srv);
        if (s_gen_mips) { s_gen_mips = 0; if (e->srv) ID3D11DeviceContext_GenerateMips(s_ctx, e->srv); }
        if (s_flush_tex) ID3D11DeviceContext_Flush(s_ctx);
        e->offset = t->offset; e->w = t->width; e->h = t->height; e->color = t->color; e->pitch = t->pitch;
        e->hash = h; e->used = s_tick;
        s_tex_idx[tex_bucket(t)] = (uint16_t)free_i;
        e->thash = th; e->sig = sample_sig(mem + t->offset, n); e->vframe = s_frame + 1;
        st_crt_us += qpc_us() - q1;
        st_decodes++;
        return e->srv ? e->srv : s_white_srv;
    }
}

/* RECOMP_GPU_TEXDUMP=<dir>: while the RECOMP_FB_DUMP_FLIPS flag file exists, write every distinct texture
 * (offset + content + palette) a draw samples as <dir>\t<offset>_<pal>_<fmt>_<w>x<h>_<hash>.bmp, decoded the way the
 * renderer sees it. For texture bugs: compare against what the scene should show. Checked once per flip. */
static int s_texdump_on;
static void texdump_poll(void)
{
    static const char *dir = (const char *)-1, *flag;
    FILE *f;
    if (dir == (const char *)-1) { dir = getenv("RECOMP_GPU_TEXDUMP"); flag = getenv("RECOMP_FB_DUMP_FLIPS"); }
    if (!dir || !flag) return;
    f = fopen(flag, "rb");
    s_texdump_on = f != NULL;
    { static int last = -1; if (last != s_texdump_on) { last = s_texdump_on; fprintf(stderr, "[TEXDUMP] %s flag=%s dir=%s\n", s_texdump_on ? "on" : "off", flag, dir); } }
    if (f) fclose(f);
}
static void texdump(const Nv2aTexture *t)
{
    static uint64_t seen[16384]; static unsigned nseen;
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
    uint64_t h;
    uint32_t *argb, k, hdr[13], *per_slot = NULL;
    char path[512];
    FILE *f;
    if (t->color == 0x1E && t->width >= 640) return;   /* the full-frame copy: new every frame, and not artwork */
    {   /* character sprite sheets: thousands of versions at one address, and no version ever comes back */
        static struct { uint32_t off, n; } per[1024]; static unsigned nper;
        unsigned j;
        for (j = 0; j < nper && per[j].off != t->offset; j++) {}
        if (j == nper && nper < 1024) { per[j].off = t->offset; per[j].n = 0; nper++; }
        if (j < nper) { if (per[j].n >= 100) return; per_slot = &per[j].n; }
    }
    h = hash_bytes(mem + t->offset, tex_bytes(t));
    if (t->color == 0x0B && t->palette) h ^= hash_bytes(mem + t->palette, 1024) * 31u;

    for (k = 0; k < nseen; k++) if (seen[k] == h) return;
    if (nseen == 16384) return;
    seen[nseen++] = h;
    if (per_slot) (*per_slot)++;
    if (!(argb = (uint32_t *)malloc((size_t)t->width * t->height * 4))) return;
    if (!nv2a_backend_decode_texture(t, argb)) { free(argb); return; }
    snprintf(path, sizeof path, "%s\\t%08X_%08X_%02X_%ux%u_%016llX.bmp", getenv("RECOMP_GPU_TEXDUMP"), t->offset, t->palette,
             t->color, t->width, t->height, (unsigned long long)h);
    if ((f = fopen(path, "wb")) != NULL) {
        uint32_t img = t->width * t->height * 4;
        fputc('B', f); fputc('M', f);
        hdr[0] = 54 + img; hdr[1] = 0; hdr[2] = 54; hdr[3] = 40; hdr[4] = t->width; hdr[5] = (uint32_t)-(int32_t)t->height;
        hdr[6] = 1 | 32 << 16; hdr[7] = 0; hdr[8] = img; hdr[9] = hdr[10] = 2835; hdr[11] = hdr[12] = 0;
        fwrite(hdr, 4, 13, f);
        fwrite(argb, 4, (size_t)t->width * t->height, f);
        fclose(f);
    }
    free(argb);
}

/* Fast path (RECOMP_GPU_TEXHASH=0 turns it off = every draw hashes the full texture bytes, the old behaviour).
 * A texture whose bytes were fully hashed earlier in this same flip interval is re-checked with sample_sig() only
 * (the palette is still hashed in full: palette animation changes it between draws). RECOMP_GPU_TEXHASH_VERIFY=1
 * also computes the full hash on every fast hit and counts disagreements (thstale in [RBX DX11]); the result is then exact. */
static ID3D11ShaderResourceView *get_tex(const Nv2aTexture *t)
{
    static int mode = -1, verify = 0;
    uint32_t n, k;
    int i;
    const uint8_t *mem;
    TexEnt *e = NULL;
    if (!tex_ok(t)) return s_white_srv;
    if (mode < 0) { const char *m = getenv("RECOMP_GPU_TEXHASH"); mode = !(m && m[0] == '0'); verify = getenv("RECOMP_GPU_TEXHASH_VERIFY") != NULL; }
    if (s_texdump_on && t->width && t->height && t->width <= 4096 && t->height <= 4096) texdump(t);
    if (!mode || !t->width || !t->height || t->width > 4096 || t->height > 4096) return get_tex_slow(t);
    for (i = 0; i < MAX_RT; i++)
        if (s_rt[i].tex && s_rt[i].srv && s_rt[i].va == t->offset && s_rt[i].w == t->width && s_rt[i].h == t->height) return get_tex_slow(t);
    k = tex_bucket(t);
    {
        TexEnt *c = &s_tx[s_tex_idx[k] < MAX_TEX ? s_tex_idx[k] : 0];
        if (c->tex && c->offset == t->offset && c->w == t->width && c->h == t->height && c->color == t->color && c->pitch == t->pitch) e = c;
    }
    if (!e) {
        for (i = 0; i < MAX_TEX; i++) {
            TexEnt *c = &s_tx[i];
            if (c->tex && c->offset == t->offset && c->w == t->width && c->h == t->height && c->color == t->color && c->pitch == t->pitch) { e = c; s_tex_idx[k] = (uint16_t)i; break; }
        }
    }
    if (!e || e->vframe != s_frame + 1 || !e->srv) {
        ID3D11ShaderResourceView *r = get_tex_slow(t);
        return r;   /* get_tex_slow keeps s_tex_idx current */
    }
    n = tex_bytes(t); mem = (const uint8_t *)xbox_GetMemoryOffset();
    {
        uint64_t h = e->thash;
        if (t->color == 0x0B && t->palette) h ^= hash_bytes(mem + t->palette, 1024) * 31u;
        if (sample_sig(mem + t->offset, n) == e->sig && h == e->hash) {
            if (verify) {
                uint64_t fh = hash_bytes(mem + t->offset, n);
                if (t->color == 0x0B && t->palette) fh ^= hash_bytes(mem + t->palette, 1024) * 31u;
                if (fh != e->hash) { static unsigned nl; st_th_stale++; if (nl++ < 40) fprintf(stderr, "[TEXV] stale f%u off=%08X %ux%u fmt=%02X\n", s_frame, t->offset, t->width, t->height, t->color); e->vframe = 0; return get_tex_slow(t); }
            }
            e->used = s_tick; st_th_fast++;
            { static int nc = -1; if (nc < 0) nc = getenv("RECOMP_GPU_NOTEXCACHE") ? 1 : 0; if (nc) return get_tex_slow(t); }
            s_noalpha = (t->color == 0x1E || t->color == 0x1F || t->color == 0x07) ? 1 : 0;
            return e->srv;
        }
    }
    e->vframe = 0;
    return get_tex_slow(t);
}

/* ---- watchdog (debug): reports when the executor stops calling the back end --- */
static unsigned st_us_clear, st_us_draw, st_us_flip;
typedef struct { uint64_t vh, th; float x0, y0; uint32_t n, w, h, fmt, c0, st; } FdRec;
static FdRec fd_cur[256], fd_prev[256]; static unsigned fd_pn, fd_lines;
extern uint32_t g_dbg_begin, g_dbg_ilf, g_dbg_dpn, g_dbg_dp, g_dbg_trunc, g_dbg_batches, g_dbg_flush0, g_dbg_xffail, g_dbg_short;
static int s_fh = -1; static uint64_t fh_dh, fh_rth; static unsigned fh_nd, fh_nc;
static volatile LONG s_stage;          /* 0 idle/outside, 1 clear, 2 draw, 3 readback, 4 flip */
static volatile DWORD s_stage_tick;
uint32_t rbx_gpu_flips(void) { return s_frame; } /* for the stall dumper in main.c */
static volatile LONG s_calls;
static void gb_mark(LONG st) { s_stage = st; s_stage_tick = GetTickCount(); s_calls++; }
static void gb_freeze_dump(int pass);
static DWORD WINAPI gb_watchdog(LPVOID p)
{
    LONG lastcalls = -1; int quiet = 0, dumps = 0; DWORD t0 = GetTickCount(); (void)p;
    for (;;) {
        Sleep(3000);
        if (s_calls == lastcalls) {
            fprintf(stderr, "[RBX DX11] WATCHDOG: no backend call for >=3 s; stage=%ld (0 idle,1 clear,2 draw,3 readback,4 flip) for %lu ms; calls=%ld\n",
                    (long)s_stage, (unsigned long)(GetTickCount() - s_stage_tick), (long)s_calls);
            /* Stuck >= 6 s past boot (> 1000 backend calls, or up > 40 s, which catches a stall soon after the logos):
             * every thread's stack, twice 6 s apart, so a spin and a block look different. */
            if (++quiet >= 2 && (s_calls > 1000 || GetTickCount() - t0 > 40000) && dumps < 2 && (quiet % 2) == 0) gb_freeze_dump(++dumps);
        } else quiet = 0;
        lastcalls = s_calls;
    }
    return 0;
}
/* ---- Nv2aBackend -------------------------------------------------------- */

static void gb_clear_impl(const Nv2aSurface *s, const Nv2aRenderState *rs, uint32_t flags, uint32_t argb, uint32_t zst)
{
    sf_flush();
    Rt *rt;
    Ds *ds;
    if (!gb_ready()) return;
    if (s_trace_all && s_frame >= s_ta_lo && s_frame <= s_ta_hi && s_trace_lines < 3000 && ta_new(0xC1EA0000ull ^ ((uint64_t)flags << 32) ^ argb ^ ((uint64_t)s->color_va << 8))) { s_trace_lines++; fprintf(stderr, "[GPUTA] f%u CLEAR va=%08X %ux%u flags=%X argb=%08X zst=%08X\n", s_frame, s->color_va, s->width, s->height, flags, argb, zst); }
    rt = get_rt(s);
    if (!rt) return;
    if (flags & 0xF0) {
        float c[4];
        c[0] = ((argb >> 16) & 255) / 255.0f; c[1] = ((argb >> 8) & 255) / 255.0f;
        c[2] = (argb & 255) / 255.0f; c[3] = ((argb >> 24) & 255) / 255.0f;
        {
            static int full = -1;
            unsigned wmask = (flags >> 4) & 15u;   /* R 1, G 2, B 4, A 8: the D3D11 write-mask bits */
            uint32_t sw = rt_full_w(rt) * (uint32_t)s_scale, sh = rt->h * (uint32_t)s_scale;
            D3D11_RECT r;
            r.left = 0; r.top = 0; r.right = (LONG)sw; r.bottom = (LONG)sh;
            if (full < 0) full = getenv("RECOMP_GPU_CLEARFULL") != NULL;
            if ((g_rbx_clear_rect_set & 3) == 3) {
                /* Inclusive x0..x1 / y0..y1 in un-antialiased pixels; the target is in AA pixels (xemu scales the
                 * rectangle by the AA factor the same way). */
                uint32_t ax = rt->aa_sx ? rt->aa_sx : 1, ay = rt->aa_sy ? rt->aa_sy : 1;
                uint32_t x0 = (g_rbx_clear_rect_h & 0xFFFF) * ax, x1 = ((g_rbx_clear_rect_h >> 16) + 1) * ax;
                uint32_t y0 = (g_rbx_clear_rect_v & 0xFFFF) * ay, y1 = ((g_rbx_clear_rect_v >> 16) + 1) * ay;
                if (x1 > rt->w) x1 = rt->w;
                if (y1 > rt->h) y1 = rt->h;
                if (x0 < x1 && y0 < y1) {
                    /* Widescreen: a clear of the full width covers the side areas too; a smaller one is in the
                     * centred 4:3 like the draws. */
                    if (x0 == 0 && x1 >= rt->w) x1 = rt_full_w(rt);
                    else { x0 += rt->ex; x1 += rt->ex; }
                    r.left = (LONG)(x0 * (uint32_t)s_scale); r.right = (LONG)(x1 * (uint32_t)s_scale);
                    r.top = (LONG)(y0 * (uint32_t)s_scale); r.bottom = (LONG)(y1 * (uint32_t)s_scale);
                }
            }
            if (!full && (wmask != 15u || r.left > 0 || r.top > 0 || (uint32_t)r.right < sw || (uint32_t)r.bottom < sh)) {
                st_clear_partial++;
                if (!clear_partial_draw(rt, wmask, c, &r))
                    ID3D11DeviceContext_ClearRenderTargetView(s_ctx, rt->rtv, c);
            } else
                ID3D11DeviceContext_ClearRenderTargetView(s_ctx, rt->rtv, c);
        }
        rt->resolve_pending = rt->msaa != NULL;
        rt->dirty = 1; rt->shown_dirty = 1;
    }
    if ((flags & 3) && (ds = get_ds(s, rs->zeta_va)) != NULL) {
        UINT f = 0;
        if (flags & 1) f |= D3D11_CLEAR_DEPTH;
        if (flags & 2) f |= D3D11_CLEAR_STENCIL;
        ID3D11DeviceContext_ClearDepthStencilView(s_ctx, ds->dsv, f, (float)(zst >> 8) / 16777215.0f, (UINT8)(zst & 0xFF));
    }
    st_clears++;
}

static int is_front(float area, uint32_t front_face)
{
    /* y grows downwards in surface pixels, so a positive area is clockwise. */
    return front_face == 0x901 ? area < 0.0f : area > 0.0f;
}

/* RECOMP_GPU_TEXTDRAW diagnostic: after a glyph draw, read the GPU texture copy and the rendered rectangle back and log statistics, so a
 * driver-specific sampling/blend/alpha-test failure shows up as numbers (e.g. the glyph rectangle coming out solid white). */
static void probe_after_draw(Rt *rt, ID3D11ShaderResourceView *srv, const Nv2aBatch *b)
{
    D3D11_MAPPED_SUBRESOURCE m;
    int x0 = 100000, y0 = 100000, x1 = -100000, y1 = -100000, x, y;
    uint32_t i;
    ID3D11Resource *res = NULL;
    for (i = 0; i < b->count && i < 6; i++) { int vx = (int)b->vertices[i].x, vy = (int)b->vertices[i].y; if (vx < x0) x0 = vx; if (vx > x1) x1 = vx; if (vy < y0) y0 = vy; if (vy > y1) y1 = vy; }
    if (srv && srv != s_white_srv) {
        static ID3D11Resource *seen[8]; static int nseen; int dup = 0, k;
        ID3D11ShaderResourceView_GetResource(srv, &res);
        for (k = 0; k < nseen; k++) if (seen[k] == res) dup = 1;
        if (res && !dup && nseen < 8) {
            D3D11_TEXTURE2D_DESC d; ID3D11Texture2D *st = NULL;
            seen[nseen++] = res;
            ID3D11Texture2D_GetDesc((ID3D11Texture2D *)res, &d);
            d.Usage = D3D11_USAGE_STAGING; d.BindFlags = 0; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ; d.MiscFlags = 0;
            if (SUCCEEDED(ID3D11Device_CreateTexture2D(s_dev, &d, NULL, &st))) {
                ID3D11DeviceContext_CopyResource(s_ctx, (ID3D11Resource *)st, res);
                if (SUCCEEDED(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)st, 0, D3D11_MAP_READ, 0, &m))) {
                    unsigned a0 = 0, aw = 0, other = 0; uint32_t n = d.Width * d.Height;
                    for (y = 0; y < (int)d.Height; y++) { const uint32_t *row = (const uint32_t *)((const uint8_t *)m.pData + (size_t)y * m.RowPitch);
                        for (x = 0; x < (int)d.Width; x++) { uint32_t p = row[x]; if ((p >> 24) == 0) a0++; else if (p == 0xFFFFFFFFu) aw++; else other++; } }
                    fprintf(stderr, "[TEXPROBE] gpu texture copy %ux%u fmt=%u: alpha0=%u%% white=%u%% shaded=%u%%\n", d.Width, d.Height, (unsigned)d.Format, a0 * 100u / n, aw * 100u / n, other * 100u / n);
                    ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)st, 0);
                }
                ID3D11Texture2D_Release(st);
            }
        }
        SAFE_REL(res);
    }
    if (x0 < 0) x0 = 0; if (y0 < 0) y0 = 0;
    ID3D11DeviceContext_CopyResource(s_ctx, (ID3D11Resource *)rt->stage, rt_guest_image(rt));
    if (SUCCEEDED(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)rt->stage, 0, D3D11_MAP_READ, 0, &m))) {
        unsigned nw = 0, n = 0, nz = 0; uint32_t first[4] = { 0, 0, 0, 0 };
        for (y = y0; y < y1; y++) for (x = x0; x < x1; x++) {
            uint32_t p = ((const uint32_t *)((const uint8_t *)m.pData + (size_t)y * m.RowPitch))[x];
            if (n < 4) first[n] = p; n++; if ((p & 0xFFFFFF) == 0xFFFFFF) nw++; if (p == 0) nz++;
        }
        fprintf(stderr, "[TEXPROBE] rendered rect (%d,%d)-(%d,%d): %u px, white=%u (%u%%) zero=%u first=%08X %08X %08X %08X\n", x0, y0, x1, y1, n, nw, n ? nw * 100u / n : 0, nz, first[0], first[1], first[2], first[3]);
        ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)rt->stage, 0);
    }
}

extern int g_rbx_precull;   /* executor: this batch was back-face culled with exact strip parity */
/* Executor (GPU clipping): this indexed batch is unclipped and unculled; bit 1 adds the z near plane at
 * g_rbx_clip_z (screen z). Odd strip triangles are already flipped, so one winding rule culls them all. */
extern int g_rbx_gpuclip;
extern float g_rbx_clip_z;
static ID3D11RasterizerState *clip_rs(const Nv2aRenderState *r, int scissor)
{
    int ccw = r->front_face == 0x901, mode = 0;
    ID3D11RasterizerState **slot;
    if (s_cull_on && r->cull_enable)
        mode = r->cull_face == 0x404 ? 1 : r->cull_face == 0x405 ? 2 : 0;
    slot = &s_rs_clip[ccw][mode][scissor ? 1 : 0];
    if (!*slot) {
        D3D11_RASTERIZER_DESC rd;
        memset(&rd, 0, sizeof rd);
        rd.FillMode = D3D11_FILL_SOLID;
        rd.CullMode = mode == 1 ? D3D11_CULL_FRONT : mode == 2 ? D3D11_CULL_BACK : D3D11_CULL_NONE;
        rd.FrontCounterClockwise = ccw ? TRUE : FALSE;
        rd.DepthClipEnable = FALSE;           /* the far plane clamps as before; the near planes are clip distances */
        rd.MultisampleEnable = TRUE;
        rd.ScissorEnable = scissor ? TRUE : FALSE;
        if (FAILED(ID3D11Device_CreateRasterizerState(s_dev, &rd, slot))) *slot = NULL;
    }
    return *slot ? *slot : s_rs;
}
/* Executor: when set for a batch, b->vertices holds each vertex once and these are its triangles (3 indices each,
 * already culled and clipped). RECOMP_GPU_INDEXED=0 in the executor turns it off. */
extern const uint32_t *g_rbx_bidx;
extern uint32_t g_rbx_bidx_n;

/* Combiner batches: stage 1 often samples with oT1; if the VSH left t1 at 0, reuse t0 so the 0919390C T1-blend does
 * not hit opaque black at UV(0,0). Then fill the vertex's RC extension (float V0, d1, all texcoords, fog). */
static void rc_vert_fix(Nv2aVertex *dv, RcVertExt *xe)
{
    float *t0 = dv->tex[0], *t1 = dv->tex[1];
    uint32_t d = dv->diffuse;
    static int force_v0 = -1;
    if (fabsf(t1[0]) + fabsf(t1[1]) + fabsf(t1[2]) < 1e-6f
        && fabsf(t0[0]) + fabsf(t0[1]) + fabsf(t0[2]) > 1e-6f)
        memcpy(t1, t0, sizeof dv->tex[0]);
    if (!xe) return;
    if (force_v0 < 0)
        force_v0 = getenv("RECOMP_GPU_FORCEV0") != NULL;
    if (force_v0) {
        xe->v0[0] = 0.95f; xe->v0[1] = 0.75f;
        xe->v0[2] = 0.35f; xe->v0[3] = 1.0f;
    } else {
        xe->v0[0] = ((d >> 16) & 255u) / 255.0f;
        xe->v0[1] = ((d >> 8) & 255u) / 255.0f;
        xe->v0[2] = (d & 255u) / 255.0f;
        xe->v0[3] = ((d >> 24) & 255u) / 255.0f;
    }
    memcpy(xe->d1, dv->d1, sizeof xe->d1);
    memcpy(xe->tc, dv->tex, sizeof xe->tc);
    xe->fog = dv->fog;
    xe->pad[0] = xe->pad[1] = xe->pad[2] = 0.0f;
}
/* Combiner state for a draw: the RC parameters, stage textures and samplers, and the per-draw constants (b0) in
 * *cbufp. t0_zero / t1_zero: every vertex's oT0 / oT1 xyz is ~0 (before the t1 <- t0 fix-up). Returns 0 when the
 * combiners cannot draw it; the caller then uses the simple path. */
static int rc_setup(const Nv2aSurface *s, const Nv2aBatch *b, uint32_t kept, int t0_zero, int t1_zero, int packed_ext,
                    int gclip, float clipz, float nearz, ID3D11ShaderResourceView *bound_srv[4],
                    ID3D11SamplerState *bound_smp[4], ID3D11Buffer **cbufp, const Rt *rt, int flat_full)
{
    float wsq = 0.0f;
    const Nv2aRenderState *r = b->state;
    D3D11_MAPPED_SUBRESOURCE m;
    ID3D11Buffer *cbuf;
    unsigned stage;
    int use_rc = 1;
    RcParams rc;
    RcDrawCb rcb;
    const Nv2aCombiner *c = b->combiner;
    /* Snapshot: batch.combiner points at the live PB state. */
    Nv2aCombiner c_local = *c;
    c = &c_local;
    /* Stage1 ic1=0919390C is lerp(R0,T1,T1.a). Packed verts already
     * reuse t1←t0 when oT1 was zero (above), so stage1 can run and
     * darken/modulate the lit fill. Auto-skipping on t1_zero left
     * stage0-only T0×white V0 → giant bright water/chrome quads.
     * Skip stage1 only when RECOMP_GPU_RC1SKIP=1, or when both oT0
     * and oT1 are ~zero (nothing to remap — UV(0,0) black risk). */
    { static int rc1skip = -1, rc1fix = 0;
      if (rc1skip < 0) rc1skip = getenv("RECOMP_GPU_RC1SKIP") != NULL;
      if ((c_local.control & 255u) >= 2u
          && c_local.color_icw[1] == 0x0919390Cu
          && (rc1skip || (t1_zero && t0_zero))) {
          c_local.control = (c_local.control & ~255u) | 1u;
          c_local.stage_program = c_local.stage_program & 31u;
          if (rc1fix < 12) {
              fprintf(stderr, "[RBX DX11] RC stage1 skip (ic1=0919390C t1_zero=%d t0_zero=%d env=%d) n=%u\n",
                      t1_zero, t0_zero, rc1skip, kept);
              rc1fix++;
          }
      } else if ((c_local.control & 255u) >= 2u
                 && c_local.color_icw[1] == 0x0919390Cu
                 && t1_zero && !t0_zero && rc1fix < 12) {
          fprintf(stderr, "[RBX DX11] RC stage1 keep via t1<-t0 (ic1=0919390C) n=%u\n", kept);
          rc1fix++;
      } }
    /* Black-V0 cel pass skipped above (pre-Unmap). */
    if (!initialize_combiners() || !(s_rc_ps = combiner_shader(c)) || !packed_ext || (gclip && !s_rc_vs_clip))
        use_rc = 0;
    else {
        memset(&rc, 0, sizeof rc);
        rc.hdr[0] = c->control; rc.hdr[1] = c->stage_program;
        rc.hdr[2] = c->final0; rc.hdr[3] = c->final1;
        rc.hdr2[0] = c->final_c0; rc.hdr2[1] = c->final_c1;
        memcpy(rc.fog, b->fog_color, sizeof rc.fog);
        for (stage = 0; stage < 4; stage++) {
            const Nv2aTexture *st = NULL;
            if ((b->stage_valid & (1u << stage)) && ((c->stage_program >> (stage * 5)) & 31u)
                && b->stages)
                st = &b->stages[stage];
            if (st) {
                int normalized = d3d8_format_is_swizzled(st->color)
                              || d3d8_format_dxt_block_bytes(st->color);
                static int whitetex = -1;
                if (whitetex < 0)
                    whitetex = getenv("RECOMP_GPU_WHITETEX") != NULL;
                bound_srv[stage] = get_tex(st);
                bound_smp[stage] = get_sampler(st);
                rc.uvs[stage][0] = normalized ? 1.0f : 1.0f / (float)st->width;
                rc.uvs[stage][1] = normalized ? 1.0f : 1.0f / (float)st->height;
                /* uvs.z==0 → ps_rc substitutes white (texture-vs-combiner probe). */
                rc.uvs[stage][2] = whitetex ? 0.0f : 1.0f;
            }
        }
        /* Widescreen: squeeze into the centred 4:3 unless this is a full-width pass of the frame; sampled wide
         * targets map to their own centred 4:3 (see wide_wsq). */
        wsq = wide_wsq(rt, flat_full && only_rt_textures(bound_srv, 4));
        for (stage = 0; stage < 4; stage++) {
            float k, o;
            wide_uv(bound_srv[stage], wsq, &k, &o);
            rc.uvs[stage][0] *= k;
            rc.uvs[stage][3] = o;
        }
        for (stage = 0; stage < 8; stage++) {
            rc.stages[2 * stage][0] = c->color_icw[stage];
            rc.stages[2 * stage][1] = c->alpha_icw[stage];
            rc.stages[2 * stage][2] = c->color_ocw[stage];
            rc.stages[2 * stage][3] = c->alpha_ocw[stage];
            rc.stages[2 * stage + 1][0] = c->factor0[stage];
            rc.stages[2 * stage + 1][1] = c->factor1[stage];
        }
        if (SUCCEEDED(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)s_rc_cb, 0,
                       D3D11_MAP_WRITE_DISCARD, 0, &m))) {
            memcpy(m.pData, &rc, sizeof rc);
            ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)s_rc_cb, 0);
        }
        memset(&rcb, 0, sizeof rcb);
        rcb.surf[0] = (float)s->width; rcb.surf[1] = (float)s->height;
        rcb.zscale = r->depth_max > 1.0f ? 1.0f / r->depth_max : 1.0f;
        rcb.aref = (float)(r->alpha_ref & 0xFF);
        rcb.atest = r->alpha_test_enable;
        rcb.afunc = r->alpha_func >= 0x200 ? r->alpha_func - 0x200 : r->alpha_func;
        { static int na = -1; if (na < 0) na = getenv("RECOMP_GPU_NOATEST") != NULL;
          if (na && s_frame >= 450) { rcb.atest = 0; rcb.afunc = 8; } }
        { static int rcdbg = -1;
          if (rcdbg < 0) {
              const char *e = getenv("RECOMP_GPU_RCDBG");
              rcdbg = e ? atoi(e) : 0;
          }
          if (rcdbg && s_frame >= 450) rcb.dbg = (uint32_t)rcdbg;
        }
        rcb.textured = bound_srv[0] != NULL;
        rcb.wsq = wsq;
        rcb.clipz = clipz; rcb.nearz = nearz;
        cbuf = s_cbr[s_cb_i++ % CB_RING];
        if (SUCCEEDED(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)cbuf, 0,
                       D3D11_MAP_WRITE_DISCARD, 0, &m))) {
            memcpy(m.pData, &rcb, sizeof rcb);
            ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)cbuf, 0);
        }
        *cbufp = cbuf;
        { static unsigned nlog; if (nlog++ < 8)
            fprintf(stderr, "[RBX DX11] RC draw n=%u stage=%08X fin=%08X/%08X tex0=%08X\n",
                    kept, c->stage_program, c->final0, c->final1,
                    b->stages && (b->stage_valid & 1) ? b->stages[0].offset : 0); }
    }
    return use_rc;
}
/* RECOMP_GPU_VSH_VERIFY: compare what the program's shader wrote for the batch's first vertices (stream-out
 * readback) with the CPU path's result the executor put in g_rbx_gvs.ref. Logs the first mismatches and, every 256
 * checked batches, the counts per program. A debug mode: the readback stalls the pipeline. */
static void gvs_verify(const Nv2aSurface *s, const Nv2aRenderState *r, int prog)
{
    static unsigned long ok[RBX_GVS_NPROG], bad[RBX_GVS_NPROG], nchk, nlog;
    static const char *const fld[8] = { "pos", "c", "d1", "t0", "t1", "t2", "t3", "fog" };
    const RbxGvs *g = &g_rbx_gvs;
    ID3D11Buffer *nb = NULL;
    D3D11_MAPPED_SUBRESOURCE m;
    UINT so_off = 0;
    float zs = r->depth_max > 1.0f ? 1.0f / r->depth_max : 1.0f;
    uint32_t v, k;
    if (!g->nref || !s_gvs_so[prog] || !s_gvs_sob || !s_gvs_sost) return;
    ID3D11DeviceContext_IASetPrimitiveTopology(s_ctx, D3D11_PRIMITIVE_TOPOLOGY_POINTLIST);
    ID3D11DeviceContext_GSSetShader(s_ctx, s_gvs_so[prog], NULL, 0);
    ID3D11DeviceContext_SOSetTargets(s_ctx, 1, &s_gvs_sob, &so_off);
    ID3D11DeviceContext_Draw(s_ctx, g->nref, 0);
    ID3D11DeviceContext_SOSetTargets(s_ctx, 1, &nb, &so_off);
    ID3D11DeviceContext_GSSetShader(s_ctx, NULL, NULL, 0);
    ID3D11DeviceContext_IASetPrimitiveTopology(s_ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_CopyResource(s_ctx, (ID3D11Resource *)s_gvs_sost, (ID3D11Resource *)s_gvs_sob);
    if (FAILED(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)s_gvs_sost, 0, D3D11_MAP_READ, 0, &m))) return;
    for (v = 0; v < g->nref; v++) {
        const RbxGvsRef *e = &g->ref[v];
        const float *gpu = (const float *)m.pData + v * 29;
        float want[29], w = e->pos[3];
        int vbad = 0;
        want[0] = (e->pos[0] / (float)s->width * 2.0f - 1.0f) * w;
        want[1] = (1.0f - e->pos[1] / (float)s->height * 2.0f) * w;
        want[2] = e->pos[2] * zs * w;
        want[3] = w;
        memcpy(want + 4, e->c, 4 * sizeof(float));
        memcpy(want + 8, e->d1, 4 * sizeof(float));
        memcpy(want + 12, e->t, 16 * sizeof(float));
        want[28] = e->fog;
        for (k = 0; k < 29; k++) {
            float a = gpu[k], b = want[k], d = fabsf(a - b), mx = fabsf(a) > fabsf(b) ? fabsf(a) : fabsf(b);
            float tol = (k >= 4 && k < 8) ? 1.01f / 255.0f : 1e-4f + mx * 1e-3f;
            if (d <= tol || (a != a && b != b) || (isinf(a) && a == b)) continue;
            vbad = 1;
            if (nlog < 40) {
                nlog++;
                fprintf(stderr, "[GVS] verify mismatch prog %d vertex %u %s[%u]: gpu %g cpu %g (screen %g %g %g w %g)\n",
                        prog, v, fld[k < 4 ? 0 : k < 28 ? 1 + (k - 4) / 4 : 7], k < 28 ? k % 4 : 0, a, b,
                        e->pos[0], e->pos[1], e->pos[2], e->pos[3]);
            }
        }
        if (vbad) bad[prog]++; else ok[prog]++;
    }
    ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)s_gvs_sost, 0);
    if (++nchk % 256 == 1)
        fprintf(stderr, "[GVS] verify vertices ok/bad: 0B25 %lu/%lu 9CEE %lu/%lu BA9E %lu/%lu 892C %lu/%lu\n",
                ok[1], bad[1], ok[2], bad[2], ok[3], bad[3], ok[4], bad[4]);
}

/* Widescreen: the pass-through program's batch (screen positions r1 * c0 + c1, then the AA scale) is screen-flat and
 * spans the full width. The transforming programs are 3D and always squeeze. */
static int gvs_flat_full(const Nv2aSurface *s, const Nv2aBatch *b, const RbxGvs *g)
{
    uint32_t i, n = b->count;
    float lo = 0, hi = 0, w0 = 0;
    if (g->prog != RBX_GVS_BA9E || !g->in || !g->nin) return 0;
    for (i = 0; i < n; i++) {
        const float *v0 = g->in + (size_t)i * g->nin * 4u;
        float x = (v0[0] * g->k.c0[0] + g->k.c1[0]) * g->k.aa[0];
        if (!i) { lo = hi = x; w0 = v0[3]; continue; }
        if (v0[3] != w0) return 0;
        if (x < lo) lo = x;
        if (x > hi) hi = x;
    }
    return n && lo <= 1.0f && hi >= (float)s->width - 1.0f;
}

/* ---- frame interpolation (on by default at level 1; F1 / Config.exe "Frame interpolation"; RECOMP_RBX_INTERP) ------
 * The game's world steps at a fixed ~30 Hz while it flips 200-800 times a second: the extra flips resubmit the same
 * draws (RECOMP_RBX_SIMRATE measured ~12 identical flips per step). This draws those repeats at in-between positions.
 *
 * Each flip's draws are recorded in order. The first draw that differs from the last flip's (key or content) marks a
 * new tick: the last flip's list becomes the previous tick (A) and the tick's arrival time is noted. Every draw is
 * then matched to A by a key that does not change while an object moves (program, vertex/index count, texture, the
 * model-space vertex data; the n-th draw with a key matches A's n-th) and drawn at lerp(A, current, t), where t runs
 * from 0 at the tick's arrival to 1 one tick interval (smoothed) later, then held there if the next tick is late.
 *
 * Latency: blending from A to the current step shows each step one tick (~33 ms) late. So by default the blend runs
 * ahead by a whole tick instead, lerp(A, current, t + 1): it predicts the next step from the last two, adding no
 * latency over the game's own; the cost is that when something turns, stops or starts, it overshoots by that step's
 * change in motion and snaps back by the same amount on the next tick (invisible at a constant speed). RECOMP_RBX_INTERP_LEAD=0..1 picks the trade-off: 1 (default)
 * predicts, 0 interpolates (smoothest, one tick of added latency), 0.5 halves both. RECOMP_RBX_INTERP_EXTRAP=0/1 is
 * the same as LEAD=0/1.
 *
 *   RECOMP_RBX_INTERP=1  the 3D world: fixed-function batches (terrain, sky, most mission geometry; their composite
 *                        matrix, blended in the executor before the CPU transform) and GPU vertex-program models
 *                        (0B25, 9CEE, 892C: matrix, light and fog constants c96-c110; viewport constants are not).
 *   RECOMP_RBX_INTERP=2  also screen-space draws: pretransformed GPU batches (BA9E) and CPU-path draws (sprites,
 *                        particles, HUD, fixed-function geometry) get their vertex positions blended when the matched
 *                        draw has the same vertex count. Text that changes can slide; that is why it is a second level.
 *
 * Without RECOMP_RBX_INTERP the "Frame interpolation" setting picks the level: On is 1, Off is 0. It can change while
 * the game runs (between frames). RECOMP_RBX_INTERP=0/1/2 overrides the setting for the whole run.
 *
 * RECOMP_RBX_INTERP_DEBUG=1 swings every blended draw +-3 steps back and forth once a second, to show what is covered.
 *
 * A draw with no match, or whose values jump too far (a camera cut, a respawn), is drawn as the game gave it. A tick
 * where most draws have no match (scene change) is not blended at all. RECOMP_GPU_VSH_VERIFY reports mismatches while
 * this is on, since the GPU then draws blended constants on purpose. */
#define IP_MAXD 4096
#define IP_MAXV (256u * 1024u)       /* float4 positions per list */
#define IP_HT   8192                 /* power of two */
#define IP_LIVE_TICKS 8              /* a draw that changed within a tick is left alone for this many ticks */
#define IP_LIVE_TICKS_3D 240         /* ...a 3D one (it changed on a flip too few 3D draws changed to be a tick) */
typedef struct { uint64_t key, ch; int next, prev_i, a_i; uint8_t tick_src, live, pending, changed; uint32_t voff, vn, used; RbxGvsConst k; } IpDraw;
typedef struct {
    IpDraw d[IP_MAXD]; unsigned n, matched;
    unsigned nt, nt_changed;              /* 3D draws this flip, and how many changed from the last flip */
    float v[IP_MAXV * 4]; uint32_t vused;
    uint64_t hkey[IP_HT]; int head[IP_HT], tail[IP_HT]; unsigned cnt[IP_HT], hgen[IP_HT], gen;
} IpList;
static IpList *s_ipA, *s_ipB, *s_ipC;
static int s_ip = -1, s_ip_new, s_ip_valid, s_ip_began;
static int s_ip_env = -1;    /* RECOMP_RBX_INTERP's level when it is set: it overrides the setting */
static int s_ip_menu = 1;    /* the "Frame interpolation" setting (video_apply), On by default */
static float s_ip_lead = 1.0f;   /* 0 = interpolate (one tick behind), 1 = extrapolate (no added latency) */
static unsigned long long s_ip_tick_t, s_ip_T, s_ip_flip_t;   /* tick_t: the current tick's start on a smoothed clock */
static unsigned long long s_ip_arrive_t;                        /* when the current tick's draws actually arrived */
static float s_ip_t;
static uint32_t s_ip_flipno = 1;   /* marks A's draws already matched this flip */
/* Camera motion over the last tick, per kind of 3D draw (1 vertex program, 2 fixed-function): D = C0 * inverse(C1)
 * for a static object, since its matrix is C = X * W (camera/projection X, its own world W) and W cancels. Estimated
 * at each tick from the matched draws (element-wise median, so moving objects do not skew it). Predicts where an
 * unmatched or mismatched draw was last tick (D * C1), and picks among identical copies. */
static float s_ip_D[3][16];
static int s_ip_D_ok[3];
static IpDraw s_ip_synth;
static int s_ip_prechecked;   /* ip_record already checked the pair against the camera prediction */
static int ip_jump(const float *a, const float *b, unsigned n, float rel, float abs_floor);
static void m4_mul(const float *a, const float *b, float *o)
{
    int i, j;
    for (i = 0; i < 4; i++) for (j = 0; j < 4; j++)
        o[4 * i + j] = a[4 * i] * b[j] + a[4 * i + 1] * b[4 + j] + a[4 * i + 2] * b[8 + j] + a[4 * i + 3] * b[12 + j];
}
static int m4_inv(const float *m, float *o)
{
    double inv[16], det;
    int i;
    inv[0] = m[5]*m[10]*m[15] - m[5]*m[11]*m[14] - m[9]*m[6]*m[15] + m[9]*m[7]*m[14] + m[13]*m[6]*m[11] - m[13]*m[7]*m[10];
    inv[4] = -m[4]*m[10]*m[15] + m[4]*m[11]*m[14] + m[8]*m[6]*m[15] - m[8]*m[7]*m[14] - m[12]*m[6]*m[11] + m[12]*m[7]*m[10];
    inv[8] = m[4]*m[9]*m[15] - m[4]*m[11]*m[13] - m[8]*m[5]*m[15] + m[8]*m[7]*m[13] + m[12]*m[5]*m[11] - m[12]*m[7]*m[9];
    inv[12] = -m[4]*m[9]*m[14] + m[4]*m[10]*m[13] + m[8]*m[5]*m[14] - m[8]*m[6]*m[13] - m[12]*m[5]*m[10] + m[12]*m[6]*m[9];
    inv[1] = -m[1]*m[10]*m[15] + m[1]*m[11]*m[14] + m[9]*m[2]*m[15] - m[9]*m[3]*m[14] - m[13]*m[2]*m[11] + m[13]*m[3]*m[10];
    inv[5] = m[0]*m[10]*m[15] - m[0]*m[11]*m[14] - m[8]*m[2]*m[15] + m[8]*m[3]*m[14] + m[12]*m[2]*m[11] - m[12]*m[3]*m[10];
    inv[9] = -m[0]*m[9]*m[15] + m[0]*m[11]*m[13] + m[8]*m[1]*m[15] - m[8]*m[3]*m[13] - m[12]*m[1]*m[11] + m[12]*m[3]*m[9];
    inv[13] = m[0]*m[9]*m[14] - m[0]*m[10]*m[13] - m[8]*m[1]*m[14] + m[8]*m[2]*m[13] + m[12]*m[1]*m[10] - m[12]*m[2]*m[9];
    inv[2] = m[1]*m[6]*m[15] - m[1]*m[7]*m[14] - m[5]*m[2]*m[15] + m[5]*m[3]*m[14] + m[13]*m[2]*m[7] - m[13]*m[3]*m[6];
    inv[6] = -m[0]*m[6]*m[15] + m[0]*m[7]*m[14] + m[4]*m[2]*m[15] - m[4]*m[3]*m[14] - m[12]*m[2]*m[7] + m[12]*m[3]*m[6];
    inv[10] = m[0]*m[5]*m[15] - m[0]*m[7]*m[13] - m[4]*m[1]*m[15] + m[4]*m[3]*m[13] + m[12]*m[1]*m[7] - m[12]*m[3]*m[5];
    inv[14] = -m[0]*m[5]*m[14] + m[0]*m[6]*m[13] + m[4]*m[1]*m[14] - m[4]*m[2]*m[13] - m[12]*m[1]*m[6] + m[12]*m[2]*m[5];
    inv[3] = -m[1]*m[6]*m[11] + m[1]*m[7]*m[10] + m[5]*m[2]*m[11] - m[5]*m[3]*m[10] - m[9]*m[2]*m[7] + m[9]*m[3]*m[6];
    inv[7] = m[0]*m[6]*m[11] - m[0]*m[7]*m[10] - m[4]*m[2]*m[11] + m[4]*m[3]*m[10] + m[8]*m[2]*m[7] - m[8]*m[3]*m[6];
    inv[11] = -m[0]*m[5]*m[11] + m[0]*m[7]*m[9] + m[4]*m[1]*m[11] - m[4]*m[3]*m[9] - m[8]*m[1]*m[7] + m[8]*m[3]*m[5];
    inv[15] = m[0]*m[5]*m[10] - m[0]*m[6]*m[9] - m[4]*m[1]*m[10] + m[4]*m[2]*m[9] + m[8]*m[1]*m[6] - m[8]*m[2]*m[5];
    det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
    if (!(fabs(det) > 1e-30)) return 0;
    for (i = 0; i < 16; i++) o[i] = (float)(inv[i] / det);
    return 1;
}
static int ip_fcmp(const void *a, const void *b) { float x = *(const float *)a, y = *(const float *)b; return (x > y) - (x < y); }
/* After a tick: the camera's motion from the 3D draws that were matched to the previous tick. */
static void ip_estimate_camera(void)
{
    enum { NS = 64 };
    static float samp[NS][16];
    float col[NS];
    int kind;
    for (kind = 1; kind <= 2; kind++) {
        unsigned i, n = 0, cnt = s_ipC->n < IP_MAXD ? s_ipC->n : IP_MAXD, step;
        unsigned matched = 0;
        for (i = 0; i < cnt; i++) if (s_ipC->d[i].tick_src == kind && s_ipC->d[i].a_i >= 0 && !s_ipC->d[i].live) matched++;
        s_ip_D_ok[kind] = 0;
        if (matched < 4) continue;
        step = matched / NS + 1;   /* spread the samples over the frame */
        for (i = 0, matched = 0; i < cnt && n < NS; i++) {
            const IpDraw *d = &s_ipC->d[i];
            float inv[16];
            if (d->tick_src != kind || d->a_i < 0 || d->live) continue;
            if (matched++ % step) continue;
            if (!m4_inv(&d->k.cu[0][0], inv)) continue;
            m4_mul(&s_ipA->d[d->a_i].k.cu[0][0], inv, samp[n]);
            n++;
        }
        if (n < 4) continue;
        for (i = 0; i < 16; i++) {
            unsigned q;
            for (q = 0; q < n; q++) col[q] = samp[q][i];
            qsort(col, n, sizeof col[0], ip_fcmp);
            s_ip_D[kind][i] = col[n / 2];
        }
        s_ip_D_ok[kind] = 1;
    }
}
static unsigned st_ip_cam, st_ip_false, st_ip_ticks, st_ip_blend, st_ip_nomatch, st_ip_jump, st_ip_cut, st_ip_gvs, st_ip_scr, st_ip_live;
/* RECOMP_RBX_INTERP_TRACE=lo-hi (seconds since the first flip): one [IPTR] line per flip with what it drew and where,
 * whether it was published, and the interpolation state. Works with interpolation off too. */
static unsigned s_tr_gvs, s_tr_cpu, s_tr_blend, s_tr_tick, s_tr_ffp; static uint32_t s_tr_gvs_va, s_tr_cpu_va;
static int s_ip_debug;   /* RECOMP_RBX_INTERP_DEBUG=1: swing blended draws back and forth, to see what is covered */

static void ip_list_reset(IpList *l);
/* Switch to level want (0 off). Nothing carries over from before: the next tick starts blending afresh. */
static void ip_set_level(int want)
{
    if (want == s_ip) return;
    if (want && !s_ipA) {
        s_ipA = (IpList *)calloc(1, sizeof *s_ipA); s_ipB = (IpList *)calloc(1, sizeof *s_ipB); s_ipC = (IpList *)calloc(1, sizeof *s_ipC);
        if (!s_ipA || !s_ipB || !s_ipC) {
            free(s_ipA); free(s_ipB); free(s_ipC); s_ipA = s_ipB = s_ipC = NULL;
            fprintf(stderr, "[RBX INTERP] out of memory: off\n");
            want = 0;
            if (want == s_ip) return;
        }
    }
    if (s_ipA) { ip_list_reset(s_ipA); ip_list_reset(s_ipB); ip_list_reset(s_ipC); }
    s_ip_valid = s_ip_new = s_ip_began = 0;
    s_ip_D_ok[1] = s_ip_D_ok[2] = 0;
    s_ip = want;
    if (s_ip)
        fprintf(stderr, "[RBX INTERP] on: level %d (%s), lead %.2f (added latency ~%.0f%% of a tick)%s\n", s_ip,
                s_ip == 1 ? "3D world" : "3D world + screen-space draws", s_ip_lead, (1.0f - s_ip_lead) * 100.0f,
                s_ip_env >= 0 ? ", from RECOMP_RBX_INTERP" : "");
    else
        fprintf(stderr, "[RBX INTERP] off%s\n", s_ip_env >= 0 ? " (RECOMP_RBX_INTERP)" : "");
}

static int ip_on(void)
{
    if (s_ip < 0) {
        const char *e = getenv("RECOMP_RBX_INTERP"), *x = getenv("RECOMP_RBX_INTERP_EXTRAP"), *l = getenv("RECOMP_RBX_INTERP_LEAD");
        if (e && e[0]) {
            s_ip_env = atoi(e);
            if (s_ip_env < 0) s_ip_env = 0;
            if (s_ip_env > 2) s_ip_env = 2;
        }
        if (x && x[0]) s_ip_lead = x[0] != '0' ? 1.0f : 0.0f;
        if (l && l[0]) s_ip_lead = (float)atof(l);
        if (s_ip_lead < 0) s_ip_lead = 0;
        if (s_ip_lead > 1) s_ip_lead = 1;
        { const char *d = getenv("RECOMP_RBX_INTERP_DEBUG"); s_ip_debug = d && d[0] && d[0] != '0'; }
        s_ip = 0;
        ip_set_level(s_ip_env >= 0 ? s_ip_env : s_ip_menu);
        if (!s_ip)   /* starting off is not a change, so ip_set_level said nothing */
            fprintf(stderr, "[RBX INTERP] off%s\n", s_ip_env >= 0 ? " (RECOMP_RBX_INTERP)" : " (Frame interpolation setting)");
    }
    return s_ip;
}
static void ip_list_reset(IpList *l) { l->nt = l->nt_changed = 0; l->n = 0; l->matched = 0; l->vused = 0; l->gen++; if (!l->gen) { memset(l->hgen, 0, sizeof l->hgen); l->gen = 1; } }
static unsigned ip_slot(IpList *l, uint64_t key, int add)
{
    unsigned i = (unsigned)(key ^ (key >> 29)) & (IP_HT - 1), probes;
    for (probes = 0; probes < IP_HT; probes++, i = (i + 1) & (IP_HT - 1)) {
        if (l->hgen[i] != l->gen) {
            if (!add) return ~0u;
            l->hgen[i] = l->gen; l->hkey[i] = key; l->head[i] = l->tail[i] = -1; l->cnt[i] = 0;
            return i;
        }
        if (l->hkey[i] == key) return i;
    }
    return ~0u;
}
/* Start of a flip's draws: the blend factor for the whole flip. */
/* The blend factor for this flip from the smoothed tick clock. t runs from 0 at the tick's (smoothed) start to 1 one
 * interval later. It may start a little below 0 when a tick arrives early, so the motion carries on from where the
 * last frame was instead of jumping ahead, and runs a little past 1 when one is late before it holds. */
static void ip_calc_t(void)
{
    double t = 1.0;
    unsigned long long T = s_ip_T > gb_qf() / 120 ? s_ip_T : gb_qf() / 120;   /* never under 1/120 s */
    if (s_ip_T && s_ip_tick_t) t = (double)(long long)(s_ip_flip_t - s_ip_tick_t) / (double)T;
    if (t < -0.5) t = -0.5;
    if (t > 1.3) t = 1.3;
    s_ip_t = (float)(t + s_ip_lead);
}
static void ip_begin(void)
{
    LARGE_INTEGER q;
    double t;
    if (s_ip_began) return;
    s_ip_began = 1;
    QueryPerformanceCounter(&q); s_ip_flip_t = (unsigned long long)q.QuadPart;
    (void)t;
    ip_calc_t();
    if (s_ip_debug) s_ip_t = (float)(1.0 + 3.0 * sin((double)s_ip_flip_t * 6.283 / (double)gb_qf()));   /* +-3 steps, 1 Hz */
}
static int ip_find(IpList *l, uint64_t key, unsigned o)
{
    unsigned sl = ip_slot(l, key, 0);
    int i;
    if (sl == ~0u) return -1;
    for (i = l->head[sl]; i >= 0 && o; o--) i = l->d[i].next;
    return i;
}
static unsigned long long s_ip_undo_tick_t, s_ip_undo_T, s_ip_undo_arrive_t;
static int s_ip_undo_valid;
static void ip_new_tick(void)
{
    IpList *t = s_ipA;
    unsigned long long gap = s_ip_arrive_t ? s_ip_flip_t - s_ip_arrive_t : 0, qf = gb_qf();
    s_tr_tick = 1;
    s_ip_undo_tick_t = s_ip_tick_t; s_ip_undo_T = s_ip_T; s_ip_undo_valid = s_ip_valid;   /* for a false tick */
    s_ip_undo_arrive_t = s_ip_arrive_t;
    s_ip_new = 1; st_ip_ticks++;
    if (gap && gap < qf / 5) s_ip_T = s_ip_T ? (s_ip_T * 7 + gap) / 8 : gap;   /* smoothed tick interval */
    /* The game steps a fixed amount of game time per tick, but its ticks reach us with ~+-10 ms of jitter (the
     * flip pacing). Timing the blend from each arrival made the motion uneven, so the tick start runs on a clock
     * that advances one interval per tick and is pulled a quarter of the way toward each arrival; it resyncs when
     * an arrival is more than half an interval off (a stall, a pause). */
    if (s_ip_tick_t && s_ip_T) {
        unsigned long long est = s_ip_tick_t + s_ip_T;
        long long diff = (long long)(s_ip_flip_t - est);
        if (diff > (long long)(s_ip_T / 2) || -diff > (long long)(s_ip_T / 2)) est = s_ip_flip_t;
        else est = (unsigned long long)((long long)est + diff / 4);
        s_ip_tick_t = est;
    } else s_ip_tick_t = s_ip_flip_t;
    s_ip_arrive_t = s_ip_flip_t;
    s_ipA = s_ipB; s_ipB = t;      /* the last flip's draws are the previous step; the old A stays in B until the flip ends */
    s_ip_valid = s_ipA->n > 0;
    if (!s_ip_debug) ip_calc_t();
}
/* Record a draw in this flip's list, detect a new tick, and return A's matching draw (or NULL: draw it as given).
 *
 * Only the 3D (GPU vertex-program) draws mark a tick: some screen-space draws (an animated HUD element, particles)
 * change on every flip, so scenes with no 3D draws (menus) are not blended. A "live" draw, one that changed between two flips of the same tick, already moves per flip and is never
 * blended. A screen-space draw that changes before the flip's tick is noticed is drawn as given and sorted out at the
 * end of the flip. */
static const IpDraw *ip_record(uint64_t key, uint64_t ch, const RbxGvsConst *k, const float *pos, uint32_t pstride,
                               uint32_t vn, int tick_src)
{
    IpList *C = s_ipC, *P;
    unsigned j = C->n, o = 0, sl;
    const IpDraw *m = NULL;
    int pi, live = 0, pending = 0;
    s_ip_prechecked = 0;
    int changed = 0;
    ip_begin();
    sl = ip_slot(C, key, 1);
    if (sl != ~0u) o = C->cnt[sl]++;
    P = s_ip_new ? s_ipA : s_ipB;              /* the last flip's list */
    pi = ip_find(P, key, o);
    if (tick_src) {
        /* A 3D draw that is in the last flip too, not known to change on its own, and different from it: a tick. A draw
         * with no counterpart (new, or keyed by data that changes) does not count. If too few 3D draws turn out to
         * have changed, ip_flip takes the tick back and marks those draws live. */
        if (pi >= 0 && P->d[pi].ch != ch) {
            if (!s_ip_new && !P->d[pi].live) { ip_new_tick(); P = s_ipA; }
            changed = 1;
        }
        C->nt++;
        if (changed && s_ip_new) C->nt_changed++;
        if (pi < 0) live = 0;
        else if (P->d[pi].live) live = changed && !s_ip_new ? IP_LIVE_TICKS_3D : s_ip_new ? P->d[pi].live - 1 : P->d[pi].live;
    } else {
        /* live counts down in ticks (IP_LIVE_TICKS after its last change between flips of one tick) */
        int carried = pi < 0 ? 0 : s_ip_new ? (P->d[pi].live ? P->d[pi].live - 1 : 0) : P->d[pi].live;
        if (pi < 0) live = 0;
        else if (P->d[pi].ch == ch || s_ip_new) live = carried;
        else if (!s_ipB->nt) live = IP_LIVE_TICKS;   /* a scene with no 3D draws (menus): no tick to go by, not blended */
        else { live = IP_LIVE_TICKS; pending = 1; }  /* changed, and no tick seen yet: live, unless this flip turns out a tick */
    }
    if (j < IP_MAXD) {
        IpDraw *d = &C->d[j];
        d->key = key; d->ch = ch; d->next = -1; d->vn = 0; d->voff = 0; d->used = 0; d->a_i = -1;
        d->prev_i = pi; d->tick_src = (uint8_t)tick_src; d->live = (uint8_t)live; d->pending = (uint8_t)pending; d->changed = (uint8_t)changed;
        if (k) d->k = *k;
        if (pos && vn && C->vused + vn <= IP_MAXV) {
            uint32_t i; float *w = &C->v[C->vused * 4];
            for (i = 0; i < vn; i++, pos = (const float *)((const uint8_t *)pos + pstride), w += 4) { w[0] = pos[0]; w[1] = pos[1]; w[2] = pos[2]; w[3] = pos[3]; }
            d->voff = C->vused; d->vn = vn; C->vused += vn;
        }
        if (sl != ~0u) { if (C->tail[sl] < 0) C->head[sl] = (int)j; else C->d[C->tail[sl]].next = (int)j; C->tail[sl] = (int)j; }
        C->n = j + 1;
    }
    if (tick_src) st_ip_gvs++; else st_ip_scr++;
    if (live) { st_ip_live++; return NULL; }
    if (s_ip_valid) {
        int ai = ip_find(s_ipA, key, o);
        float pred[16];
        const float *target = k ? &k->cu[0][0] : NULL;
        int kind = tick_src;
        if (k && kind >= 1 && kind <= 2 && s_ip_D_ok[kind]) { m4_mul(s_ip_D[kind], &k->cu[0][0], pred); target = pred; }
        if (ai >= 0 && k) {
            /* The same mesh drawn many times (a city of identical buildings) shares one key, and which copies the game
             * draws changes as they enter and leave view, so the n-th copy now need not be the n-th last tick: pairing
             * by order blended buildings toward their neighbours. Take the unused copy whose matrix is closest. */
            unsigned as = ip_slot(s_ipA, key, 0);
            int i, best = -1;
            float bd = 0;
            for (i = s_ipA->head[as]; i >= 0; i = s_ipA->d[i].next) {
                const float *a = &s_ipA->d[i].k.cu[0][0], *c = target, *cur = &k->cu[0][0];
                float dsum = 0, dcur = 0;
                unsigned q;
                if (s_ipA->d[i].used == s_ip_flipno) continue;
                /* Closest to where the camera's motion puts it (static scenery) or to where it is now (things that
                 * move with the camera, like the player's own mech, whose left and right limbs share one mesh). */
                for (q = 0; q < 16; q++) { dsum += fabsf(a[q] - c[q]); dcur += fabsf(a[q] - cur[q]); }
                if (dcur < dsum) dsum = dcur;
                if (best < 0 || dsum < bd) { best = i; bd = dsum; }
            }
            ai = best;
        }
        if (ai >= 0) { m = &s_ipA->d[ai]; s_ipA->d[ai].used = s_ip_flipno; }
        s_ip_prechecked = target == pred;
        /* A mesh the last tick did not draw at all is new to the scene (a menu screen that just came in, a model that
         * just appeared), not scenery that scrolled out of reach of its copies: the camera estimate, made from the
         * previous screen's draws (a spinning planet behind a menu), says nothing about it. Draw it as given. */
        if (!m && target == pred && ip_find(s_ipA, key, 0) < 0) target = NULL;
        if (target && target == pred && (!m || (ip_jump(&m->k.cu[0][0], pred, 16, 0.5f, 1e-3f) && ip_jump(&m->k.cu[0][0], &k->cu[0][0], 16, 0.5f, 1e-3f)))) {
            /* No counterpart, or one far from where the camera's motion puts this object: assume it is static and
             * moved only with the camera, so it stays in step with the buildings around it. Checked against the
             * prediction as well as the current matrix, so a fast turn (a big change for everything) is not a jump, and
             * neither is something that moves with the camera. */
            s_ip_synth.k = *k;
            memcpy(&s_ip_synth.k.cu[0][0], pred, sizeof pred);
            m = &s_ip_synth; st_ip_cam++;
        }
        if (j < IP_MAXD) C->d[j].a_i = m && m != &s_ip_synth ? (int)(m - s_ipA->d) : -1;
        if (m) C->matched++; else st_ip_nomatch++;
    }
    return m;
}
/* End of a flip: this flip's list becomes the comparison for the next one. A tick whose draws mostly had no match in
 * the previous one (scene change, loading) stops blending until the next tick. */
static void ip_flip(void)
{
    IpList *t;
    if (!s_ip_began) return;
    if (s_ip_new && s_ipC->nt_changed * 10 < s_ipC->nt) {
        /* False tick: under a tenth of the 3D draws changed, so those few animate on their own (inline geometry, water,
         * effects). Mark them live, so they neither mark ticks nor get blended, and put the tick state back. */
        unsigned i, n = s_ipC->n < IP_MAXD ? s_ipC->n : IP_MAXD;
        for (i = 0; i < n; i++) if (s_ipC->d[i].tick_src && s_ipC->d[i].changed) s_ipC->d[i].live = IP_LIVE_TICKS_3D;
        t = s_ipA; s_ipA = s_ipB; s_ipB = t;            /* old A back; the last flip goes where B is dropped below */
        s_ip_tick_t = s_ip_undo_tick_t; s_ip_T = s_ip_undo_T; s_ip_valid = s_ip_undo_valid; s_ip_arrive_t = s_ip_undo_arrive_t;
        s_ip_new = 0; st_ip_ticks--; st_ip_false++;
    }
    if (s_ip_new) {   /* screen-space draws that changed before the tick was noticed: live only if they were before */
        unsigned i, n = s_ipC->n < IP_MAXD ? s_ipC->n : IP_MAXD;
        for (i = 0; i < n; i++) {
            IpDraw *d = &s_ipC->d[i];
            if (d->pending) { d->live = d->prev_i >= 0 && s_ipA->d[d->prev_i].live ? s_ipA->d[d->prev_i].live - 1 : 0; d->pending = 0; }
        }
        if (s_ipC->n >= 8 && s_ipC->matched * 2 < s_ipC->n) { s_ip_valid = 0; st_ip_cut++; s_ip_D_ok[1] = s_ip_D_ok[2] = 0; }
        else ip_estimate_camera();
    }
    t = s_ipB; s_ipB = s_ipC; s_ipC = t;
    ip_list_reset(s_ipC);
    s_ip_new = 0; s_ip_began = 0;
    if (!++s_ip_flipno) s_ip_flipno = 1;
    {   /* with RECOMP_GPU_STATS or RECOMP_RBX_SIMRATE: one line every 2 s */
        static DWORD last;
        DWORD now = GetTickCount();
        if (!last) last = now;
        if (now - last >= 2000) {
            static unsigned quiet;
            if (s_verbose || sr_on() || !(quiet++ % 5))   /* otherwise every 10 s, so players' logs carry it */
                fprintf(stderr, "[RBX INTERP] ticks=%u false_ticks=%u tick_ms=%.1f draws_3d=%u draws_2d=%u blended=%u live=%u no_match=%u jumps=%u cuts=%u camera_fill=%u\n", st_ip_ticks,
                        st_ip_false, s_ip_T ? (double)s_ip_T * 1000.0 / (double)gb_qf() : 0.0, st_ip_gvs, st_ip_scr, st_ip_blend, st_ip_live,
                        st_ip_nomatch, st_ip_jump, st_ip_cut, st_ip_cam);
            st_ip_cam = st_ip_false = st_ip_ticks = st_ip_blend = st_ip_nomatch = st_ip_jump = st_ip_cut = st_ip_gvs = st_ip_scr = st_ip_live = 0;
            last = now;
        }
    }
}
static float ip_mix(float a, float b, float t) { return a + (b - a) * t; }
/* Too far to blend: a cut or a teleport. Measured against the size of the whole set of values (for a matrix, its
 * largest entry): per-row scales misfired on a building near the screen centre, whose x translation is small, so a
 * normal step looked like a jump. */
static int ip_jump(const float *a, const float *b, unsigned n, float rel, float abs_floor)
{
    unsigned i;
    float d = 0, m = 0;
    for (i = 0; i < n; i++) {
        float e = fabsf(a[i] - b[i]);
        if (e > d) d = e;
        if (fabsf(a[i]) > m) m = fabsf(a[i]);
        if (fabsf(b[i]) > m) m = fabsf(b[i]);
    }
    return d > abs_floor && d > rel * m;
}
static uint64_t ip_hash(uint64_t h, const void *p, size_t n)
{
    const uint8_t *b = (const uint8_t *)p; size_t i;
    for (i = 0; i < n; i++) h = (h ^ b[i]) * 1099511628211ULL;
    return h;
}
/* A matrix whose w row ignores x, y and z is orthographic: a screen-space draw (HUD, aiming marker, menus, title
 * screen). Level 1 is the 3D world only, and the camera estimate means nothing for these, so they are left alone. */
static int ip_persp(const float *m)
{
    float w = fabsf(m[12]) + fabsf(m[13]) + fabsf(m[14]), big = 0.0f;
    unsigned i;
    for (i = 0; i < 16; i++) if (fabsf(m[i]) > big) big = fabsf(m[i]);
    return w > big * 1e-6f;
}
/* Level 1 and screen-space draws. A HUD element that follows something in the world (the target marker around an
 * enemy, the lock-on brackets) is placed by the game once per tick, at the tick's screen position, while the world
 * under it is blended: left alone it steps 30 times a second against a target that moves smoothly, and shakes.
 * Such a marker moves as one piece, so level 1 blends a screen-space draw when every vertex moved by the same
 * amount since the last tick (within a pixel or two, which allows the slow growth of a marker as its target comes
 * closer). Text whose glyphs change, a radar sweep or an animated gauge do not move as one piece and stay as the
 * game drew them; a draw that did not move is the same either way. Level 2 blends every matched screen-space draw.
 * pos/stride: the matched draw's positions as recorded (x, y first); cur/cstride: this tick's. */
static int ip_screen_rigid(const float *pos, uint32_t pstride, const float *cur, uint32_t cstride, uint32_t nv)
{
    float dx0, dy0, minx = 1e30f, maxx = -1e30f, miny = 1e30f, maxy = -1e30f, tol;
    uint32_t i;
    if (s_ip >= 2) return 1;
    if (!nv) return 0;
    dx0 = cur[0] - pos[0]; dy0 = cur[1] - pos[1];
    for (i = 0; i < nv; i++) {
        const float *c = (const float *)((const uint8_t *)cur + (size_t)i * cstride);
        if (c[0] < minx) minx = c[0];
        if (c[0] > maxx) maxx = c[0];
        if (c[1] < miny) miny = c[1];
        if (c[1] > maxy) maxy = c[1];
    }
    tol = 1.5f + 0.04f * ((maxx - minx) > (maxy - miny) ? (maxx - minx) : (maxy - miny));
    if (fabsf(dx0) + fabsf(dy0) < 0.25f) return 0;   /* did not move: nothing to blend */
    for (i = 1; i < nv; i++) {
        const float *a = (const float *)((const uint8_t *)pos + (size_t)i * pstride);
        const float *c = (const float *)((const uint8_t *)cur + (size_t)i * cstride);
        if (fabsf((c[0] - a[0]) - dx0) > tol || fabsf((c[1] - a[1]) - dy0) > tol) return 0;
    }
    return 1;
}
/* GPU vertex-program batch: blended constants into *k, and for a pretransformed batch (level 2, or a rigid move at
 * level 1: ip_screen_rigid) its blended positions in a scratch copy of the inputs (*in). */
static void ip_gvs(int prog, const Nv2aBatch *b, const RbxGvs *g, uint32_t nidx, RbxGvsConst *k, const float **in)
{
    static float *scratch; static uint32_t scratch_n;
    uint64_t key = 1469598103934665603ULL, ch;
    uint32_t tex = b->texture ? b->texture->offset : 0, nv = b->count, stride = g->nin * 16u;
    const IpDraw *m;
    int screen = prog == RBX_GVS_BA9E;
    if (!screen && !ip_persp(&g->k.cu[0][0])) return;
    key = ip_hash(key, &prog, sizeof prog); key = ip_hash(key, &nv, sizeof nv); key = ip_hash(key, &nidx, sizeof nidx);
    key = ip_hash(key, &tex, sizeof tex);
    if (nv) {   /* model-space data identifies the object; screen-space positions move, so skip input 0 there */
        uint32_t skip = screen ? 16u : 0u;
        key = ip_hash(key, (const uint8_t *)g->in + skip, stride - skip);
        key = ip_hash(key, (const uint8_t *)g->in + (size_t)(nv - 1) * stride + skip, stride - skip);
    }
    ch = ip_hash(key, &g->k, offsetof(RbxGvsConst, aa));
    if (screen) { uint32_t i, n = nv < 64 ? nv : 64; for (i = 0; i < n; i++) ch = ip_hash(ch, (const uint8_t *)g->in + (size_t)i * stride, 16); }
    m = ip_record(key, ch, &g->k, screen ? g->in : NULL, stride, screen ? nv : 0, !screen);
    if (!m || !s_ip_valid) return;
    if (!screen) {
        const float *a = &m->k.cu[0][0], *c = &g->k.cu[0][0];
        float *o = &k->cu[0][0];
        unsigned i;
        if (!s_ip_prechecked && ip_jump(&m->k.cu[0][0], &g->k.cu[0][0], 16, 1.0f, 1e-3f)) { st_ip_jump++; return; }   /* c96-c99: the matrix */
        for (i = 0; i < 15 * 4; i++) o[i] = ip_mix(a[i], c[i], s_ip_t);
        st_ip_blend++; s_tr_blend++;
    } else {
        IpList *A = s_ipA;
        uint32_t i;
        float *w;
        if (m->vn != nv || !nv) return;
        if (!ip_screen_rigid(&A->v[m->voff * 4], 16u, (const float *)g->in, stride, nv)) return;
        if (scratch_n < nv * stride) { free(scratch); scratch = (float *)malloc(nv * stride); scratch_n = scratch ? nv * stride : 0; if (!scratch) return; }
        memcpy(scratch, g->in, (size_t)nv * stride);
        for (i = 0; i < nv; i++)
            if (ip_jump(&A->v[(m->voff + i) * 4], (const float *)((const uint8_t *)g->in + (size_t)i * stride), 2, 0.0f, 160.0f)) { st_ip_jump++; return; }
        for (i = 0, w = scratch; i < nv; i++, w = (float *)((uint8_t *)w + stride)) {
            const float *a = &A->v[(m->voff + i) * 4];
            w[0] = ip_mix(a[0], w[0], s_ip_t); w[1] = ip_mix(a[1], w[1], s_ip_t); w[2] = ip_mix(a[2], w[2], s_ip_t); w[3] = ip_mix(a[3], w[3], s_ip_t);
        }
        *in = scratch;
        st_ip_blend++; s_tr_blend++;
    }
}
/* Fixed-function batch (patches/nv2a_pb_exec_rbx.c, before it transforms the batch on the CPU): the mission world,
 * terrain and sky are fixed-function, and their composite matrix (world * view * projection * viewport) is what the
 * camera moves. Blended like a vertex program's matrix, so clipping and culling see the blended geometry. Returns 1
 * if composite was changed; the executor restores it after the batch. Its CPU-path draws are then not blended again. */
static int s_ip_in_ffp;
int rbx_ip_ffp_begin(const void *key_data, size_t key_bytes, float composite[16])
{
    RbxGvsConst k;
    const IpDraw *m;
    uint64_t key = 1469598103934665603ULL, ch;
    unsigned i;
    s_ip_in_ffp = 1;
    if (!ip_on()) return 0;
    if (!ip_persp(composite)) return 0;
    key = ip_hash(key, "ffp", 3);
    key = ip_hash(key, key_data, key_bytes);
    ch = ip_hash(key, composite, 16 * sizeof(float));
    memset(&k, 0, sizeof k);
    memcpy(&k.cu[0][0], composite, 16 * sizeof(float));
    s_tr_ffp++;
    m = ip_record(key, ch, &k, NULL, 0, 0, 2);   /* kind 2: fixed-function */
    if (!m || !s_ip_valid) return 0;
    if (s_ip_D_ok[2] && m != &s_ip_synth) {
        /* The fixed-function world (terrain, buildings) is static, so draw it at the camera's blended
         * position, D * current, instead of trusting a per-draw pairing; the whole city then moves as one. Only a
         * draw whose pair says it moved with the camera (close to its current matrix, far from the camera
         * prediction: the player's own mech) keeps its pair. */
        float pred[16];
        m4_mul(s_ip_D[2], composite, pred);
        if (!ip_jump(&m->k.cu[0][0], pred, 16, 0.05f, 1e-3f) || ip_jump(&m->k.cu[0][0], composite, 16, 0.05f, 1e-3f)) {
            memcpy(&s_ip_synth.k.cu[0][0], pred, sizeof pred);
            m = &s_ip_synth;
        }
    }
    if (!s_ip_prechecked && ip_jump(&m->k.cu[0][0], composite, 16, 1.0f, 1e-3f)) { st_ip_jump++; return 0; }
    for (i = 0; i < 16; i++) composite[i] = ip_mix((&m->k.cu[0][0])[i], composite[i], s_ip_t);
    st_ip_blend++; s_tr_blend++;
    return 1;
}
void rbx_ip_ffp_end(void) { s_ip_in_ffp = 0; }

/* CPU-path batch (level 2): a copy of the batch with blended positions, or b itself. */
static const Nv2aBatch *ip_cpu(const Nv2aSurface *s, const Nv2aBatch *b, Nv2aBatch *out)
{
    static Nv2aVertex *scratch; static uint32_t scratch_n;
    const Nv2aRenderState *r = b->state;
    uint64_t key = 1469598103934665603ULL, ch;
    uint32_t i, nv = b->count, n = nv < 256 ? nv : 256, tex = b->texture ? b->texture->offset : 0, idx = g_rbx_bidx ? g_rbx_bidx_n : 0;
    uint32_t st[4] = { r->blend_enable | r->blend_src << 1, r->blend_dst, r->depth_test_enable | r->alpha_test_enable << 1, r->color_mask };
    const IpDraw *m;
    IpList *A;
    if (s_ip < 1 || !nv || s_ip_in_ffp) return b;
    key = ip_hash(key, &nv, sizeof nv); key = ip_hash(key, &idx, sizeof idx); key = ip_hash(key, &tex, sizeof tex);
    key = ip_hash(key, st, sizeof st); key = ip_hash(key, &s->color_va, sizeof s->color_va);
    ch = key;
    for (i = 0; i < n; i++) { int32_t q[3] = { (int32_t)(b->vertices[i].x * 16), (int32_t)(b->vertices[i].y * 16), (int32_t)b->vertices[i].diffuse }; ch = ip_hash(ch, q, sizeof q); }
    m = ip_record(key, ch, NULL, &b->vertices[0].x, (uint32_t)sizeof(Nv2aVertex), nv, 0);
    if (!m || !s_ip_valid || m->vn != nv) return b;
    A = s_ipA;
    for (i = 0; i < nv; i++) {
        const float *a = &A->v[(m->voff + i) * 4];
        const Nv2aVertex *v = &b->vertices[i];
        if (fabsf(a[0] - v->x) > (float)s->width / 8 || fabsf(a[1] - v->y) > (float)s->height / 8 || (a[3] > 0) != (v->rhw > 0)) { st_ip_jump++; return b; }
    }
    if (!ip_screen_rigid(&A->v[m->voff * 4], 16u, &b->vertices[0].x, (uint32_t)sizeof(Nv2aVertex), nv)) return b;
    if (scratch_n < nv) { free(scratch); scratch = (Nv2aVertex *)malloc(nv * sizeof *scratch); scratch_n = scratch ? nv : 0; if (!scratch) return b; }
    memcpy(scratch, b->vertices, nv * sizeof *scratch);
    for (i = 0; i < nv; i++) {
        const float *a = &A->v[(m->voff + i) * 4];
        scratch[i].x = ip_mix(a[0], scratch[i].x, s_ip_t); scratch[i].y = ip_mix(a[1], scratch[i].y, s_ip_t);
        scratch[i].z = ip_mix(a[2], scratch[i].z, s_ip_t); scratch[i].rhw = ip_mix(a[3], scratch[i].rhw, s_ip_t);
    }
    *out = *b; out->vertices = scratch;
    st_ip_blend++; s_tr_blend++;
    return out;
}

/* A batch the executor handed over raw (g_rbx_gvs.prog set): the program's vertex shader transforms, clips and
 * culls; the combiners draw it. b->count is the number of unique vertices, g_rbx_gvs.in their inputs. */
static void gb_draw_gvs(const Nv2aSurface *s, const Nv2aBatch *b)
{
    const RbxGvs *g = &g_rbx_gvs;
    const Nv2aRenderState *r = b->state;
    const int prog = g->prog;
    const float nearz = (g_rbx_gpuclip & 2) ? 1.0f : 0.0f;
    uint32_t need = b->count * g->nin * 16u, ineed = g_rbx_bidx_n * 4u, nidx = g_rbx_bidx_n;
    ID3D11ShaderResourceView *bound_srv[4] = { 0, 0, 0, 0 };
    ID3D11SamplerState *bound_smp[4] = { 0, 0, 0, 0 };
    ID3D11Buffer *cbuf = NULL, *gcb;
    D3D11_MAPPED_SUBRESOURCE m;
    D3D11_VIEWPORT vp;
    ID3D11RenderTargetView *rtv;
    Nv2aTexture dflt;
    UINT stride = g->nin * 16u, voff;
    float bf[4];
    unsigned stage;
    Rt *rt;
    Ds *ds;
    RbxGvsConst ip_k;          /* the constants drawn with: the game's, or blended (RECOMP_RBX_INTERP) */
    const float *ip_in;

    if (prog <= RBX_GVS_OFF || prog >= RBX_GVS_NPROG || !s_gvs_vs[prog] || !b->combiner || !g_rbx_bidx || !nidx
        || ineed > IB_BYTES || need > VB_BYTES)
        return;
    if (s_cull_on && r->cull_enable && r->cull_face == 0x408) { st_culled += nidx / 3; return; }
    if (g->black) {   /* the black-V0 cel pass, as in gb_draw_impl */
        static int skipblk = -1;
        if (skipblk < 0) { const char *e = getenv("RECOMP_GPU_SKIPBLACK"); skipblk = !(e && e[0] == '0'); }
        if (skipblk) return;
    }
    rt = get_rt(s);
    if (!rt) return;
    ds = get_ds(s, r->zeta_va);
    if (!rc_setup(s, b, b->count, g->t0_zero, g->t1_zero, 1, 1, g_rbx_clip_z, nearz, bound_srv, bound_smp, &cbuf,
                  rt, rt->ex && gvs_flat_full(s, b, g)) || !cbuf) {
        static unsigned nf;
        if (nf++ < 8) fprintf(stderr, "[RBX DX11] GPU vertex program batch dropped: combiners unavailable\n");
        return;
    }
    /* vertices: raw inputs into the vertex ring at a byte offset; the ring stays Nv2aVertex-aligned for the others */
    s_tr_gvs++; s_tr_gvs_va = s->color_va;
    ip_k = g->k; ip_in = g->in;
    if (ip_on()) ip_gvs(prog, b, g, nidx, &ip_k, &ip_in);
    if (s_vb_off + need > VB_BYTES) {
        s_vb_off = 0; s_vb_i = (s_vb_i + 1) % VB_RING; s_vb = s_vbr[s_vb_i];
        s_xvb = s_xvbr[s_vb_i]; s_xvb_off = 0;
    }
    if (FAILED(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)s_vb, 0,
               s_vb_off == 0 ? D3D11_MAP_WRITE_DISCARD : D3D11_MAP_WRITE_NO_OVERWRITE, 0, &m)))
        return;
    memcpy((uint8_t *)m.pData + s_vb_off, ip_in, need);
    ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)s_vb, 0);
    voff = s_vb_off;
    s_vb_off += need;
    s_vb_off = (s_vb_off + (uint32_t)sizeof(Nv2aVertex) - 1) / (uint32_t)sizeof(Nv2aVertex) * (uint32_t)sizeof(Nv2aVertex);
    if (s_vb_off > VB_BYTES) s_vb_off = VB_BYTES;
    gcb = s_gvs_cbr[s_gvs_cb_i++ % GVS_CB_RING];
    if (FAILED(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)gcb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m)))
        return;
    memcpy(m.pData, &ip_k, sizeof ip_k);
    if (sr_on()) { s_sr_world = sr_fold(s_sr_world, &prog, sizeof prog); s_sr_world = sr_fold(s_sr_world, &g->k, offsetof(RbxGvsConst, aa)); }
    ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)gcb, 0);

    sf_flush();
    bf[0] = ((r->blend_color >> 16) & 255) / 255.0f; bf[1] = ((r->blend_color >> 8) & 255) / 255.0f;
    bf[2] = (r->blend_color & 255) / 255.0f; bf[3] = ((r->blend_color >> 24) & 255) / 255.0f;
    rtv = rt->rtv;
    ID3D11DeviceContext_OMSetRenderTargets(s_ctx, 1, &rtv, ds ? ds->dsv : NULL);
    ID3D11DeviceContext_OMSetBlendState(s_ctx, get_blend(r), bf, 0xFFFFFFFFu);
    ID3D11DeviceContext_OMSetDepthStencilState(s_ctx, get_dss(r, ds != NULL), r->stencil_ref & 0xFF);
    {   /* Versus split screen: keep this camera's draws on its half (see rbx_d3d11_clear_rect). */
        int split = s_rs_split && s_split_va && s_split_va == s->color_va;
        if (split) {
            D3D11_RECT rect = { 0, s_split_top * s_scale, rt_full_w(rt) * s_scale, s_split_bottom * s_scale };
            ID3D11DeviceContext_RSSetScissorRects(s_ctx, 1, &rect);
        }
        ID3D11DeviceContext_RSSetState(s_ctx, clip_rs(r, split));
    }
    { static float po = -1.0f; if (po < 0) { const char *e = getenv("RECOMP_GPU_PIXOFS"); po = e ? (float)atof(e) : 0.5f; if (po < 0) po = 0; }
      vp.TopLeftX = vp.TopLeftY = po * (float)s_scale; }
    vp.Width = (float)(rt_full_w(rt) * (uint32_t)s_scale); vp.Height = (float)(s->height * (uint32_t)s_scale);
    vp.MinDepth = 0; vp.MaxDepth = 1;
    ID3D11DeviceContext_RSSetViewports(s_ctx, 1, &vp);
    ID3D11DeviceContext_IASetInputLayout(s_ctx, s_gvs_il[prog]);
    ID3D11DeviceContext_IASetPrimitiveTopology(s_ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_IASetVertexBuffers(s_ctx, 0, 1, &s_vb, &stride, &voff);
    ID3D11DeviceContext_VSSetShader(s_ctx, s_gvs_vs[prog], NULL, 0);
    ID3D11DeviceContext_PSSetShader(s_ctx, s_rc_ps, NULL, 0);
    ID3D11DeviceContext_VSSetConstantBuffers(s_ctx, 0, 1, &cbuf);
    ID3D11DeviceContext_VSSetConstantBuffers(s_ctx, 2, 1, &gcb);
    ID3D11DeviceContext_PSSetConstantBuffers(s_ctx, 0, 1, &cbuf);
    ID3D11DeviceContext_PSSetConstantBuffers(s_ctx, 1, 1, &s_rc_cb);
    memset(&dflt, 0, sizeof dflt); dflt.addr_u = dflt.addr_v = 3;
    for (stage = 0; stage < 4; stage++) {
        ID3D11ShaderResourceView *ssrv = bound_srv[stage] ? bound_srv[stage] : s_white_srv;
        ID3D11SamplerState *ssmp = bound_smp[stage] ? bound_smp[stage] : get_sampler(&dflt);
        ID3D11DeviceContext_PSSetShaderResources(s_ctx, stage, 1, &ssrv);
        ID3D11DeviceContext_PSSetSamplers(s_ctx, stage, 1, &ssmp);
    }
    if (s_ib_off + ineed > IB_BYTES) { s_ib_off = 0; s_ib_i = (s_ib_i + 1) % VB_RING; }
    if (FAILED(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)s_ibr[s_ib_i], 0,
               s_ib_off == 0 ? D3D11_MAP_WRITE_DISCARD : D3D11_MAP_WRITE_NO_OVERWRITE, 0, &m)))
        return;
    memcpy((uint8_t *)m.pData + s_ib_off, g_rbx_bidx, ineed);
    ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)s_ibr[s_ib_i], 0);
    ID3D11DeviceContext_IASetIndexBuffer(s_ctx, s_ibr[s_ib_i], DXGI_FORMAT_R32_UINT, 0);
    ID3D11DeviceContext_DrawIndexed(s_ctx, nidx, s_ib_off / 4u, 0);
    s_ib_off += ineed;
    if (s_flush_mode >= 3) ID3D11DeviceContext_Flush(s_ctx);
    gvs_verify(s, r, prog);
    rt->dirty = 1; rt->shown_dirty = 1;
    rt->resolve_pending = rt->msaa != NULL;
    st_draws++; st_tris += nidx / 3;
}
static void gb_draw_impl(const Nv2aSurface *s, const Nv2aBatch *b)
{
    Rt *rt;
    Ds *ds;
    const Nv2aRenderState *r = b->state;
    D3D11_MAPPED_SUBRESOURCE m;
    D3D11_VIEWPORT vp;
    uint32_t i, kept = 0, need, xneed;
    int strip_odd = 0;
    const int indexed = g_rbx_bidx && g_rbx_bidx_n && s_ibr[0] && g_rbx_bidx_n * 4u <= IB_BYTES;
    const int gclip = indexed && g_rbx_gpuclip;
    const float clipz = g_rbx_clip_z, nearz = (g_rbx_gpuclip & 2) ? 1.0f : 0.0f;
    /* Built in CPU memory and copied into the mapped buffers in one go: the
     * mapped pointers are write-combined, and the loop below reads back
     * what it wrote (strip test, t1 fix-up, ext copy, SKIPBLACK). Reads
     * from WC memory are uncached -- that was ~85% of mission CPU time. */
    static Nv2aVertex *s_stage_v;
    static RcVertExt *s_stage_x;
    Nv2aVertex *dst, *mdst;
    RcVertExt *xdst = NULL, *mxdst = NULL;
    Cb cb;
    ID3D11ShaderResourceView *srv;
    ID3D11SamplerState *smp = NULL;
    ID3D11RenderTargetView *rtv;
    float bf[4];
    ID3D11BlendState *bs;
    ID3D11DepthStencilState *dss;
    UINT stride = sizeof(Nv2aVertex), off = 0;
    UINT xstride = (UINT)sizeof(RcVertExt), xoff = 0;
    int layer_draw = 0, packed_ext = 0;
    Nv2aBatch ip_b;            /* RECOMP_RBX_INTERP=2: the batch with blended positions */

    if (!gb_ready() || !b->count) return;
    if (g_rbx_gvs.prog) { gb_draw_gvs(s, b); return; }
    s_tr_cpu++; s_tr_cpu_va = s->color_va;
    if (ip_on() >= 1) b = ip_cpu(s, b, &ip_b);
    if (sr_on()) {   /* RECOMP_RBX_SIMRATE: quantised positions, colour and UVs of (up to) the first 256 vertices */
        uint32_t k, n = b->count < 256u ? b->count : 256u;
        for (k = 0; k < n; k++) {
            const Nv2aVertex *v = &b->vertices[k];
            int32_t q[5] = { (int32_t)(v->x * 16), (int32_t)(v->y * 16), (int32_t)v->diffuse, (int32_t)(v->u * 16), (int32_t)(v->v * 16) };
            s_sr_scr = sr_fold(s_sr_scr, q, sizeof q);
        }
    }
    if (gclip && s_cull_on && r->cull_enable && r->cull_face == 0x408) { st_culled += g_rbx_bidx_n / 3; return; }
    if (s_fh < 0) s_fh = getenv("RECOMP_GPU_FRAMEHASH") != NULL;
    if (s_fh > 0) { uint32_t k; uint64_t h = fh_dh * 1099511628211ULL + b->count; for (k = 0; k < b->count; k++) { const Nv2aVertex *v = &b->vertices[k]; h = (h ^ (uint64_t)(int)(v->x * 16)) * 1099511628211ULL; h = (h ^ (uint64_t)(int)(v->y * 16)) * 1099511628211ULL; h = (h ^ v->diffuse) * 1099511628211ULL; h = (h ^ (uint64_t)(int)(v->u * 16)) * 1099511628211ULL; h = (h ^ (uint64_t)(int)(v->v * 16)) * 1099511628211ULL; } if (b->texture) { uint32_t n = tex_bytes(b->texture); h = (h ^ hash_bytes((const uint8_t *)xbox_GetMemoryOffset() + b->texture->offset, n)) * 1099511628211ULL; } h = (h ^ (r->blend_enable * 7u + r->blend_src * 131u + r->blend_dst * 977u + r->alpha_test_enable * 13u + r->depth_test_enable * 17u + r->color_mask)) * 1099511628211ULL; { uint64_t vh = 1469598103934665603ULL, th = 0; for (k = 0; k < b->count; k++) { const Nv2aVertex *v = &b->vertices[k]; vh = (vh ^ (uint64_t)(int)(v->x * 16)) * 1099511628211ULL; vh = (vh ^ (uint64_t)(int)(v->y * 16)) * 1099511628211ULL; vh = (vh ^ v->diffuse) * 1099511628211ULL; vh = (vh ^ (uint64_t)(int)(v->u * 16)) * 1099511628211ULL; vh = (vh ^ (uint64_t)(int)(v->v * 16)) * 1099511628211ULL; } if (b->texture) th = hash_bytes((const uint8_t *)xbox_GetMemoryOffset() + b->texture->offset, tex_bytes(b->texture)); if (fh_nd < 256) { FdRec *q = &fd_cur[fh_nd]; q->vh = vh; q->th = th; q->x0 = b->vertices[0].x; q->y0 = b->vertices[0].y; q->n = b->count; q->w = b->texture ? b->texture->width : 0; q->h = b->texture ? b->texture->height : 0; q->fmt = b->texture ? b->texture->color : 0; q->c0 = b->vertices[0].diffuse; q->st = r->blend_enable * 1000 + r->blend_src; } } fh_dh = h; fh_nd++; }
    { static int gr = -1; if (gr < 0) gr = getenv("RECOMP_GPU_GRAD") != NULL; if (gr && b->count == 12 && !b->texture && (s_frame % 10) == 0) fprintf(stderr, "[GRAD] f%u y0=%.1f c0=%08X y2=%.1f c2=%08X y8=%.1f c8=%08X z=%.0f rhw=%g bl=%u %X/%X\n", s_frame, b->vertices[0].y, b->vertices[0].diffuse, b->vertices[2].y, b->vertices[2].diffuse, b->vertices[8].y, b->vertices[8].diffuse, b->vertices[0].z, b->vertices[0].rhw, b->state->blend_enable, b->state->blend_src, b->state->blend_dst); }
    if (s_trace && (s_frame % 150) == 60) {
        const Nv2aVertex *v0 = &b->vertices[0];
        fprintf(stderr, "[GPUTR] f%u va=%08X n=%u tex=%s %ux%u fmt=%02X off=%08X pitch=%u lv=%u | v0 (%.1f,%.1f,z%.0f,rhw%g) c=%08X uv=(%.1f,%.1f) | bl=%u %X/%X/%X dt=%u/%X/%u at=%u/%X/%u cm=%08X cull=%u/%X/%X zeta=%08X st=%u\n",
                s_frame, s->color_va, b->count, b->texture ? "Y" : "n", b->texture ? b->texture->width : 0, b->texture ? b->texture->height : 0,
                b->texture ? b->texture->color : 0, b->texture ? b->texture->offset : 0, b->texture ? b->texture->pitch : 0, b->texture ? b->texture->levels : 0,
                v0->x, v0->y, v0->z, v0->rhw, v0->diffuse, v0->u, v0->v,
                b->state->blend_enable, b->state->blend_src, b->state->blend_dst, b->state->blend_eq,
                b->state->depth_test_enable, b->state->depth_func, b->state->depth_write,
                b->state->alpha_test_enable, b->state->alpha_func, b->state->alpha_ref, b->state->color_mask,
                b->state->cull_enable, b->state->cull_face, b->state->front_face, b->state->zeta_va, b->state->stencil_enable);
    }
    if (s_trace_all && s_frame >= s_ta_lo && s_frame <= s_ta_hi && s_trace_lines < 3000 &&
        ta_new(((uint64_t)(b->texture ? b->texture->color + 1 : 0) << 56) ^ ((uint64_t)(b->texture ? b->texture->width : 0) << 40) ^ ((uint64_t)b->count << 24) ^
               ((uint64_t)r->blend_enable << 23) ^ ((uint64_t)r->blend_src << 12) ^ ((uint64_t)r->blend_dst << 3) ^ ((uint64_t)r->depth_test_enable << 22) ^ ((uint64_t)r->alpha_test_enable << 21) ^ ((uint64_t)r->color_mask * 2654435761u) ^ ((uint64_t)b->vertices[0].diffuse * 40503u) ^ ((uint64_t)s->color_va << 4))) {
        uint32_t k;
        s_trace_lines++;
        fprintf(stderr, "[GPUTA] f%u DRAW va=%08X %ux%u n=%u tex=%s fmt=%02X %ux%u bl=%u %X/%X/%X dt=%u/%X/%u at=%u/%X/%u cm=%08X\n", s_frame, s->color_va, s->width, s->height, b->count,
                b->texture ? "Y" : "n", b->texture ? b->texture->color : 0, b->texture ? b->texture->width : 0, b->texture ? b->texture->height : 0,
                r->blend_enable, r->blend_src, r->blend_dst, r->blend_eq, r->depth_test_enable, r->depth_func, r->depth_write,
                r->alpha_test_enable, r->alpha_func, r->alpha_ref, r->color_mask);
        for (k = 0; k < b->count && k < (b->count == 12 ? 12 : 6); k++)
            fprintf(stderr, "[GPUTA]    v%u (%.1f,%.1f,z%.0f,rhw%g) c=%08X uv=(%.1f,%.1f)\n", k, b->vertices[k].x, b->vertices[k].y, b->vertices[k].z, b->vertices[k].rhw, b->vertices[k].diffuse, b->vertices[k].u, b->vertices[k].v);
    }
    rt = get_rt(s);
    if (!rt) return;
    ds = get_ds(s, r->zeta_va);
    need = b->count * (uint32_t)sizeof(Nv2aVertex);
    xneed = b->combiner ? b->count * (uint32_t)sizeof(RcVertExt) : 0;
    if (need > VB_BYTES || xneed > XVB_BYTES) return;
    if (s_vb_off + need > VB_BYTES) {
        s_vb_off = 0; s_vb_i = (s_vb_i + 1) % VB_RING; s_vb = s_vbr[s_vb_i];
        s_xvb = s_xvbr[s_vb_i]; s_xvb_off = 0;
    }
    if (xneed && s_xvb && s_xvb_off + xneed > XVB_BYTES) s_xvb_off = 0;
    if (FAILED(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)s_vb, 0,
               s_vb_off == 0 ? D3D11_MAP_WRITE_DISCARD : D3D11_MAP_WRITE_NO_OVERWRITE, 0, &m)))
        return;
    if (!s_stage_v) s_stage_v = (Nv2aVertex *)malloc(VB_BYTES);
    if (!s_stage_x) s_stage_x = (RcVertExt *)malloc(XVB_BYTES);
    if (!s_stage_v || !s_stage_x) { ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)s_vb, 0); return; }
    mdst = (Nv2aVertex *)((uint8_t *)m.pData + s_vb_off);
    dst = s_stage_v;
    if (xneed && s_xvb) {
        D3D11_MAPPED_SUBRESOURCE xm;
        if (SUCCEEDED(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)s_xvb, 0,
                       s_xvb_off == 0 ? D3D11_MAP_WRITE_DISCARD : D3D11_MAP_WRITE_NO_OVERWRITE, 0, &xm))) {
            mxdst = (RcVertExt *)((uint8_t *)xm.pData + s_xvb_off);
            xdst = s_stage_x;
            packed_ext = 1;
        }
    }
    if (indexed) {
        /* Unique vertices, indices drawn as they are: the executor already rejected, culled and clipped. */
        memcpy(dst, b->vertices, b->count * sizeof *dst);
        kept = b->count;
        if (b->combiner)
            for (i = 0; i < kept; i++)
                rc_vert_fix(&dst[i], (packed_ext && xdst) ? &xdst[i] : NULL);
    } else
    for (i = 0; i + 2 < b->count; i += 3) {
        const Nv2aVertex *v = &b->vertices[i];
        float area;
        /* The executor flattens triangle strips without flipping odd triangles, so their
         * winding alternates. A triangle that reuses the previous one's v1,v2 as its v0,v1
         * is the next strip triangle (quads/fans never do): flip its winding for culling
         * only, otherwise the second half of every full-screen strip quad is culled. */
        if (g_rbx_precull)
            ;   /* strip parity only matters for culling here, and the executor culled already */
        else if (i >= 3 && !memcmp(&v[0], &v[-2], sizeof v[0]) && !memcmp(&v[1], &v[-1], sizeof v[0]))
            strip_odd ^= 1;
        else
            strip_odd = 0;
        if (!(v[0].rhw > 0 && v[1].rhw > 0 && v[2].rhw > 0)) continue;
        area = (v[1].x - v[0].x) * (v[2].y - v[0].y) - (v[1].y - v[0].y) * (v[2].x - v[0].x);
        if (!(fabsf(area) > 0.0f) || !isfinite(area)) continue;
        if (s_cull_on && r->cull_enable && !g_rbx_precull) {
            int front = is_front(strip_odd ? -area : area, r->front_face);
            if ((r->cull_face == 0x405 && !front) || (r->cull_face == 0x404 && front) || r->cull_face == 0x408) {
                st_culled++;
                continue;
            }
        }
        dst[kept] = v[0]; dst[kept + 1] = v[1]; dst[kept + 2] = v[2];
        /* Stage1 often samples with oT1; if the VSH left t1 at 0, reuse t0 so
         * the 0919390C T1-blend does not hit opaque black at UV(0,0). */
        if (b->combiner) {
            unsigned k;
            for (k = 0; k < 3; k++)
                rc_vert_fix(&dst[kept + k], (packed_ext && xdst) ? &xdst[kept + k] : NULL);
        }
        kept += 3;
    }
    /* SKIPBLACK must read dst before Unmap.
     * Mission VSH does `MOV oD0, c[100]`: lit passes load c[100]=white, cel
     * outline / darken passes load c[100]=black (often with a no-oT0 program).
     * Any all-black-V0 RC batch only paints black over the lit fill — skip it
     * for every combiner, not only ic0=00002004. RECOMP_GPU_SKIPBLACK=0 keeps
     * the hardware passes. */
    if (kept && b->combiner) {
        static int skipblk = -1, nblk;
        if (skipblk < 0) {
            const char *e = getenv("RECOMP_GPU_SKIPBLACK");
            skipblk = !(e && e[0] == '0');
        }
        if (skipblk) {
            uint32_t k, dmax = 0;
            for (k = 0; k < kept; k++) {
                uint32_t d = dst[k].diffuse & 0x00FFFFFFu;
                if (d > dmax) dmax = d;
            }
            if (dmax == 0) {
                if (nblk++ < 24)
                    fprintf(stderr, "[RBX DX11] skip black-V0 pass n=%u ic0=%08X\n",
                            kept, b->combiner->color_icw[0]);
                ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)s_vb, 0);
                if (packed_ext && s_xvb)
                    ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)s_xvb, 0);
                return;
            }
        }
    }
    if (kept) {
        memcpy(mdst, dst, kept * sizeof *dst);
        if (packed_ext && mxdst) memcpy(mxdst, xdst, kept * sizeof *xdst);
    }
    ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)s_vb, 0);
    if (packed_ext && s_xvb)
        ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)s_xvb, 0);
    if (!kept) return;
    /* RECOMP_GPU_DRAWCAP=lo:hold: from frame lo, only the first N draws of each frame render, N growing by 1
     * every `hold` frames; each change is logged with wall-clock ms so framebuffer dumps can be matched. */
    { static unsigned cap_lo, cap_hold, cap_idx, cap_last = ~0u, cap_n; static int cap_on = -1;
      if (cap_on < 0) { const char *e = getenv("RECOMP_GPU_DRAWCAP"); cap_on = (e && sscanf(e, "%u:%u", &cap_lo, &cap_hold) == 2 && cap_hold) ? 1 : 0; }
      if (cap_on && s_frame >= cap_lo) {
          if (s_frame != cap_last) {
              cap_last = s_frame; cap_idx = 0;
              if (cap_n != 1 + (s_frame - cap_lo) / cap_hold) {
                  FILETIME ft; ULARGE_INTEGER u;
                  cap_n = 1 + (s_frame - cap_lo) / cap_hold;
                  GetSystemTimeAsFileTime(&ft); u.LowPart = ft.dwLowDateTime; u.HighPart = ft.dwHighDateTime;
                  fprintf(stderr, "[DRAWCAP] t=%llu cap=%u frame=%u\n", (unsigned long long)(u.QuadPart / 10000), cap_n, s_frame);
              }
          }
          if (getenv("RECOMP_GPU_CAPONLY") ? (cap_idx++ + 1 != (unsigned)atoi(getenv("RECOMP_GPU_CAPONLY"))) : (cap_idx++ >= cap_n)) return;
      } }

    {
        int use_rc = b->combiner != NULL;
        int force_white = 0;
        ID3D11ShaderResourceView *bound_srv[4] = { 0, 0, 0, 0 };
        ID3D11SamplerState *bound_smp[4] = { 0, 0, 0, 0 };
        ID3D11Buffer *cbuf;
        unsigned stage;
        int td_probe = 0;

        if (use_rc) {
            int t1_zero = 1, t0_zero = 1;
            unsigned k;
            for (k = 0; k < b->count; k++) {
                const float *t0 = b->vertices[k].tex[0];
                const float *t1 = b->vertices[k].tex[1];
                if (fabsf(t1[0]) + fabsf(t1[1]) + fabsf(t1[2]) > 1e-5f) t1_zero = 0;
                if (fabsf(t0[0]) + fabsf(t0[1]) + fabsf(t0[2]) > 1e-5f) t0_zero = 0;
            }
            use_rc = rc_setup(s, b, kept, t0_zero, t1_zero, packed_ext, gclip, clipz, nearz, bound_srv, bound_smp, &cbuf,
                              rt, rt->ex && flat_full_width(b->vertices, b->count, s->width));
            if (use_rc) {
                srv = bound_srv[0] ? bound_srv[0] : s_white_srv;
                smp = bound_smp[0];
            }
        }

        /* RECOMP_GPU_DRAWLOG=lo-hi: one line per draw for frames lo..hi (UI/menu glitch hunting). */
        { static unsigned dl_lo = 1, dl_hi = 0, dl_n; static int dl = -1;
          if (dl < 0) { const char *e = getenv("RECOMP_GPU_DRAWLOG"); dl = (e && sscanf(e, "%u-%u", &dl_lo, &dl_hi) == 2) ? 1 : 0; }
          if (dl && s_frame >= dl_lo && s_frame <= dl_hi && dl_n++ < 4000) {
            const Nv2aCombiner *mc = use_rc ? b->combiner : NULL;
            const Nv2aVertex *v0 = &b->vertices[0];
            fprintf(stderr, "[DRAWLOG] f%u rc=%d n=%u rt=%08X %ux%u c0=%08X tex=%s off=%08X fmt=%02X bl=%u %X/%X eq=%X at=%u/%X/%u dt=%u/%X dw=%u cm=%08X cull=%u/%X sp=%08X ic=%08X ai=%08X oc=%08X ao=%08X fin=%08X/%08X\n",
                    s_frame, use_rc, kept, s->color_va, s->width, s->height, v0->diffuse,
                    b->texture ? "Y" : "n", b->texture ? b->texture->offset : 0, b->texture ? b->texture->color : 0,
                    r->blend_enable, r->blend_src, r->blend_dst, r->blend_eq,
                    r->alpha_test_enable, r->alpha_func, r->alpha_ref,
                    r->depth_test_enable, r->depth_func, r->depth_write, r->color_mask,
                    r->cull_enable, r->cull_face,
                    mc ? mc->stage_program : 0, mc ? mc->color_icw[0] : 0, mc ? mc->alpha_icw[0] : 0,
                    mc ? mc->color_ocw[0] : 0, mc ? mc->alpha_ocw[0] : 0, mc ? mc->final0 : 0, mc ? mc->final1 : 0);
            fprintf(stderr, "   blend en=%u src=%X dst=%X eq=%X col=%08X  atest en=%u func=%X ref=%u  cmask=%08X\n",
                    (unsigned)r->blend_enable, (unsigned)r->blend_src, (unsigned)r->blend_dst, (unsigned)r->blend_eq, (unsigned)r->blend_color,
                    (unsigned)r->alpha_test_enable, (unsigned)r->alpha_func, (unsigned)r->alpha_ref, (unsigned)r->color_mask);
            if (b->count <= 12 && b->texture)
              for (unsigned vi = 0; vi < b->count; vi++) { const Nv2aVertex *q = &b->vertices[vi];
                fprintf(stderr, "   v%u xyz=%.1f,%.1f,%.2f rhw=%g dif=%08X uv=%.1f,%.1f t0=%.2f,%.2f,%.2f,%.2f wh=%ux%u au=%u av=%u\n", vi, q->x, q->y, q->z, q->rhw, q->diffuse, q->u, q->v,
                        q->tex[0][0], q->tex[0][1], q->tex[0][2], q->tex[0][3], b->texture->width, b->texture->height, b->texture->addr_u, b->texture->addr_v); }
          } }

        /* Mission lighting audit: RECOMP_GPU_MISSDIFF=1. f>=450 ≈ Ch1 after CONTINUE. */
        { static int md = -1; if (md < 0) md = getenv("RECOMP_GPU_MISSDIFF") != NULL;
          if (md && s_frame >= 450) {
            static unsigned n; const Nv2aVertex *v0 = &b->vertices[0];
            uint32_t dmax = 0, k;
            float umin = 1e9f, umax = -1e9f, vmin = 1e9f, vmax = -1e9f, wmin = 1e9f, wmax = -1e9f;
            float u1min = 1e9f, u1max = -1e9f, v1min = 1e9f, v1max = -1e9f;
            for (k = 0; k < kept; k++) {
                uint32_t d = b->vertices[k].diffuse & 0x00FFFFFFu;
                const float *t = b->vertices[k].tex[0], *t1 = b->vertices[k].tex[1];
                if (d > dmax) dmax = d;
                if (t[0] < umin) umin = t[0]; if (t[0] > umax) umax = t[0];
                if (t[1] < vmin) vmin = t[1]; if (t[1] > vmax) vmax = t[1];
                if (t[3] < wmin) wmin = t[3]; if (t[3] > wmax) wmax = t[3];
                if (t1[0] < u1min) u1min = t1[0]; if (t1[0] > u1max) u1max = t1[0];
                if (t1[1] < v1min) v1min = t1[1]; if (t1[1] > v1max) v1max = t1[1];
            }
            if (n++ < 400) {
                uint32_t sp = 0, f0 = 0, f1 = 0, fmt = 0, ctl = 0;
                uint32_t ic0 = 0, ic1 = 0, oc0 = 0, oc1 = 0, ai0 = 0, ai1 = 0, ao0 = 0, ao1 = 0;
                if (use_rc && b->combiner) {
                    const Nv2aCombiner *mc = b->combiner;
                    sp = mc->stage_program; ctl = mc->control;
                    f0 = mc->final0; f1 = mc->final1;
                    ic0 = mc->color_icw[0]; ic1 = mc->color_icw[1];
                    oc0 = mc->color_ocw[0]; oc1 = mc->color_ocw[1];
                    ai0 = mc->alpha_icw[0]; ai1 = mc->alpha_icw[1];
                    ao0 = mc->alpha_ocw[0]; ao1 = mc->alpha_ocw[1];
                }
                /* Prefer 2-stage / large draws; always keep 2-stage samples. */
                int is2 = use_rc && b->combiner && (b->combiner->stage_program & ~0x1Fu) != 0;
                int skip_ui = (!is2 && n > 80 && use_rc && b->combiner
                    && kept < 24 && n < 350);
                if (!skip_ui) {
                if (use_rc && b->stages && (b->stage_valid & 1))
                    fmt = b->stages[0].color;
                fprintf(stderr, "[MISSDIFF] f%u rc=%d n=%u rt=%08X %ux%u c0=%08X dmax=%06X "
                        "uv0=[%.3f..%.3f],[%.3f..%.3f] q=[%.3f..%.3f] "
                        "uv1=[%.3f..%.3f],[%.3f..%.3f] sp=%08X ctl=%08X "
                        "ic=%08X/%08X oc=%08X/%08X ai=%08X/%08X ao=%08X/%08X fin=%08X/%08X "
                        "tex=%s off=%08X/%08X fmt=%02X bl=%u %X/%X at=%u/%X/%u dt=%u cm=%08X sv=%X\n",
                        s_frame, use_rc, kept, s->color_va, s->width, s->height, v0->diffuse,
                        dmax, umin, umax, vmin, vmax, wmin, wmax,
                        u1min, u1max, v1min, v1max, sp, ctl,
                        ic0, ic1, oc0, oc1, ai0, ai1, ao0, ao1, f0, f1,
                        (use_rc ? (bound_srv[0] != NULL) : (b->texture != NULL)) ? "Y" : "n",
                        use_rc ? (b->stages && (b->stage_valid & 1) ? b->stages[0].offset : 0)
                               : (b->texture ? b->texture->offset : 0),
                        use_rc && b->stages && (b->stage_valid & 2) ? b->stages[1].offset : 0,
                        fmt, r->blend_enable, r->blend_src, r->blend_dst,
                        r->alpha_test_enable, r->alpha_func, r->alpha_ref,
                        r->depth_test_enable, r->color_mask, b->stage_valid);
                }
            }
          } }

        /* RECOMP_GPU_FORCEWHITE=1: mission RC → untextured vertex color + no depth.
         * RECOMP_GPU_NODEPTH=1: keep combiners, drop depth/stencil only. */
        { static int fw = -1, nd = -1;
          if (fw < 0) fw = getenv("RECOMP_GPU_FORCEWHITE") != NULL;
          if (nd < 0) nd = getenv("RECOMP_GPU_NODEPTH") != NULL;
          if (s_frame >= 450 && use_rc && fw) {
            force_white = 1;
            use_rc = 0;
            memset(&cb, 0, sizeof cb);
            cb.sw = (float)s->width; cb.sh = (float)s->height;
            cb.textured = 0; cb.afunc = 0x208; cb.aref = 0;
            cb.wsq = wide_wsq(rt, 0);
            cb.clipz = clipz; cb.nearz = nearz;
            srv = s_white_srv;
            { Nv2aTexture dflt; memset(&dflt, 0, sizeof dflt); dflt.addr_u = dflt.addr_v = 3; smp = get_sampler(&dflt); }
            cbuf = s_cbr[s_cb_i++ % CB_RING];
            if (SUCCEEDED(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)cbuf, 0,
                           D3D11_MAP_WRITE_DISCARD, 0, &m))) {
                memcpy(m.pData, &cb, sizeof cb);
                ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)cbuf, 0);
            }
          } else if (s_frame >= 450 && nd) {
            force_white = 1; /* reuse flag: strip depth only */
          } }

        if (!use_rc) {
            memset(&cb, 0, sizeof cb);
            cb.sw = (float)s->width; cb.sh = (float)s->height;
            cb.textured = 0; cb.afunc = r->alpha_test_enable ? r->alpha_func : 0x207; cb.aref = (float)(r->alpha_ref & 0xFF);
            cb.clipz = clipz; cb.nearz = nearz;
            srv = s_white_srv;
            if (b->texture) {
                srv = get_tex(b->texture);
                {
                    int ti;
                    s_tex_from_pack = 0;
                    for (ti = 0; s_sf && ti < MAX_TEX; ti++)
                        if (s_tx[ti].srv == srv) { s_tex_from_pack = (int)s_tx[ti].from_pack; break; }
                }
                smp = get_sampler(b->texture);
                cb.textured = (srv != s_white_srv) ? 1 : 0; cb.pad = (s_noalpha ? 1u : 0u) | ((b->texture->use_mask & 1) ? 0u : 2u) | ((b->texture->use_mask & 2) ? 0u : 4u);
                cb.tw = (float)b->texture->width; cb.th = (float)b->texture->height;
            }
            if (!smp) { Nv2aTexture dflt; memset(&dflt, 0, sizeof dflt); dflt.addr_u = dflt.addr_v = 3; smp = get_sampler(&dflt); }
            if (rt->ex) {   /* widescreen: see wide_wsq */
                float k, o;
                int plain = !cb.textured || srv == s_white_srv || rt_of_srv(srv);
                cb.wsq = wide_wsq(rt, plain && flat_full_width(b->vertices, b->count, s->width));
                wide_uv(cb.textured ? srv : NULL, cb.wsq, &k, &o);
                if (k != 1.0f) { cb.tw /= k; cb.uofs = o; }
            }
            { static int tdm = -1; static unsigned tdn, tdf, tdo;   /* RECOMP_GPU_TEXTDRAW=N: dump the first N draws that sample a P8 texture (glyph quads) */
              if (tdm < 0) { const char *e = getenv("RECOMP_GPU_TEXTDRAW"); tdm = e ? atoi(e) : 0; { const char *ef = getenv("RECOMP_GPU_TEXTDRAW_FROM"); tdf = ef ? (unsigned)atoi(ef) : 0; ef = getenv("RECOMP_GPU_TEXTDRAW_OFF"); tdo = ef ? (unsigned)strtoul(ef, NULL, 16) : 0; } }
              if (tdn < (unsigned)tdm && b->texture && b->texture->color == 0x0B && b->texture->width == 512 && s_frame >= tdf && (!tdo || tdo == b->texture->offset)) { uint32_t k, nv = b->count < 12 ? b->count : 12; tdn++; td_probe = 1;
                  fprintf(stderr, "[TEXTDRAW] #%u f%u n=%u rt=%08X %ux%u tex off=%08X %ux%u pal=%08X filt=%08X au=%u av=%u use_mask=%u textured=%u white_srv=%d cb.pad=%u aref=%g afunc=%X | bl=%u %X/%X/%X at=%u/%X/%u dt=%u cm=%08X\n", tdn, s_frame, b->count, s->color_va, s->width, s->height,
                          b->texture->offset, b->texture->width, b->texture->height, b->texture->palette, (unsigned)b->texture->filter, (unsigned)b->texture->addr_u, (unsigned)b->texture->addr_v, (unsigned)b->texture->use_mask, (unsigned)cb.textured, srv == s_white_srv, cb.pad, cb.aref, cb.afunc,
                          r->blend_enable, r->blend_src, r->blend_dst, r->blend_eq, r->alpha_test_enable, r->alpha_func, r->alpha_ref, r->depth_test_enable, r->color_mask);
                  for (k = 0; k < nv; k++) { const Nv2aVertex *v = &b->vertices[k]; fprintf(stderr, "[TEXTDRAW]    v%u (%.2f,%.2f,z%.0f,rhw%g) c=%08X uv=(%.3f,%.3f)\n", k, v->x, v->y, v->z, v->rhw, v->diffuse, v->u, v->v); } } }
            cbuf = s_cbr[s_cb_i++ % CB_RING];
            if (SUCCEEDED(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)cbuf, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
                memcpy(m.pData, &cb, sizeof cb);
                ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)cbuf, 0);
            }
        }

        bs = get_blend(r);
        dss = get_dss(r, !force_white && ds != NULL);
        bf[0] = ((r->blend_color >> 16) & 255) / 255.0f; bf[1] = ((r->blend_color >> 8) & 255) / 255.0f;
        bf[2] = (r->blend_color & 255) / 255.0f; bf[3] = ((r->blend_color >> 24) & 255) / 255.0f;
        rtv = rt->rtv;
        if (!use_rc) {
            /* Character sprites: the 128x128 sheets the game rewrites as it goes, alpha-blended onto a full-screen target. */
            int sprite = s_sf && b->texture && b->texture->color == 0x0B && b->texture->width == 128 && b->texture->height == 128
                         && r->blend_enable && r->blend_src == 0x302 && r->blend_dst == 0x303 && s->width >= 512
                         && srv != s_white_srv && !s_tex_from_pack && sf_at_sprite_scale(b) && sf_layer_ready(SF_W, SF_H);
            if (sprite) {
                static const float none[4] = { 0, 0, 0, 0 };
                if (!s_sf_dirty || s_sf_rt != rt) {
                    sf_flush();
                    ID3D11DeviceContext_ClearRenderTargetView(s_ctx, s_sf_rtv, none);
                    ID3D11DeviceContext_ClearDepthStencilView(s_ctx, s_sf_dsv, D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);
                    s_sf_rt = rt;
                    s_sf_dirty = 1;
                }
                rtv = s_sf_rtv;
                bs = s_sf_blend;
                layer_draw = 1;
                {   /* one sprite pixel to one layer pixel: no filtering on the way in */
                    Nv2aTexture nearest = *b->texture;
                    nearest.filter = (1u << 16) | (1u << 24);
                    smp = get_sampler(&nearest);
                }
            } else {
                sf_flush();
            }
        } else {
            sf_flush();
        }
        ID3D11DeviceContext_OMSetRenderTargets(s_ctx, 1, &rtv,
            layer_draw ? s_sf_dsv : (!force_white && ds ? ds->dsv : NULL));
        ID3D11DeviceContext_OMSetBlendState(s_ctx, bs, bf, 0xFFFFFFFFu);
        ID3D11DeviceContext_OMSetDepthStencilState(s_ctx, dss, r->stencil_ref & 0xFF);
        if (!layer_draw && s_rs_split && s_split_va && s_split_va == s->color_va) {
            D3D11_RECT rect = { 0, s_split_top * s_scale, rt_full_w(rt) * s_scale, s_split_bottom * s_scale };
            ID3D11DeviceContext_RSSetScissorRects(s_ctx, 1, &rect);
            ID3D11DeviceContext_RSSetState(s_ctx, gclip ? clip_rs(r, 1) : s_rs_split);
        } else {
            ID3D11DeviceContext_RSSetState(s_ctx, gclip ? clip_rs(r, 0) : s_rs);
        }
        /* Pixel-centre shift, default +0.5 px (RECOMP_GPU_PIXOFS=<px> overrides, 0 = the old mapping). */
        { static float po = -1.0f; if (po < 0) { const char *e = getenv("RECOMP_GPU_PIXOFS"); po = e ? (float)atof(e) : 0.5f; if (po < 0) po = 0; }
          if (layer_draw) { vp.TopLeftX = po * (float)SF_W / (float)s->width; vp.TopLeftY = po * (float)SF_H / (float)s->height; }
          else vp.TopLeftX = vp.TopLeftY = po * (float)s_scale; }
        vp.Width = layer_draw ? (float)SF_W : (float)(rt_full_w(rt) * (uint32_t)s_scale); vp.Height = layer_draw ? (float)SF_H : (float)(s->height * (uint32_t)s_scale); vp.MinDepth = 0; vp.MaxDepth = 1;
        ID3D11DeviceContext_RSSetViewports(s_ctx, 1, &vp);
        ID3D11DeviceContext_IASetInputLayout(s_ctx, use_rc ? s_rc_il : s_il);
        ID3D11DeviceContext_IASetPrimitiveTopology(s_ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        if (use_rc && packed_ext && s_xvb) {
            ID3D11Buffer *vbs[2];
            UINT strs[2], offs[2];
            UINT first = s_vb_off / (UINT)sizeof(Nv2aVertex);
            UINT xfirst = s_xvb_off / (UINT)sizeof(RcVertExt);
            if (xfirst < first) first = xfirst;
            vbs[0] = s_vb; vbs[1] = s_xvb;
            strs[0] = stride; strs[1] = xstride;
            offs[0] = s_vb_off - first * (UINT)sizeof(Nv2aVertex);
            offs[1] = s_xvb_off - first * (UINT)sizeof(RcVertExt);
            ID3D11DeviceContext_IASetVertexBuffers(s_ctx, 0, 2, vbs, strs, offs);
            off = first; /* StartVertexLocation */
        } else {
            ID3D11DeviceContext_IASetVertexBuffers(s_ctx, 0, 1, &s_vb, &stride, &off);
            off = s_vb_off / (UINT)sizeof(Nv2aVertex);
        }
        ID3D11DeviceContext_VSSetShader(s_ctx, use_rc ? (gclip ? s_rc_vs_clip : s_rc_vs) : (gclip ? s_vs_clip : s_vs), NULL, 0);
        ID3D11DeviceContext_PSSetShader(s_ctx, use_rc ? s_rc_ps : s_ps, NULL, 0);
        ID3D11DeviceContext_VSSetConstantBuffers(s_ctx, 0, 1, &cbuf);
        ID3D11DeviceContext_PSSetConstantBuffers(s_ctx, 0, 1, &cbuf);
        if (use_rc) {
            Nv2aTexture dflt;
            memset(&dflt, 0, sizeof dflt); dflt.addr_u = dflt.addr_v = 3;
            ID3D11DeviceContext_PSSetConstantBuffers(s_ctx, 1, 1, &s_rc_cb);
            for (stage = 0; stage < 4; stage++) {
                ID3D11ShaderResourceView *ssrv = bound_srv[stage] ? bound_srv[stage] : s_white_srv;
                ID3D11SamplerState *ssmp = bound_smp[stage] ? bound_smp[stage] : get_sampler(&dflt);
                ID3D11DeviceContext_PSSetShaderResources(s_ctx, stage, 1, &ssrv);
                ID3D11DeviceContext_PSSetSamplers(s_ctx, stage, 1, &ssmp);
            }
        } else {
            ID3D11DeviceContext_PSSetShaderResources(s_ctx, 0, 1, &srv);
            ID3D11DeviceContext_PSSetSamplers(s_ctx, 0, 1, &smp);
        }
        if (indexed) {
            D3D11_MAPPED_SUBRESOURCE im;
            uint32_t ineed = g_rbx_bidx_n * 4u;
            if (s_ib_off + ineed > IB_BYTES) { s_ib_off = 0; s_ib_i = (s_ib_i + 1) % VB_RING; }
            if (SUCCEEDED(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)s_ibr[s_ib_i], 0,
                          s_ib_off == 0 ? D3D11_MAP_WRITE_DISCARD : D3D11_MAP_WRITE_NO_OVERWRITE, 0, &im))) {
                memcpy((uint8_t *)im.pData + s_ib_off, g_rbx_bidx, ineed);
                ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)s_ibr[s_ib_i], 0);
                ID3D11DeviceContext_IASetIndexBuffer(s_ctx, s_ibr[s_ib_i], DXGI_FORMAT_R32_UINT, 0);
                ID3D11DeviceContext_DrawIndexed(s_ctx, g_rbx_bidx_n, s_ib_off / 4u, (INT)off);
                s_ib_off += ineed;
            }
        } else
            ID3D11DeviceContext_Draw(s_ctx, kept, off);
        if (s_flush_mode && (s_flush_mode >= 3 || ((!use_rc) && cb.textured && (s_flush_mode >= 2 || (b->texture && b->texture->color == 0x0B && b->texture->width == 512))))) ID3D11DeviceContext_Flush(s_ctx);
        if (td_probe) probe_after_draw(rt, srv, b);
        s_vb_off += kept * (uint32_t)sizeof(Nv2aVertex);
        if (use_rc && packed_ext) s_xvb_off += kept * (uint32_t)sizeof(RcVertExt);
        rt->dirty = 1; rt->shown_dirty = 1;
        rt->resolve_pending = rt->msaa != NULL;
        st_draws++; st_tris += (indexed ? g_rbx_bidx_n : kept) / 3;
    }
}

/* Map a staging copy and write it into guest memory at va (w x h, pitch, bpp 4 or 2). */
/* The largest render target written back to guest memory since the last flip: the frame the window shows. The debug
 * build's mark screenshots read it (MVC2_DEBUG_ON_FLIP); the NV2A scan-out register is never set in this emulation. */
static uint32_t s_shot_va, s_shot_w, s_shot_h, s_shot_pitch, s_shot_bpp;

static void rb_write(ID3D11Texture2D *st, uint32_t va, uint32_t w, uint32_t h, uint32_t pitch, uint32_t bpp)
{
    D3D11_MAPPED_SUBRESOURCE m;
    uint8_t *mem = (uint8_t *)xbox_GetMemoryOffset();
    uint32_t y, x;
    unsigned rb0 = qpc_us();
    if (FAILED(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)st, 0, D3D11_MAP_READ, 0, &m))) return;
    if (w * h >= s_shot_w * s_shot_h) { s_shot_va = va; s_shot_w = w; s_shot_h = h; s_shot_pitch = pitch; s_shot_bpp = bpp; }
    if (s_fh > 0) { uint32_t yy; uint64_t hh = 1469598103934665603ULL; for (yy = 0; yy < h; yy++) hh = (hh ^ hash_bytes((const uint8_t *)m.pData + (size_t)yy * m.RowPitch, w * 4)) * 1099511628211ULL; fh_rth ^= hh; }
    if (bpp == 4 && pitch >= w * 4) {
        for (y = 0; y < h; y++)
            memcpy(mem + va + (size_t)y * pitch, (const uint8_t *)m.pData + (size_t)y * m.RowPitch, (size_t)w * 4);
    } else if (bpp == 2 && pitch >= w * 2) {
        for (y = 0; y < h; y++) {
            const uint32_t *src = (const uint32_t *)((const uint8_t *)m.pData + (size_t)y * m.RowPitch);
            uint16_t *d = (uint16_t *)(mem + va + (size_t)y * pitch);
            for (x = 0; x < w; x++) {
                uint32_t c = src[x];
                d[x] = (uint16_t)(((c >> 8) & 0xF800) | ((c >> 5) & 0x07E0) | ((c >> 3) & 0x001F));
            }
        }
    }
    ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)st, 0);
    st_rb_us += qpc_us() - rb0; st_rb_n++;
}

/* RECOMP_GPU_SYNC_RB=1: copy and map in the same flip (the CPU waits for the GPU to finish the frame). */
static void readback(Rt *rt)
{
    ID3D11DeviceContext_CopyResource(s_ctx, (ID3D11Resource *)rt->stage, rt_guest_image(rt));
    rb_write(rt->stage, rt->va, rt->w, rt->h, rt->pitch, rt->bpp);
    rt->dirty = 0;
}

/* Default, deferred by one flip: queue the copy now (into the staging texture the last copy did not use) and write it
 * to guest memory at the next flip, when the GPU has long finished. Mapping right after the copy made the pushbuffer
 * thread wait ~3.4 ms per flip for the GPU (~400 ms per 2 s in fights). The window shows the frame one flip later;
 * GPU-side reads of render targets use the live texture (the sampled-RT path), not guest memory. */
static void readback_start(Rt *rt)
{
    ID3D11Texture2D *st = rt->pstage == rt->stage ? rt->stage2 : rt->stage;
    if (!st) { readback(rt); return; }
    ID3D11DeviceContext_CopyResource(s_ctx, (ID3D11Resource *)st, rt_guest_image(rt));
    rt->pstage = st; rt->pend = 1;
    rt->pva = rt->va; rt->pw = rt->w; rt->ph = rt->h; rt->ppitch = rt->pitch; rt->pbpp = rt->bpp;
    rt->dirty = 0;
}
static void readback_finish(Rt *rt)
{
    if (!rt->pend) return;
    rb_write(rt->pstage, rt->pva, rt->pw, rt->ph, rt->ppitch, rt->pbpp);
    rt->pend = 0;
}

/* ---- what the window shows ---------------------------------------------- */

/* The frame goes to the window's own device in a shared texture, at full render scale. The window thread presents it
 * on its own clock, so the display never holds up the game. If sharing fails, the window keeps showing guest memory. */
extern volatile uint32_t g_nv2a_flip_va;
extern void gpu_present_set_source(void *shared_handle, uint32_t width, uint32_t height);
extern void gpu_present_set_display_size(uint32_t width, uint32_t height);
extern void gpu_present_frame_ready(void);
extern volatile LONG rbx_input_guest_frame;
static ID3D11Texture2D *s_share;
static IDXGIKeyedMutex *s_share_mutex;
static ID3D11Texture2D *s_share_retired;
static uint32_t s_share_w, s_share_h;
static int s_share_failed, s_share_on;
static HANDLE s_share_handle;
static volatile LONG s_share_abandoned;
static unsigned s_share_serial;

int rbx_present_trace(void)
{
    static volatile LONG mode;
    LONG value = mode;
    if (!value) {
        const char *option = getenv("RECOMP_RBX_PRESENT_TRACE");
        value = option && option[0] == '1' ? 1 : -1;
        InterlockedExchange(&mode, value);
    }
    return value > 0;
}

void rbx_present_abandoned(void)
{
    InterlockedExchange(&s_share_abandoned, 1);
}

/* 1 while the window shows this back end's frame (pb_exec skips most CPU frame copies then). */
int rbx_gpu_frame_shared(void) { return s_share_on; }
/* 1 when the index ring exists, so the executor may hand over indexed batches (g_rbx_bidx). */
int rbx_gpu_indexed_ok(void) { return s_ibr[0] && s_ibr[1] && s_ibr[2]; }
int rbx_gpu_clip_ok(void) { return s_vs_clip && !s_clip_failed; }
int rbx_gpu_vsh_ok(void) { return s_gvs_ready && s_rc_vs_clip && !s_clip_failed && s_ibr[0] && s_ibr[1] && s_ibr[2]; }

static void share_close(void)
{
    if (s_share_on || s_share_retired)
        gpu_present_set_source(NULL, 0, 0);
    s_share_on = 0;
    SAFE_REL(s_share_retired);
    SAFE_REL(s_share_mutex);
    SAFE_REL(s_share);
    s_share_handle = NULL;
    s_share_w = s_share_h = 0;
}

/* RECOMP_GPU_PRESDUMP=<prefix>[:every]: writes exactly what the window is given (the presented D3D target, not guest
 * memory) as <prefix>_f<frame>.bmp, 24-bit, every N-th published frame (default 30). */
static void present_dump(Rt *rt)
{
    static int on = -1, guest_frames; static unsigned every = 30, n; static char pfx[400];
    static ID3D11Texture2D *st; static uint32_t sw, sh;
    D3D11_TEXTURE2D_DESC td; D3D11_MAPPED_SUBRESOURCE m; FILE *f; char path[460]; uint32_t x, y;
    if (on < 0) {
        const char *e = getenv("RECOMP_GPU_PRESDUMP"), *c;
        const char *frame_mode = getenv("RECOMP_GPU_PRESDUMP_FRAME");
        guest_frames = frame_mode && frame_mode[0] == '1';
        on = 0;
        if (e && *e) {
            c = strrchr(e, ':');
            if (c && c > e + 2 && atoi(c + 1) > 0) { every = (unsigned)atoi(c + 1); snprintf(pfx, sizeof pfx, "%.*s", (int)(c - e), e); }
            else snprintf(pfx, sizeof pfx, "%s", e);
            on = 1;
        }
    }
    if (!on) return;
    { /* RECOMP_GPU_PRESDUMP_SECS=lo-hi: only dump inside that window (seconds since the first present) */
      static int w = -1; static unsigned lo, hi; static DWORD t0; DWORD sec;
      if (w < 0) { const char *e = getenv("RECOMP_GPU_PRESDUMP_SECS"); w = e && sscanf(e, "%u-%u", &lo, &hi) == 2; t0 = GetTickCount(); }
      sec = (GetTickCount() - t0) / 1000; if (w && (sec < lo || sec >= hi)) return; }
    if ((guest_frames ? s_frame : n++) % every) return;
    ID3D11Texture2D_GetDesc(rt->tex, &td);
    if (!st || sw != td.Width || sh != td.Height) {
        SAFE_REL(st);
        td.Usage = D3D11_USAGE_STAGING; td.BindFlags = 0; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ; td.MiscFlags = 0;
        if (FAILED(ID3D11Device_CreateTexture2D(s_dev, &td, NULL, &st))) { st = NULL; return; }
        sw = td.Width; sh = td.Height;
    }
    ID3D11DeviceContext_CopyResource(s_ctx, (ID3D11Resource *)st, (ID3D11Resource *)rt->tex);
    if (FAILED(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)st, 0, D3D11_MAP_READ, 0, &m))) return;
    snprintf(path, sizeof path, "%s_f%06u.bmp", pfx, s_frame);
    if ((f = fopen(path, "wb")) != NULL) {
        uint32_t row = (sw * 3 + 3) & ~3u, size = 54 + row * sh;
        uint8_t h[54] = { 'B', 'M' }, *line = (uint8_t *)calloc(1, row);
        *(uint32_t *)(h + 2) = size; *(uint32_t *)(h + 10) = 54; *(uint32_t *)(h + 14) = 40;
        *(int32_t *)(h + 18) = (int32_t)sw; *(int32_t *)(h + 22) = -(int32_t)sh; *(uint16_t *)(h + 26) = 1; *(uint16_t *)(h + 28) = 24;
        fwrite(h, 1, 54, f);
        for (y = 0; y < sh && line; y++) {
            const uint8_t *src = (const uint8_t *)m.pData + (size_t)y * m.RowPitch;
            for (x = 0; x < sw; x++) { line[x * 3] = src[x * 4]; line[x * 3 + 1] = src[x * 4 + 1]; line[x * 3 + 2] = src[x * 4 + 2]; }
            fwrite(line, 1, row, f);
        }
        free(line); fclose(f);
    }
    ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)st, 0);
}

static void publish_frame(void)
{
    static unsigned last_limit;
    static unsigned long long next_frame;
    LARGE_INTEGER now;
    uint32_t va = g_nv2a_flip_va;
    int i;
    Rt *rt = NULL;
    if (s_share_failed) return;
    if (InterlockedExchange(&s_share_abandoned, 0))
        share_close();
    {   /* Several targets can share the flip VA (the game changed the clip on that buffer). Showing whichever came
         * first in the table made the window alternate between them. Show the biggest; among equals, the one
         * drawn most recently. RECOMP_GPU_PUBFIRST=1 = old behaviour. */
        static int first = -1; int n = 0;
        if (first < 0) first = getenv("RECOMP_GPU_PUBFIRST") != NULL;
        for (i = 0; i < MAX_RT; i++) {
            Rt *c = &s_rt[i];
            if (!c->tex || c->va != va) continue;
            n++;
            if (!rt) { rt = c; if (first) break; continue; }
            if (c->w * c->h > rt->w * rt->h || (c->w * c->h == rt->w * rt->h && c->used > rt->used)) rt = c;
        }
        if (n > 1) st_pub_multi++;
    }
    /* No GPU target at the flip VA. One such flip (seen once per boot) used to switch the window to guest memory and
     * leave it there: s_share_on went to 0 but the texture stayed, so a later frame of the same size never handed
     * it back. Keep the last GPU frame for a few flips; only a run of them (the game drawing on the CPU) falls back,
     * and share_close() makes the next GPU frame re-attach. RECOMP_GPU_NORTFLIPS sets the run (default 3). */
    {
        static int nort_limit = -1; static unsigned nort_run;
        if (nort_limit < 0) { const char *e = getenv("RECOMP_GPU_NORTFLIPS"); nort_limit = e ? atoi(e) : 3; if (nort_limit < 1) nort_limit = 1; }
        if (!rt) { st_pub_nort++; if (++nort_run >= (unsigned)nort_limit) share_close(); return; }
        nort_run = 0;
    }
    if (!rt->shown_dirty && s_share_on) return;
    if (s_fps_limit != last_limit) { next_frame = 0; last_limit = s_fps_limit; }
    if (s_fps_limit && s_cap_drop) {
        QueryPerformanceCounter(&now);
        if ((unsigned long long)now.QuadPart < next_frame) return;
    }
    {
        uint32_t w = rt_full_w(rt) * (uint32_t)s_scale, h = rt->h * (uint32_t)s_scale;
        if (w != s_share_w || h != s_share_h || !s_share) {
            D3D11_TEXTURE2D_DESC td;
            IDXGIResource *res = NULL;
            HANDLE handle = NULL;
            /* Keep the old completed frame visible until the replacement is ready. */
            if (s_share_on) {
                s_share_retired = s_share;
                s_share = NULL;
                s_share_on = 0;
            }
            SAFE_REL(s_share_mutex);
            SAFE_REL(s_share);
            memset(&td, 0, sizeof td);
            td.Width = w; td.Height = h; td.MipLevels = td.ArraySize = 1; td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
            td.MiscFlags = D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
            if (FAILED(ID3D11Device_CreateTexture2D(s_dev, &td, NULL, &s_share))
                || FAILED(ID3D11Texture2D_QueryInterface(s_share, &IID_IDXGIKeyedMutex, (void **)&s_share_mutex))
                || FAILED(ID3D11Texture2D_QueryInterface(s_share, &IID_IDXGIResource, (void **)&res))
                || FAILED(IDXGIResource_GetSharedHandle(res, &handle))) {
                fprintf(stderr, "[RBX DX11] shared frame texture failed: the window shows guest memory\n");
                SAFE_REL(res);
                share_close();
                s_share_failed = 1;
                return;
            }
            SAFE_REL(res);
            s_share_w = w; s_share_h = h;
            s_share_handle = handle;
        }
        /* Always refresh logical aspect: AA 1280×480 must letterbox as 640×480 (4:3), and a widescreen target as its
         * own wider shape. */
        {
            uint32_t ax = rt->aa_sx ? rt->aa_sx : 1, ay = rt->aa_sy ? rt->aa_sy : 1;
            uint32_t dw = (rt_full_w(rt) / ax) * (uint32_t)s_scale, dh = (rt->h / ay) * (uint32_t)s_scale;
            if (!dw) dw = 640;
            if (!dh) dh = 480;
            gpu_present_set_display_size(dw, dh);
        }
    }
    /* 0 ms: never block the PB/GPU thread on the window's present hold.
     * A multi-ms wait here was part of the Ch1 BusyLoop/present death spiral. */
    HRESULT acquired = IDXGIKeyedMutex_AcquireSync(s_share_mutex, 0, 0);
    if (acquired != S_OK) {
        if (rbx_present_trace())
            fprintf(stderr, "[RBX PRESENT] write va=%08X frame=%u acquire=%08lX\n",
                    va, s_frame, (unsigned long)acquired);
        if (acquired == WAIT_ABANDONED)
            share_close();
        return;
    }
    rt_resolve(rt);
    ID3D11DeviceContext_CopyResource(s_ctx, (ID3D11Resource *)s_share, (ID3D11Resource *)rt->tex);
    ID3D11DeviceContext_Flush(s_ctx);
    HRESULT released = IDXGIKeyedMutex_ReleaseSync(s_share_mutex, 0);
    if (released != S_OK) {
        share_close();
        return;
    }
    if (!s_share_on) {
        gpu_present_set_source(s_share_handle, s_share_w, s_share_h);
        s_share_on = 1;
        SAFE_REL(s_share_retired);
    }
    rt->shown_dirty = 0;
    if (rbx_present_trace())
        fprintf(stderr, "[RBX PRESENT] write serial=%u va=%08X frame=%u tex=%ux%u\n",
                ++s_share_serial, va, s_frame, s_share_w, s_share_h);
    gpu_present_frame_ready();
    st_pub++;
    present_dump(rt);
    if (s_fps_limit && s_cap_drop) {
        unsigned long long interval = gb_qf() / s_fps_limit;
        unsigned long long current = (unsigned long long)now.QuadPart;
        next_frame = next_frame && current - next_frame < interval ? next_frame + interval : current + interval;
    }
}

/* Widescreen factor for the F1 choice (0 Off, 1 Auto, 2 16:9, 3 16:10, 4 21:9). RECOMP_RBX_WIDESCREEN=<w>:<h> (or a
 * plain ratio such as 2.4) with a shape the menu does not list overrides it; the listed names are read by settings. */
static float wide_target(unsigned mode)
{
    extern HWND xbox_fb_window(void);
    static const float shapes[] = { 4.0f / 3.0f, 0.0f, 16.0f / 9.0f, 16.0f / 10.0f, 64.0f / 27.0f };
    static float custom = -1.0f, pending;
    static DWORD since;
    float a;
    if (custom < 0.0f) {
        const char *e = getenv("RECOMP_RBX_WIDESCREEN");
        float x = 0, y = 0;
        custom = 0.0f;
        if (e && sscanf(e, "%f:%f", &x, &y) == 2 && x > 0 && y > 0) custom = x / y;
        else if (e && sscanf(e, "%f", &x) == 1 && x > 1.34f && x <= 4.0f) custom = x;
        if (custom == 16.0f / 9.0f || custom == 16.0f / 10.0f || (custom > 2.33f && custom < 2.34f)) custom = 0.0f;
        if (custom > 0.0f) fprintf(stderr, "[RBX DX11] widescreen: custom aspect %.4f from RECOMP_RBX_WIDESCREEN\n", custom);
    }
    if (custom > 0.0f) a = custom;
    else if (mode == 1) {
        /* Auto: the window's client shape (the monitor's in fullscreen), once a resize has settled for 300 ms so a
         * drag does not rebuild the targets every frame. */
        HWND w = xbox_fb_window();
        RECT r;
        if (!w || !GetClientRect(w, &r) || r.right <= 0 || r.bottom <= 0) return s_wide;
        a = (float)r.right / (float)r.bottom;
        if (fabsf(a - pending) > 0.002f) { pending = a; since = GetTickCount(); }
        if (GetTickCount() - since < 300) return s_wide;
    } else if (mode < sizeof shapes / sizeof shapes[0]) a = shapes[mode];
    else a = shapes[0];
    a /= 4.0f / 3.0f;
    if (a < 1.0f) a = 1.0f;
    if (a > 2.7f) a = 2.7f;   /* 32:9 */
    return floorf(a * 256.0f + 0.5f) / 256.0f;
}

/* Between frames, on the pushbuffer thread. */
static void video_apply(void)
{
    static const unsigned limits[] = {30, 60, 120, 144, 240, 360, 0};
    unsigned options = rbx_settings_video();
    int scale = (int)(options & 7) + 1, hires = (options >> 8) & 1, i;
    unsigned aa = (options >> 3) & 3, aniso = 1u << ((options >> 5) & 7);
    unsigned samples = s_msaa_mask & (1u << aa) ? 1u << aa : 1;
    s_fps_limit = limits[(options >> 9) & 7];
    s_ip_menu = (options >> 15) & 1;
    if (s_ip >= 0 && s_ip_env < 0) ip_set_level(s_ip_menu);
    if (hires != s_hires || (aniso > 1) != (s_aniso > 1)) {
        for (i = 0; i < MAX_TEX; i++) { DEFER_REL(s_tx[i].srv); DEFER_REL(s_tx[i].tex); s_tx[i].used = 0; }
        s_hires = hires;
    }
    if (aniso != s_aniso) {
        clear_states(&s_sst);
        s_aniso = aniso;
    }
    float wide = wide_target((options >> 12) & 7);
    if (scale != s_scale || samples != s_msaa || wide != s_wide) {
        ID3D11RenderTargetView *none = NULL;
        ID3D11DeviceContext_OMSetRenderTargets(s_ctx, 1, &none, NULL);
        for (i = 0; i < MAX_RT; i++) { rt_release(&s_rt[i]); s_rt[i].va = 0; }
        for (i = 0; i < 4; i++) { SAFE_REL(s_ds[i].dsv); SAFE_REL(s_ds[i].tex); s_ds[i].va = 0; }
        s_scale = scale;
        s_msaa = samples;
        s_wide = wide;
        s_resets++;
        fprintf(stderr, "[RBX DX11] video: render scale %dx, MSAA %ux, widescreen %.3f (aspect %.3f)\n", s_scale, s_msaa,
                s_wide, s_wide * 4.0f / 3.0f);
    }
}

static void ip_trace(int published)
{
    static int on = -1; static unsigned lo, hi, lines; static DWORD t0;
    DWORD ms;
    if (on < 0) { const char *e = getenv("RECOMP_RBX_INTERP_TRACE"); on = e && sscanf(e, "%u-%u", &lo, &hi) == 2; t0 = GetTickCount(); }
    if (on) {
        ms = GetTickCount() - t0;
        if (ms >= lo * 1000u && ms < hi * 1000u && lines < 20000u) {
            lines++;
            fprintf(stderr, "[IPTR] ms=%lu f=%u flip_va=%08X draws_3d=%u (rt %08X) draws_other=%u (rt %08X) ffp=%u tick=%u u=%.2f blended=%u published=%d\n",
                    (unsigned long)ms, s_frame, (unsigned)g_nv2a_flip_va, s_tr_gvs, (unsigned)s_tr_gvs_va, s_tr_cpu, (unsigned)s_tr_cpu_va,
                    s_tr_ffp, s_tr_tick, s_ip_t, s_tr_blend, published);
        }
    }
    s_tr_gvs = s_tr_cpu = s_tr_blend = s_tr_tick = s_tr_ffp = 0; s_tr_gvs_va = s_tr_cpu_va = 0;
}
static void gb_flip_impl(void)
{
    int i;
    static unsigned long last;
    unsigned long now;
    if (!s_inited || s_failed) {
        static unsigned n;
        if (n++ < 5) fprintf(stderr, "[RBX DX11] flip before the back end is ready (inited=%d failed=%d): no stats, no publish\n", s_inited, s_failed);
        return;
    }
    s_split_va = 0;
    s_frame++; s_tick++; st_flips++;
    {
        LARGE_INTEGER q; unsigned long long d, qf = gb_qf();
        QueryPerformanceCounter(&q);
        if (s_ft_last) {
            d = (unsigned long long)q.QuadPart - s_ft_last;
            st_ft_sum += d; st_ft_n++;
            if (d > st_ft_max) st_ft_max = d;
            if (d * 1000ULL > qf * 33ULL) st_hitch33++;
            if (d * 1000ULL > qf * 50ULL) st_hitch50++;
            if (d * 1000ULL > qf * 100ULL) st_hitch100++;
        }
        s_ft_last = (unsigned long long)q.QuadPart;
        if (sr_on()) sr_flip((unsigned long long)q.QuadPart);
        if (s_ip > 0) ip_flip();
    }
    rbx_input_guest_frame = (LONG)s_frame;
    s_frame_for_grave = s_frame;
    texdump_poll();
    { unsigned long k = 0, keep = 0; for (; k < s_defer_n; k++) { if (s_grave[k].f + 2 <= s_frame) IUnknown_Release(s_grave[k].o); else s_grave[keep++] = s_grave[k]; } s_defer_n = keep; }
    sf_flush();
    gb_mark(3);
    if (!s_rb_sync)
        for (i = 0; i < MAX_RT; i++)
            if (s_rt[i].tex) readback_finish(&s_rt[i]);   /* last flip's copies */
    MVC2_DEBUG_ON_FLIP(s_shot_va, s_shot_w, s_shot_h, s_shot_pitch, s_shot_bpp);   /* debug flavour only: DEL mark key, due screenshots */
    s_shot_w = s_shot_h = 0;
    for (i = 0; i < MAX_RT; i++)
        if (s_rt[i].tex && s_rt[i].dirty) {
            /* The GPU samples this target from its live texture, so guest memory only serves CPU reads (save
             * thumbnails, the window fallback, re-seeding after eviction). Reading it back every flip cost ~40 ms/s
             * on the bench. Every Nth flip, staggered per slot, keeps guest memory at most N flips old.
             * Never reading it back (RECOMP_GPU_SKIPRB=1) once stalled the game, so 0 stays opt-in. */
            if (s_rt[i].sampled + 2 >= s_frame && s_rb_every != 1) {
                if (s_rb_every == 0) { s_rt[i].dirty = 0; continue; }
                if ((s_frame + (unsigned)i) % (unsigned)s_rb_every) { st_rb_defer++; continue; }   /* stays dirty */
            }
            if (s_rb_sync) readback(&s_rt[i]); else readback_start(&s_rt[i]);
        }
    gb_mark(4);
    gb_mark(0);
    if (s_fh > 0) { unsigned k, nn = fh_nd < 256 ? fh_nd : 256; if (nn != fd_pn && fd_lines < 4000) { fprintf(stderr, "[FD] f=%u ndraws %u -> %u\n", s_frame, fd_pn, nn); fd_lines++; if (nn + 1 == fd_pn || nn == fd_pn + 1) { const FdRec *A = nn > fd_pn ? fd_cur : fd_prev; const FdRec *B = nn > fd_pn ? fd_prev : fd_cur; unsigned m = nn > fd_pn ? nn : fd_pn, j; for (j = 0; j + 1 < m; j++) if (A[j].vh != B[j].vh) break; fprintf(stderr, "[FD]   extra/missing draw at idx %u of %u: n=%u pos=(%.1f,%.1f) tex=%ux%u fmt=%02X c0=%08X blend=%u vh=%016llX\n", j, m, A[j].n, A[j].x0, A[j].y0, A[j].w, A[j].h, A[j].fmt, A[j].c0, A[j].st, (unsigned long long)A[j].vh); } } else if (nn == fd_pn) { for (k = 0; k < nn && fd_lines < 4000; k++) if (fd_cur[k].vh != fd_prev[k].vh || fd_cur[k].th != fd_prev[k].th) { fprintf(stderr, "[FD] f=%u draw#%u/%u n=%u %s%s pos=(%.1f,%.1f) tex=%ux%u fmt=%02X c0=%08X blend=%u\n", s_frame, k, nn, fd_cur[k].n, fd_cur[k].vh != fd_prev[k].vh ? "VERT " : "", fd_cur[k].th != fd_prev[k].th ? "TEXDATA" : "", fd_cur[k].x0, fd_cur[k].y0, fd_cur[k].w, fd_cur[k].h, fd_cur[k].fmt, fd_cur[k].c0, fd_cur[k].st); fd_lines++; } } memcpy(fd_prev, fd_cur, sizeof(fd_cur)); fd_pn = nn; }
    if (s_fh > 0) { fprintf(stderr, "[FH] f=%u nd=%u exb=%u fl0=%u xff=%u sh=%u tr=%u bg=%u ilf=%u dpn=%u dp=%u dh=%016llX rth=%016llX\n", s_frame, fh_nd, g_dbg_batches, g_dbg_flush0, g_dbg_xffail, g_dbg_short, g_dbg_trunc, g_dbg_begin, g_dbg_ilf, g_dbg_dpn, g_dbg_dp, (unsigned long long)fh_dh, (unsigned long long)fh_rth); fh_dh = 0; fh_rth = 0; fh_nd = 0; g_dbg_batches = g_dbg_flush0 = g_dbg_xffail = g_dbg_short = g_dbg_begin = g_dbg_ilf = g_dbg_dpn = g_dbg_dp = 0; }
    now = GetTickCount();
    if (s_verbose && now - last >= 2000) {
        fprintf(stderr, "[RBX DX11] frame=%u flips=%u draws=%u tris=%u culled=%u clears=%u texdecodes=%u texhash_us=%u dec_us=%u crt_us=%u rtsamples=%u us_draw=%u us_clear=%u us_flip=%u scan_ms=%u segs=%u thfast=%u thfull=%u thstale=%u states=%u/%u/%u state_spill=%ld\n",
                s_frame, st_flips, st_draws, st_tris, st_culled, st_clears, st_decodes, st_hash_ns_us, st_dec_us, st_crt_us, st_rtsamples, st_us_draw, st_us_clear, st_us_flip, (unsigned)(g_pb_scan_ticks * 1000ULL / gb_qf()), g_pb_scan_segs, st_th_fast, st_th_full, st_th_stale, s_bst.n, s_dsst.n, s_sst.n, (long)st_state_spill);
        /* Executor split, ms per window: batch = all of raster_batch; xf = vertex loop (fetch + vsh), vsh = interpreter alone;
         * clip = clip + triangle assembly; flush = hand-off incl. draw() (us_draw above). */
        fprintf(stderr, "[GPUSPLIT] scan_ms=%u batch_ms=%u xf_ms=%u vsh_ms=%u fetch_ms=%u clip_ms=%u flush_ms=%u xfverts=%u\n",
                (unsigned)(g_pb_scan_ticks * 1000ULL / gb_qf()), (unsigned)(g_pb_t_batch * 1000ULL / gb_qf()),
                (unsigned)(g_pb_t_xf * 1000ULL / gb_qf()), (unsigned)(g_pb_t_vsh * 1000ULL / gb_qf()),
                (unsigned)((g_pb_t_xf > g_pb_t_vsh ? g_pb_t_xf - g_pb_t_vsh : 0) * 1000ULL / gb_qf()),
                (unsigned)(g_pb_t_clip * 1000ULL / gb_qf()), (unsigned)(g_pb_t_flush * 1000ULL / gb_qf()), g_pb_n_xfverts);
        {   /* [RBXPERF]: one line per stats window. Counters from other files are cumulative; print deltas. */
            static LONG l_fc, l_fs, l_ft, l_mm, l_fw; static unsigned l_lw, l_lt, l_pd, l_pm;
            LONG fw = g_rbx_fence_wait_us; unsigned pd = g_rbx_pb_drops, pm = g_rbx_pb_midwalk_get;
            LONG fc = g_rbx_forced_catchups, fs = g_rbx_fence_spurious, ft = g_rbx_fence_timeouts, mm = g_rbx_mmio_traps;
            unsigned lw = rbx_apu_lock_waits(), lt = rbx_apu_lock_wait_timeouts();
            unsigned long long qf = gb_qf();
            unsigned win_ms = (unsigned)(now - last);
            fprintf(stderr, "[RBXPERF] ms=%u flips=%u pub=%u ft_avg_us=%u ft_max_us=%u hitch33=%u hitch50=%u hitch100=%u "
                            "rb_n=%u rb_us=%u sh_n=%u sh_us=%u sh_total=%u sh_hits=%u catchup=%ld fence_early=%ld fence_timeout=%ld "
                            "mmio_traps=%ld apu_lock_waits=%u apu_lock_timeouts=%u fence_wait_us=%ld pb_drops=%u pb_midwalk_get=%u "
                            "rt_new=%u rt_evict=%u rt_alias=%u pub_nort=%u pub_multi=%u clear_partial=%u rb_defer=%u rb_evict=%u cap=%u pace_n=%u pace_us=%u pace_calls=%u pace_late=%u\n",
                    win_ms, st_flips, st_pub,
                    st_ft_n ? (unsigned)(st_ft_sum * 1000000ULL / qf / st_ft_n) : 0u, (unsigned)(st_ft_max * 1000000ULL / qf),
                    st_hitch33, st_hitch50, st_hitch100, st_rb_n, st_rb_us, st_sh_n, st_sh_us, s_sh_total, st_sh_hits,
                    (long)(fc - l_fc), (long)(fs - l_fs), (long)(ft - l_ft), (long)(mm - l_mm), lw - l_lw, lt - l_lt,
                    (long)(fw - l_fw), pd - l_pd, pm - l_pm,
                    st_rt_new, st_rt_evict, st_rt_alias, st_pub_nort, st_pub_multi, st_clear_partial, st_rb_defer, st_rb_evict, s_fps_limit, st_pace_n, st_pace_us, st_pace_calls, st_pace_late);
            l_fc = fc; l_fs = fs; l_ft = ft; l_mm = mm; l_lw = lw; l_lt = lt; l_fw = fw; l_pd = pd; l_pm = pm;
            st_rb_us = st_rb_n = st_sh_us = st_sh_n = st_sh_hits = st_pub = 0;
            st_rt_new = st_rt_evict = st_rt_alias = st_pub_nort = st_pub_multi = st_clear_partial = st_rb_defer = st_rb_evict = st_pace_n = st_pace_us = st_pace_calls = st_pace_late = 0;
            st_ft_sum = st_ft_max = 0; st_ft_n = st_hitch33 = st_hitch50 = st_hitch100 = 0;
        }
        g_pb_scan_ticks = 0; g_pb_scan_segs = 0;
        g_pb_t_batch = g_pb_t_xf = g_pb_t_vsh = g_pb_t_clip = g_pb_t_flush = 0; g_pb_n_xfverts = 0;
        st_us_draw = st_us_clear = st_us_flip = 0;
        st_dec_us = st_crt_us = 0; st_draws = st_tris = st_culled = st_clears = st_decodes = st_hash_ns_us = st_flips = st_rtsamples = 0; st_th_fast = st_th_full = st_th_stale = 0;
        last = now;
    }
    {
        unsigned pub0 = st_pub;
        publish_frame();
        ip_trace(pub0 != st_pub);
    }
    rbx_settings_wait();
    video_apply();
}

static unsigned qpc_us(void) { static LARGE_INTEGER f; LARGE_INTEGER t; if (!f.QuadPart) QueryPerformanceFrequency(&f); QueryPerformanceCounter(&t); return (unsigned)((t.QuadPart * 1000000ULL) / f.QuadPart); }
static void gb_clear(const Nv2aSurface *s, const Nv2aRenderState *rs, uint32_t flags, uint32_t argb, uint32_t zst) { unsigned t0 = qpc_us(); gb_mark(1); gb_clear_impl(s, rs, flags, argb, zst); gb_mark(0); st_us_clear += qpc_us() - t0; }
static void gb_draw(const Nv2aSurface *s, const Nv2aBatch *b) { unsigned t0 = qpc_us(); gb_mark(2); gb_draw_impl(s, b); gb_mark(0); st_us_draw += qpc_us() - t0; }
/* The FPS limit (F1 menu). It used to drop frames on their way to the window while the game kept flipping as fast as
 * it could, so the rate on screen wandered around the cap (each shown frame waited for the next guest flip past its
 * deadline) and the game still used the whole CPU and GPU. Now the pushbuffer thread holds each flip until its slot:
 * the game waits on its fences as it would on vsync, and runs at the cap. A run more than one frame late re-anchors
 * instead of bursting to catch up. A high-resolution waitable timer sleeps to ~1 ms before the slot, then a short
 * yield loop lands on it. RECOMP_FPS_CAP_DROP=1 restores the old behaviour. */
extern volatile long g_rbx_frontend_fps;   /* src/recomp_manual.c */
static void pace_flip(void)
{
    static HANDLE timer;
    static int timer_init;
    static unsigned last_cap;
    static unsigned long long next;
    unsigned long long qf = gb_qf(), interval, now;
    LARGE_INTEGER q;
    /* Read the F1 setting here, not only through video_apply: that runs inside gb_flip_impl, which returns early
     * before the back end is ready (or if it failed), and the cap must hold whatever the renderer is doing. */
    static const unsigned limits[] = {30, 60, 120, 144, 240, 360, 0};
    unsigned rate = (rbx_settings_video() >> 9) & 7u, cap = limits[rate < 7u ? rate : 6u];
    /* The Mission Complete results screen (and the attract loop) step their logic 30 times a second but redraw and
     * flip as fast as they can; the results cycle their lines by flip count. The Xbox's vsync held those flips to
     * 60 a second; uncapped here they ran at 1000+ and the lines changed every 0.1 s. While that loop runs
     * (src/recomp_manual.c sets g_rbx_frontend_fps) the flips are held to its rate, or to the FPS limit if lower. */
    if (g_rbx_frontend_fps && (!cap || cap > (unsigned)g_rbx_frontend_fps)) cap = (unsigned)g_rbx_frontend_fps;
    if (cap != last_cap) {
        fprintf(stderr, "[RBX DX11] FPS limit %s%u (%s)\n", cap ? "" : "off ", cap,
                s_cap_drop ? "RECOMP_FPS_CAP_DROP: frames dropped, game not held" : "game held to it at each flip");
        next = 0; last_cap = cap;
    }
    st_pace_calls++;
    if (!cap || s_cap_drop) return;
    if (!timer_init) {
        timer_init = 1;
        timer = CreateWaitableTimerExW(NULL, NULL, 0x00000002 /* CREATE_WAITABLE_TIMER_HIGH_RESOLUTION */, TIMER_ALL_ACCESS);
        if (!timer) timer = CreateWaitableTimerExW(NULL, NULL, 0, TIMER_ALL_ACCESS);
    }
    interval = qf / cap;
    QueryPerformanceCounter(&q); now = (unsigned long long)q.QuadPart;
    if (!next || now > next + interval) { next = now + interval; st_pace_late++; return; }
    if (now < next) { st_pace_n++; st_pace_us += (unsigned)((next - now) * 1000000ULL / qf); }
    while (now < next) {
        unsigned long long left_us = (next - now) * 1000000ULL / qf;
        if (left_us > 1500) {
            if (timer) {
                LARGE_INTEGER due;
                due.QuadPart = -(LONGLONG)((left_us - 1000) * 10);   /* 100 ns units, relative */
                if (SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE)) WaitForSingleObject(timer, 50);
                else Sleep(1);
            } else Sleep(1);
        } else SwitchToThread();
        QueryPerformanceCounter(&q); now = (unsigned long long)q.QuadPart;
    }
    next += interval;
}
static void gb_flip(void) { unsigned t0 = qpc_us(); gb_mark(4); gb_flip_impl(); gb_mark(0); st_us_flip += qpc_us() - t0; pace_flip(); }
static const Nv2aBackend s_backend = { gb_clear, gb_draw, gb_flip };


/* ---- sampling profiler (debug): RECOMP_PROF="from-to" seconds since install; needs a PDB ---- */
#include <tlhelp32.h>
#include <psapi.h>
#include <dbghelp.h>

#pragma comment(lib, "dbghelp.lib")
typedef struct { DWORD64 a, b; unsigned n; } ProfB;
static char g_prof_chain[8][1200]; static int g_prof_chain_n;
/* RECOMP_PROF line histogram for the busiest thread: source line of each sample. */
typedef struct { char file[96]; DWORD line; unsigned n; } ProfLine;
static ProfLine g_prof_lines[2048]; static unsigned g_prof_nlines;
static void prof_line_add(DWORD64 rip)
{
    IMAGEHLP_LINE64 ln; DWORD d = 0; unsigned k; const char *f;
    memset(&ln, 0, sizeof ln); ln.SizeOfStruct = sizeof ln;
    if (!SymGetLineFromAddr64(GetCurrentProcess(), rip, &d, &ln) || !ln.FileName) return;
    f = strrchr(ln.FileName, '\\'); f = f ? f + 1 : ln.FileName;
    for (k = 0; k < g_prof_nlines; k++)
        if (g_prof_lines[k].line == ln.LineNumber && !strcmp(g_prof_lines[k].file, f)) { g_prof_lines[k].n++; return; }
    if (g_prof_nlines < 2048) { ProfLine *q = &g_prof_lines[g_prof_nlines++]; strncpy(q->file, f, sizeof q->file - 1); q->line = ln.LineNumber; q->n = 1; }
}
static void prof_line_dump(void)
{
    unsigned r, k, tot = 0;
    for (k = 0; k < g_prof_nlines; k++) tot += g_prof_lines[k].n;
    if (!tot) return;
    fprintf(stderr, "[PROF] lines of busiest thread (%u samples with line info):\n", tot);
    {   /* per-file totals */
        char fn[64][96]; unsigned fc[64], nf = 0, j;
        for (k = 0; k < g_prof_nlines; k++) {
            for (j = 0; j < nf; j++) if (!strcmp(fn[j], g_prof_lines[k].file)) break;
            if (j == nf && nf < 64) { strcpy(fn[nf], g_prof_lines[k].file); fc[nf++] = 0; }
            if (j < nf) fc[j] += g_prof_lines[k].n;
        }
        for (j = 0; j < nf; j++) if (fc[j] * 200 >= tot)
            fprintf(stderr, "[PROF]   file %5.1f%% %s\n", 100.0 * fc[j] / tot, fn[j]);
    }
    for (r = 0; r < 30; r++) {
        unsigned bi = 0, bn = 0;
        for (k = 0; k < g_prof_nlines; k++) if (g_prof_lines[k].n > bn) { bn = g_prof_lines[k].n; bi = k; }
        if (!bn) break;
        fprintf(stderr, "[PROF]   %5.1f%% %s:%lu\n", 100.0 * bn / tot, g_prof_lines[bi].file, (unsigned long)g_prof_lines[bi].line);
        g_prof_lines[bi].n = 0;
    }
}
/* RECOMP_PROF_CHAINS=1: per-thread histogram of full call chains (finds what a waiting thread waits on) */
static int s_prof_chains;
typedef struct { char txt[700]; unsigned n; } ProfChain;
static ProfChain s_pchain[64][24];
static void prof_chain_add(unsigned ti, const CONTEXT *c0)
{
    char w[700]; CONTEXT u = *c0; int f; unsigned k; w[0] = 0;
    __try {
        for (f = 0; f < 10; f++) {
            DWORD64 base, ef; PVOID hd; PRUNTIME_FUNCTION rf; char b3[sizeof(SYMBOL_INFO) + 128]; SYMBOL_INFO *s3 = (SYMBOL_INFO *)b3; DWORD64 d3 = 0;
            memset(b3, 0, sizeof b3); s3->SizeOfStruct = sizeof(SYMBOL_INFO); s3->MaxNameLen = 127;
            if (strlen(w) > 600) break;
            if (SymFromAddr(GetCurrentProcess(), u.Rip, &d3, s3)) strncat(w, s3->Name, 50); else strcat(w, "?");
            strcat(w, " < ");
            rf = RtlLookupFunctionEntry(u.Rip, &base, NULL); if (!rf) break;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, u.Rip, rf, &u, &hd, &ef, NULL);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { }
    if (ti >= 64) return;
    for (k = 0; k < 24 && s_pchain[ti][k].n; k++) if (!strcmp(s_pchain[ti][k].txt, w)) { s_pchain[ti][k].n++; return; }
    if (k < 24) { strcpy(s_pchain[ti][k].txt, w); s_pchain[ti][k].n = 1; }
}
static DWORD WINAPI gb_prof_thread(LPVOID p)
{
    unsigned lo = 0, hi = 0; DWORD t0 = GetTickCount(); DWORD self = GetCurrentThreadId();
    enum { MAXT = 64, MAXA = 4096 };
    static DWORD tids[MAXT]; static unsigned nt = 0; static ProfB *bk[MAXT]; static unsigned nb[MAXT]; static unsigned tot[MAXT];
    unsigned i, last_enum = 0;
    sscanf((const char *)p, "%u-%u", &lo, &hi);
    s_prof_chains = getenv("RECOMP_PROF_CHAINS") && atoi(getenv("RECOMP_PROF_CHAINS"));
    while ((GetTickCount() - t0) / 1000 < lo) Sleep(100);
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS); SymInitialize(GetCurrentProcess(), NULL, TRUE);
    { /* pick the 4 busiest threads over 2 s so sampling does not perturb the run */
        DWORD ids[256]; unsigned long long t1[256]; unsigned n = 0, k, j; HANDLE s = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0); THREADENTRY32 te; te.dwSize = sizeof te;
        if (s != INVALID_HANDLE_VALUE) { if (Thread32First(s, &te)) do { if (te.th32OwnerProcessID == GetCurrentProcessId() && te.th32ThreadID != self && n < 256) ids[n++] = te.th32ThreadID; } while (Thread32Next(s, &te)); CloseHandle(s); }
        for (k = 0; k < n; k++) { HANDLE h = OpenThread(THREAD_QUERY_INFORMATION, FALSE, ids[k]); FILETIME a, b, kt, ut; t1[k] = 0; if (h) { if (GetThreadTimes(h, &a, &b, &kt, &ut)) t1[k] = ((unsigned long long)kt.dwHighDateTime << 32 | kt.dwLowDateTime) + ((unsigned long long)ut.dwHighDateTime << 32 | ut.dwLowDateTime); CloseHandle(h); } }
        Sleep(2000);
        for (k = 0; k < n; k++) { HANDLE h = OpenThread(THREAD_QUERY_INFORMATION, FALSE, ids[k]); FILETIME a, b, kt, ut; unsigned long long v = 0; if (h) { if (GetThreadTimes(h, &a, &b, &kt, &ut)) v = ((unsigned long long)kt.dwHighDateTime << 32 | kt.dwLowDateTime) + ((unsigned long long)ut.dwHighDateTime << 32 | ut.dwLowDateTime); CloseHandle(h); } t1[k] = v - t1[k]; }
        unsigned ntop = getenv("RECOMP_PROF_TOP") ? (unsigned)atoi(getenv("RECOMP_PROF_TOP")) : 4; if (ntop > MAXT) ntop = MAXT;
        static unsigned char used[256];
        for (j = 0; j < ntop; j++) { unsigned bi = 0; unsigned long long bv = 0; int got = 0; for (k = 0; k < n; k++) if (!used[k] && (!got || t1[k] > bv)) { bv = t1[k]; bi = k; got = 1; } if (!got || (ntop <= 4 && bv < 1000000ULL)) break; used[bi] = 1; fprintf(stderr, "[PROF] watching thread %lu cpu=%llu ms/2s\n", ids[bi], bv / 10000); tids[nt] = ids[bi]; bk[nt] = (ProfB *)calloc(MAXA, sizeof(ProfB)); nt++; t1[bi] = 0; }
    }
    while ((GetTickCount() - t0) / 1000 < hi) {
        if (0) {
            HANDLE s = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0); THREADENTRY32 te; te.dwSize = sizeof te;
            if (s != INVALID_HANDLE_VALUE) { if (Thread32First(s, &te)) do { if (te.th32OwnerProcessID == GetCurrentProcessId() && te.th32ThreadID != self) { unsigned k; for (k = 0; k < nt; k++) if (tids[k] == te.th32ThreadID) break; if (k == nt && nt < MAXT) { tids[nt] = te.th32ThreadID; bk[nt] = (ProfB *)calloc(MAXA, sizeof(ProfB)); nt++; } } } while (Thread32Next(s, &te)); CloseHandle(s); }
            last_enum = GetTickCount();
        }
        for (i = 0; i < nt; i++) {
            HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, tids[i]);
            if (!h) continue;
            if (SuspendThread(h) != (DWORD)-1) {
                CONTEXT c; memset(&c, 0, sizeof c); c.ContextFlags = CONTEXT_CONTROL;
                if (GetThreadContext(h, &c)) {
                    if (i == 0) prof_line_add(c.Rip);
                    if (s_prof_chains && (tot[i] % 10) == 0) prof_chain_add(i, &c);
                    DWORD64 a = c.Rip, disp = 0; char buf[sizeof(SYMBOL_INFO) + 256]; SYMBOL_INFO *si = (SYMBOL_INFO *)buf; unsigned k;
                    memset(buf, 0, sizeof buf); si->SizeOfStruct = sizeof(SYMBOL_INFO); si->MaxNameLen = 255;
                    if (SymFromAddr(GetCurrentProcess(), a, &disp, si)) a = si->Address; else a &= ~0xFFULL;
                    { /* first caller frame inside the exe */
                      static DWORD64 exe_lo, exe_hi; CONTEXT u = c; int fr; DWORD64 caller = 0;
                      if (!exe_lo) { HMODULE m = GetModuleHandleA(NULL); MODULEINFO mi; exe_lo = (DWORD64)m; exe_hi = exe_lo + 0x4000000; (void)mi; }
                      { static char chainbuf[8][1200]; static int nchain = 0; static int dumped_at_end; (void)dumped_at_end;
                        if (nchain < 8 && si->Name[0] == (char)90 && !strcmp(si->Name, "ZwWriteFile") && (tot[i] % 37) == 0) { CONTEXT u2 = c; int f2; char *w = chainbuf[nchain]; w[0] = 0; for (f2 = 0; f2 < 14; f2++) { DWORD64 base2, ef2; PVOID hd2; PRUNTIME_FUNCTION rf2; char b3[sizeof(SYMBOL_INFO) + 128]; SYMBOL_INFO *s3 = (SYMBOL_INFO *)b3; DWORD64 d3 = 0; memset(b3, 0, sizeof b3); s3->SizeOfStruct = sizeof(SYMBOL_INFO); s3->MaxNameLen = 127; if (SymFromAddr(GetCurrentProcess(), u2.Rip, &d3, s3)) { strncat(w, s3->Name, 60); strcat(w, " < "); } else strcat(w, "? < "); rf2 = RtlLookupFunctionEntry(u2.Rip, &base2, NULL); if (!rf2) break; RtlVirtualUnwind(UNW_FLAG_NHANDLER, base2, u2.Rip, rf2, &u2, &hd2, &ef2, NULL); } nchain++; g_prof_chain_n = nchain; strcpy(g_prof_chain[nchain - 1], w); }
                      }
                      __try { for (fr = 0; fr < 12; fr++) { DWORD64 base, hnd_data = 0; PVOID hd; DWORD64 ef; PRUNTIME_FUNCTION rf; if (u.Rip >= exe_lo && u.Rip < exe_hi) { caller = u.Rip; break; } rf = RtlLookupFunctionEntry(u.Rip, &base, NULL); if (!rf) break; RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, u.Rip, rf, &u, &hd, &ef, NULL); (void)hnd_data; } } __except(EXCEPTION_EXECUTE_HANDLER) { }
                      if (caller && !(c.Rip >= exe_lo && c.Rip < exe_hi)) { char b2[sizeof(SYMBOL_INFO) + 256]; SYMBOL_INFO *s2 = (SYMBOL_INFO *)b2; DWORD64 d2 = 0; memset(b2, 0, sizeof b2); s2->SizeOfStruct = sizeof(SYMBOL_INFO); s2->MaxNameLen = 255; caller = SymFromAddr(GetCurrentProcess(), caller, &d2, s2) ? s2->Address : caller; } else caller = 0;
                      for (k = 0; k < nb[i]; k++) if (bk[i][k].a == a && bk[i][k].b == caller) break;
                      if (k == nb[i] && nb[i] < MAXA) { bk[i][k].a = a; bk[i][k].b = caller; nb[i]++; }
                    }
                    if (k < nb[i]) bk[i][k].n++;
                    tot[i]++;
                }
                ResumeThread(h);
            }
            CloseHandle(h);
        }
        Sleep(1);
    }
    prof_line_dump();
    for (i = 0; i < nt; i++) {
        unsigned r, k; if (tot[i] < 50) continue;
        fprintf(stderr, "[PROF] thread %lu samples=%u\n", tids[i], tot[i]);
        for (r = 0; r < 14; r++) {
            unsigned bi = 0, bn = 0; char buf[sizeof(SYMBOL_INFO) + 256]; SYMBOL_INFO *si = (SYMBOL_INFO *)buf; DWORD64 d = 0;
            for (k = 0; k < nb[i]; k++) if (bk[i][k].n > bn) { bn = bk[i][k].n; bi = k; }
            if (!bn) break;
            memset(buf, 0, sizeof buf); si->SizeOfStruct = sizeof(SYMBOL_INFO); si->MaxNameLen = 255;
            { char nm1[300]; strcpy(nm1, SymFromAddr(GetCurrentProcess(), bk[i][bi].a, &d, si) ? si->Name : "?"); memset(buf, 0, sizeof buf); si->SizeOfStruct = sizeof(SYMBOL_INFO); si->MaxNameLen = 255; fprintf(stderr, "[PROF]   %5.1f%% %s <- %s\n", 100.0 * bn / tot[i], nm1, bk[i][bi].b && SymFromAddr(GetCurrentProcess(), bk[i][bi].b, &d, si) ? si->Name : "-"); }
            bk[i][bi].n = 0;
        }
    }
    { int q; for (q = 0; q < g_prof_chain_n; q++) fprintf(stderr, "[PROF] WriteFile chain: %s\n", g_prof_chain[q]); }
    if (s_prof_chains) for (i = 0; i < nt && i < 64; i++) { unsigned r, k; fprintf(stderr, "[PROF] chains thread %lu\n", tids[i]);
        for (r = 0; r < 5; r++) { unsigned bi = 0, bn = 0; for (k = 0; k < 24; k++) if (s_pchain[i][k].n > bn) { bn = s_pchain[i][k].n; bi = k; } if (!bn) break; fprintf(stderr, "[PROF]   x%u %s\n", bn, s_pchain[i][bi].txt); s_pchain[i][bi].n = 0; } }
    fflush(stderr);
    return 0;
}
/* Freeze dump (from the watchdog): suspend each thread in turn and print up to 24 symbolized frames. Needs a PDB. */
static void gb_freeze_dump(int pass)
{
    static int sym_ok; DWORD self = GetCurrentThreadId(); HANDLE s; THREADENTRY32 te; HANDLE proc = GetCurrentProcess();
    static DWORD sym_err;
    /* ERROR_INVALID_PARAMETER (87) = already initialised for this process (main.c's crash handler does it): usable. */
    if (!sym_ok) { SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS); sym_ok = SymInitialize(proc, NULL, TRUE) ? 1 : -1; if (sym_ok < 0) { sym_err = GetLastError(); if (sym_err == ERROR_INVALID_PARAMETER) sym_ok = 1; } }
    /* exe base printed so a dump without symbols can still be resolved offline against the PDB */
    fprintf(stderr, "[FREEZE] dump %d: all threads (symbols %s, err %lu) exe base %p\n", pass, sym_ok > 0 ? "ok" : "unavailable", (unsigned long)sym_err, (void *)GetModuleHandleA(NULL));
    s = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0); te.dwSize = sizeof te;
    if (s == INVALID_HANDLE_VALUE) return;
    if (Thread32First(s, &te)) do {
        HANDLE h; CONTEXT c; char line[2400]; int fr, nfr = 0, fault = 0; DWORD64 rips[24];
        if (te.th32OwnerProcessID != GetCurrentProcessId() || te.th32ThreadID == self) continue;
        h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
        if (!h) continue;
        line[0] = 0;
        /* Only unwind while suspended; symbol lookup allocates, so it waits until the thread runs again. */
        if (SuspendThread(h) != (DWORD)-1) {
            memset(&c, 0, sizeof c); c.ContextFlags = CONTEXT_FULL;
            if (GetThreadContext(h, &c)) {
                __try {
                    for (; nfr < 24 && c.Rip; nfr++) {
                        DWORD64 base, ef; PVOID hd; PRUNTIME_FUNCTION rf;
                        rips[nfr] = c.Rip;
                        rf = RtlLookupFunctionEntry(c.Rip, &base, NULL);
                        if (rf) RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, c.Rip, rf, &c, &hd, &ef, NULL);
                        else { c.Rip = *(DWORD64 *)c.Rsp; c.Rsp += 8; }   /* leaf function */
                    }
                } __except (EXCEPTION_EXECUTE_HANDLER) { fault = 1; }
            }
            ResumeThread(h);
        }
        CloseHandle(h);
        for (fr = 0; fr < nfr; fr++) {
            char b[sizeof(SYMBOL_INFO) + 128]; SYMBOL_INFO *si = (SYMBOL_INFO *)b; DWORD64 d = 0; char nm[160];
            memset(b, 0, sizeof b); si->SizeOfStruct = sizeof(SYMBOL_INFO); si->MaxNameLen = 127;
            if (sym_ok > 0 && SymFromAddr(proc, rips[fr], &d, si)) _snprintf(nm, sizeof nm, "%s+0x%llx", si->Name, (unsigned long long)d);
            else _snprintf(nm, sizeof nm, "%llx", (unsigned long long)rips[fr]);
            nm[sizeof nm - 1] = 0;
            if (strlen(line) + strlen(nm) + 4 < sizeof line) { strcat(line, nm); strcat(line, " < "); }
        }
        fprintf(stderr, "[FREEZE] tid %lu: %s%s\n", te.th32ThreadID, line, fault ? "(unwind fault)" : "");
    } while (Thread32Next(s, &te));
    CloseHandle(s);
    fflush(stderr);
}

void rbx_d3d11_register(void)
{
    const char *e = getenv("RECOMP_D3D11");
    if (e && e[0] == '0') { fprintf(stderr, "[RBX DX11] disabled (RECOMP_D3D11=0): CPU rasteriser\n"); return; }
    if (!gb_ready()) { fprintf(stderr, "[RBX DX11] D3D11 init failed, using CPU rasteriser\n"); return; }
    nv2a_backend_register(&s_backend);
    if (!getenv("RECOMP_D3D_PRESENT")) _putenv_s("RECOMP_D3D_PRESENT", "1");
    { static char pe[64]; const char *pv = getenv("RECOMP_PROF"); if (pv) { strncpy(pe, pv, 63); CreateThread(NULL, 0, gb_prof_thread, pe, 0, NULL); } }
    if (getenv("RECOMP_GPU_WATCHDOG")) CreateThread(NULL, 0, gb_watchdog, NULL, 0, NULL);

    fprintf(stderr, "[RBX DX11] D3D11 NV2A back end registered\n");
}






#ifdef MVC2_SAVE_STATES
/* After a save-state load, on the coordinator with the GPU executor parked:
 * drop deferred readbacks (they hold post-snapshot frames and would overwrite
 * restored guest RAM at the next flip) and force a full texture re-verify. */
void rbx_gpu_state_after_load(void)
{
    int i;
    for (i = 0; i < MAX_RT; i++) { s_rt[i].pend = 0; s_rt[i].dirty = 0; }
    for (i = 0; i < MAX_TEX; i++) s_tx[i].vframe = 0;
    s_shot_w = s_shot_h = 0;
}
#endif
