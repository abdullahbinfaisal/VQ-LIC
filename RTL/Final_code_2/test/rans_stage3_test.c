/* Stage 3 of RANS_GUIDE.md 12 -- AS FAR AS IT CAN GO WITHOUT THE REFERENCE.
 *
 * Stage 3 ACCEPTANCE is a payload byte-identical to the one lib/codec.py
 * produces for the same index map. lib/context.py and lib/codec.py are not
 * available, so acceptance is
 * evaluated only when a reference dump directory is supplied (section F), and
 * nothing else in this file claims it.
 *
 *   A  CONTEXT_CODEC.md 3.5 on small vectors worked BY HAND from the rule text.
 *      These test the code against MY READING of the rule. A shared misreading
 *      passes -- which is exactly why they cannot replace the reference.
 *   B  3.4 reconstruction from a ROM in the section-3 binary layout, including
 *      the claim that an unpopulated slot holding the top-16 of the marginal
 *      reconstructs to the marginal EXACTLY.
 *   C  the section-7 header, bytes worked by hand from the field table.
 *   D  four groups end to end: ROM -> tables -> payload -> own decoder, the 8.2
 *      context-agreement check, and sliced == one-shot.
 *   E  the 7.1 mode-0 fallback on a frame built to hit it.
 *   F  given a reference dump directory: the real acceptance check.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rans.h"

#define IMG_W   1280u
#define IMG_H    720u
#define GW      ((IMG_W + 7u) / 8u)
#define GH      ((IMG_H + 7u) / 8u)
#define GN      (GW * GH)
#define WORST   (2u * GN + 4u)
#define SLOTW   (RANS_NCTX_SLOTS * RANS_ROW_ENTRIES)
#define PAYCAP  (RANS_HDR_BYTES + 4u * (4u + WORST))

static uint32_t st = 0x13579BDFu;
static uint32_t xs(void) { st ^= st << 13; st ^= st >> 17; st ^= st << 5; return st; }

static uint8_t  rom[RANS_ROM_BYTES], rom_fb[RANS_ROM_BYTES];
static uint16_t rows[4][SLOTW], rows_fb[4][SLOTW];
static uint8_t  slot_of_id[4][256];
static uint8_t  surv[4][64];
static uint8_t  planes[4][GN];
static uint8_t  dec[GN];
static uint8_t  work[4][WORST];
static uint8_t  pay[PAYCAP], pay2[PAYCAP];

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

static void put16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v & 0xFFu); p[1] = (uint8_t)((v >> 8) & 0xFFu); }
static uint32_t get16(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8); }
static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* ---- A ------------------------------------------------------------------- */
static void q_case(const char *name, const uint32_t *w, uint32_t n, uint32_t M,
                   int want_rc, const uint32_t *want)
{
    uint32_t f[16];
    memset(f, 0, sizeof f);
    const int rc = rans_quantize_to_total(w, n, M, f);
    int ok = (rc == want_rc);
    if (ok && rc == 0)
        for (uint32_t i = 0; i < n; i++) if (f[i] != want[i]) ok = 0;
    printf("  %s\n    w =", name);
    for (uint32_t i = 0; i < n; i++) printf(" %u", (unsigned)w[i]);
    printf("   M = %u\n", (unsigned)M);
    if (want_rc == 0) {
        printf("    expected f =");
        for (uint32_t i = 0; i < n; i++) printf(" %u", (unsigned)want[i]);
        printf("\n");
    } else {
        printf("    expected rc = %d\n", want_rc);
    }
    if (rc == 0) {
        printf("    produced f =");
        for (uint32_t i = 0; i < n; i++) printf(" %u", (unsigned)f[i]);
        printf("\n");
    } else {
        printf("    produced rc = %d\n", rc);
    }
    check(ok, name);
}

/* ---- ROM synthesis in the spec 3 layout ---------------------------------- */
static void make_marginal(uint16_t *marg, uint32_t g, int fallback, uint32_t v)
{
    uint32_t w[256], f[256];
    for (uint32_t s = 0; s < 256u; s++) w[s] = fallback ? 1000u : 1u + (xs() % 4u);
    if (fallback) w[v] = 1u;
    else for (uint32_t j = 0; j < 64u; j++) w[surv[g][j]] = 2000u + (xs() % 3000u);
    (void)rans_quantize_to_total(w, 256u, 65536u, f);
    for (uint32_t s = 0; s < 256u; s++) marg[s] = (uint16_t)f[s];
}

