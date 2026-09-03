/* ============================================================================
 * gen_conv_vectors.c -- vectors for tb_pw_axis_conv.sv, the ORDINARY
 * CONVOLUTION control experiment.
 *
 *   gcc -O2 -o gen_conv_vectors gen_conv_vectors.c
 *   ./gen_conv_vectors <ngroups> <outdir>
 *
 * WHY THIS EXISTS
 *   The PW datapath's arithmetic has never been data-checked (see the note in
 *   ppu.sv: "this design has never had a data check"). Every measurement to
 *   date verified timing. The VQ bit-exactness result does not cover it
 *   either -- vq_pl_verify compared PL-VQ against NEON-VQ on the SAME latent,
 *   so a numerically wrong latent would have been agreed upon by both.
 *
 *   So before blaming the VQ branch for a failing bench, run plain
 *   convolution through the SAME structural path and see whether it is right.
 *
 * THE IDENTITY-PPU TRICK
 *   The PPU computes  out = clamp(zp_out +/- round(|(acc + bias) * mult| >> shift)).
 *   With bias = 0, mult = 1 << 16, shift = 16 the product is acc << 16 and the
 *   shift is exact, so no rounding occurs at all and
 *       out = clamp(acc + zp_out, 0, 255).
 *   Operands are chosen so |acc| stays well under 128 and the clamp never
 *   engages, giving  out = acc + 128  exactly. Any mismatch is therefore in
 *   the MAC / accumulator / shadow-copy path and nowhere else -- which is
 *   precisely the question that has to be answered.
 *
 * Files:
 *   conv_latent.hex   ngroups*CIN x 64-bit stream beats (lane l in byte l)
 *   conv_weights.hex  N_OC*CIN    x 8-bit, oc-major
 *   conv_expect.hex   ngroups*COUT x 64-bit expected output beats
 *                     (one beat per output channel, lane l in byte l)
 * ==========================================================================*/
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

#define LANES 8
#define CIN   16
#define COUT  32
#define ZP    128

static uint32_t st = 0x13579BDFu;
static uint32_t xs(void){ uint32_t x=st; x^=x<<13; x^=x>>17; x^=x<<5; return st=x; }

static uint8_t  lat[4096][CIN][LANES];   /* [group][channel][lane] */
static int8_t   w  [COUT][CIN];

static FILE *xopen(const char *d, const char *n)
{
    char p[512]; snprintf(p, sizeof p, "%s/%s", d, n);
    FILE *f = fopen(p, "w"); if (!f) { perror(p); exit(1); } return f;
}

int main(int argc, char **argv)
{
    const int ng    = (argc > 1) ? atoi(argv[1]) : 64;
    const char *dir = (argc > 2) ? argv[2] : ".";
    if (ng < 1 || ng > 4096) { fprintf(stderr, "bad ngroups\n"); return 1; }

    /* small operands: u in [-3,3], w in [-2,2] -> |acc| <= 16*3*2 = 96 < 128,
     * so the output clamp never engages and out = acc + 128 exactly. */
    for (int g = 0; g < ng; g++)
        for (int c = 0; c < CIN; c++)
            for (int l = 0; l < LANES; l++)
                lat[g][c][l] = (uint8_t)(ZP - 3 + (int)(xs() % 7u));
    for (int oc = 0; oc < COUT; oc++)
        for (int c = 0; c < CIN; c++)
            w[oc][c] = (int8_t)((int)(xs() % 5u) - 2);

    FILE *f = xopen(dir, "conv_latent.hex");
    for (int g = 0; g < ng; g++)
        for (int c = 0; c < CIN; c++) {
            for (int l = LANES - 1; l >= 0; l--) fprintf(f, "%02x", lat[g][c][l]);
            fputc('\n', f);
        }
    fclose(f);

    f = xopen(dir, "conv_weights.hex");
    for (int oc = 0; oc < COUT; oc++)
        for (int c = 0; c < CIN; c++)
            fprintf(f, "%02x\n", (unsigned)(uint8_t)w[oc][c]);
    fclose(f);

    long amin = 1 << 30, amax = -(1 << 30);
    f = xopen(dir, "conv_expect.hex");
    for (int g = 0; g < ng; g++)
        for (int oc = 0; oc < COUT; oc++) {
            for (int l = LANES - 1; l >= 0; l--) {
                int32_t acc = 0;
                for (int c = 0; c < CIN; c++)
                    acc += ((int32_t)lat[g][c][l] - ZP) * (int32_t)w[oc][c];
                if (acc < amin) amin = acc;
                if (acc > amax) amax = acc;
                int32_t o = acc + ZP;
                if (o < 0) o = 0; else if (o > 255) o = 255;
                fprintf(f, "%02x", (unsigned)o);
            }
            fputc('\n', f);
        }
    fclose(f);

    printf("conv vectors: %d groups, cin=%d cout=%d -> %s\n", ng, CIN, COUT, dir);
    printf("acc range [%ld, %ld]  (must stay inside [-128,127] so the clamp is inert)\n",
           amin, amax);
    if (amin < -128 || amax > 127) {
        fprintf(stderr, "ERROR: acc escaped the no-clamp window; reference would be ambiguous\n");
        return 1;
    }
    return 0;
}
