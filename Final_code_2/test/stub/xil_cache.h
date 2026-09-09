#ifndef STUB_XIL_CACHE_H
#define STUB_XIL_CACHE_H
#include <stdint.h>
static inline void Xil_DCacheFlushRange(uintptr_t a, uint32_t n) { (void)a; (void)n; }
static inline void Xil_DCacheInvalidateRange(uintptr_t a, uint32_t n) { (void)a; (void)n; }
#endif
