/* ============================================================================
 * vq_pw.c -- see vq_pw.h.
 *
 * SCORE WIDTH DERIVATION (Step 6: derived, not assumed)
 *   u = zq - 128 in [-128, 127]   (zq uint8)
 *   v = cq - 128 in [-128, 127]   (cq uint8, same zero point)
 *   DSUB = 8.
 *
 *   per-product   u*v      in [ -16256, +16384 ]   ( -128*127, -128*-128 )
 *   acc = SUM_D   u*v      in [-16256*D, +16384*D]
 *   ||v||^2 = SUM_D v*v    in [       0, +16384*D]
 *   score = ||v||^2 - 2*acc, and per dimension  v^2 - 2uv  is
 *         max  48896  at u = +127, v = -128   (16384 + 2*127*128)
 *         min -16384  at v = u = -128         (16384 - 2*16384)
 *   so over D dimensions
 *         score in [ -16384*D , +48896*D ].
 *
 *   Signed N bits spans [-2^(N-1), 2^(N-1)-1], so N must satisfy
 *   2^(N-1) > 48896*D:
 *
 *     D =  8  (LEGACY   M=8, K=16) : max  391,168 -> 2^19 = 524,288   -> N = 20
 *     D = 16  (DEPLOYED M=4, K=64) : max  782,336 -> 2^20 = 1,048,576 -> N = 21
 *
 *   D = 16 OVERFLOWS the 20-bit width the legacy build used -- this is the one
 *   arithmetic change the new quantiser forces, and it is why VQ_SCORE_W is a
 *   parameter in the RTL rather than a constant. The bound is a function of
 *   DSUB only; K does not enter it.
 *
 *   The 24-bit PW accumulator holds |acc| <= 16384*D = 262,144 at D=16 with
 *   5 bits to spare, so ACC_WIDTH is unaffected.
 *
 *   ||v||^2 alone is 0..16384*D = 262,144 at D=16, still inside the 20-bit
 *   ADDR_VQ_NORM data field. Only the score needed widening.
 *
 *   The reachable minimum is tighter than the algebraic one: score =
 *   ||u-v||^2 - ||u||^2 >= -||u||^2 >= -16384*D. Both bounds are checked at
 *   runtime by the ctx instrumentation, so a violated assumption is loud.
 * ==========================================================================*/
#include "vq_pw.h"

int vqpw_init(vqpw_ctx_t *ctx, const int8_t *codebook, uint8_t zp)
{
    ctx->codebook = codebook;
    ctx->zp       = zp;
    ctx->obs_score_min = ctx->obs_acc_min =  0x7FFFFFFF;
    ctx->obs_score_max = ctx->obs_acc_max = -0x7FFFFFFF;

    /* The signed-INT8 weight port cannot represent cq - z0 unless z0 = 128.
     * Refuse rather than silently truncating the codebook. */
    if (zp != 128u) return -1;

    for (int m = 0; m < VQPW_M; m++) {
        for (int k = 0; k < VQPW_K; k++) {
            const int8_t *v = codebook + ((size_t)m * VQPW_K + k) * VQPW_DSUB;
            int32_t n = 0;
            for (int d = 0; d < VQPW_DSUB; d++) n += (int32_t)v[d] * (int32_t)v[d];
            ctx->norm2[m * VQPW_K + k] = n;
        }
    }
    return 0;
}

/* Reproduces vq_pq.c's gather_latent() rule exactly, including the truncating
 * (int8_t) cast. At zp = 128 on uint8 input the cast never actually wraps. */
void vqpw_gather(const uint8_t *latent, int g, int l, uint8_t zp, int8_t *u)
{
    const uint8_t *p = latent + (size_t)g * VQPW_DIM * VQPW_LANES + l;
    for (int c = 0; c < VQPW_DIM; c++)
        u[c] = (int8_t)((int32_t)p[(size_t)c * VQPW_LANES] - (int32_t)zp);
}

