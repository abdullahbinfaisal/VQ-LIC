/* Stage 2 of RANS_GUIDE.md 12: encoder -> local decoder at the DEPLOYED
 * geometry (160x90 token grid, 4 groups, 65 context slots, border slot 64).
 *
 * WHAT THIS DOES NOT PROVE. Stage 2 of the guide asks for REAL tables and a
 * REAL index plane. Neither exists here: the tables below are synthesised
 * (integer-only, satisfying the section-8 contract), and the planes are drawn
 * from them. A round trip proves the coder and decoder are mutual inverses.
 * It does not prove compatibility with anything -- that is stage 3.
 *
 * Two table shapes are exercised:
 *   identity  width 64, ids 0..63 used directly as slots
 *   deployed  width 256, ORIGINAL ids scattered over 0..255, border id 256,
 *             65-slot addressing through slot_of_id (CONTEXT_CODEC.md 1-3) */
#include <stdio.h>
#include <string.h>
#include "rans.h"

#define GW     160u
#define GH      90u
#define GN     (GW * GH)
#define NCTX    65u
#define WORST  (2u * GN + 4u)

static uint32_t st = 0x2468ACE1u;
static uint32_t xs(void) { st ^= st << 13; st ^= st >> 17; st ^= st << 5; return st; }

static uint16_t rows64 [4][NCTX * 2u * 64u];
static uint16_t rows256[4][NCTX * 2u * 256u];
static uint16_t rows_wc[NCTX * 2u * 64u];
static uint8_t  slot_map[4][256];
static uint8_t  surv[4][64];
static uint8_t  planes[4][GN];
static uint8_t  dec[GN];
static uint8_t  onebuf[WORST];
static uint8_t  work[4][WORST];
static uint8_t  tmp[WORST + 1u];
static uint8_t  body[4u * (4u + WORST)];

