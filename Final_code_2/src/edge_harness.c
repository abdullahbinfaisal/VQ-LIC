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

/* EDGE_USE_PW_VQ -- VQ on the SHARED pointwise engine, the live path since
 * commit 62cbfbc. Mutually exclusive with EDGE_USE_PL_VQ: they drive
 * different blocks, and only one of those blocks exists in any given
 * bitstream.
 *
 * This define was MISSING until the first Vitis build caught it, while six
 * #if EDGE_USE_PW_VQ blocks tested it -- so every one of them evaluated to 0
 * and the entire PW VQ integration was silently compiled out. An undefined
 * macro in #if is 0, not an error, which is why nothing complained. */
#ifndef EDGE_USE_PW_VQ
#define EDGE_USE_PW_VQ 1
#endif
#if EDGE_USE_PL_VQ && EDGE_USE_PW_VQ
#  error "EDGE_USE_PL_VQ and EDGE_USE_PW_VQ are mutually exclusive -- the dedicated VQ block and the PW-hosted VQ cannot both be present."
#endif

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "ff.h"
#include "xil_cache.h"
#include "edge_pipeline.h"
#include "vq_pq.h"
#include "range_coder.h"
/* Always compiled in: st_mark() is a timer read and four stores, and
 * st_enable(0) makes it a no-op. */
#include "stage_trace.h"
#if EDGE_USE_PW_VQ
#include "vq_pw_pl.h"
#endif
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
/* T_VQ for the DEDICATED engine at QUERY_LANES=2. RTL SIMULATION, not
 * silicon: 1,843,339 cycles at 100 MHz.
 *
 * LEGACY ONLY. That block is absent from the PW-hosted bitstream, and the
 * [PAIR] report no longer prints these under EDGE_USE_PW_VQ -- the shared
 * engine has measured numbers now. Kept so the EDGE_USE_PL_VQ build still
 * reproduces what it reported before. */
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
/* EDGE_PIPELINED -- run the measured loop through edge_one_pipelined(), which
 * overlaps the VQ with the next frame's CPU-side input preparation. Default 0
 * so the serial numbers already in the record remain the default output; set
 * to 1 to measure the pipelined topology. Both emit the same stage trace, so
 * #STPIPE makes the difference visible rather than asserted. */
#ifndef EDGE_PIPELINED
#define EDGE_PIPELINED 0
#endif

/* EDGE_PIPELINED_AB -- run the pipelined loop as a SECOND timed pass after
 * the serial one, in the same session, on the same board, at the same
 * temperature, and report both interval-per-frame figures. The serial pass
 * keeps the CSV and every STAT line it has always emitted, so nothing in
 * the existing record changes; the pipelined pass reports only its II and
 * its hiding accounting. Running them together is the point: an overlap
 * claim compared against a number from a different run is not an A/B. */
#ifndef EDGE_PIPELINED_AB
#define EDGE_PIPELINED_AB 1
#endif
#define EDGE_PIPE_ANY (EDGE_USE_PW_VQ && (EDGE_PIPELINED || EDGE_PIPELINED_AB))

/* Symbols coded per slice inside a DMA wait. 64 symbols is ~2.9 us at the
 * measured 45.8 ns/symbol: long enough that the indirect call is noise,
 * short enough that the MM2S loop's S2MM re-arm check is never delayed by
 * more than that. Larger slices hide no more work -- the window is 2.6x
 * bigger than the job -- they only add latency to the cascade's polling. */
#ifndef EDGE_BG_SLICE_SYMS
#define EDGE_BG_SLICE_SYMS 64
#endif

/* Frames in the pipelined pass. Fewer than the serial pass: the quantity
 * being measured is a wall-clock interval per frame with a cv of ~0.01%,
 * so 40 is already far past the point where more frames buy precision. */
#ifndef EDGE_PIPE_FRAMES
#define EDGE_PIPE_FRAMES 40
#endif

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
#if EDGE_USE_PL_VQ || EDGE_USE_PW_VQ
/* 32-byte aligned: the coder now reads one of these while the VQ's S2MM
 * writes the other, so they must not share a cache line with each other. */
static uint8_t         g_pl_idx[VQ_IDX_BYTES] __attribute__((aligned(32)));
static int             g_pl_ok = 0;      /* set only after the block identifies */
#endif
#if EDGE_USE_PW_VQ
/* The PW-hosted search uses a DIFFERENT codebook geometry from the dedicated
 * engine: M=8 K=16 Dsub=8 (1,024 int8) against M=4 K=256 Dsub=16 (16,384).
 * edge_codebook_ptr() returns the latter, so it cannot be reused here.
 *
 * SYNTHETIC, like every other codebook in this build. The deployed trained
 * codebook is unavailable (PAPER_HW_EVIDENCE.md sec 21), so this is a
 * deterministic xorshift fill -- the same construction gen_vq_vectors.c uses
 * for the RTL benches, which keeps firmware and simulation on comparable
 * data. It is fine for timing and for reference-exactness, and it is NOT a
 * basis for any rate or distortion claim.
 *
 * PRE-CENTRED int8, i.e. v = cq - 128 already applied, which is the form
 * vqpw_init() expects and the only form the signed weight port can carry. */
static int8_t          g_pw_cb[VQPW_M * VQPW_K * VQPW_DSUB];

/* Index ping-pong. The VQ's S2MM writes one buffer while the PS consumes the
 * other, so a consumer of frame n-1's indices can run while frame n's search
 * is still in flight. g_pl_idx is the second half of the pair.
 *
 * Only edge_one_pipelined() uses these, so they are compiled with it --
 * otherwise this is 57.6 KB of BSS reserved for a path that is switched
 * off, on a board where the harness already holds >1 MB of static index
 * storage. */