/* spec 3.1: an unpopulated slot holds a copy of the marginal. In the 48-byte
 * format that can only be its 16 most probable entries (ties to the lower id). */
static void top16_of_marginal(const uint16_t *marg, uint8_t *sym, uint16_t *freq)
{
    uint8_t taken[256];
    memset(taken, 0, sizeof taken);
    for (uint32_t k = 0; k < 16u; k++) {
        int best = -1;
        for (uint32_t s = 0; s < 256u; s++)
            if (!taken[s] && (best < 0 || marg[s] > marg[best])) best = (int)s;
        taken[best] = 1u;
        sym[k]  = (uint8_t)best;
        freq[k] = marg[best];
    }
}

static void populated(uint32_t g, uint32_t slot, uint8_t *sym, uint16_t *freq,
                      int fallback, uint32_t vj)
{
    const uint32_t S16 = 45000u + (xs() % 18000u);
    uint32_t w[16], f[16];
    for (uint32_t k = 0; k < 16u; k++) w[k] = 64u - 3u * k + (xs() % 3u);
    (void)rans_quantize_to_total(w, 16u, S16, f);
    for (uint32_t k = 0; k < 16u; k++) {
        uint32_t j = (slot + k) % 64u;
        if (fallback) { j = (slot + k) % 63u; if (j >= vj) j++; }   /* never the fallback id */
        sym[k]  = surv[g][j];
        freq[k] = (uint16_t)f[k];
    }
    for (uint32_t k = 1; k < 16u; k++)                              /* spec 3.2: descending */
        for (uint32_t j = k; j > 0u && freq[j - 1u] < freq[j]; j--) {
            const uint16_t tf = freq[j]; freq[j] = freq[j - 1u]; freq[j - 1u] = tf;
            const uint8_t  ts = sym[j];  sym[j]  = sym[j - 1u];  sym[j - 1u]  = ts;
        }
}

static int is_unpopulated(uint32_t slot) { return slot < 64u && (slot % 7u) == 3u; }

static void build_rom(uint8_t *R, int fallback)
{
    for (uint32_t g = 0; g < 4u; g++) {
        const uint32_t vj = 5u;
        uint16_t marg[256];
        make_marginal(marg, g, fallback, surv[g][vj]);
        uint8_t *G = R + (size_t)g * RANS_ROM_GROUP_BYTES;
        for (uint32_t slot = 0; slot < RANS_NCTX_SLOTS; slot++) {
            uint8_t sym[16]; uint16_t freq[16];
            if (!fallback && is_unpopulated(slot)) top16_of_marginal(marg, sym, freq);
            else populated(g, slot, sym, freq, fallback, vj);
            uint8_t *tb = G + (size_t)slot * RANS_CTX_TABLE_BYTES;
            memcpy(tb, sym, 16);
            for (uint32_t k = 0; k < 16u; k++) put16(tb + 16u + 2u * k, freq[k]);
        }
        for (uint32_t s = 0; s < 256u; s++)
            put16(G + (size_t)RANS_NCTX_SLOTS * RANS_CTX_TABLE_BYTES + 2u * s, marg[s]);
    }
}

static uint32_t search256(const uint16_t *cdf, uint32_t slot)
{
    uint32_t lo = 0u, hi = 256u;
    while (lo + 1u < hi) { const uint32_t mid = (lo + hi) >> 1; if (cdf[mid] <= slot) lo = mid; else hi = mid; }
    return lo;
}

static void draw_plane(uint8_t *pl, const rans_tables_t *T)
{
    for (uint32_t t = 0; t < GN; t++) {
        const uint32_t c = rans_ctx_slot(T, rans_context_id(pl, t, GW, T->k_border));
        const uint16_t *row = T->rows + (size_t)c * RANS_ROW_ENTRIES;
        uint32_t s;
        do { s = search256(row + 256u, xs() & 0xFFFFu); } while (T->slot_of_id[s] == RANS_SLOT_NONE);
        pl[t] = (uint8_t)s;
    }
}

