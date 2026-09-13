/* ============================================================================
 * rans.c -- see rans.h. Integer only: no float or double anywhere.
 *
 * RANS_DEBUG (define it for test builds) asserts the state invariant of
 * guide 3 after every update:  RANS_L <= x < RANS_L << 8.
 * ==========================================================================*/
#include "rans.h"
#include <string.h>

static inline const uint16_t *rans_row(const rans_tables_t *T, uint32_t c)
{
    return T->rows + (size_t)c * 2u * (size_t)T->nsym;
}

/* Spec 6.1 / guide 7: binary search, never reads cdf[nsym]. */
static inline uint32_t rans_sym_from_cdf(const uint16_t *cdf, uint32_t nsym, uint32_t slot)
{
    uint32_t lo = 0u, hi = nsym;
    while (lo + 1u < hi) {
        const uint32_t mid = (lo + hi) >> 1;
        if ((uint32_t)cdf[mid] <= slot) lo = mid; else hi = mid;
    }
    return lo;
}

static inline uint32_t rans_read_be32(const uint8_t *d)
{
    return ((uint32_t)d[0] << 24) | ((uint32_t)d[1] << 16)
         | ((uint32_t)d[2] <<  8) |  (uint32_t)d[3];
}

static inline uint16_t rans_get_le16(const uint8_t *p)
{
    return (uint16_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8));
}

