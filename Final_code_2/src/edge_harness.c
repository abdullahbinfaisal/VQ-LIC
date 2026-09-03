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
/* PL VQ. Set to 0 to build a NEON-only binary; the PL path is additive and
 * never replaces the B3 measurement, so previously reported numbers stay
 * reproducible from the same source.
 * MUST be defined before the #if below that pulls in vq_pl.h.
 *
 * DEFAULT CHANGED TO 0 ON 2026-09-04, and this is not a preference -- the
 * hardware it talks to no longer exists. vq_pl.c drives vq_pq_axi_0 at
 * 0x43C20000, and that block was deleted from the block design when VQ moved
 * onto the PW engine (axi_dma_2 now feeds pw_single_oc_axis_axi_0's
 * s_axis_vq). Built against the current bitstream this path would poll a
 * STATUS register in unmapped address space and hang, which is exactly the
 * kind of failure that looks like a datapath bug for a day.
 *
 * Setting it back to 1 is only valid against a bitstream that still carries
 * the dedicated engine -- i.e. before commit 62cbfbc, or with USE_PW_VQ=0 and
 * vq_pq_axi_0 restored.
 *
 * THE REPLACEMENT IS NOT WRITTEN YET. What it has to do is fully pinned down
 * by Zynq.srcs/sources_1/new/tb_pw_axi_vq.sv, which drives the real register
 * map and passes bit-exact. Two protocol obligations from that bench are NOT
 * obvious from the register map and cost a day each if missed:
 *
 *   1. An AXI-lite write commits only when awready, awvalid, wready and
 *      wvalid are all high in the SAME cycle. Retiring the address and data
 *      channels independently drops writes silently.
 *   2. Writing ADDR_CTRL bit 0 (start) RESETS the input FIFO. The latent DMA
 *      must be kicked AFTER the engine is started, never before, or the beats
 *      already in flight are flushed and the search runs on a shifted latent
 *      while every programmed value still verifies.
 *
 * Sequence: geometry (TILE_PIXELS, CIN_RUN=16, COUT_RUN=128, ZP_RELU=0x8080),
 * weights per OC bank via OC_SEL/W_BRAM_OFF/W_BASE, 128 norms via VQ_NORM,
 * VQ_CTRL = {vq_cin_load=64, vq_mode=1}, then start, then the DMA.
 */
#ifndef EDGE_USE_PL_VQ
#define EDGE_USE_PL_VQ 0
#endif

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "ff.h"
#include "xil_cache.h"
#include "edge_pipeline.h"
#include "vq_pq.h"
#include "range_coder.h"
#if EDGE_USE_PL_VQ
#include "vq_pl.h"
#endif

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

/* Local sqrt: the project links WITHOUT libm, and ep_sqrt is static inside
 * edge_pipeline.c so it is not visible here. Newton-Raphson, plenty for a
 * standard deviation printed outside every timed region. */
static double eh_sqrt(double x)
{
    if (x <= 0.0) return 0.0;
    double r = x;
    for (int i = 0; i < 40; i++) r = 0.5 * (r + x / r);
    return r;
}

#define NOW()   ep_timer_now()
#define MS(a,b) ep_cycles_to_ms((b) - (a))


/* ---- PMBus, supplied by main.c ------------------------------------------- */
extern int  edge_pm_init(void);
extern int  edge_pm_nrails(void);
extern int  edge_pm_scan(double *w, int n);
extern const char *edge_pm_rail_name(int i);
extern const char *edge_pm_group_name(int i);

/* POWER MEASUREMENT (2026-08-30).
 *
 * Off by default: the run adds minutes of wall time and its I2C traffic must
 * never land inside a timed bracket, so it runs only after all timing and
 * reporting is finished.
 *
 * DESIGN CONSTRAINT, from the 2026-08-05 finding already recorded in main.c:
 * the UCD9248 emits current in 1/64 A (15.625 mA) steps, and per-sample sd is
 * 15-30 mA on every rail regardless of load -- that scatter is a property of
 * the measurement, not the board. So a DIFFERENCE of two single readings is
 * unrecoverable, while the MEAN of many is not: the dither is zero-mean.
 * Everything below therefore reports means with standard errors, and states
 * plainly when a difference is not resolved.
 *
 * A and B are INTERLEAVED, not run sequentially. A sequential idle-then-active
 * attempt on 2026-07-30 returned physically impossible NEGATIVE deltas because
 * thermal drift over the run exceeded the signal. */
