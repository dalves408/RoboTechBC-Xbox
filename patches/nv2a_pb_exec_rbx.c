/* Robotech: project-pinned copy of xboxrecomp src/kernel/nv2a_pb_exec.c
 * (linked instead of the shared file, see CMakeLists.txt). RBX changes:
 *  - post-transform vertex cache in raster_batch_program (indices repeat);
 *  - program batches transformed on worker threads (RECOMP_XF_THREADS);
 *  - no QPC pair around every vertex in transform_vertex;
 *  - CPU z-buffer not cleared while a back end rasterises (it is unused);
 *  - RECOMP_PB_EXEC_VERBOSE getenv cached in draw_primitive. */
/**
 * Execute the parts of the title's pushbuffer that produce visible pixels.
 *
 * The title builds NV2A commands in guest RAM and advances DMA_PUT; without
 * something consuming them the framebuffer stays whatever it was, which is how
 * a fully booted title renders a black screen. This walks the same command
 * stream nv2a_pb_scan.c surveys and carries out the subset that decides what is
 * on screen: which surface is being drawn into, and clearing it.
 *
 * It also rasterises geometry, but only the part that can be drawn honestly:
 * batches whose attribute 0 is already in screen space, flat-shaded, straight
 * into the same guest framebuffer the clear writes. Titles draw their UI, HUD
 * and 2D overlays that way, so it is the first geometry to appear. Batches that
 * need a vertex program executed are counted and skipped rather than drawn
 * somewhere wrong -- see raster_batch(). Texturing, depth and vertex programs
 * are still a renderer, not a command decoder; the upgrade path is the D3D11
 * translator in src/nv2a/nv2a_pgraph_d3d11.c.
 *
 * Everything this does not handle is counted and ranked by
 * nv2a_pb_exec_report(), so what remains is a list rather than a guess.
 *
 * Enabled with RECOMP_PB_EXEC. RECOMP_RASTER_TEST draws one known triangle
 * after every clear, which separates "the pixel path is broken" from "the title
 * has not given us any vertices". RECOMP_FB_DUMP=<prefix> writes the surface to
 * <prefix>NNN.bmp, so the result can be looked at without a display.
 */
#include <emmintrin.h>
#include <math.h>
#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "kernel.h"   /* XBOX_CONTIG_BASE / XBOX_CONTIG_SIZE */
#include "xbox_memory_layout.h"   /* xbox_Nv2aFrameCounterFlip */
#include "nv2a_flip_hold.h"
#include "platform/host_time.h"   /* xbox_HostNowNs */
/* The swizzle decoder the D3D8 layer already uses -- one implementation of
 * Morton order, not a second one that can disagree with it. */
#include "../d3d/d3d8_swizzle.h"
#include "nv2a_vsh_interp.h"
#include "nv2a_combiner.h"
#include "nv2a_backend.h"
#include "rbx_gvs.h"   /* GPU vertex programs: hand-off to the D3D11 back end */
#if defined(_WIN32)
#include <windows.h>
#define NV_TLS __declspec(thread)
#else
#define NV_TLS __thread
#endif

/* Where the executor's time goes (QPC ticks; the GPU back end's stats line
 * reads and resets them): all of raster_batch, the vertex loop of program
 * batches (attribute fetch + interpreter), the interpreter alone, clip and
 * triangle assembly, and the hand-off to the back end. */
unsigned long long g_pb_t_batch, g_pb_t_xf, g_pb_t_vsh, g_pb_t_clip, g_pb_t_flush;
unsigned g_pb_n_xfverts;
#if defined(_WIN32)
static unsigned long long pb_ticks(void) { LARGE_INTEGER t; QueryPerformanceCounter(&t); return (unsigned long long)t.QuadPart; }
#else
static unsigned long long pb_ticks(void) { return 0; }
#endif

extern ptrdiff_t xbox_GetMemoryOffset(void);
extern void xbox_FramebufferWindowSet(uint32_t fb_va, uint32_t pitch);
extern void xbox_FramebufferWindowSetAA(uint32_t sy);
volatile uint32_t g_nv2a_flip_va;   /* the surface this flip shows, for a back end that presents it itself */
extern uint32_t xbox_Nv2aFenceWordVa(void);
extern int nv2a_pb_dma_walk(void);   /* nv2a_pb_scan.c: RECOMP_PB_WALK=dma */
extern void xbox_FramebufferWindowStart(void);
extern uint32_t g_xbox_image_lo, g_xbox_image_hi;

/* Would writing this surface land on the title's own image?
 *
 * NV097_SET_SURFACE_COLOR_OFFSET is an offset within the colour DMA object,
 * not a guest virtual address, and this executor has always used it as one.
 * That is harmless while the two happen to agree and catastrophic when they do
 * not: the Xbox Dashboard names surface 0x00088000 at 1280x960x4, so clearing
 * it wrote 4.9 MB of opaque black from 0x00088000 to 0x00538000 -- straight
 * over its own code, its D3D context at 0x000BBFC0 and the register-block
 * pointer at 0x000BE2C4. The symptom was a title that submitted one perfect
 * frame and then spun forever in a pushbuffer-full loop, three layers away,
 * with every D3D global reading 0xFF000000: the clear colour.
 *
 * So refuse, and say so. Getting the address right needs the DMA object base
 * this ignores (NV097_SET_CONTEXT_DMA_COLOR); until that exists, writing
 * nothing is strictly better than writing over the guest, and a title that
 * cannot draw is easier to debug than one that has been overwritten.
 */
static int guest_span_ok(uint64_t addr, uint32_t bytes);

static int surface_hits_image(uint32_t base, uint32_t bytes)
{
    if (!bytes)
        return 0;
    /* The thread stacks sit just above the image. No surface lives there, so
     * a DMA offset that reads as a VA in that range is just as wrong as one
     * that lands on the image. */
    if (base < XBOX_HEAP_BASE && (uint64_t)base + bytes > XBOX_STACK_BASE)
        return 1;
    /* And anything that is not guest memory at all: a clear wrote to
     * 0xC532311B, an offset that names no memory the guest owns. */
    if (!guest_span_ok(base, bytes))
        return 1;
    if (!g_xbox_image_hi)
        return 0;
    return base < g_xbox_image_hi && base + bytes > g_xbox_image_lo;
}

/* Where a DMA-object offset actually lives.
 *
 * NV097_SET_SURFACE_COLOR_OFFSET is an offset inside the colour DMA object,
 * and for a framebuffer that object covers physical memory -- so the offset
 * is a physical address, not a guest VA. Those are the same number in this
 * runtime, which is why treating it as a VA works until it does not: on
 * hardware the image is mapped at VA 0x00010000 from arbitrary physical
 * pages, so a framebuffer at physical 0x84000 does not overlap it. Here it
 * would.
 *
 * The title tells us which it is by where it allocated. Half-Life 2's
 * framebuffer comes from MmAllocateContiguousMemory, which this runtime
 * serves from the window at XBOX_CONTIG_BASE, so physical P is visible at
 * XBOX_CONTIG_BASE + P -- clear of the image, and the same bytes the title's
 * own writes and the framebuffer window reach.
 *
 * So: use the offset as a VA when that is credible, and fall back to the
 * physical mirror exactly when it is not. Titles whose surfaces already sit
 * in ordinary RAM (Wreckless renders to the tiled alias of physical
 * 0x01954000) keep the first path and are unaffected.
 */
static uint32_t dma_resolve(uint32_t offset)
{
    extern uint32_t xbox_ContiguousAllocatedBytes(void);

    /* Did this runtime hand the offset out as contiguous memory? Then the
     * bytes live in the window, and that is not a guess: the arena is a bump
     * allocator from XBOX_CONTIG_BASE, so everything below its high-water
     * mark is memory some MmAllocateContiguousMemory call returned. The
     * title's own writes go through the window, so the executor's must too.
     *
     * Checking this BEFORE the image test is the whole point. The image test
     * only catches an offset that would land on the title's code, and whether
     * it does is an accident of where the image happens to end: Half-Life 2's
     * colour surface is physical 0x00A6C000, which clears the image by 700 KB.
     * So it looked like an ordinary VA, and the executor cleared 1.2 MB of
     * black straight through the guest heap -- which faulted the title three
     * frames later on a pointer that had been overwritten, while the real
     * framebuffer at 0x80A6C000 stayed untouched and the screen stayed black. */
    if (offset < xbox_ContiguousAllocatedBytes())
        return XBOX_CONTIG_BASE + offset;
    if (!surface_hits_image(offset, 1))
        return offset;
    if ((uint64_t)offset < XBOX_CONTIG_SIZE)
        return XBOX_CONTIG_BASE + offset;
    return offset;                         /* nothing better to offer */
}

static int surface_write_refused(uint32_t base, uint32_t bytes, const char *what)
{
    static int said;

    if (!surface_hits_image(base, bytes))
        return 0;
    if (!said) {
        said = 1;
        fprintf(stderr,
                "  [GPU] REFUSING to %s surface 0x%08X..0x%08X: that overlaps "
                "the loaded image (0x%08X..0x%08X), a thread stack, or is "
                "outside guest memory.\n"
                "  [GPU]   SET_SURFACE_COLOR_OFFSET is a DMA-object offset, not "
                "a guest VA, and this executor treats it as one. Writing here "
                "would destroy the title's own code and globals.\n",
                what, base, base + bytes, g_xbox_image_lo, g_xbox_image_hi);
        fflush(stderr);
    }
    return 1;
}

/* NV097 methods this executor acts on. */
/* Blending. The pair this title programs, read from its own pushbuffer
 * rather than guessed: BLEND_ENABLE written 1168 times and left on,
 * SFACTOR 0x0302 (SRC_ALPHA) and DFACTOR 0x0303 (ONE_MINUS_SRC_ALPHA).
 * ALPHA_TEST_ENABLE is written 390 times and left at zero, so this is
 * blending and not an alpha test. */
#define NV097_SET_BLEND_ENABLE            0x0304
#define NV097_SET_BLEND_FUNC_SFACTOR      0x0344
#define NV097_SET_BLEND_FUNC_DFACTOR      0x0348
#define NV_BLEND_SRC_ALPHA                0x0302
#define NV_BLEND_ONE_MINUS_SRC_ALPHA      0x0303
#define NV097_SET_BLEND_COLOR             0x034C
#define NV097_SET_BLEND_EQUATION          0x0350

#define NV097_SET_SURFACE_CLIP_HORIZONTAL 0x0200
#define NV097_SET_SURFACE_CLIP_VERTICAL   0x0204
#define NV097_SET_SURFACE_FORMAT          0x0208
#define NV097_SET_SURFACE_PITCH           0x020C
#define NV097_SET_SURFACE_COLOR_OFFSET    0x0210
#define NV097_SET_COLOR_CLEAR_VALUE       0x1D90
#define NV097_CLEAR_SURFACE               0x1D94
#define NV097_SET_VERTEX_DATA_ARRAY_OFFSET 0x1720   /* +i*4, 16 attributes */
#define NV097_SET_VERTEX_DATA_ARRAY_FORMAT 0x1760   /* +i*4 */
#define NV097_SET_BEGIN_END               0x17FC
#define NV097_SET_TEXTURE_OFFSET          0x1B00   /* +i*0x40 */
#define NV097_SET_TEXTURE_FORMAT          0x1B04
#define NV097_SET_TEXTURE_PALETTE         0x1B20   /* offset | size<<2 | dma */
#define NV097_SET_TEXTURE_ADDRESS         0x1B08
#define NV097_SET_TEXTURE_CONTROL1        0x1B10
#define NV097_SET_TEXTURE_IMAGE_RECT      0x1B1C
/* The buffer flip. A title double-buffers by telling the GPU which buffer
 * the CRTC reads and which it draws into, advancing the write index and
 * then stalling until the flip has happened. Ignoring these means the
 * stall never clears: Half-Life 2's loader submits its initialisation,
 * asks for a flip, and waits for it in a loop that makes no kernel calls
 * and burns no dispatch, which reads as a hang with no cause.
 *
 * The flip completes here at once, because there is no scanout to be in the
 * middle of; what the console's stall did to the title (wait for the vblank
 * the swap asked for) is the flip hold's job: FLIP_STALL arms it, and the
 * ack loop holds the next walk until the guest's vblank count allows the
 * flip (nv2a_flip_hold.h). The swap's interval comes from the NO_OPERATION
 * D3D pushes before it, software method 1. */
#define NV097_NO_OPERATION                0x0100
#define NV097_SET_FLIP_READ               0x0120
#define NV097_SET_FLIP_WRITE              0x0124
#define NV097_SET_FLIP_MODULO             0x0128
#define NV097_FLIP_INCREMENT_WRITE        0x012C
#define NV097_FLIP_STALL                  0x0130
#define NV097_ARRAY_ELEMENT16             0x1800
/* Draw a run of vertices straight out of the arrays, with no index list:
 * bits 0..23 are the first vertex, bits 24..31 the count minus one. It may
 * appear several times inside one BEGIN_END to draw a longer run. */
#define NV097_DRAW_ARRAYS                 0x1810
#define NV097_INLINE_ARRAY                0x1818
/* Immediate-mode vertices. SET_VERTEX3F/4F carry the position, and writing
 * its last component completes a vertex using whatever the SET_VERTEX_DATA*
 * registers currently hold for the other attributes. This is how Half-Life
 * 2's Xbox loader and the game's own 2D drawing submit every quad -- neither
 * uses INLINE_ARRAY -- so without these the executor saw SET_BEGIN_END pairs
 * with nothing attached and reported `draws 0` while a million and a half
 * textured quads a minute went past it. */
#define NV097_SET_VERTEX3F                0x1500   /* +0..0x08, 3 floats */
#define NV097_SET_VERTEX4F                0x1518   /* +0..0x0C, 4 floats */
#define NV097_SET_VERTEX_DATA2F_M         0x1880   /* + attr*8,  2 floats */
#define NV097_SET_VERTEX_DATA4F_M         0x1A00   /* + attr*16, 4 floats */
#define NV097_SET_VERTEX_DATA4UB          0x1940   /* + attr*4,  4 x u8 */
#define NV097_SET_VERTEX_DATA2S           0x1900   /* + attr*4,  2 x s16 */
#define NV097_SET_VERTEX_DATA4S_M         0x1980   /* + attr*8,  4 x s16 */

/* One immediate vertex, as this file packs it for the shared draw path:
 * position float4, diffuse D3DCOLOR, texcoord0 float2. */
/* An immediate-mode vertex: all 16 attributes as float4. */
#define IMM_VERTEX_DWORDS (NV_VERTEX_ATTRS * 4)

#define NV097_CLEAR_COLOR_MASK            0xF0   /* R,G,B,A bits */

/* One vertex attribute stream, as the title describes it. Attribute 0 is
 * position; the rest are colours, texture coordinates and so on. */
typedef struct {
    uint32_t offset;      /* guest address of element 0 */
    uint32_t type;        /* NV097 data type nibble */
    uint32_t size;        /* components per element */
    uint32_t stride;      /* bytes between elements */
} VertexAttr;

#define NV_VERTEX_ATTRS 16
#define NV_MAX_INDICES  65536
#define NV_MAX_INLINE   65536           /* dwords of INLINE_ARRAY per batch */

/* Texture stage 0, decoded from what the title programmed.
 *
 * Only stage 0: it is the only one the dashboard configures, and a stage
 * nothing writes to is a stage nothing can be sampled from. The rest arrive
 * as unhandled methods and are counted as such, which is how the next title
 * that needs them will say so. */
typedef struct {
    uint32_t offset;                    /* guest address of texel (0,0)  */
    uint32_t width, height;             /* from IMAGE_RECT               */
    uint32_t pitch;                     /* bytes per row, from CONTROL1  */
    uint32_t color;                     /* NV097 colour-format code      */
    uint32_t addr_u, addr_v;            /* wrap mode per axis            */
    uint32_t palette;                   /* guest address of the CLUT, P8 */
    uint32_t levels;                    /* mip levels, from FORMAT       */
    uint32_t filter;                    /* SET_TEXTURE_FILTER            */
    uint32_t border;                    /* SET_TEXTURE_BORDER_COLOR      */
    int      border_color;              /* FORMAT: BORDER reads `border` */
    int      cube;                      /* FORMAT: six faces, not one    */
    int      valid;
} Texture;

static struct {
    VertexAttr attr[NV_VERTEX_ATTRS];
    uint32_t   prim;                    /* SET_BEGIN_END parameter, 0 = ended */
    uint32_t   idx[NV_MAX_INDICES];     /* DRAW_ARRAYS starts are 24-bit */
    uint32_t   idx_count;
    /* INLINE_ARRAY payload: vertices written straight into the pushbuffer
     * instead of into a buffer the title points at. Same vertex format, a
     * different place to read them from. */
    uint32_t   inline_buf[NV_MAX_INLINE];
    uint32_t   inline_count;
    /* Current values of the immediate-mode attributes, and how many complete
     * vertices they have produced in this batch. */
    /* Immediate mode: every attribute's current value, as the SET_VERTEX
     * methods leave it. Writing attribute 0 (position) emits a vertex. */
    float      imm_attr[NV_VERTEX_ATTRS][4];
    uint16_t   imm_used;                /* attributes written since BEGIN */
    uint32_t   imm_count;
    int        inline_active;
    uint32_t   draws, verts, nonzero_draws;
    float      min_x, max_x, min_y, max_y;
    uint32_t color_offset, color_base, pitch, format;
    /* The surface the last batch actually drew into. A double-buffered title
     * has already pointed color_offset at the next buffer and cleared it by
     * the time the flip arrives, so dumping the current one dumps the frame
     * that has not been drawn yet -- which is how a correctly rendered
     * sequence came out as 12 black BMPs. */
    uint32_t drawn_offset;
    /* ...and the shape of that surface. A title also draws into small
     * render targets (Burnout 3: 128x128 shadow/reflection maps, pitch 512),
     * often after the main scene, so "last surface drawn" alone picked one of
     * those and read it with the framebuffer's pitch: horizontal noise. The
     * biggest surface drawn since the last flip is the one being presented. */
    uint32_t drawn_pitch, drawn_x, drawn_y, drawn_w, drawn_h, drawn_bpp;
    int drawn_stale;      /* set at a flip: the next draw starts a new frame */
    uint64_t pixels;
    uint32_t pixel_max;   /* brightest value any pixel write carried */
    uint32_t clip_x, clip_w, clip_y, clip_h;       /* real (anti-aliased) pixels */
    uint32_t clip_raw_h, clip_raw_v;    /* SET_SURFACE_CLIP_* as sent: logical */
    float    aa_sx, aa_sy;              /* anti-aliasing scale of the surface */
    uint32_t clear_color;
    uint32_t clears, unhandled_total;
    uint32_t flip_read, flip_write, flip_modulo, flips;
    uint32_t tris_drawn, tris_skipped_offscreen, batches_untransformed;
    /* Why a batch came out flat. "Untextured" has two causes that look
     * identical on screen and want opposite fixes: the batch carried no
     * texture coordinates, or it did and the stage was not usable. */
    uint32_t batches_textured, batches_no_uv, batches_no_tex;
    uint32_t blend_enable, blend_sfactor, blend_dfactor;
    uint32_t blend_equation, blend_color;
    uint32_t color_mask;        /* SET_COLOR_MASK: A<<24 R<<16 G<<8 B */
    /* Visibility tests (D3D's Begin/EndVisibilityTest): pixels that pass the
     * depth test while counting is on, reported by GET_REPORT. */
    uint32_t zpass_enable, zpass_count, reports;
    uint32_t blend_pairs[16];   /* sfactor<<16 | dfactor seen, for the report */
    int blend_npairs;
    Texture  texs[4];                   /* one per texture stage */
    /* Register combiners and the per-pixel state around them. rc_seen: the
     * title has programmed the combiners, so they decide every pixel's
     * colour; until then the old "stage 0 texel * diffuse" stands in. */
    Nv2aCombiner rc;
    int      rc_seen;
    uint32_t clip_plane_mode;           /* SET_SHADER_CLIP_PLANE_MODE */
    uint32_t alpha_test, alpha_func, alpha_ref;
    uint32_t fog_enable, fog_mode, fog_color;
    float    fog_param[2];
    /* Vertex programs (nv2a_vsh_interp.c) and the depth buffer, which
     * together are what a 3D scene needs and a 2D one never used. */
    uint32_t xf_mode;                   /* TRANSFORM_EXECUTION_MODE: 2 = program */
    uint32_t batches_program, verts_program, tris_behind;
    /* Where program-path triangles go, so "nothing drew" has a reason. */
    uint32_t xf_degenerate, xf_offscreen, xf_drawn;
    uint64_t xf_depth_fail, xf_pixels;
    float xf_min[3], xf_max[3];
    uint32_t xf_fmt[16];  /* attribute formats seen: type | size<<4 | slot<<8 */
    int xf_nfmt, xf_seeded;
    uint32_t depth_test, depth_func, depth_mask;
    uint32_t zeta_offset, zstencil_clear;
    /* SET_CLEAR_RECT_HORIZONTAL / _VERTICAL (x0 | x1 << 16, inclusive):
     * the area CLEAR_SURFACE covers on a swizzled surface (clear_surface).
     * clear_rect_set: bit 0 horizontal, bit 1 vertical sent; the rect
     * applies once both have been. */
    uint32_t clear_rect_h, clear_rect_v;
    int clear_rect_set;
    /* Fixed-function transform: the composite matrix (world * view *
     * projection * viewport, as D3D uploads it) and whether the title has
     * sent one. The viewport offset is the constant-file entry c[59]. */
    float    composite[16];
    int      composite_set;
    uint32_t batches_ffp;
} s_gpu;

/* The last value written to every Kelvin method, as the GPU's register file
 * would hold it. State the executor does not model explicitly (lights,
 * material colours, cull and clip state handed to a back end) is read from
 * here. */
static uint32_t s_reg[0x2000 / 4];
static float reg_f(uint32_t method)
{
    float f;
    memcpy(&f, &s_reg[method / 4], 4);
    return f;
}

/* Where BACK_END_WRITE_SEMAPHORE_RELEASE lands; see nv2a_backend.h. */
static uint32_t s_sem_va;
void nv2a_pb_set_semaphore_target(uint32_t guest_va) { s_sem_va = guest_va; }

/* Unhandled methods, ranked. The interesting output is not that something was
 * skipped but which things dominate, because that is the order to implement
 * them in. */
#define PB_EXEC_MAX_UNHANDLED 2048
typedef struct { uint32_t method, count, last_param; } PbUnhandled;
static PbUnhandled s_unhandled[PB_EXEC_MAX_UNHANDLED];
static int s_unhandled_count;

/* Every texture-stage register, as the title last set it.
 * Texturing is not implemented yet; knowing which formats and sizes a title
 * actually programs is what decides which ones are worth implementing. */
#define NV_TEX_FIRST 0x1B00
#define NV_TEX_LAST  0x1BFC
static uint32_t s_tex_reg[(NV_TEX_LAST - NV_TEX_FIRST) / 4 + 1];
static uint8_t  s_tex_set[(NV_TEX_LAST - NV_TEX_FIRST) / 4 + 1];

/* Formats whose dimensions come from the format word and whose coordinates
 * arrive normalised, rather than from a pitch and SET_TEXTURE_IMAGE_RECT with
 * coordinates in texels. Swizzled and block-compressed are both in this group,
 * and every place that used to test only for swizzled needs the pair. */
static int tex_size_from_format(uint32_t fmt)
{
    return d3d8_format_is_swizzled(fmt) || d3d8_format_dxt_block_bytes(fmt);
}

static void tex_update_valid(Texture *t)
{
    t->valid = t->offset && t->width && t->height
            && (tex_size_from_format(t->color) || t->pitch);
}

static void record_tex_reg(uint32_t method, uint32_t param)
{
    s_tex_reg[(method - NV_TEX_FIRST) / 4] = param;
    s_tex_set[(method - NV_TEX_FIRST) / 4] = 1;
    /* A pitch is a linear texture's property. A swizzled one has no rows and
     * so no pitch, and requiring one here refused every swizzled texture --
     * which is nearly all of them, since swizzled is the Xbox default. That
     * left the title's own textures unsampled and every textured quad drawn in
     * flat vertex colour. */
    tex_update_valid(&s_gpu.texs[0]);
}

/* Every distinct texture a batch was drawn with, and how many batches used it.
 *
 * The per-draw verbose print shows the first few draws of the first frame,
 * which is enough to see that texturing works at all and not enough to answer
 * "is a font page ever bound". This is the same shape as the unhandled-method
 * table below it: a small set, ranked, printed with the rest of the report. */
#define PB_EXEC_MAX_TEXTURES 64
typedef struct {
    uint32_t offset, color, width, height, batches;
} PbTexUse;
static PbTexUse s_tex_use[PB_EXEC_MAX_TEXTURES];
static int s_tex_use_count;

/* Defined below, next to the sampler it goes through. */
static void dump_texture_bmp(uint32_t seq);

/* Repeat dumps are numbered from well past the first-use sequence, so a
 * listing sorts them after the textures they came from and no first-use file
 * is ever overwritten by one. */
#define TEX_DUMP_SEQ_BASE 1000u
#define TEX_DUMP_SEQ_MAX  40u

static void note_texture_use(void)
{
    int i;

    if (!s_gpu.texs[0].valid)
        return;
    for (i = 0; i < s_tex_use_count; i++) {
        if (s_tex_use[i].offset == s_gpu.texs[0].offset
         && s_tex_use[i].color  == s_gpu.texs[0].color) {
            s_tex_use[i].batches++;
            /* Dump a surface that is redrawn, every Nth time it is bound.
             *
             * First use alone cannot tell a decode error that is wrong in
             * every frame from one that accumulates across them. A block
             * transform that is wrong is wrong on its own, in the keyframe
             * as much as anywhere; motion compensation that is wrong starts
             * from a clean keyframe and smears further with each predicted
             * frame after it. In a single frame the two look identical, and
             * in a sequence they look nothing alike -- so the sequence is
             * what has to be captured.
             *
             * It belongs on this side of the return: a video surface keeps
             * one address for the whole film, so after the first frame it is
             * only ever found here, and the first-use dump below never fires
             * for it again. RECOMP_TEX_DUMP_EVERY=<n> sets the interval, and
             * RECOMP_TEX_DUMP still names the files. */
            {
                static int every = -1;
                static unsigned binds, seq;
                if (every < 0) {
                    const char *e = getenv("RECOMP_TEX_DUMP_EVERY");
                    every = e ? atoi(e) : 0;
                }
                if (every > 0 && ++binds % (unsigned)every == 0
                    && seq < TEX_DUMP_SEQ_MAX)
                    dump_texture_bmp(TEX_DUMP_SEQ_BASE + seq++);
            }
            return;
        }
    }
    if (s_tex_use_count < PB_EXEC_MAX_TEXTURES) {
        s_tex_use[s_tex_use_count].offset  = s_gpu.texs[0].offset;
        s_tex_use[s_tex_use_count].color   = s_gpu.texs[0].color;
        s_tex_use[s_tex_use_count].width   = s_gpu.texs[0].width;
        s_tex_use[s_tex_use_count].height  = s_gpu.texs[0].height;
        s_tex_use[s_tex_use_count].batches = 1;
        s_tex_use_count++;
        dump_texture_bmp((uint32_t)s_tex_use_count - 1);
    }
}

/* Slot + 1 of each method in s_unhandled: this runs for every method the
 * executor does not model, millions of times a second, so no search. The
 * report sorts s_unhandled and rebuilds the slots afterwards. */
static uint16_t s_unhandled_slot[0x2000 / 4];

static void note_unhandled(uint32_t method, uint32_t param)
{
    uint16_t *slot = &s_unhandled_slot[(method & 0x1FFC) / 4];

    s_gpu.unhandled_total++;
    if (*slot) {
        s_unhandled[*slot - 1].count++;
        s_unhandled[*slot - 1].last_param = param;
        return;
    }
    if (s_unhandled_count < PB_EXEC_MAX_UNHANDLED) {
        s_unhandled[s_unhandled_count].method = method;
        s_unhandled[s_unhandled_count].count = 1;
        s_unhandled[s_unhandled_count].last_param = param;
        s_unhandled_count++;
        *slot = (uint16_t)s_unhandled_count;
    }
}

/* Is [addr, addr + bytes) inside memory the guest owns? That is ordinary RAM
 * from 0, and the contiguous window at XBOX_CONTIG_BASE. The index of a vertex
 * comes from the title's pushbuffer, 32 bits of it, so after a desync it can
 * be anything; this is the one place every vertex read passes through. */
static int guest_span_ok(uint64_t addr, uint32_t bytes)
{
    uint64_t end = addr + bytes;

    if (end <= (uint64_t)XBOX_CONTIG_SIZE)
        return 1;
    return addr >= XBOX_CONTIG_BASE
        && end <= (uint64_t)XBOX_CONTIG_BASE + XBOX_CONTIG_SIZE;
}

/* Bytes one element of attribute `a` occupies, as fetch_attr reads it. */
static uint32_t attr_elem_bytes(const VertexAttr *a)
{
    uint32_t n = a->size < 4 ? a->size : 4;

    switch (a->type) {
    case 0: case 6: return 4;
    case 1: case 5: return 2u * n;
    case 4:         return n;
    default:        return 4u * n;
    }
}

/* Read attribute `a` of vertex `index` as floats. Only the float and the
 * normalised-byte types appear in practice; anything else returns 0 so a
 * caller sees a degenerate vertex rather than reading past the array. */
/* RBX: forced inline. transform_vertex calls this for each of 16 inputs of
 * every vertex, and the bench profile put 9% of the pushbuffer thread on
 * its opening line alone: call and prologue overhead, not the reads. */
#if defined(_MSC_VER)
#define RBX_FORCEINLINE static __forceinline
#else
#define RBX_FORCEINLINE static inline __attribute__((always_inline))
#endif
RBX_FORCEINLINE int fetch_attr(const VertexAttr *a, uint32_t index, float out[4])
{
    /* RBX: guest RAM is mapped once for the life of the process. This runs
     * for every attribute of every vertex (7% of the pushbuffer thread in the
     * bench profile), so keep the base instead of an out-of-line call each
     * time. Workers may race to set it; they all store the same value. */
    static const uint8_t *volatile s_mem_base;
    const uint8_t *mem = s_mem_base;
    const uint8_t *p;
    uint32_t i;

    out[0] = out[1] = out[2] = 0.0f;
    out[3] = 1.0f;
    if (!a->size || !a->stride)
        return 0;
    if (s_gpu.inline_active) {
        /* The batch arrived as INLINE_ARRAY, so `offset` is a byte offset into
         * the buffered payload rather than a guest address -- and 0 is a legal
         * one there, which is why the offset test is on the other side of this
         * branch. */
        size_t at = (size_t)a->offset + (size_t)index * a->stride;
        if (at + 4 > (size_t)s_gpu.inline_count * 4)
            return 0;
        p = (const uint8_t *)s_gpu.inline_buf + at;
    } else {
        uint64_t at = (uint64_t)a->offset + (uint64_t)index * a->stride;

        if (!a->offset || !guest_span_ok(at, attr_elem_bytes(a)))
            return 0;
        if (!mem)
            s_mem_base = mem = (const uint8_t *)xbox_GetMemoryOffset();
        p = mem + at;
    }

    switch (a->type) {
    case 0:                                  /* D3DCOLOR */
        /* A DWORD 0xAARRGGBB, so little-endian bytes are B,G,R,A -- not the
         * component order of every other format here. Returned as R,G,B,A so
         * callers need not know which format the title chose. */
        out[0] = (float)p[2] / 255.0f;
        out[1] = (float)p[1] / 255.0f;
        out[2] = (float)p[0] / 255.0f;
        out[3] = (float)p[3] / 255.0f;
        return 1;
    case 2:                                  /* float */
        for (i = 0; i < a->size && i < 4; i++)
            out[i] = ((const float *)p)[i];
        return 1;
    case 4:                                  /* unsigned byte, normalised */
        for (i = 0; i < a->size && i < 4; i++)
            out[i] = (float)p[i] / 255.0f;
        return 1;
    /* The formats 3D geometry uses and screen-space quads never did. */
    case 1:                                  /* signed short, normalised */
        for (i = 0; i < a->size && i < 4; i++)
            out[i] = (float)((const int16_t *)p)[i] / 32767.0f;
        return 1;
    case 5:                                  /* signed short, as is */
        for (i = 0; i < a->size && i < 4; i++)
            out[i] = (float)((const int16_t *)p)[i];
        return 1;
    case 6: {                                /* packed 11:11:10 normal */
        uint32_t v = *(const uint32_t *)p;
        out[0] = (float)((int32_t)(v << 21) >> 21) / 1023.0f;
        out[1] = (float)((int32_t)(v << 10) >> 21) / 1023.0f;
        out[2] = (float)((int32_t)v >> 22) / 511.0f;
        return 1;
    }
    default:
        return 0;
    }
}

static uint32_t surface_bpp(void);

static void note_drawn(void)
{
    if (!s_gpu.drawn_stale && s_gpu.drawn_offset
        && s_gpu.clip_w * s_gpu.clip_h < s_gpu.drawn_w * s_gpu.drawn_h)
        return;
    s_gpu.drawn_stale = 0;
    s_gpu.drawn_offset = s_gpu.color_offset;
    s_gpu.drawn_pitch = s_gpu.pitch;
    s_gpu.drawn_bpp = surface_bpp();
    s_gpu.drawn_x = s_gpu.clip_x; s_gpu.drawn_y = s_gpu.clip_y;
    s_gpu.drawn_w = s_gpu.clip_w; s_gpu.drawn_h = s_gpu.clip_h;
}

static uint32_t surface_bpp(void)
{
    /* The surface format says it outright (NV097_SET_SURFACE_FORMAT_COLOR).
     * Deriving it from pitch / clip width -- the only way before -- is right
     * only while the clip spans the whole surface: Burnout 3 narrows the clip
     * to a 250-pixel window to draw its option values, 2560 / 250 came out as
     * 10 bytes a pixel, and every such text quad was refused. */
    switch (s_gpu.format & 0xF) {
    case 0x1: case 0x2: case 0x3: case 0xA:
        return 2;
    case 0x4: case 0x5: case 0x6: case 0x7: case 0x8:
        return 4;
    case 0x9:
        return 1;
    default:
        break;
    }
    /* The pitch and the clip width together give the pixel size, which is more
     * reliable than decoding the format field: the format's colour code is
     * only meaningful alongside a type the title also sets, while the pitch is
     * always exactly how many bytes a row occupies. */
    if (!s_gpu.clip_w)
        return 0;
    return s_gpu.pitch / s_gpu.clip_w;
}


/* Write the current surface out as a 24-bit BMP.
 *
 * A framebuffer window needs someone watching it. A file does not, which makes
 * this the only way to check what a title actually rendered on a machine you
 * are not sitting at -- and the only way to put a picture in a bug report.
 *
 * ponytail: bottom-up 24bpp BMP, no palette, no compression. That is the one
 * format every viewer reads and it is 30 lines; PNG would need a dependency.
 */
/* Set while RECOMP_FB_DUMP_FLIPS is dumping every flip: the report and
 * after-draw dumps stand down then, or they would put duplicate frames into
 * the sequence. */
static int s_flip_dumping;

