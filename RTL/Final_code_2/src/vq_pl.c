// ============================================================================
// vq_pl.c -- see vq_pl.h. AXI DMA is driven in SIMPLE mode (no scatter-gather);
// one descriptor moves a whole frame, so there is no per-group host work inside
// the timed region.
// ============================================================================
#include "vq_pl.h"
#include "xil_io.h"
#include "xil_cache.h"

// ---- AXI DMA simple-mode registers -----------------------------------------
#define MM2S_DMACR   0x00u
#define MM2S_DMASR   0x04u
#define MM2S_SA      0x18u
#define MM2S_LENGTH  0x28u
#define S2MM_DMACR   0x30u
#define S2MM_DMASR   0x34u
#define S2MM_DA      0x48u
#define S2MM_LENGTH  0x58u

#define DMACR_RS     (1u << 0)
#define DMACR_RESET  (1u << 2)
#define DMASR_HALTED (1u << 0)
#define DMASR_IDLE   (1u << 1)
#define DMASR_ERRS   (0x70u)          // DMAIntErr | DMASlvErr | DMADecErr

// Latent is VQ_NGROUPS * VQ_DIM * VQ_LANES bytes; indices VQ_IDX_BYTES.
#define LATENT_BYTES ((uint32_t)VQ_NGROUPS * VQ_DIM * VQ_LANES)

// Generous spin bound. At 100 MHz a frame is ~1.85 M cycles; this is ~100x that
// and exists only so a wedged DMA reports instead of hanging the board.
#define SPIN_LIMIT   200000000u

static inline void vq_w(uint32_t off, uint32_t v) { Xil_Out32(VQ_PL_BASE + off, v); }
static inline uint32_t vq_r(uint32_t off)         { return Xil_In32(VQ_PL_BASE + off); }
static inline void dma_w(uint32_t off, uint32_t v){ Xil_Out32(VQ_PL_DMA_BASE + off, v); }
static inline uint32_t dma_r(uint32_t off)        { return Xil_In32(VQ_PL_DMA_BASE + off); }

uint32_t vq_pl_dbg_in(void)  { return vq_r(VQ_REG_DBG_IN);  }
uint32_t vq_pl_dbg_out(void) { return vq_r(VQ_REG_DBG_OUT); }

int vq_pl_init(void)
{
    const uint32_t id = vq_r(VQ_REG_ID);
    if (id != VQ_ID_MAGIC) {
        xil_printf("[VQPL] ID mismatch: read 0x%08x, expected 0x%08x\r\n",
                   (unsigned)id, (unsigned)VQ_ID_MAGIC);
        xil_printf("[VQPL] the loaded bitstream does not contain this block.\r\n");
        return -1;
    }
    const uint32_t cfg = vq_r(VQ_REG_CFG);
    const uint32_t cfg2 = vq_r(VQ_REG_CFG2);
    xil_printf("[VQPL] ID ok. CFG=0x%08x -> QUERY_LANES=%u M=%u DSUB=%u log2K=%u\r\n",
               (unsigned)cfg, (unsigned)(cfg & 0xFF), (unsigned)((cfg >> 8) & 0xFF),
               (unsigned)((cfg >> 16) & 0xFF), (unsigned)((cfg >> 24) & 0xFF));
    xil_printf("[VQPL] CFG2=0x%08x -> GROUP_BUFS=%u\r\n",
               (unsigned)cfg2, (unsigned)(cfg2 & 0xFF));

    // reset both DMA channels
    dma_w(MM2S_DMACR, DMACR_RESET);
    dma_w(S2MM_DMACR, DMACR_RESET);
    for (uint32_t i = 0; i < 1000000u; i++) {
        if (!(dma_r(MM2S_DMACR) & DMACR_RESET) && !(dma_r(S2MM_DMACR) & DMACR_RESET))
            break;
    }
    if ((dma_r(MM2S_DMACR) & DMACR_RESET) || (dma_r(S2MM_DMACR) & DMACR_RESET)) {
        xil_printf("[VQPL] DMA reset did not clear\r\n");
        return -2;
    }
    return 0;
}

