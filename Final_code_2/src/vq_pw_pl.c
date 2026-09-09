// ============================================================================
// vq_pw_pl.c -- see vq_pw_pl.h. AXI DMA in SIMPLE mode (no scatter-gather),
// same as the dedicated block's driver used.
// ============================================================================
#include "vq_pw_pl.h"
#include "xil_io.h"
#include "xil_cache.h"
#include "xil_printf.h"
#include <string.h>

// ---- AXI DMA simple-mode registers -----------------------------------------
#define MM2S_DMACR   0x00u
#define MM2S_DMASR   0x04u
#define MM2S_SA      0x18u
#define MM2S_LENGTH  0x28u
#define S2MM_DMACR   0x30u
#define S2MM_DMASR   0x34u
#define S2MM_DA      0x48u
#define S2MM_LENGTH  0x58u

#define CTRL_START       0x1u
#define CTRL_CLEAR_DONE  0x2u
#define CTRL_CLEAR_ERR   0x4u

#define DMACR_RS     (1u << 0)
#define DMACR_RESET  (1u << 2)
#define DMASR_HALTED (1u << 0)
#define DMASR_IDLE   (1u << 1)
#define DMASR_ERRS   (0x70u)

// ---- PW engine registers ---------------------------------------------------
#define PW_REG_CTRL         0x000u
#define PW_REG_STATUS       0x004u
#define PW_REG_TILE_PIXELS  0x008u
#define PW_REG_CIN_RUN      0x00Cu
#define PW_REG_ZP_RELU      0x010u
#define PW_REG_OC_SEL       0x024u
#define PW_REG_COUT_RUN     0x028u
#define PW_REG_W_BRAM_OFF   0x02Cu
#define PW_REG_STATUS2      0x020u   /* [5]=cfg_err, last start REFUSED  */
#define PW_REG_VQ_CTRL      0x034u   /* [0]=vq_mode, [23:12]=vq_cin_load */
#define PW_REG_VQ_NORM      0x038u   /* [7:0]=abs OC, [31:12]=||v_k||^2  */

/* Sticky bit the core raises instead of running a geometry it would alias.
 * Read it after every start -- it is the only signal that the engine refused
 * the run, and a refused run still pulses done. */
#define PW_STATUS2_CFG_ERR  (1u << 5)
#define PW_REG_W_BASE       0x100u

#define LATENT_BYTES  ((uint32_t)VQPW_NGROUPS * VQPW_DIM * VQPW_LANES)  /* 921,600 */

// Bounded spin so a wedged DMA reports instead of hanging the board.
#define SPIN_LIMIT   200000000u

static inline void pw_w(uint32_t off, uint32_t v) { Xil_Out32(VQ_PW_PL_BASE + off, v); }
static inline uint32_t pw_r(uint32_t off)         { return Xil_In32(VQ_PW_PL_BASE + off); }
static inline void dma_w(uint32_t off, uint32_t v){ Xil_Out32(VQ_PW_PL_DMA_BASE + off, v); }
static inline uint32_t dma_r(uint32_t off)        { return Xil_In32(VQ_PW_PL_DMA_BASE + off); }

// The harness owns the timer; declared here to avoid pulling edge_pipeline.h
// into a file that is otherwise standalone.
extern unsigned long long ep_timer_now(void);
extern double ep_cycles_to_ms(unsigned long long c);

static double s_prog_ms = -1.0;
static double s_run_ms  = -1.0;
static unsigned long long s_t_start = 0;

static vqpw_ctx_t s_ctx;
/* 4,096 B at M=4,K=64,Dsub=16 (no structural zeros); 2,048 B block-diagonal at
 * the legacy M=8,K=16,Dsub=8. Both sized from vq_pw.h, not written out here. */
static int8_t     s_wimg[VQPW_W_BYTES];
static int32_t    s_nimg[VQPW_COUT_TOTAL];     /* 256 or 128 codeword norms */

/* STATUS2 as the engine reports it. Bit 5 is cfg_err -- the sticky flag the
 * core raises instead of running a geometry it would alias. A bitstream built
 * before 2026-09-09 has no such bit and reads 0 here, so a 0 means EITHER the
 * geometry was accepted OR the PL is too old to have an opinion. Use it as a
 * diagnostic hint, never as proof the configuration is right; the thing that
 * proves that is vq_pw_pl_verify(). */
