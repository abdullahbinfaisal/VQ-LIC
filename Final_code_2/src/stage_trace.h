#ifndef STAGE_TRACE_H
#define STAGE_TRACE_H
/* ============================================================================
 * stage_trace.h -- absolute-timestamped stage boundaries, so the pipeline can
 * be drawn and its overlap MEASURED rather than assumed.
 *
 * Every existing timer in this firmware reports a DURATION. Durations cannot
 * answer "did frame n+1's DW work overlap frame n's VQ", because that needs
 * the two intervals on a common clock. This records (frame, stage, phase,
 * absolute timestamp) and derives the rest.
 *
 * COST. st_mark() is a global-timer read and four stores, tens of nanoseconds
 * against stages measured in milliseconds. It is safe inside a timed bracket,
 * unlike power scanning (see pwr_log.h).
 *
 * WHAT THE TRACE WILL SHOW TODAY. The harness calls the accelerator through
 * blocking helpers, so consecutive stages are strictly serial and the measured
 * overlap will be ZERO. That is a property of the current harness, not of the
 * hardware: vq_pw_pl_start/poll_done/finish exist precisely so a frame's VQ
 * can run while the next frame's DW work proceeds. Do not report a serial
 * trace as evidence that overlap is impossible -- report it as the baseline it
 * is, and note the model's bound (results/pipeline_timing.py).
 *
 * THE HARD CONSTRAINT ON OVERLAP. VQ and the analysis transform share ONE PW
 * engine, so their intervals can never overlap by even a cycle. What can
 * overlap is frame n's VQ against frame n+1's DW work, DDR traffic and host
 * work. st_check_exclusive() asserts the engine invariant on real data.
 * ==========================================================================*/

#include <stdint.h>

typedef enum {
    ST_FRAME = 0,   /* whole frame, idx unused                              */
    ST_PACK,        /* host: input packing                                  */
    ST_PROG,        /* host: convolution weight/param programming           */
    ST_DW_PW,       /* analysis accelerator, one block pair -- idx = pair   */
    ST_VQ_PROG,     /* codebook + geometry reload (shared weight BRAM)      */
    ST_VQ_RUN,      /* VQ on the PW engine                                  */
    ST_RANGE,       /* range coder                                          */
    ST_NSTAGE
} st_stage_t;

typedef enum { ST_BEGIN = 0, ST_END = 1 } st_phase_t;

/* Capacity: 6 block pairs x 2 + ~6 other stages x 2 = ~24 events/frame.
 * 4096 covers ~170 frames, more than the 80-frame protocol needs. */
#define ST_MAX_EVENTS 4096

typedef struct {
    uint32_t           frame;
    uint8_t            stage;
    uint8_t            idx;
    uint8_t            phase;
    unsigned long long t;      /* ARM global timer ticks */
} st_event_t;

void st_reset(void);                       /* drop all events, arm recording  */
void st_set_frame(uint32_t frame_id);      /* tag subsequent events           */
void st_mark(st_stage_t s, uint8_t idx, st_phase_t p);
void st_enable(int on);                    /* off = zero-cost no-op           */
int  st_count(void);
int  st_overflowed(void);

#define ST_BEGIN_S(s)       st_mark((s), 0, ST_BEGIN)
#define ST_END_S(s)         st_mark((s), 0, ST_END)
#define ST_BEGIN_I(s, i)    st_mark((s), (uint8_t)(i), ST_BEGIN)
#define ST_END_I(s, i)      st_mark((s), (uint8_t)(i), ST_END)

/* ---- output ------------------------------------------------------------- */

/* Raw events, one CSV row each, relative to the first event:
 *   #STCSV,frame,stage,idx,phase,t_us
 * This is the table a pipeline/Gantt figure is drawn from directly. */
void st_print_csv(void);

/* Per-frame stage durations, one row per frame:
 *   #STSUM,frame,pack_ms,prog_ms,dwpw_ms,vqprog_ms,vqrun_ms,range_ms,frame_ms */
void st_print_frame_summary(void);

/* Frame-to-frame behaviour, one row per consecutive pair:
 *   #STPIPE,frame_n,frame_n1,ii_ms,overlap_ms,pw_busy_ms,pw_idle_ms
 * ii is start(n+1) - start(n); overlap is how much of frame n extends past
 * the start of frame n+1; pw_busy is the union of ST_DW_PW and ST_VQ_RUN. */
void st_print_pipeline(void);

/* Assert the engine invariant: no ST_DW_PW interval may overlap an ST_VQ_RUN
 * interval, in the same frame or across frames -- they are one engine.
 * Returns the number of violations and prints each. */
int  st_check_exclusive(void);

/* How much of the VQ was actually HIDDEN by concurrent CPU work, per frame:
 *   #STHIDE,frame,vq_ms,hidden_ms,exposed_ms
 * hidden is the part of the ST_VQ_RUN interval covered by some other stage
 * running inside it; exposed is the remainder, and it is the only part that
 * lengthens the initiation interval. This is the number a pipelining claim
 * rests on -- "VQ is 2.45 ms" says nothing on its own once it overlaps. */
void st_print_hiding(void);

#endif /* STAGE_TRACE_H */
