#include "cnn_runner.h"

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "ff.h"
#include "xil_io.h"
#include "xil_cache.h"
#include "xparameters.h"
#include "xaxidma.h"
#include "xstatus.h"
#include "xiltimer.h"


/* ============================================================
 * User-configurable hardware base addresses
 * ============================================================ */
static XAxiDma DwDma;
static XAxiDma PwDma;
extern XAxiDma_Config XAxiDma_ConfigTable[];


#define DW_BASE_ADDR 0x43C00000u
#define PW_BASE_ADDR 0x43C10000u



#define DW_DMA_CFG_INDEX 0
#define PW_DMA_CFG_INDEX 1

#define DW_REG_CTRL    0x00
#define DW_REG_STATUS  0x04
#define DW_REG_DIMS    0x08
#define DW_REG_PARAMS  0x0C
#define DW_REG_BIAS    0x10
#define DW_REG_MULT    0x14
#define DW_REG_SHIFT   0x18
#define DW_REG_W_0     0x1C
#define DW_REG_W_1     0x20
#define DW_REG_W_2     0x24

#define PW_N_OC             30
#define PW_N_LANES          8
#define PW_REG_CTRL         0x000
#define PW_REG_STATUS       0x004
#define PW_REG_TILE_PIXELS  0x008
#define PW_REG_CIN_RUN      0x00C
#define PW_REG_ZP_RELU      0x010
#define PW_REG_BIAS         0x014
#define PW_REG_MULT         0x018
#define PW_REG_SHIFT        0x01C
#define PW_REG_STATUS2      0x020
#define PW_REG_OC_SEL       0x024
#define PW_REG_COUT_RUN     0x028
#define PW_REG_W_BRAM_OFF   0x02C
#define PW_REG_PARAM_ADDR   0x030
#define PW_REG_W_BASE       0x100

#define MAX_TENSOR_BYTES   180000000u

#define DDR_BUF_CUR_ADDR   0x02000000u
#define DDR_BUF_NEXT_ADDR  0x0D000000u
#define DDR_BUF_ADD_ADDR   0x18000000u

/* Dedicated high-memory addrs to avoid massive heap/bss sizes */
#define PW_PACK_BUF_ADDR   0x23000000u
#define PW_TILE_SCRATCH    0x27400000u
#define PW_RX_TILE_ADDR    0x27D00000u

/* Skips densely packed descending by spatial size */
#define DDR_SKIP0_ADDR     0x28000000u  /* Size: 176 MB */
#define DDR_SKIP1_ADDR     0x33000000u  /* Size:  96 MB */
#define DDR_SKIP2_ADDR     0x39000000u  /* Size:  48 MB */
#define DDR_SKIP3_ADDR     0x3C000000u  /* Size:  16 MB */
#define DDR_SKIP4_ADDR     0x3D000000u  /* Size:  16 MB */
#define DDR_SKIP5_ADDR     0x3E000000u  /* Size:  16 MB */
#define PW_TILE_PIXELS     16384

/* DMA base addrs if your low-level helpers need them */
#define DW_DMA_BASE    0x40400000u
#define PW_DMA_BASE    0x40410000u

/* ============================================================
 * Network constants
 * ============================================================ */
#define NUM_LAYERS      42
#define NUM_ADDS         6
#define NUM_SKIP_SLOTS   6
#define INSTR_RECORD_BYTES 32

/* ============================================================
 * L0 DW hardware-vs-sim diagnostic instrumentation
 *   DW_DIAG          : 1 = print per-channel config, register readback,
 *                          and first-16 input/output bytes for layer 0.
 *   DW_DIAG_STOP_L0  : 1 = halt the network run right after L0 + its
 *                          compare, so the diagnostic log stays focused.
 * Set both to 0 to restore the normal full-network behavior.
 * ============================================================ */
#define DW_DIAG          0
#define DW_DIAG_STOP_L0  0
/* DW_DIAG_IMPULSE: replace L0/ch0 input with a single known impulse pixel and
 * print the 3x3 output stamp + the self-computed expected stamp. This isolates
 * "core windows/computes wrong" from "DMA delivers wrong data". Background
 * output should be the bias-only value; the 3x3 stamp should be the 180-rotated
 * requantized kernel. Set to 0 for normal operation. */
#define DW_DIAG_IMPULSE  0
#define DW_IMP_R0        16
#define DW_IMP_C0        40
#define DW_IMP_V         255
/* Experiments — set ONE at a time; 0 = off. Keep DW_DIAG_IMPULSE=1 to read the
 * column shift from the scan, and DW_DIAG_WEIGHT_SWAP=0 for the -8 tests.
 *  DW_DIAG_START_SETTLE : N dummy AXI reads inserted between the start pulse and
 *                         the MM2S arm, so the start-triggered FIFO reset settles
 *                         before the first beat arrives. Try 256. If the -8 shift
 *                         changes -> a start<->first-beat timing race (main.c-side).
 *  DW_DIAG_PRIME_BEAT   : prepend ONE dummy input beat. If the core drops its
 *                         first captured beat, the dummy is dropped and the real
 *                         data lands aligned -> the impulse cols should snap from
 *                         31/32/33 back to 39/40/41 (-8 gone).
 *  DW_DIAG_WEIGHT_SWAP  : pre-swap kernel rows 0/1 (cancels the HW top/middle swap). */
#define DW_DIAG_START_SETTLE 0
#define DW_DIAG_PRIME_BEAT   0
#define DW_DIAG_WEIGHT_SWAP  0


typedef unsigned long long u64_cycles;

/* Zynq-7000 Cortex-A9 global timer runs at CPU clock / 2.
 * Set this to your actual CPU frequency if known.
 * Example: CPU 666.666 MHz => timer 333.333 MHz
 */
#define CPU_FREQ_HZ         666666687.0
#define GTIMER_FREQ_HZ      (CPU_FREQ_HZ / 2.0)

static inline u64_cycles timer_now(void)
{
    XTime t;
    XTime_GetTime(&t);
    return (u64_cycles)t;
}

static inline double cycles_to_ms(u64_cycles cyc)
{
    return ((double)cyc * 1000.0) / GTIMER_FREQ_HZ;
}

/* ============================================================
 * Peak DMA bandwidth / compute throughput tracking.
 *
 * "Peak attained" = best single-transfer (or single-plane/tile) rate seen
 * anywhere in the full 42-layer run, not a whole-run average — layer sizes
 * vary a lot (L0 is 2048x1435, deep layers are tiny), so the peak surfaces
 * on whichever transfer was large enough to reach steady-state streaming.
 * ============================================================ */
typedef struct {
    double   gbps;
    int      layer_idx;
    int      idx;      /* channel (DW) or tile number (PW) */
    size_t   bytes;
    double   ms;
} peak_bw_t;

typedef struct {
    double   gmacs_per_s;
    int      layer_idx;
    uint64_t macs;
    double   ms;
} peak_thr_t;

static peak_bw_t  g_peak_dw_mm2s = {0};
static peak_bw_t  g_peak_dw_s2mm = {0};
static peak_bw_t  g_peak_pw_mm2s = {0};
static peak_bw_t  g_peak_pw_s2mm = {0};
static peak_thr_t g_peak_dw_thr  = {0};
static peak_thr_t g_peak_pw_thr  = {0};

/* Whole-run totals (summed across all 42 layers) for a "full encoder"
 * roofline point — distinct from the peak trackers above, which only keep
 * the single best transfer. Bytes are in+out combined (both DMA directions);
 * MACs use the same per-op conventions as note_thr (9 MACs/pixel for DW's
 * 3x3 kernel, pixels*Cin*cout_rounded for PW). g_total_layer_cyc sums each
 * layer's own [DW/PW TIMING] "total" window, which already excludes the
 * SD-card reference-compare overhead in cnn_run_full_network's outer loop —
 * so it reflects real deployed-encoder wall time, not benchmark I/O. */
static uint64_t   g_total_dw_bytes  = 0;
static uint64_t   g_total_dw_macs   = 0;
static uint64_t   g_total_pw_bytes  = 0;
static uint64_t   g_total_pw_macs   = 0;
static u64_cycles g_total_layer_cyc   = 0;
/* Sum of DW's cyc_run + PW's cyc_dma across all layers — the DMA/compute-only
 * time basis, excluding param/weight preload, pack, and store overhead. */
static u64_cycles g_total_dma_run_cyc = 0;

/* Per-layer (ops, bytes, wall_time) triples for a full roofline scatter —
 * one point per layer instead of just the single combined "full encoder"
 * point above. wall_ms is each layer's own [DW/PW TIMING] "total" window
 * (same real-deployment-time basis as g_total_layer_cyc, per-layer instead
 * of summed) — includes param/weight BRAM preload, pack, and store.
 * dma_run_ms is the narrower DMA+compute-only window (DW's cyc_run / PW's
 * cyc_dma), excluding all of that software-side overhead — this is the
 * time basis that matches a "DMA/compute-only" achieved-throughput claim.
 * Indexed directly by layer_idx (0..NUM_LAYERS-1). */
typedef struct {
    int      kind;      /* KIND_DW or KIND_PW */
    int      Cin, Cout, H, W;
    uint64_t bytes;
    uint64_t macs;
    double   wall_ms;
    double   dma_run_ms;
} layer_stat_t;

static layer_stat_t g_layer_stats[NUM_LAYERS];

static inline void note_bw(peak_bw_t *pk, size_t bytes, u64_cycles cyc, int layer_idx, int idx)
{
    double ms = cycles_to_ms(cyc);
    if (ms <= 0.0) return;
    double gbps = ((double)bytes / 1.0e9) / (ms / 1000.0);
    if (gbps > pk->gbps) {
        pk->gbps = gbps;
        pk->layer_idx = layer_idx;
        pk->idx = idx;
        pk->bytes = bytes;
        pk->ms = ms;
    }
}

static inline void note_thr(peak_thr_t *pk, uint64_t macs, u64_cycles cyc, int layer_idx)
{
    double ms = cycles_to_ms(cyc);
    if (ms <= 0.0) return;
    double gmacs = ((double)macs / 1.0e9) / (ms / 1000.0);
    if (gmacs > pk->gmacs_per_s) {
        pk->gmacs_per_s = gmacs;
        pk->layer_idx = layer_idx;
        pk->macs = macs;
        pk->ms = ms;
    }
}



static inline void dw_write_reg(u32 offset, u32 value)
{
    Xil_Out32(DW_BASE_ADDR + offset, value);
}

static inline u32 dw_read_reg(u32 offset)
{
    return Xil_In32(DW_BASE_ADDR + offset);
}

static inline void pw_write_reg(u32 offset, u32 value)
{
    Xil_Out32(PW_BASE_ADDR + offset, value);
}

static inline u32 pw_read_reg(u32 offset)
{
    return Xil_In32(PW_BASE_ADDR + offset);
}

static inline u32 dw_pack_dims(int h, int w)
{
    return ((u32)h << 16) | (u32)w;
}

static inline u32 dw_pack_params(int stride_2, int pad, int zp_in, int zp_out)
{
    return ((u32)(zp_out & 0xFF) << 24) |
           ((u32)(zp_in  & 0xFF) << 16) |
           ((u32)(pad    & 0xFF) << 8 ) |
           ((u32)(stride_2 ? 1 : 0) << 0);
}

static inline u32 dw_pack_w0(int8_t w00, int8_t w01, int8_t w02, int8_t w10)
{
    return ((u32)((u8)w10) << 24) |
           ((u32)((u8)w02) << 16) |
           ((u32)((u8)w01) << 8 ) |
           ((u32)((u8)w00) << 0 );
}

static inline u32 dw_pack_w1(int8_t w11, int8_t w12, int8_t w20, int8_t w21)
{
    return ((u32)((u8)w21) << 24) |
           ((u32)((u8)w20) << 16) |
           ((u32)((u8)w12) << 8 ) |
           ((u32)((u8)w11) << 0 );
}

static inline u32 dw_pack_w2(int8_t w22)
{
    return ((u32)((u8)w22) << 0);
}

static inline u32 pw_pack_zp_relu(uint8_t zp_in, uint8_t zp_out, int relu_en)
{
    return ((u32)(relu_en & 0x1) << 16) |
           ((u32)zp_out << 8) |
           ((u32)zp_in);
}

static inline void pw_write_weight(int ic, int8_t w)
{
    pw_write_reg(PW_REG_W_BASE + (4 * ic), (u32)(u8)w);
}

static int init_one_dma(XAxiDma *DmaPtr, int cfg_index)
{
    int status;

    status = XAxiDma_CfgInitialize(DmaPtr, &XAxiDma_ConfigTable[cfg_index]);
    if (status != XST_SUCCESS) {
        printf("ERROR: XAxiDma_CfgInitialize failed: %d\n", status);
        return -1;
    }

    XAxiDma_Reset(DmaPtr);
    while (!XAxiDma_ResetIsDone(DmaPtr)) {}

    return 0;
}

