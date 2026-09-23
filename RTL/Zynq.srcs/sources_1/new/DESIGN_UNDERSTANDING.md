# Design Understanding — DW/PW CNN Accelerator (Zynq-7020)

> Single source of truth for the accelerator architecture. This file replaces and
> supersedes the earlier scattered notes (dataflow summaries, classification,
> arithmetic-intensity reports, and the two MAC implementation plans). Everything
> below was re-verified against the **current RTL**, not earlier drafts.
>
> Last reconciled with RTL: 2026-06-27, **except §2A and §2B (2026-07-30)**.
>
> ### CURRENT SYSTEM AT A GLANCE (2026-07-30) — see §2B for detail
> ImageEncoderLite @720p on ZC702: **36.27 ms/frame = 27.6 fps**.
> PL 100 MHz, N_OC=16, N_LANES=8, DSP 220/220, WNS +0.321 / WHS +0.012 ns.
> **Timing only — outputs are dummy-weight and NOT validated.**
>
> ⚠️ **§2's "DW path" table and all of §3 describe the SUPERSEDED DW datapath**
> (`dw_plane_run_axis` → `compute_engine` → `line_buffer_8x`). That chain is no
> longer instantiated in `hw.bd`. The live DW path is the **fused** one —
> `dw_fused_axi` → `dw_fused_axis` → `dw_fused_core` → `dw_banked_window_8x` —
> documented in **§2A** below. §3 is retained because `conv_mac_array` and `ppu`
> are unchanged and still accurate, and because the legacy path is where the
> old stride-2 support lived before it was deleted from the BD on 2026-07-09.
> For project status and open bugs see `DW_PW_FUSION_PLAN_V2.md` §0.

---

## 1. What This Design Is

A quantized **int8 MobileNet-style CNN accelerator** with two independent datapaths:

- **DW** — a direct streaming **3×3 depthwise** stencil engine.
- **PW** — a SIMD-style tiled **1×1 pointwise** vector dot-product engine.

Both share the same quantization back-end style (bias → requant multiply → rounded
shift → zero-point → clamp → optional ReLU). The network is PW-dominated: PW is ~93%
of MACs and is the compute-dense half; DW is ~7% and is memory/stencil-bound.

---

## 2. Module Map (current RTL)

### DW path
| Module | File | Role |
|---|---|---|
| AXI-Stream shell | `dw_plane_run_axis.sv` | 64-bit (8 px/beat) AXI-Stream in/out, input FIFO, output **byte-packer**, output FIFO, TLAST, `done_out`, input throttle. |
| Compute wrapper | `compute_engine.sv` | **8-wide** structural wrapper: `line_buffer_8x` + 8× `conv_mac_array` + 8× `ppu`. |
| Windowing | `line_buffer_8x.sv` | 8 px/beat line buffer; produces 8 parallel sliding 3×3 windows + 8 valid flags; internal zero-point padding; stride-2 filtering; `bypass_1x1` passthrough. |
| MAC | `conv_mac_array.sv` | One 3×3 depthwise MAC as a **9-stage input-skewed P-cascade**. |
| Post-process | `ppu.sv` | Per-lane quantization pipeline (8 stages). |

### PW path
| Module | File | Role |
|---|---|---|
| AXI-Lite shell | `pw_single_oc_axis_axi.sv` | Control/status registers, weight BRAM banks, bias/mult/shift param BRAMs. |
| AXI-Stream shell | `pw_single_oc_axis.sv` | Input/output FIFOs, packed group counting, TLAST, `done_out`. |
| Compute core | `pw_pixel_major_core.sv` | Pixel-major core: `N_LANES × N_OC` MAC grid, double pixel buffers, shadow accumulators, lane-parallel PPUs. |

### Other files in the tree (not part of the active DW/PW datapath)
- `Line_buffer.sv` (capital **L**) — the **old single-pixel** line buffer; **no longer instantiated** (replaced by `line_buffer_8x`).
- `pw_pixel_major_core_SIMCOPY.sv`, `pw_single_oc_tile_rtl_noctrl.sv` — simulation/experimental copies.
- `dw_pw_reorder.sv` — present in the tree; not covered in this analysis.
- `final_tb.sv` — DW golden-reference testbench (see §8).

---

