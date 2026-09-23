// ============================================================================
// range_coder.h -- static-model integer range coder for the VQ index stream
//                  ImageEncoderLite / ZC702, Cortex-A9 bare metal.
// ============================================================================
//
// WHY THIS EXISTS
//   vq_pq.c emits 57,600 uint8 indices per frame (14,400 positions x 4
//   codebooks). Those indices ARE the payload, but a fixed-length uint8
//   representation is 8 bits/symbol regardless of how skewed the codeword
//   distribution is. The paper's "complete edge encoder" latency must include
//   turning those indices into the actual compressed byte stream, so this file
//   supplies the entropy stage that was previously missing entirely.
//
// ALPHABET AND MODELS
//   Four INDEPENDENT alphabets, one per PQ codebook, each of VQ_K = 256
//   symbols. The codebooks quantise disjoint 16-D subvectors, so their index
//   distributions are statistically unrelated -- pooling them into a single
//   histogram would be meaningless and would misstate the achievable rate.
//   Symbol m of position p is coded with model m.
//
// PROBABILITY MODEL: STATIC, PRE-SHARED
//   Frequencies are frozen before the timed run and are identical at encoder
//   and decoder, so NO per-frame side information is transmitted and the
//   reported bpp needs no side-channel correction. Construction cost is
//   measured separately (rc_model_build) and is NOT counted inside T_RANGE.
//   See rc_model_build() for the smoothing and normalisation rules.
//
//   Totals are normalised to RC_TOT = 1<<16 so that `range / tot` in the hot
//   loop is a shift, not a divide. Every symbol keeps freq >= 1 (add-one
//   smoothing) so an unseen codeword can still be coded -- there is no escape
//   symbol and no possibility of a zero-probability stall.
//
// CODER
//   Subbotin carryless range coder, 32-bit range, 8-bit renormalisation.
//   Integer only; no floating point anywhere in the encode or decode loop.
//   Chosen over an arithmetic coder with carry propagation because the
//   carryless form has no unbounded carry chain, which makes it trivially
//   deterministic and easy to argue about in a paper.
//
// TERMINATION
//   rc_enc_finish() flushes 4 bytes of `low`, which is sufficient for the
//   decoder to resolve the final symbol. The stream is byte-addressable and
//   self-delimiting given the symbol count (which the decoder knows from the
//   fixed latent geometry -- it is not side information).
//
// ============================================================================
#ifndef RANGE_CODER_H
#define RANGE_CODER_H

#include <stdint.h>
#include <stddef.h>
#include "vq_pq.h"
#include "vq_pw.h"

// ---------------------------------------------------------------------------
// GEOMETRY. The coder core below is untouched by this choice -- only the
// alphabet size, the model count and the INDEX ADDRESSING change.
//
//   RC_GEOMETRY_PW = 1  PW-hosted VQ: M = 8 sub-codebooks of K = 16, packed
//                       as 4-bit nibbles, 4 bytes per latent position.
//   RC_GEOMETRY_PW = 0  legacy dedicated engine: M = 4 of K = 256, one byte
//                       per symbol. The hardware is gone (62cbfbc); kept so
//                       the previously published rate numbers stay
//                       reproducible from this source.
//
// THE SYMBOL COUNT FOLLOWS M, NOT K. At the LEGACY M=8/K=16 it was 14,400
// positions x 8 models = 115,200 symbols per frame on a 16-symbol alphabet,
// twice the 57,600 the dedicated M=4/K=256 engine produced. At the DEPLOYED
// M=4/K=64 it is back to 14,400 x 4 = 57,600, on a 64-symbol alphabet.
//
// So the deployed quantiser HALVES the entropy-stage symbol count relative to
// the legacy one. T_RANGE was measured at M=8/K=16 and does NOT carry over --
// it must be re-measured, and it should fall.
// ---------------------------------------------------------------------------
#ifndef RC_GEOMETRY_PW
#define RC_GEOMETRY_PW 1
#endif

#if RC_GEOMETRY_PW
#  define RC_NSYM      VQPW_K            // 16 codewords per sub-codebook
#  define RC_NMODEL    VQPW_M            // 8 independent sub-codebook models
#  define RC_NPOS      VQPW_NPOS
#  define RC_IDX_BYTES VQPW_IDX_BYTES
#else
#  define RC_NSYM      VQ_K              // 256 symbols per alphabet
#  define RC_NMODEL    VQ_M              // 4 independent codebook models
#  define RC_NPOS      VQ_NPOS
#  define RC_IDX_BYTES VQ_IDX_BYTES
#endif