static int init_dmas(void)
{
    if (init_one_dma(&DwDma, DW_DMA_CFG_INDEX) != 0) return -1;
    if (init_one_dma(&PwDma, PW_DMA_CFG_INDEX) != 0) return -1;
    return 0;
}

/* ============================================================
 * DDR work buffers
 * Keep these static, not malloc.
 * Size from your actual max tensor size.
 * ============================================================ */
#define MAX_SKIP_BYTES     (180000000u)





/* ============================================================
 * Save points / add schedule
 * ============================================================ */
static const int SAVE_POINT_LAYERS[NUM_SKIP_SLOTS] = {3, 11, 19, 25, 29, 33};

static const add_desc_t ADD_TABLE[NUM_ADDS] = {
    {0,  3,  7,  8, 2460503197u,  376513162u, 30,  65, 0, 0},
    {1, 11, 15, 16, 4004706348u, 2286822340u, 30, 100, 0, 0},
    {2, 19, 23, 24, 3878626127u, 1815223455u, 30,  99, 0, 0},
    {3, 25, 29, 30, 2643442026u, 1606958305u, 31,  49, 0, 0},
    {4, 29, 33, 34, 2208422926u, 1437014315u, 31,  64, 0, 0},
    {5, 33, 37, 38, 2231071666u, 1637576736u, 31,  42, 0, 0},
};

static int skip_slot_for_layer(int layer_idx)
{
    for (int i = 0; i < NUM_SKIP_SLOTS; i++) {
        if (SAVE_POINT_LAYERS[i] == layer_idx) return i;
    }
    return -1;
}

static const add_desc_t *add_triggered_after(int layer_idx)
{
    for (int i = 0; i < NUM_ADDS; i++) {
        if (ADD_TABLE[i].src_main_layer == layer_idx) return &ADD_TABLE[i];
    }
    return NULL;
}

/* ============================================================
 * Expected schedule (optional validation)
 * ============================================================ */
typedef struct {
    layer_kind_t kind;
    int cin;
    int cout; /* -1 for DW */
} expected_layer_t;

static const expected_layer_t EXPECTED[NUM_LAYERS] = {
    { KIND_DW,  3,  -1 }, { KIND_PW,  3,  30 }, { KIND_DW, 30,  -1 }, { KIND_PW, 30,  60 },
    { KIND_DW, 60,  -1 }, { KIND_PW, 60,  60 }, { KIND_DW, 60,  -1 }, { KIND_PW, 60,  60 },
    { KIND_DW, 60,  -1 }, { KIND_PW, 60,  90 }, { KIND_DW, 90,  -1 }, { KIND_PW, 90, 120 },
    { KIND_DW,120,  -1 }, { KIND_PW,120, 120 }, { KIND_DW,120,  -1 }, { KIND_PW,120, 120 },
    { KIND_DW,120,  -1 }, { KIND_PW,120, 180 }, { KIND_DW,180,  -1 }, { KIND_PW,180, 240 },
    { KIND_DW,240,  -1 }, { KIND_PW,240, 240 }, { KIND_DW,240,  -1 }, { KIND_PW,240, 240 },
    { KIND_DW,240,  -1 }, { KIND_PW,240, 240 }, { KIND_DW,240,  -1 }, { KIND_PW,240, 240 },
    { KIND_DW,240,  -1 }, { KIND_PW,240, 240 }, { KIND_DW,240,  -1 }, { KIND_PW,240, 240 },
    { KIND_DW,240,  -1 }, { KIND_PW,240, 240 }, { KIND_DW,240,  -1 }, { KIND_PW,240, 240 },
    { KIND_DW,240,  -1 }, { KIND_PW,240, 240 }, { KIND_DW,240,  -1 }, { KIND_PW,240, 240 },
    { KIND_DW,240,  -1 }, { KIND_PW,240, 220 },
};

/* ============================================================
 * FatFs helpers
 * ============================================================ */
static FATFS sd_fs;
static int sd_mounted = 0;

static int ensure_sd_mounted(void)
{
    if (sd_mounted) return 0;
    if (f_mount(&sd_fs, "0:/", 1) != FR_OK) {
        printf("[SD] mount failed\n");
        return -1;
    }
    sd_mounted = 1;
    return 0;
}

int load_file_from_sd(const char *path, file_blob_t *out)
{
    FIL fil;
    FRESULT fr;
    FSIZE_t sz;
    UINT rd = 0;
    uint8_t *buf;

    if (!out) return -1;
    out->data = NULL;
    out->size = 0;

    if (ensure_sd_mounted() != 0) return -1;

    fr = f_open(&fil, path, FA_READ);
    if (fr != FR_OK) {
        printf("[SD] open fail: %s (%d)\n", path, (int)fr);
        return -1;
    }

    sz = f_size(&fil);
    buf = (uint8_t *)malloc((size_t)sz);
    if (!buf) {
        printf("[SD] malloc fail for %s size=%lu\n", path, (unsigned long)sz);
        f_close(&fil);
        return -1;
    }

    fr = f_read(&fil, buf, (UINT)sz, &rd);
    f_close(&fil);

    if (fr != FR_OK || rd != (UINT)sz) {
        printf("[SD] read fail: %s\n", path);
        free(buf);
        return -1;
    }

    out->data = buf;
    out->size = (size_t)sz;
    printf("[SD] Loaded %s, size = %lu bytes\n", path, (unsigned long)sz);
    return 0;
}

void free_blob(file_blob_t *b)
{
    if (b && b->data) {
        free(b->data);
        b->data = NULL;
        b->size = 0;
    }
}

/* ============================================================
 * Parser
 * ============================================================ */
int parse_layer_desc_from_instr(const file_blob_t *instr, int idx, layer_desc_t *out)
{
    size_t off;
    const uint8_t *p;
    uint32_t w[8];
    uint32_t depthwise;

    if (!instr || !instr->data || !out) return -1;
    off = (size_t)idx * INSTR_RECORD_BYTES;
    if (off + INSTR_RECORD_BYTES > instr->size) return -1;

    p = instr->data + off;
    for (int i = 0; i < 8; i++) {
        w[i] = (uint32_t)p[i*4 + 0]
             | ((uint32_t)p[i*4 + 1] << 8)
             | ((uint32_t)p[i*4 + 2] << 16)
             | ((uint32_t)p[i*4 + 3] << 24);
    }

    depthwise = (w[0] >> 4) & 1u;

    out->layer_idx   = idx;
    out->kind        = depthwise ? KIND_DW : KIND_PW;
    out->bypass_1x1  = (int)((w[0] >> 2) & 1u);
    out->relu_en     = (int)((w[0] >> 3) & 1u);
    out->weight_addr = w[3];
    out->params_addr = w[4];
    out->H           = (int)((w[5] >> 16) & 0xFFFFu);
    out->W           = (int)((w[5] >>  0) & 0xFFFFu);
    out->Cout        = (int)((w[6] >> 16) & 0xFFFFu);
    out->Cin         = (int)((w[6] >>  0) & 0xFFFFu);
    out->zp_in       = (uint8_t)((w[7] >> 24) & 0xFFu);
    out->zp_out      = (uint8_t)((w[7] >> 16) & 0xFFu);
    out->stride      = (int)((w[7] >> 12) & 0xFu);
    out->kernel      = (int)((w[7] >>  8) & 0xFu);
    out->pad         = (int)((w[7] >>  0) & 0xFFu);

    return 0;
}

void print_layer_desc(const layer_desc_t *d)
{
    printf("L%02d %-2s Cin=%d Cout=%d H=%d W=%d k=%d s=%d p=%d zp_in=%d zp_out=%d relu=%d w=0x%08x p=0x%08x\n",
           d->layer_idx,
           d->kind == KIND_DW ? "DW" : "PW",
           d->Cin, d->Cout, d->H, d->W,
           d->kernel, d->stride, d->pad,
           d->zp_in, d->zp_out, d->relu_en,
           (unsigned)d->weight_addr, (unsigned)d->params_addr);
}

int calc_out_dim(int in_dim, int pad, int kernel, int stride)
{
    return (in_dim + 2 * pad - kernel) / stride + 1;
}

size_t tensor_numel(int C, int H, int W)
{
    int w_padded = (W + 7) & ~7;
    return (size_t)C * (size_t)H * (size_t)w_padded;
}

size_t tensor_bytes_u8(int C, int H, int W)
{
    return tensor_numel(C, H, W);
}

tensor_shape_t get_layer_output_shape(const layer_desc_t *d)
{
    tensor_shape_t s;
    s.H = calc_out_dim(d->H, d->pad, d->kernel, d->stride);
    s.W = calc_out_dim(d->W, d->pad, d->kernel, d->stride);
    s.C = (d->kind == KIND_DW) ? d->Cin : d->Cout;
    return s;
}

/* ============================================================
 * Param / weight reading helpers
 * ============================================================ */
static uint32_t le_u32(const uint8_t *p)
{
    return ((uint32_t)p[0])
         | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}

static int32_t le_s32(const uint8_t *p)
{
    return (int32_t)le_u32(p);
}

static int load_param_block_16B(const file_blob_t *params_blob,
                                uint32_t params_addr,
                                int oc,
                                int32_t *bias,
                                uint32_t *mult,
                                uint8_t *shift)
{
    size_t off = (size_t)params_addr + (size_t)oc * 16u;
    if (off + 16 > params_blob->size) return -1;

    const uint8_t *p = params_blob->data + off;
    *bias  = le_s32(p + 0);
    *mult  = le_u32(p + 4);
    *shift = p[8];
    return 0;
}

/* DW weights: 9 int8 values per channel, but layer start is 32B aligned */
static int load_dw_weights_3x3(const file_blob_t *weights_blob,
                               uint32_t weight_addr,
                               int ch,
                               int8_t w9[9])
{
    size_t off = (size_t)weight_addr + (size_t)ch * 9u;
    if (off + 9 > weights_blob->size) return -1;
    memcpy(w9, weights_blob->data + off, 9);
    return 0;
}

/* PW weights: Cin int8 values per output channel */
static int load_pw_weights_for_oc(const file_blob_t *weights_blob,
                                  uint32_t weight_addr,
                                  int oc,
                                  int Cin,
                                  int8_t *w_ic /* size Cin */)
{
    size_t off = (size_t)weight_addr + (size_t)oc * (size_t)Cin;
    if (off + (size_t)Cin > weights_blob->size) return -1;
    memcpy(w_ic, weights_blob->data + off, (size_t)Cin);
    return 0;
}

/* ============================================================
 * Residual add (CPU) - optimized
 *
 * Default mode preserves exact semantics:
 *   round((a - zp_a)*mult_a + (b - zp_b)*mult_b)
 *
 * Optional approximate mode:
 *   round((a - zp_a)*mult_a) + round((b - zp_b)*mult_b)
 *
 * Keep USE_APPROX_SPLIT_ROUNDING = 0 for bit-exact model/RTL matching.
 * ============================================================ */

#define USE_L1_SPLIT_LUT 1
#define USE_APPROX_SPLIT_ROUNDING 0

static inline int32_t round_shift_ties_to_even_i64(int64_t x, uint8_t shift)
{
    int sh = (int)shift;
    int sign;
    uint64_t ax, q, r, half;
    int inc;
    int64_t y;

    if (sh <= 0) return (int32_t)x;
    if (sh >= 63) return 0;

    sign = (x < 0);
    ax   = sign ? (uint64_t)(-x) : (uint64_t)x;

    q    = ax >> sh;
    r    = ax & (((uint64_t)1 << sh) - 1u);
    half = ((uint64_t)1 << (sh - 1));

    inc  = (r > half) || ((r == half) && (q & 1u));
    if (inc) q++;

    y = sign ? -(int64_t)q : (int64_t)q;
    return (int32_t)y;
}

static inline int32_t round_shift_ties_to_even_i64_fast(int64_t x, int sh)
{
    int sign = (x < 0);
    uint64_t ax = sign ? (uint64_t)(-x) : (uint64_t)x;

    uint64_t q = ax >> sh;
    uint64_t r = ax & (((uint64_t)1 << sh) - 1u);
    uint64_t half = ((uint64_t)1 << (sh - 1));

    int inc = (r > half) || ((r == half) && (q & 1u));
    if (inc) q++;

    int64_t y = sign ? -(int64_t)q : (int64_t)q;
    return (int32_t)y;
}

static inline uint8_t clamp_u8_i32(int32_t x)
{
    if (x < 0) return 0;
    if (x > 255) return 255;
    return (uint8_t)x;
}

