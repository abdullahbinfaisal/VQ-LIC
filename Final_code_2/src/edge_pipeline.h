// ============================================================================
// edge_pipeline.h -- complete edge-encoder measurement harness
//                    DDR image -> PL analysis -> VQ -> range coding -> bytes
// ============================================================================
//
// PURPOSE
//   The manuscript's "complete edge encoder" must include VQ assignment AND
//   range coding, neither of which was previously timed (range coding did not
//   exist at all). This file owns the timing boundaries and does NOT modify
//   the existing surrogate timing path, so previously reported B1/B2 numbers
//   remain reproducible from the same binary.
//
// TIMING PRIMITIVES  (all in ARM Global Timer ticks; see main.c cycles_to_ms)
//
//   t_load    SD read of one frame.  REPORTED, NOT INCLUDED in edge latency:
//             a deployed encoder receives frames from a sensor/ISP, not FatFs.
//   t_deint   HWC->CHW de-interleave, only if the source is interleaved.
//             REPORTED SEPARATELY -- include it in edge latency only if the
//             deployed system really receives interleaved pixels.
//   t_pack    pack_input_group_major
//   t_prog    DW+PW coefficient/param programming, as actually performed
//   t_cache   cache maintenance
//   t_pl      sum of the six per-pair PL windows  (== manuscript B1)
//   t_gap     t_host - (t_pack+t_prog+t_cache+t_pl).  Inter-pair host
//             turnaround + DMA descriptor setup. Previously unattributed;
//             surfaced explicitly so it cannot hide inside B2.
//   t_host    full analysis transform, entry..last PL output  (== B2)
//   t_vq      nearest-codeword assignment, all 4 codebooks     (== B3)
//   t_range   rc_encode_frame only: model lookup + code + flush (== B4)
//   t_edge_direct  ONE timer around host+vq+range              (== B5)
//
//   t_edge_sum = t_host + t_vq + t_range  (calculated, for cross-check)
//
// WHAT IS DELIBERATELY EXCLUDED FROM t_range
//   rc_model_build. The frequency model is static and pre-shared; its
//   construction is offline work and is timed separately as t_model_build.
//
// ============================================================================
#ifndef EDGE_PIPELINE_H
#define EDGE_PIPELINE_H

#include <stdint.h>
#include <stddef.h>
#include "vq_pq.h"
#include "range_coder.h"

#define EP_W        1280
#define EP_H        720
#define EP_CH       3
#define EP_RAW_BYTES ((size_t)EP_W * EP_H * EP_CH)   // 2,764,800

// Source pixel layout on the SD card.
typedef enum {
    EP_SRC_UNKNOWN = 0,
    EP_SRC_PLANAR,        // R plane, G plane, B plane   (what the packer wants)
    EP_SRC_INTERLEAVED,   // R0G0B0 R1G1B1 ...           (needs de-interleave)
    EP_SRC_NOT_RAW        // magic bytes say container (PNG/JPEG) -> unusable
} ep_src_layout_t;

typedef struct {
    int    frame_id;
    double t_load, t_deint, t_pack, t_prog, t_cache, t_pl, t_gap;
    double t_host, t_vq, t_range, t_edge_sum, t_edge_direct;
    size_t range_bytes;
    double range_bits, range_bpp, fixed_bpp;
    double h_emp[RC_NMODEL];        // empirical entropy per codebook, bits/sym
    long   rc_mismatch;             // round-trip mismatches, -1 if not checked
    long   rc_first_bad;
} ep_frame_stat_t;

// Sniff one file: returns the layout, and *nbytes gets the file size.
// EP_SRC_NOT_RAW is returned for a PNG/JPEG magic, with a printed diagnostic.
ep_src_layout_t ep_probe_source(const char *path, size_t *nbytes);

// HWC -> CHW. src and dst must not overlap. Timed as t_deint.
void ep_deinterleave_rgb(const uint8_t *src_hwc, uint8_t *dst_chw);

// Deterministic synthetic planar frame; fallback when the dataset is not raw.
// See edge_pipeline.c for why this is acceptable for a timing/power run.
void ep_synth_frame_planar(uint8_t *dst_chw, int seed);

// Build the static frequency model from `ncal` calibration frames already
// present as index arrays. Returns build time in ms via *ms_out.
void ep_build_model(rc_models_t *M, const uint8_t *const *cal_idx,
                    int ncal, double *ms_out);

// Run one complete frame and fill `st`. `latent` must point at the
// accelerator's block-5 output for this frame.
// Returns 0 on success.
int ep_run_frame(int frame_id,
                 const vq_pq_ctx_t *vq,
                 const rc_models_t *M,
                 const uint8_t *latent,
                 uint8_t *idx_buf,      // VQ_IDX_BYTES
                 uint8_t *bs_buf,       // >= VQ_IDX_BYTES*2
                 size_t   bs_cap,
                 uint8_t *rt_buf,       // VQ_IDX_BYTES, round-trip scratch
                 int      do_roundtrip,
                 ep_frame_stat_t *st);

// CSV emitters. Printed to stdout OUTSIDE any timed region.
void ep_print_csv_header(void);
void ep_print_csv_row(const ep_frame_stat_t *st);
void ep_print_summary(const ep_frame_stat_t *v, int n);

#endif // EDGE_PIPELINE_H