uint32_t vq_pw_pl_status2(void) { return pw_r(PW_REG_STATUS2); }

/* ---------------------------------------------------------------------------
 * BITSTREAM IDENTITY PROBE
 *
 * Answers one question positively rather than by inference: does the PL
 * currently loaded contain the configuration guard, i.e. is it a build from
 * 2026-09-09 or later?
 *
 * WHY INFERENCE IS NOT ENOUGH. An old bitstream running the new firmware
 * produces index mismatches, but so would a new bitstream with a genuine RTL
 * bug, and the two look identical from software: same reload time (the driver
 * writes the same registers either way) and the same search time (the
 * convolution schedule depends on cout_run and N_OC, not on VQ_K, so 8 batches
 * cost 272 cyc/group on both). cfg_err reads 0 on an old build because the bit
 * does not exist, and 0 on a new build that accepted the geometry. Nothing
 * observable during a normal run separates them.
 *
 * HOW. Deliberately program a geometry the NEW engine must refuse:
 * cout_run = 288 needs 9 weight batches and the engine has 8, so the guard
 * fires, the run is refused, and cfg_err latches.
 *
 * WHY IT ARMS THE DMA. The OLD engine has no guard, so it does not refuse --
 * it STARTS. A started run that is never fed stalls in S_LOAD_FIRST with busy
 * stuck high, and the register map has no soft reset to recover it (CTRL bit 2
 * clears sticky FLAGS, not the FSM). So the probe supplies one group of input
 * and a sink for the output, and lets the old engine finish harmlessly. One
 * group is 64 beats in and 4 beats out, about 3 us.
 *
 * Returns  1 = guard present   -> NEW bitstream
 *          0 = guard absent    -> OLD bitstream
 *         <0 = inconclusive    -> engine not responding, verdict unsafe
 * ------------------------------------------------------------------------- */
/* One group of traffic: cin_load beats of 8 bytes in, N_LANES/2 beats out.
 * Contents are irrelevant -- nothing checks them. This exists so that an
 * engine which does NOT refuse a start still reaches S_DONE instead of
 * stalling in S_LOAD_FIRST with busy stuck high, which the register map has
 * no soft reset to recover from. */
static uint8_t s_probe_in [64 * 8] __attribute__((aligned(64)));
static uint8_t s_probe_out[ 4 * 8] __attribute__((aligned(64)));

static void probe_run(uint32_t cout_run)
{
    Xil_DCacheFlushRange((UINTPTR)s_probe_in, sizeof s_probe_in);
    Xil_DCacheInvalidateRange((UINTPTR)s_probe_out, sizeof s_probe_out);

    pw_w(PW_REG_TILE_PIXELS, (uint32_t)VQPW_LANES);   /* exactly one group */
    pw_w(PW_REG_CIN_RUN,     (uint32_t)VQPW_CIN_MAC);
    pw_w(PW_REG_COUT_RUN,    cout_run);
    pw_w(PW_REG_ZP_RELU,     0x00008080u);
    pw_w(PW_REG_VQ_CTRL,     ((uint32_t)VQPW_CIN_LOAD << 12) | 1u);
    pw_w(PW_REG_CTRL,        CTRL_START);

    dma_w(S2MM_DMACR,  DMACR_RS);
    dma_w(S2MM_DA,     (uint32_t)(UINTPTR)s_probe_out);
    dma_w(S2MM_LENGTH, (uint32_t)sizeof s_probe_out);
    dma_w(MM2S_DMACR,  DMACR_RS);
    dma_w(MM2S_SA,     (uint32_t)(UINTPTR)s_probe_in);
    dma_w(MM2S_LENGTH, (uint32_t)sizeof s_probe_in);

    for (uint32_t i = 0; i < 2000000u; i++) {
        if (dma_r(S2MM_DMASR) & DMASR_IDLE) break;
        if (pw_r(PW_REG_STATUS) & 0x1u)     break;    /* done_sticky */
    }

    dma_w(MM2S_DMACR, DMACR_RESET);
    dma_w(S2MM_DMACR, DMACR_RESET);
    for (uint32_t i = 0; i < 100000u; i++)
        if (!(dma_r(MM2S_DMACR) & DMACR_RESET) && !(dma_r(S2MM_DMACR) & DMACR_RESET))
            break;
}

