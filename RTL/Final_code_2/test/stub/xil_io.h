#ifndef STUB_XIL_IO_H
#define STUB_XIL_IO_H
#include <stdint.h>
#define UINTPTR uintptr_t
#define MAXW 8192
typedef struct { uint32_t addr, data; } wr_t;
extern wr_t g_wr[MAXW];
extern int  g_nwr;
/* Record the PW engine register OFFSET, and leave every other peripheral at
 * its absolute address. Masking everything to 12 bits would alias the DMA
 * MM2S_LENGTH (0x28) onto PW_REG_COUT_RUN (0x028): different peripherals,
 * different bases, same low bits. */
static inline void Xil_Out32(uint32_t a, uint32_t d)
{
  const uint32_t off = (a >= 0x43C10000u && a < 0x43C11000u) ? (a - 0x43C10000u) : a;
  if (g_nwr < MAXW) { g_wr[g_nwr].addr = off; g_wr[g_nwr].data = d; }
  g_nwr++;
}
/* STATUS reads a plausible non-error value, DMASR reports idle so the driver
 * spin loops terminate, and STATUS2 bit 5 (cfg_err) reads 0. */
static inline uint32_t Xil_In32(uint32_t a) { (void)a; return 0x00000002u; }
#endif
