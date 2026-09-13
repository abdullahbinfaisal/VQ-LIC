/* ============================================================================
 * rans.h -- context-conditioned rANS for the VQ index stream (payload mode 3),
 *           the mode-0 raw fallback, the table ROM and the payload container.
 *
 * Normative sources, in order of authority: lib/context.py and lib/codec.py
 * (NOT AVAILABLE to this project), CONTEXT_CODEC.md ("spec N" below),
 * RANS_GUIDE.md ("guide N" below).
 *
 * Coder mechanics, unchanged from the guide and spec 4-5:
 *   uint32 state initialised to RANS_L                    (guide 4, 9.3)
 *   symbols encoded BACKWARD, t = n-1 .. 0                 (spec 5, guide 9.5)
 *   renormalisation BEFORE the update, x_max from the
 *     frequency of THIS symbol                             (guide 4, 9.2, 9.7)
 *   one exact divide: q = x/fs, r = x - q*fs               (spec 5, guide 10)
 *   four-byte flush, LSB emitted first                     (spec 5, guide 9.4)
 *   output written backward from the buffer end, so the
 *     state lands at the front, MSB first                  (guide 5)
 *   independent state per group                           (spec 1, guide 9.12)
 *   context looked up per token                            (spec 2, guide 9.15)
 *   16-bit probability precision, integer only             (spec 4)
 *   body-only mode-0 comparison, tie to raw                (spec 7.1, guide 14)
 *   resumable on symbol boundaries                         (guide 11)
 *
 * IDS AND SLOTS -- read before touching the context path.
 *   spec 1: code ids are NOT remapped in the bitstream. A coded symbol is its
 *   original id 0..255, every table is 256 wide, the mode-0 body carries the
 *   original ids, and the header says K = 256. Only 64 ids can occur per group.
 *   spec 2: the context id is the left-neighbour id, or K = 256 on column 0.
 *   spec 3.1: tables live in a FIXED 65-slot array per group.
 *   So there is a remap, but it is ADDRESSING ONLY: slot_of_id maps a context id
 *   to its slot 0..63 and the border id to slot 64. It chooses which stored
 *   table a token reads and cannot change a coded byte, provided it selects the
 *   same table the reference selects for that id.
 *
 *   The accelerator emits k = 0..63. The id to code is a per-group 64-entry
 *   lookup that has to come from the model export along with slot_of_id.
 *
 * TERMINOLOGY. "group" = codec group = one sub-codebook, G = 4, one stream
 * each. It is NOT the 8-lane pixel group of the accelerator.
 * ==========================================================================*/
#ifndef RANS_H
#define RANS_H

#include <stdint.h>
#include <stddef.h>

#define RANS_PROB_BITS  16u
#define RANS_PROB_MASK  0xFFFFu                           /* (1<<PROB_BITS) - 1 */
#define RANS_L          (1u << 23)                        /* state lower bound  */
#define RANS_X_MAX_BASE ((RANS_L >> RANS_PROB_BITS) << 8) /* 32768, DERIVED     */

/* Guide 3: keep X_MAX_BASE derived, so a PROB_BITS change cannot leave it stale. */
typedef char rans_assert_x_max_base[(RANS_X_MAX_BASE == 32768u) ? 1 : -1];

#define RANS_G                 4u    /* independent group streams             */
#define RANS_K_NOMINAL       256u    /* spec 1, 7: ids 0..255; header K       */
#define RANS_NCTX_SLOTS       65u    /* spec 3.1: 64 codes + the border       */
#define RANS_SLOT_NONE      0xFFu    /* this id cannot occur in this group    */
#define RANS_TOP_N            16u    /* spec 3.2                              */
#define RANS_CTX_TABLE_BYTES  48u    /* sym[16] then freq[16] LE, no padding  */
#define RANS_ROW_ENTRIES     512u    /* dense 256-wide slot: f[256], cdf[256] */
#define RANS_ROM_GROUP_BYTES (RANS_NCTX_SLOTS * RANS_CTX_TABLE_BYTES + 512u) /*  3,632 */
#define RANS_ROM_BYTES       (RANS_G * RANS_ROM_GROUP_BYTES)                 /* 14,528 */
#define RANS_HDR_BYTES        17u    /* spec 7, struct <4sBBHBII              */

/* Guide 6: exact worst case of one group stream. Size for it, not typical. */
#define RANS_WORST_BYTES(n)  (2u * (size_t)(n) + 4u)

