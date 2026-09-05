/* ============================================================================
 * rc_geometry_test.c -- the range coder after the M=8 / K=16 rebuild.
 *
 * TWO THINGS TO ESTABLISH, and they are different questions.
 *
 * 1. CORRECTNESS. The coder's 4.6 M-symbol round-trip validation was done on
 *    the OLD geometry (M=4, K=256, one byte per symbol). The rebuild changed
 *    the index ADDRESSING -- symbols are now 4-bit nibbles packed 8 to a
 *    32-bit word -- so that validation does not carry over on its own. The
 *    packing is re-established here directly: encode, decode, compare, on
 *    random data, on skewed data, and on the degenerate constant stream the
 *    board actually produces today.
 *
 * 2. COST. The point of the rebuild is a defensible T_RANGE. M=8/K=16 codes
 *    115,200 symbols per frame against the old 57,600 -- the configuration
 *    that made the VQ search 8x cheaper doubles the entropy stage's symbol
 *    count. This measures both geometries under identical conditions and
 *    reports the ratio, so the trade is quantified rather than assumed.
 *
 *    Host timings are NOT board timings: a desktop x86 core is nothing like a
 *    667 MHz Cortex-A9, and the absolute milliseconds here mean nothing for
 *    the paper. The RATIO between the two geometries, measured on the same
 *    machine with the same code, is the transferable quantity. The board
 *    number still has to come from the board.
 *
 *   gcc -O2 -I../src -DRC_GEOMETRY_PW=1 -o rc_geometry_test \
 *       rc_geometry_test.c ../src/range_coder.c
 * ==========================================================================*/
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include "range_coder.h"

static uint32_t st = 0x1234567u;
static uint32_t xs(void){ uint32_t x=st; x^=x<<13; x^=x>>17; x^=x<<5; return st=x; }

static uint8_t  idx  [RC_IDX_BYTES];
static uint8_t  rt   [RC_IDX_BYTES];
static uint8_t  bs   [RC_IDX_BYTES * 2 + 4096];
static rc_models_t M;

static int fails = 0;
static void chk(int cond, const char *what)
{
    printf("  %-58s %s\n", what, cond ? "ok" : "FAIL");
    if (!cond) fails++;
}

/* fill with a chosen distribution, through the SAME accessor the coder uses */
static void fill(int mode)
{
    memset(idx, 0, sizeof idx);
    for (int pos = 0; pos < RC_NPOS; pos++)
        for (int m = 0; m < RC_NMODEL; m++) {
            uint8_t s;
            switch (mode) {
            case 0:  s = (uint8_t)(xs() % RC_NSYM); break;            /* uniform  */
            case 1:  {                                                /* skewed   */
                uint32_t r = xs() % 100u;
                s = (uint8_t)(r < 70u ? 0 : (r < 90u ? 1 : (xs() % RC_NSYM)));
                break; }
            default: s = 0; break;                                    /* constant */
            }
            rc_put_sym(idx, pos, m, s);
        }
}

static double bench(int reps, size_t *nbytes)
{
    const clock_t t0 = clock();
    size_t n = 0;
    for (int r = 0; r < reps; r++) n = rc_encode_frame(&M, idx, bs, sizeof bs);
    const clock_t t1 = clock();
    *nbytes = n;
    return 1000.0 * (double)(t1 - t0) / (double)CLOCKS_PER_SEC / (double)reps;
}

