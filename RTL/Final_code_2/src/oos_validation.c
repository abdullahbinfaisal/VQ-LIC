/* ============================================================================
 * oos_validation.c -- out-of-sample silicon validation of the compute-
 *                     communication service model.
 *
 * WHAT THIS MEASURES
 *   Per architectural block, the PL/DMA service window only:
 *       g_acc_hw_p[p] = t_end - t_hw0
 *   i.e. exactly the interval the fused DW->PW engine owns the AXI DMAs. This
 *   is the SAME boundary the existing per-pair silicon validation uses, so the
 *   controls are directly comparable with everything already reported.
 *
 * WHAT IS EXCLUDED, BY CONSTRUCTION (not by subtraction)
 *   - t_pack  : CPU group-major packing        (g_acc_pack_p)
 *   - t_prog  : DW/PW coefficient programming  (g_acc_prog_p)
 *   - t_cache : cache maintenance              (g_acc_cache_p)
 *   - VQ search and range coding: never invoked from this file at all.
 *   - Image loading and HWC->CHW conversion: one synthetic planar frame is
 *     built ONCE, before any measurement, and reused for every frame of every
 *     candidate. The accelerator's timing is data-independent (it depends only
 *     on tensor shape and DMA/compute rates), so reusing one frame removes the
 *     SD/de-interleave path from the experiment entirely rather than trying to
 *     subtract it afterwards.
 *
 * PROTOCOL
 *   For each candidate: select schedule -> OOS_NWARM warm-up frames (discarded)
 *   -> OOS_NTIMED measured frames. Per-block cycles are read from the silent
 *   accumulators BETWEEN frames, never inside a timed bracket, and differenced
 *   frame-to-frame so per-frame min/max/sd are available.
 *
 * NO MODEL FITTING HAPPENS HERE. This file emits measurements only. The frozen
 * analytical predictions live in results/svc_model.py and are joined offline.
 * ==========================================================================*/
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#define OOS_NWARM    3
#define OOS_NTIMED  80
#define OOS_MAXP     8

/* ---- from main.c ---- */
extern int   edge_select_schedule(int idx);
extern int   surr_sched_count(void);
extern int   surr_sched_current(void);
extern const char *surr_sched_name(int i);
extern int   edge_run_six_pairs(const uint8_t *gm_in_frame);
extern int   edge_num_pairs(void);
extern void  edge_reset_accums(void);
extern void  edge_reset_pair_accums(void);
extern void  edge_read_pair_cycles(unsigned long long *hw, unsigned long long *pack,
                                   unsigned long long *prog, unsigned long long *cache,
                                   int n);
extern void  edge_read_pair_dims(int *cout, int *hout, int *wout, int *groups, int n);
extern void  edge_set_quiet(int q);
extern uint8_t *edge_chw_ptr(void);

/* ---- from edge_pipeline.c ---- */
extern void   ep_synth_frame_planar(uint8_t *dst_chw, int seed);
extern double ep_cycles_to_ms(unsigned long long);

/* The six candidates, as indices into g_surr_scheds[] in main.c. Index 6
 * (the legacy 8-16-64 surrogate) is deliberately NOT measured here. */
static const int  g_oos_idx[]  = { 0, 1, 2, 3, 4, 5 };
static const char *g_oos_role[] = { "CTRL-A", "CTRL-B", "OOS-A", "OOS-B", "OOS-C", "OOS-D" };
#define OOS_NCAND ((int)(sizeof(g_oos_idx) / sizeof(g_oos_idx[0])))

static double oos_sqrt(double x)
{
    if (x <= 0.0) return 0.0;
    double r = x, p = 0.0;
    for (int i = 0; i < 60 && r != p; i++) { p = r; r = 0.5 * (r + x / r); }
    return r;
}