void vq_pl_load_codebook(const int8_t *codebook, uint8_t zp, int ngroups)
{
    vq_w(VQ_REG_ZP,      (uint32_t)zp);
    vq_w(VQ_REG_NGROUPS, (uint32_t)ngroups);

    for (int m = 0; m < VQ_M; m++) {
        for (int k = 0; k < VQ_K; k++) {
            const int8_t *c = codebook + ((size_t)m * VQ_K + k) * VQ_DSUB;
            uint32_t w[4] = {0, 0, 0, 0};
            // element d occupies bits [8d +: 8], matching vq_pq_top's in_z
            for (int d = 0; d < VQ_DSUB; d++)
                w[d >> 2] |= ((uint32_t)(uint8_t)c[d]) << (8 * (d & 3));
            vq_w(VQ_REG_CB_D0, w[0]);
            vq_w(VQ_REG_CB_D1, w[1]);
            vq_w(VQ_REG_CB_D2, w[2]);
            vq_w(VQ_REG_CB_D3, w[3]);
            vq_w(VQ_REG_CB_CTRL, (1u << 16) | ((uint32_t)m << 8) | (uint32_t)k);
        }
    }
}

// Cache maintenance ONLY. Split out of vq_pl_start (2026-08-30) because it was
// inside the timed bracket and accounted for the entire apparent 18% shortfall:
// the block's own counter says 1,843,537 cycles (18.435 ms) while the wall
// clock said 21.755 ms. The 3.32 ms difference is this function.
void vq_pl_cache_prep(const void *latent, void *idx_out, int flush_latent)
{
    if (flush_latent) {
        // 921,600 B clean+writeback, ~28,800 lines, measured ~3.3 ms.
        Xil_DCacheFlushRange((UINTPTR)latent, LATENT_BYTES);
    }
    // idx_out is written by the S2MM DMA and read by the CPU afterwards, so
    // stale lines here would be read as results. Cheap (57,600 B) and kept.
    Xil_DCacheInvalidateRange((UINTPTR)idx_out, VQ_IDX_BYTES);
}

int vq_pl_start(const void *latent, void *idx_out)
{
    (void)latent; (void)idx_out;      /* cache work is vq_pl_cache_prep's job */

    vq_w(VQ_REG_CTRL, 0x2u);          // CLRDBG
    vq_w(VQ_REG_CTRL, 0x1u);          // START -- arms framing before data moves

    dma_w(S2MM_DMACR, DMACR_RS);
    dma_w(S2MM_DA,     (uint32_t)(UINTPTR)idx_out);
    dma_w(S2MM_LENGTH, (uint32_t)VQ_IDX_BYTES);   // writing LENGTH starts it

    dma_w(MM2S_DMACR, DMACR_RS);
    dma_w(MM2S_SA,     (uint32_t)(UINTPTR)latent);
    dma_w(MM2S_LENGTH, LATENT_BYTES);
    return 0;
}

int vq_pl_poll_done(void)
{
    const uint32_t sr = dma_r(S2MM_DMASR);
    if (sr & DMASR_ERRS) {
        xil_printf("[VQPL] S2MM error, DMASR=0x%08x\r\n", (unsigned)sr);
        return -1;
    }
    return (sr & DMASR_IDLE) ? 1 : 0;
}

void vq_pl_finish(void *idx_out)
{
    Xil_DCacheInvalidateRange((UINTPTR)idx_out, VQ_IDX_BYTES);
}

