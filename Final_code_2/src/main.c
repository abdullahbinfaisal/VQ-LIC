#include "cnn_runner.h"

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "ff.h"
#include "xil_io.h"
#include "xil_cache.h"
#include "xil_mmu.h"      /* Xil_SetTlbAttributes / NORM_NONCACHE */
#include "xiicps.h"       /* PS I2C0 -> PCA9548 -> UCD9248 PMBus (power) */
#include "sleep.h"
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

/* dw_fused_axi_0 register map. As of the 2026-07-09 BD rewrite the legacy
 * DW_conv_accel_0 (DW_REG_* above) was deleted outright and dw_fused_axi_0
 * was auto-assigned the same freed S_AXI slot, so DWF_* reuses DW_BASE_ADDR
 * -- there is no separate DWF_BASE_ADDR. dw_fused_axi_0->dw_reorder_0 is
 * wired straight into axi_dma_0 (the same DMA the old DW_REG_* path used),
 * i.e. DwDma below. See hw_dw_run_plane()'s guard for why DW_REG_* is dead. */
#define DWF_REG_CTRL     0x00   /* W:  bit0=start (pulse); gates on !busy */
#define DWF_REG_STATUS   0x04   /* R:  bit0=done_sticky, bit1=busy */
#define DWF_REG_CIN_RUN  0x08   /* RW: C (channels) */
#define DWF_REG_N_GROUPS 0x0C   /* RW: G = ceil(W_padded/8) */
#define DWF_REG_ZP_RELU  0x10   /* RW: [7:0]=zp_in [15:8]=zp_out [16]=relu_en */
#define DWF_REG_CH_ADDR  0x14   /* RW: channel index for weight/param loads */
#define DWF_REG_W0       0x18   /* RW: {w3,w2,w1,w0} */
#define DWF_REG_W1       0x1C   /* RW: {w7,w6,w5,w4} */
#define DWF_REG_W2       0x20   /* W:  {w8} -- commits 9 weights to wram[CH_ADDR] */
#define DWF_REG_BIAS     0x24   /* RW: INT32 */
#define DWF_REG_MULT     0x28   /* RW: requant multiplier */
#define DWF_REG_SHIFT    0x2C   /* W:  [7:0] -- commits {bias,mult,shift} to pram[CH_ADDR] */
#define DWF_REG_IMG_WIDTH 0x30  /* RW: real, unpadded row width W (samples). Added
                                 * 2026-07-24 for dw_banked_window_8x's horizontal
                                 * edge masking -- DWF_REG_N_GROUPS (ceil(W/8)) alone
                                 * loses the remainder needed for exact per-lane
                                 * validity at the row tail/start. */
#define DWF_REG_N_ROWS    0x34  /* RW: H (real rows). Added 2026-07-24 -- V1
                                 * implicitly assumed H=1; the real network's DW
                                 * layers need genuine vertical windowing across H
                                 * rows. See DW_PW_FUSION_PLAN_V2.md. */
/* DEBUG read-only regs (2026-07-25), for the hardware no-output hang. */
#define DWF_REG_DBG_CONSUMED 0x38  /* R: input words the core pulled */
#define DWF_REG_DBG_WRITTEN  0x3C  /* R: 64b groups written into out FIFO (THE fork) */
#define DWF_REG_DBG_PRODUCED 0x40  /* R: 32b beats handed to S2MM */
#define DWF_REG_DBG_WINSTATE 0x44  /* R: {running,draining,r_cnt[5:0],g_cnt[11:0],c_cnt[11:0]} */
#define DWF_REG_DBG_FLAGS    0x48  /* R: {..,in_empty,out_prog_full,out_full_ever,all_written,core_done_seen} */
#define DWF_REG_DBG_WINCFG   0x4C  /* R: windower's OWN latched {H_r[7:0],G_r[11:0],C_r[11:0]} */

/* MUST match CONFIG.N_OC on pw_single_oc_axis_axi_0 in the BD. It drives
 * cout_rounded, the weight-bank select (oc % N_OC) and the weight BRAM offset
 * ((oc / N_OC) * Cin) -- a mismatch silently programs weights into the wrong
 * banks and computes the wrong number of output channels.
 * Changed 30 -> 8 on 2026-07-28 with the N_OC=8 bitstream: 8 divides the
 * surrogate's Couts (8/16/64) exactly, so cout_rounded == Cout at every stage,
 * removing both the padding waste and the inter-pair channel repack.
 * The old network (Couts 30/60/90/120/180) rounds up badly at 8 -- accepted,
 * it is debug-only now. */
/* 2026-08-08: 16 -> 32 for the N_OC=32 build. MUST equal CONFIG.N_OC on
 * pw_single_oc_axis_axi_0 in the BD -- it drives cout_rounded, the bank select
 * (oc % N_OC) and the weight BRAM offset ((oc / N_OC) * Cin). A mismatch
 * misprograms weights SILENTLY, with no error and plausible-looking timing.
 * Revert to 16 together with the bitstream, never separately. */
#define PW_N_OC             32
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
/* Experiments ??? set ONE at a time; 0 = off. Keep DW_DIAG_IMPULSE=1 to read the
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

/* DW_LEGACY_HW_ENABLED: hw_dw_run_plane() targets DW_REG_* at DW_BASE_ADDR,
 * which used to be DW_conv_accel_0. That block was deleted from hw.bd on
 * 2026-07-09 and dw_fused_axi_0 (a different register layout, see DWF_REG_*)
 * now occupies the same address. Until the fused pipeline replaces this
 * call path for real, hw_dw_run_plane() refuses to run unless this is set to
 * 1 (only valid if the BD has been reverted to the legacy DW core). */
#define DW_LEGACY_HW_ENABLED 0

/* DWF_L0_LOOPBACK_TEST: 1 = run hw_dw_fused_l0_loopback_test() once, right
 * after L0's descriptor/input are parsed, exercising the new
 * dw_fused_axi_0 -> dw_reorder_0 -> DRAM loop (no PW) and comparing the
 * result against the golden 0:/REF/L00.BIN reference. This is Stage 1 of
 * the staged validation plan in DW_PW_FUSION_PLAN.md: it validates the
 * line_buffer_8x row-parity fix is moot here (dw_seq_window_8x replaces it)
 * and confirms dw_reorder_0's output byte order matches the group-major
 * layout compare_group_major_vs_ref expects. Independent of
 * DW_LEGACY_HW_ENABLED / DW_DIAG_STOP_L0. 0 = normal operation. */
#define DWF_L0_LOOPBACK_TEST 0

/* DWF_CASCADE_TEST: 1 = run hw_dw_pw_cascade_l0_l1() once at boot, exercising
 * the on-chip DW(L00)->PW(L01) cascade wired 2026-07-27 (BD script F1). Requires
 * the CASCADE bitstream: axi_dma_0 MM2S -> dw_fused_axi_0 -> pw_single_oc_axis_axi_0
 * -> axi_dma_1 S2MM, with axi_dma_0's S2MM and axi_dma_1's MM2S channels REMOVED.
 * The DW->DRAM loopback test does NOT work in that bitstream (no dma0 S2MM) --
 * set DWF_L0_LOOPBACK_TEST to 0 when this is 1. Use BD script F2 to go back.
 * NOTE: DW still has the one-word output offset (deferred 2026-07-27), so PW
 * receives channel-misaligned input -- this validates STRUCTURE, not values. */
#define DWF_CASCADE_TEST 0

/* CASCADE_H_OVERRIDE: non-zero = run the cascade with this many rows instead of
 * the real H=2048. Added 2026-07-27 after the first full-scale run stalled: DW
 * completed (STATUS=done) so PW had consumed all 1,105,920 input words, but PW
 * stayed busy and the first 32MB S2MM chunk never filled. At H=64 the whole run
 * is 276,480 B in / 2,764,800 B out -- one S2MM chunk, ~3.5 ms of PW work --
 * so it either completes or stalls in milliseconds. Data is meaningless at a
 * truncated H (the DW vertical window is wrong at the seam); this is purely a
 * does-the-pipeline-drain test. 0 = real H. */
/* 2026-07-27: the out_prog_full fix did NOT change the stall -- bit-identical,
 * frozen at 65,280 output beats = 0xFF00 = exactly 2,176 pixel groups of 30,
 * at both H=64 and (consistent with) H=2048. So the mechanism is not the output
 * FIFO. Sweep H to test whether 65,280 is an ABSOLUTE ceiling:
 *     H=8  -> 43,200 total beats  (BELOW the ceiling -> should COMPLETE)
 *     H=16 -> 86,400 total beats  (ABOVE -> should stall at 65,280 again)
 * Completing at 8 and stalling at 16 pins it as a fixed limit rather than
 * anything data- or rate-dependent. */
/* 2026-07-28: H=16 PASSES with the DW prog_full fix in the fabric
 * (consumed=written=produced=8640, 691,200 B landed, TLAST clean).
 * 0 = real H=2048: 8,847,360 B in / 88,473,600 B out, and the first run that
 * exercises the 32MB S2MM chunking. If it fails, drop back to 64 to separate
 * "scale" from "chunking" -- 64 is the case that used to stall at 19%. */
#define CASCADE_H_OVERRIDE 0

/* Sentinel byte pre-written into the PW output buffer so a stall can be
 * measured ("how many bytes actually landed") instead of guessed. */
#define CASCADE_SENTINEL 0x5A

/* CASCADE_INSTRUMENT: 1 = sentinel-fill the output buffer, sample the drain
 * frontier for stall detection, and scan the whole buffer afterwards to report
 * how many bytes landed. That machinery is what root-caused the PW hang, but it
 * is pure HOST-side cost -- a memset + cache flush + a byte-by-byte scan of the
 * output on a 666MHz A9. Measured 2026-07-28 across the three surrogate pairs:
 * ~385 ms of instrumentation around ~32 ms of actual datapath time.
 * 0 = measurement mode: plain timeouts, no fill, no scan. Turn back on to debug
 * a stall (it is the only thing that reports WHERE the output stopped). */
#define CASCADE_INSTRUMENT 0

/* Blocking UART printf is expensive and several of the cascade driver's status
 * lines sit INSIDE the timed brackets -- three of them inside the HW bracket
 * itself -- so every figure measured with them enabled is inflated by console
 * time. g_cascade_quiet suppresses them for the steady-state measurement.
 * Error paths keep plain printf so a failure is never silent. */
static int g_cascade_quiet = 0;
#define CPRINT(...) do { if (!g_cascade_quiet) printf(__VA_ARGS__); } while (0)

/* ============================================================
 * ZC702 BOARD POWER MEASUREMENT (2026-07-30)
 *
 * Reads the three onboard UCD9248 PMBus controllers via PS I2C0 -> PCA9548
 * mux (0x74, channel 7). I2C0 is already enabled on MIO 50..51 in the BD, so
 * this needs NO bitstream change.
 *
 * WHY THIS EXISTS: Vivado's report_power gives 2.187 W total with **Medium**
 * confidence -- vectorless, <25% of internal nodes specified -- and 1.573 W of
 * that (72%) is a generic PS7 model that knows nothing about the real CPU
 * workload. This measures the rails directly and settles it.
 *
 * WHAT IT MEASURES: regulator OUTPUT power. It EXCLUDES regulator conversion
 * losses and the unmonitored 5 V USB rail, so it is NOT 12 V connector input
 * power. Say so in any write-up.
 *
 * Set RUN_POWER_MEASUREMENT 0 for a normal timing run -- the A/B adds ~70 s
 * and the I2C traffic must never land inside a timed bracket.
 * ============================================================ */
#define RUN_POWER_MEASUREMENT   0

#define PM_MEASURE_SECONDS      30U   /* averaging window per phase */
#define PM_SETTLE_SECONDS       3U    /* discarded after a state change */

#define PM_MUX_ADDR   0x74U
#define PM_MUX_CH7    0x80U
#define PM_UCD0       0x34U
#define PM_UCD1       0x35U
#define PM_UCD2       0x36U
#define PM_PAGE       0x00U
#define PM_VOUT_MODE  0x20U
#define PM_READ_VOUT  0x8BU
#define PM_READ_IOUT  0x8CU
#define PM_I2C_HZ     100000U

#if defined(XPAR_XIICPS_0_DEVICE_ID)
#  define PM_IIC_ARG  XPAR_XIICPS_0_DEVICE_ID
#elif defined(XPAR_XIICPS_0_BASEADDR)
#  define PM_IIC_ARG  XPAR_XIICPS_0_BASEADDR
#else
#  error "PS I2C0 not found in xparameters.h -- enable I2C0 in the PS."
#endif

/* Rail grouping. The single "is this PL" flag in the reference monitor is not
 * enough here: this workload is PS-heavy (pack + weight programming on the A9)
 * and the Vivado estimate's biggest term is PS7, so PS and DDR must be
 * separable from PL to check it. */
typedef enum { PM_PL = 0, PM_PS, PM_DDR, PM_MISC, PM_NGROUP } pm_group_t;
static const char *pm_group_name[PM_NGROUP] = { "PL", "PS", "DDR", "MISC" };

typedef struct {
    const char *name;
    uint8_t     addr;
    uint8_t     page;
    pm_group_t  group;
} pm_rail_t;

static const pm_rail_t pm_rails[] = {
    { "VCCINT",   PM_UCD0, 0, PM_PL   },  /* PL core logic  */
    { "VCCPINT",  PM_UCD0, 1, PM_PS   },  /* PS core        */
    { "VCCAUX",   PM_UCD0, 2, PM_PL   },
    { "VCCPAUX",  PM_UCD0, 3, PM_PS   },
    { "VCCADJ",   PM_UCD1, 0, PM_MISC },
    { "VCC1V5PS", PM_UCD1, 1, PM_DDR  },  /* DDR3 + PS 1.5 V */
    { "VCC_MIO",  PM_UCD1, 2, PM_PS   },
    { "VCCBRAM",  PM_UCD1, 3, PM_PL   },
    { "VCC3V3",   PM_UCD2, 0, PM_MISC },
    { "VCC2V5",   PM_UCD2, 1, PM_MISC },
};
#define PM_NRAILS (sizeof(pm_rails) / sizeof(pm_rails[0]))

typedef struct {
    float v, i, p;
    float i_lsb;   /* current quantisation step implied by the Linear11 exponent */
} pm_sample_t;

/* Interleaved A/B. Sequential idle-then-active CANNOT separate the signal from
 * thermal drift: the first attempt (2026-07-30) returned NEGATIVE deltas on
 * both PL and PS -- physically impossible for an accelerator doing work -- with
 * every delta under 20 mW and random in sign. Alternating the phases makes any
 * monotonic drift common-mode so it cancels to first order. */
/* Set to 0 once the diagnostics have been interpreted -- they cost ~60 s. */
#define PM_RUN_DIAGNOSTICS  0

/* ENERGY-PER-FRAME MODE. Skips the idle/active A/B entirely and just runs
 * frames back to back, averaging total board power across the run.
 *
 * This is the number that survives the 2026-08-05 finding. The UCD9248
 * emits current in 1/64 A steps (15.625 mA), so a DIFFERENCE of two
 * ~2 W readings -- which is what incremental accelerator power is -- sits
 * under the quantum and cannot be recovered. The MEAN of a single 2 W
 * reading is unaffected: the dither is zero-mean, so ~400 samples pin it
 * to a few mW.
 *
 * Duty cycle barely matters here, and that follows from the same fact:
 * idle and active differ by under 20 mW, so the ~12 ms scans interleaved
 * between frames cost almost nothing in average power. Frame time is
 * taken from the frames actually executed inside this loop. */
#define PM_ENERGY_ONLY      1
#define PM_ENERGY_SECONDS   60U

/* Sizing, 2026-08-05. The 8x5 s run gave SE(total) = 27.7 mW against a
 * 13 mW delta. The per-sample scatter is white (see the diagnostics), so
 * SE falls as sqrt(n): reaching a ~7 mW SE needs ~16x the samples. Both
 * levers are used -- a shorter scan interval and more cycles -- giving
 * ~43 samples per phase per cycle x 16 cycles = ~690 per phase, about
 * 14x the previous 48. Cost is ~4.5 min of wall time.
 *
 * The scan interval cannot go much below ~120 ms: a 10-rail scan is
 * ~40 I2C transactions at 100 kHz, roughly 12 ms, and that time is
 * stolen from the active phase's duty cycle. At 150 ms the duty cost is
 * ~8%, which the reported duty figure makes visible. */
#define PM_AB_CYCLES      16U   /* interleaved idle/active repetitions */
#define PM_PHASE_SECONDS  8U    /* each phase within one cycle        */
#define PM_PHASE_SKIP_MS  1500  /* discarded at the start of a phase  */
#define PM_SCAN_INTERVAL_MS 150 /* between full 10-rail scans          */

static float pm_sqrtf(float x)
{
    float r;
    if (x <= 0.0f) return 0.0f;
    r = (x > 1.0f) ? x : 1.0f;
    for (int i = 0; i < 30; i++) r = 0.5f * (r + x / r);
    return r;
}

static XIicPs pm_iic;

static void pm_wait(void) { while (XIicPs_BusIsBusy(&pm_iic)) { } }

static int pm_init(void)
{
    XIicPs_Config *cfg = XIicPs_LookupConfig(PM_IIC_ARG);
    uint8_t ch = PM_MUX_CH7;
    int s;
    if (!cfg) return XST_FAILURE;
    s = XIicPs_CfgInitialize(&pm_iic, cfg, cfg->BaseAddress);
    if (s != XST_SUCCESS) return s;
    s = XIicPs_SetSClk(&pm_iic, PM_I2C_HZ);
    if (s != XST_SUCCESS) return s;
    pm_wait();
    s = XIicPs_MasterSendPolled(&pm_iic, &ch, 1, PM_MUX_ADDR);   /* select ch7 */
    pm_wait();
    return s;
}

static int pm_wr_byte(uint8_t slave, uint8_t cmd, uint8_t val)
{
    uint8_t tx[2] = { cmd, val };
    int s;
    pm_wait();
    s = XIicPs_MasterSendPolled(&pm_iic, tx, 2, slave);
    pm_wait();
    return s;
}

/* START, slave+W, cmd, REPEATED START, slave+R, data, STOP */
static int pm_rd(uint8_t slave, uint8_t cmd, uint8_t *buf, int len)
{
    int s;
    pm_wait();
    XIicPs_SetOptions(&pm_iic, XIICPS_REP_START_OPTION);
    s = XIicPs_MasterSendPolled(&pm_iic, &cmd, 1, slave);
    XIicPs_ClearOptions(&pm_iic, XIICPS_REP_START_OPTION);
    if (s != XST_SUCCESS) return s;
    s = XIicPs_MasterRecvPolled(&pm_iic, buf, len, slave);
    pm_wait();
    return s;
}

static int16_t pm_sext(uint16_t v, unsigned bits)
{
    uint16_t sb = (uint16_t)(1U << (bits - 1U));
    uint16_t mk = (uint16_t)((1U << bits) - 1U);
    v &= mk;
    return (int16_t)((v ^ sb) - sb);
}

static float pm_pow2(float v, int e)
{
    if (e >= 0) { while (e-- > 0) v *= 2.0f; }
    else        { while (e++ < 0) v *= 0.5f; }
    return v;
}

static int pm_read_rail(const pm_rail_t *r, pm_sample_t *o)
{
    uint8_t mode, rx[2];
    uint16_t raw;
    int s;

    if ((s = pm_wr_byte(r->addr, PM_PAGE, r->page)) != XST_SUCCESS) return s;
    if ((s = pm_rd(r->addr, PM_VOUT_MODE, &mode, 1)) != XST_SUCCESS) return s;
    if ((mode & 0xE0U) != 0x00U) return XST_FAILURE;   /* not Linear mode */

    if ((s = pm_rd(r->addr, PM_READ_VOUT, rx, 2)) != XST_SUCCESS) return s;
    raw = (uint16_t)(((uint16_t)rx[1] << 8) | rx[0]);          /* little endian */
    o->v = pm_pow2((float)raw, pm_sext(mode & 0x1FU, 5));      /* Linear16 */

    if ((s = pm_rd(r->addr, PM_READ_IOUT, rx, 2)) != XST_SUCCESS) return s;
    raw = (uint16_t)(((uint16_t)rx[1] << 8) | rx[0]);
    {
        int e = (int)pm_sext((uint16_t)((raw >> 11) & 0x1FU), 5);
        o->i     = pm_pow2((float)pm_sext(raw & 0x07FFU, 11), e); /* Linear11 */
        /* One LSB of the mantissa = 2^e amps. If the incremental current we are
         * chasing is smaller than this, no amount of averaging recovers it --
         * a perfectly stable reading sits on one code and averages to itself. */
        o->i_lsb = pm_pow2(1.0f, e);
    }

    o->p = o->v * o->i;
    return XST_SUCCESS;
}

static int pm_scan(pm_sample_t *out)
{
    for (unsigned r = 0; r < PM_NRAILS; r++)
        if (pm_read_rail(&pm_rails[r], &out[r]) != XST_SUCCESS) return XST_FAILURE;
    return XST_SUCCESS;
}

static float pm_group_w(const pm_sample_t *s, pm_group_t g)
{
    float t = 0.0f;
    for (unsigned r = 0; r < PM_NRAILS; r++) if (pm_rails[r].group == g) t += s[r].p;
    return t;
}

static float pm_total_w(const pm_sample_t *s)
{
    float t = 0.0f;
    for (unsigned r = 0; r < PM_NRAILS; r++) t += s[r].p;
    return t;
}

/* Frames averaged for the steady-state figure.
 * 2026-07-30: 5 -> 100 for a publication-grade distribution. Individual frames
 * are no longer printed (100 lines of blocking UART would be noise); the
 * reported statistics are mean, std dev, median, min, max, P95 and P99.
 * SURR_WARMUP frames run first and are DISCARDED -- the first frame after a
 * cold start is ~2.6x slower (first-touch DRAM on the weight blobs), and one
 * discard was previously enough only because 5 frames hid the tail. */
#define SURR_FRAMES 100
#define SURR_WARMUP 3

static double g_stat_median, g_stat_p95, g_stat_p99;

/* SURR_CHAIN_PAIRS (2026-07-30)
 *   1 = REAL pipeline. Pair p's PW output feeds pair p+1's DW directly, so
 *       pairs 1..n-1 need NO pack and NO cache flush -- the CPU never touches
 *       those bytes. This is what a deployed network does.
 *   0 = legacy harness. Every pair re-packs the same raw_in into a shared
 *       scratch buffer, i.e. three independent runs. Measures ~14.75 ms/frame
 *       of packing and flushing that no real pipeline performs.
 * Kept switchable so the two can be compared rather than the improvement
 * being silently absorbed.
 *
 * NOTE: chaining propagates DATA between stages, so pair 0's output errors
 * now reach pair 1. The DW one-word offset and the left/right-8-column edge
 * bugs are still open, so chained OUTPUT VALUES are expected to be wrong.
 * The timing is what this measures; correctness needs those bugs fixed. */
#define SURR_CHAIN_PAIRS 1

/* SURR_GM_IN_NONCACHED (2026-07-30) ??? experiment, MEASURE BOTH SIDES.
 *   1 = map the gm_in window normal-non-cacheable (NORM_NONCACHE, 0x11DE2) so
 *       pair 0's Xil_DCacheFlushRange disappears entirely. That flush is
 *       8.88 ms/frame (28.7%) and costs ~3.2 ms/MB because it does L1 AND
 *       L2/PL310 maintenance -- ~103 ns per 32-byte line, controller overhead
 *       rather than bandwidth.
 *   0 = cached gm_in + explicit flush (previous behaviour).
 *
 * THE TRADE IS NOT OBVIOUS. pack then writes straight through the write buffer
 * instead of into L1/L2, so `pack` may get slower while `cache` goes to zero.
 * Sequential uncached writes coalesce reasonably on the A9, but this has to be
 * measured, not assumed -- compare pack AND cache, not just the frame total.
 *
 * Xil_SetTlbAttributes works on 1 MB sections and does a full Xil_DCacheFlush()
 * internally, so dirty lines are written back before the attribute changes.
 * One-time setup cost, not per frame. */
#define SURR_GM_IN_NONCACHED 1

/* Flags for hw_dw_pw_cascade_l0_l1()'s in_flags. */
#define CASC_IN_PREPACKED  (1u << 0)  /* gm_in already group-major (chained): no pack */
#define CASC_IN_NONCACHED  (1u << 1)  /* gm_in is non-cacheable: no flush needed */

/* Only pair 0 (not chained) writes gm_in, which is the window we optionally map
 * non-cacheable. Chained pairs read chainA/chainB: those stay CACHED but are
 * never flushed anyway, because the CPU never writes them. */
#if SURR_GM_IN_NONCACHED
#  define SURR_IN_FLAGS(chained) ((chained) ? CASC_IN_PREPACKED : CASC_IN_NONCACHED)
#else
#  define SURR_IN_FLAGS(chained) ((chained) ? CASC_IN_PREPACKED : 0u)
#endif

/* Phase accumulators, summed by the cascade driver when enabled. Needed because
 * the COLD-START phase numbers do not transfer to steady state: cold-start
 * phases sum to 73.5 ms (HW alone 46.1) against a measured 34.25 ms warm, so at
 * least one is badly inflated when cold -- almost certainly by the wrapper's
 * input memset leaving ~1.4 MB dirty in L2, whose writebacks contend with the
 * DMA. Picking the next optimisation off those numbers would repeat the mistake
 * that sent us into the cascade: optimising a term that wasn't the big one. */
static int g_cascade_accum = 0;
/* accumulators themselves are declared just after the u64_cycles typedef */

/* DWF_L0_TEST_H_OVERRIDE: diagnostic only (2026-07-25), chasing an MM2S
 * timeout that persists on real hardware at real L0 scale (H=2048) despite
 * passing standalone xsim (H=4) and clean routed timing closure. AXI-Lite
 * control-layer readback ruled out a stuck busy/failed start_pulse.
 * Leading suspect now: routed timing only proves setup/hold met, not that
 * the synthesized BRAM primitives' actual same-address read/write
 * collision behavior matches the RTL simulator's model -- at this scale
 * dw_banked_window_8x's line0/line1/pend_ram need 67 RAMB36 tiles
 * (cascaded), never exercised by anything except tiny-scale behavioral
 * xsim. Set to a small nonzero value (e.g. 1) to run the Stage-1 test
 * against real C=3/W=1435 but an artificially small H, isolating whether
 * the hang is scale/duration-dependent or present even at H=1. 0 = use
 * the real L0 descriptor unmodified. */
#define DWF_L0_TEST_H_OVERRIDE 0

/* RUN_SURROGATE_ESTIMATE: 1 = run cnn_run_dw_pw_surrogate() instead of the
 * real 42-layer SD-card network. That estimates real HW run time for an
 * unrelated dense-conv encoder (Conv2d 3->8->16->64, all dense 3x3, not
 * depthwise-separable) by mapping each dense conv onto a DW(Cin)+PW(Cin->Cout)
 * surrogate pair -- the same decomposition MobileNet uses -- so it can run on
 * the real DW/PW accelerators with synthetic dummy weights (no SD card files
 * needed). Numerically meaningless; only the measured cycle counts matter.
 * 0 = normal behavior, untouched. */
#define RUN_SURROGATE_ESTIMATE 1

