/* ============================================================================
 * gen_vq_vectors.c -- emit $readmemh vectors for tb_pw_vq.sv from the SAME
 * golden model the firmware will use, so the RTL is checked against
 * vqpw_encode_frame() and not against a second re-derivation.
 *
 *   gcc -O2 -I../src -o gen_vq_vectors gen_vq_vectors.c ../src/vq_pw.c
 *   ./gen_vq_vectors <ngroups> <scenario> <outdir>
 *
 * scenario: 0 = random, 1 = tie storm (codewords duplicated in pairs, so every
 *           search has co-minimal candidates and the lowest index must win),
 *           2 = INT8 extremes (latent and codebook at the corners)
 *
 * Files (all hex, one value per line):
 *   latent.hex   ngroups*64  x 64-bit stream beats, in the order the DMA
 *                            delivers them: for each group, one beat per
 *                            channel, lane l in byte l.
 *   weights.hex  32*64       x 8-bit, oc-major: line (oc*64 + addr), where
 *                            addr = w_addr_base + ic = 16*batch + ic.
 *   norms.hex    128         x 20-bit, absolute OC order (batch*32 + oc).
 *   expect.hex   ngroups*4   x 64-bit expected output beats: two packed
 *                            32-bit positions per beat, low half = lower
 *                            position (little-endian, as the S2MM writes it).
 * ==========================================================================*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vq_pw.h"

static uint32_t st = 0x2468ACEu;
static uint32_t xs(void){ uint32_t x=st; x^=x<<13; x^=x>>17; x^=x<<5; return st=x; }

static int8_t  cb[VQPW_M * VQPW_K * VQPW_DSUB];
static int8_t  wimg[VQPW_W_BYTES];
static int32_t nimg[VQPW_COUT_TOTAL];
static uint8_t latent[(size_t)VQPW_NGROUPS * VQPW_DIM * VQPW_LANES];
static uint8_t idx[VQPW_IDX_BYTES];

static FILE *xopen(const char *dir, const char *name)
{
    char path[512];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    FILE *f = fopen(path, "w");
    if (!f) { perror(path); exit(1); }
    return f;
}

int main(int argc, char **argv)
{
    const int ng       = (argc > 1) ? atoi(argv[1]) : 64;
    const int scenario = (argc > 2) ? atoi(argv[2]) : 0;
    const char *dir    = (argc > 3) ? argv[3] : ".";
    vqpw_ctx_t ctx;

    if (ng < 1 || ng > VQPW_NGROUPS) { fprintf(stderr, "bad ngroups\n"); return 1; }
    st = 0x2468ACEu + (uint32_t)scenario * 7919u;

    /* ---- codebook ---- */
    for (int i = 0; i < VQPW_M * VQPW_K * VQPW_DSUB; i++) cb[i] = (int8_t)(xs() & 0xFF);
    if (scenario == 1) {
        for (int m = 0; m < VQPW_M; m++)
            for (int k = 0; k < VQPW_K; k += 2)
                memcpy(cb + ((size_t)m * VQPW_K + k + 1) * VQPW_DSUB,
                       cb + ((size_t)m * VQPW_K + k)     * VQPW_DSUB, VQPW_DSUB);
    } else if (scenario == 2) {
        for (int i = 0; i < VQPW_M * VQPW_K * VQPW_DSUB; i++)
            cb[i] = (int8_t)((xs() & 1) ? -128 : 127);
    }
    if (vqpw_init(&ctx, cb, 128) != 0) { fprintf(stderr, "init failed\n"); return 1; }
    vqpw_build_weights(&ctx, wimg);
    vqpw_build_norms(&ctx, nimg);

    /* ---- latent ---- */
    const size_t nbytes = (size_t)ng * VQPW_DIM * VQPW_LANES;
    for (size_t i = 0; i < nbytes; i++) {
        latent[i] = (scenario == 2) ? (uint8_t)((xs() & 1) ? 255u : 0u)
                                    : (uint8_t)(xs() & 0xFF);
    }
    vqpw_encode_frame(&ctx, latent, idx);   /* full-frame model; we use ng groups */

    /* ---- latent.hex : stream beats ---- */
    FILE *f = xopen(dir, "latent.hex");
    for (int g = 0; g < ng; g++)
        for (int c = 0; c < VQPW_DIM; c++) {
            const uint8_t *b = latent + ((size_t)g * VQPW_DIM + c) * VQPW_LANES;
            /* lane l in byte l -> byte 0 is the LOW byte of the 64-bit beat */
            for (int l = VQPW_LANES - 1; l >= 0; l--) fprintf(f, "%02x", b[l]);
            fputc('\n', f);
        }
    fclose(f);

    /* ---- weights.hex : oc-major ---- */
    f = xopen(dir, "weights.hex");
    for (int oc = 0; oc < VQPW_N_OC; oc++)
        for (int a = 0; a < VQPW_W_PER_BANK; a++)
            fprintf(f, "%02x\n",
                    (unsigned)(uint8_t)wimg[(size_t)oc * VQPW_W_PER_BANK + a]);
    fclose(f);

    /* ---- norms.hex : absolute OC order ---- */
    f = xopen(dir, "norms.hex");
    for (int i = 0; i < VQPW_COUT_TOTAL; i++)
        fprintf(f, "%05x\n", (unsigned)(nimg[i] & 0xFFFFF));
    fclose(f);

    /* ---- expect.hex : two positions per 64-bit beat ---- */
    f = xopen(dir, "expect.hex");
    for (int g = 0; g < ng; g++)
        for (int beat = 0; beat < VQPW_LANES / 2; beat++) {
            const int p0 = g * VQPW_LANES + 2 * beat;
            const int p1 = p0 + 1;
            uint32_t w0 = 0, w1 = 0;
            for (int j = 3; j >= 0; j--) {
                w0 = (w0 << 8) | idx[(size_t)p0 * 4 + j];
                w1 = (w1 << 8) | idx[(size_t)p1 * 4 + j];
            }
            fprintf(f, "%08x%08x\n", (unsigned)w1, (unsigned)w0); /* hi = p1 */
        }
    fclose(f);

    printf("scenario %d: %d groups, %d positions -> %s\n",
           scenario, ng, ng * VQPW_LANES, dir);
    return 0;
}
