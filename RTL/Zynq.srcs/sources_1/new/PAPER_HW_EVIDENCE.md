# Hardware Evidence Package — DW/PW Accelerator (ZC702)

**Compiled 2026-07-30.** Numbered to match the 25-section hardware information
request, so each section can be lifted directly into the paper.

**Status legend:** ✅ have · ⚠️ partial · ❌ missing

> ### PROVENANCE RULES — apply to every number below
> - **MEASURED** = read from hardware on the board.
> - **REPORT** = Vivado post-route implementation report. NOT a measurement.
> - **DERIVED** = arithmetic from measured/report values.
> - **MODEL** = analytical prediction.
>
> Two brackets in the driver log must **never** be quoted: `chunk done` (it
> routinely exceeds 1 beat/PL-cycle, which is physically impossible) and the
> **cold-pass** per-pair numbers (inflated ~2.6× by blocking UART inside the
> timed region). Only the WARM per-pair table is usable.
>
> **All performance figures were obtained with synthetic dummy weights.
> No output has ever been checked against a software reference (§21).**

---

## 0. Master per-layer specification table

> ### ✅ CURRENT PER-PAIR NUMBERS (2026-08-08, `N_OC=32`) — use these
> The `cyc/grp` and `HW ms` columns in the table below are the **`N_OC=16`**
> figures and are superseded. Geometry, MAC counts and DMA byte counts are
> unchanged and remain valid.
>
> | blk | cyc/grp | HW ms | bound by |
> |---|---|---|---|
> | 0 | 26.1 | 7.50 | PW (drain) |
> | 1 | 75.2 | 5.42 | DW input + tail |
> | 2 | 149.6 | 2.69 | DW input + tail |
> | 3 | 72.0 | 1.30 | PW |
> | 4 | 110.0 | 1.98 | PW |
> | 5 | 174.4 | 3.14 | PW |
> | **Σ** | | **22.03** | frame 29.552 ms, 33.8 fps |
>
> Blocks 0, 3, 4 and 5 match the cost model to ~1% (§16). Blocks 1 and 2 are
> read-bound: b2's floor is 230,400 cycles = 2.304 ms against 2.69 measured, the
> difference being the documented DW-bound tail.
>
> **Fusion traffic is now byte-exact** — intermediate 3,916,800 B, every pair
> `produced == written`. The earlier 3,916,792 was the P0 corruption (§21).

Workload: **ImageEncoderLite** (NeuralImageCodec encoder), 720×1280 input,
6 depthwise-separable blocks. Each block = DW 3×3 (groups=C) → BN → ReLU6 →
PW 1×1 → BN → ReLU6; block 5's PW has no activation.

| blk | DW C | DW H×W | s | PW Cin→Cout | PW H×W | groups | batches | cyc/grp (meas) | HW ms (meas) | DW MAC | PW MAC | MM2S B | S2MM B |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 0 | 3 | 720×1280 | 2 | 3→16 | 360×640 | 28,800 | 1 | 36.1 | **10.38** | 6,220,800 | 11,059,200 | 2,764,800 | 3,686,400 |
| 1 | 16 | 360×640 | 2 | 16→32 | 180×320 | 7,200 | 2 | 77.3 | **5.57** | 8,294,400 | 29,491,200 | 3,686,400 | 1,843,200 |
| 2 | 32 | 180×320 | 2 | 32→32 | 90×160 | 1,800 | 2 | 149.0 | **2.68** | 4,147,200 | 14,745,600 | 1,843,200 | 460,800 |
| 3 | 32 | 90×160 | 1 | 32→32 | 90×160 | 1,800 | 2 | 104.0 | **1.87** | 4,147,200 | 14,745,600 | 460,800 | 460,800 |
| 4 | 32 | 90×160 | 1 | 32→64 | 90×160 | 1,800 | 4 | 180.0 | **3.24** | 4,147,200 | 29,491,200 | 460,800 | 921,600 |
| 5 | 64 | 90×160 | 1 | 64→64 | 90×160 | 1,800 | 4 | 308.4 | **5.55** | 8,294,400 | 58,982,400 | 921,600 | 921,600 |
| **Σ** | | | | | | 43,200 | | | **29.30** | **35,251,200** | **158,515,200** | **10,137,600** | **8,294,400** |

`batches = cout_rounded / N_OC`, `groups = PW pixels / 8`, `cyc/grp` from warm
per-pair HW time ÷ groups at 100 MHz. MACs are DERIVED (DW = C·H_out·W_out·9;
PW = Cin·Cout·pixels).

---

## 1. FPGA platform ✅

| | |
|---|---|
| Board | **Xilinx ZC702** |
| Device | **xc7z020clg484-1** |
| Family / speed grade | Zynq-7000 / **−1** |
| Toolchain | **Vivado + Vitis 2020.2** |
| RTL language | **SystemVerilog** |
| Target clock | 100 MHz (PS `FCLK0`) |
| Achieved (post-route) | **100 MHz, closes** — WNS +0.321 ns |
| Runs on real hardware? | **Yes** — all performance figures MEASURED on board |
| External memory | **DDR3**, Micron **MT41J256M8 HX-15E** |
| DDR bus width | **32-bit** |
| DDR clock | **533.33 MHz** (DDR3-1066) |
| DDR theoretical peak | **4.27 GB/s** (32 b × 1066.67 MT/s) — DERIVED |
| Fabric↔memory interface | **AXI4 via 2× AXI DMA → AXI SmartConnect → PS AXI HP0/HP1** |
| HP ports | HP0 + HP1 enabled, **64-bit each**; HP2/HP3 unused |
| HP peak bandwidth | **800 MB/s per port**, 1.6 GB/s aggregate @100 MHz — DERIVED |
| Host processor | **PS Cortex-A9** (dual-core; **one core used**), bare-metal standalone |

---

## 2. Block diagram ❌ (topology known, diagram not drawn)

Topology, from `Zynq.srcs/sources_1/bd/hw/hw.bd`:

```
                    ┌──────────── PS7 (Cortex-A9, DDR3 controller) ────────────┐
   DDR3 ────────────┤  HP0 (64b, read)              HP1 (64b, write)          │
                    └──────┬──────────────────────────────────┬───────────────┘
                           │                                  │
                    smartconnect_0                     smartconnect_1
                           │                                  │
                 ┌─────────▼─────────┐              ┌─────────▼─────────┐
                 │ axi_dma_0         │              │ axi_dma_1         │
                 │ MM2S only (64b)   │              │ S2MM only (64b)   │
                 └─────────┬─────────┘              └─────────▲─────────┘
                    64b AXIS│                                 │64b AXIS
                 ┌─────────▼──────────────┐   64b AXIS  ┌─────┴──────────────────┐
                 │ dw_fused_axi_0         ├────────────►│ pw_single_oc_axis_axi_0│
                 │  AXI-Lite cfg          │             │  AXI-Lite cfg          │
                 │  in FIFO 2048×64       │             │  in FIFO / out 8192×65 │
                 │  dw_banked_window_8x   │             │  pixel-major core      │
                 │  8× conv_mac_array     │             │  16×8 MAC grid         │
                 │  8× ppu                │             │  shadow BRAM + 8 PPU   │
                 └────────────────────────┘             └────────────────────────┘
                           ▲                                  ▲
                           └────── AXI-Lite (PS GP0) ─────────┘
```

- **Off-chip:** input frame, all inter-block feature maps, final output.
- **On-chip:** line buffers, pend/half buffers, weight + param RAMs, pixel
  double-buffers, shadow accumulator BRAM, stream FIFOs.
- **No codebook memory exists** (§7).

**Still needed:** a drawn, labelled figure with bus widths and data directions.

---

## 3. Top-level architecture ✅

- **Input:** one 720p frame, group-major packed int8, in DDR.
- **Output:** encoder feature map (64×90×160 int8) in DDR.
- **Granularity:** processes **one DW→PW pair (two fused layers) per invocation**;
  the 6 pairs run back-to-back, chained through DDR.
- **Supported layer types:** 3×3 depthwise (stride 1 or 2, pad 1) and 1×1
  pointwise (stride 1). No standard dense conv, no pooling, no residual add.
- **DW and PW are separate hardware blocks**, connected **on-chip by AXI-Stream**
  — the DW output never returns to DDR within a pair. This is the fusion.
- **Programmable, not fixed-function:** all layer geometry is runtime
  configurable via AXI-Lite.
- **Runtime configurable:** `CIN_RUN`, `N_GROUPS`, `IMG_WIDTH`, `N_ROWS`,
  `stride2`, `zp_in`, `zp_out`, `relu_en`, `TILE_PIXELS`, `COUT_RUN`, all
  weights and requant params.
- **Requires resynthesis:** `N_LANES`, `N_OC`, `CIN_MAX`, `COUT_MAX`,
  `MAX_CG_PRODUCT`, `ACC_WIDTH`, FIFO depths.
- **Encoder only** is accelerated. There is no decoder in this design.
- **On the CPU:** input transpose to group-major (`pack`), weight/param
  programming, DMA arm/poll, cache maintenance. Nothing else.

---

## 4. Processing-element organization ✅

| | DW engine | PW engine |
|---|---|---|
| Parallel lanes | 8 (`N_LANES`) | 8 pixels (`N_LANES`) |
| MACs per lane | 9 (one 3×3 kernel) | `N_OC` = 16 output channels |
| Physical multipliers | 72 | 128 (64 DSPs × 2 packed) |
| MAC/cycle | **72** | **128** |
| DSPs used for MACs | 72 (1 MAC/DSP, no packing) | **64 (2 MAC/DSP, packed)** |
| DSP total in module | 88 | 132 |
| Multiplier inputs | 8-bit act (zp-subtracted, 9-bit signed) × 8-bit weight | same, two 9-bit acts packed into one 25-bit operand |
| Accumulator width | `ACC_WIDTH` = **32** | `ACC_WIDTH` = **24** |
| Pipeline depth | 9 (MAC P-cascade) + 8 (PPU) + 1 = **18** | MAC 4 stages + PPU |
| Initiation interval | 1 (streaming) | 1 per MAC issue |

**Combined compute roof = 200 MAC/cycle = 20.0 GMAC/s @100 MHz** (DERIVED).

> ⚠️ **CORRECTED 2026-08-08.** This section previously said "the remaining ~84
> are PPU requant multipliers, the `tile_groups × cout_run` product, and address
> arithmetic". **That was a guess and it was wrong.** Measured from the routed
> checkpoint, the 220 are:
>
> | function | DSPs |
> |---|---|
> | DW MAC array (8 lanes × 9 taps) | 72 |
> | DW PPU requant (8 × 2) | 16 |
> | PW MAC grid — multiply (`N_OC × N_LANES/2`) | 64 |
> | **PW accumulators** (`p_0_out*`) | **50** |
> | PW PPU requant (8 × 2) | 16 |
> | PW shell (`total_groups_r`, `pp_lo/hi`) | 2 |
>
> Only **34** are PPU/shell, not 84. The 50 accumulators are adders Vivado
> absorbed into DSP48 ALUs with `first_ic` driving OPMODE — which is also why the
> §19 critical path has **zero logic levels**: it ends on a DSP control pin.
> The mapping is **opportunistic**, not required: at `N_OC=32` Vivado yields
> `acc = 0` DSPs with no source change. Full ledger and the cost-per-DSP table
> in §28.1–28.2.

**PW DSP packing** (`pw_pixel_major_core.sv`): two 9-bit activations are packed
into one 25-bit operand, one multiply yields both products, unpacked at
`[15:0]` and `[32:16]`. Grid cost is therefore `N_OC × N_LANES/2` DSPs.

### Parameter meanings

| parameter | meaning |
|---|---|
| `N_LANES` = 8 | pixels processed in parallel; also the group size (8 px per 64-bit beat) |
| `N_OC` = 16 | output channels computed in parallel by PW |
| `CIN_RUN` | input channels for the current layer (runtime) |
| `COUT_RUN` | `cout_rounded`, output channels rounded up to a multiple of `N_OC` |
| `N_GROUPS` | groups per row = `ceil(W/8)` |
| `TILE_PIXELS` | total pixels PW processes in one invocation (whole layer here) |
| `MAX_CG_PRODUCT` = 2048 | line-buffer depth; must be ≥ `max(N_GROUPS × CIN_RUN)` |
| `CIN_MAX` = 240, `COUT_MAX` = 240 | compile-time channel ceilings |

---

## 5. Dataflow ✅

**DW: input-stationary streaming.** The windower holds vertical context; the
kernel (9 weights per channel) is fetched per channel and consumed immediately.
**PW: output-stationary with weight reuse across pixels.** Accumulators
`acc[N_OC][N_LANES]` stay resident for the whole pixel group; each input-channel
step broadcasts 8 activations to all 16 output-channel banks.

- **Stays in the core:** DW vertical line context; PW accumulators.
- **Broadcast:** PW activations — the same 8 pixels feed all `N_OC` weight banks.
- **Streamed:** activations (both engines), DW output → PW input on-chip.
- **Re-read:** nothing within a layer. Feature maps are written once, read once.
- **Partial sums:** PW `acc[][]` registers → shadow BRAM → PPU. Never leave the chip.
- **To DDR:** only after the PW's PPU, once per pair.

### Loop nest

```
DW (dw_banked_window_8x):
  for row r in 0..H-1:                    # sequential
    for group g in 0..G-1:                # sequential (+1 flush slot)
      for channel c in 0..C-1:            # sequential, fastest-varying
        emit 8 windows in parallel        # UNROLLED across 8 lanes
          for kr in 0..2: for kc in 0..2: # UNROLLED, 9-deep DSP P-cascade

PW (pw_pixel_major_core):
  for pixel group in 0..tile_groups-1:    # sequential
    for oc_batch in 0..batches-1:         # sequential
      for ic in 0..cin_run-1:             # sequential, 1 cycle each (PIPELINED)
        for oc in 0..N_OC-1:              # UNROLLED across weight banks
          for lane in 0..N_LANES-1:       # UNROLLED (2 lanes share one DSP)
            acc[oc][lane] += (act[lane]-zp) * w[oc][ic]
    drain acc -> shadow BRAM -> PPU       # N_OC beats, overlaps next group's load
```

- **Unrolled:** 8 lanes (both engines); 9 kernel taps (DW); `N_OC` banks (PW).
- **Pipelined:** the `ic` loop (II = 1); DW is fully streaming.
- **Sequential:** rows, groups, output-channel batches.
- **Tiled:** output channels, into `batches = cout_rounded / N_OC`.

---

## 6. Input activation format ✅

- **Layout: custom group-major, channel-minor.**
  `for row r: for group g: for channel c:` one 8-pixel beat.
- **8 bits per activation**, unsigned, zero-point 128.
- **8 activations per 64-bit AXI word**; lane *l* at byte *l* (little-endian).
- **AXIS width 64-bit** on both DW and PW, in and out.
- **Padding:** zero-point (`zp_in`) substitution, generated in hardware by the
  windower's per-tap bounds check — no padded copy is stored in DRAM.
