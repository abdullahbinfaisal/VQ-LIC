/* Does the slot-probe codebook really read a byte back verbatim?
 * v_k = (-128 + 4k, 0, 0, ...) makes score(k) = (a_k - u[0])^2 - u[0]^2,
 * so argmin must be the a_k nearest u[0], i.e. k = round(B/4) for raw byte B.
 * Check it against the golden model, at every byte, in every sub-codebook,
 * and check the OTHER dimensions really cannot influence the answer. */
#include <stdio.h>
#include <stdlib.h>
#include "vq_pw.h"

static int8_t cb[VQPW_M * VQPW_K * VQPW_DSUB];

int main(void)
{
    vqpw_ctx_t ctx;
    int bad = 0, reach[VQPW_K];
    unsigned seed = 12345u;

    for (int k = 0; k < VQPW_K; k++) reach[k] = 0;
    for (size_t i = 0; i < sizeof cb; i++) cb[i] = 0;
    for (int m = 0; m < VQPW_M; m++)
        for (int k = 0; k < VQPW_K; k++)
            cb[((size_t)m * VQPW_K + k) * VQPW_DSUB + 0] = (int8_t)(-128 + 4 * k);

    if (vqpw_init(&ctx, cb, 128) != 0) { printf("init failed\n"); return 1; }

    /* The ladder spans a_0 = -128 up to a_(K-1) = -128 + 4(K-1), so only
     * bytes up to 4(K-1)+2 round correctly; past that every byte saturates on
     * the top codeword. At K=16 that is byte 62. The probe's own markers are
     * 4*(g+8) = 32..60, inside the range at BOTH profiles. */
    const int B_MAX = (4 * (VQPW_K - 1) + 2 < 255) ? 4 * (VQPW_K - 1) + 2 : 255;
    printf("  ladder covers bytes 0..%d; probe markers are 32..60\n", B_MAX);

    for (int B = 0; B <= B_MAX; B++) {
        /* every other dimension gets NOISE, to prove it cannot reach the score */
        int8_t u[VQPW_DSUB];
        int want = (B + 2) / 4;                 /* round(B/4), ties resolve up */
        if (want > VQPW_K - 1) want = VQPW_K - 1;
        /* the lowest-index-wins tie rule pulls an exact .5 down, not up */
        if ((B % 4) == 2) want = B / 4;

        for (int m = 0; m < VQPW_M; m++) {
            u[0] = (int8_t)(B - 128);
            for (int d = 1; d < VQPW_DSUB; d++) {
                seed = seed * 1103515245u + 12345u;
                u[d] = (int8_t)((seed >> 16) & 0xFF);
            }
            int got = vqpw_search_sub(&ctx, u, m, NULL);
            if (got != want) {
                if (bad < 8)
                    printf("  B=%3d m=%d want k=%d got k=%d\n", B, m, want, got);
                bad++;
            }
            if (got >= 0 && got < VQPW_K) reach[got] = 1;
        }
    }

    int unreached = 0;
    for (int k = 0; k < VQPW_K; k++) if (!reach[k]) unreached++;

    printf("profile %d: M=%d K=%d DSUB=%d\n", VQPW_PROFILE,
           VQPW_M, VQPW_K, VQPW_DSUB);
    printf("  byte->index mismatches : %d of %d\n", bad, (B_MAX + 1) * VQPW_M);
    printf("  codewords reachable    : %d of %d\n", VQPW_K - unreached, VQPW_K);
    printf("  %s\n", bad
           ? "the index does NOT track the byte -- the probe cannot be trusted"
           : "the index is a faithful read-out of the byte");
    printf("RESULT: %s\n", bad ? "FAIL" : "PASS");
    return bad != 0;
}