int vq_pw_pl_probe_guard(void)
{
    const uint32_t OVER = (uint32_t)VQPW_PW_COUT_MAX + 32u;   /* 288: 9 batches */
    uint32_t s2;

    if (pw_r(PW_REG_STATUS) == 0xFFFFFFFFu) return -1;

    memset(s_probe_in, 128, sizeof s_probe_in);       /* zp: u = 0 */
    memset(s_probe_out, 0, sizeof s_probe_out);

    /* Start from a clean slate so a pre-existing sticky bit cannot be read as
     * this probe's answer. */
    pw_w(PW_REG_CTRL, CTRL_CLEAR_DONE | CTRL_CLEAR_ERR);
    if (pw_r(PW_REG_STATUS2) & PW_STATUS2_CFG_ERR) {
        pw_w(PW_REG_VQ_CTRL, 0u);
        return -2;                                    /* will not clear */
    }

    /* THE QUESTION: does this engine refuse a geometry it cannot execute? */
    probe_run(OVER);
    s2 = pw_r(PW_REG_STATUS2);

    /* THE CLEAN-UP, and it is not optional.
     *
     * On a bitstream whose cfg_err_sticky is set from the LEVEL rather than
     * its rising edge -- which is every build up to and including
     * pwvq_k64 -- the sticky bit CANNOT be cleared while the core still holds
     * cfg_err high, and the core holds it until the next start it ACCEPTS.
     * Writing clear on its own leaves the bit set, the next real run reads it,
     * and a perfectly good frame is rejected as a refused geometry.
     *
     * So issue a LEGAL run first. That drives the core's cfg_err low, and only
     * then does the clear take. Cheap: one group, about 3 us. */
    probe_run((uint32_t)VQPW_COUT_TOTAL);

    pw_w(PW_REG_VQ_CTRL, 0u);
    pw_w(PW_REG_CTRL, CTRL_CLEAR_DONE | CTRL_CLEAR_ERR);

    if (pw_r(PW_REG_STATUS2) & PW_STATUS2_CFG_ERR) {
        /* Still set after a legal run and a clear. Report rather than let the
         * caller mistake it for a refusal of its own geometry. */
        xil_printf("[VQPW] WARNING: cfg_err would not clear after the probe "
                   "(STATUS2=0x%08x). Runs will be reported as refused.\r\n",
                   (unsigned)pw_r(PW_REG_STATUS2));
        return -3;
    }
    return (s2 & PW_STATUS2_CFG_ERR) ? 1 : 0;
}

double vq_pw_pl_last_prog_ms(void) { return s_prog_ms; }
double vq_pw_pl_last_run_ms(void)  { return s_run_ms;  }

int vq_pw_pl_init(void)
{
    // Prove the engine answers before trusting anything else. STATUS is
    // read-only; a bus fault or a wrong base reads back all-ones here.
    const uint32_t st = pw_r(PW_REG_STATUS);
    if (st == 0xFFFFFFFFu) {
        xil_printf("[VQPW] PW engine not responding at 0x%08x\r\n",
                   (unsigned)VQ_PW_PL_BASE);
        return -1;
    }

    dma_w(MM2S_DMACR, DMACR_RESET);
    dma_w(S2MM_DMACR, DMACR_RESET);
    for (uint32_t i = 0; i < 100000u; i++) {
        if (!(dma_r(MM2S_DMACR) & DMACR_RESET) && !(dma_r(S2MM_DMACR) & DMACR_RESET))
            break;
    }
    if ((dma_r(MM2S_DMACR) & DMACR_RESET) || (dma_r(S2MM_DMACR) & DMACR_RESET)) {
        xil_printf("[VQPW] DMA reset did not clear\r\n");
        return -2;
    }

    // Leave VQ mode off so a convolution run is never surprised by it.
    pw_w(PW_REG_VQ_CTRL, 0u);

    // The derived geometry must be self-consistent before anything is
    // programmed. This catches a VQPW_PROFILE edit that compiled but cannot
    // be executed -- e.g. a c_out beyond the weight batches the IP has.
    {
        const int rc = vqpw_check_build();
        if (rc != 0) {
            xil_printf("[VQPW] build geometry invalid, vqpw_check_build=%d\r\n", rc);
            return -3;
        }
    }
    xil_printf("[VQPW] M=%u K=%u Dsub=%u cin_mac=%u cout=%u batches=%u "
               "score=%ub bits/pos=%u\r\n",
               (unsigned)VQPW_M, (unsigned)VQPW_K, (unsigned)VQPW_DSUB,
               (unsigned)VQPW_CIN_MAC, (unsigned)VQPW_COUT_TOTAL,
               (unsigned)VQPW_NBATCH, (unsigned)VQPW_SCORE_BITS,
               (unsigned)VQPW_BITS_PER_POS);
    return 0;
}