- **Edges:** each of the 3 tap columns per lane gets an independent bounds check
  (10-position `X_byte`/`is_x_valid` scheme, k=0 is lane 0's left neighbour,
  k=9 is lane 7's right neighbour).
- **Sliding window:** two banked line buffers (`line0`/`line1`) addressed flat by
  (group, channel) with row-parity ping-pong, plus a per-channel `pend_ram`
  giving one-group-delayed emission for left/right continuity.
- **Software rearrangement: YES** for block 0 only (`pack_input_group_major`).
  Blocks 1–5 consume the previous PW output directly, unrearranged.
- **8 pixels loaded per cycle** (1 beat/cycle).
- **DMA burst length:** `axi_dma_0` MM2S 8, S2MM 16; `axi_dma_1` MM2S 16,
  S2MM 256. `sg-length-width` = 26 → 64 MB max single transfer.

---

## 7. Weight, index and codebook format ⚠️

**There is no vector codebook in this design.** No codebook memory, no index
storage, no lookup logic, no decompression. Every codebook-related field in the
request is **N/A**. If the paper requires a codebook contribution, that work does
not exist yet — see the open scope question.

**Weights that do exist:**

| | DW | PW |
|---|---|---|
| Format | 9 int8 taps per channel | int8, `Cin × Cout` |
| Storage | BRAM, addressed by channel | BRAM, `N_OC` banks |
| Bank select | — | `oc % N_OC` |
| Bank offset | — | `(oc / N_OC) × Cin` |
| Load path | AXI-Lite, `W0/W1/W2` (W2 commits) | AXI-Lite, `PW_REG_W_BASE + 4·ic` |
| Read ports | 1 per channel | 1 per bank, `N_OC` banks in parallel |
| Reads/cycle | 9 taps (one channel) | `N_OC` weights (one per bank) |
| Reprogrammed | every layer, every frame | every layer, every frame |

Requant params (bias 32b, mult 32b, shift 8b) live in a separate param BRAM,
addressed by channel (DW) or output channel (PW).

---

## 8. Convolution execution schedule ✅

**Standard convolution: not supported.**

### Depthwise
- **One lane per pixel, not per channel.** 8 spatially-adjacent pixels of a
  *single* channel are processed in parallel; channels are sequential.
- All 9 kernel elements per cycle (unrolled P-cascade).
- Weights supplied per channel from BRAM, indexed by the windower's `ch_out` tag.
- No cross-cycle accumulation — each output is one complete 3×3 dot product.
- **Utilisation does not drop for narrow layers**; it drops only if `W` is not a
  multiple of 8 (the last group has idle lanes).

### Pointwise
- **1 input channel accumulated per cycle**; `N_OC` = 16 output channels in parallel.
- Activation broadcast: one 8-pixel group feeds all 16 banks.
- Weight broadcast: one weight per bank per cycle.
- Partial sums in `acc[16][8]` registers, then shadow BRAM.
- **Passes required = `cout_rounded / N_OC`** (`batches`), 1–4 for this model.

### Fused DW→PW
- **Fusion boundary: DW's PPU output → PW's input FIFO, on-chip AXI-Stream.**
- DW output is **never materialised in DDR** within a pair.
- Intermediate FIFOs: DW out 2048×64, PW in (BD-configured), PW out 8192×65.
- **DDR transfers eliminated: one full write + one full read per pair.**
  MEASURED from the DW beat counters (2026-07-30), not estimated:
  ```
  intermediate (DW output) total   3,916,792 B
  avoided by fusion (write+read)   7,833,584 B
  actual DMA traffic              18,432,000 B
  unfused would have been         26,265,584 B
  TRAFFIC REDUCTION                    29.8%
  ```
  (An earlier draft said "~16.6 MB/frame saved" — that was WRONG; it multiplied
  from PW output bytes rather than DW output bytes. The correct figure is
  7.83 MB.)
- **Rate mismatch handling:** DW throttles on PW's `prog_full` (back-pressure),
  with the threshold set clear of DW's 18-cycle pipeline so in-flight beats
  always have somewhere to land.
- **Both blocks run simultaneously.** MEASURED: DW is fully hidden behind PW in
  5 of 6 blocks; in block 2 DW binds and the cascade costs
  `DW_beats + PW pipeline tail`.

---

## 9. Tiling strategy ⚠️

- **Output channels: tiled** into `batches = cout_rounded / N_OC`.
- **Input channels: not tiled** — `CIN_MAX` = 240 covers all layers (max 64).
- **Spatial: NOT tiled.** `TILE_PIXELS` = the whole layer (up to 230,400 px).
- **Kernel: not tiled** (3×3 fully unrolled).
- **Batch size = 1.**

Chosen because on-chip storage is sized by `CIN_MAX` and `MAX_CG_PRODUCT`, not
by frame size, so no layer needs spatial splitting. Incomplete tiles arise only
when `Cout` is not a multiple of `N_OC` — then `cout_rounded > Cout` and the
padding channels are computed and discarded (**zero padding waste for this
model**: 16/32/32/32/64/64 are all multiples of 16). Unused lanes are **idle,
not clock-gated**.

---

## 10. On-chip memory ⚠️ (structures known; per-memory table incomplete)

| memory | module | width × depth | purpose | notes |
|---|---|---|---|---|
| `line0`, `line1` | DW windower | 64 b × 2048 each | vertical context, row-parity ping-pong | BRAM; deferred write-back to avoid cross-port collision |
| `pend_ram` | DW windower | 216 b × `CIN_MAX` | one-group-delayed horizontal context + left continuity | per channel |
| `half_ram` | DW windower | 220 b × `CIN_MAX` | stride-2 even-group park (27 masked bytes + 4 valid bits) | instantiated regardless of stride |
| `wram` | DW core | 72 b × `CIN_MAX` | 9 int8 taps per channel | 1-cycle registered read |
| `pram` | DW core | 72 b × `CIN_MAX` | bias/mult/shift per channel | |
| in FIFO | DW shell | 64 b × 2048 | MM2S → core | `xpm_fifo_sync`, FWFT |
| out FIFO | DW shell | 64 b × 2048 | core → PW | `prog_full` throttle at depth−32 |
| pixel buf A/B | PW core | 64 b × `CIN_MAX` | **double-buffered** activation group | ping-pong: load next while computing |
| weight BRAM | PW core | int8, `N_OC` banks | `(oc/N_OC)·Cin + ic` | `N_OC` parallel read ports |
| param BRAM | PW core | 72 b × `COUT_MAX` | bias/mult/shift per oc | |
| shadow BRAM | PW core | `ACC_WIDTH`·`N_LANES` × `N_OC` | accumulator snapshot for PPU drain | **single-buffered — this is the bottleneck (§16)** |
| out FIFO | PW shell | 65 b × 8192 | data + TLAST tag | tag travels with its beat |

**Totals (REPORT, post-route):** BRAM tiles **73.5 / 140 (52.5%)** —
DW 24 RAMB36 + 1 RAMB18, PW 41 RAMB36 + 4 RAMB18, DMAs/interconnect ~5.

- **Peak requirement is set by `MAX_CG_PRODUCT`, not by frame size.** Worst
  `N_GROUPS × CIN_RUN` for this model = **1280** (blocks 1, 2, 5), against the
  2048 provisioned. **No layer exceeds any buffer.**
- **Banking:** DW line buffers are addressed flat by `(group, channel)`;
  conflicts avoided by deferring write-back one cycle so read and write never
  hit the same address.
- **Duplication for read ports:** PW's weight BRAM is split into `N_OC` banks
  precisely to give `N_OC` concurrent reads.

---

## 11. Shadow BRAM and PPU ✅ (still present; this is the current design)

- **Written into shadow BRAM:** *complete* accumulator values for one pixel
  group and one output-channel batch — **not** partial sums across batches.
- **Format:** `ACC_WIDTH` = 24-bit signed, `N_LANES` = 8 values per entry, `N_OC`
  entries per batch.
- **Copy takes `N_OC` cycles**, one output channel per cycle.
- **PPU starts** when the main FSM reaches `S_BATCH_DONE` and issues
  `P_PARAM_WAIT` (1 cycle for param BRAM latency), then `P_DRAIN`.
- **Overlap:** compute and post-processing **partially** overlap. `S_COMPUTE`
  cannot exit until `ppu_st == P_IDLE`, so batch *N+1* waits for batch *N*'s
  drain to fully retire. **This serialisation is the measured bottleneck (§16).**
- **8 PPUs** (one per lane), 1 value per PPU per cycle, pipeline depth 8.
- **Requant:** `out = clamp( ((acc + bias) * mult) >>_round shift ) + zp_out`,
  with rounding on the shift, saturation to uint8, optional ReLU.
- **Output packing:** 8 results → one 64-bit beat, group-major, channel-minor.

---

## 12. Control architecture ✅

**PW main FSM:** `S_IDLE → S_LOAD_FIRST → S_COMPUTE → S_BATCH_DONE → S_WAIT_PPU → S_DONE`,
plus two concurrent sub-FSMs — a **load** FSM (preloads the next pixel group
into the inactive buffer) and a **PPU** FSM (`P_IDLE → P_PARAM_WAIT → P_DRAIN`).

**DW control** is a 3-level counter (row → group → channel) with a per-row
horizontal flush slot and one extra vertical flush row.

### Configuration registers

**DW (`dw_fused_axi`, AXI-Lite):**
```
0x00 CTRL      bit0 start, bit1 clear_done
0x04 STATUS    bit0 done_sticky, bit1 busy
0x08 CIN_RUN   0x0C N_GROUPS   0x30 IMG_WIDTH   0x34 N_ROWS
0x10 ZP_RELU   [7:0] zp_in [15:8] zp_out [16] relu_en [17] stride2
0x14 CH_ADDR   0x18/0x1C/0x20 W0/W1/W2 (W2 commits 9 weights)
0x24/0x28/0x2C BIAS/MULT/SHIFT (SHIFT commits)
0x38..0x4C     debug: consumed / written / produced / winstate / flags / wincfg
```

**PW (`pw_single_oc_axis_axi`, AXI-Lite):**
```
0x000 CTRL   0x004 STATUS   0x008 TILE_PIXELS   0x00C CIN_RUN
0x010 ZP_RELU  0x014 BIAS  0x018 MULT  0x01C SHIFT  0x020 STATUS2
0x024 OC_SEL  0x028 COUT_RUN  0x02C W_BRAM_OFF  0x030 PARAM_ADDR
0x100 W_BASE (weight window)
```

- **Layer start:** software writes geometry + weights, then pulses `CTRL[0]`.
  **PW is started before DW** so its FIFO reset precedes any DW output.
- **Completion:** `done` is sticky in `STATUS[0]`; software **polls** — no interrupts.
- **Timeout/error:** driver-side cycle-count timeouts with a full state dump
  (both cores' STATUS, DMA SR/CR, DW debug counters).
- **Descriptors:** generated by software per layer; **no hardware descriptor engine**.
- **Back-to-back layers:** yes, but each requires full weight reprogramming
  (2.44 ms/frame across 12 layers) because the layers share the weight RAMs.

---

## 13. AXI and DMA ✅

| | |
|---|---|
| AXI protocol | AXI4 (memory-mapped), AXI4-Stream (datapath), AXI4-Lite (control) |
| AXIS data width | **64-bit** |
| MM side width | **64-bit** (all four DMA width params) |
| AXI clock | 100 MHz, single domain |
| DMA engine | **Xilinx AXI DMA v7.1**, Direct Register (Simple) mode, **no SG** |
| Master ports | `axi_dma_0` MM2S → HP0; `axi_dma_1` S2MM → HP1 |
| Channels | `axi_dma_0`: MM2S only. `axi_dma_1`: S2MM only |
| Burst length | dma_0 MM2S 8 / S2MM 16; dma_1 MM2S 16 / S2MM 256 |
| Max transfer | `sg-length-width` 26 → **64 MB** |
| DRE | dma_0 MM2S yes; dma_1 S2MM yes |
| Transfers overlap compute? | **Yes** — MM2S streams while both cores run |
| Ping-pong | Yes, on the *chained buffers* (`chainA`/`chainB`) between pairs |
| Alignment | Buffers 1 MB-aligned |
| CPU per transfer | arm + poll `XAxiDma_Busy` (no interrupts) |
| DMA vs compute time | DMA fully overlapped; **not separable** from the HW bracket |

---

## 14. Memory bandwidth ✅

### Theoretical
```
HP port width       64 bit          HP ports used        2 (HP0 read, HP1 write)
Peak read           800 MB/s        Peak write           800 MB/s
Combined peak       1.6 GB/s        DDR3 peak            4.27 GB/s (not binding)
```

### Actual (MEASURED traffic, DERIVED rates)
```
MM2S (activations in)   10,137,600 B/frame
S2MM (activations out)   8,294,400 B/frame
Total DMA               18,432,000 B/frame  (18.43 MB)
CPU pack traffic         5,529,600 B/frame  (block 0 only, read+write)
Weights/params          reprogrammed per frame, AXI-Lite (not DMA)

Sustained (frame time)  509 MB/s
Across HW window        629 MB/s     reads 346 MB/s (43% of 800)
                                     writes 283 MB/s (35% of 800)
Arithmetic intensity    10.51 MAC/byte of DMA traffic
```

### Fusion traffic, MEASURED from DW beat counters (2026-07-30)

| blk | consumed | written | produced | DW out B | implied C | padded? |
|---|---|---|---|---|---|---|
| 0 | 345,600 | 86,400 | 86,400 | 691,200 | **3.00** | no |
| 1 | 460,800 | 115,200 | 115,200 | 921,600 | **16.00** | no |
| 2 | 230,400 | 57,600 | **57,599** ⚠️ | 460,792 | 32.00 | no |
| 3 | 57,600 | 57,600 | 57,600 | 460,800 | 32.00 | no |
| 4 | 57,600 | 57,600 | 57,600 | 460,800 | 32.00 | no |
| 5 | 115,200 | 115,200 | 115,200 | 921,600 | **64.00** | no |

`implied C = produced / output groups`. **Block 0 streams exactly 3 channels —
DW does not pad**, and every block matches its `Cin` exactly.

⚠️ **Block 2 anomaly: `produced` = `written` − 1.** See §21.

- Traffic is **calculated from tensor sizes**, which here equal the DMA transfer
  lengths exactly (verified against the driver's armed byte counts and the DW
  `consumed`/`written`/`produced` counters).
- **No padding or alignment bytes** are included — every layer's width is a
  multiple of 8, so no ragged groups exist.
- **No codebook traffic** (none exists).
- **Fusion eliminates ~16.6 MB/frame** of intermediate DW traffic (§8).

---

## 15. Operation-count convention ⚠️ — DECISION REQUIRED

```
MACs per frame = 193,766,400   (DW 35,251,200 = 18.2% | PW 158,515,200 = 81.8%)
```
Counted: **convolution MACs only** — DW `C·H_out·W_out·9`, PW `Cin·Cout·pixels`.
**Not counted:** bias add, requantisation multiply/shift, ReLU, zero-point
subtraction, padding. Vector lookup: N/A.

```
Throughput = operations per frame / end-to-end frame time
           = 193,766,400 × 33.8 fps = 6.55 GMAC/s          [2026-08-08]
           = 13.11 GOP/s   IF 1 MAC = 2 ops
```
(Superseded: 27.6 fps → 5.35 GMAC/s → 10.70 GOP/s, the `N_OC=16` figure.)

⚠️ **The 1-vs-2 ops/MAC choice is not yet made.** It must be stated explicitly
and matched to whatever the comparison table uses — it is the single easiest
way for an accelerator comparison to be misread.

---

## 16. Cycle-count model ✅

> ### ✅ CURRENT MODEL (2026-08-08) — validated against hardware to 2.4%
> ```
>   cyc/group = B·cin_run + Q_last + 6B + 1
>       B      = ceil(cout_run / N_OC)
>       Q_last = cout_run − (B−1)·N_OC        [partial-batch drain]
> ```
> Four terms: compute; the one final drain that does **not** overlap; per-batch
> overhead; per-group overhead. Fitted in simulation to <0.05 cycles across
> `N_OC ∈ {8,16,32}` and 20+ shapes, then checked on silicon:
>
> | blk | predicted | measured | |
> |---|---|---|---|
> | 0 | 26 | **26.1** | |
> | 3 | 71 | **72.0** | |
> | 4 | 109 | **110.0** | |
> | 5 | 173 | **174.4** | |
>
> Whole frame **21.5 predicted vs 22.03 measured — 2.4%**. Blocks 1 and 2 are
> DW-input-bound, where the cascade costs `DW_beats + tail`, not `max()`.
>
> **Correction to the old model below:** the "≈21 cycles per group" constant was
> `Q_last(16) + 1`, i.e. the shadow-copy length plus one — **not** a fixed
> overhead. The true per-group constant is **1 cycle**. That misreading pointed
> optimisation work at the wrong target for some time; the recoverable time is
> the *copy* (§29.7), which is 46% of per-group time and overlaps compute 0.00
> cycles.

**Measured (warm, on hardware) vs MODEL:**

```
cyc/group = batches × (cin_run + 12.3) + 20.8            [N_OC = 16]
if DW binds:  total ≈ DW_input_beats + PW pipeline tail   (NOT max())
```

| blk | measured cyc/grp | model | measured HW | predicted HW |
|---|---|---|---|---|
| 0 | 36.1 | 36.1 | 10.38 | 10.40 |
| 1 | 77.3 | 77.4 | 5.57 | 5.57 |
| 2 | 149.0 | 109.4 | 2.68 | 2.30 |
| 3 | 104.0 | 109.4 | 1.87 | 1.97 |
| 4 | 180.0 | 198.0 | 3.24 | 3.56 |
| 5 | 308.4 | 326.0 | 5.55 | 5.87 |
| **Σ** | | | **29.30 ms** | **29.67 ms** |

**Error: −1.3% on HW, −2.6% on the full frame.** The prediction was recorded
before the run. Validated across **three `N_OC` values (8, 16, 30) and nine
layer configurations**.

**Discrepancy cause — block 2 (+17%).** Blocks 2 and 3 have *identical* PW
configuration yet differ 149.0 vs 104.0 cyc/group; the only difference is DW
input volume (230,400 vs 57,600 beats). When DW binds, the cascade costs
`DW_beats + PW tail` (~37,900 cycles here), not `max(DW, PW)`.

**Where the overhead comes from.** ~12 cycles per **batch** + ~21 per **group**.
Root cause in `pw_pixel_major_core.sv`: `S_COMPUTE` cannot exit until
`ppu_st == P_IDLE`, so batch *N+1*'s compute cannot finish until batch *N*'s PPU
drain has fully retired (`N_OC` issues + ~9-cycle PPU pipeline tail). The ideal
— if the drain overlapped — is `max(batches·cin, cout_rounded)`, worth
**~10 ms/frame**. Fix: double-buffer the shadow accumulator (~1 RAMB18, no DSPs).

**Not separately instrumented:** input-load, weight-load, output-write,
pipeline fill/drain and stall cycles are all inside the single HW bracket.

---

## 17. Latency and throughput ✅

> ### ✅ CURRENT FIGURE — 29.552 ms / 33.8 fps (2026-08-08). QUOTE THIS ONE.
> The do-not-quote warning is **lifted**. §29.0 is the operating point of record:
> `N_OC=32` + partial-batch drain + v3 + Option C + DW `USE_DSP(0)` + P0 + P1.
> ```
>   frame       29.552 ms    std dev 0.003 ms (0.011% CoV)
>   P95 / P99   29.558 / 29.559 ms      min/max 29.545 / 29.559
>   throughput  33.8 fps mean, 33.8 fps at P99
>   HW 22.03 (74.5%)  pack 5.07 (17.2%)  prog 2.44 (8.3%)  cache 0.00
> ```
> **Every number below this box is superseded** and is retained only as the
> history of how the frame time moved:
>
> | date | frame | fps | what changed |
> |---|---|---|---|
> | 2026-07-30 | 36.261 ms | 27.6 | baseline, `N_OC=16` |
> | 2026-08-05 | 38.594 ms | 25.9 | *no HW change* — a compiler hoisting decision on `pack` (§26) |
> | 2026-08-07 | 37.228 ms | 26.9 | `pack` source fix, still `N_OC=16` |
> | **2026-08-08** | **29.552 ms** | **33.8** | `N_OC=32` + drain/copy work + P0 + P1 |
>
> The 07-30 → 08-05 swing is the cautionary one: **2.3 ms of frame time moved
> with bit-identical hardware**, purely because unrelated code was added nearby.
> Any fps quoted from this design should carry the PACKBENCH line from the same
> run (§26).

**(SUPERSEDED) 100-frame distribution, 2026-07-30.** Retained for the history above.

```
Frames timed             100      Warm-up discarded        3
mean                     36.261 ms
std dev                   0.002 ms   (0.005% of mean)
median                   36.261 ms
min / max                36.254 / 36.266 ms
P95 / P99                36.264 / 36.266 ms
spread (max-min)          0.012 ms
throughput (mean)        27.6 fps
worst case (P99)         27.6 fps     <-- P99 == mean to 3 d.p.
```

Phase breakdown, averaged over the same 100 frames:
```
  HW datapath            29.30 ms  80.8%
  pack (block 0)          4.51 ms  12.4%
  weight/param program    2.44 ms   6.7%
  cache maintenance       0.00 ms   0.0%   (gm_in mapped non-cacheable)
  driver sum             36.25 ms          outside driver 0.01 ms
Cycles per frame         3,626,100 PL cycles @100 MHz
Batch size               1
Throughput               5.35 GMAC/s  (10.70 GOP/s at 2 ops/MAC)
Measured on real HW      YES
```

**The 0.005% coefficient of variation is a result in itself.** Bare-metal, single
core, no OS, no interrupts, fixed-size DMA descriptors — the only jitter sources
are DRAM refresh and cache state. **Worst-case throughput equals mean
throughput**, which is the property a real-time claim actually needs.

**Included in the frame time:** DMA, CPU setup, weight programming, cache
maintenance, output transfer. **Excluded:** nothing — `outside driver` = 0.01 ms.

Loading and computation **do** overlap (DMA streams while cores run). This is a
streaming design with `II = 1` per beat; initial latency is the DW pipeline
(18 cycles) plus the PW pipeline before the first output beat.

⚠️ Only **5 frames**; publication normally wants ~100 with a spread.

---

## 18. Resource utilisation ✅ (REPORT — post-route implementation)

| resource | used | available | % |
|---|---|---|---|
| Slice LUTs | 18,280 | 53,200 | 34.4% |
| LUTRAM | 458 | — | — |
| Flip-flops | 15,750 | 106,400 | 14.8% |
| Block RAM tiles | 73.5 | 140 | **52.5%** |
| URAM | 0 | 0 | n/a (7-series) |
| **DSP48E1** | **220** | **220** | **100.0%** |

### By module

| module | LUT | FF | RAMB36 | RAMB18 | DSP |
|---|---|---|---|---|---|
| `dw_fused_axi_0` | 6,043 | 4,300 | 24 | 1 | 88 |
| `pw_single_oc_axis_axi_0` | 7,164 | 4,622 | 41 | 4 | 132 |
| `axi_dma_0` + `axi_dma_1` | ~2,240 | ~2,980 | 5 | 2 | 0 |
| `smartconnect_0/1` | ~2,250 | ~3,160 | 0 | 0 | 0 |
| **total `hw_i`** | **18,280** | **15,750** | **70** | **7** | **220** |

Report stage: **post-route** (`hw_wrapper_utilization_placed.rpt` after
`route_design`). Synthesis-only estimates are not used anywhere in this document.

---

## 19. Timing closure ✅ (REPORT)

```
Target clock period      10.000 ns (100 MHz, PS FCLK0)
Worst negative slack     +0.321 ns
Total negative slack      0.000 ns      Failing setup endpoints: 0
Worst hold slack         +0.012 ns
Total hold slack          0.000 ns      Failing hold endpoints: 0
Worst pulse width slack  +3.750 ns
Endpoints analysed       58,303
Verdict                  "All user specified timing constraints are met."
Report stage             post-route
Implied Fmax             ~103.3 MHz  (10.000 − 0.321 ns)
Clock domains            1 for the accelerator (FCLK0); PS DDR/IO domains separate
CDC                      none inside the accelerator
Constraints              multicycle on reg_zp_relu_reg[*] (quasi-static config)
```

### Critical path — ROUTING-DOMINATED, zero logic levels

```
START  hw_i/pw_single_oc_axis_axi_0/inst/u_pw/u_core/first_ic_reg_rep__0/C
END    hw_i/pw_single_oc_axis_axi_0/inst/u_pw/u_core/p_0_out__42/OPMODE[4]
SLACK  +0.321 ns      REQUIREMENT 10.000 ns
DELAY   7.505 ns  =  logic 0.518 ns (7%)  +  net 6.987 ns (93%)
LOGIC LEVELS  0
```

**93% of the critical path is routing, and there are zero logic levels.**

`first_ic` is the PW core's first-input-channel flag — it clears the accumulator
(`acc <= (first_ic ? '0 : acc) + …`) and therefore drives the **OPMODE pins of
every DSP48 in the 16×8 MAC grid**. It is a high-fanout control net. Vivado has
already replicated it (`first_ic_reg_rep__0`) and it is still 7 ns of net delay.

### Top 5 setup paths

| slack | from → to | logic / net |
|---|---|---|
| +0.321 | `first_ic_reg_rep__0` → `p_0_out__42` (DSP OPMODE) | 0.518 / 6.987 |
| +0.322 | `wram_reg` → `p_cas_reg[0]` (DW weight RAM → MAC cascade) | 2.454 / 3.434 |
| +0.325 | `PS7_i` → `reg_cin_run_reg[31]` (AXI-Lite config) | 1.450 / 7.734 |

### Top 5 hold paths

| slack | from → to | logic levels |
|---|---|---|
| **+0.012** | `rounded_q_s3a_reg[12]` → `pre_clamp_s3_reg[12]` | 2 |
| +0.012 | `rounded_q_s3a_reg[16]` → `pre_clamp_s3_reg[16]` | 2 |
| +0.013 | `rounded_q_s3a_reg[20]` → … | 2 |
| +0.032 | `reg_w0_reg[13]` → `wram_reg` | 0 |

Hold is tightest in the **PPU requantisation rounding→clamp stage**. Note hold
slack is **independent of clock period** — raising the frequency neither helps
nor hurts it — but any re-place-and-route can flip +12 ps negative.

### ⚠️ CONSEQUENCE FOR THE CLOCK-RATE PLAN

At 125 MHz the period is 8.000 ns. The critical path already consumes
**7.505 ns**, so the projected slack is roughly **−1.7 ns — it will not close**
without work, and **the path has zero logic levels, so it cannot be pipelined.**
The fix must attack fanout/routing:

1. **Replicate `first_ic` further** (per DSP column / per `N_OC` bank) so each
   copy drives ~8 DSPs instead of 64 — a `max_fanout` attribute or explicit
   duplication, exactly the technique already used for `zp_in_lane`.
2. **Add a multicycle on the quasi-static AXI-Lite config registers.** Path 3
   (`PS7_i → reg_cin_run_reg`) is 9.18 ns of a genuinely quasi-static value —
   `dw_fused_timing.xdc` already does this for `reg_zp_relu_reg[*]`; extending
   it to `reg_cin_run`, `reg_n_groups`, `reg_img_width`, `reg_n_rows` is free
   margin.
3. The DW path (`wram_reg → p_cas_reg[0]`, 2.45 ns logic) is the only one with
   real logic depth and is **not** the binding constraint.

**Do not assume 125 MHz gives 1.25× throughput** — it requires timing work
first, and the ~30 fps target depends on it.

---

## 20. Power ✅ MEASURED (whole board) / ❌ accelerator share NOT SEPARABLE

**Source:** ZC702 PMBus, PS I2C0 → PCA9548 (0x74, ch7) → 3× UCD9248
(0x34/35/36), all 10 rails. 2026-08-05. 60 s of back-to-back encoder frames,
343 full-rail scans, 1374 frames.

### Headline — ✅ UPDATED 2026-08-08

```
Board power        2.0027 ± 0.0078 W     (regulator OUTPUT, 10 rails)
Frame time         29.551 ms             (33.8 fps)
ENERGY PER FRAME   59.18 ± 0.23 mJ
GOP/s/W            5.343
```
356 PMBus scans over 60.0 s, 1,785 frames, duty cycle 0.879.

**Energy fell 76.15 → 59.18 mJ/frame (−22%) while board power ROSE slightly**
(1.9731 → 2.0027 W). That is the point worth making: the saving is entirely
*time*, not power. A faster datapath draws marginally more instantaneous power
and still wins decisively on energy, because the frame is 25% shorter.

Per-rail at the new operating point: VCCINT 0.1829, VCCPINT 0.3516, VCCAUX
0.0288, VCCPAUX 0.1286, VCCADJ 0.0292, VCC1V5PS 0.4744, VCC_MIO 0.0110,
VCCBRAM 0.0126, VCC3V3 0.7678, VCC2V5 0.0159 W. VCCINT (the PL logic rail) rose
0.1699 → 0.1829 W, consistent with `N_OC=32` doing more work per cycle.

**The ±0.05 W run-to-run caveat below still applies — quote 59 ± 2 mJ.**

### (SUPERSEDED) 2026-08-05 headline
```
Board power        1.9731 ± 0.0076 W     (regulator OUTPUT, 10 rails)
Frame time         38.593 ms             (25.9 fps)
ENERGY PER FRAME   76.15 ± 0.29 mJ
```

| rail | group | mean W | ±SE |
|---|---|---|---|
| VCCINT | PL | 0.1699 | 0.0015 |
| VCCPINT | PS | 0.3504 | 0.0015 |
| VCCAUX | PL | 0.0272 | 0.0021 |
| VCCPAUX | PS | 0.1322 | 0.0029 |
| VCCADJ | MISC | 0.0341 | 0.0022 |
| VCC1V5PS | DDR | 0.4759 | 0.0021 |
| VCC_MIO | PS | 0.0114 | 0.0009 |
| VCCBRAM | PL | 0.0083 | 0.0006 |
| VCC3V3 | MISC | 0.7429 | 0.0051 |
| VCC2V5 | MISC | 0.0208 | 0.0019 |
| **TOTAL** | | **1.9731** | **0.0076** |

### ⚠️ Quote 76 ± 2 mJ, not ± 0.29

The ±0.29 mJ is **within-run** precision. Across four runs on 2026-08-05 the
total read **1.936, 1.973, 1.978 and 2.029 W** — a ±0.05 W spread from thermal
drift, **6× the within-run SE**. The tight SE says the 60 s average is stable;
it says nothing about run-to-run reproducibility, which is the uncertainty a
reader cares about.

### Duty cycle 0.8835 — why it does not bias the result

1374 × 38.593 ms = 53.0 s of the 60.0 s wall; the other 7.0 s is PMBus
scanning with the PL idle. Idle and active differ by **under 20 mW** (below),
so the worst-case bias is 0.12 × 20 mW = **under 0.1 mJ/frame** — inside the
quoted precision. The same fact that defeats incremental measurement makes the
total insensitive to duty cycle.

### ❌ Incremental (accelerator-only) power — NOT MEASURABLE on this board

**Root cause, measured 2026-08-05: the UCD9248 reports current in steps of
1/64 A = 15.625 mA, on every rail.** Decoded from `READ_IOUT` raw codes:

| rail | codes seen | distinct | mantissa step | current step |
|---|---|---|---|---|
| VCCINT | 41472…41856 (exp 2⁻¹²) | 7 | 64 | 15.6 mA |
| VCC1V5PS | 43552…43744 (exp 2⁻¹¹) | 7 | 32 | 15.6 mA |
| VCCBRAM | 32768…37376 | 3 | — | 15.6 mA |
| VCCAUX | 32768…39424 | 5 | — | 15.6 mA |

Different rails, different Linear11 exponents, **identical 15.625 mA quantum**.
On VCCINT that is **15.6 mW per code**. The Linear11 format *advertises*
0.244 mA resolution; the converter never emits it. Reporting the format LSB as
the resolution is a trap — it says "variance-limited, average harder" when the
truth is that the information was never encoded.

This explains the entire failure history:

| observation | explanation |
|---|---|
| per-sample sd ≈ 25 mA on **every** rail, independent of voltage and load | dither across a fixed 15.6 mA current grid |
| VCCAUX sd (0.0391 W) **exceeds** its mean (0.0307 W) | 3 grid codes near zero |
| no step visible when the accelerator starts | step < 1 code |
| first A/B gave **negative** deltas | thermal drift > signal; fixed by interleaving |
| interleaved A/B still "below noise" | quantisation, not drift |

**Controls run:**

- **Interleaved 16×2×8 s A/B** — all rails below noise. PL delta −0.0023 ± 0.0055 W.
- **Saturating DDR memcpy positive control** — VCC1V5PS moved **−0.0002 ± 0.0066 W**.
  A load worth tens of mW was **invisible**. The instrument, not the design, is
  the limit.

**Best statement available:** accelerator incremental draw is **below the
~55 mW detection limit**, i.e. under 3% of board total. That is an upper bound,
not a measurement.

⚠️ **Framing error to avoid.** The A/B compares *PL clocked and idle* against
*PL clocked and computing*. The clock tree runs in **both**, so even a working
A/B would measure only the data-toggle increment, not "accelerator power". A
clock-stop reference (FCLK0 100 MHz vs ~250 kHz) is implemented in `main.c`
behind `PM_RUN_DIAGNOSTICS` and would give the design's total contribution —
untested as of this writing.

### Derived — MEASURED

```
Energy per frame     76.15 mJ        (1.9731 W × 38.593 ms)
GOP/s/W              5.423           (10.70 GOP/s, 2 ops/MAC)
GMAC/s/W             2.71
Energy per MAC       369 pJ/MAC      whole board, 206.5 MMAC/frame
```

⚠️ Every GOP figure here inherits the unresolved **op-count convention**
(§15) and moves by 2× under the other choice.

### Vivado estimate — superseded, kept for comparison

`report_power`, routed, vectorless, 2026-07-30. **Medium** confidence
(<25% of internal nodes specified).

| | Vivado | measured | |
|---|---|---|---|
| Total | 2.187 W | 1.973 W | Vivado +11% |
| PS7 / PS rails | 1.573 W | 0.494 W | **Vivado 3.2× over** |
| PL fabric / PL rails | 0.450 W | 0.205 W | **Vivado 2.2× over** |
| DDR (VCC1V5PS) | not split out | 0.476 W | — |
| 3V3 / 2V5 / ADJ peripherals | **not modelled** | **0.798 W** | — |

Vivado's total is close **by cancellation**: it overstates PS7 and PL while
omitting 0.8 W of board peripherals entirely. The PS7 term — 72% of its total —
is a generic model with no knowledge of the actual CPU workload.

### Scope of the measured number

Regulator **output** power across all 10 rails: PS, DDR and board peripherals
included. **Excludes** regulator conversion losses and the unmonitored 5 V USB
rail — **this is not 12 V wall-input power**, which needs an external meter.

---

## 21. Hardware validation ❌ — NOT PERFORMED

**No output has ever been compared against a software reference.**

- All timing was taken with **synthetic dummy weights**; outputs are numerically
  meaningless by construction.
- **Two known open defects:** a DW one-word output offset, and a horizontal edge
  bug affecting the left and right 8 columns (a 2026-07-26 loopback measured
  99.08% correct with ~87% of mismatches at those edges).
- **Chaining propagates errors between blocks.** Non-zero-point bytes were
  observed in blocks 1–5 where uniform `zp_out` was expected — consistent with
  those defects compounding along the chain.
- Observability is limited: DW output cannot be inspected in cascade mode; the
  BD must be switched to loopback first, and that script no longer exists.

**Consequence: no accuracy, PSNR, MS-SSIM, bitrate or task-quality claim can be
supported by this work in its current state.**

### ✅ RESOLVED 2026-08-08 — block 2 was a real DATA-CORRUPTION bug, now fixed

**Everything in the section below is superseded.** The block-2 anomaly was not a
read race, it was not benign, and it was not "not reproducible". It was a genuine
data-corruption bug present in **every bitstream this project has ever built**,
and it is now root-caused and fixed. See §29.2.

Short version: PW's `consume_in` is registered, so the pop lands one cycle after
the core samples `valid_in`. While a read is in flight `in_empty` still reads 0
for a word that is about to be taken, so on the last word of a burst the core
latched the same `dout` twice — counting a beat it never received. From then on it
ran permanently one beat ahead: **every group shifted by one channel**, and DW was
left holding the surplus with `STATUS=busy`.

Reachable only when PW's input actually runs dry, i.e. only in a DW-bound block —
which is why block 2 was the sole victim, and why severity scaled from 1 beat at
`N_OC=16` to 60 at `N_OC=32` (the longer contiguous drain starves PW far more
often). At `N_OC=32` it left DW busy and the next pair's MM2S timed out.

Fix (`pw_single_oc_axis.sv`, both parts required):
```systemverilog
assign in_rd_en      = core_consume_in;                            // unconditional
assign core_valid_in = !in_empty && (in_occ > (core_consume_in ? 1 : 0));
```
Verified on six cascade shapes; **throughput-neutral** where PW binds (22,908 and
12,316 cycles bit-identical).

> **The counters were right and the data was wrong.** `written`/`produced`
> disagreeing by one was the *symptom*; the corruption was a one-channel shift
> that no beat-count check could ever have seen. Treat any future
> `written != produced` as a data-integrity failure, not an accounting artifact.

### ⚠️ (SUPERSEDED) OPEN ANOMALY — block 2 `produced` = `written` − 1 (2026-07-30)

In the fusion-traffic capture pass, block 2 reported `written = 57,600` but
`produced = 57,599` — **one 64-bit beat written into DW's output FIFO but never
handed to PW**. Every other block had `produced == written` exactly.

It is **not reproducible across passes**: the same block in the cold pass of the
same run reported `written = produced = 57,600`.

Two candidate explanations, not yet distinguished:
1. **Benign read race.** `written` and `produced` are read as separate AXI-Lite
   transactions; if the final handshake straddles them the pair can disagree by
   one. This would make it a measurement artifact.
2. **A genuinely stranded beat.** If real, PW consumed one word fewer than the
   57,600 it needs, and its final pixel group used stale data — silently.

Argument for (1): PW's output beat count and the S2MM byte count were both
exactly correct, and a starved final group would be expected to disturb one of
them. Argument for taking (2) seriously: **this is precisely the class of defect
that produced a full-length, cleanly-completing, correctly-counted stream of
garbage earlier the same day** (the PW over-production bug). Off-by-one beat
accounting has already been wrong here twice.

**Do not quote the fusion-traffic figure to byte precision until this is
resolved.** The aggregate effect is 8 bytes in 26 MB (0.00003%), so the 29.8%
reduction is unaffected — but the anomaly itself must be explained.

Cheapest way to distinguish: read the counters twice in succession in the same
pass and compare; a race gives different answers, a stranded beat gives the same
one. If it is real, `dbg_flags` bit `out_full_ever` and the FIFO occupancy at
end-of-run will say whether a beat is still sitting in the FIFO.

---

## 22. Scalability ✅ (partial — `N_OC` axis only)

> ### SUPERSEDED BY §28.5 — encoder-level `N_OC` sweep, 2026-08-08
> The table below is the **legacy 3-pair surrogate**, whose `Cout` values do not
> match the encoder's. It is what produced the "non-monotonic and
> workload-dependent" description, and it should not be quoted for the real
> model. Measured PW-only totals across all six encoder blocks:
>
> | `N_OC` | PW total | note |
> |---|---|---|
> | 8 | 36.73 ms | |
> | 16 | 23.63 ms | |
> | 32 | **19.09 ms** | **requires partial-batch drain** |
>
> Without partial-batch drain, `N_OC=32` is 23.70 ms — i.e. **worse than 16**,
> because block 0's `Cout=16` pads to 32 and doubles its drain across 28,800
> groups. That padding penalty is exactly what made `N_OC` look non-monotonic;
> once removed, the axis is monotonic and the optimum moves.
> `N_OC=16 → 8` measures 1.554×, independently reproducing the ~54% figure
> previously derived from hardware alone.

| `N_OC` | HW time (legacy 3-pair) | DSP | LUT | BRAM | WNS |
|---|---|---|---|---|---|
| 30 | 21.04 ms | — | — | — | — |
| 8 | 15.44 ms | 170 (77%) | 16,681 | 69.5 | +0.305 |
| 16 | 15.76 ms | **220 (100%)** | 18,280 | 73.5 | +0.321 |

**Scaling stops at DSP = 220/220.** Both the PW MAC grid (`N_OC × N_LANES/2`)
and the PPU requant multipliers (`N_LANES`) consume DSPs, so neither `N_OC` nor
`N_LANES` can increase further on this device.

**`N_OC` is non-monotonic and workload-dependent** — a result worth reporting.
At `N_OC = 16` the legacy 3-pair surrogate got *slower* (48.3 vs 49.1 fps)
because `Cout = 8` pads to `cout_rounded = 16`, doubling that layer's output
beats; the same change makes the real encoder ~54% faster because all its
`Cout` values are multiples of 16.

❌ Not measured: throughput/power/bandwidth vs lane count; routing limits.

---

## 23. Ablations ✅ (six measured configuration comparisons)

| ablation | effect | resource effect |
|---|---|---|
| **Fused/chained vs independent buffers** | **−14.75 ms/frame** (45.69 → 30.97) | none |
| **`gm_in` cacheable vs non-cacheable** | **−10.60 ms/frame** (30.97 → 20.37) | none |
| **Stride-1 proxy vs real stride-2 DW** | +22.73 ms (honesty correction, not a gain) | +4 BRAM |
| **DW MACs in fabric vs DSP** | WNS +0.078 → +0.305 ns | **−6,390 LUT, +72 DSP** |
| **Line-buffer depth 10800 → 1024** | none | **BRAM 92.5% → 49.6%** |
| **`N_OC` 8 / 16 / 30** | non-monotonic (§22) | DSP 170 → 220 |
| **`pack` generic vs C=3 hoisted** | **−1.76 ms** (6.08 → 4.32, same binary) | none — host software (§26) |
| `pack` NEON vs C=3 hoisted | +0.68 ms — **NEON is slower** (§26) | none |

The `gm_in` result is a genuine systems finding: caching a DMA staging buffer
was **actively harmful**, not merely useless. The Cortex-A9 L1 is
**write-allocate**, so every store into a not-yet-cached line first *read* 32
bytes from DRAM — lines `pack` then fully overwrote. Removing caching killed the
8.88 ms flush **and** made packing 1.4× faster (6.24 → 4.52 ms).

❌ Not done: codebook (N/A), double-buffering on/off, tile size, precision,
clock frequency.

---

## 24. Comparison-ready row ⚠️

| field | value |
|---|---|
| FPGA | Xilinx Zynq-7020 (xc7z020clg484-1), ZC702 |
| Technology node | 28 nm |
| Precision | INT8 weights + activations, INT24/INT32 accumulate |
| Clock | 100 MHz (post-route closes; Fmax ~103 MHz) |
| Input resolution | 1280×720 |
| Model | ImageEncoderLite, 6 DW-separable blocks |
| Scope | **Encoder only** |
| MAC/frame | 193.77 M |
| GOP/frame | 0.388 (at 2 ops/MAC) |
| Throughput | 6.56 GMAC/s = **13.11 GOP/s** (at 2 ops/MAC — §15) |
| FPS | **33.8** |
| Latency | 29.55 ms end-to-end |
| LUT / FF | 18,280 / 15,750 |
| BRAM / DSP | 73.5 / **220** |
| External BW | 18.43 MB/frame; **624 MB/s sustained** (18.432 MB / 29.55 ms) |
| Power | **2.0027 ± 0.0078 W** (whole board, 10 rails; ±0.05 W run-to-run) |
| GOP/s/W | **5.343** (at 2 ops/MAC) |
| **GOP/s/DSP** | **0.0486** (10.70 / 220) |
| Energy/frame | **59.18 ± 0.23 mJ** (quote 59 ± 2 — see §20 run-to-run) |
| Model quality | ❌ **still unvalidated (§21)** — dummy weights; no accuracy, PSNR, MS-SSIM or bitrate claim is supportable |

> **Row updated 2026-08-08 from §29.0.** Resources are unchanged in kind but the
> configuration is now `N_OC=32`: LUT 28,509 (53.6%), FF 20,195 (19.0%),
> BRAM 73.5 (52.5%), **DSP 220/220**, WNS +0.119 ns, WHS +0.014 ns.
> GOP/s/DSP = 13.11 / 220 = **0.0596**.
>
> ⚠️ Every GOP figure here inherits the unresolved **op-count convention** (§15)
> and halves under the other choice. That decision is still open and it is the
> single easiest way for this row to be misread against other accelerators.

---

## 25. Available artifacts

**Have:** RTL (`Zynq.srcs/sources_1/new/*.sv`), block design (`hw.bd`),
register descriptions (§12), post-route utilization/timing reports
(`Zynq.runs/impl_1/*.rpt`), hardware measurement logs, per-layer latency and
bandwidth tables (§0), cycle-count model (§16), build/verify scripts
(`vivado_scripts/`), two self-checking testbenches
(`sim_1/new/dw_banked_window_8x_stride2_tb.sv`,
`pw_single_oc_axis_instrumented_tb.sv`), platform spec (§1).

**Missing:** architecture **diagram** (figure), representative waveforms,
hardware-vs-software correctness comparison.

**Build/measurement workflow:** §27 — sim, bitstream, platform repoint, app
switches, and the silent-failure traps.

---

## 26. Host-side `pack` — a compiler-dependent frame time ⚠️ TRACK THIS

> ### FOUR-BUILD RECORD (updated 2026-08-08) — the instability is real and asymmetric
> Same in-binary benchmark, same buffers, 720×1280 C=3, min of 5:
>
> | variant | 08-05 | 08-07 | 08-08a | 08-08b | verdict |
> |---|---|---|---|---|---|
> | generic | 6.08 | 6.07 | 6.07 | 6.08 | stable, always slowest |
> | **C=3 ptr-hoisted** | 4.32 | **5.47** | 4.32 | **4.31** | fastest in **3 of 4** |
> | C=3 NEON 2-group | 5.00 | 5.00 | 5.07 | 5.07 | stable, never fastest |
>
> **Writing the hoist into the source did NOT stabilise it.** The scalar variant
> still moved 27% between builds of *identical* source, because what varies is
> register allocation and scheduling around the hot loop — not the one multiply
> that was hoisted. That is the durable lesson: a source-level fix cannot pin a
> compiler's scheduling decision.
>
> **But the asymmetry matters.** `ptr`'s bad case (5.47) is only 0.40 ms worse
> than NEON's steady 5.07, while its good case is 0.76 ms better. With one bad
> sample in two it looked unstable; with one in four it looks like an outlier.
>
> Dispatch is currently **NEON** (`main.c:1997`), and the 33.8 fps figure was
> measured with `pack = 5.07 ms`. Switching to `ptr` is worth ~0.76 ms →
> ~28.79 ms → **~34.7 fps**.
>
> **Recommendation: take `ptr`, and record the PACKBENCH line alongside any
> quoted fps.** That documents the variance instead of paying 0.76 ms
> permanently to avoid it. `pack` is 17.2% of frame time and remains the only
> phase that can move without an RTL change — if frame time shifts while
> `HW datapath` and `weight/param` are unchanged, it is this.

**Why this section exists:** frame time moved 6% between two builds with
**identical hardware**, and the cause was neither the hardware nor the
algorithm. It will recur, silently, on any future rebuild. This is the record
of how it was found so it is recognised next time instead of re-investigated.

### The symptom

| | 2026-07-30 | 2026-08-05 | Δ |
|---|---|---|---|
| HW datapath | 29.30 ms | 29.30 ms | 0 |
| weight/param | 2.44 ms | 2.44 ms | 0 |
| cache | 0.00 ms | 0.00 ms | 0 |
| **`pack`** | **4.51 ms** | **6.84 ms** | **+2.33** |
| frame total | 36.261 ms | 38.594 ms | +2.333 |
| throughput | 27.6 fps | 25.9 fps | −1.7 |

Bit-identical HW cycles, same real stride-2 geometry, same non-cacheable
`gm_in`, same source data, same function. **The entire frame-time difference
is `pack`.** The only change between the builds was ~400 lines of unrelated
PMBus power code elsewhere in `main.c`.

### Why two builds could not settle it

The hypothesis was code placement — I-cache line alignment, branch-predictor
aliasing. **That hypothesis is untestable by rebuilding**, because every
rebuild reshuffles placement. A second build is a second sample of the
confound, not a control for it.

So the variants were compared **inside one binary**, back to back, on the same
buffers, with placement held constant and each candidate `__attribute__
((aligned(64)))` so its hot loop starts on a cache line whatever the linker
does around it.

### MEASURED, one binary, 720×1280 C=3, min of 5 passes

| variant | min ms | vs generic | correctness |
|---|---|---|---|
| generic (loop over `c`, recomputes `src_h + c*H*W + col0`) | 6.08 | 1.00× | reference |
| **C=3 pointer-hoisted, inner loop unrolled** | **4.32** | **1.41×** | **byte-exact** |
| C=3 NEON, 2 groups/iter, 3× `vst1q` | 5.00 | 1.22× | byte-exact |

**4.32 ms is where `pack` sat on 2026-07-30 (4.51 ms).** So the work never
changed: the older build's compiler hoisted the `c*H*W` multiply out of a
345,600-iteration loop and strength-reduced the 3-trip inner loop; this build
did not. Adding unrelated code was enough to flip that decision.

**Fix applied:** the hoist is now written into the source
(`pack_gm_c3_ptr` in `main.c`, dispatched from `pack_input_group_major` when
`C==3 && W%8==0`; the generic path stays for every other shape). This removes
the dependency on a compiler decision, which matters more than the 2.3 ms —
otherwise frame time moves whenever anything nearby is edited.

### ❌ NEON lost — record the reason, not just the result

The prediction was that halving the store count would win, because `gm_in` is
Normal non-cacheable and stores go through the write buffer. **Wrong.** The
write buffer already merges consecutive 8-byte stores, so store count was never
the bottleneck — loop overhead and address arithmetic were, which is exactly
what the scalar hoist removes. `vcombine` also costs register moves the A9's
NEON load/store path does not earn back.

Note also the access pattern bound (unchanged): the **destination** is
sequential but the **source** jumps `H*W` bytes between channels (921,600 B on
pair 0). This is an 8-byte-granular block scatter across C streams. NEON has no
scatter, and `vst2/3/4` interleave *elements*, not 8-byte blocks. Plain 8-byte
moves are the right primitive.

### Benchmark ≠ in-frame

The benchmark's generic row reads **6.08 ms** while in-frame `pack` reads
**6.84 ms**. The gap is expected and must not be mistaken for noise: the
benchmark takes the *minimum* of five back-to-back passes with `raw_in` warm in
L2, whereas in-frame `pack` runs once after a full hardware pass has churned
the caches. It understates all three variants roughly equally, so the **1.41×
ratio transfers but the absolute numbers do not**.

### ⏳ Status

- ✅ Cause identified, fix written into the source, byte-exactness verified.
- ⏳ **End-to-end frame time with the fix NOT yet measured.** Predicted `pack`
  ≈ 4.9 ms in-frame → frame ≈ 36.6 ms → ≈ 27.3 fps.
- ⏳ On confirmation, update §17 (drop the do-not-quote warning), §20 (energy
  falls to ≈ 72 mJ — board power is unchanged, the frame is shorter), §23, §24.

### Recurrence check for future builds

`pack` is 18% of frame time and is the **only** phase that can move without an
RTL change. If frame time shifts while `HW datapath` and `weight/param` are
unchanged, it is this. The in-binary benchmark stays behind `SURR_PACK_BENCH`
in `main.c` — re-enable it rather than re-deriving the diagnosis.

---

## 27. Build and measurement workflow — RTL → sim → bitstream → platform → board

Every number in this document came through this path. Recorded because the
failure modes below are **silent**: they produce a clean-looking run that
measures the wrong thing.

### Paths

| what | where |
|---|---|
| Vivado project | `C:\Users\Fahad\Zynq\Zynq.xpr` |
| RTL (live tree) | `Zynq.srcs\sources_1\new\*.sv` |
| **PW core the IP actually builds from** | `Zynq.srcs\src\pw_pixel_major_core.sv` |
| PW IP package | `Zynq.srcs\component.xml` |
| **DW IP package** ⚠️ | `C:\Users\Fahad\ip_repo\dw_fused_axi_1.0\component.xml` — **outside the project tree** |
| **DW sources the IP builds from** ⚠️ | declared **TWICE**: `ip_repo\dw_fused_axi_1.0\src\*.sv` **and** `..\..\Zynq\Zynq.srcs\sources_1\new\*.sv` |
| Testbenches | `Zynq.srcs\sim_1\new\*_tb.sv` (sim set `sim_1`, lib `xil_defaultlib`) |
| Build scripts | `vivado_scripts\*.tcl` |
| XSA written by the build | `C:\Users\Fahad\Zynq\noc16_cg2048.xsa` |
| Vitis platform | `C:\Users\Fahad\Zynq\Final_2\` |
| Platform XSA copy | `Final_2\hw\noc16_cg2048.xsa` |
| Application | `C:\Users\Fahad\Zynq\Final_code_2\` (`src\main.c`) |
| Bitstream | `Zynq.runs\impl_1\hw_wrapper.bit` |

### ⚠️ THE DW DOUBLE-DECLARATION TRAP — located 2026-08-08

The DW IP lives at `C:\Users\Fahad\ip_repo\dw_fused_axi_1.0`, **outside
`C:\Users\Fahad\Zynq` entirely** — which is why no `component.xml` inside the
project mentions `dw_fused`. Its `component.xml` declares all six sources
**twice**: once as `src/<f>.sv` (the IP's own copy) and once as
`../../Zynq/Zynq.srcs/sources_1/new/<f>.sv` (the live tree). Which copy
synthesises is a coin flip that has flipped between consecutive builds.

`generate_target` copies from the **packaged IP**, so editing only the live tree
changes nothing. Proved again on 2026-08-08: a `USE_DSP(1)→(0)` edit applied only
to `Zynq.srcs\sources_1\new\dw_fused_core.sv` passed the source pre-flight, and
the build then aborted on the *generated* copy at
`Zynq.gen\...\ipshared\c9c1\src\dw_fused_core.sv`, which still carried
`USE_DSP(1)`.

**Rule: every DW edit must be written to BOTH paths, and the two asserted
byte-identical before building.** `S_build_noc32.tcl` now does exactly that for
all six files and aborts on any divergence. Do not rely on the purge — it
refreshes the generated copies *from the package*, not from the live tree.

Contrast with PW, whose `component.xml` declares each file **once** and is
therefore immune to this specific trap — though it is MIXED (core from `src/`,
shell from `sources_1/new/`), which is its own hazard.

### A0. ⚠️ TOOL ENVIRONMENT — read this before running anything

**There are TWO `Vivado\2020.2` directories on this machine and only one works.**

| path | state |
|---|---|
| **`C:\SPROJ\Vivado\2020.2\`** | ✅ **THE LIVE INSTALL.** Complete `bin\` — `vivado.bat`, `xvlog.bat`, `xelab.bat`, `xsim.bat`, `xsc.bat` |
| `C:\Xilinx\Vivado\2020.2\` | ❌ 3.2 GB remnant — only `data\xsim`, `tps\`, `win64\`. **No `bin\` at all** |
| `C:\Xilinx\2025.1\Vivado\` | present, but **do not open `Zynq.xpr` with it** — forces an irreversible project upgrade |

Verified 2026-08-07 by tracing the running process
(`C:\SPROJ\Vivado\2020.2\bin\unwrapped\win64.o\vivado.exe`). A search of
`C:\Xilinx` alone finds only the remnant and gives the false impression that
2020.2 is gone — it is not. **2020.2 is the version that built the current
bitstream** (Build 3064766) and is the version to keep using; nothing here
needs 2025.1.

```bat
set VIV=C:\SPROJ\Vivado\2020.2\bin
```

### A. Simulation

Testbenches are self-checking — they compare against a golden model computed
from a pixel function that mixes `c`, `y` and `x`, so a tap taken from the
wrong channel, row or column disagrees. They check emission **count and order**
too, so an off-by-one in group pairing or row parity fails loudly instead of
producing plausible garbage.

**In-project** (GUI: Flow ▸ Run Simulation, or batch):

```tcl
# %VIV%\vivado.bat -mode batch -source <this file>
open_project C:/Users/Fahad/Zynq/Zynq.xpr
set_property top dw_stride2_tb [get_filesets sim_1]
launch_simulation
run -all
```

⚠️ A GUI instance holds a lock on `Zynq.xpr` — **close Vivado before any batch
run against the same project**, or the batch run fails or blocks.

**Standalone** (no project, no lock, fastest iteration — used for the variant
sweeps in `scratchpad\pwsim\`, `dbl\`, `syn\`):

```bat
%VIV%\xvlog.bat -sv <rtl>.sv <tb>.sv
%VIV%\xelab.bat -debug typical <tb_top> -s <snap>
%VIV%\xsim.bat <snap> -R
```

⚠️ **Compile the same files the IP packages**, or you simulate a design that
is not the one being built: the PW testbench instantiates `pw_single_oc_axis`,
so it needs the shell from `sources_1\new\pw_single_oc_axis.sv` **and** the
core from `Zynq.srcs\src\pw_pixel_major_core.sv` — not the `sources_1\new\`
copy of the core.

| testbench | covers |
|---|---|
| `dw_banked_window_8x_stride2_tb.sv` (top `dw_stride2_tb`) | DW windower, **stride 1 and 2**, all 72 taps, count + order |
| `pw_single_oc_axis_instrumented_tb.sv` | PW core, incl. the `ppu_issue_idx` over-production bug (§19) |
| `dw_banked_window_8x_tb.sv`, `dw_fused_axi*_tb.sv`, `final_tb.sv` | earlier DW variants |

⚠️ The stride-2 TB instantiates the DUT with `MAX_CG_PRODUCT(10800)`, but the
**synthesised** build uses `2048`. The TB will not catch a `cin_run × n_groups`
overflow at the built size. Preconditions the hardware does **not** check:
`n_groups` even, `cin_run ≥ 2`.

### B. Bitstream build (Vivado, batch)

```bash
vivado -mode batch -source vivado_scripts/R_regen_bd_and_build.tcl
```

Set `SET_N_OC` at the top of the script (currently `16`). It does, in this
order — and the order is the whole point:

1. **Verify the fix marker is in the real IP source** (`Zynq.srcs\src\...`).
2. `update_ip_catalog -rebuild -scan_changes`.
3. **Narrow purge** of `ipshared` + `bd/hw/Zynq` copies only.
4. `reset_target all` — *before* regenerating. Deleting files on disk does
   **not** tell Vivado its output products are stale; it tracks generation
   state internally, so a purge alone leaves `generate_target` regenerating
   **nothing**.
5. `open_bd_design`, set `CONFIG.N_OC`, read it back, `validate_bd_design
   -force` — `reset_target` invalidates parameter propagation, and without the
   forced re-validate the BD still reports "already validated", skips
   propagation, and smartconnect fails with `key "si_properties" not known`.
6. `generate_target all`, then **check every generated copy** of
   `pw_pixel_major_core.sv` carries the marker.
7. `reset_run synth_1`, `launch_runs impl_1 -to_step write_bitstream`.
8. Scan `runme.log` for `Synth 8-2490` (duplicate module — a stale copy may
   have won).
9. `write_hw_platform -fixed -include_bit -force`.

Then verify and re-export:

```bash
vivado -mode batch -source vivado_scripts/V_verify_netlist_and_export.tcl
```

This counts `ppu_issue_idx_reg*` flops in the **post-synth netlist** and
derives the expected width from the build's own `CONFIG.N_OC`
(`⌈log₂(N_OC+1)⌉` → 4 flops at `N_OC=8`, 5 at `N_OC=16`). **This is the only
check that cannot be fooled by a stale source copy, a wrong IP repository, or
a guard that validated the wrong file — all three of which happened on
2026-07-30.**

### C. Platform update (Vitis) — the step that keeps breaking

The XSA lands at `C:\Users\Fahad\Zynq\noc16_cg2048.xsa`. Vitis reads it from
inside the platform, so:

1. **Back up the platform JSON first.** The convention already in the tree is
   `vitis-comp.json.bak_pre_<reason>_repoint` (11 such backups exist).
2. Copy the XSA into the platform: `Final_2\hw\noc16_cg2048.xsa`.
3. Edit `C:\Users\Fahad\Zynq\Final_2\vitis-comp.json` — **two fields, both
   must change together**:

```json
"xsa": "C:\\Users\\Fahad\\Zynq\\Final_2\\hw\\noc16_cg2048.xsa",
"xsaPathInPlatform": "Final_2\\hw\\noc16_cg2048.xsa",
```

Note the **escaped backslashes**, the absolute path in `xsa`, and the
**platform-relative** path in `xsaPathInPlatform`. Changing only one is the
usual cause of "platform invalidated".

4. In Vitis: regenerate/rebuild the platform, then rebuild the application.
   The app component `Final_code_2\vitis-comp.json` binds to the platform by
   **name** (`"platform": "Final_2"`, domain
   `standalone_ps7_cortexa9_0`) — that field does not change when the XSA does.

### D. Application changes (`main.c`) → board

`main.c` is a single ~5,300-line file; edit, rebuild the app in Vitis, and run.
Compile-time switches that change what is measured:

| macro | current | effect |
|---|---|---|
| `PW_N_OC` | 16 | **MUST equal the BD's `CONFIG.N_OC`** — see the trap below |
| `SURR_MODEL_ENCODER` | 1 | real 6-block `ImageEncoderLite` vs the small surrogate |
| `SURR_CHAIN_PAIRS` | 1 | pair *p*'s PW output feeds pair *p+1*'s DW |
| `SURR_GM_IN_NONCACHED` | 1 | maps `gm_in` `NORM_NONCACHE`, removes the pair-0 flush |
| `SURR_FRAMES` / `SURR_WARMUP` | 100 / 3 | steady-state timing sample |
| `RUN_POWER_MEASUREMENT` | 1 | PMBus block (§20); **adds ~90 s** |
| `PM_ENERGY_ONLY` | 1 | 60 s energy/frame run instead of the idle/active A/B |
| `PM_RUN_DIAGNOSTICS` | 0 | telemetry diagnostics incl. the PL clock-stop test |
| `SURR_PACK_BENCH` | 1 | in-binary pack variant benchmark (§26) |
| `CASCADE_ENGINE_SPLIT` | 1 | DW/PW overlap polling — **adds AXI-Lite polls inside the HW bracket**; quote HW totals from a `0` run |

### ⚠️ Traps — every one of these has produced a clean run with wrong numbers

| trap | consequence | guard |
|---|---|---|
| **`PW_N_OC` ≠ BD `CONFIG.N_OC`** | weights misprogrammed **silently**; no error, plausible timing | `V_…tcl` prints the required value |
| **Editing `sources_1\new\pw_pixel_major_core.sv`** | changes **nothing** — the IP builds the core from `Zynq.srcs\src\`; packaging is *mixed*, the AXIS shell comes from `sources_1\new\` | marker check in `R_…tcl` |
| **JTAG "Run" without programming** | stale fabric from the previous session | explicitly program `hw_wrapper.bit` every run |
| **Only one of the two JSON paths updated** | "platform invalidated" | §C step 3 |
| **Wholesale purge of `bd\hw`** | smartconnect `si_properties` failure | narrow purge only (§B step 3) |
| **Frame time moved, HW cycles identical** | compiler stopped hoisting in `pack` | §26; re-enable `SURR_PACK_BENCH` |
| **Editing RTL without a backup** | project rule — copy to a backup folder first | — |

### Reproducing the numbers in this document

| section | how |
|---|---|
| §17 latency, §0 per-layer | `RUN_POWER_MEASUREMENT 0`, `CASCADE_ENGINE_SPLIT 0`, 100 frames |
| §20 power / energy | `RUN_POWER_MEASUREMENT 1`, `PM_ENERGY_ONLY 1` (60 s, ~90 s total) |
| §14 fusion traffic | DW beat counters, dedicated **untimed** pass |
| §18/§19 resources, timing | `report_utilization` / `report_timing_summary` on `impl_1` |
| §26 pack | `SURR_PACK_BENCH 1` |
| DW/PW overlap | `CASCADE_ENGINE_SPLIT 1` — for the ratio only, not for HW totals |

---

## 28. DSP allocation study — the 220-DSP budget, measured and optimised

**Compiled 2026-08-07/08.** This section replaces the guess in §4 ("136 are MACs;
the remaining ~84 are PPU requant multipliers, the `tile_groups × cout_run`
product, and address arithmetic"). That sentence is **wrong** — only 34 DSPs are
PPU/shell. It also supersedes §22, which reported the `N_OC` axis only on the
legacy 3-pair surrogate.

### 28.1 The measured ledger ✅ MEASURED (routed checkpoint)

`open_checkpoint Zynq.runs/impl_1/hw_wrapper_routed.dcp`, then
`get_cells -hier -filter {REF_NAME =~ DSP48*}` bucketed by hierarchical name:

| function | DSPs | scaling law |
|---|---|---|
| DW `conv_mac_array` | 72 | `N_LANES × K` (1 MAC/DSP, **no packing**) |
| DW PPU requant | 16 | `N_LANES × 2` |
| **DW total** | **88** | |
| PW MAC grid — multiply | 64 | `N_OC × N_LANES/2` (**2 MAC/DSP, packed**) |
| **PW accumulators** (`p_0_out*`) | **50** | opportunistic — see below |
| PW PPU requant | 16 | `N_LANES × 2` |
| PW shell (`total_groups_r`, `pp_lo/hi`) | 2 | fixed |
| **PW total** | **132** | |
| **device** | **220** | |

**The "missing 50" are accumulators, not PPUs.** They are named
`p_0_out__0 … p_0_out__42` inside `u_pw/u_core`, and **`p_0_out__42` is the exact
net in the §19 critical path** (`first_ic_reg_rep__0/C → p_0_out__42/OPMODE[4]`).
`first_ic` occurs only in
`acc[oci][p] <= (first_ic ? '0 : acc[oci][p]) + p_packed_reg[…]`, so these are
accumulator adders that Vivado absorbed into DSP48 ALUs with `first_ic` driving
OPMODE. That also explains why the critical path has **zero logic levels**: it
ends on a DSP control pin, not in fabric.

**The absorption is opportunistic, not required.** Synthesising the core at
`N_LANES=8, N_OC=32` with no source change already yields `acc = 0` DSPs —
Vivado backs off once the multiply grid alone needs 128 of 220. So raising
`N_OC` releases these DSPs for free.

### 28.2 What a DSP costs, per source ✅ MEASURED (OOC synthesis)

`synth_design -mode out_of_context -part xc7z020clg484-1`, cell counts from the
post-synth netlist. PPU variants:

| `ppu` variant | LUT | FF | DSP |
|---|---|---|---|
| `use_dsp="yes"`, 33×25 (current) | 579 | 234 | **2** |
| `use_dsp="no"`, 33×25 | 1,646 | 390 | 0 |
| `use_dsp="no"`, narrowed 25×18 | 1,091 | 334 | 0 |

A PPU is **2 DSPs**, confirmed directly. The fabric multiply costs
**1,067 LUT**; narrowing (`bias` 32→24 b so `acc_biased` fits 25 b, `mult_conv`
24→18 b) saves a further 555 LUT/PPU at no precision cost — 17 mantissa bits
against a 1/255 output step is eleven bits of margin.

PW core, `N_LANES=8, N_OC=16`, accumulators free vs forced to fabric:

| variant | LUT | FF | DSP | multiply | accumulate | PPU |
|---|---|---|---|---|---|---|
| as-built | 7,291 | 4,381 | 144 | 64 | 64 | 16 |
| `acc` tagged `use_dsp="no"` | 9,634 | 6,474 | 80 | 64 | 0 | 16 |

**Cost of freeing one DSP — this is the key table:**

| source | LUT / DSP freed |
|---|---|
| PW accumulators → fabric | **36.6** |
| DW MACs → fabric (their own §23 ablation: 72 DSP for 6,390 LUT) | 88.8 |
| PPUs → fabric | **533** |

**PPUs are the worst source by 15×.** Any plan that frees DSPs by putting the
requant multipliers in fabric should be rejected on these numbers.

### 28.3 The cycle model ✅ MEASURED (xsim, 20+ configurations)

Fitted to simulation and exact to **< 0.05 cycles** on every configuration
tried, across `N_OC ∈ {8,16,32}`:

```
cyc/group = B·cin_run + Q_last + 6B + 1
            B      = ceil(cout_run / N_OC)
            Q_last = cout_run − (B−1)·N_OC        [partial-batch drain]
```

The four terms are: compute, the one final drain that does **not** overlap the
next batch, per-batch overhead, per-group overhead. `cyc/group` is **independent
of `N_LANES`** — the `ic` loop is one cycle per input channel and the drain is
one cycle per output channel regardless of lane count — so PW time scales as
`1/N_LANES` purely through the group count.

Expressed in DSP terms, with `D_PW = N_OC·N_LANES/2`:

```
T_PW,j = MAC_j/(2·D_PW)  +  P_j·Q_last/L  +  3·P_j·cout_j/D_PW  +  P_j/L
```

Two consequences follow directly:

1. **DW's design freedom is one number.** DW time is `M_j/(L·K)` — it depends
   only on the *product*, never on `L` and `K` separately.
2. **At fixed `D_PW`, larger `L` is always better.** Raising `L` while lowering
   `N_OC` to hold `D_PW` constant leaves terms 1 and 3 unchanged and shrinks
   terms 2 and 4. **Lanes strictly dominate output channels.** This is the
   analytical reason `N_OC` saturates and `N_LANES` does not.

Sim-vs-hardware check on the **currently flashed bitstream** (pre-v3 core,
`bak_pre_v3_2026-07-30`), against the 2026-08-07 board run:

| blk | hardware cyc/grp | simulated | error |
|---|---|---|---|
| 0 | 36.1 | 36.01 | −0.3% |
| 1 | 77.3 | 77.02 | −0.4% |
| 2 | 149.0 | 103.03 | *DW-bound — see below* |
| 3 | 104.0 | 103.03 | −0.9% |
| 4 | 180.0 | 179.03 | −0.5% |
| 5 | 308.4 | 307.05 | −0.4% |

Five of six within 1%. Block 2 is not a miss: the simulation models PW alone and
returns 103.03 — **identical to block 3**, exactly as it must, since blocks 2 and
3 have identical PW configurations. Hardware reads 149.0 only because DW binds
there. This independently reproduces §16's diagnosis from the opposite direction.

### 28.4 Partial-batch drain ✅ IMPLEMENTED, simulated

Without it the drain always issues `N_OC` beats, so a layer whose `cout` is not a
multiple of `N_OC` pays for padding channels that are computed and discarded.
Block 0 (`cout=16`) at `N_OC=32` measured **42.02** cyc/group against **26.02**
at `N_OC=16` — exactly the 16 extra drain cycles.

Two changes were required, and the second is the non-obvious one:

1. `P_DRAIN` issues `min(N_OC, cout_run − batch·N_OC)` beats, and
   `cout_batches_r` becomes a **ceiling** (`(cout_run+N_OC−1)/N_OC`) — with a
   floor, `cout < N_OC` gives zero batches and the core emits nothing.
2. **The `acc → shadow` copy must be bounded identically.** With only the drain
   bounded, block 0 still measured **42.01** — the copy runs `N_OC` cycles and
   both `S_COMPUTE` and `S_WAIT_PPU` gate on `!shadow_copying`, so it simply
   became the new limiter. Bounding both gives **26.02**, with
   `vin == vout == fifo_writes` passing.

It also removes a latent bug: the shell predicts `total_groups_r =
tile_groups × cout_run`, which disagreed with the `cout_rounded` beats the core
actually emitted whenever `cout` was not a multiple of `N_OC`. That is the same
predicted-count class that has bitten this design four times.

### 28.5 Design space ✅ MEASURED (xsim, all six encoder block shapes)

PW-only time for the whole encoder, post v3 + Option C + partial-batch drain:

| `N_OC` | b0 | b1 | b2 | b3 | b4 | b5 | **PW total** |
|---|---|---|---|---|---|---|---|
| 8 | 28.02 | 97.03 | 161.03 | 161.03 | 313.03 | 569.05 | **36.73 ms** |
| 16 | 26.02 | 61.02 | 93.03 | 93.03 | 169.03 | 297.05 | **23.63 ms** |
| 32 | 26.02 | 55.02 | 71.03 | 71.03 | 109.03 | 173.05 | **19.09 ms** |

(cyc/group; `N_OC=32` requires partial-batch drain — without it block 0 is 42.02
and the total is 23.70 ms, i.e. **worse than `N_OC=16`**.)

`N_OC=16 → 8` is 1.554×, independently reproducing the ~54% figure previously
derived from hardware only.

### 28.6 The binding constraint is DMA, not compute

Per-block floors at 8 B/cycle per 64-bit HP port, 100 MHz:

| blk | read `I/8` | write `O/8` | DW MAC `M/72` |
|---|---|---|---|
| 0 | 345,600 | 460,800 | 86,400 |
| 1 | 460,800 | 230,400 | 115,200 |
| 2 | 230,400 | 57,600 | 57,600 |
| 3 | 57,600 | 57,600 | 57,600 |
| 4 | 57,600 | 115,200 | 57,600 |
| 5 | 115,200 | 115,200 | 115,200 |

Block time is `max(read, DW MAC, PW, write)` — DW and PW run concurrently, that
is the fusion. Note **DW MAC exactly equals the read floor on the three stride-1
blocks**: 72 MACs is precisely what sustains 1 beat/cycle ingest at stride 1.
DW's 72 DSPs are therefore set by a *rate-matching* requirement, not by its 18.2%
share of the arithmetic.

`Σ_j max(read, write) = 1,440,000 cycles = **14.40 ms**` — the floor no DSP
allocation can beat.

### 28.7 Operating points ✅ resource numbers MEASURED (OOC synth)

| config | DSP | LUT | HW | fps | binds on |
|---|---|---|---|---|---|
| today — L8 Q16, DW on DSP | 220 | 18,280 | 24.5 ms | 30.6 | PW |
| L8 Q32 + partial drain, **DW MACs → fabric** | 162 | ~32,400 (61%) | **20.76 ms** | **35.4** | b0 PW, b1/b2 read |
| L8 Q40 | 194 | ~35,000 (66%) | 20.47 ms | 35.8 | same |
| **L16 Q16 — max** | **194** | **~42,500 (80%)** | **16.55 ms** | **41.6** | b0 write, b1/b2 read |
| *(bound)* | — | — | *14.40 ms* | *~48* | DMA only |

L8/Q32 needs 234 DSPs as built, so DW's MACs must move to fabric. That is **free
in throughput terms** — the array is still `8 × 9 = 72` MAC/cycle either way —
costs 6,390 LUT, and gives back the ~4× setup-slack margin the DSP P-register was
providing (§23).

**Block 0 is the wall at `N_LANES=8`.** Its 26 cyc/group × 28,800 groups =
7.49 ms is 36% of the frame, and since `cout=16` caps its drain at 16 regardless
of `N_OC`, no amount of PW width touches it — Q=40 buys 1.4% over Q=32. Only
halving the group count (`N_LANES=16`) moves it, exactly as the `1/L` term in
§28.3 predicts.

### 28.8 Caveats

- All cycle figures are **simulation**, validated against hardware to <1% on the
  currently flashed bitstream (§28.3). Nothing in §28.7 has been built.
- LUT figures for `N_LANES=16` are **extrapolated** from OOC core synthesis plus
  the §23 DW-fabric ablation; the DW windower, line buffers and PPUs also widen,
  and 80% is close enough to the limit that it must be confirmed by a real build.
- BRAM at `N_OC=32` is **NOT** a risk — measured. OOC core synthesis returns
  6 RAMB at `N_OC` = 16, 32 and 48 alike: the weight memory is
  `(COUT_MAX/N_OC) × CIN_MAX` deep across `N_OC` banks, so total capacity is
  invariant and only the banking changes. BRAM *does* double at `N_LANES=16`
  (6 → 12 in the core), because `SA_DW = N_LANES × ACC_WIDTH` and the pixel
  buffers both widen — that is the configuration where 52.5% today needs
  checking.
- `first_ic` already owns the critical path driving 64 DSPs' OPMODE with 93%
  routing delay and zero logic levels. Any increase in `N_OC` or `N_LANES` makes
  that worse, so the fanout fix (§19) is a **prerequisite**, not an option.
- `cout_batches_r` now uses a ceiling. That changes behaviour for any layer where
  `cout` is not a multiple of `N_OC` — the intended fix, but re-run the
  `vin == vout == fifo_writes` check on every shape to be deployed.

### 28.9 Reproducing this

```
# DSP ledger from the routed checkpoint
vivado -mode batch -source scratchpad/dsp_breakdown.tcl
# PPU and PW-core resource sweeps (OOC)
vivado -mode batch -source scratchpad/accexp/sweep_core.tcl
# cycle model / design space
scratchpad/sim/run.ps1  -NOC <q> -CIN <c> -COUT <o> -GROUPS <g> -Core <path>
scratchpad/sim/sweep.ps1 -NOC <q>          # all six encoder shapes
```
Simulation uses `xsim` on the RTL directly rather than `launch_simulation`; it
reproduces the recorded v3 numbers (27.0/61.0/169.0/297.0) to two decimals.

---

## 29.0 ✅ HEADLINE RESULT — 33.8 fps, measured 2026-08-08

**`N_OC=32` + partial-batch drain + v3 + Option C + DW `USE_DSP(0)` + P0 + P1.**
100 frames, 3 warm-up discarded. This supersedes every earlier frame time.

```
  frame        29.552 ms      std dev 0.003 ms  (0.011% of mean)
  median       29.552 ms      min/max 29.545 / 29.559    spread 0.015 ms
  P95 / P99    29.558 / 29.559 ms
  throughput   33.8 fps (mean)   33.8 fps (P99)
```
```
  HW datapath   22.03 ms  74.5%      (was 29.30)
  pack           5.07 ms  17.2%
  weight/param   2.44 ms   8.3%
  cache          0.00 ms   0.0%
  -- driver sum 29.54 ms             outside driver 0.01 ms
```
```
  board power   2.0027 ± 0.0078 W     ENERGY  59.18 ± 0.23 mJ/frame
  GOP/s/W       5.343                 (was 74.03 mJ at 26.9 fps)
```

**26.9 → 33.8 fps, +25.7%.** HW datapath −7.27 ms. Energy −20%, entirely because
the frame shortened — board power is essentially unchanged. P99 equals mean to
three decimals, so worst-case throughput equals mean throughput.

### Both fixes confirmed on silicon

**P0** — the decisive check, pair 2: `written=57600 produced=57600` with
`DW STATUS=0x1`. **Every** pair now reports `written == produced` exactly and DW
finishes done rather than busy. The anomaly present in every prior bitstream is
gone, and the fusion total is now byte-exact — **3,916,800 B**, against
3,916,792 before. *The missing 8 bytes were the corruption.* §14/§21's "do not
quote to byte precision" caveat is lifted.

**P1** — pair 0: `Cout=16 (rounded 16)`, `out=3686400 B`, `COUT=16` in the
register. No longer padded to 32 and 7,372,800 B. Partial-batch drain is now
actually engaged, and pair 0's PW time halved **10.38 → 7.50 ms**.

### Cost model validated on hardware

| blk | predicted cyc/grp | measured | |
|---|---|---|---|
| 0 | 26 | **26.1** | PW-bound |
| 1 | 55 | 75.2 | DW-input-bound + tail |
| 2 | 71 | 149.6 | DW-input-bound + tail |
| 3 | 71 | **72.0** | PW-bound |
| 4 | 109 | **110.0** | PW-bound |
| 5 | 173 | **174.4** | PW-bound |

Four PW-bound blocks within ~1%. Blocks 1 and 2 are read-bound and carry the
documented tail (b2's floor is 230,400 cyc = 2.304 ms against 2.69 measured).
**Whole-frame prediction 21.5 ms vs 22.03 measured — 2.4% error.**

DW/PW overlap is also sane again: **21.71 ms of DW runs underneath PW**, ratio
0.99, tail 1.4%. Pairs 1 and 2 now read 1.00 / 0.00 — the previous `DWbusy=0.00`
artifact on pair 2 was itself a symptom of the P0 bug.

> ⚠️ Still dummy-weight. **§21 correctness is untouched** — no accuracy, PSNR,
> MS-SSIM or bitrate claim is supportable. The DW one-word offset and the
> left/right-8-column edge bug remain open.

---

## 29. Session record — 2026-08-08

Compiled at the end of a long working session. Everything here is MEASURED unless
marked otherwise. Several earlier claims in this document are **retracted**; those
are listed in §29.5 so they are not re-derived.

### 29.1 What now exists that did not before

| artifact | why it matters |
|---|---|
| `scratchpad/sim/cascade_tb.sv` | **first** testbench instantiating DW and PW together. Every prior PW result came from a PW-only harness with an ideal driver; the fused path had only ever been exercised on hardware |
| `scratchpad/sim/pw_cfg_tb.sv` phase counters | per-group residency in each FSM state — turns the fitted cost-model constants into measured facts |
| `golden_lane()` in `pw_cfg_tb.sv` | **first** data check in this project's history. Currently blocked by simulation X (§29.4), but the model itself is written and reusable on hardware |
| `S_build_noc32.tcl` multi-file marker guards | PW core, PW shell, `ppu.sv` and DW core, checked in both source and **every generated copy** |

### 29.2 Bugs found and fixed

**P0 — PW over-consumes a beat when its input FIFO runs dry.** The §21 block-2
anomaly. Root-caused from a cycle-by-cycle trace (`[TR] OVER-CONSUME cyc=433`,
`S_LOAD_FIRST`, channel 31 of the first group). Data corruption, present in every
build. Fixed and regression-clean. Full detail in §21.

**P1 — the driver rounded `cout` up to a multiple of `N_OC`.** With partial-batch
drain in the RTL the driver must pass the *true* `Cout`, otherwise the fix never
engages: on 2026-08-08 block 0 (`Cout=16`) was programmed `cout_run=32` and
emitted **7.37 MB instead of 3.69 MB**. Fixed at `main.c:2620` and `:3290`.

> ⚠️ **`main.c` has TWO independent PW paths with duplicated geometry maths.**
> `:2620` is the standalone path; **`:3290` is the cascade path the board
> actually runs**, and it is the one feeding `pw_write_reg(PW_REG_COUT_RUN,…)`.
> Fixing only the first changes nothing. Grep every use of a geometry variable
> before assuming one edit is enough.

### 29.3 Source-of-truth traps — the full map

This project's dominant failure mode is a stale copy winning. The complete
picture, now that all of it is known:

| file | declared by | source paths | generated copies |
|---|---|---|---|
| `pw_pixel_major_core.sv` | PW IP, once | 1 (`Zynq.srcs/src/`) | 2 |
| `pw_single_oc_axis.sv` | PW IP, once | 1 (`sources_1/new/`) | 2 |
| `dw_fused_core.sv` | **DW IP, twice** | 2 | 4 |
| **`ppu.sv`** | **PW once + DW twice** | **2** | **6** |

**The DW IP is not in the project tree at all** — it lives at
`C:\Users\Fahad\ip_repo\dw_fused_axi_1.0`, which is why no `component.xml` under
`C:\Users\Fahad\Zynq` mentions `dw_fused`. Its `component.xml` declares all six
sources **twice**: once as `src/<f>.sv` and once as
`../../Zynq/Zynq.srcs/sources_1/new/<f>.sv`. `generate_target` copies from the
**packaged IP**, so editing only the live tree changes nothing — proved again on
2026-08-08 when a `USE_DSP(1)→(0)` edit passed the source pre-flight and the build
aborted on the generated copy.

**Two more staleness traps confirmed the same day:**
- **The XSA must be re-copied into the Vitis platform after EVERY rebuild**, not
  only when its filename changes. Twice the platform copy was still the previous
  build (`6E5A7AD6…`, then `47E72258…`) while the fresh one sat in `Zynq/`. Same
  name, different content — Vitis would silently package the old bitstream.
- **There are two `Vivado/2020.2` directories and only one works.**
  `C:\SPROJ\Vivado\2020.2` is the live install; `C:\Xilinx\Vivado\2020.2` is a
  partial install with `data/` but no `bin/`.

### 29.4 Why there is still no data check

`golden_lane()` is written and correct in form, but **every output reads X in
simulation** — 51,200/51,200 unknown, with **ZERO numeric mismatches**. There is
no evidence of a data bug; the X is a simulation-initialisation artifact.

The core's datapath registers deliberately have no reset (to stay out of the
async-reset fanout cone), so simulation starts them X while silicon powers them
to 0. Three fixes were attempted and **all failed**:

| approach | outcome |
|---|---|
| `xelab --initreg 0` | option does not exist in this xsim (2025.1) |
| testbench zero deposits | work for plain hierarchical names — the **core is now fully X-free** — but **silently do nothing** for anything inside a generate block, which is every PPU and MAC instance |
| declaration initialisers in `ppu.sv` | synthesisable and kept, but the registers are re-clocked from an X source before the measurement window, so they do not help |

> **The `int'(X) → 0` trap.** The first version of this check reported "16,512
> mismatches, hardware emits 0 everywhere". That was entirely the checker
> converting X to zero. **Any data check in this project must test
> `$isunknown()` before comparing**, or it will manufacture a bug that does not
> exist.

**Recommended path: validate on hardware.** Real registers power up to 0, so the
problem does not arise. Dump a block's output from DDR and compare against
`golden_lane`. That closes §21 — the largest gap in this document — without
further simulator archaeology.

### 29.4b ⚠️ A METHODOLOGICAL ERROR WORTH REMEMBERING — comparing an optimised A against an unoptimised B

**What happened.** Having measured that the group-boundary shadow copy was worth
~4.65 ms, the conclusion drawn was:

> *"`L=8, N_OC=32` + copy overlap reaches 16.11 ms, which beats the `L=16,
> N_OC=16` optimum at 16.58 ms — so the lane doubling is unnecessary."*

**That comparison was invalid.** It applied the copy overlap to one configuration
and not the other. Applying it to both:

| config | without overlap | with overlap |
|---|---|---|
| `L=8, N_OC=32` | 20.76 ms | **16.15 ms** |
| `L=16, N_OC=16` | 16.58 ms | **16.12 ms** |

They **tie to within 0.2%**. The original conclusion happened to survive — `L=8`
is still the right build target because it is already built — but it survived for
the wrong reason, and the *stated* reason ("it beats the 16-lane optimum") was
false.

**Why it is easy to make.** An optimisation is naturally discovered while working
on one configuration, so it gets attached to that configuration in the mind. But
a change to the *cost model* applies to every point in the design space. The
correct discipline is: **when an optimisation changes a term in the model,
re-evaluate the whole space, not the configuration you found it in.**

**What it would have cost.** Left uncorrected, this would have justified closing
the `N_LANES=16` investigation on a false premise. The right justification —
*they tie, so take the one already built* — is stronger and leads to a different
follow-on question (why do they tie?), which produced the finding below.

**And the finding it produced.** Both configurations have `D_PW = 128`
(`32·8/2` and `16·16/2`) — identical PW multiplier capacity. Theorem 2's
preference for lanes comes from the drain term `P·Q_last/L` falling as `1/L²`,
**but that term IS the un-overlapped copy.** Overlap it and only the ~1-cycle
`P/L` term still favours lanes.

> **The spatial-vs-channel parallelism trade-off in this architecture was decided
> by the drain schedule, not by the dataflow.** That is a sharper and more
> defensible claim than "we chose 8 lanes", and it only became visible because
> the flawed comparison was corrected rather than patched.

### 29.5 Retractions

| claim | status |
|---|---|
| §4 "~84 non-MAC DSPs are PPU requant, the group product and address arithmetic" | **WRONG.** 50 of them are **accumulators**; only 34 are PPU/shell. §28.1 |
| §21 "not reproducible … benign read race" | **WRONG** on both counts. Deterministic, and it corrupts data. §21, §29.2 |
| Option C worth ~5.8 ms/frame | **WRONG by ~20×.** Measured 0.29 ms. The ~9-cycle PPU tail is already hidden behind the compute-pipeline flush in every multi-batch config |
| `first_ic` fanout 6,669 at `N_OC=32`, a blocking prerequisite | **RETRACTED.** Not reproducible (274 in another session); implausible against the ~380 expected loads. Timing closes at `N_OC=32` regardless — `first_ic` was never the blocker |
| explicit source-level `first_ic` replication | **NO-OP.** Vivado merges functionally identical registers regardless of `dont_touch`; produced a bit-identical netlist |
| "the hardware emits 0 everywhere" (data check) | **RETRACTED.** Artifact of `int'(X)→0`. Zero numeric mismatches |
| "17 cycles of per-group overhead" | **MISREAD.** That was `Q_last(16) + 1` — the shadow copy plus one cycle. The true per-group constant is **1** |
| "`L=8,N_OC=32`+overlap beats the `L=16,N_OC=16` optimum" | **INVALID COMPARISON** — overlap applied to one side only. With it applied to both they tie within 0.2%. Conclusion survived, reasoning did not. §29.4b |
| §22 "`N_OC` is non-monotonic and workload-dependent" | **ARTIFACT.** It came from the legacy surrogate's `cout` padding. With partial-batch drain the axis is monotonic (36.73 / 23.63 / 19.09 ms) |
| `N_LANES` / `K` described as settable in §7.0 of the analysis doc | **NEITHER IS A PARAMETER.** `N_LANES` is a data-layout contract (DW hardcodes 8); `K` does not exist — `conv_mac_array` unrolls the 3×3 by hand as `p_cas[0..8]` |
| "the channel schedule is **near-optimal** for this hardware" (§29.9) | **WRONG — and now disproved, not merely unproven.** Exhaustive search over a declared feasible set puts it at rank 786 of 3,436 (35.21% utilisation vs 54.93% best), and `8-32-64-64-64-64` strictly dominates it on time, arithmetic *and* utilisation simultaneously. §29.9 |
| *(intermediate draft)* "the question is unanswerable without RD data, because `T` is monotone in every channel width" | **WRONG OBJECTIVE** — but only *partially*. True of frame time; utilisation is a ratio, so `max U s.t. T ≤ budget` is well-posed for an *occupancy* study. RD does remain necessary for architecture *selection*. `CHANNEL_SCHEDULE_ANALYSIS.md` §6 |
| draft 3's frontier, ranks, fps and the `C2=32` codesign sentence | **VOID — enumerated with the Route A half-drain law while calibrated against full-drain silicon.** Correct calibration 1.0615, not 1.2527. Under the authoritative model b1 at `C2=32` is PW-bound (338,400 cyc vs 230,400 DMA), so the crossover is not the binding transition and the sentence has no support until Route A is board-measured. §29.9, Task #18 |
| "`8-32-64-64-64-64` **strictly dominates** the deployed schedule" | **TOO STRONG.** Improves three chosen proxies (time, PW MACs, utilisation) but conv weights rise 10,363 → 16,731 (**+61.4%**); at 2.44 ms/invocation coefficient programming that is **+1.50 ms, exceeding the 0.54 ms compute saving**. Correct wording: *dominated under the selected hardware-efficiency proxy metrics* |
| "block 0's overhead is worth **1.73 ms**" | **WRONG DERIVATION** — used the write (230,400 cyc) as b0's floor and ignored the RGB read (345,600 cyc), while taking PW from the other drain model. On the built RTL the saving is **0.576 ms**. Task #16 rescoped |
| "a block saturates the PW engine **iff** `cin ≥ N_OC`" | **NECESSARY, NOT SUFFICIENT.** Also needs `cout ≥ s²·cin`. Counterexample in the deployed schedule: b2 has `cin=32` yet `I_svc = 8`, because `cout=32` where 128 is required |
| the quantity called "**arithmetic intensity**" throughout §29.9 | **MISNAMED.** It divides by `max(read, write)`, not total bytes — it models overlapped directional service. Renamed **directional service intensity** to avoid collision with the paper's existing usage |
| "no output has ever been validated against a reference" (§21) | **OBSOLETE 2026-08-16** — outputs have since been validated. B3/B4 remain untimed, which is a separate limitation |
| `L=8, N_OC=32` called the "**global** optimum" | **OVERSTATED.** It is the best of four enumerated points (`L∈{8,16} × N_OC∈{16,32}`) at fixed `D_PW=128`, fixed DSP split and fixed channel schedule. Optimum over a declared space is defensible; "global" is not |

### 29.7 Where the per-group time actually goes — MEASURED

Phase residency per group, `pw_cfg_tb` at `N_OC=32` (new `[PHASE]` counters):

| shape | cyc/grp | COMPUTE | WAIT_PPU | copying | copy hidden under compute |
|---|---|---|---|---|---|
| b0 `cin3 cout16` (B=1) | 26.02 | 8 | **17.01** | 16 | **0.00** |
| b2/b3 `cin32 cout32` (B=1) | 71.03 | 37 | **33.01** | 32 | **0.00** |
| b5 `cin64 cout64` (B=2) | 173.05 | 138 | **33.01** | 64 | 32 |

`S_WAIT_PPU` is 46% of block 2/3's per-group time and it is **pure waiting on the
`acc → shadow` copy**. v3 already hides the *intra-group* copies (b5 hides 32 of
64); what remains exposed is the **final batch's copy at every group boundary**,
costing `Q_last + 1`.

It waits because the next group's compute overwrites all of `acc` every cycle
(`first_ic` clears it) while the copy is still reading `acc`. That conflict is the
sole reason.

**Prize**, applying the DMA floors at `N_OC=32`: b0 748,800 → 460,800 (becomes
write-bound, −2.88 ms), b3/b4/b5 −0.59 ms each, b1/b2 unchanged (already at their
DMA floor). **20.76 → 16.11 ms, ~41.8 fps** — the same result as `N_LANES=16`,
with no windower rewrite.

#### ✅ ROUTE A IMPLEMENTED 2026-08-09 — split even/odd shadow halves

The shadow accumulator is split into **even-OC and odd-OC halves**, so the copy
writes two channels per cycle and its cost halves to `ceil(Q_last/2)`. The drain
still consumes one channel per cycle, alternating halves. Both halves share one
address bus — OC `j` is at half-index `j>>1` in half `j&1`, and `j`/`j+1` share
`j>>1` when `j` is even — with `sha_rd_parity` registered so the output mux lands
with the data. `sha_wr_en_o` is suppressed on a ragged final pair, so an odd
`Q_last` writes only the even channel.

Measured in simulation, `N_OC=32`:

| shape | cyc/grp | was | `WAIT_PPU` | copying |
|---|---|---|---|---|
| cin3 cout16 | **18.20** | 26.02 | 9.09 | 8 |
| cin16 cout32 | **39.30** | 55.02 | 17.13 | 16 |
| cin32 cout32 | **55.38** | 71.03 | 17.13 | 16 |
| cin32 cout64 | **93.39** | 109.03 | 17.13 | 32 |
| cin64 cout64 | **157.54** | 173.05 | 17.13 | 32 |

Copy and `S_WAIT_PPU` both exactly halved; ~15.6 cycles/group saved on every
shape. All six cascade shapes re-pass with exact beat counts, **including both
DW-bound cases** — where the P0 bug lived.

Projected from the *measured* 08-08 baseline with DMA floors applied:
b0 7.50→5.24, b3 1.30→1.00, b4 1.98→1.68, b5 3.14→2.84; b1 and b2 unchanged
because they are DW-input-bound. **HW 22.03 → 18.87 ms, frame 29.55 → 26.39 ms,
33.8 → ~37.9 fps.**

**Route B (double-buffer `acc`) deliberately NOT done.** Its remaining ~2 ms
requires a mux on the accumulator feedback, on a design with almost no margin.

> ### ⚠️ BUILT 2026-08-09 — AND "Route A cannot touch the critical path" WAS WRONG
> ```
>   WNS   +0.043 ns   (was +0.119)      0 failing endpoints
>   WHS   +0.007 ns   (was +0.014)      0 failing endpoints
>   LUT   28,364 / 53,200  53.3%        BRAM 76.5 / 140  (was 73.5, +3 as predicted)
>   DSP   220/220                       all constraints met
> ```
> The claim was that splitting the shadow "touches only the copy-write and
> drain-read paths and structurally cannot affect the critical path". **WNS
> nonetheless fell 64%, from +0.119 to +0.043 ns.**
>
> The reasoning error: I checked whether the change added *logic* to the critical
> path, and it did not. But it added **3 RAMB36 and a 192-bit output mux**, and
> that placement pressure degraded a path it never touches logically.
> **Adding area moves the floorplan; "not on that path" does not mean
> "no timing effect".**
>
> **And the critical path has MOVED — it is no longer `first_ic`.** It is now
> inside the DW MAC array, which `USE_DSP(0)` put into fabric:
> ```
>   Source:      …/u_dw/u_core/G_MAC[4].u_mac/G_SKEW[2].win_sr_reg[2][0]/C
>   Destination: …/u_dw/u_core/G_MAC[4].u_mac/p_cas_reg[2][17]/D
>   Logic Levels: 13  (CARRY4=8  LUT2=2  LUT3=1  LUT6=2)
> ```
> That is the `p_cas` accumulate chain. It is now **logic-dominated (13 levels)**,
> where the old `first_ic` path was routing-dominated with **zero** logic levels —
> a completely different problem, and a more tractable one.
>
> **Cheapest fix, not yet applied: `ACC_WIDTH` 32 → 24 in the DW core.** The
> 8 CARRY4s are a 32-bit adder; 9 taps of int8×int8 peak at 146,304, which needs
> **18 bits**, so 32 is over-provisioned by 8 bits and 24 is still generous. That
> removes 2 CARRY4 from the critical path for free.
>
> **Both margins are now thin — +0.043 ns setup, +0.007 ns hold.** Timing is MET
> with zero failing endpoints, so the bitstream is valid, but a re-place-and-route
> could flip either. Do not re-run implementation casually, and treat the DW
> `ACC_WIDTH` reduction as the next timing action rather than an optimisation.

> ⚠️ **The verification gap applies to this change.** An OC-ordering error would
> give correct beat counts with channels swapped, and the TB uses all-`+1`
> weights so every channel yields the same value — **invisible**. Beat accounting
> and the addressing structure are verified; **values are not**. Cheapest fix
> once the X problem is resolved: make the TB weights channel-dependent
> (`w_rd_data[o] = o+1`), which makes any ordering error immediately visible.

### 29.9 Is the channel schedule throughput-optimal? — NO, it is strictly dominated

> ⚠️ **This section previously ended "Verdict: near-optimal." That claim is
> withdrawn**, and the *opposite* is now established by an exhaustive search over
> a declared feasible set. `CHANNEL_SCHEDULE_ANALYSIS.md` §0 has the full
> revision history — including that an intermediate draft wrongly argued the
> question was unanswerable without RD data. It is answerable: the objective is
> **utilisation** (a ratio, non-degenerate), not frame time (a total, degenerate).

Full derivation in **`CHANNEL_SCHEDULE_ANALYSIS.md`**. The headline:

**Every inter-block tensor is written once and read once, so a channel is paid
for twice.** Marginal cost per channel, at 8 B/cycle per HP port:

| tensor | resolution | **cost per channel** |
|---|---|---|
| **C1** (b0→b1) | 360×640 | **0.576 ms** |
| C2 (b1→b2) | 180×320 | 0.072 ms |
| C5 (b4→b5→out) | 90×160 | 0.036 ms |

> **`C1 = 16` channels × 0.576 ms = 9.22 ms — 64% of the entire 14.40 ms DMA
> floor.** Block 0's output width is the single most consequential number in the
> channel schedule, because it is the only place a still-large resolution carries
> a non-trivial channel count.

#### Saturation theorem

Machine balance is `256 MAC/cyc ÷ 8 B/cyc = 32 MAC/byte`. Writing out the
arithmetic intensity `AI = cin·cout·P_out / max(cin·P_in, cout·P_out)` gives
`min(cin,cout)` at stride 1 and `cin·cout/max(4cin,cout)` at stride 2 — and
**both are capped by `cin`**:

> **A block's arithmetic intensity can never exceed its input channel count, at
> any output width, at either stride. So a block saturates the PW engine iff
> `cin ≥ N_OC = 32`.**

| blk | `cin` | `AI` now | **ceiling** | saturable? |
|---|---|---|---|---|
| b0 | 3 | 3 | **3** | never — `cin=3` is RGB |
| b1 | 16 | 8 | **16** | never — capped at half the ridge |
| b2 | 32 | 8 | 32 | at `cout=128` |
| b3 / b4 | 32 | 32 | 32 | **exactly at the ridge** |
| b5 | 64 | 64 | 64 | 2× above it |

b1's ceiling **is** `C1`. Since b0 and b1 can never use the engine *and* `C1` is
the most expensive tensor on the chip, `C1` should be as **small** as RD permits.
Widening it to "feed" the engine is exactly backwards.

#### Exhaustive search — the deployed schedule loses

`max U s.t. T ≤ 17.59 ms` over `C₁..C₆ ∈ {8,…,128}⁶` (262,144 points; strides,
block count and hardware fixed; PW from the §16 silicon-validated model):

| variant | feasible | best `U` | **current rank** |
|---|---|---|---|
| `C₆` free | 29,154 | 56.15% | **5,174** |
| `C₆` pinned at 64 | 3,436 | 54.93% | **786** |
| pinned + non-decreasing | — | 51.74% | — |

Deployed sits at **35.21%**. And `8-32-64-64-64-64` — an ordinary non-decreasing
shape — **strictly dominates it on all three axes at once**:

| | time | MACs | utilisation |
|---|---|---|---|
| current `16-32-32-32-64-64` | 17.59 ms | 158.5 M | 35.21% |
| **`8-32-64-64-64-64`** | **17.12 ms** | **226.7 M** | **51.74%** |
| | −2.7% | **+43%** | **+16.5 pts** |

Three separable mechanisms: **(1)** `C1 16→8` frees 3.46 ms — it is the binding
term in *both* b0-write and b1-read; **(2)** `C3 32→64` is **free** — b2 is
read-bound (230 vs 115 kcyc), so its output width is unpriced, and utilisation
doubles 25%→50% at zero time cost; **(3)** `C4 32→64` spends 1.84 ms to take b3
from `AI=32` to `AI=64`, 58%→82%.

Changing **only** `C1`, utilisation falls monotonically over the whole range —
38.22% (`C1=8`) → 35.21% (16) → 29.65% (32) → 21.20% (128).

#### Block 0 is a control-overhead defect, not a schedule defect

b0 is **PW-bound** while running the engine at 5–8%. At `cin=3, cout=8`:
`cyc/grp = 1·3 + 4 + 6 + 1 = 14`, of which **only 3 are compute — 11 of 14 are
per-group overhead**. Remove it and b0 becomes DMA-bound at 230 kcyc: **1.73 ms
saved, 10% of the frame, with no model change and no RD risk.** Block 0 costs
23–29% of the frame to perform 7% of the arithmetic.

**⚠️ MACs are a capacity proxy, not rate–distortion.** The dominating schedule
has more parameters and more arithmetic, so it is not obviously weaker — but no
RD claim is made or available (§21). The hardware case is unambiguous; the
compression case is untested.

#### ⚠️ 2026-08-16 — the numbers above are DRAFT-3 and were enumerated with the WRONG cycle law

**All frame-time and utilisation figures in this section came from the Route A
half-drain law `B·cin + ⌈Q_last/2⌉ + 6B + 1`, while the 22.03 ms calibration
point was measured on full-drain hardware.** Route A is built and timing-closed
but **has never been board-measured**. Corrected values:

| | full drain (**authoritative**) | Route A (built, unmeasured) |
|---|---|---|
| current `16-32-32-32-64-64` | **20.754 ms**, 29.84% util | 17.586 ms, 35.21% |
| candidate `8-32-64-64-64-64` | **20.214 ms**, 43.81% util | 17.118 ms, 51.74% |
| calibration vs 22.03 ms board | **1.0615** | 1.2527 ✗ |

**The codesign sentence previously in this section is WITHDRAWN.** It claimed the
frontier's slope break sits at b1's read/write crossover `C2 = s²·cin = 32`.
Under the authoritative model b1 at `C2=32` has read = write = 230,400 cyc but
**PW = 338,400** — it is *PW-bound*, so the crossover is not the binding
transition. The claim exists only under Route A (PW = 223,200) and is therefore
**conditional on a measurement that has not been made** (Task #18).

**Also withdrawn from this section:** "strictly dominated" — it ignores conv
weight count (10,363 → 16,731, **+61.4%**) and coefficient-programming time
(2.44 ms/invocation; if it scales with weights, **+1.50 ms, exceeding the 0.54 ms
compute saving and making the candidate net slower**). The ordinal ranks
(5,174 / 786) were tie-sensitive — at the current budget **1,010 schedules are
strictly better** on utilisation. And "measurement costs nothing" understated the
host work (DMA descriptors, buffers, weight generation, reference outputs,
correctness checks).

**What survives unchanged, because it depends only on transfer volumes:** the
service-intensity bound `I_svc ≤ cin`; the crossover formula `cout = s²·cin`;
C1's 0.576 ms/channel; `C3: 32→64` being latency-neutral at b2; and the −10.0%
activation traffic (18.432 → 16.589 MB).

**⚠️ SUPERSEDED BY RD MEASUREMENT 2026-08-16.** `8-32-64-64-64-64` measured WORSE for compression than the deployed schedule. Revised candidate is `16-32-64-64-64-64` (early capacity bit-identical to deployed, +56% MACs, +11.7 pts utilisation). The RD experiment is confounded across three variables and needs the two ablations first.

Full corrected treatment: **`CHANNEL_SCHEDULE_ANALYSIS.md` draft 5.**

**Rule of thumb: a channel costs `2 × pixels / 8` cycles wherever it crosses
DDR — 16× more at 360×640 than at 90×160. If capacity must be added, add it
late.**

### 29.8a ⚠️ CORRECTION 2026-08-09 — the optimum is `N_LANES=8, N_OC=32`

§29.8b below compares `L=8,Q=32` **with** the copy overlap against `L=16,Q=16`
**without** it. That is unfair and its conclusion is superseded. Applying the
overlap to both:

| config | without overlap | **with overlap** |
|---|---|---|
| `L=8, N_OC=32` | 20.76 ms | **16.15 ms** |
| `L=16, N_OC=16` | 16.58 ms | **16.12 ms** |

**They tie to within 0.2%.**

**Why:** both have `D_PW = 128` (`32·8/2` and `16·16/2`) — identical PW
multiplier capacity, so the compute term is identical. Theorem 2's preference for
lanes comes from the drain term `P·Q_last/L` falling as `1/L²`, **but that term
IS the un-overlapped shadow copy.** Overlap it and only the ~1-cycle `P/L`
per-group term still favours lanes.

> **The copy overlap does not just save 4.65 ms — it dissolves the reason to
> prefer more lanes.** Spatial parallelism's apparent advantage over channel
> parallelism in this architecture was a **control-path artifact**, not a
> fundamental property. That is a paper-grade finding: it says the DW/PW
> parallelism trade-off is decided by the drain schedule, not by the dataflow.

**Consequence: `N_LANES=8, N_OC=32` + copy overlap is the global optimum in
practice** — it ties the 16-lane point and is already built and measured.
`N_LANES=16` would cost three RTL projects (windower, MAC cascade, 128-bit
datapath) plus BD and host changes for ≤0.2%. **P5 should be closed, not
deferred.**

### 29.8b The optimum arrangement, and why we are not on it

Full derivation in `DSP_ALLOCATION_ANALYSIS.md` **§7.0**. Summary:

**Optimum: `N_LANES=16, N_OC=16, K=3, DW MACs in fabric` — 194/220 DSPs,
≈16.58 ms, ≈42 fps.** Split is PW 162 / DW 32.

Why each axis is there:
- **`N_LANES` maximised** — it is the only axis in all four denominators of the
  PW cost expression, and the only one that reduces the *group count*, which
  multiplies every per-group cost. Lanes dominate output channels at equal DSP
  cost (Theorem 2).
- **`N_OC`=16** — at `L=16` the PPUs alone take `4L=64` DSPs, capping the grid at
  `N_OC ≤ 19`. 18–19 is 1.3% better; 16 is chosen for clean `cout` alignment.
- **`K`=3** — DW time depends only on the product `L·K` (Theorem 1), so `K` is a
  threshold, not a throughput axis. `K=9` saves 0.03 ms for ~8,500 more LUT.
- **DW's MACs in fabric** — cheapest DSPs in the design to release (88.8 LUT/DSP
  vs 533 for PPUs), *cycle-neutral*, and what makes `L=16` fit at all.

**Is the DW/PW split optimal there? Yes** — DW sits at its minimum non-binding
allocation and PW takes the remainder. The engines are asymmetric: PW gets
2 MAC/DSP from activation packing, DW gets 1 (a depthwise kernel has nothing to
share), and measured per-DSP productivity is **2.48 vs 0.49 MMAC/DSP/frame — 5.1×**.
DW's requirement is a *rate*: 72 MACs at `L=8` is exactly what sustains one beat
per cycle at stride 1. So the rule is **"DW gets a rate, PW gets the remainder"**.

**Why we are not running it:**
1. **Not reachable by parameter change, and `N_LANES=16` on the BD would be
   SILENTLY WRONG.** `N_LANES` is a data-layout contract, not a PW width: one
   beat is *one channel × N_LANES adjacent pixels*, and DW hardcodes 8
   (`dw_fused_axis.sv`: `CORE_W = 8*DATA_WIDTH`, `m_axis_tdata [63:0]`). Setting
   16 makes the shell's FIFO fuse two consecutive 64-bit writes — which are two
   different CHANNELS — into one 128-bit "16-pixel" word. It elaborates,
   synthesises, runs, and every value is wrong. Same failure class as block 2.
   Also breaks: `M_AXIS_DATA_WIDTH` 128 into a 64-bit S2MM, the chained handoff,
   and `pack_input_group_major`.
   **`K` is not a parameter either** — `conv_mac_array.sv` unrolls the 3×3 by
   hand as `p_cas[0]`…`p_cas[8]`. `K=3` is a cascade restructure.
2. **A cheaper change reaches the same place.** Removing the serialised
   group-boundary copy (§29.7) takes the *built* `L=8, N_OC=32` from 20.76 →
   **16.11 ms — better than the `L=16` optimum** — inside the PW core, no
   rewrite.

That inverts the roadmap: `N_LANES=16` is no longer how to reach ~16 ms, it is a
way to go *past* it, and the 14.40 ms DMA floor caps that at ~46 fps.

### 29.8 `N_LANES=16` is not a parameter change

There is **no `N_LANES` parameter anywhere in the DW path** — not in
`dw_banked_window_8x.sv`, `conv_mac_array.sv`, `dw_fused_core.sv` or
`dw_fused_axis.sv`. The windower is hardcoded to 8 lanes and structurally so (the
10-position `X_byte`/`is_x_valid` halo scheme, `s2_R*_hold[7:0]`, "one dense
8-lane beat per input group pair"). PW parameterises cleanly — OOC synthesis at
`L=16` works (18,754 LUT, 160 DSP, BRAM 6→12). So `L=16` is a **rewrite of the
depthwise windower**, the most intricate module in the design.

**And it would buy less than it appears.** Blocks 0–2 are DMA-bound and contribute
11.52 ms — 70% of the frame — for *any* `L` and `N_OC`. The absolute floor is
`Σ max(read, write) = 14.40 ms ≈ 46 fps` for the CNN at 64-bit DMA. `L=16` lands
~16.5 ms, i.e. within 15% of a bound it cannot cross. `L=16, N_OC=16` is also
**near-optimal but not optimal** — `N_OC=18–19` models 1.3% better, since
partial-batch drain removes the requirement that `N_OC` divide `cout`.

Beyond that the only lever is the **second HP port pair (HP2/HP3 are unused)**,
which would halve the DMA floor.

### 29.6 Open decisions

- **Pack dispatch** (§26): now **four** builds. `ptr-hoisted` 4.32 / 5.47 / 4.32 /
  **4.31**; NEON 5.00 / 5.00 / 5.07 / **5.07**. So `ptr` is faster in **3 of 4**
  and its one bad sample looks like the outlier, not the rule. Dispatch is on
  NEON, leaving **~0.76 ms** on the table — worth **33.8 → ~34.7 fps**. One
  identifier at `main.c:1997`. Recommendation has flipped: take `ptr`, and quote
  fps from a run whose PACKBENCH line is recorded alongside it.
- **Op-count convention** (§15): still unmade, still blocks every comparison.

---

## Summary of what still blocks drafting

| # | gap | cost to close |
|---|---|---|
| 1 | **Block diagram** (§2) | ~half a day, no new data needed |
| 2 | ~~**Power** (§20)~~ | ✅ CLOSED 2026-08-05 — 76.15 mJ/frame measured. Accelerator-only share is **not obtainable** on this board (15.625 mA telemetry quantum); state the bound, not a delta |
| 3 | **Correctness** (§21) | needs the DW offset + edge bugs fixed first |
| 4 | **Op-count convention** (§15) | one decision |
| 5 | **Baseline comparison** (§24) | literature work, no hardware needed |
| 6 | **100-frame variance** (§17) | one run |
| 7 | **Critical path** (§19) | one Vivado query |
| 8 | **Codebook scope** (§7) | project-level decision — it does not exist |
| 0 | ~~**Frame time / fps / energy**~~ | ✅ **CLOSED 2026-08-08 — 29.552 ms, 33.8 fps, 59.18 mJ/frame, 2.0027 W.** See §29.0. Supersedes every earlier figure; §17, §20 and §24 must be refreshed from it |
| 9 | ~~**`pack` frame time** (§26)~~ | ✅ CLOSED 2026-08-07 — measured 37.228 ms / 26.9 fps, pack 5.48 ms. But the ptr-hoisted variant proved UNSTABLE across builds (4.32 → 5.47) while NEON held 5.00 in both, so the dispatcher now selects NEON. One more ELF-only run to confirm ~36.76 ms / 27.2 fps |
| 10 | ~~**DSP allocation** (§4)~~ | ✅ CLOSED 2026-08-07 — see §28. §4's "~84 non-MAC DSPs" is **wrong**; 50 of them are accumulators. §4 and §22 both need rewriting from §28 |
| 11 | ~~**Operating point not built**~~ | ✅ BUILT 2026-08-08. `N_OC=32` + partial-batch drain + v3 + Option C + DW `USE_DSP(0)` + P0 + P1. WNS **+0.119 ns**, WHS +0.014, LUT 53.6%, BRAM 52.5% unchanged, DSP 220/220, all constraints met. Awaiting the board run (predicted 20.76 ms, ~35.4 fps) |
| 12 | ~~**Block 2 anomaly**~~ | ✅ CLOSED 2026-08-08 — root-caused as a real data-corruption bug and FIXED. §21, §29.2. It was in every prior bitstream |
| 13 | **Correctness still unvalidated** (§21) | the golden model now EXISTS (`golden_lane` in `pw_cfg_tb.sv`) but cannot run in simulation because of X (§29.4). Validate on **hardware** instead — one board session closes the largest gap in this document |
| 13b | ⚠️ **Timing margin is nearly gone** (§29.7) | WNS **+0.043 ns**, WHS **+0.007 ns** after the split shadow. MET with zero failing endpoints, but a re-place-and-route could flip either. The critical path has MOVED to the DW fabric MAC cascade (13 logic levels, 8 CARRY4) — no longer `first_ic`. **Fix: `ACC_WIDTH` 32 → 24 in the DW core.** With `USE_DSP(0)`, `CAS_WIDTH = ACC_WIDTH`, so the 32-bit adder *is* those 8 CARRY4; 9 taps of int8×int8 peak at 146,304 = **18 bits**, so 24 is still generous and removes 2 CARRY4 for free. Re-run the DW and cascade TBs — narrowing an accumulator silently saturates |
| 14 | **Per-group overhead** (§29.7) | `S_WAIT_PPU` is 46% of per-group time and is pure waiting on the shadow copy, which overlaps compute **0.00** cycles. Worth **4.65 ms → ~41.8 fps**, equal to `N_LANES=16` without the windower rewrite. Blocked on #13 |