int vqpw_search_sub(vqpw_ctx_t *ctx, const int8_t *u, int m, int32_t *score_out)
{
    const int8_t  *cb = ctx->codebook + (size_t)m * VQPW_K * VQPW_DSUB;
    const int32_t *n2 = ctx->norm2    + (size_t)m * VQPW_K;
    int32_t best = 0x7FFFFFFF;
    int     best_k = 0;

    for (int k = 0; k < VQPW_K; k++) {
        const int8_t *v = cb + (size_t)k * VQPW_DSUB;
        int32_t acc = 0;
        for (int d = 0; d < VQPW_DSUB; d++) acc += (int32_t)u[d] * (int32_t)v[d];
        const int32_t score = n2[k] - 2 * acc;

        if (acc   < ctx->obs_acc_min)   ctx->obs_acc_min   = acc;
        if (acc   > ctx->obs_acc_max)   ctx->obs_acc_max   = acc;
        if (score < ctx->obs_score_min) ctx->obs_score_min = score;
        if (score > ctx->obs_score_max) ctx->obs_score_max = score;
        if (score_out) score_out[k] = score;

        /* STRICT less-than: the lowest codeword index wins every tie. This is
         * the same rule vq_engine.sv's comparator implements, and the RTL VQ
         * branch must match it because k is swept in ascending OC order. */
        if (score < best) { best = score; best_k = k; }
    }
    return best_k;
}

/* First pb_ram entry the batch reads. DSUB per sub-codebook, and the batch's
 * first absolute output channel is batch*N_OC, so the sub-codebook it starts
 * on is (batch*N_OC)/K. Integer division is deliberate: at K > N_OC several
 * consecutive batches map to the SAME sub-codebook and therefore the same
 * window, which is exactly what vq_pb_base does in the RTL. */
int vqpw_batch_window(int batch)
{
    return ((batch * VQPW_N_OC) / VQPW_K) * VQPW_DSUB;
}

/* One rule for both profiles. abs = batch*N_OC + oc is the absolute output
 * channel; m = abs/K and k = abs%K, exactly as the RTL slices vq_abs.
 *
 *   K=16, N_OC=32 : m = 2*batch + oc/16, k = oc%16, window = 16*batch,
 *                   so ic_lo = m*8 - 16*batch = 8*(oc/16)  -- 0 or 8.
 *   K=64, N_OC=32 : m = batch/2, k = 32*(batch%2) + oc, window = 16*(batch/2),
 *                   so ic_lo = m*16 - 16*(batch/2) = 0 always.
 */
int vqpw_map_oc(int batch, int oc, int *k_out, int *ic_lo)
{
    const int abs = batch * VQPW_N_OC + oc;
    const int m   = abs / VQPW_K;
    if (k_out) *k_out = abs % VQPW_K;
    if (ic_lo) *ic_lo = m * VQPW_DSUB - vqpw_batch_window(batch);
    return m;
}

/* ---- index packing ---------------------------------------------------------
 * The transport word is 32 bits little-endian per position at every K; field m
 * sits at bit KW*m. Reading through the assembled word rather than a byte
 * makes this correct for KW that do not divide 8 (KW = 6 straddles bytes). */
uint8_t vqpw_get_index(const uint8_t *idx, int pos, int m)
{
    const uint8_t *p = idx + (size_t)pos * 4;
    const uint32_t w = (uint32_t)p[0]        | ((uint32_t)p[1] <<  8)
                     | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    return (uint8_t)((w >> (VQPW_KW * m)) & VQPW_KMASK);
}

void vqpw_put_index(uint8_t *idx, int pos, int m, uint8_t k)
{
    uint8_t *p = idx + (size_t)pos * 4;
    uint32_t w = (uint32_t)p[0]        | ((uint32_t)p[1] <<  8)
               | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    w &= ~(VQPW_KMASK << (VQPW_KW * m));
    w |= ((uint32_t)k & VQPW_KMASK) << (VQPW_KW * m);
    p[0] = (uint8_t)(w      ); p[1] = (uint8_t)(w >>  8);
    p[2] = (uint8_t)(w >> 16); p[3] = (uint8_t)(w >> 24);
}

/* Every invariant the RTL relies on, checked once so a profile edit that the
 * preprocessor accepted cannot reach the hardware. Negative code = which. */
