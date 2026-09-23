#ifndef CNN_RUNNER_H
#define CNN_RUNNER_H

/*
 * cnn_runner.h  –  Milestone 1: PS-side orchestration for DW/PW CNN accelerator
 *
 * Platform: ZC702, Zynq bare-metal, Vitis/Vivado 2020.2
 * FatFs for SD card access; printf for logging.
 *
 * In Milestone 2:
 *   - Replace run_dw_layer_debug() / run_pw_layer_debug() with real DMA +
 *     hardware-register calls (see cnn_runner.c, MILESTONE2 markers).
 *   - Keep all orchestration, buffer management, and compare logic unchanged.
 */

#include <stdint.h>
#include <stddef.h>

/* ------------------------------------------------------------------ */
/* Layer kind                                                           */
/* ------------------------------------------------------------------ */
typedef enum {
    KIND_DW = 0,   /* depthwise conv  */
    KIND_PW = 1    /* pointwise conv  */
} layer_kind_t;

/* ------------------------------------------------------------------ */
/* Per-layer descriptor decoded from instr.bin                         */
/* ------------------------------------------------------------------ */
typedef struct {
    int          layer_idx;
    layer_kind_t kind;

    /* spatial dims (input) */
    int H, W;
    int Cin, Cout;

    /* conv geometry */
    int stride, pad, kernel;

    /* quantisation */
    uint8_t zp_in, zp_out;

    /* file offsets into weights.bin / params.bin */
    uint32_t weight_addr;
    uint32_t params_addr;

    /* flags from word0 */
    int relu_en;
    int bypass_1x1;
} layer_desc_t;

/* ------------------------------------------------------------------ */
/* Residual add descriptor (hard-coded from spec)                      */
/* ------------------------------------------------------------------ */
typedef struct {
    int      add_idx;
    int      src_skip_layer;   /* the saved skip tensor layer         */
    int      src_main_layer;   /* the "current" layer feeding the add */
    int      out_feeds_layer;  /* the layer that receives add output  */

    /* quantisation – PPU-compatible */
    uint32_t mult_a;           /* multiplier for skip (branch A)      */
    uint32_t mult_b;           /* multiplier for main (branch B)      */
    int      shift;            /* arithmetic right-shift after sum    */
    uint8_t  zp_a;             /* input zp for branch A               */
    uint8_t  zp_b;             /* input zp for branch B               */
    uint8_t  zp_out;           /* output zero-point                   */
} add_desc_t;

/* ------------------------------------------------------------------ */
/* Saved skip tensor                                                    */
/* ------------------------------------------------------------------ */
typedef struct {
    int      valid;
    int      layer_idx;
    int      H, W, C;
    size_t   num_bytes;
    uint8_t *buf;
} saved_tensor_t;

/* ------------------------------------------------------------------ */
/* Generic file blob                                                    */
/* ------------------------------------------------------------------ */
typedef struct {
    uint8_t *data;
    size_t   size;
} file_blob_t;

/* ------------------------------------------------------------------ */
/* Debug / runner configuration                                         */
/* ------------------------------------------------------------------ */
typedef struct {
    int verbose;
    int compare_layers;
    int compare_adds;
    int stop_on_fail;
    int dump_failed_outputs;
} runner_cfg_t;

/* ------------------------------------------------------------------ */
/* Tensor shape helper                                                  */
/* ------------------------------------------------------------------ */
typedef struct { int C, H, W; } tensor_shape_t;

/* ------------------------------------------------------------------ */
/* Public API                                                           */
/* ------------------------------------------------------------------ */

/* SD / file helpers */
int  load_file_from_sd(const char *path, file_blob_t *out);
void free_blob(file_blob_t *b);

/* Instruction parser */
int  parse_layer_desc_from_instr(const file_blob_t *instr, int idx,
                                  layer_desc_t *out);
void print_layer_desc(const layer_desc_t *d);

/* Tensor geometry */
int            calc_out_dim(int in_dim, int pad, int kernel, int stride);
size_t         tensor_numel(int C, int H, int W);
size_t         tensor_bytes_u8(int C, int H, int W);
tensor_shape_t get_layer_output_shape(const layer_desc_t *d);

/* Residual add (real arithmetic, not stubbed) */
uint8_t residual_add_pixel(int a, int b,
                            uint32_t mult_a, uint32_t mult_b,
                            int shift,
                            int zp_a, int zp_b, int zp_out);
void    run_residual_add_tensor(const uint8_t *src_a, const uint8_t *src_b,
                                uint8_t *dst, size_t n_elem,
                                const add_desc_t *ad);

/* Stub layer runners – REPLACE in Milestone 2 */
int run_dw_layer_debug(const layer_desc_t *d,
                       const uint8_t *in_tensor,
                       uint8_t *out_tensor);
int run_pw_layer_debug(const layer_desc_t *d,
                       const uint8_t *in_tensor,
                       uint8_t *out_tensor);

/* Compare / dump */
int compare_tensor_vs_ref(const uint8_t *got, int C, int H, int W,
                          const char *ref_path, const char *label,
                          const runner_cfg_t *cfg);

/* Main orchestration */
int cnn_run_full_network(const runner_cfg_t *cfg);

#endif /* CNN_RUNNER_H */
