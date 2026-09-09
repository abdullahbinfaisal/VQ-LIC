/* ============================================================================
 * vq_pw_golden_test.c -- host-side verification of the PW-hosted VQ golden
 * model. Not compiled into the Vitis app: it lives outside src/ precisely so
 * its main() cannot collide with the firmware's.
 *
 *   gcc -O2 -I../src -o vq_pw_golden_test vq_pw_golden_test.c ../src/vq_pw.c
 *   gcc -O2 -DVQPW_PROFILE=0 -I../src -o vq_pw_golden_test_legacy \
 *       vq_pw_golden_test.c ../src/vq_pw.c
 *
 * GEOMETRY-GENERAL. Every loop below is written in terms of VQPW_M / VQPW_K /
 * VQPW_DSUB / VQPW_KW, so the same source proves BOTH profiles:
 *     VQPW_PROFILE = 1   DEPLOYED  M=4, K=64, Dsub=16   24 bits/position
 *     VQPW_PROFILE = 0   LEGACY    M=8, K=16, Dsub=8    32 bits/position
 *
 * The important test is hw_emulate_group(): it recomputes the indices from the
 * HOST-LOADED IMAGES (weight image + per-OC norm image) using the PW engine's
 * own arithmetic -- pb_ram window base vqpw_batch_window(b), CIN_MAC-tap MAC,
 * uint8-minus-uint8 pre-adder, per-lane argmin that resets exactly every K
 * codewords and PERSISTS ACROSS BATCH BOUNDARIES, and the score truncated to
 * VQPW_SCORE_BITS. Agreement proves the mapping, the batch/OC assignment, the
 * window offsets, the score width and the packing are all consistent, which is
 * the part a re-derivation is most likely to get wrong.
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

/* Sign-extend the low `bits` of v, exactly as the RTL does with
 * vq_score[g] = sc32[VQ_SCORE_W-1:0]. If VQPW_SCORE_BITS is wide enough this
 * is the identity; if it is too narrow the emulation diverges from the
 * reference and every comparison below fails loudly. That is the point. */
static int32_t trunc_signed(int32_t v, int bits)
{
    const uint32_t m = 1u << (bits - 1);
    return (int32_t)((((uint32_t)v) & (2u * m - 1u)) ^ m) - (int32_t)m;
}

/* Read the KW-bit field for sub-codebook m out of a packed 32-bit word. */
static unsigned fld(uint32_t w, int m)
{
    return (unsigned)((w >> (VQPW_KW * m)) & VQPW_KMASK);
}

