/* Robotech: project-pinned copy of xboxrecomp src/apu/apu_dsp.c (linked
 * instead of the shared file, see CMakeLists.txt). RBX changes:
 *  - mcpx_apu_dsp_frame folds the mixbins down to stereo the way the
 *    XDK's DirectSound bins are laid out, instead of "even bins left, odd
 *    bins right" (rbx_mixdown below). The even/odd sum played the I3DL2
 *    reverb send and the FX sends dry, at full level, on top of the 3D
 *    voices they were copied from, so positional effects came out several
 *    dB louder than 2D voices (speech, which sits on the front bins only),
 *    and the centre bin played on the left only.
 *  - RECOMP_APU_BINSTAT=1 logs each mixbin's level and headroom every
 *    five seconds ([APU-BIN]). */
/*
 * MCPX APU DSP (GP/EP) - Stub implementation
 *
 * The DSP Global Processor (GP) and Encode Processor (EP) handle effects
 * processing (reverb, chorus, etc.) and final output encoding. The full
 * DSP is ~3000 lines of DSP56300 emulation code.
 *
 * For initial audio, we bypass the DSP entirely:
 * - VP mixbins are passed directly to the EP output
 * - GP effects processing is skipped
 * - The EP just copies mixbin 0/1 (front L/R) to the monitor buffer
 *
 * This gives us basic voice playback without effects. The DSP can be
 * connected later for reverb, EQ, and other processing.
 *
 * Copyright (c) 2012 espes
 * Copyright (c) 2019-2025 Matt Borgerson
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 */

#include "apu_state.h"
#include "fpconv.h"

#include <stdlib.h>
#include <string.h>
#include <math.h>

/* ── DSP command doorbell acknowledgement ────────────────────────────────
 *
 * DirectSound does not stop at creating the device. It hands the audio DSP a
 * command block in guest RAM, writes a command word, and spins until the DSP
 * writes zero back. On real hardware the GP runs a DSP56300 program that does
 * that. Here the DSP is a passthrough stub, so the word never changes and the
 * title hangs inside DirectSound initialisation -- which on Wreckless gates the
 * entire engine, not just audio.
 *
 * RECOMP_APU_DSP_ACK=<addr>[,<addr>...] clears those guest dwords once per APU
 * frame, which is what "the command completed" looks like to the title.
 *
 * RECOMP_APU_DSP_ACK=auto, the default when the variable is unset, finds the
 * doorbell from what the title programs; "0" or "off" turns the ack off.
 * The XDK DSOUND GP program takes its commands through the first page of
 * the GP's scratch space: a 2 KB command FIFO, then a header whose dword at
 * +0x810 is the doorbell the host writes and the DSP program clears (the
 * DSOUND submit of every XDK title seen spins on <block>+0x810). The
 * scratch pages are what NV_PAPU_GPSADDR's scatter-gather table maps -- 8
 * bytes an entry, the physical page in the first dword, as xemu's apu.c
 * scratch_rw walks it -- so the doorbell is SGE[0] + 0x810 wherever the
 * contiguous allocator put the block. A fixed address moves with allocation
 * order (reserving physical page 0 moved one title's by a page); this does
 * not. Wreckless fits too: its doorbell 0x014F8810 is the first page of the
 * scratch its GPSADDR table (0x01504000) maps, not the table itself, which
 * is where the earlier note here looked.
 *
 * ponytail: this is a handshake acknowledgement, not a DSP. It says every
 * command succeeded instantly and computes nothing, so anything whose *result*
 * the title reads back will still be wrong. The real fix is DSP56300 emulation
 * in the GP/EP; this exists so audio init stops blocking everything behind it.
 */
#define APU_DSP_ACK_MAX 8
#define GP_DOORBELL_OFFSET 0x810u
static uint32_t s_dsp_ack[APU_DSP_ACK_MAX];
static int s_dsp_ack_count = -1;
static int s_dsp_ack_auto;                /* =auto: derive from GPSADDR */
static uint32_t s_dsp_ack_auto_page;      /* the page last reported */
static volatile int s_gp_in_reset;        /* last GPRST write held the GP */

