// ============================================================================
// edge_harness.c -- complete edge-encoder measurement run.
//
// Self-contained. Does NOT modify the existing surrogate timing path, so the
// legacy B1/B2 numbers remain reproducible from the same binary. Enable with
//   #define RUN_EDGE_VALIDATION 1
// in main.c and call edge_validation_run() after the platform is up and the
// SD card is mounted.
//
// PROTOCOL (agreed 2026-08-25)
//   frames 001..020   calibration: build the static frequency model.
//                     Model build time is measured SEPARATELY (t_model_build)
//                     and is NOT part of T_RANGE.
//   frames 021..100   timed measurement set (80 frames), preceded by 3
//                     discarded warm-up frames.
//
// BOUNDARIES
//   t_deint is OUTSIDE T_EDGE_DIRECT (agreed): the deployed system receives
//   planar pixels from the ISP, so HWC->CHW is a harness artefact, not encoder
//   work. It is still measured and reported.
//   t_load (SD read) is likewise outside and reported separately.
//
//   T_EDGE_DIRECT is ONE contiguous timer pair spanning exactly:
//        six cascade pairs  +  VQ assignment  +  range coding
//   i.e. it starts at the same instant as T_HOST and ends when the coded byte
//   stream is complete in memory.
//
// RATE REPORTING
//   The codec's rate/RD is characterised in software. Coded byte counts here
//   exist only to derive coding throughput; they are NOT presented as codec
//   rate, because the deployed weights are not available on this board.
// ============================================================================
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "ff.h"
#include "xil_cache.h"
#include "edge_pipeline.h"
#include "vq_pq.h"
#include "range_coder.h"

// ---- supplied by main.c -----------------------------------------------------
extern unsigned long long ep_timer_now(void);
extern double             ep_cycles_to_ms(unsigned long long);

// main.c's cascade entry point and its per-frame phase accumulators.
struct layer_desc_s;
extern int  edge_run_six_pairs(const uint8_t *gm_in_frame);   // thin shim in main.c
extern void edge_reset_accums(void);
extern void edge_read_accums(double *pack, double *prog, double *cache,
                             double *pl, double *total);
extern const uint8_t *edge_latent_ptr(void);       // block-5 output buffer
extern uint8_t       *edge_raw_ptr(void);          // 2.76 MB staging for SD read
extern uint8_t       *edge_chw_ptr(void);          // de-interleave destination
extern const int8_t  *edge_codebook_ptr(void);
extern void          edge_set_quiet(int q);     // VQ codebook (may be synthetic)

/* per-pair silicon timing -- read-only, never inside a timed bracket */
extern int  edge_num_pairs(void);
extern int  edge_split_mode(void);
extern void edge_reset_pair_accums(void);
extern void edge_read_pair_cycles(unsigned long long *hw, unsigned long long *pack,
                                  unsigned long long *prog, unsigned long long *cache,
                                  int n);
extern void edge_read_pair_dims(int *cout, int *hout, int *wout, int *groups, int n);

#define EP_MAXP 8
/* T_VQ for QUERY_LANES=2. RTL SIMULATION, not silicon: 1,843,339 cycles at
 * 100 MHz. Labelled as such everywhere it is printed. Replace after Stage 4. */
#define VQ_SIM_MS       18.433
#define VQ_CYC_PER_GRP  1024

#define EDGE_NCAL      20
#define EDGE_NWARM      3
#define EDGE_NTIMED    80
#define EDGE_FIRST_TIMED (EDGE_NCAL + 1)

#define NOW()   ep_timer_now()
#define MS(a,b) ep_cycles_to_ms((b) - (a))

// Static so we never touch the heap in a timed path.
static ep_frame_stat_t g_stat[EDGE_NTIMED];
static uint8_t         g_idx  [VQ_IDX_BYTES];
static uint8_t         g_rt   [VQ_IDX_BYTES];
static uint8_t         g_bs   [VQ_IDX_BYTES * 2 + 4096];
static uint8_t        *g_cal_idx[EDGE_NCAL];
static uint8_t         g_cal_store[EDGE_NCAL][VQ_IDX_BYTES];

static void edge_path(char *dst, size_t cap, int n)
{
    // FatFs here is 8.3; "img_001.png" fits as IMG_001.PNG.
    snprintf(dst, cap, "0:/DIV2K/img_%03d.png", n);
}

