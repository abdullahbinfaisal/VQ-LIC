/* ============================================================================
 * rans_edge.c -- see rans_edge.h. The coding path is integer only; the one
 * double (fit_ms) is boot-time reporting.
 * ==========================================================================*/
#include "rans_edge.h"
#include <string.h>

extern unsigned long long ep_timer_now(void);
extern double             ep_cycles_to_ms(unsigned long long c);

#if VQPW_M != 4 || VQPW_K != 64 || VQPW_KW != 6
#  error "rans_edge.c implements the deployed M=4, K=64 geometry only (VQPW_PROFILE=1)"
#endif

static uint8_t       re_k2id   [RANS_G][VQPW_K];     /* hardware k -> original id */
static uint8_t       re_id2slot[RANS_G][256];        /* original id -> slot       */
static uint8_t       re_rom    [RANS_ROM_BYTES];     /* spec-3 layout, 14,528 B   */
static uint16_t      re_rows   [RANS_G][RANS_NCTX_SLOTS * RANS_ROW_ENTRIES];
static uint32_t      re_recip  [RANS_G][RANS_NCTX_SLOTS * (RANS_ROW_ENTRIES / 2u)];  /* 266,240 B */
static rans_tables_t re_T[RANS_G];
static int           re_ok = 0;
static int           re_fast = 1;                    /* divide-free encoder, same bytes */

static uint8_t       re_planes [RANS_G][RE_N];
static uint8_t       re_work   [RANS_G][RE_WORK_BYTES];
static uint8_t       re_dec    [RE_N];