uint8_t residual_add_pixel(int a, int b,
                           uint32_t mult_a, uint32_t mult_b,
                           int shift,
                           int zp_a, int zp_b, int zp_out)
{
    int32_t da = (int32_t)a - (int32_t)zp_a;
    int32_t db = (int32_t)b - (int32_t)zp_b;

    int64_t acc =
        (int64_t)da * (int64_t)mult_a +
        (int64_t)db * (int64_t)mult_b;

    int32_t y = round_shift_ties_to_even_i64(acc, (uint8_t)shift);
    int32_t pre = y + zp_out;

    return clamp_u8_i32(pre);
}

/* Flattened LUT:
 * index = (a << 8) | b
 *
 * 256 x 256 = 65536 bytes.
 * Align to 64B for better cache-line behavior on Cortex-A9.
 */
#if !USE_L1_SPLIT_LUT
static uint8_t add_lut_flat[256u * 256u] __attribute__((aligned(64)));
#endif

#if USE_L1_SPLIT_LUT
static int64_t partial_a[256] __attribute__((aligned(64)));
static int64_t partial_b[256] __attribute__((aligned(64)));
#elif USE_APPROX_SPLIT_ROUNDING
static int32_t contrib_a[256] __attribute__((aligned(64)));
static int32_t contrib_b[256] __attribute__((aligned(64)));
#endif

#if USE_L1_SPLIT_LUT
static void build_residual_add_lut_split_exact(const add_desc_t *ad)
{
    for (int i = 0; i < 256; i++) {
        partial_a[i] = (int64_t)((int32_t)i - (int32_t)ad->zp_a) * (int64_t)ad->mult_a;
        partial_b[i] = (int64_t)((int32_t)i - (int32_t)ad->zp_b) * (int64_t)ad->mult_b;
    }
}
#else
static void build_residual_add_lut_exact(const add_desc_t *ad)
{
    for (int a = 0; a < 256; a++) {
        int32_t da = (int32_t)a - (int32_t)ad->zp_a;

        for (int b = 0; b < 256; b++) {
            int32_t db = (int32_t)b - (int32_t)ad->zp_b;

            int64_t acc =
                (int64_t)da * (int64_t)ad->mult_a +
                (int64_t)db * (int64_t)ad->mult_b;

            int32_t y = round_shift_ties_to_even_i64(acc, (uint8_t)ad->shift);
            int32_t pre = y + ad->zp_out;

            add_lut_flat[((uint32_t)a << 8) | (uint32_t)b] = clamp_u8_i32(pre);
        }
    }
}

#if USE_APPROX_SPLIT_ROUNDING
static void build_residual_add_lut_split_rounding(const add_desc_t *ad)
{
    for (int i = 0; i < 256; i++) {
        int64_t va =
            (int64_t)((int32_t)i - (int32_t)ad->zp_a) *
            (int64_t)ad->mult_a;

        int64_t vb =
            (int64_t)((int32_t)i - (int32_t)ad->zp_b) *
            (int64_t)ad->mult_b;

        contrib_a[i] = round_shift_ties_to_even_i64(va, (uint8_t)ad->shift);
        contrib_b[i] = round_shift_ties_to_even_i64(vb, (uint8_t)ad->shift);
    }

    for (int a = 0; a < 256; a++) {
        for (int b = 0; b < 256; b++) {
            int32_t pre = contrib_a[a] + contrib_b[b] + ad->zp_out;
            add_lut_flat[((uint32_t)a << 8) | (uint32_t)b] = clamp_u8_i32(pre);
        }
    }
}
#endif

static void build_residual_add_lut(const add_desc_t *ad)
{
#if USE_APPROX_SPLIT_ROUNDING
    build_residual_add_lut_split_rounding(ad);
#else
    build_residual_add_lut_exact(ad);
#endif
}
#endif

void run_residual_add_tensor(const uint8_t * restrict src_a,
                             const uint8_t * restrict src_b,
                             uint8_t       * restrict dst,
                             size_t n_elem,
                             const add_desc_t *ad)
{
    u64_cycles t0 = timer_now();

#if USE_L1_SPLIT_LUT
    build_residual_add_lut_split_exact(ad);
#else
    build_residual_add_lut(ad);
#endif

    u64_cycles t1 = timer_now();

    const uint8_t * restrict a_ptr = src_a;
    const uint8_t * restrict b_ptr = src_b;
    uint8_t       * restrict d_ptr = dst;

#if USE_L1_SPLIT_LUT
    /* Use exact split-LUT option fitting in L1 cache */
    size_t i = 0;
    int shift = ad->shift;
    int zp_out = ad->zp_out;

    if (shift <= 0) {
        for (; i + 8 <= n_elem; i += 8) {
            uint32_t a0 = a_ptr[i + 0];
            uint32_t a1 = a_ptr[i + 1];
            uint32_t a2 = a_ptr[i + 2];
            uint32_t a3 = a_ptr[i + 3];
            uint32_t a4 = a_ptr[i + 4];
            uint32_t a5 = a_ptr[i + 5];
            uint32_t a6 = a_ptr[i + 6];
            uint32_t a7 = a_ptr[i + 7];

            uint32_t b0 = b_ptr[i + 0];
            uint32_t b1 = b_ptr[i + 1];
            uint32_t b2 = b_ptr[i + 2];
            uint32_t b3 = b_ptr[i + 3];
            uint32_t b4 = b_ptr[i + 4];
            uint32_t b5 = b_ptr[i + 5];
            uint32_t b6 = b_ptr[i + 6];
            uint32_t b7 = b_ptr[i + 7];

            d_ptr[i + 0] = clamp_u8_i32((int32_t)(partial_a[a0] + partial_b[b0]) + zp_out);
            d_ptr[i + 1] = clamp_u8_i32((int32_t)(partial_a[a1] + partial_b[b1]) + zp_out);
            d_ptr[i + 2] = clamp_u8_i32((int32_t)(partial_a[a2] + partial_b[b2]) + zp_out);
            d_ptr[i + 3] = clamp_u8_i32((int32_t)(partial_a[a3] + partial_b[b3]) + zp_out);
            d_ptr[i + 4] = clamp_u8_i32((int32_t)(partial_a[a4] + partial_b[b4]) + zp_out);
            d_ptr[i + 5] = clamp_u8_i32((int32_t)(partial_a[a5] + partial_b[b5]) + zp_out);
            d_ptr[i + 6] = clamp_u8_i32((int32_t)(partial_a[a6] + partial_b[b6]) + zp_out);
            d_ptr[i + 7] = clamp_u8_i32((int32_t)(partial_a[a7] + partial_b[b7]) + zp_out);
        }
        for (; i < n_elem; i++) {
            d_ptr[i] = clamp_u8_i32((int32_t)(partial_a[a_ptr[i]] + partial_b[b_ptr[i]]) + zp_out);
        }
    } else if (shift >= 63) {
        uint8_t clamped_zp = clamp_u8_i32(zp_out);
        memset(d_ptr, clamped_zp, n_elem);
    } else {
        for (; i + 8 <= n_elem; i += 8) {
            uint32_t a0 = a_ptr[i + 0];
            uint32_t a1 = a_ptr[i + 1];
            uint32_t a2 = a_ptr[i + 2];
            uint32_t a3 = a_ptr[i + 3];
            uint32_t a4 = a_ptr[i + 4];
            uint32_t a5 = a_ptr[i + 5];
            uint32_t a6 = a_ptr[i + 6];
            uint32_t a7 = a_ptr[i + 7];

            uint32_t b0 = b_ptr[i + 0];
            uint32_t b1 = b_ptr[i + 1];
            uint32_t b2 = b_ptr[i + 2];
            uint32_t b3 = b_ptr[i + 3];
            uint32_t b4 = b_ptr[i + 4];
            uint32_t b5 = b_ptr[i + 5];
            uint32_t b6 = b_ptr[i + 6];
            uint32_t b7 = b_ptr[i + 7];

            d_ptr[i + 0] = clamp_u8_i32(round_shift_ties_to_even_i64_fast(partial_a[a0] + partial_b[b0], shift) + zp_out);
            d_ptr[i + 1] = clamp_u8_i32(round_shift_ties_to_even_i64_fast(partial_a[a1] + partial_b[b1], shift) + zp_out);
            d_ptr[i + 2] = clamp_u8_i32(round_shift_ties_to_even_i64_fast(partial_a[a2] + partial_b[b2], shift) + zp_out);
            d_ptr[i + 3] = clamp_u8_i32(round_shift_ties_to_even_i64_fast(partial_a[a3] + partial_b[b3], shift) + zp_out);
            d_ptr[i + 4] = clamp_u8_i32(round_shift_ties_to_even_i64_fast(partial_a[a4] + partial_b[b4], shift) + zp_out);
            d_ptr[i + 5] = clamp_u8_i32(round_shift_ties_to_even_i64_fast(partial_a[a5] + partial_b[b5], shift) + zp_out);
            d_ptr[i + 6] = clamp_u8_i32(round_shift_ties_to_even_i64_fast(partial_a[a6] + partial_b[b6], shift) + zp_out);
            d_ptr[i + 7] = clamp_u8_i32(round_shift_ties_to_even_i64_fast(partial_a[a7] + partial_b[b7], shift) + zp_out);
        }
        for (; i < n_elem; i++) {
            d_ptr[i] = clamp_u8_i32(round_shift_ties_to_even_i64_fast(partial_a[a_ptr[i]] + partial_b[b_ptr[i]], shift) + zp_out);
        }
    }
#else
    /* Original loop accessing 64KB flat LUT */
    size_t i = 0;
    for (; i + 8 <= n_elem; i += 8) {
        uint32_t a0 = a_ptr[i + 0];
        uint32_t a1 = a_ptr[i + 1];
        uint32_t a2 = a_ptr[i + 2];
        uint32_t a3 = a_ptr[i + 3];
        uint32_t a4 = a_ptr[i + 4];
        uint32_t a5 = a_ptr[i + 5];
        uint32_t a6 = a_ptr[i + 6];
        uint32_t a7 = a_ptr[i + 7];

        uint32_t b0 = b_ptr[i + 0];
        uint32_t b1 = b_ptr[i + 1];
        uint32_t b2 = b_ptr[i + 2];
        uint32_t b3 = b_ptr[i + 3];
        uint32_t b4 = b_ptr[i + 4];
        uint32_t b5 = b_ptr[i + 5];
        uint32_t b6 = b_ptr[i + 6];
        uint32_t b7 = b_ptr[i + 7];

        d_ptr[i + 0] = add_lut_flat[(a0 << 8) | b0];
        d_ptr[i + 1] = add_lut_flat[(a1 << 8) | b1];
        d_ptr[i + 2] = add_lut_flat[(a2 << 8) | b2];
        d_ptr[i + 3] = add_lut_flat[(a3 << 8) | b3];
        d_ptr[i + 4] = add_lut_flat[(a4 << 8) | b4];
        d_ptr[i + 5] = add_lut_flat[(a5 << 8) | b5];
        d_ptr[i + 6] = add_lut_flat[(a6 << 8) | b6];
        d_ptr[i + 7] = add_lut_flat[(a7 << 8) | b7];
    }

    for (; i < n_elem; i++) {
        d_ptr[i] = add_lut_flat[((uint32_t)a_ptr[i] << 8) | (uint32_t)b_ptr[i]];
    }
#endif

    u64_cycles t2 = timer_now();

    printf("[ADD TIMING] total      : %.3f ms\n", cycles_to_ms(t2 - t0));
    printf("[ADD TIMING] lut build  : %.3f ms\n", cycles_to_ms(t1 - t0));

#if USE_L1_SPLIT_LUT
    printf("[ADD TIMING] lut mode   : split-LUT L1-cache friendly exact\n");
#elif USE_APPROX_SPLIT_ROUNDING
    printf("[ADD TIMING] lut mode   : split-rounding approximate\n");
#else
    printf("[ADD TIMING] lut mode   : exact combined-rounding\n");
#endif

    printf("[ADD TIMING] lut apply  : %.3f ms (n_elem=%lu)\n",
           cycles_to_ms(t2 - t1), (unsigned long)n_elem);
}

/* ============================================================
 * Compare helper (optional)
 * ============================================================ */