/* RUN_EDGE_VALIDATION: 1 = run the COMPLETE edge encoder validation
 * (PL analysis -> VQ -> range coding) instead of the surrogate timing loop.
 * Takes precedence over RUN_SURROGATE_ESTIMATE. The surrogate path itself is
 * unmodified, so setting this to 0 reproduces the legacy B1/B2 numbers. */
#define RUN_EDGE_VALIDATION 1


typedef unsigned long long u64_cycles;

/* Warm-frame phase accumulators (see g_cascade_accum above). Declared here
 * rather than beside that flag because they need u64_cycles. */
static u64_cycles g_acc_pack, g_acc_prog, g_acc_cache, g_acc_hw, g_acc_total;

/* PER-PAIR warm accumulation (2026-07-30). The aggregate above cannot tell us
 * whether the ~25.6 cyc/group overhead is a per-GROUP constant or something
 * that varies by layer -- that was one constant fitted to one total. The cold
 * pass has per-pair numbers but they are useless (blocking UART inside the
 * brackets, 2.6x inflated). So accumulate per pair during the SILENT warm
 * frames. g_acc_pair is set by the caller before each cascade call. */
/* CASCADE_ENGINE_SPLIT (2026-07-30) ??? measure DW/PW overlap.
 *   1 = poll both cores' STATUS during the HW wait loops and record the first
 *       instant each reports done. Gives, per block:
 *         DW busy   = t_dw_done - t_hw0
 *         PW busy   = t_pw_done - t_hw0
 *         overlap   = DW busy          (PW is live the whole time DW is)
 *         PW tail   = PW busy - DW busy
 *       PW can never finish before DW -- it needs DW's last beat -- so
 *       t_pw_done >= t_dw_done always and the split is unambiguous.
 *   0 = clean timing, no extra bus traffic.
 *
 * ?????? THIS PERTURBS THE MEASUREMENT. Each poll is 2 AXI-Lite reads (~150 ns
 * each) inside the timed HW bracket. Polling every spin would add MORE time
 * than the datapath takes, so it is throttled to 1 poll per ENGINE_POLL_MASK+1
 * spins (~10 us). Even so, HW totals from a split run run slightly long --
 * compare against a CASCADE_ENGINE_SPLIT=0 run and quote the clean one.
 * The headline 36.261 ms / 27.6 fps figures were taken with this at 0. */
#define CASCADE_ENGINE_SPLIT 0   /* 0 for the clean pair-timing run: =1 adds AXI-Lite status polling inside the timed HW interval */
#define ENGINE_POLL_MASK     0x3F      /* poll every 64 spins */

#define SURR_MAX_PAIRS 8
static int        g_acc_pair = 0;

#if CASCADE_ENGINE_SPLIT
static int        g_dw_done_seen, g_pw_done_seen;
static u64_cycles g_dw_done_cyc,  g_pw_done_cyc;
static u64_cycles g_acc_dwbusy_p[SURR_MAX_PAIRS];
static u64_cycles g_acc_pwbusy_p[SURR_MAX_PAIRS];
/* STATUS bit0 is done_sticky on both cores and is cleared by the start pulse
 * (verified: both read 0x2 = busy immediately after start), so a sampled poll
 * cannot miss the transition. */
#define ENGINE_POLL()                                                          \
    do {                                                                       \
        if (!g_dw_done_seen && (dw_read_reg(DWF_REG_STATUS) & 1u)) {           \
            g_dw_done_seen = 1; g_dw_done_cyc = timer_now();                   \
        }                                                                      \
        if (!g_pw_done_seen && (pw_read_reg(PW_REG_STATUS) & 1u)) {            \
            g_pw_done_seen = 1; g_pw_done_cyc = timer_now();                   \
        }                                                                      \
    } while (0)
#else
#define ENGINE_POLL() do { } while (0)
#endif

/* FUSION-TRAFFIC CAPTURE (2026-07-30). The DW debug counters are per-run (they
 * reset on start_in), so after the timed loop they only hold the LAST pair's
 * values. Capture them in a dedicated UNTIMED pass instead: reading 3 AXI-Lite
 * registers per pair inside the timed loop would land inside f_cyc and perturb
 * the very distribution we are trying to measure. */
static int      g_capture_beats = 0;
static uint32_t g_dw_consumed_p[SURR_MAX_PAIRS];
static uint32_t g_dw_written_p [SURR_MAX_PAIRS];
static uint32_t g_dw_produced_p[SURR_MAX_PAIRS];
static u64_cycles g_acc_hw_p[SURR_MAX_PAIRS];
static u64_cycles g_acc_pack_p[SURR_MAX_PAIRS];
static u64_cycles g_acc_prog_p[SURR_MAX_PAIRS];
static u64_cycles g_acc_cache_p[SURR_MAX_PAIRS];

/* Zynq-7000 Cortex-A9 global timer runs at CPU clock / 2.
 * Set this to your actual CPU frequency if known.
 * Example: CPU 666.666 MHz => timer 333.333 MHz
 */
#define CPU_FREQ_HZ         666666687.0
#define GTIMER_FREQ_HZ      (CPU_FREQ_HZ / 2.0)

/* This BSP's XilTimer driver (SDT flow) only exports XTime_GetTime(), not
 * XTime_SetTime() -- the classic standalone BSP's setter that flips the
 * Global Timer's enable bit. Without it the counter stays at whatever a
 * JTAG/debugger launch leaves it in (often stopped), so every timer_now()
 * delta reads 0. Same register layout either way (ARM Cortex-A9 Global
 * Timer, unchanged by the driver split), so poke it directly instead of
 * depending on a symbol this BSP doesn't link. */
#define GLOBAL_TMR_BASEADDR         0xF8F00200u
#define GTIMER_COUNTER_LOWER_OFFSET 0x00u
#define GTIMER_COUNTER_UPPER_OFFSET 0x04u
#define GTIMER_CONTROL_OFFSET       0x08u

static inline void start_global_timer(void)
{
    Xil_Out32(GLOBAL_TMR_BASEADDR + GTIMER_CONTROL_OFFSET, 0x0u);
    Xil_Out32(GLOBAL_TMR_BASEADDR + GTIMER_COUNTER_LOWER_OFFSET, 0x0u);
    Xil_Out32(GLOBAL_TMR_BASEADDR + GTIMER_COUNTER_UPPER_OFFSET, 0x0u);
    Xil_Out32(GLOBAL_TMR_BASEADDR + GTIMER_CONTROL_OFFSET, 0x1u);
}

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
 * anywhere in the full 42-layer run, not a whole-run average ??? layer sizes
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
 * roofline point ??? distinct from the peak trackers above, which only keep
 * the single best transfer. Bytes are in+out combined (both DMA directions);
 * MACs use the same per-op conventions as note_thr (9 MACs/pixel for DW's
 * 3x3 kernel, pixels*Cin*cout_rounded for PW). g_total_layer_cyc sums each
 * layer's own [DW/PW TIMING] "total" window, which already excludes the
 * SD-card reference-compare overhead in cnn_run_full_network's outer loop ???
 * so it reflects real deployed-encoder wall time, not benchmark I/O. */
static uint64_t   g_total_dw_bytes  = 0;
static uint64_t   g_total_dw_macs   = 0;
static uint64_t   g_total_pw_bytes  = 0;
static uint64_t   g_total_pw_macs   = 0;
static u64_cycles g_total_layer_cyc   = 0;
/* Sum of DW's cyc_run + PW's cyc_dma across all layers ??? the DMA/compute-only
 * time basis, excluding param/weight preload, pack, and store overhead. */
static u64_cycles g_total_dma_run_cyc = 0;

/* Per-layer (ops, bytes, wall_time) triples for a full roofline scatter ???
 * one point per layer instead of just the single combined "full encoder"
 * point above. wall_ms is each layer's own [DW/PW TIMING] "total" window
 * (same real-deployment-time basis as g_total_layer_cyc, per-layer instead
 * of summed) ??? includes param/weight BRAM preload, pack, and store.
 * dma_run_ms is the narrower DMA+compute-only window (DW's cyc_run / PW's
 * cyc_dma), excluding all of that software-side overhead ??? this is the
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
/* PROBE support (2026-07-27): snapshot of one raw input row, taken before the
 * DMA overwrites/consumes the buffers, so the probe can ask whether the DW
 * output is just the input passed through unconvolved. Filled by
 * hw_dw_fused_l0_loopback_test(); ignored when probe_in_valid == 0. */
#define PROBE_ROW_MAX 2048
static uint8_t probe_in_row[PROBE_ROW_MAX];
static int     probe_in_valid = 0;

/* PROBE v3: the three input rows the 3x3 window needs at the probe row
 * (h-1, h, h+1 of channel 0), plus channel 0's kernel/requant params, so the
 * host can recompute the convolution under different tap masks and identify
 * which taps the hardware is actually applying. */
static uint8_t  probe_in3[3][PROBE_ROW_MAX];
static int8_t   probe_w9[9];
static int32_t  probe_bias;
static uint32_t probe_mult;
static uint8_t  probe_shift;
static uint8_t  probe_zp_in, probe_zp_out;
static int      probe_relu;
static int      probe_conv_valid = 0;

/* PROBE v4: got rows at h=H/2 per channel, and ref rows at h=H/2-1..+1 per
 * channel, for the cross-channel / cross-row match matrix. */
static uint8_t  probe_got_c[3][PROBE_ROW_MAX];
static uint8_t  probe_ref_ch[3][3][PROBE_ROW_MAX];
static unsigned probe_matrix_valid = 0;

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

            /* PROBE v4 stash (2026-07-27): keep got rows at h=H/2 for every
             * channel, and ref rows at h=H/2-1..H/2+1 for every channel, so the
             * cross-channel / cross-row match matrix can be printed after the
             * pass (rows arrive in file order, so this cannot be done inline).
             * v2's "uniform positive bias => arithmetic" was a bad inference:
             * a DIFFERENT CHANNEL's correct output would also read as a uniform
             * offset, since each channel has its own kernel and bias. */
            if (c < 3 && h == (H / 2) && W <= PROBE_ROW_MAX) {
                memcpy(probe_got_c[c], got_row, (size_t)W);
                probe_matrix_valid |= (1u << c);
            }
            if (c < 3 && W <= PROBE_ROW_MAX) {
                int dh = h - (H / 2);
                if (dh >= -1 && dh <= 1) memcpy(probe_ref_ch[c][dh + 1], ref_row, (size_t)W);
            }

            /* CORRUPTION PROBE (2026-07-27). The 64b build returns exact beat
             * counts but ~96% of pixels wrong, deterministically. This
             * discriminates the remaining explanations on ONE clean mid-row,
             * away from the known-broken left/right edges:
             *   - best_d != 0 with a high match rate  => the row is SHIFTED by
             *     best_d pixels (a dropped/duplicated group somewhere upstream)
             *   - best_d == 0 but lane-permutation scores high => bytes are
             *     reordered WITHIN each 8-pixel group (endianness/lane mapping)
             *   - nothing scores above chance (~0.4%) => values are genuinely
             *     miscomputed, not misplaced; the arithmetic is wrong.
             * Cheap: runs once, on one row, prints a handful of lines. */
            if (c == 0 && h == (H / 2)) {
                int lo = 64, hi = W - 64;          /* interior only */
                int span = hi - lo;
                int best_d = 0, best_hits = -1;
                printf("[PROBE] row c=0 h=%d, scanning shifts over w[%d..%d)\n", h, lo, hi);
                for (int d = -16; d <= 16; d++) {
                    int hits = 0;
                    for (int w = lo; w < hi; w++) {
                        int rw = w + d;
                        if (rw >= 0 && rw < W && got_row[w] == ref_row[rw]) hits++;
                    }
                    if (hits > best_hits) { best_hits = hits; best_d = d; }
                    if (d >= -2 && d <= 2) {
                        printf("[PROBE]   shift %+d : %d/%d (%d.%02d%%)\n", d, hits, span,
                               (hits * 100) / span, ((hits * 10000) / span) % 100);
                    }
                }
                printf("[PROBE] best shift %+d : %d/%d (%d.%02d%%)  [chance ~0.39%%]\n",
                       best_d, best_hits, span, (best_hits * 100) / span,
                       ((best_hits * 10000) / span) % 100);

                /* Lane permutation within the 8-pixel group, at zero shift. */
                for (int p = 0; p < 3; p++) {
                    int hits = 0;
                    for (int w = lo; w < hi; w++) {
                        int base = w & ~7, lane = w & 7, rl;
                        if      (p == 0) rl = lane ^ 4;   /* 32b half-swap */
                        else if (p == 1) rl = 7 - lane;   /* full reverse  */
                        else             rl = lane ^ 1;   /* byte-pair swap*/
                        if (got_row[w] == ref_row[base + rl]) hits++;
                    }
                    printf("[PROBE]   perm %s : %d/%d (%d.%02d%%)\n",
                           p == 0 ? "half-swap (lane^4)" :
                           p == 1 ? "reverse   (7-lane)" : "pair-swap (lane^1)",
                           hits, span, (hits * 100) / span, ((hits * 10000) / span) % 100);
                }

                printf("[PROBE] got[720..735]:");
                for (int k = 720; k < 736; k++) printf(" %02X", got_row[k]);
                printf("\n[PROBE] ref[720..735]:");
                for (int k = 720; k < 736; k++) printf(" %02X", ref_row[k]);
                printf("\n");

                /* PROBE v2 (2026-07-27). v1 showed no shift and no lane
                 * permutation -- the data is miscomputed, not misplaced -- and
                 * got > ref in 16/16 sampled bytes. Quantify that bias over the
                 * whole row rather than 16 samples. If the sign split is uniform
                 * the cause is arithmetic and positional theories are dead; if
                 * it comes back ~50/50 the next step is a cross-row/channel scan
                 * (a wrong ROW or CHANNEL is the one positional story v1 could
                 * not see, since it only scanned horizontally within one row). */
                {
                    long gt = 0, lt = 0, eq = 0, sum = 0;
                    int dmin = 255, dmax = -255;
                    for (int w = lo; w < hi; w++) {
                        int diff = (int)got_row[w] - (int)ref_row[w];
                        if (diff > 0) gt++; else if (diff < 0) lt++; else eq++;
                        sum += diff;
                        if (diff < dmin) dmin = diff;
                        if (diff > dmax) dmax = diff;
                    }
                    printf("[PROBE] diff got-ref over w[%d..%d): >0:%ld  <0:%ld  ==:%ld\n",
                           lo, hi, gt, lt, eq);
                    printf("[PROBE] diff mean=%ld.%02ld min=%d max=%d\n",
                           sum / span, (sum * 100 / span) % 100, dmin, dmax);
                    printf("[PROBE]   (uniform >0 => arithmetic bias; ~50/50 => positional)\n");
                }

                /* Is the output actually the raw INPUT passed through unconvolved?
                 * probe_in_row was snapshotted before the DMA. */
                if (probe_in_valid) {
                    int hits = 0;
                    for (int w = lo; w < hi; w++)
                        if (got_row[w] == probe_in_row[w]) hits++;
                    printf("[PROBE] got vs INPUT same row: %d/%d (%d.%02d%%)\n",
                           hits, span, (hits * 100) / span, ((hits * 10000) / span) % 100);
                    printf("[PROBE] in [720..735]:");
                    for (int k = 720; k < 736; k++) printf(" %02X", probe_in_row[k]);
                    printf("\n");
                }

                /* PROBE v3 (2026-07-27). The bias is uniform and upward
                 * (>0 in 1290/1307, mean +49.6), i.e. the accumulator runs
                 * high, which is what dropping negative-weight taps looks like
                 * (K0 = 4 16 -2 / -60 -27 10 / -128 -42 11, left column sums
                 * -184). Rather than infer which taps from the bias magnitude
                 * -- which needs the row's mean input -- recompute the
                 * convolution here under each tap mask and see which one
                 * reproduces the hardware output. mask bit i = tap i APPLIED.
                 * Uses the project's own round_shift_ties_to_even_i64 +
                 * clamp_u8_i32 so the arithmetic matches the golden path. */
                if (probe_conv_valid) {
                    static const struct { unsigned m; const char *name; } masks[] = {
                        { 0x1FF, "all 9 (correct)"      },
                        { 0x1B6, "no left col (0,3,6)"  },
                        { 0x0DB, "no right col (2,5,8)" },
                        { 0x1F8, "no top row (0,1,2)"   },
                        { 0x03F, "no bottom row (6,7,8)"},
                        { 0x1C7, "no mid row (3,4,5)"   },
                        { 0x010, "center tap only (4)"  },
                        { 0x1FE, "no tap 0"             },
                        { 0x1F7, "no tap 3"             },
                        { 0x1BF, "no tap 6"             },
                    };
                    int nm = (int)(sizeof(masks) / sizeof(masks[0]));
                    printf("[PROBE] tap-mask reconstruction on c=0 h=%d"
                           " (w9 = %d %d %d %d %d %d %d %d %d)\n", h,
                           probe_w9[0], probe_w9[1], probe_w9[2],
                           probe_w9[3], probe_w9[4], probe_w9[5],
                           probe_w9[6], probe_w9[7], probe_w9[8]);
                    for (int mi = 0; mi < nm; mi++) {
                        unsigned msk = masks[mi].m;
                        int hit_got = 0, hit_ref = 0;
                        for (int w = lo; w < hi; w++) {
                            int32_t acc = probe_bias;
                            for (int r = 0; r < 3; r++) {
                                for (int k = 0; k < 3; k++) {
                                    int idx = r * 3 + k;
                                    if (!((msk >> idx) & 1u)) continue;
                                    int xw = w - 1 + k;
                                    int xv = (xw >= 0 && xw < W)
                                             ? (int)probe_in3[r][xw] : (int)probe_zp_in;
                                    acc += (int32_t)probe_w9[idx] *
                                           (int32_t)(xv - (int)probe_zp_in);
                                }
                            }
                            {
                                int64_t sc = (int64_t)acc * (int64_t)probe_mult;
                                int32_t y  = round_shift_ties_to_even_i64(sc, probe_shift);
                                int32_t pre = y + (int32_t)probe_zp_out;
                                uint8_t out = clamp_u8_i32(pre);
                                if (out == got_row[w]) hit_got++;
                                if (out == ref_row[w]) hit_ref++;
                            }
                        }
                        printf("[PROBE]   %-22s vs got %4d/%d (%2d.%02d%%)   vs ref %4d/%d (%2d.%02d%%)\n",
                               masks[mi].name,
                               hit_got, span, (hit_got * 100) / span, ((hit_got * 10000) / span) % 100,
                               hit_ref, span, (hit_ref * 100) / span, ((hit_ref * 10000) / span) % 100);
                    }
                    printf("[PROBE]   ('all 9' should match ref ~100%% -- if it does not,\n");
                    printf("[PROBE]    the host model is wrong, not the hardware.)\n");
                }
            }

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

    /* PROBE v4 report: does got[channel A] equal the CORRECT output of some
     * other channel and/or row? A near-100% cell off the diagonal means the
     * datapath is producing correct values at the wrong (channel,row) slot --
     * an indexing/ordering fault, not an arithmetic one. A blank matrix (all
     * near the 0.39% chance floor) means the values are genuinely miscomputed
     * and belong to no channel or neighbouring row. */
    if (probe_matrix_valid == 0x7u && C >= 3 && W <= PROBE_ROW_MAX) {
        int lo = 64, hi = W - 64, span = hi - lo;
        printf("[PROBE] cross-channel/row matrix, h=%d, w[%d..%d)  [chance ~0.39%%]\n",
               H / 2, lo, hi);
        printf("[PROBE]            ref c0        ref c1        ref c2\n");
        for (int gc = 0; gc < 3; gc++) {
            for (int dh = 0; dh < 3; dh++) {
                printf("[PROBE]  got c%d vs h%+d:", gc, dh - 1);
                for (int rc = 0; rc < 3; rc++) {
                    int hits = 0;
                    for (int w = lo; w < hi; w++)
                        if (probe_got_c[gc][w] == probe_ref_ch[rc][dh][w]) hits++;
                    printf("  %4d/%d (%2d.%02d%%)", hits, span,
                           (hits * 100) / span, ((hits * 10000) / span) % 100);
                }
                printf("\n");
            }
        }

        /* PROBE v5: is this a CHANNEL bug, or is the whole stream offset by
         * one 64b word? Channel is the fastest-varying index -- linear order is
         * (h, g, c) -- so a one-word offset shows up as got[c_k] = ref[c_k+1]
         * within a group, and got[c_last] = ref[c_0] of the NEXT group, i.e.
         * 8 pixels to the right. The first two pairings were already ~95-96%;
         * this tests the third, which only a global one-word shift predicts.
         * If it lands ~95%, the fault is in the output path of dw_fused_axis.sv
         * (one word lost at the head of the stream), NOT in the windower. */
        {
            int lo2 = 64, hi2 = W - 64 - 8, span2 = hi2 - lo2;
            int h0 = 0, h1 = 0, h2 = 0;
            for (int w = lo2; w < hi2; w++) {
                if (probe_got_c[0][w] == probe_ref_ch[1][1][w])     h0++;
                if (probe_got_c[1][w] == probe_ref_ch[2][1][w])     h1++;
                if (probe_got_c[2][w] == probe_ref_ch[0][1][w + 8]) h2++;
            }
            printf("[PROBE] one-word-offset test over w[%d..%d):\n", lo2, hi2);
            printf("[PROBE]   got c0 == ref c1 (same group) : %4d/%d (%2d.%02d%%)\n",
                   h0, span2, (h0 * 100) / span2, ((h0 * 10000) / span2) % 100);
            printf("[PROBE]   got c1 == ref c2 (same group) : %4d/%d (%2d.%02d%%)\n",
                   h1, span2, (h1 * 100) / span2, ((h1 * 10000) / span2) % 100);
            printf("[PROBE]   got c2 == ref c0 (NEXT group, w+8) : %4d/%d (%2d.%02d%%)\n",
                   h2, span2, (h2 * 100) / span2, ((h2 * 10000) / span2) % 100);
            printf("[PROBE]   all three high => stream offset by one 64b word\n");
        }
    }

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

/* ============================================================
 * Group-major helpers (fused DW datapath)
 *
 * dw_fused_axi_0 / dw_reorder_0 / pw_single_oc_axis_axi_0 all speak
 * group-major/channel-minor order: for g in 0..G-1 { for c in 0..C-1 {
 * 8 samples of channel c at sequence positions [8g..8g+7] } }, vs. the
 * legacy DW_REG_* path's channel-major-planar order (W_padded stride).
 * Ported from the TCSVT fusion rewrite (C:\Users\Fahad\TCSVT\sources_1\new\main.c).
 * ============================================================ */
/* Generic fallback for any C and any W. The C==3 fast path below handles
 * the only shape this network actually packs; see the dispatcher. */
static void pack_input_group_major_generic(const uint8_t *src_unpadded,
                                   uint8_t *dst_group_major,
                                   int C, int H, int W, uint8_t pad_val)
{
    const int W_padded  = (W + 7) & ~7;
    const int G_per_row = W_padded / 8;

    /* OPTIMISATION 2026-07-30. The inner loop used to copy ONE BYTE AT A TIME
     * with a per-byte bounds test:
     *     for (l = 0; l < 8; l++) dst_lane[l] = (col0+l < W) ? src_row[col0+l] : pad;
     * That test can only fail in the LAST group of a row, and only when W is
     * not a multiple of 8 -- which is never the case for this network (every
     * surrogate layer is 1280/640/320/160 wide). So the design was paying 8
     * compares + 8 byte loads + 8 byte stores per (row, group, channel) to
     * handle a case that does not occur.
     *
     * Measured before: 2.76 MB of traffic per frame (1.38 MB in + 1.38 MB out)
     * in 8.20 ms = 337 MB/s, roughly a third of what this DDR3 streams. The
     * gap was the byte loop, not memory.
     *
     * Now: groups fully inside the image use one 8-byte move (memcpy with a
     * constant size inlines to ldrd/strd -- both pointers are 8-aligned, since
     * the base is aligned and W and g*8 are multiples of 8), and the ragged
     * tail keeps the original byte path for correctness at non-multiple-of-8
     * widths.
     *
     * NOTE the access pattern, because it bounds what SIMD can do here: the
     * DESTINATION is fully sequential, but the SOURCE jumps H*W bytes between
     * channels (230 KB on pair 0). This is an 8-byte-granular block scatter
     * across C streams, so NEON's wide loads do not apply and NEON has no
     * scatter. vst2/3/4 interleave ELEMENTS, not 8-byte blocks. Plain 8-byte
     * moves are the right primitive. */
    const int G_full = W / 8;   /* groups whose 8 columns are all inside W */

    for (int h = 0; h < H; h++) {
        const uint8_t *src_h = src_unpadded + (size_t)h * W;
        uint8_t       *dst_h = dst_group_major + (size_t)h * G_per_row * (size_t)C * 8;

        for (int g = 0; g < G_full; g++) {
            uint8_t *dst_group = dst_h + (size_t)g * (size_t)C * 8;
            const int col0 = g * 8;
            for (int c = 0; c < C; c++) {
                memcpy(dst_group + (size_t)c * 8,
                       src_h + (size_t)c * H * W + col0,
                       8);
            }
        }

        /* Ragged tail: only entered when W % 8 != 0. */
        for (int g = G_full; g < G_per_row; g++) {
            uint8_t *dst_group = dst_h + (size_t)g * (size_t)C * 8;
            const int col0 = g * 8;
            for (int c = 0; c < C; c++) {
                uint8_t *dst_lane = dst_group + (size_t)c * 8;
                const uint8_t *src_row = src_h + (size_t)c * H * W;
                for (int l = 0; l < 8; l++) {
                    const int col = col0 + l;
                    dst_lane[l] = (col < W) ? src_row[col] : pad_val;
                }
            }
        }
    }
}

/* ==================================================================
 * PACK VARIANTS + IN-BINARY BENCHMARK (2026-08-05)
 *
 * pack read 4.51 ms on 2026-07-30 and 6.84 ms today while HW (29.30 ms)
 * and weight programming (2.44 ms) stayed bit-identical, the geometry
 * stayed at real stride-2, and gm_in stayed non-cacheable. The whole
 * 2.333 ms frame-time difference is this one function, on the same
 * data, doing the same work. Nothing in it changed.
 *
 * That leaves code placement -- I-cache line alignment, branch-predictor
 * aliasing -- disturbed by the ~400 lines of PMBus code added nearby.
 * Which is precisely the kind of claim that CANNOT be tested by
 * comparing two different builds: every rebuild reshuffles placement,
 * so a second measurement is a second sample of the confound, not a
 * control for it.
 *
 * So the variants are compared INSIDE ONE BINARY, back to back, on the
 * same buffers, where placement is held constant. Each is 64-byte
 * aligned so its hot loop starts on a cache-line boundary whatever the
 * linker does around it -- that alone removes the suspected cause.
 *
 * Only pair 0 packs (C=3); pairs 1-5 are chained and pack nothing. So
 * the specialised paths target C==3 and W%8==0 (1280), and the generic
 * function stays for every other shape.
 * ================================================================== */
#define SURR_PACK_BENCH   1

#include <arm_neon.h>

/* B: three hoisted source pointers, no inner loop, no per-iteration
 * multiply. The generic version recomputes src_h + c*H*W + col0 every
 * iteration and re-enters a 3-trip loop 345,600 times per frame. */
