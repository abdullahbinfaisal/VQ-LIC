#ifndef PWR_LOG_H
#define PWR_LOG_H
/* ============================================================================
 * pwr_log.h -- board power sampling for energy-per-frame, via
 * PS I2C0 -> PCA9548 ch7 -> three UCD9248 PMBus controllers (ZC702).
 *
 * ------------------------------------------------------------------------
 * WHAT THIS CANNOT DO, AND WHY THE API DOES NOT PRETEND OTHERWISE
 * ------------------------------------------------------------------------
 * PER-STAGE POWER IS NOT OBTAINABLE ON THIS BOARD. One 10-rail scan is about
 * 40 I2C transactions at 100 kHz -- roughly 12 ms. The VQ stage is 2.45 ms
 * and a whole frame is around 16 ms, so a single scan cannot even be
 * contained by the stage it would be attributing. There is no API here that
 * returns "power during VQ", because any such number would be a blend of
 * several stages plus idle.
 *
 * Worse, the UCD9248 filters internally over seconds (main.c documents this
 * from the 2026-08-05 A/B), so even a faster bus would not resolve a
 * millisecond stage.
 *
 * WHAT IS OBTAINABLE, and what the manuscript should quote: a sustained mean
 * power at a KNOWN initiation interval, giving energy per frame as
 *      mJ/frame = mean_W * II_ms
 * The II comes from stage_trace.c (#STPIPE), measured on the same run.
 * pwr_energy_per_frame_mj() does exactly that multiplication and nothing more.
 *
 * SCANNING MUST NOT SIT INSIDE A TIMED BRACKET. 12 ms of I2C stolen from a
 * 16 ms frame would corrupt both the latency numbers and the duty cycle. Run
 * power in its own pass, at a fixed sustained rate, with the latency trace
 * disabled -- or between frames, never within one.
 *
 * UNCERTAINTY. Quote the RUN-TO-RUN spread, not the within-run standard
 * error. Four runs on 2026-08-05 read 1.936 / 1.973 / 1.978 / 2.029 W, a
 * +/-0.05 W spread against a within-run SE of ~0.008 W. pwr_summary_t carries
 * both so the right one can be reported.
 *
 * RELATIONSHIP TO main.c. main.c has its own static PMBus helpers for the
 * ~70 s idle/active A/B. Both drive the same controller, so only one may be
 * initialised in a run. This module is the one to use for ordinary
 * energy-per-frame logging; main.c's RUN_POWER_MEASUREMENT path is the older
 * incremental-power experiment and stays as it is.
 * ==========================================================================*/

#include <stdint.h>

typedef enum { PWR_PL = 0, PWR_PS, PWR_DDR, PWR_MISC, PWR_NGROUP } pwr_group_t;

#define PWR_NRAILS 10

typedef struct {
    unsigned long long t;              /* ARM global timer, scan start       */
    float rail_w[PWR_NRAILS];
    float group_w[PWR_NGROUP];
    float total_w;
    int   ok;                          /* 0 if any rail read failed          */
} pwr_scan_t;

typedef struct {
    int   n;
    float mean_w, sd_w, min_w, max_w;
    float se_w;                        /* within-run standard error          */
    float group_mean_w[PWR_NGROUP];
    float scan_ms;                     /* measured cost of one scan          */
} pwr_summary_t;

/* Bring up I2C0 and select mux channel 7. 0 on success. */
int  pwr_log_init(void);

/* One full 10-rail scan. ~12 ms. NEVER call inside a timed bracket. */
int  pwr_log_scan(pwr_scan_t *out);

/* Accumulate scans for `seconds`, at `interval_ms` between scans, into the
 * internal buffer. Returns the number of scans taken. */
int  pwr_log_collect(unsigned seconds, unsigned interval_ms);

void pwr_log_summary(pwr_summary_t *out);
void pwr_log_print(const pwr_summary_t *s);
const char *pwr_rail_name(int i);

/* Energy per frame from a sustained mean and a MEASURED initiation interval.
 * Both arguments must come from the same run; ii_ms should be the #STPIPE
 * median, not a modelled value. */
double pwr_energy_per_frame_mj(float mean_w, double ii_ms);

#endif /* PWR_LOG_H */
