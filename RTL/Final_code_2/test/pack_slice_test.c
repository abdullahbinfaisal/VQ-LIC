/* ============================================================================
 * pack_slice_test.c -- the input packer after it was made suspendable.
 *
 * WHY THIS EXISTS. Packing now runs a few rows at a time from inside the
 * analysis cascade's DMA wait loops, so that frame n+1's 5.0127 ms of packing
 * disappears under frame n's 13.4820 ms of PL time. That is only sound if
 * stopping between two rows is unobservable in the output.
 *
 * A defect here is invisible at run time. The DW MM2S reads gm_in and cannot
 * tell a half-packed buffer from a whole one: it would stream the wrong bytes,
 * the convolution would produce plausible numbers, and the only symptom would
 * be a codec that quietly got worse. So the row-slicing contract is checked
 * here, before it reaches the board.
 *
 * The functions under test are EXTRACTED FROM main.c AT BUILD TIME by
 * gen_pack_slice_src.py, not copied. A copy would drift.
 *
 * The NEON path is the one the board actually takes (C=3, W=1280). It is
 * compiled here against a small shim that models vld1_u8 / vcombine_u8 /
 * vst1q_u8 as byte moves, which is exactly what they are -- so this tests the
 * real loop structure and the real addressing, which is where a row-slicing
 * bug would live.
 *
 *   python gen_pack_slice_src.py            # writes pack_extracted.inc
 *   gcc -O2 -Wall -o pack_slice_test pack_slice_test.c
 * ==========================================================================*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* ---- NEON shim: these three intrinsics are byte moves, nothing more ------ */
typedef struct { uint8_t v[8];  } uint8x8_t;
typedef struct { uint8_t v[16]; } uint8x16_t;

static inline uint8x8_t vld1_u8(const uint8_t *p)
{
    uint8x8_t r; memcpy(r.v, p, 8); return r;
}
static inline uint8x16_t vcombine_u8(uint8x8_t a, uint8x8_t b)
{
    uint8x16_t r; memcpy(r.v, a.v, 8); memcpy(r.v + 8, b.v, 8); return r;
}
static inline void vst1q_u8(uint8_t *p, uint8x16_t a)
{
    memcpy(p, a.v, 16);
}

#include "pack_extracted.inc"

/* ---- geometry: pair 0 of the live 16-48-64 schedule ---------------------- */
#define TH   720
#define TW  1280
#define TC     3
#define PAD  128

#define DROW  ((size_t)(((TW + 7) & ~7) / 8) * (size_t)TC * 8u)
#define DBYTES ((size_t)TH * DROW)
#define SBYTES ((size_t)TC * TH * TW)

#define GUARD 4096
#define SENT  0xA5

static uint8_t *src, *ref, *got;

static int fails = 0;
static void chk(int cond, const char *what)
{
    printf("  %-62s %s\n", what, cond ? "ok" : "FAIL");
    if (!cond) fails++;
}

static uint32_t st = 0xC0FFEEu;
static uint32_t xs(void) { uint32_t x = st; x ^= x << 13; x ^= x >> 17; x ^= x << 5; return st = x; }

/* pack rows [0,H) in ragged slices, exactly as edge_pack_step does */
static void sliced(uint8_t *dst, int nrows_per_slice, int use_neon)
{
    int h = 0;
    while (h < TH) {
        int h1 = h + nrows_per_slice;
        if (h1 > TH) h1 = TH;
        if (use_neon) pack_gm_c3_neon_rows(src, dst, TH, TW, h, h1);
        else          pack_input_group_major_generic_rows(src, dst, TC, TH, TW,
                                                          PAD, h, h1);
        h = h1;
    }
}