// Load one frame from SD into raw staging. Returns 0 on success.
static int edge_load_raw(int n, uint8_t *dst, double *ms_out)
{
    char path[64]; FIL f; UINT rd;
    edge_path(path, sizeof path, n);
    unsigned long long t0 = NOW();
    if (f_open(&f, path, FA_READ) != FR_OK) { printf("[EDGE] open fail %s\n", path); return -1; }
    size_t got = 0;
    while (got < EP_RAW_BYTES) {
        UINT want = (UINT)((EP_RAW_BYTES - got > 65536u) ? 65536u : (EP_RAW_BYTES - got));
        if (f_read(&f, dst + got, want, &rd) != FR_OK || rd == 0) break;
        got += rd;
    }
    f_close(&f);
    unsigned long long t1 = NOW();
    if (ms_out) *ms_out = MS(t0, t1);
    if (got != EP_RAW_BYTES) {
        printf("[EDGE] %s short read: %lu of %lu\n", path,
               (unsigned long)got, (unsigned long)EP_RAW_BYTES);
        return -1;
    }
    return 0;
}

static int edge_prepare(int n, ep_src_layout_t layout, double *ms_load, double *ms_deint);

// Wrapper: real file if usable, deterministic synthetic frame otherwise.
static int edge_prepare2(int n, ep_src_layout_t layout, int use_synth,
                         double *ms_load, double *ms_deint)
{
    if (use_synth) {
        *ms_load = 0.0; *ms_deint = 0.0;
        ep_synth_frame_planar(edge_chw_ptr(), n);
        Xil_DCacheFlushRange((UINTPTR)edge_chw_ptr(), EP_RAW_BYTES);
        return 0;
    }
    return edge_prepare(n, layout, ms_load, ms_deint);
}

// Prepare one frame: SD read + de-interleave. Both OUTSIDE T_EDGE.
static int edge_prepare(int n, ep_src_layout_t layout,
                        double *ms_load, double *ms_deint)
{
    uint8_t *raw = edge_raw_ptr();
    uint8_t *chw = edge_chw_ptr();
    if (edge_load_raw(n, raw, ms_load) != 0) return -1;

    if (layout == EP_SRC_INTERLEAVED) {
        unsigned long long t0 = NOW();
        ep_deinterleave_rgb(raw, chw);
        unsigned long long t1 = NOW();
        *ms_deint = MS(t0, t1);
    } else {
        memcpy(chw, raw, EP_RAW_BYTES);   // already planar
        *ms_deint = 0.0;
    }
    Xil_DCacheFlushRange((UINTPTR)chw, EP_RAW_BYTES);
    return 0;
}

// ---------------------------------------------------------------------------
// One measured frame.  The single contiguous bracket is t_e0..t_e1.
// ---------------------------------------------------------------------------
static int edge_one(int frame_id, const vq_pq_ctx_t *vq, const rc_models_t *M,
                    int do_roundtrip, ep_frame_stat_t *st)
{
    memset(st, 0, sizeof(*st));
    st->frame_id = frame_id;

    edge_reset_accums();

    unsigned long long t_e0 = NOW();
    if (edge_run_six_pairs(edge_chw_ptr()) != 0) return -1;   // == T_HOST
    unsigned long long t_h1 = NOW();

    vq_pq_encode_frame(vq, edge_latent_ptr(), g_idx);         // == T_VQ
    unsigned long long t_v1 = NOW();

    size_t nb = rc_encode_frame(M, g_idx, g_bs, sizeof g_bs); // == T_RANGE
    unsigned long long t_e1 = NOW();

    if (nb == 0) { printf("[EDGE] f%d range overflow\n", frame_id); return -1; }

    st->t_edge_direct = MS(t_e0, t_e1);
    st->t_vq          = MS(t_h1, t_v1);
    st->t_range       = MS(t_v1, t_e1);

    double pack, prog, cache, pl, total;
    edge_read_accums(&pack, &prog, &cache, &pl, &total);
    st->t_pack = pack; st->t_prog = prog; st->t_cache = cache; st->t_pl = pl;
    st->t_host = total;
    st->t_gap  = total - (pack + prog + cache + pl);

    st->range_bytes = nb;
    st->range_bits  = (double)nb * 8.0;
    st->range_bpp   = st->range_bits / ((double)EP_W * (double)EP_H);
    st->fixed_bpp   = ((double)VQ_IDX_BYTES * 8.0) / ((double)EP_W * (double)EP_H);
    st->t_edge_sum  = st->t_host + st->t_vq + st->t_range;

    // everything below is OUTSIDE all timed regions
    rc_frame_entropy(g_idx, st->h_emp);
    st->rc_mismatch = -1; st->rc_first_bad = -1;
    if (do_roundtrip) {
        rc_decode_frame(M, g_bs, nb, g_rt);
        long bad = 0, first = -1;
        for (long i = 0; i < (long)VQ_IDX_BYTES; i++)
            if (g_rt[i] != g_idx[i]) { if (first < 0) first = i; bad++; }
        st->rc_mismatch = bad; st->rc_first_bad = first;
    }
    return 0;
}