static void dump_surface_bmp(void)
{
    const char *prefix = getenv("RECOMP_FB_DUMP");
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
    int drawn = s_gpu.drawn_offset != 0;
    uint32_t offset = drawn ? s_gpu.drawn_offset : s_gpu.color_offset;
    uint32_t pitch = drawn ? s_gpu.drawn_pitch : s_gpu.pitch;
    uint32_t cx = drawn ? s_gpu.drawn_x : s_gpu.clip_x;
    uint32_t cy = drawn ? s_gpu.drawn_y : s_gpu.clip_y;
    static int seq;
    char path[512];
    uint32_t w = drawn ? s_gpu.drawn_w : s_gpu.clip_w;
    uint32_t h = drawn ? s_gpu.drawn_h : s_gpu.clip_h, y, x;
    uint32_t bpp = drawn ? s_gpu.drawn_bpp : surface_bpp();
    uint32_t row_bytes, pad, filesz;
    uint32_t base;
    const uint32_t ram = 64u * 1024u * 1024u;
    uint8_t hdr[54];
    FILE *f;

    if (!prefix || !w || !h || (bpp != 2 && bpp != 4) || !offset || !pitch)
        return;

    /* RBX-17: drawn_* / clip can disagree with the real surface; a bad
     * pitch×row walk AV'd the process mid-mission (VA 0xE793E86A). Clamp
     * to the 64 MB map before touching pixels. */
    base = dma_resolve(offset);
    if (base >= ram)
        return;
    {
        uint64_t room = ram - base;
        uint64_t max_rows = room / pitch;
        uint32_t max_x;
        if (cy >= max_rows)
            return;
        if ((uint64_t)cy + h > max_rows)
            h = (uint32_t)(max_rows - cy);
        if (!h)
            return;
        max_x = (uint32_t)(pitch / bpp);
        if (cx >= max_x)
            return;
        if (cx + w > max_x)
            w = max_x - cx;
        if (!w)
            return;
    }

    row_bytes = w * 3;
    pad = (4 - (row_bytes & 3)) & 3;
    filesz = 54 + (row_bytes + pad) * h;

    snprintf(path, sizeof path, "%s%05d.bmp", prefix, seq++);
    f = fopen(path, "wb");
    if (!f)
        return;

    memset(hdr, 0, sizeof hdr);
    hdr[0] = 'B'; hdr[1] = 'M';
    memcpy(hdr + 2, &filesz, 4);
    hdr[10] = 54;
    hdr[14] = 40;
    memcpy(hdr + 18, &w, 4);
    memcpy(hdr + 22, &h, 4);
    hdr[26] = 1;
    hdr[28] = 24;
    fwrite(hdr, 1, sizeof hdr, f);

    /* Resolve AA into a logical-size dump when the surface is taller/wider
     * than the display (CS: clip 640x960 with aa_sy=2). Dumping raw AA
     * rows looked like a cropped top-half boot frame. */
    {
        uint32_t aa_sy = s_gpu.aa_sy > 1.5f ? 2u : 1u;
        uint32_t aa_sx = s_gpu.aa_sx > 1.5f ? 2u : 1u;
        if (aa_sy < 2u && w == 640u && h >= 960u)
            aa_sy = 2u;
        if (aa_sx < 2u && h == 480u && w >= 1280u)
            aa_sx = 2u;
        uint32_t out_w = w / aa_sx;
        uint32_t out_h = h / aa_sy;
        uint32_t out_row, out_pad, out_filesz;
        uint8_t out_hdr[54];

        if (out_w && out_h && (aa_sx > 1 || aa_sy > 1)) {
            uint32_t max_row = cy + (out_h ? (out_h - 1) * aa_sy : 0);
            uint64_t last = (uint64_t)base + (uint64_t)max_row * pitch
                          + (uint64_t)(cx + (out_w ? (out_w - 1) * aa_sx : 0) + 1) * bpp;
            /* RBX-17: AA-resolve dumped past RAM when drawn_* disagreed with
             * the real surface (menu AV at 0x0089C8CA). Fall through to the
             * raw dump when the resolve would read out of the 64 MB map. */
            if (base < 64u * 1024u * 1024u
                && last <= 64ull * 1024ull * 1024ull
                && max_row < h) {
                fclose(f);
                out_row = out_w * 3;
                out_pad = (4 - (out_row & 3)) & 3;
                out_filesz = 54 + (out_row + out_pad) * out_h;
                f = fopen(path, "wb");
                if (!f)
                    return;
                memset(out_hdr, 0, sizeof out_hdr);
                out_hdr[0] = 'B'; out_hdr[1] = 'M';
                memcpy(out_hdr + 2, &out_filesz, 4);
                out_hdr[10] = 54;
                out_hdr[14] = 40;
                memcpy(out_hdr + 18, &out_w, 4);
                memcpy(out_hdr + 22, &out_h, 4);
                out_hdr[26] = 1;
                out_hdr[28] = 24;
                fwrite(out_hdr, 1, sizeof out_hdr, f);
                for (y = out_h; y-- > 0; ) {
                    const uint8_t *row = mem + base
                        + (size_t)(cy + y * aa_sy) * pitch;
                    for (x = 0; x < out_w; x++) {
                        uint8_t bgr[3];
                        if (bpp == 4) {
                            uint32_t v = ((const uint32_t *)row)[cx + x * aa_sx];
                            bgr[0] = (uint8_t)(v);
                            bgr[1] = (uint8_t)(v >> 8);
                            bgr[2] = (uint8_t)(v >> 16);
                        } else {
                            uint16_t v = ((const uint16_t *)row)[cx + x * aa_sx];
                            bgr[0] = (uint8_t)(( v        & 0x1F) << 3);
                            bgr[1] = (uint8_t)(((v >>  5) & 0x3F) << 2);
                            bgr[2] = (uint8_t)(((v >> 11) & 0x1F) << 3);
                        }
                        fwrite(bgr, 1, 3, f);
                    }
                    if (out_pad) {
                        static const uint8_t zero[3] = {0, 0, 0};
                        fwrite(zero, 1, out_pad, f);
                    }
                }
                fclose(f);
                if (seq == 1)
                    fprintf(stderr,
                            "  [GPU] framebuffer dump: %s (%ux%u AA-resolved "
                            "from %ux%u @0x%08X %ubpp)\n",
                            path, out_w, out_h, w, h, s_gpu.color_offset, bpp);
                return;
            }
        }
    }

    /* BMP rows run bottom-up. */
    for (y = h; y-- > 0; ) {
        const uint8_t *row = mem + base + (size_t)(cy + y) * pitch;
        for (x = 0; x < w; x++) {
            uint8_t bgr[3];
            if (bpp == 4) {
                uint32_t v = ((const uint32_t *)row)[cx + x];
                bgr[0] = (uint8_t)(v);
                bgr[1] = (uint8_t)(v >> 8);
                bgr[2] = (uint8_t)(v >> 16);
            } else {
                uint16_t v = ((const uint16_t *)row)[cx + x];
                bgr[0] = (uint8_t)(( v        & 0x1F) << 3);
                bgr[1] = (uint8_t)(((v >>  5) & 0x3F) << 2);
                bgr[2] = (uint8_t)(((v >> 11) & 0x1F) << 3);
            }
            fwrite(bgr, 1, 3, f);
        }
        if (pad) {
            static const uint8_t zero[3] = {0, 0, 0};
            fwrite(zero, 1, pad, f);
        }
    }
    fclose(f);
    if (seq == 1)
        fprintf(stderr, "  [GPU] framebuffer dump: %s (%ux%u from 0x%08X %ubpp)\n",
                path, w, h, s_gpu.color_offset, bpp);
}

/* Defined below, next to the rest of the rasteriser; the clear path uses it
 * for RECOMP_RASTER_TEST. */
static void raster_triangle(const float a[2], const float b[2],
                            const float c[2], uint32_t argb,
                            const float uv[3][2]);

/* The surface rectangle in real pixels.
 *
 * SET_SURFACE_CLIP_* is in logical pixels; with anti-aliasing on, the surface
 * is larger by the AA factor in SET_SURFACE_FORMAT bits 12-15 (0 = 1x1,
 * 1 = 2x1 "center corner 2", 2 = 2x2 "square offset 4"), and so are the
 * coordinates a vertex program or the composite matrix produces. X-Men
 * Legends renders its 640x480 menu into a 1280x960 surface (pitch 5120),
 * which read as an 8-bytes-per-pixel surface and drew nothing until this was
 * applied. */
static void surface_apply_clip(void)
{
    uint32_t aa = (s_gpu.format >> 12) & 0xF;
    uint32_t sx = aa ? 2 : 1, sy = aa == 2 ? 2 : 1;

    s_gpu.aa_sx = (float)sx;
    s_gpu.aa_sy = (float)sy;

    s_gpu.clip_x = (s_gpu.clip_raw_h & 0xFFFF) * sx;
    s_gpu.clip_w = ((s_gpu.clip_raw_h >> 16) & 0xFFFF) * sx;
    s_gpu.clip_y = (s_gpu.clip_raw_v & 0xFFFF) * sy;
    s_gpu.clip_h = ((s_gpu.clip_raw_v >> 16) & 0xFFFF) * sy;
}

/* ---- Render back end (nv2a_backend.h) ------------------------------- */

static const Nv2aBackend *s_backend;

void nv2a_backend_register(const Nv2aBackend *backend)
{
    s_backend = backend;
}

static int backend_draws(void)
{
    return s_backend && s_backend->draw;
}

static void current_surface(Nv2aSurface *out)
{
    out->color_va        = dma_resolve(s_gpu.color_offset);
    out->width           = s_gpu.clip_w;
    out->height          = s_gpu.clip_h;
    out->pitch           = s_gpu.pitch;
    out->bytes_per_pixel = surface_bpp();
    out->aa_sx           = s_gpu.aa_sx > 1.5f ? 2 : 1;
    out->aa_sy           = s_gpu.aa_sy > 1.5f ? 2 : 1;
}

/* Render state in the NV2A's own values, gathered from what the executor
 * tracks and from the register mirror for what it does not (culling, the
 * depth range). */
static void current_render_state(Nv2aRenderState *r)
{
    r->blend_enable      = s_gpu.blend_enable;
    r->blend_src         = s_gpu.blend_sfactor;
    r->blend_dst         = s_gpu.blend_dfactor;
    r->blend_eq          = s_gpu.blend_equation;
    r->blend_color       = s_gpu.blend_color;
    r->alpha_test_enable = s_gpu.alpha_test;
    r->alpha_func        = s_gpu.alpha_func;
    r->alpha_ref         = s_gpu.alpha_ref;
    r->depth_test_enable = s_gpu.depth_test;
    r->depth_func        = s_gpu.depth_func;
    r->depth_write       = s_gpu.depth_mask;
    r->color_mask        = s_gpu.color_mask;
    r->cull_enable       = s_reg[0x0308 / 4];        /* SET_CULL_FACE_ENABLE */
    r->cull_face         = s_reg[0x039C / 4];        /* SET_CULL_FACE */
    r->front_face        = s_reg[0x03A0 / 4];        /* SET_FRONT_FACE */
    r->zeta_va           = s_gpu.zeta_offset ? dma_resolve(s_gpu.zeta_offset) : 0;
    r->depth_min         = reg_f(0x0394);            /* SET_CLIP_MIN */
    r->depth_max         = reg_f(0x0398);            /* SET_CLIP_MAX */
    r->stencil_enable    = s_reg[0x032C / 4];        /* SET_STENCIL_TEST_ENABLE */
    r->stencil_write_mask = s_reg[0x0360 / 4];       /* SET_STENCIL_MASK */
    r->stencil_func      = s_reg[0x0364 / 4];        /* SET_STENCIL_FUNC */
    r->stencil_ref       = s_reg[0x0368 / 4];        /* SET_STENCIL_FUNC_REF */
    r->stencil_func_mask = s_reg[0x036C / 4];        /* SET_STENCIL_FUNC_MASK */
    r->stencil_fail      = s_reg[0x0370 / 4];        /* SET_STENCIL_OP_FAIL */
    r->stencil_zfail     = s_reg[0x0374 / 4];        /* SET_STENCIL_OP_ZFAIL */
    r->stencil_zpass     = s_reg[0x0378 / 4];        /* SET_STENCIL_OP_ZPASS */
}

static int sample_texture(uint32_t u, uint32_t v, uint32_t *argb);

int nv2a_backend_decode_texture(const Nv2aTexture *tex, uint32_t *argb_out)
{
    Texture saved = s_gpu.texs[0];
    uint32_t x, y;
    int ok = 1;

    s_gpu.texs[0].offset = tex->offset;
    s_gpu.texs[0].width  = tex->width;
    s_gpu.texs[0].height = tex->height;
    s_gpu.texs[0].pitch  = tex->pitch;
    s_gpu.texs[0].color  = tex->color;
    s_gpu.texs[0].addr_u = 3;
    s_gpu.texs[0].addr_v = 3;
    s_gpu.texs[0].palette = tex->palette;
    s_gpu.texs[0].levels = 1;
    s_gpu.texs[0].filter = 0;
    s_gpu.texs[0].cube   = 0;
    s_gpu.texs[0].valid  = 1;
    /* Fast path: swizzled 32-bit and palette-8 textures (the title's sprite uploads) were
     * decoded texel by texel through sample_tex(), which rebuilt the Morton masks for every
     * pixel -- ~1 ms per texture and the bulk of a fight frame's cost. Same maths, hoisted. */
    if ((tex->color == 0x06 || tex->color == 0x07 || tex->color == 0x0B) && tex->width && tex->height
        && tex->width <= 4096 && tex->height <= 4096 && (tex->color != 0x0B || tex->palette)) {
        const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
        const uint8_t *src = mem + tex->offset;
        const uint32_t *pal = tex->color == 0x0B ? (const uint32_t *)(mem + tex->palette) : NULL;
        uint32_t mx, my;
        static NV_TLS uint32_t xo[4096];
        xbox_swizzle_masks(tex->width, tex->height, &mx, &my);
        for (x = 0; x < tex->width; x++) xo[x] = swizzle_deposit(x, mx);
        for (y = 0; y < tex->height; y++) {
            uint32_t oy = swizzle_deposit(y, my);
            uint32_t *dst = &argb_out[(size_t)y * tex->width];
            if (pal) {
                for (x = 0; x < tex->width; x++) dst[x] = pal[src[xo[x] | oy]];
            } else if (tex->color == 0x06) {
                const uint32_t *s32 = (const uint32_t *)src;
                for (x = 0; x < tex->width; x++) dst[x] = s32[xo[x] | oy];
            } else {
                const uint32_t *s32 = (const uint32_t *)src;
                for (x = 0; x < tex->width; x++) dst[x] = s32[xo[x] | oy] | 0xFF000000u;
            }
        }
        { static int verify = -1;
          if (verify < 0) verify = getenv("RECOMP_DEC_VERIFY") ? 1 : 0;
          if (verify) {   /* debug: compare against the generic per-texel path */
              size_t bad = 0; uint32_t *tmp = (uint32_t *)malloc((size_t)tex->width * tex->height * 4);
              if (tmp) {
                  for (y = 0; y < tex->height; y++) for (x = 0; x < tex->width; x++) {
                      uint32_t a = 0;
                      sample_texture(x, y, &a);
                      if (a != argb_out[(size_t)y * tex->width + x]) { if (!bad) fprintf(stderr, "[DECV] mismatch fmt=%02X %ux%u at (%u,%u) fast=%08X slow=%08X\n", tex->color, tex->width, tex->height, x, y, argb_out[(size_t)y * tex->width + x], a); bad++; }
                  }
                  free(tmp);
              }
              if (bad) fprintf(stderr, "[DECV] %zu bad texels\n", bad);
          } }
        s_gpu.texs[0] = saved;
        return 1;
    }
    for (y = 0; y < tex->height && ok; y++)
        for (x = 0; x < tex->width; x++)
            if (!sample_texture(x, y, &argb_out[(size_t)y * tex->width + x])) {
                ok = 0;
                break;
            }
    s_gpu.texs[0] = saved;
    return ok;
}

/* Swizzled colour surfaces (SURFACE_FORMAT type 2, log2 width/height in bits
 * 16-19/24-27): render-to-texture. The pixel at (x, y) lives at the same
 * Morton offset a swizzled texture of that size uses, so the texture sampler
 * reads back what was drawn. Written row-major instead, every render target a
 * title samples later comes back scrambled -- SSX 3 draws six 128x128 rider
 * shadow maps per frame and projects them onto the snow. s_swz_* are set with
 * the surface, before the batch's pixels. RECOMP_SWIZZLED_RT=0 writes
 * row-major. */
static int s_swz_on;
static uint32_t s_swz_mx, s_swz_my;

static void surface_swizzle_update(void)
{
    static int enabled = -1;
    if (enabled < 0) {
        const char *e = getenv("RECOMP_SWIZZLED_RT");
        enabled = !(e && e[0] == '0');
    }
    s_swz_on = enabled && ((s_gpu.format >> 8) & 0xF) == 2;
    if (s_swz_on)
        xbox_swizzle_masks(1u << ((s_gpu.format >> 16) & 0xF),
                           1u << ((s_gpu.format >> 24) & 0xF),
                           &s_swz_mx, &s_swz_my);
}

/* CLEAR_RECT of the clear in progress, for the D3D11 back end (rbx_d3d11.c). */
uint32_t g_rbx_clear_rect_h, g_rbx_clear_rect_v;
int g_rbx_clear_rect_set;

static void clear_surface(uint32_t param)
{
    uint8_t *mem = (uint8_t *)xbox_GetMemoryOffset();
    uint32_t bpp = surface_bpp();
    uint32_t y, x;

    /* A back end clears colour and/or depth/stencil itself (flags are the
     * CLEAR_SURFACE bits: Z 0x1, stencil 0x2, colour 0xF0). */
    if (s_backend && s_backend->clear) {
        Nv2aSurface surf;
        Nv2aRenderState rs;

        if (!s_gpu.color_offset || !s_gpu.pitch || !s_gpu.clip_h || bpp == 0)
            return;
        current_surface(&surf);
        current_render_state(&rs);
        /* The back end's clear has no rectangle argument; hand it CLEAR_RECT
         * here (read by rbx_d3d11.c's clear, which used to wipe the whole
         * target whatever rectangle the title asked for). */
        g_rbx_clear_rect_h = s_gpu.clear_rect_h;
        g_rbx_clear_rect_v = s_gpu.clear_rect_v;
        g_rbx_clear_rect_set = s_gpu.clear_rect_set;
        /* Capture the original hardware rectangles at the renderer boundary. */
        static int trace_clear = -1;
        if (trace_clear < 0)
            trace_clear = getenv("RECOMP_RBX_CLEAR_TRACE") != NULL;
        if (trace_clear)
            fprintf(stderr, "[RBX CLEAR] va=%08X clip=%u,%u %ux%u rect=%08X,%08X rect_set=%d flags=%02X color=%08X\n",
                    surf.color_va, s_gpu.clip_x, s_gpu.clip_y, surf.width, surf.height,
                    s_gpu.clear_rect_h, s_gpu.clear_rect_v, s_gpu.clear_rect_set,
                    param, s_gpu.clear_color);
        extern void rbx_d3d11_clear_rect(uint32_t, unsigned, unsigned, unsigned,
                                             uint32_t, uint32_t, uint32_t);
        if (s_gpu.clear_rect_set == 3)
            rbx_d3d11_clear_rect(surf.color_va, surf.width, surf.height, surf.aa_sx,
                                 param, s_gpu.clear_rect_h, s_gpu.clear_rect_v);
        s_backend->clear(&surf, &rs, param, s_gpu.clear_color,
                         s_gpu.zstencil_clear);
        s_gpu.clears++;
        return;
    }
    if (!(param & NV097_CLEAR_COLOR_MASK))
        return;                            /* depth/stencil only */
    if (!s_gpu.color_offset || !s_gpu.pitch || !s_gpu.clip_h || bpp == 0)
        return;
    {
        uint32_t base = dma_resolve(s_gpu.color_offset);
        if (surface_write_refused(base,
                                  (s_gpu.clip_y + s_gpu.clip_h) * s_gpu.pitch,
                                  "clear"))
            return;
        s_gpu.color_base = base;
    }

    surface_swizzle_update();
    if (s_swz_on) {
        /* The clear covers CLEAR_RECT, not the surface clip: SSX 3 clears
         * its 128x128 shadow maps whole but draws them through a 124x124
         * clip, and samples them clamped -- the ring the clip leaves must be
         * the clear colour, or everything outside a shadow is dark. */
        uint8_t *base = mem + s_gpu.color_base;
        uint32_t sw = 1u << ((s_gpu.format >> 16) & 0xF);
        uint32_t sh = 1u << ((s_gpu.format >> 24) & 0xF);
        uint32_t x0 = s_gpu.clip_x, x1 = s_gpu.clip_x + s_gpu.clip_w;
        uint32_t y0 = s_gpu.clip_y, y1 = s_gpu.clip_y + s_gpu.clip_h;
        uint16_t v16 = (uint16_t)(((s_gpu.clear_color >> 8) & 0xF800)
                                | ((s_gpu.clear_color >> 5) & 0x07E0)
                                | ((s_gpu.clear_color >> 3) & 0x001F));
        if ((s_gpu.clear_rect_set & 3) == 3) {
            x0 = s_gpu.clear_rect_h & 0xFFFF; x1 = (s_gpu.clear_rect_h >> 16) + 1;
            y0 = s_gpu.clear_rect_v & 0xFFFF; y1 = (s_gpu.clear_rect_v >> 16) + 1;
        }
        if (x1 > sw) x1 = sw;
        if (y1 > sh) y1 = sh;
        for (y = y0; y < y1; y++)
            for (x = x0; x < x1; x++) {
                size_t i = swizzle_deposit(x, s_swz_mx) | swizzle_deposit(y, s_swz_my);
                if (bpp == 4)
                    ((uint32_t *)base)[i] = s_gpu.clear_color;
                else if (bpp == 2)
                    ((uint16_t *)base)[i] = v16;
            }
    } else
    for (y = 0; y < s_gpu.clip_h; y++) {
        uint8_t *row = mem + s_gpu.color_base
                     + (size_t)(s_gpu.clip_y + y) * s_gpu.pitch;
        if (bpp == 4) {
            uint32_t *p = (uint32_t *)row + s_gpu.clip_x;
            for (x = 0; x < s_gpu.clip_w; x++)
                p[x] = s_gpu.clear_color;
        } else if (bpp == 2) {
            /* The clear value is always given as A8R8G8B8; a 16-bit surface
             * takes the same colour reduced to 5:6:5. */
            uint16_t v = (uint16_t)(((s_gpu.clear_color >> 8) & 0xF800)
                                  | ((s_gpu.clear_color >> 5) & 0x07E0)
                                  | ((s_gpu.clear_color >> 3) & 0x001F));
            uint16_t *p = (uint16_t *)row + s_gpu.clip_x;
            for (x = 0; x < s_gpu.clip_w; x++)
                p[x] = v;
        }
    }
    s_gpu.clears++;
    /* Progress markers, interleaved with everything else in the log. The
     * summary says drawing stopped; only a marker next to the surrounding
     * activity says what the title was doing when it stopped. */
    if ((s_gpu.clears % 100) == 0)
        fprintf(stderr, "  [GPU] clear #%u\n", s_gpu.clears);
    /* Distinct clear colours actually used. "Cleared to black" and "the clear
     * never ran" look identical in the framebuffer, and only one of them is a
     * bug -- so record what was asked for, not just how often. */
    {
        static uint32_t seen[8];
        static int n;
        int i;
        for (i = 0; i < n; i++)
            if (seen[i] == s_gpu.clear_color) break;
        if (i == n && n < 8) {
            seen[n++] = s_gpu.clear_color;
            fprintf(stderr, "  [GPU] clear colour 0x%08X -> surface 0x%08X"
                            " (%ubpp)\n",
                    s_gpu.clear_color, s_gpu.color_offset, surface_bpp());
        }
    }

    /* Prove the pixel path end to end, independent of whether the title has
     * given us any geometry yet.
     *
     * "Nothing on screen" has three very different causes -- the surface
     * address or pitch is wrong, the rasteriser is broken, or the title's
     * vertex buffers are empty -- and they are indistinguishable from a black
     * window. RECOMP_RASTER_TEST draws one known triangle into the surface
     * just cleared, so a visible triangle rules out the first two and leaves
     * only the third. On Wreckless it is the third: attribute 0 decodes
     * correctly (float, size 2, stride 16) and the buffer it points at stays
     * zero.
     *
     * ponytail: bring-up aid, not a feature. It costs one branch per clear. */
    static int raster_test = -1;
    if (raster_test < 0)
        raster_test = getenv("RECOMP_RASTER_TEST") != NULL;
    if (raster_test) {
        static int announced;
        /* Every clear, not once: the title clears each frame and double-buffers,
         * so a triangle drawn a single time is erased before anyone sees it. */
        if (s_gpu.clip_w && s_gpu.clip_h) {
            float a[2], b[2], c[2];
            a[0] = s_gpu.clip_w * 0.5f; a[1] = s_gpu.clip_h * 0.15f;
            b[0] = s_gpu.clip_w * 0.85f; b[1] = s_gpu.clip_h * 0.85f;
            c[0] = s_gpu.clip_w * 0.15f; c[1] = s_gpu.clip_h * 0.85f;
            raster_triangle(a, b, c, 0xFFFF00FFu, NULL);  /* magenta: never a clear colour */
            if (announced++ == 0)
            fprintf(stderr, "  [GPU] raster self-test: triangle (%.0f,%.0f)"
                            " (%.0f,%.0f) (%.0f,%.0f) into 0x%08X %ubpp\n",
                    a[0], a[1], b[0], b[1], c[0], c[1],
                    s_gpu.color_offset, surface_bpp());
        }
    }

    /* Show the surface actually being drawn into. A title that double-buffers
     * renders into the back buffer, so following AvSetDisplayMode's address
     * would show the one nothing is writing. */
    /* The window has to read where the pixels actually are, which is the
     * resolved address rather than the DMA-object offset. */
    /* Only until the title flips. Following the draw surface on every scan
     * shows the buffer being written right now, half a frame at a time; past
     * the first flip the window is repointed at the finished one instead. */
    if (s_gpu.flips == 0) {
        uint32_t aa_sy = s_gpu.aa_sy > 1.5f ? 2u : 1u;
        if (aa_sy < 2u && s_gpu.clip_w == 640u && s_gpu.clip_h >= 960u)
            aa_sy = 2u;
        xbox_FramebufferWindowSet(dma_resolve(s_gpu.color_offset), s_gpu.pitch);
        xbox_FramebufferWindowSetAA(aa_sy);
    }

    /* And open the window, rather than waiting for AvSetDisplayMode to do it.
     *
     * That was the only caller, so a title which draws before setting a display
     * mode -- or never sets one at all -- got no window however much it
     * rendered. The Xbox Dashboard clears a 1280x960 surface at 0x00088000 on
     * its first frame and had not called AvSetDisplayMode by then, so
     * RECOMP_FB_WINDOW=1 was set, the executor knew the address and the pitch,
     * and nothing appeared.
     *
     * Here is the better trigger anyway: this runs when a surface address is
     * known to be real, because a clear just used it. Idempotent and gated on
     * RECOMP_FB_WINDOW, so the cost is one interlocked compare per clear. */
    xbox_FramebufferWindowStart();
}


/* ── Rasteriser ──────────────────────────────────────────────────────────
 *
 * Fills triangles straight into the guest framebuffer, the same memory
 * clear_surface() writes and the framebuffer window already shows. That is the
 * whole reason it is done on the CPU rather than through D3D: nothing new has
 * to be plumbed for the result to be visible.
 *
 * ponytail: flat-shaded, no depth buffer, no texturing, no perspective
 * correction, and only batches whose attribute 0 is already in screen space.
 * A title running a vertex program hands over object-space positions that mean
 * nothing without executing the program, so those batches are counted and
 * skipped rather than drawn somewhere wrong. Upgrade path is the D3D11
 * translator in src/nv2a/nv2a_pgraph_d3d11.c once vertex programs are
 * translated; this exists to get the first geometry on screen for every title,
 * which in practice is UI, HUD and 2D overlays -- all pre-transformed.
 */

/* One texel, in the title's own format.
 *
 * The codes are the NV097 colour field, which is the Xbox D3DFMT_ enum --
 * src/d3d/d3d8_xbox.h is the table, and it is the table to check against
 * rather than recollection: 0x1E is LIN_X8R8G8B8 and not, as this first read
 * it, a byte-reversed BGRA. Getting that one wrong turned an opaque black
 * render target into a screen of pure blue, which is the kind of wrong that
 * looks like content.
 *
 * Only the linear (LIN_) formats are read. A swizzled texture stores its
 * texels in Morton order rather than in rows, so reading one as if it had a
 * pitch does not give a slightly wrong colour, it gives a different image --
 * and inventing that image is exactly what this is not for. An unsupported
 * format samples nothing and the caller keeps the vertex colour, which is
 * visibly wrong rather than quietly wrong.
 *
 * ponytail: nearest texel, no filtering, whatever SET_TEXTURE_FILTER asked
 * for. Bilinear when a title's output actually depends on it.
 */
/* Off the edge of the texture, the way the title asked for.
 *
 * Refusing to sample instead is not neutral: it hands the caller back the
 * vertex colour, so a pass whose coordinates reach the last texel by half a
 * texel gets a bright line down the edge of the screen. The dashboard's
 * resolve does exactly that -- its last column and last row, 1119 pixels of
 * white on a black frame, from a rounding step at the boundary.
 */
static uint32_t wrap_coord(uint32_t c, uint32_t size, uint32_t mode)
{
    /* Coordinates are signed: a bilinear tap one texel left of or above
     * texel 0 is -1, which must wrap to the far edge or clamp to 0 -- as an
     * unsigned value it clamped to the far edge instead. */
    int32_t sc = (int32_t)c, n = (int32_t)size;
    if (!size)
        return 0;
    if (mode == 1)                         /* wrap */
        return (uint32_t)(((sc % n) + n) % n);
    if (mode == 2) {                       /* mirror */
        int32_t m = ((sc % (2 * n)) + 2 * n) % (2 * n);
        return (uint32_t)(m < n ? m : 2 * n - 1 - m);
    }
    return sc < 0 ? 0u : (sc >= n ? size - 1 : c);   /* clamp, and the rest */
}

static uint32_t expand(uint32_t v, uint32_t bits)
{
    return d3d8_expand_channel(v, bits);
}

/* The linear format that decodes the same texels as a swizzled one.
 *
 * Swizzling changes where a texel lives, not what it says: A8R8G8B8 (0x06) and
 * LIN_A8R8G8B8 (0x12) are the same four bytes in the same order. So the whole
 * difference is the address calculation, and one of those lets every format
 * below serve both. Pairs read off the table in d3d8_xbox.h rather than
 * recalled -- the comment above this one is about getting exactly that wrong. */
static uint32_t linear_twin(uint32_t fmt)
{
    switch (fmt) {
    case 0x00: return 0x13;                /* L8        -> LIN_L8        */
    case 0x02: return 0x10;                /* A1R5G5B5  -> LIN_A1R5G5B5  */
    case 0x03: return 0x1C;                /* X1R5G5B5  -> LIN_X1R5G5B5  */
    case 0x04: return 0x1D;                /* A4R4G4B4  -> LIN_A4R4G4B4  */
    case 0x05: return 0x11;                /* R5G6B5    -> LIN_R5G6B5    */
    case 0x06: return 0x12;                /* A8R8G8B8  -> LIN_A8R8G8B8  */
    case 0x07: return 0x1E;                /* X8R8G8B8  -> LIN_X8R8G8B8  */
    case 0x19: return 0x1F;                /* A8        -> LIN_A8        */
    default:   return fmt;                 /* already linear, or unhandled */
    }
}

/* Sample texel (u, v) of stage texture `t`; `face` offsets a cube map. */
static int sample_tex(const Texture *t, uint32_t face_offset,
                      uint32_t u, uint32_t v, uint32_t *argb)
{
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
    const uint8_t *p;
    uint32_t fmt, base = t->offset + face_offset;

    if (!t->valid)
        return 0;
    /* BORDER (4) with FORMAT's border source set to "colour": texels outside
     * the image read SET_TEXTURE_BORDER_COLOR. Projected shadow maps rely on
     * it -- clamped instead, everything outside the projection takes the
     * edge texel and the shadow's frustum is drawn as a dark wedge.
     * (Coordinates are signed in unsigned: a negative one is >= the size.) */
    if (t->border_color
        && ((t->addr_u == 4 && u >= t->width) || (t->addr_v == 4 && v >= t->height))) {
        *argb = t->border;
        return 1;
    }
    u = wrap_coord(u, t->width,  t->addr_u);
    v = wrap_coord(v, t->height, t->addr_v);

    fmt = t->color;
    if (d3d8_format_dxt_block_bytes(fmt)) {
        /* Decoded blocks, direct-mapped by address. Every texel of a DXT
         * texture decodes its whole 4x4 block, and bilinear reads four
         * neighbours that nearly always share one -- this was a tenth of
         * the executor's time. Keyed on the block's bytes as well as its
         * address, so a texture rewritten in place (a render target reused,
         * a streamed mip) never returns stale texels.
         * ponytail: single-threaded executor, so a plain static table. */
        static NV_TLS struct { uintptr_t key; uint32_t fmt; uint64_t raw[2];
                        uint32_t px[16]; } cache[4096];
        uint32_t bb = d3d8_format_dxt_block_bytes(fmt);
        uint32_t bx = u >> 2, by = v >> 2, bw = (t->width + 3) >> 2;
        const uint8_t *blk = mem + base + ((size_t)by * bw + bx) * bb;
        uintptr_t key = (uintptr_t)blk;
        size_t slot = ((key / bb) ^ (key >> 16)) & 4095;
        uint64_t raw[2] = {0, 0};
        memcpy(raw, blk, bb);
        if (cache[slot].key != key || cache[slot].fmt != fmt
            || cache[slot].raw[0] != raw[0] || cache[slot].raw[1] != raw[1]) {
            uint32_t i;
            for (i = 0; i < 16; i++)
                d3d8_dxt_decode_texel(mem + base, fmt, bx * 4 + (i & 3),
                                      by * 4 + (i >> 2), t->width,
                                      &cache[slot].px[i]);
            cache[slot].key = key;
            cache[slot].fmt = fmt;
            cache[slot].raw[0] = raw[0];
            cache[slot].raw[1] = raw[1];
        }
        *argb = cache[slot].px[(v & 3) * 4 + (u & 3)];
        return 1;
    }
    if (d3d8_format_is_swizzled(fmt)) {
        /* Morton order: a texel's index is interleaved from x and y instead of
         * v*pitch + u, so index from the base of the image. The switch below
         * casts to each format's own width, which makes that index a texel
         * index for every one of them. */
        fmt = linear_twin(fmt);
        p = mem + base;
        u = swizzle_offset(u, v, t->width, t->height);
    } else {
        p = mem + base + (size_t)v * t->pitch;
    }

    switch (fmt) {

    /* 32-bit, alpha-red-green-blue in the dword. */
    case 0x12:                                      /* LIN_A8R8G8B8 */
        *argb = ((const uint32_t *)p)[u];
        return 1;
    case 0x1E:                                      /* LIN_X8R8G8B8 */
        *argb = ((const uint32_t *)p)[u] | 0xFF000000u;
        return 1;

    /* 8-bit palette index, swizzled. Burnout 3 draws its logo and frontend
     * header art this way; without a case every such quad came out as the
     * unsupported-format fill, a white box where the logo should be. */
    case 0x0B:                                      /* SZ_I8_A8R8G8B8 */
        if (!t->palette)
            return 0;
        *argb = ((const uint32_t *)(mem + t->palette))[p[u]];
        return 1;

    /* 32-bit, other channel orders. The name gives the byte order from the
     * top of the dword down, so each is a permutation of the same four. */
    case 0x3F: {                                    /* LIN_A8B8G8R8 */
        uint32_t t = ((const uint32_t *)p)[u];
        *argb = (t & 0xFF00FF00u) | ((t & 0xFF) << 16) | ((t >> 16) & 0xFF);
        return 1;
    }
    case 0x40: {                                    /* LIN_B8G8R8A8 */
        uint32_t t = ((const uint32_t *)p)[u];
        *argb = ((t & 0xFFu) << 24)                 /* A, from the bottom */
              | (((t >>  8) & 0xFFu) << 16)         /* R */
              | (((t >> 16) & 0xFFu) <<  8)         /* G */
              |  ((t >> 24) & 0xFFu);               /* B, from the top */
        return 1;
    }
    case 0x41: {                                    /* LIN_R8G8B8A8 */
        uint32_t t = ((const uint32_t *)p)[u];
        *argb = ((t & 0xFFu) << 24) | (t >> 8);
        return 1;
    }

    /* 16-bit. */
    case 0x10: {                                    /* LIN_A1R5G5B5 */
        uint32_t t = ((const uint16_t *)p)[u];
        *argb = ((t & 0x8000u) ? 0xFF000000u : 0u)
              | (expand((t >> 10) & 0x1F, 5) << 16)
              | (expand((t >>  5) & 0x1F, 5) <<  8)
              |  expand( t        & 0x1F, 5);
        return 1;
    }
    case 0x1C: {                                    /* LIN_X1R5G5B5 */
        uint32_t t = ((const uint16_t *)p)[u];
        *argb = 0xFF000000u
              | (expand((t >> 10) & 0x1F, 5) << 16)
              | (expand((t >>  5) & 0x1F, 5) <<  8)
              |  expand( t        & 0x1F, 5);
        return 1;
    }
    case 0x11: {                                    /* LIN_R5G6B5 */
        uint32_t t = ((const uint16_t *)p)[u];
        *argb = 0xFF000000u
              | (expand((t >> 11) & 0x1F, 5) << 16)
              | (expand((t >>  5) & 0x3F, 6) <<  8)
              |  expand( t        & 0x1F, 5);
        return 1;
    }
    case 0x1D: {                                    /* LIN_A4R4G4B4 */
        uint32_t t = ((const uint16_t *)p)[u];
        *argb = (expand((t >> 12) & 0x0F, 4) << 24)
              | (expand((t >>  8) & 0x0F, 4) << 16)
              | (expand((t >>  4) & 0x0F, 4) <<  8)
              |  expand( t        & 0x0F, 4);
        return 1;
    }

    /* 8-bit. */
    case 0x13: {                                    /* LIN_L8 */
        uint32_t t = p[u];
        *argb = 0xFF000000u | (t << 16) | (t << 8) | t;
        return 1;
    }
    case 0x1F:                                      /* LIN_A8 */
        *argb = ((uint32_t)p[u] << 24) | 0x00FFFFFFu;
        return 1;

    /* 4:2:2 packed YUV, two texels per four bytes.
     *
     * This is how a title hands over a decoded video frame, and without it
     * the frame falls through to `default` -- which returns 0, so the caller
     * paints the quad's vertex colour and the movie is a flat rectangle.
     *
     * The chroma pair is shared between an even texel and the one after it,
     * so the group is found by masking the bottom bit of the index. BT.601,
     * the same coefficients the D3D8 upload path converts with, so the two
     * paths agree rather than each having its own idea of the colour. */
    case 0x24:                                      /* LC_CR8YB8CB8YA8, YUY2 */
    case 0x25: {                                    /* LC_YB8CR8YA8CB8, UYVY */
        uint32_t yoff = (fmt == 0x24) ? 0u : 1u;
        const uint8_t *g = p + (size_t)(u & ~1u) * 2;
        int c  = (int)g[(u & 1u) ? 2 + yoff : yoff] - 16;
        int cu = (int)g[1 - yoff] - 128;
        int cv = (int)g[3 - yoff] - 128;
        int r = (298 * c + 409 * cv + 128) >> 8;
        int gg = (298 * c - 100 * cu - 208 * cv + 128) >> 8;
        int b = (298 * c + 516 * cu + 128) >> 8;
        if (r < 0) r = 0;
        if (r > 255) r = 255;
        if (gg < 0) gg = 0;
        if (gg > 255) gg = 255;
        if (b < 0) b = 0;
        if (b > 255) b = 255;
        *argb = 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)gg << 8)
              | (uint32_t)b;
        return 1;
    }

    default:
        return 0;
    }
}

