/* ============================================================================
 * rans_edge.h -- the deployed entropy stage on the edge, as one resumable job:
 *
 *     transport indices (4 bytes/position, 6-bit k fields, from the PW search)
 *       -> four raster planes of ORIGINAL code ids (CONTEXT_CODEC.md 1)
 *       -> rANS, mode 3, four independent streams (rans.c)
 *       -> the complete payload, 17-byte header + body, mode 0 fallback
 *
 * SYNTHETIC MODEL. Two inputs the shipped model would supply do not exist on
 * this project yet, so both are synthesised and nothing produced here can be
 * compared with the reference:
 *   - the per-group map hardware k (0..63) -> original id (0..255), and the
 *     id -> slot map. A fixed permutation, with slot == k.
 *   - the context-table ROM. FITTED on this run's own calibration indices, in
 *     the CONTEXT_CODEC.md section-3 binary layout, then expanded through the
 *     same boot path (rans_rom_expand_group) the real ROM will take.
 * Timing is meaningful. Payload size is NOT a rate.
 *
 * The unpack and the coder share static planes and work buffers, so only ONE
 * job may be live at a time. The harness never overlaps two.
 * ==========================================================================*/
#ifndef RANS_EDGE_H
#define RANS_EDGE_H

#include <stdint.h>
#include <stddef.h>
#include "rans.h"
#include "vq_pw.h"

#define RE_W              ((uint32_t)VQPW_MAP_W)               /* 160 token-grid width */
#define RE_H              ((uint32_t)VQPW_MAP_H)               /*  90                  */
#define RE_N              ((uint32_t)VQPW_NPOS)                /* 14,400 per group     */
#define RE_IMG_W          (RE_W * 8u)                          /* 1280, spec 1         */
#define RE_IMG_H          (RE_H * 8u)                          /*  720                 */
#define RE_WORK_BYTES     RANS_WORST_BYTES(RE_N)               /* 28,804 per group     */
#define RE_PAYLOAD_CAP    (RANS_HDR_BYTES + RANS_G * (4u + RE_WORK_BYTES))   /* 115,249 */

typedef struct {
    uint32_t populated[RANS_G];   /* context slots that saw calibration data   */
    uint32_t ctx_tokens[RANS_G];  /* calibration tokens behind those slots     */
    double   fit_ms;              /* boot-time work, NOT part of any T_RANGE   */
    int      rc;
} re_fit_report_t;

/* Synthetic k -> id and id -> slot maps. Call before re_fit_tables(). */
void re_synth_maps(void);

/* Fit top-16 context tables and group marginals on `ncal` transport frames,
 * serialise them in the spec-3 ROM layout, expand and check. 0 on success. */
int  re_fit_tables(const uint8_t *const *cal_transport, int ncal, re_fit_report_t *rep);

/* 1 (default) = the divide-free encoder, 0 = the reference divide encoder. The
 * two write identical bytes (test/rans_recip_test.c); this exists for that
 * test and for an on-board A/B. Never call it while a job is live. */
void re_set_fast_divide(int on);

typedef struct {
    const uint8_t *src;         /* transport indices of the frame being coded */
    uint32_t       upos;        /* positions unpacked                         */
    int            phase;       /* 1 unpack, 2 code, 3 coded, 4 payload built */
    rans_frame_t   F;
    uint8_t       *payload;
    size_t         cap, len;
    rans_mode_t    mode;
    int            err;
} re_job_t;

/* Arm a job. The transport buffer must stay unchanged until re_job_finish(). */
void     re_job_start(re_job_t *J, const uint8_t *transport, uint8_t *payload, size_t cap);
/* Advance by `budget` symbol-units (unpacking one position costs 4, coding one
 * symbol costs 1); 0 = run to the end of coding. Never blocks. Returns 1 once
 * all coding is done -- the payload is not assembled until re_job_finish(). */
int      re_job_step(re_job_t *J, uint32_t budget);
/* Complete whatever is left, then assemble the payload. Returns payload bytes
 * (header included) and sets *mode; 0 on error. */
size_t   re_job_finish(re_job_t *J, rans_mode_t *mode);
uint32_t re_job_unpacked(const re_job_t *J);   /* positions, 0..14,400        */
uint32_t re_job_coded(const re_job_t *J);      /* symbols,   0..57,600        */
/* Decode the payload with the test decoder, map ids back to hardware k and
 * compare with the transport indices. Returns mismatching symbols (0 = the
 * whole chain is lossless), negative if there is no payload. Harness only. */
long     re_job_verify(const re_job_t *J, const uint8_t *transport, long *first_bad);

#endif /* RANS_EDGE_H */