## 2A. DW Datapath — CURRENT (fused), and STRIDE CONTROL

*Added 2026-07-30. This supersedes §2's DW table and §3 for everything except
`conv_mac_array` and `ppu`, which are unchanged.*

### 2A.1 Module chain

| Module | File | Role |
|---|---|---|
| AXI-Lite shell | `dw_fused_axi.sv` | Config/status registers, weight + PPU-param RAM loads. |
| AXI-Stream shell | `dw_fused_axis.sv` | 64-bit in/out FIFOs, input throttle (`prog_full`), **observed** TLAST via a 1-deep holding register, debug counters. |
| Compute core | `dw_fused_core.sv` | `dw_banked_window_8x` + 8× `conv_mac_array` + 8× `ppu`, with per-channel weights/params addressed by the windower's `ch_out` tag. |
| Windowing | `dw_banked_window_8x.sv` | Real 3×3 group-major windower, banked per channel. **Stride 1 or 2.** |

Stream order in and out is **group-major, channel-minor**:
`for row r: for group g: for channel c:` one 8-sample 64-bit beat.

`dw_fused_axis` derives TLAST from what the datapath *actually* produced (the
last beat still held when the core is done and the FIFO is empty), not from a
geometric formula. **Consequence: changing the emission count — as stride-2 does,
quartering it — requires no constant to be updated anywhere.**

### 2A.2 Register map (`dw_fused_axi`, AXI-Lite)

```
0x00 CTRL       W   bit0=start, bit1=clear_done
0x04 STATUS     R   bit0=done_sticky, bit1=busy
0x08 CIN_RUN    RW  C   (channels)
0x0C N_GROUPS   RW  G = ceil(W/8), groups per row
0x10 ZP_RELU    RW  [7:0]=zp_in  [15:8]=zp_out  [16]=relu_en  [17]=stride2
0x14 CH_ADDR    RW  channel index for weight/param loads
0x18..0x20 W0/W1/W2   weight load (W2 commits 9 weights)
0x24..0x2C BIAS/MULT/SHIFT  (SHIFT commits)
0x30 IMG_WIDTH  RW  real unpadded row width W
0x34 N_ROWS     RW  H
0x38..0x4C      R   debug: consumed / written / produced / winstate / flags / wincfg
```

### 2A.3 Stride is a RUNTIME bit, not a build option

**Both strides are always present in the fabric.** `ZP_RELU[17]` selects per
run, so different DW layers in the same network can use different strides
without rebuilding. It rides in `ZP_RELU` rather than taking a new address so
the IP's AXI-Lite map — and `component.xml` — are unchanged; it is as
quasi-static as the fields beside it, so the existing `dw_fused_timing.xdc`
multicycle on `reg_zp_relu_reg[*]` covers it.

**Bit 17 defaults to 0 = stride 1.** `main.c` writes this register in three
places (`:2390`, `:2738`, `:4054`), all via `pw_pack_zp_relu()`, which sets only
bits `[16:0]` — so everything currently running is stride-1, including on the
2026-07-30 bitstream that contains the stride-2 logic.

To enable it for a layer:

```c
dw_write_reg(DWF_REG_ZP_RELU,
             pw_pack_zp_relu(d->zp_in, d->zp_out, d->relu_en) | (1u << 17));
```

The stride-1 datapath is **untouched** by the stride-2 work. Stride-2 is a
parallel path selected by a mux on the output register:

```systemverilog
valid_out_vec   <= S2_r ? valid_s2            : valid_comb;
window[i][r][c] <= S2_r ? window_s2[i][r][c]  : window_comb[i][r][c];
```

With `S2_r = 0` the park/release machinery is bypassed entirely — identical
windows, emission count and order as before.

Both strides are needed concurrently, which is why this is runtime-selectable:
the **surrogate**'s three DW layers are all stride-2; the **debug 1435×2048
model** has stride-1 layers; and the current **timing proxy** runs each DW
stride-1 at its PW's resolution.

### 2A.4 How stride-2 works

Output pixel `(i,j)` is centred on input `(2i, 2j)` — the usual k=3/s=2/p=1
convention, giving `ceil(H/2) × ceil(W/2)` outputs.