__attribute__((aligned(64)))
static void pack_gm_c3_ptr(const uint8_t *src, uint8_t *dst, int H, int W)
{
    const int    G     = W / 8;
    const size_t plane = (size_t)H * (size_t)W;
    const size_t drow  = (size_t)(((W + 7) & ~7) / 8) * 3u * 8u;

    for (int h = 0; h < H; h++) {
        const uint8_t *s0 = src + (size_t)h * (size_t)W;
        const uint8_t *s1 = s0 + plane;
        const uint8_t *s2 = s1 + plane;
        uint8_t       *d  = dst + (size_t)h * drow;
        for (int g = 0; g < G; g++) {
            memcpy(d,      s0, 8);
            memcpy(d +  8, s1, 8);
            memcpy(d + 16, s2, 8);
            d += 24; s0 += 8; s1 += 8; s2 += 8;
        }
    }
}

/* C: two groups at a time so the destination leaves as 16-byte stores.
 * 48 contiguous output bytes per iteration = 3 x vst1q instead of 6 x
 * 8-byte stores. gm_in is Normal non-cacheable, so stores go through the
 * write buffer -- halving their count is the lever that matters, not
 * load width. G = W/8 = 160 is even, so no odd-group tail. */
__attribute__((aligned(64)))
static void pack_gm_c3_neon(const uint8_t *src, uint8_t *dst, int H, int W)
{
    const int    G     = W / 8;
    const size_t plane = (size_t)H * (size_t)W;
    const size_t drow  = (size_t)(((W + 7) & ~7) / 8) * 3u * 8u;

    for (int h = 0; h < H; h++) {
        const uint8_t *s0 = src + (size_t)h * (size_t)W;
        const uint8_t *s1 = s0 + plane;
        const uint8_t *s2 = s1 + plane;
        uint8_t       *d  = dst + (size_t)h * drow;
        for (int g = 0; g < G; g += 2) {
            uint8x8_t a0 = vld1_u8(s0),     b0 = vld1_u8(s1);
            uint8x8_t c0 = vld1_u8(s2),     a1 = vld1_u8(s0 + 8);
            uint8x8_t b1 = vld1_u8(s1 + 8), c1 = vld1_u8(s2 + 8);
            vst1q_u8(d,      vcombine_u8(a0, b0));
            vst1q_u8(d + 16, vcombine_u8(c0, a1));
            vst1q_u8(d + 32, vcombine_u8(b1, c1));
            d += 48; s0 += 16; s1 += 16; s2 += 16;
        }
    }
}

/* Dispatcher. Same in-binary benchmark, 720x1280 C=3, min of 5, TWO builds:
 *
 *                          2026-08-05   2026-08-07
 *     generic                 6.08 ms      6.07 ms    stable
 *     C=3 ptr-hoisted         4.32 ms      5.47 ms    +27%, UNSTABLE
 *     C=3 NEON 2-group        5.00 ms      5.00 ms    stable  <-- adopted
 *
 * Both specialised paths reproduce the generic output byte for byte, in
 * both builds.
 *
 * 2026-08-07: ADOPTION SWITCHED FROM ptr-hoisted TO NEON. Writing the
 * hoist into the source was supposed to remove the dependency on a
 * compiler decision. It did not: the scalar variant still moved 4.32 ->
 * 5.47 ms between two builds of the same source, because what actually
 * varies is register allocation and scheduling around the hot loop, not
 * the one multiply that got hoisted. Meanwhile NEON measured 5.00 ms in
 * BOTH builds, to the hundredth.
 *
 * So the earlier "NEON lost" conclusion was drawn from a single sample of
 * the confound it was trying to control for. On best case NEON is slower;
 * on REPRODUCIBILITY it wins outright, and it is now also faster in
 * absolute terms. A frame time that does not move when unrelated code is
 * edited is worth more than 0.5 ms of best case -- pack is 15% of the
 * frame and is the only phase that can shift without an RTL change.
 *
 * The mechanism behind NEON's stability is the same one that made it lose
 * before: it is store-bound through the write buffer on non-cacheable
 * gm_in, and that path has no register pressure for the compiler to
 * schedule differently. The scalar version is loop-overhead bound, which
 * is exactly what scheduling perturbs. */
static void pack_input_group_major(const uint8_t *src_unpadded,
                                   uint8_t *dst_group_major,
                                   int C, int H, int W, uint8_t pad_val)
{
    if (C == 3 && (W & 7) == 0) {      /* no ragged tail => pad_val unused */
        pack_gm_c3_neon(src_unpadded, dst_group_major, H, W);
        return;
    }
    pack_input_group_major_generic(src_unpadded, dst_group_major, C, H, W, pad_val);
}

#if SURR_PACK_BENCH
/* Same buffers, same data, five passes each, minimum reported. The
 * minimum is the right estimator here: DRAM refresh and stray cache
 * state can only ADD time, never subtract it. */
static void surr_pack_bench(const uint8_t *src, uint8_t *dst,
                            int C, int H, int W, uint8_t pad)
{
    double best[3] = { 1e9, 1e9, 1e9 };
    const char *nm[3] = { "generic (current)", "C=3 ptr-hoisted", "C=3 NEON 2-group" };

    if (C != 3 || (W & 7)) { printf("[PACKBENCH] skipped (C=%d W=%d)\n", C, W); return; }

    for (int rep = 0; rep < 5; rep++) {
        u64_cycles t;
        /* Deliberately the GENERIC body, not the dispatcher -- the
         * dispatcher now routes C==3 to the fast path, which would make
         * this row measure the same code twice. */
        t = timer_now(); pack_input_group_major_generic(src, dst, C, H, W, pad);
        { double m = cycles_to_ms(timer_now() - t); if (m < best[0]) best[0] = m; }
        t = timer_now(); pack_gm_c3_ptr(src, dst, H, W);
        { double m = cycles_to_ms(timer_now() - t); if (m < best[1]) best[1] = m; }
        t = timer_now(); pack_gm_c3_neon(src, dst, H, W);
        { double m = cycles_to_ms(timer_now() - t); if (m < best[2]) best[2] = m; }
    }

    printf("\n[PACKBENCH] C=%d %dx%d, 5 reps, min ms (same binary, same buffers)\n",
           C, H, W);
    for (int v = 0; v < 3; v++)
        printf("[PACKBENCH]   %-20s %6.2f ms   %5.2fx\n",
               nm[v], best[v], best[0] / best[v]);

    /* Correctness: the specialised paths must reproduce the generic
     * output byte for byte, or a speed-up means nothing. */
    {
        size_t nb = (size_t)H * (size_t)(((W + 7) & ~7) / 8) * (size_t)C * 8u;
        static uint8_t *ref = (uint8_t *)DDR_SKIP3_ADDR;
        int bad_ptr, bad_neon;
        pack_input_group_major_generic(src, ref, C, H, W, pad);
        pack_gm_c3_ptr(src, dst, H, W);
        bad_ptr = (memcmp(ref, dst, nb) != 0);
        pack_gm_c3_neon(src, dst, H, W);
        bad_neon = (memcmp(ref, dst, nb) != 0);
        printf("[PACKBENCH]   vs generic: ptr %s, neon %s\n",
               bad_ptr ? "MISMATCH" : "exact", bad_neon ? "MISMATCH" : "exact");
        pack_input_group_major(src, dst, C, H, W, pad);   /* leave dst valid */
    }
}
#endif

/* Inverse: group-major -> channel-major-planar (W_padded stride), so
 * compare_tensor_vs_ref (channel-major) can be reused unchanged. */
static void unpack_group_major_to_channel_major(const uint8_t *src_group_major,
                                                uint8_t *dst_channel_major,
                                                int C, int H, int W)
{
    int W_padded = (W + 7) & ~7;
    int G_per_row = W_padded / 8;
    for (int h = 0; h < H; h++) {
        for (int g = 0; g < G_per_row; g++) {
            const uint8_t *src_group = src_group_major + ((size_t)h * G_per_row + g) * (size_t)C * 8;
            int col0 = g * 8;
            for (int c = 0; c < C; c++) {
                uint8_t *dst_row = dst_channel_major + (size_t)c * H * W_padded + (size_t)h * W_padded;
                memcpy(dst_row + col0, src_group + (size_t)c * 8, 8);
            }
        }
    }
}

/* Debug-only: compare a group-major buffer against a channel-major-planar
 * golden reference file, by unpacking into scratch first. */
static int compare_group_major_vs_ref(const uint8_t *got_group_major, int C, int H, int W,
                                      const char *ref_path, const char *label,
                                      const runner_cfg_t *cfg,
                                      uint8_t *unpack_scratch, size_t scratch_size)
{
    /* Unpack into a caller-provided DDR buffer. The old static 256KB scratch
     * only fit tiny/diagnostic H; a real layer's output is multi-MB (L0 =
     * 3*2048*1440 = 8.4MB). Caller passes group_major_scratch (the input
     * buffer, no longer needed after the run and exactly tensor_bytes big). */
    size_t need = tensor_bytes_u8(C, H, W);
    if (need > scratch_size) {
        printf("[CMP] %s group-major unpack scratch too small (%lu > %lu)\n",
               label, (unsigned long)need, (unsigned long)scratch_size);
        return -1;
    }
    unpack_group_major_to_channel_major(got_group_major, unpack_scratch, C, H, W);
    return compare_tensor_vs_ref(unpack_scratch, C, H, W, ref_path, label, cfg);
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

#if !DW_LEGACY_HW_ENABLED
    (void)in_pixels; (void)out_h; (void)true_out_w; (void)out_w_padded; (void)out_pixels;
    (void)stride_2; (void)t; (void)t_op_start;
    (void)in_plane; (void)out_plane; (void)stride; (void)pad; (void)zp_in; (void)zp_out;
    (void)bias; (void)mult; (void)shift; (void)w9;
    (void)out_mm2s_cyc; (void)out_s2mm_cyc; (void)out_total_cyc;
    printf("[DW] ERROR: DW_conv_accel_0 no longer exists in hw.bd (deleted 2026-07-09, "
           "replaced by dw_fused_axi_0 at the same address) -- hw_dw_run_plane() refuses "
           "to run. Set DW_LEGACY_HW_ENABLED=1 only if the BD has been reverted.\n");
    return -1;
#endif

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
     * output beats) before MM2S is even submitted, so this ??? not the later
     * MM2S timer ??? is the correct t=0 for both S2MM's own duration and the
     * plane's total wall time. */
    t_op_start = timer_now();
    if (XAxiDma_SimpleTransfer(&DwDma, (UINTPTR)out_plane, out_pixels, XAXIDMA_DEVICE_TO_DMA) != XST_SUCCESS) {
        printf("ERROR: DW S2MM submit failed\n");
        return -1;
    }

    /* Pulse start ??? resets the input/output FIFOs, then core begins.
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
         * total wall time ??? there's no further wait after S2MM clears. */
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

    /* Arm RX (S2MM) first ??? so output has somewhere to drain.
     * True operation start: S2MM can begin accepting output beats from here,
     * well before MM2S is even submitted (PW streams into DDR as soon as
     * completed tiles are dumped, not only after MM2S fully finishes). */
    t_op_start = timer_now();
    if (XAxiDma_SimpleTransfer(&PwDma, (UINTPTR)rx_buf, rx_len, XAXIDMA_DEVICE_TO_DMA) != XST_SUCCESS) {
        printf("ERROR: PW S2MM submit failed\n");
        return -1;
    }
    //printf("[PW_ONCE] S2MM armed\n");

    /* Pulse start ??? this resets both FIFOs for 5 cycles, then core begins.
     * MUST happen BEFORE MM2S arm, otherwise the DMA pushes data into the
     * input FIFO and the start-triggered FIFO reset destroys it. */
    pw_write_reg(PW_REG_CTRL, 1u << 0);
    pw_write_reg(PW_REG_CTRL, 0);

    /* Check if start took effect */
    //printf("[PW_ONCE] STATUS after start: 0x%08X\n",
    //       (unsigned)pw_read_reg(PW_REG_STATUS));

    /* Now arm TX (MM2S) ??? data flows into FIFO AFTER reset is done */
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
         * DW, this is NOT necessarily the tile's total wall time ??? the core
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

    /* 2026-08-08: NO LONGER ROUNDED. Was
     *     ((Cout + PW_N_OC - 1) / PW_N_OC) * PW_N_OC
     * which padded Cout up to a multiple of N_OC so the core always ran whole
     * batches, computing and emitting garbage channels that were discarded.
     *
     * The core now implements PARTIAL-BATCH DRAIN: it drains
     * min(N_OC, cout_run - batch*N_OC) beats and its batch count is a ceiling,
     * so it emits EXACTLY cout_run channels. Feeding it the rounded value
     * defeats that entirely -- on 2026-08-08 block 0 (Cout=16) was programmed
     * with cout_run=32 at N_OC=32 and emitted 7.37 MB instead of 3.69 MB,
     * doubling its S2MM traffic and its drain time.
     *
     * Keeping the name (rather than substituting Cout at ~40 use sites) so the
     * change is one line and trivially revertible, but it is now an identity.
     * It also makes the chained handoff exact: pair p emits Cout channels,
     * which is byte-for-byte what pair p+1's DW expects, with no repack -- the
     * property ??14/??23 already claim but the driver was not delivering.
     *
     * REVERT TOGETHER with the RTL: an unrounded Cout on a core WITHOUT
     * partial-batch drain makes cout_batches_r floor to 0 when Cout < N_OC and
     * the core emits nothing. */
    const int    cout_rounded  = Cout;

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
     * Step 4: run tiles ??? single DMA per tile, all OCs at once
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

/* ============================================================
 * Stage 1 of the staged fusion validation plan (DW_PW_FUSION_PLAN.md):
 * dw_fused_axi_0 -> dw_reorder_0 -> DRAM loopback for one layer, no PW.
 * Confirms (a) dw_fused_axi_0's per-channel CH_ADDR weight/param load and
 * run sequencing work on real hardware, and (b) dw_reorder_0's output byte
 * order matches the group-major layout PW will eventually consume, by
 * comparing against the same golden 0:/REF/L%02d.BIN reference the legacy
 * path used. raw_input_unpadded is channel-major, C*H*W bytes (as loaded
 * straight from an INPUT_*.BIN/skip buffer, before any padding/packing).
 * group_major_scratch and group_major_out must be distinct DDR buffers,
 * each >= tensor_bytes_u8(d->Cin, d->H, d->W) bytes -- concurrent MM2S/S2MM
 * on the same fused core cannot safely share one buffer for in and out.
 * Ported from TCSVT's hw_dw_pw_fused_run (C:\Users\Fahad\TCSVT\sources_1\new\main.c),
 * with the PW half removed and DwDma reused for both directions since
 * dw_reorder_0 loops straight back into axi_dma_0/S_AXIS_S2MM. */
/* __attribute__((unused)): only called under #if DWF_L0_LOOPBACK_TEST, which is
 * 0 in cascade mode (the cascade bitstream has no axi_dma_0 S2MM for it to use).
 * Keep the definition compiled so switching back via BD script F2 needs only the
 * flag flipped, without tripping -Wunused-function. */
__attribute__((unused))
static int hw_dw_fused_l0_loopback_test(const layer_desc_t *d,
                                        const uint8_t *raw_input_unpadded,
                                        uint8_t *group_major_scratch,
                                        uint8_t *group_major_out,
                                        const file_blob_t *params_blob,
                                        const file_blob_t *weights_blob,
                                        const runner_cfg_t *cfg)
{
    const int C          = d->Cin;
    const int W_padded    = (d->W + 7) & ~7;
    const int G           = W_padded / 8;
    const size_t tensor_bytes = (size_t)C * (size_t)d->H * (size_t)W_padded;
    int t;

    /* H no longer has to be 1: dw_banked_window_8x (2026-07-24) does genuine
     * per-channel-banked 3x3 vertical windowing across real H rows, unlike
     * the 1x3-only dw_seq_window_8x this replaced. See DW_PW_FUSION_PLAN_V2.md. */

    /* DIAGNOSTIC (2026-07-25, chasing the MM2S-timeout-on-real-scale bug):
     * read STATUS before touching anything. If busy is already stuck at 1
     * here, start_pulse in dw_fused_axi.sv's AXI-Lite FSM will never fire
     * ("if (s_axi_wdata[0] && !busy) start_pulse<=1"), the windower never
     * enters running, and s_axis_tready still accepts DMA data up to the
     * 2048-entry input FIFO depth before genuinely stalling -- indistinguishable
     * from a real core hang until you look at this register. Nothing in
     * this session's xsim exercised this AXI-Lite control layer at all (the
     * testbench drove start_in directly on the windower port). */
    {
        u32 pre_status = dw_read_reg(DWF_REG_STATUS);
        printf("[DWF-L0] pre-run STATUS=0x%08X (bit0=done_sticky=%d bit1=busy=%d)\n",
               (unsigned)pre_status, (int)(pre_status & 1), (int)((pre_status >> 1) & 1));
    }

    pack_input_group_major(raw_input_unpadded, group_major_scratch, C, d->H, d->W, d->zp_in);

    /* Snapshot the raw input row the probe inspects (c=0, h=H/2), before
     * group_major_scratch gets reused as the compare unpack buffer. */
    probe_in_valid = 0;
    probe_conv_valid = 0;
    if (d->W <= PROBE_ROW_MAX) {
        int ph = d->H / 2;
        memcpy(probe_in_row,
               raw_input_unpadded + (size_t)ph * (size_t)d->W, (size_t)d->W);
        probe_in_valid = 1;

        /* rows ph-1, ph, ph+1 of channel 0 -- the 3x3 window's vertical extent */
        for (int r = 0; r < 3; r++) {
            int hr = ph - 1 + r;
            if (hr >= 0 && hr < d->H) {
                memcpy(probe_in3[r],
                       raw_input_unpadded + (size_t)hr * (size_t)d->W, (size_t)d->W);
            } else {
                memset(probe_in3[r], d->zp_in, (size_t)d->W);
            }
        }
        probe_zp_in  = d->zp_in;
        probe_zp_out = d->zp_out;
        probe_relu   = d->relu_en;
        if (load_dw_weights_3x3(weights_blob, d->weight_addr, 0, probe_w9) == 0 &&
            load_param_block_16B(params_blob, d->params_addr, 0,
                                 &probe_bias, &probe_mult, &probe_shift) == 0) {
            probe_conv_valid = 1;
        }
    }

    for (int c = 0; c < C; c++) {
        int8_t   w9[9];
        int32_t  bias;
        uint32_t mult;
        uint8_t  shift;

        if (load_dw_weights_3x3(weights_blob, d->weight_addr, c, w9) != 0) {
            printf("[DWF-L0] weight load fail ch=%d\n", c);
            return -1;
        }
        if (load_param_block_16B(params_blob, d->params_addr, c, &bias, &mult, &shift) != 0) {
            printf("[DWF-L0] param load fail ch=%d\n", c);
            return -1;
        }

        dw_write_reg(DWF_REG_CH_ADDR, c);
        dw_write_reg(DWF_REG_W0, dw_pack_w0(w9[0], w9[1], w9[2], w9[3]));
        dw_write_reg(DWF_REG_W1, dw_pack_w1(w9[4], w9[5], w9[6], w9[7]));
        dw_write_reg(DWF_REG_W2, dw_pack_w2(w9[8]));       /* commits weights */
        dw_write_reg(DWF_REG_BIAS,  (u32)bias);
        dw_write_reg(DWF_REG_MULT,  mult);
        dw_write_reg(DWF_REG_SHIFT, shift);                /* commits params */
    }

    dw_write_reg(DWF_REG_CIN_RUN, C);
    dw_write_reg(DWF_REG_N_GROUPS, G);
    dw_write_reg(DWF_REG_IMG_WIDTH, (u32)d->W);
    dw_write_reg(DWF_REG_N_ROWS, (u32)d->H);
    dw_write_reg(DWF_REG_ZP_RELU, pw_pack_zp_relu(d->zp_in, d->zp_out, d->relu_en));

    /* DIAGNOSTIC: read back what the AXI-Lite wrapper actually latched,
     * independent of what we think we wrote -- same technique that proved
     * register programming was fine (and thus not the culprit) during the
     * original L0 hardware debug. */
    printf("[DWF-L0] readback CIN_RUN=%u N_GROUPS=%u IMG_WIDTH=%u N_ROWS=%u ZP_RELU=0x%08X (expect %d %d %d %d)\n",
           (unsigned)dw_read_reg(DWF_REG_CIN_RUN), (unsigned)dw_read_reg(DWF_REG_N_GROUPS),
           (unsigned)dw_read_reg(DWF_REG_IMG_WIDTH), (unsigned)dw_read_reg(DWF_REG_N_ROWS),
           (unsigned)dw_read_reg(DWF_REG_ZP_RELU), C, G, d->W, d->H);

    Xil_DCacheFlushRange((UINTPTR)group_major_scratch, tensor_bytes);
    /* DIAGNOSTIC (2026-07-25): sentinel-fill the output buffer so a timeout can
     * reveal how many bytes S2MM actually landed. S2MM writes a contiguous
     * prefix, so the last non-sentinel byte ~= how far the core got before
     * stalling (0 = produced nothing; == tensor_bytes = produced everything
     * but TLAST never delivered). 0x5A is an unlikely genuine output byte. */
    memset(group_major_out, 0x5A, tensor_bytes);
    Xil_DCacheFlushRange((UINTPTR)group_major_out, tensor_bytes);
    Xil_DCacheInvalidateRange((UINTPTR)group_major_out, tensor_bytes);

    /* Arm S2MM (output), pulse start (resets FIFOs, begins windower), THEN
     * arm MM2S (input) -- same ordering rule as hw_dw_run_plane: starting
     * after MM2S is armed lets the FIFO-reset destroy data the DMA already
     * pushed in. */
    if (XAxiDma_SimpleTransfer(&DwDma, (UINTPTR)group_major_out, tensor_bytes,
                               XAXIDMA_DEVICE_TO_DMA) != XST_SUCCESS) {
        printf("[DWF-L0] S2MM submit failed\n");
        return -1;
    }

    dw_write_reg(DWF_REG_CTRL, 1u << 0);
    dw_write_reg(DWF_REG_CTRL, 0);

    /* DIAGNOSTIC: if busy didn't go to 1 here, start_pulse never latched
     * (CTRL FSM gates on !busy -- if pre-run STATUS above already showed
     * busy=1, this confirms it, rather than us discovering it only via
     * the eventual MM2S timeout). */
    {
        u32 post_start_status = dw_read_reg(DWF_REG_STATUS);
        printf("[DWF-L0] post-start STATUS=0x%08X (bit0=done_sticky=%d bit1=busy=%d) -- expect busy=1\n",
               (unsigned)post_start_status, (int)(post_start_status & 1),
               (int)((post_start_status >> 1) & 1));
    }

    if (XAxiDma_SimpleTransfer(&DwDma, (UINTPTR)group_major_scratch, tensor_bytes,
                               XAXIDMA_DMA_TO_DEVICE) != XST_SUCCESS) {
        printf("[DWF-L0] MM2S submit failed\n");
        return -1;
    }

    t = 0;
    while (XAxiDma_Busy(&DwDma, XAXIDMA_DMA_TO_DEVICE)) {
        if (++t > 50000000) { printf("[DWF-L0] MM2S timeout\n"); return -1; }
    }
    t = 0;
    while (XAxiDma_Busy(&DwDma, XAXIDMA_DEVICE_TO_DMA)) {
        if (++t > 50000000) {
            u32 st = dw_read_reg(DWF_REG_STATUS);
            size_t last = 0, nonsent = 0;
            const uint8_t *ob = (const uint8_t *)group_major_out;
            Xil_DCacheInvalidateRange((UINTPTR)group_major_out, tensor_bytes);
            for (size_t k = 0; k < tensor_bytes; k++) {
                if (ob[k] != 0x5A) { nonsent++; last = k + 1; }
            }
            printf("[DWF-L0] S2MM timeout. post STATUS=0x%08X (done_sticky=%d busy=%d)\n",
                   (unsigned)st, (int)(st & 1), (int)((st >> 1) & 1));
            printf("[DWF-L0]   S2MM landed: last_nonsentinel=%lu of %lu bytes, %lu non-sentinel total\n",
                   (unsigned long)last, (unsigned long)tensor_bytes, (unsigned long)nonsent);
            printf("[DWF-L0]   out[0..15]:");
            for (int k = 0; k < 16 && (size_t)k < tensor_bytes; k++) printf(" %02X", ob[k]);
            printf("\n");
            /* Internal datapath counters -- read straight from the core over
             * AXI-Lite. consumed = input words pulled; written = 64b groups the
             * core pushed into the out FIFO (0 => windower emits nothing;
             * >0 => output path/hold register not draining); produced = beats to
             * S2MM. win_state = frozen windower FSM position. */
            {
                u32 dc = dw_read_reg(DWF_REG_DBG_CONSUMED);
                u32 dw = dw_read_reg(DWF_REG_DBG_WRITTEN);
                u32 dp = dw_read_reg(DWF_REG_DBG_PRODUCED);
                u32 ws = dw_read_reg(DWF_REG_DBG_WINSTATE);
                u32 fl = dw_read_reg(DWF_REG_DBG_FLAGS);
                printf("[DWF-L0]   DBG consumed=%u written=%u produced=%u\n",
                       (unsigned)dc, (unsigned)dw, (unsigned)dp);
                printf("[DWF-L0]   DBG win_state=0x%08X (running=%d draining=%d r_cnt=%u g_cnt=%u c_cnt=%u)\n",
                       (unsigned)ws, (int)((ws >> 31) & 1), (int)((ws >> 30) & 1),
                       (unsigned)((ws >> 24) & 0x3F), (unsigned)((ws >> 12) & 0xFFF),
                       (unsigned)(ws & 0xFFF));
                printf("[DWF-L0]   DBG flags=0x%08X (core_done_seen=%d all_written=%d out_full_ever=%d out_prog_full=%d in_empty=%d)\n",
                       (unsigned)fl, (int)(fl & 1), (int)((fl >> 1) & 1),
                       (int)((fl >> 2) & 1), (int)((fl >> 3) & 1), (int)((fl >> 4) & 1));
                {
                    u32 cfg = dw_read_reg(DWF_REG_DBG_WINCFG);
                    printf("[DWF-L0]   DBG win_cfg=0x%08X (windower-latched C_r=%u G_r=%u H_r=%u -- expect 3 180 4)\n",
                           (unsigned)cfg, (unsigned)(cfg & 0xFFF),
                           (unsigned)((cfg >> 12) & 0xFFF), (unsigned)((cfg >> 24) & 0xFF));
                }
            }
            return -1;
        }
    }
    t = 0;
    while ((dw_read_reg(DWF_REG_STATUS) & 1) == 0) {
        if (++t > 5000000) {
            printf("[DWF-L0] core did not complete. STATUS=0x%08X\n",
                   (unsigned)dw_read_reg(DWF_REG_STATUS));
            return -1;
        }
    }

    Xil_DCacheInvalidateRange((UINTPTR)group_major_out, tensor_bytes);

    /* Counters on the SUCCESS path too (2026-07-27). The identical dump below
     * only fires in the S2MM-timeout branch, so a run that completes but
     * returns wrong data -- exactly the 64b-width regression -- printed
     * nothing. At 64b every counter unit is one 8-byte group, so consumed and
     * produced should both equal C*G*H = tensor_bytes/8. written > that means
     * the windower emits extra flush beats and S2MM truncates the tail;
     * written < that means beats are being dropped at the FIFO. */
    {
        u32 dc = dw_read_reg(DWF_REG_DBG_CONSUMED);
        u32 dwr = dw_read_reg(DWF_REG_DBG_WRITTEN);
        u32 dp = dw_read_reg(DWF_REG_DBG_PRODUCED);
        u32 ws = dw_read_reg(DWF_REG_DBG_WINSTATE);
        u32 fl = dw_read_reg(DWF_REG_DBG_FLAGS);
        u32 expect = (u32)C * (u32)G * (u32)d->H;
        printf("[DWF-L0] DBG consumed=%u written=%u produced=%u (expect %u each)\n",
               (unsigned)dc, (unsigned)dwr, (unsigned)dp, (unsigned)expect);
        printf("[DWF-L0] DBG delta consumed=%+ld written=%+ld produced=%+ld\n",
               (long)dc - (long)expect, (long)dwr - (long)expect,
               (long)dp - (long)expect);
        printf("[DWF-L0] DBG win_state=0x%08X (running=%d draining=%d r_cnt=%u g_cnt=%u c_cnt=%u)\n",
               (unsigned)ws, (int)((ws >> 31) & 1), (int)((ws >> 30) & 1),
               (unsigned)((ws >> 24) & 0x3F), (unsigned)((ws >> 12) & 0xFFF),
               (unsigned)(ws & 0xFFF));
        printf("[DWF-L0] DBG flags=0x%08X (core_done_seen=%d all_written=%d out_full_ever=%d out_prog_full=%d in_empty=%d)\n",
               (unsigned)fl, (int)(fl & 1), (int)((fl >> 1) & 1),
               (int)((fl >> 2) & 1), (int)((fl >> 3) & 1), (int)((fl >> 4) & 1));
    }

    printf("[DWF-L0] loopback complete (C=%d H=%d W=%d G=%d) -- comparing vs golden L00\n",
           C, d->H, d->W, G);
    return compare_group_major_vs_ref(group_major_out, C, d->H, d->W,
                                      "0:/REF/L00.BIN", "DWF-L0", cfg,
                                      group_major_scratch, tensor_bytes);
}

