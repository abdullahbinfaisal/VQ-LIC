/* ============================================================================
 * vq_pw_pl_prog_test.c -- does the DRIVER program the engine with the same
 * image the RTL bench was proved against?
 *
 * tb_pw_axi_vq.sv established that a particular weight image and norm table,
 * written through a particular register sequence, make the hardware produce
 * bit-exact indices. Nothing so far checks that vq_pw_pl.c emits THAT image
 * and THAT sequence -- a transposed bank index or a mis-shifted norm field
 * would compile, run, and silently produce wrong indices on the board.
 *
 * This replays the driver's AXI-lite writes into a model of the weight banks
 * and the norm ROM and compares against vqpw_build_weights() /
 * vqpw_build_norms() directly.
 *
 *   gcc -O2 -I<stubdir> -I../src -o vq_pw_pl_prog_test \
 *       vq_pw_pl_prog_test.c ../src/vq_pw_pl.c ../src/vq_pw.c
 *
 * The stub headers (xil_io.h, xil_cache.h, xil_printf.h) capture every write
 * instead of touching hardware; see the harness that builds this.
 * ==========================================================================*/
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "vq_pw_pl.h"

/* ---- the write log the stub fills ---------------------------------------- */
#define MAXW 8192
typedef struct { uint32_t addr, data; } wr_t;
extern wr_t  g_wr[MAXW];
extern int   g_nwr;

#define PW_REG_OC_SEL       0x024u
#define PW_REG_W_BRAM_OFF   0x02Cu
#define PW_REG_VQ_CTRL      0x034u
#define PW_REG_VQ_NORM      0x038u
#define PW_REG_W_BASE       0x100u
#define PW_REG_CIN_RUN      0x00Cu
#define PW_REG_COUT_RUN     0x028u
#define PW_REG_TILE_PIXELS  0x008u
#define PW_REG_ZP_RELU      0x010u
#define PW_REG_CTRL         0x000u

static uint32_t st = 0x0BADC0DEu;
static uint32_t xs(void){ uint32_t x=st; x^=x<<13; x^=x>>17; x^=x<<5; return st=x; }

static int8_t  cb  [VQPW_M * VQPW_K * VQPW_DSUB];
static int8_t  ref_w[VQPW_W_BYTES];
static int32_t ref_n[VQPW_COUT_TOTAL];

/* model of what the hardware would end up holding */
static int  seen_w[VQPW_N_OC][VQPW_W_PER_BANK];
static int  seen_n[VQPW_COUT_TOTAL];

static int fails = 0;
static void chk(int cond, const char *what)
{
    printf("  %-58s %s\n", what, cond ? "ok" : "FAIL");
    if (!cond) fails++;
}

