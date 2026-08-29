# DW L0 Hardware-Mismatch Debug — Handoff (2026-07-02)

## DECISION & FINAL STATUS (2026-07-02) — PARKED; moving to the fused DW rewrite
**Root cause is localized (not the DW core — the DMA↔core input-stream delivery), and we are DONE
debugging the current design.** The user is moving on to the **fused DW→PW rewrite** (new reordering RTL
+ modified DW) and will **attach an ILA to the NEW DW** to nail and fix this beat-delivery bug there,
rather than keep band-aiding the current build.

### The bug, in one line
The DW **core RTL is proven correct in every sim** (behavioral, +DMA-stress, post-synth functional,
post-synth timing/SDF). On the board, the **AXIS input stream delivered by the DMA/interconnect is off by
beats** — the core is fine, the delivery is not. Three confirmed hardware-only symptoms, all beat-level,
almost certainly ONE integration defect:

| # | Symptom | Proven by | Software band-aid (in main.c, gated) |
|---|---|---|---|
| 1 | **First input beat dropped** → whole output shifted −8 columns | `DW_DIAG_PRIME_BEAT=1` (prepend a beat) snaps impulse cols 31/32/33 → 39/40/41; `DW_DIAG_START_SETTLE` does nothing (structural, not timing) | prime one beat ✅ |
| 2 | **line0/line1 BRAM banks swapped** → top/middle kernel rows w[0]/w[1] swapped (w[2]=streaming always correct) | impulse row order flips; identical on even+odd rows (uniform) | `DW_DIAG_WEIGHT_SWAP=1` (pre-swap kernel rows 0/1) ✅ |
| 3 | **Last input beat of each row missing** → rightmost ~8 output cols = background (58) every row | right-edge dump: correct to ~col 1426 then all 58; `[CMP] loc` = 47150/55414 in cols≥1427 | none (exposed once 1&2 fixed) |

With band-aids 1+2 on and real input, `[CMP] L00` = **55,414 mismatches out of 8,816,640 (99.4% correct)**,
the residual being symptom #3 (right edge) + a little top-edge/scatter.

### For the fused rewrite (what to carry over)
- **Verify the new DW against golden L0 the same way**: golden values in §2, the impulse method in §4, and
  the `[CMP]` + location-bin + right-edge dumps already in `main.c` (`compare_tensor_vs_ref`, `run_dw_layer_real`).
- **The integration seam to design out / watch with the ILA:** the new DW's `s_axis` must capture **beat 0**
  and every row's **last beat**. Prime suspects in the CURRENT design (check the analog in the new one):
  the **`!start_in` gate on `s_axis_tready`** (`dw_plane_run_axis.sv` ~L130) eating the first beat during the
  start pulse, the **start↔MM2S-arm sequencing** in `hw_dw_run_plane()`, and/or a **smartconnect register
  slice** dropping beats at start. ILA on `s_axis`+`m_axis`, trigger on first `s_axis_tvalid`, then literally
  count beats per row.
- Fusion design docs: `C:\Users\Fahad\TCSVT\sources_1\new\DW_PW_FUSION_PLAN.md` and
  `FUSION_HANDOFF_2026-06-28.md`.

### main.c diag macros — set to 0 to restore a normal run (all currently instrumented):
`DW_DIAG, DW_DIAG_STOP_L0, DW_DIAG_IMPULSE, DW_DIAG_START_SETTLE, DW_DIAG_PRIME_BEAT, DW_DIAG_WEIGHT_SWAP`.
Backups: `sources_1/new/backup_pre_hwdebug/` (main.c, line_buffer_8x.sv), `sim_1/new/backup_pre_hwdebug/`
(final_tb.sv). RTL edit made and left in place: `line_buffer_8x.sv` FSM+toggle resets → synchronous
(cleared one REQP-1839; no functional change on its own).

---

## TL;DR
The first depthwise layer **L0** byte-matches the golden reference in **every simulation**
(behavioral, behavioral+DMA-stress, post-synth functional, post-synth timing w/ SDF) but
produces a **wrong result on the board**. We have proven the DW core RTL is correct and
localized the fault to a **hardware-only, deterministic, uniform transform**:

> **hardware output(r,c) = correct_conv(r, c+8)  AND  the top/middle kernel rows (w[0]/w[1]) are swapped.**