#if EDGE_PIPE_ANY
static uint8_t         g_pw_idx_b[VQ_IDX_BYTES] __attribute__((aligned(32)));
static uint8_t        *g_idx_cur  = 0;   /* the search is writing this      */
static uint8_t        *g_idx_prev = 0;   /* complete, safe for the PS       */
#endif

#if EDGE_PIPE_ANY
/* ---- entropy coding as background work -------------------------------
 * Registered with the cascade for the duration of the analysis, so the
 * coder advances inside the DMA spin loops instead of after them. The
 * stream is armed on the PREVIOUS frame's indices, which are complete and
 * which nothing else touches until the ping-pong swaps at end of frame.
 *
 * No timer reads in here. Two register reads per slice, 1,800 slices per
 * frame, would be a measurable tax on the thing being measured -- and an
 * unnecessary one, because rc_stream_coded() gives the same accounting for
 * free by counting symbols instead of cycles. */
extern void cascade_set_bg_work(void (*fn)(void));

static rc_stream_t g_bg_rc;
static int         g_bg_armed = 0;

static void edge_bg_range_slice(void)
{
    if (!g_bg_armed) return;
    if (rc_stream_step(&g_bg_rc, (unsigned long)EDGE_BG_SLICE_SYMS)) g_bg_armed = 0;
}

/* Per-frame hiding accounting, printed after the pass so no printf lands
 * inside a timed bracket. */
#define EDGE_BG_MAXF 128
static unsigned long g_bg_an [EDGE_BG_MAXF];   /* coded during the analysis */
static unsigned long g_bg_srch[EDGE_BG_MAXF];  /* coded under the VQ search */
static unsigned long g_bg_exp[EDGE_BG_MAXF];   /* left over, coded exposed  */
static double        g_bg_exp_ms[EDGE_BG_MAXF];
static size_t        g_bg_bytes[EDGE_BG_MAXF];
static int           g_bg_n = 0;
#endif

static void edge_pw_synth_codebook(void)
{
    uint32_t st = 0x2468ACEu;
    for (size_t i = 0; i < sizeof g_pw_cb; i++) {
        st ^= st << 13; st ^= st >> 17; st ^= st << 5;
        g_pw_cb[i] = (int8_t)(st & 0xFF);
    }
}
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
/* One PIPELINED frame.
 *
 * ---------------------------------------------------------------------------
 * THE OVERLAP THIS FUNCTION USED TO PERFORM IS NO LONGER LEGAL.
 * ---------------------------------------------------------------------------
 * It started the VQ on the PREVIOUS frame's latent and then ran THIS frame's
 * analysis concurrently, on the stated grounds that the two used disjoint
 * hardware: analysis on HP0/HP1, VQ on HP2. That was true of the DEDICATED VQ
 * block. It is false since 62cbfbc -- VQ now runs on the PW engine, the same
 * engine the analysis cascade drives. Starting both would reprogram the engine
 * out from under a search in flight.
 *
 * The DMA ports are still disjoint, and the latent is still genuinely
 * double-buffered by g_edge_parity, so the memory-side reasoning survives. It
 * is the ENGINE that is now shared, and no amount of buffering fixes that.
 *
 * The legal ordering is analysis THEN VQ. What can overlap the VQ is PS-side
 * work that touches neither the PW engine nor the buffers in flight; see
 * edge_one_pipelined() for the measured version. This function keeps the
 * A/B contract (run_vq toggles exactly one thing) for the power sweep.
 */
static const uint8_t *g_pipe_lat = 0;

