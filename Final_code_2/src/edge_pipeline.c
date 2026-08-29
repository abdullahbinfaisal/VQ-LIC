// ============================================================================
// edge_pipeline.c -- see edge_pipeline.h for the timing contract.
// No printf inside any timed region.
// ============================================================================
#include "edge_pipeline.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ff.h"

// Supplied by main.c -- the same timer every existing measurement uses, so
// these numbers are directly comparable with the legacy B1/B2 path.
extern unsigned long long ep_timer_now(void);
extern double             ep_cycles_to_ms(unsigned long long);

#define NOW()   ep_timer_now()
#define MS(a,b) ep_cycles_to_ms((b) - (a))

// ---------------------------------------------------------------------------
// Source probing.
//
// A file named .png that is EXACTLY EP_RAW_BYTES long is raw pixels with a
// misleading extension. A file carrying PNG/JPEG magic is a real container and
// cannot be decoded on this bare-metal target -- we say so loudly rather than
// silently reading compressed bytes as pixels.
//
// Planar vs interleaved cannot be distinguished from size alone, so the caller
// selects it; this function only rules out the impossible cases.
// ---------------------------------------------------------------------------
ep_src_layout_t ep_probe_source(const char *path, size_t *nbytes)
{
    FIL f; UINT rd; uint8_t hdr[16];
    *nbytes = 0;
    if (f_open(&f, path, FA_READ) != FR_OK) {
        printf("[EP] probe: cannot open %s\n", path);
        return EP_SRC_UNKNOWN;
    }
    *nbytes = (size_t)f_size(&f);
    if (f_read(&f, hdr, sizeof hdr, &rd) != FR_OK || rd < 8) {
        f_close(&f); return EP_SRC_UNKNOWN;
    }
    f_close(&f);

    if (hdr[0] == 0x89 && hdr[1] == 'P' && hdr[2] == 'N' && hdr[3] == 'G') {
        printf("[EP] %s carries PNG magic (89 50 4E 47) and is %lu bytes.\n",
               path, (unsigned long)*nbytes);
        printf("[EP] This is a COMPRESSED CONTAINER. Bare-metal FatFs has no\n"
               "[EP] PNG decoder, so it cannot be used as pixel input.\n"
               "[EP] Expected raw size for %dx%dx%d is %lu bytes.\n",
               EP_W, EP_H, EP_CH, (unsigned long)EP_RAW_BYTES);
        return EP_SRC_NOT_RAW;
    }
    if (hdr[0] == 0xFF && hdr[1] == 0xD8) {
        printf("[EP] %s carries JPEG magic -- compressed container, unusable.\n", path);
        return EP_SRC_NOT_RAW;
    }
    if (*nbytes != EP_RAW_BYTES) {
        printf("[EP] %s is %lu bytes; expected %lu for raw %dx%dx%d.\n",
               path, (unsigned long)*nbytes, (unsigned long)EP_RAW_BYTES,
               EP_W, EP_H, EP_CH);
        return EP_SRC_UNKNOWN;
    }
    return EP_SRC_INTERLEAVED;   // caller overrides if the set is planar
}

// ---------------------------------------------------------------------------
// HWC -> CHW.  Scalar and simple on purpose: correctness and reproducibility
// first, per the brief. This is timed as t_deint and reported separately, so
// its cost is visible rather than folded into the packer.
// ---------------------------------------------------------------------------
void ep_deinterleave_rgb(const uint8_t *src_hwc, uint8_t *dst_chw)
{
    const size_t plane = (size_t)EP_W * EP_H;
    uint8_t *r = dst_chw, *g = dst_chw + plane, *b = dst_chw + 2 * plane;
    const uint8_t *s = src_hwc;
    for (size_t i = 0; i < plane; i++) {
        r[i] = s[0]; g[i] = s[1]; b[i] = s[2];
        s += 3;
    }
}

void ep_build_model(rc_models_t *M, const uint8_t *const *cal_idx,
                    int ncal, double *ms_out)
{
    unsigned long long t0 = NOW();
    rc_model_build(M, cal_idx, ncal);
    unsigned long long t1 = NOW();
    if (ms_out) *ms_out = MS(t0, t1);
}