**Columns.** A group covers input `x = 8g .. 8g+7`, and `8g` is always even, so
the stride-2 centres are **always lanes 0, 2, 4, 6** — the parity never shifts
between groups. Each input group yields 4 output pixels, so one input-group
**pair** `(2m, 2m+1)` yields 8, mapping to output `x = 8m..8m+7` in order.

The windower therefore **parks** the even group's 4 windows in a per-channel
`half_ram` and **releases** them together with the odd group's 4 as one dense,
all-valid beat. Parking is per channel because emission for a given channel
recurs only every `C` beats.

This matters because `dw_fused_core.sv:99` takes lane 0 to speak for the whole
beat (`win_valid = win_vvec[0]`) and emits all 8 PPU bytes. Emitting 4 valid
lanes would push garbage downstream. Keeping `valid_out_vec` all-or-nothing is
what lets `conv_mac_array`, `ppu`, `dw_fused_core` and the AXIS shell stay
**completely unchanged**.

**Rows.** Only even `Y` survive (`Y = s2_row - 1`). The `r == H` vertical-flush
row emits `Y = H-1`, odd, so for even `H` it is discarded — harmless, and left
in place rather than special-cased so the FSM is untouched.

**Right halo unused:** the last stride-2 centre in a pair is at `8(2m+1)+6`, so
taps reach only `k ≤ 8`, never `k = 9`.

Storage: `half_ram` is `CIN_MAX × (27 masked bytes + 4 x-valid bits)` — the 9
masked bytes per context row that the 4 even lanes' taps span, plus their
x-validity. Vertical validity is not stored: both groups of a pair sit in the
same row, so `is_y_valid` is identical for them and is applied once at emission.

### 2A.5 Preconditions and downstream consequences

**The hardware does NOT check these. The driver must.**

- **`n_groups` must be EVEN** (i.e. `img_width` a multiple of 16). Otherwise the
  last group of every row is an even-index group with no partner, and its 4
  output pixels are silently dropped. Surrogate widths 1280/640/320 all qualify.
- **`cin_run >= 2`.** `half_ram` is read at the s1 stage and written at s2;
  those are the same channel only when `C == 1`.

**Downstream geometry changes.** Output becomes `ceil(H/2) × ceil(W/2)`, and
beats drop to `(H/2) × (G/2) × C` — one quarter of stride-1. The next stage's
config must follow: for a DW→PW pair, PW's `TILE_PIXELS` must be the *halved*
resolution, or you get a length mismatch between what DW emits and what PW
expects. (For surrogate pair 0: DW in `720×1280`, out `360×640`, so PW sees
230,400 pixels.)

**BRAM cost is unconditional.** `half_ram` is instantiated whether or not
stride-2 is enabled, and contributes to the 92.5% BRAM utilisation of the
2026-07-30 build. If BRAM becomes the binding constraint, gating it on a
build-time parameter would reclaim it.

### 2A.6 Verification status

- **Simulated and passing**: `sim_1/new/dw_banked_window_8x_stride2_tb.sv`
  checks stride 1 and 2 against a golden model, all 72 taps per emission, plus
  emission count and order. Configs `C=3,W=32,H=8` and `C=8,W=64,H=16`:
  stride-1 96/96 and 1024/1024, stride-2 24/24 and 256/256, **zero tap errors**.
- **Present but dormant** in the 2026-07-30 bitstream (see
  `DW_PW_FUSION_PLAN_V2.md` §0 — it got there via the DW IP's duplicate source
  declaration, which is not a mechanism to rely on).
- **Not yet exercised on hardware.**

---

## 2B. Deployed System, Measured (2026-07-30)

*The whole-system view. Numbers here are measured on hardware unless marked.
Full derivation, per-block table and caveats: `DW_PW_FUSION_PLAN_V2.md` §0.*

### 2B.1 Execution model

Six DW→PW pairs run **chained**: each pair's PW output feeds the next pair's DW
directly in DRAM. This is legal because PW emits group-major/channel-minor with
`cout_rounded` channels per pixel group — byte-for-byte what the next DW
consumes — and with `N_OC=16` every `Cout` (16/32/32/32/64/64) is a multiple of
16, so `cout_rounded == Cout` and no repack is needed. Consequently pairs 1..5
do **no packing and no cache maintenance**: the CPU never touches those bytes.

