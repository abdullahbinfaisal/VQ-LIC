/* Stages 0 and 1 of RANS_GUIDE.md 12: the two test vectors of section 13.
 * No model, no tables, no image. Each must match BYTE FOR BYTE. */
#include <stdio.h>
#include <string.h>
#include "rans.h"

static int fails = 0;

static void show(const char *tag, const uint8_t *b, size_t n)
{
    printf("  %-9s", tag);
    for (size_t i = 0; i < n; i++) printf(" %02X", (unsigned)b[i]);
    printf("   (%u bytes)\n", (unsigned)n);
}

static void verdict(int ok, const char *what)
{
    printf("  -> %-34s %s\n", what, ok ? "MATCH" : "MISMATCH");
    if (!ok) fails++;
}

int main(void)
{
    uint8_t buf[64];
    const uint8_t *s = 0;

    /* ---- STAGE 0: vector A, static rANS, K = 4 ---------------------------- */
    static const uint16_t A_freq[4] = { 32768, 16384, 8192, 8192 };
    static const uint16_t A_cdf [4] = {     0, 32768, 49152, 57344 };
    static const uint8_t  A_sym [8] = { 0, 1, 2, 3, 0, 0, 1, 2 };
    static const uint8_t  A_out [6] = { 0x00, 0x80, 0x1F, 0x8E, 0x00, 0x00 };

    printf("STAGE 0 -- static rANS, test vector A (K=4, prob_bits=16)\n");
    const size_t na = rans_encode_static(A_freq, A_cdf, 4u, A_sym, 8u, buf, sizeof buf, &s);
    show("expected", A_out, sizeof A_out);
    show("produced", s ? s : buf, s ? na : 0u);
    verdict(s && na == sizeof A_out && memcmp(s, A_out, na) == 0, "vector A bytes");
    printf("  leading byte 0x%02X kept (guide 5: not stripped)\n", s ? (unsigned)s[0] : 0u);

    uint8_t dA[8];
    const int ra = rans_decode_static(A_freq, A_cdf, 4u, A_out, sizeof A_out, dA, 8u);
    show("decoded", dA, 8u);
    verdict(ra == 0 && memcmp(dA, A_sym, 8) == 0, "vector A decode, pointer on EOF");

    /* ---- STAGE 1: vector B, context rANS, K = 4, 2x4 grid ----------------- */
    static const uint16_t B_t0 [4] = { 40000, 15000,  8000,  2536 };   /* ctx 0..3 */
    static const uint16_t B_t1 [4] = {  2536,  8000, 15000, 40000 };   /* ctx 4    */
    static const uint16_t B_c0 [4] = {     0, 40000, 55000, 63000 };
    static const uint16_t B_c1 [4] = {     0,  2536, 10536, 25536 };
    static const uint8_t  B_idx[8] = { 0, 1, 2, 3,  3, 2, 1, 0 };
    static const uint8_t  B_ctx[8] = { 4, 0, 1, 2,  4, 3, 2, 1 };
    static const uint8_t  B_out[6] = { 0x11, 0x89, 0x05, 0x3B, 0x66, 0x10 };

    uint16_t rows[5 * 2 * 4];
    for (int c = 0; c < 4; c++) {
        memcpy(rows + c * 8,     B_t0, sizeof B_t0);
        memcpy(rows + c * 8 + 4, B_c0, sizeof B_c0);
    }
    memcpy(rows + 4 * 8,     B_t1, sizeof B_t1);
    memcpy(rows + 4 * 8 + 4, B_c1, sizeof B_c1);
    const rans_tables_t TB = { 4u, 5u, 4u, 0, rows };

    printf("\nSTAGE 1 -- context rANS, test vector B (K=4, order left, w=4)\n");
    const int tc = rans_tables_check(&TB);
    printf("  table contract (sum 65536, f>=1, cdf consistent): rc=%d\n", tc);
    verdict(tc == 0, "tables valid");

    /* guide 13: check the CONTEXTS before the bytes */
    uint8_t ctx[8];
    for (uint32_t t = 0; t < 8u; t++) ctx[t] = (uint8_t)rans_context_id(B_idx, t, 4u, TB.k_border);
    show("B_ctx exp", B_ctx, 8u);
    show("B_ctx got", ctx, 8u);
    verdict(memcmp(ctx, B_ctx, 8) == 0, "vector B contexts");

    rans_enc_t e;
    rans_enc_begin(&e, B_idx, 8u, 4u, &TB, buf, sizeof buf);
    const size_t nb = rans_enc_finish(&e, &s);
    show("expected", B_out, sizeof B_out);
    show("produced", s ? s : buf, s ? nb : 0u);
    printf("  encoder err=%d\n", e.err);
    verdict(s && nb == sizeof B_out && memcmp(s, B_out, nb) == 0, "vector B bytes");

    uint8_t dB[8];
    const int rb = rans_decode_ctx(&TB, 4u, B_out, sizeof B_out, dB, 8u);
    show("decoded", dB, 8u);
    verdict(rb == 0 && memcmp(dB, B_idx, 8) == 0, "vector B decode, pointer on EOF");

    printf("\nRESULT: %s (%d checks failed)\n", fails ? "FAIL" : "PASS", fails);
    return fails != 0;
}