int main(void)
{
    printf("input packer, row slicing: C=%d %dx%d, %lu B in, %lu B out\n",
           TC, TH, TW, (unsigned long)SBYTES, (unsigned long)DBYTES);

    src = malloc(SBYTES);
    ref = malloc(DBYTES + 2 * GUARD);
    got = malloc(DBYTES + 2 * GUARD);
    if (!src || !ref || !got) { printf("alloc failed\n"); return 2; }

    for (size_t i = 0; i < SBYTES; i++) src[i] = (uint8_t)xs();

    /* ---- 0. the two implementations must already agree whole-frame ------- */
    memset(ref, SENT, DBYTES + 2 * GUARD);
    memset(got, SENT, DBYTES + 2 * GUARD);
    pack_input_group_major_generic_rows(src, ref + GUARD, TC, TH, TW, PAD, 0, TH);
    pack_gm_c3_neon_rows(src, got + GUARD, TH, TW, 0, TH);
    chk(memcmp(ref + GUARD, got + GUARD, DBYTES) == 0,
        "NEON row-ranged equals generic row-ranged, whole frame");

    /* ---- 1. sliced == whole-frame, at ragged slice sizes ----------------- */
    /* 1 is the worst case: a cut after every single row. 7 and 13 are chosen
     * so slices do not divide TH and the last one is short. */
    static const int slices[] = { 1, 2, 3, 7, 13, 64, 359, 720 };
    for (int impl = 0; impl < 2; impl++) {
        int bad_bytes = 0, bad_guard = 0;
        for (unsigned si = 0; si < sizeof slices / sizeof slices[0]; si++) {
            memset(got, SENT, DBYTES + 2 * GUARD);
            sliced(got + GUARD, slices[si], impl);
            if (memcmp(ref + GUARD, got + GUARD, DBYTES) != 0) bad_bytes++;
            for (int g = 0; g < GUARD; g++)
                if (got[g] != SENT || got[GUARD + DBYTES + g] != SENT) { bad_guard++; break; }
        }
        char what[96];
        snprintf(what, sizeof what, "%s: sliced output identical to whole-frame",
                 impl ? "NEON" : "generic");
        chk(bad_bytes == 0, what);
        snprintf(what, sizeof what, "%s: no write outside the destination buffer",
                 impl ? "NEON" : "generic");
        chk(bad_guard == 0, what);
    }

    /* ---- 2. a row range must touch ONLY its own rows ---------------------
     * This is the property that makes interleaving safe. If packing rows
     * [a,b) also disturbed row b, resuming would corrupt work already done --
     * and only at slice boundaries, so it would show up as a rare, seed-
     * dependent artefact rather than an obvious failure. */
    for (int impl = 0; impl < 2; impl++) {
        const int a = 137, b = 291;
        memset(got, SENT, DBYTES + 2 * GUARD);
        if (impl) pack_gm_c3_neon_rows(src, got + GUARD, TH, TW, a, b);
        else      pack_input_group_major_generic_rows(src, got + GUARD, TC, TH, TW,
                                                      PAD, a, b);
        int before = 0, inside = 0, after = 0;
        for (size_t i = 0; i < (size_t)a * DROW; i++)
            if (got[GUARD + i] != SENT) { before++; break; }
        if (memcmp(got + GUARD + (size_t)a * DROW,
                   ref + GUARD + (size_t)a * DROW,
                   (size_t)(b - a) * DROW) != 0) inside++;
        for (size_t i = (size_t)b * DROW; i < DBYTES; i++)
            if (got[GUARD + i] != SENT) { after++; break; }
        char what[96];
        snprintf(what, sizeof what,
                 "%s: rows [%d,%d) written correctly, rows outside untouched",
                 impl ? "NEON" : "generic", a, b);
        chk(before == 0 && inside == 0 && after == 0, what);
    }

    /* ---- 3. an empty range must do nothing ------------------------------- */
    {
        memset(got, SENT, DBYTES + 2 * GUARD);
        pack_gm_c3_neon_rows(src, got + GUARD, TH, TW, 400, 400);
        pack_input_group_major_generic_rows(src, got + GUARD, TC, TH, TW, PAD, 400, 400);
        int any = 0;
        for (size_t i = 0; i < DBYTES; i++) if (got[GUARD + i] != SENT) { any++; break; }
        chk(any == 0, "an empty row range writes nothing");
    }

    printf("\n%d checks failed\n", fails);
    printf(fails ? "RESULT: FAIL\n" : "RESULT: PASS\n");
    free(src); free(ref); free(got);
    return fails != 0;
}