Only pair 0 costs CPU time, converting the camera frame to group-major. Its
`gm_in` buffer is mapped `NORM_NONCACHE`, which removes the flush entirely *and*
makes the packing faster (the A9 L1 is write-allocate, so a cached buffer was
forcing a wasted 32-byte DRAM read per line that `pack` then overwrote).

### 2B.2 Cost model — PW is per-BATCH, not per-group

```
cyc/group = batches * (cin_run + 12.3) + 20.8          [measured, N_OC=16]
batches   = cout_rounded / N_OC          groups = pixels / 8
when DW binds:  total ~ DW_input_beats + PW pipeline tail   (NOT max())
```

~12 cycles per **batch** plus ~21 per **group**. The cause is in
`pw_pixel_major_core.sv`: `S_COMPUTE` cannot exit until `ppu_st == P_IDLE`, so
batch N+1's compute cannot finish until batch N's PPU drain has fully retired.
Validated across three `N_OC` values and nine layer configurations; it predicted
the encoder's frame time to 2.6% before the run.

The **ideal**, if batch N+1 overlapped batch N's drain, is
`max(batches*cin, cout_rounded)`. Closing that gap (double-buffer the shadow
accumulator; ~1 RAMB18, no DSPs) is worth ~10 ms/frame — the largest remaining
lever, and the only one left, since DSP is saturated.

### 2B.3 Measured results

```
frame 36.27 ms = 27.6 fps      HW 29.30 (80.8%)   pack 4.52 (12.5%)
                               prog 2.44 (6.7%)   cache 0.00
MACs/frame 193.8 M (DW 18.2%, PW 81.8%)  ->  5.35 GMAC/s
DMA 18.43 MB/frame  ->  509 MB/s sustained, 629 MB/s across the HW window
arithmetic intensity 10.51 MAC/byte      DSP peak 44.0 GMAC/s -> 12.2% achieved
```

The 12.2% of peak is limited by the per-batch serialisation above, not by the
MAC array.

### 2B.4 What is NOT established

**Functional correctness.** Every figure above was taken with synthetic dummy
weights; no output has ever been checked against a software reference. The DW
one-word output offset and the left/right-8-column edge bug are both open, and
chaining propagates them between blocks — non-zero-point bytes were observed in
blocks 1–5 where uniform `zp_out` was expected. No accuracy or reconstruction-
quality claim can be supported by this work as it stands.

---

## 3. DW Datapath (LEGACY — superseded, see §2A)

End-to-end:

```
AXI-Stream in (64b) → input FIFO → line_buffer_8x → 8×(3×3 window)
   → 8× conv_mac_array → 8× ppu → byte-packer → output FIFO → AXI-Stream out (64b)
```

### 3.1 `dw_plane_run_axis` (AXI-Stream shell)
- **64-bit datapath**: every AXI beat carries **8 pixels**. Input FIFO is distributed
  RAM, depth 2048; output FIFO depth 2048 with a `[64]=last` tag bit.
- **Real-pixel consumption**: `in_pop = consume_in && valid_in`. Padding positions are
  generated internally and do **not** pop the FIFO.
- **Byte-packer** (output side): the core emits up to 8 valid pixels per cycle on
  `valid_out_vec`. The packer extracts only the valid lanes, row-aligns them into
  64-bit beats via a 128-bit shift/mask (case-based, not a 15-CARRY4 chain), and
  flushes partial words at end-of-line (`eol`) and at `core_done`.
- **TLAST**: `out_words_r = ceil(out_w/8) * out_h` is latched at `start_in`;
  `will_last = (produced_cnt == out_words_r - 1)`.
- **Completion**: `done_out = m_axis_tvalid && m_axis_tready && m_axis_tlast` — pulses
  only when the **final output beat is accepted** downstream.
- **Backpressure**: `throttle_in = (out_count >= OUT_FIFO_DEPTH - 32)` pauses input
  consumption before the output FIFO can overflow.
- Output dims: stride-1 → `out_w=W, out_h=H`; stride-2 → `ceil(W/2), ceil(H/2)`.

### 3.2 `line_buffer_8x` (windowing)
- Stores rows in two BRAMs (`line0`, `line1`, 64-bit words = 8 px). Produces **8
  sliding 3×3 windows in parallel** plus `valid_out_vec[7:0]`.