int compare_tensor_vs_ref(const uint8_t *got, int C, int H, int W,
                          const char *ref_path, const char *label,
                          const runner_cfg_t *cfg)
{
    FIL fil;
    FRESULT fr;
    int mismatches = 0;
    int first_idx = -1;
    int W_padded = (W + 7) & ~7;
    /* mismatch-location bins (to see if the residual is an edge effect) */
    int mm_colL = 0, mm_colR = 0, mm_colMid = 0;
    int mm_rowT = 0, mm_rowB = 0, mm_rowMid = 0;
    int mm_minw = W, mm_maxw = -1, mm_minh = H, mm_maxh = -1;

    if (ensure_sd_mounted() != 0) {
        printf("[CMP] %s cannot mount SD\n", label);
        return -1;
    }

    fr = f_open(&fil, ref_path, FA_READ);
    if (fr != FR_OK) {
        printf("[CMP] %s cannot open ref %s (%d)\n", label, ref_path, (int)fr);
        return -1;
    }

    FSIZE_t expected_size = (FSIZE_t)C * H * W;
    if (f_size(&fil) != expected_size) {
        printf("[CMP] %s size mismatch file=%lu expected=%lu\n",
               label, (unsigned long)f_size(&fil), (unsigned long)expected_size);
        f_close(&fil);
        return -1;
    }

    #define CMP_BUF_SIZE 4096
    static uint8_t ref_row[CMP_BUF_SIZE];

    if (W > CMP_BUF_SIZE) {
        printf("[CMP] %s row width %d exceeds buffer size %d\n", label, W, CMP_BUF_SIZE);
        f_close(&fil);
        return -1;
    }

    size_t pixel_idx = 0;
    for (int c = 0; c < C; c++) {
        for (int h = 0; h < H; h++) {
            UINT read_len = 0;
            fr = f_read(&fil, ref_row, (UINT)W, &read_len);
            if (fr != FR_OK || read_len != (UINT)W) {
                printf("[CMP] %s read error at c=%d, h=%d, fr=%d, read=%u\n",
                       label, c, h, (int)fr, (unsigned)read_len);
                f_close(&fil);
                return -1;
            }

            const uint8_t *got_row = got + (size_t)c * H * W_padded + (size_t)h * W_padded;
            for (int w = 0; w < W; w++) {
                if (got_row[w] != ref_row[w]) {
                    if (mismatches < 8) {
                        printf("[CMP] %s mismatch at c=%d, h=%d, w=%d (pixel_idx=%lu): got=%u ref=%u\n",
                               label, c, h, w, (unsigned long)pixel_idx, got_row[w], ref_row[w]);
                    }
                    if (first_idx < 0) {
                        first_idx = (int)pixel_idx;
                    }
                    if (w < 8)        mm_colL++;
                    else if (w >= W-8) mm_colR++;
                    else               mm_colMid++;
                    if (h < 8)        mm_rowT++;
                    else if (h >= H-8) mm_rowB++;
                    else               mm_rowMid++;
                    if (w < mm_minw) mm_minw = w;
                    if (w > mm_maxw) mm_maxw = w;
                    if (h < mm_minh) mm_minh = h;
                    if (h > mm_maxh) mm_maxh = h;
                    mismatches++;
                }
                pixel_idx++;
            }
        }
    }

    f_close(&fil);

    if (mismatches == 0) {
        printf("[CMP] %s PASS\n", label);
    } else {
        printf("[CMP] %s FAIL mismatches=%d first=%d\n", label, mismatches, first_idx);
        printf("[CMP] %s loc cols: L(<8)=%d  R(>=%d)=%d  mid=%d\n",
               label, mm_colL, W-8, mm_colR, mm_colMid);
        printf("[CMP] %s loc rows: T(<8)=%d  B(>=%d)=%d  mid=%d\n",
               label, mm_rowT, H-8, mm_rowB, mm_rowMid);
        printf("[CMP] %s span: w[%d..%d] h[%d..%d]\n",
               label, mm_minw, mm_maxw, mm_minh, mm_maxh);
    }

    return mismatches;
}

static int load_file_into_buffer_sd(const char *path, uint8_t *dst, size_t max_bytes, size_t *bytes_out)
{
    FIL fil;
    FRESULT fr;
    FSIZE_t sz;
    UINT rd = 0;

    if (!dst) return -1;
    if (ensure_sd_mounted() != 0) return -1;

    fr = f_open(&fil, path, FA_READ);
    if (fr != FR_OK) {
        printf("[SD] open fail: %s (%d)\n", path, (int)fr);
        return -1;
    }

    sz = f_size(&fil);
    if ((size_t)sz > max_bytes) {
        printf("[SD] file too large for target buffer: %s size=%lu max=%lu\n",
               path, (unsigned long)sz, (unsigned long)max_bytes);
        f_close(&fil);
        return -1;
    }

    fr = f_read(&fil, dst, (UINT)sz, &rd);
    f_close(&fil);

    if (fr != FR_OK || rd != (UINT)sz) {
        printf("[SD] read fail: %s fr=%d rd=%u expected=%lu\n",
               path, (int)fr, (unsigned)rd, (unsigned long)sz);
        return -1;
    }

    if (bytes_out) *bytes_out = (size_t)sz;
    printf("[SD] Loaded %s into buffer, size = %lu bytes\n", path, (unsigned long)sz);
    return 0;
}

/* ============================================================
 * Low-level hardware hooks YOU must complete with your tested code
 * ============================================================ */

/*
 * REAL DW plane run.
 * Input/output are planar single-channel buffers of size H*W / outH*outW.
 *
 * Fill this with your working DW board code:
 * - program REG_DIMS / REG_PARAMS / REG_BIAS / REG_MULT / REG_SHIFT
 * - pack 9 weights into REG_W_0..REG_W_2
 * - DMA in one plane
 * - DMA out one plane
 * - wait done
 */
static int hw_dw_run_plane(const uint8_t *in_plane,
                           uint8_t *out_plane,
                           int H, int W,
                           int stride, int pad,
                           uint8_t zp_in, uint8_t zp_out,
                           int32_t bias, uint32_t mult, uint8_t shift,
                           const int8_t w9[9],
                           u64_cycles *out_mm2s_cyc,
                           u64_cycles *out_s2mm_cyc,
                           u64_cycles *out_total_cyc)
{
    int w_padded = (W + 7) & ~7;
    const int in_pixels  = H * w_padded;
    const int out_h      = (H + 2 * pad - 3) / stride + 1;
    const int true_out_w = (W + 2 * pad - 3) / stride + 1;
    int out_w_padded = (true_out_w + 7) & ~7;
    const int out_pixels = out_h * out_w_padded;
    int t;
    int stride_2 = (stride == 2);
    u64_cycles t_op_start;

    Xil_DCacheFlushRange((UINTPTR)in_plane,  in_pixels);
    Xil_DCacheInvalidateRange((UINTPTR)out_plane, out_pixels);

    dw_write_reg(DW_REG_STATUS, 1);
    dw_write_reg(DW_REG_DIMS,   dw_pack_dims(H, W));
    dw_write_reg(DW_REG_PARAMS, dw_pack_params(stride_2, pad, zp_in, zp_out));
    dw_write_reg(DW_REG_BIAS,   (u32)bias);
    dw_write_reg(DW_REG_MULT,   mult);
    dw_write_reg(DW_REG_SHIFT,  shift);

    dw_write_reg(DW_REG_W_0, dw_pack_w0(w9[0], w9[1], w9[2], w9[3]));
    dw_write_reg(DW_REG_W_1, dw_pack_w1(w9[4], w9[5], w9[6], w9[7]));
    dw_write_reg(DW_REG_W_2, dw_pack_w2(w9[8]));

#if DW_DIAG
    /* Read the registers back through the AXI-Lite slave. This proves what
     * the DW_conv_accel S00_AXI wrapper actually latched into slv_reg2..9
     * (the values the core sees), independent of what we *think* we wrote.
     * Only dump for the first 3 DW plane runs = the 3 channels of L0. */
    {
        static int dw_plane_call = 0;
        if (dw_plane_call < 3) {
            printf("[DIAG RB ch%d] dims=0x%08X params=0x%08X bias=0x%08X mult=0x%08X\n",
                   dw_plane_call,
                   (unsigned)dw_read_reg(DW_REG_DIMS),
                   (unsigned)dw_read_reg(DW_REG_PARAMS),
                   (unsigned)dw_read_reg(DW_REG_BIAS),
                   (unsigned)dw_read_reg(DW_REG_MULT));
            printf("[DIAG RB ch%d] shift=0x%08X w0=0x%08X w1=0x%08X w2=0x%08X status=0x%08X\n",
                   dw_plane_call,
                   (unsigned)dw_read_reg(DW_REG_SHIFT),
                   (unsigned)dw_read_reg(DW_REG_W_0),
                   (unsigned)dw_read_reg(DW_REG_W_1),
                   (unsigned)dw_read_reg(DW_REG_W_2),
                   (unsigned)dw_read_reg(DW_REG_STATUS));
            printf("[DIAG RB ch%d] sw-side: H=%d W=%d stride=%d pad=%d zp_in=%d zp_out=%d "
                   "in_pixels=%d out_h=%d out_w=%d out_pixels=%d\n",
                   dw_plane_call, H, W, stride, pad, zp_in, zp_out,
                   in_pixels, out_h, true_out_w, out_pixels);
        }
        dw_plane_call++;
    }
#endif

    /* ---- Arm S2MM, pulse start (resets FIFOs), [settle], arm MM2S ---- */
    /* True operation start: S2MM is armed (and thus can start accepting
     * output beats) before MM2S is even submitted, so this — not the later
     * MM2S timer — is the correct t=0 for both S2MM's own duration and the
     * plane's total wall time. */
    t_op_start = timer_now();
    if (XAxiDma_SimpleTransfer(&DwDma, (UINTPTR)out_plane, out_pixels, XAXIDMA_DEVICE_TO_DMA) != XST_SUCCESS) {
        printf("ERROR: DW S2MM submit failed\n");
        return -1;
    }

    /* Pulse start — resets the input/output FIFOs, then core begins.
     * MUST happen BEFORE MM2S arm, otherwise the DMA pushes data into the
     * input FIFO and the start-triggered FIFO reset destroys it. */
    dw_write_reg(DW_REG_CTRL, 1);
    dw_write_reg(DW_REG_CTRL, 0);

#if DW_DIAG_START_SETTLE
    /* Settling delay: dummy AXI reads let the start FIFO-reset finish before the
     * first input beat arrives (tests a start<->first-beat timing race). */
    { volatile u32 _junk = 0; for (int _s = 0; _s < DW_DIAG_START_SETTLE; _s++) _junk = dw_read_reg(DW_REG_STATUS); (void)_junk; }
#endif

#if DW_DIAG_PRIME_BEAT
    /* Prime one dummy beat at the front of the input stream (reads 8 throwaway
     * bytes just below in_plane; value irrelevant, it's consumed/dropped as the
     * prime). Length +8 = one extra beat. */
    if (XAxiDma_SimpleTransfer(&DwDma, (UINTPTR)(in_plane - 8), in_pixels + 8, XAXIDMA_DMA_TO_DEVICE) != XST_SUCCESS) {
        printf("ERROR: DW MM2S(prime) submit failed\n");
        return -1;
    }
#else
    if (XAxiDma_SimpleTransfer(&DwDma, (UINTPTR)in_plane, in_pixels, XAXIDMA_DMA_TO_DEVICE) != XST_SUCCESS) {
        printf("ERROR: DW MM2S submit failed\n");
        return -1;
    }
#endif

    {
        u64_cycles tm0 = timer_now();
        t = 0;
        while (XAxiDma_Busy(&DwDma, XAXIDMA_DMA_TO_DEVICE)) {
            if (++t > 50000000) {
                printf("ERROR: DW MM2S timeout\n");
                return -1;
            }
        }
        if (out_mm2s_cyc) *out_mm2s_cyc = timer_now() - tm0;
    }

    {
        t = 0;
        while (XAxiDma_Busy(&DwDma, XAXIDMA_DEVICE_TO_DMA)) {
            if (++t > 50000000) {
                printf("ERROR: DW S2MM timeout\n");
                return -1;
            }
        }
        /* S2MM's own busy window, measured from its true arm time, not from
         * when MM2S happened to finish. For DW this also IS the plane's
         * total wall time — there's no further wait after S2MM clears. */
        {
            u64_cycles now = timer_now();
            if (out_s2mm_cyc)  *out_s2mm_cyc  = now - t_op_start;
            if (out_total_cyc) *out_total_cyc = now - t_op_start;
        }
    }

    Xil_DCacheInvalidateRange((UINTPTR)out_plane, out_pixels);
    return 0;
}

