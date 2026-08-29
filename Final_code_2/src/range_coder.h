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

#define RC_NSYM      VQ_K          // 256 symbols per alphabet
#define RC_NMODEL    VQ_M          // 4 independent codebook models
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

// ---- model construction (NOT part of T_RANGE) -------------------------------
// Accumulates a histogram over `nframes` index arrays, applies add-one
// smoothing, normalises each model to exactly RC_TOT, and freezes.
// idx_frames[f] points at VQ_IDX_BYTES bytes laid out as vq_pq_encode_frame
// writes them (position-major, codebook-minor).
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
// Encode all VQ_IDX_BYTES indices of one frame. Returns bytes written.
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

#endif // RANGE_CODER_H
