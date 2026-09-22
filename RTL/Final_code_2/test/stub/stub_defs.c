#include <stdint.h>
#define MAXW 8192
typedef struct { uint32_t addr, data; } wr_t;
wr_t g_wr[MAXW];
int  g_nwr = 0;
unsigned long long ep_timer_now(void) { return 0ull; }
double ep_cycles_to_ms(unsigned long long c) { return (double)c; }
