#ifndef VQ_PW_H
#define VQ_PW_H
/* ============================================================================
 * vq_pw.h -- golden model for PW-ENGINE-HOSTED product quantisation.
 *
 * Replaces the dedicated VQ engine's M=4/K=256/DSUB=16 search with a search
 * evaluated on the existing INT8 pointwise-convolution engine.
 *
 * TWO PROFILES, selected by VQPW_PROFILE. Everything else is derived; no
 * free-standing geometry constant is left in this header.
 *
 *   VQPW_PROFILE = 1  DEPLOYED   M=4, K=64, Dsub=16  -> 24 bits/position
 *   VQPW_PROFILE = 0  LEGACY     M=8, K=16, Dsub=8   -> 32 bits/position
 *
 * The legacy profile is kept as a regression mode: it is the geometry the
 * 2026-09-05 board run measured (T_VQ_ACC = 2.4505 ms against a 2.4480 ms
 * model), so a change that breaks it is visible immediately.
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
 * HARDWARE MAPPING -- one rule, both profiles.
 *   Write abs = batch*N_OC + oc for the absolute output channel. Then
 *       m = abs / K      which sub-codebook this channel scores
 *       k = abs % K      which codeword within it
 *   and the batch's pb_ram window starts at DSUB * (batch*N_OC / K).
 *
 *   K = 16 < N_OC = 32  (legacy): ONE batch holds TWO whole sub-codebooks,
 *       batch b, oc  0..15 -> m = 2b,   input dims 16b+0 .. 16b+7
 *       batch b, oc 16..31 -> m = 2b+1, input dims 16b+8 .. 16b+15
 *       4 batches cover M = 8. Because cin_mac = 16 but each output channel
 *       only uses DSUB = 8 of those dimensions, HALF of every channel's
 *       weights are structural zeros.
 *
 *   K = 64 > N_OC = 32  (deployed): ONE sub-codebook spans TWO batches,
 *       batch b, oc 0..31 -> m = b/2, k = 32*(b%2) + oc
 *       8 batches cover M = 4, and the input window HOLDS STILL across
 *       batches 2m and 2m+1 because both score the same 16 dimensions.
 *       DSUB = 16 = cin_mac exactly, so there are NO structural zeros here:
 *       every weight the MAC reads is a real codeword coefficient, and the
 *       MAC utilisation of the VQ mapping is 100%% rather than 50%%.
 *
 *   In both cases the engine streams the full 64-channel group
 *   (cin_load = 64, one contiguous DDR read) and each batch walks cin_mac = 16
 *   pb_ram entries.
 *
 *   ||v_k||^2 is delivered by a DEDICATED VQPW_NORM_D x VQPW_SCORE_BITS
 *   distributed-RAM ROM in the VQ branch, written over AXI-lite at
 *   ADDR_VQ_NORM (0x038) and addressed by ABSOLUTE OC. That address field is
 *   now 8 bits (0..255); it was 7 when only 128 norms were needed, and
 *   0..127 decode identically, so the legacy profile sees no change.
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
 *              Sub-codebook m occupies bits [KW*m + KW-1 : KW*m], KW=log2(K).
 *              The transport word is 32 bits at EVERY K, so idx_out is always
 *              14,400 * 4 = 57,600 B/frame and the S2MM length, the cache
 *              ranges and the 4-beats-per-group output count never change.
 *
 *              K = 16, M = 8 : eight nibbles, [3:0]=idx0 .. [31:28]=idx7.
 *                              All 32 bits carry index.
 *              K = 64, M = 4 : four 6-bit fields, [5:0]=idx0, [11:6]=idx1,
 *                              [17:12]=idx2, [23:18]=idx3.
 *                              BITS [31:24] ARE ZERO -- the RTL masks them
 *                              structurally (VQ_WORD_MASK in
 *                              pw_pixel_major_core.sv), so a reader may rely
 *                              on the top byte being 0x00. The RATE is
 *                              24 bits/position, not 32, even though the
 *                              transport still moves 4 bytes.
 *              Use vqpw_get_index()/vqpw_put_index() rather than open-coding
 *              the shift; they are the only place the packing is written down
 *              on the software side.
 * ==========================================================================*/

#include <stdint.h>
#include <stddef.h>   /* size_t -- not pulled in by stdint on arm-none-eabi */

#define VQPW_DIM        64

/* ---- profile selection ---------------------------------------------------
 * 1 = DEPLOYED M=4,K=64,Dsub=16 ; 0 = LEGACY M=8,K=16,Dsub=8 (regression).
 * The RTL must be elaborated to match -- VQ_K / VQ_NORM_D / VQ_SCORE_W on
 * pw_single_oc_axis_axi. There is no way for software to read those back, so
 * keep the two in step by hand and run vq_pw_pl_verify() at bring-up: a
 * mismatch shows up as index mismatches on the very first frame. */
#ifndef VQPW_PROFILE
#define VQPW_PROFILE     1
#endif

#if VQPW_PROFILE
#  define VQPW_M         4
#  define VQPW_K        64      /* index fits 6 bits */
#  define VQPW_KW        6
#else
#  define VQPW_M         8
#  define VQPW_K        16      /* index fits 4 bits */
#  define VQPW_KW        4
#endif

#define VQPW_DSUB       (VQPW_DIM / VQPW_M)          /* 16 or 8 */
#define VQPW_KMASK      ((1u << VQPW_KW) - 1u)
#define VQPW_LANES       8      /* pixels per group, matches the accelerator */

#define VQPW_MAP_W     160
#define VQPW_MAP_H      90
#define VQPW_NPOS      (VQPW_MAP_W * VQPW_MAP_H)      /* 14,400 */
#define VQPW_NGROUPS   (VQPW_NPOS / VQPW_LANES)       /*  1,800 */
#define VQPW_IDX_BYTES (VQPW_NPOS * 4)                /* 57,600 */

