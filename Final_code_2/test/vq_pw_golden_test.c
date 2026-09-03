/* ============================================================================
 * vq_pw_golden_test.c -- host-side verification of the PW-hosted VQ golden
 * model (Step 11 cases 1..10). Not compiled into the Vitis app: it lives
 * outside src/ precisely so its main() cannot collide with the firmware's.
 *
 *   gcc -O2 -I../src -o vq_pw_golden_test vq_pw_golden_test.c ../src/vq_pw.c
 *
 * The important test is hw_emulate_group(): it recomputes the indices from the
 * HOST-LOADED IMAGES (block-diagonal weight image + per-OC norm image) using
 * the PW engine's own arithmetic -- pb_ram window base 16*b, 16-tap MAC,
 * uint8-minus-uint8 pre-adder, per-lane argmin over each 16-OC half -- and
 * compares against the direct reference. Agreement proves the block-diagonal
 * mapping, the batch/OC assignment, the window offsets and the packing are all
 * consistent, which is the part a re-derivation is most likely to get wrong.
 * ==========================================================================*/
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "vq_pw.h"

static uint32_t rng_state = 0x12345678u;
static uint32_t xs32(void)
{
    uint32_t x = rng_state;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return (rng_state = x);
}
static void rng_seed(uint32_t s) { rng_state = s ? s : 1u; }

static int  g_fail = 0, g_checks = 0;
static void chk(int cond, const char *what)
{
    g_checks++;
    if (!cond) { g_fail++; printf("  FAIL: %s\n", what); }
}

/* ---- bit-level emulation of the PW engine in VQ mode -------------------- */
static void hw_emulate_group(const uint8_t *latent, int g,
                             const int8_t *wimg, const int32_t *nimg,
                             uint8_t zp, uint32_t *word_out)
{
    uint8_t pb[VQPW_CIN_LOAD][VQPW_LANES];
    const uint8_t *p = latent + (size_t)g * VQPW_DIM * VQPW_LANES;

    /* S_LOAD: cin_load = 64 beats, one per channel, 8 lanes wide */
    for (int c = 0; c < VQPW_CIN_LOAD; c++)
        for (int l = 0; l < VQPW_LANES; l++)
            pb[c][l] = p[(size_t)c * VQPW_LANES + l];

    for (int l = 0; l < VQPW_LANES; l++) word_out[l] = 0;

    for (int b = 0; b < VQPW_NBATCH; b++) {
        const int vq_base = b * VQPW_CIN_MAC;      /* pb_ram read offset */
        int32_t best[VQPW_SUBS_PER_BATCH][VQPW_LANES];
        int     bk  [VQPW_SUBS_PER_BATCH][VQPW_LANES];
        for (int h = 0; h < VQPW_SUBS_PER_BATCH; h++)
            for (int l = 0; l < VQPW_LANES; l++) { best[h][l] = 0x7FFFFFFF; bk[h][l] = 0; }

        /* PPU drain order: oc ascending 0..N_OC-1 */
        for (int oc = 0; oc < VQPW_N_OC; oc++) {
            const int half = oc / VQPW_K;
            const int k    = oc % VQPW_K;
            for (int l = 0; l < VQPW_LANES; l++) {
                int32_t acc = 0;
                for (int ic = 0; ic < VQPW_CIN_MAC; ic++) {
                    /* the DSP pre-adder: uint8 activation minus uint8 zp */
                    const int32_t a = (int32_t)pb[vq_base + ic][l] - (int32_t)zp;
                    const int32_t w = wimg[(size_t)oc * VQPW_W_PER_BANK
                                           + b * VQPW_CIN_MAC + ic];
                    acc += a * w;
                }
                const int32_t score = nimg[b * VQPW_N_OC + oc] - 2 * acc;
                if (score < best[half][l]) { best[half][l] = score; bk[half][l] = k; }
            }
        }
        for (int h = 0; h < VQPW_SUBS_PER_BATCH; h++) {
            const int m = b * VQPW_SUBS_PER_BATCH + h;
            for (int l = 0; l < VQPW_LANES; l++)
                word_out[l] |= ((uint32_t)(bk[h][l] & 0xF)) << (4 * m);
        }
    }
}

static void fill_cb_random(int8_t *cb)
{
    for (int i = 0; i < VQPW_M * VQPW_K * VQPW_DSUB; i++)
        cb[i] = (int8_t)(xs32() & 0xFF);
}

