// ============================================================================
// range_coder.c -- see range_coder.h for the model/coder contract.
// Integer only. No floating point in rc_enc_sym / rc_dec_sym.
// ============================================================================
#include "range_coder.h"
#include <string.h>


// ---------------------------------------------------------------------------
// Model construction.  NOT timed as part of T_RANGE.
//
// Rules, stated explicitly because the paper must not claim a rate that a
// decoder could not reproduce:
//   1. histogram over the calibration frames, per codebook
//   2. add-one smoothing so every symbol has non-zero probability
//   3. scale to RC_TOT, floor at 1
//   4. fix the rounding residue on the most frequent symbol so the total is
//      EXACTLY RC_TOT (the coder's correctness depends on that identity)
// ---------------------------------------------------------------------------
void rc_model_build(rc_models_t *M, const uint8_t *const *idx_frames, int nframes)
{
    static uint32_t hist[RC_NMODEL][RC_NSYM];
    memset(hist, 0, sizeof(hist));

    for (int f = 0; f < nframes; f++) {
        const uint8_t *p = idx_frames[f];
        for (int pos = 0; pos < RC_NPOS; pos++)
            for (int m = 0; m < RC_NMODEL; m++)
                hist[m][rc_get_sym(p, pos, m)]++;
    }

    for (int m = 0; m < RC_NMODEL; m++) {
        uint64_t tot = 0;
        for (int s = 0; s < RC_NSYM; s++) { hist[m][s] += 1u; tot += hist[m][s]; }

        uint32_t acc = 0;
        uint32_t f16[RC_NSYM];
        int      argmax = 0;
        uint32_t maxv = 0;
        for (int s = 0; s < RC_NSYM; s++) {
            uint64_t v = ((uint64_t)hist[m][s] * RC_TOT) / tot;
            if (v == 0) v = 1;
            f16[s] = (uint32_t)v;
            acc += f16[s];
            if (hist[m][s] > maxv) { maxv = hist[m][s]; argmax = s; }
        }
        // force the total to be exactly RC_TOT
        if (acc != RC_TOT) {
            long long d = (long long)RC_TOT - (long long)acc;
            long long nv = (long long)f16[argmax] + d;
            if (nv < 1) {                       // pathological; spread instead
                for (int s = 0; s < RC_NSYM && d != 0; s++) {
                    if (d > 0)      { f16[s]++; d--; }
                    else if (f16[s] > 1) { f16[s]--; d++; }
                }
            } else {
                f16[argmax] = (uint32_t)nv;
            }
        }
        M->m[m].cum[0] = 0;
        for (int s = 0; s < RC_NSYM; s++)
            M->m[m].cum[s + 1] = (uint16_t)(M->m[m].cum[s] + f16[s]);
        // cum[RC_NSYM] wraps to 0 in uint16 when total == 65536; that is fine
        // because every comparison below uses cum[s] and cum[s+1] with s<255,
        // and the s==255 upper bound is handled as RC_TOT explicitly.
    }
    M->frozen = 1;
}

void rc_model_uniform(rc_models_t *M)
{
    const uint32_t f = RC_TOT / RC_NSYM;         // 256 exactly
    for (int m = 0; m < RC_NMODEL; m++) {
        M->m[m].cum[0] = 0;
        for (int s = 0; s < RC_NSYM; s++)
            M->m[m].cum[s + 1] = (uint16_t)(M->m[m].cum[s] + f);
    }
    M->frozen = 1;
}


// ---------------------------------------------------------------------------
// Local log2 -- this project deliberately links WITHOUT libm (see pm_sqrtf in
// main.c), so <math.h> log() is unavailable. Used only for entropy reporting,
// outside every timed region, so clarity beats speed.
//   log2(x) = e + (2/ln2) * atanh(z),  z = (m-1)/(m+1),  x = m*2^e, 1<=m<2
// ---------------------------------------------------------------------------
static double rc_log2(double x)
{
    if (x <= 0.0) return 0.0;
    int e = 0;
    while (x >= 2.0) { x *= 0.5; e++; }
    while (x <  1.0) { x *= 2.0; e--; }
    const double z = (x - 1.0) / (x + 1.0);
    const double z2 = z * z;
    double term = z, sum = 0.0;
    for (int k = 1; k <= 31; k += 2) { sum += term / (double)k; term *= z2; }
    return (double)e + sum * 2.8853900817779268;   /* 2/ln2 */
}
static inline uint32_t mdl_cum(const rc_model_t *m, int s)
{
    return (s >= RC_NSYM) ? RC_TOT : (uint32_t)m->cum[s];
}
static inline uint32_t mdl_freq(const rc_model_t *m, int s)
{
    return mdl_cum(m, s + 1) - mdl_cum(m, s);
}