static uint32_t word_at(const uint8_t *idx, int pos)
{
    const uint8_t *o = idx + (size_t)pos * 4;
    return (uint32_t)o[0] | ((uint32_t)o[1] << 8)
         | ((uint32_t)o[2] << 16) | ((uint32_t)o[3] << 24);
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

    /* One running argmin per sub-codebook per lane. In the RTL there is only
     * ONE such register per lane, because the codewords of a sub-codebook are
     * contiguous in absolute-OC order and vq_first (k == 0) is what clears it.
     * Keeping M of them here expresses the same thing without replaying the
     * exact issue order -- and it is what makes the K > N_OC case meaningful:
     * best[m] must survive the batch boundary between k=31 and k=32. */
    int32_t best[VQPW_M][VQPW_LANES];
    int     bk  [VQPW_M][VQPW_LANES];
    for (int m = 0; m < VQPW_M; m++)
        for (int l = 0; l < VQPW_LANES; l++) { best[m][l] = 0; bk[m][l] = 0; }

    for (int b = 0; b < VQPW_NBATCH; b++) {
        /* pb_ram read offset. Advances by DSUB once per SUB-CODEBOOK, not once
         * per batch: at K > N_OC consecutive batches share a window. This is
         * vq_pb_base in pw_pixel_major_core.sv. */
        const int vq_base = vqpw_batch_window(b);

        /* PPU drain order: oc ascending 0..N_OC-1 */
        for (int oc = 0; oc < VQPW_N_OC; oc++) {
            const int abs = b * VQPW_N_OC + oc;
            const int m   = abs / VQPW_K;
            const int k   = abs % VQPW_K;
            for (int l = 0; l < VQPW_LANES; l++) {
                int32_t acc = 0;
                for (int ic = 0; ic < VQPW_CIN_MAC; ic++) {
                    /* the DSP pre-adder: uint8 activation minus uint8 zp */
                    const int32_t a = (int32_t)pb[vq_base + ic][l] - (int32_t)zp;
                    const int32_t w = wimg[(size_t)oc * VQPW_W_PER_BANK
                                           + b * VQPW_CIN_MAC + ic];
                    acc += a * w;
                }
                /* nimg is in ABSOLUTE OC order, which is what vq_abs indexes */
                const int32_t score =
                    trunc_signed(nimg[abs] - 2 * acc, VQPW_SCORE_BITS);
                /* vq_first is (k == 0); STRICT less-than keeps the lowest k */
                if (k == 0 || score < best[m][l]) { best[m][l] = score; bk[m][l] = k; }
            }
        }
    }

    for (int l = 0; l < VQPW_LANES; l++) {
        word_out[l] = 0;
        for (int m = 0; m < VQPW_M; m++)
            word_out[l] |= ((uint32_t)bk[m][l] & VQPW_KMASK) << (VQPW_KW * m);
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
            const uint32_t ref = word_at(idx, pos);
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
    printf("codebook %d B, weight image %d B, norms %d entries, "
           "score %d bits, %d bits/position\n\n",
           VQPW_M * VQPW_K * VQPW_DSUB, VQPW_W_BYTES, VQPW_COUT_TOTAL,
           VQPW_SCORE_BITS, VQPW_BITS_PER_POS);

    /* ---- -1. derived geometry is self-consistent ---- */
    printf("[-1] build self-check\n");
    {
        const int rc = vqpw_check_build();
        if (rc) printf("    vqpw_check_build() = %d\n", rc);
        chk(rc == 0, "vqpw_check_build() accepts this profile");
        chk(VQPW_CIN_MAC == 16, "cin_mac is 16 in both profiles");
        chk(VQPW_CIN_LOAD == 64, "the full 64-channel latent is streamed");
        chk(VQPW_COUT_TOTAL <= VQPW_PW_COUT_MAX,
            "c_out fits the engine's weight batches");
        for (int b = 0; b < VQPW_NBATCH; b++) {
            const int w = vqpw_batch_window(b);
            chk(w >= 0 && w + VQPW_CIN_MAC <= VQPW_CIN_LOAD,
                "batch window inside the loaded channels");
        }
        chk(vqpw_batch_window(VQPW_NBATCH - 1) + VQPW_CIN_MAC == VQPW_CIN_LOAD,
            "the last batch window ends exactly at cin_load");
    }

    /* ---- 0. the z0 = 128 guard ---- */
    printf("[0] zero-point guard\n");
    fill_cb_random(cb);
    chk(vqpw_init(&ctx, cb, 127) < 0, "zp=127 must be rejected");
    chk(vqpw_init(&ctx, cb, 129) < 0, "zp=129 must be rejected");
    chk(vqpw_init(&ctx, cb, 128) == 0, "zp=128 must be accepted");
    vqpw_build_weights(&ctx, wimg);
    vqpw_build_norms(&ctx, nimg);

    /* Structural zeros. K < N_OC packs two sub-codebooks per batch and each
     * output channel zeroes the other half; K > N_OC needs none at all, and
     * that is a claim worth pinning down rather than assuming. */
    {
        long nz = 0;
        for (int i = 0; i < VQPW_W_BYTES; i++) if (wimg[i] == 0) nz++;
        printf("    weight image zeros: %ld / %d\n", nz, VQPW_W_BYTES);
        if (VQPW_SUBS_PER_BATCH > 1) {
            chk(nz >= VQPW_W_BYTES / 2, "block-diagonal image is >= half zeros");
        } else {
            /* Only codebook coefficients that happen to be 0 may appear, about
             * 1 in 256 for a random int8 codebook. */
            chk(nz < VQPW_W_BYTES / 32,
                "Dsub == cin_mac -> NO structural zeros in the weight image");
            long mism = 0, offs = 0;
            for (int oc = 0; oc < VQPW_N_OC; oc++)
                for (int b = 0; b < VQPW_NBATCH; b++) {
                    int k, ic_lo;
                    const int m = vqpw_map_oc(b, oc, &k, &ic_lo);
                    if (ic_lo != 0) offs++;
                    for (int ic = 0; ic < VQPW_CIN_MAC; ic++)
                        if (wimg[(size_t)oc * VQPW_W_PER_BANK + b * VQPW_CIN_MAC + ic]
                            != cb[((size_t)m * VQPW_K + k) * VQPW_DSUB + ic]) mism++;
                }
            chk(offs == 0, "K > N_OC -> the window offset is always 0");
            chk(mism == 0, "every weight entry is a real codeword coefficient");
        }
    }

    /* ---- 0b. norm image addressing, including addresses above 127 ---- */
    printf("[0b] norm image addressing\n");
    {
        long mism = 0;
        for (int b = 0; b < VQPW_NBATCH; b++)
            for (int oc = 0; oc < VQPW_N_OC; oc++) {
                int k; const int m = vqpw_map_oc(b, oc, &k, 0);
                if (nimg[b * VQPW_N_OC + oc] != ctx.norm2[m * VQPW_K + k]) mism++;
            }
        chk(mism == 0, "norm image indexed by ABSOLUTE OC matches norm2[m*K+k]");
        printf("    highest norm address used: %d\n", VQPW_COUT_TOTAL - 1);
        chk(VQPW_COUT_TOTAL - 1 <= 255, "norm address fits the 8-bit AXI field");
#if VQPW_PROFILE
        chk(VQPW_COUT_TOTAL - 1 > 127,
            "DEPLOYED profile exercises norm addresses ABOVE 127");
#endif
        int32_t nmax = 0;
        for (int i = 0; i < VQPW_COUT_TOTAL; i++) if (nimg[i] > nmax) nmax = nimg[i];
        printf("    largest ||v_k||^2 = %ld (20-bit signed field limit 524287)\n",
               (long)nmax);
        chk(nmax <= 524287L, "||v_k||^2 fits the 20-bit signed AXI norm field");
    }

    /* ---- 1. random vectors ---- */
    printf("[1] random latent, 256 groups\n");
    rng_seed(0xC0FFEEu);
    for (size_t i = 0; i < sizeof latent; i++) latent[i] = (uint8_t)(xs32() & 0xFF);
    vqpw_encode_frame(&ctx, latent, idx);
    chk(compare(&ctx, latent, 256, wimg, nimg, idx) == 0,
        "random: golden == hw emulation");

    /* ---- 1b. the unused high bits of the transport word are ZERO ---- */
    printf("[1b] transport word: bits above M*KW must be zero\n");
    {
        long dirty = 0;
        const uint32_t used = (VQPW_BITS_PER_POS >= 32)
                            ? 0xFFFFFFFFu : ((1u << VQPW_BITS_PER_POS) - 1u);
        for (int pos = 0; pos < 256 * VQPW_LANES; pos++)
            if (word_at(idx, pos) & ~used) dirty++;
        printf("    positions with a non-zero unused bit: %ld\n", dirty);
        chk(dirty == 0, "unused transport bits are zero");
    }

    /* ---- 1c. the packing helpers round-trip and agree with encode_frame ---- */
    printf("[1c] vqpw_get_index / vqpw_put_index\n");
    {
        long mism = 0;
        for (int pos = 0; pos < 4096; pos++)
            for (int m = 0; m < VQPW_M; m++)
                if (vqpw_get_index(idx, pos, m) != (uint8_t)fld(word_at(idx, pos), m))
                    mism++;
        chk(mism == 0, "vqpw_get_index agrees with the field layout");

        static uint8_t scratch[VQPW_IDX_BYTES];
        memset(scratch, 0, sizeof scratch);
        rng_seed(0x7A57Eu);
        for (int pos = 0; pos < 4096; pos++)
            for (int m = 0; m < VQPW_M; m++)
                vqpw_put_index(scratch, pos, m, (uint8_t)(xs32() & VQPW_KMASK));
        rng_seed(0x7A57Eu);
        long rt = 0, hi = 0;
        for (int pos = 0; pos < 4096; pos++)
            for (int m = 0; m < VQPW_M; m++) {
                const uint8_t want = (uint8_t)(xs32() & VQPW_KMASK);
                if (vqpw_get_index(scratch, pos, m) != want) rt++;
                if (want > 15u) hi++;
            }
        chk(rt == 0, "put/get round-trips every field");
        printf("    round-tripped indices above 15: %ld\n", hi);
#if VQPW_PROFILE
        chk(hi > 0, "DEPLOYED profile round-trips indices ABOVE 15");
#endif
        long dirty = 0;
        const uint32_t used = (VQPW_BITS_PER_POS >= 32)
                            ? 0xFFFFFFFFu : ((1u << VQPW_BITS_PER_POS) - 1u);
        for (int pos = 0; pos < 4096; pos++)
            if (word_at(scratch, pos) & ~used) dirty++;
        chk(dirty == 0, "put_index never touches the unused bits");
    }

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
            chk(compare(&ctx, latent, 64, wimg, nimg, idx) == 0,
                "extreme cb x extreme latent");
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
        for (int pos = 0; pos < 64 * VQPW_LANES; pos++)
            for (int m = 0; m < VQPW_M; m++)
                if (fld(word_at(idx, pos), m) & 1u) odd++;
        printf("    odd (higher-of-pair) indices selected: %ld / %d\n",
               odd, 64 * VQPW_LANES * VQPW_M);
        chk(odd == 0, "every tie resolved to the LOWER codeword index");

        /* A tie that STRADDLES A BATCH BOUNDARY is the case K > N_OC creates
         * and K = 16 cannot: codeword N_OC-1 (last of batch 2m) against
         * codeword N_OC (first of batch 2m+1), same sub-codebook. Duplicate
         * exactly that pair; the lower index must still win, which is only
         * true if the running minimum survives the batch boundary. */
        if (VQPW_K > VQPW_N_OC) {
            rng_seed(0xB0117u);
            fill_cb_random(cb);
            for (int m = 0; m < VQPW_M; m++)
                memcpy(cb + ((size_t)m * VQPW_K + VQPW_N_OC)     * VQPW_DSUB,
                       cb + ((size_t)m * VQPW_K + VQPW_N_OC - 1) * VQPW_DSUB,
                       VQPW_DSUB);
            vqpw_init(&ctx, cb, 128);
            vqpw_build_weights(&ctx, wimg); vqpw_build_norms(&ctx, nimg);
            for (size_t i = 0; i < (size_t)64 * VQPW_DIM * VQPW_LANES; i++)
                latent[i] = (uint8_t)(xs32() & 0xFF);
            vqpw_encode_frame(&ctx, latent, idx);
            chk(compare(&ctx, latent, 64, wimg, nimg, idx) == 0,
                "cross-batch tie: golden == hw");
            long hitHi = 0, hitLo = 0;
            for (int pos = 0; pos < 64 * VQPW_LANES; pos++)
                for (int m = 0; m < VQPW_M; m++) {
                    const unsigned k = fld(word_at(idx, pos), m);
                    if (k == (unsigned)VQPW_N_OC)     hitHi++;
                    if (k == (unsigned)VQPW_N_OC - 1) hitLo++;
                }
            printf("    k=%d chosen %ld times, k=%d chosen %ld times\n",
                   VQPW_N_OC, hitHi, VQPW_N_OC - 1, hitLo);
            chk(hitHi == 0,
                "a tie across a batch boundary resolves to the LOWER index");
            chk(hitLo > 0,
                "the cross-batch duplicate pair is actually being selected");
        }
    }

    /* ---- 7. all sub-codebooks / all codewords exercised ---- */
    printf("[7] per-sub-codebook coverage\n");
    {
        rng_seed(0x5EEDu); fill_cb_random(cb);
        vqpw_init(&ctx, cb, 128);
        vqpw_build_weights(&ctx, wimg); vqpw_build_norms(&ctx, nimg);
        for (size_t i = 0; i < sizeof latent; i++)
            latent[i] = (uint8_t)(xs32() & 0xFF);
        vqpw_encode_frame(&ctx, latent, idx);
        static int seen[VQPW_M][VQPW_K];
        memset(seen, 0, sizeof seen);
        unsigned maxk = 0;
        for (int pos = 0; pos < VQPW_NPOS; pos++) {
            const uint32_t w = word_at(idx, pos);
            for (int m = 0; m < VQPW_M; m++) {
                const unsigned k = fld(w, m);
                seen[m][k] = 1;
                if (k > maxk) maxk = k;
            }
        }
        int covered = 0;
        for (int m = 0; m < VQPW_M; m++)
            for (int k = 0; k < VQPW_K; k++) covered += seen[m][k];
        printf("    (m,k) pairs observed: %d / %d, highest index %u\n",
               covered, VQPW_M * VQPW_K, maxk);
        chk(covered == VQPW_M * VQPW_K,
            "every sub-codebook reaches every codeword");
#if VQPW_PROFILE
        chk(maxk > 15u, "DEPLOYED profile actually selects indices ABOVE 15");
#endif
    }

    /* ---- 8. consecutive spatial positions must stay independent ---- */
    printf("[8] consecutive positions / lane independence\n");
    {
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
            for (size_t i = 0; i < sizeof latent; i++)
                latent[i] = (uint8_t)(xs32() & 0xFF);
            vqpw_encode_frame(&ctx, latent, idx);
            const long bad = compare(&ctx, latent, VQPW_NGROUPS, wimg, nimg, idx);
            printf("    frame %d: %ld mismatches over %d positions\n",
                   f, bad, VQPW_NPOS);
            total_bad += bad;
        }
        chk(total_bad == 0, "3 full frames, zero mismatches");
    }

    /* ---- 11. DIRECTED CORNERS: attain the derived extremes ----------------
     * Random data never approaches them, so construct them explicitly.
     *   score_max : v = -128 (all d), u = +127 (all d)
     *               norm = D*16384, acc = -D*16256
     *               score = D*16384 + 2*D*16256 = D*48896
     *   score_min : v = -128, u = -128 -> acc = +D*16384, norm = D*16384
     *               score = D*16384 - 2*D*16384 = -D*16384
     * The algebraic lower bound assumes norm = 0 AND acc maximal, but acc is
     * maximised by large |v|, which forces norm large. So -16384*D is the
     * REACHABLE minimum and VQPW_SCORE_MIN is loose. Both fit the width. */
    printf("[11] directed corners\n");
    {
        vqpw_ctx_t cx;
        static int8_t  cbx[VQPW_M * VQPW_K * VQPW_DSUB];
        int8_t  u_hi[VQPW_DSUB], u_lo[VQPW_DSUB];
        static int32_t sc[VQPW_K];
        const long D        = VQPW_DSUB;
        const long want_max =  48896L * D;
        const long want_min = -16384L * D;
        const long acc_min  = -16256L * D;   /* u=+127, v=-128 */
        const long acc_max  =  16384L * D;   /* u=-128, v=-128 */

        for (int i = 0; i < VQPW_M * VQPW_K * VQPW_DSUB; i++) cbx[i] = (int8_t)-128;
        chk(vqpw_init(&cx, cbx, 128) == 0, "corner ctx init");
        for (int d = 0; d < VQPW_DSUB; d++) { u_hi[d] = (int8_t)127; u_lo[d] = (int8_t)-128; }

        vqpw_search_sub(&cx, u_hi, 0, sc);
        printf("    v=-128, u=+127 -> score %ld (expect %ld)\n", (long)sc[0], want_max);
        chk(sc[0] == want_max, "maximum score attained exactly");
        chk(sc[0] == VQPW_SCORE_MAX, "attained max equals the derived bound");

        vqpw_search_sub(&cx, u_lo, 0, sc);
        printf("    v=-128, u=-128 -> score %ld (expect %ld)\n", (long)sc[0], want_min);
        chk(sc[0] == want_min, "reachable minimum attained exactly");

        printf("    corner acc range [%ld, %ld]\n",
               (long)cx.obs_acc_min, (long)cx.obs_acc_max);
        chk(cx.obs_acc_min == acc_min, "acc minimum attained exactly");
        chk(cx.obs_acc_max == acc_max, "acc maximum attained exactly");

        /* VQPW_SCORE_BITS must be necessary as well as sufficient. */
        printf("    width: max %ld needs > 2^%d = %ld, have 2^%d = %ld\n",
               want_max, VQPW_SCORE_BITS - 2, (1L << (VQPW_SCORE_BITS - 2)),
               VQPW_SCORE_BITS - 1, (1L << (VQPW_SCORE_BITS - 1)));
        chk(want_max >  (1L << (VQPW_SCORE_BITS - 2)) - 1,
            "one bit narrower would OVERFLOW the max score");
        chk(want_max <= (1L << (VQPW_SCORE_BITS - 1)) - 1,
            "VQPW_SCORE_BITS is sufficient for the max score");
        chk(want_min >= -(1L << (VQPW_SCORE_BITS - 1)),
            "VQPW_SCORE_BITS covers the reachable min");
        /* trunc_signed must be the identity at this width -- if it is not, the
         * emulation above silently diverged and every comparison was vacuous */
        chk(trunc_signed((int32_t)want_max, VQPW_SCORE_BITS) == (int32_t)want_max,
            "truncation to VQPW_SCORE_BITS is lossless at the max score");
        chk(trunc_signed((int32_t)want_min, VQPW_SCORE_BITS) == (int32_t)want_min,
            "truncation to VQPW_SCORE_BITS is lossless at the min score");
#if VQPW_PROFILE
        /* the whole point of the width change */
        chk(want_max > (1L << 19) - 1,
            "DEPLOYED max score EXCEEDS the old 20-bit range");
        chk(trunc_signed((int32_t)want_max, 20) != (int32_t)want_max,
            "20 bits would demonstrably corrupt the DEPLOYED max score");
#endif
    }

    /* ---- width claim ---- */
    printf("\nobserved acc   range [%ld, %ld]   (derived [%ld, %ld])\n",
           (long)ctx.obs_acc_min, (long)ctx.obs_acc_max,
           -16256L * VQPW_DSUB, 16384L * VQPW_DSUB);
    printf("observed score range [%ld, %ld]   (derived [%ld, %ld], %d-bit signed)\n",
           (long)ctx.obs_score_min, (long)ctx.obs_score_max,
           (long)VQPW_SCORE_MIN, (long)VQPW_SCORE_MAX, VQPW_SCORE_BITS);
    chk(ctx.obs_score_min >= VQPW_SCORE_MIN && ctx.obs_score_max <= VQPW_SCORE_MAX,
        "observed scores inside the derived envelope");
    chk(ctx.obs_acc_min >= -16256L * VQPW_DSUB
        && ctx.obs_acc_max <= 16384L * VQPW_DSUB,
        "observed accumulators inside the derived envelope");

    printf("\n%d checks, %d failures -> %s\n", g_checks, g_fail,
           g_fail ? "FAIL" : "PASS");
    return g_fail ? 1 : 0;
}