static int sample_texture(uint32_t u, uint32_t v, uint32_t *argb)
{
    return sample_tex(&s_gpu.texs[0], 0, u, v, argb);
}

/* Write a bound texture out as a BMP, through the sampler rather than around it.
 *
 * "Which texture is this" is not answerable from an address and a format, and
 * it is the question behind most of the ones that matter -- is that a font
 * page or an icon atlas, did the swizzle decode, is the alpha inverted. Going
 * through sample_texture means the file shows exactly what the rasteriser
 * sees, so a decode bug appears here rather than only as a wrong-looking
 * triangle.
 *
 * ponytail: RGB only, alpha dropped. A glyph page is alpha and would come out
 * black, so alpha is composited onto mid-grey to stay legible; that is a
 * viewing choice, not a decode. One file per distinct texture, first use only.
 */
static void dump_texture_bmp(uint32_t seq)
{
    const char *prefix = getenv("RECOMP_TEX_DUMP");
    uint32_t w = s_gpu.texs[0].width, h = s_gpu.texs[0].height, x, y;
    uint32_t row_bytes, pad, filesz;
    uint8_t hdr[54];
    char path[512];
    FILE *f;

    if (!prefix || !w || !h || w > 4096 || h > 4096)
        return;
    row_bytes = w * 3;
    pad = (4 - (row_bytes & 3)) & 3;
    filesz = 54 + (row_bytes + pad) * h;

    snprintf(path, sizeof path, "%s%02u_%08X_fmt%02X.bmp",
             prefix, seq, s_gpu.texs[0].offset, s_gpu.texs[0].color);
    f = fopen(path, "wb");
    if (!f)
        return;
    memset(hdr, 0, sizeof hdr);
    hdr[0] = 'B'; hdr[1] = 'M';
    memcpy(hdr + 2, &filesz, 4);
    hdr[10] = 54; hdr[14] = 40;
    memcpy(hdr + 18, &w, 4);
    memcpy(hdr + 22, &h, 4);
    hdr[26] = 1; hdr[28] = 24;
    fwrite(hdr, 1, sizeof hdr, f);

    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            uint32_t argb = 0, a;
            uint8_t px[3];
            if (!sample_texture(x, h - 1 - y, &argb))
                argb = 0;
            a = (argb >> 24) & 0xFFu;
            /* over mid-grey, so an alpha-only page is visible either way */
            px[0] = (uint8_t)(((argb & 0xFFu) * a + 128u * (255u - a)) / 255u);
            px[1] = (uint8_t)((((argb >> 8) & 0xFFu) * a + 128u * (255u - a)) / 255u);
            px[2] = (uint8_t)((((argb >> 16) & 0xFFu) * a + 128u * (255u - a)) / 255u);
            fwrite(px, 1, 3, f);
        }
        if (pad) {
            static const uint8_t zero[3] = {0, 0, 0};
            fwrite(zero, 1, pad, f);
        }
    }
    fclose(f);
    fprintf(stderr, "  [TEXDUMP] %s (%ux%u fmt 0x%02X)\n",
            path, w, h, s_gpu.texs[0].color);
    fflush(stderr);
}

/* The surface, resolved once per batch.
 *
 * dma_resolve consults the contiguous arena's high-water mark and
 * surface_hits_image walks the image range; both were being done per pixel
 * -- dma_resolve twice -- which cost more than the rasterisation they
 * guarded. Neither answer can change inside a batch, because the colour
 * offset arrives as a method and a method cannot arrive mid-triangle.
 *
 * This is not a micro-optimisation for its own sake: the loader's video
 * paces on frames actually presented, so the rasteriser's throughput is the
 * playback rate. */
static uint8_t *s_surface;          /* host address of surface row 0 */

/* Element index of pixel (x, y) in the current colour surface, relative to
 * the `row` it sets: the row itself, or the surface base for a swizzled one. */
static inline size_t surface_index(int x, int y, uint8_t **row)
{
    if (s_swz_on) {
        *row = s_surface;
        return swizzle_deposit((uint32_t)x, s_swz_mx)
             | swizzle_deposit((uint32_t)y, s_swz_my);
    }
    *row = s_surface + (size_t)y * s_gpu.pitch;
    return (size_t)x;
}

static int surface_begin_batch(const uint8_t *mem)
{
    uint32_t base = dma_resolve(s_gpu.color_offset);

    surface_swizzle_update();

    if (surface_hits_image(base, (s_gpu.clip_y + s_gpu.clip_h) * s_gpu.pitch))
        return 0;
    s_surface = (uint8_t *)mem + base;
    return 1;
}

static uint32_t pack_color(const float c[4]);

/* One GL blend factor, per channel (r g b a). */
static void blend_factor(uint32_t f, const float s[4], const float d[4],
                         float out[4])
{
    int k;
    for (k = 0; k < 4; k++) {
        float c = (float)((s_gpu.blend_color >> (k == 3 ? 24 : 16 - 8 * k))
                          & 0xFF) / 255.0f;
        switch (f) {
        case 0x0000: out[k] = 0.0f;            break;   /* ZERO                */
        case 0x0001: out[k] = 1.0f;            break;   /* ONE                 */
        case 0x0300: out[k] = s[k];            break;   /* SRC_COLOR           */
        case 0x0301: out[k] = 1.0f - s[k];     break;
        case 0x0302: out[k] = s[3];            break;   /* SRC_ALPHA           */
        case 0x0303: out[k] = 1.0f - s[3];     break;
        case 0x0304: out[k] = d[3];            break;   /* DST_ALPHA           */
        case 0x0305: out[k] = 1.0f - d[3];     break;
        case 0x0306: out[k] = d[k];            break;   /* DST_COLOR           */
        case 0x0307: out[k] = 1.0f - d[k];     break;
        case 0x0308: out[k] = k == 3 ? 1.0f : fminf(s[3], 1.0f - d[3]); break;
        case 0x8001: out[k] = c;               break;   /* CONSTANT_COLOR      */
        case 0x8002: out[k] = 1.0f - c;        break;
        case 0x8003: out[k] = (float)(s_gpu.blend_color >> 24) / 255.0f; break;
        case 0x8004: out[k] = 1.0f - (float)(s_gpu.blend_color >> 24) / 255.0f; break;
        default:     out[k] = f ? 1.0f : 0.0f; break;
        }
    }
}

static void put_pixel(uint8_t *mem, uint32_t bpp, int x, int y, uint32_t argb)
{
    uint8_t *row;
    size_t xi;

    (void)mem;
    if (x < (int)s_gpu.clip_x || x >= (int)(s_gpu.clip_x + s_gpu.clip_w))
        return;
    if (y < (int)s_gpu.clip_y || y >= (int)(s_gpu.clip_y + s_gpu.clip_h))
        return;
    /* A swizzled target is 1<<log on a side, whatever the clip says: past
     * it the swizzle index wraps onto other texels (xemu bounds it too). */
    if (s_swz_on && ((uint32_t)x >> ((s_gpu.format >> 16) & 0xF)
                     || (uint32_t)y >> ((s_gpu.format >> 24) & 0xF)))
        return;
    s_gpu.pixels++;
    if ((argb & 0x00FFFFFFu) > (s_gpu.pixel_max & 0x00FFFFFFu))
        s_gpu.pixel_max = argb;
    xi = surface_index(x, y, &row);

    /* Blending, with the GL factor set and equations the NV2A takes.
     *
     * Only SRC_ALPHA / ONE_MINUS_SRC_ALPHA used to be honoured and every other
     * pair was written opaque. Menus never noticed; a race does: Burnout 3
     * finishes its 3D frame with screen-space passes that modulate or add
     * over the scene, and written opaque they paint the whole view over. */
    if (s_gpu.blend_enable
        && !(s_gpu.blend_sfactor == 1 && s_gpu.blend_dfactor == 0)) {
        uint32_t dst = 0xFF000000u;
        float sc[4], dc[4], sf[4], df[4], out[4];
        int k;
        if (bpp == 4) {
            dst = ((const uint32_t *)row)[xi];
        } else if (bpp == 2) {
            uint32_t t = ((const uint16_t *)row)[xi];
            dst = 0xFF000000u | ((t & 0xF800u) << 8) | ((t & 0x07E0u) << 5)
                | ((t & 0x001Fu) << 3);
        }
        for (k = 0; k < 4; k++) {           /* r g b a, 0..1 */
            int sh = k == 3 ? 24 : 16 - 8 * k;
            sc[k] = (float)((argb >> sh) & 0xFF) / 255.0f;
            dc[k] = (float)((dst  >> sh) & 0xFF) / 255.0f;
        }
        blend_factor(s_gpu.blend_sfactor, sc, dc, sf);
        blend_factor(s_gpu.blend_dfactor, sc, dc, df);
        for (k = 0; k < 4; k++) {
            float a = sc[k] * sf[k], b = dc[k] * df[k];
            switch (s_gpu.blend_equation) {
            case 0x800A: out[k] = a - b; break;               /* SUBTRACT     */
            case 0x800B: out[k] = b - a; break;               /* REV_SUBTRACT */
            case 0x8007: out[k] = fminf(sc[k], dc[k]); break; /* MIN          */
            case 0x8008: out[k] = fmaxf(sc[k], dc[k]); break; /* MAX          */
            default:     out[k] = a + b; break;               /* ADD          */
            }
        }
        argb = pack_color(out);
    }

    if (s_gpu.color_mask != 0x01010101u) {
        uint32_t keep = 0, old;
        if (!(s_gpu.color_mask & 0x01000000u)) keep |= 0xFF000000u;
        if (!(s_gpu.color_mask & 0x00010000u)) keep |= 0x00FF0000u;
        if (!(s_gpu.color_mask & 0x00000100u)) keep |= 0x0000FF00u;
        if (!(s_gpu.color_mask & 0x00000001u)) keep |= 0x000000FFu;
        if (keep == 0xFFFFFFFFu)
            return;
        if (bpp == 4) {
            old = ((const uint32_t *)row)[xi];
        } else {
            uint32_t t = ((const uint16_t *)row)[xi];
            old = ((t & 0xF800u) << 8) | ((t & 0x07E0u) << 5) | ((t & 0x001Fu) << 3);
        }
        argb = (argb & ~keep) | (old & keep);
    }
    if (bpp == 4) {
        ((uint32_t *)row)[xi] = argb;
    } else if (bpp == 2) {
        ((uint16_t *)row)[xi] = (uint16_t)(((argb >> 8) & 0xF800)
                                        | ((argb >> 5) & 0x07E0)
                                        | ((argb >> 3) & 0x001F));
    }
}

/* Half-space fill. Barycentric edge functions rather than scanline slopes:
 * the same test decides both windings, so a title that emits clockwise
 * triangles does not silently render nothing. */
static void raster_triangle(const float a[2], const float b[2],
                            const float c[2], uint32_t argb,
                            const float uv[3][2])
{
    uint8_t *mem = (uint8_t *)xbox_GetMemoryOffset();
    uint32_t bpp = surface_bpp();
    float area;
    int minx, maxx, miny, maxy, x, y;
    int textured = uv && s_gpu.texs[0].valid;

    if (bpp != 4 && bpp != 2)
        return;

    area = (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0]);
    if (area == 0.0f)
        return;                            /* degenerate */

    /* Where this batch writes. The same check the per-pixel path made, made
     * once: a surface address landing on the title's own image is no safer
     * one pixel at a time than 4.9 MB at once. */
    if (!surface_begin_batch(mem))
        return;

    minx = (int)floorf(fminf(a[0], fminf(b[0], c[0])));
    maxx = (int)ceilf (fmaxf(a[0], fmaxf(b[0], c[0])));
    miny = (int)floorf(fminf(a[1], fminf(b[1], c[1])));
    maxy = (int)ceilf (fmaxf(a[1], fmaxf(b[1], c[1])));

    if (minx < (int)s_gpu.clip_x) minx = (int)s_gpu.clip_x;
    if (miny < (int)s_gpu.clip_y) miny = (int)s_gpu.clip_y;
    if (maxx > (int)(s_gpu.clip_x + s_gpu.clip_w)) maxx = (int)(s_gpu.clip_x + s_gpu.clip_w);
    if (maxy > (int)(s_gpu.clip_y + s_gpu.clip_h)) maxy = (int)(s_gpu.clip_y + s_gpu.clip_h);
    if (minx >= maxx || miny >= maxy) {
        s_gpu.tris_skipped_offscreen++;
        return;
    }

    for (y = miny; y < maxy; y++) {
        for (x = minx; x < maxx; x++) {
            float px = (float)x + 0.5f, py = (float)y + 0.5f;
            float w0 = (b[0] - a[0]) * (py - a[1]) - (b[1] - a[1]) * (px - a[0]);
            float w1 = (c[0] - b[0]) * (py - b[1]) - (c[1] - b[1]) * (px - b[0]);
            float w2 = (a[0] - c[0]) * (py - c[1]) - (a[1] - c[1]) * (px - c[0]);
            if (!((w0 >= 0 && w1 >= 0 && w2 >= 0)
               || (w0 <= 0 && w1 <= 0 && w2 <= 0)))
                continue;
            if (textured) {
                /* Barycentric, straight from the edge functions already
                 * computed: w1 is the area opposite a, w2 opposite b, w0
                 * opposite c, and the three sum to the whole triangle.
                 *
                 * No perspective divide. These are screen-space vertices with
                 * no w to divide by -- which is exactly the case a full-screen
                 * pass is, and the only case that reaches here. */
                uint32_t texel;
                float su = (w1 * uv[0][0] + w2 * uv[1][0] + w0 * uv[2][0]) / area;
                float sv = (w1 * uv[0][1] + w2 * uv[1][1] + w0 * uv[2][1]) / area;
                if (su < 0.0f) su = 0.0f;
                if (sv < 0.0f) sv = 0.0f;
                if (sample_texture((uint32_t)su, (uint32_t)sv, &texel)) {
                    put_pixel(mem, bpp, x, y, texel);
                    continue;
                }
            }
            put_pixel(mem, bpp, x, y, argb);
        }
    }
    s_gpu.tris_drawn++;
    note_drawn();
}

/* Attribute 3 is diffuse colour in every NV2A layout that sets one. Absent it,
 * white -- a visible wrong colour beats an invisible correct one during
 * bring-up. */
/* Which attribute carries the colour.
 *
 * Slot 3 is diffuse by convention and titles that follow it are read straight
 * from there. Half-Life 2 does not: its vertex is position, colour, texcoord
 * at stride 24, and the colour arrives in slot 5. So fall back to the format
 * rather than the slot number -- D3DCOLOR is the one attribute type that is
 * only ever a colour, which makes it a stronger signal than the convention. */
static const VertexAttr *color_attr(void)
{
    uint32_t a;

    if (s_gpu.attr[3].offset && s_gpu.attr[3].stride)
        return &s_gpu.attr[3];
    for (a = 0; a < NV_VERTEX_ATTRS; a++)
        if (s_gpu.attr[a].type == 0 && s_gpu.attr[a].size == 4
                && s_gpu.attr[a].offset && s_gpu.attr[a].stride)
            return &s_gpu.attr[a];
    return &s_gpu.attr[3];
}

/* Attribute 9 is texture coordinate 0 in the NV2A vertex layout, the same way
 * 0 is position and 3 is diffuse -- for a title that follows the convention.
 *
 * Half-Life 2 does not, in either place. Its menu and HUD vertex is position,
 * colour, texcoord at stride 24, with the colour in slot 5 and the texcoords
 * in slot 7, so reading slot 9 found nothing and every batch drew untextured.
 * That is invisible rather than wrong-looking: the menu paints a full-screen
 * quad and then draws its text over it, and with no sampling both come out
 * white, so the screen is blank white and nothing suggests the text was ever
 * drawn.
 *
 * Falling back to the format works because the three attributes of such a
 * vertex are distinguishable: position is float3, colour is D3DCOLOR, and a
 * float2 is a texture coordinate and nothing else.
 *
 * ponytail: takes the first float2 it finds, so a title with two texcoord sets
 * gets stage 0's -- which is what this single-texture rasteriser samples
 * anyway. Multi-texture wants the D3D11 translator, not another heuristic. */
static const VertexAttr *texcoord_attr(void)
{
    uint32_t a;

    if (s_gpu.attr[9].offset && s_gpu.attr[9].stride)
        return &s_gpu.attr[9];
    for (a = 0; a < NV_VERTEX_ATTRS; a++)
        if (s_gpu.attr[a].type == 2 && s_gpu.attr[a].size == 2
                && s_gpu.attr[a].offset && s_gpu.attr[a].stride)
            return &s_gpu.attr[a];
    return &s_gpu.attr[9];
}

/* Texel coordinates, whichever convention the title used.
 *
 * The two are not interchangeable and the format decides which is in force: a
 * swizzled texture is addressed in [0,1], a linear one in texels. Both are
 * scaled to texels here so that everything downstream -- the barycentric
 * interpolation and the sampler -- works in one unit.
 *
 * This mattered the moment swizzled formats became samplable. Normalised
 * coordinates truncated to a texel index land on texel 0 for any coordinate
 * below 1.0, so a whole quad sampled a single texel and came out flat: the
 * background painted one near-black colour, which looks like a texture that
 * decoded wrong rather than one that was never indexed. */
static int fetch_texcoord(uint32_t index, float out[2])
{
    float t[4];

    if (!fetch_attr(texcoord_attr(), index, t))
        return 0;
    out[0] = t[0];
    out[1] = t[1];
    if (tex_size_from_format(s_gpu.texs[0].color)) {
        out[0] *= (float)s_gpu.texs[0].width;
        out[1] *= (float)s_gpu.texs[0].height;
    }
    return 1;
}

static uint32_t vertex_color(uint32_t index)
{
    float c[4];

    if (!fetch_attr(color_attr(), index, c))
        return 0xFFFFFFFFu;
    return ((uint32_t)(c[3] * 255.0f) << 24)
         | ((uint32_t)(c[0] * 255.0f) << 16)
         | ((uint32_t)(c[1] * 255.0f) <<  8)
         |  (uint32_t)(c[2] * 255.0f);
}

/* ---- Fixed-function transform and lighting --------------------------------
 *
 * With TRANSFORM_EXECUTION_MODE on "fixed" (not "program") and a composite
 * matrix uploaded, attribute 0 is an object-space position and the hardware
 * computes
 *     clip[i] = sum_j M[4i+j] * pos[j]
 * with the 16 floats in the order the methods deliver them (translation in
 * M[3], M[7], M[11]; X-Men Legends' menu matrix ends in the row 0 0 0 1), then
 *     screen = clip.xyz / clip.w + viewport offset (c[59]).
 * The composite matrix carries the viewport scale for the *logical* surface;
 * with anti-aliasing the result is scaled up to the real surface.
 *
 * The result is handed on in the same shape a vertex program produces
 * (Nv2aVshOutput, position already in screen space, w the clip w), so a
 * fixed-function batch shares the program path's near-plane clipping,
 * texturing, combiners and depth buffer.
 *
 * ponytail: no texture-coordinate generation or texture matrices (texture
 * coordinates are taken from the attribute streams as they are), no fog
 * coordinate, no specular, no point sprites. */

/* Is this batch transformed by the fixed-function unit? Inline-array
 * (DrawPrimitiveUP-style) batches are transformed too: fetch_attr reads their
 * payload like any vertex array, and the hardware does not care how vertices
 * arrived. Leaving them to the screen-space path drew world-space meshes
 * (Robotech mission geometry at hundreds of thousands of units; RedCard's
 * stadium banners at +-1750) as raw pixels — fully off-surface — so the
 * present stayed black while triangle counts climbed. Pre-transformed
 * (XYZRHW) vertices do not come this way: XDK D3D draws them with a
 * pass-through vertex program. RECOMP_FFP_INLINE=0 restores the old
 * screen-space treatment. */
static int batch_is_ffp(void)
{
    static int ffp_inline = -1;

    if (ffp_inline < 0) {
        const char *e = getenv("RECOMP_FFP_INLINE");
        ffp_inline = !(e && e[0] == '0');
    }
    return s_gpu.composite_set && (s_gpu.xf_mode & 3) != 2
        && (ffp_inline || !s_gpu.inline_active);
}

/* Fixed-function vertex lighting (SET_LIGHTING_ENABLE).
 *
 * With lighting on, the diffuse colour the combiners see is computed per
 * vertex from the normal and up to eight lights; the vertex's own colour
 * stream is ignored unless SET_COLOR_MATERIAL routes it in. Using the colour
 * stream anyway is what drew every lit mesh -- characters, streets -- black.
 *
 * XDK D3D pre-multiplies material into the light colours and folds
 * emission + material ambient * global ambient into SCENE_AMBIENT_COLOR, so
 * the hardware sum is simply
 *   colour = scene_ambient + sum_i atten_i * (ambient_i + diffuse_i * max(0, N.L_i))
 * with alpha from SET_MATERIAL_ALPHA. Everything is in eye space.
 *
 * ponytail: normals go through the model-view matrix itself rather than the
 * inverse transpose (exact for rotations and uniform scale), no specular, no
 * spot cones, no back-face colours, no skinning. Add them when a mesh looks
 * wrong rather than dark. */
#define NV_LIGHTS 8

/* RECOMP_FFP_FLIP_NORMALS: a compatibility option, off by default. See
 * lit_color(). */
static int ffp_flip_normals(void)
{
    static int on = -1;

    if (on < 0)
        on = getenv("RECOMP_FFP_FLIP_NORMALS") != NULL;
    return on;
}

static int lit_color(uint32_t index, float out[4])
{
    const float *mv = (const float *)&s_reg[0x0480 / 4];
    float pos[4], nrm[4], vc[4], e[3], n[3], len;
    uint32_t mask = s_reg[0x03BC / 4], colmat = s_reg[0x0298 / 4];
    int i, have_vc;

    if (!s_reg[0x0314 / 4] || !s_gpu.attr[2].size)
        return 0;
    if (!fetch_attr(&s_gpu.attr[0], index, pos)
     || !fetch_attr(&s_gpu.attr[2], index, nrm))
        return 0;
    have_vc = fetch_attr(color_attr(), index, vc);

    for (i = 0; i < 3; i++) {
        e[i] = mv[4 * i] * pos[0] + mv[4 * i + 1] * pos[1]
             + mv[4 * i + 2] * pos[2] + mv[4 * i + 3];
        n[i] = mv[4 * i] * nrm[0] + mv[4 * i + 1] * nrm[1]
             + mv[4 * i + 2] * nrm[2];
    }
    len = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
    if (len > 1e-8f) { n[0] /= len; n[1] /= len; n[2] /= len; }
    /* Optional (RECOMP_FFP_FLIP_NORMALS): light the side the viewer sees.
     * Some meshes carry normals that face away from the camera (X-Men
     * Legends' streets and sidewalks: authentic file data, drawn with culling
     * off), and lighting them as stored leaves only the light's ambient term
     * -- black ground. Flipping a normal that points away from the eye is
     * two-sided lighting with the back material equal to the front one.
     * This is NOT hardware behaviour: it applies even when the title left
     * two-sided lighting off (NV097_SET_TWO_SIDE_LIGHT_EN, method 0x17C4,
     * which X-Men Legends writes as 0), and how the hardware makes such a mesh
     * look right is not established. So it is a compatibility option a game
     * project turns on for meshes it has seen need it, and off otherwise, so
     * it cannot change how any other title is lit.
     * ponytail: per vertex, not per face; exact for flat ground, a vertex on
     * a silhouette may pick the other side. */
    if (ffp_flip_normals() && n[0] * e[0] + n[1] * e[1] + n[2] * e[2] > 0.0f) {
        n[0] = -n[0]; n[1] = -n[1]; n[2] = -n[2];
    }

    out[0] = reg_f(0x0A10); out[1] = reg_f(0x0A14); out[2] = reg_f(0x0A18);
    for (i = 0; i < NV_LIGHTS; i++) {
        uint32_t kind = (mask >> (2 * i)) & 3, b = 0x1000 + (uint32_t)i * 0x80;
        float l[3], att = 1.0f, ndl, amb[3], dif[3];
        int k;

        if (!kind)
            continue;
        if (kind == 1) {                                    /* infinite */
            l[0] = reg_f(b + 0x34); l[1] = reg_f(b + 0x38); l[2] = reg_f(b + 0x3C);
        } else {                                            /* local / spot */
            float d;
            l[0] = reg_f(b + 0x5C) - e[0];
            l[1] = reg_f(b + 0x60) - e[1];
            l[2] = reg_f(b + 0x64) - e[2];
            d = sqrtf(l[0] * l[0] + l[1] * l[1] + l[2] * l[2]);
            if (d > reg_f(b + 0x24) && reg_f(b + 0x24) > 0.0f)
                continue;                                   /* out of range */
            if (d > 1e-8f) { l[0] /= d; l[1] /= d; l[2] /= d; }
            att = reg_f(b + 0x68) + reg_f(b + 0x6C) * d + reg_f(b + 0x70) * d * d;
            att = att > 1e-8f ? 1.0f / att : 1.0f;
        }
        ndl = n[0] * l[0] + n[1] * l[1] + n[2] * l[2];
        if (ndl < 0.0f) ndl = 0.0f;
        for (k = 0; k < 3; k++) {
            amb[k] = reg_f(b + 0x00 + 4 * k);
            dif[k] = reg_f(b + 0x0C + 4 * k);
            /* COLOR_MATERIAL: 1 in the ambient (bits 2-3) or diffuse (bits
             * 4-5) field takes that material colour from the vertex. */
            if (have_vc && ((colmat >> 2) & 3) == 1) amb[k] *= vc[k];
            if (have_vc && ((colmat >> 4) & 3) == 1) dif[k] *= vc[k];
            out[k] += att * (amb[k] + dif[k] * ndl);
        }
    }
    out[3] = (have_vc && ((colmat >> 4) & 3) == 1) ? vc[3] : reg_f(0x03B4);
    for (i = 0; i < 4; i++)
        out[i] = out[i] < 0.0f ? 0.0f : out[i] > 1.0f ? 1.0f : out[i];
    return 1;
}

/* One fixed-function vertex, in the shape a vertex program produces. */
static int ffp_vertex(uint32_t index, Nv2aVshOutput *out)
{
    const float *m = s_gpu.composite, *off = nv2a_vsh_constant(59);
    float in[4], clip[4], c[4], w;
    uint32_t st;
    int i;

    if (!fetch_attr(&s_gpu.attr[0], index, in))
        return 0;
    for (i = 0; i < 4; i++)
        clip[i] = m[4 * i] * in[0] + m[4 * i + 1] * in[1]
                + m[4 * i + 2] * in[2] + m[4 * i + 3] * in[3];
    /* A vertex behind the camera (w < 0) is kept: the clipper cuts it at the
     * near plane, and rejecting it dropped every batch that reached behind
     * the eye -- X-Men Legends' street and sidewalk meshes, which extend past
     * a top-down camera. w = 0 (the eye plane) has no screen image, but
     * returning 0 here drops the whole batch: RedCard's sky dome is centred
     * on the camera, so horizon verts on the eye plane made the sky vanish
     * on alternate frames. Keep a tiny w of the same sign; raster_xf_clipped
     * cuts it at the near plane. */
    w = clip[3];
    if (!(fabsf(w) > 1e-6f)) {
        if (w != w)
            return 0;                      /* NaN: no position at all */
        w = signbit(w) ? -1e-6f : 1e-6f;
    }

    memset(out, 0, sizeof *out);
    out->pos[0] = (clip[0] / w + off[0]) * s_gpu.aa_sx;
    out->pos[1] = (clip[1] / w + off[1]) * s_gpu.aa_sy;
    out->pos[2] = clip[2] / w + off[2];
    out->pos[3] = w;

    if (!lit_color(index, c) && !fetch_attr(color_attr(), index, c))
        c[0] = c[1] = c[2] = c[3] = 1.0f;
    for (i = 0; i < 4; i++)
        out->d0[i] = c[i] < 0.0f ? 0.0f : c[i] > 1.0f ? 1.0f : c[i];

    fetch_attr(texcoord_attr(), index, out->tex[0]);
    for (st = 1; st < 4; st++)
        fetch_attr(&s_gpu.attr[9 + st], index, out->tex[st]);
    return 1;
}

/* An untransformed batch drawn as if it were screen space smears a few pixels
 * into the corner, so the batch has to be classified before it is rasterised.
 *
 * This used to demand that every vertex land inside the surface, which is a
 * different question and the wrong one: geometry that extends past the
 * viewport is ordinary, and clipping it is raster_triangle's job (it clamps
 * its span to the clip rect). The dashboard is exactly the case that exposed
 * it -- a full-screen pass drawn as one oversized triangle, vertices at
 * (-0.5,-0.5), (2*w,-0.5), (-0.5,2*h), all correct and all rejected.
 *
 * What actually separates the two is scale. Object-space positions are model
 * units, a handful either side of the origin; screen-space ones are measured
 * in pixels of a surface hundreds of pixels wide. So: the batch has to be able
 * to touch the surface at all, and it has to be bigger than object space.
 *
 * ponytail: a genuinely tiny screen-space sprite reads as object space and is
 * skipped. It is counted as skipped rather than silently dropped, and the
 * unambiguous answer needs the vertex-program state, which is not tracked yet.
 */
#define OBJECT_SPACE_SPAN 8.0f

/* The surface's anti-aliasing scale, never below 1, for positions that arrive
 * in logical pixels (pre-transformed vertices) and must land in real ones. */
static float aa_scale_x(void) { return s_gpu.aa_sx > 1.0f ? s_gpu.aa_sx : 1.0f; }
static float aa_scale_y(void) { return s_gpu.aa_sy > 1.0f ? s_gpu.aa_sy : 1.0f; }

static int batch_is_screen_space(void)
{
    float p[4], lo_x, hi_x, lo_y, hi_y;
    uint32_t i;

    if (!s_gpu.clip_w || !s_gpu.clip_h || !s_gpu.idx_count)
        return 0;
    if (!fetch_attr(&s_gpu.attr[0], s_gpu.idx[0], p))
        return 0;
    lo_x = hi_x = p[0];
    lo_y = hi_y = p[1];
    for (i = 1; i < s_gpu.idx_count; i++) {
        if (!fetch_attr(&s_gpu.attr[0], s_gpu.idx[i], p))
            return 0;
        if (p[0] < lo_x) lo_x = p[0];
        if (p[0] > hi_x) hi_x = p[0];
        if (p[1] < lo_y) lo_y = p[1];
        if (p[1] > hi_y) hi_y = p[1];
    }

    /* Entirely off the surface: nothing to draw under either reading. The
     * positions are in logical pixels and the clip rect is in real ones. */
    if (hi_x * aa_scale_x() < (float)s_gpu.clip_x
     || lo_x * aa_scale_x() > (float)(s_gpu.clip_x + s_gpu.clip_w)
     || hi_y * aa_scale_y() < (float)s_gpu.clip_y
     || lo_y * aa_scale_y() > (float)(s_gpu.clip_y + s_gpu.clip_h))
        return 0;

    /* Small enough to be model units rather than pixels. */
    if (hi_x - lo_x < OBJECT_SPACE_SPAN && hi_y - lo_y < OBJECT_SPACE_SPAN)
        return 0;

    return 1;
}

/* NV097 primitive types.
 *
 * These are the operand of SET_BEGIN_END, where 0 is END and the list starts
 * at 1. They were each one too low, so every title's geometry was decomposed
 * as the primitive below the one it asked for -- a strip as a fan, a fan as
 * quads, and TRIANGLES, the one case whose vertex count must be a multiple
 * of three, as a strip.
 *
 * The vertex order says which numbering is right without taking a table on
 * trust: a strip arrives in Z order and a fan in cyclic order, and they only
 * line up with the primitive under this one. */
#define NV_PRIM_POINTS         1
#define NV_PRIM_LINES          2
#define NV_PRIM_LINE_LOOP      3
#define NV_PRIM_LINE_STRIP     4
#define NV_PRIM_TRIANGLES      5
#define NV_PRIM_TRIANGLE_STRIP 6
#define NV_PRIM_TRIANGLE_FAN   7
#define NV_PRIM_QUADS          8
#define NV_PRIM_QUAD_STRIP     9
#define NV_PRIM_POLYGON        10

/* How many post-draw captures to keep: enough to see whether the geometry
 * is stable from frame to frame, few enough not to fill a directory. */
#define FB_DUMP_AFTER_DRAW 8
static int s_drawn_dumps;

static void dump_surface_bmp(void);

/* A fixed-function batch without texture coordinates is drawn untextured, as
 * the screen-space path does, whatever stale texture the stage still holds. */
static int s_xf_no_tex;

/* Triangles bound for a back end: collected for the whole batch, handed over
 * in one draw() call by backend_flush(). Whatever feeds them -- the program
 * and fixed-function paths here, or the screen-space path below -- the back
 * end only ever sees surface pixels. Past the cap the rest of the batch is
 * dropped. */
#define NV_BACKEND_MAX_VERTS (NV_MAX_INDICES * 3)
static Nv2aVertex *s_bverts;     /* about 5.5 MB: allocated by the first vertex */
static uint32_t   s_bcount;

static Nv2aVertex *backend_slot(void)
{
    if (!s_bverts
        && !(s_bverts = (Nv2aVertex *)calloc(NV_BACKEND_MAX_VERTS, sizeof *s_bverts)))
        return NULL;
    if (s_bcount + 3 > NV_BACKEND_MAX_VERTS)
        return NULL;
    return &s_bverts[s_bcount];
}

static float fog_factor(float d);
static int batch_uses_program(void);

/* RBX: texture-unit-0 coordinate scale, set once per batch at xf_done. */
static float s_bx_su = 1.0f, s_bx_sv = 1.0f;
/* RBX: back-face culling in the executor, where the strip parity is exact
 * (s_xf_odd), instead of after conversion in the back end, which had to guess
 * it from repeated vertices. g_rbx_precull tells the back end the batch it is
 * handed was already culled. RECOMP_GPU_CULL=0 turns culling off in both. */
static int s_xf_odd;
int g_rbx_precull;
static int xf_cull_on(void)
{
    static int on = -1;
    if (on < 0) {
        const char *c = getenv("RECOMP_GPU_CULL");
        on = !(c && c[0] == '0');
    }
    return on;
}