- Padding is synthesized from `zp_in` for out-of-image positions (top/bottom via the
  `Y = row-1` row test, left/right via per-byte `is_x_valid`).
- **Stride-2** filtering happens at window-valid generation (`is_x_even && is_y_even`),
  not after the MAC.
- `bypass_1x1` produces a center-only window (1×1 passthrough mode), used by the PW
  reuse of this shell — in the DW wrapper it is tied to 0.
- **Latency ≈ 3 cycles**: Stage 0 (BRAM read/write issue) → Stage 1 (shift regs) →
  Stage 3 (registered `window` + `valid_out_vec`).

### 3.3 `conv_mac_array` (9-stage DSP P-cascade)
This is the **current** MAC — the older 2-stage "products + adder-tree" design is gone.

- Computes `sum_{9 taps} (window[r][c] − zp) * weight[r][c]` for one window.
- **Input skew**: tap *i* is delayed by *i* cycles (`G_SKEW` shift registers) so that a
  given window's 9 taps line up with the sequential cascade.
- **Linear cascade**: `p_cas[0] = prod0`; `p_cas[i] = p_cas[i-1] + prod_i`. The result
  appears at `p_cas[8]` after **9 cycles**; `valid_out = valid_sr[9]`.
- Skew alignment verified correct: window arriving at cycle *T* yields its full 9-tap
  sum at *T+9*, matching the 9-cycle `valid` delay.
- `USE_DSP` parameter selects the cascade implementation (see §7.1 — **note the current
  instantiation passes `USE_DSP(0)`**).

### 3.4 `ppu` (8-stage quantization pipeline)
Pipeline stages (each registered): pre-add (`acc+bias`) → 24-bit requant multiply
(DSP, `MREG`/`PREG`) → absolute value → barrel-shift + round-prep → rounding increment
+ truncate to 32b → sign-restore + `zp_out` → clamp `[0,255]` + optional ReLU → output
register. **Latency = 8 cycles.** The 24-bit `mult_conv` cap saves 2 DSPs per PPU.

> The earlier "PPU is unpipelined → 40–65 MHz bottleneck" finding is **resolved** — the
> PPU is now a fully pipelined 8-stage path.

### 3.5 DW pipeline latency (verified)
```
line_buffer_8x   conv_mac_array      ppu
   ~3 cyc      →     9 cyc       →   8 cyc
```
Window-in → pixel-out through the compute core = **17 cycles**. Throughput is **1
output pixel-group/clock** in steady state (8 pixels/clock from the parallel lanes).

---

## 4. PW Datapath (detailed)

End-to-end:

```
AXI in → input FIFO → packed N_LANES pixel group → double pixel buffer
  → N_LANES×N_OC MAC array → acc[oc][lane] → shadow_acc → lane-parallel PPUs
  → output FIFO → AXI out
```

- **Three overlapped processes** in one clocked controller:
  - *Main FSM* — schedules compute over pixel groups, input channels, output-channel batches.
  - *Load sub-FSM* — preloads the next pixel group into the inactive buffer (double buffering).
  - *PPU sub-FSM* — drains completed `shadow_acc` through `N_LANES` PPUs while compute continues.
- **Reuse**: per input-channel step the same `N_LANES` activations feed all `N_OC`
  weight banks → `N_LANES × N_OC` MACs/cycle.
- **Batching**: `cout_batches = cout_run / N_OC` (assumes `cout_run` divisible by `N_OC`);
  `tile_groups = tile_pixels / N_LANES`.
- **Output packing**: one output beat = `N_LANES` pixels for one output channel.
- **Completion**: `done_out` follows acceptance of the final packed output group.
- Config from `main.c`: `PW_N_LANES = 8`, `PW_N_OC = 3` (see open check §7.4).
  **Stale — `PW_N_OC` is 8 as of 2026-07-28** and must equal `CONFIG.N_OC` on the
  BD instance. It drives `cout_rounded`, the bank select `oc % N_OC` and the
  weight BRAM offset `(oc / N_OC) * Cin`; a mismatch misprograms weights
  **silently**, with no error and plausible-looking timing.

### 4.1 Choosing `N_OC` — read before changing it

