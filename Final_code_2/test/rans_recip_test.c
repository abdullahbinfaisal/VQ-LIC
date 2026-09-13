/* The divide-free rANS encoder must write the SAME BYTES as the divide encoder.
 *
 *   1. the reciprocal table rans_recip_fill() builds meets the exactness bound
 *      m*fs - 2^(31+sh) < 2^sh for EVERY fs in 2..65535;
 *   2. exhaustive worst case: for every fs and every quotient q the encoder can
 *      reach (x < 32768*fs), x = q*fs + fs-1 -- the largest remainder, where an
 *      over-estimate would show first -- and x = q*fs both give q exactly;
 *   3. whole streams on random valid tables (fs = 1 and fs near 65536 forced,
 *      identity and remapped slots, several widths): reference one-shot vs
 *      fast one-shot vs fast in random slices vs slices ALTERNATING between
 *      the two paths, all byte-identical, and the fast stream decodes;
 *   4. both paths stop on the same token with the same error code.
 *
 * Host timing at the end is x86 and says nothing about the Cortex-A9. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "rans.h"

static uint32_t st = 0x2545F491u;
static uint32_t xs(void) { st ^= st << 13; st ^= st >> 17; st ^= st << 5; return st; }

static int fails = 0;
static void check(int ok, const char *what)
{
    printf("  %s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) fails++;
}

#define MAXW   256u
#define MAXC    66u
#define MAXN 14400u

static uint16_t rows[MAXC * 2u * MAXW];
static uint32_t recip[MAXC * MAXW];
static uint8_t  som[MAXW];
static uint8_t  plane[MAXN], dec[MAXN];
static uint8_t  b_ref[RANS_WORST_BYTES(MAXN)], b_fast[RANS_WORST_BYTES(MAXN)];
static uint8_t  b_sl[RANS_WORST_BYTES(MAXN)],  b_alt[RANS_WORST_BYTES(MAXN)];

/* ---- 1 + 2 ---------------------------------------------------------------- */
static void test_reciprocals(void)
{
    /* One fake table whose entries ARE the frequencies 0..65535, so the values
     * checked are the ones the fill function writes, not a re-derivation. */
    static uint16_t fr[256u * 2u * 256u];
    static uint32_t rc[256u * 256u];
    for (uint32_t c = 0u; c < 256u; c++)
        for (uint32_t s = 0u; s < 256u; s++) fr[c * 512u + s] = (uint16_t)(c * 256u + s);
    const rans_tables_t F = { 256u, 256u, 256u, 0, fr, 0 };
    rans_recip_fill(&F, rc);

    unsigned long bound_bad = 0ul, exh_bad = 0ul;
    unsigned long long checked = 0ull;
    for (uint32_t fs = 2u; fs <= 65535u; fs++) {
        const uint32_t sh = 32u - (uint32_t)__builtin_clz(fs - 1u);
        const uint64_t m  = rc[fs];
        const uint64_t e  = m * fs - (1ull << (31u + sh));      /* wraps if m*fs < 2^(31+sh) */
        if (m == 0u || m * fs < (1ull << (31u + sh)) || e >= (1ull << sh)) bound_bad++;

        const uint32_t qmax = RANS_X_MAX_BASE;                  /* x < 32768*fs  ->  q < 32768 */
        for (uint32_t q = 0u; q < qmax; q++) {
            const uint32_t x1 = q * fs + (fs - 1u);
            const uint32_t x0 = q * fs;
            const uint32_t q1 = (uint32_t)(((uint64_t)x1 * m) >> 32) >> (sh - 1u);
            const uint32_t q0 = (uint32_t)(((uint64_t)x0 * m) >> 32) >> (sh - 1u);
            exh_bad += (q1 != q) + (q0 != q);
        }
        checked += 2ull * qmax;
    }
    printf("  fs 2..65535: bound violations %lu; worst-case quotients checked %llu, wrong %lu\n",
           bound_bad, checked, exh_bad);
    check(bound_bad == 0ul, "m*fs - 2^(31+sh) < 2^sh for every fs (exactness bound for x < 2^31)");
    check(exh_bad == 0ul, "every reachable quotient exact at remainder 0 and fs-1, every fs");
}