static void backend_from_xf(const Nv2aVshOutput *v, Nv2aVertex *o)
{
    const float su = s_bx_su, sv = s_bx_sv;

    o->x = v->pos[0];
    o->y = v->pos[1];
    o->z = v->pos[2];
    o->rhw = 1.0f / v->pos[3];
    o->diffuse = pack_color(v->d0);
    o->u = v->tex[0][0] * su;
    o->v = v->tex[0][1] * sv;
    memcpy(o->d1, v->d1, sizeof o->d1);
    memcpy(o->tex, v->tex, sizeof o->tex);
    o->fog = fog_factor(v->fog[0]);
}

/* RBX: an indexed batch or a strip hands each transformed vertex to two or
 * three triangles, and each one converted it again (1/w, colour packing,
 * fog). Keep the converted vertex per transform slot for the batch, in a
 * small direct-mapped cache (strips and indexed meshes reuse nearby slots).
 * Clipped vertices live on the stack, not in s_xf, and are converted as
 * before. s_bvc_gen moves on with every batch (raster_batch_program).
 * RECOMP_XF_CONVCACHE=0 turns it off. */
static Nv2aVshOutput *s_xf;     /* transformed vertices of the current batch */
#define BVC_N 1024
static Nv2aVertex s_bvc[BVC_N];
static uint32_t s_bvc_slot[BVC_N], s_bvc_tag[BVC_N], s_bvc_gen = 1;
static void backend_conv(const Nv2aVshOutput *v, Nv2aVertex *o)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RECOMP_XF_CONVCACHE");
        on = !(e && e[0] == '0');
    }
    if (on && s_xf && v >= s_xf && v < s_xf + NV_MAX_INDICES) {
        uint32_t slot = (uint32_t)(v - s_xf), k = slot & (BVC_N - 1);
        if (s_bvc_tag[k] != s_bvc_gen || s_bvc_slot[k] != slot) {
            backend_from_xf(v, &s_bvc[k]);
            s_bvc_tag[k] = s_bvc_gen;
            s_bvc_slot[k] = slot;
        }
        *o = s_bvc[k];
        return;
    }
    backend_from_xf(v, o);
}

/* RBX: indexed hand-off. A program batch used to reach the back end as three
 * full vertices per triangle (~112 bytes each), which the back end copied
 * again per triangle: together ~20% of this thread in the bench profile.
 * Now each transformed vertex is converted and handed over once, and the
 * triangles as 32-bit indices (g_rbx_bidx); the back end draws them with
 * DrawIndexed. Clipped vertices (stack copies) are appended as new ones.
 * RECOMP_GPU_INDEXED=0 restores the expanded hand-off. */
const uint32_t *g_rbx_bidx;
uint32_t g_rbx_bidx_n;
static uint32_t *s_bidx, s_bidx_n, *s_bslot_tag, *s_bslot_out, s_bslot_gen;
static int s_bidx_mode;
extern int rbx_gpu_indexed_ok(void);

/* At the start of a program batch's triangles: use the indexed hand-off? */
static void bidx_begin(void)
{
    static int on = -1;
    s_bidx_mode = 0;
    s_bidx_n = 0;
    if (on < 0) {
        const char *e = getenv("RECOMP_GPU_INDEXED");
        on = !(e && e[0] == '0');
    }
    if (!on || s_bcount || !rbx_gpu_indexed_ok())
        return;
    if (!s_bverts && !(s_bverts = (Nv2aVertex *)calloc(NV_BACKEND_MAX_VERTS, sizeof *s_bverts)))
        return;
    if (!s_bidx && !(s_bidx = (uint32_t *)malloc(NV_BACKEND_MAX_VERTS * sizeof *s_bidx)))
        return;
    if (!s_bslot_tag) {
        s_bslot_tag = (uint32_t *)calloc(NV_MAX_INDICES, sizeof *s_bslot_tag);
        s_bslot_out = (uint32_t *)malloc(NV_MAX_INDICES * sizeof *s_bslot_out);
        if (!s_bslot_tag || !s_bslot_out) {
            free(s_bslot_tag); free(s_bslot_out);
            s_bslot_tag = s_bslot_out = NULL;
            return;
        }
    }
    if (++s_bslot_gen == 0) {          /* wrapped: forget every slot */
        memset(s_bslot_tag, 0, NV_MAX_INDICES * sizeof *s_bslot_tag);
        s_bslot_gen = 1;
    }
    s_bidx_mode = 1;
}

/* RBX: GPU clipping (RECOMP_GPU_CLIP=1). A program or fixed-function batch
 * goes to the back end unclipped and unculled: each transformed vertex once,
 * the primitive's triangles as indices with odd strip triangles flipped, so
 * the winding is the same for every triangle. The back end's vertex shader
 * writes the two near planes the CPU clipper used (w = NV_CLIP_W and the
 * c[59].z plane) as clip distances, and the rasterizer culls with the NV2A
 * cull state. g_rbx_gpuclip: bit 0 on, bit 1 the z plane too; g_rbx_clip_z is
 * that plane's offset in screen z. */
int g_rbx_gpuclip;
float g_rbx_clip_z;
static int s_gclip;
extern int rbx_gpu_clip_ok(void);
static int gclip_on(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RECOMP_GPU_CLIP");
        on = e && e[0] == '1';
        if (on)
            fprintf(stderr, "[RBX XF] GPU clipping on (RECOMP_GPU_CLIP=1)\n");
    }
    return on;
}

/* The hand-off index of a vertex, converting it on first use; ~0u when full. */
static uint32_t bidx_vertex(const Nv2aVshOutput *v)
{
    uint32_t slot = 0;
    int cached = s_xf && v >= s_xf && v < s_xf + NV_MAX_INDICES;
    if (cached) {
        slot = (uint32_t)(v - s_xf);
        if (s_bslot_tag[slot] == s_bslot_gen)
            return s_bslot_out[slot];
    }
    if (s_bcount >= NV_BACKEND_MAX_VERTS)
        return ~0u;
    backend_from_xf(v, &s_bverts[s_bcount]);
    if (s_gclip) {
        /* Unclipped: a vertex behind the eye is drawn too, so it needs a usable
         * position. w = -0 gives rhw = -inf: keep it just behind the w plane. */
        Nv2aVertex *o = &s_bverts[s_bcount];
        float x = o->x, y = o->y, z = o->z;
        if (x - x != 0.0f || y - y != 0.0f || z - z != 0.0f || o->rhw != o->rhw)
            return ~0u;
        if (o->rhw - o->rhw != 0.0f)
            o->rhw = o->rhw > 0.0f ? 1e30f : -1e30f;
    }
    if (cached) {
        s_bslot_tag[slot] = s_bslot_gen;
        s_bslot_out[slot] = s_bcount;
    }
    return s_bcount++;
}

/* `textured`: the batch carried texture coordinates, so the bound texture is
 * part of it. */
uint32_t g_dbg_batches, g_dbg_flush0, g_dbg_xffail, g_dbg_short, g_dbg_begin, g_dbg_ilf, g_dbg_dpn, g_dbg_dp;
static void backend_flush(int textured)
{
    Nv2aSurface surf;
    Nv2aRenderState rs;
    Nv2aTexture tex;
    Nv2aBatch batch;
    Nv2aTexture stages[4];
    unsigned st;
    int use_rgb, use_a, in_textured = textured;

    int indexed = s_bidx_mode;
    s_bidx_mode = 0;
    if (!s_bcount || (indexed && !s_bidx_n)) { g_dbg_flush0++; s_bcount = 0; s_bidx_n = 0; return; }
    current_surface(&surf);
    current_render_state(&rs);
    batch.vertices = s_bverts;
    batch.count = s_bcount;
    batch.texture = NULL;
    batch.state = &rs;
    batch.combiner = NULL;
    batch.stages = NULL;
    batch.stage_valid = 0;
    memset(batch.fog_color, 0, sizeof batch.fog_color);
    /* 3D program / FFP batches: hand the combiners and all four texture stages
     * to the back end (Redcard-style). Screen-space quads keep the simple path. */
    if (s_gpu.rc_seen && (batch_uses_program() || batch_is_ffp())) {
        batch.combiner = &s_gpu.rc;
        batch.stages = stages;
        nv2a_rc_unpack(s_gpu.fog_color, batch.fog_color);
        { float r = batch.fog_color[0]; batch.fog_color[0] = batch.fog_color[2]; batch.fog_color[2] = r; }
        memset(stages, 0, sizeof stages);
        for (st = 0; st < 4; st++) {
            const Texture *t = &s_gpu.texs[st];
            if (t->valid)
                batch.stage_valid |= 1u << st;
            stages[st].offset = t->offset;
            stages[st].width = t->width;
            stages[st].height = t->height;
            stages[st].pitch = t->pitch;
            stages[st].color = t->color;
            stages[st].addr_u = t->addr_u;
            stages[st].addr_v = t->addr_v;
            stages[st].levels = t->levels;
            stages[st].filter = t->filter;
            stages[st].palette = t->palette;
            stages[st].use_mask = 3u;
        }
    }
    /* Once the title programs the combiners they decide what texture stage 0
     * contributes. A vertex-colour quad (the boot screen gradient) leaves a
     * stale texture bound and the combiners ignore it; modulating by it painted
     * the quad black. A text quad takes only the glyph alpha. So probe the
     * combiners: does the output colour (rgb) or alpha move with texture 0?
     * Only for the simple (non-RC) path — RC backends evaluate combiners themselves. */
    use_rgb = use_a = 1;
    if (!batch.combiner && textured && s_gpu.rc_seen) {
        use_rgb = use_a = 0;
        if (s_gpu.rc.stage_program & 0x1F) {
            float v0[4] = { 0.3f, 0.6f, 0.9f, 0.5f }, v1[4] = { 0.2f, 0.4f, 0.6f, 0.8f };
            float fog[4] = { 0.1f, 0.2f, 0.3f, 0.4f }, t[4][4], oa[4], ob[4];
            int k, j;
            for (k = 0; k < 4; k++)
                for (j = 0; j < 4; j++)
                    t[k][j] = 0.5f;
            t[0][0] = 0.2f; t[0][1] = 0.4f; t[0][2] = 0.6f; t[0][3] = 0.2f;
            nv2a_rc_eval(&s_gpu.rc, v0, v1, fog, (const float (*)[4])t, oa);
            t[0][0] = 0.8f; t[0][1] = 0.9f; t[0][2] = 0.7f; t[0][3] = 0.9f;
            nv2a_rc_eval(&s_gpu.rc, v0, v1, fog, (const float (*)[4])t, ob);
            for (j = 0; j < 3; j++)
                if (fabsf(oa[j] - ob[j]) > 0.004f)
                    use_rgb = 1;
            if (fabsf(oa[3] - ob[3]) > 0.004f)
                use_a = 1;
        }
        if (!use_rgb && !use_a)
            textured = 0;
    }
    {
        static int on = -1, nlog;
        if (on < 0) on = getenv("RECOMP_RC_DEBUG") != NULL;
        if (on && nlog < 400) {
            static uint32_t last[4];
            uint32_t sg[4] = { s_gpu.rc.stage_program, s_gpu.rc.color_icw[0] ^ s_gpu.rc.alpha_icw[0], (uint32_t)use_rgb | (uint32_t)use_a << 1 | (uint32_t)textured << 2 | (batch.combiner ? 8u : 0u), s_gpu.rc.final0 ^ s_gpu.rc.alpha_ocw[0] };
            if (memcmp(last, sg, sizeof sg)) {
                memcpy(last, sg, sizeof sg);
                nlog++;
                fprintf(stderr, "[RCDBG] n=%u stage=%08X cicw0=%08X aicw0=%08X cocw0=%08X aocw0=%08X fin0=%08X fin1=%08X texfmt=%02X use_rgb=%d use_a=%d textured=%d in_textured=%d rc=%d valid=%d off=%08X pal=%08X\n",
                        s_bcount, s_gpu.rc.stage_program, s_gpu.rc.color_icw[0], s_gpu.rc.alpha_icw[0], s_gpu.rc.color_ocw[0], s_gpu.rc.alpha_ocw[0],
                        s_gpu.rc.final0, s_gpu.rc.final1, s_gpu.texs[0].color, use_rgb, use_a, textured, in_textured, batch.combiner != NULL, s_gpu.texs[0].valid, s_gpu.texs[0].offset, s_gpu.texs[0].palette);
            }
        }
    }
    if (textured && s_gpu.texs[0].valid) {
        tex.offset = s_gpu.texs[0].offset;
        tex.width  = s_gpu.texs[0].width;
        tex.height = s_gpu.texs[0].height;
        tex.pitch  = s_gpu.texs[0].pitch;
        tex.color  = s_gpu.texs[0].color;
        tex.addr_u = s_gpu.texs[0].addr_u;
        tex.addr_v = s_gpu.texs[0].addr_v;
        tex.levels = s_gpu.texs[0].pitch ? 1 : s_gpu.texs[0].levels;
        tex.filter = s_gpu.texs[0].filter;
        tex.palette = s_gpu.texs[0].palette;
        tex.use_mask = (use_rgb ? 1u : 0u) | (use_a ? 2u : 0u);
        batch.texture = &tex;
    }
    if (indexed) { g_rbx_bidx = s_bidx; g_rbx_bidx_n = s_bidx_n; }
    s_backend->draw(&surf, &batch);
    s_gpu.tris_drawn += (indexed ? s_bidx_n : s_bcount) / 3;
    g_rbx_bidx = NULL; g_rbx_bidx_n = 0; s_bidx_n = 0;
    s_bcount = 0;
    note_drawn();
}

/* One triangle by vertex index: gather position and, if the batch has one,
 * texture coordinate 0. A vertex whose position cannot be read is not drawn;
 * a batch whose texcoords cannot be read is drawn untextured rather than not
 * at all, so a missing coordinate stream costs the colour and not the shape.
 */
/* Forward: defined next to draw_primitive; also used mid-raster. */
static void swm5_pace_wake(void);

static void raster_indexed(uint32_t i0, uint32_t i1, uint32_t i2, uint32_t argb)
{
    float p[3][4], uv[3][2];
    int textured, j;
    static unsigned s_tri_pace;

    if ((++s_tri_pace & 0x3Fu) == 0)
        swm5_pace_wake();

    if (!fetch_attr(&s_gpu.attr[0], i0, p[0])
     || !fetch_attr(&s_gpu.attr[0], i1, p[1])
     || !fetch_attr(&s_gpu.attr[0], i2, p[2]))
        return;
    for (j = 0; j < 3; j++) {                        /* logical -> real pixels */
        p[j][0] *= aa_scale_x();
        p[j][1] *= aa_scale_y();
    }

    textured = fetch_texcoord(i0, uv[0])
            && fetch_texcoord(i1, uv[1])
            && fetch_texcoord(i2, uv[2]);

    if (backend_draws()) {                 /* a back end rasterises */
        Nv2aVertex *o = backend_slot();
        const uint32_t ix[3] = { i0, i1, i2 };
        int k;

        if (!o)
            return;
        for (k = 0; k < 3; k++) {
            o[k].x = p[k][0];
            o[k].y = p[k][1];
            o[k].z = p[k][2];
            o[k].rhw = p[k][3];
            o[k].diffuse = vertex_color(ix[k]);
            o[k].u = textured ? uv[k][0] : 0.0f;
            o[k].v = textured ? uv[k][1] : 0.0f;
        }
        s_bcount += 3;
        return;
    }
    raster_triangle(p[0], p[1], p[2], argb,
                    textured ? (const float (*)[2])uv : NULL);
}

/* ---- 3D: vertex programs and depth -------------------------------------
 *
 * A batch drawn while TRANSFORM_EXECUTION_MODE says "program" carries
 * model-space positions. Each vertex is run through the title's own vertex
 * program (nv2a_vsh_interp.c), whose oPos output on Xbox D3D is already
 * screen space -- the runtime appends the viewport transform and the divide
 * by w -- with the clip-space w left in oPos.w. So the rasteriser gets pixel
 * coordinates and a depth, and w for perspective-correct texturing.
 *
 * ponytail: no clipping. A triangle with any vertex behind the eye (w <= 0)
 * is dropped rather than clipped, which loses the slivers that cross the near
 * plane; road right under the camera is where that shows. Clip in
 * homogeneous space if it matters. */

/* Depth buffers, host-side, one per zeta surface the title uses. The real one
 * lives in guest memory in a tiled format nothing here decodes, and nothing
 * the title does reads it back, so a float buffer per zeta offset is enough. */
#define NV_ZBUF_W 1280   /* 640x480 at 2x2 anti-aliasing */
#define NV_ZBUF_H 1024
static struct { uint32_t offset; float *z; } s_zbufs[4];
static int s_ftrace;            /* frame trace: 0 idle, 2 tracing, 3 done */
static int s_zbuf_next;

static float zclear_value(void)
{
    /* Zeta format in SURFACE_FORMAT bits 4-7: 1 is Z16, 2 is Z24S8. */
    uint32_t zf = (s_gpu.format >> 4) & 0xF;
    return zf == 1 ? (float)(s_gpu.zstencil_clear & 0xFFFF)
                   : (float)(s_gpu.zstencil_clear >> 8);
}

static float *zbuf_current(int create)
{
    int i;
    size_t k;

    for (i = 0; i < 4; i++)
        if (s_zbufs[i].z && s_zbufs[i].offset == s_gpu.zeta_offset)
            return s_zbufs[i].z;
    if (!create)
        return NULL;
    i = s_zbuf_next++ & 3;
    if (!s_zbufs[i].z)
        s_zbufs[i].z = (float *)malloc(sizeof(float) * NV_ZBUF_W * NV_ZBUF_H);
    if (!s_zbufs[i].z)
        return NULL;
    s_zbufs[i].offset = s_gpu.zeta_offset;
    for (k = 0; k < (size_t)NV_ZBUF_W * NV_ZBUF_H; k++)
        s_zbufs[i].z[k] = 3.4e38f;
    return s_zbufs[i].z;
}

static void zbuf_clear(void)
{
    float *z = zbuf_current(1), v = zclear_value();
    size_t k;
    if (backend_draws())
        return;
    if (z)
        for (k = 0; k < (size_t)NV_ZBUF_W * NV_ZBUF_H; k++)
            z[k] = v;
}

static int depth_pass(float z, float stored)
{
    switch (s_gpu.depth_func) {                   /* GL enums, as the NV2A */
    case 0x200: return 0;
    case 0x201: return z <  stored;
    case 0x202: return z == stored;
    case 0x203: return z <= stored;
    case 0x204: return z >  stored;
    case 0x205: return z != stored;
    case 0x206: return z >= stored;
    default:    return 1;
    }
}

static uint32_t pack_color(const float c[4])
{
    /* RBX: SSE form of clamp(c,0,1)*255+0.5 truncated; max_ps maps NaN to 0. */
    __m128 f = _mm_min_ps(_mm_max_ps(_mm_loadu_ps(c), _mm_setzero_ps()), _mm_set1_ps(1.0f));
    __m128i b = _mm_cvttps_epi32(_mm_add_ps(_mm_mul_ps(f, _mm_set1_ps(255.0f)), _mm_set1_ps(0.5f)));
    uint32_t r = (uint32_t)_mm_cvtsi128_si32(b);
    uint32_t g = (uint32_t)_mm_cvtsi128_si32(_mm_srli_si128(b, 4));
    uint32_t bl = (uint32_t)_mm_cvtsi128_si32(_mm_srli_si128(b, 8));
    uint32_t a = (uint32_t)_mm_cvtsi128_si32(_mm_srli_si128(b, 12));
    return (a << 24) | (r << 16) | (g << 8) | bl;
}

static uint32_t modulate(uint32_t t, const float c[4])
{
    float f[4];
    f[0] = (float)((t >> 16) & 0xFF) / 255.0f * c[0];
    f[1] = (float)((t >> 8) & 0xFF) / 255.0f * c[1];
    f[2] = (float)(t & 0xFF) / 255.0f * c[2];
    f[3] = (float)(t >> 24) / 255.0f * c[3];
    return pack_color(f);
}

/* Bytes one texel of a (non-DXT) format takes, for cube-face strides. */
static uint32_t tex_texel_bytes(uint32_t fmt)
{
    switch (fmt) {
    case 0x00: case 0x01: case 0x0B: case 0x13: case 0x19: case 0x1F:
        return 1;
    case 0x02: case 0x03: case 0x04: case 0x05: case 0x10: case 0x11:
    case 0x1C: case 0x1D:
        return 2;
    default:
        return 4;
    }
}

/* Bytes between cube faces: one face with all its mip levels, rounded up to
 * the NV2A's 128-byte face alignment (xemu texture.c). */
static uint32_t tex_face_stride(const Texture *t)
{
    uint32_t w = t->width, h = t->height, lv, total = 0;
    uint32_t block = d3d8_format_dxt_block_bytes(t->color);
    uint32_t levels = t->levels ? t->levels : 1;

    if (!tex_size_from_format(t->color))
        return (t->pitch * t->height + 127u) & ~127u;
    for (lv = 0; lv < levels; lv++) {
        uint32_t lw = (w >> lv) ? (w >> lv) : 1, lh = (h >> lv) ? (h >> lv) : 1;
        total += block ? ((lw + 3) / 4) * ((lh + 3) / 4) * block
                       : lw * lh * tex_texel_bytes(t->color);
    }
    return (total + 127u) & ~127u;
}

/* One texel at normalised (swizzled/DXT) or texel (linear) coordinates, as
 * floats. An unusable stage reads white, so a missing texture multiplies
 * through instead of blacking the pixel out. */
static void rc_texel(const Texture *t, uint32_t face, float u, float v,
                     float out[4])
{
    uint32_t texel, mag = (t->filter >> 24) & 0xF, min = (t->filter >> 16) & 0xFF;
    if (tex_size_from_format(t->color)) {
        u *= (float)t->width;
        v *= (float)t->height;
    }
    /* TENT (bilinear) when the title asks for it, magnifying or minifying:
     * a 64x32 sky gradient stretched over the screen is blocks with nearest
     * texels and a gradient with four. MAG 2 is tent; MIN 2 is tent at LOD 0
     * and 4/6 the tent mip modes.
     * ponytail: level 0 only, no mip selection -- minified textures shimmer.
     * Add LOD from the screen-space derivative when that shows. */
    if (mag == 2 || min == 2 || min == 4 || min == 6) {
        float fu = u - 0.5f, fv = v - 0.5f, wu, wv, c[4][4];
        int32_t iu = (int32_t)floorf(fu), iv = (int32_t)floorf(fv), k, j;
        wu = fu - (float)iu;
        wv = fv - (float)iv;
        for (k = 0; k < 4; k++) {
            if (sample_tex(t, face, (uint32_t)(iu + (k & 1)),
                           (uint32_t)(iv + (k >> 1)), &texel))
                nv2a_rc_unpack(texel, c[k]);
            else
                c[k][0] = c[k][1] = c[k][2] = c[k][3] = 1.0f;
        }
        for (j = 0; j < 4; j++)
            out[j] = (c[0][j] * (1.0f - wu) + c[1][j] * wu) * (1.0f - wv)
                   + (c[2][j] * (1.0f - wu) + c[3][j] * wu) * wv;
        return;
    }
    if (sample_tex(t, face, (uint32_t)(int32_t)floorf(u),
                   (uint32_t)(int32_t)floorf(v), &texel))
        nv2a_rc_unpack(texel, out);
    else
        out[0] = out[1] = out[2] = out[3] = 1.0f;
}

/* What texture stage `st` contributes, by its SHADER_STAGE_PROGRAM mode
 * (xemu psh.c). Returns 0 when a clip-plane stage kills the pixel. */
static int rc_stage_fetch(int st, const float c[4], float out[4])
{
    const Texture *t = &s_gpu.texs[st];
    uint32_t mode = (s_gpu.rc.stage_program >> (st * 5)) & 0x1F, j;
    float q = c[3] != 0.0f ? c[3] : 1.0f;

    switch (mode) {
    case 0:                                       /* NONE */
        out[0] = out[1] = out[2] = 0.0f; out[3] = 1.0f;
        return 1;
    case 1: case 2:                               /* PROJECT2D / 3D */
        rc_texel(t, 0, c[0] / q, c[1] / q, out);
        return 1;
    case 3: {                                     /* CUBEMAP */
        float x = c[0], y = c[1], z = c[2];
        float ax = fabsf(x), ay = fabsf(y), az = fabsf(z), ma, sc, tc;
        uint32_t face;
        if (!t->cube) {
            rc_texel(t, 0, c[0], c[1], out);
            return 1;
        }
        if (ax >= ay && ax >= az) {
            face = x > 0 ? 0 : 1; ma = ax; sc = x > 0 ? -z : z; tc = -y;
        } else if (ay >= az) {
            face = y > 0 ? 2 : 3; ma = ay; sc = x; tc = y > 0 ? z : -z;
        } else {
            face = z > 0 ? 4 : 5; ma = az; sc = z > 0 ? x : -x; tc = -y;
        }
        if (ma == 0.0f)
            ma = 1.0f;
        rc_texel(t, face * tex_face_stride(t), (sc / ma + 1.0f) * 0.5f,
                 (tc / ma + 1.0f) * 0.5f, out);
        return 1;
    }
    case 4:                                       /* PASSTHRU */
        for (j = 0; j < 4; j++)
            out[j] = c[j] < 0.0f ? 0.0f : (c[j] > 1.0f ? 1.0f : c[j]);
        return 1;
    case 5:                                       /* CLIPPLANE */
        out[0] = out[1] = out[2] = out[3] = 0.0f;
        for (j = 0; j < 4; j++) {
            int ge = (s_gpu.clip_plane_mode >> (st * 4 + j)) & 1;
            if (ge ? c[j] >= 0.0f : c[j] < 0.0f)
                return 0;
        }
        return 1;
    default:
        /* ponytail: bump-env and dot-product modes read zero; they are water
         * and bump effects -- add them when a title's look depends on one. */
        out[0] = out[1] = out[2] = out[3] = 0.0f;
        return 1;
    }
}

/* The fixed-function fog unit, per vertex, from the program's oFog.x
 * (xemu vsh.c). Fog disabled reads as factor 1: no fog. */
static float fog_factor(float d)
{
    float f, px = s_gpu.fog_param[0], py = s_gpu.fog_param[1];
    if (!s_gpu.fog_enable)
        return 1.0f;
    switch (s_gpu.fog_mode) {
    case 0x800: case 0x802:                       /* EXP, EXP_ABS */
        f = px + exp2f(d * py * 16.0f) - 1.5f;
        break;
    case 0x801: case 0x803:                       /* EXP2, EXP2_ABS */
        f = px + exp2f(-d * d * py * py * 32.0f) - 1.5f;
        break;
    default:                                      /* LINEAR, LINEAR_ABS */
        f = px + d * py - 1.0f;
        break;
    }
    if (s_gpu.fog_mode == 0x802 || s_gpu.fog_mode == 0x803
        || s_gpu.fog_mode == 0x804)
        f = fabsf(f);
    if (f != f)
        f = 1.0f;
    return f < 0.0f ? 0.0f : (f > 1.0f ? 1.0f : f);
}

static int alpha_test_pass(float a)
{
    int v = (int)(a * 255.0f + 0.5f), r = (int)(s_gpu.alpha_ref & 0xFF);
    switch (s_gpu.alpha_func) {
    case 0x200: return 0;                         /* NEVER    */
    case 0x201: return v <  r;                    /* LESS     */
    case 0x202: return v == r;                    /* EQUAL    */
    case 0x203: return v <= r;                    /* LEQUAL   */
    case 0x204: return v >  r;                    /* GREATER  */
    case 0x205: return v != r;                    /* NOTEQUAL */
    case 0x206: return v >= r;                    /* GEQUAL   */
    default:    return 1;                         /* ALWAYS   */
    }
}

/* Everything a triangle's pixels need, computed once per triangle, so the
 * pixel loop can run over any subset of rows on any thread. */
typedef struct {
    uint8_t *mem;
    uint32_t bpp;
    const Nv2aVshOutput *va, *vb, *vc;
    const float *a, *b, *c;
    float iw[3], uv[3][2], stc[3][4][4], vfog[3], fogc[4], inv_area;
    float *zb;
    int minx, maxx, miny, maxy, use_rc, textured;
} XfTri;

typedef struct { uint64_t depth_fail, pixels; uint32_t zpass; } XfCount;

/* Rows y0, y0+step, ... < maxy of triangle T. */
static void xf_rows(const XfTri *T, int y0, int step, XfCount *cnt)
{
    const float *a = T->a, *b = T->b, *c = T->c;
    const Nv2aVshOutput *va = T->va, *vb = T->vb, *vc = T->vc;
    int x, y, k;

    for (y = y0; y < T->maxy; y += step) {
        for (x = T->minx; x < T->maxx; x++) {
            float px = (float)x + 0.5f, py = (float)y + 0.5f;
            float w0 = (b[0] - a[0]) * (py - a[1]) - (b[1] - a[1]) * (px - a[0]);
            float w1 = (c[0] - b[0]) * (py - b[1]) - (c[1] - b[1]) * (px - b[0]);
            float w2 = (a[0] - c[0]) * (py - c[1]) - (a[1] - c[1]) * (px - c[0]);
            float l0, l1, l2, z, col[4], pw;
            uint32_t argb;
            if (!((w0 >= 0 && w1 >= 0 && w2 >= 0)
               || (w0 <= 0 && w1 <= 0 && w2 <= 0)))
                continue;
            /* w1 is opposite a, w2 opposite b, w0 opposite c. */
            l0 = w1 * T->inv_area; l1 = w2 * T->inv_area; l2 = w0 * T->inv_area;
            z = l0 * a[2] + l1 * b[2] + l2 * c[2];
            {
                float *zp = T->zb ? &T->zb[(size_t)y * NV_ZBUF_W + x] : NULL;
                int shade = (s_gpu.color_mask & 0x01010101u) || s_gpu.alpha_test;
                /* Depth test, then shade, then alpha test, and only then the
                 * depth write: an alpha-tested texel that is cut away must
                 * not leave its depth behind (foliage, fences). */
                if (zp && s_gpu.depth_test && !depth_pass(z, *zp)) {
                    cnt->depth_fail++;
                    continue;
                }
                argb = 0;
                if (shade) {
                    for (k = 0; k < 4; k++)
                        col[k] = l0 * va->d0[k] + l1 * vb->d0[k] + l2 * vc->d0[k];
                    pw = l0 * T->iw[0] + l1 * T->iw[1] + l2 * T->iw[2];
                    if (T->use_rc) {
                        float d1[4], fog[4], t[4][4], out[4], tc[4];
                        int st, j, keep = 1;
                        for (k = 0; k < 4; k++)
                            d1[k] = l0 * va->d1[k] + l1 * vb->d1[k] + l2 * vc->d1[k];
                        fog[0] = T->fogc[0]; fog[1] = T->fogc[1]; fog[2] = T->fogc[2];
                        fog[3] = l0 * T->vfog[0] + l1 * T->vfog[1] + l2 * T->vfog[2];
                        for (st = 0; st < 4 && keep; st++) {
                            if (!((s_gpu.rc.stage_program >> (st * 5)) & 0x1F)) {
                                t[st][0] = t[st][1] = t[st][2] = 0.0f;
                                t[st][3] = 1.0f;
                                continue;
                            }
                            for (j = 0; j < 4; j++)
                                tc[j] = (l0 * T->stc[0][st][j] + l1 * T->stc[1][st][j]
                                       + l2 * T->stc[2][st][j]) / pw;
                            keep = rc_stage_fetch(st, tc, t[st]);
                        }
                        if (!keep)
                            continue;
                        nv2a_rc_eval(&s_gpu.rc, col, d1, fog,
                                     (const float (*)[4])t, out);
                        if (s_gpu.alpha_test && !alpha_test_pass(out[3]))
                            continue;
                        argb = pack_color(out);
                    } else {
                        argb = pack_color(col);
                        if (T->textured) {
                            uint32_t texel;
                            float tu, tv;
                            tu = (l0 * T->uv[0][0] + l1 * T->uv[1][0] + l2 * T->uv[2][0]) / pw;
                            tv = (l0 * T->uv[0][1] + l1 * T->uv[1][1] + l2 * T->uv[2][1]) / pw;
                            if (sample_texture((uint32_t)(int32_t)floorf(tu),
                                               (uint32_t)(int32_t)floorf(tv), &texel))
                                argb = modulate(texel, col);
                        }
                        if (s_gpu.alpha_test
                            && !alpha_test_pass((float)(argb >> 24) / 255.0f))
                            continue;
                    }
                }
                if (zp && s_gpu.depth_mask)
                    *zp = z;
                if (s_gpu.zpass_enable)
                    cnt->zpass++;
                if (!(s_gpu.color_mask & 0x01010101u))
                    continue;              /* colour writes off: depth only */
                cnt->pixels++;
            }
            put_pixel(T->mem, T->bpp, x, y, argb);
        }
    }
}

/* Worker threads for big triangles.
 *
 * The rasteriser is a software GPU, and a Burnout 3 frame is dominated by a
 * handful of full-screen passes -- composites, blurs, the menu backdrops --
 * each two triangles of 300,000 pixels through the register combiners. Those
 * split cleanly by row: every pixel reads only its own depth and colour, so
 * interleaved rows on N threads need no locking. Small triangles stay on the
 * executor thread, where waking workers would cost more than it saves.
 * RECOMP_RASTER_THREADS=<n> sets the count (1 = off); the default leaves a
 * few cores for the title and the host.
 * ponytail: s_gpu.pixels/pixel_max in put_pixel are unsynchronised stats and
 * may undercount; nothing depends on them. */
#define NV_RASTER_MAX_THREADS 16
#define NV_RASTER_MT_MIN_PIXELS 8192

#if defined(_WIN32)
static struct {
    int n;                                  /* threads incl. the caller */
    HANDLE start[NV_RASTER_MAX_THREADS], done;
    volatile LONG pending;
    const XfTri *tris;
    int ntris;
    XfCount cnt[NV_RASTER_MAX_THREADS];
} s_pool;

/* Worker k owns the rows y with y % n == k, for every triangle of the run,
 * in submission order. Each pixel therefore sees its triangles in the order
 * the title drew them, whichever thread draws it. */
static void xf_rows_owned(const XfTri *tris, int ntris, int k, int n,
                          XfCount *cnt)
{
    int i;
    for (i = 0; i < ntris; i++) {
        int y0 = tris[i].miny + ((k - tris[i].miny) % n + n) % n;
        xf_rows(&tris[i], y0, n, cnt);
    }
}

static DWORD WINAPI raster_worker(LPVOID arg)
{
    int k = (int)(intptr_t)arg;
    for (;;) {
        WaitForSingleObject(s_pool.start[k], INFINITE);
        memset(&s_pool.cnt[k], 0, sizeof s_pool.cnt[k]);
        xf_rows_owned(s_pool.tris, s_pool.ntris, k, s_pool.n, &s_pool.cnt[k]);
        if (InterlockedDecrement(&s_pool.pending) == 0)
            SetEvent(s_pool.done);
    }
    return 0;
}

static int raster_pool_size(void)
{
    static int init;
    if (!init) {
        const char *e = getenv("RECOMP_RASTER_THREADS");
        SYSTEM_INFO si;
        int n, k;
        init = 1;
        GetSystemInfo(&si);
        n = e ? atoi(e) : (int)si.dwNumberOfProcessors - 4;
        if (n < 1) n = 1;
        if (n > NV_RASTER_MAX_THREADS) n = NV_RASTER_MAX_THREADS;
        s_pool.n = n;
        if (n > 1) {
            s_pool.done = CreateEventA(NULL, FALSE, FALSE, NULL);
            for (k = 0; k < n - 1; k++) {
                s_pool.start[k] = CreateEventA(NULL, FALSE, FALSE, NULL);
                CloseHandle(CreateThread(NULL, 0, raster_worker,
                                         (LPVOID)(intptr_t)k, 0, NULL));
            }
        }
    }
    return s_pool.n;
}