/* ============================================================
 * On-chip DW(L00) -> PW(L01) cascade (2026-07-27)
 *
 * Bitstream topology (BD script F1_bd_cascade_mode.tcl):
 *     axi_dma_0 MM2S -> dw_fused_axi_0 -> pw_single_oc_axis_axi_0 -> axi_dma_1 S2MM
 * There is no DW->DRAM path in this build, so DW's output is never observable
 * directly; only PW's output reaches memory.
 *
 * Why no transpose is needed: pw_pixel_major_core consumes GROUP-MAJOR --
 * S_LOAD_FIRST and L_LOADING each pull cin_run consecutive 64b words into the
 * pixel buffer at address ic_idx, i.e. one word per input channel for the same
 * 8 pixels. That is exactly dw_fused_axis's emission order. Both streams are
 * 64b (CORE_DATA_W = N_LANES*DATA_WIDTH = 64), so the handoff is a plain FIFO.
 *
 * Sizing for L00->L01 (C=3, H=2048, W=1435 -> W_padded=1440, Cout=30):
 *   in_bytes      = 3 * 2048 * 1440            =  8,847,360
 *   total_pixels  = 2048 * 1440                =  2,949,120
 *   tile_groups   = total_pixels / 8           =    368,640   (DW emits 3 words each)
 *   out_bytes     = total_pixels * cout_rounded= 88,473,600   (~84.4 MB)
 *
 * S2MM CHUNKING: axi_dma's c_sg_length_width=26 caps ONE transfer at 64MB-1,
 * and the output is 84.4MB, so S2MM is armed in chunks. A chunk that fills
 * without TLAST completes normally; PW backpressures through its output FIFO
 * while we re-arm, and the final chunk ends on PW's TLAST.
 *
 * Ordering: PW is started before DW (a start pulse resets that core's FIFOs for
 * 5 cycles and would destroy anything already queued), and MM2S is armed last
 * for the same reason -- same rule the standalone paths follow.
 * ============================================================ */
#define CASCADE_S2MM_CHUNK  (32u * 1024u * 1024u)   /* 8-byte aligned, < 64MB cap */

/* Dump an AXI DMA channel's simple-mode registers. The cascade stalls with the
 * S2MM still reporting Busy after exactly 65,536 output beats (2^16, = 256
 * bursts of 256), which is what a HALTED-on-error channel looks like from
 * XAxiDma_Busy()'s point of view. Halted/Idle plus the three error bits
 * distinguish "the DMA gave up" from "PW stopped sending". */
#define CASC_DMA_RX   0x30u   /* XAXIDMA_RX_OFFSET      */
#define CASC_DMA_CR   0x00u   /* XAXIDMA_CR_OFFSET      */
#define CASC_DMA_SR   0x04u   /* XAXIDMA_SR_OFFSET      */
#define CASC_DMA_ADDR 0x18u   /* XAXIDMA_DESTADDR_OFFSET*/
#define CASC_DMA_LEN  0x28u   /* XAXIDMA_BUFFLEN_OFFSET */

static void cascade_dump_dma(XAxiDma *dma, const char *label)
{
    UINTPTR b = dma->RegBase;
    u32 cr  = XAxiDma_ReadReg(b, CASC_DMA_RX + CASC_DMA_CR);
    u32 sr  = XAxiDma_ReadReg(b, CASC_DMA_RX + CASC_DMA_SR);
    u32 ad  = XAxiDma_ReadReg(b, CASC_DMA_RX + CASC_DMA_ADDR);
    u32 ln  = XAxiDma_ReadReg(b, CASC_DMA_RX + CASC_DMA_LEN);
    printf("[CASCADE]   %s S2MM CR=0x%08X SR=0x%08X ADDR=0x%08X LEN=%lu\n",
           label, (unsigned)cr, (unsigned)sr, (unsigned)ad, (unsigned long)ln);
    printf("[CASCADE]     SR: Halted=%d Idle=%d IntErr=%d SlvErr=%d DecErr=%d "
           "IOC_Irq=%d Err_Irq=%d\n",
           (int)(sr & 1), (int)((sr >> 1) & 1), (int)((sr >> 4) & 1),
           (int)((sr >> 5) & 1), (int)((sr >> 6) & 1),
           (int)((sr >> 12) & 1), (int)((sr >> 14) & 1));
    if ((sr >> 4) & 7)
        printf("[CASCADE]     -> DMA ERRORED: it stopped, PW did not.\n");
    else if (!((sr >> 1) & 1))
        printf("[CASCADE]     -> DMA healthy and still waiting: PW stopped sending.\n");
}

/* Coarse progress frontier: samples every 4KB rather than scanning all of it,
 * so it can be called repeatedly DURING the S2MM wait. Distinguishes the two
 * cases the single-shot scan cannot: a PW that has genuinely stalled (frontier
 * frozen between samples) from one that is merely slow (frontier advancing).
 * Returns the highest 4KB-aligned offset holding non-sentinel data. */
__attribute__((unused))
static size_t cascade_frontier(const uint8_t *buf, size_t n)
{
    size_t frontier = 0;
    Xil_DCacheInvalidateRange((UINTPTR)buf, n);
    for (size_t k = 0; k < n; k += 4096) {
        if (buf[k] != CASCADE_SENTINEL) frontier = k + 1;
    }
    return frontier;
}

/* How far did PW's output actually get? Scans back from the end of the
 * sentinel-filled buffer for the last byte the DMA overwrote. Reports in
 * PW output-beat units (8 B) and in pixel groups (cout_rounded beats each)
 * so the stall point maps onto the core's own loop counters. */
__attribute__((unused))
static void cascade_report_landed(const uint8_t *buf, size_t n)
{
    size_t last = 0, nonsent = 0;
    Xil_DCacheInvalidateRange((UINTPTR)buf, n);
    for (size_t k = 0; k < n; k++) {
        if (buf[k] != CASCADE_SENTINEL) { nonsent++; last = k + 1; }
    }
    printf("[CASCADE]   landed: last_nonsentinel=%lu of %lu B (%lu non-sentinel)\n",
           (unsigned long)last, (unsigned long)n, (unsigned long)nonsent);
    printf("[CASCADE]   = %lu output beats of 8 B\n", (unsigned long)(last / 8));
    if (last == 0)
        printf("[CASCADE]   PW emitted NOTHING -- stall is before the first output beat\n");
}

/* NOTE: DW and PW take SEPARATE blob pairs. The real network keeps every
 * layer's weights in one PARAMS.BIN/WEIGHTS.BIN and distinguishes them by
 * weight_addr/params_addr, so it passes the same blob for both. The surrogate
 * allocates a separate dummy blob per layer, each at addr 0, so it must pass
 * different ones -- reading DW weights out of a PW-sized blob overruns it. */
static int hw_dw_pw_cascade_l0_l1(const layer_desc_t *dw_d,
                                  const layer_desc_t *pw_d,
                                  const uint8_t *raw_input_unpadded,
                                  uint8_t *gm_in,
                                  uint8_t *pw_out,
                                  const file_blob_t *dw_params_blob,
                                  const file_blob_t *dw_weights_blob,
                                  const file_blob_t *pw_params_blob,
                                  const file_blob_t *pw_weights_blob,
                                  unsigned in_flags)
{
    const int input_prepacked = (in_flags & CASC_IN_PREPACKED) != 0u;
    const int input_noncached = (in_flags & CASC_IN_NONCACHED) != 0u;
    const int C            = dw_d->Cin;
    const int W            = dw_d->W;
    const int H            = dw_d->H;
    const int W_padded     = (W + 7) & ~7;
    const int G            = W_padded / 8;
    const size_t in_bytes  = (size_t)C * (size_t)H * (size_t)W_padded;

    /* STRIDE-2 (2026-07-30). H/W above are the DW's INPUT dims. PW runs on the
     * DW's OUTPUT, which is half-resolution when the DW stage downsamples.
     * Previously total_px used the DW input dims, which was only correct
     * because the caller forced dw.H = pw.H / dw.W = pw.W -- the stride-1 cost
     * proxy. With a real stride-2 DW those are different and PW must be sized
     * from the DW OUTPUT or the two ends of the cascade disagree on length.
     *
     * Output columns: the windower emits G/2 dense groups of 8 per row, so
     * W_out = W_padded/2 exactly. Output rows: only even Y survive => ceil(H/2). */
    const int dw_stride2   = (dw_d->stride == 2);
    const int H_out        = dw_stride2 ? (H + 1) / 2 : H;
    const int W_out        = dw_stride2 ? (W_padded / 2) : W_padded;

    const int Cin_pw       = pw_d->Cin;
    const int Cout_pw      = pw_d->Cout;
    /* 2026-08-08: NO LONGER ROUNDED -- this is the CASCADE path, the one the
     * board actually runs, and the one that programmed COUT=32 for a Cout=16
     * layer on 2026-08-08 (pair 0 emitted 7.37 MB instead of 3.69 MB).
     *
     * The core now drains min(N_OC, cout_run - batch*N_OC) and ceilings its
     * batch count, so it emits EXACTLY cout_run channels. Rounding here
     * defeats partial-batch drain completely: the RTL never sees the true Cout.
     *
     * out_bytes, the S2MM arm length, the chained handoff to the next pair's DW
     * and the readback stride all derive from this one value, so they follow
     * automatically. The handoff becomes exact -- pair p emits Cout channels,
     * byte-for-byte what pair p+1's DW consumes.
     *
     * REVERT TOGETHER with the RTL: unrounded Cout on a core WITHOUT
     * partial-batch drain floors cout_batches_r to 0 when Cout < N_OC and the
     * core emits nothing at all. */
    const int cout_rounded = Cout_pw;
    const size_t total_px  = (size_t)H_out * (size_t)W_out;
    const size_t out_bytes = total_px * (size_t)cout_rounded;

    /* Hardware does NOT check these; see the SCOPE block in
     * dw_banked_window_8x.sv. Violating them corrupts silently. */
    if (dw_stride2) {
        if (G & 1) {
            CPRINT("[CASCADE] stride2 needs an EVEN group count, got G=%d "
                   "(img_width %d must be a multiple of 16)\n", G, W);
            return -1;
        }
        if (C < 2) {
            CPRINT("[CASCADE] stride2 needs cin_run >= 2, got C=%d\n", C);
            return -1;
        }
    }

    static int8_t w_ic[240];
    int32_t  bias; uint32_t mult; uint8_t shift;
    int t;

    CPRINT("[CASCADE] L00 DW [%d,%d,%d] -> L01 PW Cout=%d (rounded %d)\n",
           C, H, W, Cout_pw, cout_rounded);
    CPRINT("[CASCADE] in=%lu B  total_px=%lu  out=%lu B\n",
           (unsigned long)in_bytes, (unsigned long)total_px,
           (unsigned long)out_bytes);

    if (Cin_pw != dw_d->Cout) {
        CPRINT("[CASCADE] shape mismatch: DW Cout=%d but PW Cin=%d\n",
               dw_d->Cout, Cin_pw);
        return -1;
    }

    /* Timing breakdown (2026-07-28). The old "chunk done" figure was NOT the
     * datapath time: t_drain0 is taken after MM2S completes, but MM2S only
     * completes once DW has consumed all its input, and DW is backpressured by
     * PW -- so by then PW is nearly finished and the drain measures only the
     * tail. (Pair 2 measured 155 cycles for 1.3 MB, which gives it away.) The
     * real hardware time runs from starting the cores to the last byte landing,
     * so it is bracketed by t_hw0..t_end below. */
    u64_cycles t_fn0 = timer_now(), t_pack, t_prog, t_cache, t_hw0, t_end;

    /* ---- input ----
     * input_prepacked = 1 means gm_in ALREADY holds group-major data that the
     * previous pair's PW wrote there via S2MM. Chaining (2026-07-30): PW emits
     * group-major, channel-minor with cout_rounded channels per pixel group,
     * which is byte-for-byte the format the next DW consumes -- and at N_OC=8
     * cout_rounded == Cout exactly, so no repack is needed. The byte counts
     * line up on the nose: pair 0 out = 230400 px x 8 = 1,843,200 B, pair 1
     * DW in = 8 x 360 x 640 = 1,843,200 B. So the CPU does no packing AND no
     * flushing for pairs 1..n-1: it never touches those bytes. */
    if (!input_prepacked)
        pack_input_group_major(raw_input_unpadded, gm_in, C, H, W, dw_d->zp_in);
    t_pack = timer_now();

    /* ---- DW weights/params, one channel at a time ---- */
    for (int c = 0; c < C; c++) {
        int8_t w9[9];
        if (load_dw_weights_3x3(dw_weights_blob, dw_d->weight_addr, c, w9) != 0 ||
            load_param_block_16B(dw_params_blob, dw_d->params_addr, c,
                                 &bias, &mult, &shift) != 0) {
            CPRINT("[CASCADE] DW weight/param load fail ch=%d\n", c);
            return -1;
        }
        dw_write_reg(DWF_REG_CH_ADDR, c);
        dw_write_reg(DWF_REG_W0, dw_pack_w0(w9[0], w9[1], w9[2], w9[3]));
        dw_write_reg(DWF_REG_W1, dw_pack_w1(w9[4], w9[5], w9[6], w9[7]));
        dw_write_reg(DWF_REG_W2, dw_pack_w2(w9[8]));
        dw_write_reg(DWF_REG_BIAS,  (u32)bias);
        dw_write_reg(DWF_REG_MULT,  mult);
        dw_write_reg(DWF_REG_SHIFT, shift);
    }

    /* ---- PW params (addr = oc) then weights (bank = oc%N_OC, batch = oc/N_OC) ---- */
    for (int oc = 0; oc < Cout_pw; oc++) {
        if (load_param_block_16B(pw_params_blob, pw_d->params_addr, oc,
                                 &bias, &mult, &shift) != 0) {
            CPRINT("[CASCADE] PW param load fail oc=%d\n", oc);
            return -1;
        }
        pw_write_reg(PW_REG_PARAM_ADDR, oc);
        pw_write_reg(PW_REG_BIAS,  (u32)bias);
        pw_write_reg(PW_REG_MULT,  mult);
        pw_write_reg(PW_REG_SHIFT, shift);
    }
    for (int oc = 0; oc < Cout_pw; oc++) {
        if (load_pw_weights_for_oc(pw_weights_blob, pw_d->weight_addr, oc,
                                   Cin_pw, w_ic) != 0) {
            CPRINT("[CASCADE] PW weight load fail oc=%d\n", oc);
            return -1;
        }
        pw_write_reg(PW_REG_OC_SEL,      oc % PW_N_OC);
        pw_write_reg(PW_REG_W_BRAM_OFF, (oc / PW_N_OC) * Cin_pw);
        for (int ic = 0; ic < Cin_pw; ic++) pw_write_weight(ic, w_ic[ic]);
    }

    /* ---- runtime config ---- */
    dw_write_reg(DWF_REG_CIN_RUN,   C);
    dw_write_reg(DWF_REG_N_GROUPS,  G);
    dw_write_reg(DWF_REG_IMG_WIDTH, (u32)W);
    dw_write_reg(DWF_REG_N_ROWS,    (u32)H);
    /* ZP_RELU[17] = stride2 (see DESIGN_UNDERSTANDING.md 2A.3). CIN_RUN /
     * N_GROUPS / IMG_WIDTH / N_ROWS above stay the DW INPUT geometry at either
     * stride -- the windower consumes G groups per row and emits G/2. */
    dw_write_reg(DWF_REG_ZP_RELU,
                 pw_pack_zp_relu(dw_d->zp_in, dw_d->zp_out, dw_d->relu_en)
                 | (dw_stride2 ? (1u << 17) : 0u));

    pw_write_reg(PW_REG_TILE_PIXELS, (u32)total_px);
    pw_write_reg(PW_REG_CIN_RUN,     (u32)Cin_pw);
    pw_write_reg(PW_REG_COUT_RUN,    (u32)cout_rounded);
    pw_write_reg(PW_REG_ZP_RELU,
                 pw_pack_zp_relu(pw_d->zp_in, pw_d->zp_out, pw_d->relu_en));

    CPRINT("[CASCADE] DW readback CIN=%u NG=%u IMGW=%u NROWS=%u | "
           "PW TILE_PX=%u CIN=%u COUT=%u\n",
           (unsigned)dw_read_reg(DWF_REG_CIN_RUN),
           (unsigned)dw_read_reg(DWF_REG_N_GROUPS),
           (unsigned)dw_read_reg(DWF_REG_IMG_WIDTH),
           (unsigned)dw_read_reg(DWF_REG_N_ROWS),
           (unsigned)pw_read_reg(PW_REG_TILE_PIXELS),
           (unsigned)pw_read_reg(PW_REG_CIN_RUN),
           (unsigned)pw_read_reg(PW_REG_COUT_RUN));

    t_prog = timer_now();

#if CASCADE_INSTRUMENT
    /* Sentinel-fill the output so a partial drain is measurable.
     * Costly: memset + full-buffer cache flush (6.9 MB on surrogate pair 0). */
    memset(pw_out, CASCADE_SENTINEL, out_bytes);
    Xil_DCacheFlushRange((UINTPTR)pw_out, out_bytes);
#endif

    /* gm_in was just written by the CPU (pack) -- must flush so the MM2S DMA
     * reads real data rather than stale DRAM. Unavoidable WHEN THE CPU WROTE IT.
     *
     * When input_prepacked, the bytes came from the previous pair's S2MM: the
     * CPU never touched them, so there are no dirty lines to write back and
     * nothing to flush. (Stale CLEAN lines are harmless -- MM2S reads DRAM,
     * which S2MM already wrote correctly.) This is the single largest saving
     * from chaining: the flush costs ~3.2 ms/MB because Xil_DCacheFlushRange
     * does L1 AND L2/PL310 maintenance, and each L2 op is a register write
     * plus sync -- ~103 ns per 32-byte line, not memory-bandwidth bound.
     *
     * CAUTION if CASCADE_INSTRUMENT is ever turned on with chaining: the
     * sentinel fill DOES make the CPU write these buffers, and then dirty
     * lines could evict over DMA-written data. Flush would be required again.
     *
     * input_noncached: gm_in is mapped NORM_NONCACHE, so the CPU's writes went
     * straight to DRAM through the write buffer -- there is no cache state to
     * maintain and nothing to flush. */
    if (!input_prepacked && !input_noncached)
        Xil_DCacheFlushRange((UINTPTR)gm_in, in_bytes);

#if CASCADE_INSTRUMENT
    /* Only needed when the CPU has WRITTEN pw_out (the sentinel fill): dirty
     * lines could otherwise evict over DMA-written data. With instrumentation
     * off the CPU never writes pw_out, so there is nothing to write back and
     * this full-buffer invalidate is pure cost -- 6.9 MB on surrogate pair 0,
     * measured at ~24.9 ms, i.e. more than pair 0's entire datapath time. */
    Xil_DCacheInvalidateRange((UINTPTR)pw_out, out_bytes);
#endif
    t_cache = timer_now();

    /* ---- arm the first S2MM chunk before either core runs ---- */
    t_hw0 = timer_now();            /* <-- hardware run starts here */
#if CASCADE_ENGINE_SPLIT
    g_dw_done_seen = g_pw_done_seen = 0;
    g_dw_done_cyc  = g_pw_done_cyc  = t_hw0;
#endif
    size_t armed = (out_bytes > CASCADE_S2MM_CHUNK) ? CASCADE_S2MM_CHUNK : out_bytes;
    if (XAxiDma_SimpleTransfer(&PwDma, (UINTPTR)pw_out, armed,
                               XAXIDMA_DEVICE_TO_DMA) != XST_SUCCESS) {
        CPRINT("[CASCADE] S2MM submit failed (chunk 0)\n");
        return -1;
    }

    /* ---- start PW first (its FIFO reset must precede any DW output) ---- */
    pw_write_reg(PW_REG_CTRL, (1u << 1) | (1u << 2));   /* clear core state */
    pw_write_reg(PW_REG_CTRL, 0);
    pw_write_reg(PW_REG_CTRL, 1u << 0);                 /* start */
    pw_write_reg(PW_REG_CTRL, 0);

    /* ---- then DW ---- */
    dw_write_reg(DWF_REG_CTRL, 1u << 0);
    dw_write_reg(DWF_REG_CTRL, 0);
    CPRINT("[CASCADE] post-start DW STATUS=0x%08X PW STATUS=0x%08X\n",
           (unsigned)dw_read_reg(DWF_REG_STATUS),
           (unsigned)pw_read_reg(PW_REG_STATUS));

    /* ---- finally MM2S, so no data predates the FIFO resets ---- */
    if (XAxiDma_SimpleTransfer(&DwDma, (UINTPTR)gm_in, in_bytes,
                               XAXIDMA_DMA_TO_DEVICE) != XST_SUCCESS) {
        CPRINT("[CASCADE] MM2S submit failed\n");
        return -1;
    }

    /* ---- wait for MM2S, SERVICING S2MM CHUNKS MEANWHILE ----
     * DEADLOCK FIX 2026-07-28. This used to wait for MM2S alone and only re-arm
     * S2MM chunks afterwards. At full scale PW fills the first 32MB chunk long
     * before MM2S has delivered all 8.8MB of input: S2MM then stops accepting,
     * PW's output FIFO fills, PW backpressures DW, DW throttles (correctly, now
     * that prog_full works) and stops consuming -- so MM2S can NEVER finish,
     * while the code that would arm the next chunk sits after the very wait it
     * is blocked in. Symptom: "MM2S timeout" with both cores still busy.
     * Invisible at H<=16, where one chunk covers the whole output. */
    t = 0;
    while (XAxiDma_Busy(&DwDma, XAXIDMA_DMA_TO_DEVICE)) {
        if ((t & ENGINE_POLL_MASK) == 0) ENGINE_POLL();
        if (armed < out_bytes && !XAxiDma_Busy(&PwDma, XAXIDMA_DEVICE_TO_DMA)) {
            size_t rest  = out_bytes - armed;
            size_t chunk = (rest > CASCADE_S2MM_CHUNK) ? CASCADE_S2MM_CHUNK : rest;
            if (XAxiDma_SimpleTransfer(&PwDma, (UINTPTR)(pw_out + armed), chunk,
                                       XAXIDMA_DEVICE_TO_DMA) != XST_SUCCESS) {
                CPRINT("[CASCADE] S2MM submit failed at %lu (during MM2S wait)\n",
                       (unsigned long)armed);
                return -1;
            }
            CPRINT("[CASCADE]   armed S2MM chunk at %lu (+%lu) during MM2S wait\n",
                   (unsigned long)armed, (unsigned long)chunk);
            armed += chunk;
            t = 0;              /* real progress -- restart the patience counter */
        }
        if (++t > 400000000) {
            CPRINT("[CASCADE] MM2S timeout. DW STATUS=0x%08X PW STATUS=0x%08X "
                   "STATUS2=0x%08X armed=%lu/%lu\n",
                   (unsigned)dw_read_reg(DWF_REG_STATUS),
                   (unsigned)pw_read_reg(PW_REG_STATUS),
                   (unsigned)pw_read_reg(PW_REG_STATUS2),
                   (unsigned long)armed, (unsigned long)out_bytes);
            /* Full state on the MM2S-timeout path too (2026-07-28): chunk 0
             * filled and chunk 1 was armed, yet NOTHING landed in chunk 1 --
             * PW stopped sending at the chunk boundary. Need the S2MM
             * registers (did the re-armed transfer actually start?) and DW's
             * counters (is DW still feeding, or has it been throttled to a
             * halt behind a wedged PW?) to tell those apart. */
            CPRINT("[CASCADE]   DW dbg consumed=%u written=%u produced=%u (of %u)\n",
                   (unsigned)dw_read_reg(DWF_REG_DBG_CONSUMED),
                   (unsigned)dw_read_reg(DWF_REG_DBG_WRITTEN),
                   (unsigned)dw_read_reg(DWF_REG_DBG_PRODUCED),
                   (unsigned)((size_t)C * (size_t)G * (size_t)H));
            CPRINT("[CASCADE]   DW winstate=0x%08X flags=0x%08X\n",
                   (unsigned)dw_read_reg(DWF_REG_DBG_WINSTATE),
                   (unsigned)dw_read_reg(DWF_REG_DBG_FLAGS));
            cascade_dump_dma(&PwDma, "PW");
            cascade_report_landed(pw_out, out_bytes);
            return -1;
        }
    }
    CPRINT("[CASCADE] MM2S complete (%lu B in)\n", (unsigned long)in_bytes);

    /* ---- drain S2MM, re-arming until the whole output has landed ----
     * The wait samples the output frontier periodically instead of using a bare
     * poll count. Two consecutive identical frontiers = genuinely stalled; a
     * rising frontier = merely slow, and it keeps waiting. That separation is
     * the whole point: the previous fixed-count timeout could not tell them
     * apart, and the two have opposite fixes. */
    {
        u64_cycles t_drain0 = timer_now();
        while (1) {
            unsigned long spins = 0;
#if CASCADE_INSTRUMENT
            size_t prev_front = cascade_frontier(pw_out, out_bytes);
            int    stuck_rounds = 0;
#endif

            while (XAxiDma_Busy(&PwDma, XAXIDMA_DEVICE_TO_DMA)) {
                if ((spins & ENGINE_POLL_MASK) == 0) ENGINE_POLL();
#if !CASCADE_INSTRUMENT
                /* measurement mode: plain timeout, no buffer scanning */
                if (++spins > 400000000ul) {
                    CPRINT("[CASCADE] S2MM timeout (armed=%lu/%lu)\n",
                           (unsigned long)armed, (unsigned long)out_bytes);
                    cascade_dump_dma(&PwDma, "PW");
                    return -1;
                }
                continue;
#else
                if (++spins < 20000000ul) continue;
                spins = 0;
                {
                    size_t f = cascade_frontier(pw_out, out_bytes);
                    CPRINT("[CASCADE]   drain frontier=%lu/%lu B (%lu beats)%s\n",
                           (unsigned long)f, (unsigned long)out_bytes,
                           (unsigned long)(f / 8),
                           (f == prev_front) ? "  <-- NO PROGRESS" : "");
                    if (f == prev_front) {
                        if (++stuck_rounds >= 3) {
                            CPRINT("[CASCADE] STALLED: frontier frozen at %lu B across "
                                   "3 samples. armed=%lu/%lu DW=0x%08X PW=0x%08X STATUS2=0x%08X\n",
                                   (unsigned long)f, (unsigned long)armed,
                                   (unsigned long)out_bytes,
                                   (unsigned)dw_read_reg(DWF_REG_STATUS),
                                   (unsigned)pw_read_reg(PW_REG_STATUS),
                                   (unsigned)pw_read_reg(PW_REG_STATUS2));
                            /* DW's counters in the STALL path (2026-07-28).
                             * Behavioural sim of pw_single_oc_axis PASSES at the
                             * exact config that stalls here (H=16, 86,400 beats,
                             * grp_idx reaching tile_groups, TLAST clean) when fed
                             * the right NUMBER of input words. So PW's internals
                             * are not the problem -- its input is the open
                             * question, and it has never been measured on a
                             * stalling run. PW needs tile_groups*cin_run words;
                             * if DW emitted fewer, PW starves mid-group and every
                             * observation so far follows. */
                            {
                                u32 dc = dw_read_reg(DWF_REG_DBG_CONSUMED);
                                u32 dwr = dw_read_reg(DWF_REG_DBG_WRITTEN);
                                u32 dp = dw_read_reg(DWF_REG_DBG_PRODUCED);
                                u32 need = (u32)((size_t)C * (size_t)G * (size_t)H);
                                CPRINT("[CASCADE]   DW dbg consumed=%u written=%u produced=%u"
                                       " (DW should emit %u; PW needs %u)\n",
                                       (unsigned)dc, (unsigned)dwr, (unsigned)dp,
                                       (unsigned)need,
                                       (unsigned)((total_px / 8u) * (u32)Cin_pw));
                                CPRINT("[CASCADE]   DW winstate=0x%08X flags=0x%08X\n",
                                       (unsigned)dw_read_reg(DWF_REG_DBG_WINSTATE),
                                       (unsigned)dw_read_reg(DWF_REG_DBG_FLAGS));
                            }
                            cascade_dump_dma(&PwDma, "PW");
                            cascade_report_landed(pw_out, out_bytes);
                            return -1;
                        }
                    } else {
                        stuck_rounds = 0;
                        prev_front = f;
                    }
                }
#endif  /* CASCADE_INSTRUMENT */
            }
            CPRINT("[CASCADE]   chunk done (armed=%lu/%lu B, %lu cyc)\n",
                   (unsigned long)armed, (unsigned long)out_bytes,
                   (unsigned long)(timer_now() - t_drain0));
            if (armed >= out_bytes) break;
            {
                size_t rest  = out_bytes - armed;
                size_t chunk = (rest > CASCADE_S2MM_CHUNK) ? CASCADE_S2MM_CHUNK : rest;
                if (XAxiDma_SimpleTransfer(&PwDma, (UINTPTR)(pw_out + armed), chunk,
                                           XAXIDMA_DEVICE_TO_DMA) != XST_SUCCESS) {
                    CPRINT("[CASCADE] S2MM submit failed at offset %lu\n",
                           (unsigned long)armed);
                    return -1;
                }
                armed += chunk;
            }
        }   /* while (1) */
    }       /* drain block */

    t_end = timer_now();            /* <-- hardware run ends here */

    /* Invalidate only what the CPU actually reads, not the whole output.
     * The DMA wrote pw_out in DRAM; any cached lines from a previous frame are
     * stale, so an invalidate IS required before reading -- but only over the
     * bytes read. Full-buffer invalidation of 6.9 MB was costing more than the
     * datapath itself.
     *
     * NOTE ON GENERALITY: this is legitimate here AND representative of a real
     * pipeline whose next consumer is hardware (a DMA reads DRAM directly and
     * needs no CPU cache maintenance at all). It would NOT be legitimate if the
     * CPU had to read the whole tensor -- e.g. to strip PW's padding channels
     * between pairs -- in which case that cost comes back. */
    Xil_DCacheInvalidateRange((UINTPTR)pw_out, 64);

#if CASCADE_ENGINE_SPLIT
    /* Final sample: if a core finished between the last poll and t_end, catch
     * it now rather than reporting a stale timestamp. */
    ENGINE_POLL();
    if (g_cascade_accum && g_acc_pair >= 0 && g_acc_pair < SURR_MAX_PAIRS) {
        g_acc_dwbusy_p[g_acc_pair] += g_dw_done_cyc - t_hw0;
        g_acc_pwbusy_p[g_acc_pair] += g_pw_done_cyc - t_hw0;
    }
#endif

    /* Fusion-traffic capture. Deliberately OUTSIDE the g_cascade_accum block so
     * the dedicated capture pass does not also pollute the phase accumulators,
     * and after t_end so the AXI-Lite reads cannot land inside any bracket. */
    if (g_capture_beats && g_acc_pair >= 0 && g_acc_pair < SURR_MAX_PAIRS) {
        g_dw_consumed_p[g_acc_pair] = dw_read_reg(DWF_REG_DBG_CONSUMED);
        g_dw_written_p [g_acc_pair] = dw_read_reg(DWF_REG_DBG_WRITTEN);
        g_dw_produced_p[g_acc_pair] = dw_read_reg(DWF_REG_DBG_PRODUCED);
    }

    if (g_cascade_accum) {
        g_acc_pack  += t_pack  - t_fn0;
        g_acc_prog  += t_prog  - t_pack;
        g_acc_cache += t_cache - t_prog;
        g_acc_hw    += t_end   - t_hw0;
        g_acc_total += t_end   - t_fn0;
        if (g_acc_pair >= 0 && g_acc_pair < SURR_MAX_PAIRS) {
            g_acc_hw_p   [g_acc_pair] += t_end   - t_hw0;
            g_acc_pack_p [g_acc_pair] += t_pack  - t_fn0;
            g_acc_prog_p [g_acc_pair] += t_prog  - t_pack;
            g_acc_cache_p[g_acc_pair] += t_cache - t_prog;
        }
    }

    {
        double ms_pack  = cycles_to_ms(t_pack  - t_fn0);
        double ms_prog  = cycles_to_ms(t_prog  - t_pack);
        double ms_cache = cycles_to_ms(t_cache - t_prog);
        double ms_hw    = cycles_to_ms(t_end   - t_hw0);
        double ms_total = cycles_to_ms(t_end   - t_fn0);
        CPRINT("[CASCADE] TIME pack=%.2f prog=%.2f cache=%.2f  **HW=%.2f**  total=%.2f ms\n",
               ms_pack, ms_prog, ms_cache, ms_hw, ms_total);
        CPRINT("[CASCADE]      HW = cores started -> last byte landed "
               "(%lu out beats, %.2f beats/PL-cycle @100MHz)\n",
               (unsigned long)(out_bytes / 8),
               (ms_hw > 0.0) ? ((double)(out_bytes / 8) / (ms_hw * 100000.0)) : 0.0);
    }

    CPRINT("[CASCADE] complete. DW STATUS=0x%08X PW STATUS=0x%08X\n",
           (unsigned)dw_read_reg(DWF_REG_STATUS),
           (unsigned)pw_read_reg(PW_REG_STATUS));
    /* At stride 2 the DW consumes 4x what it emits, so "expect N each" is no
     * longer right: consumed = H*G*C (input beats), written = produced =
     * (H/2)*(G/2)*C (dense output beats). Print both so a correct stride-2 run
     * is not mistaken for the truncation failure mode we chased on 07-30. */
    CPRINT("[CASCADE] DW dbg consumed=%u (expect %lu)  written=%u produced=%u (expect %lu)\n",
           (unsigned)dw_read_reg(DWF_REG_DBG_CONSUMED),
           (unsigned long)((size_t)C * (size_t)G * (size_t)H),
           (unsigned)dw_read_reg(DWF_REG_DBG_WRITTEN),
           (unsigned)dw_read_reg(DWF_REG_DBG_PRODUCED),
           (unsigned long)((size_t)C * (size_t)(dw_stride2 ? G / 2 : G)
                                     * (size_t)(dw_stride2 ? (H + 1) / 2 : H)));
#if CASCADE_INSTRUMENT
    cascade_report_landed(pw_out, out_bytes);   /* byte-scan of the whole output */
#endif
    /* The hex bytes must be suppressed too -- the CPRINT conversion matched only
     * lines starting "[CASCADE]", so this loop kept printing inside the timed
     * region: ~49 chars x 3 pairs per frame, ~9 ms of blocking UART at the
     * ~64 us/char this console runs at. */
    if (!g_cascade_quiet) {
        printf("[CASCADE] PW out[0..15]:");
        for (int k = 0; k < 16; k++) printf(" %02X", pw_out[k]);
        printf("\n");
    }
    return 0;
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
         * order) ??? only the -8 column shift should remain. */
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
                printf("   (NONE ??? output is uniformly background; impulse never reached the core)\n");
            else
                printf("   total non-bg reported: %d (cap 48)\n", found);

            /* Re-read the impulse byte straight from DRAM (invalidate its line
             * first) to confirm the DMA actually saw 255 ??? i.e. that the flush
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
            /* right edge of row 0 (decimal) ??? compare to golden l00 */
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

