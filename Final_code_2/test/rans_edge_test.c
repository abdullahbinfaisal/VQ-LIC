/* Host check of the on-board entropy stage (rans_edge.c) before it reaches the
 * Cortex-A9: synthetic transport frames with left-neighbour structure, context
 * tables fitted through the spec-3 ROM path, the resumable job sliced against
 * one-shot, and a full decode back to hardware k.
 *
 * Everything here is SYNTHETIC. It proves the glue is lossless and that slicing
 * cannot change a byte; it says nothing about compatibility with the reference. */
#include <stdio.h>
#include <string.h>
#include "rans_edge.h"

unsigned long long ep_timer_now(void) { static unsigned long long t = 0u; return t += 1000u; }
double ep_cycles_to_ms(unsigned long long c) { return (double)c * 1e-5; }

#define NCAL 20
static uint8_t frames[NCAL + 1][VQPW_IDX_BYTES];
static uint8_t pay1[RE_PAYLOAD_CAP], pay2[RE_PAYLOAD_CAP];

static uint32_t st = 0x9E3779B9u;
static uint32_t xs(void) { st ^= st << 13; st ^= st >> 17; st ^= st << 5; return st; }

static int fails = 0;
static void check(int ok, const char *what)
{
    printf("  %s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) fails++;
}

/* 6-bit k per group, 60% copy of the left neighbour, otherwise mostly one of a
 * few common codes -- enough structure for the context model to act on. */
static void make_frame(uint8_t *tr)
{
    uint32_t prev = 0u;
    for (uint32_t pos = 0u; pos < RE_N; pos++) {
        uint32_t w = 0u;
        for (uint32_t g = 0u; g < 4u; g++) {
            uint32_t k;
            if ((pos % RE_W) && (xs() % 100u) < 60u) k = (prev >> (6u * g)) & 0x3Fu;
            else if ((xs() % 10u) == 0u)             k = xs() % 64u;
            else                                     k = xs() % 20u;
            w |= k << (6u * g);
        }
        tr[4u * pos]      = (uint8_t)(w & 0xFFu);
        tr[4u * pos + 1u] = (uint8_t)((w >> 8) & 0xFFu);
        tr[4u * pos + 2u] = (uint8_t)((w >> 16) & 0xFFu);
        tr[4u * pos + 3u] = 0u;
        prev = w;
    }
}

int main(void)
{
    printf("rans_edge host check: transport -> original-id planes -> rANS payload -> decode -> k\n");
    for (int f = 0; f <= NCAL; f++) make_frame(frames[f]);

    re_synth_maps();
    const uint8_t *cal[NCAL];
    for (int f = 0; f < NCAL; f++) cal[f] = frames[f];
    re_fit_report_t fr;
    const int frc = re_fit_tables(cal, NCAL, &fr);
    printf("  fit rc=%d, populated slots %u %u %u %u of 65\n", frc,
           (unsigned)fr.populated[0], (unsigned)fr.populated[1],
           (unsigned)fr.populated[2], (unsigned)fr.populated[3]);
    check(frc == 0, "context tables fitted, expanded and contract-checked");

    re_job_t J;
    rans_mode_t m1 = RANS_MODE_RAW;
    re_job_start(&J, frames[NCAL], pay1, sizeof pay1);
    const size_t len1 = re_job_finish(&J, &m1);
    long first = -1;
    const long bad = re_job_verify(&J, frames[NCAL], &first);
    printf("  one-shot: payload %u B, mode %d, err %d; decode back to k: %ld mismatches (first %ld)\n",
           (unsigned)len1, (int)m1, J.err, bad, first);
    printf("    first 24:");
    for (size_t i = 0; i < 24u && i < len1; i++) printf(" %02X", (unsigned)pay1[i]);
    printf("\n");
    check(len1 > RANS_HDR_BYTES && bad == 0, "unpack + rANS + payload is lossless back to hardware k");

    re_job_t J2;
    rans_mode_t m2 = RANS_MODE_RAW;
    re_job_start(&J2, frames[NCAL], pay2, sizeof pay2);
    int slices = 0, mono = 1;
    uint32_t lu = 0u, lc = 0u;
    while (!re_job_step(&J2, 64u)) {
        slices++;
        const uint32_t u = re_job_unpacked(&J2), c = re_job_coded(&J2);
        if (u < lu || c < lc) mono = 0;
        lu = u; lc = c;
    }
    slices++;
    const uint32_t fu = re_job_unpacked(&J2), fc = re_job_coded(&J2);
    const size_t len2 = re_job_finish(&J2, &m2);
    printf("  sliced: %d slices of 64 units; unpacked %u of %u positions, coded %u of %u symbols; payload %u B\n",
           slices, (unsigned)fu, (unsigned)RE_N, (unsigned)fc, (unsigned)(4u * RE_N), (unsigned)len2);
    check(mono && fu == RE_N && fc == 4u * RE_N, "progress counters are monotonic and complete");
    check(len2 == len1 && m2 == m1 && memcmp(pay1, pay2, len1) == 0, "sliced payload byte-identical to one-shot");

    printf("\nRESULT: %s (%d checks failed)\n", fails ? "FAIL" : "PASS", fails);
    return fails != 0;
}