static void xf_rows_parallel(const XfTri *T, int ntris, long px,
                             XfCount *total)
{
    int n = raster_pool_size(), k;

    if (n <= 1 || px < NV_RASTER_MT_MIN_PIXELS) {
        for (k = 0; k < ntris; k++)
            xf_rows(&T[k], T[k].miny, 1, total);
        return;
    }
    s_pool.tris = T;
    s_pool.ntris = ntris;
    s_pool.pending = n - 1;
    for (k = 0; k < n - 1; k++)
        SetEvent(s_pool.start[k]);
    {
        XfCount mine = {0, 0, 0};
        xf_rows_owned(T, ntris, n - 1, n, &mine);
        WaitForSingleObject(s_pool.done, INFINITE);
        total->depth_fail += mine.depth_fail;
        total->pixels += mine.pixels;
        total->zpass += mine.zpass;
    }
    for (k = 0; k < n - 1; k++) {
        total->depth_fail += s_pool.cnt[k].depth_fail;
        total->pixels += s_pool.cnt[k].pixels;
        total->zpass += s_pool.cnt[k].zpass;
    }
}
#else
static void xf_rows_parallel(const XfTri *T, int ntris, long px,
                             XfCount *total)
{
    int k;
    (void)px;
    for (k = 0; k < ntris; k++)
        xf_rows(&T[k], T[k].miny, 1, total);
}
#endif

/* Triangles waiting for the end of their batch.
 *
 * A level is thousands of small triangles, each far below the size worth
 * waking the pool for, so they all drew on the executor thread while the
 * workers slept: Fallout: Brotherhood of Steel's first level ran at 5 fps
 * with the executor at 100% and every worker idle. Queuing a batch's
 * triangles and handing the whole run over at once costs one wake per batch.
 * GPU state cannot change inside a batch, so drawing later reads the same
 * state. The vertices are copied: near-plane clipping passes stack locals.
 * ponytail: a fixed-size queue flushed when full; 2048 covers a typical
 * batch, and a bigger one just splits into several runs. */
#define XF_QUEUE_MAX 2048
typedef struct { XfTri t; Nv2aVshOutput v[3]; } XfQueued;
static XfQueued *s_xfq;
static XfTri s_xfq_tris[XF_QUEUE_MAX];
static int s_xfq_n;
static long s_xfq_px;

static void xf_queue_flush(void)
{
    XfCount cnt = {0, 0, 0};
    int i;

    if (!s_xfq_n)
        return;
    for (i = 0; i < s_xfq_n; i++)
        s_xfq_tris[i] = s_xfq[i].t;
    xf_rows_parallel(s_xfq_tris, s_xfq_n, s_xfq_px, &cnt);
    s_gpu.xf_depth_fail += cnt.depth_fail;
    s_gpu.xf_pixels += cnt.pixels;
    s_gpu.zpass_count += cnt.zpass;
    s_xfq_n = 0;
    s_xfq_px = 0;
}

static void xf_queue_push(const XfTri *T)
{
    XfQueued *q;

    if (!s_xfq && !(s_xfq = (XfQueued *)malloc(XF_QUEUE_MAX * sizeof *s_xfq))) {
        XfCount cnt = {0, 0, 0};
        xf_rows_parallel(T, 1, 0, &cnt);
        return;
    }
    if (s_xfq_n == XF_QUEUE_MAX)
        xf_queue_flush();
    q = &s_xfq[s_xfq_n++];
    q->t = *T;
    q->v[0] = *T->va; q->v[1] = *T->vb; q->v[2] = *T->vc;
    q->t.va = &q->v[0]; q->t.vb = &q->v[1]; q->t.vc = &q->v[2];
    q->t.a = q->v[0].pos; q->t.b = q->v[1].pos; q->t.c = q->v[2].pos;
    s_xfq_px += (long)(T->maxx - T->minx) * (T->maxy - T->miny);
}

static void raster_xf_triangle(const Nv2aVshOutput *va, const Nv2aVshOutput *vb,
                               const Nv2aVshOutput *vc)
{
    XfTri T;
    XfCount cnt = {0, 0, 0};
    const Nv2aVshOutput *v[3];
    const float *a, *b, *c;
    float area, su = 1.0f, sv = 1.0f;
    int k;
    static int no_rc = -1;
    static unsigned s_xf_pace;

    if ((++s_xf_pace & 0x3Fu) == 0)
        swm5_pace_wake();

    if (backend_draws()) {
        /* RBX: a back end rasterises -- skip the CPU-raster setup (memset of
         * XfTri etc.); same rejections as the general path below. */
        const float *pa = va->pos, *pb = vb->pos, *pc = vc->pos;
        uint32_t bpp = surface_bpp();
        float ar;
        Nv2aVertex *o;

        if (bpp != 4 && bpp != 2)
            return;
        if (pa[3] <= 0.0f || pb[3] <= 0.0f || pc[3] <= 0.0f) {
            s_gpu.tris_behind++;
            return;
        }
        ar = (pb[0] - pa[0]) * (pc[1] - pa[1]) - (pb[1] - pa[1]) * (pc[0] - pa[0]);
        if (ar == 0.0f || ar != ar) {
            s_gpu.xf_degenerate++;
            return;
        }
        if (s_reg[0x0308 / 4] && xf_cull_on()) {       /* SET_CULL_FACE_ENABLE */
            uint32_t cf = s_reg[0x039C / 4], ff = s_reg[0x03A0 / 4];
            float sa = s_xf_odd ? -ar : ar;
            /* y grows downwards in surface pixels: positive area is clockwise. */
            int front = ff == 0x901 ? sa < 0.0f : sa > 0.0f;
            if ((cf == 0x405 && !front) || (cf == 0x404 && front) || cf == 0x408)
                return;
        }
        if (s_bidx_mode) {
            uint32_t ia, ib, ic;
            if (s_bidx_n + 3 > NV_BACKEND_MAX_VERTS)
                return;
            ia = bidx_vertex(va); ib = bidx_vertex(vb); ic = bidx_vertex(vc);
            if (ia == ~0u || ib == ~0u || ic == ~0u)
                return;
            s_bidx[s_bidx_n] = ia; s_bidx[s_bidx_n + 1] = ib; s_bidx[s_bidx_n + 2] = ic;
            s_bidx_n += 3;
            return;
        }
        if ((o = backend_slot()) != NULL) {
            backend_conv(va, &o[0]);
            backend_conv(vb, &o[1]);
            backend_conv(vc, &o[2]);
            s_bcount += 3;
        }
        return;
    }
    if (no_rc < 0)
        no_rc = getenv("RECOMP_NO_COMBINERS") != NULL;
    memset(&T, 0, sizeof T);
    T.mem = (uint8_t *)xbox_GetMemoryOffset();
    T.bpp = surface_bpp();
    T.textured = s_gpu.texs[0].valid && !s_xf_no_tex;
    T.use_rc = s_gpu.rc_seen && !no_rc;
    T.va = va; T.vb = vb; T.vc = vc;

    v[0] = va; v[1] = vb; v[2] = vc;
    a = va->pos; b = vb->pos; c = vc->pos;
    T.a = a; T.b = b; T.c = c;
    if (T.bpp != 4 && T.bpp != 2)
        return;
    if (a[3] <= 0.0f || b[3] <= 0.0f || c[3] <= 0.0f) {
        s_gpu.tris_behind++;
        return;
    }
    area = (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0]);
    if (area == 0.0f || area != area) {
        s_gpu.xf_degenerate++;
        return;
    }
    if (backend_draws()) {                     /* a back end rasterises */
        Nv2aVertex *o = backend_slot();
        if (o) {
            backend_from_xf(va, &o[0]);
            backend_from_xf(vb, &o[1]);
            backend_from_xf(vc, &o[2]);
            s_bcount += 3;
        }
        return;
    }
    for (k = 0; k < 3; k++) {
        if (!s_gpu.xf_seeded)
            s_gpu.xf_min[k] = s_gpu.xf_max[k] = a[k];
        if (a[k] < s_gpu.xf_min[k]) s_gpu.xf_min[k] = a[k];
        if (a[k] > s_gpu.xf_max[k]) s_gpu.xf_max[k] = a[k];
    }
    s_gpu.xf_seeded = 1;
    if (!surface_begin_batch(T.mem))
        return;
    if (s_gpu.depth_test || s_gpu.depth_mask)
        T.zb = zbuf_current(1);
    if (tex_size_from_format(s_gpu.texs[0].color)) {
        su = (float)s_gpu.texs[0].width;
        sv = (float)s_gpu.texs[0].height;
    }
    for (k = 0; k < 3; k++) {
        T.iw[k] = 1.0f / v[k]->pos[3];
        T.uv[k][0] = v[k]->tex[0][0] * su * T.iw[k];
        T.uv[k][1] = v[k]->tex[0][1] * sv * T.iw[k];
    }
    if (T.use_rc) {
        int st, j;
        for (k = 0; k < 3; k++) {
            for (st = 0; st < 4; st++)
                for (j = 0; j < 4; j++)
                    T.stc[k][st][j] = v[k]->tex[st][j] * T.iw[k];
            T.vfog[k] = fog_factor(v[k]->fog[0]);
        }
        /* FOG_COLOR is R in bits 0-7, the reverse of a D3DCOLOR. */
        nv2a_rc_unpack(s_gpu.fog_color, T.fogc);
        { float r = T.fogc[2]; T.fogc[2] = T.fogc[0]; T.fogc[0] = r; }
    }

    T.minx = (int)floorf(fminf(a[0], fminf(b[0], c[0])));
    T.maxx = (int)ceilf (fmaxf(a[0], fmaxf(b[0], c[0])));
    T.miny = (int)floorf(fminf(a[1], fminf(b[1], c[1])));
    T.maxy = (int)ceilf (fmaxf(a[1], fmaxf(b[1], c[1])));
    if (T.minx < (int)s_gpu.clip_x) T.minx = (int)s_gpu.clip_x;
    if (T.miny < (int)s_gpu.clip_y) T.miny = (int)s_gpu.clip_y;
    if (T.maxx > (int)(s_gpu.clip_x + s_gpu.clip_w)) T.maxx = (int)(s_gpu.clip_x + s_gpu.clip_w);
    if (T.maxy > (int)(s_gpu.clip_y + s_gpu.clip_h)) T.maxy = (int)(s_gpu.clip_y + s_gpu.clip_h);
    if (T.maxx > NV_ZBUF_W) T.maxx = NV_ZBUF_W;
    if (T.maxy > NV_ZBUF_H) T.maxy = NV_ZBUF_H;
    if (T.minx >= T.maxx || T.miny >= T.maxy) {
        s_gpu.tris_skipped_offscreen++;
        s_gpu.xf_offscreen++;
        return;
    }
    s_gpu.xf_drawn++;
    T.inv_area = 1.0f / area;

    xf_queue_push(&T);
    (void)cnt;
    s_gpu.tris_drawn++;
    note_drawn();
}


/* Near-plane clipping.
 *
 * A triangle with a vertex behind the eye (w <= 0) used to be dropped whole.
 * That is invisible for distant geometry and ruinous close up: in a chase
 * view the road under the camera is exactly the set of triangles that reach
 * behind it, so the bottom of the frame showed a hole -- a quarter of every
 * race frame's triangles went this way.
 *
 * Clipping has to happen in clip space, but the D3D epilogue has already
 * divided x, y and z by w and applied the viewport, leaving w alone. Each
 * vertex is taken to (x*w, y*w, z*w, w): screen = clip / w * scale + offset
 * makes that an affine image of clip space, so clipping and interpolating
 * there is exact without knowing the scale. That matters: the viewport is
 * the title's own business -- c[58]/c[59] for most, but SSX 3's programs
 * keep it elsewhere and c[58] reads 0, which turned every clipped triangle
 * into a point. Everything else the vertex carries -- colours, fog, texture
 * coordinates -- is linear in clip space and is interpolated directly.
 *
 * The polygon is clipped against w = NV_CLIP_W and then the near plane
 * z = 0 (Sutherland-Hodgman), divided back by w, and fanned. The near plane
 * is screen z = the viewport's z offset (s_xf_o[2], 0 for D3D). Clipping at
 * w alone let geometry between the eye and the near plane through at huge
 * screen coordinates: torn streaks at the frame's edges. RECOMP_CLIP_NEAR_Z=0
 * clips at w only.
 * ponytail: near planes only; x/y/far fall to the rasteriser's bounds
 * check, which is correct, just slower for huge off-screen triangles. */
#define NV_CLIP_W 1e-3f

/* The z offset of the current batch's screen-space transform (the near
 * plane, see above), scaled like the vertices. Set by xf_set_viewport(). */
static float s_xf_o[3];

static void xf_lerp(const Nv2aVshOutput *a, const Nv2aVshOutput *b, float t,
                    Nv2aVshOutput *out)
{
    const float *pa = (const float *)a, *pb = (const float *)b;
    float *po = (float *)out;
    size_t i, n = sizeof(Nv2aVshOutput) / sizeof(float);
    for (i = 0; i < n; i++)
        po[i] = pa[i] + (pb[i] - pa[i]) * t;
}

/* Signed distance of a vertex in (x*w, y*w, z*w, w) to clip plane `pl`:
 * 0 is w = NV_CLIP_W, 1 the near plane. */
static float xf_clip_dist(const Nv2aVshOutput *v, int pl)
{
    return pl == 0 ? v->pos[3] - NV_CLIP_W : v->pos[2] - s_xf_o[2] * v->pos[3];
}

/* Sutherland-Hodgman: clip polygon in[0..n) against plane pl into out. */
static int xf_clip_poly(const Nv2aVshOutput *in, int n, int pl,
                        Nv2aVshOutput *out)
{
    int i, m = 0;
    for (i = 0; i < n; i++) {
        const Nv2aVshOutput *p = &in[i], *q = &in[(i + 1) % n];
        float dp = xf_clip_dist(p, pl), dq = xf_clip_dist(q, pl);
        if (dp >= 0.0f)
            out[m++] = *p;
        if ((dp >= 0.0f) != (dq >= 0.0f)) {
            Nv2aVshOutput *v = &out[m++];
            xf_lerp(p, q, dp / (dp - dq), v);
            /* Exactly on the plane: at w in the 1e4s the lerp's rounding is
             * bigger than NV_CLIP_W, and a w <= 0 divides to inf/NaN. */
            if (pl == 0)
                v->pos[3] = NV_CLIP_W;
            else
                v->pos[2] = s_xf_o[2] * v->pos[3];
        }
    }
    return m;
}

/* The near-plane clip itself, out of line. Its two 8-vertex polygons are
 * ~2.5 KB of stack; inlined, every triangle paid for that frame (and its
 * security cookie) although few need clipping. RBX. */
#if defined(_MSC_VER)
__declspec(noinline)
#else
__attribute__((noinline))
#endif
static void raster_xf_clip_slow(const Nv2aVshOutput *a, const Nv2aVshOutput *b,
                                const Nv2aVshOutput *c, int planes)
{
    const Nv2aVshOutput *in[3];
    Nv2aVshOutput p0[8], p1[8];
    int n = 3, i;

    in[0] = a; in[1] = b; in[2] = c;
    for (i = 0; i < 3; i++) {
        p0[i] = *in[i];
        for (int j = 0; j < 3; j++)
            p0[i].pos[j] *= p0[i].pos[3];
    }
    n = xf_clip_poly(p0, n, 0, p1);
    if (planes > 1 && n)
        n = xf_clip_poly(p1, n, 1, p0);
    else
        memcpy(p0, p1, sizeof p1);
    if (n < 3) {
        s_gpu.tris_behind++;                  /* wholly clipped away */
        return;
    }
    /* Back to screen space: divide by the (now positive) w. */
    for (i = 0; i < n; i++)
        for (int j = 0; j < 3; j++)
            p0[i].pos[j] /= p0[i].pos[3];
    for (i = 1; i + 1 < n; i++)
        raster_xf_triangle(&p0[0], &p0[i], &p0[i + 1]);
}

/* RECOMP_CLIP_NEAR_Z=0: clip at w = NV_CLIP_W only, not at the c[59].z plane. */
static int clip_near_z(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RECOMP_CLIP_NEAR_Z");
        on = !(e && e[0] == '0');
    }
    return on;
}

static void raster_xf_clipped(const Nv2aVshOutput *a, const Nv2aVshOutput *b,
                              const Nv2aVshOutput *c)
{
    const Nv2aVshOutput *in[3];
    int i, inside = 1, near_z = clip_near_z();

    in[0] = a; in[1] = b; in[2] = c;
    /* Only W and Z contribute to the near planes. Compute X and Y in
     * clip space only for triangles that need polygon clipping. */
    for (i = 0; i < 3; i++) {
        float w = in[i]->pos[3];
        float d = w - NV_CLIP_W;
        if (d != d || d - d != 0.0f)
            return;
        if (d < 0.0f)
            inside = 0;
        if (near_z) {
            float z = in[i]->pos[2] * w;
            d = z - s_xf_o[2] * w;
            if (d != d || d - d != 0.0f)
                return;                       /* not finite: no position at all */
            if (d < 0.0f)
                inside = 0;
        }
    }
    if (inside)
        raster_xf_triangle(a, b, c);
    else
        raster_xf_clip_slow(a, b, c, near_z ? 2 : 1);
}

/* s_xf (declared above backend_conv): about 7 MB, so allocated by the first
 * batch that transforms rather than carried in every build that never turns
 * the executor on. */

/* Does this batch run the title's vertex program? RECOMP_NO_VSH turns the
 * interpreter off (the batch then falls to the screen-space heuristics). */
static int batch_uses_program(void)
{
    static int no_vsh = -1;

    if (no_vsh < 0)
        no_vsh = getenv("RECOMP_NO_VSH") != NULL;
    return (s_gpu.xf_mode & 3) == 2 && !no_vsh;
}

/* RBX: a shadow of the vertex program, from the same PROGRAM_LOAD / PROGRAM /
 * PROGRAM_START methods the interpreter gets, so the executor can tell which
 * program a batch runs (the toolkit exposes no program identity). The hash
 * covers the slots from the start slot through the first FINAL. Used to pick
 * a specialised transform (vsh_fast_*) and logged once per new program.
 * ponytail: nv2a_vsh_set_instruction writes bypass this shadow; nothing in
 * this title calls it. */
static uint32_t s_vp_words[136 * 4], s_vp_load, s_vp_start;
static int s_vp_dirty = 1;
static uint64_t s_vp_hash;
static uint32_t s_vp_len;

static uint64_t vp_hash(void)
{
    if (s_vp_dirty) {
        uint64_t h = 1469598103934665603ULL ^ s_vp_start;
        uint32_t slot, k;
        s_vp_len = 0;
        for (slot = s_vp_start; slot < 136; slot++) {
            for (k = 0; k < 4; k++)
                h = (h ^ s_vp_words[slot * 4 + k]) * 1099511628211ULL;
            s_vp_len++;
            if (s_vp_words[slot * 4 + 3] & 1u)          /* FINAL */
                break;
        }
        s_vp_hash = h;
        s_vp_dirty = 0;
        {
            static uint64_t seen[32];
            static unsigned nseen;
            unsigned i;
            for (i = 0; i < nseen && seen[i] != h; i++)
                ;
            if (i == nseen && nseen < 32) {
                seen[nseen++] = h;
                fprintf(stderr, "[VSH] program %016llX start %u slots %u\n",
                        (unsigned long long)h, s_vp_start, s_vp_len);
            }
        }
    }
    return s_vp_hash;
}

/* RBX: specialised transforms for the vertex programs this title runs.
 *
 * The whole game uses seven programs; two of them (the world/model transform
 * and its skinned twin) are ~86% of all transformed vertices on the bench.
 * Interpreting them cost ~16-23% of the pushbuffer thread, the bottleneck.
 * Each function below is the program written out in C, reading only the
 * inputs and constants it uses, with the interpreter's semantics: outputs
 * not written keep the interpreter's defaults (oD0.w = oD1.w = 1, oTn =
 * 0,0,0,1, the rest 0), colours clamp to [0,1], R12 aliases oPos, and the
 * paired MAC+ILU of the epilogue reads before it writes (ILU result to R1).
 * An unknown program, CXT_WRITE or RECOMP_VSH_FAST=0 uses the interpreter.
 * RECOMP_VSH_FASTCHECK=N also runs the interpreter on every Nth vertex and
 * compares; a mismatch is logged and turns that program's fast path off. */
static uint32_t s_rbx_cxt_write;   /* defined with the transform workers below */
static void vsh_in(uint32_t a, uint32_t index, float v[4])
{
    if (!s_gpu.attr[a].size)
        memcpy(v, s_gpu.imm_attr[a], 4 * sizeof(float));
    else
        fetch_attr(&s_gpu.attr[a], index, v);
}
static float vsh_dp4(const float *a, const float *b) { return ((a[0] * b[0] + a[1] * b[1]) + a[2] * b[2]) + a[3] * b[3]; }
static float vsh_dp3(const float *a, const float *b) { return (a[0] * b[0] + a[1] * b[1]) + a[2] * b[2]; }
static float vsh_clamp01(float x) { return x < 0.0f ? 0.0f : x > 1.0f ? 1.0f : x; }
static float vsh_rcc(float x)
{
    float r = 1.0f / x;
    if (r >= 0.0f) { if (r < 5.42101e-20f) r = 5.42101e-20f; else if (r > 1.884467e19f) r = 1.884467e19f; }
    else { if (r > -5.42101e-20f) r = -5.42101e-20f; else if (r < -1.884467e19f) r = -1.884467e19f; }
    return r;
}
static void vsh_defaults(Nv2aVshOutput *o)
{
    int t;
    memset(o, 0, sizeof *o);
    o->d0[3] = o->d1[3] = 1.0f;
    for (t = 0; t < 4; t++) o->tex[t][3] = 1.0f;
}
/* oPos epilogue shared by the D3D programs: pos.xyz = pos.xyz * c58 * rcc(w) + c59. */
static void vsh_epilogue(Nv2aVshOutput *o)
{
    const float *c58 = nv2a_vsh_constant(58), *c59 = nv2a_vsh_constant(59);
    float r1 = vsh_rcc(o->pos[3]);
    int k;
    for (k = 0; k < 3; k++)
        o->pos[k] = (o->pos[k] * c58[k]) * r1 + c59[k];
}
static void vsh_color(float d[4], const float *c)
{
    int k;
    for (k = 0; k < 4; k++) d[k] = vsh_clamp01(c[k]);
}

/* 0B25A84A2631EC80: oPos = v0 * c96..99; oD0 = c100; oT0 = v2;
 * oFog.x = (v0.c110) * c106.x + c106.y; oT1.x = v1.-c101 (dp3); oT1.y = c102.w */
static void vsh_fast_0b25(uint32_t index, Nv2aVshOutput *o)
{
    float v0[4], v1[4], r2z, nc101[4];
    const float *c101 = nv2a_vsh_constant(101), *c106 = nv2a_vsh_constant(106);
    int k;
    vsh_in(0, index, v0); vsh_in(1, index, v1);
    vsh_defaults(o);
    for (k = 0; k < 4; k++) o->pos[k] = vsh_dp4(v0, nv2a_vsh_constant(96 + k));
    vsh_color(o->d0, nv2a_vsh_constant(100));
    o->d0_written = 1;
    r2z = vsh_dp4(v0, nv2a_vsh_constant(110));
    vsh_in(2, index, o->tex[0]);
    o->fog[0] = r2z * c106[0] + c106[1];
    for (k = 0; k < 4; k++) nc101[k] = -c101[k];
    o->tex[1][0] = vsh_dp3(v1, nc101);
    o->tex[1][1] = nv2a_vsh_constant(102)[3];
    vsh_epilogue(o);
}

/* 9CEE4A21EBB77FF1: R3 = v0 + v1 * c101.xxxy; oPos = R3 * c96..99; oD0 = c100;
 * oFog.x = (v0.c110) * c106.x + c106.y */
static void vsh_fast_9cee(uint32_t index, Nv2aVshOutput *o)
{
    float v0[4], v1[4], r3[4], r4z;
    const float *c101 = nv2a_vsh_constant(101), *c106 = nv2a_vsh_constant(106);
    int k;
    vsh_in(0, index, v0); vsh_in(1, index, v1);
    vsh_defaults(o);
    r3[0] = v0[0] + v1[0] * c101[0];
    r3[1] = v0[1] + v1[1] * c101[0];
    r3[2] = v0[2] + v1[2] * c101[0];
    r3[3] = v0[3] + v1[3] * c101[1];
    vsh_color(o->d0, nv2a_vsh_constant(100));
    o->d0_written = 1;
    r4z = vsh_dp4(v0, nv2a_vsh_constant(110));
    for (k = 0; k < 4; k++) o->pos[k] = vsh_dp4(r3, nv2a_vsh_constant(96 + k));
    o->fog[0] = r4z * c106[0] + c106[1];
    vsh_epilogue(o);
}

/* BA9E387C24FCECE1 (pretransformed pass-through): R1 = v0 with w = rcp(v0.w);
 * oPos = R1 * c0 + c1; oD0 = v3; oD1 = v4 (ILU mov); oFog = rcp(v0.w);
 * oT0..3 = v9..v12. oPts and the back colours (v1, v7, v8) have no output
 * slot here. */
static float vsh_rcp(float x) { return x == 0.0f ? INFINITY : 1.0f / x; }
static void vsh_fast_ba9e(uint32_t index, Nv2aVshOutput *o)
{
    float v0[4], v3[4], v4[4], r1[4];
    const float *c0 = nv2a_vsh_constant(0), *c1 = nv2a_vsh_constant(1);
    int k, t;
    vsh_in(0, index, v0); vsh_in(3, index, v3); vsh_in(4, index, v4);
    vsh_defaults(o);
    r1[0] = v0[0]; r1[1] = v0[1]; r1[2] = v0[2]; r1[3] = vsh_rcp(v0[3]);
    vsh_color(o->d0, v3);
    o->d0_written = 1;
    for (k = 0; k < 4; k++) o->fog[k] = r1[3];
    vsh_color(o->d1, v4);
    for (k = 0; k < 4; k++) o->pos[k] = r1[k] * c0[k] + c1[k];
    for (t = 0; t < 4; t++) vsh_in(9 + (uint32_t)t, index, o->tex[t]);
}

/* 892CED0FDB0FB299 / E0318D6247FFFA7A (same words, start 0 or 23; morph):
 * R3 = v0 * c100.y + v2 * c100.x; oPos = R3 * c96..99; oD0 = v4; oT0 = v5;
 * oFog.x = (R3.c110) * c106.x + c106.y */
static void vsh_fast_892c(uint32_t index, Nv2aVshOutput *o)
{
    float v0[4], v2[4], v4[4], r3[4], r4z;
    const float *c100 = nv2a_vsh_constant(100), *c106 = nv2a_vsh_constant(106);
    int k;
    vsh_in(0, index, v0); vsh_in(2, index, v2); vsh_in(4, index, v4);
    vsh_defaults(o);
    for (k = 0; k < 4; k++) r3[k] = v0[k] * c100[1] + v2[k] * c100[0];
    vsh_color(o->d0, v4);
    o->d0_written = 1;
    r4z = vsh_dp4(r3, nv2a_vsh_constant(110));
    vsh_in(5, index, o->tex[0]);
    for (k = 0; k < 4; k++) o->pos[k] = vsh_dp4(r3, nv2a_vsh_constant(96 + k));
    o->fog[0] = r4z * c106[0] + c106[1];
    vsh_epilogue(o);
}

typedef void (*VshFastFn)(uint32_t, Nv2aVshOutput *);
static struct { uint64_t hash; VshFastFn fn; volatile LONG off, ok; } s_vsh_fast[] = {
    { 0x0B25A84A2631EC80ULL, vsh_fast_0b25, 0, 0 },
    { 0x9CEE4A21EBB77FF1ULL, vsh_fast_9cee, 0, 0 },
    { 0xBA9E387C24FCECE1ULL, vsh_fast_ba9e, 0, 0 },
    { 0x892CED0FDB0FB299ULL, vsh_fast_892c, 0, 0 },
    { 0xE0318D6247FFFA7AULL, vsh_fast_892c, 0, 0 },
};

/* The fast transform for the batch's program, or -1 when there is none. */
static int vsh_fast_lookup(void)
{
    static int on = -1;
    int i;
    if (on < 0) {
        const char *e = getenv("RECOMP_VSH_FAST");
        on = !(e && e[0] == '0') && !getenv("RECOMP_VSH_DUMP") && !getenv("RECOMP_VSH_VERIFY")
             && !getenv("RECOMP_VSH_NODECODE");
    }
    if (!on || s_rbx_cxt_write)
        return -1;
    for (i = 0; i < (int)(sizeof s_vsh_fast / sizeof s_vsh_fast[0]); i++)
        if (s_vsh_fast[i].hash == s_vp_hash && !s_vsh_fast[i].off)
            return i;
    return -1;
}

static int vsh_close(float a, float b)
{
    float d = fabsf(a - b), m = fabsf(a) > fabsf(b) ? fabsf(a) : fabsf(b);
    return d <= 1e-4f + m * 1e-4f || (a != a && b != b);
}

/* RECOMP_VSH_FASTCHECK: compare a fast result with the interpreter's. */
static void vsh_fast_check(int fi, const float in[NV2A_VSH_INPUTS][4], const Nv2aVshOutput *f)
{
    Nv2aVshOutput r;
    const float *a = (const float *)f, *b = (const float *)&r;
    size_t k, n = offsetof(Nv2aVshOutput, d0_written) / sizeof(float);
    static volatile LONG logged;
    if (!nv2a_vsh_run(in, &r))
        return;
    for (k = 0; k < n; k++)
        if (!vsh_close(a[k], b[k]))
            break;
    if (k == n && f->d0_written == r.d0_written) {
        if (InterlockedIncrement(&s_vsh_fast[fi].ok) == 1000)
            fprintf(stderr, "[VSH] fast path %016llX matched the interpreter on 1000 checked vertices\n",
                    (unsigned long long)s_vsh_fast[fi].hash);
        return;
    }
    InterlockedExchange(&s_vsh_fast[fi].off, 1);
    if (InterlockedIncrement(&logged) <= 20)
        fprintf(stderr, "[VSH] fast path %016llX MISMATCH at float %u: fast %g interp %g (pos %g %g %g %g vs %g %g %g %g); turned off\n",
                (unsigned long long)s_vsh_fast[fi].hash, (unsigned)k, k < n ? a[k] : 0.0f, k < n ? b[k] : 0.0f,
                f->pos[0], f->pos[1], f->pos[2], f->pos[3], r.pos[0], r.pos[1], r.pos[2], r.pos[3]);
}

static int transform_vertex(uint32_t index, Nv2aVshOutput *out)
{
    float in[NV2A_VSH_INPUTS][4];
    uint32_t a;

    if (!batch_uses_program())
        return ffp_vertex(index, out);
    {
        int fi = vsh_fast_lookup();
        if (fi >= 0) {
            static int every = -1;
            static volatile LONG tick;
            if (every < 0) { const char *e = getenv("RECOMP_VSH_FASTCHECK"); every = e ? atoi(e) : 0; }
            s_vsh_fast[fi].fn(index, out);
            if (every > 0 && InterlockedIncrement(&tick) % every == 0) {
                for (a = 0; a < NV2A_VSH_INPUTS; a++) {
                    if (!s_gpu.attr[a].size && a < NV_VERTEX_ATTRS)
                        memcpy(in[a], s_gpu.imm_attr[a], sizeof in[a]);
                    else
                        fetch_attr(&s_gpu.attr[a], index, in[a]);
                }
                vsh_fast_check(fi, (const float (*)[4])in, out);
            }
            goto post;
        }
    }
    for (a = 0; a < NV2A_VSH_INPUTS; a++) {
        /* An attribute with no array (size 0) reads its current value, the
         * one the SET_VERTEX_DATA* methods left -- as the hardware does --
         * not a fixed 0,0,0,1. SSX 3's sprite program passes its 4x4
         * transforms in v8..v15 this way, set per batch with
         * SET_VERTEX_DATA4F; reading 0,0,0,1 there sent every vertex of
         * its front end to the viewport offset. imm_attr starts at 0,0,0,1,
         * so an attribute nobody set still reads the old default. */
        if (!s_gpu.attr[a].size && a < NV_VERTEX_ATTRS)
            memcpy(in[a], s_gpu.imm_attr[a], sizeof in[a]);
        else
            fetch_attr(&s_gpu.attr[a], index, in[a]);   /* absent: 0,0,0,1 */
    }
    if (!nv2a_vsh_run((const float (*)[4])in, out))
        return 0;
post:
    /* oPos.w == 0 comes from a program that never divided by w: x, y and z
     * are already surface units. The hardware draws such a vertex (xemu
     * clamps w away from zero, in front of the eye), but raster_xf_clipped
     * would count it behind the eye. w = 1 draws it at the same place and
     * keeps 1/w finite. -0 stays behind the eye and is clipped, as on xemu. */
    if (out->pos[3] == 0.0f && !signbit(out->pos[3]))
        out->pos[3] = 1.0f;
    /* Programs that never write oD0 leave diffuse at 0. The rasteriser then
     * modulates every texel by black, so a correctly transformed mission
     * (Robotech after briefing) presents as a near-black frame while
     * triangle/pixel counters climb. Default to white only when oD0.rgb was
     * never written — not when the program wrote black (cel outlines).
     * RECOMP_VSH_DEFAULT_DIFFUSE=0 keeps the hardware zero. */
    {
        static int def_d0 = -1;
        if (def_d0 < 0) {
            const char *e = getenv("RECOMP_VSH_DEFAULT_DIFFUSE");
            def_d0 = !(e && e[0] == '0');
        }
        if (def_d0 && !out->d0_written) {
            out->d0[0] = out->d0[1] = out->d0[2] = 1.0f;
            if (out->d0[3] == 0.0f)
                out->d0[3] = 1.0f;
        }
    }
    /* The program's viewport is in logical pixels; the surface may be larger
     * (anti-aliasing). */
    out->pos[0] *= s_gpu.aa_sx;
    out->pos[1] *= s_gpu.aa_sy;
    return 1;
}

/* Parallel vertex transform (RBX). transform_vertex on a program batch reads
 * only batch-constant state and guest memory, and the decoded interpreter is
 * reentrant -- unless the program writes constants (CXT_WRITE), or a debug
 * mode (dump / verify / reference interpreter) is on, which stay serial.
 * Fixed-function batches (ffp_vertex) only read batch state, so they go too.
 * RECOMP_XF_THREADS=n workers (default: one on hosts with 4+ logical CPUs,
 * max 7; 0 = off). */
static uint32_t s_rbx_cxt_write;
#if defined(_WIN32)
#define XP_MAXW 7
#define XP_CHUNK 16
static int s_xp_nw = -1;
static HANDLE s_xp_go[XP_MAXW];
/* Claim word: batch generation in the high half, next index in the low half,
 * so a worker that wakes late can never claim work of a later batch. */
static volatile LONG64 s_xp_claim;
static volatile LONG s_xp_gen, s_xp_pending, s_xp_fail;
static volatile LONG s_xp_sleeping[XP_MAXW];
static LONG s_xp_n;
static uint32_t *s_xp_list;

static void xp_run(LONG gen)
{
    for (;;) {
        LONG64 cur = s_xp_claim;
        LONG k = (LONG)(cur & 0xFFFFFFFF), e, j;
        if ((LONG)(cur >> 32) != gen || k >= s_xp_n)
            return;
        if (InterlockedCompareExchange64(&s_xp_claim, cur + XP_CHUNK, cur) != cur)
            continue;
        e = k + XP_CHUNK < s_xp_n ? k + XP_CHUNK : s_xp_n;
        for (j = k; j < e; j++) {
            uint32_t at = s_xp_list[j];
            if (!transform_vertex(s_gpu.idx[at], &s_xf[at]))
                s_xp_fail = 1;
        }
        InterlockedExchangeAdd(&s_xp_pending, -(e - k));
    }
}

/* Workers spin briefly between batches (a batch is tens of microseconds;
 * an event wake-up costs about as much), then sleep on their event. The
 * 1 ms timeout covers a SetEvent racing the sleeping flag.
 * The spin is bounded in time, not in pause instructions: 40000 pauses was
 * 0.5-5 ms depending on the CPU, so workers rarely slept and each cost
 * ~60% of a core (bench). RECOMP_XF_SPIN_US sets it (default 200 us,
 * which covers the gap between batches inside a frame). */
static LONGLONG s_xp_spin_ticks;
static DWORD WINAPI xp_worker(LPVOID arg)
{
    LONG idx = (LONG)(size_t)arg, my = 0;
    HANDLE go = s_xp_go[idx];
    for (;;) {
        unsigned spins = 0;
        LARGE_INTEGER t0, t1;
        QueryPerformanceCounter(&t0);
        while (s_xp_gen == my) {
            if ((++spins & 255) != 0) { YieldProcessor(); continue; }
            QueryPerformanceCounter(&t1);
            if (t1.QuadPart - t0.QuadPart < s_xp_spin_ticks) continue;
            s_xp_sleeping[idx] = 1;
            MemoryBarrier();
            if (s_xp_gen == my)
                WaitForSingleObject(go, 1);
            s_xp_sleeping[idx] = 0;
            spins = 0;
            QueryPerformanceCounter(&t0);
        }
        my = s_xp_gen;
        xp_run(my);
    }
}

