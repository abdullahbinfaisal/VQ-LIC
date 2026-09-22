// ============================================================================
// vq_pq.c - see vq_pq.h. Cortex-A9, bare metal.
//
// Cost per position: VQ_M * VQ_K * VQ_DSUB = 16,384 MAC.
// Whole frame: 14,400 * 16,384 = 235.9 MMAC, i.e. 1.22x the CNN's 193.8 MMAC.
// At 2/4/8 MAC per cycle on a 667 MHz A9 that is 177 / 88 / 44 ms.
//
// The inner loop is deliberately structured so the query subvector stays in
// registers across all 256 codewords: the codebook streams, the query does not.
// That is what makes this memory-friendly - 4 KiB of codebook per subvector
// against 16 bytes of query.
// ============================================================================

#include "vq_pq.h"
#include <string.h>

#if defined(__ARM_NEON) || defined(__ARM_NEON__)
  #include <arm_neon.h>
  #define VQ_HAVE_NEON 1
#else
  #define VQ_HAVE_NEON 0
#endif

// ---------------------------------------------------------------------------
// init: ||c||^2 per codeword
// ---------------------------------------------------------------------------
void vq_pq_init(vq_pq_ctx_t *ctx, const int8_t *codebook, uint8_t zp)
{
    ctx->codebook = codebook;
    ctx->zp       = zp;
    for (int m = 0; m < VQ_M; m++) {
        for (int k = 0; k < VQ_K; k++) {
            const int8_t *c = codebook + ((size_t)m * VQ_K + k) * VQ_DSUB;
            int32_t s = 0;
            for (int d = 0; d < VQ_DSUB; d++) s += (int32_t)c[d] * (int32_t)c[d];
            ctx->norm2[m * VQ_K + k] = s;   // max 16*127^2 = 258,064 -> int32
        }
    }
}

// ---------------------------------------------------------------------------
// Gather one position's VQ_DIM-element vector out of group-major storage and
// subtract the zero point. With zp=128 and uint8 input the result is exactly
// the int8 range, so this cannot overflow.
// ---------------------------------------------------------------------------
static inline void gather_latent(const uint8_t *latent, int g, int l,
                                 uint8_t zp, int8_t *z)
{
    const uint8_t *p = latent + (size_t)g * VQ_DIM * VQ_LANES + l;
    for (int c = 0; c < VQ_DIM; c++) {
        z[c] = (int8_t)((int32_t)p[(size_t)c * VQ_LANES] - (int32_t)zp);
    }
}