i.e. a **constant −8 column shift** (one 8-pixel AXIS beat) + a **w[0]↔w[1] swap**, identical on
every row (even and odd). The bottom kernel row `w[2]` (fed by the **streaming** path, not BRAM) is
always correct; only the two `line0`/`line1` **BRAM banks** (`R0`/`R1`) are off. Conclusion:
**the line buffer's BRAM read path lands one beat/bank behind the streaming path — on silicon only.**

**Next step:** Tier-1 boundary **ILA** on the DW core's `s_axis`/`m_axis` to split "DMA delivers a
shifted stream" vs "core shifts internally." User is inserting the ILA now. See §8.

---

## 1. The system
- Zynq-7020 (`xc7z020clg484-1`), Vivado 2020.2. int8 MobileNet-style CNN accel, 42 layers
  (21 DW + 21 PW). DW = 3×3 depthwise; PW = 1×1 pointwise. Runs from an ARM app (`main.c`)
  that DMAs each layer through the accelerators and compares against golden `.bin` refs on SD.
- **Vitis app:** `Final_code_2` on platform **`C:\Users\Fahad\Zynq\Final_2.xsa`** (confirmed).
- DW core is a **packaged IP** `DW_conv_accel` (VLNV `xilinx.com:user:DW_conv_accel:1.0`),
  IP repo at **`C:\Users\Fahad\ip_repo\DW_conv_accel_1.0`**. It wraps `dw_plane_run_axis`.
- Clock: `clk_fpga_0`, 100 MHz (10 ns).