#ifndef EDGE_POWER
#define EDGE_POWER 1      /* 1 = run the power measurement after all timing */
#endif
#define EDGE_PWR_CYCLES    16U   /* interleaved A/B repetitions        */
#define EDGE_PWR_PHASE_S    8U   /* seconds per phase per cycle        */
#define EDGE_PWR_SKIP_MS 1500.0  /* discarded at each phase start      */
#define EDGE_PWR_SCAN_MS  150.0  /* between full rail scans            */
#define EDGE_PWR_MAXR      16

// Static so we never touch the heap in a timed path.
static ep_frame_stat_t g_stat[EDGE_NTIMED];
static uint8_t         g_idx  [VQ_IDX_BYTES];
static uint8_t         g_rt   [VQ_IDX_BYTES];
static uint8_t         g_bs   [VQ_IDX_BYTES * 2 + 4096];
static uint8_t        *g_cal_idx[EDGE_NCAL];
static uint8_t         g_cal_store[EDGE_NCAL][VQ_IDX_BYTES];
#if EDGE_USE_PL_VQ
static uint8_t         g_pl_idx[VQ_IDX_BYTES];
static int             g_pl_ok = 0;      /* set only after the block identifies */
#endif

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


/* Interleaved A/B energy measurement.
 *   phase A = PL analysis only
 *   phase B = PL analysis + PL VQ
 * Both run frames back to back and sample PMBus between frames -- the A9
 * cannot poll I2C and run a frame at the same instant. The scan costs ~12 ms
 * of a 150 ms interval, so ~8% of each phase is spent not computing; the
 * reported duty makes that visible rather than hiding it.
 */
#if EDGE_POWER
/* One PIPELINED frame, the deployed topology: the PL VQ for the PREVIOUS
 * frame's latent overlaps THIS frame's analysis. They use disjoint hardware --
 * analysis on HP0/HP1, VQ on HP2 -- and disjoint latent buffers, which is what
 * the chain double-buffering exists for: edge_run_six_pairs writes the buffer
 * the VQ is not reading.
 *
 * run_vq=0 gives the same loop with the VQ never started, so A and B differ in
 * exactly one thing. */
static const uint8_t *g_pipe_lat = 0;

static int edge_pipe_frame(int run_vq)
{
    int started = 0;
    if (run_vq && g_pl_ok && g_pipe_lat) {
        vq_pl_cache_prep(g_pipe_lat, g_pl_idx, 0);
        if (vq_pl_start(g_pipe_lat, g_pl_idx) == 0) started = 1;
    }
    if (edge_run_six_pairs(edge_chw_ptr()) != 0) return -1;
    if (started) {
        int r; unsigned long guard = 0;
        while ((r = vq_pl_poll_done()) == 0 && ++guard < 200000000u) { }
        if (r > 0) vq_pl_finish(g_pl_idx);
    }
    g_pipe_lat = edge_latent_ptr();      /* becomes the NEXT frame's VQ input */
    return 0;
}

/* Interleaved, DUTY-MATCHED A/B.
 *
 *   A = pipelined loop, VQ never started
 *   B = pipelined loop, VQ overlapped
 *
 * Both phases pad every frame to the SAME period. Without that the comparison
 * is confounded: the first attempt ran VQ serially, so phase B's analysis was
 * only 51% busy against 100% in phase A, the VQ replaced analysis activity
 * rather than adding to it, and the delta came out at +4.9 mW -- indistinguish-
 * able from zero for the wrong reason.
 *
 * The pad is a spin, not a sleep. That is deliberate: the CPU behaves
 * identically in both phases, so it cancels in the difference. */