`N_OC` is a BD parameter on `pw_single_oc_axis_axi_0`. Changing it changes cost
per pixel group (`max(batches × cin_run, cout_rounded)` cycles) and how much
padding is wasted when `Cout` is not a multiple of `N_OC`. Modelled cycles for
the surrogate: `N_OC=30 → 1,242,000`, `16 → 691,200`, `8 → 576,000`. `N_OC=8`
divides the surrogate's Couts (8/16/64) exactly, so `cout_rounded == Cout`
everywhere — no padding, and no inter-pair channel repack.

⚠️ **Historical trap (fixed 2026-07-30, but understand it before touching this).**
`ppu_issue_idx` in `pw_pixel_major_core.sv` was declared `$clog2(N_OC)` bits,
which **cannot represent `N_OC` when `N_OC` is a power of two**. The P_DRAIN
guard `ppu_issue_idx < N_OC` then never went false and the drain over-issued by
the PPU latency (9): 17 beats/batch at `N_OC=8`, 25 at `N_OC=16`, correct only
at `N_OC=30` because 30 is not a power of two. Every group emitted ~2× its
beats, so the predicted TLAST total was reached at roughly half the input, the
stream was truncated with a garbage tail, and the S2MM DMA completed looking
perfectly healthy. Fixed to `$clog2(N_OC+1)`.

**Lessons that outlive the bug:**
- PW's TLAST is still **predicted** (`produced_cnt == total_groups_r - 1`,
  `pw_single_oc_axis.sv:236`), unlike DW's observed TLAST. Any error in the
  produced-beat rate silently truncates the stream instead of hanging. Making it
  observed is an outstanding item.
- `cout_run` must be a multiple of `N_OC`; `tile_groups = tile_pixels / N_LANES`.
- Verify a parameter-width change reached the **netlist**, not just the source:
  `open_run synth_1` then count
  `get_cells -hier -filter {NAME =~ "*ppu_issue_idx_reg*"}`.
  Expect `$clog2(N_OC+1)` flops (4 at `N_OC=8`).