static void dsp_ack_init(void)
{
    const char *spec = getenv("RECOMP_APU_DSP_ACK");
    char buf[256], *p, *end;

    s_dsp_ack_count = 0;
    if (!spec || !*spec)
        spec = "auto";                    /* default: derive it */
    if (!strcmp(spec, "0") || !strcmp(spec, "off"))
        return;
    if (!strcmp(spec, "auto")) {
        s_dsp_ack_auto = 1;
        fprintf(stderr, "[APU] DSP doorbell ack: auto (GP scratch page 0"
                        " + 0x%X, from GPSADDR)\n", GP_DOORBELL_OFFSET);
        return;
    }
    strncpy(buf, spec, sizeof buf - 1);
    buf[sizeof buf - 1] = 0;
    for (p = buf; *p && s_dsp_ack_count < APU_DSP_ACK_MAX; ) {
        unsigned long v = strtoul(p, &end, 0);
        if (end == p)
            break;
        if (v)
            s_dsp_ack[s_dsp_ack_count++] = (uint32_t)v;
        p = (*end == ',') ? end + 1 : end;
    }
    if (s_dsp_ack_count)
        fprintf(stderr, "[APU] DSP doorbell ack: %d address(es), first 0x%08X\n",
                s_dsp_ack_count, s_dsp_ack[0]);
}

/* GPRST (GP region + 0xFFFC): the GP runs only with both reset bits set, as
 * in xemu's gp_write. Nothing else models the GP, so this is kept for the
 * =auto ack alone. Before the first write the GP counts as running: a title
 * that never touches GPRST keeps its ack. */
void mcpx_apu_dsp_gprst_write(uint32_t val)
{
    const uint32_t run = NV_PAPU_GPRST_GPRST | NV_PAPU_GPRST_GPDSPRST;
    int held = (val & run) != run;

    if (held != s_gp_in_reset && s_dsp_ack_auto)
        fprintf(stderr, "[APU] GPRST 0x%X: GP %s; DSP doorbell ack %s\n",
                val, held ? "held in reset" : "running",
                held ? "paused" : "resumed");
    s_gp_in_reset = held;
}

/* The doorbell for =auto: SGE[0] of the GP scratch table + 0x810, re-read on
 * every poll, so the slot always lies in the page the table names now. NULL
 * while GPSADDR is unset, the table's first entry names no page, or the GP
 * is held in reset: a title that tears DirectSound down frees the scratch,
 * and a dword cleared there after that would land in whatever reuses the
 * page. */
static uint32_t *dsp_ack_auto_slot(MCPXAPUState *d)
{
    uint32_t sge = qatomic_read(&d->regs[NV_PAPU_GPSADDR]), page;

    if (!sge || s_gp_in_reset)
        return NULL;
    page = *(const uint32_t *)mcpx_apu_phys(sge);
    if (!page || (page & 0xFFF))
        return NULL;                       /* table not filled in yet */
    if (page != s_dsp_ack_auto_page) {
        s_dsp_ack_auto_page = page;
        fprintf(stderr, "[APU] DSP doorbell: GP scratch SGE table 0x%08X maps"
                        " page 0x%08X (%s); doorbell at physical 0x%08X\n",
                sge, page, mcpx_apu_phys_class(page),
                page + GP_DOORBELL_OFFSET);
    }
    return (uint32_t *)(mcpx_apu_phys(page) + GP_DOORBELL_OFFSET);
}

void mcpx_apu_dsp_ack_poll(MCPXAPUState *d)
{
    int i;

    if (s_dsp_ack_count < 0)
        dsp_ack_init();
    if (!d->ram_ptr)
        return;
    if (s_dsp_ack_auto) {
        uint32_t *slot = dsp_ack_auto_slot(d);
        if (slot && *slot) {
            static int shown;
            if (shown++ < 3)
                fprintf(stderr, "[APU] DSP doorbell (auto): command 0x%08X"
                                " acknowledged\n", *slot);
            *slot = 0;
        }
    }
    for (i = 0; i < s_dsp_ack_count; i++) {
        uint32_t *slot = (uint32_t *)(d->ram_ptr + s_dsp_ack[i]);
        if (*slot) {
            static int shown[APU_DSP_ACK_MAX];
            if (shown[i]++ < 3)
                fprintf(stderr, "[APU] DSP doorbell 0x%08X: command 0x%08X"
                                " acknowledged\n", s_dsp_ack[i], *slot);
            *slot = 0;
        }
    }
}

void mcpx_apu_dsp_init(MCPXAPUState *d)
{
    /* The doorbell ack's state is file-static, so it outlives an instance:
     * a GP the last instance held in reset would leave this one's ack
     * paused, and the RECOMP_APU_DSP_ACK setting would never be read again.
     * Start from what a fresh process has. */
    s_dsp_ack_count = -1;                  /* re-read on the first poll */
    s_dsp_ack_auto = 0;
    s_dsp_ack_auto_page = 0;
    s_gp_in_reset = 0;

    /* Allocate minimal DSP state for GP and EP.
     * We need these to exist so reset doesn't crash,
     * but they won't actually run DSP programs. */
    d->gp.dsp = (DSPState *)calloc(1, sizeof(DSPState));
    d->ep.dsp = (DSPState *)calloc(1, sizeof(DSPState));

    if (d->gp.dsp) d->gp.dsp->is_gp = true;
    if (d->ep.dsp) d->ep.dsp->is_gp = false;

    d->gp.realtime = false;
    d->ep.realtime = false;

    fprintf(stderr, "[APU] DSP GP/EP initialized (STUBBED - passthrough mode)\n");
}