/* ---- 3 + 4 ---------------------------------------------------------------- */
static void make_row(uint32_t W, uint16_t *row, int kind)
{
    uint32_t w[256] = { 0u }, f[256];
    for (uint32_t s = 0u; s < W; s++) w[s] = 1u + (xs() % 1000u);
    if (kind == 1) { for (uint32_t s = 0u; s < W; s++) w[s] = 1u; w[xs() % W] = 4000000000u; } /* fs=1 + huge */
    if (kind == 2) { for (uint32_t s = 0u; s < W; s++) w[s] = (xs() % 4u == 0u) ? 1u : 0u; w[0] += 1u; }
    if (rans_quantize_to_total(w, W, 65536u, f) != 0) { printf("quantize failed\n"); exit(2); }
    uint32_t cum = 0u;
    for (uint32_t s = 0u; s < W; s++) { row[s] = (uint16_t)f[s]; row[W + s] = (uint16_t)cum; cum += f[s]; }
}

static size_t enc(const rans_tables_t *Tp, rans_tables_t *mut, const uint32_t *rp, uint32_t n, uint32_t w,
                  uint8_t *buf, int mode, int *err, int32_t *t_at)
{
    /* mode 0 ref one-shot, 1 fast one-shot, 2 fast sliced, 3 alternate ref/fast slices */
    rans_enc_t e;
    mut->recip = (mode == 0) ? 0 : rp;
    rans_enc_begin(&e, plane, n, w, Tp, buf, RANS_WORST_BYTES(n));
    if (mode <= 1) {
        (void)rans_enc_step(&e, 0u);
    } else {
        int flip = 0;
        while (!rans_enc_step(&e, 1u + (xs() % 300u)))
            if (mode == 3) { flip ^= 1; mut->recip = flip ? 0 : rp; }
    }
    *err = e.err; *t_at = e.t;
    const uint8_t *s = 0;
    const size_t len = e.err ? 0u : rans_enc_finish(&e, &s);
    if (len) memmove(buf, s, len);
    mut->recip = rp;
    return len;
}

