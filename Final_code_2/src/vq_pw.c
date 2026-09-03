/* ============================================================================
 * vq_pw.c -- see vq_pw.h.
 *
 * SCORE WIDTH DERIVATION (Step 6: derived, not assumed)
 *   u = zq - 128 in [-128, 127]   (zq uint8)
 *   v = cq - 128 in [-128, 127]   (cq uint8, same zero point)
 *   DSUB = 8.
 *
 *   per-product   u*v      in [ -16256, +16384 ]   ( -128*127, -128*-128 )
 *   acc = SUM_8   u*v      in [-130048, +131072 ]   -> 19 bits signed
 *   ||v||^2 = SUM_8 v*v    in [      0, +131072 ]   -> 18 bits unsigned
 *   score = ||v||^2 - 2*acc
 *         max: 131072 - 2*(-130048) = +391168     (v all -128, u all +127)
 *         min:      0 - 2*( 131072) = -262144     (v all 0,    u.v maximal)
 *
 *   Signed N bits spans [-2^(N-1), 2^(N-1)-1]. +391168 needs 2^(N-1) > 391168,
 *   i.e. N-1 >= 19 (2^19 = 524288), so N = 20. -262144 fits with room.
 *   => 20-bit signed is the minimum safe score width. The 24-bit PW
 *      accumulator holds acc (19 bits) with 5 bits to spare.
 *
 *   The reachable minimum is tighter than the algebraic one: score =
 *   ||u-v||^2 - ||u||^2 >= -||u||^2 >= -131072. Both bounds are checked at
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

int vqpw_map_oc(int batch, int oc, int *k_out, int *ic_lo)
{
    const int half = oc / VQPW_K;                 /* 0 or 1 */
    const int m    = batch * VQPW_SUBS_PER_BATCH + half;
    if (k_out) *k_out = oc % VQPW_K;
    if (ic_lo) *ic_lo = half * VQPW_DSUB;         /* 0 or 8, within the window */
    return m;
}

void vqpw_encode_frame(vqpw_ctx_t *ctx, const uint8_t *latent, uint8_t *idx_out)
{
    int8_t u[VQPW_DIM];

    for (int g = 0; g < VQPW_NGROUPS; g++) {
        for (int l = 0; l < VQPW_LANES; l++) {
            vqpw_gather(latent, g, l, ctx->zp, u);
            const int pos = g * VQPW_LANES + l;

            /* Walk the sub-codebooks in HARDWARE order (batch, then oc) so the
             * model exercises the same traversal the RTL will. The result is
             * order-independent, but driving the RTL testbench from this loop
             * keeps the two in step. */
            uint32_t word = 0;
            for (int b = 0; b < VQPW_NBATCH; b++) {
                for (int half = 0; half < VQPW_SUBS_PER_BATCH; half++) {
                    const int m = b * VQPW_SUBS_PER_BATCH + half;
                    const int k = vqpw_search_sub(ctx, u + m * VQPW_DSUB, m, 0);
                    word |= ((uint32_t)(k & 0xF)) << (4 * m);
                }
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
    /* w[oc][b*CIN_MAC + ic]. Block diagonal: the low half of the 16-entry
     * input window feeds sub-codebook 2b, the high half feeds 2b+1, and each
     * half is zero for the other's output channels. Those zeros cost nothing
     * in time -- all 32 OCs multiply in parallel regardless. */
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
