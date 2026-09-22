// ============================================================================
// vq_pl.h -- driver for the PL product-quantisation search block.
//
// Replaces the Cortex-A9 NEON path (vq_pq.c) with the hardware block:
//   vq_pq_axi_0 @ 0x43C20000   control + status
//   axi_dma_2   @ 0x40420000   MM2S latent in, S2MM indices out, simple mode
//
// The block is bit-exact with vq_pq.c by construction and by regression: the
// same golden vectors check both, including a tie-storm scenario where every
// search has co-minimal candidates. vq_pl_verify() re-establishes that on the
// real device before any timing is quoted.
//
// USAGE
//   vq_pl_init();                         // checks ID/CFG, resets the DMA
//   vq_pl_load_codebook(cb, zp, ngroups); // once per model, not timed
//   vq_pl_encode_frame(latent, idx);      // blocking, this is T_VQ(PL)
//
// CONCURRENCY
//   vq_pl_start() / vq_pl_poll_done() expose the non-blocking form. The block
//   runs autonomously in PL on HP2 once started, so frame N's search can
//   overlap frame N+1's analysis on HP0/HP1.
//
//   !! The caller MUST double-buffer the latent. In the current cascade the
//   final PW output lands in chainB == DDR_SKIP2_ADDR, which is exactly what
//   edge_latent_ptr() returns. Starting frame N+1's analysis while frame N's
//   search is still reading that buffer corrupts the search. Alternate chainB
//   between two regions (it peaks at 1.84 MB; DDR_SKIP2 is 48 MB).
// ============================================================================
#ifndef VQ_PL_H
#define VQ_PL_H

#include <stdint.h>
#include "vq_pq.h"          // VQ_M, VQ_K, VQ_DSUB, VQ_NGROUPS, VQ_IDX_BYTES

#define VQ_PL_BASE        0x43C20000u
#define VQ_PL_DMA_BASE    0x40420000u

// ---- vq_pq_axi register map (see vq_pq_axi.sv) ----
#define VQ_REG_CTRL       0x00u   // W1P: bit0 START, bit1 CLRDBG
#define VQ_REG_STATUS     0x04u   // RO : bit0 BUSY, bit1 DONE, bit2 S_RDY, bit3 M_VLD
#define VQ_REG_ZP         0x08u
#define VQ_REG_NGROUPS    0x0Cu
#define VQ_REG_CB_D0      0x10u
#define VQ_REG_CB_D1      0x14u
#define VQ_REG_CB_D2      0x18u
#define VQ_REG_CB_D3      0x1Cu
#define VQ_REG_CB_CTRL    0x20u   // [7:0] k, [9:8] sub-codebook, bit16 WRITE
#define VQ_REG_DBG_IN     0x24u
#define VQ_REG_DBG_OUT    0x28u
#define VQ_REG_ID         0x2Cu
#define VQ_REG_CFG        0x30u
#define VQ_REG_CFG2       0x34u   // [7:0] GROUP_BUFS
#define VQ_REG_DBG_WAIT   0x38u   // cycles stalled: no full input group
#define VQ_REG_DBG_OUTFULL 0x3Cu  // cycles stalled: output FIFO full
#define VQ_REG_DBG_ENGWAIT 0x40u  // cycles waiting on engine ready
#define VQ_REG_DBG_BUSY   0x44u   // total cycles, start to last beat

#define VQ_ID_MAGIC       0x56513034u   // "VQ04" -- cycle-attribution counters

#define VQ_STATUS_BUSY    (1u << 0)
#define VQ_STATUS_DONE    (1u << 1)

// Returns 0 on success. Fails if the ID register does not match, which is the
// cheap guard against a stale bitstream answering for a fresh one.
int  vq_pl_init(void);

// Push all VQ_M * VQ_K codewords plus zp and the frame length. Untimed.
void vq_pl_load_codebook(const int8_t *codebook, uint8_t zp, int ngroups);

// Blocking encode. `latent` and `idx_out` are physical addresses in DDR.
// Returns 0 on success, negative on DMA timeout.
int  vq_pl_encode_frame(const void *latent, void *idx_out);

// Cache maintenance, split out of vq_pl_start so it can be timed separately.
// It is NOT accelerator work. Measured at 3.32 ms for the 921,600 B latent,
// which is exactly why T_VQ_PL read 21.75 ms while the block's own cycle
// counter said 18.435 ms.
//
// flush_latent selects whether the latent is cleaned before the PL reads it.
// It is very likely unnecessary: the latent reaches DDR via the PW's S2MM DMA
// and is only ever READ by the CPU, so there are no dirty lines to write back.
// The test is self-verifying -- if coherency really were needed, the PL
// (reading DDR) and NEON (reading cache) would diverge and the index
// comparison would fail. Do not drop the flush without that check passing.
void vq_pl_cache_prep(const void *latent, void *idx_out, int flush_latent);

// Non-blocking form, for overlapping with the next frame's analysis.
// Performs NO cache maintenance -- call vq_pl_cache_prep first.
int  vq_pl_start(const void *latent, void *idx_out);
int  vq_pl_poll_done(void);      // 1 = complete, 0 = still running, <0 = error
void vq_pl_finish(void *idx_out); // cache-invalidate the index buffer

// Compare the PL result against vq_pq.c on the real device. Returns the
// mismatch count; 0 is the only acceptable answer.
long vq_pl_verify(const vq_pq_ctx_t *ref, const void *latent,
                  uint8_t *pl_idx, uint8_t *sw_idx, long *first_bad, int flush);

uint32_t vq_pl_dbg_in(void);
uint32_t vq_pl_dbg_out(void);

// Print the cycle attribution for the last frame. dbg_busy should equal
// wait + outfull + engwait + productive, so the residual is the search itself.
void vq_pl_report_cycles(int ngroups);

#endif // VQ_PL_H
