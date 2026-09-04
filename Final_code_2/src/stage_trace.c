// ============================================================================
// stage_trace.c -- see stage_trace.h.
// ============================================================================
#include "stage_trace.h"
#include <stdio.h>

extern unsigned long long ep_timer_now(void);
extern double ep_cycles_to_ms(unsigned long long c);

static st_event_t s_ev[ST_MAX_EVENTS];
static int        s_n       = 0;
static int        s_ovf     = 0;
static int        s_on      = 1;
static uint32_t   s_frame   = 0;

static const char *st_name[ST_NSTAGE] = {
    "FRAME", "PACK", "PROG", "DW_PW", "VQ_PROG", "VQ_RUN", "RANGE"
};

void st_reset(void)      { s_n = 0; s_ovf = 0; s_on = 1; s_frame = 0; }
void st_set_frame(uint32_t f) { s_frame = f; }
void st_enable(int on)   { s_on = on; }
int  st_count(void)      { return s_n; }
int  st_overflowed(void) { return s_ovf; }

void st_mark(st_stage_t s, uint8_t idx, st_phase_t p)
{
    if (!s_on) return;
    if (s_n >= ST_MAX_EVENTS) { s_ovf = 1; return; }
    s_ev[s_n].frame = s_frame;
    s_ev[s_n].stage = (uint8_t)s;
    s_ev[s_n].idx   = idx;
    s_ev[s_n].phase = (uint8_t)p;
    s_ev[s_n].t     = ep_timer_now();
    s_n++;
}

static double us_since_start(unsigned long long t)
{
    if (s_n == 0) return 0.0;
    return ep_cycles_to_ms(t - s_ev[0].t) * 1000.0;
}

void st_print_csv(void)
{
    printf("#STCSV,frame,stage,idx,phase,t_us\n");
    for (int i = 0; i < s_n; i++)
        printf("#STCSV,%u,%s,%u,%s,%.3f\n",
               (unsigned)s_ev[i].frame,
               st_name[s_ev[i].stage < ST_NSTAGE ? s_ev[i].stage : 0],
               (unsigned)s_ev[i].idx,
               s_ev[i].phase == ST_BEGIN ? "begin" : "end",
               us_since_start(s_ev[i].t));
    if (s_ovf) printf("#STCSV,OVERFLOW,trace truncated at %d events\n", ST_MAX_EVENTS);
}

/* Total time spent inside `stage` during `frame`, summed over all idx. */
static double stage_ms(uint32_t frame, st_stage_t stage)
{
    double tot = 0.0;
    for (int i = 0; i < s_n; i++) {
        if (s_ev[i].frame != frame || s_ev[i].stage != (uint8_t)stage ||
            s_ev[i].phase != ST_BEGIN) continue;
        for (int j = i + 1; j < s_n; j++) {
            if (s_ev[j].stage == s_ev[i].stage && s_ev[j].idx == s_ev[i].idx &&
                s_ev[j].frame == s_ev[i].frame && s_ev[j].phase == ST_END) {
                tot += ep_cycles_to_ms(s_ev[j].t - s_ev[i].t);
                break;
            }
        }
    }
    return tot;
}

static int frame_span(uint32_t frame, unsigned long long *t0, unsigned long long *t1)
{
    int got = 0;
    for (int i = 0; i < s_n; i++) {
        if (s_ev[i].frame != frame) continue;
        if (!got) { *t0 = *t1 = s_ev[i].t; got = 1; }
        if (s_ev[i].t < *t0) *t0 = s_ev[i].t;
        if (s_ev[i].t > *t1) *t1 = s_ev[i].t;
    }
    return got;
}

static uint32_t frame_at(int k)   /* k-th distinct frame id, in order seen */
{
    int seen = 0;
    for (int i = 0; i < s_n; i++) {
        int dup = 0;
        for (int j = 0; j < i; j++) if (s_ev[j].frame == s_ev[i].frame) { dup = 1; break; }
        if (dup) continue;
        if (seen == k) return s_ev[i].frame;
        seen++;
    }
    return 0xFFFFFFFFu;
}