static void test_streams(void)
{
    static const uint32_t Ws[] = { 4u, 16u, 64u, 256u };
    static const uint32_t widths[] = { 1u, 7u, 160u, 64u };
    int trials = 0, same = 0, decoded = 0, err_same = 0, err_trials = 0;

    for (int trial = 0; trial < 240; trial++) {
        const uint32_t W    = Ws[trial % 4];
        const uint32_t nctx = (W < 64u ? W : 64u) + 1u;
        const uint32_t w    = widths[(trial / 4) % 4];
        uint32_t h = 1u + (xs() % 90u);
        while (h * w > MAXN) h--;
        const uint32_t n = h * w;
        const int remap = (W > nctx - 1u) || (trial & 1);

        for (uint32_t c = 0u; c < nctx; c++) make_row(W, rows + (size_t)c * 2u * W, (int)((c + trial) % 3u));
        /* ids that occur map to slots 0..nctx-2; with W > 64 only 64 ids occur */
        const uint32_t nid = nctx - 1u;
        uint8_t ids[256];
        for (uint32_t i = 0u; i < W; i++) ids[i] = (uint8_t)i;
        for (uint32_t i = W - 1u; i > 0u; i--) { uint32_t j = xs() % (i + 1u); uint8_t t = ids[i]; ids[i] = ids[j]; ids[j] = t; }
        memset(som, RANS_SLOT_NONE, sizeof som);
        for (uint32_t i = 0u; i < nid; i++) som[ids[i]] = (uint8_t)(remap ? (nid - 1u - i) : ids[i]);
        if (!remap) for (uint32_t i = 0u; i < nid; i++) som[ids[i]] = ids[i];

        rans_tables_t T = { W, nctx, W, remap ? som : 0, rows, 0 };
        if (!remap && W > nid) { printf("bad setup\n"); exit(2); }
        if (rans_tables_check(&T) != 0) { printf("table check failed, trial %d\n", trial); exit(2); }
        rans_recip_fill(&T, recip);
        T.recip = recip;

        for (uint32_t t = 0u; t < n; t++)
            plane[t] = (t % w && (xs() % 100u) < 55u) ? plane[t - 1u] : ids[xs() % nid];

        int e0, e1, e2, e3; int32_t t0, t1, t2, t3;
        const size_t l0 = enc(&T, &T, recip, n, w, b_ref,  0, &e0, &t0);
        const size_t l1 = enc(&T, &T, recip, n, w, b_fast, 1, &e1, &t1);
        const size_t l2 = enc(&T, &T, recip, n, w, b_sl,   2, &e2, &t2);
        const size_t l3 = enc(&T, &T, recip, n, w, b_alt,  3, &e3, &t3);
        trials++;
        if (l0 && l0 == l1 && l1 == l2 && l2 == l3 && !memcmp(b_ref, b_fast, l0)
            && !memcmp(b_ref, b_sl, l0) && !memcmp(b_ref, b_alt, l0)) same++;
        else if (fails < 4) printf("    trial %d W=%u w=%u n=%u: len %zu %zu %zu %zu err %d %d %d %d\n",
                                   trial, W, w, n, l0, l1, l2, l3, e0, e1, e2, e3);
        if (l1 && rans_decode_ctx(&T, w, b_fast, l1, dec, n) == 0 && !memcmp(dec, plane, n)) decoded++;

        /* 4: poison one token with an id that has no slot, as a left context */
        if (n >= 3u && w > 1u) {
            uint32_t bad_id = 0u;
            while (bad_id < W && som[bad_id] != RANS_SLOT_NONE) bad_id++;
            if (remap && bad_id < W) {
                uint32_t at = n / 2u;
                if ((at + 1u) % w == 0u) at--;      /* its right neighbour must not be column 0 */
                const uint8_t keep = plane[at];
                plane[at] = (uint8_t)bad_id;
                const size_t m0 = enc(&T, &T, recip, n, w, b_ref,  0, &e0, &t0);
                const size_t m1 = enc(&T, &T, recip, n, w, b_fast, 1, &e1, &t1);
                const size_t m2 = enc(&T, &T, recip, n, w, b_sl,   2, &e2, &t2);
                err_trials++;
                if (m0 == 0u && m1 == 0u && m2 == 0u && e0 && e0 == e1 && e1 == e2 && t0 == t1 && t1 == t2) err_same++;
                plane[at] = keep;
            }
        }
    }
    printf("  %d random streams: byte-identical in all four schedules %d, fast stream decodes %d\n",
           trials, same, decoded);
    printf("  %d poisoned streams: same error code and same stopping token %d\n", err_trials, err_same);
    check(same == trials, "reference, fast, fast-sliced and alternating-sliced streams byte-identical");
    check(decoded == trials, "every fast stream decodes back to the plane");
    check(err_trials > 0 && err_same == err_trials, "both paths stop on the same token with the same error");
}

static void host_timing(void)
{
    const uint32_t W = 256u, nctx = 65u, w = 160u, n = 14400u;
    for (uint32_t c = 0u; c < nctx; c++) make_row(W, rows + (size_t)c * 2u * W, 0);
    uint8_t ids[64];
    memset(som, RANS_SLOT_NONE, sizeof som);
    for (uint32_t i = 0u; i < 64u; i++) { ids[i] = (uint8_t)((i * 97u + 7u) & 0xFFu); som[ids[i]] = (uint8_t)i; }
    rans_tables_t T = { W, nctx, W, som, rows, 0 };
    rans_recip_fill(&T, recip);
    for (uint32_t t = 0u; t < n; t++) plane[t] = (t % w && (xs() % 100u) < 55u) ? plane[t - 1u] : ids[xs() % 64u];
    double sec[2];
    for (int mode = 0; mode < 2; mode++) {
        int e; int32_t ta;
        const clock_t c0 = clock();
        for (int r = 0; r < 200; r++) (void)enc(&T, &T, recip, n, w, mode ? b_fast : b_ref, mode, &e, &ta);
        sec[mode] = (double)(clock() - c0) / CLOCKS_PER_SEC;
    }
    printf("  host x86 only, NOT the A9: divide %.1f ns/token, divide-free %.1f ns/token\n",
           1e9 * sec[0] / (200.0 * n), 1e9 * sec[1] / (200.0 * n));
}

int main(void)
{
    printf("rANS divide-free encoder vs the divide encoder: same bytes\n");
    test_reciprocals();
    test_streams();
    host_timing();
    printf("\nRESULT: %s (%d checks failed)\n", fails ? "FAIL" : "PASS", fails);
    return fails != 0;
}