void rc_model_entropy(const rc_models_t *M, double bits_per_sym[RC_NMODEL])
{
    for (int m = 0; m < RC_NMODEL; m++) {
        double H = 0.0;
        for (int s = 0; s < RC_NSYM; s++) {
            double p = (double)mdl_freq(&M->m[m], s) / (double)RC_TOT;
            if (p > 0.0) H -= p * rc_log2(p);
        }
        bits_per_sym[m] = H;
    }
}

// ---------------------------------------------------------------------------
// Subbotin carryless range coder.
// ---------------------------------------------------------------------------
void rc_enc_init(rc_enc_t *e, uint8_t *out, size_t cap)
{
    e->low = 0; e->range = 0xFFFFFFFFu;
    e->out = out; e->cap = cap; e->n = 0; e->overflow = 0;
}

static inline void rc_put(rc_enc_t *e, uint8_t b)
{
    if (e->n < e->cap) e->out[e->n++] = b;
    else               e->overflow = 1;
}

void rc_enc_sym(rc_enc_t *e, const rc_model_t *m, uint8_t sym)
{
    const uint32_t cum = mdl_cum(m, sym);
    const uint32_t frq = mdl_cum(m, sym + 1) - cum;

    e->range >>= RC_TOT_BITS;            // tot is 2^16 -> shift, no divide
    e->low   += cum * e->range;
    e->range *= frq;

    for (;;) {
        if ((e->low ^ (e->low + e->range)) < RC_TOP) {
            /* top byte settled */
        } else if (e->range < RC_BOT) {
            e->range = (~e->low + 1u) & (RC_BOT - 1u);   // -low & (BOT-1)
        } else break;
        rc_put(e, (uint8_t)(e->low >> 24));
        e->low   <<= 8;
        e->range <<= 8;
    }
}

size_t rc_enc_finish(rc_enc_t *e)
{
    for (int i = 0; i < 4; i++) { rc_put(e, (uint8_t)(e->low >> 24)); e->low <<= 8; }
    return e->n;
}

void rc_dec_init(rc_dec_t *d, const uint8_t *in, size_t cap)
{
    d->low = 0; d->range = 0xFFFFFFFFu; d->code = 0;
    d->in = in; d->cap = cap; d->n = 0;
    for (int i = 0; i < 4; i++)
        d->code = (d->code << 8) | (uint32_t)((d->n < d->cap) ? d->in[d->n++] : 0);
}

uint8_t rc_dec_sym(rc_dec_t *d, const rc_model_t *m)
{
    d->range >>= RC_TOT_BITS;
    uint32_t t = (d->code - d->low) / d->range;
    if (t >= RC_TOT) t = RC_TOT - 1;

    // binary search for the symbol whose [cum, cum+freq) contains t
    int lo = 0, hi = RC_NSYM - 1;
    while (lo < hi) {
        int mid = (lo + hi + 1) >> 1;
        if (mdl_cum(m, mid) <= t) lo = mid; else hi = mid - 1;
    }
    const uint8_t sym = (uint8_t)lo;
    const uint32_t cum = mdl_cum(m, lo);
    const uint32_t frq = mdl_cum(m, lo + 1) - cum;

    d->low   += cum * d->range;
    d->range *= frq;

    for (;;) {
        if ((d->low ^ (d->low + d->range)) < RC_TOP) {
            /* settled */
        } else if (d->range < RC_BOT) {
            d->range = (~d->low + 1u) & (RC_BOT - 1u);
        } else break;
        d->code  = (d->code << 8) | (uint32_t)((d->n < d->cap) ? d->in[d->n++] : 0);
        d->low   <<= 8;
        d->range <<= 8;
    }
    return sym;
}