#if DWF_CASCADE_TEST
    /* Stage 2 fusion: on-chip DW(L00) -> PW(L01), no DRAM round trip.
     * skips[1] holds the group-major DW input (8.4MB of 96MB);
     * skips[0] holds the PW output (84.4MB of 176MB). */
    {
        layer_desc_t csc_dw = descs[0];
        layer_desc_t csc_pw = descs[1];
#if CASCADE_H_OVERRIDE
        printf("[CASCADE] CASCADE_H_OVERRIDE set -- using H=%d instead of real H=%d "
               "(pipeline-drain test; output values are meaningless)\n",
               CASCADE_H_OVERRIDE, csc_dw.H);
        csc_dw.H = CASCADE_H_OVERRIDE;
        csc_pw.H = CASCADE_H_OVERRIDE;
#endif
        /* real network: one blob pair serves both layers, distinguished by
         * weight_addr/params_addr in the descriptors */
        int csc_rc = hw_dw_pw_cascade_l0_l1(&csc_dw, &csc_pw, buf_next,
                                            skips[1].buf, skips[0].buf,
                                            &params_blob, &weights_blob,
                                            &params_blob, &weights_blob,
                                            0u /* in_flags: standalone test packs its own, cached buffer */);
        printf("[CASCADE] returned %d (0=ok, <0=error)\n", csc_rc);
    }
#endif

#if DWF_L0_LOOPBACK_TEST
    /* Stage 1 fusion validation -- see DWF_L0_LOOPBACK_TEST's definition and
     * hw_dw_fused_l0_loopback_test()'s header comment. skips[0]/skips[1] are
     * unused this early in boot (nothing has run yet to populate a skip
     * connection), so they're borrowed as scratch DRAM for the group-major
     * in/out buffers -- both are 16MB+, far larger than one L0 tensor. */
    {
        layer_desc_t dwf_desc = descs[0];
#if DWF_L0_TEST_H_OVERRIDE
        printf("[DWF-L0] DWF_L0_TEST_H_OVERRIDE set -- using H=%d instead of real H=%d "
               "(diagnostic run, expect compare to FAIL even if hardware behaves correctly)\n",
               DWF_L0_TEST_H_OVERRIDE, dwf_desc.H);
        dwf_desc.H = DWF_L0_TEST_H_OVERRIDE;
#endif
        int dwf_rc = hw_dw_fused_l0_loopback_test(&dwf_desc, buf_next,
                                                  skips[0].buf, skips[1].buf,
                                                  &params_blob, &weights_blob, cfg);
        printf("[DWF-L0] test returned %d (0=pass, >0=mismatches, <0=setup error)\n", dwf_rc);
    }
#endif

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
        printf(" reference-compare I/O ??? this is real deployed-encoder wall time)\n");
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
        printf("[TOTAL] achieved thr., DMA-only: %.3f GMACs/s  (x2 for GFLOPs/s ??? compare against 60.5 GOP/s here)\n",
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
 * DW/PW surrogate estimate for an unrelated dense-conv encoder
 *
 * Target model (not this project's network):
 *   encoder.encoder.0  Conv2d(3,8,k3)   + BN(8)
 *   encoder.encoder.3  Conv2d(8,16,k3)  + BN(16)
 *   encoder.encoder.6  Conv2d(16,64,k3) + bias, no BN
 *
 * Every one of those is a DENSE 3x3 conv (mixes all input channels per
 * output channel) -- not depthwise, not 1x1. Neither DW (per-channel only,
 * no cross-channel mixing) nor PW (1x1 only, no spatial extent) can run
 * that natively. This function is a SURROGATE: each dense stage is
 * approximated as DW(Cin) -> PW(Cin->Cout), the same low-rank decomposition
 * MobileNet uses, so it becomes six real layers the existing accelerators
 * can execute, giving a real board-measured cycle count for a same-shaped
 * separable stand-in. It is NOT a bit-exact reproduction of the target
 * model's math (BN folding, bias, and actual trained weights are all
 * ignored) -- only a hardware run-time estimate.
 *
 * Each of the target model's three conv stages does stride-2 downsampling.
 * This hardware's PW engine is 1x1/stride-1 only (no stride register) --
 * spatial reduction can only happen in DW -- so the DW half of every
 * surrogate pair carries stride=2/pad=1/kernel=3, and the PW half that
 * follows runs at the already-downsampled resolution as stride=1/kernel=1.
 * Starting resolution assumed 720p (1280x720); edit SURR_IN_H/W below if
 * that's wrong. Weights/params are synthetic dummy values (no SD card
 * needed) -- this hardware's timing depends only on tensor shapes and
 * DMA/compute rates, never on data values, so dummy weights give the same
 * cycle counts real trained weights would.
 * ============================================================ */
/* SURR_MODEL_ENCODER
 *   0 = legacy 3-pair surrogate (3->8, 8->16, 16->64). Worst G*C = 640, so it
 *       fits MAX_CG_PRODUCT=1024 and runs on the CURRENT fabric. Use this to
 *       validate the cost model against the already-measured 15.44 ms HW.
 *   1 = ImageEncoderLite, the real encoder. Worst G*C = 1280, so it REQUIRES
 *       the MAX_CG_PRODUCT=2048 rebuild. Running it on a 1024 fabric silently
 *       aliases the line buffers in blocks 1, 2 and 5 -- no error, wrong data. */
#define SURR_MODEL_ENCODER 1

#if SURR_MODEL_ENCODER
#  define SURR_NUM_LAYERS 12
#else
#  define SURR_NUM_LAYERS 6
#endif
#define SURR_IN_H        720
#define SURR_IN_W       1280

static void surr_build_schedule(layer_desc_t descs[SURR_NUM_LAYERS])
{
    /* ImageEncoderLite (NeuralImageCodec encoder), 6 depthwise-separable
     * blocks. Each block is DW(3x3, groups=C) -> BN -> ReLU6 -> PW(1x1) -> BN
     * -> ReLU6, except block 5 whose final PW ends at BN with NO activation.
     *
     *   block        0     1     2     3     4     5
     *   DW  C        3    16    32    32    32    64
     *   DW  stride   2     2     2     1     1     1
     *   PW  Cout    16    32    32    32    64    64
     *
     * Resolutions from 720x1280 (stride-2 in blocks 0-2 only):
     *   b0 DW 720x1280 -> 360x640 ; PW  3->16 @ 360x640
     *   b1 DW 360x640  -> 180x320 ; PW 16->32 @ 180x320
     *   b2 DW 180x320  ->  90x160 ; PW 32->32 @  90x160
     *   b3 DW  90x160  ->  90x160 ; PW 32->32 @  90x160
     *   b4 DW  90x160  ->  90x160 ; PW 32->64 @  90x160
     *   b5 DW  90x160  ->  90x160 ; PW 64->64 @  90x160
     *
     * HARDWARE CONSTRAINTS CHECKED:
     *  - stride-2 needs EVEN n_groups and cin_run>=2: b0 G=160/C=3,
     *    b1 G=80/C=16, b2 G=40/C=32. All satisfied.
     *  - MAX_CG_PRODUCT must be >= max(G*C) over DW layers = 1280 (b1, b2, b5).
     *    dw_banked_window_8x.sv was raised 1024 -> 2048 for this. REBUILD RTL.
     *  - every Cout is a multiple of PW_N_OC=8, so cout_rounded == Cout: no
     *    padding waste and the chained hand-off needs no repack.
     *  - CIN_MAX=240 covers the max C of 64.
     *  - chaining: PW Cout must equal the next DW's Cin -- 16,32,32,32,64 all
     *    line up, and the byte counts match exactly at every boundary.
     *
     * NOT MODELLED: the activation is ReLU6, the PPU implements plain ReLU
     * (clamp at zp_out with no upper bound). ReLU6's ceiling needs an upper
     * clamp at the quantised value of 6.0. This does not affect TIMING, which
     * is what this harness measures, but it is a real fidelity gap for
     * correctness work. BatchNorm folds into the conv weights/bias at
     * quantisation time and needs no hardware support. */
#if SURR_MODEL_ENCODER
    static const int dw_c   [SURR_NUM_LAYERS / 2] = {  3, 16, 32, 32, 32, 64 };
    static const int pw_cout[SURR_NUM_LAYERS / 2] = { 16, 32, 32, 32, 64, 64 };
    static const int dw_str [SURR_NUM_LAYERS / 2] = {  2,  2,  2,  1,  1,  1 };
#else
    /* Legacy 3-pair surrogate: the configuration every measurement up to
     * 2026-07-30 was taken on. Kept so the cost model can be checked against
     * a known result (HW = 15.44 ms) on the current fabric. */
    static const int dw_c   [SURR_NUM_LAYERS / 2] = {  3,  8, 16 };
    static const int pw_cout[SURR_NUM_LAYERS / 2] = {  8, 16, 64 };
    static const int dw_str [SURR_NUM_LAYERS / 2] = {  2,  2,  2 };
#endif

    int H = SURR_IN_H, W = SURR_IN_W;

    memset(descs, 0, SURR_NUM_LAYERS * sizeof(descs[0]));

    for (int b = 0; b < SURR_NUM_LAYERS / 2; b++) {
        layer_desc_t *dw = &descs[2 * b];
        layer_desc_t *pw = &descs[2 * b + 1];

        /* ---- depthwise ---- */
        dw->layer_idx = 2 * b;
        dw->kind      = KIND_DW;
        dw->Cin       = dw_c[b];
        dw->Cout      = dw_c[b];
        dw->H         = H;
        dw->W         = W;
        dw->stride    = dw_str[b];
        dw->pad       = 1;
        dw->kernel    = 3;
        dw->zp_in     = 128;
        dw->zp_out    = 128;
        dw->relu_en   = 1;          /* ReLU6 -- see caveat above */

        H = calc_out_dim(H, dw->pad, dw->kernel, dw->stride);
        W = calc_out_dim(W, dw->pad, dw->kernel, dw->stride);

        /* ---- pointwise, at the DW's OUTPUT resolution ---- */
        pw->layer_idx = 2 * b + 1;
        pw->kind      = KIND_PW;
        pw->Cin       = dw_c[b];
        pw->Cout      = pw_cout[b];
        pw->H         = H;
        pw->W         = W;
        pw->stride    = 1;
        pw->pad       = 0;
        pw->kernel    = 1;
        pw->zp_in     = 128;
        pw->zp_out    = 128;
        /* block 5's PW is the encoder output: BatchNorm only, no activation */
        pw->relu_en   = (b == (SURR_NUM_LAYERS / 2) - 1) ? 0 : 1;
    }
}

/* Dummy param block: bias=0, mult=0x00010000 (65536), shift=16 -- a
 * pass-through-scale quantization config, arbitrary but well-formed. */
static void surr_fill_dummy_params(uint8_t *blk16)
{
    blk16[0] = 0; blk16[1] = 0; blk16[2] = 0; blk16[3] = 0;       /* bias  (LE i32) */
    blk16[4] = 0x00; blk16[5] = 0x00; blk16[6] = 0x01; blk16[7] = 0x00; /* mult (LE u32) */
    blk16[8] = 16;                                                 /* shift */
    blk16[9] = blk16[10] = blk16[11] = 0;
}

static int surr_alloc_dw_blobs(int Cin, file_blob_t *w, file_blob_t *p)
{
    w->size = (size_t)Cin * 9u;
    w->data = (uint8_t *)malloc(w->size);
    p->size = (size_t)Cin * 16u;
    p->data = (uint8_t *)malloc(p->size);
    if (!w->data || !p->data) return -1;

    memset(w->data, 1, w->size); /* every 3x3 tap = 1 */
    for (int c = 0; c < Cin; c++) {
        surr_fill_dummy_params(p->data + (size_t)c * 16u);
    }
    return 0;
}

static int surr_alloc_pw_blobs(int Cin, int Cout, file_blob_t *w, file_blob_t *p)
{
    w->size = (size_t)Cout * (size_t)Cin;
    w->data = (uint8_t *)malloc(w->size);
    p->size = (size_t)Cout * 16u;
    p->data = (uint8_t *)malloc(p->size);
    if (!w->data || !p->data) return -1;

    memset(w->data, 1, w->size); /* every ic weight = 1 */
    for (int oc = 0; oc < Cout; oc++) {
        surr_fill_dummy_params(p->data + (size_t)oc * 16u);
    }
    return 0;
}

/* Time one DW layer on the FUSED DW engine, via a DRAM loopback.
 *
 * The surrogate's own path calls run_dw_layer_real() -> hw_dw_run_plane(), which
 * targets DW_conv_accel_0 -- deleted from the BD 2026-07-09. This replaces it.
 *
 * COST PROXY, not a functional run: the surrogate's DW layers are all stride-2
 * and dw_banked_window_8x is stride-1 only (see its SCOPE comment, line ~51), so
 * this executes the same layer at stride 1. The windower's work is driven by the
 * INPUT it consumes (C*G*H group-beats), which is identical either way -- only
 * the emitted row count differs -- so the measured cycles are a good estimate of
 * the stride-2 cost, and if anything an over-estimate on the output side.
 *
 * Buffer contents are irrelevant: this hardware's timing depends only on tensor
 * shape and DMA/compute rates, never on data values. Requires LOOPBACK BD mode
 * (BD script F2) -- axi_dma_0 needs its S2MM channel back.
 */
/* Per-layer measured cycles for the surrogate end-to-end estimate. */
#if RUN_POWER_MEASUREMENT
/* One complete encoder frame, identical to the timed loop's body. */
static int surr_run_one_frame(const layer_desc_t *descs,
                              file_blob_t *w_blob, file_blob_t *p_blob,
                              uint8_t *raw_in, uint8_t *gm_in,
                              uint8_t *chainA, uint8_t *chainB, int npairs)
{
    for (int p = 0; p < npairs; p++) {
        layer_desc_t dw = descs[2 * p], pw = descs[2 * p + 1];
        const int chained = SURR_CHAIN_PAIRS && (p > 0);
        uint8_t  *out_buf = (p % 2 == 0) ? chainA : chainB;
        uint8_t  *in_buf  = (p == 0) ? gm_in : ((p % 2 == 1) ? chainA : chainB);
        if (hw_dw_pw_cascade_l0_l1(&dw, &pw, raw_in, in_buf, out_buf,
                                   &p_blob[2 * p],     &w_blob[2 * p],
                                   &p_blob[2 * p + 1], &w_blob[2 * p + 1],
                                   SURR_IN_FLAGS(chained)) != 0) return -1;
    }
    return 0;
}

/* ==================================================================
 * DIAGNOSTICS -- run BEFORE the A/B, because they decide whether the
 * A/B can possibly work.
 *
 * The 8-cycle interleaved run (2026-08-05) killed the drift -- deltas
 * stopped being systematically negative -- but every rail still came
 * back "below noise". Two facts from that run frame these tests.
 *
 *  1. It is NOT quantisation-limited. Per-sample sd was 60-120x the
 *     Linear11 current LSB (VCCINT: sd 28 mA vs LSB 0.24 mA). So the
 *     signal is not hiding under one code, and averaging is still the
 *     right lever -- PROVIDED the noise is white.
 *
 *  2. The sd was ~15-30 mA on EVERY rail once converted to current,
 *     independent of that rail's voltage, its load, and whether its
 *     load changed at all (VCCBRAM 9 mA, VCCINT 28 mA, VCC3V3 25 mA,
 *     VCCAUX 22 mA -- on a rail whose sd EXCEEDS its mean). Scatter
 *     that is constant in the current domain across ten unrelated
 *     regulators is a property of the measurement, not of the board.
 *
 * Fact 2 has two explanations that demand opposite responses:
 *   (a) real current-sense noise in the UCD9248 -- white, so more
 *       samples buy sqrt(n) and a long run resolves the delta;
 *   (b) our own I2C sequence. Each read writes PAGE, then reads
 *       VOUT_MODE / READ_VOUT / READ_IOUT. If a PAGE write has not
 *       taken effect when the reads land we silently sample a
 *       DIFFERENT rail on that controller. That gives large,
 *       multi-modal, non-Gaussian scatter, and -- fatally -- it mixes
 *       rails, dragging every delta toward zero. Averaging makes it
 *       WORSE, not better, because it converges on the mixture.
 *
 * A third possibility is independent of both: the UCD9248 averages its
 * telemetry internally. If that time constant is seconds, a 5 s phase
 * never separates and both phases read a blend.
 *
 * And one more, already half-answered: the "idle" phase calls
 * usleep(), which on the standalone A9 BSP is a POLLED delay -- the
 * CPU spins at full tilt. So the PS rails are not expected to move at
 * all, and a PS delta of zero is a correct result rather than a
 * failure. Only PL/DDR can legitimately show the workload.
 * ================================================================== */

#define PM_DIAG_N 128
static uint16_t pm_diag_raw[PM_DIAG_N];
static double   pm_diag_i[PM_DIAG_N];

static int pm_rd_iout_raw(uint8_t slave, uint16_t *raw)
{
    uint8_t rx[2];
    int s = pm_rd(slave, PM_READ_IOUT, rx, 2);
    if (s != XST_SUCCESS) return s;
    *raw = (uint16_t)(((uint16_t)rx[1] << 8) | rx[0]);
    return XST_SUCCESS;
}

/* One noise pass over a single rail. rewrite_page=0 sets PAGE once and
 * then hammers READ_IOUT; rewrite_page=1 repeats the full production
 * sequence. If (1) is much noisier than (0), hypothesis (b) is proven
 * and the fix is in our driver, not in the averaging. */