static int xp_workers(void)
{
    if (s_xp_nw < 0) {
        const char *e = getenv("RECOMP_XF_THREADS");
        SYSTEM_INFO si;
        int n, i;
        GetSystemInfo(&si);
        /* One worker on 4+ logical CPUs. Four workers measured no faster on
         * the bench (6800H): transform time fell but clip time on the
         * pushbuffer thread rose by as much, and they cost ~1.9 more cores. */
        n = e ? atoi(e) : si.dwNumberOfProcessors >= 4;
        {
            const char *sp = getenv("RECOMP_XF_SPIN_US");
            LARGE_INTEGER f;
            QueryPerformanceFrequency(&f);
            s_xp_spin_ticks = f.QuadPart * (sp ? atoi(sp) : 200) / 1000000;
        }
        if (getenv("RECOMP_VSH_DUMP") || getenv("RECOMP_VSH_VERIFY") || getenv("RECOMP_VSH_NODECODE"))
            n = 0;
        if (n > XP_MAXW) n = XP_MAXW;
        if (n < 0) n = 0;
        for (i = 0; i < n; i++) {
            HANDLE t;
            s_xp_go[i] = CreateEventA(NULL, FALSE, FALSE, NULL);
            t = CreateThread(NULL, 0, xp_worker, (LPVOID)(size_t)i, 0, NULL);
            if (!t) { n = i; break; }
            CloseHandle(t);
        }
        s_xp_nw = n;
        fprintf(stderr, "[RBX XF] %d vertex-transform worker thread(s)\n", n);
    }
    return s_xp_nw;
}

/* Transform list[0..n) in parallel; returns 0 if any vertex failed. */
static int xp_transform(uint32_t *list, uint32_t n)
{
    int w = xp_workers(), i;
    LONG gen = s_xp_gen + 1;
    if (w > (int)(n / XP_CHUNK)) w = (int)(n / XP_CHUNK);
    if (w == 0) {
        for (uint32_t j = 0; j < n; j++) {
            uint32_t at = list[j];
            if (!transform_vertex(s_gpu.idx[at], &s_xf[at]))
                return 0;
        }
        return 1;
    }

    /* Close the previous claim before changing its batch size and list.
     * A worker can still be in xp_run after its final pending decrement. */
    InterlockedExchange64(&s_xp_claim, ((LONG64)s_xp_gen << 32) | NV_MAX_INDICES);
    s_xp_list = list; s_xp_n = (LONG)n; s_xp_fail = 0;
    s_xp_pending = (LONG)n;
    InterlockedExchange64(&s_xp_claim, (LONG64)gen << 32);
    InterlockedExchange(&s_xp_gen, gen);
    for (i = 0; i < w; i++)
        if (s_xp_sleeping[i])
            SetEvent(s_xp_go[i]);
    xp_run(gen);
    while (s_xp_pending)
        YieldProcessor();
    return !s_xp_fail;
}
#endif

/* One triangle for the GPU to clip and cull (RECOMP_GPU_CLIP=1). */
static void gclip_tri(const Nv2aVshOutput *a, const Nv2aVshOutput *b,
                      const Nv2aVshOutput *c)
{
    static unsigned pace;
    uint32_t ia, ib, ic;

    if ((++pace & 0x3Fu) == 0)
        swm5_pace_wake();
    if (s_bidx_n + 3 > NV_BACKEND_MAX_VERTS)
        return;
    /* Cull here when the whole triangle is in front of the eye (the screen-space
     * winding is then exact), so the vertices of back faces are not converted
     * and handed over at all; that conversion was the new hot spot in menus.
     * Triangles reaching behind the w plane go to the GPU, which culls them
     * after clipping. RECOMP_GPU_CLIP_CPUCULL=0 leaves all culling to the GPU. */
    if (a->pos[3] > NV_CLIP_W && b->pos[3] > NV_CLIP_W && c->pos[3] > NV_CLIP_W) {
        static int cpucull = -1;
        if (cpucull < 0) {
            const char *e = getenv("RECOMP_GPU_CLIP_CPUCULL");
            cpucull = !(e && e[0] == '0');
        }
        if (cpucull && s_reg[0x0308 / 4] && xf_cull_on()) {      /* SET_CULL_FACE_ENABLE */
            const float *pa = a->pos, *pb = b->pos, *pc = c->pos;
            uint32_t cf = s_reg[0x039C / 4], ff = s_reg[0x03A0 / 4];
            float ar = (pb[0] - pa[0]) * (pc[1] - pa[1]) - (pb[1] - pa[1]) * (pc[0] - pa[0]);
            float sa = s_xf_odd ? -ar : ar;
            int front = ff == 0x901 ? sa < 0.0f : sa > 0.0f;
            if (ar == 0.0f || (cf == 0x405 && !front) || (cf == 0x404 && front) || cf == 0x408)
                return;
        }
    }
    ia = bidx_vertex(a); ib = bidx_vertex(b); ic = bidx_vertex(c);
    if (ia == ~0u || ib == ~0u || ic == ~0u)
        return;
    if (s_xf_odd) { uint32_t t = ib; ib = ic; ic = t; }
    s_bidx[s_bidx_n] = ia; s_bidx[s_bidx_n + 1] = ib; s_bidx[s_bidx_n + 2] = ic;
    s_bidx_n += 3;
}
#define XF_TRI(a, b, c) (s_gclip ? gclip_tri((a), (b), (c)) : raster_xf_clipped((a), (b), (c)))

static void xf_set_viewport(void)
{
    const float *of = nv2a_vsh_constant(59);

    static int trace_viewport = -1;
    static float previous[4];
    if (trace_viewport < 0)
        trace_viewport = getenv("RECOMP_RBX_CLEAR_TRACE") != NULL;
    if (trace_viewport && memcmp(previous, of, sizeof previous) != 0) {
        fprintf(stderr, "[RBX VIEWPORT] offset=(%g,%g,%g,%g) mode=%u\n",
                of[0], of[1], of[2], of[3], s_gpu.xf_mode & 3u);
        memcpy(previous, of, sizeof previous);
    }

    s_xf_o[0] = of[0] * s_gpu.aa_sx;
    s_xf_o[1] = of[1] * s_gpu.aa_sy;
    s_xf_o[2] = of[2];
}

/* RBX: GPU vertex programs (RECOMP_GPU_VSH=1; patches/rbx_gvs.h). A program
 * batch whose program has a fast path above, drawn through the combiners, is
 * not transformed here: each unique vertex's raw inputs are gathered, the
 * primitive becomes triangle indices (odd strip triangles flipped, as with
 * GPU clipping), and the back end's vertex shader for the program transforms,
 * clips and culls. The black-V0 skip and the combiner's stage-1 check need
 * per-batch facts about the outputs; they are worked out from the inputs.
 * RECOMP_GPU_VSH_VERIFY=N: every Nth such batch also runs the CPU path on its
 * first vertices, and the back end compares what its shader wrote. */
RbxGvs g_rbx_gvs;
static const uint8_t s_gvs_attrs[RBX_GVS_NPROG][RBX_GVS_MAXIN + 1] = {
    { 0 },
    { 3, 0, 1, 2 },
    { 2, 0, 1 },
    { 7, 0, 3, 4, 9, 10, 11, 12 },
    { 4, 0, 2, 4, 5 },
};
static int gvs_on(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RECOMP_GPU_VSH");
        on = e && e[0] == '1';
        if (on)
            fprintf(stderr, "[RBX XF] GPU vertex programs on (RECOMP_GPU_VSH=1)\n");
    }
    return on;
}
static int gvs_prog(void)
{
    int fi = vsh_fast_lookup();
    if (fi < 0)
        return RBX_GVS_OFF;
    switch (s_vsh_fast[fi].hash) {
    case 0x0B25A84A2631EC80ULL: return RBX_GVS_0B25;
    case 0x9CEE4A21EBB77FF1ULL: return RBX_GVS_9CEE;
    case 0xBA9E387C24FCECE1ULL: return RBX_GVS_BA9E;
    case 0x892CED0FDB0FB299ULL:
    case 0xE0318D6247FFFA7AULL: return RBX_GVS_892C;
    default: return RBX_GVS_OFF;
    }
}
static int gvs_nonzero3(const float *t) { return fabsf(t[0]) + fabsf(t[1]) + fabsf(t[2]) > 1e-5f; }

/* The CPU path's result for one vertex, as the back end would receive it. */
static void gvs_ref(uint32_t index, RbxGvsRef *r)
{
    Nv2aVshOutput o;
    Nv2aVertex v;
    uint32_t d;
    memset(r, 0, sizeof *r);
    if (!transform_vertex(index, &o))
        return;
    backend_from_xf(&o, &v);
    memcpy(r->pos, o.pos, sizeof r->pos);
    d = v.diffuse;
    r->c[0] = ((d >> 16) & 255u) / 255.0f; r->c[1] = ((d >> 8) & 255u) / 255.0f;
    r->c[2] = (d & 255u) / 255.0f;         r->c[3] = ((d >> 24) & 255u) / 255.0f;
    memcpy(r->d1, v.d1, sizeof r->d1);
    memcpy(r->t, v.tex, sizeof r->t);
    if (fabsf(r->t[1][0]) + fabsf(r->t[1][1]) + fabsf(r->t[1][2]) < 1e-6f
        && fabsf(r->t[0][0]) + fabsf(r->t[0][1]) + fabsf(r->t[0][2]) > 1e-6f)
        memcpy(r->t[1], r->t[0], sizeof r->t[1]);
    r->fog = v.fog;
}

/* RBX: per-program counts of batches that took the GPU path, and of black passes
 * skipped whole (below); logged every 16384 such batches. */
static unsigned long s_gvs_taken[RBX_GVS_NPROG], s_black_skipped[RBX_GVS_NPROG];
static void gvs_count(unsigned long *tab, int prog)
{
    static unsigned long n;
    tab[prog]++;
    if (++n % 16384 == 0)
        fprintf(stderr, "[GVS] batches GPU/black-skipped: 0B25 %lu/%lu 9CEE %lu/%lu BA9E %lu/%lu 892C %lu/%lu\n",
                s_gvs_taken[1], s_black_skipped[1], s_gvs_taken[2], s_black_skipped[2],
                s_gvs_taken[3], s_black_skipped[3], s_gvs_taken[4], s_black_skipped[4]);
}

/* RBX: the outline programs (0B25 / 9CEE) take oD0 from c[100], and the cel
 * outline and darkening passes load it black. The back end drops every
 * combiner batch whose diffuse is all black (RECOMP_GPU_SKIPBLACK, on by
 * default), so such a batch was transformed, clipped and converted only to be
 * dropped -- on the bench about half of all vertex work. Decide it here from
 * c[100] and skip the batch whole, on the CPU and GPU paths alike.
 * RECOMP_GPU_SKIPBLACK=0 keeps drawing them, as before. */
static int black_pass_skip(uint32_t n)
{
    static int skipblk = -1;
    int prog;
    float d0[4];
    if (skipblk < 0) {
        const char *e = getenv("RECOMP_GPU_SKIPBLACK");
        skipblk = !(e && e[0] == '0');
    }
    if (!skipblk || n < 3 || !s_gpu.rc_seen || !backend_draws()
        || s_gpu.prim < NV_PRIM_TRIANGLES || s_gpu.prim > NV_PRIM_POLYGON)
        return 0;
    prog = gvs_prog();
    if (prog != RBX_GVS_0B25 && prog != RBX_GVS_9CEE)
        return 0;
    vsh_color(d0, nv2a_vsh_constant(100));
    if (pack_color(d0) & 0x00FFFFFFu)
        return 0;
    gvs_count(s_black_skipped, prog);
    s_gpu.batches_program++;
    s_gpu.verts_program += n;
    note_drawn();
    return 1;
}

/* Draw the current batch through a GPU vertex program; 0 = not this batch. */
static int gvs_batch(uint32_t n)
{
    static float *in;
    static uint32_t *map;
    static int verify = -1;
    static unsigned vtick;
    uint32_t tag[256], slot[256], nu = 0, i, k, nin;
    const uint8_t *at;
    RbxGvs *g = &g_rbx_gvs;
    int prog, t0z = 1, t1z = 1, black = 1;
    float nc101[4];
    unsigned long long t0;

    if (!gvs_on() || n < 3 || s_gpu.prim < NV_PRIM_TRIANGLES || s_gpu.prim > NV_PRIM_POLYGON
        || !s_gpu.rc_seen || !backend_draws() || !rbx_gpu_vsh_ok())
        return 0;
    if ((prog = gvs_prog()) == RBX_GVS_OFF)
        return 0;
    {
        uint32_t bpp = surface_bpp();
        if (bpp != 4 && bpp != 2)
            return 0;
    }
    if (!in && !(in = (float *)malloc((size_t)NV_MAX_INDICES * RBX_GVS_MAXIN * 4 * sizeof *in)))
        return 0;
    if (!map && !(map = (uint32_t *)malloc(NV_MAX_INDICES * sizeof *map)))
        return 0;
    bidx_begin();
    if (!s_bidx_mode)
        return 0;
    t0 = pb_ticks();
    at = s_gvs_attrs[prog];
    nin = at[0];
    for (k = 0; k < 4; k++)
        nc101[k] = -nv2a_vsh_constant(101)[k];
    memset(tag, 0xFF, sizeof tag);
    for (i = 0; i < n; i++) {
        uint32_t ix = s_gpu.idx[i], h = (ix * 2654435761u) >> 24;
        float *v;
        if (tag[h] == ix) { map[i] = slot[h]; continue; }
        v = in + (size_t)nu * nin * 4;
        for (k = 0; k < nin; k++)
            vsh_in(at[1 + k], ix, v + 4 * k);
        /* What the back end needs to know about the outputs (see the shader). */
        switch (prog) {
        case RBX_GVS_0B25:
            if (t0z && gvs_nonzero3(v + 8)) t0z = 0;
            if (t1z) {
                float t1[3] = { vsh_dp3(v + 4, nc101), nv2a_vsh_constant(102)[3], 0.0f };
                if (gvs_nonzero3(t1)) t1z = 0;
            }
            break;
        case RBX_GVS_BA9E:
            if (black && (pack_color(v + 4) & 0x00FFFFFFu)) black = 0;
            if (t0z && gvs_nonzero3(v + 12)) t0z = 0;
            if (t1z && gvs_nonzero3(v + 16)) t1z = 0;
            break;
        case RBX_GVS_892C:
            if (black && (pack_color(v + 8) & 0x00FFFFFFu)) black = 0;
            if (t0z && gvs_nonzero3(v + 12)) t0z = 0;
            break;
        default:
            break;
        }
        tag[h] = ix; slot[h] = nu; map[i] = nu; nu++;
    }
    if (prog == RBX_GVS_0B25 || prog == RBX_GVS_9CEE) {
        float d0[4];
        vsh_color(d0, nv2a_vsh_constant(100));
        black = !(pack_color(d0) & 0x00FFFFFFu);
    }
#define GVS_TRI(a, b, c) do { if (s_bidx_n + 3 <= NV_BACKEND_MAX_VERTS) { \
        s_bidx[s_bidx_n] = map[a]; s_bidx[s_bidx_n + 1] = map[b]; s_bidx[s_bidx_n + 2] = map[c]; s_bidx_n += 3; } } while (0)
    switch (s_gpu.prim) {
    case NV_PRIM_TRIANGLES:
        for (i = 0; i + 2 < n; i += 3) GVS_TRI(i, i + 1, i + 2);
        break;
    case NV_PRIM_TRIANGLE_STRIP:
        for (i = 0; i + 2 < n; i++) {
            if (i & 1) GVS_TRI(i, i + 2, i + 1);
            else GVS_TRI(i, i + 1, i + 2);
        }
        break;
    case NV_PRIM_TRIANGLE_FAN:
    case NV_PRIM_POLYGON:
        for (i = 1; i + 1 < n; i++) GVS_TRI(0, i, i + 1);
        break;
    case NV_PRIM_QUADS:
        for (i = 0; i + 3 < n; i += 4) { GVS_TRI(i, i + 1, i + 2); GVS_TRI(i, i + 2, i + 3); }
        break;
    case NV_PRIM_QUAD_STRIP:
        for (i = 0; i + 3 < n; i += 2) { GVS_TRI(i, i + 1, i + 3); GVS_TRI(i, i + 3, i + 2); }
        break;
    default:
        break;
    }
#undef GVS_TRI
    g->prog = prog;
    g->nin = nin;
    g->in = in;
    g->black = black;
    g->t0_zero = t0z;
    g->t1_zero = t1z;
    g->nref = 0;
    memcpy(g->k.c0, nv2a_vsh_constant(0), sizeof g->k.c0);
    memcpy(g->k.c1, nv2a_vsh_constant(1), sizeof g->k.c1);
    memcpy(g->k.c58, nv2a_vsh_constant(58), sizeof g->k.c58);
    memcpy(g->k.c59, nv2a_vsh_constant(59), sizeof g->k.c59);
    for (k = 0; k < 15; k++)
        memcpy(g->k.cu[k], nv2a_vsh_constant(96 + k), sizeof g->k.cu[k]);
    g->k.aa[0] = s_gpu.aa_sx; g->k.aa[1] = s_gpu.aa_sy; g->k.aa[2] = g->k.aa[3] = 0.0f;
    g->k.fogp[0] = s_gpu.fog_param[0]; g->k.fogp[1] = s_gpu.fog_param[1];
    g->k.fogp[2] = g->k.fogp[3] = 0.0f;
    g->k.fogm[0] = s_gpu.fog_mode; g->k.fogm[1] = s_gpu.fog_enable ? 1u : 0u;
    g->k.fogm[2] = g->k.fogm[3] = 0;
    if (verify < 0) {
        const char *e = getenv("RECOMP_GPU_VSH_VERIFY");
        verify = e ? atoi(e) : 0;
    }
    if (verify > 0 && ++vtick % (unsigned)verify == 0) {
        /* The unique vertices in gather order: the first one to land in each output slot. */
        uint32_t want = nu < RBX_GVS_VERIFY_N ? nu : RBX_GVS_VERIFY_N;
        for (i = 0; i < n && g->nref < want; i++)
            if (map[i] == g->nref)
                gvs_ref(s_gpu.idx[i], &g->ref[g->nref++]);
    }
    g_pb_t_xf += pb_ticks() - t0;
    g_pb_n_xfverts += n;
    gvs_count(s_gvs_taken, prog);
    s_gpu.batches_program++;
    s_gpu.verts_program += n;
    if (s_gpu.texs[0].valid)
        note_texture_use();
    s_bcount = nu;
    g_rbx_precull = 1;
    g_rbx_gpuclip = clip_near_z() ? 3 : 1;
    g_rbx_clip_z = nv2a_vsh_constant(59)[2];
    t0 = pb_ticks();
    backend_flush(1);
    g_pb_t_flush += pb_ticks() - t0;
    g_rbx_precull = 0;
    g_rbx_gpuclip = 0;
    g->prog = RBX_GVS_OFF;
    return 1;
}

static void raster_batch_program(void)
{
    /* Keep each cached transform in its original slot. Assemble primitives
     * through the index map instead of expanding duplicate vertex payloads. */
    static uint32_t vertex_slot[NV_MAX_INDICES];
    uint32_t i, n = s_gpu.idx_count;
    unsigned long long t0, t1;

    if (!s_xf && !(s_xf = (Nv2aVshOutput *)calloc(NV_MAX_INDICES, sizeof *s_xf)))
        return;
    if (batch_uses_program()) {
        (void)vp_hash();          /* settle the program identity before workers read it */
        if (black_pass_skip(n) || gvs_batch(n))
            return;
    }
    t0 = pb_ticks();
    {   /* Post-transform cache, as the hardware has: an indexed batch names
         * most vertices several times, and the state is fixed for the batch. */
        enum { VC = 256 };
        uint32_t tag[VC], slot[VC];
        /* RECOMP_XF_STATS: where vertex-program work goes (serial vs pool). */
        static int xst = -1;
        static unsigned long long st_b, st_pb, st_sb, st_su, st_cx, st_ffp, st_small;
        if (xst < 0) xst = getenv("RECOMP_XF_STATS") != NULL;
        if (xst) {
            st_b++;
            if (!batch_uses_program()) st_ffp++;
            else if (s_rbx_cxt_write) st_cx++;
            else if (n < 48) st_small++;
            if ((st_b & 0xFFFF) == 0)
                fprintf(stderr, "[XF STATS] batches %llu par %llu ser %llu (ser uniq verts %llu)"
                                " ffp %llu cxt %llu small %llu\n", st_b, st_pb, st_sb, st_su,
                        st_ffp, st_cx, st_small);
        }
        memset(tag, 0xFF, sizeof tag);
#if defined(_WIN32)
        if (n >= 48 && !(batch_uses_program() && s_rbx_cxt_write) && xp_workers() > 0) {
            static uint32_t *uniq;
            uint32_t nu = 0;
            if (!uniq) uniq = (uint32_t *)malloc(NV_MAX_INDICES * sizeof *uniq);
            if (uniq) {
                for (i = 0; i < n; i++) {
                    uint32_t ix = s_gpu.idx[i], h = (ix * 2654435761u) >> 24;
                    if (tag[h] == ix) { vertex_slot[i] = slot[h]; continue; }
                    tag[h] = ix; slot[h] = i; vertex_slot[i] = i; uniq[nu++] = i;
                }
                /* The first vertex runs here: it decodes the program and settles
                 * every lazily-initialised static before the workers start. */
                if (!transform_vertex(s_gpu.idx[uniq[0]], &s_xf[uniq[0]])
                    || !xp_transform(uniq + 1, nu - 1)) {
                    g_dbg_xffail++;
                    g_pb_t_xf += pb_ticks() - t0;
                    return;
                }
                if (xst) st_pb++;
                goto xf_done;
            }
        }
#endif
        for (i = 0; i < n; i++) {
            uint32_t ix = s_gpu.idx[i], h = (ix * 2654435761u) >> 24;
            if (tag[h] == ix) { vertex_slot[i] = slot[h]; continue; }
            if (!transform_vertex(ix, &s_xf[i])) { g_dbg_xffail++;
                g_pb_t_xf += pb_ticks() - t0;
                return; }                                /* no program loaded */
            tag[h] = ix; slot[h] = i; vertex_slot[i] = i;
            if (xst) st_su++;
        }
        if (xst) st_sb++;
    }
xf_done:
    t1 = pb_ticks();
    g_pb_t_xf += t1 - t0;
    g_pb_n_xfverts += n;
    xf_set_viewport();
    if (tex_size_from_format(s_gpu.texs[0].color)) {
        s_bx_su = (float)s_gpu.texs[0].width;
        s_bx_sv = (float)s_gpu.texs[0].height;
    } else {
        s_bx_su = s_bx_sv = 1.0f;
    }
    {
        const VertexAttr *tc = texcoord_attr();
        s_xf_no_tex = !batch_uses_program() && !(tc->offset && tc->stride);
    }
    if (batch_uses_program()) {
        s_gpu.batches_program++;
        s_gpu.verts_program += n;
    } else {
        static int ftrace = -1;

        s_gpu.batches_ffp++;
        /* RECOMP_FFP_TRACE: the matrix and the first vertices of the first few
         * fixed-function batches, raw and transformed -- the one view that
         * tells a wrong matrix layout from wrong vertex data. */
        if (ftrace < 0)
            ftrace = getenv("RECOMP_FFP_TRACE") != NULL;
        if (ftrace && s_gpu.batches_ffp <= 4) {
            const float *m = s_gpu.composite;
            float in[4];

            fprintf(stderr, "  [FFP] batch %u prim %u n %u mode 0x%X"
                            " clip %ux%u+%u+%u aa %gx%g\n"
                            "  [FFP]   M = %g %g %g %g | %g %g %g %g |"
                            " %g %g %g %g | %g %g %g %g\n",
                    s_gpu.batches_ffp, s_gpu.prim, n, s_gpu.xf_mode,
                    s_gpu.clip_w, s_gpu.clip_h, s_gpu.clip_x, s_gpu.clip_y,
                    s_gpu.aa_sx, s_gpu.aa_sy,
                    m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7],
                    m[8], m[9], m[10], m[11], m[12], m[13], m[14], m[15]);
            for (i = 0; i < n && i < 4; i++) {
                const Nv2aVshOutput *vertex = &s_xf[vertex_slot[i]];
                fetch_attr(&s_gpu.attr[0], s_gpu.idx[i], in);
                fprintf(stderr, "  [FFP]   v%u (%g %g %g %g) -> (%g %g %g, w %g)\n",
                        s_gpu.idx[i], in[0], in[1], in[2], in[3],
                        vertex->pos[0], vertex->pos[1], vertex->pos[2],
                        vertex->pos[3]);
            }
        }
    }
    if (s_gpu.blend_enable) {
        uint32_t pair = s_gpu.blend_sfactor << 16 | (s_gpu.blend_dfactor & 0xFFFF);
        int j;
        for (j = 0; j < s_gpu.blend_npairs && s_gpu.blend_pairs[j] != pair; j++)
            ;
        if (j == s_gpu.blend_npairs && j < 16)
            s_gpu.blend_pairs[s_gpu.blend_npairs++] = pair;
    }
    {
        /* RECOMP_VSH_TRACE=<n>: print n program batches -- attribute setup,
         * raw inputs, key constants, and what came out. */
        static int left = -1;
        if (left < 0) {
            const char *t = getenv("RECOMP_VSH_TRACE");
            left = t ? atoi(t) : 0;
        }
        /* Spread out: one every 20000 program batches, so a run that spends
         * its first half in menus still traces the 3D scene. */
        if (left > 0 && n >= 3 && s_gpu.batches_program % 20000 == 0) {
            float in[4];
            left--;
            fprintf(stderr, "[VTRACE] prim %u n %u idx %u %u %u\n", s_gpu.prim,
                    n, s_gpu.idx[0], s_gpu.idx[1], s_gpu.idx[2]);
            for (i = 0; i < NV_VERTEX_ATTRS; i++) {
                const VertexAttr *at = &s_gpu.attr[i];
                if (!at->size)
                    continue;
                fetch_attr(at, s_gpu.idx[0], in);
                fprintf(stderr, "[VTRACE]   v%u off %08X type %u size %u"
                        " stride %u = %g %g %g %g\n", i, at->offset, at->type,
                        at->size, at->stride, in[0], in[1], in[2], in[3]);
            }
            {
                static const uint32_t cs[] = {58, 59, 96, 97, 112, 113, 114, 115};
                for (i = 0; i < 8; i++) {
                    const float *c = nv2a_vsh_constant(cs[i]);
                    fprintf(stderr, "[VTRACE]   c[%u] %g %g %g %g%c", cs[i],
                            c[0], c[1], c[2], c[3], 10);
                }
            }
            for (i = 0; i < 3; i++) {
                const Nv2aVshOutput *vertex = &s_xf[vertex_slot[i]];
                fprintf(stderr, "[VTRACE]   out%u pos %g %g %g %g"
                        " d0 %g %g %g %g d0w=%u d1 %g %g %g %g"
                        " t0 %g %g %g %g\n",
                        i,
                        vertex->pos[0], vertex->pos[1], vertex->pos[2],
                        vertex->pos[3],
                        vertex->d0[0], vertex->d0[1], vertex->d0[2],
                        vertex->d0[3], (unsigned)vertex->d0_written,
                        vertex->d1[0], vertex->d1[1], vertex->d1[2],
                        vertex->d1[3],
                        vertex->tex[0][0], vertex->tex[0][1],
                        vertex->tex[0][2], vertex->tex[0][3]);
            }
            fprintf(stderr, "[VTRACE]   attr3 size %u stride %u lit=%u"
                    " scene_amb %g %g %g\n",
                    s_gpu.attr[3].size, s_gpu.attr[3].stride,
                    s_reg[0x0314 / 4],
                    reg_f(0x0A10), reg_f(0x0A14), reg_f(0x0A18));
        }
    }
    for (i = 0; i < NV2A_VSH_INPUTS; i++) {
        const VertexAttr *at = &s_gpu.attr[i];
        uint32_t f = at->type | (at->size << 4) | (i << 8), j;
        if (!at->size || !at->stride)
            continue;
        for (j = 0; j < (uint32_t)s_gpu.xf_nfmt && s_gpu.xf_fmt[j] != f; j++)
            ;
        if (j == (uint32_t)s_gpu.xf_nfmt && s_gpu.xf_nfmt < 16)
            s_gpu.xf_fmt[s_gpu.xf_nfmt++] = f;
    }
    if (s_gpu.texs[0].valid)
        note_texture_use();

    if (++s_bvc_gen == 0)          /* new batch: converted vertices are stale */
        s_bvc_gen = 1;
    s_gclip = 0;
    if (backend_draws()) {
        bidx_begin();
        if (s_bidx_mode && gclip_on() && rbx_gpu_clip_ok()) {
            uint32_t bpp = surface_bpp();
            s_gclip = bpp == 4 || bpp == 2;
        }
    }
    switch (s_gpu.prim) {
    case NV_PRIM_TRIANGLES:
        for (i = 0; i + 2 < n; i += 3)
            XF_TRI(&s_xf[vertex_slot[i]], &s_xf[vertex_slot[i+1]], &s_xf[vertex_slot[i+2]]);
        break;
    case NV_PRIM_TRIANGLE_STRIP:
        /* RBX-17: do NOT flip odd triangles here. That "DX11 cull" experiment
         * painted white boxes behind every menu glyph (UI strips). Host cull
         * for 3D strips belongs in the backend, not by rewriting NV2A order. */
        for (i = 0; i + 2 < n; i++) {
            s_xf_odd = (int)(i & 1);
            XF_TRI(&s_xf[vertex_slot[i]], &s_xf[vertex_slot[i+1]], &s_xf[vertex_slot[i+2]]);
        }
        s_xf_odd = 0;
        break;
    case NV_PRIM_TRIANGLE_FAN:
    case NV_PRIM_POLYGON:
        for (i = 1; i + 1 < n; i++)
            XF_TRI(&s_xf[vertex_slot[0]], &s_xf[vertex_slot[i]], &s_xf[vertex_slot[i+1]]);
        break;
    case NV_PRIM_QUADS:
        for (i = 0; i + 3 < n; i += 4) {
            XF_TRI(&s_xf[vertex_slot[i]], &s_xf[vertex_slot[i+1]], &s_xf[vertex_slot[i+2]]);
            XF_TRI(&s_xf[vertex_slot[i]], &s_xf[vertex_slot[i+2]], &s_xf[vertex_slot[i+3]]);
        }
        break;
    case NV_PRIM_QUAD_STRIP:
        for (i = 0; i + 3 < n; i += 2) {
            XF_TRI(&s_xf[vertex_slot[i]], &s_xf[vertex_slot[i+1]], &s_xf[vertex_slot[i+3]]);
            XF_TRI(&s_xf[vertex_slot[i]], &s_xf[vertex_slot[i+3]], &s_xf[vertex_slot[i+2]]);
        }
        break;
    default:
        break;
    }
    /* The batch's triangles, all at once (see xf_queue_push). */
    xf_queue_flush();
    t0 = pb_ticks();
    g_pb_t_clip += t0 - t1;
    if (backend_draws()) {
        g_rbx_precull = 1;
        g_rbx_gpuclip = s_gclip ? (clip_near_z() ? 3 : 1) : 0;
        g_rbx_clip_z = s_xf_o[2];
        backend_flush(!s_xf_no_tex);
        g_rbx_precull = 0;
        g_rbx_gpuclip = 0;
    }
    s_gclip = 0;
    g_pb_t_flush += pb_ticks() - t0;
}

static void raster_batch(void)
{
    uint32_t i;
    uint32_t before = s_gpu.tris_drawn;
    int textured = 0;

    if (s_gpu.idx_count < 3) { g_dbg_short++; return; }
    g_dbg_batches++;
    if (batch_uses_program() || batch_is_ffp()) {
        raster_batch_program();
        return;
    }
    if (!batch_is_screen_space()) {
        s_gpu.batches_untransformed++;
        return;
    }

    /* Count why, once per batch: the texture stage cannot change inside one. */
    {
        const VertexAttr *tc = texcoord_attr();

        if (!(tc->offset && tc->stride))
            s_gpu.batches_no_uv++;
        else if (!s_gpu.texs[0].valid)
            s_gpu.batches_no_tex++;
        else {
            s_gpu.batches_textured++;
            textured = 1;
            note_texture_use();
        }
    }

    switch (s_gpu.prim) {
    case NV_PRIM_TRIANGLES:
        for (i = 0; i + 2 < s_gpu.idx_count; i += 3)
            raster_indexed(s_gpu.idx[i], s_gpu.idx[i+1], s_gpu.idx[i+2],
                           vertex_color(s_gpu.idx[i]));
        break;
    case NV_PRIM_TRIANGLE_STRIP:
        /* See program-path strip: keep NV2A order (menu UI glyphs). */
        for (i = 0; i + 2 < s_gpu.idx_count; i++)
            raster_indexed(s_gpu.idx[i], s_gpu.idx[i+1], s_gpu.idx[i+2],
                           vertex_color(s_gpu.idx[i]));
        break;
    case NV_PRIM_TRIANGLE_FAN:
    case NV_PRIM_POLYGON:
        for (i = 1; i + 1 < s_gpu.idx_count; i++)
            raster_indexed(s_gpu.idx[0], s_gpu.idx[i], s_gpu.idx[i+1],
                           vertex_color(s_gpu.idx[0]));
        break;
    case NV_PRIM_QUADS:
        /* Independent quads, four vertices each. A batch of eight is two
         * quads, not one six-triangle fan around the first vertex; with
         * exactly four the two agreed, which is why sharing the fan arm
         * looked right. */
        for (i = 0; i + 3 < s_gpu.idx_count; i += 4) {
            raster_indexed(s_gpu.idx[i], s_gpu.idx[i+1], s_gpu.idx[i+2],
                           vertex_color(s_gpu.idx[i]));
            raster_indexed(s_gpu.idx[i], s_gpu.idx[i+2], s_gpu.idx[i+3],
                           vertex_color(s_gpu.idx[i]));
        }
        break;
    case NV_PRIM_QUAD_STRIP:
        /* Each vertex pair past the first closes another quad against the
         * pair before it. */
        for (i = 0; i + 3 < s_gpu.idx_count; i += 2) {
            raster_indexed(s_gpu.idx[i], s_gpu.idx[i+1], s_gpu.idx[i+3],
                           vertex_color(s_gpu.idx[i]));
            raster_indexed(s_gpu.idx[i], s_gpu.idx[i+3], s_gpu.idx[i+2],
                           vertex_color(s_gpu.idx[i]));
        }
        break;
    default:
        break;                             /* points and lines: not yet */
    }
    if (backend_draws())
        backend_flush(textured);

    if (s_gpu.tris_drawn && (s_gpu.tris_drawn % 500) == 0)
        fprintf(stderr, "  [GPU] %u triangles rasterised\n", s_gpu.tris_drawn);

    /* Capture the surface while the geometry is still on it.
     *
     * The periodic report dumps too, but a title clears every frame and draws
     * in only some of them, so a report almost always lands on a surface that
     * was wiped a moment ago -- which reads as "nothing was drawn" when the
     * triangles went down correctly just before it. A few frames that actually
     * contain geometry are worth more than any number of clears. */
    if (s_gpu.tris_drawn != before && s_drawn_dumps < FB_DUMP_AFTER_DRAW
        && !s_flip_dumping) {
        s_drawn_dumps++;
        dump_surface_bmp();
    }
}

/* What a batch actually contains. Before anything can be rasterised, the
 * question is what space attribute 0 arrives in: a title running a vertex
 * program hands over object-space positions that mean nothing without running
 * it, while pre-transformed screen-space coordinates can be drawn directly. */
/* RECOMP_FRAME_TRACE=<flag file>: once the file exists, log every batch of
 * the next whole frame (flip to flip) -- where it drew, with what texture,
 * blend and depth state, and how many pixels it actually wrote. "The frame
 * is black" has many causes and this is what tells them apart. */