int vq_pw_pl_load_codebook(const int8_t *cb, uint8_t zp)
{
    const unsigned long long t0 = ep_timer_now();

    if (vqpw_init(&s_ctx, cb, zp) != 0) {
        // vqpw_init refuses any zero point but 128: the weight port is signed
        // int8 and v = cq - z0 only fits there. Refusing beats truncating.
        xil_printf("[VQPW] codebook rejected: zero point %u != 128\r\n", (unsigned)zp);
        return -1;
    }
    vqpw_build_weights(&s_ctx, s_wimg);
    vqpw_build_norms(&s_ctx, s_nimg);

    // Weight banks: one per OC slot, VQPW_W_PER_BANK entries each. The engine
    // reads bank oc at address batch*cin_mac + ic, which is exactly how
    // vqpw_build_weights lays the image out.
    for (int oc = 0; oc < VQPW_N_OC; oc++) {
        pw_w(PW_REG_OC_SEL,     (uint32_t)oc);
        pw_w(PW_REG_W_BRAM_OFF, 0u);
        for (int a = 0; a < VQPW_W_PER_BANK; a++) {
            const uint8_t b = (uint8_t)s_wimg[(size_t)oc * VQPW_W_PER_BANK + a];
            pw_w(PW_REG_W_BASE + (uint32_t)a * 4u, (uint32_t)b);
        }
    }

    // Codeword norms, absolute OC order: [7:0] = OC, [31:12] = ||v_k||^2.
    // The ADDRESS field widened 7 -> 8 bits so 256 norms are reachable; the
    // DATA field is still 20 bits signed, which is enough because ||v_k||^2 is
    // at most 16384*Dsub = 262,144 at Dsub = 16. Only the SCORE needed 21 bits.
    for (int i = 0; i < VQPW_COUT_TOTAL; i++) {
        const uint32_t n = ((uint32_t)s_nimg[i] & 0xFFFFFu) << 12;
        pw_w(PW_REG_VQ_NORM, n | (uint32_t)(i & 0xFF));
    }

    s_prog_ms = ep_cycles_to_ms(ep_timer_now() - t0);
    return 0;
}

void vq_pw_pl_cache_prep(const void *latent, void *idx_out, int flush_latent)
{
    if (flush_latent) {
        // Only needed if the CPU has WRITTEN the latent. In the normal cascade
        // it arrives via the PW's own S2MM and the CPU only reads it, so there
        // are no dirty lines -- vq_pl.c measured 40 frames each way, identical
        // indices, and the flush cost 2.938 ms/frame for nothing.
        Xil_DCacheFlushRange((UINTPTR)latent, LATENT_BYTES);
    }
    // The S2MM writes idx_out behind the cache; stale lines would be read back
    // as results. Cheap at 57,600 B and always kept.
    Xil_DCacheInvalidateRange((UINTPTR)idx_out, VQPW_IDX_BYTES);
}