int vqpw_check_build(void)
{
    if (VQPW_DIM != VQPW_M * VQPW_DSUB)              return -1;
    if ((1 << VQPW_KW) != VQPW_K)                    return -2;
    if (VQPW_BITS_PER_POS > 32)                      return -3;
    if (VQPW_COUT_TOTAL % VQPW_N_OC)                 return -4;
    if (VQPW_COUT_TOTAL > VQPW_PW_COUT_MAX)          return -5;
    if (VQPW_CIN_MAC * VQPW_NBATCH > VQPW_W_PER_BANK * 1) {
        /* weight image must tile the per-bank window exactly */
        if (VQPW_W_PER_BANK != VQPW_NBATCH * VQPW_CIN_MAC) return -6;
    }
    /* the batch windows must tile the loaded channels and stop inside them */
    if (vqpw_batch_window(VQPW_NBATCH - 1) + VQPW_CIN_MAC > VQPW_CIN_LOAD)
        return -7;
    /* the score must fit the width the RTL was elaborated with */
    if (VQPW_SCORE_MAX >= (1L << (VQPW_SCORE_BITS - 1)))  return -8;
    if (VQPW_SCORE_MIN <  -(1L << (VQPW_SCORE_BITS - 1))) return -9;
    /* norms ride a 20-bit signed AXI field regardless of the score width */
    if (16384L * VQPW_DSUB >= (1L << 19))            return -10;
    return 0;
}

void vqpw_encode_frame(vqpw_ctx_t *ctx, const uint8_t *latent, uint8_t *idx_out)
{
    int8_t u[VQPW_DIM];

    for (int g = 0; g < VQPW_NGROUPS; g++) {
        for (int l = 0; l < VQPW_LANES; l++) {
            vqpw_gather(latent, g, l, ctx->zp, u);
            const int pos = g * VQPW_LANES + l;

            /* One search per sub-codebook. The hardware happens to walk the
             * codewords in (batch, oc) order, but the argmin is over the whole
             * sub-codebook either way, so the result is traversal-independent.
             * Field m goes at bit KW*m; bits at and above M*KW stay ZERO, which
             * is what VQ_WORD_MASK guarantees on the RTL side. */
            uint32_t word = 0;
            for (int m = 0; m < VQPW_M; m++) {
                const int k = vqpw_search_sub(ctx, u + m * VQPW_DSUB, m, 0);
                word |= ((uint32_t)k & VQPW_KMASK) << (VQPW_KW * m);
            }
            /* little-endian, matching the ARM and the S2MM byte order */
            uint8_t *o = idx_out + (size_t)pos * 4;
            o[0] = (uint8_t)( word        & 0xFF);
            o[1] = (uint8_t)((word >>  8) & 0xFF);
            o[2] = (uint8_t)((word >> 16) & 0xFF);
            o[3] = (uint8_t)((word >> 24) & 0xFF);
        }
    }
}

void vqpw_build_weights(const vqpw_ctx_t *ctx, int8_t *out)
{
    /* w[oc][b*CIN_MAC + ic], with ic_lo from vqpw_map_oc() saying where in the
     * batch window this output channel's sub-vector starts.
     *
     * K < N_OC (legacy): block diagonal. The low half of the 16-entry window
     *   feeds sub-codebook 2b and the high half feeds 2b+1, so each output
     *   channel zeroes the other half. Those zeros cost nothing in time -- all
     *   32 OCs multiply in parallel regardless -- but they do mean only half
     *   the MAC work is useful.
     *
     * K > N_OC (deployed): ic_lo is 0 and DSUB == CIN_MAC, so the `d` range
     *   test below is true for every ic and NO structural zero is written.
     *   Every weight the MAC reads is a real codeword coefficient. */
    for (int oc = 0; oc < VQPW_N_OC; oc++) {
        for (int b = 0; b < VQPW_NBATCH; b++) {
            int k, ic_lo;
            const int m = vqpw_map_oc(b, oc, &k, &ic_lo);
            const int8_t *v = ctx->codebook + ((size_t)m * VQPW_K + k) * VQPW_DSUB;
            for (int ic = 0; ic < VQPW_CIN_MAC; ic++) {
                const int d = ic - ic_lo;
                out[(size_t)oc * VQPW_W_PER_BANK + b * VQPW_CIN_MAC + ic] =
                    (d >= 0 && d < VQPW_DSUB) ? v[d] : (int8_t)0;
            }
        }
    }
}

void vqpw_build_norms(const vqpw_ctx_t *ctx, int32_t *out)
{
    for (int b = 0; b < VQPW_NBATCH; b++) {
        for (int oc = 0; oc < VQPW_N_OC; oc++) {
            int k;
            const int m = vqpw_map_oc(b, oc, &k, 0);
            out[b * VQPW_N_OC + oc] = ctx->norm2[m * VQPW_K + k];
        }
    }
}