static int fails = 0;
static void check(int ok, const char *what)
{
    printf("  %s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) fails++;
}

static void show(const char *tag, const uint8_t *b, size_t n)
{
    printf("    %-10s", tag);
    for (size_t i = 0; i < n; i++) printf(" %02X", (unsigned)b[i]);
    printf("\n");
}

/* Test-only slot synthesis, integer only, peaked at symbol `peak`. */
static void make_row(uint16_t *row, uint32_t nsym, uint32_t peak, uint32_t sharp)
{
    uint32_t wgt[256], f[256], acc = 0u, arg = 0u;
    uint64_t tot = 0u;
    for (uint32_t s = 0; s < nsym; s++) {
        const uint32_t d = (s > peak) ? s - peak : peak - s;
        wgt[s] = 1u + ((d < 12u) ? (sharp >> d) : 0u) + (xs() & 0x3Fu);
        tot += wgt[s];
    }
    for (uint32_t s = 0; s < nsym; s++) {
        uint64_t v = ((uint64_t)wgt[s] << 16) / tot;
        if (v == 0u) v = 1u;
        f[s] = (uint32_t)v;
        acc += f[s];
        if (f[s] > f[arg]) arg = s;
    }
    if (acc < 65536u) f[arg] += 65536u - acc;
    else {
        uint32_t over = acc - 65536u;
        for (uint32_t s = 0; s < nsym && over; s++) {
            uint32_t take = (f[s] > 1u) ? f[s] - 1u : 0u;
            if (take > over) take = over;
            f[s] -= take; over -= take;
        }
    }
    uint32_t c = 0u;
    for (uint32_t s = 0; s < nsym; s++) { row[s] = (uint16_t)f[s]; row[nsym + s] = (uint16_t)c; c += f[s]; }
}

static void make_tables_identity(uint16_t *rows)
{
    for (uint32_t c = 0; c < NCTX; c++)
        make_row(rows + (size_t)c * 128u, 64u, (c == NCTX - 1u) ? (xs() % 64u) : c,
                 20000u + (xs() & 0x7FFFu));
}

static void make_tables_mapped(uint16_t *rows, uint32_t g)
{
    for (uint32_t c = 0; c < NCTX; c++)
        make_row(rows + (size_t)c * 512u, 256u,
                 (c == NCTX - 1u) ? surv[g][xs() % 64u] : surv[g][c],
                 20000u + (xs() & 0x7FFFu));
}

/* f = 1 for symbol v in the slots a constant-v plane touches: guide 6 worst case */
static void make_worst_tables(uint16_t *rows, uint32_t v)
{
    make_tables_identity(rows);
    for (uint32_t c = 0; c < NCTX; c++) {
        if (c != v && c != NCTX - 1u) continue;
        uint16_t *row = rows + (size_t)c * 128u;
        const uint32_t base = 65535u / 63u, rem = 65535u - base * 63u;
        uint32_t acc = 0u;
        for (uint32_t s = 0; s < 64u; s++) {
            uint32_t fs = (s == v) ? 1u : base;
            if (s == ((v + 1u) % 64u)) fs += rem;
            row[s] = (uint16_t)fs; row[64u + s] = (uint16_t)acc; acc += fs;
        }
    }
}

static uint32_t search(const uint16_t *cdf, uint32_t nsym, uint32_t slot)
{
    uint32_t lo = 0u, hi = nsym;
    while (lo + 1u < hi) { const uint32_t mid = (lo + hi) >> 1; if (cdf[mid] <= slot) lo = mid; else hi = mid; }
    return lo;
}

/* Draw a plane FROM the model, raster order, so it has the neighbour structure
 * the context coder exploits. Only ids that can occur are drawn. */
static void plane_from_model(uint8_t *pl, const rans_tables_t *T)
{
    for (uint32_t t = 0; t < GN; t++) {
        const uint32_t c = rans_ctx_slot(T, rans_context_id(pl, t, GW, T->k_border));
        const uint16_t *row = T->rows + (size_t)c * 2u * T->nsym;
        uint32_t s;
        do { s = search(row + T->nsym, T->nsym, xs() & 0xFFFFu); }
        while (T->slot_of_id ? (T->slot_of_id[s] == RANS_SLOT_NONE) : (s >= T->k_border));
        pl[t] = (uint8_t)s;
    }
}

static size_t roundtrip(const char *label, const rans_tables_t *T, const uint8_t *pl)
{
    rans_enc_t e;
    const uint8_t *s = 0;
    rans_enc_begin(&e, pl, GN, GW, T, onebuf, sizeof onebuf);
    const size_t len = rans_enc_finish(&e, &s);
    const int rc = s ? rans_decode_ctx(T, GW, s, len, dec, GN) : -9;
    const int eq = s && memcmp(dec, pl, GN) == 0;
    printf("  [%s] n=%u  stream %u B  (%u.%03u B/sym)  enc err=%d  decode rc=%d  indices %s\n",
           label, (unsigned)GN, (unsigned)len, (unsigned)(len / GN),
           (unsigned)((len * 1000u / GN) % 1000u), e.err, rc, eq ? "IDENTICAL" : "DIFFER");
    if (s) show("first 12", s, len < 12u ? len : 12u);
    char m[200];
    snprintf(m, sizeof m, "%s: decoded plane identical AND read pointer exactly on EOF", label);
    check(len > 0u && rc == 0 && eq, m);
    return len;
}

int main(void)
{
    printf("STAGE 2 -- encoder -> local decoder, grid %ux%u, G=4, %u slots, border slot %u\n",
           (unsigned)GW, (unsigned)GH, (unsigned)NCTX, (unsigned)(NCTX - 1u));
    printf("  SYNTHETIC tables and planes -- the real-tables criterion of stage 2 is NOT met\n");
#ifdef RANS_DEBUG
    printf("  RANS_DEBUG on: state invariant 2^23 <= x < 2^31 asserted after every update\n");
#endif

    rans_tables_t T64[4], T256[4];
    for (uint32_t g = 0; g < 4u; g++) {
        memset(slot_map[g], RANS_SLOT_NONE, sizeof slot_map[g]);
        for (uint32_t j = 0; j < 64u; j++) {
            const uint8_t id = (uint8_t)((j * 37u + 11u * g + 3u) & 0xFFu);
            surv[g][j] = id;
            slot_map[g][id] = (uint8_t)j;
        }
        make_tables_identity(rows64[g]);
        make_tables_mapped(rows256[g], g);
        T64[g]  = (rans_tables_t){ 64u,  NCTX, 64u,  0,           rows64[g]  };
        T256[g] = (rans_tables_t){ 256u, NCTX, 256u, slot_map[g], rows256[g] };
        check(rans_tables_check(&T64[g]) == 0 && rans_tables_check(&T256[g]) == 0,
              "synthetic tables satisfy the section-8 contract");
    }
    printf("  deployed shape: group 0 surviving ids begin %u %u %u %u ... (original ids, not 0..63)\n",
           (unsigned)surv[0][0], (unsigned)surv[0][1], (unsigned)surv[0][2], (unsigned)surv[0][3]);

    printf("\n-- one group at a time --\n");
    plane_from_model(planes[0], &T64[0]);
    const size_t len_model = roundtrip("identity width 64, model-drawn plane", &T64[0], planes[0]);

    for (uint32_t t = 0; t < GN; t++) planes[1][t] = (uint8_t)(xs() % 64u);
    (void)roundtrip("identity width 64, uniform random plane", &T64[1], planes[1]);

    plane_from_model(planes[2], &T256[2]);
    (void)roundtrip("deployed width 256, original ids via slot map", &T256[2], planes[2]);

    memset(planes[3], 7, GN);
    (void)roundtrip("identity width 64, constant plane", &T64[3], planes[3]);

    printf("\n-- worst case, guide 6: f = 1 on every token --\n");
    make_worst_tables(rows_wc, 5u);
    const rans_tables_t TW = { 64u, NCTX, 64u, 0, rows_wc };
    check(rans_tables_check(&TW) == 0, "worst-case tables satisfy the contract");
    static uint8_t pw[GN];
    memset(pw, 5, GN);
    const size_t lw = roundtrip("identity width 64, f=1 plane", &TW, pw);
    printf("    expected exactly 2n+4 = %u B, produced %u B\n", (unsigned)WORST, (unsigned)lw);
    check(lw == WORST, "worst-case stream is exactly 2n+4 bytes");

    printf("\n-- decoder must refuse a damaged stream, not read past it (9.13) --\n");
    {
        rans_enc_t e; const uint8_t *s = 0;
        rans_enc_begin(&e, planes[0], GN, GW, &T64[0], onebuf, sizeof onebuf);
        const size_t len = rans_enc_finish(&e, &s);
        const int r_short = rans_decode_ctx(&T64[0], GW, s, len - 1u, dec, GN);
        memcpy(tmp, s, len); tmp[len] = 0x00u;
        const int r_long = rans_decode_ctx(&T64[0], GW, tmp, len + 1u, dec, GN);
        printf("    truncated by 1 byte -> rc=%d (expect -1)   one extra byte -> rc=%d (expect -2)\n",
               r_short, r_long);
        check(r_short == -1 && r_long == -2, "truncation and trailing bytes both detected");
        check(len == len_model, "re-encoding the same plane gives the same length");

        printf("\n-- resumability, guide 11: random slices must give IDENTICAL bytes --\n");
        rans_enc_t e2; int slices = 0;
        rans_enc_begin(&e2, planes[0], GN, GW, &T64[0], work[0], WORST);
        while (!rans_enc_step(&e2, 1u + (xs() % 257u))) slices++;
        slices++;
        const size_t len2 = e2.cap - e2.p;
        printf("    one-shot %u B, sliced %u B over %d slices, err=%d\n",
               (unsigned)len, (unsigned)len2, slices, e2.err);
        check(e2.err == 0 && len2 == len && memcmp(e2.buf + e2.p, s, len) == 0,
              "sliced stream byte-identical to one-shot");
    }

    printf("\n-- four groups, deployed shape, body, mode decision (guide 14) --\n");
    const uint8_t *P[4]; const rans_tables_t *TP[4]; uint8_t *WK[4];
    for (int g = 0; g < 4; g++) { plane_from_model(planes[g], &T256[g]); P[g] = planes[g]; TP[g] = &T256[g]; WK[g] = work[g]; }
    rans_frame_t F;
    rans_frame_begin(&F, P, TP, GN, GW, WK, WORST);
    int steps = 0;
    while (!rans_frame_step(&F, 64u)) steps++;            /* EDGE_BG_SLICE_SYMS */
    steps++;
    rans_mode_t mode = RANS_MODE_CONTEXT;
    const size_t body_len = rans_frame_finish(&F, body, sizeof body, &mode);
    printf("    %d slices of 64 tokens; group streams %u %u %u %u B; body %u B; raw would be %u B; mode %d\n",
           steps, (unsigned)F.len[0], (unsigned)F.len[1], (unsigned)F.len[2], (unsigned)F.len[3],
           (unsigned)body_len, (unsigned)(GN * 4u), (int)mode);
    check(F.err == 0 && mode == RANS_MODE_CONTEXT, "coded body smaller than raw -> mode 3");

    size_t o = 0u; int ok = 1;
    for (int g = 0; g < 4 && ok; g++) {
        const uint32_t L = (uint32_t)body[o] | ((uint32_t)body[o + 1] << 8)
                         | ((uint32_t)body[o + 2] << 16) | ((uint32_t)body[o + 3] << 24);
        o += 4u;
        rans_enc_t e1; const uint8_t *s1 = 0;
        rans_enc_begin(&e1, planes[g], GN, GW, &T256[g], onebuf, sizeof onebuf);
        const size_t l1 = rans_enc_finish(&e1, &s1);
        const int rc = rans_decode_ctx(&T256[g], GW, body + o, L, dec, GN);
        const int same_as_oneshot = (l1 == L && memcmp(s1, body + o, L) == 0);
        printf("    group %d: len %u (LE prefix), decode rc=%d, plane %s, bytes %s one-shot\n",
               g, (unsigned)L, rc, memcmp(dec, planes[g], GN) == 0 ? "identical" : "DIFFERS",
               same_as_oneshot ? "identical to" : "DIFFER from");
        ok = (rc == 0 && memcmp(dec, planes[g], GN) == 0 && same_as_oneshot);
        o += L;
    }
    check(ok && o == body_len, "body parses into 4 streams, each decodes exactly, consumed == body length");

    for (int g = 0; g < 4; g++) { memset(planes[g], 5, GN); TP[g] = &TW; }
    rans_frame_begin(&F, P, TP, GN, GW, WK, WORST);
    const size_t raw_len = rans_frame_finish(&F, body, sizeof body, &mode);
    size_t coded = 0u;
    for (int g = 0; g < 4; g++) coded += 4u + F.len[g];
    printf("    worst-case frame: coded body would be %u B >= raw %u B -> mode %d, body %u B\n",
           (unsigned)coded, (unsigned)(GN * 4u), (int)mode, (unsigned)raw_len);
    int raw_ok = (mode == RANS_MODE_RAW && raw_len == GN * 4u);
    for (int g = 0; g < 4 && raw_ok; g++) raw_ok = (memcmp(body + (size_t)g * GN, planes[g], GN) == 0);
    check(raw_ok, "mode 0 fallback: body is the 4 planes, group-major, one byte per index");

    check(rans_choose_mode(GN * 4u, GN, 4u) == RANS_MODE_RAW, "tie (body == raw) goes to raw");
    check(rans_choose_mode(GN * 4u - 1u, GN, 4u) == RANS_MODE_CONTEXT, "one byte under raw stays mode 3");

    printf("\nRESULT: %s (%d checks failed)\n", fails ? "FAIL" : "PASS", fails);
    return fails != 0;
}