### Key files
| What | Path |
|---|---|
| ARM app (instrumented) | `C:\Users\Fahad\Zynq\Zynq.srcs\sources_1\new\main.c` |
| DW core RTL | same dir: `dw_plane_run_axis.sv`, `compute_engine.sv`, `line_buffer_8x.sv`, `conv_mac_array.sv`, `ppu.sv` |
| DW AXI-Lite wrapper (register decode + start) | `C:\Users\Fahad\Zynq\Zynq.gen\sources_1\bd\hw\ipshared\9c18\hdl\DW_conv_accel_v1_0_S00_AXI.v` |
| DW golden testbench (drives `dw_plane_run_axis` standalone) | `C:\Users\Fahad\Zynq\Zynq.srcs\sim_1\new\final_tb.sv` (top `tb_dw_plane_run_axis_auto_fixed`) |
| Reference data (instr/params/weights + input + golden outputs) | `C:\Users\Fahad\TCSVT\` (`mem\instr.bin`,`params.bin`,`weights.bin`; `ref\input_planar.bin`,`l00.bin`; `fpga_data\layers_config.json`) |
| Backups | `...\sources_1\new\backup_pre_hwdebug\` (main.c, line_buffer_8x.sv); `...\sim_1\new\backup_pre_hwdebug\` (final_tb.sv) |
| Live memory notes | `C:\Users\Fahad\.claude\projects\C--Users-Fahad-Zynq-Zynq-srcs-sources-1-new\memory\dw-l0-hardware-mismatch.md` |

---

## 2. Golden L0 facts (verified from `TCSVT` binaries and confirmed by on-board register read-back)
- L0: **H=2048, W=1435, Cin=Cout=3, kernel=3, stride=1, pad=1, zp_in=0, zp_out=70**, weight_addr=0, params_addr=0.
- W_padded = `(W+7)&~7` = **1440**; **180 beats/row** (8 px/beat, 64-bit AXIS); in_pixels = out_pixels = **2949120** bytes/plane.
- Per-channel params & weights (row-major 3×3 = [top; mid; bot]):
  - **ch0**: bias=-6209, mult=4029303, shift=31, w = `4 16 -2  -60 -27 10  -128 -42 11`
  - **ch1**: bias=1160,  mult=3554271, shift=31, w = `74 32 -17  127 53 -10  -16 -28 -9`
  - **ch2**: bias=-3054, mult=4193110, shift=31, w = `-5 92 127  -11 -8 2  4 -10 -8`
- Input `input_planar.bin` is planar `[C][H][W]` tightly packed (W=1435). ch0 row0[0..15] =
  `80 00 FF EE 00 65 1F 00 00 00 1A 00 84 E5 6B CD`.
- Golden output `l00.bin` (planar [C][H][W], W=1435). ch0 row0[0..15] =
  `2D 0F 1A 01 06 1B 22 16 32 24 33 27 39 1C 0F 26`.
- PPU math (verified): `out = clamp(round_half_even((acc+bias)*mult, shift) + zp_out, 0,255)`,
  `acc = Σ w_tap*(pix - zp_in)`. Bias-only (acc=0) value for ch0 = **58** (0x3A).
- **Impulse response, V=255 at interior, ch0** (this is the fingerprint we use everywhere):
  ```
  correct 3x3 stamp (rows R-1/R/R+1, cols C-1/C/C+1):
     64 38  0     <- w[2]  (bottom)
     63 45 30     <- w[1]  (middle)
     57 66 60     <- w[0]  (top)
  background (far interior) = 58
  ```

---

## 3. What is RULED OUT (with evidence)
1. **`main.c` programming** — on-board AXI **register read-back** (`[DIAG RB]`) of the DW slave
   regs matches golden dims/params/bias/mult/shift/weights for all 3 channels. Not it.
2. **DW AXI-Lite wrapper decode** — verified field-by-field in `DW_conv_accel_v1_0_S00_AXI.v`
   (`img_width=slv_reg2[11:0]`, `img_height=slv_reg2[27:16]`, params in reg3, bias/mult/shift
   reg4/5/6, weights reg7/8/9 row-major). Correct.
3. **SD files & input layout** — on-board `in_plane[0..15]` and `REF/L00.BIN` match golden.
   DRAM read-back of an injected impulse byte = 255 at the right offset → **input delivery to DRAM is correct**.
4. **Stale/old bitstream** — synth log shows `hw_DW_conv_accel_0_0` synthesized fresh from current
   HDL; the 7/2 build is current (a sync-reset edit we made *did* change the DRC, proving it took).
5. **Gross timing** — STA "met" (WNS +0.206 ns). Hold WHS **+0.014 ns** (razor-thin but 0 failing).
6. **Core logic** — **post-synth FUNCTIONAL sim PASSES** (matched all 2,938,880 L0/ch0 px). So the
   synthesized netlist (real RAMB w/ inferred WRITE_MODE, etc.) is functionally correct →
   read-first/collision and 3-bank theories are moot.
7. **Handshake logic** — behavioral sim with **DMA stress** (`s_axis_tvalid` gaps + `m_axis_tready`
   backpressure, knobs in `final_tb.sv`) PASSES.
8. **Real timing** — **post-synth TIMING sim (SDF, ~1h44m, stress on) PASSES** (all 2,938,880 px).
9. **Async-reset hazard (REQP-1839/1840)** — we converted `line_buffer_8x` `col/row/toggle` resets to
   synchronous; it synthesized (DRC violation moved from `col_reg`→`in_count_reg`) but produced
   **byte-identical** output → async reset is not the cause (it's a reset-event hazard).
10. **Output-side S2MM first-beat capture race** — experiment arming S2MM **after** the start pulse
    (`DW_DIAG_S2MM_AFTER_START`) → **no change**. Not an output-capture race.
11. **Accumulating / random DMA beat drift** — impulses at rows {16,512,1024/1025,1536/1537}
    all show the **same constant −8** (does NOT grow with row) → rules out drift / random beat loss
    spread through the stream. S2MM completes with **no timeout** (exact OUTPUT beat count).
    **NOT ruled out (correction):** a **single dropped/skipped FIRST INPUT beat at start** — that
    produces a *constant per-row* −8 (the row structure is baked into the linear stream), so it looks
    exactly like what we see. A first-beat handshake/start race is a LIVE hypothesis for the column
    half. The w[0]/w[1] vertical swap, however, points inside the line buffer (a horizontal beat skip
    doesn't obviously swap the two BRAM banks). Tier-1 ILA (does the core capture s_axis beat 0?)
    settles it. Cheap pre-ILA main.c tests: (a) settling delay between the start pulse and arming
    MM2S; (b) prime one extra dummy beat at the front of the transfer (if the core drops beat 0,
    priming realigns and the −8 vanishes).

**Net:** the standalone `dw_plane_run_axis` is correct under logic + handshake + real timing. The fault
is in the **integration seam** (DMA ↔ core startup/first-beat) or a hardware-only BRAM-read timing that
none of the standalone sims reproduce — the standalone TB drives a clean start→data handshake; the board
sequences start via SW AXI-lite writes then arms the DMA with real interconnect latency.

---

## 4. The bug, precisely characterized (impulse method)
Injecting single-pixel impulses (V=255) into L0/ch0 at input rows {16, 512, 1025, 1537}, col 40, and
scanning the whole output plane for non-background pixels:

- Every impulse's 3×3 response lands at **cols 31/32/33** (should be 39/40/41) → **constant −8 columns, every row, no drift.**
- Rows are correctly centered, but per row: `R-1 = w[2]` (correct), `R = w[0]`, `R+1 = w[1]`
  → **w[0]/w[1] (top/middle) swapped**, identical on **even and odd** rows (NOT parity-dependent).
- `w[2]` comes from the **streaming path `R2 = s1_px_in`** (no BRAM) → always correct.
  `w[0]`/`w[1]` come from **`line0`/`line1` BRAM banks (`R0`/`R1`)** → swapped/off-by-one.

**Refined split (2026-07-02) — likely TWO independent 1-beat defects, separable by logic type:**
- The −8 column shift also affects the **`w[2]` / streaming-path (`R2`) output** (impulse `w[2]`=`64 38 0`
  lands at cols 31/32/33 too). `R2` and the col-label/packer are **pure fabric** (no BRAM), which behaves
  identically in functional + SDF-timing sim + silicon (both sims passed). So a fabric misalignment can't
  arise internally → the −8 means the **input AXIS stream is delivered one beat out of phase** (dropped/
  skipped beat 0) = a **DMA/interconnect/first-beat delivery** issue, NOT the BRAM and NOT the output
  packer. → probe with `DW_DIAG_PRIME_BEAT` / `DW_DIAG_START_SETTLE` (main.c, no rebuild).
- The **w[0]/w[1] swap is a compute result** (which weight hit which input row) — the packer only relocates
  computed pixels and cannot cause it. So it's the line-buffer `R0`/`R1` **BRAM banks** or their **parity
  control** (`s1_row[0]` mux / `toggle`). This is a genuine BRAM/sim divergence → ILA (or the proven
  `DW_DIAG_WEIGHT_SWAP` band-aid).

The output byte-packer / flatten path in `dw_plane_run_axis` is **effectively exonerated**: it is pure
fabric and passed the SDF timing sim, so it is faithful on silicon.

---

## 5. Software compensation (CONFIRMED working, for L0) — answers "can we just shift inputs?"
The transform is uniform & shift-invariant, hence invertible:
- **Row swap → FIXED in software.** Programming the DW weights with **rows 0 and 1 pre-swapped**
  cancels the hardware swap. Confirmed on-board: with `DW_DIAG_WEIGHT_SWAP=1`, the impulse stamps
  flipped to correct order (`64 38 0 / 63 45 30 / 57 66 60`), uniform across even+odd rows.
- **Column −8 → invertible** by pre-shifting each input row right by 8 (pad 8 zp on the left), OR by a
  +8 output read offset. NOT yet wired up.
- **Caveats (why it is a band-aid, not a network fix):** loses ~8 columns at the right edge per row
  (0.6% of L0 W=1435, but ~9% of L40 W=90 — grows for small layers); the **4 stride-2 DW layers
  (L8/16/24/38)** use the decimation path and are **uncharacterized** (likely different); PW has its own
  latent `WRITE_FIRST` collision (REQP-181 on `pw_single_oc_axis_axi.../u_shadow_bram`). And it doesn't
  remove the seam the fused rewrite will re-hit.

---

## 6. Diagnostic instrumentation currently in `main.c` (all gated by macros near top)
```
#define DW_DIAG              1   // per-channel config/weights + AXI read-back + in/out dumps for L0
#define DW_DIAG_STOP_L0      1   // halt the run right after L0 + its compare
#define DW_DIAG_IMPULSE      1   // replace L0/ch0 input with impulses, scan output for the stamp
#define DW_IMP_R0 16 / DW_IMP_C0 40 / DW_IMP_V 255
   // impulse rows array in run_dw_layer_real: { 16, 512, 1025, 1537 }
