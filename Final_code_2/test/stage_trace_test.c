/* ============================================================================
 * stage_trace_test.c -- does the pipeline arithmetic actually compute what it
 * claims?
 *
 * #STPIPE's overlap and pw_busy columns are the numbers a pipelining figure
 * and any throughput claim would rest on. They are derived from event
 * timestamps by code that has no natural cross-check on hardware, so they get
 * one here: synthetic traces with KNOWN answers, including the two cases that
 * matter -- fully serial frames (overlap must be 0) and frames deliberately
 * overlapped (overlap must equal the constructed amount).
 *
 * The engine-exclusivity check is also exercised both ways: a legal trace must
 * report zero violations, and a trace where DW_PW and VQ_RUN deliberately
 * straddle must be caught, because that combination is physically impossible
 * on one PW engine and would mean the instrumentation is lying.
 *
 *   gcc -O2 -I../src -o stage_trace_test stage_trace_test.c ../src/stage_trace.c
 * ==========================================================================*/
#include <stdio.h>
#include <string.h>
#include "stage_trace.h"

/* Fake clock: 1 tick = 1 us at the rate ep_cycles_to_ms assumes below. */
static unsigned long long g_now = 0;
unsigned long long ep_timer_now(void) { return g_now; }
double ep_cycles_to_ms(unsigned long long c) { return (double)c / 1000.0; }

static int fails = 0;
static void chk(int cond, const char *what)
{
    printf("  %-62s %s\n", what, cond ? "ok" : "FAIL");
    if (!cond) fails++;
}

static void at(unsigned long long t) { g_now = t; }

/* one serial frame: prog, 3 block pairs, vq prog, vq run -- no overlap */
static unsigned long long emit_frame(uint32_t f, unsigned long long t0,
                                     unsigned dwpw_us, unsigned vq_us)
{
    unsigned long long t = t0;
    st_set_frame(f);
    at(t); ST_BEGIN_S(ST_FRAME);
    at(t); ST_BEGIN_S(ST_PROG);   t += 2000; at(t); ST_END_S(ST_PROG);
    for (int p = 0; p < 3; p++) {
        at(t); ST_BEGIN_I(ST_DW_PW, p);
        t += dwpw_us / 3;
        at(t); ST_END_I(ST_DW_PW, p);
    }
    at(t); ST_BEGIN_S(ST_VQ_PROG); t += 300; at(t); ST_END_S(ST_VQ_PROG);
    at(t); ST_BEGIN_S(ST_VQ_RUN);  t += vq_us; at(t); ST_END_S(ST_VQ_RUN);
    at(t); ST_END_S(ST_FRAME);
    return t;
}

int main(void)
{
    printf("stage_trace pipeline arithmetic\n");

    /* ---- 1. serial frames: overlap must be exactly zero ------------------ */
    st_reset();
    unsigned long long t = 1000;
    for (uint32_t f = 0; f < 3; f++) t = emit_frame(f, t, 12000, 2450);

    chk(!st_overflowed(), "trace did not overflow");
    chk(st_count() == 3 * 14, "event count matches what was emitted");
    chk(st_check_exclusive() == 0, "serial trace: no engine-exclusivity violation");

    printf("  -- serial trace --\n");
    st_print_frame_summary();
    st_print_pipeline();

    /* ---- 2. overlapped frames: overlap must equal what was constructed --- */
    /* frame 1 starts 5,000 us before frame 0 ends -> overlap 5,000 us = 5 ms */
    st_reset();
    st_set_frame(0);
    at(0);      ST_BEGIN_S(ST_FRAME);
    at(0);      ST_BEGIN_I(ST_DW_PW, 0);
    at(10000);  ST_END_I(ST_DW_PW, 0);
    at(10000);  ST_BEGIN_S(ST_VQ_RUN);
    at(20000);  ST_END_S(ST_VQ_RUN);
    at(20000);  ST_END_S(ST_FRAME);

    st_set_frame(1);
    at(15000);  ST_BEGIN_S(ST_FRAME);      /* begins while frame 0's VQ runs */
    at(15000);  ST_BEGIN_S(ST_PACK);       /* host work -- legal to overlap  */
    at(19000);  ST_END_S(ST_PACK);
    at(20000);  ST_BEGIN_I(ST_DW_PW, 0);   /* engine free only after VQ ends */
    at(30000);  ST_END_I(ST_DW_PW, 0);
    at(30000);  ST_END_S(ST_FRAME);

    printf("  -- overlapped trace --\n");
    st_print_pipeline();
    chk(st_check_exclusive() == 0,
        "overlapped trace is still engine-legal (DW_PW starts as VQ ends)");

    /* ---- 3. an illegal trace must be CAUGHT ------------------------------ */
    st_reset();
    st_set_frame(0);
    at(0);     ST_BEGIN_S(ST_VQ_RUN);
    at(10000); ST_END_S(ST_VQ_RUN);
    st_set_frame(1);
    at(5000);  ST_BEGIN_I(ST_DW_PW, 0);    /* straddles frame 0's VQ */
    at(15000); ST_END_I(ST_DW_PW, 0);
    chk(st_check_exclusive() == 1,
        "impossible trace IS caught (DW_PW straddling VQ_RUN on one engine)");

    /* ---- 4. the hiding metric on the REAL pipelined shape ---------------- */
    /* edge_one_pipelined's structure: analysis, then VQ with the next frame's
     * input preparation running inside it. VQ 2,450 us, PACK 1,800 us nested
     * entirely within it -> hidden 1.8 ms, exposed 0.65 ms. */
    st_reset();
    st_set_frame(0);
    at(0);     ST_BEGIN_S(ST_FRAME);
    at(0);     ST_BEGIN_I(ST_DW_PW, 0);
    at(13446); ST_END_I(ST_DW_PW, 0);
    at(13446); ST_BEGIN_S(ST_VQ_PROG);
    at(13746); ST_END_S(ST_VQ_PROG);
    at(13746); ST_BEGIN_S(ST_VQ_RUN);
    at(13800); ST_BEGIN_S(ST_PACK);          /* CPU work under the search */
    at(15600); ST_END_S(ST_PACK);            /* 1,800 us, fully inside    */
    at(16196); ST_END_S(ST_VQ_RUN);          /* 2,450 us total            */
    at(16196); ST_END_S(ST_FRAME);
    printf("  -- pipelined shape --\n");
    st_print_hiding();
    chk(st_check_exclusive() == 0, "pipelined shape is engine-legal");

    /* a fully serial frame must report the whole VQ as exposed */
    st_reset();
    st_set_frame(0);
    at(0);    ST_BEGIN_S(ST_VQ_RUN);
    at(2450); ST_END_S(ST_VQ_RUN);
    printf("  -- serial shape --\n");
    st_print_hiding();

    /* ---- 5. disabled tracing costs nothing and records nothing ----------- */
    st_reset();
    st_enable(0);
    st_set_frame(0);
    at(0); ST_BEGIN_S(ST_FRAME);
    at(1); ST_END_S(ST_FRAME);
    chk(st_count() == 0, "st_enable(0) records nothing");

    printf("\n%d checks failed\n", fails);
    printf(fails ? "RESULT: FAIL\n" : "RESULT: PASS\n");
    return fails != 0;
}