int main(void)
{
    vqpw_ctx_t ctx;
    for (size_t i = 0; i < sizeof cb; i++) cb[i] = (int8_t)(xs() & 0xFF);

    if (vqpw_init(&ctx, cb, 128) != 0) { printf("init failed\n"); return 1; }
    vqpw_build_weights(&ctx, ref_w);
    vqpw_build_norms(&ctx, ref_n);

    printf("vq_pw_pl driver programming check\n");

    g_nwr = 0;
    int rc = vq_pw_pl_load_codebook(cb, 128);
    chk(rc == 0, "load_codebook accepts zp=128");
    chk(g_nwr > 0, "driver emitted AXI-lite writes");

    /* ---- replay ---------------------------------------------------------- */
    memset(seen_w, 0xFF, sizeof seen_w);
    memset(seen_n, 0xFF, sizeof seen_n);
    int cur_oc = -1, cur_off = 0, nnorm = 0, nweight = 0;
    for (int i = 0; i < g_nwr; i++) {
        const uint32_t a = g_wr[i].addr, d = g_wr[i].data;
        if (a == PW_REG_OC_SEL)          cur_oc  = (int)d;
        else if (a == PW_REG_W_BRAM_OFF) cur_off = (int)d;
        else if (a >= PW_REG_W_BASE && a < PW_REG_W_BASE + VQPW_W_PER_BANK * 4u) {
            const int idx = cur_off + (int)((a - PW_REG_W_BASE) >> 2);
            if (cur_oc >= 0 && cur_oc < VQPW_N_OC && idx < VQPW_W_PER_BANK) {
                seen_w[cur_oc][idx] = (int)(int8_t)(uint8_t)d;
                nweight++;
            }
        } else if (a == PW_REG_VQ_NORM) {
            /* the address field is EIGHT bits now (0..255), so at the
             * deployed geometry this covers OCs the old 7-bit mask could
             * not even express */
            const int oc = (int)(d & 0xFFu);
            int32_t v = (int32_t)((d >> 12) & 0xFFFFFu);
            if (v & 0x80000) v -= 0x100000;          /* 20-bit sign extend */
            seen_n[oc] = v; nnorm++;
        }
    }
    chk(nweight == VQPW_N_OC * VQPW_W_PER_BANK, "every weight entry written exactly once");
    chk(nnorm   == VQPW_COUT_TOTAL,             "every codeword norm written exactly once");

    /* ---- weights match the reference image ------------------------------- */
    int wbad = 0, wfirst = -1;
    for (int oc = 0; oc < VQPW_N_OC; oc++)
        for (int a = 0; a < VQPW_W_PER_BANK; a++) {
            const int want = (int)ref_w[(size_t)oc * VQPW_W_PER_BANK + a];
            if (seen_w[oc][a] != want) { if (wfirst < 0) wfirst = oc * VQPW_W_PER_BANK + a; wbad++; }
        }
    if (wbad) printf("    first weight mismatch at flat index %d\n", wfirst);
    chk(wbad == 0, "weight banks match vqpw_build_weights() exactly");

    /* ---- norms match, and survive the 20-bit field round trip ------------ */
    int nbad = 0, nfirst = -1;
    for (int i = 0; i < VQPW_COUT_TOTAL; i++)
        if (seen_n[i] != ref_n[i]) { if (nfirst < 0) nfirst = i; nbad++; }
    if (nbad) printf("    first norm mismatch at OC %d: got %d want %d\n",
                     nfirst, seen_n[nfirst], ref_n[nfirst]);
    chk(nbad == 0, "norm ROM matches vqpw_build_norms() after 20-bit packing");

    /* ---- the norms really do need 20 bits, and they fit ------------------ */
    int32_t nmin = ref_n[0], nmax = ref_n[0];
    for (int i = 1; i < VQPW_COUT_TOTAL; i++) {
        if (ref_n[i] < nmin) nmin = ref_n[i];
        if (ref_n[i] > nmax) nmax = ref_n[i];
    }
    printf("    norm range [%d, %d]\n", (int)nmin, (int)nmax);
    chk(nmin >= -524288 && nmax <= 524287, "norms fit the 20-bit signed field");

    /* ---- geometry and mode ----------------------------------------------- */
    g_nwr = 0;
    uint8_t dummy_latent = 0, dummy_idx = 0;
    vq_pw_pl_start(&dummy_latent, &dummy_idx);

    int i_ctrl = -1, i_vq = -1, saw_cin = 0, saw_cout = 0, saw_tp = 0, saw_zp = 0;
    uint32_t vqval = 0;
    for (int i = 0; i < g_nwr; i++) {
        const uint32_t a = g_wr[i].addr, d = g_wr[i].data;
        if (a == PW_REG_CIN_RUN)          saw_cin  = (d == VQPW_CIN_MAC);
        else if (a == PW_REG_COUT_RUN)    saw_cout = (d == VQPW_COUT_TOTAL);
        else if (a == PW_REG_TILE_PIXELS) saw_tp   = (d == (uint32_t)VQPW_NPOS);
        else if (a == PW_REG_ZP_RELU)     saw_zp   = (d == 0x00008080u);
        else if (a == PW_REG_VQ_CTRL)   { i_vq = i; vqval = d; }
        else if (a == PW_REG_CTRL)      { if (i_ctrl < 0) i_ctrl = i; }
    }
    chk(saw_cin,  "CIN_RUN = 16 (the MAC window, not the streamed channels)");
    chk(saw_cout, "COUT_RUN = M*K (every codeword is an output channel)");
    chk(saw_tp,   "TILE_PIXELS = 14400 latent positions");
    chk(saw_zp,   "ZP_RELU = zp_in 128, zp_out 128, relu off");
    chk((vqval & 1u) == 1u, "VQ_CTRL sets vq_mode");
    chk(((vqval >> 12) & 0xFFFu) == (uint32_t)VQPW_CIN_LOAD,
        "VQ_CTRL carries vq_cin_load = 64");
    chk(i_vq >= 0 && i_ctrl >= 0 && i_vq < i_ctrl,
        "vq_mode is set BEFORE start (mode selects the AXIS pair)");

    printf("\n%d checks failed\n", fails);
    printf(fails ? "RESULT: FAIL\n" : "RESULT: PASS\n");
    return fails != 0;
}