static long load_file(const char *dir, const char *name, uint8_t *buf, size_t cap)
{
    char path[1024];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    FILE *fp = fopen(path, "rb");
    if (!fp) return -1;
    const size_t n = fread(buf, 1, cap, fp);
    const int more = (fgetc(fp) != EOF);
    fclose(fp);
    return more ? -2 : (long)n;
}

static const char *region(const uint8_t *p, size_t len, size_t off)
{
    static char buf[96];
    if (off < RANS_HDR_BYTES) { snprintf(buf, sizeof buf, "header byte %u", (unsigned)off); return buf; }
    if (p[4] != 3u) {
        snprintf(buf, sizeof buf, "mode-%u body byte %u", (unsigned)p[4], (unsigned)(off - RANS_HDR_BYTES));
        return buf;
    }
    size_t o = RANS_HDR_BYTES;
    for (uint32_t g = 0; g < 4u && o + 4u <= len; g++) {
        const uint32_t L = get32(p + o);
        if (off < o + 4u) { snprintf(buf, sizeof buf, "group %u length prefix", (unsigned)g); return buf; }
        if (off < o + 4u + L) {
            snprintf(buf, sizeof buf, "group %u stream byte %u of %u", (unsigned)g,
                     (unsigned)(off - o - 4u), (unsigned)L);
            return buf;
        }
        o += 4u + L;
    }
    snprintf(buf, sizeof buf, "past the reference body");
    return buf;
}