/* ---- realized PW engine geometry (hw.bd / *.xci), not RTL defaults ---- */
#define VQPW_N_OC        32     /* pw N_OC  */
#define VQPW_N_LANES      8     /* pw N_LANES */

/* Sub-codebooks that fit in ONE batch of N_OC channels (K <= N_OC), and
 * batches spanned by ONE sub-codebook (K > N_OC). Exactly one of the two
 * exceeds 1; the other is 1. These mirror VQ_BPS / the packing in the RTL. */
#define VQPW_SUBS_PER_BATCH  ((VQPW_K < VQPW_N_OC) ? (VQPW_N_OC / VQPW_K) : 1)
#define VQPW_BATCH_PER_SUB   ((VQPW_K > VQPW_N_OC) ? (VQPW_K / VQPW_N_OC) : 1)

#define VQPW_COUT_TOTAL  (VQPW_M * VQPW_K)                  /* 256 or 128 */
#define VQPW_NBATCH      (VQPW_COUT_TOTAL / VQPW_N_OC)      /*   8 or   4 */
#define VQPW_CIN_MAC     (VQPW_DSUB * VQPW_SUBS_PER_BATCH)  /*  16 in both */
#define VQPW_CIN_LOAD    VQPW_DIM                           /*  64 */
#define VQPW_NORM_D      VQPW_COUT_TOTAL                    /* norm ROM depth */

/* Weight image: bank = oc, address = b*VQPW_CIN_MAC + ic. */
#define VQPW_W_PER_BANK  (VQPW_NBATCH * VQPW_CIN_MAC) /* 128 or 64      */
#define VQPW_W_BYTES     (VQPW_N_OC * VQPW_W_PER_BANK)/* 4,096 or 2,048 */

/* Index bits actually carried per latent position; the remaining bits of the
 * 32-bit transport word are zero. 24 at K=64/M=4, 32 at K=16/M=8. */
#define VQPW_BITS_PER_POS (VQPW_M * VQPW_KW)

/* Score width, DERIVED -- and it depends on DSUB, NOT on K. Per dimension
 * ||v||^2 - 2uv is maximised at u=+127, v=-128 (16384 + 2*127*128 = 48,896)
 * and minimised at v=u=-128 (16384 - 32768 = -16,384), so over DSUB dimensions
 *     score in [ -16384*DSUB , +48896*DSUB ].
 * A signed N-bit register needs 2^(N-1) > 48896*DSUB:
 *     DSUB =  8 -> max  391,168 -> 2^19 = 524,288   -> N = 20  (legacy)
 *     DSUB = 16 -> max  782,336 -> 2^20 = 1,048,576 -> N = 21  (deployed)
 * DSUB = 16 OVERFLOWS the old 20-bit width. The RTL parameter VQ_SCORE_W must
 * equal VQPW_SCORE_BITS, and a_vq_score_fits in pw_pixel_major_core.sv checks
 * the bound on every single compare in simulation.
 *
 * ||v_k||^2 by itself is 0 .. 16384*DSUB = 262,144 at DSUB=16, which still
 * fits the 20-bit AXI norm field -- only the SCORE needed widening, so the
 * ADDR_VQ_NORM data field is unchanged. */
#define VQPW_SCORE_MAX   (48896L * (long)VQPW_DSUB)
#define VQPW_SCORE_MIN  (-16384L * (long)VQPW_DSUB)
#if VQPW_DSUB <= 8
#  define VQPW_SCORE_BITS 20
#else
#  define VQPW_SCORE_BITS 21
#endif

/* The engine cannot execute a c_out it has no weight batches for. This is the
 * same ceiling the RTL now enforces at S_IDLE (COUT_HW_MAX). */
#define VQPW_PW_COUT_MAX 256    /* (COUT_MAX/N_OC)*N_OC at COUT_MAX = 256 */

typedef struct {
    const int8_t *codebook;                 /* [m][k][d], pre-centred */
    int32_t       norm2[VQPW_M * VQPW_K];   /* ||v_k||^2, VQPW_NORM_D entries */
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
 * pb_ram offset WITHIN the batch's VQPW_CIN_MAC-entry window. At K > N_OC
 * *ic_lo is always 0 (the window is exactly the sub-vector); at K < N_OC it
 * is DSUB * (which half of the batch this channel is in). */
int  vqpw_map_oc(int batch, int oc, int *k_out, int *ic_lo);

/* First pb_ram entry the given BATCH reads, in input channels. Advances by
 * DSUB once per sub-codebook, i.e. every VQPW_BATCH_PER_SUB batches -- this
 * is what vq_pb_base does in the RTL. */
int  vqpw_batch_window(int batch);

/* ---- index packing -- the ONLY software definition of the wire format ----
 * idx points at VQPW_IDX_BYTES; pos is 0..VQPW_NPOS-1; m is 0..VQPW_M-1.
 * Reads/writes the KW-bit field at bit KW*m of the position's 32-bit
 * little-endian word. Bits at and above VQPW_BITS_PER_POS are left zero. */
uint8_t vqpw_get_index(const uint8_t *idx, int pos, int m);
void    vqpw_put_index(uint8_t *idx, int pos, int m, uint8_t k);

/* Compile-time self-consistency of the derived geometry. Returns 0 if this
 * build is coherent, or a negative code naming the first broken invariant.
 * Call it once at bring-up: it is the cheapest way to catch a profile edit
 * that the preprocessor accepted but the hardware cannot execute. */
int  vqpw_check_build(void);

#endif /* VQ_PW_H */