// ---------------------------------------------------------------------------
// One subvector search: argmin over VQ_K codewords.
// ---------------------------------------------------------------------------
#if VQ_HAVE_NEON
// ---------------------------------------------------------------------------
// RESTRUCTURED 2026-08-26.  The previous version extracted the dot product to
// an ARM register with vget_lane_s32 INSIDE the k loop -- 256 NEON->core
// transfers per subvector, 14.7 M per frame. On Cortex-A9 the NEON pipe is
// decoupled from the integer pipe, so each transfer stalls ~20 cycles. Measured
// cost: 37.4 cycles per codeword compare, T_VQ = 827 ms/frame.
//
// This version evaluates FOUR codewords per iteration, keeps the running
// minimum and its index in NEON registers, and extracts once at the end --
// 8 transfers per subvector instead of 256.
//
// Lane j accumulates over codewords k == j (mod 4); the final cross-lane
// reduction takes the smaller value, breaking ties toward the smaller index.
// That reproduces the scalar "first strictly-smaller d" rule EXACTLY --
// verified on 40,257 cases including all-equal and every minimum position.
// ---------------------------------------------------------------------------
static inline uint8_t search_sub(const int8_t *z, const int8_t *cb,
                                 const int32_t *norm2)
{
    const int8x16_t vz    = vld1q_s8(z);
    const int8x8_t  vz_lo = vget_low_s8(vz);
    const int8x8_t  vz_hi = vget_high_s8(vz);

    int32x4_t vbest = vdupq_n_s32(0x7FFFFFFF);
    int32x4_t vidx  = vdupq_n_s32(0);
    int32x4_t vk    = (int32x4_t){ 0, 1, 2, 3 };
    const int32x4_t vfour = vdupq_n_s32(4);

    for (int k = 0; k < VQ_K; k += 4) {
        const int8_t *c = cb + (size_t)k * VQ_DSUB;

        /* four independent 16-element int8 dot products */
        int32x4_t s0 = vpaddlq_s16(vmull_s8(vz_lo, vget_low_s8 (vld1q_s8(c))));
        s0 = vpadalq_s16(s0, vmull_s8(vz_hi, vget_high_s8(vld1q_s8(c))));
        int32x4_t s1 = vpaddlq_s16(vmull_s8(vz_lo, vget_low_s8 (vld1q_s8(c + VQ_DSUB))));
        s1 = vpadalq_s16(s1, vmull_s8(vz_hi, vget_high_s8(vld1q_s8(c + VQ_DSUB))));
        int32x4_t s2 = vpaddlq_s16(vmull_s8(vz_lo, vget_low_s8 (vld1q_s8(c + 2 * VQ_DSUB))));
        s2 = vpadalq_s16(s2, vmull_s8(vz_hi, vget_high_s8(vld1q_s8(c + 2 * VQ_DSUB))));
        int32x4_t s3 = vpaddlq_s16(vmull_s8(vz_lo, vget_low_s8 (vld1q_s8(c + 3 * VQ_DSUB))));
        s3 = vpadalq_s16(s3, vmull_s8(vz_hi, vget_high_s8(vld1q_s8(c + 3 * VQ_DSUB))));

        /* fold each int32x4 to a scalar sum, pairwise, without leaving NEON */
        const int32x2_t f0 = vadd_s32(vget_low_s32(s0), vget_high_s32(s0));
        const int32x2_t f1 = vadd_s32(vget_low_s32(s1), vget_high_s32(s1));
        const int32x2_t f2 = vadd_s32(vget_low_s32(s2), vget_high_s32(s2));
        const int32x2_t f3 = vadd_s32(vget_low_s32(s3), vget_high_s32(s3));
        const int32x4_t dots = vcombine_s32(vpadd_s32(f0, f1), vpadd_s32(f2, f3));

        /* d = norm2 - 2*dot, then min-with-index entirely in NEON */
        const int32x4_t d    = vsubq_s32(vld1q_s32(norm2 + k), vshlq_n_s32(dots, 1));
        const uint32x4_t m   = vcltq_s32(d, vbest);
        vbest = vbslq_s32(m, d,  vbest);
        vidx  = vbslq_s32(m, vk, vidx);
        vk    = vaddq_s32(vk, vfour);
    }

    /* single extraction point: 8 transfers instead of 256 */
    int32_t bv[4], bi[4];
    vst1q_s32(bv, vbest);
    vst1q_s32(bi, vidx);
    int32_t v = bv[0]; int32_t idx = bi[0];
    for (int j = 1; j < 4; j++)
        if (bv[j] < v || (bv[j] == v && bi[j] < idx)) { v = bv[j]; idx = bi[j]; }
    return (uint8_t)idx;
}
#else
static inline uint8_t search_sub(const int8_t *z, const int8_t *cb,
                                 const int32_t *norm2)
{
    int32_t best = 0x7FFFFFFF;
    int     best_k = 0;
    for (int k = 0; k < VQ_K; k++) {
        const int8_t *c = cb + (size_t)k * VQ_DSUB;
        int32_t dot = 0;
        for (int d = 0; d < VQ_DSUB; d++) dot += (int32_t)z[d] * (int32_t)c[d];
        const int32_t dd = norm2[k] - 2 * dot;
        if (dd < best) { best = dd; best_k = k; }
    }
    return (uint8_t)best_k;
}
#endif

// ---------------------------------------------------------------------------
// Frame encode. Iterating group-then-lane keeps the 512-byte group resident
// while all 8 of its positions are consumed.
// ---------------------------------------------------------------------------
void vq_pq_encode_frame(const vq_pq_ctx_t *ctx,
                        const uint8_t *latent,
                        uint8_t *idx_out)
{
    int8_t z[VQ_DIM] __attribute__((aligned(16)));

    for (int g = 0; g < VQ_NGROUPS; g++) {
        for (int l = 0; l < VQ_LANES; l++) {
            gather_latent(latent, g, l, ctx->zp, z);

            const int pos = g * VQ_LANES + l;
            uint8_t *o = idx_out + (size_t)pos * VQ_M;
            for (int m = 0; m < VQ_M; m++) {
                o[m] = search_sub(z + m * VQ_DSUB,
                                  ctx->codebook + (size_t)m * VQ_K * VQ_DSUB,
                                  &ctx->norm2[m * VQ_K]);
            }
        }
    }
}

