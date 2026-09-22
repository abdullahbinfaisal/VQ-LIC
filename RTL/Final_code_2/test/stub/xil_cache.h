#ifndef STUB_XIL_CACHE_H
#define STUB_XIL_CACHE_H
#include <stdint.h>
/* UINTPTR is a BSP typedef; the firmware casts with it, so the stub must
 * provide it too or every cache call fails to parse here. */
#ifndef UINTPTR
#define UINTPTR uintptr_t
#endif
static inline void Xil_DCacheFlushRange(uintptr_t a, uint32_t n) { (void)a; (void)n; }
static inline void Xil_DCacheInvalidateRange(uintptr_t a, uint32_t n) { (void)a; (void)n; }
#endif