static void edge_power_measure(const vq_pq_ctx_t *vq)
{
    const int nr = edge_pm_nrails();
    double sum[2][EDGE_PWR_MAXR], sq[2][EDGE_PWR_MAXR], now[EDGE_PWR_MAXR];
    unsigned long ns[2] = {0,0}, nfr[2] = {0,0};
    double t_fr[2] = {0.0,0.0}, wall[2] = {0.0,0.0};
    double t_pipe = 0.0, pad_ms;

    (void)vq;
    if (nr <= 0 || nr > EDGE_PWR_MAXR) { printf("PWRBAD rails %d\n", nr); return; }
    if (edge_pm_init() != 0) { printf("PWRSKIP PMBus init failed\n"); return; }
    for (int ph = 0; ph < 2; ph++)
        for (int r = 0; r < nr; r++) { sum[ph][r] = 0.0; sq[ph][r] = 0.0; }

    edge_set_quiet(1);

    /* ---- calibrate the pipelined period, and report it: this is also the
     * first MEASURED confirmation of max(analysis, VQ) rather than arithmetic */
    g_pipe_lat = 0;
    for (int i = 0; i < 3; i++) if (edge_pipe_frame(1) != 0) { edge_set_quiet(0); printf("PWRBAD prime\n"); return; }
    {
        unsigned long long c0 = NOW();
        const int NC = 20;
        for (int i = 0; i < NC; i++)
            if (edge_pipe_frame(1) != 0) { edge_set_quiet(0); printf("PWRBAD cal\n"); return; }
        t_pipe = MS(c0, NOW()) / (double)NC;
    }
    pad_ms = t_pipe * 1.02;               /* small headroom so A can always reach it */
    edge_set_quiet(0);
    printf("PWRPIPE measured pipelined frame period = %.4f ms -> %.2f fps\n",
           t_pipe, 1000.0 / t_pipe);
    printf("PWRPIPE both phases padded to %.4f ms so duty is identical\n", pad_ms);
    edge_set_quiet(1);

    for (unsigned c = 0; c < EDGE_PWR_CYCLES; c++) {
        for (int ph = 0; ph < 2; ph++) {
            unsigned long long p0 = NOW(), tl = p0;
            while (MS(p0, NOW()) < (double)EDGE_PWR_PHASE_S * 1000.0) {
                unsigned long long f0 = NOW();
                if (edge_pipe_frame(ph == 1) != 0) {
                    edge_set_quiet(0); printf("PWRBAD frame failed\n"); return;
                }
                t_fr[ph] += MS(f0, NOW());
                nfr[ph]++;
                while (MS(f0, NOW()) < pad_ms) { }      /* duty match */

                if (MS(p0, NOW()) < EDGE_PWR_SKIP_MS) continue;
                if (MS(tl, NOW()) < EDGE_PWR_SCAN_MS) continue;
                tl = NOW();
                if (edge_pm_scan(now, nr) != 0) continue;
                for (int r = 0; r < nr; r++) { sum[ph][r] += now[r]; sq[ph][r] += now[r]*now[r]; }
                ns[ph]++;
            }
            wall[ph] += MS(p0, NOW());
        }
    }
    edge_set_quiet(0);
    if (ns[0] < 8 || ns[1] < 8) { printf("PWRBAD too few samples\n"); return; }

    double totA = 0.0, totB = 0.0, seA2 = 0.0, seB2 = 0.0;
    printf("PWRTAB rail        group     A_W       B_W      delta_W\n");
    for (int r = 0; r < nr; r++) {
        double mA = sum[0][r]/ns[0], mB = sum[1][r]/ns[1];
        double vA = sq[0][r]/ns[0] - mA*mA, vB = sq[1][r]/ns[1] - mB*mB;
        if (vA < 0.0) vA = 0.0;
        if (vB < 0.0) vB = 0.0;
        totA += mA; totB += mB; seA2 += vA/(double)ns[0]; seB2 += vB/(double)ns[1];
        printf("PWRTAB %-10s %-6s %8.4f %8.4f %+9.4f\n",
               edge_pm_rail_name(r), edge_pm_group_name(r), mA, mB, mB-mA);
    }
    double seA = eh_sqrt(seA2), seB = eh_sqrt(seB2), seD = eh_sqrt(seA2+seB2);
    double dut[2];
    for (int ph = 0; ph < 2; ph++) dut[ph] = 100.0*t_fr[ph]/wall[ph];

    printf("PWRSUM samples A=%lu B=%lu   frames A=%lu B=%lu\n", ns[0], ns[1], nfr[0], nfr[1]);
    printf("PWRSUM compute duty A=%.1f%% B=%.1f%%  (must match; the rest is pad+scan)\n",
           dut[0], dut[1]);
    printf("PWRSUM board A (no VQ)   = %.4f W +- %.4f\n", totA, seA);
    printf("PWRSUM board B (with VQ) = %.4f W +- %.4f\n", totB, seB);
    printf("PWRSUM VQ incremental    = %+.4f W +- %.4f\n", totB-totA, seD);
    if ((totB-totA) < 2.0*seD)
        printf("PWRSUM NOT RESOLVED: |delta| < 2 SE -> VQ costs less than %.1f mW.\n"
               "PWRSUM Report as an UPPER BOUND, not as zero.\n", 2000.0*seD);
    else
        printf("PWRSUM resolved at %.1f sigma\n", (totB-totA)/seD);

    printf("PWRSUM PIPELINED energy per frame = %.4f W x %.4f ms = %.2f mJ\n",
           totB, t_pipe, totB*t_pipe);
    printf("PWRSUM regulator OUTPUT power only -- excludes conversion losses and\n");
    printf("PWRSUM the unmonitored 5 V USB rail. NOT 12 V input power.\n");
}
#endif /* EDGE_POWER */

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

    /* ---- PL VQ, timed SEPARATELY and strictly after t_e1 -------------------
     * Deliberately outside the T_EDGE_DIRECT bracket so B1..B5 stay identical
     * to the NEON-only run and remain comparable with everything already
     * reported. The latent buffer is untouched until the next frame's
     * analysis, so the PL sees exactly the data the NEON path just used. */
    st->t_vq_pl       = -1.0;
    st->t_vq_pl_cache = -1.0;
    st->t_vq_pl_block = -1.0;
    st->pl_flushed    = -1;
    st->pl_mismatch   = -1;
