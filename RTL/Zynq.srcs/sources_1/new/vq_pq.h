#ifndef VQ_PQ_H
#define VQ_PQ_H
// ============================================================================
// vq_pq.h - product-quantisation (nearest-codeword) search for the
//           ImageEncoderLite latent, Cortex-A9 bare-metal.
//
// PURPOSE: close the B3 gap. The encoder CNN (B2) is measured; the codeword
// search that turns its 64-channel latent into transmittable indices has only
// ever been modelled (235.9 MMAC/frame, 1.22x the whole CNN). Until B3 is
// measured, 27.6 fps is a component figure and not the codec figure.
//
// OUTPUT IS INDICES ONLY. One uint8 index per (position, codebook):
//   14,400 positions x 4 codebooks = 57,600 B/frame
// versus 921,600 B/frame if embeddings were emitted. The embeddings never
// leave this function.
//
// METHOD: argmin_j ||z - c_j||^2 = argmin_j ( ||c_j||^2 - 2 z.c_j ), since
// ||z||^2 is common to all j. ||c_j||^2 is precomputed once by vq_pq_init, so
// the per-codeword cost is a 16-element dot product and a compare.
//
// LAYOUT: the accelerator writes group-major / channel-minor, so one pixel
// group of 8 holds all C channels in C*8 CONTIGUOUS bytes. Channel c of lane l
// in group g is at  base + (g*C + c)*8 + l. A whole group is 512 B at C=64,
// i.e. 8 cache lines, so gathering is sequential-ish rather than a wild scatter.
// ============================================================================

#include <stdint.h>

#define VQ_DIM     64      // latent channels (= PW block 5 Cout)
#define VQ_M       4       // codebooks / subvectors
#define VQ_DSUB    16      // VQ_DIM / VQ_M
#define VQ_K       256     // codewords per codebook  -> index fits uint8
#define VQ_LANES   8       // pixels per group, matches the accelerator

// 90x160 latent map = 14,400 positions
#define VQ_MAP_W   160
#define VQ_MAP_H   90
#define VQ_NPOS    (VQ_MAP_W * VQ_MAP_H)
#define VQ_NGROUPS (VQ_NPOS / VQ_LANES)
#define VQ_IDX_BYTES (VQ_NPOS * VQ_M)     // 57,600

typedef struct {
    // codebooks[m][k][d], int8, 4*256*16 = 16,384 B -> L1-resident on the A9
    const int8_t *codebook;
    // ||c||^2 per codeword, precomputed. 4*256*4 = 4,096 B
    int32_t       norm2[VQ_M * VQ_K];
    uint8_t       zp;      // zero point the accelerator encoded with (usually 128)
} vq_pq_ctx_t;

// Precompute ||c||^2. Call once at init, not per frame.
// codebook must remain valid for the lifetime of ctx.
void vq_pq_init(vq_pq_ctx_t *ctx, const int8_t *codebook, uint8_t zp);

// Encode one frame. latent is the accelerator's block-5 output
// (group-major/channel-minor, VQ_DIM channels). idx_out receives VQ_IDX_BYTES
// bytes, laid out position-major: idx_out[pos*VQ_M + m].
void vq_pq_encode_frame(const vq_pq_ctx_t *ctx,
                        const uint8_t *latent,
                        uint8_t *idx_out);

// Scalar reference. Same result, no NEON, no unrolling - used to prove the
// fast path byte-for-byte before trusting its timing.
void vq_pq_encode_frame_ref(const vq_pq_ctx_t *ctx,
                            const uint8_t *latent,
                            uint8_t *idx_out);

// ---------------------------------------------------------------------------
// Self-contained B3 measurement. Compiled only when VQ_PQ_BENCH is defined, so
// the host correctness harness does not need the BSP.
//
// Deliberately does its own timing with XTime_GetTime rather than reaching into
// main.c's brackets: B3 is a separate measurement from B2 and mixing them is
// how the cold-pass and "chunk done" brackets got poisoned before. Prints
// ms/frame; add it to B2 to get B4.
//
// latent must point at the encoder's block-5 output (chainB for a 6-pair run,
// since pair 5 is odd). idx_out needs VQ_IDX_BYTES = 57,600 bytes.
// ---------------------------------------------------------------------------
#ifdef VQ_PQ_BENCH
void vq_pq_bench(const vq_pq_ctx_t *ctx, const uint8_t *latent,
                 uint8_t *idx_out, int nframes);
// Compares the NEON/shortcut path against the full-distance reference on the
// real board data. Run this ONCE before trusting any B3 timing - the host test
// can only exercise the scalar path.
int  vq_pq_selftest(const vq_pq_ctx_t *ctx, const uint8_t *latent,
                    uint8_t *idx_fast, uint8_t *idx_ref);
#endif

#endif // VQ_PQ_H