int main(int argc, char **argv)
{
    printf("STAGE 3 -- tables, container, fallback (CONTEXT_CODEC.md 3, 7)\n");
    printf("  image %ux%u -> token grid %ux%u, n = %u, G = 4, K = %u\n",
           (unsigned)IMG_W, (unsigned)IMG_H, (unsigned)GW, (unsigned)GH, (unsigned)GN,
           (unsigned)RANS_K_NOMINAL);
    printf("  reference implementation lib/context.py, lib/codec.py: NOT AVAILABLE\n");
#ifdef RANS_DEBUG
    printf("  RANS_DEBUG on: state invariant asserted after every update\n");
#endif

    for (uint32_t g = 0; g < 4u; g++) {
        memset(slot_of_id[g], RANS_SLOT_NONE, sizeof slot_of_id[g]);
        for (uint32_t j = 0; j < 64u; j++) {
            const uint8_t id = (uint8_t)((j * 97u + 29u * g + 7u) & 0xFFu);
            surv[g][j] = id;
            slot_of_id[g][id] = (uint8_t)j;
        }
    }

    /* ---- A ---- */
    printf("\n-- A. spec 3.5 on vectors worked by hand from the rule text --\n");
    printf("  (checks the code against my reading of the rule, NOT against the reference)\n");
    {
        static const uint32_t w1[] = { 5, 3, 3, 1 },           e1[] = { 5, 2, 2, 1 };
        static const uint32_t w2[] = { 2, 5, 5, 2 },           e2[] = { 2, 6, 5, 2 };
        static const uint32_t w3[] = { 10, 10, 1, 1, 1, 1 },   e3[] = { 2, 2, 1, 1, 1, 1 };
        static const uint32_t w4[] = { 50, 1, 1, 1, 1, 1 },    e4[] = { 2, 1, 1, 1, 1, 1 };
        static const uint32_t w5[] = { 0, 0, 0 },              e5[] = { 2, 2, 1 };
        static const uint32_t w6[] = { 1, 1, 1 };
        q_case("d > 0: the leftover unit goes to the largest weight", w1, 4u, 10u, 0, e1);
        q_case("d > 0 with tied weights: the lower index wins the tie", w2, 4u, 15u, 0, e2);
        q_case("d < 0: one unit off each visited f > 1, in order", w3, 6u, 8u, 0, e3);
        q_case("d < 0 wrapping the order more than once", w4, 6u, 7u, 0, e4);
        q_case("sum(w) == 0: every weight becomes 1", w5, 3u, 5u, 0, e5);
        q_case("impossible: M below the >= 1 floor is refused", w6, 3u, 2u, -1, 0);
    }

    /* ---- B ---- */
    printf("\n-- B. ROM in the spec 3 layout -> 65 dense slots per group (spec 3.4) --\n");
    build_rom(rom, 0);
    printf("  ROM %u B = 4 x (65 x %u + 512); spec 3 says 14,528\n",
           (unsigned)RANS_ROM_BYTES, (unsigned)RANS_CTX_TABLE_BYTES);
    check(RANS_ROM_BYTES == 14528u, "ROM size matches spec 3");

    rans_tables_t T[4];
    int exp_ok = 1;
    for (uint32_t g = 0; g < 4u; g++) {
        const int rc = rans_rom_expand_group(rom + (size_t)g * RANS_ROM_GROUP_BYTES, rows[g]);
        T[g] = (rans_tables_t){ 256u, RANS_NCTX_SLOTS, RANS_K_NOMINAL, slot_of_id[g], rows[g] };
        const int tc = rans_tables_check(&T[g]);
        printf("  group %u: expand rc=%d, table contract rc=%d\n", (unsigned)g, rc, tc);
        if (rc != 0 || tc != 0) exp_ok = 0;
    }
    check(exp_ok, "every slot of every group reconstructs: sum 65536, every f >= 1, cdf consistent");

    {
        int stored_ok = 1, unpop_ok = 1;
        unsigned nstored = 0u, nunpop = 0u;
        for (uint32_t g = 0; g < 4u; g++) {
            const uint8_t *G = rom + (size_t)g * RANS_ROM_GROUP_BYTES;
            uint16_t marg[256];
            for (uint32_t s = 0; s < 256u; s++)
                marg[s] = (uint16_t)get16(G + (size_t)RANS_NCTX_SLOTS * RANS_CTX_TABLE_BYTES + 2u * s);
            for (uint32_t c = 0; c < RANS_NCTX_SLOTS; c++) {
                const uint8_t  *tb = G + (size_t)c * RANS_CTX_TABLE_BYTES;
                const uint16_t *r  = rows[g] + (size_t)c * RANS_ROW_ENTRIES;
                for (uint32_t k = 0; k < 16u; k++) {
                    if (r[tb[k]] != get16(tb + 16u + 2u * k)) stored_ok = 0;
                    nstored++;
                }
                if (is_unpopulated(c)) {
                    nunpop++;
                    for (uint32_t s = 0; s < 256u; s++) if (r[s] != marg[s]) unpop_ok = 0;
                }
            }
        }
        printf("  %u stored entries checked; %u unpopulated slots compared with the marginal\n",
               nstored, nunpop);
        check(stored_ok, "every stored top-16 frequency survives reconstruction exactly");
        check(unpop_ok, "an unpopulated slot (top-16 of the marginal) reconstructs to the marginal EXACTLY");

        const uint8_t *tb = rom;
        uint32_t s16 = 0u;
        uint8_t in16[256];
        memset(in16, 0, sizeof in16);
        for (uint32_t k = 0; k < 16u; k++) { s16 += get16(tb + 16u + 2u * k); in16[tb[k]] = 1u; }
        uint32_t tmin = 65536u, tmax = 0u, tsum = 0u;
        for (uint32_t s = 0; s < 256u; s++) {
            if (in16[s]) continue;
            const uint32_t v = rows[0][s];
            tsum += v; if (v < tmin) tmin = v; if (v > tmax) tmax = v;
        }
        printf("  group 0 slot 0: stored mass %u, escape %u, tail over 240 symbols sums to %u (min %u, max %u)\n",
               (unsigned)s16, (unsigned)(65536u - s16), (unsigned)tsum, (unsigned)tmin, (unsigned)tmax);
        check(tsum == 65536u - s16 && tmin >= 1u, "tail mass equals the escape mass, every tail f >= 1");
    }
    {
        uint16_t marg0[256], bad[256], fq[16], row[512];
        uint8_t sym[16];
        for (uint32_t s = 0; s < 256u; s++)
            marg0[s] = (uint16_t)get16(rom + (size_t)RANS_NCTX_SLOTS * RANS_CTX_TABLE_BYTES + 2u * s);
        for (uint32_t k = 0; k < 16u; k++) { sym[k] = (uint8_t)k; fq[k] = 1000u; }
        sym[1] = 0u;
        const int r1 = rans_reconstruct_row(sym, fq, marg0, row);
        sym[1] = 1u;
        fq[0] = 65521u; for (uint32_t k = 1; k < 16u; k++) fq[k] = 1u;
        const int r2 = rans_reconstruct_row(sym, fq, marg0, row);
        fq[0] = 65421u;
        const int r3 = rans_reconstruct_row(sym, fq, marg0, row);
        for (uint32_t k = 0; k < 16u; k++) fq[k] = 1000u;
        memcpy(bad, marg0, sizeof bad); bad[3] = 0u;
        const int r4 = rans_reconstruct_row(sym, fq, bad, row);
        printf("  refusals: duplicate symbol rc=%d (want -2), stored sum 65536 rc=%d (want -4),\n"
               "            escape 100 < 240 rc=%d (want -5), zero in marginal rc=%d (want -6)\n",
               r1, r2, r3, r4);
        check(r1 == -2 && r2 == -4 && r3 == -5 && r4 == -6, "malformed tables are refused, never coded");
    }

    /* ---- C ---- */
    printf("\n-- C. header, spec 7 -- expected bytes worked by hand from the field table --\n");
    {
        static const uint8_t want_hdr[17] = { 0x4E, 0x49, 0x43, 0x31, 0x03, 0x04, 0x00, 0x01, 0x10,
                                              0xD0, 0x02, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00 };
        uint8_t hdr[17];
        const size_t hn = rans_write_header(hdr, sizeof hdr, RANS_MODE_CONTEXT, RANS_K_NOMINAL, IMG_H, IMG_W);
        printf("  NIC1 | mode 3 | G 4 | K 256 u16 LE | prob_bits 16 | H 720 u32 LE | W 1280 u32 LE\n");
        show("expected", want_hdr, 17u);
        show("produced", hdr, hn);
        check(hn == 17u && memcmp(hdr, want_hdr, 17) == 0, "17-byte header matches the spec 7 layout");
    }

    /* ---- D ---- */
    printf("\n-- D. four groups end to end: ROM tables -> payload -> own decoder --\n");
    const uint8_t *P[4]; const rans_tables_t *TP[4]; uint8_t *WK[4];
    for (uint32_t g = 0; g < 4u; g++) {
        draw_plane(planes[g], &T[g]);
        P[g] = planes[g]; TP[g] = &T[g]; WK[g] = work[g];
    }
    rans_frame_t F;
    rans_frame_begin(&F, P, TP, GN, GW, WK, WORST);
    int slices = 0;
    while (!rans_frame_step(&F, 64u)) slices++;
    slices++;
    rans_mode_t mode = RANS_MODE_RAW;
    const size_t plen = rans_frame_payload(&F, IMG_H, IMG_W, pay, sizeof pay, &mode);
    printf("  %d slices of 64; streams %u %u %u %u B; payload %u B = 17 + body; mode %d\n",
           slices, (unsigned)F.len[0], (unsigned)F.len[1], (unsigned)F.len[2], (unsigned)F.len[3],
           (unsigned)plen, (int)mode);
    show("first 24", pay, plen < 24u ? plen : 24u);
    check(plen > RANS_HDR_BYTES && mode == RANS_MODE_CONTEXT, "payload produced in mode 3");

    {
        const int hdr_ok = memcmp(pay, "NIC1", 4) == 0 && pay[4] == 3u && pay[5] == 4u
                        && get16(pay + 6) == 256u && pay[8] == 16u
                        && get32(pay + 9) == IMG_H && get32(pay + 13) == IMG_W;
        const uint32_t hh = (get32(pay + 9) + 7u) / 8u, ww = (get32(pay + 13) + 7u) / 8u, nn = hh * ww;
        printf("  header parses: mode %u G %u K %u prob_bits %u H %u W %u -> h %u w %u n %u\n",
               (unsigned)pay[4], (unsigned)pay[5], (unsigned)get16(pay + 6), (unsigned)pay[8],
               (unsigned)get32(pay + 9), (unsigned)get32(pay + 13), (unsigned)hh, (unsigned)ww, (unsigned)nn);
        check(hdr_ok && ww == GW && nn == GN, "header fields and the derived token grid");

        size_t o = RANS_HDR_BYTES;
        int all_ok = 1;
        unsigned ctx_bad = 0u;
        for (uint32_t g = 0; g < 4u && all_ok; g++) {
            const uint32_t L = get32(pay + o);
            o += 4u;
            const int rc = rans_decode_ctx(&T[g], ww, pay + o, L, dec, nn);
            const int eq = memcmp(dec, planes[g], nn) == 0;
            /* spec 8.2: bulk contexts from the index map against incremental
             * contexts from what the decoder produced */
            for (uint32_t t = 0; t < nn; t++) {
                const uint32_t bulk = rans_context_id(planes[g], t, ww, RANS_K_NOMINAL);
                const uint32_t incr = (t % ww) ? (uint32_t)dec[t - 1u] : RANS_K_NOMINAL;
                if (bulk != incr) ctx_bad++;
            }
            printf("  group %u: len %u, decode rc=%d, ids %s\n", (unsigned)g, (unsigned)L, rc,
                   eq ? "identical" : "DIFFER");
            all_ok = (rc == 0 && eq);
            o += L;
        }
        check(all_ok && o == plen, "every stream decodes to the original ids; payload consumed exactly");
        printf("  spec 8.2: %u context disagreements over %u tokens\n", ctx_bad, (unsigned)(4u * nn));
        check(ctx_bad == 0u, "bulk and incremental contexts agree token for token");
    }
    rans_frame_begin(&F, P, TP, GN, GW, WK, WORST);
    {
        const size_t plen2 = rans_frame_payload(&F, IMG_H, IMG_W, pay2, sizeof pay2, &mode);
        check(plen2 == plen && memcmp(pay, pay2, plen) == 0, "one-shot payload byte-identical to the sliced one");
    }

    /* ---- E ---- */
    printf("\n-- E. mode-0 fallback on a frame built to hit it (spec 7.1) --\n");
    {
        build_rom(rom_fb, 1);
        rans_tables_t TF[4];
        int fb_ok = 1;
        for (uint32_t g = 0; g < 4u; g++) {
            const int rc = rans_rom_expand_group(rom_fb + (size_t)g * RANS_ROM_GROUP_BYTES, rows_fb[g]);
            TF[g] = (rans_tables_t){ 256u, RANS_NCTX_SLOTS, RANS_K_NOMINAL, slot_of_id[g], rows_fb[g] };
            if (rc != 0 || rans_tables_check(&TF[g]) != 0) fb_ok = 0;
            memset(planes[g], surv[g][5], GN);
            TP[g] = &TF[g];
        }
        check(fb_ok, "fallback ROM reconstructs");
        rans_frame_begin(&F, P, TP, GN, GW, WK, WORST);
        const size_t flen = rans_frame_payload(&F, IMG_H, IMG_W, pay, sizeof pay, &mode);
        size_t coded = 0u;
        int worst_ok = 1;
        for (uint32_t g = 0; g < 4u; g++) { coded += 4u + F.len[g]; if (F.len[g] != WORST) worst_ok = 0; }
        printf("  streams %u %u %u %u B (2n+4 = %u); coded body would be %u B >= raw %u B -> mode %d, payload %u B\n",
               (unsigned)F.len[0], (unsigned)F.len[1], (unsigned)F.len[2], (unsigned)F.len[3],
               (unsigned)WORST, (unsigned)coded, (unsigned)(4u * GN), (int)mode, (unsigned)flen);
        show("header", pay, 17u);
        int raw_ok = (mode == RANS_MODE_RAW && flen == RANS_HDR_BYTES + 4u * GN
                      && memcmp(pay, "NIC1", 4) == 0 && pay[4] == 0u);
        for (uint32_t g = 0; g < 4u && raw_ok; g++)
            raw_ok = memcmp(pay + RANS_HDR_BYTES + (size_t)g * GN, planes[g], GN) == 0;
        check(worst_ok, "every stream hits the exact 2n+4 worst case");
        check(raw_ok, "mode 0: header mode byte 0, body = the 4 planes of ORIGINAL ids, group-major");
    }

    /* ---- F ---- */
    if (argc >= 2) {
        printf("\n-- F. STAGE 3 ACCEPTANCE against the reference dump in %s --\n", argv[1]);
        static uint8_t ref_rom[RANS_ROM_BYTES], ref_slots[4u * 256u], ref_idx[4u * GN], ref_pay[PAYCAP];
        const long nr = load_file(argv[1], "rom.bin",     ref_rom,   sizeof ref_rom);
        const long ns = load_file(argv[1], "slots.bin",   ref_slots, sizeof ref_slots);
        const long ni = load_file(argv[1], "idx.bin",     ref_idx,   sizeof ref_idx);
        const long np = load_file(argv[1], "payload.bin", ref_pay,   sizeof ref_pay);
        printf("  rom.bin %ld B, slots.bin %ld B, idx.bin %ld B, payload.bin %ld B\n", nr, ns, ni, np);
        int ok = (nr == (long)RANS_ROM_BYTES && ns == 1024L && ni == (long)(4u * GN)
                  && np >= (long)RANS_HDR_BYTES);
        if (ok && (get32(ref_pay + 9) != IMG_H || get32(ref_pay + 13) != IMG_W)) {
            printf("  reference is %ux%u; this harness is sized for %ux%u\n",
                   (unsigned)get32(ref_pay + 13), (unsigned)get32(ref_pay + 9), (unsigned)IMG_W, (unsigned)IMG_H);
            ok = 0;
        }
        rans_tables_t RT[4];
        for (uint32_t g = 0; g < 4u && ok; g++) {
            const int rc = rans_rom_expand_group(ref_rom + (size_t)g * RANS_ROM_GROUP_BYTES, rows[g]);
            RT[g] = (rans_tables_t){ 256u, RANS_NCTX_SLOTS, RANS_K_NOMINAL, ref_slots + (size_t)g * 256u, rows[g], 0 };
            const int tc = rans_tables_check(&RT[g]);
            if (rc != 0 || tc != 0) { printf("  group %u: ROM rc=%d, contract rc=%d\n", (unsigned)g, rc, tc); ok = 0; }
            P[g] = ref_idx + (size_t)g * GN;
            TP[g] = &RT[g];
        }
        size_t rlen = 0u;
        rans_mode_t rmode = RANS_MODE_RAW;
        if (ok) {
            rans_frame_begin(&F, P, TP, GN, GW, WK, WORST);
            rlen = rans_frame_payload(&F, IMG_H, IMG_W, pay2, sizeof pay2, &rmode);
            if (rlen == 0u) { printf("  encoder refused the reference index map (err %d)\n", F.err); ok = 0; }
        }
        const int same = ok && rlen == (size_t)np && memcmp(pay2, ref_pay, rlen) == 0;
        if (ok) {
            printf("  reference %ld B mode %u; produced %u B mode %d\n", np, (unsigned)ref_pay[4],
                   (unsigned)rlen, (int)rmode);
            if (!same) {
                const size_t lim = (rlen < (size_t)np) ? rlen : (size_t)np;
                size_t off = 0u;
                while (off < lim && pay2[off] == ref_pay[off]) off++;
                printf("  FIRST DIFFERENCE at byte %u: %s\n", (unsigned)off, region(ref_pay, (size_t)np, off));
                const size_t a = (off >= 8u) ? off - 8u : 0u;
                size_t na = (size_t)np - a; if (na > 16u) na = 16u;
                size_t nb = rlen - a;       if (nb > 16u) nb = 16u;
                show("reference", ref_pay + a, na);
                show("produced",  pay2 + a,    nb);
            }
        }
        check(same, "STAGE 3 ACCEPTANCE: payload byte-identical to the reference");
    } else {
        printf("\nSTAGE 3 ACCEPTANCE NOT EVALUATED -- no reference payload supplied.\n");
        printf("  usage: rans_stage3.exe DIR, with\n");
        printf("    DIR/rom.bin      14,528 B, spec 3 layout, groups 0..3\n");
        printf("    DIR/slots.bin     1,024 B, per group: context id -> slot, 0xFF = cannot occur\n");
        printf("    DIR/idx.bin      57,600 B, original ids, group-major, raster order\n");
        printf("    DIR/payload.bin  the reference payload for that index map\n");
    }

    printf("\nRESULT: %s (%d checks failed)%s\n", fails ? "FAIL" : "PASS", fails,
           (argc >= 2) ? "" : "  -- self-consistency only, NOT stage 3 acceptance");
    return fails != 0;
}