#if EDGE_USE_PL_VQ
    if (g_pl_ok) {
        /* FLUSH TEST. Alternate the latent flush frame by frame so a single
         * run gives both populations under identical conditions. It is
         * self-verifying: if the flush really were needed for coherency, the
         * PL (reading DDR) and NEON (reading cache) would diverge on the
         * unflushed frames and pl_mismatch would fire. */
        /* Always no-flush now: the A/B at bring-up re-establishes each run
         * that this is safe, so the timed population stays single-valued
         * instead of bimodal. */
        const int do_flush = 0;
        st->pl_flushed = do_flush;

        unsigned long long p0 = NOW();
        vq_pl_cache_prep(edge_latent_ptr(), g_pl_idx, do_flush);
        unsigned long long pc = NOW();
        if (vq_pl_start(edge_latent_ptr(), g_pl_idx) == 0) {
            int r; unsigned long long guard = 0;
            while ((r = vq_pl_poll_done()) == 0 && ++guard < 200000000u) { }
            if (r > 0) vq_pl_finish(g_pl_idx);
            unsigned long long p1 = NOW();
            st->t_vq_pl_cache = MS(p0, pc);
            st->t_vq_pl_block = MS(pc, p1);
            st->t_vq_pl = MS(p0, p1);
            long bad = 0;
            for (long i = 0; i < (long)VQ_IDX_BYTES; i++)
                if (g_pl_idx[i] != g_idx[i]) bad++;
            st->pl_mismatch = bad;      /* every timed frame is checked */
        }
    }