static inline uint32_t re_word(const uint8_t *t, uint32_t pos)
{
    const uint8_t *p = t + (size_t)pos * 4u;
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static inline void re_put16(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
}

void re_synth_maps(void)
{
    for (uint32_t g = 0u; g < RANS_G; g++) {
        memset(re_id2slot[g], RANS_SLOT_NONE, sizeof re_id2slot[g]);
        for (uint32_t k = 0u; k < (uint32_t)VQPW_K; k++) {
            const uint8_t id = (uint8_t)((k * 97u + 29u * g + 7u) & 0xFFu);   /* 97 odd: a permutation */
            re_k2id[g][k]     = id;
            re_id2slot[g][id] = (uint8_t)k;
        }
    }
}

/* Top-16 by count, then by marginal, then lowest id. cnt == 0 selects by the
 * marginal alone, which is how an unpopulated slot holds a copy of it. */
static void re_top16(const uint32_t *cnt, const uint16_t *marg, uint8_t *sym, uint32_t *c16)
{
    uint8_t taken[256];
    memset(taken, 0, sizeof taken);
    for (uint32_t k = 0u; k < RANS_TOP_N; k++) {
        int best = -1;
        for (uint32_t s = 0u; s < 256u; s++) {
            if (taken[s]) continue;
            if (best < 0) { best = (int)s; continue; }
            const uint32_t cb = cnt ? cnt[best] : 0u;
            const uint32_t cs = cnt ? cnt[s]    : 0u;
            if (cs > cb || (cs == cb && marg[s] > marg[best])) best = (int)s;
        }
        taken[best] = 1u;
        sym[k] = (uint8_t)best;
        c16[k] = cnt ? cnt[best] : 0u;
    }
}

int re_fit_tables(const uint8_t *const *cal, int ncal, re_fit_report_t *rep)
{
    static uint32_t marg_cnt[RANS_G][256];
    static uint32_t ctx_cnt [RANS_G][RANS_NCTX_SLOTS][256];
    const unsigned long long t0 = ep_timer_now();

    memset(marg_cnt, 0, sizeof marg_cnt);
    memset(ctx_cnt,  0, sizeof ctx_cnt);
    if (rep) memset(rep, 0, sizeof *rep);
    re_ok = 0;

    /* ---- histograms: per group, per context slot, over original ids ---- */
    for (int f = 0; f < ncal; f++) {
        const uint8_t *tr = cal ? cal[f] : 0;
        if (!tr) continue;
        for (uint32_t pos = 0u; pos < RE_N; pos++) {
            const uint32_t col = pos % RE_W;
            const uint32_t w   = re_word(tr, pos);
            const uint32_t wl  = col ? re_word(tr, pos - 1u) : 0u;
            for (uint32_t g = 0u; g < RANS_G; g++) {
                const uint32_t id   = re_k2id[g][(w >> (6u * g)) & 0x3Fu];
                const uint32_t slot = col ? re_id2slot[g][re_k2id[g][(wl >> (6u * g)) & 0x3Fu]]
                                          : (RANS_NCTX_SLOTS - 1u);
                marg_cnt[g][id]++;
                ctx_cnt[g][slot][id]++;
            }
        }
    }

    int rc = 0;
    for (uint32_t g = 0u; g < RANS_G && rc == 0; g++) {
        uint8_t *G = re_rom + (size_t)g * RANS_ROM_GROUP_BYTES;
        uint32_t f[256];
        uint16_t marg[256];

        /* marginal: every id >= 1 by the spec-3.5 floor, sums to 65536 */
        if (rans_quantize_to_total(marg_cnt[g], 256u, 65536u, f) != 0) { rc = -1; break; }
        for (uint32_t s = 0u; s < 256u; s++) marg[s] = (uint16_t)f[s];

        for (uint32_t c = 0u; c < RANS_NCTX_SLOTS; c++) {
            const uint32_t *cc = ctx_cnt[g][c];
            uint64_t tot = 0u;
            for (uint32_t s = 0u; s < 256u; s++) tot += cc[s];

            uint8_t  sym[RANS_TOP_N];
            uint32_t c16[RANS_TOP_N], q16[RANS_TOP_N];
            uint16_t fq[RANS_TOP_N];

            if (tot == 0u) {
                re_top16(0, marg, sym, c16);                     /* spec 3.1 */
                for (uint32_t k = 0u; k < RANS_TOP_N; k++) fq[k] = marg[sym[k]];
            } else {
                if (rep) { rep->populated[g]++; rep->ctx_tokens[g] += (uint32_t)tot; }
                re_top16(cc, marg, sym, c16);
                uint64_t n16 = 0u;
                for (uint32_t k = 0u; k < RANS_TOP_N; k++) n16 += c16[k];
                /* stored mass follows the empirical share; the escape keeps at
                 * least 512 so every one of the 240 tail ids can hold f >= 1 */
                uint64_t M16 = (65536u * n16) / (tot + 256u);
                if (M16 > 65536u - 512u) M16 = 65536u - 512u;
                if (M16 < RANS_TOP_N)    M16 = RANS_TOP_N;
                if (rans_quantize_to_total(c16, RANS_TOP_N, (uint32_t)M16, q16) != 0) { rc = -2; break; }
                for (uint32_t k = 0u; k < RANS_TOP_N; k++) fq[k] = (uint16_t)q16[k];
            }
            for (uint32_t k = 1u; k < RANS_TOP_N; k++)            /* spec 3.2 order */
                for (uint32_t j = k; j > 0u && fq[j - 1u] < fq[j]; j--) {
                    const uint16_t tf = fq[j];  fq[j]  = fq[j - 1u];  fq[j - 1u]  = tf;
                    const uint8_t  ts = sym[j]; sym[j] = sym[j - 1u]; sym[j - 1u] = ts;
                }
            uint8_t *tb = G + (size_t)c * RANS_CTX_TABLE_BYTES;
            memcpy(tb, sym, RANS_TOP_N);
            for (uint32_t k = 0u; k < RANS_TOP_N; k++) re_put16(tb + RANS_TOP_N + 2u * k, fq[k]);
        }
        if (rc != 0) break;
        for (uint32_t s = 0u; s < 256u; s++)
            re_put16(G + (size_t)RANS_NCTX_SLOTS * RANS_CTX_TABLE_BYTES + 2u * s, marg[s]);

        const int e = rans_rom_expand_group(G, re_rows[g]);
        re_T[g] = (rans_tables_t){ 256u, RANS_NCTX_SLOTS, RANS_K_NOMINAL, re_id2slot[g], re_rows[g], 0 };
        if (e != 0 || rans_tables_check(&re_T[g]) != 0) rc = -3;
        else {
            rans_recip_fill(&re_T[g], re_recip[g]);          /* boot-time, from the expanded rows */
            if (re_fast) re_T[g].recip = re_recip[g];
        }
    }

    re_ok = (rc == 0);
    if (rep) { rep->rc = rc; rep->fit_ms = ep_cycles_to_ms(ep_timer_now() - t0); }
    return rc;
}

void re_set_fast_divide(int on)
{
    re_fast = on ? 1 : 0;
    if (re_ok)
        for (uint32_t g = 0u; g < RANS_G; g++) re_T[g].recip = re_fast ? re_recip[g] : 0;
}

/* ---- the resumable job ----------------------------------------------------- */
static void re_unpack(const uint8_t *src, uint32_t p0, uint32_t p1)
{
    for (uint32_t pos = p0; pos < p1; pos++) {
        const uint32_t w = re_word(src, pos);
        re_planes[0][pos] = re_k2id[0][ w        & 0x3Fu];
        re_planes[1][pos] = re_k2id[1][(w >>  6) & 0x3Fu];
        re_planes[2][pos] = re_k2id[2][(w >> 12) & 0x3Fu];
        re_planes[3][pos] = re_k2id[3][(w >> 18) & 0x3Fu];
    }
}

void re_job_start(re_job_t *J, const uint8_t *transport, uint8_t *payload, size_t cap)
{
    memset(J, 0, sizeof *J);
    J->src = transport; J->payload = payload; J->cap = cap;
    J->phase = 1;
    J->mode = RANS_MODE_RAW;
    if (!re_ok || !transport || !payload || cap < RE_PAYLOAD_CAP) { J->err = 1; J->phase = 3; }
}

int re_job_step(re_job_t *J, uint32_t budget)
{
    if (J->err || J->phase >= 3) return 1;
    const int all = (budget == 0u);

    if (J->phase == 1) {
        const uint32_t npos = all ? RE_N : (budget + 3u) / 4u;
        uint32_t end = J->upos + npos;
        if (end > RE_N) end = RE_N;
        re_unpack(J->src, J->upos, end);
        const uint32_t used = (end - J->upos) * 4u;
        J->upos = end;
        if (end < RE_N) return 0;

        const uint8_t       *P[RANS_G];
        const rans_tables_t *TP[RANS_G];
        uint8_t             *WK[RANS_G];
        for (uint32_t g = 0u; g < RANS_G; g++) { P[g] = re_planes[g]; TP[g] = &re_T[g]; WK[g] = re_work[g]; }
        rans_frame_begin(&J->F, P, TP, RE_N, RE_W, WK, RE_WORK_BYTES);
        if (J->F.err) { J->err = 2; J->phase = 3; return 1; }
        J->phase = 2;
        if (!all) {
            if (used >= budget) return 0;
            budget -= used;
        }
    }

    if (rans_frame_step(&J->F, all ? 0u : budget)) {
        if (J->F.err) J->err = 3;
        J->phase = 3;
        return 1;
    }
    return 0;
}

size_t re_job_finish(re_job_t *J, rans_mode_t *mode)
{
    if (J->phase != 4) {
        (void)re_job_step(J, 0u);
        J->len = 0u;
        if (!J->err)
            J->len = rans_frame_payload(&J->F, RE_IMG_H, RE_IMG_W, J->payload, J->cap, &J->mode);
        if (J->len == 0u && !J->err) J->err = 4;
        J->phase = 4;
    }
    if (mode) *mode = J->mode;
    return J->len;
}

uint32_t re_job_unpacked(const re_job_t *J) { return J->upos; }

uint32_t re_job_coded(const re_job_t *J)
{
    return (J->phase >= 2 && !J->err) ? rans_frame_coded(&J->F) : 0u;
}

long re_job_verify(const re_job_t *J, const uint8_t *transport, long *first_bad)
{
    if (first_bad) *first_bad = -1;
    if (J->phase != 4 || J->err || J->len < RANS_HDR_BYTES || !transport) return -1;

    const uint8_t *p = J->payload;
    long bad = 0;

    if (p[4] == (uint8_t)RANS_MODE_RAW) {
        for (uint32_t g = 0u; g < RANS_G; g++)
            for (uint32_t pos = 0u; pos < RE_N; pos++) {
                const uint32_t id = p[RANS_HDR_BYTES + (size_t)g * RE_N + pos];
                const uint32_t k  = (re_word(transport, pos) >> (6u * g)) & 0x3Fu;
                if (re_id2slot[g][id] != k) {
                    if (first_bad && *first_bad < 0) *first_bad = (long)pos * 4L + (long)g;
                    bad++;
                }
            }
        return bad;
    }

    size_t o = RANS_HDR_BYTES;
    for (uint32_t g = 0u; g < RANS_G; g++) {
        if (o + 4u > J->len) return -2;
        const uint32_t L = (uint32_t)p[o] | ((uint32_t)p[o + 1] << 8)
                         | ((uint32_t)p[o + 2] << 16) | ((uint32_t)p[o + 3] << 24);
        o += 4u;
        if (o + L > J->len) return -2;
        if (rans_decode_ctx(&re_T[g], RE_W, p + o, L, re_dec, RE_N) != 0) {
            if (first_bad && *first_bad < 0) *first_bad = (long)g;
            bad += (long)RE_N;
        } else {
            for (uint32_t pos = 0u; pos < RE_N; pos++) {
                const uint32_t k = (re_word(transport, pos) >> (6u * g)) & 0x3Fu;
                if (re_id2slot[g][re_dec[pos]] != k) {
                    if (first_bad && *first_bad < 0) *first_bad = (long)pos * 4L + (long)g;
                    bad++;
                }
            }
        }
        o += L;
    }
    return (o == J->len) ? bad : -3;
}