#define RC_NSYM_PER_FRAME ((size_t)RC_NPOS * RC_NMODEL)
#define RC_TOT_BITS  16
#define RC_TOT       (1u << RC_TOT_BITS)   // 65536, power of two -> shift not divide

#define RC_TOP       (1u << 24)
#define RC_BOT       (1u << 16)

// One frozen model: cumulative frequencies, cum[0]=0 .. cum[RC_NSYM]=RC_TOT.
// freq[s] = cum[s+1] - cum[s], always >= 1.
typedef struct {
    uint16_t cum[RC_NSYM + 1];
} rc_model_t;

typedef struct {
    rc_model_t m[RC_NMODEL];
    int        frozen;            // 1 once rc_model_build has run
} rc_models_t;

// ---- encoder ----------------------------------------------------------------
typedef struct {
    uint32_t  low;
    uint32_t  range;
    uint8_t  *out;
    size_t    cap;
    size_t    n;                  // bytes written
    int       overflow;           // 1 if cap was hit; stream is invalid
} rc_enc_t;

// ---- decoder ----------------------------------------------------------------
typedef struct {
    uint32_t       low;
    uint32_t       range;
    uint32_t       code;
    const uint8_t *in;
    size_t         cap;
    size_t         n;
} rc_dec_t;

// ---- index addressing -------------------------------------------------------
// The ONLY thing the geometry changes inside the coder. Packed 4-bit symbols
// are read and written here so every loop below stays layout-agnostic.
//
// Packing (vq_pw.c): one 32-bit little-endian word per position, sub-codebook
// m in bits [KW*m + KW-1 : KW*m], KW = log2(K).
//
//   KW = 4 (M=8,K=16) : byte j holds model 2j low, model 2j+1 high. Fields do
//                       not straddle bytes, so the single-byte path below is
//                       exact -- and it is the path the measured T_RANGE was
//                       taken on, so it is kept rather than generalised away.
//   KW = 6 (M=4,K=64) : fields DO straddle bytes (model 1 is bits 6..11), so
//                       the word must be assembled before shifting.
//
// RC_NSYM = VQPW_K = 64 <= 256, so the uint8_t symbol path still holds; the
// alphabet grows but the per-frame symbol COUNT halves, 14,400*4 = 57,600
// against 115,200 at K=16.
static inline uint8_t rc_get_sym(const uint8_t *idx, int pos, int m)
{
#if !RC_GEOMETRY_PW
    return idx[(size_t)pos * RC_NMODEL + (size_t)m];
#elif VQPW_KW == 4
    const uint8_t b = idx[(size_t)pos * 4 + (size_t)(m >> 1)];
    return (uint8_t)((m & 1) ? (b >> 4) : (b & 0x0Fu));
#else
    const uint8_t *p = idx + (size_t)pos * 4;
    const uint32_t w = (uint32_t)p[0]        | ((uint32_t)p[1] <<  8)
                     | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    return (uint8_t)((w >> (VQPW_KW * m)) & VQPW_KMASK);
#endif
}

static inline void rc_put_sym(uint8_t *idx, int pos, int m, uint8_t sym)
{
#if !RC_GEOMETRY_PW
    idx[(size_t)pos * RC_NMODEL + (size_t)m] = sym;
#elif VQPW_KW == 4
    uint8_t *b = &idx[(size_t)pos * 4 + (size_t)(m >> 1)];
    *b = (uint8_t)((m & 1) ? ((*b & 0x0Fu) | (uint8_t)((sym & 0x0Fu) << 4))
                           : ((*b & 0xF0u) | (uint8_t)(sym & 0x0Fu)));
#else
    uint8_t *p = idx + (size_t)pos * 4;
    uint32_t w = (uint32_t)p[0]        | ((uint32_t)p[1] <<  8)
               | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    w &= ~(VQPW_KMASK << (VQPW_KW * m));
    w |= ((uint32_t)sym & VQPW_KMASK) << (VQPW_KW * m);
    p[0] = (uint8_t)(w      ); p[1] = (uint8_t)(w >>  8);
    p[2] = (uint8_t)(w >> 16); p[3] = (uint8_t)(w >> 24);
#endif
}