static int nframes(void)
{
    int n = 0;
    for (int i = 0; i < s_n; i++) {
        int dup = 0;
        for (int j = 0; j < i; j++) if (s_ev[j].frame == s_ev[i].frame) { dup = 1; break; }
        if (!dup) n++;
    }
    return n;
}

void st_print_frame_summary(void)
{
    printf("#STSUM,frame,pack_ms,prog_ms,dwpw_ms,vqprog_ms,vqrun_ms,range_ms,frame_ms\n");
    const int nf = nframes();
    for (int k = 0; k < nf; k++) {
        const uint32_t f = frame_at(k);
        unsigned long long a, b;
        const double span = frame_span(f, &a, &b) ? ep_cycles_to_ms(b - a) : 0.0;
        printf("#STSUM,%u,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f\n", (unsigned)f,
               stage_ms(f, ST_PACK), stage_ms(f, ST_PROG), stage_ms(f, ST_DW_PW),
               stage_ms(f, ST_VQ_PROG), stage_ms(f, ST_VQ_RUN),
               stage_ms(f, ST_RANGE), span);
    }
}

/* Union of the PW engine's busy intervals in a frame: DW_PW and VQ_RUN both
 * occupy it, so their union is the engine occupancy that bounds the II. */
static double pw_busy_ms(uint32_t frame)
{
    return stage_ms(frame, ST_DW_PW) + stage_ms(frame, ST_VQ_RUN);
}

void st_print_pipeline(void)
{
    printf("#STPIPE,frame_n,frame_n1,ii_ms,overlap_ms,pw_busy_ms,pw_idle_ms\n");
    const int nf = nframes();
    for (int k = 0; k + 1 < nf; k++) {
        const uint32_t f0 = frame_at(k), f1 = frame_at(k + 1);
        unsigned long long a0, b0, a1, b1;
        if (!frame_span(f0, &a0, &b0) || !frame_span(f1, &a1, &b1)) continue;
        const double ii = ep_cycles_to_ms(a1 - a0);
        /* how much of frame n is still running after frame n+1 began */
        const double ov = (b0 > a1) ? ep_cycles_to_ms(b0 - a1) : 0.0;
        const double busy = pw_busy_ms(f0);
        printf("#STPIPE,%u,%u,%.4f,%.4f,%.4f,%.4f\n",
               (unsigned)f0, (unsigned)f1, ii, ov, busy,
               (ii > busy) ? (ii - busy) : 0.0);
    }
}

int st_check_exclusive(void)
{
    int bad = 0;
    for (int i = 0; i < s_n; i++) {
        if (s_ev[i].stage != ST_DW_PW || s_ev[i].phase != ST_BEGIN) continue;
        unsigned long long a0 = s_ev[i].t, b0 = 0;
        for (int j = i + 1; j < s_n; j++)
            if (s_ev[j].stage == ST_DW_PW && s_ev[j].idx == s_ev[i].idx &&
                s_ev[j].frame == s_ev[i].frame && s_ev[j].phase == ST_END) { b0 = s_ev[j].t; break; }
        if (!b0) continue;
        for (int m = 0; m < s_n; m++) {
            if (s_ev[m].stage != ST_VQ_RUN || s_ev[m].phase != ST_BEGIN) continue;
            unsigned long long a1 = s_ev[m].t, b1 = 0;
            for (int j = m + 1; j < s_n; j++)
                if (s_ev[j].stage == ST_VQ_RUN && s_ev[j].frame == s_ev[m].frame &&
                    s_ev[j].phase == ST_END) { b1 = s_ev[j].t; break; }
            if (!b1) continue;
            if (a0 < b1 && a1 < b0) {
                printf("#STEXCL,VIOLATION,dwpw frame %u pair %u vs vq_run frame %u\n",
                       (unsigned)s_ev[i].frame, (unsigned)s_ev[i].idx,
                       (unsigned)s_ev[m].frame);
                bad++;
            }
        }
    }
    printf("#STEXCL,violations,%d  (DW_PW and VQ_RUN share one engine; must be 0)\n", bad);
    return bad;
}