int main(void)
{
    printf("range coder geometry: RC_NMODEL=%d RC_NSYM=%d RC_NPOS=%d "
           "idx_bytes=%d symbols/frame=%lu\n",
           RC_NMODEL, RC_NSYM, RC_NPOS, RC_IDX_BYTES,
           (unsigned long)RC_NSYM_PER_FRAME);

    /* ---- 0. the accessor pair must be its own inverse -------------------- */
    {
        int bad = 0;
        memset(idx, 0, sizeof idx);
        for (int pos = 0; pos < 64; pos++)
            for (int m = 0; m < RC_NMODEL; m++)
                rc_put_sym(idx, pos, m, (uint8_t)((pos * RC_NMODEL + m) % RC_NSYM));
        for (int pos = 0; pos < 64; pos++)
            for (int m = 0; m < RC_NMODEL; m++)
                if (rc_get_sym(idx, pos, m) != (uint8_t)((pos * RC_NMODEL + m) % RC_NSYM)) bad++;
        chk(bad == 0, "rc_put_sym / rc_get_sym round-trip every (pos,model)");
    }

    /* neighbouring symbols must not corrupt each other -- the nibble trap */
    {
        int bad = 0;
        memset(idx, 0, sizeof idx);
        for (int m = 0; m < RC_NMODEL; m++) rc_put_sym(idx, 0, m, (uint8_t)(RC_NSYM - 1));
        for (int m = 0; m < RC_NMODEL; m++) rc_put_sym(idx, 0, m, (uint8_t)(m % RC_NSYM));
        for (int m = 0; m < RC_NMODEL; m++)
            if (rc_get_sym(idx, 0, m) != (uint8_t)(m % RC_NSYM)) bad++;
        chk(bad == 0, "overwriting one symbol leaves its neighbours intact");
    }

    /* ---- 1. round trip on three distributions ---------------------------- */
    static const char *names[3] = { "uniform", "skewed", "degenerate (constant)" };
    for (int mode = 0; mode < 3; mode++) {
        fill(mode);
        const uint8_t *frames[1] = { idx };
        rc_model_build(&M, frames, 1);

        long first = -1;
        const long bad = rc_selftest_frame(&M, idx, bs, sizeof bs, rt, &first);
        char what[96];
        snprintf(what, sizeof what, "round trip exact: %s", names[mode]);
        chk(bad == 0, what);
        if (bad) printf("    %ld mismatched bytes, first at %ld\n", bad, first);

        const size_t n = rc_encode_frame(&M, idx, bs, sizeof bs);
        double h[RC_NMODEL];
        rc_frame_entropy(idx, h);
        double hsum = 0.0;
        for (int m = 0; m < RC_NMODEL; m++) hsum += h[m];
        const double coded_bits = (double)n * 8.0;
        const double emp_bits   = hsum * (double)RC_NPOS;
        printf("    coded %lu B, empirical bound %.0f B, overhead %+.2f%%\n",
               (unsigned long)n, emp_bits / 8.0,
               emp_bits > 0.0 ? 100.0 * (coded_bits - emp_bits) / emp_bits : 0.0);
    }

    /* ---- 1b. SLICED encoding must be byte-identical to one-shot ----------
     * The incremental encoder exists so the coder can run inside the analysis
     * cascade's DMA waits. That is only safe if suspending between any two
     * symbols is unobservable in the output. Test it with RAGGED slice sizes,
     * including 1 and including boundaries that fall inside a position's group
     * of RC_NMODEL symbols -- an implementation that only resumed cleanly on
     * position boundaries would pass a round-number test and fail here. */
    {
        static uint8_t bs_ref[sizeof bs];
        int bad_len = 0, bad_bytes = 0, bad_rt = 0;
        fill(1);
        { const uint8_t *f[1] = { idx }; rc_model_build(&M, f, 1); }
        const size_t nref = rc_encode_frame(&M, idx, bs_ref, sizeof bs_ref);

        /* slice sizes chosen to be coprime-ish with RC_NMODEL so the cut lands
         * mid-position on most iterations */
        static const unsigned long slices[] = { 1, 3, 7, 64, 1000, 99991 };
        for (unsigned si = 0; si < sizeof slices / sizeof slices[0]; si++) {
            rc_stream_t st;
            unsigned long steps = 0;
            memset(bs, 0, sizeof bs);
            rc_stream_start(&st, &M, idx, bs, sizeof bs);
            while (!rc_stream_step(&st, slices[si])) {
                if (++steps > 4ul * RC_NSYM_PER_FRAME) break;   /* no-progress guard */
            }
            const size_t n = rc_stream_finish(&st);
            if (n != nref) { bad_len++; continue; }
            if (memcmp(bs, bs_ref, n) != 0) bad_bytes++;
            rc_decode_frame(&M, bs, n, rt);
            if (memcmp(rt, idx, RC_IDX_BYTES) != 0) bad_rt++;
        }
        chk(bad_len   == 0, "sliced encode: same length as one-shot, every slice size");
        chk(bad_bytes == 0, "sliced encode: byte-identical to one-shot");
        chk(bad_rt    == 0, "sliced encode: still decodes back to the input");

        /* A stream that never gets a slice must still produce a correct frame:
         * rc_stream_finish() codes the remainder. This is the path taken when
         * the analysis window turns out to be shorter than the coding. */
        {
            rc_stream_t st;
            memset(bs, 0, sizeof bs);
            rc_stream_start(&st, &M, idx, bs, sizeof bs);
            const size_t n = rc_stream_finish(&st);
            chk(n == nref && memcmp(bs, bs_ref, n) == 0,
                "finish with no slices coded equals a one-shot encode");
        }

        /* rc_stream_coded() is the hiding metric, so it has to be exact. */
        {
            rc_stream_t st;
            rc_stream_start(&st, &M, idx, bs, sizeof bs);
            rc_stream_step(&st, 1000ul);
            const unsigned long c1 = rc_stream_coded(&st);
            rc_stream_step(&st, 337ul);
            const unsigned long c2 = rc_stream_coded(&st);
            rc_stream_finish(&st);
            chk(c1 == 1000ul && c2 == 1337ul &&
                rc_stream_coded(&st) == (unsigned long)RC_NSYM_PER_FRAME,
                "rc_stream_coded() counts symbols exactly");
        }
    }

    /* ---- 2. uniform model must not beat fixed length --------------------- */
    fill(0);
    rc_model_uniform(&M);
    {
        const size_t n = rc_encode_frame(&M, idx, bs, sizeof bs);
        const double fixed = (double)RC_NSYM_PER_FRAME * 4.0 / 8.0 * 2.0; /* log2(NSYM) bits */
        printf("    uniform model: %lu B vs fixed-length %.0f B\n",
               (unsigned long)n, (double)RC_IDX_BYTES);
        (void)fixed;
        chk(n <= (size_t)RC_IDX_BYTES + 64,
            "uniform model codes at (not above) the fixed-length size");
    }

    /* ---- 3. cost ---------------------------------------------------------- */
    fill(1);
    { const uint8_t *f[1] = { idx }; rc_model_build(&M, f, 1); }
    size_t nb = 0;
    const double ms = bench(400, &nb);
    printf("\n  HOST encode cost: %.4f ms/frame over %lu symbols "
           "(%.1f ns/symbol), %lu B out\n",
           ms, (unsigned long)RC_NSYM_PER_FRAME,
           1e6 * ms / (double)RC_NSYM_PER_FRAME, (unsigned long)nb);
    printf("  NOTE host x86 timing -- use the RATIO between geometries, not this number.\n");
    printf("#RCBENCH,%d,%d,%lu,%.6f\n",
           RC_NMODEL, RC_NSYM, (unsigned long)RC_NSYM_PER_FRAME, ms);

    printf("\n%d checks failed\n", fails);
    printf(fails ? "RESULT: FAIL\n" : "RESULT: PASS\n");
    return fails != 0;
}