// ---------------------------------------------------------------------------
// One frame: VQ then range coding, plus the direct end-to-end bracket.
//
// NOTE ON t_edge_direct: the caller must have already recorded t_host for this
// frame and pass it in via st->t_host. The analysis transform runs inside
// main.c's existing cascade call, which we do not modify; ep_run_frame times
// the VQ+range tail and reconstructs the direct bracket as
//   t_edge_direct = (host bracket) + (this bracket)
// measured with ONE contiguous timer pair spanning both where the caller
// arranges it (see ep_run_frame_with_host below in main.c's harness).
// ---------------------------------------------------------------------------
int ep_run_frame(int frame_id,
                 const vq_pq_ctx_t *vq,
                 const rc_models_t *M,
                 const uint8_t *latent,
                 uint8_t *idx_buf,
                 uint8_t *bs_buf,
                 size_t   bs_cap,
                 uint8_t *rt_buf,
                 int      do_roundtrip,
                 ep_frame_stat_t *st)
{
    unsigned long long t0, t1;

    st->frame_id = frame_id;

    // ---- B3: VQ nearest-codeword assignment -------------------------------
    t0 = NOW();
    vq_pq_encode_frame(vq, latent, idx_buf);
    t1 = NOW();
    st->t_vq = MS(t0, t1);

    // ---- B4: range coding (model lookup + code + flush only) --------------
    t0 = NOW();
    size_t nb = rc_encode_frame(M, idx_buf, bs_buf, bs_cap);
    t1 = NOW();
    st->t_range = MS(t0, t1);

    if (nb == 0) { printf("[EP] frame %d: range buffer overflow\n", frame_id); return -1; }

    st->range_bytes = nb;
    st->range_bits  = (double)nb * 8.0;
    // bpp over the SOURCE image pixels, which is what a codec reports
    st->range_bpp   = st->range_bits / ((double)EP_W * (double)EP_H);
    st->fixed_bpp   = ((double)VQ_IDX_BYTES * 8.0) / ((double)EP_W * (double)EP_H);

    // empirical entropy of THIS frame -- the valid lower bound for the coded
    // rate. Outside the timed region.
    rc_frame_entropy(idx_buf, st->h_emp);

    // ---- round-trip verification (outside all timed regions) --------------
    st->rc_mismatch = -1; st->rc_first_bad = -1;
    if (do_roundtrip) {
        rc_decode_frame(M, bs_buf, nb, rt_buf);
        long bad = 0, first = -1;
        for (long i = 0; i < (long)VQ_IDX_BYTES; i++)
            if (rt_buf[i] != idx_buf[i]) { if (first < 0) first = i; bad++; }
        st->rc_mismatch = bad; st->rc_first_bad = first;
    }

    st->t_edge_sum = st->t_host + st->t_vq + st->t_range;
    return 0;
}

// ---------------------------------------------------------------------------
// CSV output. Called only outside timed regions.
// ---------------------------------------------------------------------------
void ep_print_csv_header(void)
{
    printf("CSV,frame_id,t_load,t_deint,t_pack,t_prog,t_cache,t_pl,t_gap,"
           "t_host,t_vq,t_range,t_edge_sum,t_edge_direct,"
           "range_bytes,range_bpp,fixed_bpp,H0,H1,H2,H3,rc_mismatch\n");
}

void ep_print_csv_row(const ep_frame_stat_t *s)
{
    printf("CSV,%d,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,"
           "%.4f,%.4f,%.4f,%.4f,%.4f,%lu,%.5f,%.5f,%.4f,%.4f,%.4f,%.4f,%ld\n",
           s->frame_id, s->t_load, s->t_deint, s->t_pack, s->t_prog, s->t_cache,
           s->t_pl, s->t_gap, s->t_host, s->t_vq, s->t_range,
           s->t_edge_sum, s->t_edge_direct,
           (unsigned long)s->range_bytes, s->range_bpp, s->fixed_bpp,
           s->h_emp[0], s->h_emp[1], s->h_emp[2], s->h_emp[3],
           s->rc_mismatch);
}

// ---- order statistics -------------------------------------------------------

// Local sqrt -- this project links WITHOUT libm (see pm_sqrtf in main.c).
// Newton-Raphson; used only for standard deviation in the summary, outside
// every timed region.
static double ep_sqrt(double x)
{
    if (x <= 0.0) return 0.0;
    double r = (x > 1.0) ? x : 1.0;
    for (int i = 0; i < 60; i++) {
        double nr = 0.5 * (r + x / r);
        if (nr == r) break;
        r = nr;
    }
    return r;
}
static int dcmp(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return (x < y) ? -1 : (x > y) ? 1 : 0;
}

static void stat_line(const char *name, double *v, int n)
{
    static double s[512];
    if (n > 512) n = 512;
    memcpy(s, v, (size_t)n * sizeof(double));
    qsort(s, (size_t)n, sizeof(double), dcmp);
    double sum = 0.0;
    for (int i = 0; i < n; i++) sum += s[i];
    double mean = sum / n;
    double var = 0.0;
    for (int i = 0; i < n; i++) { double d = s[i] - mean; var += d * d; }
    double sd = (n > 1) ? ep_sqrt(var / (n - 1)) : 0.0;
    int p95 = (int)(0.95 * (n - 1) + 0.5);
    printf("STAT,%s,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.2f\n",
           name, mean, sd, s[0], s[n - 1], s[n / 2], s[p95],
           (mean != 0.0) ? 100.0 * sd / mean : 0.0);
}