// ---------------------------------------------------------------------------
int edge_validation_run(void)
{
    char path[64];
    size_t nbytes;
    vq_pq_ctx_t vq;
    rc_models_t M;
    double t_model_build = 0.0;

    printf("\n================================================\n");
    printf(" COMPLETE EDGE ENCODER VALIDATION\n");
    printf(" PL analysis -> VQ -> range coding -> byte stream\n");
    printf("================================================\n");

    // ---- probe the dataset before doing anything else ----------------------
    edge_path(path, sizeof path, 1);
    ep_src_layout_t layout = ep_probe_source(path, &nbytes);
    int use_synth = 0;
    if (layout == EP_SRC_NOT_RAW || layout == EP_SRC_UNKNOWN) {
        use_synth = 1;
        printf("[EDGE] *********************************************************\n");
        printf("[EDGE] DATASET NOT USABLE AS RAW PIXELS -> SYNTHETIC INPUT IN USE\n");
        printf("[EDGE] Latency below is from a deterministic synthetic frame, NOT\n");
        printf("[EDGE] DIV2K. T_HOST and T_VQ are data-independent so they stay\n");
        printf("[EDGE] valid; T_RANGE and power are weakly data-dependent and\n");
        printf("[EDGE] MUST be labelled synthetic in the manuscript.\n");
        printf("[EDGE] To use the real set, convert to raw 2764800-byte frames.\n");
        printf("[EDGE] *********************************************************\n");
    }
    printf("[EDGE] source: %lu bytes/frame, treating as %s\n",
           (unsigned long)nbytes,
           (layout == EP_SRC_INTERLEAVED) ? "INTERLEAVED RGB888 (will de-interleave)"
                                          : "PLANAR CHW");

    /* Silence the per-pair diagnostics: they are inside the timed bracket. */
    edge_set_quiet(1);

    vq_pq_init(&vq, edge_codebook_ptr(), 0);

    // ---- calibration pass: frames 1..EDGE_NCAL -----------------------------
    printf("[EDGE] calibration over frames 1..%d (model build timed separately)\n",
           EDGE_NCAL);
    double ms_load, ms_deint;
    for (int i = 0; i < EDGE_NCAL; i++) {
        if (edge_prepare2(i + 1, layout, use_synth, &ms_load, &ms_deint) != 0) return -1;
        edge_reset_accums();
        if (edge_run_six_pairs(edge_chw_ptr()) != 0) return -1;
        vq_pq_encode_frame(&vq, edge_latent_ptr(), g_cal_store[i]);
        g_cal_idx[i] = g_cal_store[i];
    }
    ep_build_model(&M, (const uint8_t *const *)g_cal_idx, EDGE_NCAL, &t_model_build);
    double Hm[RC_NMODEL];
    rc_model_entropy(&M, Hm);
    printf("[EDGE] model built in %.4f ms (NOT counted in T_RANGE)\n", t_model_build);
    printf("[EDGE] model entropy per codebook: %.4f %.4f %.4f %.4f bits/sym\n",
           Hm[0], Hm[1], Hm[2], Hm[3]);

    /* ON-TARGET VERIFICATION of the restructured NEON search (2026-08-26).
     * search_sub now keeps the running minimum in NEON registers. Confirm on the
     * real device that it still yields indices identical to the scalar reference
     * before any timing is reported. vq_pq_selftest() lives behind VQ_PQ_BENCH,
     * so compare against vq_pq_encode_frame_ref() directly. */
    {
        long bad = 0, first = -1;
        vq_pq_encode_frame(&vq, edge_latent_ptr(), g_idx);
        vq_pq_encode_frame_ref(&vq, edge_latent_ptr(), g_rt);
        for (long i = 0; i < (long)VQ_IDX_BYTES; i++)
            if (g_idx[i] != g_rt[i]) { if (first < 0) first = i; bad++; }
        printf("[EDGE] VQ NEON vs scalar reference: %ld mismatches of %d, first at %ld -> %s\n",
               bad, VQ_IDX_BYTES, first, (bad == 0) ? "PASS" : "FAIL");
        if (bad != 0) {
            printf("[EDGE] ABORT: restructured VQ search is not reference-exact.\n");
            edge_set_quiet(0);
            return -1;
        }
    }
    // ---- warm-up, discarded -------------------------------------------------
    ep_frame_stat_t junk;
    for (int w = 0; w < EDGE_NWARM; w++) {
        int fid = EDGE_FIRST_TIMED + w;
        if (edge_prepare2(fid, layout, use_synth, &ms_load, &ms_deint) != 0) return -1;
        edge_one(fid, &vq, &M, 0, &junk);
    }
    printf("[EDGE] %d warm-up frames discarded\n", EDGE_NWARM);

    // ---- timed set ----------------------------------------------------------
    /* Per-pair accumulators are zeroed here so the calibration and warm-up
     * frames do not contribute. They are read only between frames. */
    static unsigned long long p_prev[EP_MAXP], p_now[EP_MAXP];
    static unsigned long long p_sum[EP_MAXP], p_min[EP_MAXP], p_max[EP_MAXP];
    const int npairs = edge_num_pairs();
    edge_reset_pair_accums();
    for (int p = 0; p < EP_MAXP; p++) {
        p_prev[p] = 0; p_sum[p] = 0; p_max[p] = 0;
        p_min[p] = (unsigned long long)-1;
    }

    int n = 0;
    for (int i = 0; i < EDGE_NTIMED; i++) {
        int fid = EDGE_FIRST_TIMED + i;
        if (fid > 100) break;
        if (edge_prepare2(fid, layout, use_synth, &ms_load, &ms_deint) != 0) return -1;
        if (edge_one(fid, &vq, &M, 1, &g_stat[n]) != 0) return -1;
        g_stat[n].t_load  = ms_load;    // outside T_EDGE, reported only
        g_stat[n].t_deint = ms_deint;   // outside T_EDGE, reported only

        /* snapshot AFTER the frame returns -- outside every timed bracket */
        edge_read_pair_cycles(p_now, 0, 0, 0, EP_MAXP);
        for (int p = 0; p < npairs; p++) {
            unsigned long long d = p_now[p] - p_prev[p];
            p_prev[p] = p_now[p];
            p_sum[p] += d;
            if (d < p_min[p]) p_min[p] = d;
            if (d > p_max[p]) p_max[p] = d;
        }
        n++;
    }

    // ---- report (all printf outside any timed region) -----------------------
    edge_set_quiet(0);
    printf("\n[EDGE] timed frames: %d\n", n);
    ep_print_csv_header();
    for (int i = 0; i < n; i++) ep_print_csv_row(&g_stat[i]);
    printf("\n");
    ep_print_summary(g_stat, n);

    /* ================= per-pair silicon timing =============================
     * Every value here comes from accumulators the cascade already maintained
     * silently. No print, no poll and no extra read entered the timed bracket.
     * ===================================================================== */
    {
        int cout[EP_MAXP], hout[EP_MAXP], wout[EP_MAXP], grp[EP_MAXP];
        double ms[EP_MAXP], tot_ms = 0.0;
        edge_read_pair_dims(cout, hout, wout, grp, EP_MAXP);
        for (int p = 0; p < npairs; p++) {
            ms[p]   = ep_cycles_to_ms(p_sum[p]) / (double)n;
            tot_ms += ms[p];
        }

        printf("\n[PAIR] ===== per-pair silicon timing, DW->PW cascade =====\n");
        printf("[PAIR] CASCADE_ENGINE_SPLIT=%d   cascade diagnostics suppressed for\n",
               edge_split_mode());
        printf("[PAIR] every timed frame; per-pair values read only BETWEEN frames.\n");
        printf("[PAIR] timed frames = %d.  PL clock taken as 100 MHz for cycles.\n", n);
        printf("[PAIR]\n");
        printf("[PAIR] pair Cout    HxW      groups    PL cycles   HW ms    min ms   max ms  cyc/grp  share%%\n");
        for (int p = 0; p < npairs; p++) {
            double cyc = ms[p] * 100000.0;                 /* ms -> cycles @100 MHz */
            printf("[PAIR]  %d   %4d %4dx%-4d  %7d %11.0f  %7.4f  %7.4f %7.4f %8.1f  %5.1f\n",
                   p + 1, cout[p], hout[p], wout[p], grp[p], cyc, ms[p],
                   ep_cycles_to_ms(p_min[p]), ep_cycles_to_ms(p_max[p]),
                   grp[p] ? cyc / (double)grp[p] : 0.0,
                   (tot_ms > 0.0) ? 100.0 * ms[p] / tot_ms : 0.0);
        }
        printf("[PAIR] total PL analysis (sum of pairs) = %.4f ms\n", tot_ms);

        {
            const int p6   = npairs - 1;
            const double m6 = ms[p6];
            const double c6 = m6 * 100000.0;
            const int    g6 = grp[p6];
            const double prod = (g6 > 0) ? c6 / (double)g6 : 0.0;

            printf("\n[PAIR] ---- final pair (pair %d) ----\n", npairs);
            printf("[PAIR]   duration       : %.0f PL cycles = %.4f ms\n", c6, m6);
            printf("[PAIR]   output groups  : %d\n", g6);
            printf("[PAIR]   cycles / group : %.1f\n", prod);
            printf("[PAIR]   share of total PL analysis : %.2f %%\n",
                   (tot_ms > 0.0) ? 100.0 * m6 / tot_ms : 0.0);

            printf("\n[PAIR] ---- same-frame DDR trailing-read overlap, QUERY_LANES=2 ----\n");
            printf("[PAIR]   T_analysis (SILICON, measured)      = %.4f ms\n", tot_ms);
            printf("[PAIR]   T_pair6    (SILICON, measured)      = %.4f ms\n", m6);
            printf("[PAIR]   T_VQ       (RTL SIMULATION ONLY)    = %.4f ms\n",
                   (double)VQ_SIM_MS);
            printf("[PAIR]   T_latency_est = T_analysis + T_VQ - T_pair6 = %.4f ms\n",
                   tot_ms + (double)VQ_SIM_MS - m6);
            printf("[PAIR]   NOTE: T_VQ is simulated, not silicon. This estimate mixes\n");
            printf("[PAIR]   measurement classes and must be labelled so until Stage 4.\n");

            printf("\n[PAIR] ---- direct PW->VQ stream fusion feasibility ----\n");
            printf("[PAIR]   pair%d producer rate : %.1f cycles/group\n", npairs, prod);
            printf("[PAIR]   VQ consumer rate    : %d cycles/group (QUERY_LANES=2)\n",
                   VQ_CYC_PER_GRP);
            if (prod >= (double)VQ_CYC_PER_GRP) {
                printf("[PAIR]   VERDICT: producer is SLOWER than the VQ consumer ->\n");
                printf("[PAIR]            direct stream fusion IS rate-feasible.\n");
            } else {
                printf("[PAIR]   VERDICT: producer is %.1fx FASTER than the VQ consumer ->\n",
                       (double)VQ_CYC_PER_GRP / ((prod > 0.0) ? prod : 1.0));
                printf("[PAIR]            direct stream fusion is NOT rate-feasible; VQ would\n");
                printf("[PAIR]            backpressure the PW and stretch pair %d.\n", npairs);
            }
        }
        printf("[PAIR] ===== end per-pair report =====\n");
    }

    // sum-vs-direct cross-check
    double d = 0.0, s = 0.0;
    for (int i = 0; i < n; i++) { d += g_stat[i].t_edge_direct; s += g_stat[i].t_edge_sum; }
    d /= n; s /= n;
    printf("XCHK,T_EDGE_DIRECT_mean_ms,%.4f\n", d);
    printf("XCHK,T_EDGE_SUM_mean_ms,%.4f\n", s);
    printf("XCHK,difference_ms,%.4f\n", d - s);
    printf("XCHK,difference_pct,%.3f\n", (s != 0.0) ? 100.0 * (d - s) / s : 0.0);
    printf("MODEL,t_model_build_ms,%.4f\n", t_model_build);
    printf("[EDGE] done\n");
    return 0;
}