static int run_pw_once(int tile_pixels, int cin_run, int cout_run,
                       uint8_t zp_in, uint8_t zp_out, int relu_en,
                       uint8_t *tx_buf, int tx_len,
                       uint8_t *rx_buf, int rx_len,
                       u64_cycles *out_mm2s_cyc,
                       u64_cycles *out_s2mm_cyc,
                       u64_cycles *out_total_cyc)
{
    int t;
    u64_cycles t_op_start;

    //printf("[PW_ONCE] tile_pixels=%d cin_run=%d cout_run=%d tx_len=%d rx_len=%d tx=0x%08X rx=0x%08X\n",
    //       tile_pixels, cin_run, cout_run, tx_len, rx_len,
    //       (unsigned)(UINTPTR)tx_buf, (unsigned)(UINTPTR)rx_buf);

    Xil_DCacheFlushRange((UINTPTR)tx_buf, tx_len);
    Xil_DCacheInvalidateRange((UINTPTR)rx_buf, rx_len);

    /* Program runtime config */
    pw_write_reg(PW_REG_TILE_PIXELS, tile_pixels);
    pw_write_reg(PW_REG_CIN_RUN, cin_run);
    pw_write_reg(PW_REG_COUT_RUN, cout_run);
    pw_write_reg(PW_REG_ZP_RELU, pw_pack_zp_relu(zp_in, zp_out, relu_en));

    /* Verify registers were written */
    //printf("[PW_ONCE] readback: TILE_PIXELS=%u CIN_RUN=%u COUT_RUN=%u ZP_RELU=0x%08X\n",
    //       (unsigned)pw_read_reg(PW_REG_TILE_PIXELS),
    //       (unsigned)pw_read_reg(PW_REG_CIN_RUN),
    //       (unsigned)pw_read_reg(PW_REG_COUT_RUN),
    //       (unsigned)pw_read_reg(PW_REG_ZP_RELU));

    /* Read status BEFORE clear */
    //printf("[PW_ONCE] STATUS before clear: 0x%08X\n",
    //       (unsigned)pw_read_reg(PW_REG_STATUS));

    /* Clear/reset core state BEFORE DMA/start */
    pw_write_reg(PW_REG_CTRL, (1u << 1) | (1u << 2));
    pw_write_reg(PW_REG_CTRL, 0);

    //printf("[PW_ONCE] STATUS after clear: 0x%08X\n",
    //       (unsigned)pw_read_reg(PW_REG_STATUS));

    /* Arm RX (S2MM) first — so output has somewhere to drain.
     * True operation start: S2MM can begin accepting output beats from here,
     * well before MM2S is even submitted (PW streams into DDR as soon as
     * completed tiles are dumped, not only after MM2S fully finishes). */
    t_op_start = timer_now();
    if (XAxiDma_SimpleTransfer(&PwDma, (UINTPTR)rx_buf, rx_len, XAXIDMA_DEVICE_TO_DMA) != XST_SUCCESS) {
        printf("ERROR: PW S2MM submit failed\n");
        return -1;
    }
    //printf("[PW_ONCE] S2MM armed\n");

    /* Pulse start — this resets both FIFOs for 5 cycles, then core begins.
     * MUST happen BEFORE MM2S arm, otherwise the DMA pushes data into the
     * input FIFO and the start-triggered FIFO reset destroys it. */
    pw_write_reg(PW_REG_CTRL, 1u << 0);
    pw_write_reg(PW_REG_CTRL, 0);

    /* Check if start took effect */
    //printf("[PW_ONCE] STATUS after start: 0x%08X\n",
    //       (unsigned)pw_read_reg(PW_REG_STATUS));

    /* Now arm TX (MM2S) — data flows into FIFO AFTER reset is done */
    if (XAxiDma_SimpleTransfer(&PwDma, (UINTPTR)tx_buf, tx_len, XAXIDMA_DMA_TO_DEVICE) != XST_SUCCESS) {
        printf("ERROR: PW MM2S submit failed\n");
        return -1;
    }
    //printf("[PW_ONCE] MM2S armed\n");

    {
        u64_cycles tm0 = timer_now();
        t = 0;
        while (XAxiDma_Busy(&PwDma, XAXIDMA_DMA_TO_DEVICE)) {
            if (++t > 50000000) {
                printf("ERROR: PW MM2S timeout\n");
                return -1;
            }
        }
        if (out_mm2s_cyc) *out_mm2s_cyc = timer_now() - tm0;
    }
    //printf("[PW_ONCE] MM2S done\n");

    {
        t = 0;
        while (XAxiDma_Busy(&PwDma, XAXIDMA_DEVICE_TO_DMA)) {
            if (++t > 50000000) {
                printf("ERROR: PW S2MM timeout\n");
                return -1;
            }
        }
        /* S2MM's own busy window, measured from its true arm time. Unlike
         * DW, this is NOT necessarily the tile's total wall time — the core
         * must still assert done below, which for PW's accumulate-then-dump
         * architecture can trail S2MM's last accepted beat. */
        if (out_s2mm_cyc) *out_s2mm_cyc = timer_now() - t_op_start;
    }
    //printf("[PW_ONCE] S2MM done\n");

    t = 0;
    while ((pw_read_reg(PW_REG_STATUS) & 1) == 0) {
        if (++t > 5000000) {
            u32 st  = pw_read_reg(PW_REG_STATUS);
            u32 st2 = pw_read_reg(PW_REG_STATUS2);
            printf("ERROR: PW core did not complete. STATUS=0x%08X STATUS2=0x%08X\n",
                   (unsigned)st, (unsigned)st2);
            return -1;
        }
    }
    //printf("[PW_ONCE] core done OK\n");

    if (out_total_cyc) *out_total_cyc = timer_now() - t_op_start;

    Xil_DCacheInvalidateRange((UINTPTR)rx_buf, rx_len);
    return 0;
}

/*
 * REAL PW full-layer run.
 * Input is planar [Cin][H][W], output is planar [Cout][outH][outW].
 *
 * Fill this with your working PW board code.
 * Since your tested PW board flow already handles one OC at a time,
 * this helper should loop OC=0..Cout-1 internally and call the real PW engine.
 */

/*
 * ============================================================
 * Input tensor layout note
 * ============================================================
 *
 * in_tensor is stored PLANAR (channel-major):
 *   in_tensor[ic * total_pixels + p]
 *
 * The PW DMA hardware expects each transfer to be a contiguous
 * tiled-planar slice:
 *   bytes[ic * tile_pixels + p]
 *
 * Single-tile case:
 *   if total_pixels <= PW_TILE_PIXELS, then tile_pixels == total_pixels
 *   and the full planar tensor already matches the required transfer
 *   layout for one DMA call, so tx_ptr = in_tensor is valid.
 *
 * Multi-tile case:
 *   each tile is a non-contiguous slice from each channel plane, so
 *   it must be packed into a contiguous buffer before DMA.
 *
 * Strategy:
 *
 * Case A: full packed layer fits in memory
 *   - allocate one full-layer packed buffer
 *   - pack once before OC loop
 *   - reuse for all OCs
 *
 * Case B: full packed layer too large
 *   - allocate one tile scratch buffer
 *   - for each tile: pack once
 *   - run all OCs on that packed tile
 *
 * This removes the old Cout * Ntiles repack behavior.
 */

#define PW_PACK_BUDGET  (64u * 1024u * 1024u)
#define PW_MAX_TILES    1024

/* ------------------------------------------------------------------ */
/* Pack the full planar input into a contiguous tiled buffer          */
/* ------------------------------------------------------------------ */
static int pack_layer_input(const uint8_t *in_tensor,
                            int            total_pixels,
                            int            Cin,
                            uint8_t       *packed,
                            size_t        *tile_offsets,
                            int           *tile_sizes)
{
    int    ntiles    = 0;
    size_t write_pos = 0;

    for (int pix_base = 0; pix_base < total_pixels; pix_base += PW_TILE_PIXELS) {
        int tile_pixels = total_pixels - pix_base;
        if (tile_pixels > PW_TILE_PIXELS) tile_pixels = PW_TILE_PIXELS;

        if (ntiles >= PW_MAX_TILES) {
            printf("[PW] ERROR: ntiles exceeds PW_MAX_TILES (%d)\n", PW_MAX_TILES);
            return -1;
        }

        tile_offsets[ntiles] = write_pos;
        tile_sizes[ntiles]   = tile_pixels;

        for (int ic = 0; ic < Cin; ic++) {
            const uint8_t *src = in_tensor + ((size_t)ic * (size_t)total_pixels) + pix_base;
            memcpy(packed + write_pos + ((size_t)ic * (size_t)tile_pixels),
                   src,
                   (size_t)tile_pixels);
        }

        write_pos += (size_t)tile_pixels * (size_t)Cin;
        ntiles++;
    }

    return ntiles;
}

/* ------------------------------------------------------------------ */
/* Pack one tile from planar input into contiguous scratch            */
/* ------------------------------------------------------------------ */
static void pack_one_tile(const uint8_t *in_tensor,
                          int            total_pixels,
                          int            Cin,
                          int            pix_base,
                          int            tile_pixels,
                          uint8_t       *dst)
{
    for (int ic = 0; ic < Cin; ic++) {
        const uint8_t *src = in_tensor + ((size_t)ic * (size_t)total_pixels) + pix_base;
        memcpy(dst + ((size_t)ic * (size_t)tile_pixels),
               src,
               (size_t)tile_pixels);
    }
}

/* ------------------------------------------------------------------ */
/* Transpose logic (O3 optimized to avoid pointer math bottlenecks)   */
/* ------------------------------------------------------------------ */
typedef uint64_t __attribute__((__may_alias__)) aliased_u64;