// --- experiments, set ONE at a time (0 = off); keep DW_DIAG_IMPULSE=1, DW_DIAG_WEIGHT_SWAP=0 for the -8 tests:
#define DW_DIAG_START_SETTLE 0   // N dummy AXI reads between start pulse and MM2S arm (try 256). -8 changes => start/first-beat race
#define DW_DIAG_PRIME_BEAT   0   // prepend 1 dummy input beat. If core drops beat 0, impulse cols snap 31/32/33 -> 39/40/41
#define DW_DIAG_WEIGHT_SWAP  0   // pre-swap kernel rows 0/1 -> CANCELS the row swap (confirmed working when =1)
```
(Removed the earlier `DW_DIAG_S2MM_AFTER_START` experiment — arming S2MM after start made no difference.)
The two first-beat tests live in `hw_dw_run_plane()`; `DW_DIAG_PRIME_BEAT` DMAs from `in_plane-8`, length `in_pixels+8`.
Interpretation: if either test snaps the impulse stamps from cols 31/32/33 back to **39/40/41**, the −8 is a
**main.c/DMA first-beat handshake** issue (fixable in software, no rebuild). If both leave it at 31/32/33, the −8 is
inside the core/interconnect → proceed with the Tier-1 boundary ILA.
Key functions: `hw_dw_run_plane()` (per-plane DMA + reg program; has the read-back and the S2MM-order
experiment), `run_dw_layer_real()` (loops channels; has the impulse inject, weight-swap, and output scan).

**RTL edit made:** `line_buffer_8x.sv` — the FSM block (`running/col/row/done_out`) and the `toggle`
block were changed from `@(posedge clk or negedge rst_n)` to synchronous `@(posedge clk)`
(behavior-identical in sim; cleared one REQP-1839; did NOT change hardware result). This edit is IN
PLACE in `sources_1/new` and in the IP repo, and was in the 7/2 bitstream.

---

## 7. Recommended macro settings FOR THE ILA RUN
Turn off the compensations/impulse so the ILA sees the **raw, real-data** behavior:
```
DW_DIAG_IMPULSE         0   // real input, so s_axis beat 0 = 80 00 FF EE 00 65 1F 00 (identifiable)
DW_DIAG_WEIGHT_SWAP     0   // see the uncompensated bug
DW_DIAG_S2MM_AFTER_START 0  // (or 1 — no effect)
DW_DIAG_STOP_L0         1   // stop after L0
DW_DIAG                 1   // keep the read-back/prints
```

---

## 8. NEXT STEP — Tier-1 boundary ILA (user is inserting this now)
**Goal:** decide input-vs-core. Probe the DW core's AXIS boundary:
- `s_axis_tdata[63:0]`, `s_axis_tvalid`, `s_axis_tready`  (DMA → core input)
- `m_axis_tdata[63:0]`, `m_axis_tvalid`, `m_axis_tready`, `m_axis_tlast`  (core → S2MM output)

**Setup:** In the BD, right-click the `s_axis` / `m_axis` interfaces on `DW_conv_accel_0` → Debug
(inserts System ILA). Trigger on `s_axis_tvalid` rising, trigger position early (~sample 64 of 1024),
depth 1024. Run with **real input** (see §7). Program `.bit`+`.ltx`, arm, run `main.c`, capture.

**How to hand results to the next chat:** In the ILA waveform pane → right-click → **Export ILA Data →
CSV**. Paste the **header row** + roughly the **first ~300 samples** (covers startup + first row's input
and its first output beats). A CSV is readable as text; a screenshot is not.

**What the next chat will check:**
- Is `s_axis` **beat 0 = `80 00 FF EE 00 65 1F 00`** and beats sequential? → DMA delivers correctly.
- On `m_axis`, do the **first output beats** correspond to output cols 0–7, or is the −8 already present
  when the core emits? → splits **DMA-delivery shift** vs **inside-the-core shift**.
- If input is clean but output is shifted → go **Tier-2**: add `(* mark_debug = "true" *)` in the IP RTL
  to `line_buffer_8x`: `col, toggle, line0_rd[7:0], line1_rd[7:0], s1_px_in[7:0], s2_col, valid_out_vec`
  and `dw_plane_run_axis`: `start_in, consume_in`. That shows the one-beat BRAM-vs-streaming offset
  directly (which register to fix).

---

## 9. Strategy / open decisions
- **Core rewrite won't fix this** — the core is proven correct; the defect is the DMA↔core BRAM-read
  seam, which the planned **fused DW→PW rewrite reuses**. So pin it before/with the rewrite.
- Options once the ILA names the register: (a) targeted RTL fix + one rebuild; (b) design it out in the
  fused rewrite; (c) short-term software compensation (§5) to unblock stride-1 testing only.
- Remember to also validate **PW** (own REQP-181 collision) and the **4 stride-2 DW layers** before
  trusting any full-network run.

## 10. Sims already run (all on standalone `dw_plane_run_axis` via `final_tb`, all PASS)
behavioral · behavioral+stress(gaps+backpressure) · post-synth functional · post-synth timing (SDF, ~1h44m).
The `final_tb.sv` STRESS knobs (`STRESS_IN_GAPS`, `STRESS_OUT_BP`) are currently `1`.