static void frame_trace_flip(void)
{
    static const char *flag = (const char *)-1;
    if (flag == (const char *)-1)
        flag = getenv("RECOMP_FRAME_TRACE");
    if (!flag)
        return;
    if (s_ftrace == 3) {
        /* Deleting the flag file re-arms the trace for the next time it
         * appears, so one run can trace several moments. */
        FILE *f = fopen(flag, "rb");
        if (f)
            fclose(f);
        else
            s_ftrace = 0;
        return;
    }
    if (s_ftrace == 2) {
        s_ftrace = 3;
        fprintf(stderr, "[FTRACE] end of frame\n");
        return;
    }
    if (s_ftrace == 0) {
        FILE *f = fopen(flag, "rb");
        if (!f)
            return;
        fclose(f);
        s_ftrace = 2;
        fprintf(stderr, "[FTRACE] frame begins (flip %u)\n", s_gpu.flips);
    }
}

/* RBX-17: see nv2a_pb_scan.c — wake D3D BlockOnTime during long raster. */
static void swm5_pace_wake(void)
{
    static DWORD s_pace_last;
    DWORD now;
    int pace_ms;
    extern int xbox_Swm5PaceMs(void);
    pace_ms = xbox_Swm5PaceMs();
    if (pace_ms <= 0)
        return;
    now = GetTickCount();
    if (!s_pace_last)
        s_pace_last = now;
    else if (now - s_pace_last >= (DWORD)pace_ms) {
        extern void xbox_NV2ASoftwareMethodIdleWake(void);
        xbox_NV2ASoftwareMethodIdleWake();
        s_pace_last = now;
    }
}

static void draw_primitive(void)
{
    float v[4];
    uint32_t i;

    if (!s_gpu.prim || !s_gpu.idx_count) { g_dbg_dpn++; return; }
    g_dbg_dp++;
    s_gpu.draws++;
    swm5_pace_wake();
    {   /* Was unconditional: ~7,500 lines per 170 s fight run, the bulk of all stderr. */
        static int vdraws = -1;
        if (vdraws < 0)
            vdraws = getenv("RECOMP_PB_EXEC_VERBOSE") != NULL;
        if (vdraws && (s_gpu.draws % 200) == 0)
            fprintf(stderr, "  [GPU] draw #%u\n", s_gpu.draws);
    }
    s_gpu.verts += s_gpu.idx_count;

    /* How many batches carry coordinates at all, and what range they span.
     * A pipeline that decodes perfectly and draws nothing is indistinguishable
     * from one that never ran, unless the vertices themselves are measured. */
    {
        float p[4];
        if (fetch_attr(&s_gpu.attr[0], s_gpu.idx[0], p)) {
            if (p[0] != 0.0f || p[1] != 0.0f || p[2] != 0.0f) {
                s_gpu.nonzero_draws++;
                if (p[0] < s_gpu.min_x) s_gpu.min_x = p[0];
                if (p[0] > s_gpu.max_x) s_gpu.max_x = p[0];
                if (p[1] < s_gpu.min_y) s_gpu.min_y = p[1];
                if (p[1] > s_gpu.max_y) s_gpu.max_y = p[1];
            }
        }
    }

    {
        uint32_t t0 = s_gpu.tris_drawn;
        uint64_t p0 = s_gpu.pixels;
        unsigned long long tb = pb_ticks();
        raster_batch();
        g_pb_t_batch += pb_ticks() - tb;
        if (s_ftrace == 2) {
            fprintf(stderr, "[FTRACE] %s prim %u n %u surf %08X %ux%u+%u+%u"
                    " pitch %u | tex %s %08X %ux%u fmt %02X | blend %u %X/%X"
                    " eq %X | z test %u func %X mask %u | tris %u px %llu\n",
                    (s_gpu.xf_mode & 3) == 2 ? "PRG" : "FIX", s_gpu.prim,
                    s_gpu.idx_count, s_gpu.color_offset, s_gpu.clip_w,
                    s_gpu.clip_h, s_gpu.clip_x, s_gpu.clip_y, s_gpu.pitch,
                    s_gpu.texs[0].valid ? "on" : "off", s_gpu.texs[0].offset,
                    s_gpu.texs[0].width, s_gpu.texs[0].height, s_gpu.texs[0].color,
                    s_gpu.blend_enable, s_gpu.blend_sfactor,
                    s_gpu.blend_dfactor, s_gpu.blend_equation,
                    s_gpu.depth_test, s_gpu.depth_func, s_gpu.depth_mask,
                    s_gpu.tris_drawn - t0,
                    (unsigned long long)(s_gpu.pixels - p0));
            if ((s_gpu.xf_mode & 3) == 2 && s_xf)
                fprintf(stderr, "[FTRACE]     v0 %g %g %g %g  v1 %g %g  v2 %g %g%c",
                        s_xf[0].pos[0], s_xf[0].pos[1], s_xf[0].pos[2],
                        s_xf[0].pos[3], s_xf[1].pos[0], s_xf[1].pos[1],
                        s_xf[2].pos[0], s_xf[2].pos[1], 10);
            {   /* RECOMP_FRAME_TRACE_VERTS: every vertex's screen position and the
                 * texture coordinates the program wrote, for texture-mapping bugs. */
                static int verts = -1;
                if (verts < 0)
                    verts = getenv("RECOMP_FRAME_TRACE_VERTS") != NULL;
                if (verts && (s_gpu.xf_mode & 3) == 2 && s_xf) {
                    uint32_t k;
                    fprintf(stderr, "[FTRACE]     tex0 addr %X/%X\n",
                            s_gpu.texs[0].addr_u, s_gpu.texs[0].addr_v);
                    for (k = 0; k < s_gpu.idx_count && k < 96; k++)
                        fprintf(stderr, "[FTRACE]     vx%u %.1f %.1f w %g | t0 %g %g %g %g%c", k,
                                s_xf[k].pos[0], s_xf[k].pos[1], s_xf[k].pos[3],
                                s_xf[k].tex[0][0], s_xf[k].tex[0][1],
                                s_xf[k].tex[0][2], s_xf[k].tex[0][3], 10);
                }
            }
            if (s_gpu.rc_seen) {
                int st;
                fprintf(stderr, "[FTRACE]     rc stages %u ctl %08X prog %05X fin %08X %08X",
                        s_gpu.rc.control & 0xFF, s_gpu.rc.control,
                        s_gpu.rc.stage_program, s_gpu.rc.final0, s_gpu.rc.final1);
                for (st = 0; st < 4; st++)
                    if ((s_gpu.rc.stage_program >> (st * 5)) & 0x1F)
                        fprintf(stderr, " | t%d %08X %ux%u fmt %02X%s", st,
                                s_gpu.texs[st].offset, s_gpu.texs[st].width,
                                s_gpu.texs[st].height, s_gpu.texs[st].color,
                                s_gpu.texs[st].valid ? "" : " (invalid)");
                fputc(10, stderr);
                for (st = 0; st < (int)(s_gpu.rc.control & 0xFF) && st < 8; st++)
                    fprintf(stderr, "[FTRACE]       s%d icw %08X %08X ocw %08X %08X c %08X %08X%c",
                            st, s_gpu.rc.color_icw[st], s_gpu.rc.alpha_icw[st],
                            s_gpu.rc.color_ocw[st], s_gpu.rc.alpha_ocw[st],
                            s_gpu.rc.factor0[st], s_gpu.rc.factor1[st], 10);
            }
            if ((s_gpu.xf_mode & 3) == 2) {
                uint32_t r;
                for (r = 112; r < 116; r++) {
                    const float *c = nv2a_vsh_constant(r);
                    fprintf(stderr, "[FTRACE]     c[%u] %g %g %g %g%c", r,
                            c[0], c[1], c[2], c[3], 10);
                }
            }
        }
    }

    static int pbv = -1;
    if (pbv < 0) pbv = getenv("RECOMP_PB_EXEC_VERBOSE") != NULL;
    if (pbv) {
        static int shown;
        if (shown++ < 6) {
            fprintf(stderr, "  [GPU] prim %u, %u indices, pos attr:"
                            " off 0x%08X type %u size %u stride %u\n",
                    s_gpu.prim, s_gpu.idx_count, s_gpu.attr[0].offset,
                    s_gpu.attr[0].type, s_gpu.attr[0].size, s_gpu.attr[0].stride);
            /* The texture stage, for either kind of batch. This used to print
             * only for inline batches, which meant a title drawing through
             * vertex arrays -- Half-Life 2's menu, for one -- showed no
             * texture state at all, and the reason a quad sampled flat was
             * invisible. */
            fprintf(stderr, "  [GPU]   tex: off 0x%08X %ux%u pitch %u"
                            " colour 0x%02X swizzled %d valid %d\n",
                    s_gpu.texs[0].offset, s_gpu.texs[0].width, s_gpu.texs[0].height,
                    s_gpu.texs[0].pitch, s_gpu.texs[0].color,
                    d3d8_format_is_swizzled(s_gpu.texs[0].color), s_gpu.texs[0].valid);
            {
                uint32_t k;
                for (k = 0; k < s_gpu.idx_count && k < 3; k++) {
                    float t[2];
                    if (fetch_texcoord(s_gpu.idx[k], t))
                        fprintf(stderr, "  [GPU]   uv[%u] = %.3f %.3f\n",
                                k, t[0], t[1]);
                }
            }
            /* An inline batch has no guest buffer to go and look at -- the
             * vertices are the payload -- so print the payload too. */
            if (s_gpu.inline_active) {
                uint32_t k;
                fprintf(stderr, "  [GPU]   inline %u dwords:", s_gpu.inline_count);
                for (k = 0; k < s_gpu.inline_count && k < 16; k++)
                    fprintf(stderr, " %08X", s_gpu.inline_buf[k]);
                fprintf(stderr, "\n");
                for (k = 0; k < s_gpu.idx_count && k < 4; k++) {
                    float t[2];
                    if (fetch_texcoord(s_gpu.idx[k], t))
                        fprintf(stderr, "  [GPU]   uv[%u] = %.3f %.3f\n",
                                k, t[0], t[1]);
                }
                fprintf(stderr, "  [GPU]   tex: off 0x%08X %ux%u pitch %u"
                                " colour 0x%02X valid %d\n",
                        s_gpu.texs[0].offset, s_gpu.texs[0].width, s_gpu.texs[0].height,
                        s_gpu.texs[0].pitch, s_gpu.texs[0].color, s_gpu.texs[0].valid);
            }
            {
                /* Every attribute the batch has, not just position. If the
                 * other streams carry data and position does not, the problem
                 * is one buffer rather than the whole vertex path. */
                const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
                uint32_t a, k;
                for (a = 0; a < NV_VERTEX_ATTRS; a++) {
                    const VertexAttr *at = &s_gpu.attr[a];
                    uint32_t nz = 0;
                    if (!at->offset || !at->size)
                        continue;
                    for (k = 0; k < 64; k++)
                        if (mem[at->offset + k]) nz++;
                    fprintf(stderr, "  [GPU]   attr%-2u off 0x%08X type %u"
                                    " size %u stride %-3u  %u/64 bytes set\n",
                            a, at->offset, at->type, at->size, at->stride, nz);
                }
            }
            {
                /* Raw bytes at the array, in case the values read as zero:
                 * that looks the same whether the offset is wrong or the
                 * buffer genuinely has not been filled yet. */
                const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
                uint32_t k;
                fprintf(stderr, "  [GPU]   bytes @0x%08X:", s_gpu.attr[0].offset);
                for (k = 0; k < 32; k++)
                    fprintf(stderr, " %02X", mem[s_gpu.attr[0].offset + k]);
                fprintf(stderr, "\n");
                fprintf(stderr, "  [GPU]   indices:");
                for (k = 0; k < s_gpu.idx_count && k < 8; k++)
                    fprintf(stderr, " %u", s_gpu.idx[k]);
                fprintf(stderr, "\n");
            }
            for (i = 0; i < s_gpu.idx_count && i < 3; i++) {
                if (fetch_attr(&s_gpu.attr[0], s_gpu.idx[i], v))
                    fprintf(stderr, "  [GPU]   v[%u] = %.3f %.3f %.3f %.3f\n",
                            s_gpu.idx[i], v[0], v[1], v[2], v[3]);
            }
        }
    }
}

/* Draw the vertices the title wrote straight into the pushbuffer.
 *
 * INLINE_ARRAY carries no offsets and no indices: the dwords between BEGIN and
 * END *are* the vertex buffer, packed in attribute order using the same
 * SET_VERTEX_DATA_ARRAY_FORMAT registers an ordinary array would use. So the
 * whole batch is describable as a vertex array whose base happens to be that
 * payload, which means synthesising the layout and handing it to the existing
 * path -- rather than a second copy of the topology and rasterisation code.
 *
 * The title's own attribute table is saved and put back: these offsets and
 * strides are ours, and it has not stopped using its.
 *
 * ponytail: each attribute is padded to a whole dword. That is exact for the
 * float and D3DCOLOR formats every inline batch actually uses; a packed
 * sub-dword attribute would need the unpadded layout.
 */
static void draw_inline_array(void)
{
    VertexAttr saved[NV_VERTEX_ATTRS];
    uint32_t off = 0, a, i, vsize, count;

    memcpy(saved, s_gpu.attr, sizeof saved);

    for (a = 0; a < NV_VERTEX_ATTRS; a++) {
        uint32_t bytes;
        if (!s_gpu.attr[a].size)
            continue;
        switch (s_gpu.attr[a].type) {
        case 0:  bytes = 4;                        break;  /* D3DCOLOR   */
        case 2:  bytes = 4 * s_gpu.attr[a].size;   break;  /* float      */
        case 4:  bytes = s_gpu.attr[a].size;       break;  /* ubyte norm */
        default: bytes = 4 * s_gpu.attr[a].size;   break;
        }
        s_gpu.attr[a].offset = off;
        off += (bytes + 3u) & ~3u;
    }
    vsize = off;
    if (!vsize)
        goto out;

    count = (s_gpu.inline_count * 4) / vsize;
    if (count < 3 || count > NV_MAX_INDICES) { g_dbg_ilf++; goto out; }
    for (a = 0; a < NV_VERTEX_ATTRS; a++)
        if (s_gpu.attr[a].size)
            s_gpu.attr[a].stride = vsize;

    for (i = 0; i < count; i++)
        s_gpu.idx[i] = i;
    s_gpu.idx_count = count;

    s_gpu.inline_active = 1;
    draw_primitive();
    s_gpu.inline_active = 0;

out:
    memcpy(s_gpu.attr, saved, sizeof saved);
    s_gpu.idx_count = 0;
}

/* Draw the vertices SET_VERTEX3F/4F completed.
 *
 * Same trick as draw_inline_array: rather than a second copy of the topology
 * and rasterisation code, describe what was accumulated as an ordinary vertex
 * array and hand it to the existing path. The layout is ours and fixed, so
 * the attribute table is written out here rather than derived from the
 * title's format registers.
 *
 * The title's own table is saved and put back -- it has not stopped using it.
 *
 * ponytail: position, diffuse and texcoord0 only. That is what a 2D quad
 * carries and what this rasteriser samples; a second texcoord set or a normal
 * would need the D3D11 translator, not more slots here.
 */
static void draw_immediate(void)
{
    VertexAttr saved[NV_VERTEX_ATTRS];
    uint32_t i;

    if (s_gpu.imm_count < 3)
        return;

    memcpy(saved, s_gpu.attr, sizeof saved);
    memset(s_gpu.attr, 0, sizeof s_gpu.attr);
    /* Offsets are byte offsets into inline_buf here, not guest addresses --
     * fetch_attr reads them that way while inline_active is set, which is
     * also why 0 is a legal offset for position. Every attribute is a float4;
     * the ones the batch never set carry their standing values, as on the
     * GPU. */
    for (i = 0; i < NV_VERTEX_ATTRS; i++) {
        s_gpu.attr[i].type = 2;
        s_gpu.attr[i].size = 4;
        s_gpu.attr[i].offset = i * 16;
        s_gpu.attr[i].stride = IMM_VERTEX_DWORDS * 4;
    }

    for (i = 0; i < s_gpu.imm_count && i < NV_MAX_INDICES; i++)
        s_gpu.idx[i] = i;
    s_gpu.idx_count = i;

    /* fetch_attr bounds-checks against inline_count dwords. */
    s_gpu.inline_count = s_gpu.imm_count * IMM_VERTEX_DWORDS;
    s_gpu.inline_active = 1;
    draw_primitive();
    s_gpu.inline_active = 0;

    memcpy(s_gpu.attr, saved, sizeof saved);
    s_gpu.idx_count = 0;
    s_gpu.inline_count = 0;
}

/* A vertex is complete: append all 16 attributes in the layout
 * draw_immediate describes. */
static void imm_emit_vertex(void)
{
    uint32_t at = s_gpu.imm_count * IMM_VERTEX_DWORDS;

    if (!s_gpu.prim || at + IMM_VERTEX_DWORDS > NV_MAX_INLINE)
        return;
    memcpy(&s_gpu.inline_buf[at], s_gpu.imm_attr, sizeof s_gpu.imm_attr);
    s_gpu.imm_count++;
}

/* The immediate-mode writes. Returns 1 if `method` was one of them.
 *
 * Split out because it is a range test against five separate bases, and that
 * reads better than five more cases in an already long switch.
 */
/* The immediate-mode attribute methods (xemu pgraph.c SET_VERTEX_DATA*).
 * Each sets an attribute's current value; completing attribute 0 -- the
 * position -- emits a vertex carrying every attribute as it stands. This used
 * to keep position, diffuse and texcoord 0 only, which dropped any quad with
 * a second texture coordinate: Burnout 3 composites its whole 3D scene into
 * the frame with exactly such a quad, and the frame never got the scene. */
static int imm_vertex_method(uint32_t method, uint32_t param)
{
    union { uint32_t u; float f; } v;
    uint32_t attr, c;
    float *a;
    v.u = param;

    if (method >= NV097_SET_VERTEX4F && method < NV097_SET_VERTEX4F + 16) {
        c = (method - NV097_SET_VERTEX4F) / 4;
        s_gpu.imm_attr[0][c] = v.f;
        s_gpu.imm_used |= 1;
        if (c == 3)
            imm_emit_vertex();
        return 1;
    }
    if (method >= NV097_SET_VERTEX3F && method < NV097_SET_VERTEX3F + 12) {
        c = (method - NV097_SET_VERTEX3F) / 4;
        s_gpu.imm_attr[0][c] = v.f;
        s_gpu.imm_used |= 1;
        if (c == 2) {
            s_gpu.imm_attr[0][3] = 1.0f;
            imm_emit_vertex();
        }
        return 1;
    }
    if (method >= NV097_SET_VERTEX_DATA2F_M
            && method < NV097_SET_VERTEX_DATA2F_M + NV_VERTEX_ATTRS * 8) {
        attr = (method - NV097_SET_VERTEX_DATA2F_M) / 8;
        c = ((method - NV097_SET_VERTEX_DATA2F_M) % 8) / 4;
        a = s_gpu.imm_attr[attr];
        a[c] = v.f;
        if (c == 1) {
            a[2] = 0.0f; a[3] = 1.0f;
        }
        s_gpu.imm_used |= (uint16_t)(1u << attr);
        if (attr == 0 && c == 1)
            imm_emit_vertex();
        return 1;
    }
    if (method >= NV097_SET_VERTEX_DATA4F_M
            && method < NV097_SET_VERTEX_DATA4F_M + NV_VERTEX_ATTRS * 16) {
        attr = (method - NV097_SET_VERTEX_DATA4F_M) / 16;
        c = ((method - NV097_SET_VERTEX_DATA4F_M) % 16) / 4;
        s_gpu.imm_attr[attr][c] = v.f;
        s_gpu.imm_used |= (uint16_t)(1u << attr);
        if (attr == 0 && c == 3)
            imm_emit_vertex();
        return 1;
    }
    if (method >= NV097_SET_VERTEX_DATA2S
            && method < NV097_SET_VERTEX_DATA2S + NV_VERTEX_ATTRS * 4) {
        attr = (method - NV097_SET_VERTEX_DATA2S) / 4;
        a = s_gpu.imm_attr[attr];
        a[0] = (float)(int16_t)(param & 0xFFFF);
        a[1] = (float)(int16_t)(param >> 16);
        a[2] = 0.0f; a[3] = 1.0f;
        s_gpu.imm_used |= (uint16_t)(1u << attr);
        if (attr == 0)
            imm_emit_vertex();
        return 1;
    }
    if (method >= NV097_SET_VERTEX_DATA4UB
            && method < NV097_SET_VERTEX_DATA4UB + NV_VERTEX_ATTRS * 4) {
        /* Bytes in register order, x from the low byte (xemu). */
        attr = (method - NV097_SET_VERTEX_DATA4UB) / 4;
        a = s_gpu.imm_attr[attr];
        a[0] = (float)( param        & 0xFF) / 255.0f;
        a[1] = (float)((param >>  8) & 0xFF) / 255.0f;
        a[2] = (float)((param >> 16) & 0xFF) / 255.0f;
        a[3] = (float)( param >> 24        ) / 255.0f;
        s_gpu.imm_used |= (uint16_t)(1u << attr);
        if (attr == 0)
            imm_emit_vertex();
        return 1;
    }
    if (method >= NV097_SET_VERTEX_DATA4S_M
            && method < NV097_SET_VERTEX_DATA4S_M + NV_VERTEX_ATTRS * 8) {
        attr = (method - NV097_SET_VERTEX_DATA4S_M) / 8;
        c = ((method - NV097_SET_VERTEX_DATA4S_M) % 8) / 4;
        a = s_gpu.imm_attr[attr];
        a[c * 2]     = (float)(int16_t)(param & 0xFFFF);
        a[c * 2 + 1] = (float)(int16_t)(param >> 16);
        s_gpu.imm_used |= (uint16_t)(1u << attr);
        if (attr == 0 && c == 1)
            imm_emit_vertex();
        return 1;
    }
    return 0;
}
/* The texture stage registers, all four stages. Stage i's registers are
 * 0x1B00 + 0x40*i onwards; only stage 0 used to be decoded, which was
 * enough for menus drawn one texture at a time and not for a 3D frame that
 * composites its scene through a second stage. */
static void tex_stage_method(uint32_t method, uint32_t param)
{
    uint32_t stage = (method - NV_TEX_FIRST) / 0x40;
    uint32_t reg = NV_TEX_FIRST + ((method - NV_TEX_FIRST) & 0x3F);
    Texture *t = &s_gpu.texs[stage];

    switch (reg) {
    case NV097_SET_TEXTURE_OFFSET:
        /* A DMA-object offset like a surface's: physical, reached through
         * the contiguous window when it names contiguous memory. */
        t->offset = dma_resolve(param);
        break;
    case NV097_SET_TEXTURE_FORMAT:
        t->color = (param >> 8) & 0xFF;
        t->cube = (param >> 2) & 1;
        t->border_color = (param >> 3) & 1;    /* BORDER_SOURCE: colour */
        t->levels = (param >> 16) & 0xF;
        /* A swizzled texture carries its own dimensions here, as log2 in
         * BASE_SIZE_U/V. It has to: IMAGE_RECT describes a linear image, and
         * a title that only uses swizzled textures never sends one. */
        if (tex_size_from_format(t->color)) {
            t->width  = 1u << ((param >> 20) & 0xF);
            t->height = 1u << ((param >> 24) & 0xF);
        }
        break;
    case NV097_SET_TEXTURE_PALETTE:
        /* The low six bits carry the DMA context and the entry count. */
        t->palette = dma_resolve(param & ~0x3Fu);
        break;
    case NV097_SET_TEXTURE_ADDRESS:
        /* Four bits per axis: 1 wrap, 2 mirror, 3 clamp-to-edge, 4 border
         * (the border colour when FORMAT selects it, else clamped). */
        t->addr_u =  param        & 0xF;
        t->addr_v = (param >>  8) & 0xF;
        break;
    case NV097_SET_TEXTURE_CONTROL1:
        t->pitch = param >> 16;          /* linear formats only */
        break;
    case NV097_SET_TEXTURE_IMAGE_RECT:
        t->width  = param >> 16;
        t->height = param & 0xFFFF;
        break;
    case 0x1B14:                                  /* SET_TEXTURE_FILTER */
        t->filter = param;
        break;
    case 0x1B24:                                  /* SET_TEXTURE_BORDER_COLOR */
        t->border = param;
        break;
    default:
        break;
    }
    tex_update_valid(t);
    if (stage == 0)
        record_tex_reg(method, param);
}

/* The flip hold (nv2a_flip_hold.h). The executor arms it at FLIP_STALL; the
 * ack loop in xbox_memory_layout.c polls it each pass and, while it holds,
 * walks nothing, leaves DMA_GET behind PUT and the PFB flush unacknowledged.
 * Both run on the ack thread, so the state needs no lock. */
static XboxFlipHold s_flip_hold;
static uint32_t s_vbl_count;        /* the vblank count last seen to move */
static uint64_t s_vbl_moved_ns;
/* The vblank count when the walk that reaches a FLIP_STALL began, which is
 * close to when the title submitted the segment holding its Swap. Sampled
 * at the FLIP_STALL instead, the segment's own draw time (several ms) moved
 * a frame the title swapped just before a vblank past it; the title's next
 * frame then counted as a second flip in the same vblank and was held for
 * most of a frame. */
static uint32_t s_walk_vbl;

void nv2a_pb_exec_walk_begin(void)
{
    s_walk_vbl = xbox_VblankCount();
}

/* RECOMP_FLIP_PACING: unset or 0 off (flips complete at once and frame
 * counters are bumped on every flip, as before), edge (D3D's rule: a late
 * frame waits for the next vblank), anything else on. Opt-in: it holds the
 * title's GPU on the guest vblank clock, which only runs under RECOMP_VBLANK,
 * and a held flip is a stall a title's own pacing has never seen here.
 * The first caller can be the timer thread, a KeSetTimer caller (through
 * the vblank mode) or the ack thread. Two first callers read the same
 * variable and store the same values, the edge flag before the mode, so the
 * race costs at most a second log line. No Interlocked: tests link the
 * executor without the platform layer (tests/nv2a_ffp_backend). */
int nv2a_pb_exec_flip_pacing(void)
{
    static volatile int mode = -1;
    if (mode < 0) {
        const char *e = getenv("RECOMP_FLIP_PACING");
        int m = (!e || !*e || !strcmp(e, "0")) ? 0
                : !strcmp(e, "edge") ? 2 : 1;
        s_flip_hold.edge = m == 2;
        mode = m;
        if (e)
            fprintf(stderr, "  [GPU] flip pacing %s (RECOMP_FLIP_PACING=%s)\n",
                    m == 0 ? "off" : m == 2 ? "edge" : "on", e);
    }
    return mode;
}

/* The guest vblank count, and whether it is a running clock: vblanks are on
 * and the count moved within XBOX_FLIP_CLOCK_STALE_NS. Without it a hold
 * would only ever end on its timeout. */
static uint32_t flip_clock(uint64_t now_ns, int *ok)
{
    uint32_t c = xbox_VblankCount();
    if (c != s_vbl_count) {
        s_vbl_count = c;
        s_vbl_moved_ns = now_ns;
    }
    *ok = s_vbl_moved_ns && now_ns - s_vbl_moved_ns < XBOX_FLIP_CLOCK_STALE_NS;
    return c;
}

/* Once per pass of the ack loop: 1 while a flip is held. */
int nv2a_pb_exec_flip_poll(void)
{
    uint64_t now;
    uint32_t c;
    int ok;

    if (!s_flip_hold.held)
        return 0;
    now = xbox_HostNowNs();
    c = flip_clock(now, &ok);
    return xbox_FlipHoldPoll(&s_flip_hold, c, now, ok);
}

/* The hold's line in the periodic [GPU] report, then reset. */
static void flip_hold_report(void)
{
    XboxFlipHold *h = &s_flip_hold;
    if (nv2a_pb_exec_flip_pacing() == 0)
        return;
    fprintf(stderr, "[GPU] flip hold: holds %u held_ms %.1f hold_timeouts %u"
            " clock_stops %u iv0 %u iv1 %u iv2 %u iv3 %u nonop %u"
            " counters_owned %u\n", h->holds, h->held_ns / 1e6, h->timeouts,
            h->clock_stops, h->iv[0], h->iv[1], h->iv[2], h->iv[3], h->nonop,
            xbox_Nv2aFrameCountersOwned());
    h->holds = h->timeouts = h->clock_stops = h->nonop = 0;
    h->iv[0] = h->iv[1] = h->iv[2] = h->iv[3] = 0;
    h->held_ns = 0;
}

/* RECOMP_PB_FLIPSUM=1: one line per FLIP_STALL summarising the methods since
 * the last one, so a frame that lost work can be compared with its
 * neighbours (methods, BEGINs, clears, surface changes, software methods,
 * vertex-program words/loads/starts, per-subchannel counts). */
static int s_fsum = -1;
static unsigned fs_m, fs_beg, fs_clr, fs_surf, fs_nop, fs_pw, fs_pl, fs_ps, fs_sub[8];
static uint32_t fs_surf_last[4], fs_nop_last[4];
static void flipsum_note(uint32_t subch, uint32_t method, uint32_t param)
{
    fs_m++; fs_sub[subch & 7]++;
    if (subch) return;
    if (method == 0x17FC && param) fs_beg++;
    else if (method == 0x1D94) fs_clr++;
    else if (method == 0x0210) fs_surf_last[fs_surf++ & 3] = param;
    else if (method == 0x0100 && param) fs_nop_last[fs_nop++ & 3] = param;
    else if (method >= 0x0B00 && method < 0x0B80) fs_pw++;
    else if (method == 0x1E9C) fs_pl++;
    else if (method == 0x1EA0) fs_ps++;
}
static void flipsum_flip(void)
{
    fprintf(stderr, "[FSUM] m=%u beg=%u clr=%u surf=%u(%08X,%08X) nop=%u(%08X,%08X) pw=%u pl=%u ps=%u sub=%u/%u/%u/%u/%u/%u/%u/%u\n",
            fs_m, fs_beg, fs_clr, fs_surf, fs_surf_last[0], fs_surf_last[1], fs_nop, fs_nop_last[0], fs_nop_last[1],
            fs_pw, fs_pl, fs_ps, fs_sub[0], fs_sub[1], fs_sub[2], fs_sub[3], fs_sub[4], fs_sub[5], fs_sub[6], fs_sub[7]);
    fs_m = fs_beg = fs_clr = fs_surf = fs_nop = fs_pw = fs_pl = fs_ps = 0;
    memset(fs_sub, 0, sizeof fs_sub);
    memset(fs_surf_last, 0, sizeof fs_surf_last); memset(fs_nop_last, 0, sizeof fs_nop_last);
}

uint32_t g_rbx_last_sem;   /* last BACK_END_WRITE_SEMAPHORE_RELEASE the walk executed */
/* Set when the walk writes the title's fence word (fence-exec mode), so a
 * thread waiting for the fence (BlockOnFence's recheck, recomp_manual.c) can
 * sleep on it instead of spinning. Created by that waiter; NULL = nobody. */
void *volatile g_rbx_fence_event;
volatile long g_rbx_fence_event_live;   /* the walk has signalled it at least once */