typedef enum { RANS_MODE_RAW = 0, RANS_MODE_CONTEXT = 3 } rans_mode_t;

/* ---- dense tables -----------------------------------------------------------
 * One group. Slot c is 2*nsym uint16 entries:
 *   rows[c*2*nsym ..]        f[0..nsym-1]
 *   rows[c*2*nsym + nsym ..] cdf[0..nsym-1]
 * freq and cdf adjacent so the two lookups of one token share cache lines
 * (guide 10). cdf holds nsym entries, never nsym+1: cdf[nsym] = 65536 does not
 * fit uint16 and is never read (guide 8). The LAST slot is the column-0 border. */
typedef struct {
    uint32_t        nsym;        /* table width; 256 deployed                   */
    uint32_t        nctx;        /* slots; 65 deployed                          */
    uint32_t        k_border;    /* out-of-band context id K; 256 deployed      */
    const uint8_t  *slot_of_id;  /* k_border entries: id -> slot 0..nctx-2, or
                                    RANS_SLOT_NONE. NULL = identity (slot = id). */
    const uint16_t *rows;        /* nctx * 2 * nsym entries                     */
    const uint32_t *recip;       /* nctx * nsym, from rans_recip_fill(); NULL =
                                    encode with the divide. Same bytes either way. */
} rans_tables_t;

/* Boot-time contract (spec 8.1, guide 8): every f >= 1, cdf[s] = sum f[0..s-1],
 * every slot sums to exactly 65536, slot map in range. Returns 0 if it holds,
 * 1 + c for the first bad slot c, -1 for an invalid descriptor. */
int rans_tables_check(const rans_tables_t *T);

/* Exact reciprocals for the divide-free encoder: recip[c*nsym + s] =
 * ceil(2^(31+sh) / f) with sh = ceil(log2 f), for f >= 2 (0 otherwise). Call
 * once the rows are final; point T->recip at the result to use it. The
 * encoder side only -- the decoder has no divide. */
void rans_recip_fill(const rans_tables_t *T, uint32_t *recip);

/* Spec 2: the context ID. Shared by encoder, decoder and tests so all three
 * provably derive the same one. t % w is the column in THIS group plane, w the
 * token-grid width (guide 9.14). */
static inline uint32_t rans_context_id(const uint8_t *plane, uint32_t t, uint32_t w,
                                       uint32_t k_border)
{
    return (t % w) ? (uint32_t)plane[t - 1] : k_border;
}

/* Context id -> slot. Returns T->nctx (out of range) if the id has no slot. */
static inline uint32_t rans_ctx_slot(const rans_tables_t *T, uint32_t id)
{
    if (id == T->k_border) return T->nctx - 1u;
    if (id >  T->k_border) return T->nctx;
    const uint32_t s = T->slot_of_id ? (uint32_t)T->slot_of_id[id] : id;
    return (s < T->nctx - 1u) ? s : T->nctx;
}

/* ---- table reconstruction (spec 3.4, 3.5) -------------------------------- */

/* Spec 3.5, the normative integer distribution rule. Distributes M over n <= 256
 * weights; writes f. 0 on success, -1 if impossible (M < n under the >= 1
 * floor) or invalid. */
int rans_quantize_to_total(const uint32_t *w, uint32_t n, uint32_t M, uint32_t *f);

/* Spec 3.4: one dense 256-wide slot (f then cdf, 512 entries) from a top-16
 * table and the group marginal. T, the 240 unstored symbols, is taken in
 * ascending id. 0 ok; -2 duplicate stored symbol; -3 stored freq 0; -4 stored
 * sum >= 65536; -5 escape cannot be distributed; -6 bad marginal; -7 result
 * breaks the contract. */
int rans_reconstruct_row(const uint8_t *sym, const uint16_t *freq,
                         const uint16_t *marginal, uint16_t *row);

/* Spec 3: one group of ROM as shipped -- 65 CtxTable (sym[16], freq[16] LE)
 * followed by MargTable (256 x uint16 LE), RANS_ROM_GROUP_BYTES in all --
 * expanded into 65 dense slots (65 * 512 uint16). Boot-time work. 0 ok,
 * 1 + c for the first slot c that fails, -1 invalid argument. */
int rans_rom_expand_group(const uint8_t *rom, uint16_t *rows);