static void pm_diag_noise_pass(const pm_rail_t *r, int rewrite_page)
{
    double sum = 0.0, sum2 = 0.0;
    uint16_t lo = 0xFFFFU, hi = 0U;
    int distinct = 0, nok = 0, e = 0;
    uint8_t mode;

    if (!rewrite_page) {
        if (pm_wr_byte(r->addr, PM_PAGE, r->page) != XST_SUCCESS) return;
    }
    for (int k = 0; k < PM_DIAG_N; k++) {
        uint16_t raw;
        if (rewrite_page) {
            if (pm_wr_byte(r->addr, PM_PAGE, r->page) != XST_SUCCESS) continue;
            if (pm_rd(r->addr, PM_VOUT_MODE, &mode, 1) != XST_SUCCESS) continue;
        }
        if (pm_rd_iout_raw(r->addr, &raw) != XST_SUCCESS) continue;
        pm_diag_raw[nok++] = raw;
        if (raw < lo) lo = raw;
        if (raw > hi) hi = raw;
    }
    if (nok < 8) { printf("[PWRDIAG]   read failures\n"); return; }

    for (int k = 0; k < nok; k++) {
        double i_a;
        e   = (int)pm_sext((uint16_t)((pm_diag_raw[k] >> 11) & 0x1FU), 5);
        i_a = (double)pm_pow2((float)pm_sext(pm_diag_raw[k] & 0x07FFU, 11), e);
        pm_diag_i[k] = i_a;
        sum += i_a; sum2 += i_a * i_a;
    }
    /* The TRUE resolution is the smallest gap between distinct decoded
     * readings, NOT the Linear11 exponent's LSB. Those differ by 64x on
     * this board -- the format advertises 0.244 mA while the converter
     * only ever emits multiples of 1/64 A. Reporting the format LSB is
     * what made the 8-cycle run look like it was merely variance-limited. */
    {
        double gap = 1.0e9;
        for (int k = 0; k < nok; k++) {
            int seen = 0;
            for (int j = 0; j < k; j++) {
                if (pm_diag_raw[j] == pm_diag_raw[k]) { seen = 1; }
                else {
                    double d = pm_diag_i[j] - pm_diag_i[k];
                    if (d < 0.0) d = -d;
                    if (d > 0.0 && d < gap) gap = d;
                }
            }
            if (!seen) distinct++;
        }
        {
            double m = sum / (double)nok;
            double v = sum2 / (double)nok - m * m;
            double s = (v > 0.0) ? (double)pm_sqrtf((float)v) : 0.0;
            printf("[PWRDIAG]   PAGE %-9s n=%3d  mean %7.1f mA  sd %6.1f mA"
                   "  codes %u..%u  distinct %d  fmt-lsb %.3f mA"
                   "  TRUE step %.2f mA\n",
                   rewrite_page ? "each read" : "once", nok,
                   m * 1000.0, s * 1000.0,
                   (unsigned)lo, (unsigned)hi, distinct,
                   (double)pm_pow2(1.0f, e) * 1000.0,
                   (gap < 1.0e8) ? gap * 1000.0 : 0.0);
        }
    }
}

__attribute__((unused))
static void pm_diag_noise(void)
{
    printf("\n[PWRDIAG] 1. noise character (is the scatter ours or the regulator's?)\n");
    for (unsigned r = 0; r < PM_NRAILS; r++) {
        if (pm_rails[r].group != PM_PL && pm_rails[r].group != PM_DDR) continue;
        printf("[PWRDIAG]  %s\n", pm_rails[r].name);
        pm_diag_noise_pass(&pm_rails[r], 0);
        pm_diag_noise_pass(&pm_rails[r], 1);
    }
    printf("[PWRDIAG]  read: 'distinct' near 1-3 = a stable reading dithering over\n"
           "[PWRDIAG]   adjacent codes (white, averaging works). A wide spread of\n"
           "[PWRDIAG]   codes that SHRINKS when PAGE is written once = we were\n"
           "[PWRDIAG]   sampling the wrong rail part of the time.\n");
}

/* 2. Step response. Does a rail actually MOVE when the accelerator
 * starts, and how long does the telemetry take to get there? If the
 * UCD9248 filters over seconds, no 5 s phase can separate idle from
 * active no matter how many cycles we run. */
__attribute__((unused))
static void pm_diag_step(const layer_desc_t *descs,
                         file_blob_t *w_blob, file_blob_t *p_blob,
                         uint8_t *raw_in, uint8_t *gm_in,
                         uint8_t *chainA, uint8_t *chainB, int npairs)
{
    pm_sample_t s[PM_NRAILS];
    u64_cycles t0 = timer_now(), tlast = t0;
    int fired = 0;

    printf("\n[PWRDIAG] 2. step response: 2 s idle, then 8 s active\n");
    printf("[PWRDIAG]   t(ms)  VCCINT(PL)  VCCBRAM  VCC1V5PS(DDR)   TOTAL\n");
    for (;;) {
        double t = cycles_to_ms(timer_now() - t0);
        if (t >= 10000.0) break;
        if (t < 2000.0) {
            usleep(5000);
        } else {
            if (surr_run_one_frame(descs, w_blob, p_blob, raw_in, gm_in,
                                   chainA, chainB, npairs) != 0) return;
            if (!fired) { printf("[PWRDIAG]   ---- accelerator starts here ----\n"); fired = 1; }
        }
        if (cycles_to_ms(timer_now() - tlast) < 150.0) continue;
        tlast = timer_now();
        if (pm_scan(s) != XST_SUCCESS) continue;
        printf("[PWRDIAG]  %6.0f  %10.4f %8.4f %13.4f %7.4f\n",
               t, s[0].p, s[7].p, s[5].p, pm_total_w(s));
    }
}

/* 3. POSITIVE CONTROL. Before believing "accelerator power < 20 mW",
 * prove the instrument can see a load it definitely should see. A
 * back-to-back memcpy far larger than the 512 KB L2 saturates the DDR
 * controller in both directions -- that is worth well over 100 mW on
 * VCC1V5PS. If THIS comes back "below noise", the measurement is
 * blind and the accelerator bound is meaningless. */
__attribute__((unused))
static void pm_diag_control(uint8_t *a, uint8_t *b)
{
    const unsigned BURN = 2U * 1024U * 1024U;
    pm_sample_t now[PM_NRAILS];
    double sp[2][PM_NRAILS], sp2[2][PM_NRAILS];
    unsigned long n[2] = { 0, 0 };

    printf("\n[PWRDIAG] 3. positive control: idle vs saturating DDR memcpy\n");
    for (int ph = 0; ph < 2; ph++)
        for (unsigned r = 0; r < PM_NRAILS; r++) sp[ph][r] = sp2[ph][r] = 0.0;

    for (unsigned c = 0; c < 4U; c++) {
        for (int ph = 0; ph < 2; ph++) {
            u64_cycles p0 = timer_now(), tl = p0;
            while (cycles_to_ms(timer_now() - p0) < 2500.0) {
                if (ph) memcpy(a, b, BURN); else usleep(5000);
                if (cycles_to_ms(timer_now() - p0) < 800.0) continue;
                if (cycles_to_ms(timer_now() - tl) < 120.0) continue;
                tl = timer_now();
                if (pm_scan(now) != XST_SUCCESS) continue;
                for (unsigned r = 0; r < PM_NRAILS; r++) {
                    sp[ph][r] += now[r].p; sp2[ph][r] += now[r].p * now[r].p;
                }
                n[ph]++;
            }
        }
        printf("%c", 'c');
    }
    printf("\n[PWRDIAG]   samples %lu/%lu\n", n[0], n[1]);
    if (n[0] < 8 || n[1] < 8) { printf("[PWRDIAG]   too few\n"); return; }
    {
        double ti = 0.0, ta = 0.0, tv = 0.0;
        printf("[PWRDIAG]   rail          idle    memcpy     delta      +-SE\n");
        for (unsigned r = 0; r < PM_NRAILS; r++) {
            double m0 = sp[0][r] / n[0], m1 = sp[1][r] / n[1];
            double v0 = sp2[0][r] / n[0] - m0 * m0;
            double v1 = sp2[1][r] / n[1] - m1 * m1;
            double se;
            if (v0 < 0.0) v0 = 0.0;
            if (v1 < 0.0) v1 = 0.0;
            se = (double)pm_sqrtf((float)(v0 / n[0] + v1 / n[1]));
            ti += m0; ta += m1; tv += se * se;
            printf("[PWRDIAG]   %-9s %8.4f %9.4f %9.4f %9.4f  %s\n",
                   pm_rails[r].name, m0, m1, m1 - m0, se,
                   ((m1 - m0) > 2.0 * se) ? "RESOLVED" : "");
        }
        {
            double se = (double)pm_sqrtf((float)tv);
            printf("[PWRDIAG]   %-9s %8.4f %9.4f %9.4f %9.4f  %s\n",
                   "TOTAL", ti, ta, ta - ti, se,
                   ((ta - ti) > 2.0 * se) ? "RESOLVED" : "BLIND?");
            if ((ta - ti) <= 2.0 * se)
                printf("[PWRDIAG]   >> A saturating DDR memcpy is invisible. The\n"
                       "[PWRDIAG]      instrument, not the accelerator, is the limit.\n");
        }
    }
}

/* 4. PL CLOCK-STOP REFERENCE -- the largest load we can legitimately
 * switch, and the last chance to prove the telemetry sees anything.
 *
 * The A/B compares "PL clocked and idle" against "PL clocked and
 * computing". The clock tree runs in BOTH, and on a design this size
 * the clock tree is likely most of the PL dynamic power -- so the A/B
 * was only ever measuring the DATA-TOGGLE INCREMENT on top of an
 * already-clocked design. That is a genuinely small quantity, and I
 * had been calling it "accelerator power", which it is not.
 *
 * Dropping FCLK0 from 100 MHz to ~250 kHz removes the clock tree and
 * essentially all switching. Vivado puts PL at 0.450 W; if even a third
 * of that is dynamic, this is a 150 mW step -- ten grid codes on
 * VCCINT, impossible to miss. If THIS is invisible, the ZC702 PMBus
 * telemetry cannot support any incremental power claim and we report
 * total board power with a stated detection limit instead.
 *
 * SAFETY: only the clock divisors move -- no reset is asserted -- so PL
 * state survives and nothing needs re-initialising. The PL must not be
 * touched while slowed: an AXI-Lite access would stall the CPU until
 * the clock returns. Hence usleep only, and PMBus is PS-side I2C. The
 * original divisor word is restored before the function returns. */
#define PM_SLCR_UNLOCK      0xF8000008U
#define PM_SLCR_LOCK        0xF8000004U
#define PM_FPGA0_CLK_CTRL   0xF8000170U
#define PM_SLCR_UNLOCK_KEY  0x0000DF0DU
#define PM_SLCR_LOCK_KEY    0x0000767BU
#define PM_CLKDIV_MASK      ((0x3FU << 8) | (0x3FU << 20))

__attribute__((unused))
static void pm_diag_clockstop(void)
{
    pm_sample_t now[PM_NRAILS];
    double sp[2][PM_NRAILS], sp2[2][PM_NRAILS];
    unsigned long n[2] = { 0, 0 };
    uint32_t orig, slow;

    Xil_Out32(PM_SLCR_UNLOCK, PM_SLCR_UNLOCK_KEY);
    orig = Xil_In32(PM_FPGA0_CLK_CTRL);
    slow = (orig & ~PM_CLKDIV_MASK) | (63U << 8) | (63U << 20);

    printf("\n[PWRDIAG] 4. PL clock-stop reference (FCLK0 100 MHz vs ~250 kHz)\n");
    printf("[PWRDIAG]   FPGA0_CLK_CTRL 0x%08lX -> 0x%08lX\n",
           (unsigned long)orig, (unsigned long)slow);

    for (int ph = 0; ph < 2; ph++)
        for (unsigned r = 0; r < PM_NRAILS; r++) sp[ph][r] = sp2[ph][r] = 0.0;

    for (unsigned c = 0; c < 6U; c++) {
        for (int ph = 0; ph < 2; ph++) {     /* 0 = clock slowed, 1 = 100 MHz */
            u64_cycles p0, tl;
            Xil_Out32(PM_FPGA0_CLK_CTRL, ph ? orig : slow);
            p0 = timer_now(); tl = p0;
            while (cycles_to_ms(timer_now() - p0) < 3000.0) {
                usleep(5000);               /* never touch the PL while slowed */
                if (cycles_to_ms(timer_now() - p0) < 700.0) continue;
                if (cycles_to_ms(timer_now() - tl) < 120.0) continue;
                tl = timer_now();
                if (pm_scan(now) != XST_SUCCESS) continue;
                for (unsigned r = 0; r < PM_NRAILS; r++) {
                    sp[ph][r] += now[r].p; sp2[ph][r] += now[r].p * now[r].p;
                }
                n[ph]++;
            }
        }
        printf("%c", 'k');
    }
    Xil_Out32(PM_FPGA0_CLK_CTRL, orig);      /* restore BEFORE any PL access */
    Xil_Out32(PM_SLCR_LOCK, PM_SLCR_LOCK_KEY);
    usleep(10000);

    printf("\n[PWRDIAG]   samples %lu slow / %lu fast\n", n[0], n[1]);
    if (n[0] < 8 || n[1] < 8) { printf("[PWRDIAG]   too few\n"); return; }
    {
        double ti = 0.0, ta = 0.0, tv = 0.0, pld = 0.0, plv = 0.0;
        printf("[PWRDIAG]   rail        250kHz    100MHz     delta      +-SE\n");
        for (unsigned r = 0; r < PM_NRAILS; r++) {
            double m0 = sp[0][r] / n[0], m1 = sp[1][r] / n[1];
            double v0 = sp2[0][r] / n[0] - m0 * m0;
            double v1 = sp2[1][r] / n[1] - m1 * m1;
            double se;
            if (v0 < 0.0) v0 = 0.0;
            if (v1 < 0.0) v1 = 0.0;
            se = (double)pm_sqrtf((float)(v0 / n[0] + v1 / n[1]));
            ti += m0; ta += m1; tv += se * se;
            if (pm_rails[r].group == PM_PL) { pld += m1 - m0; plv += se * se; }
            printf("[PWRDIAG]   %-9s %8.4f %9.4f %9.4f %9.4f  %s\n",
                   pm_rails[r].name, m0, m1, m1 - m0, se,
                   ((m1 - m0) > 2.0 * se) ? "RESOLVED" : "");
        }
        {
            double pse = (double)pm_sqrtf((float)plv);
            double se  = (double)pm_sqrtf((float)tv);
            printf("[PWRDIAG]   %-9s %8.4f %9.4f %9.4f %9.4f  %s\n",
                   "PL group", 0.0, 0.0, pld, pse,
                   (pld > 2.0 * pse) ? "RESOLVED" : "");
            printf("[PWRDIAG]   %-9s %8.4f %9.4f %9.4f %9.4f  %s\n",
                   "TOTAL", ti, ta, ta - ti, se,
                   ((ta - ti) > 2.0 * se) ? "RESOLVED" : "BLIND");
            if ((ta - ti) > 2.0 * se)
                printf("[PWRDIAG]   >> Telemetry works. %.4f W is the PL design's\n"
                       "[PWRDIAG]      TOTAL contribution (clock tree + logic).\n"
                       "[PWRDIAG]      Quote this, not the idle/active increment.\n",
                       ta - ti);
            else
                printf("[PWRDIAG]   >> Stopping the PL clock is invisible. The ZC702\n"
                       "[PWRDIAG]      PMBus telemetry cannot support ANY incremental\n"
                       "[PWRDIAG]      power claim. Report total board power only.\n");
        }
    }
}

/* Interleaved idle/active board power measurement with an explicit noise floor.
 *
 * SINGLE-CORE CAVEAT: the A9 cannot poll PMBus and run frames at the same
 * instant, so the active phase runs frames back to back and samples between
 * them. The fraction of wall time actually spent computing is reported as the
 * DUTY CYCLE; below 1.0 the true continuously-active power is HIGHER than shown.
 *
 * WHY INTERLEAVED: the first attempt ran idle for 30 s then active for 30 s and
 * produced NEGATIVE deltas on PL and PS -- impossible -- because board power
 * drifted downward across the two minutes and the drift exceeded the signal.
 * Alternating A/B/A/B... makes drift common-mode.
 *
 * WHY THE STATISTICS: a delta is only meaningful against its own uncertainty.
 * This reports, per rail, the sample standard deviation of both phases and the
 * standard error of the difference, then marks each rail RESOLVED only when
 * |delta| > 2 * SE. It also prints the Linear11 current LSB, because if the
 * incremental current is below one LSB the measurement cannot succeed at all
 * and no amount of averaging will change that. */
static void surr_power_ab(const layer_desc_t *descs,
                          file_blob_t *w_blob, file_blob_t *p_blob,
                          uint8_t *raw_in, uint8_t *gm_in,
                          uint8_t *chainA, uint8_t *chainB, int npairs)
{
    pm_sample_t now[PM_NRAILS];
    /* [0] = idle phase, [1] = active phase */
    double sp[2][PM_NRAILS], sp2[2][PM_NRAILS];
    unsigned long n[2] = { 0, 0 };
    float lsb[PM_NRAILS];
    u64_cycles t_frames = 0, t_wall0;
    /* Duty must be measured against the ACTIVE phase's wall time only.
     * Dividing by the whole run counts the idle phases as idle compute and
     * pins the answer at ~0.5 no matter how efficient the active phase is
     * -- which is exactly what the 8-cycle run reported (0.4881). */
    u64_cycles t_active_wall = 0, t_active_wall0 = 0;
    unsigned long nframe = 0;
    double wall_ms;

    printf("\n========================================\n");
    printf(" BOARD POWER (ZC702 PMBus, UCD9248 x3)\n");
    printf("========================================\n");

    if (pm_init() != XST_SUCCESS) {
        printf("[PWR] PMBus init FAILED -- is I2C0 enabled and the mux at 0x74?\n");
        return;
    }

    for (int ph = 0; ph < 2; ph++)
        for (unsigned r = 0; r < PM_NRAILS; r++) { sp[ph][r] = sp2[ph][r] = 0.0; }
    for (unsigned r = 0; r < PM_NRAILS; r++) lsb[r] = 0.0f;

    printf("[PWR] interleaved A/B: %u cycles x 2 phases x %us"
           " (first %d ms of each phase discarded)\n",
           PM_AB_CYCLES, PM_PHASE_SECONDS, PM_PHASE_SKIP_MS);

    g_cascade_quiet = 1;

#if PM_ENERGY_ONLY
    {
        double s1[PM_NRAILS], s2[PM_NRAILS];
        unsigned long ns = 0;
        u64_cycles e0, tl;

        for (unsigned r = 0; r < PM_NRAILS; r++) s1[r] = s2[r] = 0.0;
        printf("[PWR] energy-per-frame mode: %u s of back-to-back frames\n",
               PM_ENERGY_SECONDS);

        e0 = timer_now(); tl = e0;
        while (cycles_to_ms(timer_now() - e0) < PM_ENERGY_SECONDS * 1000.0) {
            u64_cycles f0 = timer_now();
            if (surr_run_one_frame(descs, w_blob, p_blob, raw_in, gm_in,
                                   chainA, chainB, npairs) != 0) {
                g_cascade_quiet = 0; printf("\n[PWR] frame FAILED\n"); return;
            }
            t_frames += timer_now() - f0;
            nframe++;
            if (cycles_to_ms(timer_now() - tl) < 150.0) continue;
            tl = timer_now();
            if (pm_scan(now) != XST_SUCCESS) continue;
            for (unsigned r = 0; r < PM_NRAILS; r++) {
                s1[r] += now[r].p; s2[r] += now[r].p * now[r].p;
            }
            ns++;
        }
        wall_ms = cycles_to_ms(timer_now() - e0);
        g_cascade_quiet = 0;

        if (ns < 8 || nframe == 0) { printf("[PWR] too few samples\n"); return; }
        {
            double tot = 0.0, var = 0.0;
            double t_fr = cycles_to_ms(t_frames) / (double)nframe;
            double se;
            printf("[PWR] samples %lu over %.1f s, %lu frames\n",
                   ns, wall_ms / 1000.0, nframe);
            printf("[PWR]   rail        mean W     +-SE\n");
            for (unsigned r = 0; r < PM_NRAILS; r++) {
                double m = s1[r] / (double)ns;
                double v = s2[r] / (double)ns - m * m;
                if (v < 0.0) v = 0.0;
                tot += m; var += v / (double)ns;
                printf("[PWR]   %-9s %9.4f %8.4f\n",
                       pm_rails[r].name, m, (double)pm_sqrtf((float)(v / ns)));
            }
            se = (double)pm_sqrtf((float)var);
            printf("[PWR]   %-9s %9.4f %8.4f\n", "TOTAL", tot, se);
            printf("\n[PWR] frame time      : %.3f ms  (%.1f fps)\n",
                   t_fr, 1000.0 / t_fr);
            printf("[PWR] board power     : %.4f +- %.4f W\n", tot, se);
            printf("[PWR] ENERGY/FRAME    : %.2f +- %.2f mJ\n",
                   tot * t_fr, se * t_fr);
            printf("[PWR] duty cycle      : %.4f  (%.1f ms compute of %.1f ms wall)\n",
                   cycles_to_ms(t_frames) / wall_ms, cycles_to_ms(t_frames), wall_ms);
            printf("[PWR] GOP/s/W         : %.3f\n", 10.70 / tot);
            printf("[PWR] NOTE: regulator OUTPUT power across all 10 rails.\n");
            printf("[PWR]   Excludes conversion losses and the unmonitored 5V\n");
            printf("[PWR]   USB rail -- this is NOT 12V wall-input power.\n");
            printf("[PWR]   This is WHOLE-BOARD energy: PS, DDR and the 0.79 W\n");
            printf("[PWR]   of 3V3/2V5/ADJ peripherals are all included. The\n");
            printf("[PWR]   accelerator's own share is NOT separable -- the\n");
            printf("[PWR]   UCD9248 current quantum is 15.625 mA (1/64 A),\n");
            printf("[PWR]   which is 15.6 mW on VCCINT and swamps the delta.\n");
        }
        return;
    }
#endif

#if PM_RUN_DIAGNOSTICS
    /* Diagnostics 1 and 2 have already done their job (2026-08-05): the
     * quantum is 15.625 mA = 1/64 A on every rail, PAGE writes are
     * innocent, and there is no visible step. Left in but off; set to 1
     * to re-run them. */
#if 0
    pm_diag_noise();
    pm_diag_step(descs, w_blob, p_blob, raw_in, gm_in, chainA, chainB, npairs);
#endif
    pm_diag_control(chainA, chainB);
    pm_diag_clockstop();
#endif

    t_wall0 = timer_now();

    for (unsigned c = 0; c < PM_AB_CYCLES; c++) {
        for (int ph = 0; ph < 2; ph++) {            /* 0 = idle, 1 = active */
            u64_cycles p0 = timer_now();
            u64_cycles t_scan = p0;
            if (ph == 1) t_active_wall0 = p0;
            while (cycles_to_ms(timer_now() - p0) < PM_PHASE_SECONDS * 1000.0) {
                if (ph == 1) {                       /* keep the PL busy */
                    u64_cycles f0 = timer_now();
                    if (surr_run_one_frame(descs, w_blob, p_blob, raw_in, gm_in,
                                           chainA, chainB, npairs) != 0) {
                        g_cascade_quiet = 0;
                        printf("\n[PWR] frame FAILED\n"); return;
                    }
                    t_frames += timer_now() - f0;
                    nframe++;
                } else {
                    usleep(20000);
                }
                /* Discard the head of each phase: the rail settles and the
                 * UCD9248 averages internally, so early samples are a blend of
                 * both phases and would dilute the very delta we want. */
                if (cycles_to_ms(timer_now() - p0) < (double)PM_PHASE_SKIP_MS)
                    continue;
                /* Throttle the scan. A full 10-rail scan is ~40 I2C
                 * transactions at 100 kHz, roughly 12 ms -- scanning after
                 * every 38 ms frame would drop the active duty cycle to ~0.76
                 * and understate active power. Same interval in both phases so
                 * the two sample counts stay comparable. */
                if (cycles_to_ms(timer_now() - t_scan) < (double)PM_SCAN_INTERVAL_MS)
                    continue;
                t_scan = timer_now();
                if (pm_scan(now) == XST_SUCCESS) {
                    for (unsigned r = 0; r < PM_NRAILS; r++) {
                        double p = now[r].p;
                        sp [ph][r] += p;
                        sp2[ph][r] += p * p;
                        if (now[r].i_lsb > lsb[r]) lsb[r] = now[r].i_lsb;
                    }
                    n[ph]++;
                }
            }
            if (ph == 1) t_active_wall += timer_now() - t_active_wall0;
            printf("%c", ph ? 'A' : 'i');
        }
    }
    wall_ms = cycles_to_ms(timer_now() - t_wall0);
    g_cascade_quiet = 0;
    printf("\n[PWR] samples: %lu idle, %lu active; %lu frames\n",
           n[0], n[1], nframe);
    if (n[0] < 4 || n[1] < 4) { printf("[PWR] too few samples\n"); return; }

    /* ---------------- REPORT ---------------- */
    {
        double mean[2][PM_NRAILS], sd[2][PM_NRAILS];
        double gd[PM_NGROUP], gse[PM_NGROUP], gi[PM_NGROUP], ga[PM_NGROUP];
        double tot_i = 0.0, tot_a = 0.0, tot_var = 0.0;
        double duty  = cycles_to_ms(t_frames) / cycles_to_ms(t_active_wall);
        double t_fr  = cycles_to_ms(t_frames) / (double)nframe;
        int    resolved_total;

        for (int ph = 0; ph < 2; ph++)
            for (unsigned r = 0; r < PM_NRAILS; r++) {
                double m = sp[ph][r] / (double)n[ph];
                double v = sp2[ph][r] / (double)n[ph] - m * m;
                mean[ph][r] = m;
                sd  [ph][r] = (v > 0.0) ? (double)pm_sqrtf((float)v) : 0.0;
            }
        for (int g = 0; g < PM_NGROUP; g++) { gd[g] = gse[g] = gi[g] = ga[g] = 0.0; }

        printf("\n[PWR] per-rail power (W).  SE = standard error of the difference.\n");
        printf("[PWR]   rail      grp     idle+-sd        active+-sd        delta+-SE   I_lsb   verdict\n");
        for (unsigned r = 0; r < PM_NRAILS; r++) {
            double d  = mean[1][r] - mean[0][r];
            double se = (double)pm_sqrtf((float)(sd[0][r]*sd[0][r]/(double)n[0]
                                               + sd[1][r]*sd[1][r]/(double)n[1]));
            int g = (int)pm_rails[r].group;
            gi[g] += mean[0][r]; ga[g] += mean[1][r];
            gd[g] += d;          gse[g] += se * se;
            tot_i += mean[0][r]; tot_a += mean[1][r]; tot_var += se * se;
            printf("[PWR]   %-9s %-4s %7.4f+-%.4f %7.4f+-%.4f %8.4f+-%.4f %7.4f  %s\n",
                   pm_rails[r].name, pm_group_name[g],
                   mean[0][r], sd[0][r], mean[1][r], sd[1][r], d, se, lsb[r],
                   (d > 2.0 * se) ? "RESOLVED" :
                   (d < -2.0 * se) ? "NEGATIVE!" : "below noise");
        }

        printf("\n[PWR] by group (W)        idle    active     delta      +-SE   verdict\n");
        for (int g = 0; g < PM_NGROUP; g++) {
            double se = (double)pm_sqrtf((float)gse[g]);
            printf("[PWR]   %-8s %10.4f %9.4f %9.4f %9.4f   %s\n",
                   pm_group_name[g], gi[g], ga[g], gd[g], se,
                   (gd[g] > 2.0 * se) ? "RESOLVED" :
                   (gd[g] < -2.0 * se) ? "NEGATIVE!" : "below noise");
        }
        {
            double se = (double)pm_sqrtf((float)tot_var);
            resolved_total = ((tot_a - tot_i) > 2.0 * se);
            printf("[PWR]   %-8s %10.4f %9.4f %9.4f %9.4f   %s\n", "TOTAL",
                   tot_i, tot_a, tot_a - tot_i, se,
                   resolved_total ? "RESOLVED" :
                   ((tot_a - tot_i) < -2.0 * se) ? "NEGATIVE!" : "below noise");
        }

        printf("\n[PWR] duty cycle      : %.4f  (%.1f ms compute of %.1f ms ACTIVE"
               " wall; %.1f ms total run)\n",
               duty, cycles_to_ms(t_frames), cycles_to_ms(t_active_wall), wall_ms);
        printf("[PWR] frames           : %lu, %.3f ms/frame\n", nframe, t_fr);
        printf("[PWR] energy/frame     : %.3f mJ total\n", tot_a * t_fr);
        if (resolved_total)
            printf("[PWR] incremental      : %.4f W, %.3f mJ/frame, %.3f GOP/s/W\n",
                   tot_a - tot_i, (tot_a - tot_i) * t_fr, 10.70 / (tot_a - tot_i));
        else
            printf("[PWR] incremental      : NOT RESOLVED -- |delta| < 2*SE.\n"
                   "[PWR]   Report total power only. Detection limit is ~%.4f W;\n"
                   "[PWR]   quote that as an upper bound on accelerator power.\n",
                   2.0 * (double)pm_sqrtf((float)tot_var));
        printf("[PWR] GOP/s/W (total)  : %.3f\n", 10.70 / tot_a);
        printf("[PWR] NOTE: regulator OUTPUT power. Excludes conversion losses\n");
        printf("[PWR]   and the unmonitored 5V USB rail -- NOT 12V input power.\n");
        printf("[PWR]   If duty < 1.0 the true continuously-active power is higher.\n");
        printf("[PWR]   Compare against Vivado report_power: 2.187 W total,\n");
        printf("[PWR]   PS7 1.573 W, PL 0.450 W -- Medium confidence, vectorless.\n");
    }
}
#endif  /* RUN_POWER_MEASUREMENT */