- The PW IP builds from **`Zynq\Zynq.srcs\component.xml`**, with the core from
  `Zynq.srcs\src\` and the shell/ppu from `Zynq.srcs\sources_1\new\` — editing
  the core in `sources_1\new\` changes nothing in the netlist. See
  `DW_PW_FUSION_PLAN_V2.md` §0.

---

## 5. Architecture Classification

| Block | Best label | Explicitly **not** |
|---|---|---|
| DW | Direct streaming 3×3 stencil / depthwise convolution engine | GEMM, SIMT, systolic |
| PW | SIMD-style tiled pointwise vector dot-product array | SIMT, general GEMM, systolic |
| Whole | Custom CNN dataflow accelerator with separate DW & PW datapaths | GPU/SIMT, generic matrix engine |

- **SIMD, not SIMT** — one FSM drives all lanes in lockstep; no thread IDs, warp
  scheduler, divergence masks, or per-lane program counters.
- **GEMM-like, not a GEMM engine** — PW is mathematically a tiled matmul, but the RTL is
  specialized for 1×1 conv (pixel-major groups, fixed `N_LANES`/`N_OC`, local accumulators).
- **Not systolic** — data is read from FIFOs/BRAMs into local MAC lanes; no PE-to-PE
  wavefront forwarding.
- DW is a direct spatial convolution (line-buffer windows), **not** im2col/GEMM.

Presentation wording: *"DW is direct convolution over a streaming 3×3 window. PW is a
banked dot-product engine — one controller, multiple pixel lanes and output-channel
banks. SIMD where it has lanes, not SIMT; parallel, but not systolic."*

---

## 6. Arithmetic Intensity

Convention: `1 MAC = 1 mul + 1 add`, `ops = 2·MACs`; activations/weights/outputs 1 byte;
bias 4B, mult 4B, shift 1B per output channel (9 param bytes/OC).

### DW (formula-level)
- `DW_MACs = out_h·out_w·9`, `DW_ops = 18·out_h·out_w`.
- `intensity = 18·out_h·out_w / (H·W + out_h·out_w + 18)`.
- Stride-1 large image → **~9 ops/byte**; stride-2 → **~3.6 ops/byte**.
- Reuse is spatial (line buffer); compute per output is fixed at 9 MACs.

### PW (formula-level)
- `PW_MACs = P·Cin·Cout`, `PW_ops = 2·P·Cin·Cout`.
- `intensity = 2·P·Cin·Cout / (P·Cin + P·Cout + Cin·Cout + 9·Cout)`.
- Large `P`, `Cin=Cout=C` → **~C ops/byte** (grows with channel count).
- RTL inner-step intensity ≈ `2·L·O / (L+O)` (e.g. L=8,O=3 → 4.36; L=8,O=5 → 6.15 ops/byte).

### Exact per-layer report (resolved layers, sizes from 2026-04-28)
| Group | Layers | GMAC | GOP | GB | Ops/Byte |
|---|---:|---:|---:|---:|---:|
| DW | 20 | 8.585 | 17.170 | 2.148 | 7.995 |
| PW | 21 | 112.737 | 225.473 | 2.160 | 104.403 |
| **All resolved** | 41 | 121.322 | 242.643 | 4.307 | 56.334 |

PW contributes ~92.9% of resolved MACs, DW ~7.1%. Both move similar bytes, but PW does
far more arithmetic per byte. (The unresolved first `/depthwise/Conv` row is excluded.)

<details>
<summary>Full per-layer table</summary>

| Layer | Type | GMAC | GOP | MB | Ops/Byte |
|---|---|---:|---:|---:|---:|
| `/pointwise/Conv` | PW | 0.264 | 0.529 | 96.983 | 5.455 |
| `/depthwise_1/Conv` | DW | 0.793 | 1.587 | 176.333 | 9.000 |
| `/pointwise_1/Conv` | PW | 5.290 | 10.580 | 264.502 | 40.000 |
| `/depthwise_2/Conv` | DW | 1.587 | 3.174 | 352.667 | 9.000 |
| `/pointwise_2/Conv` | PW | 10.580 | 21.160 | 352.670 | 59.999 |
| `/depthwise_3/Conv` | DW | 1.587 | 3.174 | 352.667 | 9.000 |
| `/pointwise_3/Conv` | PW | 10.580 | 21.160 | 352.670 | 59.999 |
| `/depthwise_4/Conv` | DW | 0.397 | 0.794 | 220.448 | 3.602 |
| `/pointwise_4/Conv` | PW | 3.970 | 7.941 | 110.291 | 71.996 |
| `/depthwise_5/Conv` | DW | 0.596 | 1.191 | 132.343 | 9.000 |
| `/pointwise_5/Conv` | PW | 7.941 | 15.881 | 154.411 | 102.849 |
| `/depthwise_6/Conv` | DW | 0.794 | 1.588 | 176.458 | 9.000 |
| `/pointwise_6/Conv` | PW | 10.587 | 21.175 | 176.471 | 119.989 |
| `/depthwise_7/Conv` | DW | 0.794 | 1.588 | 176.458 | 9.000 |
| `/pointwise_7/Conv` | PW | 10.587 | 21.175 | 176.471 | 119.989 |
| `/depthwise_8/Conv` | DW | 0.199 | 0.397 | 110.287 | 3.600 |
| `/pointwise_8/Conv` | PW | 3.970 | 7.941 | 55.166 | 143.939 |
| `/depthwise_9/Conv` | DW | 0.298 | 0.596 | 66.174 | 9.000 |
| `/pointwise_9/Conv` | PW | 7.941 | 15.881 | 77.245 | 205.593 |
| `/depthwise_10/Conv` | DW | 0.397 | 0.794 | 88.232 | 9.000 |
| `/pointwise_10/Conv` | PW | 10.587 | 21.175 | 88.288 | 239.838 |
| `/depthwise_11/Conv` | DW | 0.397 | 0.794 | 88.232 | 9.000 |
| `/pointwise_11/Conv` | PW | 10.587 | 21.175 | 88.288 | 239.838 |
| `/depthwise_12/Conv` | DW | 0.100 | 0.199 | 55.177 | 3.608 |
| `/pointwise_12/Conv` | PW | 2.654 | 5.308 | 22.178 | 239.353 |
| `/depthwise_13/Conv` | DW | 0.100 | 0.199 | 22.123 | 8.998 |
| `/pointwise_13/Conv` | PW | 2.654 | 5.308 | 22.178 | 239.353 |
| `/depthwise_14/Conv` | DW | 0.100 | 0.199 | 22.123 | 8.998 |
| `/pointwise_14/Conv` | PW | 2.654 | 5.308 | 22.178 | 239.353 |
| `/depthwise_15/Conv` | DW | 0.100 | 0.199 | 22.123 | 8.998 |
| `/pointwise_15/Conv` | PW | 2.654 | 5.308 | 22.178 | 239.353 |
| `/depthwise_16/Conv` | DW | 0.100 | 0.199 | 22.123 | 8.998 |
| `/pointwise_16/Conv` | PW | 2.654 | 5.308 | 22.178 | 239.353 |
| `/depthwise_17/Conv` | DW | 0.100 | 0.199 | 22.123 | 8.998 |
| `/pointwise_17/Conv` | PW | 2.654 | 5.308 | 22.178 | 239.353 |
| `/depthwise_18/Conv` | DW | 0.100 | 0.199 | 22.123 | 8.998 |
| `/pointwise_18/Conv` | PW | 2.654 | 5.308 | 22.178 | 239.353 |
| `/depthwise_19/Conv` | DW | 0.025 | 0.050 | 13.828 | 3.599 |
| `/pointwise_19/Conv` | PW | 0.664 | 1.327 | 5.589 | 237.434 |
| `/depthwise_20/Conv` | DW | 0.025 | 0.050 | 5.534 | 8.993 |
| `/pointwise_20/Conv` | PW | 0.608 | 1.217 | 5.354 | 227.216 |

</details>

### Throughput (needs measured Fmax)
At 150 MHz, with `N_LANES=8, N_OC=3`: DW peak ≈ `18·Fmax` = 2.7 GOP/s; PW peak ≈
`2·N_LANES·N_OC·Fmax` = 48·Fmax = 7.2 GOP/s. Sustained will be lower under load/drain/DMA stalls.

---

## 7. Current State & Open Items (coding-relevant)

### 7.1 ⚠️ The DW cascade is built in **fabric**, not DSPs
`compute_engine.sv` instantiates `conv_mac_array` with **`USE_DSP(0)`**, which forces
`(* use_dsp = "no" *)` and `CAS_WIDTH = ACC_WIDTH (32)`. The cascade's original goal was
to move the adder trees into DSP `PCOUT→PCIN` and **save ~1,152 LUTs**. As wired today
that LUT saving is **not realized** — it's a fabric adder cascade. Decision needed: keep
fabric (DSP-budget reasons: 8 lanes × 9 = 72 DSPs, plus 8 PPUs) or switch to `USE_DSP(1)`.

### 7.2 ⚠️ `done_out` alignment (14) vs. core latency (17) — verify in sim
`compute_engine.sv` delays `lb_done_out` by **14 cycles** to align `core_done` with the
MAC+PPU drain. Measured core latency is `9 + 8 = 17`. The 14 figure is empirical
(accounts for where `lb_done_out` asserts relative to the last window). `core_done` feeds
the byte-packer flush, so misalignment can cause premature/late partial-word flush.
**Confirm against `final_tb` before trusting it.**

### 7.3 Vestigial / dead ports (harmless, candidates for cleanup)
- `compute_engine`: `psum_in`, `psum_clear`, `pad_top` are inputs but **unused** (the
  MAC no longer takes a partial sum; padding is row-based in `line_buffer_8x`).
  `bypass_1x1` is wired through to the line buffer but tied to 0 by the DW wrapper.
- `dw_plane_run_axis`: ties `psum_in('0)`, `psum_clear(1'b1)`, `bypass_1x1(1'b0)`.
- `Line_buffer.sv` (capital L) is dead — safe to remove from the project sources.

### 7.4 Open checks (from analysis)
- Confirm synthesized `N_LANES`/`N_OC` (RTL defaults differ across PW files; `main.c` uses 8/3).
- Confirm `cout_run % N_OC == 0` and `tile_pixels % N_LANES == 0` always hold.
- Replace estimated Fmax/DSP/BRAM numbers with measured Vivado utilization/timing.

---

## 8. Verification

`final_tb.sv` (in `sim_1/new`) runs a full DW convolution layer through
`dw_plane_run_axis` and compares every output byte against a golden reference file,
including TLAST position and `done_out`. Run it in Vivado after any DW datapath change;
it is the regression gate for the cascade, the PPU pipeline, and the byte-packer.

> Vivado simulation cannot be run from the agent environment — these runs are manual.