void ep_print_summary(const ep_frame_stat_t *v, int n)
{
    static double tmp[512];
    printf("STAT,name,mean_ms,sd_ms,min_ms,max_ms,median_ms,p95_ms,cv_pct\n");
#define EMIT(field, label) \
    do { for (int i = 0; i < n; i++) tmp[i] = v[i].field; stat_line(label, tmp, n); } while (0)
    EMIT(t_pl,          "T_PL_DMA_B1");
    EMIT(t_host,        "T_HOST_B2");
    EMIT(t_vq,          "T_VQ_B3");
    EMIT(t_range,       "T_RANGE_B4");
    EMIT(t_edge_direct, "T_EDGE_DIRECT_B5");
    EMIT(t_edge_sum,    "T_EDGE_SUM_calc");
    EMIT(t_gap,         "T_GAP_unattributed");
    EMIT(t_pack,        "t_pack");
    EMIT(t_prog,        "t_prog");
    EMIT(t_cache,       "t_cache");
    EMIT(t_deint,       "t_deint");
    EMIT(t_load,        "t_load_SD_excluded");
#undef EMIT
    // rate stats
    for (int i = 0; i < n; i++) tmp[i] = v[i].range_bpp;
    stat_line("range_bpp", tmp, n);

    double sum_bytes = 0.0, sum_direct = 0.0, sum_host = 0.0,
           sum_vq = 0.0, sum_rng = 0.0;
    long   bad = 0;
    for (int i = 0; i < n; i++) {
        sum_bytes  += (double)v[i].range_bytes;
        sum_direct += v[i].t_edge_direct;
        sum_host   += v[i].t_host;
        sum_vq     += v[i].t_vq;
        sum_rng    += v[i].t_range;
        if (v[i].rc_mismatch > 0) bad += v[i].rc_mismatch;
    }
    double mdir = sum_direct / n;
    printf("COMP,stage,mean_ms,pct_of_edge\n");
    printf("COMP,T_HOST,%.4f,%.2f\n",  sum_host / n, 100.0 * (sum_host / n) / mdir);
    printf("COMP,T_VQ,%.4f,%.2f\n",    sum_vq   / n, 100.0 * (sum_vq   / n) / mdir);
    printf("COMP,T_RANGE,%.4f,%.2f\n", sum_rng  / n, 100.0 * (sum_rng  / n) / mdir);
    printf("EDGE,fps_edge,%.3f\n", 1000.0 / mdir);
    printf("EDGE,mean_range_bytes,%.1f\n", sum_bytes / n);
    printf("EDGE,mean_fixed_bytes,%d\n", VQ_IDX_BYTES);
    printf("EDGE,mean_saving_pct,%.2f\n",
           100.0 * (1.0 - (sum_bytes / n) / (double)VQ_IDX_BYTES));
    printf("EDGE,roundtrip_mismatches_total,%ld\n", bad);
}

// ---------------------------------------------------------------------------
// Deterministic synthetic frame, used ONLY when the SD dataset cannot be read
// as raw pixels (e.g. it is still PNG-encoded and this bare-metal target has
// no inflate).
//
// Deliberately NOT a constant fill. The legacy timed path used
// memset(raw_in, zp_in, ...), which makes every activation exactly zero after
// zero-point subtraction -- that would never exercise the dual-MAC borrow path
// fixed on 2026-08-25. This generator produces smooth gradients plus structured
// texture plus bounded noise, so activations span both signs across the full
// int9 range and the packed-product borrow fires on roughly half the samples.
//
// Timing validity: T_HOST and T_VQ are data-independent (fixed tensor shapes;
// the PQ search always scans all VQ_K codewords). T_RANGE is weakly
// data-dependent through renormalisation frequency. Power is weakly
// data-dependent through switching activity. Those two are the only numbers a
// real dataset would move, and both are already unrepresentative because the
// deployed codebook is unavailable on this board.
// ---------------------------------------------------------------------------
void ep_synth_frame_planar(uint8_t *dst_chw, int seed)
{
    const size_t plane = (size_t)EP_W * EP_H;
    uint32_t s = 0x9E3779B9u ^ (uint32_t)(seed * 2654435761u);
    for (int c = 0; c < EP_CH; c++) {
        uint8_t *p = dst_chw + (size_t)c * plane;
        for (int y = 0; y < EP_H; y++) {
            for (int x = 0; x < EP_W; x++) {
                s ^= s << 13; s ^= s >> 17; s ^= s << 5;
                int grad  = (x * 255) / EP_W;                  // horizontal ramp
                int band  = ((y >> 4) & 1) ? 40 : -40;         // horizontal structure
                int tex   = (((x >> 3) ^ (y >> 3)) & 15) * 4;  // block texture
                int noise = (int)(s & 31) - 16;                // bounded noise
                int v = grad + band + tex + noise + c * 17;
                if (v < 0) v = 0; else if (v > 255) v = 255;
                p[(size_t)y * EP_W + x] = (uint8_t)v;
            }
        }
    }
}
