/* GPU vertex programs (default on; RECOMP_GPU_VSH=0 turns them off): the hand-off between the pushbuffer
 * executor (patches/nv2a_pb_exec_rbx.c) and the D3D11 back end
 * (src/rbx_d3d11.c). Included by both, so the layout cannot drift.
 *
 * For a batch whose vertex program is one of the title's known ones, the
 * executor does not transform on the CPU. It gathers each unique vertex's raw
 * inputs (only the attributes that program reads, as float4s), builds the
 * triangle indices, snapshots the constants the program reads, and the back
 * end's vertex shader for that program does the transform, the executor's
 * post-fixes, the back end's conversions and the near-plane clip distances. */
#ifndef RBX_GVS_H
#define RBX_GVS_H

#include <stdint.h>

enum {
    RBX_GVS_OFF = 0,
    RBX_GVS_0B25,     /* world/model transform: v0 v1 v2 */
    RBX_GVS_9CEE,     /* extruded (outline) transform: v0 v1 */
    RBX_GVS_BA9E,     /* pretransformed pass-through: v0 v3 v4 v9 v10 v11 v12 */
    RBX_GVS_892C,     /* morph (two start slots, same words): v0 v2 v4 v5 */
    RBX_GVS_NPROG
};
#define RBX_GVS_MAXIN 7

/* The vertex shader's constant buffer (b2). 16-byte rows, HLSL packing. */
typedef struct {
    float c0[4], c1[4], c58[4], c59[4];
    float cu[15][4];      /* c[96] .. c[110] */
    float aa[4];          /* anti-aliasing scale x, y */
    float fogp[4];        /* fog_param[0], fog_param[1] */
    uint32_t fogm[4];     /* fog mode, fog enable */
} RbxGvsConst;

/* What the CPU path hands the back end for one vertex (RECOMP_GPU_VSH_VERIFY):
 * screen position and w as Nv2aVshOutput has them after the post-fixes, the
 * packed-then-unpacked diffuse, d1, texture coordinates after the t1 <- t0
 * fix-up, and the fog factor. */
typedef struct {
    float pos[4], c[4], d1[4], t[4][4], fog;
} RbxGvsRef;
#define RBX_GVS_VERIFY_N 32

typedef struct {
    int prog;             /* RBX_GVS_*: RBX_GVS_OFF = the batch is not a GPU-program batch */
    uint32_t nin;         /* float4 inputs per vertex */
    const float *in;      /* batch.count vertices x nin float4 */
    int black;            /* every vertex's diffuse rgb packs to 0 (the back end's black-V0 skip) */
    int t0_zero, t1_zero; /* every vertex's oT0 / oT1 xyz is ~0 (before the t1 <- t0 fix-up) */
    uint32_t nref;        /* verify: ref[] holds the CPU path's result for vertices 0 .. nref-1 */
    RbxGvsRef ref[RBX_GVS_VERIFY_N];
    RbxGvsConst k;
} RbxGvs;

extern RbxGvs g_rbx_gvs;
int rbx_gpu_vsh_ok(void);

#endif