int vq_pw_pl_start(const void *latent, void *idx_out)
{
    // Geometry. cin_run is the MAC window (two sub-codebooks x Dsub), NOT the
    // number of channels streamed -- that is vq_cin_load, which is the whole
    // 64-channel latent vector read once per group.
    pw_w(PW_REG_TILE_PIXELS, (uint32_t)VQPW_NPOS);
    pw_w(PW_REG_CIN_RUN,     (uint32_t)VQPW_CIN_MAC);      /* 16       */
    pw_w(PW_REG_COUT_RUN,    (uint32_t)VQPW_COUT_TOTAL);   /* 256 or 128 */
    pw_w(PW_REG_ZP_RELU,     0x00008080u);                 /* zp_in = zp_out = 128, relu off */

    // VQ mode on. This also switches the engine onto the s_axis_vq/m_axis_vq
    // pair, so it must be set before the DMA moves anything.
    pw_w(PW_REG_VQ_CTRL, ((uint32_t)VQPW_CIN_LOAD << 12) | 1u);

    // START. This RESETS the input FIFO -- see note 1 in the header. Nothing
    // may be in flight yet. Clear the sticky error in the same breath so the
    // read below reports THIS start and not an older one.
    pw_w(PW_REG_CTRL, CTRL_CLEAR_ERR);
    pw_w(PW_REG_CTRL, CTRL_START);

    // The core validates the geometry at start and REFUSES an aliasing one.
    // A refused run still pulses done, so without this check the frame would
    // come back as whatever the buffer already held. Cheap: one AXI read.
    //
    // STATUS2[5] is STICKY -- it answers "was any start refused since the last
    // clear", not "was THIS start refused". Reading it without clearing first
    // makes one old refusal reject every frame thereafter. The clear is issued
    // above, immediately before CTRL_START, so what is read here can only have
    // come from the start just issued.
    if (pw_r(PW_REG_STATUS2) & PW_STATUS2_CFG_ERR) {
        xil_printf("[VQPW] engine REFUSED the geometry: cin=%u cout=%u "
                   "cin_load=%u -- check VQ_K / VQ_NORM_D / COUT_MAX in the "
                   "bitstream against VQPW_PROFILE\r\n",
                   (unsigned)VQPW_CIN_MAC, (unsigned)VQPW_COUT_TOTAL,
                   (unsigned)VQPW_CIN_LOAD);
        pw_w(PW_REG_VQ_CTRL, 0u);
        return -1;
    }

    // Only now arm the DMA. S2MM first so the sink is ready before the source
    // produces; writing LENGTH is what actually starts each channel.
    dma_w(S2MM_DMACR,  DMACR_RS);
    dma_w(S2MM_DA,     (uint32_t)(UINTPTR)idx_out);
    dma_w(S2MM_LENGTH, (uint32_t)VQPW_IDX_BYTES);

    dma_w(MM2S_DMACR,  DMACR_RS);
    dma_w(MM2S_SA,     (uint32_t)(UINTPTR)latent);
    dma_w(MM2S_LENGTH, LATENT_BYTES);

    s_t_start = ep_timer_now();
    return 0;
}

int vq_pw_pl_poll_done(void)
{
    const uint32_t sr = dma_r(S2MM_DMASR);
    if (sr & DMASR_ERRS) {
        xil_printf("[VQPW] S2MM error, DMASR=0x%08x\r\n", (unsigned)sr);
        return -1;
    }
    return (sr & DMASR_IDLE) ? 1 : 0;
}

void vq_pw_pl_finish(void *idx_out)
{
    s_run_ms = ep_cycles_to_ms(ep_timer_now() - s_t_start);
    // Clear VQ mode. Left set, the next convolution would emit onto
    // axi_dma_2 and wait forever for input the DW engine cannot deliver.
    pw_w(PW_REG_VQ_CTRL, 0u);
    Xil_DCacheInvalidateRange((UINTPTR)idx_out, VQPW_IDX_BYTES);
}

int vq_pw_pl_encode_frame(const void *latent, void *idx_out)
{
    vq_pw_pl_cache_prep(latent, idx_out, 0);
    if (vq_pw_pl_start(latent, idx_out) != 0) return -1;
    for (uint32_t i = 0; i < SPIN_LIMIT; i++) {
        const int r = vq_pw_pl_poll_done();
        if (r < 0) { pw_w(PW_REG_VQ_CTRL, 0u); return -1; }
        if (r > 0) { vq_pw_pl_finish(idx_out); return 0; }
    }
    xil_printf("[VQPW] TIMEOUT. MM2S_SR=0x%08x S2MM_SR=0x%08x PW_STATUS=0x%08x\r\n",
               (unsigned)dma_r(MM2S_DMASR), (unsigned)dma_r(S2MM_DMASR),
               (unsigned)pw_r(PW_REG_STATUS));
    pw_w(PW_REG_VQ_CTRL, 0u);
    return -2;
}

