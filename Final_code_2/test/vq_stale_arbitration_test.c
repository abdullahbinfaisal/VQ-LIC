/* Which fault is it? Settled from the 2026-09-10 board log, offline.
 *
 * The board's VQ self-test runs fills 128, 255, 0, 129, each twice, so the byte
 * left in activation slot 0 when a case FIRST runs is the previous case's fill.
 * Two mechanisms were live:
 *
 *   (a) slot 0 holds that stale byte
 *   (b) channel 0 never reaches the score, which always means u[0] = 0
 *
 * The u=+127 case cannot separate them, because the byte it would inherit is
 * 128 and 128 IS the zero point -- and that is the case every earlier diagnostic
 * leaned on, which is why the campaign went in circles. The u=+1 case inherits
 * raw 0, i.e. u[0] = -128, and separates them cleanly.
 *
 * BOARD (2026-09-10):   u=+127  hw k=14, sw k=7
 *                       u=+1    hw k= 9, sw k=54
 *
 * This test reproduces edge_harness.c's synthetic codebook exactly -- the sw
 * answers act as a checksum on that reproduction -- and shows (a) predicts the
 * board and (b) does not. Keep it: if the codebook generator ever changes, this
 * stops matching and the recorded conclusion must be re-derived, not assumed. */
#include <stdio.h>
#include "vq_pw.h"

#if VQPW_PROFILE == 1

static int8_t cb[VQPW_M * VQPW_K * VQPW_DSUB];

static void synth_codebook(void)        /* verbatim from edge_harness.c */
{
    uint32_t st = 0x2468ACEu;
    for (size_t i = 0; i < sizeof cb; i++) {
        st ^= st << 13; st ^= st >> 17; st ^= st << 5;
        cb[i] = (int8_t)(st & 0xFF);
    }
}

static int run(vqpw_ctx_t *c, int fill, int ch0, int m)
{
    int8_t u[VQPW_DSUB];
    for (int d = 0; d < VQPW_DSUB; d++) u[d] = (int8_t)(fill - 128);
    if (m == 0 && ch0 >= 0) u[0] = (int8_t)(ch0 - 128);
    return vqpw_search_sub(c, u, m, NULL);
}

int main(void)
{
    vqpw_ctx_t c;
    int bad = 0;

    synth_codebook();
    if (vqpw_init(&c, cb, 128) != 0) { printf("init failed\n"); return 1; }

    /* the codebook reproduction is only trustworthy if the CLEAN answers match */
    const int sw127[4] = {7, 37, 33, 32};
    const int sw1  [4] = {54, 46, 48, 45};
    for (int m = 0; m < 4; m++) {
        if (run(&c, 255, -1, m) != sw127[m]) {
            printf("  codebook mismatch: u=+127 m=%d gave %d, board said %d\n",
                   m, run(&c, 255, -1, m), sw127[m]); bad++;
        }
        if (run(&c, 129, -1, m) != sw1[m]) {
            printf("  codebook mismatch: u=+1 m=%d gave %d, board said %d\n",
                   m, run(&c, 129, -1, m), sw1[m]); bad++;
        }
    }
    if (bad) { printf("RESULT: FAIL (codebook no longer reproduces)\n"); return 1; }

    const int stale_127 = run(&c, 255, 128, 0);   /* (a) inherits raw 128 */
    const int dead      = run(&c, 129, 128, 0);   /* (b) u[0] = 0         */
    const int stale_1   = run(&c, 129,   0, 0);   /* (a) inherits raw 0   */

    printf("u=+127: board hw=14   stale->%d   dead->%d   (cannot arbitrate)\n",
           stale_127, stale_127);
    printf("u=+1  : board hw= 9   stale->%d   dead->%d\n", stale_1, dead);

    if (stale_127 != 14)                    { printf("  +127 stale does not reproduce\n"); bad++; }
    if (stale_1   !=  9)                    { printf("  +1 stale does not reproduce\n");   bad++; }
    if (dead      ==  9)                    { printf("  dead is no longer separable\n");   bad++; }

    printf("%s\n", bad ? "  conclusion no longer holds -- re-derive it"
                       : "  a stale byte in slot 0 reproduces the board; "
                         "a dead channel 0 does not");
    printf("RESULT: %s\n", bad ? "FAIL" : "PASS");
    return bad != 0;
}

#else   /* the board log this arbitrates is DEPLOYED geometry only */
#include <stdio.h>
int main(void)
{
    printf("nothing to arbitrate at LEGACY: the board log is M=4 K=64\n");
    printf("RESULT: PASS\n");
    return 0;
}
#endif