/* Called after the frame thread is joined. Only mcpx_apu_reset_locked
 * (currently uncalled) reads this state, and it would run on that thread.
 * Safe to call twice. */
void mcpx_apu_dsp_finalize(MCPXAPUState *d)
{
    free(d->gp.dsp);
    free(d->ep.dsp);
    d->gp.dsp = NULL;
    d->ep.dsp = NULL;
}

void mcpx_apu_update_dsp_preference(MCPXAPUState *d)
{
    /* In the real xemu, this reads settings to decide whether
     * GP/EP should run in realtime or cached mode. We ignore it. */
    (void)d;
}

/* ── Stereo fold-down of the mixbins ─────────────────────────────────────
 *
 * The bins as the XDK's DirectSound lays them out (DSMIXBIN_*):
 *    0 front L   1 front R   2 centre    3 LFE       4 back L    5 back R
 *    6 XTLK FL   7 XTLK FR   8 XTLK BL   9 XTLK BR  (3D voices, via HRTF)
 *   10 I3DL2 reverb send   11..30 FXSEND_0..19   31 unused
 *
 * On hardware the GP's DSP image runs the crosstalk canceller over 6..9,
 * the reverb over 10 and the title's effects over the sends, and mixes their
 * outputs into the speaker feeds. The GP is a stub here, so this is the
 * nearest thing to it: speaker and crosstalk bins folded down with the
 * usual -3 dB for centre and rears, and the sends (an effect's *input*, not
 * its output) mixed in mono well below the dry signal, standing in for the
 * reverb's wet return.
 *
 * RECOMP_APU_MIXDOWN=stereo (default) | all (the toolkit's even/odd sum) |
 * front (bins 0/1 only). RECOMP_APU_SEND_GAIN=<0..1> sets the send level
 * (default 0.18, about -15 dB). RECOMP_APU_HEADROOM_RESTORE=1 multiplies each
 * bin back up by the headroom the title asked the VP to leave in it, as the
 * DSP image's output stage does. */
enum { RBX_MIX_STEREO, RBX_MIX_ALL, RBX_MIX_FRONT };
static int s_rbx_mix = -1;
static float s_rbx_send_gain = 0.18f;
static int s_rbx_hr_restore;
static int s_rbx_binstat;

static void rbx_mixdown_init(void)
{
    const char *e = getenv("RECOMP_APU_MIXDOWN");
    s_rbx_mix = RBX_MIX_STEREO;
    if (e && !strcmp(e, "all")) s_rbx_mix = RBX_MIX_ALL;
    else if (e && !strcmp(e, "front")) s_rbx_mix = RBX_MIX_FRONT;
    /* The toolkit's escape hatch keeps its meaning: =0 is front only. */
    e = getenv("RECOMP_APU_MIXDOWN_ALL");
    if (e && *e && !getenv("RECOMP_APU_MIXDOWN"))
        s_rbx_mix = atoi(e) ? RBX_MIX_ALL : RBX_MIX_FRONT;
    e = getenv("RECOMP_APU_SEND_GAIN");
    if (e && *e) {
        float g = (float)atof(e);
        s_rbx_send_gain = g < 0.0f ? 0.0f : g > 1.0f ? 1.0f : g;
    }
    e = getenv("RECOMP_APU_HEADROOM_RESTORE");
    s_rbx_hr_restore = e && *e && atoi(e) != 0;
    e = getenv("RECOMP_APU_BINSTAT");
    s_rbx_binstat = e && *e && atoi(e) != 0;
    fprintf(stderr, "[APU] mixdown: %s, send gain %.2f, headroom restore %s\n",
            s_rbx_mix == RBX_MIX_STEREO ? "stereo fold-down"
            : s_rbx_mix == RBX_MIX_ALL ? "all bins even/odd" : "front bins only",
            s_rbx_send_gain, s_rbx_hr_restore ? "on" : "off");
}