void nv2a_pb_exec_method(uint32_t subch, uint32_t method, uint32_t param)
{
    static int inited;

    if (s_fsum) {
        if (s_fsum < 0) s_fsum = getenv("RECOMP_PB_FLIPSUM") != NULL;
        if (s_fsum) flipsum_note(subch, method, param);
    }

    /* BACK_END_WRITE_SEMAPHORE_RELEASE. The title's pointer already names the
     * semaphore word; SET_SEMAPHORE_OFFSET is not added (in X-Men Legends it
     * held 0xFF000000 at times, which put the write outside guest memory). */
    if (subch == 0 && method == 0x1D70) g_rbx_last_sem = param;
    if (subch == 0 && method == 0x1D70) {                    /* fence-exec: see xbox_Nv2aFenceExecMode */
        extern int xbox_Nv2aFenceExecMode(void);
        static int fx_log;
        if (xbox_Nv2aFenceExecMode() && !s_sem_va) s_sem_va = xbox_Nv2aFenceWordVa();
        static int fence_log = -1;
        if (fence_log < 0) fence_log = getenv("RECOMP_FENCE_LOG") != NULL;   /* once, not per release */
        if (fence_log && fx_log++ < 40)
            fprintf(stderr, "[FENCE] semaphore release param=%08X sem_va=%08X\n", param, s_sem_va);
    }
    if (subch == 0 && method == 0x1D70 && s_sem_va) {
        uint8_t *mem = (uint8_t *)xbox_GetMemoryOffset();
        *(volatile uint32_t *)(mem + s_sem_va) = param;
        if (g_rbx_fence_event) {
            SetEvent((HANDLE)g_rbx_fence_event);
            g_rbx_fence_event_live = 1;
        }
        return;
    }
    if (!inited) {
        inited = 1;
        s_gpu.aa_sx = s_gpu.aa_sy = 1.0f;
        /* Stencil state as the hardware resets it: GL_ALWAYS, masks 0xFF,
         * GL_KEEP, until the title says otherwise. */
        s_reg[0x0364 / 4] = 0x207u;                            /* FUNC */
        s_reg[0x0360 / 4] = s_reg[0x036C / 4] = 0xFFu;         /* MASK, FUNC_MASK */
        s_reg[0x0370 / 4] = s_reg[0x0374 / 4] = s_reg[0x0378 / 4] = 0x1E00u;
        s_gpu.color_mask = 0x01010101u;           /* all channels, as reset */
        {
            int i;
            for (i = 0; i < NV_VERTEX_ATTRS; i++)
                s_gpu.imm_attr[i][3] = 1.0f;
        }
        s_gpu.min_x = s_gpu.min_y = 1e30f;
        s_gpu.max_x = s_gpu.max_y = -1e30f;
    }
    if (subch == 0)                        /* the 3D class's register file */
        s_reg[(method & 0x1FFCu) / 4] = param;
    /* Bring-up: the first parameters each surface method carries. A wrong
     * pitch or clip is indistinguishable from a method never arriving unless
     * the values are visible. Cached: this runs for every method, and an
     * uncached getenv here was a quarter of the executor's time. */
    static int verbose = -1;
    if (verbose < 0)
        verbose = getenv("RECOMP_PB_EXEC_VERBOSE") != NULL;
    if (verbose) {
        static int shown[8];
        int slot = -1;
        switch (method) {
        case NV097_SET_SURFACE_CLIP_HORIZONTAL: slot = 0; break;
        case NV097_SET_SURFACE_CLIP_VERTICAL:   slot = 1; break;
        case NV097_SET_SURFACE_FORMAT:          slot = 2; break;
        case NV097_SET_SURFACE_PITCH:           slot = 3; break;
        case NV097_SET_SURFACE_COLOR_OFFSET:    slot = 4; break;
        case NV097_SET_COLOR_CLEAR_VALUE:       slot = 5; break;
        case NV097_CLEAR_SURFACE:               slot = 6; break;
        default: break;
        }
        if (slot >= 0 && shown[slot]++ < 4)
            fprintf(stderr, "  [GPU] subch %u method 0x%04X param 0x%08X\n",
                    subch, method, param);
    }

    if (s_ftrace == 2) {
        /* RECOMP_FRAME_TRACE_METHODS: every method of the traced frame,
         * less the bulk (vertex data, program and constant uploads). */
        static int all = -1;
        if (all < 0)
            all = getenv("RECOMP_FRAME_TRACE_METHODS") != NULL;
        if (all && !(method >= 0x1800 && method < 0x1A00)
            && !(method >= 0x0B00 && method < 0x0C00))
            fprintf(stderr, "[FTRACE]   m %u:%04X = %08X%c", subch, method,
                    param, 10);
    }
    if (subch == 0 && method >= NV_TEX_FIRST && method <= NV_TEX_LAST) {
        tex_stage_method(method, param);
        return;
    }
    /* XDK D3D's KickOff (found in a title's statically linked D3D, which
     * reads it back; see d3d8ltcg-device-context.md) ends every kick with
     * method 0x0310 on subchannel 5:
     *
     *     ((pushbuffer_address * 8 | (fence & 0x1F)) << 2) | (wrap & 3)
     *
     * The GPU latches it into PGRAPH 0x400B10, and D3D waits there for the
     * kick's fence bits. Unmodelled, 0x400B10 read 0 and the wait ended only
     * when the semaphore happened to be a multiple of 32. (DMA_GET, which D3D
     * reads next, is published by the walk itself; see nv2a_pb_scan.c. The
     * marker's own address is behind the executor's read position, so it is
     * not used for that.)
     *
     * D3D compares the latch's fence bits with the semaphore, so the two have
     * to tell the same story. When a back end names the semaphore the
     * executor writes it from the title's own releases, and the marker agrees
     * with it. A project that mirrors the title's fence instead (the runtime
     * writes "everything submitted is consumed" into the word,
     * xbox_Nv2aMirrorFence) holds D3D's own submitted counter there, which
     * the marker's fence bits do not match, and D3D's wait then lasts for
     * ever: the title froze at the first screen whose walk was long enough to
     * enter it. So for a mirrored fence the latch carries the mirror's fence
     * bits and the marker's address, which is what a GPU that had consumed
     * everything would report. */
    if (method == 0x0310 && subch == 5 && nv2a_pb_dma_walk()) {
        volatile uint32_t *r = xbox_Nv2aRegPtr(0x400B10);

        if (!r)
            return;
        if (!s_sem_va) {
            uint32_t va = xbox_Nv2aFenceWordVa();

            if (va) {
                uint32_t sem = *(volatile uint32_t *)
                               ((uint8_t *)xbox_GetMemoryOffset() + va);
                param = (param & ~0x7Cu) | ((sem & 0x1Fu) << 2);
            }
        }
        *r = param;
        return;
    }
    if (subch != 0) {                      /* 3D class lives on subchannel 0 */
        note_unhandled(method, param);
        return;
    }
    switch (method) {
    case NV097_SET_SURFACE_CLIP_HORIZONTAL:
        s_gpu.clip_raw_h = param;
        surface_apply_clip();
        break;
    case NV097_SET_SURFACE_CLIP_VERTICAL:
        s_gpu.clip_raw_v = param;
        surface_apply_clip();
        break;
    case NV097_SET_SURFACE_FORMAT:
        s_gpu.format = param;
        surface_apply_clip();
        break;
    case 0x1D98:                       /* SET_CLEAR_RECT_HORIZONTAL */
        s_gpu.clear_rect_h = param;
        s_gpu.clear_rect_set |= 1;
        break;
    case 0x1D9C:                       /* SET_CLEAR_RECT_VERTICAL */
        s_gpu.clear_rect_v = param;
        s_gpu.clear_rect_set |= 2;
        break;
    case NV097_SET_SURFACE_PITCH:
        s_gpu.pitch = param & 0xFFFF;      /* colour pitch; zeta is the top half */
        break;
    case NV097_SET_SURFACE_COLOR_OFFSET:
        if (s_ftrace == 2)
            fprintf(stderr, "[FTRACE] color offset -> %08X%c", param, 10);
        s_gpu.color_offset = param;
        break;
    case NV097_SET_COLOR_CLEAR_VALUE:
        s_gpu.clear_color = param;
        break;
    case NV097_SET_BLEND_ENABLE:
        s_gpu.blend_enable = param;
        break;
    case NV097_SET_BLEND_EQUATION:
        s_gpu.blend_equation = param;
        break;
    case 0x0358:                                  /* SET_COLOR_MASK */
        s_gpu.color_mask = param;
        break;

    /* Register combiners. */
    case 0x0288: s_gpu.rc.final0 = param; break;  /* SPECULAR_FOG_CW0 */
    case 0x028C: s_gpu.rc.final1 = param; break;  /* SPECULAR_FOG_CW1 */
    case 0x1E20: s_gpu.rc.final_c0 = param; break;/* SPECULAR_FOG_FACTOR */
    case 0x1E24: s_gpu.rc.final_c1 = param; break;
    case 0x1E60:                                  /* COMBINER_CONTROL */
        s_gpu.rc.control = param;
        s_gpu.rc_seen = 1;
        break;
    case 0x1E70: s_gpu.rc.stage_program = param; break;
    case 0x17F8: s_gpu.clip_plane_mode = param; break;

    /* Alpha test and fog. */
    case 0x0300: s_gpu.alpha_test = param; break;
    case 0x033C: s_gpu.alpha_func = param; break;
    case 0x0340: s_gpu.alpha_ref = param; break;
    case 0x02A4: s_gpu.fog_enable = param; break;
    case 0x02A8: s_gpu.fog_color = param; break;  /* R in bits 0-7 */
    case 0x029C: s_gpu.fog_mode = param; break;
    case 0x09C0: memcpy(&s_gpu.fog_param[0], &param, 4); break;
    case 0x09C4: memcpy(&s_gpu.fog_param[1], &param, 4); break;
    case 0x17C8:                                  /* CLEAR_REPORT_VALUE */
        s_gpu.zpass_count = 0;
        break;
    case 0x17CC:                                  /* SET_ZPASS_PIXEL_COUNT_ENABLE */
        s_gpu.zpass_enable = param;
        break;
    case 0x17D0: {                                /* GET_REPORT */
        /* The GPU's answer to a visibility test: 16 bytes at the report
         * DMA offset -- a timestamp, the pixel count, and 0 for "done".
         * Never written, a title polling it (D3D's GetVisibilityTestResult
         * starts the slot at 0xFFFFFFFF) sees every test incomplete. Burnout
         * 3 culls its world with these, so the race's main view drew only
         * the sky while the cube-map passes, which do not test, drew it all.
         * The report DMA object starts at physical 0 here, as D3D sets it. */
        uint8_t *mem = (uint8_t *)xbox_GetMemoryOffset();
        uint32_t at = dma_resolve(param & 0x00FFFFFFu);
        uint64_t stamp = (uint64_t)s_gpu.reports++ * 1000u;
        memcpy(mem + at, &stamp, 8);
        memcpy(mem + at + 8, &s_gpu.zpass_count, 4);
        memset(mem + at + 12, 0, 4);
        break;
    }
    case NV097_SET_BLEND_COLOR:
        s_gpu.blend_color = param;
        break;
    case NV097_SET_BLEND_FUNC_SFACTOR:
        s_gpu.blend_sfactor = param;
        break;
    case NV097_SET_BLEND_FUNC_DFACTOR:
        s_gpu.blend_dfactor = param;
        break;
    case NV097_CLEAR_SURFACE:
        if (s_ftrace == 2)
            fprintf(stderr, "[FTRACE] clear %X surf %08X %ux%u colour %08X%c",
                    param, s_gpu.color_offset, s_gpu.clip_w, s_gpu.clip_h,
                    s_gpu.clear_color, 10);
        clear_surface(param);
        if (param & 0x01)                         /* Z */
            zbuf_clear();
        break;

    case NV097_SET_BEGIN_END:
        if (param) {
            g_dbg_begin++;
            s_gpu.prim = param;
            s_gpu.idx_count = 0;
            s_gpu.inline_count = 0;
            s_gpu.imm_count = 0;
        } else {
            /* Three ways a batch can have arrived, and only one is in use at
             * a time: vertices completed by SET_VERTEX4F, a payload written
             * with INLINE_ARRAY, or indices into the title's own arrays. */
            if (s_gpu.imm_count)
                draw_immediate();
            else if (s_gpu.inline_count)
                draw_inline_array();
            else
                draw_primitive();
            s_gpu.prim = 0;
            s_gpu.inline_count = 0;
            s_gpu.imm_count = 0;
        }
        break;

    case NV097_INLINE_ARRAY:
        /* Vertex data, not a pointer to it. Buffered rather than decoded here
         * because the format is only fully known at END. */
        if (s_gpu.prim && s_gpu.inline_count < NV_MAX_INLINE)
            s_gpu.inline_buf[s_gpu.inline_count++] = param;
        break;

    case NV097_DRAW_ARRAYS: {
        /* The method this title actually draws with, and the reason the
         * executor reported zero draws while geometry was being submitted the
         * whole time: BEGIN_END arrived, END arrived, and in between came a
         * run description rather than the index list the draw path wanted, so
         * every batch ended with idx_count == 0 and was dropped in silence.
         *
         * Expanded into indices because that is what the rasteriser consumes,
         * and an implicit run is just the indices start..start+count-1. */
        uint32_t start = param & 0x00FFFFFFu;
        uint32_t count = ((param >> 24) & 0xFFu) + 1u;
        uint32_t i;

        if (!s_gpu.prim)
            break;
        for (i = 0; i < count && s_gpu.idx_count < NV_MAX_INDICES; i++)
            s_gpu.idx[s_gpu.idx_count++] = start + i;
        break;
    }

    case NV097_ARRAY_ELEMENT16:
        /* Two 16-bit indices per parameter word. */
        if (s_gpu.prim && s_gpu.idx_count + 2 <= NV_MAX_INDICES) {
            s_gpu.idx[s_gpu.idx_count++] = (uint16_t)(param & 0xFFFF);
            s_gpu.idx[s_gpu.idx_count++] = (uint16_t)(param >> 16);
        }
        break;

    case 0x1808:                            /* ARRAY_ELEMENT32: one index */
        if (s_gpu.prim && s_gpu.idx_count < NV_MAX_INDICES)
            s_gpu.idx[s_gpu.idx_count++] = param;
        break;
    case NV097_NO_OPERATION:
        /* B1x: D3D software method 5 wakes the fence waiter (kernel_bridge.c).
         * Dropping it froze the title whenever the GPU was far behind. */
        if (param) {
            extern int xbox_NV2ASoftwareMethod(uint32_t param);
            if (xbox_NV2ASoftwareMethod(param))
                return;
        }
        /* Non-zero, it is one of D3D's software methods, which the GPU traps
         * to D3D's interrupt handler on the console. Only the swap's interval
         * matters here (nv2a_flip_hold.h); the rest stays a no-op. */
        if (!nv2a_pb_exec_flip_pacing()) {
            if (!imm_vertex_method(method, param))
                note_unhandled(method, param);   /* as before the hold */
            break;
        }
        if (param)
            xbox_FlipHoldSwap(&s_flip_hold, param);
        return;

    case NV097_SET_FLIP_READ:
        if (s_ftrace == 2)
            fprintf(stderr, "[FTRACE] P_READ %u%c", param, 10);
        s_gpu.flip_read = param;
        return;

    case NV097_SET_FLIP_WRITE:
        if (s_ftrace == 2)
            fprintf(stderr, "[FTRACE] P_WRITE %u%c", param, 10);
        s_gpu.flip_write = param;
        return;

    case NV097_SET_FLIP_MODULO:
        s_gpu.flip_modulo = param;
        return;

    case NV097_FLIP_INCREMENT_WRITE:
        s_gpu.flip_write = s_gpu.flip_modulo
                         ? (s_gpu.flip_write + 1) % s_gpu.flip_modulo
                         : s_gpu.flip_write + 1;
        s_gpu.flips++;
        frame_trace_flip();
        return;

    case NV097_FLIP_STALL: {
        /* The flip is measured from the vblank the title swapped in (the
         * walk's start, s_walk_vbl); the hold's clock starts here, before
         * the present below: a vsynced present can hold this thread for a
         * host frame, and that time counts against the hold. */
        uint64_t stall_ns = xbox_HostNowNs();
        int clock_ok;
        uint32_t stall_vbl;
        if (s_fsum > 0) flipsum_flip();
        (void)flip_clock(stall_ns, &clock_ok);
        stall_vbl = s_walk_vbl;
        if (s_ftrace == 2)
            fprintf(stderr, "[FTRACE] FLIP_STALL presenting %08X (color now %08X)%c",
                    s_gpu.drawn_offset, s_gpu.color_offset, 10);
        /* The stall ends when the buffer being read is the one just finished.
         * There is no scanout here to wait for, so that is now; when the
         * title may go on is the flip hold's (below). */
        s_gpu.flip_read = s_gpu.flip_write;
        /* And this is a completed swap, which is what a title's own swap
         * counter counts -- see xbox_Nv2aFrameCounterFlip. */
        xbox_Nv2aFrameCounterFlip();
        /* Hand the window a copy of the frame just finished.
         *
         * The buffer the title has finished is the one the last batch drew
         * into, which is what drawn_offset holds and why it exists: by the
         * flip, color_offset has already moved to the next buffer. Copying
         * here, rather than letting the window read guest memory on its own
         * clock, is what stops it showing a surface the rasteriser is still
         * writing. */
        {
            extern void xbox_FramebufferWindowFrameStats(uint32_t);
            static uint32_t draws_at_flip;
            xbox_FramebufferWindowFrameStats(s_gpu.draws - draws_at_flip);
            draws_at_flip = s_gpu.draws;
        }
        /* A back end puts its frame into guest memory here, before the window copies it. */
        {
            extern volatile uint32_t g_nv2a_flip_va;
            uint32_t shown = s_gpu.drawn_offset ? s_gpu.drawn_offset : s_gpu.color_offset;
            g_nv2a_flip_va = shown ? dma_resolve(shown) : 0;
        }
        if (s_backend && s_backend->flip)
            s_backend->flip();
        if (s_gpu.pitch) {
            extern void xbox_FramebufferWindowPresent(uint32_t, uint32_t);
            uint32_t done = s_gpu.drawn_offset ? s_gpu.drawn_offset
                                               : s_gpu.color_offset;
            uint32_t pitch = s_gpu.drawn_offset ? s_gpu.drawn_pitch
                                                : s_gpu.pitch;
            if (done) {
                uint32_t aa_sy = s_gpu.aa_sy > 1.5f ? 2u : 1u;
                /* CS sets clip 640x960 with AA format bits clear (1x1), so
                 * aa_sy stayed 1 and Present showed the top 480 only —
                 * off-center / cropped title UI. Infer 2x vertical resolve
                 * when the colour clip is a 2× display height at 640-wide. */
                if (aa_sy < 2u && s_gpu.clip_w == 640u && s_gpu.clip_h >= 960u)
                    aa_sy = 2u;
                if (aa_sy < 2u) {
                    const char *e = getenv("RECOMP_FB_AA_SY");
                    if (e && e[0] == '2')
                        aa_sy = 2u;
                }
                static int s_aa_logged;
                if (!s_aa_logged) {
                    fprintf(stderr,
                            "  [FB] Present AA sy=%u clip=%ux%u pitch=%u "
                            "(format aa_sy=%.0f)\n",
                            aa_sy, s_gpu.clip_w, s_gpu.clip_h, pitch,
                            s_gpu.aa_sy);
                    fflush(stderr);
                    s_aa_logged = 1;
                }
                xbox_FramebufferWindowSet(dma_resolve(done), pitch);
                xbox_FramebufferWindowSetAA(aa_sy);
                {
                    /* While the window shows the D3D11 frame (shared
                     * texture), this CPU copy of guest memory only feeds
                     * the GDI fallback, the overlay and dumps, yet it cost
                     * ~5% of this thread (bench profile). Copy every Nth
                     * flip then (RECOMP_FB_COPY_EVERY, default 8; 1 = every
                     * flip; 8 measured safe on the bench). With no shared frame, every flip
                     * as before. */
                    extern int rbx_gpu_frame_shared(void);
                    static int every = -1;
                    static unsigned n;
                    if (every < 0) {
                        const char *e = getenv("RECOMP_FB_COPY_EVERY");
                        every = e ? atoi(e) : 8;
                        if (every < 1) every = 1;
                    }
                    if (every == 1 || !rbx_gpu_frame_shared() || ++n % (unsigned)every == 0)
                        xbox_FramebufferWindowPresent(dma_resolve(done), pitch);
                }
            }
        }
        {
            /* RECOMP_FB_DUMP_FLIPS: dump every presented frame, not one per
             * report -- a consecutive sequence, which is what a headless
             * recording (frames -> ffmpeg) needs. "1" dumps from boot; any
             * other value names a file, and frames are dumped while it
             * exists, so a capture can start mid-game without writing
             * gigabytes of menus first. */
            static const char *spec = (const char *)-1;
            if (spec == (const char *)-1)
                spec = getenv("RECOMP_FB_DUMP_FLIPS");
            if (spec) {
                int on = strcmp(spec, "1") == 0;
                if (!on) {
                    FILE *f = fopen(spec, "rb");
                    if (f) { fclose(f); on = 1; }
                }
                s_flip_dumping = on;
                if (on)
                    dump_surface_bmp();
            }
        }
        s_gpu.drawn_stale = 1;

        if (verbose) {
            static unsigned n;
            if (n++ < 8) {
                fprintf(stderr, "  [GPU] flip %u: read=%u write=%u\n",
                        s_gpu.flips, s_gpu.flip_read, s_gpu.flip_write);
                fflush(stderr);
            }
        }
        /* Arm the hold. Not inside a pushbuffer CALL: the subroutine's
         * return lives in this walk alone, so the hold could not resume
         * there; that flip completes at once and still sets the count the
         * next one is measured from. The swap above (frame counter,
         * present) is counted now, when the hold is armed, not at its
         * release: what the title waits on is GET and the PFB flush, and
         * a title polling its swap counter sees the flip up to a vblank
         * early. */
        if (nv2a_pb_exec_flip_pacing()) {
            extern int nv2a_pb_scan_in_call(void);
            xbox_FlipHoldStall(&s_flip_hold, stall_vbl, stall_ns,
                               clock_ok && !nv2a_pb_scan_in_call());
        }
        return;
    }

    /* Vertex programs and their constants: forwarded as they arrive. */
    case 0x1E94:                                  /* TRANSFORM_EXECUTION_MODE */
        s_gpu.xf_mode = param;
        break;
    case 0x1E98:                                  /* _PROGRAM_CXT_WRITE_EN */
        nv2a_vsh_set_cxt_write(param);
        s_rbx_cxt_write = param;
        break;
    case 0x1E9C:                                  /* _PROGRAM_LOAD */
        nv2a_vsh_set_load_slot(param);
        s_vp_load = param < 136 ? param * 4 : 136 * 4;
        break;
    case 0x1EA0:                                  /* _PROGRAM_START */
        nv2a_vsh_set_start_slot(param);
        s_vp_start = param; s_vp_dirty = 1;
        break;
    case 0x1EA4:                                  /* _CONSTANT_LOAD */
        nv2a_vsh_set_constant_load(param);
        break;

    /* Fixed-function transform: the composite matrix, 16 floats. The viewport
     * offset arrives in the constant file, above. */
    case 0x0680: case 0x0684: case 0x0688: case 0x068C:
    case 0x0690: case 0x0694: case 0x0698: case 0x069C:
    case 0x06A0: case 0x06A4: case 0x06A8: case 0x06AC:
    case 0x06B0: case 0x06B4: case 0x06B8: case 0x06BC:
        memcpy(&s_gpu.composite[(method - 0x0680) / 4], &param, 4);
        s_gpu.composite_set = 1;
        break;

    /* Depth. */
    case 0x030C: s_gpu.depth_test = param; break; /* DEPTH_TEST_ENABLE */
    case 0x0354: s_gpu.depth_func = param; break; /* DEPTH_FUNC */
    case 0x035C: s_gpu.depth_mask = param; break; /* DEPTH_MASK */
    case 0x0214: s_gpu.zeta_offset = param; break;/* SURFACE_ZETA_OFFSET */
    case 0x1D8C: s_gpu.zstencil_clear = param; break; /* ZSTENCIL_CLEAR_VALUE */

    default:
        if (method >= 0x0260 && method < 0x0280) {         /* ALPHA_ICW(i) */
            s_gpu.rc.alpha_icw[(method - 0x0260) / 4] = param;
            break;
        }
        if (method >= 0x0AC0 && method < 0x0AE0) {         /* COLOR_ICW(i) */
            s_gpu.rc.color_icw[(method - 0x0AC0) / 4] = param;
            break;
        }
        if (method >= 0x1E40 && method < 0x1E60) {         /* COLOR_OCW(i) */
            s_gpu.rc.color_ocw[(method - 0x1E40) / 4] = param;
            break;
        }
        if (method >= 0x0AA0 && method < 0x0AC0) {         /* ALPHA_OCW(i) */
            s_gpu.rc.alpha_ocw[(method - 0x0AA0) / 4] = param;
            break;
        }
        if (method >= 0x0A60 && method < 0x0A80) {         /* FACTOR0(i) */
            s_gpu.rc.factor0[(method - 0x0A60) / 4] = param;
            break;
        }
        if (method >= 0x0A80 && method < 0x0AA0) {         /* FACTOR1(i) */
            s_gpu.rc.factor1[(method - 0x0A80) / 4] = param;
            break;
        }
        /* SET_VIEWPORT_OFFSET / _SCALE. The GPU keeps these in the constant
         * file, at c[59] and c[58] -- exactly where the D3D epilogue reads
         * them -- so they are constants that happen to arrive by method.
         * Dropped, every vertex program put its whole batch at the origin:
         * no 3D anywhere, and a black car on the garage screen. */
        if (method >= 0x0A20 && method < 0x0A30) {
            nv2a_vsh_constant_component(59, (method - 0x0A20) / 4, param);
            break;
        }
        if (method >= 0x0AF0 && method < 0x0B00) {
            nv2a_vsh_constant_component(58, (method - 0x0AF0) / 4, param);
            break;
        }
        /* The rest of the transform context that shares the constant file:
         * SET_FOG_PLANE is c[57] and SET_EYE_POSITION c[56]. Vertex programs
         * read them like any constant -- SSX 3's shadow-caster program takes
         * its colour from c[57] -- and dropped, they read whatever an earlier
         * constant upload left there: half its shadow maps came out opaque. */
        if (method >= 0x09D0 && method < 0x09E0) {         /* SET_FOG_PLANE */
            nv2a_vsh_constant_component(57, (method - 0x09D0) / 4, param);
            break;
        }
        if (method >= 0x0A50 && method < 0x0A60) {         /* SET_EYE_POSITION */
            nv2a_vsh_constant_component(56, (method - 0x0A50) / 4, param);
            break;
        }
        if (method >= 0x0B00 && method < 0x0B80) {    /* TRANSFORM_PROGRAM(i) */
            nv2a_vsh_program_word(param);
            if (s_vp_load < 136 * 4) { s_vp_words[s_vp_load++] = param; s_vp_dirty = 1; }
            break;
        }
        if (method >= 0x0B80 && method < 0x0C00) {    /* TRANSFORM_CONSTANT(i) */
            nv2a_vsh_constant_word(param);
            break;
        }
        if (method >= NV_TEX_FIRST && method <= NV_TEX_LAST)
            record_tex_reg(method, param);
        if (method >= NV097_SET_VERTEX_DATA_ARRAY_OFFSET
                && method < NV097_SET_VERTEX_DATA_ARRAY_OFFSET + NV_VERTEX_ATTRS * 4) {
            /* Resolved here, once, so every consumer -- the rasteriser's
             * attribute reads and the diagnostics alike -- sees the same
             * address. A vertex array offset is a DMA-object offset exactly
             * like a surface offset: physical, and addressable only through
             * the window when it names contiguous memory. */
            s_gpu.attr[(method - NV097_SET_VERTEX_DATA_ARRAY_OFFSET) / 4].offset =
                dma_resolve(param);
        } else if (method >= NV097_SET_VERTEX_DATA_ARRAY_FORMAT
                && method < NV097_SET_VERTEX_DATA_ARRAY_FORMAT + NV_VERTEX_ATTRS * 4) {
            VertexAttr *a = &s_gpu.attr[(method - NV097_SET_VERTEX_DATA_ARRAY_FORMAT) / 4];
            a->type   =  param        & 0x0F;
            a->size   = (param >> 4)  & 0x0F;
            if (a->size == 7)                   /* SIZE_3W */
                a->size = 3;
            a->stride = (param >> 8)  & 0xFF;
        } else if (!imm_vertex_method(method, param)) {
            note_unhandled(method, param);
        }
        break;
    }
}

/* Find where the title actually wrote its quad.
 *
 * The GPU is pointed at a buffer that stays zero, which says the data went
 * somewhere else -- and the only way to find somewhere else is to look for the
 * data. A screen-space quad for a 640x480 target contains 640.0f and 480.0f as
 * floats, which is a distinctive enough pair to search guest RAM for. Whatever
 * address that turns up is where the title's writes are landing, and the
 * difference from the programmed offset is the bug. */
static void find_quad_vertices(void)
{
    const uint32_t W = 0x44200000u;   /* 640.0f */
    const uint32_t H = 0x43F00000u;   /* 480.0f */
    const uint32_t *ram = (const uint32_t *)xbox_GetMemoryOffset();
    uint32_t i, hits = 0;

    fprintf(stderr, "[GPU] searching guest RAM for 640.0f/480.0f pairs...\n");
    for (i = 0x1000 / 4; i < (0x04000000u / 4) - 8 && hits < 12; i++) {
        if (ram[i] != W && ram[i] != H)
            continue;
        /* Both values within a few words of each other: a lone 640.0f is
         * common, the pair much less so. */
        {
            int has_w = 0, has_h = 0;
            uint32_t k;
            for (k = 0; k < 8; k++) {
                if (ram[i + k] == W) has_w = 1;
                if (ram[i + k] == H) has_h = 1;
            }
            if (!has_w || !has_h)
                continue;
        }
        hits++;
        fprintf(stderr, "  [GPU]   0x%08X:", i * 4);
        {
            uint32_t k;
            for (k = 0; k < 8; k++)
                fprintf(stderr, " %08X", ram[i + k]);
        }
        fprintf(stderr, "\n");
        i += 8;
    }
    if (!hits)
        fprintf(stderr, "  [GPU]   none found -- the quad is not in RAM in"
                        " that form\n");
    fflush(stderr);
}

/* Locate NaN-filled transform matrices in guest RAM.
 *
 * A matrix arriving as NaN says the maths went wrong somewhere upstream, and
 * the only way to find where is to find the matrix and watch who writes it.
 * Three consecutive real-indefinite values is a distinctive enough signature:
 * ordinary data does not contain runs of 0xFFC00000. */
static void find_nan_matrices(void)
{
    const uint32_t NAN_NEG = 0xFFC00000u;
    const uint32_t *ram = (const uint32_t *)xbox_GetMemoryOffset();
    uint32_t i, hits = 0;

    fprintf(stderr, "[GPU] searching guest RAM for NaN matrices...\n");
    for (i = 0x1000 / 4; i < (0x04000000u / 4) - 20 && hits < 10; i++) {
        if (ram[i] != NAN_NEG || ram[i + 1] != NAN_NEG || ram[i + 2] != NAN_NEG)
            continue;
        hits++;
        fprintf(stderr, "  [GPU]   0x%08X:", i * 4);
        {
            uint32_t k;
            for (k = 0; k < 16; k++)
                fprintf(stderr, " %08X", ram[i + k]);
        }
        fprintf(stderr, "\n");
        i += 16;
    }
    if (!hits)
        fprintf(stderr, "  [GPU]   none in RAM -- the NaNs are computed into"
                        " registers, not stored\n");
    fflush(stderr);
}

/* Print guest dwords named by RECOMP_PEEK, as hex and as float.
 *
 * Chasing a value backwards means reading it, and a value that is only wrong
 * for one frame in a thousand cannot be caught by stopping. Both
 * interpretations are printed because the question is usually "is this a
 * pointer or a number", and guessing wrong costs a run. */
static void peek_addresses(void)
{
    const char *spec = getenv("RECOMP_PEEK");
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
    char buf[256];
    char *tok, *ctx = NULL;

    if (!spec)
        return;
    strncpy(buf, spec, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = 0;
    for (tok = strtok_s(buf, ",", &ctx); tok; tok = strtok_s(NULL, ",", &ctx)) {
        uint32_t va = (uint32_t)strtoul(tok, NULL, 0);
        uint32_t v;
        float f;
        if (va < 0x1000u || va >= 0x04000000u)
            continue;
        v = *(const uint32_t *)(mem + va);
        memcpy(&f, &v, 4);
        fprintf(stderr, "  [PEEK] 0x%08X = %08X  (%g)\n", va, v, f);
    }
    fflush(stderr);
}

/* Walk a pointer chain and print every step.
 *
 * RECOMP_PEEK_CHAIN="0x1315A8,8,0x10,0" starts at that address, and for each
 * offset dereferences the current pointer and adds it. Following a chain by
 * hand costs one run per level; this costs one run for the whole chain, and
 * prints where it goes wrong when a level is null.
 */
static void peek_chain(void)
{
    const char *spec = getenv("RECOMP_PEEK_CHAIN");
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
    char buf[256], *tok, *ctx = NULL;
    uint32_t cur = 0;
    int step = 0;

    if (!spec)
        return;
    strncpy(buf, spec, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = 0;

    for (tok = strtok_s(buf, ",", &ctx); tok; tok = strtok_s(NULL, ",", &ctx)) {
        uint32_t off = (uint32_t)strtoul(tok, NULL, 0);
        if (step == 0) {
            cur = off;
            fprintf(stderr, "  [CHAIN] start 0x%08X\n", cur);
        } else {
            if (cur < 0x1000u || cur + 4 >= 0x04000000u) {
                fprintf(stderr, "  [CHAIN] step %d: 0x%08X is not a guest"
                                " pointer -- chain ends\n", step, cur);
                return;
            }
            cur = *(const uint32_t *)(mem + cur) + off;
            fprintf(stderr, "  [CHAIN] step %d: deref +0x%X -> 0x%08X\n",
                    step, off, cur);
        }
        step++;
    }
    if (cur >= 0x1000u && cur + 4 < 0x04000000u)
        fprintf(stderr, "  [CHAIN] final value at 0x%08X = 0x%08X\n",
                cur, *(const uint32_t *)(mem + cur));
    fflush(stderr);
}

void nv2a_pb_exec_report(void)
{
    peek_addresses();
    peek_chain();
    if (getenv("RECOMP_FIND_NAN")) {
        /* Every report, not once: the matrix is fine early on and only turns
         * to NaN later, so a single scan at startup finds nothing and says
         * nothing. */
        find_nan_matrices();
    }
    if (getenv("RECOMP_FIND_QUAD")) {
        static int done;
        if (!done) { done = 1; find_quad_vertices(); }
    }
    int i, j;

    {
        /* Frames per second of guest time, from flips between reports. */
        static uint32_t last_flips;
        static unsigned long last_ms;
        unsigned long now = (unsigned long)(clock() * 1000.0 / CLOCKS_PER_SEC);
        if (last_ms && now > last_ms)
            fprintf(stderr, "[GPU] %.2f fps (%u flips)%c",
                    (s_gpu.flips - last_flips) * 1000.0 / (now - last_ms),
                    s_gpu.flips, 10);
        last_flips = s_gpu.flips;
        last_ms = now;
    }
    flip_hold_report();
    fprintf(stderr, "[GPU] surface 0x%08X pitch %u clip %ux%u+%u+%u"
                    " clears %u | %u unhandled methods (%d distinct)\n",
            s_gpu.color_offset, s_gpu.pitch, s_gpu.clip_w, s_gpu.clip_h,
            s_gpu.clip_x, s_gpu.clip_y, s_gpu.clears,
            s_gpu.unhandled_total, s_unhandled_count);
    fprintf(stderr, "[GPU] draws %u (%u with coordinates), %u indices;"
                    " x %.1f..%.1f  y %.1f..%.1f\n",
            s_gpu.draws, s_gpu.nonzero_draws, s_gpu.verts,
            s_gpu.min_x, s_gpu.max_x, s_gpu.min_y, s_gpu.max_y);
    /* One picture per report rather than per clear: a title clears hundreds of
     * times a second and nobody wants that many files. */
    if (!s_flip_dumping)
        dump_surface_bmp();

    /* Drawn and skipped separately: "nothing appeared" and "every batch needed
     * a vertex program we do not run" look identical on screen, and only one
     * of them means the rasteriser is broken. */
    fprintf(stderr, "[GPU] brightest pixel written 0x%08X\n", s_gpu.pixel_max);
    fprintf(stderr, "[GPU] %llu pixels written; draw surface 0x%08X"
                    " -> 0x%08X, clear surface 0x%08X -> 0x%08X\n",
            (unsigned long long)s_gpu.pixels, s_gpu.drawn_offset,
            dma_resolve(s_gpu.drawn_offset), s_gpu.color_offset,
            dma_resolve(s_gpu.color_offset));
    fprintf(stderr, "[GPU] rasterised %u triangles; %u batches skipped as not"
                    " screen-space, %u triangles fully off-surface\n",
            s_gpu.tris_drawn, s_gpu.batches_untransformed,
            s_gpu.tris_skipped_offscreen);
    fprintf(stderr, "[GPU] vertex programs: %u batches, %u vertices;"
                    " %u triangles dropped behind the eye\n",
            s_gpu.batches_program, s_gpu.verts_program, s_gpu.tris_behind);
    if (s_gpu.batches_ffp)
        fprintf(stderr, "[GPU] fixed-function transform: %u batches\n",
                s_gpu.batches_ffp);
    fprintf(stderr, "[GPU]   of the rest: %u degenerate/NaN, %u off-surface,"
                    " %u drawn; pixels %llu written, %llu depth-failed;"
                    " x %.0f..%.0f y %.0f..%.0f z %g..%g\n",
            s_gpu.xf_degenerate, s_gpu.xf_offscreen, s_gpu.xf_drawn,
            (unsigned long long)s_gpu.xf_pixels,
            (unsigned long long)s_gpu.xf_depth_fail,
            s_gpu.xf_min[0], s_gpu.xf_max[0], s_gpu.xf_min[1], s_gpu.xf_max[1],
            s_gpu.xf_min[2], s_gpu.xf_max[2]);
    {
        fprintf(stderr, "[GPU]   blend pairs (src/dst):");
        for (j = 0; j < s_gpu.blend_npairs; j++)
            fprintf(stderr, " %X/%X", s_gpu.blend_pairs[j] >> 16,
                    s_gpu.blend_pairs[j] & 0xFFFF);
        fputc(10, stderr);
        fprintf(stderr, "[GPU]   program attribute formats (slot:type/size):");
        for (j = 0; j < s_gpu.xf_nfmt; j++)
            fprintf(stderr, " v%u:%u/%u", s_gpu.xf_fmt[j] >> 8,
                    s_gpu.xf_fmt[j] & 15, (s_gpu.xf_fmt[j] >> 4) & 15);
        fprintf(stderr, "\n");
    }

    /* And of the batches that did rasterise, how many sampled anything. A menu
     * that draws its background from one texture and its text from another
     * shows both as flat colour if either half is missing, so the split is
     * what says which half. */
    fprintf(stderr, "[GPU] batches: %u textured, %u with no texcoords,"
                    " %u with texcoords but no usable stage\n",
            s_gpu.batches_textured, s_gpu.batches_no_uv, s_gpu.batches_no_tex);
    for (i = 0; i < s_tex_use_count; i++)
        fprintf(stderr, "  [TEXUSE] 0x%08X %ux%u fmt 0x%02X%s: %u batches\n",
                s_tex_use[i].offset, s_tex_use[i].width, s_tex_use[i].height,
                s_tex_use[i].color,
                d3d8_format_dxt_block_bytes(s_tex_use[i].color) ? " dxt"
                    : d3d8_format_is_swizzled(s_tex_use[i].color) ? " swz" : " lin",
                s_tex_use[i].batches);

    if (getenv("RECOMP_TEX_STATE")) {
        uint32_t k;
        for (k = 0; k < sizeof s_tex_set / sizeof s_tex_set[0]; k++)
            if (s_tex_set[k])
                fprintf(stderr, "  [TEX] 0x%04X = 0x%08X\n",
                        (unsigned)(NV_TEX_FIRST + k * 4), s_tex_reg[k]);
    }

    /* Top ten by frequency: selection sort over a small table, once every few
     * seconds, is not worth a better algorithm.
     *
     * RECOMP_PB_UNHANDLED_ALL lists every one instead. Ten is the right
     * default -- the tail is a long list of state registers nobody needs to
     * read -- but when a title stops and the question is which method it
     * stopped on, the answer is as likely to be the one seen twice as the
     * one seen a thousand times, and ten hides it. */
    {
        int shown = getenv("RECOMP_PB_UNHANDLED_ALL") ? s_unhandled_count : 10;
    for (i = 0; i < shown && i < s_unhandled_count; i++) {
        int best = i;
        for (j = i + 1; j < s_unhandled_count; j++)
            if (s_unhandled[j].count > s_unhandled[best].count)
                best = j;
        if (best != i) {
            PbUnhandled t = s_unhandled[i];
            s_unhandled[i] = s_unhandled[best];
            s_unhandled[best] = t;
        }
        {
            /* The value as well as the count. A method nobody decoded is
             * a guess until you see what it carried: screen coordinates,
             * a 0..1 texcoord and a packed colour are told apart at a
             * glance, and that is what says which vertex encoding a title
             * is using. */
            union { uint32_t u; float f; } v;
            v.u = s_unhandled[i].last_param;
            fprintf(stderr, "  [GPU]   0x%04X x%-8u last=0x%08X (%.4f)\n",
                    s_unhandled[i].method, s_unhandled[i].count,
                    v.u, v.f);
        }
    }
    }
    for (i = 0; i < s_unhandled_count; i++)       /* the sort moved them */
        s_unhandled_slot[(s_unhandled[i].method & 0x1FFC) / 4] = (uint16_t)(i + 1);
    fflush(stderr);
}