// ---------------------------------------------------------------------------
// B3 measurement and on-target self-check. See header.
// ---------------------------------------------------------------------------
#ifdef VQ_PQ_BENCH
#include <stdio.h>
#include "xtime_l.h"

void vq_pq_bench(const vq_pq_ctx_t *ctx, const uint8_t *latent,
                 uint8_t *idx_out, int nframes)
{
    XTime t0, t1;
    double ms;

    if (nframes < 1) nframes = 1;

    // One warm pass: the codebook is 16 KiB and must be resident before timing,
    // otherwise the first frame measures the fill, not the search.
    vq_pq_encode_frame(ctx, latent, idx_out);

    XTime_GetTime(&t0);
    for (int i = 0; i < nframes; i++)
        vq_pq_encode_frame(ctx, latent, idx_out);
    XTime_GetTime(&t1);

    ms = ((double)(t1 - t0) * 1000.0) /
         ((double)COUNTS_PER_SECOND * (double)nframes);

    printf("[B3] VQ search  %.3f ms/frame over %d frames\n", ms, nframes);
    printf("[B3]   %d positions x %d codebooks x %d codewords x %dD\n",
           VQ_NPOS, VQ_M, VQ_K, VQ_DSUB);
    printf("[B3]   %ld MAC/frame, %.2f MAC/cycle at 667 MHz\n",
           (long)VQ_NPOS * VQ_M * VQ_K * VQ_DSUB,
           ((double)VQ_NPOS * VQ_M * VQ_K * VQ_DSUB) / (ms * 1e-3 * 667e6));
    printf("[B3]   indices out %d B/frame (embeddings never leave the CPU)\n",
           VQ_IDX_BYTES);
}

int vq_pq_selftest(const vq_pq_ctx_t *ctx, const uint8_t *latent,
                   uint8_t *idx_fast, uint8_t *idx_ref)
{
    long bad = 0, first = -1;

    vq_pq_encode_frame(ctx, latent, idx_fast);
    vq_pq_encode_frame_ref(ctx, latent, idx_ref);

    for (long i = 0; i < VQ_IDX_BYTES; i++) {
        if (idx_fast[i] != idx_ref[i]) { if (!bad) first = i; bad++; }
    }
    if (bad == 0) {
        printf("[B3] selftest PASS: fast == reference on all %d indices\n",
               VQ_IDX_BYTES);
    } else {
        printf("[B3] selftest FAIL: %ld/%d mismatch, first at %ld"
               " (fast=%u ref=%u)\n",
               bad, VQ_IDX_BYTES, first,
               idx_fast[first], idx_ref[first]);
    }
    return (bad == 0) ? 0 : -1;
}
#endif // VQ_PQ_BENCH

// ---------------------------------------------------------------------------
// Reference: full ||z-c||^2, no algebraic shortcut, no NEON. Any disagreement
// with vq_pq_encode_frame is a bug in the fast path, not a tie-break: ties are
// broken identically (strict <, ascending k) in both.
// ---------------------------------------------------------------------------
void vq_pq_encode_frame_ref(const vq_pq_ctx_t *ctx,
                            const uint8_t *latent,
                            uint8_t *idx_out)
{
    int8_t z[VQ_DIM];

    for (int g = 0; g < VQ_NGROUPS; g++) {
        for (int l = 0; l < VQ_LANES; l++) {
            gather_latent(latent, g, l, ctx->zp, z);
            const int pos = g * VQ_LANES + l;

            for (int m = 0; m < VQ_M; m++) {
                const int8_t *zs = z + m * VQ_DSUB;
                const int8_t *cb = ctx->codebook + (size_t)m * VQ_K * VQ_DSUB;
                int32_t best = 0x7FFFFFFF;
                int     best_k = 0;
                for (int k = 0; k < VQ_K; k++) {
                    const int8_t *c = cb + (size_t)k * VQ_DSUB;
                    int32_t acc = 0;
                    for (int d = 0; d < VQ_DSUB; d++) {
                        const int32_t diff = (int32_t)zs[d] - (int32_t)c[d];
                        acc += diff * diff;
                    }
                    if (acc < best) { best = acc; best_k = k; }
                }
                idx_out[(size_t)pos * VQ_M + m] = (uint8_t)best_k;
            }
        }
    }
}