int vq_pl_encode_frame(const void *latent, void *idx_out)
{
    /* NO latent flush. Measured 2026-08-30: 40 frames with the flush and 40
     * without produced identical indices (0 mismatches in 2,304,000
     * comparisons) and identical block time (18.6238 ms both ways), while
     * the flush cost 2.938 ms/frame. It is unnecessary because the latent
     * reaches DDR via the PW's S2MM DMA and the CPU only ever READS it, so
     * there are no dirty lines to write back.
     *
     * PRECONDITION: nothing on the CPU may WRITE the latent buffer. If
     * CPU-side post-processing of the latent is ever added, restore the
     * flush -- the PL would otherwise read stale DDR. */
    vq_pl_cache_prep(latent, idx_out, 0);
    vq_pl_start(latent, idx_out);
    for (uint32_t i = 0; i < SPIN_LIMIT; i++) {
        const int r = vq_pl_poll_done();
        if (r < 0) return -1;
        if (r > 0) { vq_pl_finish(idx_out); return 0; }
    }
    xil_printf("[VQPL] TIMEOUT. MM2S_SR=0x%08x S2MM_SR=0x%08x in=%u out=%u\r\n",
               (unsigned)dma_r(MM2S_DMASR), (unsigned)dma_r(S2MM_DMASR),
               (unsigned)vq_pl_dbg_in(), (unsigned)vq_pl_dbg_out());
    return -2;
}

long vq_pl_verify(const vq_pq_ctx_t *ref, const void *latent,
                  uint8_t *pl_idx, uint8_t *sw_idx, long *first_bad, int flush)
{
    long bad = 0;
    int r; unsigned long long guard = 0;
    *first_bad = -1;

    vq_pl_cache_prep(latent, pl_idx, flush);
    if (vq_pl_start(latent, pl_idx) != 0) return -1;
    while ((r = vq_pl_poll_done()) == 0 && ++guard < 200000000u) { }
    if (r <= 0) return -1;
    vq_pl_finish(pl_idx);
    vq_pq_encode_frame_ref(ref, (const uint8_t *)latent, sw_idx);

    for (long i = 0; i < (long)VQ_IDX_BYTES; i++) {
        if (pl_idx[i] != sw_idx[i]) {
            if (*first_bad < 0) *first_bad = i;
            bad++;
        }
    }

    // the block's own counters must agree with the transfer sizes, otherwise a
    // "0 mismatch" could just mean nothing moved
    const uint32_t din  = vq_pl_dbg_in();
    const uint32_t dout = vq_pl_dbg_out();
    const uint32_t exp_in  = LATENT_BYTES / 8u;
    const uint32_t exp_out = (uint32_t)VQ_IDX_BYTES / 8u;
    if (din != exp_in || dout != exp_out) {
        xil_printf("[VQPL] beat count mismatch: in %u/%u out %u/%u\r\n",
                   (unsigned)din, (unsigned)exp_in, (unsigned)dout, (unsigned)exp_out);
        if (bad == 0) bad = -2;      // do not report a clean pass on no data
    }
    return bad;
}

// ---------------------------------------------------------------------------
// Cycle attribution for the last frame. This exists because a first attempt to
// explain an 18% shortfall by input starvation was WRONG: quadrupling the input
// buffer changed the measured time by 0.0001 ms. Rather than guess a second
// time, the block now counts where its cycles go.
// ---------------------------------------------------------------------------
void vq_pl_report_cycles(int ngroups)
{
    const uint32_t w  = vq_r(VQ_REG_DBG_WAIT);
    const uint32_t o  = vq_r(VQ_REG_DBG_OUTFULL);
    const uint32_t e  = vq_r(VQ_REG_DBG_ENGWAIT);
    const uint32_t b  = vq_r(VQ_REG_DBG_BUSY);
    const uint32_t ideal = (uint32_t)ngroups * 1024u;   /* QUERY_LANES=2 */

    xil_printf("[VQCYC] busy    = %u cycles  (ideal %u, excess %u)\r\n",
               (unsigned)b, (unsigned)ideal,
               (unsigned)((b > ideal) ? (b - ideal) : 0));
    xil_printf("[VQCYC] wait    = %u   (input: no full group ready)\r\n", (unsigned)w);
    xil_printf("[VQCYC] outfull = %u   (output FIFO full)\r\n", (unsigned)o);
    xil_printf("[VQCYC] engwait = %u   (normal wait for the engine window)\r\n", (unsigned)e);
    xil_printf("[VQCYC] per group: busy %u  wait %u  outfull %u\r\n",
               (unsigned)(b / (uint32_t)ngroups),
               (unsigned)(w / (uint32_t)ngroups),
               (unsigned)(o / (uint32_t)ngroups));
}