/* ---------------------------------------------------------------------------
 * SUB-SYSTEM SELF-TEST
 *
 * The board disagrees with its own RTL simulation on one specific input, and a
 * whole-frame comparison cannot say WHICH part of the engine is wrong. These
 * one-group runs can, because each choice of latent removes a term from
 *
 *     score(k) = ||v_k||^2 - 2 * (u . v_k)
 *
 *   u = 0      (latent 128)  the dot product vanishes for EVERY codeword, so
 *                            score(k) = norm(k) exactly. The argmin then
 *                            depends on the NORM ROM ALONE. If this disagrees,
 *                            the norms the engine holds are not the norms the
 *                            driver wrote, and the MAC is irrelevant.
 *   u = +127   (latent 255)  the failing vector from the board, and the
 *                            largest-magnitude input the datapath can see.
 *   u = -128   (latent 0)    the other extreme, opposite sign.
 *   u = +1     (latent 129)  small magnitude, so score is norm-dominated but
 *                            the MAC still contributes.
 *
 * Read together these separate "the norm ROM is wrong" from "the MAC is wrong"
 * from "only the extreme magnitudes are wrong", which is the difference
 * between a programming fault, a datapath fault and a timing fault.
 *
 * One group is 512 input bytes and 32 output bytes, a few microseconds each.
 * ------------------------------------------------------------------------- */
static uint8_t s_st_lat[64 * 8]  __attribute__((aligned(64)));
static uint8_t s_st_hw [4 * 8]   __attribute__((aligned(64)));
static uint8_t s_st_sw [VQPW_IDX_BYTES];

int vq_pw_pl_selftest(const int8_t *cb, uint8_t zp)
{
    static const uint8_t fills[4] = { 128u, 255u, 0u, 129u };
    static const char   *names[4] = { "u=0   (norm ROM alone)",
                                      "u=+127 (board's failing vector)",
                                      "u=-128",
                                      "u=+1  " };
    int worst = 0;

    if (vqpw_init(&s_ctx, cb, zp) != 0) return -1;

    xil_printf("[VQST] sub-system self-test, one group per case\r\n");

    for (int t = 0; t < 4; t++) {
        long bad_m[VQPW_M];
        for (int m = 0; m < VQPW_M; m++) bad_m[m] = 0;

        for (size_t i = 0; i < sizeof s_st_lat; i++) s_st_lat[i] = fills[t];
        Xil_DCacheFlushRange((UINTPTR)s_st_lat, sizeof s_st_lat);
        Xil_DCacheInvalidateRange((UINTPTR)s_st_hw, sizeof s_st_hw);

        pw_w(PW_REG_TILE_PIXELS, (uint32_t)VQPW_LANES);
        pw_w(PW_REG_CIN_RUN,     (uint32_t)VQPW_CIN_MAC);
        pw_w(PW_REG_COUT_RUN,    (uint32_t)VQPW_COUT_TOTAL);
        pw_w(PW_REG_ZP_RELU,     0x00008080u);
        pw_w(PW_REG_VQ_CTRL,     ((uint32_t)VQPW_CIN_LOAD << 12) | 1u);
        pw_w(PW_REG_CTRL,        CTRL_CLEAR_ERR);
        pw_w(PW_REG_CTRL,        CTRL_START);

        dma_w(S2MM_DMACR,  DMACR_RS);
        dma_w(S2MM_DA,     (uint32_t)(UINTPTR)s_st_hw);
        dma_w(S2MM_LENGTH, (uint32_t)sizeof s_st_hw);
        dma_w(MM2S_DMACR,  DMACR_RS);
        dma_w(MM2S_SA,     (uint32_t)(UINTPTR)s_st_lat);
        dma_w(MM2S_LENGTH, (uint32_t)sizeof s_st_lat);

        for (uint32_t i = 0; i < SPIN_LIMIT; i++)
            if (dma_r(S2MM_DMASR) & DMASR_IDLE) break;

        pw_w(PW_REG_VQ_CTRL, 0u);
        dma_w(MM2S_DMACR, DMACR_RESET);
        dma_w(S2MM_DMACR, DMACR_RESET);
        for (uint32_t i = 0; i < 100000u; i++)
            if (!(dma_r(MM2S_DMACR) & DMACR_RESET)
                && !(dma_r(S2MM_DMACR) & DMACR_RESET)) break;
        Xil_DCacheInvalidateRange((UINTPTR)s_st_hw, sizeof s_st_hw);

        /* The reference over the same one group. vqpw_encode_frame walks whole
         * frames, so run it on a buffer whose first group is this one; only
         * the first 8 positions are compared. */
        vqpw_encode_frame(&s_ctx, s_st_lat, s_st_sw);

        for (int pos = 0; pos < VQPW_LANES; pos++)
            for (int m = 0; m < VQPW_M; m++)
                if (vqpw_get_index(s_st_hw, pos, m)
                    != vqpw_get_index(s_st_sw, pos, m)) bad_m[m]++;

        {
            long tot = 0;
            for (int m = 0; m < VQPW_M; m++) tot += bad_m[m];
            if (tot > worst) worst = (int)tot;
            xil_printf("[VQST]   %s : ", names[t]);
            for (int m = 0; m < VQPW_M; m++)
                xil_printf("m%d=%d ", m, (int)bad_m[m]);
            xil_printf("of %d  -> %s\r\n", VQPW_LANES * VQPW_M,
                       tot ? "MISMATCH" : "ok");
            if (tot) {
                xil_printf("[VQST]     hw[");
                for (int m = 0; m < VQPW_M; m++)
                    xil_printf("%s%d", m ? "," : "",
                               (int)vqpw_get_index(s_st_hw, 0, m));
                xil_printf("] sw[");
                for (int m = 0; m < VQPW_M; m++)
                    xil_printf("%s%d", m ? "," : "",
                               (int)vqpw_get_index(s_st_sw, 0, m));
                xil_printf("]\r\n");
            }
        }
    }

    xil_printf("[VQST] READ IT AS: case 1 wrong -> the NORM ROM the engine "
               "holds is not what was written.\r\n");
    xil_printf("[VQST]              case 1 right, others wrong -> the MAC "
               "(weights or activations).\r\n");
    return worst;
}

