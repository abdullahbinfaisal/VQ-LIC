#ifndef VQ_PW_H
#define VQ_PW_H
/* ============================================================================
 * vq_pw.h -- golden model for PW-ENGINE-HOSTED product quantisation.
 *
 * Replaces the dedicated VQ engine's M=4/K=256/DSUB=16 search with
 * M=8/K=16/DSUB=8, evaluated on the existing INT8 pointwise-convolution
 * engine. Both configurations emit M*log2(K) = 32 bits per latent position.
 *
 * WHY THE PW MAC CAN DO THIS UNMODIFIED
 *   pw_pixel_major_core computes, per output channel oc:
 *       acc[oc] = SUM_ic ( uint8(activation) - uint8(zp_in) ) * int8(weight)
 *   Write u = zq - z0 (activation minus zero point) and v_k = cq,k - z0
 *   (codeword minus the SAME zero point). Then
 *       ||zq - cq,k||^2 = ||u - v_k||^2 = ||u||^2 - 2*u.v_k + ||v_k||^2
 *   and since ||u||^2 does not depend on k,
 *       argmin_k ||zq - cq,k||^2 = argmin_k ( ||v_k||^2 - 2*acc[k] ).
 *   acc[k] is exactly what the existing MAC already produces, provided the
 *   codebook is stored PRE-CENTRED as v = cq - z0. No MAC change at all.
 *
 * THE z0 = 128 DEPENDENCY  (load-bearing -- assert it, do not assume it)
 *   The weight port is signed INT8. v = cq - z0 fits [-128,127] only because
 *   the whole pipeline runs zp_in = zp_out = 128 and cq is uint8. At any other
 *   zero point v needs 9 bits and this mapping FAILS. vqpw_init() rejects it.
 *
 * QUANTISATION IS FREE
 *   The final PW layer already emits uint8 at zp_out = 128, so the VQ-search
 *   integer domain IS the stored latent: zq = latent byte, z0 = 128. The
 *   "quantiser" is the zero-point subtract the DSP pre-adder already performs.
 *   No divider, no rescale, no extra pass. This reproduces vq_pq.c's rule
 *   exactly, including its truncating (int8_t) cast.
 *
 * HARDWARE MAPPING (option D)
 *   N_OC = 32 and K = 16, so ONE OC batch holds TWO sub-codebooks, with
 *   block-diagonal weights over a 16-channel input window:
 *       batch b, oc  0..15 -> sub-codebook m = 2b,   input dims 16b+0 .. 16b+7
 *       batch b, oc 16..31 -> sub-codebook m = 2b+1, input dims 16b+8 .. 16b+15
 *   4 batches cover all M = 8. The engine streams the full 64-channel group
 *   (cin_load = 64, one contiguous DDR read) but each batch's MAC walks only
 *   cin_mac = 16 pb_ram entries starting at vq_base = 16*b.
 *
 *   ||v_k||^2 is delivered by a DEDICATED 128 x 20-bit distributed-RAM ROM in
 *   the VQ branch, written over AXI-lite at ADDR_VQ_NORM (0x038) and addressed
 *   {batch[1:0], oc[4:0]} in absolute OC order.
 *
 *   THIS PARAGRAPH USED TO SAY the norm rides the existing per-OC param-BRAM
 *   bias read, with no new storage. That was the plan and it is NOT what was
 *   built. The bias path had an off-by-one -- the last output channel of every
 *   batch received the previous channel's bias, mult and shift (DEFECT P1 in
 *   pw_pixel_major_core.sv) -- so the norm was given its own ROM rather than
 *   made to depend on it. P1 has since been fixed and tb_pw_bias_align.sv now
 *   reports skewed = 0, but the ROM stays: it is verified, it costs ~40
 *   LUTRAM, and it keeps VQ independent of the requantiser parameter path
 *   entirely. Do not "restore" the bias-path version.
 *
 * LAYOUTS
 *   latent   : group-major/channel-minor, as the accelerator writes it.
 *              channel c of lane l in group g is at (g*DIM + c)*LANES + l.
 *   codebook : int8, PRE-CENTRED (v = cq - z0), cb[m][k][d], M*K*DSUB = 1024 B.
 *   idx_out  : one 32-bit little-endian word per position, position-major.
 *              bits [4m+3 : 4m] = index of sub-codebook m, so
 *              [3:0]=idx0, [7:4]=idx1, ... [31:28]=idx7.
 *              14,400 * 4 = 57,600 B/frame, same as the M=4/K=256 path.
 * ==========================================================================*/

