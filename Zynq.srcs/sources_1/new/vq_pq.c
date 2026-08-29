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
static inline uint8_t search_sub(const int8_t *z, const int8_t *cb,
                                 const int32_t *norm2)
{
    const int8x16_t vz = vld1q_s8(z);          // query held in registers
    const int8x8_t  vz_lo = vget_low_s8(vz);
    const int8x8_t  vz_hi = vget_high_s8(vz);

    int32_t best = 0x7FFFFFFF;
    int     best_k = 0;

    for (int k = 0; k < VQ_K; k++) {
        const int8x16_t vc = vld1q_s8(cb + (size_t)k * VQ_DSUB);
        // int8 x int8 -> int16 is exact (max 16129); 16 of them fit int32.
        int16x8_t p0 = vmull_s8(vz_lo, vget_low_s8(vc));
        int16x8_t p1 = vmull_s8(vz_hi, vget_high_s8(vc));
        int32x4_t acc = vpaddlq_s16(p0);
        acc = vpadalq_s16(acc, p1);
        int32x2_t h = vadd_s32(vget_low_s32(acc), vget_high_s32(acc));
        h = vpadd_s32(h, h);
        const int32_t dot = vget_lane_s32(h, 0);

        // ||z-c||^2 without ||z||^2, which is common to every k
        const int32_t d = norm2[k] - 2 * dot;
        if (d < best) { best = d; best_k = k; }
    }
    return (uint8_t)best_k;
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