long vq_pw_pl_verify(const int8_t *cb, uint8_t zp, const void *latent,
                     uint8_t *pl_idx, uint8_t *sw_idx, long *first_bad)
{
    long bad = 0;
    *first_bad = -1;

    if (vq_pw_pl_load_codebook(cb, zp) != 0) return -1;
    if (vq_pw_pl_encode_frame(latent, pl_idx) != 0) return -1;

    // THE TWO SIDES MUST READ THE SAME BYTES.
    //
    // The engine reads the latent out of DDR by DMA. vqpw_encode_frame() reads
    // it through the CPU data cache. The latent was WRITTEN into DDR by the
    // cascade's own S2MM, which does not go through that cache, so any line the
    // CPU happens to hold from an earlier pass is STALE -- and stale is exactly
    // what a comparison must not be, because the disagreement then belongs to
    // the cache and not to the engine.
    //
    // A stale 32-byte line spans 4 channels x 8 lanes of ONE group, so it
    // corrupts a whole group in all eight lanes and leaves every other group
    // untouched: a scattered, data-dependent, roughly-1% mismatch that looks
    // like a hardware fault and is not one.
    //
    // Invalidate, do not flush: the CPU has no business having written here,
    // and flushing would push stale CPU data over good DMA data.
    Xil_DCacheInvalidateRange((UINTPTR)latent, LATENT_BYTES);

    // Same context, same codebook, same latent -- this compares the engine
    // against vqpw_encode_frame(), the model the RTL bench also checks
    // against, not against a second re-derivation of it.
    vqpw_encode_frame(&s_ctx, (const uint8_t *)latent, sw_idx);

    for (long i = 0; i < (long)VQPW_IDX_BYTES; i++) {
        if (pl_idx[i] != sw_idx[i]) {
            if (*first_bad < 0) *first_bad = i;
            bad++;
        }
    }
    return bad;
}