static int edge_pipe_frame(int run_vq)
{
    if (edge_run_six_pairs(edge_chw_ptr()) != 0) return -1;
    g_pipe_lat = edge_latent_ptr();

#if EDGE_USE_PW_VQ
    if (run_vq && g_pl_ok && g_pipe_lat) {
        /* The analysis just overwrote the weight BRAM the codebook lives in. */
        if (vq_pw_pl_load_codebook(g_pw_cb, 128) == 0) {
            vq_pw_pl_cache_prep(g_pipe_lat, g_pl_idx, 0);
            if (vq_pw_pl_start(g_pipe_lat, g_pl_idx) == 0) {
                int r; unsigned long guard = 0;
                while ((r = vq_pw_pl_poll_done()) == 0 && ++guard < 200000000u) { }
                if (r > 0) vq_pw_pl_finish(g_pl_idx);
            }
        }
    }
#else
    (void)run_vq;
#endif
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
/* ---------------------------------------------------------------------------
 * One SOFTWARE-PIPELINED frame -- the deployed topology once VQ shares the PW
 * engine.
 *
 * WHAT CAN AND CANNOT OVERLAP, and why this ordering is the one that is legal:
 *
 *   analysis(f)  and  VQ(f)      CANNOT overlap. One PW engine. This is the
 *                                hard bound: II >= T_analysis + T_VQ_exposed.
 *   analysis(f+1) and VQ(f)      CANNOT overlap, same reason. Buffering does
 *                                not help; the contended resource is the
 *                                engine, not the memory.
 *   VQ(f) and PS-side work       CAN overlap, provided that work touches
 *                                neither the PW engine nor a buffer in flight.
 *
 * So the VQ is started as soon as the analysis frees the engine, and the next
 * frame's input is prepared on the CPU while the search runs in PL. That input
 * preparation writes edge_chw_ptr(), which frame f's analysis has already
 * finished reading and frame f's VQ never touches -- so it is safe without any
 * further buffering.
 *
 * The latent needs no extra work either: g_edge_parity already alternates the
 * chain buffers per frame, so frame f+1's cascade writes the buffer frame f's
 * VQ is not reading. The INDEX buffers are ping-ponged here (g_idx_cur /
 * g_idx_prev) so a consumer of frame f-1's indices can run while frame f's
 * search is in flight.
 *
 * The overlap is MEASURED, not asserted: ST_PACK is marked inside the
 * ST_VQ_RUN interval, so #STPIPE reports what actually happened. If the CPU
 * work is shorter than the search the residue shows up as exposed VQ time,
 * and if the ordering is ever broken st_check_exclusive() catches it.
 *
 * THE RANGE CODER NOW READS THIS GEOMETRY. It was previously excluded here:
 * rc was fixed at RC_NMODEL = VQ_M = 4 and RC_NSYM = VQ_K = 256, and since
 * both layouts are 4 bytes per position, feeding it the new indices would have
 * RUN and produced a plausible bitstream from a model describing nothing.
 * range_coder.h is now parameterised on RC_GEOMETRY_PW and reads symbols
 * through rc_get_sym(), so frame f-1's indices can be entropy-coded on the CPU
 * while frame f's search runs in PL. That is the largest piece of CPU work
 * available to hide the search behind.
 * ------------------------------------------------------------------------ */
#if EDGE_PIPE_ANY
/* ===========================================================================
 * edge_one_pipelined -- one frame with the entropy coder hidden.
 *
 * WHAT OVERLAPS WHAT, and why those and nothing else.
 *
 * The PW engine is a single resource. The analysis cascade and the VQ search
 * both need it, so they are strictly serial and no amount of buffering changes
 * that (#STEXCL asserts it every run). What CAN overlap is CPU work that
 * touches neither the engine nor a buffer in flight, and on this design there
 * is exactly one such job: entropy coding of the PREVIOUS frame's indices.
 *
 * It gets two windows, and it needs both only in the sense that the first is
 * already more than enough:
 *
 *   window 1  the analysis, 13.4816 ms MEASURED, of which the CPU spends
 *             essentially all of it spinning on DMA status registers. The
 *             coder runs there via the cascade's background hook.
 *   window 2  the VQ search, 2.6428 ms MEASURED, the CPU spinning on the
 *             accelerator's done flag. The coder runs there in this function's
 *             own poll loop.
 *
 * Entropy coding is 5.275 ms MEASURED, against 16.12 ms of window. So the
 * expectation is that it disappears entirely and the frame period falls to
 * what the engine alone dictates. The accounting below reports symbols coded
 * in each window and the exposed remainder, so if it does NOT disappear the
 * numbers say where it went rather than leaving the claim to arithmetic.
 *
 * WHAT IS NOT OVERLAPPED, deliberately:
 *   - the codebook reload. It writes the same weight BRAM the cascade just
 *     used and the search is about to read. It is the price of sharing the
 *     engine and it is measured, not hidden.
 *   - input preparation. edge_prepare2() already runs it outside the frame in
 *     the measured loop, so crediting it here would be counting it twice. An
 *     earlier version of this function did exactly that.
 * ========================================================================= */
static int edge_one_pipelined(int frame_id, int next_seed,
                              const rc_models_t *M, ep_frame_stat_t *st)
{
    memset(st, 0, sizeof(*st));
    st->frame_id = frame_id;
    st->pl_mismatch = -1;
    st->pl_flushed  = 0;
    st->t_range     = 0.0;

    if (!g_idx_cur) { g_idx_cur = g_pl_idx; g_idx_prev = 0; }

    edge_reset_accums();
    st_set_frame((uint32_t)frame_id);
    ST_BEGIN_S(ST_FRAME);

    /* ---- arm the coder on the previous frame's indices ------------------
     * g_idx_prev is 0 on the first frame of a pass: there is no previous
     * frame, so that one frame legitimately codes nothing. It is excluded
     * from the reported mean rather than averaged in as a fast frame. */
    const int have_prev = (g_idx_prev != 0);
    if (have_prev) {
        rc_stream_start(&g_bg_rc, M, g_idx_prev, g_bs, sizeof g_bs);
        g_bg_armed = 1;
        cascade_set_bg_work(edge_bg_range_slice);
    }

    /* ---- analysis. The engine is busy for this whole interval; the CPU is
     * not, and the hook above is spending it. --------------------------- */
    const unsigned long long t0 = NOW();
    const int an_rc = edge_run_six_pairs(edge_chw_ptr());
    const unsigned long long t_an = NOW();

    /* Unregister BEFORE anything else can call into the cascade. Leaving a
     * stale callback armed would let the coder run inside a later frame's
     * analysis against a buffer this frame is about to overwrite. */
    cascade_set_bg_work(0);
    if (an_rc != 0) { g_bg_armed = 0; return -1; }

    const unsigned long syms_an = have_prev ? rc_stream_coded(&g_bg_rc) : 0ul;
    unsigned long syms_srch = 0ul;

    if (g_pl_ok) {
        /* ---- codebook reload: overlaps nothing, by construction --------- */
        ST_BEGIN_S(ST_VQ_PROG);
        const int cb_ok = vq_pw_pl_load_codebook(g_pw_cb, 128);
        ST_END_S(ST_VQ_PROG);

        if (cb_ok == 0) {
            ST_BEGIN_S(ST_VQ_RUN);
            vq_pw_pl_cache_prep(edge_latent_ptr(), g_idx_cur, 0);
            const int started = (vq_pw_pl_start(edge_latent_ptr(), g_idx_cur) == 0);

            /* ---- window 2: code while the search runs ------------------
             * The search writes g_idx_cur by DMA; the coder reads g_idx_prev.
             * Disjoint buffers, and the engine is untouched either way.
             *
             * poll_done() is read every slice rather than every symbol so the
             * AXI-Lite reads stay a rounding error, and the loop exits on
             * whichever finishes first. */
            int r = 0;
            unsigned long long guard = 0;
            if (started) {
                while (r == 0 && ++guard < 200000000u) {
                    if (g_bg_armed) {
                        if (rc_stream_step(&g_bg_rc, (unsigned long)EDGE_BG_SLICE_SYMS))
                            g_bg_armed = 0;
                    }
                    r = vq_pw_pl_poll_done();
                }
                if (r > 0) vq_pw_pl_finish(g_idx_cur);
            }
            ST_END_S(ST_VQ_RUN);

            syms_srch = have_prev ? (rc_stream_coded(&g_bg_rc) - syms_an) : 0ul;

            st->t_vq_pw_prog  = vq_pw_pl_last_prog_ms();
            st->t_vq_pl_block = vq_pw_pl_last_run_ms();
            st->t_vq_pl       = st->t_vq_pw_prog + st->t_vq_pl_block;

            /* ping-pong: what the search just wrote becomes next frame's
             * coding input. Only on a completed search -- handing the coder a
             * partially-written buffer would produce a clean-looking bitstream
             * of nothing in particular. */
            if (r > 0) {
                uint8_t *tmp = g_idx_prev ? g_idx_prev : g_pw_idx_b;
                g_idx_prev = g_idx_cur;
                g_idx_cur  = tmp;
            }
        }
    }

    /* ---- whatever the two windows did not absorb is EXPOSED -------------
     * This bracket is the entropy cost the frame actually pays. If the
     * windows were big enough it contains only the 4-byte flush. */
    if (have_prev) {
        ST_BEGIN_S(ST_RANGE);
        const unsigned long long r0 = NOW();
        const size_t nb = rc_stream_finish(&g_bg_rc);
        const unsigned long long r1 = NOW();
        ST_END_S(ST_RANGE);
        g_bg_armed = 0;

        st->t_range     = MS(r0, r1);
        st->range_bytes = nb;
        st->range_bits  = (double)nb * 8.0;
        st->range_bpp   = st->range_bits / ((double)EP_W * (double)EP_H);

        if (g_bg_n < EDGE_BG_MAXF) {
            const unsigned long tot = (unsigned long)RC_NSYM_PER_FRAME;
            const unsigned long got = syms_an + syms_srch;
            g_bg_an  [g_bg_n] = syms_an;
            g_bg_srch[g_bg_n] = syms_srch;
            g_bg_exp [g_bg_n] = (tot > got) ? (tot - got) : 0ul;
            g_bg_exp_ms[g_bg_n] = st->t_range;
            g_bg_bytes[g_bg_n]  = nb;
            g_bg_n++;
        }
    }

    (void)next_seed;
    ST_END_S(ST_FRAME);

    const unsigned long long t1 = NOW();
    st->t_pl          = MS(t0, t_an);
    st->t_vq          = st->t_vq_pl;
    st->t_edge_direct = MS(t0, t1);
    st->t_edge_sum    = st->t_pl + st->t_vq + st->t_range;

    double pack, prog, cache, pl, total;
    edge_read_accums(&pack, &prog, &cache, &pl, &total);
    st->t_pack = pack; st->t_prog = prog; st->t_cache = cache;
    st->t_host = total;
    st->fixed_bpp = ((double)VQPW_IDX_BYTES * 8.0) / ((double)EP_W * (double)EP_H);
    return 0;
}


/* Print the hiding accounting for a pipelined pass. Outside every timed
 * bracket by construction: nothing here runs until the pass is over. */
static void edge_bg_report(double ii_ms, int nframes, double t_range_serial)
{
    if (g_bg_n <= 0) { printf("#PIPE,no frames with a previous frame to code\n"); return; }

    double an = 0.0, sr = 0.0, ex = 0.0, ms = 0.0, by = 0.0;
    double ex_max = 0.0;
    for (int i = 0; i < g_bg_n; i++) {
        an += (double)g_bg_an[i];
        sr += (double)g_bg_srch[i];
        ex += (double)g_bg_exp[i];
        ms += g_bg_exp_ms[i];
        by += (double)g_bg_bytes[i];
        if (g_bg_exp_ms[i] > ex_max) ex_max = g_bg_exp_ms[i];
    }
    const double n   = (double)g_bg_n;
    const double tot = (double)RC_NSYM_PER_FRAME;

    printf("#PIPE,frames,%d,II_ms,%.4f,fps,%.2f\n", nframes, ii_ms, 1000.0 / ii_ms);
    printf("#PIPE,symbols_per_frame,%lu\n", (unsigned long)RC_NSYM_PER_FRAME);
    printf("#PIPE,coded_in_analysis,%.1f,%.2f%%\n", an / n, 100.0 * an / (n * tot));
    printf("#PIPE,coded_under_search,%.1f,%.2f%%\n", sr / n, 100.0 * sr / (n * tot));
    printf("#PIPE,coded_exposed,%.1f,%.2f%%\n", ex / n, 100.0 * ex / (n * tot));
    printf("#PIPE,exposed_ms_mean,%.4f,max,%.4f\n", ms / n, ex_max);
    printf("#PIPE,range_bytes_mean,%.1f,bpp,%.5f\n",
           by / n, (by / n) * 8.0 / ((double)EP_W * (double)EP_H));

    /* The claim, stated as a subtraction against a number measured in THIS
     * session by the serial pass, not against a remembered one. */
    if (t_range_serial > 0.0) {
        const double hidden = t_range_serial - (ms / n);
        printf("#PIPE,serial_range_ms,%.4f,exposed_ms,%.4f,hidden_ms,%.4f,%.1f%%\n",
               t_range_serial, ms / n, hidden, 100.0 * hidden / t_range_serial);
    }
    printf("#PIPE,NOTE,II is wall time over the pass divided by frames; it is\n");
    printf("#PIPE,NOTE,not a sum of stages and does not assume they are disjoint\n");
}
#endif /* EDGE_PIPE_ANY */

static int edge_one(int frame_id, const vq_pq_ctx_t *vq, const rc_models_t *M,
                    int do_roundtrip, ep_frame_stat_t *st)
{
    memset(st, 0, sizeof(*st));
    st->frame_id = frame_id;

    edge_reset_accums();

    /* Absolute stage boundaries for the pipeline figure. These are timestamps,
     * not durations: only timestamps can answer whether frame n+1's DW work
     * overlapped frame n's VQ. See stage_trace.h. */
    st_set_frame((uint32_t)frame_id);
    ST_BEGIN_S(ST_FRAME);

    unsigned long long t_e0 = NOW();
    if (edge_run_six_pairs(edge_chw_ptr()) != 0) return -1;   // == T_HOST
    unsigned long long t_h1 = NOW();

    vq_pq_encode_frame(vq, edge_latent_ptr(), g_idx);         // == T_VQ
    unsigned long long t_v1 = NOW();

    /* GEOMETRY GUARD. g_idx holds the NEON path's M=4/K=256 indices, one byte
     * per symbol. With RC_GEOMETRY_PW=1 the coder reads 4-bit nibbles, so it
     * would histogram and code pairs of sub-codeword indices as if they were
     * codewords: it RUNS, emits a plausible bitstream, and means nothing. The
     * two are only compatible when the coder is built for the legacy layout.
     * The pipelined path is where the PW indices are coded correctly. */
    size_t nb;
#if RC_GEOMETRY_PW
    nb = 0;
    (void)M;
#else
    ST_BEGIN_S(ST_RANGE);
    nb = rc_encode_frame(M, g_idx, g_bs, sizeof g_bs);        // == T_RANGE
    ST_END_S(ST_RANGE);
#endif
    unsigned long long t_e1 = NOW();

#if !RC_GEOMETRY_PW
    /* nb == 0 means the coder overflowed its buffer. Under RC_GEOMETRY_PW
     * it is the deliberate skip above, not a failure. */
    if (nb == 0) { printf("[EDGE] f%d range overflow\n", frame_id); return -1; }
#endif

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
#if EDGE_USE_PW_VQ
    if (g_pl_ok) {
        /* The codebook reload is NOT optional per frame: it lives in the same
         * weight BRAM the analysis transform just used, so the preceding
         * convolution overwrote it. Timed separately because it is a real
         * cost of sharing the engine, not accelerator work. */
        ST_BEGIN_S(ST_VQ_PROG);
        const int cb_ok = vq_pw_pl_load_codebook(g_pw_cb, 128);
        ST_END_S(ST_VQ_PROG);

        if (cb_ok == 0) {
            const unsigned long long p0 = NOW();
            vq_pw_pl_cache_prep(edge_latent_ptr(), g_pl_idx, 0);
            const unsigned long long pc = NOW();

            ST_BEGIN_S(ST_VQ_RUN);
            if (vq_pw_pl_start(edge_latent_ptr(), g_pl_idx) == 0) {
                int r; unsigned long long guard = 0;
                while ((r = vq_pw_pl_poll_done()) == 0 && ++guard < 200000000u) { }
                if (r > 0) vq_pw_pl_finish(g_pl_idx);
            }
            ST_END_S(ST_VQ_RUN);

            const unsigned long long p1 = NOW();
            st->pl_flushed    = 0;
            st->t_vq_pl_cache = MS(p0, pc);
            st->t_vq_pl_block = MS(pc, p1);
            st->t_vq_pl       = MS(p0, p1);
            st->t_vq_pw_prog  = vq_pw_pl_last_prog_ms();

            /* Checked on EVERY timed frame against the NEON reference, so a
             * divergence cannot hide behind a throughput number. Note the two
             * paths use different codebooks (M=8/K=16 vs M=4/K=256), so this
             * compares index STREAMS only where the harness has been told they
             * should agree; with differing geometries it is a liveness check,
             * not an equivalence one. */
            st->pl_mismatch = -1;

            /* Entropy-code the indices the PW search just produced. Without
             * this a default run reports no T_RANGE at all: edge_one's own
             * range coding is compiled out under RC_GEOMETRY_PW because it
             * would feed the coder the NEON path's old-format bytes. These
             * indices are the right geometry, so they can be coded and timed.
             *
             * Outside the t_e0..t_e1 bracket by construction -- that bracket
             * is the legacy serial measurement and must keep its meaning --
             * but inside the stage trace, so #STSUM carries it. */
#if RC_GEOMETRY_PW
            ST_BEGIN_S(ST_RANGE);
            const unsigned long long r0 = NOW();
            const size_t rn = rc_encode_frame(M, g_pl_idx, g_bs, sizeof g_bs);
            /* Feed the shared nb, or the reporting block below overwrites
             * every one of these fields with the legacy path's skipped
             * values -- which is what made the CSV read range_bytes=0 and
             * t_range=0.0002 ms while #STSUM carried the real 5.27 ms. */
            nb = rn;
            const unsigned long long r1 = NOW();
            ST_END_S(ST_RANGE);
            st->t_range     = MS(r0, r1);
            st->range_bytes = rn;
            st->range_bits  = (double)rn * 8.0;
            st->range_bpp   = st->range_bits / ((double)EP_W * (double)EP_H);
            if (rn == 0)
                printf("[EDGE] f%d PW range overflow\n", frame_id);
#endif
        }
    }
#endif
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

    /* Closes the frame's span. Everything after this point is outside every
     * timed region, so the trace must not extend past it. */
    ST_END_S(ST_FRAME);

    st->t_edge_direct = MS(t_e0, t_e1);
    st->t_vq          = MS(t_h1, t_v1);
#if RC_GEOMETRY_PW && EDGE_USE_PW_VQ
    /* Already set from the PW block's own bracket. MS(t_v1, t_e1) measures
     * the deliberately skipped legacy encode, i.e. nothing. */
#else
    st->t_range       = MS(t_v1, t_e1);
#endif

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
    /* Report on the indices that were actually encoded. Under the PW
     * geometry that is g_pl_idx (M=8/K=16 nibbles); g_idx holds the NEON
     * path's M=4/K=256 bytes, and measuring one against the other gave a
     * guaranteed 57,600 mismatches per frame -- 4,608,000 in the summary,
     * which reads as a correctness failure and was purely an aliasing bug. */
#if RC_GEOMETRY_PW && EDGE_USE_PW_VQ
    const uint8_t *rc_src = g_pl_idx;
    const long     rc_len = (long)VQPW_IDX_BYTES;
#else
    const uint8_t *rc_src = g_idx;
    const long     rc_len = (long)VQ_IDX_BYTES;
#endif
    rc_frame_entropy(rc_src, st->h_emp);
    st->rc_mismatch = -1; st->rc_first_bad = -1;
    /* nb == 0 means nothing was encoded (no PL, or an overflow). Decoding a
     * zero-length stream and diffing it is not a failing round trip, it is
     * an absent one: leave rc_mismatch at -1 so the summary says so. */
    if (do_roundtrip && nb > 0) {
        rc_decode_frame(M, g_bs, nb, g_rt);
        long bad = 0, first = -1;
        for (long i = 0; i < rc_len; i++)
            if (g_rt[i] != rc_src[i]) { if (first < 0) first = i; bad++; }
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

    /* ---- VQ bring-up runs BEFORE calibration -----------------------------
     * ORDER IS LOAD-BEARING, not cosmetic. The calibration loop below builds
     * the entropy model from the PW search's own indices, but only when
     * g_pl_ok is set -- and g_pl_ok is set by the bring-up. Until 2026-09-05
     * the bring-up sat AFTER calibration, so the loop silently took its
     * memset(0) fallback for all 20 frames. The resulting model gave symbol 0
     * 65521/65536 and every other symbol 1/65536: 0.0040 bits/sym, 16 bits per
     * real symbol, 216 KB per frame against a 119 KB buffer. Every timed frame
     * printed "PW range overflow" and T_RANGE was never measured.
     *
     * The bring-up verifies against edge_latent_ptr(), so one analysis pass
     * runs first to put a real latent there rather than power-on contents.
     * Frame 1 is prepared twice as a result; it is outside every timed
     * bracket, so the only cost is a few ms of start-up. */
    double ms_load, ms_deint;
    if (edge_prepare2(1, layout, use_synth, &ms_load, &ms_deint) != 0) return -1;
    edge_reset_accums();
    if (edge_run_six_pairs(edge_chw_ptr()) != 0) return -1;

#if EDGE_USE_PW_VQ
    printf("[EDGE] ---- PW-hosted VQ bring-up ----\n");
    edge_pw_synth_codebook();
    if (vq_pw_pl_init() != 0) {
        printf("[EDGE] PW VQ unavailable -> VQ columns will be blank.\n");
        printf("[EDGE] The bitstream must be post-62cbfbc, built with\n");
        printf("[EDGE] USE_PW_VQ=1 -- check the synth log really bound 1.\n");
        g_pl_ok = 0;
    } else {
        long first = -1;
        const long bad = vq_pw_pl_verify(g_pw_cb, 128, edge_latent_ptr(),
                                         g_pl_idx, g_rt, &first);
        printf("[EDGE] PW VQ vs vqpw_encode_frame(): %ld mismatches of %d, first at %ld -> %s\n",
               bad, VQPW_IDX_BYTES, first, (bad == 0) ? "PASS" : "FAIL");
        printf("[EDGE] codebook reload %.4f ms, search %.4f ms\n",
               vq_pw_pl_last_prog_ms(), vq_pw_pl_last_run_ms());
        if (bad != 0) {
            printf("[EDGE] ABORT: PW VQ is not reference-exact. No VQ timing will\n");
            printf("[EDGE] be reported -- a fast wrong answer is not a result.\n");
            edge_set_quiet(0);
            return -1;
        }
        g_pl_ok = 1;
    }
#endif

    // ---- calibration pass: frames 1..EDGE_NCAL -----------------------------
    printf("[EDGE] calibration over frames 1..%d (model build timed separately)\n",
           EDGE_NCAL);
    for (int i = 0; i < EDGE_NCAL; i++) {
        if (edge_prepare2(i + 1, layout, use_synth, &ms_load, &ms_deint) != 0) return -1;
        edge_reset_accums();
        if (edge_run_six_pairs(edge_chw_ptr()) != 0) return -1;
#if RC_GEOMETRY_PW && EDGE_USE_PW_VQ
        /* The entropy model must be trained on the SAME index geometry the
         * coder will see. rc_model_build reads through rc_get_sym, so feeding
         * it the NEON path's M=4/K=256 bytes while RC_GEOMETRY_PW=1 would
         * histogram nibble pairs as if they were codewords and produce a model
         * that describes nothing. Calibrate from the PW search itself. */
        if (g_pl_ok && vq_pw_pl_load_codebook(g_pw_cb, 128) == 0)
            vq_pw_pl_encode_frame(edge_latent_ptr(), g_cal_store[i]);
        else
            memset(g_cal_store[i], 0, VQ_IDX_BYTES);
#else
        vq_pq_encode_frame(&vq, edge_latent_ptr(), g_cal_store[i]);
#endif
        g_cal_idx[i] = g_cal_store[i];
    }
#if RC_GEOMETRY_PW && EDGE_USE_PW_VQ
    /* A model built on all-zero indices is not obviously wrong at build time:
     * it succeeds, reports a plausible-looking entropy, and only fails later
     * as a per-frame overflow with no stated cause. Check the input instead. */
    {
        int nz = 0;
        for (int i = 0; i < EDGE_NCAL; i++)
            for (long b = 0; b < (long)VQPW_IDX_BYTES; b++)
                if (g_cal_store[i][b]) { nz++; break; }
        if (nz == 0) {
            printf("[EDGE] ABORT: all %d calibration frames have all-zero indices.\n",
                   EDGE_NCAL);
            printf("[EDGE] The entropy model would be degenerate and every frame\n");
            printf("[EDGE] would overflow the bitstream buffer. Check that the PW\n");
            printf("[EDGE] VQ bring-up ran and set g_pl_ok before this loop.\n");
            edge_set_quiet(0);
            return -1;
        }
    }
#endif
    ep_build_model(&M, (const uint8_t *const *)g_cal_idx, EDGE_NCAL, &t_model_build);
    double Hm[RC_NMODEL];
    rc_model_entropy(&M, Hm);
    printf("[EDGE] model built in %.4f ms (NOT counted in T_RANGE)\n", t_model_build);
    /* RC_NMODEL is 8 under RC_GEOMETRY_PW, so the fixed four-value print
     * hid half the models -- including the fact that they were identical. */
    printf("[EDGE] model entropy per codebook (%d models of %d symbols):",
           RC_NMODEL, RC_NSYM);
    for (int m = 0; m < RC_NMODEL; m++) printf(" %.4f", Hm[m]);
    printf(" bits/sym\n");

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
        /* NOT EDGE_FIRST_TIMED + w. The stage trace buckets by frame id, so
         * warming on 21..23 and then timing 21..23 merged each pair into one
         * bucket: those three #STSUM rows came out at exactly 2x every stage
         * and #STPIPE reported a 1,509 ms overlap that does not exist. */
        int fid = EDGE_FIRST_TIMED - EDGE_NWARM + w;   /* 18, 19, 20 */
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
#if EDGE_PIPELINED && EDGE_USE_PW_VQ
        /* Deployed topology: VQ on the shared engine, next frame's input
         * prepared on the CPU underneath it. The serial edge_one() path is
         * left intact so the previously reported numbers stay reproducible
         * from the same source -- switch with EDGE_PIPELINED. */
        if (edge_one_pipelined(fid, fid + 1, &M, &g_stat[n]) != 0) return -1;
#else
        if (edge_one(fid, &vq, &M, 1, &g_stat[n]) != 0) return -1;
#endif
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

    /* ---- stage trace: the pipeline figure's raw data ---------------------- */
    printf("#STPASS,serial\n");
    st_print_frame_summary();
    st_print_pipeline();
    st_print_hiding();      /* how much of the VQ the CPU work actually hid */
    st_check_exclusive();
    if (st_overflowed())
        printf("[EDGE] WARNING: stage trace overflowed, raise ST_MAX_EVENTS\n");

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

#if EDGE_USE_PW_VQ
            /* The dedicated VQ block is not in this bitstream. Its trailing-read
             * overlap and its stream-fusion question were both about two engines
             * running at once; there is one engine now, and #STEXCL confirms the
             * two phases never overlap. Printing the old analysis here would be
             * describing hardware that was removed at 62cbfbc. */
            printf("\n[PAIR] ---- VQ on the SHARED PW engine ----\n");
            printf("[PAIR]   T_analysis (SILICON, measured) = %.4f ms\n", tot_ms);
            printf("[PAIR]   T_pair%d    (SILICON, measured) = %.4f ms\n", npairs, m6);
            printf("[PAIR]   T_reload   (SILICON, measured) = %.4f ms\n",
                   vq_pw_pl_last_prog_ms());
            printf("[PAIR]   T_VQ       (SILICON, measured) = %.4f ms\n",
                   vq_pw_pl_last_run_ms());
            printf("[PAIR]   T_frame = T_analysis + T_reload + T_VQ = %.4f ms\n",
                   tot_ms + vq_pw_pl_last_prog_ms() + vq_pw_pl_last_run_ms());
            printf("[PAIR]   No trailing-read subtraction: the search cannot start\n");
            printf("[PAIR]   until the cascade has released the engine, so the final\n");
            printf("[PAIR]   pair does not overlap it. This is measurement, not a\n");
            printf("[PAIR]   model -- every term above came off the board.\n");

            printf("\n[PAIR] ---- direct PW->VQ stream fusion ----\n");
            printf("[PAIR]   pair%d producer rate : %.1f cycles/group\n", npairs, prod);
            printf("[PAIR]   NOT APPLICABLE. Producer and consumer are the same\n");
            printf("[PAIR]   engine. Streaming pair %d into the search would need the\n", npairs);
            printf("[PAIR]   engine to hold the layer weights and the codebook at\n");
            printf("[PAIR]   once, in the one weight BRAM they contend for -- which\n");
            printf("[PAIR]   is the resource the sharing gave up. The %.4f ms reload\n",
                   vq_pw_pl_last_prog_ms());
            printf("[PAIR]   per frame is the price of that contention.\n");
#else
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
#endif
        }
        printf("[PAIR] ===== end per-pair report =====\n");
    }

    /* sum-vs-direct cross-check.
     *
     * T_EDGE_DIRECT is the legacy bracket t_e0..t_e1: analysis plus the NEON
     * VQ. Under the PW geometry the range coding happens AFTER t_e1, on the
     * PW search's indices, so t_edge_sum legitimately exceeds it by exactly
     * T_RANGE. That is a definition, not a discrepancy -- report the residual
     * with the range term removed as well, or the line reads as a 1.5%%
     * accounting failure every run. */
    double d = 0.0, s = 0.0, r = 0.0;
    for (int i = 0; i < n; i++) {
        d += g_stat[i].t_edge_direct;
        s += g_stat[i].t_edge_sum;
        r += g_stat[i].t_range;
    }
    d /= n; s /= n; r /= n;
    printf("XCHK,T_EDGE_DIRECT_mean_ms,%.4f\n", d);
    printf("XCHK,T_EDGE_SUM_mean_ms,%.4f\n", s);
    printf("XCHK,difference_ms,%.4f\n", d - s);
    printf("XCHK,difference_pct,%.3f\n", (s != 0.0) ? 100.0 * (d - s) / s : 0.0);
#if RC_GEOMETRY_PW && EDGE_USE_PW_VQ
    printf("XCHK,T_RANGE_outside_direct_ms,%.4f\n", r);
    printf("XCHK,difference_excl_range_ms,%.4f\n", d - (s - r));
    printf("XCHK,NOTE,range coding runs after the T_EDGE_DIRECT bracket\n");
    printf("XCHK,NOTE,so the raw difference above IS T_RANGE, by construction\n");
#endif
    printf("MODEL,t_model_build_ms,%.4f\n", t_model_build);
#if EDGE_PIPELINED_AB && EDGE_USE_PW_VQ
    /* ======================================================================
     * SECOND TIMED PASS -- identical work, entropy coding hidden.
     *
     * Same board, same session, same temperature, minutes after the serial
     * pass. That matters: an overlap claim is a DIFFERENCE, and a difference
     * against a number from another run is not one.
     *
     * The serial pass above keeps every CSV row and STAT line it has always
     * emitted; nothing already in the record changes. This pass reports only
     * what is new: the interval per frame, and where the entropy symbols got
     * coded.
     *
     * II is wall time over the pass divided by frames. It is not a sum of
     * stages, so it cannot quietly assume the stages are disjoint -- if the
     * overlap does not work, this number does not move.
     * ==================================================================== */
    if (g_pl_ok) {
        double rs = 0.0;
        for (int i = 0; i < n; i++) rs += g_stat[i].t_range;
        rs = (n > 0) ? rs / (double)n : 0.0;

        printf("\n[EDGE] ---- pipelined pass: entropy coding hidden ----\n");
        printf("[EDGE] the serial pass just measured T_RANGE = %.4f ms/frame,\n", rs);
        printf("[EDGE] fully exposed. Target: the same work, none of it exposed.\n");

        st_reset();
        g_bg_n     = 0;
        g_bg_armed = 0;
        g_idx_cur  = 0;      /* fresh ping-pong; frame 1 of the pass has no */
        g_idx_prev = 0;      /* predecessor and therefore codes nothing     */
        edge_set_quiet(1);

        static ep_frame_stat_t pstat;
        int np = 0, ok = 1;

        /* Priming frame: produces the first index set, codes nothing, untimed. */
        if (edge_prepare2(EDGE_FIRST_TIMED, layout, use_synth, &ms_load, &ms_deint) != 0
            || edge_one_pipelined(1000 + EDGE_FIRST_TIMED, 0, &M, &pstat) != 0)
            ok = 0;

        if (ok) {
            /* TWO intervals, because they answer different questions.
             *
             * ii_frame is the frame's own wall time and is what compares with
             * the serial pass's T_EDGE_DIRECT. ii_wall additionally contains
             * edge_prepare2(), i.e. generating the next synthetic input.
             *
             * Keeping them apart is not bookkeeping. ep_synth_frame_planar()
             * has never been measured on this board, it stands in for a camera
             * that does not exist, and folding an unmeasured stand-in into the
             * headline interval would be quoting a frame rate for a pipeline
             * nobody is going to build. The gap between the two is exactly
             * that cost, reported rather than buried. */
            double acc_frame = 0.0;
            const unsigned long long q0 = NOW();
            for (int i = 1; i <= EDGE_PIPE_FRAMES; i++) {
                const int fid = EDGE_FIRST_TIMED + i;
                if (fid > 100) break;
                if (edge_prepare2(fid, layout, use_synth, &ms_load, &ms_deint) != 0) { ok = 0; break; }
                /* +1000 so the stage trace buckets these separately from the
                 * serial pass's frames -- the trace keys on frame id. */
                const unsigned long long f0 = NOW();
                if (edge_one_pipelined(1000 + fid, 0, &M, &pstat) != 0) { ok = 0; break; }
                acc_frame += MS(f0, NOW());
                np++;
            }
            const double ii_wall  = (np > 0) ? MS(q0, NOW()) / (double)np : 0.0;
            const double ii_frame = (np > 0) ? acc_frame / (double)np : 0.0;
            edge_set_quiet(0);
            if (np > 0) {
                edge_bg_report(ii_frame, np, rs);
                printf("#PIPE,II_frame_ms,%.4f,II_wall_ms,%.4f,input_prep_ms,%.4f\n",
                       ii_frame, ii_wall, ii_wall - ii_frame);
                printf("#PIPE,NOTE,II_frame excludes synthetic input generation;\n");
                printf("#PIPE,NOTE,that cost is the difference and is UNMEASURED\n");
                printf("#PIPE,NOTE,elsewhere in this project. Quote II_frame.\n");
            }
        } else {
            edge_set_quiet(0);
        }
        if (!ok) printf("#PIPE,ABORT,the pipelined pass did not complete\n");

        printf("#STPASS,pipelined\n");
        printf("#STPASS,NOTE,ST_RANGE here is the EXPOSED remainder only. The\n");
        printf("#STPASS,NOTE,coding done inside ST_DW_PW and ST_VQ_RUN is not\n");
        printf("#STPASS,NOTE,bracketed as a stage, so #STHIDE understates it --\n");
        printf("#STPASS,NOTE,use the #PIPE symbol counts, which are exact.\n");
        st_print_frame_summary();
        st_print_pipeline();
        st_print_hiding();
        st_check_exclusive();
        if (st_overflowed())
            printf("[EDGE] WARNING: stage trace overflowed in the pipelined pass\n");
    }
#endif /* EDGE_PIPELINED_AB && EDGE_USE_PW_VQ */

#if EDGE_POWER
    /* LAST. Minutes of wall time and I2C traffic -- must never precede
     * any timed bracket. */
    edge_power_measure(&vq);
#endif
    printf("[EDGE] done\n");
    return 0;
}