__attribute__((optimize("O3")))
static void fast_transpose_pw_store(uint8_t *out_tensor, const uint8_t *pw_rx_tile,
                                    int tile_groups, int Cout, int cout_rounded,
                                    int total_pixels, int pix_base)
{
    const int BLOCK_OC = 16;
    const int BLOCK_G  = 32;

    static uint8_t scratch[32][16][8] __attribute__((aligned(64)));

    for (int g_blk = 0; g_blk < tile_groups; g_blk += BLOCK_G) {
        int g_end = g_blk + BLOCK_G;
        if (g_end > tile_groups) g_end = tile_groups;
        int g_cnt = g_end - g_blk;

        for (int oc_blk = 0; oc_blk < Cout; oc_blk += BLOCK_OC) {
            int oc_end = oc_blk + BLOCK_OC;
            if (oc_end > Cout) oc_end = Cout;
            int oc_cnt = oc_end - oc_blk;

            for (int g = 0; g < g_cnt; g++) {
                const uint8_t *src_row = pw_rx_tile + ((g_blk + g) * cout_rounded + oc_blk) * 8;
                uint8_t *scr = &scratch[g][0][0];

                if (g + 4 < g_cnt) {
                    const uint8_t *pf = pw_rx_tile + ((g_blk + g + 4) * cout_rounded + oc_blk) * 8;
                    __builtin_prefetch(pf, 0, 1);
                }

                for (int oc_local = 0; oc_local < oc_cnt; oc_local++) {
                    *(aliased_u64 *)scr = *(const aliased_u64 *)src_row;
                    scr     += 8;
                    src_row += 8;
                }
            }

            for (int oc_local = 0; oc_local < oc_cnt; oc_local++) {
                int oc = oc_blk + oc_local;
                uint8_t *dst = out_tensor + oc * total_pixels + pix_base + g_blk * 8;
                const uint8_t *scr = &scratch[0][oc_local][0];

                for (int g = 0; g < g_cnt; g++) {
                    *(aliased_u64 *)dst = *(const aliased_u64 *)scr;
                    dst += 8;
                    scr += 128; /* BLOCK_OC * 8 */
                }
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* Main function                                                      */
/* ------------------------------------------------------------------ */
static int hw_pw_run_full_layer(const uint8_t      *in_tensor,
                                uint8_t            *out_tensor,
                                int H, int W,
                                int Cin, int Cout,
                                uint8_t zp_in, uint8_t zp_out,
                                int relu_en,
                                const file_blob_t  *params_blob,
                                const file_blob_t  *weights_blob,
                                uint32_t            params_addr,
                                uint32_t            weight_addr,
                                int                 layer_idx)
{
    const int    W_padded      = (W + 7) & ~7;
    const int    total_pixels  = H * W_padded;
    const int    single_tile   = (total_pixels <= PW_TILE_PIXELS);
    const size_t full_pack_sz  = (size_t)total_pixels * (size_t)Cin;
    const int    use_full_pack = (!single_tile && (full_pack_sz <= PW_PACK_BUDGET));

    /* Round Cout up to next multiple of N_OC so the core processes
     * complete batches.  Extra channels produce garbage we discard. */
    const int    cout_rounded  = ((Cout + PW_N_OC - 1) / PW_N_OC) * PW_N_OC;

    size_t tile_offsets[PW_MAX_TILES];
    int    tile_sizes[PW_MAX_TILES];
    int    ntiles = 0;

    static int8_t   w_ic[240];
    static int32_t  bias_cache[256];
    static uint32_t mult_cache[256];
    static uint8_t  shift_cache[256];

    uint8_t *full_pack_buf = (uint8_t *)PW_PACK_BUF_ADDR;
    uint8_t *tile_scratch  = (uint8_t *)PW_TILE_SCRATCH;
    uint8_t *pw_rx_tile    = (uint8_t *)PW_RX_TILE_ADDR;

    u64_cycles cyc_total_start, cyc_total_end;
    u64_cycles cyc_param  = 0;
    u64_cycles cyc_weight = 0;
    u64_cycles cyc_pack   = 0;
    u64_cycles cyc_dma    = 0;
    u64_cycles cyc_store  = 0;
    uint64_t   layer_bytes = 0, layer_macs = 0;

    if (Cin > 240) {
        printf("[PW] Cin exceeds 240: %d\n", Cin);
        return -1;
    }
    if (Cout > 256) {
        printf("[PW] Cout exceeds 256: %d\n", Cout);
        return -1;
    }

    cyc_total_start = timer_now();

    /* ============================================================
     * Step 1: preload ALL params into param BRAMs
     * ============================================================ */
    {
        u64_cycles t0 = timer_now();

        for (int oc = 0; oc < Cout; oc++) {
            if (load_param_block_16B(params_blob, params_addr, oc,
                                     &bias_cache[oc],
                                     &mult_cache[oc],
                                     &shift_cache[oc]) != 0) {
                printf("[PW] param preload fail oc=%d\n", oc);
                return -1;
            }

            /* Write params into the on-chip param BRAMs at address = oc */
            pw_write_reg(PW_REG_PARAM_ADDR, oc);
            pw_write_reg(PW_REG_BIAS,  (u32)bias_cache[oc]);
            pw_write_reg(PW_REG_MULT,  mult_cache[oc]);
            pw_write_reg(PW_REG_SHIFT, shift_cache[oc]);
        }

        cyc_param = timer_now() - t0;
    }

    /* ============================================================
     * Step 2: preload ALL weights into weight BRAMs
     *
     * Weight BRAM layout per bank:
     *   address = batch * Cin + ic
     *   where batch = oc / N_OC, bank = oc % N_OC
     * ============================================================ */
    {
        u64_cycles t0 = timer_now();

        for (int oc = 0; oc < Cout; oc++) {
            int bank  = oc % PW_N_OC;
            int batch = oc / PW_N_OC;

            if (load_pw_weights_for_oc(weights_blob, weight_addr, oc, Cin, w_ic) != 0) {
                printf("[PW] weight load fail oc=%d\n", oc);
                return -1;
            }

            pw_write_reg(PW_REG_OC_SEL, bank);
            pw_write_reg(PW_REG_W_BRAM_OFF, batch * Cin);
            for (int ic = 0; ic < Cin; ic++) {
                pw_write_weight(ic, w_ic[ic]);
            }
        }

        cyc_weight = timer_now() - t0;
        printf("[PW] preloaded %d OC weights (cout_rounded=%d)\n", Cout, cout_rounded);
    }

    /* ============================================================
     * Step 3: prepare tile metadata / packed input
     * ============================================================ */
    if (single_tile) {
        tile_offsets[0] = 0;
        tile_sizes[0]   = total_pixels;
        ntiles          = 1;

    } else if (use_full_pack) {
        u64_cycles t0 = timer_now();

        ntiles = pack_layer_input(in_tensor, total_pixels, Cin,
                                  full_pack_buf, tile_offsets, tile_sizes);
        if (ntiles < 0) return -1;

        cyc_pack += timer_now() - t0;

    } else {
        ntiles = 0;
        for (int pix_base = 0; pix_base < total_pixels; pix_base += PW_TILE_PIXELS) {
            int tp = total_pixels - pix_base;
            if (tp > PW_TILE_PIXELS) tp = PW_TILE_PIXELS;

            if (ntiles >= PW_MAX_TILES) {
                printf("[PW] ERROR: ntiles exceeds PW_MAX_TILES (%d)\n", PW_MAX_TILES);
                return -1;
            }

            tile_sizes[ntiles]   = tp;
            tile_offsets[ntiles] = (size_t)pix_base;
            ntiles++;
        }
    }

    /* ============================================================
     * Step 4: run tiles — single DMA per tile, all OCs at once
     *
     * Core output order (pixel-major):
     *   for each pixel_group g (N_LANES pixels):
     *     for each oc in 0..cout_rounded-1:
     *       N_LANES output bytes
     *
     * We transpose to channel-major: out_tensor[oc][pixel]
     * ============================================================ */
    for (int t = 0; t < ntiles; t++) {
        const int tile_pixels = tile_sizes[t];
        const int tile_groups = tile_pixels / PW_N_LANES;
        const int tx_len      = tile_pixels * Cin;
        const int rx_len      = tile_pixels * cout_rounded;
        int       pix_base;

        /* Determine input pointer and pix_base */
        const uint8_t *tx_ptr;
        if (single_tile) {
            tx_ptr   = in_tensor;
            pix_base = 0;
        } else if (use_full_pack) {
            tx_ptr   = full_pack_buf + tile_offsets[t];
            pix_base = t * PW_TILE_PIXELS;
        } else {
            /* scratch-tile: pack this tile */
            pix_base = (int)tile_offsets[t];
            {
                u64_cycles t0 = timer_now();
                pack_one_tile(in_tensor, total_pixels, Cin,
                              pix_base, tile_pixels, tile_scratch);
                cyc_pack += timer_now() - t0;
            }
            tx_ptr = tile_scratch;
        }

        /* Single DMA transfer for all Cout channels */
        {
            u64_cycles t0 = timer_now();
            u64_cycles mm2s_cyc = 0, s2mm_cyc = 0, total_cyc = 0;

            if (run_pw_once(tile_pixels, Cin, cout_rounded,
                            zp_in, zp_out, relu_en,
                            (uint8_t *)tx_ptr, tx_len,
                            pw_rx_tile, rx_len,
                            &mm2s_cyc, &s2mm_cyc, &total_cyc) != 0) {
                printf("[PW] run fail tile=%d\n", t);
                return -1;
            }

            cyc_dma += timer_now() - t0;

            note_bw(&g_peak_pw_mm2s, (size_t)tx_len, mm2s_cyc, layer_idx, t);
            note_bw(&g_peak_pw_s2mm, (size_t)rx_len, s2mm_cyc, layer_idx, t);
            /* MACs = one MAC per (pixel, input channel, output channel).
             * total_cyc spans S2MM-arm through core-done (not just S2MM's
             * own busy window), since PW's accumulate-then-dump pipeline can
             * leave the core still finishing after S2MM's last accepted beat. */
            note_thr(&g_peak_pw_thr,
                     (uint64_t)tile_pixels * (uint64_t)Cin * (uint64_t)cout_rounded,
                     total_cyc, layer_idx);

            g_total_pw_bytes += (uint64_t)tx_len + (uint64_t)rx_len;
            g_total_pw_macs  += (uint64_t)tile_pixels * (uint64_t)Cin * (uint64_t)cout_rounded;
            layer_bytes      += (uint64_t)tx_len + (uint64_t)rx_len;
            layer_macs       += (uint64_t)tile_pixels * (uint64_t)Cin * (uint64_t)cout_rounded;
        }

        {
            u64_cycles t0 = timer_now();
            fast_transpose_pw_store(out_tensor, pw_rx_tile, tile_groups, Cout, cout_rounded, total_pixels, pix_base);
            cyc_store += timer_now() - t0;
        }

        if ((t % 4) == 0 || t == ntiles - 1) {
            printf("[PW] tile %d / %d done\n", t + 1, ntiles);
        }
    }

    cyc_total_end = timer_now();

    g_total_layer_cyc   += (cyc_total_end - cyc_total_start);
    g_total_dma_run_cyc += cyc_dma;

    g_layer_stats[layer_idx].kind       = KIND_PW;
    g_layer_stats[layer_idx].Cin        = Cin;
    g_layer_stats[layer_idx].Cout       = Cout;
    g_layer_stats[layer_idx].H          = H;
    g_layer_stats[layer_idx].W          = W;
    g_layer_stats[layer_idx].bytes      = layer_bytes;
    g_layer_stats[layer_idx].macs       = layer_macs;
    g_layer_stats[layer_idx].wall_ms    = cycles_to_ms(cyc_total_end - cyc_total_start);
    g_layer_stats[layer_idx].dma_run_ms = cycles_to_ms(cyc_dma);

    {
        const char *case_str =
            single_tile   ? "single_tile" :
            use_full_pack ? "full_pack"   :
                            "tile_scratch";

        printf("[PW TIMING] H=%d W=%d Cin=%d Cout=%d cout_rounded=%d total_pixels=%d ntiles=%d case=%s\n",
               H, W, Cin, Cout, cout_rounded, total_pixels, ntiles, case_str);
        printf("[PW TIMING] total   : %.3f ms\n", cycles_to_ms(cyc_total_end - cyc_total_start));
        printf("[PW TIMING] params  : %.3f ms (preload to BRAMs)\n", cycles_to_ms(cyc_param));
        printf("[PW TIMING] weights : %.3f ms (preload to BRAMs)\n", cycles_to_ms(cyc_weight));
        printf("[PW TIMING] pack    : %.3f ms\n", cycles_to_ms(cyc_pack));
        printf("[PW TIMING] dma/run : %.3f ms (%d tiles x 1 DMA each)\n", cycles_to_ms(cyc_dma), ntiles);
        printf("[PW TIMING] store   : %.3f ms (transpose)\n", cycles_to_ms(cyc_store));
    }

    return 0;
}

/* ============================================================
 * Layer wrappers
 * ============================================================ */
int run_dw_layer_debug(const layer_desc_t *d,
                       const uint8_t *in_tensor,
                       uint8_t *out_tensor)
{
    (void)d; (void)in_tensor; (void)out_tensor;
    return -1;
}

int run_pw_layer_debug(const layer_desc_t *d,
                       const uint8_t *in_tensor,
                       uint8_t *out_tensor)
{
    (void)d; (void)in_tensor; (void)out_tensor;
    return -1;
}

static int run_dw_layer_real(const layer_desc_t *d,
                             const uint8_t *in_tensor,
                             uint8_t *out_tensor,
                             const file_blob_t *params_blob,
                             const file_blob_t *weights_blob)
{
    u64_cycles t_start = timer_now();
    u64_cycles cyc_param = 0, cyc_weight = 0, cyc_run = 0;
    uint64_t layer_bytes = 0, layer_macs = 0;

    tensor_shape_t outs = get_layer_output_shape(d);
    const int in_pixels = d->H * ((d->W + 7) & ~7);
    const int out_pixels = outs.H * ((outs.W + 7) & ~7);
    int8_t w9[9];

    for (int c = 0; c < d->Cin; c++) {
        int32_t bias;
        uint32_t mult;
        uint8_t shift;
        const uint8_t *in_plane = in_tensor + (size_t)c * (size_t)in_pixels;
        uint8_t *out_plane = out_tensor + (size_t)c * (size_t)out_pixels;

        u64_cycles t0 = timer_now();
        if (load_param_block_16B(params_blob, d->params_addr, c, &bias, &mult, &shift) != 0) {
            printf("[DW] param load fail ch=%d\n", c);
            return -1;
        }
        cyc_param += timer_now() - t0;

        t0 = timer_now();
        if (load_dw_weights_3x3(weights_blob, d->weight_addr, c, w9) != 0) {
            printf("[DW] weight load fail ch=%d\n", c);
            return -1;
        }
        cyc_weight += timer_now() - t0;

#if DW_DIAG_WEIGHT_SWAP
        /* Pre-swap kernel rows 0 (top) and 1 (middle). The hardware applies an
         * effective kernel with top/middle swapped (line-buffer BRAM bank
         * mis-order), so programming pre-swapped weights cancels it. If this
         * works, the impulse stamp rows come out as w[2]/w[1]/w[0] (correct
         * order) — only the -8 column shift should remain. */
        { int8_t tmp; for (int j = 0; j < 3; j++) { tmp = w9[j]; w9[j] = w9[j+3]; w9[j+3] = tmp; } }
#endif

#if DW_DIAG
        if (d->layer_idx == 0) {
            printf("[DIAG L00 ch%d] bias=%d mult=%u shift=%u\n",
                   c, (int)bias, (unsigned)mult, (unsigned)shift);
            printf("[DIAG L00 ch%d] weights:", c);
            for (int j = 0; j < 9; j++) printf(" %d", (int)w9[j]);
            printf("\n");
            printf("[DIAG L00 ch%d] in_plane[0..15]:", c);
            for (int j = 0; j < 16; j++) printf(" %02X", in_plane[j]);
            printf("\n");
        }
#else
        if (d->layer_idx == 0 && c == 0) {
            printf("[DEBUG L00 K0] bias=%d mult=%u shift=%u\n", (int)bias, (unsigned)mult, (unsigned)shift);
            printf("[DEBUG L00 K0] weights: ");
            for (int j = 0; j < 9; j++) {
                printf("%d ", (int)w9[j]);
            }
            printf("\n");
        }
#endif

#if DW_DIAG_IMPULSE
        if (d->layer_idx == 0 && c == 0) {
            int wpad = (d->W + 7) & ~7;
            uint8_t *imp = (uint8_t *)in_plane;   /* buf_cur plane 0, writable */
            /* Multiple impulses down the SAME column at spaced-out rows. If the
             * output stamps all sit at (row, c0-8) the shift is a constant per
             * row; if the column shift GROWS with row, beats are being
             * inserted/dropped in the DMA stream (accumulating) -> PS/DMA. */
            /* Mix of EVEN (16,512) and ODD (1025,1537) input rows. If the
             * w0/w1 swap looks identical on odd rows -> uniform kernel swap ->
             * a global weight pre-swap can compensate it. If odd rows differ
             * (no swap / opposite) -> parity-dependent -> a global swap breaks
             * them, and input pre-compensation is not viable. */
            static const int imp_rows[] = { 16, 512, 1025, 1537 };
            memset(imp, 0, (size_t)in_pixels);
            printf("[IMPULSE] L0 ch0: V=%d at c=%d, rows:", DW_IMP_V, DW_IMP_C0);
            for (int k = 0; k < (int)(sizeof(imp_rows)/sizeof(imp_rows[0])); k++) {
                imp[(size_t)imp_rows[k] * wpad + DW_IMP_C0] = (uint8_t)DW_IMP_V;
                printf(" %d", imp_rows[k]);
            }
            printf(" (wpad=%d)\n", wpad);
        }
#endif

        t0 = timer_now();
        {
            u64_cycles mm2s_cyc = 0, s2mm_cyc = 0, total_cyc = 0;
            if (hw_dw_run_plane(in_plane, out_plane,
                                d->H, d->W,
                                d->stride, d->pad,
                                d->zp_in, d->zp_out,
                                bias, mult, shift,
                                w9,
                                &mm2s_cyc, &s2mm_cyc, &total_cyc) != 0) {
                printf("[DW] hw run fail ch=%d\n", c);
                return -1;
            }
            note_bw(&g_peak_dw_mm2s, (size_t)in_pixels, mm2s_cyc, d->layer_idx, c);
            note_bw(&g_peak_dw_s2mm, (size_t)out_pixels, s2mm_cyc, d->layer_idx, c);
            /* 3x3 kernel -> 9 MACs per output pixel (padded pixels included,
             * a minor overcount vs true_out_w that doesn't matter for a peak
             * figure since it's the same for every DW plane). total_cyc is
             * the plane's true wall time (S2MM-arm to S2MM-done), not a sum
             * of two windows that would now overlap. */
            note_thr(&g_peak_dw_thr, (uint64_t)out_pixels * 9ull, total_cyc, d->layer_idx);

            g_total_dw_bytes += (uint64_t)in_pixels + (uint64_t)out_pixels;
            g_total_dw_macs  += (uint64_t)out_pixels * 9ull;
            layer_bytes      += (uint64_t)in_pixels + (uint64_t)out_pixels;
            layer_macs       += (uint64_t)out_pixels * 9ull;
        }
        cyc_run += timer_now() - t0;

#if DW_DIAG_IMPULSE
        if (d->layer_idx == 0 && c == 0) {
            int owpad = ((((d->W + 2*d->pad - 3)/d->stride + 1) + 7) & ~7);
            int64_t xb = ((int64_t)0 + bias) * (int64_t)mult;
            int bg = clamp_u8_i32(round_shift_ties_to_even_i64(xb, shift) + (int)d->zp_out);
            printf("[IMPULSE] expected background (far interior) = %d\n", bg);
            printf("[IMPULSE] EXPECTED 3x3 stamp (out rows r0-1..r0+1, cols c0-1..c0+1):\n");
            for (int a = -1; a <= 1; a++) {
                printf("   EXP:");
                for (int b = -1; b <= 1; b++) {
                    int tap = (int)w9[(1-a)*3 + (1-b)];
                    int64_t x = ((int64_t)tap * DW_IMP_V + bias) * (int64_t)mult;
                    int y = clamp_u8_i32(round_shift_ties_to_even_i64(x, shift) + (int)d->zp_out);
                    printf(" %3d", y);
                }
                printf("\n");
            }
            /* With zp_in=0 and an all-zero input plane, EVERY output pixel
             * (edges included) is background; the ONLY non-bg pixels are the
             * impulse's 3x3 response. Scan the whole plane to locate it. */
            int out_h2      = (d->H + 2*d->pad - 3)/d->stride + 1;
            int true_out_w2 = (d->W + 2*d->pad - 3)/d->stride + 1;
            int found = 0;
            printf("[IMPULSE] scanning full output plane for non-bg(%d) pixels (impulse in @ r=%d,c=%d):\n",
                   bg, DW_IMP_R0, DW_IMP_C0);
            for (int r = 0; r < out_h2 && found < 48; r++) {
                for (int cc = 0; cc < true_out_w2; cc++) {
                    uint8_t v = out_plane[(size_t)r * owpad + cc];
                    if (v != (uint8_t)bg) {
                        printf("   nz @ (r=%d,c=%d) = %u\n", r, cc, v);
                        if (++found >= 48) break;
                    }
                }
            }
            if (found == 0)
                printf("   (NONE — output is uniformly background; impulse never reached the core)\n");
            else
                printf("   total non-bg reported: %d (cap 48)\n", found);

            /* Re-read the impulse byte straight from DRAM (invalidate its line
             * first) to confirm the DMA actually saw 255 — i.e. that the flush
             * landed. This separates "input delivery" from "core compute". */
            {
                int wpad2 = (d->W + 7) & ~7;
                size_t off = (size_t)DW_IMP_R0 * wpad2 + DW_IMP_C0;
                uint8_t *line = (uint8_t *)((UINTPTR)(in_plane + off) & ~((UINTPTR)31));
                Xil_DCacheInvalidateRange((UINTPTR)line, 64);
                printf("[IMPULSE] DRAM readback @off=%d: %u (expect %d); around:",
                       (int)off, (unsigned)in_plane[off], DW_IMP_V);
                for (int k = -2; k <= 2; k++) printf(" %u", (unsigned)in_plane[off + k]);
                printf("\n");
            }
        }
#endif

#if DW_DIAG
        if (d->layer_idx == 0) {
            /* out_plane already cache-invalidated inside hw_dw_run_plane */
            int owpad2 = (d->W + 7) & ~7;   /* stride-1 => out_w_padded == W_padded */
            printf("[DIAG L00 ch%d] out_plane[0..15]:", c);
            for (int j = 0; j < 16; j++) printf(" %02X", out_plane[j]);
            printf("\n");
            /* right edge of row 0 (decimal) — compare to golden l00 */
            printf("[DIAG L00 ch%d] out row0 cols[1425..1434]:", c);
            for (int j = 1425; j <= 1434; j++) printf(" %3u", out_plane[j]);
            printf("\n");
            printf("[DIAG L00 ch%d] out row100 cols[1425..1434]:", c);
            for (int j = 1425; j <= 1434; j++) printf(" %3u", out_plane[(size_t)100*owpad2 + j]);
            printf("\n");
        }
#endif
    }

    u64_cycles t_end = timer_now();

    g_total_layer_cyc   += (t_end - t_start);
    g_total_dma_run_cyc += cyc_run;

    g_layer_stats[d->layer_idx].kind       = d->kind;
    g_layer_stats[d->layer_idx].Cin        = d->Cin;
    g_layer_stats[d->layer_idx].Cout       = d->Cout;
    g_layer_stats[d->layer_idx].H          = d->H;
    g_layer_stats[d->layer_idx].W          = d->W;
    g_layer_stats[d->layer_idx].bytes      = layer_bytes;
    g_layer_stats[d->layer_idx].macs       = layer_macs;
    g_layer_stats[d->layer_idx].wall_ms    = cycles_to_ms(t_end - t_start);
    g_layer_stats[d->layer_idx].dma_run_ms = cycles_to_ms(cyc_run);

    printf("[DW TIMING] H=%d W=%d Cin=%d Cout=%d\n", d->H, d->W, d->Cin, d->Cout);
    printf("[DW TIMING] total   : %.3f ms\n", cycles_to_ms(t_end - t_start));
    printf("[DW TIMING] params  : %.3f ms\n", cycles_to_ms(cyc_param));
    printf("[DW TIMING] weights : %.3f ms\n", cycles_to_ms(cyc_weight));
    printf("[DW TIMING] dma/run : %.3f ms\n", cycles_to_ms(cyc_run));

    return 0;
}

static int run_pw_layer_real(const layer_desc_t *d,
                             const uint8_t *in_tensor,
                             uint8_t *out_tensor,
                             const file_blob_t *params_blob,
                             const file_blob_t *weights_blob)
{
    return hw_pw_run_full_layer(in_tensor, out_tensor,
                                d->H, d->W,
                                d->Cin, d->Cout,
                                d->zp_in, d->zp_out,
                                d->relu_en,
                                params_blob, weights_blob,
                                d->params_addr, d->weight_addr,
                                d->layer_idx);
}

/* ============================================================
 * Validation
 * ============================================================ */
static int validate_layer_schedule(const layer_desc_t descs[NUM_LAYERS])
{
    int errors = 0;

    for (int i = 0; i < NUM_LAYERS; i++) {
        const layer_desc_t *d = &descs[i];
        const expected_layer_t *e = &EXPECTED[i];

        if (d->kind != e->kind) errors++;
        if (d->Cin != e->cin) errors++;
        if (d->kind == KIND_DW && d->Cout != d->Cin) errors++;
        if (d->kind == KIND_PW && e->cout > 0 && d->Cout != e->cout) errors++;
    }

    if (errors == 0) printf("[VALIDATE] schedule OK\n");
    else printf("[VALIDATE] schedule errors=%d\n", errors);

    return errors;
}

static void pad_input_tensor(const uint8_t *src_unpadded, uint8_t *dst_padded,
                             int C, int H, int W, uint8_t pad_val)
{
    int W_padded = (W + 7) & ~7;
    for (int c = 0; c < C; c++) {
        for (int h = 0; h < H; h++) {
            const uint8_t *src_row = src_unpadded + (size_t)c * H * W + (size_t)h * W;
            uint8_t *dst_row = dst_padded + (size_t)c * H * W_padded + (size_t)h * W_padded;
            memcpy(dst_row, src_row, W);
            memset(dst_row + W, pad_val, W_padded - W);
        }
    }
}

/* ============================================================
 * Main full-network run
 * ============================================================ */
int cnn_run_full_network(const runner_cfg_t *cfg)
{
    file_blob_t instr_blob = {0};
    file_blob_t params_blob = {0};
    file_blob_t weights_blob = {0};
    layer_desc_t descs[NUM_LAYERS];
    saved_tensor_t skips[NUM_SKIP_SLOTS];

    uint8_t *buf_cur  = (uint8_t *)DDR_BUF_CUR_ADDR;
    uint8_t *buf_next = (uint8_t *)DDR_BUF_NEXT_ADDR;
    uint8_t *buf_add  = (uint8_t *)DDR_BUF_ADD_ADDR;

    memset(descs, 0, sizeof(descs));
    memset(skips, 0, sizeof(skips));

    skips[0].buf = (uint8_t *)DDR_SKIP0_ADDR;
    skips[1].buf = (uint8_t *)DDR_SKIP1_ADDR;
    skips[2].buf = (uint8_t *)DDR_SKIP2_ADDR;
    skips[3].buf = (uint8_t *)DDR_SKIP3_ADDR;
    skips[4].buf = (uint8_t *)DDR_SKIP4_ADDR;
    skips[5].buf = (uint8_t *)DDR_SKIP5_ADDR;

    if (load_file_from_sd("0:/MEM/INSTR.BIN", &instr_blob) != 0) return -1;
    if (load_file_from_sd("0:/MEM/PARAMS.BIN", &params_blob) != 0) return -1;
    if (load_file_from_sd("0:/MEM/WEIGHTS.BIN", &weights_blob) != 0) return -1;

    for (int i = 0; i < NUM_LAYERS; i++) {
        if (parse_layer_desc_from_instr(&instr_blob, i, &descs[i]) != 0) {
            printf("[PARSE] fail L%02d\n", i);
            free_blob(&instr_blob);
            free_blob(&params_blob);
            free_blob(&weights_blob);
             
            return -1;
        }
        if (cfg && cfg->verbose) print_layer_desc(&descs[i]);
    }

    if (validate_layer_schedule(descs) != 0 && cfg->stop_on_fail) {
        free_blob(&instr_blob);
        free_blob(&params_blob);
        free_blob(&weights_blob);
         
        return -1;
    }

    size_t input_bytes = 0;
    if (load_file_into_buffer_sd("0:/INPUTS/INPUT_~1.BIN", buf_next, MAX_TENSOR_BYTES, &input_bytes) != 0) {
        if (load_file_into_buffer_sd("0:/INPUTS/INPUT_0.BIN", buf_next, MAX_TENSOR_BYTES, &input_bytes) != 0) {
            free_blob(&instr_blob);
            free_blob(&params_blob);
            free_blob(&weights_blob);
            return -1;
        }
    }

    pad_input_tensor(buf_next, buf_cur, descs[0].Cin, descs[0].H, descs[0].W, descs[0].zp_in);

    printf("[DEBUG L00] first 16 bytes of unpadded input (buf_next): ");
    for (int j = 0; j < 16; j++) {
        printf("%02X ", buf_next[j]);
    }
    printf("\n");

    printf("[DEBUG L00] first 16 bytes of padded input (buf_cur): ");
    for (int j = 0; j < 16; j++) {
        printf("%02X ", buf_cur[j]);
    }
    printf("\n");

    size_t padded_input_bytes = tensor_bytes_u8(descs[0].Cin, descs[0].H, descs[0].W);
    Xil_DCacheFlushRange((UINTPTR)buf_cur, padded_input_bytes);


    for (int i = 0; i < NUM_LAYERS; i++) {
        const layer_desc_t *d = &descs[i];
        tensor_shape_t outs = get_layer_output_shape(d);
        size_t out_bytes = tensor_bytes_u8(outs.C, outs.H, outs.W);
        int rc;

        if (out_bytes > MAX_TENSOR_BYTES) {
            printf("[RUN] L%02d output too large: %lu\n", i, (unsigned long)out_bytes);
            free_blob(&instr_blob);
            free_blob(&params_blob);
            free_blob(&weights_blob);
            
            return -1;
        }

        printf("\n[RUN] L%02d %s in=[%d,%d,%d] out=[%d,%d,%d]\n",
               i, d->kind == KIND_DW ? "DW" : "PW",
               d->Cin, d->H, d->W, outs.C, outs.H, outs.W);

        if (d->kind == KIND_DW) {
            rc = run_dw_layer_real(d, buf_cur, buf_next, &params_blob, &weights_blob);
        } else {
            rc = run_pw_layer_real(d, buf_cur, buf_next, &params_blob, &weights_blob);
        }

        if (rc != 0) {
            printf("[RUN] layer failed L%02d\n", i);
            free_blob(&instr_blob);
            free_blob(&params_blob);
            free_blob(&weights_blob);
             
            return -1;
        }

        if (cfg && cfg->compare_layers) {
            char ref_path[32];
            char label[16];
            snprintf(ref_path, sizeof(ref_path), "0:/REF/L%02d.BIN", i);
            snprintf(label, sizeof(label), "L%02d", i);

            if (compare_tensor_vs_ref(buf_next, outs.C, outs.H, outs.W, ref_path, label, cfg) > 0 &&
                cfg->stop_on_fail) {
                free_blob(&instr_blob);
                free_blob(&params_blob);
                free_blob(&weights_blob);

                return -1;
            }
        }

#if DW_DIAG_STOP_L0
        if (i == 0) {
            printf("[DIAG] DW_DIAG_STOP_L0 set -> halting after L0 + compare\n");
            free_blob(&instr_blob);
            free_blob(&params_blob);
            free_blob(&weights_blob);
            return 0;
        }
#endif

        {
            int slot = skip_slot_for_layer(i);
            if (slot >= 0) {
                memcpy(skips[slot].buf, buf_next, out_bytes);
                Xil_DCacheFlushRange((UINTPTR)skips[slot].buf, out_bytes);

                skips[slot].valid = 1;
                skips[slot].layer_idx = i;
                skips[slot].C = outs.C;
                skips[slot].H = outs.H;
                skips[slot].W = outs.W;
                skips[slot].num_bytes = out_bytes;

                printf("[SKIP] saved L%02d -> slot %d\n", i, slot);
            }
        }

        {
            uint8_t *tmp = buf_cur;
            buf_cur = buf_next;
            buf_next = tmp;
        }

        {
            const add_desc_t *ad = add_triggered_after(i);
            if (ad) {
                int slot = skip_slot_for_layer(ad->src_skip_layer);
                size_t n = tensor_numel(outs.C, outs.H, outs.W);

                if (slot < 0 || !skips[slot].valid) {
                    printf("[ADD%d] missing skip\n", ad->add_idx);
                    free_blob(&instr_blob);
                    free_blob(&params_blob);
                    free_blob(&weights_blob);
                     
                    return -1;
                }

                if (skips[slot].num_bytes != n) {
                    printf("[ADD%d] size mismatch\n", ad->add_idx);
                    free_blob(&instr_blob);
                    free_blob(&params_blob);
                    free_blob(&weights_blob);
                     
                    return -1;
                }

                printf("[ADD%d] L%02d + L%02d -> L%02d\n",
                       ad->add_idx, ad->src_skip_layer, ad->src_main_layer, ad->out_feeds_layer);

                run_residual_add_tensor(skips[slot].buf, buf_cur, buf_add, n, ad);
                Xil_DCacheFlushRange((UINTPTR)buf_add, n);

                if (cfg && cfg->compare_adds) {
                    char ref_path[32];
                    char label[16];
                    snprintf(ref_path, sizeof(ref_path), "0:/REF/ADD%d.BIN", ad->add_idx);
                    snprintf(label, sizeof(label), "ADD%d", ad->add_idx);

                    if (compare_tensor_vs_ref(buf_add, outs.C, outs.H, outs.W, ref_path, label, cfg) > 0 &&
                        cfg->stop_on_fail) {
                        free_blob(&instr_blob);
                        free_blob(&params_blob);
                        free_blob(&weights_blob);
                         
                        return -1;
                    }
                }

                {
                    uint8_t *tmp = buf_cur;
                    buf_cur = buf_add;
                    buf_add = tmp;
                }
            }
        }
    }

    printf("\n[RESULT] PASS end-to-end network run\n");

    printf("\n========================================\n");
    printf(" PEAK BENCHMARK SUMMARY (best single transfer/plane/tile seen)\n");
    printf("========================================\n");
    printf("[PEAK] DW DMA  MM2S (DDR->core): %.3f GB/s  (L%02d ch%d, %lu bytes in %.4f ms)\n",
           g_peak_dw_mm2s.gbps, g_peak_dw_mm2s.layer_idx, g_peak_dw_mm2s.idx,
           (unsigned long)g_peak_dw_mm2s.bytes, g_peak_dw_mm2s.ms);
    printf("[PEAK] DW DMA  S2MM (core->DDR): %.3f GB/s  (L%02d ch%d, %lu bytes in %.4f ms)\n",
           g_peak_dw_s2mm.gbps, g_peak_dw_s2mm.layer_idx, g_peak_dw_s2mm.idx,
           (unsigned long)g_peak_dw_s2mm.bytes, g_peak_dw_s2mm.ms);
    printf("[PEAK] DW compute throughput   : %.3f GMACs/s (L%02d, %lu MACs in %.4f ms)\n",
           g_peak_dw_thr.gmacs_per_s, g_peak_dw_thr.layer_idx,
           (unsigned long)g_peak_dw_thr.macs, g_peak_dw_thr.ms);
    printf("\n");
    printf("[PEAK] PW DMA  MM2S (DDR->core): %.3f GB/s  (L%02d tile%d, %lu bytes in %.4f ms)\n",
           g_peak_pw_mm2s.gbps, g_peak_pw_mm2s.layer_idx, g_peak_pw_mm2s.idx,
           (unsigned long)g_peak_pw_mm2s.bytes, g_peak_pw_mm2s.ms);
    printf("[PEAK] PW DMA  S2MM (core->DDR): %.3f GB/s  (L%02d tile%d, %lu bytes in %.4f ms)\n",
           g_peak_pw_s2mm.gbps, g_peak_pw_s2mm.layer_idx, g_peak_pw_s2mm.idx,
           (unsigned long)g_peak_pw_s2mm.bytes, g_peak_pw_s2mm.ms);
    printf("[PEAK] PW compute throughput   : %.3f GMACs/s (L%02d, %lu MACs in %.4f ms)\n",
           g_peak_pw_thr.gmacs_per_s, g_peak_pw_thr.layer_idx,
           (unsigned long)g_peak_pw_thr.macs, g_peak_pw_thr.ms);
    printf("========================================\n");

    {
        uint64_t total_bytes     = g_total_dw_bytes + g_total_pw_bytes;
        uint64_t total_macs      = g_total_dw_macs  + g_total_pw_macs;
        double   total_ms        = cycles_to_ms(g_total_layer_cyc);
        double   dma_run_ms      = cycles_to_ms(g_total_dma_run_cyc);
        double   gmacs_per_s     = (total_ms > 0.0)   ? (((double)total_macs / 1.0e9) / (total_ms / 1000.0))   : 0.0;
        double   gmacs_per_s_dma = (dma_run_ms > 0.0) ? (((double)total_macs / 1.0e9) / (dma_run_ms / 1000.0)) : 0.0;
        double   intensity       = (total_bytes > 0) ? ((double)total_macs / (double)total_bytes) : 0.0;

        printf("\n========================================\n");
        printf(" FULL ENCODER TOTALS (summed across all %d layers, excludes SD-card\n", NUM_LAYERS);
        printf(" reference-compare I/O — this is real deployed-encoder wall time)\n");
        printf("========================================\n");
        printf("[TOTAL] DW bytes moved (in+out): %.0f  (%.3f MB)\n",
               (double)g_total_dw_bytes, (double)g_total_dw_bytes / 1.0e6);
        printf("[TOTAL] PW bytes moved (in+out): %.0f  (%.3f MB)\n",
               (double)g_total_pw_bytes, (double)g_total_pw_bytes / 1.0e6);
        printf("[TOTAL] combined bytes moved   : %.0f  (%.3f MB)\n",
               (double)total_bytes, (double)total_bytes / 1.0e6);
        printf("[TOTAL] DW MACs                : %.0f  (%.4f GMAC)\n",
               (double)g_total_dw_macs, (double)g_total_dw_macs / 1.0e9);
        printf("[TOTAL] PW MACs                : %.0f  (%.4f GMAC)\n",
               (double)g_total_pw_macs, (double)g_total_pw_macs / 1.0e9);
        printf("[TOTAL] combined MACs          : %.0f  (%.4f GMAC)\n",
               (double)total_macs, (double)total_macs / 1.0e9);
        printf("[TOTAL] layer wall time (full) : %.3f ms  (incl. param/weight preload, pack, store)\n", total_ms);
        printf("[TOTAL] DMA/compute-only time  : %.3f ms  (DW cyc_run + PW cyc_dma only)\n", dma_run_ms);
        printf("[TOTAL] achieved thr., full    : %.3f GMACs/s  (x2 for GFLOPs/s if that's your op convention)\n",
               gmacs_per_s);
        printf("[TOTAL] achieved thr., DMA-only: %.3f GMACs/s  (x2 for GFLOPs/s — compare against 60.5 GOP/s here)\n",
               gmacs_per_s_dma);
        printf("[TOTAL] operational intensity  : %.4f MACs/byte  (x2 for FLOPs/byte)\n", intensity);
        printf("========================================\n");
    }

    printf("\n========================================\n");
    printf(" PER-LAYER ROOFLINE POINTS (one row per layer, CSV)\n");
    printf("========================================\n");
    printf("[LSTAT] idx,kind,Cin,Cout,H,W,bytes,macs,wall_ms,dma_run_ms,intensity_macs_per_byte,achieved_gmacs_per_s_full,achieved_gmacs_per_s_dma\n");
    for (int li = 0; li < NUM_LAYERS; li++) {
        const layer_stat_t *ls = &g_layer_stats[li];
        double intensity_l = (ls->bytes > 0) ? ((double)ls->macs / (double)ls->bytes) : 0.0;
        double gmacs_l     = (ls->wall_ms > 0.0)    ? (((double)ls->macs / 1.0e9) / (ls->wall_ms / 1000.0))    : 0.0;
        double gmacs_l_dma = (ls->dma_run_ms > 0.0) ? (((double)ls->macs / 1.0e9) / (ls->dma_run_ms / 1000.0)) : 0.0;

        printf("[LSTAT] %d,%s,%d,%d,%d,%d,%.0f,%.0f,%.4f,%.4f,%.4f,%.4f,%.4f\n",
               li, ls->kind == KIND_DW ? "DW" : "PW",
               ls->Cin, ls->Cout, ls->H, ls->W,
               (double)ls->bytes, (double)ls->macs, ls->wall_ms, ls->dma_run_ms,
               intensity_l, gmacs_l, gmacs_l_dma);
    }
    printf("========================================\n");

    free_blob(&instr_blob);
    free_blob(&params_blob);
    free_blob(&weights_blob);
     
    return 0;
}

/* ============================================================
 * main
 * ============================================================ */
int main(void)
{
    runner_cfg_t cfg;
    int rc;

    printf("========================================\n");
    printf(" Real DW/PW full-network runner\n");
    printf("========================================\n");

    if (init_dmas() != 0) {
        printf("ERROR: DMA init failed\n");
        return -1;
    }

    cfg.verbose = 1;
    cfg.compare_layers = 1;
    cfg.compare_adds = 0;
    cfg.stop_on_fail = 0;
    cfg.dump_failed_outputs = 0;

    rc = cnn_run_full_network(&cfg);
    printf("main() returning %d\n", rc);
    return rc;
}