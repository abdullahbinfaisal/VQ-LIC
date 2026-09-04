#ifndef VQ_PW_PL_H
#define VQ_PW_PL_H
/* ============================================================================
 * vq_pw_pl.h -- driver for VQ running on the SHARED pointwise engine.
 *
 * Replaces vq_pl.c, which drove the dedicated vq_pq_axi_0 block at
 * 0x43C20000. That block was deleted from the design when VQ moved onto the
 * PW engine (commit 62cbfbc); axi_dma_2 now feeds pw_single_oc_axis_axi_0's
 * s_axis_vq and drains its m_axis_vq.
 *
 * CONFIGURATION (see vq_pw.h): M = 8 sub-codebooks, K = 16 codewords,
 * Dsub = 8, so one output-channel batch of Q = 32 holds TWO sub-codebooks and
 * four batches cover all eight. 32 bits per latent position, unchanged.
 *
 * ------------------------------------------------------------------------
 * THREE THINGS THAT ARE NOT OBVIOUS FROM THE REGISTER MAP
 * ------------------------------------------------------------------------
 * 1. START BEFORE THE DMA. Writing CTRL bit 0 resets the engine's input FIFO
 *    (pw_single_oc_axis.sv:88, deliberate -- a run must not inherit beats
 *    from an aborted predecessor). Anything the DMA has already pushed is
 *    flushed. Kicking MM2S first costs you the beats in flight and the search
 *    then runs on a latent shifted by however many were buffered, while every
 *    programmed register still reads back correct. Verified in
 *    tb_pw_axi_vq.sv: 9 beats lost that way, and every output wrong.
 *
 * 2. THE WEIGHT BRAM IS SHARED WITH CONVOLUTION. The codebook occupies the
 *    same per-OC weight banks the analysis transform uses, so it CANNOT be
 *    loaded once per model -- the next convolution overwrites it, and the
 *    next VQ overwrites the convolution's. vq_pw_pl_load_codebook() therefore
 *    runs once per frame, and its cost is reported separately by
 *    vq_pw_pl_last_prog_ms(). That reload is a real price of sharing the
 *    engine and belongs in any latency accounting.
 *
 * 3. VQ MODE MUST BE CLEARED. The mode bit also selects which AXIS pair the
 *    engine talks on. Leaving it set sends the next convolution's output to
 *    axi_dma_2 and starves it of input from the DW engine.
 *    vq_pw_pl_finish() clears it.
 * ==========================================================================*/

#include <stdint.h>
#include "vq_pw.h"

#define VQ_PW_PL_BASE       0x43C10000u   /* pw_single_oc_axis_axi_0        */
#define VQ_PW_PL_DMA_BASE   0x40420000u   /* axi_dma_2, MM2S in / S2MM out  */

/* Probe the engine and reset both DMA channels. 0 on success. */
int  vq_pw_pl_init(void);

/* Build the block-diagonal weight image and the codeword-norm table from
 * `cb` (pre-centred int8, M*K*Dsub) and program both over AXI-lite.
 * MUST be called before every frame -- see note 2 above. 0 on success,
 * <0 if vqpw_init() rejects the zero point. */
int  vq_pw_pl_load_codebook(const int8_t *cb, uint8_t zp);

/* Cache maintenance split out so it can be timed separately from the
 * accelerator, exactly as vq_pl.c did. */
void vq_pw_pl_cache_prep(const void *latent, void *idx_out, int flush_latent);

/* Non-blocking. start() programs geometry, enters VQ mode, starts the engine
 * and only THEN kicks the DMA. */
int  vq_pw_pl_start(const void *latent, void *idx_out);
int  vq_pw_pl_poll_done(void);     /* 1 done, 0 busy, <0 error */
void vq_pw_pl_finish(void *idx_out);

/* Blocking: cache_prep + start + spin + finish. */
int  vq_pw_pl_encode_frame(const void *latent, void *idx_out);

/* Compare the engine against vqpw_encode_frame() on the same latent.
 * Returns the mismatch count, or <0 on a hardware fault. */
long vq_pw_pl_verify(const int8_t *cb, uint8_t zp, const void *latent,
                     uint8_t *pl_idx, uint8_t *sw_idx, long *first_bad);

/* Timing of the most recent frame, milliseconds. */
double vq_pw_pl_last_prog_ms(void);   /* codebook + geometry reload        */
double vq_pw_pl_last_run_ms(void);    /* start -> DMA idle, accelerator    */

#endif /* VQ_PW_PL_H */