#endif

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
    /* ---- PL VQ bring-up and correctness gate ------------------------------
     * Nothing about the PL path may be reported unless it returns exactly the
     * same indices as the scalar reference ON THIS BOARD. The ID register is
     * checked first: a stale bitstream without this block would otherwise fail
     * in a much more confusing way. */
#if EDGE_USE_PL_VQ
    printf("[EDGE] ---- PL VQ bring-up ----\n");
    if (vq_pl_init() != 0) {
        printf("[EDGE] PL VQ unavailable -> PL columns will be blank.\n");
        printf("[EDGE] If this is unexpected, the loaded bitstream is stale:\n");
        printf("[EDGE] rebuild the platform against vq_accel.xsa and reprogram.\n");
        g_pl_ok = 0;
    } else {
        vq_pl_load_codebook(edge_codebook_ptr(), 0, VQ_NGROUPS);
        long first = -1;
        /* A/B the latent flush once per run. Both halves are compared against
         * the scalar reference, so if the flush ever becomes necessary this
         * fires here rather than silently corrupting a measurement. */
        unsigned long long f0 = NOW();
        long bad_f = vq_pl_verify(&vq, edge_latent_ptr(), g_pl_idx, g_rt, &first, 1);
        unsigned long long f1 = NOW();
        long bad_n = vq_pl_verify(&vq, edge_latent_ptr(), g_pl_idx, g_rt, &first, 0);
        unsigned long long f2 = NOW();
        printf("[EDGE] flush A/B: with %.4f ms (mism %ld), without %.4f ms (mism %ld)\n",
               MS(f0, f1), bad_f, MS(f1, f2), bad_n);
        if (bad_n != 0) {
            printf("[EDGE] *** the latent flush IS required after all -- restore it\n");
            printf("[EDGE] *** in vq_pl_encode_frame (vq_pl.c) before trusting timing\n");
        }
        long bad = (bad_f != 0) ? bad_f : bad_n;
        printf("[EDGE] PL VQ vs scalar reference: %ld mismatches of %d, first at %ld -> %s\n",
               bad, VQ_IDX_BYTES, first, (bad == 0) ? "PASS" : "FAIL");
        printf("[EDGE] PL beats in=%u out=%u (expect %u / %u)\n",
               (unsigned)vq_pl_dbg_in(), (unsigned)vq_pl_dbg_out(),
               (unsigned)((uint32_t)VQ_NGROUPS * VQ_DIM * VQ_LANES / 8u),
               (unsigned)(VQ_IDX_BYTES / 8u));
        if (bad != 0) {
            printf("[EDGE] ABORT: PL VQ is not reference-exact. No PL timing will\n");
            printf("[EDGE] be reported -- a fast wrong answer is not a result.\n");
            edge_set_quiet(0);
            return -1;
        }
        vq_pl_report_cycles(VQ_NGROUPS);
        g_pl_ok = 1;
    }
#endif

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

    /* ---- PL VQ results ---------------------------------------------------- */