int edge_oos_validation_run(void)
{
    unsigned long long prev[OOS_MAXP], now[OOS_MAXP];
    double sum[OOS_MAXP], sumsq[OOS_MAXP], mn[OOS_MAXP], mx[OOS_MAXP];
    double f_sum = 0.0, f_sumsq = 0.0, f_mn = 0.0, f_mx = 0.0;
    int cout[OOS_MAXP], hout[OOS_MAXP], wout[OOS_MAXP], grp[OOS_MAXP];

    printf("\n================================================================\n");
    printf(" OUT-OF-SAMPLE SERVICE-MODEL VALIDATION\n");
    printf(" one bitstream, one binary, %d candidates, %d warm + %d measured\n",
           OOS_NCAND, OOS_NWARM, OOS_NTIMED);
    printf(" boundary: PL/DMA service window per block (t_end - t_hw0)\n");
    printf(" excluded: pack, coefficient programming, cache, VQ, range coding,\n");
    printf("           image load, HWC->CHW\n");
    printf("================================================================\n");

    /* One synthetic planar frame, built once, reused everywhere. Deterministic,
     * so every candidate sees byte-identical input. */
    ep_synth_frame_planar(edge_chw_ptr(), 12345);
    printf("[OOS] synthetic planar input built once and reused for all frames\n");

    edge_set_quiet(1);

    printf("\n#BLOCKCSV,role,candidate,nblocks,block,cout,hout,wout,groups,"
           "frames,mean_cyc,mean_ms,min_ms,max_ms,sd_ms\n");
    printf("#CANDCSV,role,candidate,nblocks,frames,mean_ms,min_ms,max_ms,sd_ms,"
           "sum_of_blocks_ms\n");

    for (int ci = 0; ci < OOS_NCAND; ci++) {
        const int idx = g_oos_idx[ci];

        printf("\n---------------- [%s] candidate %d/%d ----------------\n",
               g_oos_role[ci], ci + 1, OOS_NCAND);
        if (edge_select_schedule(idx) != 0) {
            printf("[OOS] SKIPPED %s: schedule rejected as not hardware-feasible\n",
                   surr_sched_name(idx));
            continue;
        }

        const int npairs = edge_num_pairs();

        /* ---- warm-up: builds descriptors/blobs, settles DDR and caches ---- */
        for (int i = 0; i < OOS_NWARM; i++) {
            edge_reset_accums();
            if (edge_run_six_pairs(edge_chw_ptr()) != 0) {
                printf("[OOS] ABORT: warm-up frame %d failed\n", i);
                return -1;
            }
        }

        edge_read_pair_dims(cout, hout, wout, grp, OOS_MAXP);
        for (int p = 0; p < OOS_MAXP; p++) {
            sum[p] = sumsq[p] = 0.0; mn[p] = 1e30; mx[p] = -1e30;
        }
        f_sum = f_sumsq = 0.0; f_mn = 1e30; f_mx = -1e30;

        edge_reset_pair_accums();
        edge_read_pair_cycles(prev, 0, 0, 0, OOS_MAXP);

        /* ---- measured frames ---- */
        for (int i = 0; i < OOS_NTIMED; i++) {
            edge_reset_accums();
            if (edge_run_six_pairs(edge_chw_ptr()) != 0) {
                printf("[OOS] ABORT: measured frame %d failed\n", i);
                return -1;
            }
            /* read BETWEEN frames -- never inside a timed bracket */
            edge_read_pair_cycles(now, 0, 0, 0, OOS_MAXP);
            double f_ms = 0.0;
            for (int p = 0; p < npairs; p++) {
                const double ms = ep_cycles_to_ms(now[p] - prev[p]);
                sum[p]   += ms;
                sumsq[p] += ms * ms;
                if (ms < mn[p]) mn[p] = ms;
                if (ms > mx[p]) mx[p] = ms;
                f_ms += ms;
                prev[p] = now[p];
            }
            f_sum   += f_ms;
            f_sumsq += f_ms * f_ms;
            if (f_ms < f_mn) f_mn = f_ms;
            if (f_ms > f_mx) f_mx = f_ms;
        }

        /* ---- report ---- */
        const double n = (double)OOS_NTIMED;
        double blocks_ms = 0.0;
        printf("[OOS] %s  %s  (%d blocks)\n",
               g_oos_role[ci], surr_sched_name(idx), npairs);
        printf("[OOS] blk Cout    HxW    groups     mean ms    min ms    max ms     sd ms\n");
        for (int p = 0; p < npairs; p++) {
            const double mean = sum[p] / n;
            double var = sumsq[p] / n - mean * mean;
            if (var < 0.0) var = 0.0;
            blocks_ms += mean;
            printf("[OOS] %3d %4d %4dx%-4d %7d  %10.4f %9.4f %9.4f %9.5f\n",
                   p + 1, cout[p], hout[p], wout[p], grp[p],
                   mean, mn[p], mx[p], oos_sqrt(var));
            printf("#BLOCKCSV,%s,%s,%d,%d,%d,%d,%d,%d,%d,%.1f,%.6f,%.6f,%.6f,%.6f\n",
                   g_oos_role[ci], surr_sched_name(idx), npairs, p + 1,
                   cout[p], hout[p], wout[p], grp[p], OOS_NTIMED,
                   mean * 1.0e5, mean, mn[p], mx[p], oos_sqrt(var));
        }
        const double fmean = f_sum / n;
        double fvar = f_sumsq / n - fmean * fmean;
        if (fvar < 0.0) fvar = 0.0;
        printf("[OOS] TOTAL T_svc = %.4f ms   (min %.4f  max %.4f  sd %.5f)\n",
               fmean, f_mn, f_mx, oos_sqrt(fvar));
        printf("#CANDCSV,%s,%s,%d,%d,%.6f,%.6f,%.6f,%.6f,%.6f\n",
               g_oos_role[ci], surr_sched_name(idx), npairs, OOS_NTIMED,
               fmean, f_mn, f_mx, oos_sqrt(fvar), blocks_ms);
    }

    /* Leave the board on the deployed schedule so a later run of any other
     * measurement in this binary is unaffected by this sweep. */
    edge_select_schedule(0);
    edge_set_quiet(0);
    printf("\n[OOS] sweep complete; schedule restored to [0] %s\n", surr_sched_name(0));
    printf("[OOS] copy every #BLOCKCSV / #CANDCSV line into results/ for analysis\n");
    return 0;
}
