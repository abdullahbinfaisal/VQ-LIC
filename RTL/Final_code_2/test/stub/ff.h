#ifndef STUB_FF_H
#define STUB_FF_H
/* Minimal FatFs surface, enough for a host-side syntax check of the harness.
 * Never compiled into the firmware -- the BSP supplies the real ff.h. */
#include <stddef.h>
typedef unsigned int UINT;
typedef unsigned long DWORD;
typedef int FRESULT;
#define FR_OK 0
#define FA_READ 1
typedef struct { int dummy; } FIL;
typedef struct { int dummy; } FATFS;
FRESULT f_open(FIL*, const char*, unsigned char);
FRESULT f_read(FIL*, void*, UINT, UINT*);
FRESULT f_close(FIL*);
FRESULT f_lseek(FIL*, DWORD);
FRESULT f_mount(FATFS*, const char*, unsigned char);
DWORD   f_size(FIL*);
#endif