#if EDGE_USE_PL_VQ
    if (g_pl_ok) {
        double s = 0.0, mn = 1e30, mx = -1e30;
        long   bad_total = 0, checked_frames = 0;
        for (int i = 0; i < n; i++) {
            if (g_stat[i].t_vq_pl < 0.0) continue;
            const double v = g_stat[i].t_vq_pl;
            s += v; if (v < mn) mn = v; if (v > mx) mx = v;
            if (g_stat[i].pl_mismatch > 0) bad_total += g_stat[i].pl_mismatch;
            checked_frames++;
        }
        if (checked_frames > 0) {
            const double mean = s / (double)checked_frames;
            double var = 0.0;
            for (int i = 0; i < n; i++)
                if (g_stat[i].t_vq_pl >= 0.0) {
                    const double d = g_stat[i].t_vq_pl - mean;
                    var += d * d;
                }
            const double sd = eh_sqrt(var / (double)checked_frames);

            printf("\n[PLVQ] ===== PL VQ block, MEASURED ON SILICON =====\n");
            printf("STAT,T_VQ_PL,%.4f,%.4f,%.4f,%.4f,,,%.2f\n",
                   mean, sd, mn, mx, (mean > 0.0) ? 100.0 * sd / mean : 0.0);
            printf("[PLVQ] frames timed          : %ld\n", checked_frames);
            printf("[PLVQ] mean                  : %.4f ms  (sd %.4f, min %.4f, max %.4f)\n",
                   mean, sd, mn, mx);
            printf("[PLVQ] index mismatches      : %ld over %ld frames x %d indices\n",
                   bad_total, checked_frames, VQ_IDX_BYTES);
            printf("[PLVQ] equivalent cycles     : %.0f at 100 MHz\n", mean * 100000.0);
            printf("[PLVQ] cycles / 8-pixel group: %.1f  (RTL simulation: %d)\n",
                   mean * 100000.0 / (double)VQ_NGROUPS, VQ_CYC_PER_GRP);
            printf("[PLVQ] RTL simulation said   : %.3f ms -- silicon vs sim = %+.3f ms (%+.2f%%)\n",
                   (double)VQ_SIM_MS, mean - (double)VQ_SIM_MS,
                   100.0 * (mean - (double)VQ_SIM_MS) / (double)VQ_SIM_MS);

            /* NEON comparison, both measured in this same run */
            double sn = 0.0;
            for (int i = 0; i < n; i++) sn += g_stat[i].t_vq;
            sn /= (double)n;
            /* --- cache split and flush test --- */
            {
                double sf=0, snf=0, cf=0, cnf=0, bf=0, bnf=0;
                int nf=0, nnf=0; long badf=0, badnf=0;
                for (int i = 0; i < n; i++) {
                    if (g_stat[i].t_vq_pl < 0.0) continue;
                    if (g_stat[i].pl_flushed) {
                        sf += g_stat[i].t_vq_pl; cf += g_stat[i].t_vq_pl_cache;
                        bf += g_stat[i].t_vq_pl_block; nf++;
                        if (g_stat[i].pl_mismatch > 0) badf += g_stat[i].pl_mismatch;
                    } else {
                        snf += g_stat[i].t_vq_pl; cnf += g_stat[i].t_vq_pl_cache;
                        bnf += g_stat[i].t_vq_pl_block; nnf++;
                        if (g_stat[i].pl_mismatch > 0) badnf += g_stat[i].pl_mismatch;
                    }
                }
                printf("[PLVQ] ---- cache split and flush test ----\n");
                if (nf > 0)
                    printf("[PLVQ]  flush ON  n=%d cache %.4f block %.4f total %.4f ms  mism %ld\n",
                           nf, cf/nf, bf/nf, sf/nf, badf);
                if (nnf > 0)
                    printf("[PLVQ]  flush OFF n=%d cache %.4f block %.4f total %.4f ms  mism %ld\n",
                           nnf, cnf/nnf, bnf/nnf, snf/nnf, badnf);
                if (nf > 0 && nnf > 0) {
                    printf("[PLVQ]  flush costs %.4f ms/frame\n", (sf/nf) - (snf/nnf));
                    if (badnf == 0)
                        printf("[PLVQ]  VERDICT: unflushed frames bit-exact -> flush NOT needed.\n");
                    else
                        printf("[PLVQ]  VERDICT: unflushed frames MISMATCH -> flush IS required.\n");
                }
                printf("[PLVQ]  block = accelerator time; cache = host work.\n");
            }
            printf("[PLVQ] NEON T_VQ this run    : %.4f ms -> speedup %.2fx\n",
                   sn, (mean > 0.0) ? sn / mean : 0.0);

            if (bad_total != 0)
                printf("[PLVQ] *** MISMATCHES PRESENT -- the timing above is NOT a result ***\n");
            printf("[PLVQ] ===== end PL VQ =====\n");
        }
    } else {
        printf("\n[PLVQ] PL VQ not available this run; no silicon T_VQ measured.\n");
    }
#endif

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
#if EDGE_POWER
    /* LAST. Minutes of wall time and I2C traffic -- must never precede
     * any timed bracket. */
    edge_power_measure(&vq);
#endif
    printf("[EDGE] done\n");
    return 0;
}