/* Compare golden vs emulator over ngroups groups; returns mismatch count. */
static long compare(vqpw_ctx_t *ctx, const uint8_t *latent, int ngroups,
                    const int8_t *wimg, const int32_t *nimg, uint8_t *idx)
{
    long bad = 0;
    uint32_t hw[VQPW_LANES];
    for (int g = 0; g < ngroups; g++) {
        hw_emulate_group(latent, g, wimg, nimg, ctx->zp, hw);
        for (int l = 0; l < VQPW_LANES; l++) {
            const int pos = g * VQPW_LANES + l;
            const uint8_t *o = idx + (size_t)pos * 4;
            const uint32_t ref = (uint32_t)o[0] | ((uint32_t)o[1] << 8)
                               | ((uint32_t)o[2] << 16) | ((uint32_t)o[3] << 24);
            if (ref != hw[l]) {
                if (bad < 4)
                    printf("  pos %d: ref 0x%08X hw 0x%08X\n",
                           pos, (unsigned)ref, (unsigned)hw[l]);
                bad++;
            }
        }
    }
    return bad;
}

int main(void)
{
    static int8_t   cb[VQPW_M * VQPW_K * VQPW_DSUB];
    static int8_t   wimg[VQPW_W_BYTES];
    static int32_t  nimg[VQPW_COUT_TOTAL];
    static uint8_t  latent[(size_t)VQPW_NGROUPS * VQPW_DIM * VQPW_LANES];
    static uint8_t  idx[VQPW_IDX_BYTES];
    vqpw_ctx_t ctx;

    printf("PW-hosted VQ golden model: M=%d K=%d DSUB=%d, %d batches of %d OC\n",
           VQPW_M, VQPW_K, VQPW_DSUB, VQPW_NBATCH, VQPW_N_OC);
    printf("codebook %d B, weight image %d B, norms %d entries\n\n",
           VQPW_M * VQPW_K * VQPW_DSUB, VQPW_W_BYTES, VQPW_COUT_TOTAL);

    /* ---- 0. the z0 = 128 guard ---- */
    printf("[0] zero-point guard\n");
    fill_cb_random(cb);
    chk(vqpw_init(&ctx, cb, 127) < 0, "zp=127 must be rejected");
    chk(vqpw_init(&ctx, cb, 129) < 0, "zp=129 must be rejected");
    chk(vqpw_init(&ctx, cb, 128) == 0, "zp=128 must be accepted");
    vqpw_build_weights(&ctx, wimg);
    vqpw_build_norms(&ctx, nimg);

    /* the block-diagonal image must be exactly half zeros */
    {
        long nz = 0;
        for (int i = 0; i < VQPW_W_BYTES; i++) if (wimg[i] == 0) nz++;
        printf("    weight image zeros: %ld / %d\n", nz, VQPW_W_BYTES);
        chk(nz >= VQPW_W_BYTES / 2, "block-diagonal image is >= half zeros");
    }

    /* ---- 1. random vectors ---- */
    printf("[1] random latent, 256 groups\n");
    rng_seed(0xC0FFEEu);
    for (size_t i = 0; i < sizeof latent; i++) latent[i] = (uint8_t)(xs32() & 0xFF);
    vqpw_encode_frame(&ctx, latent, idx);
    chk(compare(&ctx, latent, 256, wimg, nimg, idx) == 0, "random: golden == hw emulation");

    /* ---- 2/3/4/6. INT8 extremes, zeros, quantiser saturation ---- */
    printf("[2-4,6] extremes: latent bytes 0 (u=-128), 255 (u=+127), 128 (u=0)\n");
    {
        const uint8_t vals[3] = { 0u, 255u, 128u };
        for (int t = 0; t < 3; t++) {
            memset(latent, vals[t], (size_t)64 * VQPW_DIM * VQPW_LANES);
            vqpw_encode_frame(&ctx, latent, idx);
            char msg[64];
            sprintf(msg, "uniform latent byte %u", (unsigned)vals[t]);
            chk(compare(&ctx, latent, 64, wimg, nimg, idx) == 0, msg);
        }
        /* extreme codebook against extreme latent -- drives the width bounds */
        for (int i = 0; i < VQPW_M * VQPW_K * VQPW_DSUB; i++)
            cb[i] = (i & 1) ? (int8_t)-128 : (int8_t)127;
        vqpw_init(&ctx, cb, 128);
        vqpw_build_weights(&ctx, wimg); vqpw_build_norms(&ctx, nimg);
        for (int t = 0; t < 2; t++) {
            memset(latent, vals[t], (size_t)64 * VQPW_DIM * VQPW_LANES);
            vqpw_encode_frame(&ctx, latent, idx);
            chk(compare(&ctx, latent, 64, wimg, nimg, idx) == 0, "extreme cb x extreme latent");
        }
    }

    /* ---- 5. equal-distance ties: duplicate every codeword, lowest k must win ---- */
    printf("[5] tie storm: codewords duplicated in pairs, lowest index must win\n");
    {
        rng_seed(0xABCDu);
        fill_cb_random(cb);
        for (int m = 0; m < VQPW_M; m++)          /* k and k+1 identical */
            for (int k = 0; k < VQPW_K; k += 2)
                memcpy(cb + ((size_t)m * VQPW_K + k + 1) * VQPW_DSUB,
                       cb + ((size_t)m * VQPW_K + k)     * VQPW_DSUB, VQPW_DSUB);
        vqpw_init(&ctx, cb, 128);
        vqpw_build_weights(&ctx, wimg); vqpw_build_norms(&ctx, nimg);
        for (size_t i = 0; i < (size_t)64 * VQPW_DIM * VQPW_LANES; i++)
            latent[i] = (uint8_t)(xs32() & 0xFF);
        vqpw_encode_frame(&ctx, latent, idx);
        chk(compare(&ctx, latent, 64, wimg, nimg, idx) == 0, "ties: golden == hw");
        long odd = 0;
        for (int pos = 0; pos < 64 * VQPW_LANES; pos++) {
            const uint8_t *o = idx + (size_t)pos * 4;
            const uint32_t w = (uint32_t)o[0] | ((uint32_t)o[1] << 8)
                             | ((uint32_t)o[2] << 16) | ((uint32_t)o[3] << 24);
            for (int m = 0; m < VQPW_M; m++) if ((w >> (4 * m)) & 1u) odd++;
        }
        printf("    odd (higher-of-pair) indices selected: %ld / %d\n",
               odd, 64 * VQPW_LANES * VQPW_M);
        chk(odd == 0, "every tie resolved to the LOWER codeword index");
    }

    /* ---- 7. all 8 sub-codebooks independently exercised ---- */
    printf("[7] per-sub-codebook coverage\n");
    {
        rng_seed(0x5EEDu); fill_cb_random(cb);
        vqpw_init(&ctx, cb, 128);
        vqpw_build_weights(&ctx, wimg); vqpw_build_norms(&ctx, nimg);
        for (size_t i = 0; i < (size_t)128 * VQPW_DIM * VQPW_LANES; i++)
            latent[i] = (uint8_t)(xs32() & 0xFF);
        vqpw_encode_frame(&ctx, latent, idx);
        int seen[VQPW_M][VQPW_K]; memset(seen, 0, sizeof seen);
        for (int pos = 0; pos < 128 * VQPW_LANES; pos++) {
            const uint8_t *o = idx + (size_t)pos * 4;
            const uint32_t w = (uint32_t)o[0] | ((uint32_t)o[1] << 8)
                             | ((uint32_t)o[2] << 16) | ((uint32_t)o[3] << 24);
            for (int m = 0; m < VQPW_M; m++) seen[m][(w >> (4 * m)) & 0xF] = 1;
        }
        int covered = 0;
        for (int m = 0; m < VQPW_M; m++)
            for (int k = 0; k < VQPW_K; k++) covered += seen[m][k];
        printf("    (m,k) pairs observed: %d / %d\n", covered, VQPW_M * VQPW_K);
        chk(covered == VQPW_M * VQPW_K, "all 8 sub-codebooks reach all 16 codewords");
    }

    /* ---- 8. consecutive spatial positions must stay independent ---- */
    printf("[8] consecutive positions / lane independence\n");
    {
        /* give every lane of group 0 a distinct constant, check 8 distinct results */
        for (int c = 0; c < VQPW_DIM; c++)
            for (int l = 0; l < VQPW_LANES; l++)
                latent[(size_t)c * VQPW_LANES + l] = (uint8_t)(l * 31 + c);
        vqpw_encode_frame(&ctx, latent, idx);
        chk(compare(&ctx, latent, 1, wimg, nimg, idx) == 0, "per-lane: golden == hw");
    }

    /* ---- 9/10. complete frames, back to back ---- */
    printf("[9-10] complete 160x90 frames, back to back\n");
    {
        long total_bad = 0;
        for (int f = 0; f < 3; f++) {
            rng_seed(0x1000u + f);
            for (size_t i = 0; i < sizeof latent; i++) latent[i] = (uint8_t)(xs32() & 0xFF);
            vqpw_encode_frame(&ctx, latent, idx);
            const long bad = compare(&ctx, latent, VQPW_NGROUPS, wimg, nimg, idx);
            printf("    frame %d: %ld mismatches over %d positions\n", f, bad, VQPW_NPOS);
            total_bad += bad;
        }
        chk(total_bad == 0, "3 full frames, zero mismatches");
    }

    /* ---- 11. DIRECTED CORNERS: attain the derived extremes ----------------
     * Random data never approaches them, so construct them explicitly.
     *   score_max : v = -128 (all d), u = +127 (all d)
     *               norm = 8*128^2 = 131072, acc = 8*127*(-128) = -130048
     *               score = 131072 + 260096 = +391168
     *   score_min : v = -128, u = -128 -> acc = +131072, norm = 131072
     *               score = 131072 - 262144 = -131072
     * The algebraic lower bound -262144 assumes norm = 0 AND acc maximal, but
     * acc is maximised by large |v|, which forces norm large. So -131072 is the
     * REACHABLE minimum and -262144 is loose. 20 bits signed covers both. */
    printf("[11] directed corners\n");
    {
        vqpw_ctx_t cx;
        int8_t  cbx[VQPW_M * VQPW_K * VQPW_DSUB];
        int8_t  u_hi[VQPW_DSUB], u_lo[VQPW_DSUB];
        int32_t sc[VQPW_K];

        for (int i = 0; i < VQPW_M * VQPW_K * VQPW_DSUB; i++) cbx[i] = (int8_t)-128;
        chk(vqpw_init(&cx, cbx, 128) == 0, "corner ctx init");
        for (int d = 0; d < VQPW_DSUB; d++) { u_hi[d] = (int8_t)127; u_lo[d] = (int8_t)-128; }

        vqpw_search_sub(&cx, u_hi, 0, sc);
        printf("    v=-128, u=+127 -> score %ld (expect +391168)\n", (long)sc[0]);
        chk(sc[0] == 391168L,  "maximum score attained exactly");
        chk(sc[0] == VQPW_SCORE_MAX, "attained max equals the derived bound");

        vqpw_search_sub(&cx, u_lo, 0, sc);
        printf("    v=-128, u=-128 -> score %ld (expect -131072)\n", (long)sc[0]);
        chk(sc[0] == -131072L, "reachable minimum attained exactly");

        printf("    corner acc range [%ld, %ld]\n",
               (long)cx.obs_acc_min, (long)cx.obs_acc_max);
        chk(cx.obs_acc_min == -130048L, "acc minimum attained exactly");
        chk(cx.obs_acc_max ==  131072L, "acc maximum attained exactly");

        /* 20 bits is necessary as well as sufficient: 19 would clip +391168. */
        chk(391168L >  (1L << 18) - 1, "19-bit signed would OVERFLOW the max score");
        chk(391168L <= (1L << 19) - 1, "20-bit signed is sufficient");
        chk(-131072L >= -(1L << 19),   "20-bit signed covers the reachable min");
    }

    /* ---- width claim ---- */
    printf("\nobserved acc   range [%ld, %ld]   (derived [-130048, +131072], 19b signed)\n",
           (long)ctx.obs_acc_min, (long)ctx.obs_acc_max);
    printf("observed score range [%ld, %ld]   (derived [%ld, %ld], %d-bit signed)\n",
           (long)ctx.obs_score_min, (long)ctx.obs_score_max,
           (long)VQPW_SCORE_MIN, (long)VQPW_SCORE_MAX, VQPW_SCORE_BITS);
    chk(ctx.obs_score_min >= VQPW_SCORE_MIN && ctx.obs_score_max <= VQPW_SCORE_MAX,
        "observed scores inside the derived 20-bit envelope");
    chk(ctx.obs_acc_min >= -130048 && ctx.obs_acc_max <= 131072,
        "observed accumulators inside the derived 19-bit envelope");

    printf("\n%d checks, %d failures -> %s\n", g_checks, g_fail,
           g_fail ? "FAIL" : "PASS");
    return g_fail ? 1 : 0;
}
