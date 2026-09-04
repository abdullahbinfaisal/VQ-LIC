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
#define PW_REG_VQ_CTRL      0x034u   /* [0]=vq_mode, [23:12]=vq_cin_load */
#define PW_REG_VQ_NORM      0x038u   /* [6:0]=abs OC, [31:12]=||v_k||^2  */
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
static int8_t     s_wimg[VQPW_W_BYTES];        /* 2,048 B block-diagonal image */
static int32_t    s_nimg[VQPW_COUT_TOTAL];     /* 128 codeword norms           */

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

    // Codeword norms, absolute OC order: [6:0] = OC, [31:12] = ||v_k||^2.
    // 20 bits signed; the width is derived in vq_pw.c, not assumed.
    for (int i = 0; i < VQPW_COUT_TOTAL; i++) {
        const uint32_t n = ((uint32_t)s_nimg[i] & 0xFFFFFu) << 12;
        pw_w(PW_REG_VQ_NORM, n | (uint32_t)(i & 0x7F));
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
    pw_w(PW_REG_CIN_RUN,     (uint32_t)VQPW_CIN_MAC);      /* 16  */
    pw_w(PW_REG_COUT_RUN,    (uint32_t)VQPW_COUT_TOTAL);   /* 128 */
    pw_w(PW_REG_ZP_RELU,     0x00008080u);                 /* zp_in = zp_out = 128, relu off */

    // VQ mode on. This also switches the engine onto the s_axis_vq/m_axis_vq
    // pair, so it must be set before the DMA moves anything.
    pw_w(PW_REG_VQ_CTRL, ((uint32_t)VQPW_CIN_LOAD << 12) | 1u);

    // START. This RESETS the input FIFO -- see note 1 in the header. Nothing
    // may be in flight yet.
    pw_w(PW_REG_CTRL, 0x1u);

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

long vq_pw_pl_verify(const int8_t *cb, uint8_t zp, const void *latent,
                     uint8_t *pl_idx, uint8_t *sw_idx, long *first_bad)
{
    long bad = 0;
    *first_bad = -1;

    if (vq_pw_pl_load_codebook(cb, zp) != 0) return -1;
    if (vq_pw_pl_encode_frame(latent, pl_idx) != 0) return -1;

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