// ---------------------------------------------------------------------------
// Whole-frame helpers.
// rc_encode_frame is EXACTLY what gets bracketed as T_RANGE: model lookup plus
// coding plus flush, nothing else. No allocation, no model construction.
// ---------------------------------------------------------------------------
void rc_stream_start(rc_stream_t *s, const rc_models_t *M,
                     const uint8_t *idx, uint8_t *out, size_t cap)
{
    rc_enc_init(&s->e, out, cap);
    s->M = M; s->idx = idx; s->pos = 0; s->m = 0; s->done = 0;
}

int rc_stream_step(rc_stream_t *s, unsigned long nsym)
{
    const int unlimited = (nsym == 0ul);
    if (s->done) return 1;
    while (s->pos < RC_NPOS) {
        while (s->m < RC_NMODEL) {
            if (!unlimited && nsym == 0ul) return 0;      /* budget spent */
            rc_enc_sym(&s->e, &s->M->m[s->m], rc_get_sym(s->idx, s->pos, s->m));
            s->m++;
            if (!unlimited) nsym--;
        }
        s->m = 0;
        s->pos++;
    }
    s->done = 1;
    return 1;
}

size_t rc_stream_finish(rc_stream_t *s)
{
    rc_stream_step(s, 0ul);                  /* anything the slices did not reach */
    const size_t n = rc_enc_finish(&s->e);
    return s->e.overflow ? 0 : n;
}

unsigned long rc_stream_coded(const rc_stream_t *s)
{
    return (unsigned long)s->pos * (unsigned long)RC_NMODEL + (unsigned long)s->m;
}

/* One-shot encode. Deliberately a thin wrapper rather than a second copy of
 * the loop: the sliced and unsliced paths must not be able to diverge, and the
 * only way to be sure of that is for there to be one path. */
size_t rc_encode_frame(const rc_models_t *M, const uint8_t *idx,
                       uint8_t *out, size_t cap)
{
    rc_stream_t s;
    rc_stream_start(&s, M, idx, out, cap);
    return rc_stream_finish(&s);
}

int rc_decode_frame(const rc_models_t *M, const uint8_t *in, size_t n,
                    uint8_t *idx_out)
{
    rc_dec_t d;
    rc_dec_init(&d, in, n);
    for (int pos = 0; pos < RC_NPOS; pos++) {
        for (int m = 0; m < RC_NMODEL; m++)
            rc_put_sym(idx_out, pos, m, rc_dec_sym(&d, &M->m[m]));
    }
    return 0;
}

long rc_selftest_frame(const rc_models_t *M, const uint8_t *idx,
                       uint8_t *scratch_bs, size_t bs_cap,
                       uint8_t *scratch_idx, long *first_bad)
{
    *first_bad = -1;
    size_t n = rc_encode_frame(M, idx, scratch_bs, bs_cap);
    if (n == 0) { *first_bad = 0; return RC_IDX_BYTES; }   // overflow
    rc_decode_frame(M, scratch_bs, n, scratch_idx);
    long bad = 0;
    for (long i = 0; i < (long)RC_IDX_BYTES; i++) {
        if (scratch_idx[i] != idx[i]) {
            if (*first_bad < 0) *first_bad = i;
            bad++;
        }
    }
    return bad;
}

// ---------------------------------------------------------------------------
// Empirical entropy of one actual frame, per codebook.
// The coded rate must be >= this. Comparing against the MODEL entropy instead
// is invalid whenever the model was not trained on exactly this frame.
// ---------------------------------------------------------------------------
void rc_frame_entropy(const uint8_t *idx, double bits_per_sym[RC_NMODEL])
{
    static uint32_t h[RC_NMODEL][RC_NSYM];
    memset(h, 0, sizeof(h));
    for (int pos = 0; pos < RC_NPOS; pos++)
        for (int m = 0; m < RC_NMODEL; m++)
            h[m][rc_get_sym(idx, pos, m)]++;
    for (int m = 0; m < RC_NMODEL; m++) {
        double H = 0.0;
        const double N = (double)RC_NPOS;
        for (int s = 0; s < RC_NSYM; s++) {
            if (!h[m][s]) continue;
            double p = (double)h[m][s] / N;
            H -= p * rc_log2(p);
        }
        bits_per_sym[m] = H;
    }
}