static inline void rans_put_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v         & 0xFFu);
    p[1] = (uint8_t)((v >>  8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

/* ---- tables --------------------------------------------------------------- */
int rans_tables_check(const rans_tables_t *T)
{
    if (!T || !T->rows || T->nsym == 0u || T->nsym > 256u || T->nctx < 2u
        || T->k_border > 256u || T->nsym > T->k_border)
        return -1;
    if (T->slot_of_id) {
        for (uint32_t id = 0u; id < T->k_border; id++) {
            const uint32_t v = T->slot_of_id[id];
            if (v != RANS_SLOT_NONE && v >= T->nctx - 1u) return -1;
        }
    } else if (T->k_border > T->nctx - 1u) {
        return -1;                                /* identity needs a slot per id */
    }
    for (uint32_t c = 0u; c < T->nctx; c++) {
        const uint16_t *f   = rans_row(T, c);
        const uint16_t *cdf = f + T->nsym;
        uint32_t acc = 0u;
        for (uint32_t s = 0u; s < T->nsym; s++) {
            if (f[s] == 0u || (uint32_t)cdf[s] != acc) return (int)(c + 1u);
            acc += f[s];
        }
        if (acc != (1u << RANS_PROB_BITS)) return (int)(c + 1u);
    }
    return 0;
}

/* Spec 3.5, step numbers in the comments. */
int rans_quantize_to_total(const uint32_t *w_in, uint32_t n, uint32_t M, uint32_t *f)
{
    if (!w_in || !f || n == 0u || n > 256u) return -1;

    uint32_t w[256];
    uint64_t sw = 0u;
    for (uint32_t i = 0u; i < n; i++) { w[i] = w_in[i]; sw += w[i]; }
    if (sw == 0u) {                                           /* 1 */
        for (uint32_t i = 0u; i < n; i++) w[i] = 1u;
        sw = n;
    }

    int64_t sf = 0;
    for (uint32_t i = 0u; i < n; i++) {
        uint64_t v = ((uint64_t)w[i] * (uint64_t)M) / sw;     /* 2, 64-bit */
        if (v < 1u) v = 1u;                                   /* 3 */
        f[i] = (uint32_t)v;
        sf += (int64_t)v;
    }

    /* 4: order = descending w, ties by ascending index. An insertion that only
     * moves past STRICTLY smaller weights is stable, so equal weights keep
     * ascending index without depending on a library sort. */
    uint16_t order[256];
    for (uint32_t i = 0u; i < n; i++) {
        uint32_t j = i;
        while (j > 0u && w[order[j - 1u]] < w[i]) { order[j] = order[j - 1u]; j--; }
        order[j] = (uint16_t)i;
    }

    int64_t d = (int64_t)M - sf;
    if (d > 0) {
        for (int64_t j = 0; j < d; j++) f[order[(uint32_t)(j % (int64_t)n)]] += 1u;
    } else if (d < 0) {
        /* walk order cyclically, one unit off each visited f > 1 */
        uint32_t j = 0u, idle = 0u;
        while (d < 0) {
            const uint32_t i = order[j];
            j = (j + 1u == n) ? 0u : j + 1u;
            if (f[i] > 1u) { f[i] -= 1u; d++; idle = 0u; }
            else if (++idle >= n) return -1;                  /* nothing above 1 */
        }
    }

    uint64_t chk = 0u;                                        /* 5 */
    for (uint32_t i = 0u; i < n; i++) chk += f[i];
    return (chk == (uint64_t)M) ? 0 : -1;
}

/* Spec 3.4, step numbers in the comments. */
int rans_reconstruct_row(const uint8_t *sym, const uint16_t *freq,
                         const uint16_t *marginal, uint16_t *row)
{
    if (!sym || !freq || !marginal || !row) return -1;

    uint32_t msum = 0u;                                       /* spec 3.3 */
    for (uint32_t s = 0u; s < 256u; s++) {
        if (marginal[s] == 0u) return -6;
        msum += marginal[s];
    }
    if (msum != 65536u) return -6;

    uint32_t f[256];
    uint8_t  stored[256];
    memset(f, 0, sizeof f);                                   /* 1 */
    memset(stored, 0, sizeof stored);
    uint32_t sum16 = 0u;
    for (uint32_t i = 0u; i < RANS_TOP_N; i++) {              /* 2 */
        const uint32_t s = sym[i];
        if (stored[s]) return -2;
        if (freq[i] == 0u) return -3;
        stored[s] = 1u;
        f[s] = freq[i];
        sum16 += freq[i];
    }
    if (sum16 >= 65536u) return -4;
    const uint32_t escape = 65536u - sum16;                   /* 3 */

    uint32_t tsym[256], tw[256], tf[256], nt = 0u;            /* 4: T, ascending id */
    for (uint32_t s = 0u; s < 256u; s++)
        if (!stored[s]) { tsym[nt] = s; tw[nt] = marginal[s]; nt++; }
    if (rans_quantize_to_total(tw, nt, escape, tf) != 0) return -5;
    for (uint32_t j = 0u; j < nt; j++) f[tsym[j]] = tf[j];

    uint32_t acc = 0u;                                        /* 5 */
    for (uint32_t s = 0u; s < 256u; s++) {
        if (f[s] == 0u || f[s] > 0xFFFFu) return -7;
        row[s]        = (uint16_t)f[s];
        row[256u + s] = (uint16_t)acc;
        acc += f[s];
    }
    return (acc == 65536u) ? 0 : -7;
}

int rans_rom_expand_group(const uint8_t *rom, uint16_t *rows)
{
    if (!rom || !rows) return -1;
    uint16_t marg[256];
    const uint8_t *mp = rom + (size_t)RANS_NCTX_SLOTS * RANS_CTX_TABLE_BYTES;
    for (uint32_t s = 0u; s < 256u; s++) marg[s] = rans_get_le16(mp + 2u * s);

    for (uint32_t c = 0u; c < RANS_NCTX_SLOTS; c++) {
        const uint8_t *tb = rom + (size_t)c * RANS_CTX_TABLE_BYTES;
        uint16_t freq[RANS_TOP_N];
        for (uint32_t i = 0u; i < RANS_TOP_N; i++)
            freq[i] = rans_get_le16(tb + RANS_TOP_N + 2u * i);
        if (rans_reconstruct_row(tb, freq, marg, rows + (size_t)c * RANS_ROW_ENTRIES) != 0)
            return (int)(c + 1u);
    }
    return 0;
}

/* ---- static coder --------------------------------------------------------- */
size_t rans_encode_static(const uint16_t *f, const uint16_t *cdf, uint32_t nsym,
                          const uint8_t *sym, uint32_t n,
                          uint8_t *buf, size_t cap, const uint8_t **stream)
{
    if (stream) *stream = 0;
    if (!f || !cdf || !sym || !buf || n > 0x7FFFFFFFu || cap < RANS_WORST_BYTES(n))
        return 0u;

    uint32_t x = RANS_L;                              /* NOT zero, 9.3 */
    size_t   p = cap;

    for (int32_t t = (int32_t)n - 1; t >= 0; --t) {   /* BACKWARD */
        const uint32_t s = sym[t];
        if (s >= nsym) return 0u;
        const uint32_t fs = f[s];
        if (fs == 0u) return 0u;                      /* 9.6 */
        const uint32_t x_max = RANS_X_MAX_BASE * fs;  /* fs of THIS symbol */

        while (x >= x_max) {                          /* renorm BEFORE update */
            buf[--p] = (uint8_t)(x & 0xFFu);
            x >>= 8;
        }
        const uint32_t q = x / fs;                    /* one divide */
        const uint32_t r = x - q * fs;
        x = (q << RANS_PROB_BITS) + r + cdf[s];
#ifdef RANS_DEBUG
        if (x < RANS_L || x >= (RANS_L << 8)) return 0u;
#endif
    }
    for (int i = 0; i < 4; ++i) {                     /* flush, LSB first */
        buf[--p] = (uint8_t)(x & 0xFFu);
        x >>= 8;
    }
    if (stream) *stream = buf + p;
    return cap - p;
}

int rans_decode_static(const uint16_t *f, const uint16_t *cdf, uint32_t nsym,
                       const uint8_t *d, size_t len, uint8_t *out, uint32_t n)
{
    if (!f || !cdf || !d || !out || len < 4u) return -1;
    uint32_t x = rans_read_be32(d);
    size_t   p = 4u;

    for (uint32_t t = 0u; t < n; ++t) {
        const uint32_t slot = x & RANS_PROB_MASK;
        const uint32_t s    = rans_sym_from_cdf(cdf, nsym, slot);
        out[t] = (uint8_t)s;
        /* update BEFORE renorm */
        x = (uint32_t)f[s] * (x >> RANS_PROB_BITS) + slot - (uint32_t)cdf[s];
        while (x < RANS_L) {
            if (p >= len) return -1;                  /* 9.13 */
            x = (x << 8) | (uint32_t)d[p++];
        }
    }
    return (p == len) ? 0 : -2;
}

/* ---- context coder, resumable ------------------------------------------------ */
void rans_enc_begin(rans_enc_t *e, const uint8_t *plane, uint32_t n, uint32_t w,
                    const rans_tables_t *T, uint8_t *buf, size_t cap)
{
    memset(e, 0, sizeof *e);
    e->plane = plane; e->n = n; e->w = w; e->T = T; e->buf = buf; e->cap = cap;
    e->x = RANS_L;                                    /* per group, 9.12 */
    e->t = (int32_t)n - 1;
    e->p = cap;
    if (!plane || !T || !T->rows || !buf || n == 0u || w == 0u || (n % w) != 0u
        || n > 0x7FFFFFFFu || cap < RANS_WORST_BYTES(n) || T->nctx < 2u
        || T->nsym == 0u || T->nsym > 256u || T->k_border > 256u)
        e->err = 1;
}

void rans_recip_fill(const rans_tables_t *T, uint32_t *recip)
{
    const uint32_t W = T->nsym;
    for (uint32_t c = 0u; c < T->nctx; c++) {
        const uint16_t *row = rans_row(T, c);
        for (uint32_t s = 0u; s < W; s++) {
            const uint32_t fs = row[s];
            uint32_t m = 0u;                          /* f = 0 is never coded, f = 1 needs none */
            if (fs >= 2u) {
                const uint32_t sh = 32u - (uint32_t)__builtin_clz(fs - 1u);
                m = (uint32_t)(((1ull << (31u + sh)) + fs - 1u) / fs);
            }
            recip[(size_t)c * W + s] = m;
        }
    }
}

/* The same coder with no divide in the loop. The Zynq-7000 Cortex-A9 has no
 * hardware divide, so x / fs and the column t % w were two __aeabi_uidivmod
 * calls per token. Here the quotient is
 *
 *     q = mulhi(x, m) >> (sh - 1),   m = ceil(2^(31+sh) / fs),  sh = ceil(log2 fs)
 *
 * which equals x / fs for every x < 2^31, because m*fs - 2^(31+sh) < fs <= 2^sh
 * (Granlund and Montgomery 1994, thm 4.2); after renormalisation x < 32768*fs
 * < 2^31. With r = x - q*fs the update (q << 16) + r + cdf is written as
 * x + cdf + q*(65536 - fs). f = 1 gives q = x, r = 0. The column is carried
 * and counted down instead of recomputed, and the descriptor is copied to
 * locals because every byte written through buf may alias it.
 * test/rans_recip_test.c checks the bound for every fs, every reachable
 * quotient at its worst remainder, and whole streams against the divide path. */
static int rans_enc_step_recip(rans_enc_t *e, uint32_t budget)
{
    const rans_tables_t  T     = *e->T;
    const uint8_t *const plane = e->plane;
    uint8_t *const       buf   = e->buf;
    const uint32_t       W     = T.nsym;
    const uint32_t       w     = e->w;
    uint32_t x = e->x;
    int32_t  t = e->t;
    size_t   p = e->p;

    int32_t stop = -1;                                /* slice ends on a symbol boundary */
    if (budget != 0u && (int64_t)t - (int64_t)budget > -1) stop = t - (int32_t)budget;
    uint32_t col = (t >= 0) ? (uint32_t)t % w : 0u;   /* one divide per slice, not per token */

    while (t > stop) {
        const uint32_t c = rans_ctx_slot(&T, col ? (uint32_t)plane[t - 1] : T.k_border);
        const uint32_t s = plane[t];
        if (s >= W || c >= T.nctx) { e->err = 2; goto out; }

        const uint16_t *row = rans_row(&T, c);
        const uint32_t  fs  = row[s];
        if (fs == 0u) { e->err = 3; goto out; }
        const uint32_t x_max = RANS_X_MAX_BASE * fs;

        while (x >= x_max) {                          /* renorm BEFORE update */
            buf[--p] = (uint8_t)(x & 0xFFu);
            x >>= 8;
        }
        const uint32_t cs = row[W + s];
        if (fs >= 2u) {
            const uint32_t sh = 32u - (uint32_t)__builtin_clz(fs - 1u);
            const uint32_t q  = (uint32_t)(((uint64_t)x * T.recip[(size_t)c * W + s]) >> 32) >> (sh - 1u);
            x += cs + q * ((1u << RANS_PROB_BITS) - fs);
        } else {
            x = (x << RANS_PROB_BITS) + cs;
        }
#ifdef RANS_DEBUG
        if (x < RANS_L || x >= (RANS_L << 8)) { e->err = 9; goto out; }
#endif
        --t;
        col = col ? col - 1u : w - 1u;
    }
    if (t < 0) {
        for (int i = 0; i < 4; ++i) {                 /* flush, LSB first */
            buf[--p] = (uint8_t)(x & 0xFFu);
            x >>= 8;
        }
        e->done = 1;
    }
out:
    e->x = x; e->t = t; e->p = p;
    return (e->done || e->err) ? 1 : 0;
}

int rans_enc_step(rans_enc_t *e, uint32_t budget)
{
    if (e->err || e->done) return 1;
    if (e->T->recip) return rans_enc_step_recip(e, budget);

    const rans_tables_t *T   = e->T;
    const uint32_t       W   = T->nsym;
    const int            all = (budget == 0u);
    uint32_t x = e->x;                                /* the only carried state */
    int32_t  t = e->t;
    size_t   p = e->p;

    while (t >= 0) {
        if (!all) {
            if (budget == 0u) goto out;               /* slice ends on a symbol boundary */
            budget--;
        }
        const uint32_t c = rans_ctx_slot(T, rans_context_id(e->plane, (uint32_t)t, e->w,
                                                            T->k_border));
        const uint32_t s = e->plane[t];
        if (s >= W || c >= T->nctx) { e->err = 2; goto out; }

        const uint16_t *row = rans_row(T, c);         /* re-fetched every token, 9.15 */
        const uint32_t fs   = row[s];
        if (fs == 0u) { e->err = 3; goto out; }       /* 9.6 */
        const uint32_t x_max = RANS_X_MAX_BASE * fs;

        while (x >= x_max) {                          /* renorm BEFORE update */
            e->buf[--p] = (uint8_t)(x & 0xFFu);
            x >>= 8;
        }
        const uint32_t q = x / fs;                    /* one divide */
        const uint32_t r = x - q * fs;
        x = (q << RANS_PROB_BITS) + r + (uint32_t)row[W + s];
#ifdef RANS_DEBUG
        if (x < RANS_L || x >= (RANS_L << 8)) { e->err = 9; goto out; }
#endif
        --t;
    }
    for (int i = 0; i < 4; ++i) {                     /* flush, LSB first */
        e->buf[--p] = (uint8_t)(x & 0xFFu);
        x >>= 8;
    }
    e->done = 1;
out:
    e->x = x; e->t = t; e->p = p;
    return (e->done || e->err) ? 1 : 0;
}

size_t rans_enc_finish(rans_enc_t *e, const uint8_t **stream)
{
    (void)rans_enc_step(e, 0u);
    if (e->err || !e->done) { if (stream) *stream = 0; return 0u; }
    if (stream) *stream = e->buf + e->p;
    return e->cap - e->p;
}

uint32_t rans_enc_coded(const rans_enc_t *e)
{
    return (uint32_t)((int32_t)e->n - 1 - e->t);
}

int rans_decode_ctx(const rans_tables_t *T, uint32_t w, const uint8_t *d, size_t len,
                    uint8_t *out, uint32_t n)
{
    if (!T || !T->rows || !d || !out || w == 0u || len < 4u) return -1;
    const uint32_t W = T->nsym;
    uint32_t x = rans_read_be32(d);
    size_t   p = 4u;

    for (uint32_t t = 0u; t < n; ++t) {
        const uint32_t c = rans_ctx_slot(T, rans_context_id(out, t, w, T->k_border));
        if (c >= T->nctx) return -3;
        const uint16_t *row  = rans_row(T, c);
        const uint32_t  slot = x & RANS_PROB_MASK;
        const uint32_t  s    = rans_sym_from_cdf(row + W, W, slot);
        out[t] = (uint8_t)s;
        x = (uint32_t)row[s] * (x >> RANS_PROB_BITS) + slot - (uint32_t)row[W + s];
        while (x < RANS_L) {
            if (p >= len) return -1;
            x = (x << 8) | (uint32_t)d[p++];
        }
    }
    return (p == len) ? 0 : -2;
}

/* ---- body, fallback, container ---------------------------------------------- */
rans_mode_t rans_choose_mode(size_t coded_body_len, uint32_t n, uint32_t G)
{
    return (coded_body_len >= (size_t)n * (size_t)G) ? RANS_MODE_RAW : RANS_MODE_CONTEXT;
}

void rans_frame_begin(rans_frame_t *F, const uint8_t *const planes[RANS_G],
                      const rans_tables_t *const T[RANS_G], uint32_t n, uint32_t w,
                      uint8_t *const work[RANS_G], size_t work_cap)
{
    memset(F, 0, sizeof *F);
    F->n = n; F->w = w; F->work_cap = work_cap;
    for (uint32_t g = 0u; g < RANS_G; g++) {
        F->planes[g] = planes[g];
        F->T[g]      = T[g];
        F->work[g]   = work[g];
        if (!planes[g] || !T[g] || !work[g]) F->err = 1;
    }
    if (!F->err) {
        rans_enc_begin(&F->enc, F->planes[0], n, w, F->T[0], F->work[0], work_cap);
        if (F->enc.err) F->err = 1;
    }
}

int rans_frame_step(rans_frame_t *F, uint32_t budget)
{
    const int all = (budget == 0u);
    while (!F->err && F->g < RANS_G) {
        if (!all && budget == 0u) break;
        const uint32_t before = rans_enc_coded(&F->enc);
        const int fin = rans_enc_step(&F->enc, all ? 0u : budget);
        if (F->enc.err) { F->err = 2; break; }
        if (!all) {
            const uint32_t did = rans_enc_coded(&F->enc) - before;
            budget = (did >= budget) ? 0u : budget - did;
        }
        if (!fin) break;                              /* slice ended inside this group */
        F->stream[F->g] = F->enc.buf + F->enc.p;
        F->len[F->g]    = F->enc.cap - F->enc.p;
        F->g++;
        if (F->g < RANS_G) {                          /* fresh state for the next group */
            rans_enc_begin(&F->enc, F->planes[F->g], F->n, F->w, F->T[F->g],
                           F->work[F->g], F->work_cap);
            if (F->enc.err) { F->err = 1; break; }
        }
    }
    return (F->err || F->g >= RANS_G) ? 1 : 0;
}

uint32_t rans_frame_coded(const rans_frame_t *F)
{
    if (F->g >= RANS_G) return F->n * RANS_G;
    return F->g * F->n + rans_enc_coded(&F->enc);
}

size_t rans_frame_finish(rans_frame_t *F, uint8_t *out, size_t cap, rans_mode_t *mode)
{
    (void)rans_frame_step(F, 0u);
    if (F->err || F->g < RANS_G || !out || !mode) return 0u;

    size_t coded = 0u;
    for (uint32_t g = 0u; g < RANS_G; g++) coded += 4u + F->len[g];

    const rans_mode_t m = rans_choose_mode(coded, F->n, RANS_G);
    if (m == RANS_MODE_RAW) {
        const size_t raw = (size_t)F->n * RANS_G;
        if (cap < raw) return 0u;
        for (uint32_t g = 0u; g < RANS_G; g++)
            memcpy(out + (size_t)g * F->n, F->planes[g], F->n);
        *mode = m;
        return raw;
    }
    if (cap < coded) return 0u;
    size_t o = 0u;
    for (uint32_t g = 0u; g < RANS_G; g++) {
        rans_put_le32(out + o, (uint32_t)F->len[g]);   /* little-endian length */
        o += 4u;
        memcpy(out + o, F->stream[g], F->len[g]);
        o += F->len[g];
    }
    *mode = m;
    return o;
}

size_t rans_write_header(uint8_t *out, size_t cap, rans_mode_t mode,
                         uint32_t K, uint32_t H, uint32_t W)
{
    if (!out || cap < RANS_HDR_BYTES || K > 0xFFFFu) return 0u;
    out[0] = (uint8_t)'N';
    out[1] = (uint8_t)'I';
    out[2] = (uint8_t)'C';
    out[3] = (uint8_t)'1';
    out[4] = (uint8_t)mode;
    out[5] = (uint8_t)RANS_G;
    out[6] = (uint8_t)(K & 0xFFu);
    out[7] = (uint8_t)((K >> 8) & 0xFFu);
    out[8] = (uint8_t)RANS_PROB_BITS;
    rans_put_le32(out + 9,  H);
    rans_put_le32(out + 13, W);
    return RANS_HDR_BYTES;
}

size_t rans_frame_payload(rans_frame_t *F, uint32_t H, uint32_t W,
                          uint8_t *out, size_t cap, rans_mode_t *mode)
{
    if (!F || !out || !mode || cap < RANS_HDR_BYTES || H == 0u || W == 0u || !F->T[0])
        return 0u;
    const uint64_t h = ((uint64_t)H + 7u) / 8u;
    const uint64_t w = ((uint64_t)W + 7u) / 8u;
    if (w != (uint64_t)F->w || h * w != (uint64_t)F->n) return 0u;   /* spec 7 */

    const size_t body = rans_frame_finish(F, out + RANS_HDR_BYTES, cap - RANS_HDR_BYTES, mode);
    if (body == 0u) return 0u;
    if (rans_write_header(out, cap, *mode, F->T[0]->k_border, H, W) != RANS_HDR_BYTES)
        return 0u;
    return RANS_HDR_BYTES + body;
}