/* Per-bin left/right weights for the stereo fold-down. */
#define RBX_M3DB 0.70710678f
static const float s_fold[10][2] = {
    { 1.0f, 0.0f },          /* 0 front L */
    { 0.0f, 1.0f },          /* 1 front R */
    { RBX_M3DB, RBX_M3DB },  /* 2 centre */
    { 0.5f, 0.5f },          /* 3 LFE */
    { RBX_M3DB, 0.0f },      /* 4 back L */
    { 0.0f, RBX_M3DB },      /* 5 back R */
    { 1.0f, 0.0f },          /* 6 XTLK front L */
    { 0.0f, 1.0f },          /* 7 XTLK front R */
    { RBX_M3DB, 0.0f },      /* 8 XTLK back L */
    { 0.0f, RBX_M3DB },      /* 9 XTLK back R */
};

/* [APU-BIN]: RMS of each bin over the window, in dBFS, with its headroom. */
static double s_bin_sq[NUM_MIXBINS];
static unsigned s_bin_n;

static void rbx_binstat(MCPXAPUState *d,
                        float mixbins[NUM_MIXBINS][NUM_SAMPLES_PER_FRAME])
{
    char line[1024];
    int len = 0;

    for (int b = 0; b < NUM_MIXBINS; b++)
        for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++)
            s_bin_sq[b] += (double)mixbins[b][i] * mixbins[b][i];
    s_bin_n += NUM_SAMPLES_PER_FRAME;
    if (s_bin_n < 48000u * 5)
        return;
    for (int b = 0; b < NUM_MIXBINS; b++) {
        double rms = sqrt(s_bin_sq[b] / s_bin_n);
        if (rms > 1e-5)
            len += snprintf(line + len, sizeof(line) - len, " b%d=%.1fdB/hr%u",
                            b, 20.0 * log10(rms), d->vp.submix_headroom[b]);
        s_bin_sq[b] = 0;
    }
    s_bin_n = 0;
    fprintf(stderr, "[APU-BIN] hrtf_hr=%u hrtf_bins=%u/%u/%u/%u%s\n",
            d->vp.hrtf_headroom, d->vp.hrtf_submix[0], d->vp.hrtf_submix[1],
            d->vp.hrtf_submix[2], d->vp.hrtf_submix[3],
            len ? line : " (silent)");
}

void mcpx_apu_dsp_frame(MCPXAPUState *d,
                         float mixbins[NUM_MIXBINS][NUM_SAMPLES_PER_FRAME])
{
    int off = (d->ep_frame_div % 8) * NUM_SAMPLES_PER_FRAME;
    float w[NUM_MIXBINS][2];

    if (s_rbx_mix < 0)
        rbx_mixdown_init();
    if (s_rbx_binstat)
        rbx_binstat(d, mixbins);

    for (int b = 0; b < NUM_MIXBINS; b++) {
        float hr = s_rbx_hr_restore ? (float)(1 << d->vp.submix_headroom[b]) : 1.0f;
        switch (s_rbx_mix) {
        case RBX_MIX_ALL:
            w[b][0] = (b & 1) ? 0.0f : 1.0f;
            w[b][1] = (b & 1) ? 1.0f : 0.0f;
            break;
        case RBX_MIX_FRONT:
            w[b][0] = b == 0 ? 1.0f : 0.0f;
            w[b][1] = b == 1 ? 1.0f : 0.0f;
            break;
        default:
            if (b < 10) {
                w[b][0] = s_fold[b][0];
                w[b][1] = s_fold[b][1];
            } else {
                w[b][0] = w[b][1] = s_rbx_send_gain * RBX_M3DB;
            }
            break;
        }
        w[b][0] *= hr;
        w[b][1] *= hr;
    }

    if (d->monitor.point != MCPX_APU_DEBUG_MON_VP) {
        for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++) {
            float left = 0.0f, right = 0.0f;
            for (int b = 0; b < NUM_MIXBINS; ++b) {
                float s = mixbins[b][i];
                if (s == 0.0f)
                    continue;
                left += w[b][0] * s;
                right += w[b][1] * s;
            }
            /* Clamp to [-1, 1] range */
            if (left > 1.0f) left = 1.0f;
            if (left < -1.0f) left = -1.0f;
            if (right > 1.0f) right = 1.0f;
            if (right < -1.0f) right = -1.0f;

            /* Write (not accumulate) into the frame buffer: each of the 8
             * sub-frames writes its own 32-sample slice. */
            d->monitor.frame_buf[off + i][0] = (int16_t)(left * 32767.0f);
            d->monitor.frame_buf[off + i][1] = (int16_t)(right * 32767.0f);
        }
    }

    g_dbg.gp.cycles = 0;
    g_dbg.ep.cycles = 0;
}