/* ---- static coder: bring-up only (guide 1, stage 0) --------------------- */
size_t rans_encode_static(const uint16_t *f, const uint16_t *cdf, uint32_t nsym,
                          const uint8_t *sym, uint32_t n,
                          uint8_t *buf, size_t cap, const uint8_t **stream);
/* 0 = ok and the read pointer landed exactly on len; -1 past len; -2 left over. */
int rans_decode_static(const uint16_t *f, const uint16_t *cdf, uint32_t nsym,
                       const uint8_t *d, size_t len, uint8_t *out, uint32_t n);

/* ---- context coder, resumable (guide 4, 5, 11) ------------------------- */
typedef struct {
    uint32_t             x;      /* coder state                               */
    int32_t              t;      /* next token, counts DOWN; -1 = all coded   */
    size_t               p;      /* write pointer into buf, counts DOWN       */
    int                  done;   /* 1 once the 4-byte flush is written        */
    int                  err;    /* nonzero: stream invalid                   */
    const uint8_t       *plane;  /* n = h*w ids, raster order                 */
    uint32_t             n, w;
    const rans_tables_t *T;
    uint8_t             *buf;
    size_t               cap;
} rans_enc_t;

void     rans_enc_begin(rans_enc_t *e, const uint8_t *plane, uint32_t n, uint32_t w,
                        const rans_tables_t *T, uint8_t *buf, size_t cap);
/* Codes up to `budget` tokens (0 = all remaining); writes the flush in the same
 * call that codes token 0. Never blocks. Returns 1 when complete or failed. */
int      rans_enc_step(rans_enc_t *e, uint32_t budget);
size_t   rans_enc_finish(rans_enc_t *e, const uint8_t **stream);
uint32_t rans_enc_coded(const rans_enc_t *e);

/* Test-harness decoder (spec 6, guide 7). 0 = ok and the read pointer landed
 * exactly on len; -1 past len; -2 left over; -3 context or table error. */
int rans_decode_ctx(const rans_tables_t *T, uint32_t w, const uint8_t *d, size_t len,
                    uint8_t *out, uint32_t n);

/* ---- body, fallback, container (spec 7, guide 14) ---------------------- */
/* coded_body_len INCLUDES the four 4-byte length prefixes and EXCLUDES the
 * 17-byte header. >=, so a tie goes to raw. */
rans_mode_t rans_choose_mode(size_t coded_body_len, uint32_t n, uint32_t G);

typedef struct {
    rans_enc_t           enc;
    uint32_t             g;                   /* group in progress           */
    uint32_t             n, w;
    size_t               work_cap;
    const uint8_t       *planes[RANS_G];
    const rans_tables_t *T[RANS_G];
    uint8_t             *work[RANS_G];        /* one per group, >= 2n+4 B    */
    const uint8_t       *stream[RANS_G];
    size_t               len[RANS_G];
    int                  err;
} rans_frame_t;

void     rans_frame_begin(rans_frame_t *F, const uint8_t *const planes[RANS_G],
                          const rans_tables_t *const T[RANS_G], uint32_t n, uint32_t w,
                          uint8_t *const work[RANS_G], size_t work_cap);
int      rans_frame_step(rans_frame_t *F, uint32_t budget);
uint32_t rans_frame_coded(const rans_frame_t *F);
/* BODY only:  mode 3: uint32 LE len_g, stream_g, g = 0..3
 *             mode 0: the four planes, group-major, one byte per index
 * Returns body length and sets *mode; 0 on error. out must not overlap the
 * work buffers; 4*(2n+8) always suffices. */
size_t   rans_frame_finish(rans_frame_t *F, uint8_t *out, size_t cap, rans_mode_t *mode);

/* Spec 7 header: "NIC1", mode, G, K (u16 LE), prob_bits, H (u32 LE), W (u32 LE).
 * H and W are IMAGE dimensions. Returns 17, or 0 on error. */
size_t rans_write_header(uint8_t *out, size_t cap, rans_mode_t mode,
                         uint32_t K, uint32_t H, uint32_t W);

/* The complete payload: 17-byte header, then the body. The frame must have
 * been begun with w = ceil(W/8) and n = ceil(H/8) * w. K is taken from the
 * group-0 tables. Returns total bytes, 0 on error. */
size_t rans_frame_payload(rans_frame_t *F, uint32_t H, uint32_t W,
                          uint8_t *out, size_t cap, rans_mode_t *mode);

#endif /* RANS_H */