// ---- model construction (NOT part of T_RANGE) -------------------------------
// Accumulates a histogram over `nframes` index arrays, applies add-one
// smoothing, normalises each model to exactly RC_TOT, and freezes.
// idx_frames[f] points at RC_IDX_BYTES bytes in whatever layout the selected
// geometry uses; rc_get_sym() is the only thing that knows which.
void rc_model_build(rc_models_t *M, const uint8_t *const *idx_frames, int nframes);

// Uniform fallback (freq = RC_TOT/RC_NSYM for all symbols). Useful as a
// control: coded size should then equal the fixed-length size.
void rc_model_uniform(rc_models_t *M);

// Empirical (per-codebook) entropy of ONE actual index frame, bits/symbol.
// This -- not the model entropy -- is the correct lower bound to compare a
// coded rate against, because the model may have been trained on other data.
void rc_frame_entropy(const uint8_t *idx, double bits_per_sym[RC_NMODEL]);

// Shannon entropy of each frozen model, bits/symbol. Reported per codebook.
void rc_model_entropy(const rc_models_t *M, double bits_per_sym[RC_NMODEL]);

// ---- encode / decode --------------------------------------------------------
void   rc_enc_init(rc_enc_t *e, uint8_t *out, size_t cap);
void   rc_enc_sym (rc_enc_t *e, const rc_model_t *m, uint8_t sym);
size_t rc_enc_finish(rc_enc_t *e);

void   rc_dec_init(rc_dec_t *d, const uint8_t *in, size_t cap);
uint8_t rc_dec_sym (rc_dec_t *d, const rc_model_t *m);

// ---- whole-frame helpers ----------------------------------------------------
// Encode all RC_NSYM_PER_FRAME symbols of one frame. Returns bytes written.
// This is the function timed as T_RANGE.
size_t rc_encode_frame(const rc_models_t *M, const uint8_t *idx,
                       uint8_t *out, size_t cap);

// Decode a frame back into `idx_out`. Returns 0 on success.
int    rc_decode_frame(const rc_models_t *M, const uint8_t *in, size_t n,
                       uint8_t *idx_out);

// Round-trip check: encode then decode then compare. Returns mismatch count,
// and writes the first failing symbol offset to *first_bad (-1 if none).
long   rc_selftest_frame(const rc_models_t *M, const uint8_t *idx,
                         uint8_t *scratch_bs, size_t bs_cap,
                         uint8_t *scratch_idx, long *first_bad);

// ---- incremental encoder ----------------------------------------------------
// Same coder, driven a slice at a time so it can run in someone else's idle
// gaps. The Subbotin coder is a sequential state machine over (low, range,
// output cursor), so suspending it between any two symbols costs nothing and
// changes nothing: rc_encode_frame() is IMPLEMENTED on top of this, which is
// what guarantees the sliced output is byte-identical to the one-shot output
// rather than merely intended to be.
//
// Why this exists: on the 16-48-64 schedule the CPU spends 13.4816 ms per
// frame spinning on the analysis cascade's DMA status registers, and entropy
// coding of the PREVIOUS frame's indices is 5.275 ms of pure CPU work that
// touches neither the PW engine nor any buffer in flight. It fits in that gap
// 2.6x over.
//
//   rc_stream_start (s, M, idx, out, cap);
//   while (!rc_stream_step(s, 64)) { ...someone else's wait loop... }
//   n = rc_stream_finish(s);
//
// rc_stream_finish() codes whatever is left before flushing, so it is always
// safe to call -- a stream that never got a single slice still produces a
// correct frame, just with none of it hidden.
typedef struct {
    rc_enc_t           e;
    const rc_models_t *M;
    const uint8_t     *idx;
    int                pos;      // next position
    int                m;        // next sub-codebook within that position
    int                done;     // all symbols coded (flush may still be due)
} rc_stream_t;

void   rc_stream_start (rc_stream_t *s, const rc_models_t *M,
                        const uint8_t *idx, uint8_t *out, size_t cap);

// Code up to nsym more symbols; nsym == 0 means "code all that remain".
// Returns 1 when no symbols are left, 0 when the budget ran out first.
int    rc_stream_step  (rc_stream_t *s, unsigned long nsym);

// Code any remainder, flush the coder, return bytes written (0 on overflow).
size_t rc_stream_finish(rc_stream_t *s);

// Symbols coded so far. Cheap: no timer, no side effects -- call it around a
// window to find out how much of the frame that window absorbed.
unsigned long rc_stream_coded(const rc_stream_t *s);

#endif // RANGE_CODER_H