#include <stdint.h>

#define VQPW_DIM        64
#define VQPW_M           8
#define VQPW_DSUB        8      /* VQPW_DIM / VQPW_M */
#define VQPW_K          16      /* index fits 4 bits */
#define VQPW_LANES       8      /* pixels per group, matches the accelerator */

#define VQPW_MAP_W     160
#define VQPW_MAP_H      90
#define VQPW_NPOS      (VQPW_MAP_W * VQPW_MAP_H)      /* 14,400 */
#define VQPW_NGROUPS   (VQPW_NPOS / VQPW_LANES)       /*  1,800 */
#define VQPW_IDX_BYTES (VQPW_NPOS * 4)                /* 57,600 */

/* ---- realized PW engine geometry (hw.bd / *.xci), not RTL defaults ---- */
#define VQPW_N_OC        32     /* pw N_OC  */
#define VQPW_N_LANES      8     /* pw N_LANES */
#define VQPW_SUBS_PER_BATCH (VQPW_N_OC / VQPW_K)      /* 2 */
#define VQPW_NBATCH      (VQPW_M / VQPW_SUBS_PER_BATCH) /* 4 */
#define VQPW_CIN_MAC     (VQPW_SUBS_PER_BATCH * VQPW_DSUB) /* 16 */
#define VQPW_CIN_LOAD    VQPW_DIM                     /* 64 */
#define VQPW_COUT_TOTAL  (VQPW_NBATCH * VQPW_N_OC)    /* 128 */

/* Weight image: bank = oc, address = b*VQPW_CIN_MAC + ic. */
#define VQPW_W_PER_BANK  (VQPW_NBATCH * VQPW_CIN_MAC) /* 64 */
#define VQPW_W_BYTES     (VQPW_N_OC * VQPW_W_PER_BANK)/* 2,048 */

/* Score width, DERIVED (see vq_pw.c for the derivation and the runtime check). */
#define VQPW_SCORE_BITS  20
#define VQPW_SCORE_MAX   391168L
#define VQPW_SCORE_MIN  (-262144L)

typedef struct {
    const int8_t *codebook;                 /* [m][k][d], pre-centred */
    int32_t       norm2[VQPW_M * VQPW_K];   /* ||v_k||^2, 128 entries */
    uint8_t       zp;                       /* must be 128 */
    /* instrumentation -- observed extremes, for the width claim */
    int32_t       obs_score_min, obs_score_max;
    int32_t       obs_acc_min,   obs_acc_max;
} vqpw_ctx_t;

/* Precompute ||v_k||^2. Returns 0 on success, <0 if zp != 128 (see above).
 * codebook must remain valid for the lifetime of ctx. */
int  vqpw_init(vqpw_ctx_t *ctx, const int8_t *codebook, uint8_t zp);

/* Bit-exact reference for one 8-D subvector: returns argmin k, lowest index
 * wins on ties (matches the RTL's strict-less-than comparator). If score_out
 * is non-NULL it receives all VQPW_K scores. */
int  vqpw_search_sub(vqpw_ctx_t *ctx, const int8_t *u, int m,
                     int32_t *score_out);

/* Encode one frame. latent is the accelerator's final-PW output. */
void vqpw_encode_frame(vqpw_ctx_t *ctx, const uint8_t *latent,
                       uint8_t *idx_out);

/* Gather one position's centred 64-D vector, reproducing vq_pq.c's rule. */
void vqpw_gather(const uint8_t *latent, int g, int l, uint8_t zp, int8_t *u);

/* ---- host-side images for the PW engine ---- */

/* Block-diagonal weight image, w[oc][b*VQPW_CIN_MAC + ic]. Signed INT8.
 * out must hold VQPW_W_BYTES. */
void vqpw_build_weights(const vqpw_ctx_t *ctx, int8_t *out);

/* ||v_k||^2 in absolute-OC order: out[b*VQPW_N_OC + oc]. VQPW_COUT_TOTAL
 * entries, loaded through the existing param-BRAM bias port. */
void vqpw_build_norms(const vqpw_ctx_t *ctx, int32_t *out);

/* Which (m,k) a given batch/oc evaluates, and which input window it reads.
 * Returns the sub-codebook m; *k_out gets the codeword, *ic_lo the first
 * pb_ram offset within the batch's 16-entry window. */
int  vqpw_map_oc(int batch, int oc, int *k_out, int *ic_lo);

#endif /* VQ_PW_H */