static u64_cycles surr_layer_cyc[SURR_NUM_LAYERS];

/* ============================================================
 * Surrogate end-to-end estimate ON THE CASCADE HARDWARE (2026-07-28)
 *
 * Runs on the CURRENT cascade bitstream -- no rebuild. The surrogate is
 * exactly three DW->PW pairs, which is what the cascade datapath executes
 * natively (MM2S -> DW -> PW -> S2MM), so each pair is one call to the
 * existing generic hw_dw_pw_cascade_l0_l1().
 *
 * STRIDE HANDLING: the surrogate's DW layers are stride-2 but
 * dw_banked_window_8x is stride-1 only, so a DW fed the full 720p frame would
 * emit 4x what its PW expects and the pair would mismatch. Instead each DW is
 * run at ITS PW's (already downsampled) resolution, which makes the handoff
 * exact: DW emits C*G*H words, PW consumes tile_groups*cin_run of them, and
 * those are equal by construction.
 *   -> PW is timed at its TRUE workload (this is what we want: PW dominates)
 *   -> DW is timed at 1/4 of its true input, so its own number is an
 *      under-estimate -- acceptable because in a fused pair the cost is
 *      max(DW,PW), and PW wins by a wide margin at every stage
 *      (e.g. pair 0: DW at full 720p is ~345,600 input words ~= 3.5 ms,
 *       against PW L1 at ~2.2M cycles ~= 22 ms).
 * If a pair ever comes out DW-bound, that conclusion needs re-checking.
 *
 * cout is still rounded up to a multiple of N_OC, so L1/L3 carry the
 * 30-channel penalty -- that is real, and it is what the current hardware
 * would actually cost.
 * ============================================================ */
static int cnn_run_surrogate_cascade_estimate(void)
{
    layer_desc_t descs[SURR_NUM_LAYERS];
    file_blob_t  w_blob[SURR_NUM_LAYERS];
    file_blob_t  p_blob[SURR_NUM_LAYERS];
    u64_cycles   pair_cyc[SURR_NUM_LAYERS / 2];
    uint8_t     *raw_in = (uint8_t *)DDR_BUF_CUR_ADDR;
    uint8_t     *gm_in  = (uint8_t *)DDR_SKIP1_ADDR;
    uint8_t     *pw_out = (uint8_t *)DDR_SKIP0_ADDR;
    int npairs = SURR_NUM_LAYERS / 2;

    /* Chaining ping-pong. The DMA must never read and write the same region in
     * one pass, so successive pairs alternate between two buffers:
     *   p=0: in = gm_in (CPU-packed camera frame)  out = chainA
     *   p=1: in = chainA                           out = chainB
     *   p=2: in = chainB                           out = chainA
     * chainA is DDR_SKIP0 (the old pw_out, 176 MB) and chainB is DDR_SKIP2
     * (48 MB). Largest intermediate is pair 0's 1,843,200 B, so both are
     * enormously oversized -- deliberately, they are fixed DDR windows. */
    uint8_t     *chainA = (uint8_t *)DDR_SKIP0_ADDR;
    uint8_t     *chainB = (uint8_t *)DDR_SKIP2_ADDR;
    (void)pw_out;

#if SURR_GM_IN_NONCACHED
    /* Map the gm_in window normal-non-cacheable, so pair 0's pack writes go
     * straight to DRAM through the write buffer and the 8.88 ms/frame flush
     * disappears. Xil_SetTlbAttributes operates on 1 MB sections; pair 0 needs
     * 2,764,800 B = 0x2A3000 -> 3 sections, so map 4 for headroom. gm_in is
     * DDR_SKIP1_ADDR = 0x33000000, already 1 MB aligned.
     *
     * Each call does a full Xil_DCacheFlush() internally, writing back anything
     * already dirty in this range BEFORE the attribute changes -- so there is
     * no window where stale dirty lines could later evict over DMA data. This
     * is one-time setup, not per frame. */
    {
        const unsigned gm_sections = 4u;
        for (unsigned s = 0; s < gm_sections; s++)
            Xil_SetTlbAttributes((INTPTR)((UINTPTR)gm_in + (UINTPTR)s * 0x100000u),
                                 NORM_NONCACHE);
        printf("[SURR-CASC] gm_in @ %p mapped NORM_NONCACHE across %u MB"
               " -- pair 0 flush removed\n", (void *)gm_in, gm_sections);
    }
#endif

    memset(w_blob, 0, sizeof(w_blob));
    memset(p_blob, 0, sizeof(p_blob));
    memset(pair_cyc, 0, sizeof(pair_cyc));

    surr_build_schedule(descs);

    for (int i = 0; i < SURR_NUM_LAYERS; i++) {
        int rc = (descs[i].kind == KIND_DW)
               ? surr_alloc_dw_blobs(descs[i].Cin, &w_blob[i], &p_blob[i])
               : surr_alloc_pw_blobs(descs[i].Cin, descs[i].Cout, &w_blob[i], &p_blob[i]);
        if (rc != 0) {
            printf("[SURR-CASC] blob alloc failed L%d\n", i);
            for (int j = 0; j <= i; j++) { free_blob(&w_blob[j]); free_blob(&p_blob[j]); }
            return -1;
        }
    }

#if SURR_PACK_BENCH
    surr_pack_bench(raw_in, gm_in, descs[0].Cin, descs[0].H, descs[0].W,
                    descs[0].zp_in);
#endif

    printf("\n========================================\n");
    printf(" SURROGATE END-TO-END ESTIMATE -- ON CASCADE HW (720p in)\n");
    printf("========================================\n");

    for (int p = 0; p < npairs; p++) {
        layer_desc_t dw = descs[2 * p];
        layer_desc_t pw = descs[2 * p + 1];
        u64_cycles   t0;
        int rc;

        /* PROXY REMOVED 2026-07-30 (see the warm-up loop). This used to force
         * dw.H = pw.H / dw.W = pw.W. Leaving it here while the steady-state
         * loops had it removed made the cold pass run a DIFFERENT, and wrong,
         * configuration: the DW got the already-halved PW dims AND the new
         * stride-2 halving on top, so PW was sized at a quarter of the real
         * resolution (pair 0 reported TILE_PX=57600 instead of 230400). It
         * completed cleanly while computing the wrong thing -- and since the
         * cold pass prints the only per-pair diagnostics, the output described
         * a configuration that was not the one being measured. */

        const int chained  = SURR_CHAIN_PAIRS && (p > 0);
        uint8_t  *out_buf  = (p % 2 == 0) ? chainA : chainB;
        uint8_t  *in_buf   = (p == 0) ? gm_in
                                      : ((p % 2 == 1) ? chainA : chainB);

        /* Only pair 0 needs a source image; later pairs read the previous
         * pair's PW output, which the DMA already placed in in_buf. */
        if (!chained) {
            size_t raw_bytes = (size_t)dw.Cin * (size_t)dw.H * (size_t)dw.W;
            memset(raw_in, dw.zp_in, raw_bytes);
            Xil_DCacheFlushRange((UINTPTR)raw_in, raw_bytes);
        }

        printf("\n[SURR-CASC] pair %d : DW(%d->%d) %dx%d  ->  PW(%d->%d)%s\n",
               p, dw.Cin, dw.Cout, dw.H, dw.W, pw.Cin, pw.Cout,
               chained ? "  [chained: no pack, no flush]" : "");

        t0 = timer_now();
        rc = hw_dw_pw_cascade_l0_l1(&dw, &pw, raw_in, in_buf, out_buf,
                                    &p_blob[2 * p],     &w_blob[2 * p],      /* DW */
                                    &p_blob[2 * p + 1], &w_blob[2 * p + 1], /* PW */
                                    SURR_IN_FLAGS(chained));
        pair_cyc[p] = timer_now() - t0;

        if (rc != 0) {
            printf("[SURR-CASC] pair %d FAILED (rc=%d)\n", p, rc);
            for (int j = 0; j < SURR_NUM_LAYERS; j++) { free_blob(&w_blob[j]); free_blob(&p_blob[j]); }
            return -1;
        }
        printf("[SURR-CASC] pair %d : %.3f ms\n", p, cycles_to_ms(pair_cyc[p]));
    }

    {
        double total_ms = 0.0;
        printf("\n===== COLD-START PASS (console enabled -- NOT the figure) =====\n");
        for (int p = 0; p < npairs; p++) {
            printf("[SURR-CASC] pair %d : %.3f ms\n", p, cycles_to_ms(pair_cyc[p]));
            total_ms += cycles_to_ms(pair_cyc[p]);
        }
        printf("[SURR-CASC] cold-start total : %.2f ms (includes blocking UART)\n",
               total_ms);
    }

    /* ============================================================
     * STEADY-STATE MEASUREMENT -- this is the defensible figure.
     *
     * Everything above is a cold-start pass with the console on. Blocking UART
     * printf sits inside the timed brackets (three lines inside the HW bracket
     * alone), so those numbers are inflated by console time and none of them
     * should be quoted.
     *
     * Here: console silenced, no sentinel fill, no test-harness memsets, first
     * frame discarded as warm-up, then SURR_FRAMES frames averaged.
     *
     * WHAT IS INCLUDED, and why it is per-frame rather than amortisable:
     *  - weight/param programming. DW's weight RAM is indexed by channel and
     *    PW's by batch*Cin+ic, so the three pairs' weights OVERLAP in the same
     *    storage -- they cannot all stay resident, and each pair must reprogram
     *    every frame. (Batching by layer across frames would amortise this, at
     *    the cost of holding every intermediate tensor.)
     *  - input cache flush and output cache invalidate, on the real ranges.
     *  - the full DW->PW hardware run.
     *
     * WHAT IS EXCLUDED, and is therefore NOT claimed:
     *  - input packing (a camera/DMA front end would deliver group-major).
     *  - inter-pair repacking. PW emits cout_rounded channels per pixel group
     *    (30 where only 8 are wanted), so chaining pair p into pair p+1 needs
     *    the padding channels stripped. That cost is real and is not measured
     *    here -- each pair is fed an independent buffer.
     * ============================================================ */
    {
        /* static, not stack: 100 x u64 is fine on a host but the standalone
         * BSP's default stack is small and this sits inside a deep call. */
        static u64_cycles f_cyc[SURR_FRAMES];
        u64_cycles warm;
        double sum_ms = 0.0, best_ms = 0.0, worst_ms = 0.0;

        printf("\n[SURR-CASC] steady state: %d frames, console silenced...\n",
               SURR_FRAMES);

        g_cascade_quiet = 1;

        /* warm-up frames, DISCARDED -- accumulation stays OFF for them */
        warm = timer_now();
        for (int wu = 0; wu < SURR_WARMUP; wu++)
        for (int p = 0; p < npairs; p++) {
            layer_desc_t dw = descs[2 * p], pw = descs[2 * p + 1];
            /* PROXY REMOVED 2026-07-30. This used to force
             *     dw.H = pw.H; dw.W = pw.W;
             * running each DW stride-1 at its PW's already-downsampled
             * resolution, because no stride-2 DW existed. The DW now
             * downsamples itself, so it takes its own FULL input resolution
             * from the schedule (720x1280 / 360x640 / 180x320) and reads 4x
             * the bytes it used to. */
            const int chained = SURR_CHAIN_PAIRS && (p > 0);
            uint8_t  *out_buf = (p % 2 == 0) ? chainA : chainB;
            uint8_t  *in_buf  = (p == 0) ? gm_in : ((p % 2 == 1) ? chainA : chainB);
            if (hw_dw_pw_cascade_l0_l1(&dw, &pw, raw_in, in_buf, out_buf,
                                       &p_blob[2 * p],     &w_blob[2 * p],
                                       &p_blob[2 * p + 1], &w_blob[2 * p + 1],
                                       SURR_IN_FLAGS(chained)) != 0) {
                g_cascade_quiet = 0;
                printf("[SURR-CASC] warm-up frame FAILED at pair %d\n", p);
                for (int j = 0; j < SURR_NUM_LAYERS; j++) { free_blob(&w_blob[j]); free_blob(&p_blob[j]); }
                return -1;
            }
        }
        (void)warm;

        g_acc_pack = g_acc_prog = g_acc_cache = g_acc_hw = g_acc_total = 0;
        for (int i = 0; i < SURR_MAX_PAIRS; i++) {
            g_acc_hw_p[i] = g_acc_pack_p[i] = g_acc_prog_p[i] = g_acc_cache_p[i] = 0;
#if CASCADE_ENGINE_SPLIT
            g_acc_dwbusy_p[i] = g_acc_pwbusy_p[i] = 0;
#endif
        }
        g_cascade_accum = 1;

        for (int f = 0; f < SURR_FRAMES; f++) {
            u64_cycles t0 = timer_now();
            for (int p = 0; p < npairs; p++) {
                layer_desc_t dw = descs[2 * p], pw = descs[2 * p + 1];
                /* stride-1 proxy removed -- see the warm-up loop above */
                const int chained = SURR_CHAIN_PAIRS && (p > 0);
                uint8_t  *out_buf = (p % 2 == 0) ? chainA : chainB;
                uint8_t  *in_buf  = (p == 0) ? gm_in : ((p % 2 == 1) ? chainA : chainB);
                g_acc_pair = p;   /* attribute this call's phases to pair p */
                if (hw_dw_pw_cascade_l0_l1(&dw, &pw, raw_in, in_buf, out_buf,
                                           &p_blob[2 * p],     &w_blob[2 * p],
                                           &p_blob[2 * p + 1], &w_blob[2 * p + 1],
                                           SURR_IN_FLAGS(chained)) != 0) {
                    g_cascade_quiet = 0;
                    printf("[SURR-CASC] frame %d FAILED at pair %d\n", f, p);
                    for (int j = 0; j < SURR_NUM_LAYERS; j++) { free_blob(&w_blob[j]); free_blob(&p_blob[j]); }
                    return -1;
                }
            }
            f_cyc[f] = timer_now() - t0;
        }

        g_cascade_accum = 0;

        /* Dedicated UNTIMED pass purely to read the DW beat counters per pair.
         * Accumulation is already off, so this cannot affect any reported
         * phase or the frame distribution. */
        g_capture_beats = 1;
        for (int p = 0; p < npairs; p++) {
            layer_desc_t dw = descs[2 * p], pw = descs[2 * p + 1];
            const int chained = SURR_CHAIN_PAIRS && (p > 0);
            uint8_t  *out_buf = (p % 2 == 0) ? chainA : chainB;
            uint8_t  *in_buf  = (p == 0) ? gm_in : ((p % 2 == 1) ? chainA : chainB);
            g_acc_pair = p;
            (void)hw_dw_pw_cascade_l0_l1(&dw, &pw, raw_in, in_buf, out_buf,
                                         &p_blob[2 * p],     &w_blob[2 * p],
                                         &p_blob[2 * p + 1], &w_blob[2 * p + 1],
                                         SURR_IN_FLAGS(chained));
        }
        g_capture_beats = 0;

#if RUN_POWER_MEASUREMENT
        /* Deliberately AFTER the timed loop and the capture pass: the A/B adds
         * ~70 s and its I2C traffic must never land inside a timed bracket. */
        surr_power_ab(descs, w_blob, p_blob, raw_in, gm_in, chainA, chainB, npairs);
#endif

        g_cascade_quiet = 0;

        /* Per-frame times, sorted, for order statistics. With SURR_FRAMES=100
         * the individual frames are NOT printed -- 100 lines of blocking UART
         * is noise, and the distribution is what matters. */
        {
            static double fms[SURR_FRAMES];
            for (int f = 0; f < SURR_FRAMES; f++) {
                fms[f] = cycles_to_ms(f_cyc[f]);
                sum_ms += fms[f];
                if (f == 0 || fms[f] < best_ms)  best_ms  = fms[f];
                if (f == 0 || fms[f] > worst_ms) worst_ms = fms[f];
            }
            /* insertion sort -- N is small and this avoids pulling in qsort */
            for (int i = 1; i < SURR_FRAMES; i++) {
                double key = fms[i]; int j = i - 1;
                while (j >= 0 && fms[j] > key) { fms[j + 1] = fms[j]; j--; }
                fms[j + 1] = key;
            }
            g_stat_median = fms[SURR_FRAMES / 2];
            g_stat_p95    = fms[(SURR_FRAMES * 95) / 100];
            g_stat_p99    = fms[(SURR_FRAMES * 99) / 100];
        }

        {
            double mean_ms = sum_ms / (double)SURR_FRAMES;

            /* population standard deviation, and a hand-rolled sqrt so this
             * does not depend on libm being linked into the BSP */
            double var = 0.0;
            for (int f = 0; f < SURR_FRAMES; f++) {
                double d = cycles_to_ms(f_cyc[f]) - mean_ms;
                var += d * d;
            }
            var /= (double)SURR_FRAMES;
            double sd = var;
            if (sd > 0.0) {           /* Newton-Raphson, plenty for a std dev */
                double x = (var > 1.0) ? var : 1.0;
                for (int it = 0; it < 40; it++) x = 0.5 * (x + var / x);
                sd = x;
            }

            printf("\n========================================\n");
            printf(" ENCODER END-TO-END (steady state, 720p, %d frames)\n", SURR_FRAMES);
            printf("========================================\n");
            printf("[SURR-CASC] frames     : %d timed, %d warm-up discarded\n",
                   SURR_FRAMES, SURR_WARMUP);
            printf("[SURR-CASC] mean       : %8.3f ms\n", mean_ms);
            printf("[SURR-CASC] std dev    : %8.3f ms  (%.3f%% of mean)\n",
                   sd, mean_ms > 0 ? 100.0 * sd / mean_ms : 0.0);
            printf("[SURR-CASC] median     : %8.3f ms\n", g_stat_median);
            printf("[SURR-CASC] min / max  : %8.3f / %.3f ms\n", best_ms, worst_ms);
            printf("[SURR-CASC] P95 / P99  : %8.3f / %.3f ms\n", g_stat_p95, g_stat_p99);
            printf("[SURR-CASC] spread     : %8.3f ms  (max-min)\n", worst_ms - best_ms);
            printf("[SURR-CASC] throughput : %8.1f fps (from mean)\n",
                   (mean_ms > 0.0) ? 1000.0 / mean_ms : 0.0);
            printf("[SURR-CASC] worst-case : %8.1f fps (from P99)\n",
                   (g_stat_p99 > 0.0) ? 1000.0 / g_stat_p99 : 0.0);

            /* WARM phase breakdown -- summed over the timed frames only, so
             * unlike the cold-start figures these are what the steady-state
             * frame is actually made of. Use THESE to pick what to optimise. */
            {
                double n     = (double)SURR_FRAMES;
                double pk    = cycles_to_ms(g_acc_pack)  / n;
                double pg    = cycles_to_ms(g_acc_prog)  / n;
                double ca    = cycles_to_ms(g_acc_cache) / n;
                double hw    = cycles_to_ms(g_acc_hw)    / n;
                double tot   = cycles_to_ms(g_acc_total) / n;
                double unacc = mean_ms - tot;
                printf("\n[SURR-CASC] WARM per-frame breakdown (all %d pairs summed):\n",
                       npairs);
                printf("[SURR-CASC]   pack          : %6.2f ms  (%4.1f%%)\n",
                       pk, mean_ms > 0 ? 100.0 * pk / mean_ms : 0.0);
                printf("[SURR-CASC]   weight/param  : %6.2f ms  (%4.1f%%)\n",
                       pg, mean_ms > 0 ? 100.0 * pg / mean_ms : 0.0);
                printf("[SURR-CASC]   cache         : %6.2f ms  (%4.1f%%)\n",
                       ca, mean_ms > 0 ? 100.0 * ca / mean_ms : 0.0);
                printf("[SURR-CASC]   HW datapath   : %6.2f ms  (%4.1f%%)\n",
                       hw, mean_ms > 0 ? 100.0 * hw / mean_ms : 0.0);
                printf("[SURR-CASC]   -- driver sum : %6.2f ms\n", tot);

                /* PER-PAIR WARM BREAKDOWN + cost-model check (2026-07-30).
                 * Model: PW cycles/group = max(batches*cin, cout_rounded) + OVH,
                 * batches = cout_rounded/PW_N_OC, groups = pixels/8. The whole
                 * point is to see whether OVH is the same for every pair -- so
                 * far it has only ever been ONE constant fitted to the TOTAL.
                 * Also reports the DW input beats, since once PW gets fast
                 * enough the DW ingest (1 beat/cycle) becomes the real bound. */
                printf("\n[SURR-CASC] PER-PAIR WARM (cost-model check, PL @100MHz):\n");
                printf("[SURR-CASC]  pr   HWms   HWcyc   groups  cyc/grp  model  OVH  DWbeats bound\n");
                for (int p = 0; p < npairs && p < SURR_MAX_PAIRS; p++) {
                    const layer_desc_t *dwd = &descs[2 * p];
                    const layer_desc_t *pwd = &descs[2 * p + 1];
                    int    wpad   = (pwd->W + 7) & ~7;
                    long   groups = (long)pwd->H * wpad / 8;
                    int    cr     = pwd->Cout;
                    int    batch  = cr / PW_N_OC;
                    int    model  = (batch * pwd->Cin > cr) ? batch * pwd->Cin : cr;
                    double hwms   = cycles_to_ms(g_acc_hw_p[p]) / n;
                    double hwcyc  = hwms * 1e5;   /* 100 MHz PL */
                    double cpg    = groups ? hwcyc / (double)groups : 0.0;
                    long   dwb    = (long)dwd->Cin * dwd->H * ((dwd->W + 7) & ~7) / 8;
                    printf("[SURR-CASC]  %2d %6.2f %8.0f %8ld %8.1f %6d %5.1f %8ld  %s\n",
                           p, hwms, hwcyc, groups, cpg, model, cpg - (double)model,
                           dwb, ((double)dwb > hwcyc) ? "DW" : "PW");
                }
                printf("[SURR-CASC]  (OVH should be ~equal across pairs if the\n");
                printf("[SURR-CASC]   overhead really is a per-GROUP constant)\n");

#if CASCADE_ENGINE_SPLIT
                /* ---- DW / PW ENGINE OVERLAP ----
                 * PW cannot finish before DW (it needs DW's last beat), so
                 * t_pw_done >= t_dw_done and the split is unambiguous:
                 *   overlap = DW busy              (PW is live for all of it)
                 *   PW tail = PW busy - DW busy    (PW working alone)
                 * The informative number is the TAIL: how much of each block is
                 * PW-only, i.e. how completely DW disappears underneath PW. */
                printf("\n[SURR-CASC] DW/PW ENGINE OVERLAP (poll-sampled ~10us):\n");
                printf("[SURR-CASC]  pr   DWbusy   PWbusy  overlap   PWtail  DW/PW   tail%%\n");
                {
                    double sdw = 0.0, spw = 0.0;
                    for (int p = 0; p < npairs && p < SURR_MAX_PAIRS; p++) {
                        double dwb  = cycles_to_ms(g_acc_dwbusy_p[p]) / n;
                        double pwb  = cycles_to_ms(g_acc_pwbusy_p[p]) / n;
                        double tail = pwb - dwb;
                        sdw += dwb; spw += pwb;
                        printf("[SURR-CASC]  %2d %8.2f %8.2f %8.2f %8.2f %6.2f %6.1f%%\n",
                               p, dwb, pwb, dwb, tail,
                               pwb > 0.0 ? dwb / pwb : 0.0,
                               pwb > 0.0 ? 100.0 * tail / pwb : 0.0);
                    }
                    printf("[SURR-CASC] ALL %8.2f %8.2f %8.2f %8.2f %6.2f %6.1f%%\n",
                           sdw, spw, sdw, spw - sdw,
                           spw > 0.0 ? sdw / spw : 0.0,
                           spw > 0.0 ? 100.0 * (spw - sdw) / spw : 0.0);
                    printf("[SURR-CASC]  => %.2f ms of DW work runs underneath PW"
                           " and costs nothing\n", sdw);
                    printf("[SURR-CASC]  WARNING: CASCADE_ENGINE_SPLIT=1 adds AXI-Lite\n");
                    printf("[SURR-CASC]   polls inside the HW bracket. Quote HW/frame\n");
                    printf("[SURR-CASC]   totals from a CASCADE_ENGINE_SPLIT=0 run.\n");
                }
#endif

                /* ---- FUSION TRAFFIC, from MEASURED beat counters ----
                 * The DW output is the intermediate tensor that fusion keeps
                 * on-chip. Unfused, it would be written to DDR and read back,
                 * so the avoided traffic is 2x the DW output bytes. Beats are
                 * 64-bit. `produced` counts m_axis handshakes, i.e. beats that
                 * physically crossed into PW -- not a geometric estimate. */
                {
                    unsigned long dwout_B = 0, dma_B = 0;
                    printf("\n[SURR-CASC] FUSION TRAFFIC (measured DW beat counters):\n");
                    printf("[SURR-CASC]  pr  consumed  written  produced   DWout B   geom C  pad?\n");
                    for (int p = 0; p < npairs && p < SURR_MAX_PAIRS; p++) {
                        const layer_desc_t *dwd = &descs[2 * p];
                        const layer_desc_t *pwd = &descs[2 * p + 1];
                        int wpad = (pwd->W + 7) & ~7;
                        /* channels implied by the measured beat count */
                        long og = (long)pwd->H * wpad / 8;
                        double implied_c = og ? (double)g_dw_produced_p[p] / (double)og : 0.0;
                        unsigned long b = (unsigned long)g_dw_produced_p[p] * 8UL;
                        dwout_B += b;
                        printf("[SURR-CASC]  %2d %9u %8u %9u %9lu %8.2f  %s\n",
                               p, g_dw_consumed_p[p], g_dw_written_p[p],
                               g_dw_produced_p[p], b, implied_c,
                               (implied_c > (double)dwd->Cin + 0.01) ? "PADDED" : "no");
                        dma_B += (unsigned long)dwd->Cin * dwd->H * ((dwd->W + 7) & ~7);
                        dma_B += (unsigned long)pwd->H * wpad
                                 * (unsigned long)(pwd->Cout);
                    }
                    printf("[SURR-CASC]  intermediate (DW out) total : %lu B\n", dwout_B);
                    printf("[SURR-CASC]  avoided by fusion (w+r)     : %lu B\n", 2UL * dwout_B);
                    printf("[SURR-CASC]  actual DMA traffic          : %lu B\n", dma_B);
                    printf("[SURR-CASC]  unfused would be            : %lu B\n",
                           dma_B + 2UL * dwout_B);
                    printf("[SURR-CASC]  traffic reduction           : %.1f%%\n",
                           (dma_B + 2UL * dwout_B) ?
                           100.0 * (2.0 * dwout_B) / (double)(dma_B + 2UL * dwout_B) : 0.0);
                }
                printf("[SURR-CASC]  pack/prog/cache per pair (ms):");
                for (int p = 0; p < npairs && p < SURR_MAX_PAIRS; p++)
                    printf(" [%d]%.2f/%.2f/%.2f", p,
                           cycles_to_ms(g_acc_pack_p[p])  / n,
                           cycles_to_ms(g_acc_prog_p[p])  / n,
                           cycles_to_ms(g_acc_cache_p[p]) / n);
                printf("\n");
                printf("[SURR-CASC]   outside driver: %6.2f ms  (post-run invalidate,\n",
                       unacc);
                printf("[SURR-CASC]                   loop overhead)\n");
            }
            printf("[SURR-CASC] includes  : weight/param reprogramming (unavoidable --\n");
            printf("[SURR-CASC]             the %d pairs share DW/PW weight storage),\n",
                   npairs);
            printf("[SURR-CASC]             cache maintenance, full DW->PW hardware run\n");
#if SURR_CHAIN_PAIRS
            printf("[SURR-CASC] chaining  : ON -- pair p's PW output feeds pair p+1's\n");
            printf("[SURR-CASC]             DW directly. Pairs 1..n-1 do NO pack and NO\n");
            printf("[SURR-CASC]             cache flush: those bytes are written by S2MM\n");
            printf("[SURR-CASC]             and never touched by the CPU. Only pair 0's\n");
            printf("[SURR-CASC]             camera-side transpose remains.\n");
            printf("[SURR-CASC] excludes  : nothing on the inter-pair path -- it is real now\n");
#else
            printf("[SURR-CASC] excludes  : input packing, inter-pair channel repack\n");
#endif
            {   /* derive rather than hardcode -- these change with PW_N_OC */
                int r0 = descs[1].Cout;
                int r1 = descs[3].Cout;
                int r2 = descs[5].Cout;
                printf("[SURR-CASC]             (PW emits cout_rounded=%d/%d/%d for "
                       "Cout=%d/%d/%d, N_OC=%d)\n",
                       r0, r1, r2, descs[1].Cout, descs[3].Cout, descs[5].Cout,
                       PW_N_OC);
                if (r0 == descs[1].Cout && r1 == descs[3].Cout && r2 == descs[5].Cout)
                    printf("[SURR-CASC]             -> exact at every stage: no padding,\n"
                           "[SURR-CASC]                no inter-pair channel repack needed\n");
            }
            printf("[SURR-CASC] geometry  : REAL stride-2 DW -- each DW ingests its own\n");
            printf("[SURR-CASC]             full resolution (720x1280 / 360x640 / 180x320)\n");
            printf("[SURR-CASC]             and downsamples; PW runs on the DW OUTPUT.\n");
            printf("[SURR-CASC]             The stride-1 cost proxy is GONE, so DW input\n");
            printf("[SURR-CASC]             traffic (and its pack + cache flush) is 4x\n");
            printf("[SURR-CASC]             what the proxy measured.\n");
            printf("[SURR-CASC] still NOT validated: outputs are dummy-weight values;\n");
            printf("[SURR-CASC]             the DW one-word offset and edge bugs remain.\n");
        }
    }

    for (int j = 0; j < SURR_NUM_LAYERS; j++) { free_blob(&w_blob[j]); free_blob(&p_blob[j]); }
    return 0;
}

static int surr_run_dw_fused(const layer_desc_t *d,
                             uint8_t *gm_in, uint8_t *gm_out,
                             const file_blob_t *params_blob,
                             const file_blob_t *weights_blob,
                             u64_cycles *out_cyc)
{
    const int C        = d->Cin;
    const int W        = d->W;
    const int H        = d->H;
    const int W_padded = (W + 7) & ~7;
    const int G        = W_padded / 8;
    const size_t bytes = (size_t)C * (size_t)H * (size_t)W_padded;
    u64_cycles t0;
    int t;

    for (int c = 0; c < C; c++) {
        int8_t   w9[9];
        int32_t  bias;
        uint32_t mult;
        uint8_t  shift;
        if (load_dw_weights_3x3(weights_blob, d->weight_addr, c, w9) != 0 ||
            load_param_block_16B(params_blob, d->params_addr, c,
                                 &bias, &mult, &shift) != 0) {
            printf("[SURR] DW weight/param load fail L%d ch=%d\n", d->layer_idx, c);
            return -1;
        }
        dw_write_reg(DWF_REG_CH_ADDR, c);
        dw_write_reg(DWF_REG_W0, dw_pack_w0(w9[0], w9[1], w9[2], w9[3]));
        dw_write_reg(DWF_REG_W1, dw_pack_w1(w9[4], w9[5], w9[6], w9[7]));
        dw_write_reg(DWF_REG_W2, dw_pack_w2(w9[8]));
        dw_write_reg(DWF_REG_BIAS,  (u32)bias);
        dw_write_reg(DWF_REG_MULT,  mult);
        dw_write_reg(DWF_REG_SHIFT, shift);
    }

    dw_write_reg(DWF_REG_CIN_RUN,   C);
    dw_write_reg(DWF_REG_N_GROUPS,  G);
    dw_write_reg(DWF_REG_IMG_WIDTH, (u32)W);
    dw_write_reg(DWF_REG_N_ROWS,    (u32)H);
    dw_write_reg(DWF_REG_ZP_RELU,
                 pw_pack_zp_relu(d->zp_in, d->zp_out, d->relu_en));

    Xil_DCacheFlushRange((UINTPTR)gm_in, bytes);
    Xil_DCacheInvalidateRange((UINTPTR)gm_out, bytes);

    t0 = timer_now();

    if (XAxiDma_SimpleTransfer(&DwDma, (UINTPTR)gm_out, bytes,
                               XAXIDMA_DEVICE_TO_DMA) != XST_SUCCESS) {
        printf("[SURR] DW S2MM submit failed L%d\n", d->layer_idx);
        return -1;
    }
    dw_write_reg(DWF_REG_CTRL, 1u << 0);
    dw_write_reg(DWF_REG_CTRL, 0);
    if (XAxiDma_SimpleTransfer(&DwDma, (UINTPTR)gm_in, bytes,
                               XAXIDMA_DMA_TO_DEVICE) != XST_SUCCESS) {
        printf("[SURR] DW MM2S submit failed L%d\n", d->layer_idx);
        return -1;
    }

    t = 0;
    while (XAxiDma_Busy(&DwDma, XAXIDMA_DMA_TO_DEVICE))
        if (++t > 200000000) { printf("[SURR] DW MM2S timeout L%d\n", d->layer_idx); return -1; }
    t = 0;
    while (XAxiDma_Busy(&DwDma, XAXIDMA_DEVICE_TO_DMA))
        if (++t > 200000000) { printf("[SURR] DW S2MM timeout L%d\n", d->layer_idx); return -1; }
    t = 0;
    while ((dw_read_reg(DWF_REG_STATUS) & 1) == 0)
        if (++t > 5000000) { printf("[SURR] DW core never done L%d\n", d->layer_idx); return -1; }

    *out_cyc = timer_now() - t0;

    printf("[SURR]   L%d DW(fused,stride1 proxy) C=%d %dx%d  %lu B  %.3f ms  "
           "dbg c=%u w=%u p=%u (exp %u)\n",
           d->layer_idx, C, H, W, (unsigned long)bytes,
           cycles_to_ms(*out_cyc),
           (unsigned)dw_read_reg(DWF_REG_DBG_CONSUMED),
           (unsigned)dw_read_reg(DWF_REG_DBG_WRITTEN),
           (unsigned)dw_read_reg(DWF_REG_DBG_PRODUCED),
           (unsigned)((size_t)C * (size_t)G * (size_t)H));
    return 0;
}

int cnn_run_dw_pw_surrogate(void)
{
    layer_desc_t descs[SURR_NUM_LAYERS];
    file_blob_t  w_blob[SURR_NUM_LAYERS];
    file_blob_t  p_blob[SURR_NUM_LAYERS];

    uint8_t *buf_cur  = (uint8_t *)DDR_BUF_CUR_ADDR;
    uint8_t *buf_next = (uint8_t *)DDR_BUF_NEXT_ADDR;

    memset(w_blob, 0, sizeof(w_blob));
    memset(p_blob, 0, sizeof(p_blob));

    surr_build_schedule(descs);

    for (int i = 0; i < SURR_NUM_LAYERS; i++) {
        int rc = (descs[i].kind == KIND_DW)
            ? surr_alloc_dw_blobs(descs[i].Cin, &w_blob[i], &p_blob[i])
            : surr_alloc_pw_blobs(descs[i].Cin, descs[i].Cout, &w_blob[i], &p_blob[i]);
        if (rc != 0) {
            printf("[SURR] blob alloc failed L%d\n", i);
            for (int j = 0; j <= i; j++) { free_blob(&w_blob[j]); free_blob(&p_blob[j]); }
            return -1;
        }
    }

    /* Reset the shared peak/total trackers so the summary below reflects
     * only this surrogate run, not whatever ran earlier this boot. */
    g_peak_dw_mm2s = (peak_bw_t){0};
    g_peak_dw_s2mm = (peak_bw_t){0};
    g_peak_pw_mm2s = (peak_bw_t){0};
    g_peak_pw_s2mm = (peak_bw_t){0};
    g_peak_dw_thr  = (peak_thr_t){0};
    g_peak_pw_thr  = (peak_thr_t){0};
    g_total_dw_bytes = 0; g_total_dw_macs = 0;
    g_total_pw_bytes = 0; g_total_pw_macs = 0;
    g_total_layer_cyc = 0;
    g_total_dma_run_cyc = 0;
    memset(g_layer_stats, 0, sizeof(g_layer_stats));

    /* Dummy input tensor: Cin x H x W, constant = zp_in. Meaningless
     * numerically, but a stable, well-formed input for a timing run. */
    {
        size_t in_bytes = tensor_bytes_u8(descs[0].Cin, descs[0].H, descs[0].W);
        memset(buf_cur, descs[0].zp_in, in_bytes);
        Xil_DCacheFlushRange((UINTPTR)buf_cur, in_bytes);
    }

    printf("\n========================================\n");
    printf(" DW/PW SURROGATE ESTIMATE (dense-conv encoder -> DW+PW pairs)\n");
    printf(" input assumed 720p (%dx%d); dummy weights -- shapes/timing only\n", SURR_IN_W, SURR_IN_H);
    printf("========================================\n");

    for (int i = 0; i < SURR_NUM_LAYERS; i++) {
        const layer_desc_t *d = &descs[i];
        tensor_shape_t outs = get_layer_output_shape(d);
        int rc;

        printf("\n[SURR] L%d %s in=[%d,%d,%d] out=[%d,%d,%d]\n",
               i, d->kind == KIND_DW ? "DW" : "PW",
               d->Cin, d->H, d->W, outs.C, outs.H, outs.W);

        if (d->kind == KIND_DW) {
            /* Fused DW instead of run_dw_layer_real(): the latter targets
             * DW_conv_accel_0, deleted from the BD 2026-07-09. Stride-1 cost
             * proxy -- see surr_run_dw_fused()'s header. */
            u64_cycles dw_cyc = 0;
            rc = surr_run_dw_fused(d, buf_cur, buf_next,
                                   &p_blob[i], &w_blob[i], &dw_cyc);
            if (rc == 0) surr_layer_cyc[i] = dw_cyc;
        } else {
            u64_cycles t0 = timer_now();
            rc = run_pw_layer_real(d, buf_cur, buf_next, &p_blob[i], &w_blob[i]);
            if (rc == 0) {
                surr_layer_cyc[i] = timer_now() - t0;
                printf("[SURR]   L%d PW Cin=%d Cout=%d %dx%d  %.3f ms\n",
                       i, d->Cin, d->Cout, d->H, d->W,
                       cycles_to_ms(surr_layer_cyc[i]));
            }
        }

        if (rc != 0) {
            printf("[SURR] layer failed L%d\n", i);
            for (int j = 0; j < SURR_NUM_LAYERS; j++) { free_blob(&w_blob[j]); free_blob(&p_blob[j]); }
            return -1;
        }

        {
            uint8_t *tmp = buf_cur;
            buf_cur  = buf_next;
            buf_next = tmp;
        }
    }

    {
        double   total_ms    = cycles_to_ms(g_total_layer_cyc);
        double   dma_ms      = cycles_to_ms(g_total_dma_run_cyc);
        uint64_t total_macs  = g_total_dw_macs  + g_total_pw_macs;
        uint64_t total_bytes = g_total_dw_bytes + g_total_pw_bytes;

        printf("\n========================================\n");
        printf(" SURROGATE TOTALS (%d layers, dummy weights, real HW-measured cycles)\n", SURR_NUM_LAYERS);
        printf("========================================\n");
        printf("[SURR TOTAL] wall time (full) : %.3f ms  (incl. param/weight preload, pack, store)\n", total_ms);
        printf("[SURR TOTAL] DMA/compute-only  : %.3f ms\n", dma_ms);
        printf("[SURR TOTAL] combined MACs     : %.0f  (%.4f GMAC)\n", (double)total_macs, (double)total_macs / 1.0e9);
        printf("[SURR TOTAL] combined bytes    : %.0f  (%.3f MB)\n", (double)total_bytes, (double)total_bytes / 1.0e6);

        /* End-to-end estimate from the per-layer measurements (2026-07-28).
         * SEQUENTIAL = every layer through DRAM, which is what this run does.
         * FUSED = the on-chip DW->PW cascade, where DW runs concurrently with
         * the PW it feeds, so each pair costs max(DW,PW) rather than the sum.
         * DW figures are stride-1 proxies (see surr_run_dw_fused). */
        {
            double seq_ms = 0.0, fused_ms = 0.0;
            printf("----------------------------------------\n");
            for (int li = 0; li < SURR_NUM_LAYERS; li++) {
                printf("[SURR EST] L%d %s : %.3f ms\n", li,
                       (li & 1) ? "PW" : "DW", cycles_to_ms(surr_layer_cyc[li]));
                seq_ms += cycles_to_ms(surr_layer_cyc[li]);
            }
            for (int li = 0; li + 1 < SURR_NUM_LAYERS; li += 2) {
                double dw = cycles_to_ms(surr_layer_cyc[li]);
                double pw = cycles_to_ms(surr_layer_cyc[li + 1]);
                fused_ms += (dw > pw) ? dw : pw;
            }
            printf("[SURR EST] SEQUENTIAL (DRAM round trip) : %.2f ms  -> %.1f fps\n",
                   seq_ms, (seq_ms > 0.0) ? 1000.0 / seq_ms : 0.0);
            printf("[SURR EST] FUSED (DW||PW cascade)       : %.2f ms  -> %.1f fps\n",
                   fused_ms, (fused_ms > 0.0) ? 1000.0 / fused_ms : 0.0);
            printf("[SURR EST]   (720p in; DW timed at stride 1 as a cost proxy;\n");
            printf("[SURR EST]    PW cout rounded up to a multiple of N_OC=%d)\n", PW_N_OC);
        }

        for (int li = 0; li < SURR_NUM_LAYERS; li++) {
            const layer_stat_t *ls = &g_layer_stats[li];
            printf("[SURR LSTAT] L%d %s Cin=%d Cout=%d wall_ms=%.4f dma_run_ms=%.4f\n",
                   li, ls->kind == KIND_DW ? "DW" : "PW", ls->Cin, ls->Cout,
                   ls->wall_ms, ls->dma_run_ms);
        }
        printf("========================================\n");
    }

    for (int i = 0; i < SURR_NUM_LAYERS; i++) {
        free_blob(&w_blob[i]);
        free_blob(&p_blob[i]);
    }

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

    /* See start_global_timer() above: an SD-card boot through the FSBL
     * usually leaves the Global Timer already running, but a JTAG/debugger
     * launch often doesn't. Without this, timer_now() reads a frozen
     * counter and every [*_TIMING] figure below comes out 0.000 ms
     * regardless of real elapsed time. */
    start_global_timer();

    if (init_dmas() != 0) {
        printf("ERROR: DMA init failed\n");
        return -1;
    }

    cfg.verbose = 0;
    cfg.compare_layers = 0;
    cfg.compare_adds = 0;
    cfg.stop_on_fail = 0;
    cfg.dump_failed_outputs = 0;

#if RUN_EDGE_VALIDATION
    /* COMPLETE EDGE ENCODER VALIDATION (2026-08-25).
     * Runs PL analysis -> VQ -> range coding and reports every timing
     * primitive plus one directly measured end-to-end bracket. Takes
     * precedence over the surrogate estimate; set RUN_EDGE_VALIDATION 0 to
     * fall back to the legacy timed path, which is unmodified. */
    {
        extern int edge_validation_run(void);
        rc = edge_validation_run();
    }
#elif RUN_SURROGATE_ESTIMATE
    /* Cascade-hardware variant: runs on the CURRENT bitstream, no rebuild.
     * cnn_run_dw_pw_surrogate() is the older per-layer path and needs LOOPBACK
     * mode (axi_dma_0 S2MM + axi_dma_1 MM2S), both removed by the cascade. */
    rc = cnn_run_surrogate_cascade_estimate();
#else
    rc = cnn_run_full_network(&cfg);
#endif
    printf("main() returning %d\n", rc);
    return rc;
}
/* ============================================================================
 * EDGE VALIDATION SHIM (2026-08-25)
 *
 * Thin adapters so edge_harness.c can drive the SAME validated cascade path
 * the surrogate timing loop uses, without modifying that loop. Everything here
 * is additive; set RUN_EDGE_VALIDATION 0 to compile it out entirely.
 *
 * The six-pair body is byte-identical in structure to surr_run_one_frame(),
 * so T_HOST measured here is directly comparable with the legacy B2.
 * ==========================================================================*/
#if RUN_EDGE_VALIDATION

#include "vq_pq.h"

/* timer adapters -- same clock as every other measurement in this file */
unsigned long long ep_timer_now(void)                 { return (unsigned long long)timer_now(); }
double             ep_cycles_to_ms(unsigned long long c) { return cycles_to_ms((u64_cycles)c); }

/* persistent state for the edge run */
static layer_desc_t  g_edge_descs[SURR_NUM_LAYERS];
static file_blob_t   g_edge_w[SURR_NUM_LAYERS];
static file_blob_t   g_edge_p[SURR_NUM_LAYERS];
static int           g_edge_ready = 0;

/* 2.76 MB SD staging and de-interleave destination, in spare DDR windows */
uint8_t *edge_raw_ptr(void) { return (uint8_t *)DDR_BUF_NEXT_ADDR; }
uint8_t *edge_chw_ptr(void) { return (uint8_t *)DDR_BUF_ADD_ADDR;  }

/* block-5 output. With 6 pairs the last write lands in chainB (p=5 is odd). */
const uint8_t *edge_latent_ptr(void) { return (const uint8_t *)DDR_SKIP2_ADDR; }

/* VQ codebook. NO TRAINED CODEBOOK IS AVAILABLE ON THIS BOARD, so a fixed
 * deterministic synthetic codebook is used. This makes VQ TIMING valid (the
 * search cost is data-independent: 256 codewords are always scanned) but the
 * resulting indices are NOT the deployed codec's indices. Never report rate
 * or distortion from this run. */
static int8_t g_edge_cb[VQ_M * VQ_K * VQ_DSUB];
const int8_t *edge_codebook_ptr(void)
{
    static int done = 0;
    if (!done) {
        uint32_t s = 0xC0FFEEu;
        for (size_t i = 0; i < sizeof g_edge_cb; i++) {
            s ^= s << 13; s ^= s >> 17; s ^= s << 5;
            g_edge_cb[i] = (int8_t)(s & 0xFF);
        }
        done = 1;
    }
    return g_edge_cb;
}

void edge_reset_accums(void)
{
    g_acc_pack = g_acc_prog = g_acc_cache = g_acc_hw = g_acc_total = 0;
    g_cascade_accum = 1;
}

/* The per-pair CPRINT diagnostics are ~12 UART lines per pair, i.e. ~72 lines
 * per frame, and they sit INSIDE hw_dw_pw_cascade_l0_l1 -- therefore inside the
 * T_EDGE_DIRECT bracket. At UART speed that is tens of ms of pure serial time
 * charged to the encoder. Silence them for the whole measured run. */
void edge_set_quiet(int q) { g_cascade_quiet = q; }

void edge_read_accums(double *pack, double *prog, double *cache,
                      double *pl, double *total)
{
    *pack  = cycles_to_ms(g_acc_pack);
    *prog  = cycles_to_ms(g_acc_prog);
    *cache = cycles_to_ms(g_acc_cache);
    *pl    = cycles_to_ms(g_acc_hw);
    *total = cycles_to_ms(g_acc_total);
}

/* ---------------------------------------------------------------------------
 * Per-pair silicon timing (2026-08-28).
 *
 * These only READ the per-pair accumulators that hw_dw_pw_cascade_l0_l1 already
 * maintains silently. Nothing is added inside the timed bracket: the
 * accumulate block runs after t_end, and the reads below happen after the whole
 * frame returns. g_cascade_quiet stays 1 throughout, so no CPRINT/UART traffic
 * is introduced -- the timed region is bit-for-bit the code that produced the
 * headline B1/B2 numbers.
 *
 * The caller snapshots after each frame and differences, which is what gives
 * per-frame min/max without any per-frame state living in main.c.
 * ------------------------------------------------------------------------- */
int edge_num_pairs(void)  { return SURR_NUM_LAYERS / 2; }
int edge_split_mode(void) { return CASCADE_ENGINE_SPLIT; }

void edge_reset_pair_accums(void)
{
    for (int i = 0; i < SURR_MAX_PAIRS; i++) {
        g_acc_hw_p[i] = g_acc_pack_p[i] = g_acc_prog_p[i] = g_acc_cache_p[i] = 0;
    }
}

void edge_read_pair_cycles(unsigned long long *hw, unsigned long long *pack,
                           unsigned long long *prog, unsigned long long *cache,
                           int n)
{
    for (int i = 0; i < n && i < SURR_MAX_PAIRS; i++) {
        if (hw)    hw[i]    = (unsigned long long)g_acc_hw_p[i];
        if (pack)  pack[i]  = (unsigned long long)g_acc_pack_p[i];
        if (prog)  prog[i]  = (unsigned long long)g_acc_prog_p[i];
        if (cache) cache[i] = (unsigned long long)g_acc_cache_p[i];
    }
}

/* Output geometry of pair p is the geometry of its PW half, layer 2p+1.
 * Taken from the live descriptors rather than restated, so the group counts
 * cannot drift from the schedule the hardware actually ran. */
void edge_read_pair_dims(int *cout, int *hout, int *wout, int *groups, int n)
{
    for (int p = 0; p < n && p < SURR_MAX_PAIRS; p++) {
        int c = 0, h = 0, w = 0;
        if (g_edge_ready) {
            tensor_shape_t s = get_layer_output_shape(&g_edge_descs[2 * p + 1]);
            c = s.C; h = s.H; w = s.W;
        }
        if (cout)   cout[p]   = c;
        if (hout)   hout[p]   = h;
        if (wout)   wout[p]   = w;
        if (groups) groups[p] = (h * w) / 8;      /* 8 pixels per group */
    }
}

/* One complete analysis transform: six DW->PW pairs, chained on-chip.
 * Mirrors surr_run_one_frame() exactly. */
int edge_run_six_pairs(const uint8_t *gm_in_frame)
{
    uint8_t *chainA = (uint8_t *)DDR_SKIP0_ADDR;
    uint8_t *chainB = (uint8_t *)DDR_SKIP2_ADDR;
    uint8_t *gm_in  = (uint8_t *)DDR_SKIP1_ADDR;
    uint8_t *raw_in = (uint8_t *)gm_in_frame;
    const int npairs = SURR_NUM_LAYERS / 2;

    if (!g_edge_ready) {
        surr_build_schedule(g_edge_descs);
        for (int j = 0; j < SURR_NUM_LAYERS; j++) {
            int rc = (g_edge_descs[j].kind == KIND_DW)
                   ? surr_alloc_dw_blobs(g_edge_descs[j].Cin,
                                         &g_edge_w[j], &g_edge_p[j])
                   : surr_alloc_pw_blobs(g_edge_descs[j].Cin, g_edge_descs[j].Cout,
                                         &g_edge_w[j], &g_edge_p[j]);
            if (rc != 0) { printf("[EDGE] blob alloc failed at layer %d\n", j); return -1; }
        }
#if SURR_GM_IN_NONCACHED
        /* CRITICAL: the surrogate maps gm_in NORM_NONCACHE once at setup.
         * Skipping this leaves gm_in cached, so dirty lines write back lazily
         * DURING the DMA burst and contend with the PL for DDR -- measured as
         * a ~2.7x inflation of the HW window on the first run. One-time cost. */
        for (unsigned s = 0; s < 4u; s++)
            Xil_SetTlbAttributes((INTPTR)((UINTPTR)gm_in + (UINTPTR)s * 0x100000u),
                                 NORM_NONCACHE);
        printf("[EDGE] gm_in @ %p mapped NORM_NONCACHE across 4 MB\n", (void *)gm_in);
#endif
        g_edge_ready = 1;
    }

    for (int p = 0; p < npairs; p++) {
        layer_desc_t dw = g_edge_descs[2 * p];
        layer_desc_t pw = g_edge_descs[2 * p + 1];
        const int chained = SURR_CHAIN_PAIRS && (p > 0);
        uint8_t *out_buf = (p % 2 == 0) ? chainA : chainB;
        uint8_t *in_buf  = (p == 0) ? gm_in : ((p % 2 == 1) ? chainA : chainB);
        g_acc_pair = p;
        if (hw_dw_pw_cascade_l0_l1(&dw, &pw, raw_in, in_buf, out_buf,
                                   &g_edge_p[2 * p],     &g_edge_w[2 * p],
                                   &g_edge_p[2 * p + 1], &g_edge_w[2 * p + 1],
                                   SURR_IN_FLAGS(chained)) != 0) return -1;
    }
    return 0;
}

extern int edge_validation_run(void);
#endif /* RUN_EDGE_VALIDATION */
