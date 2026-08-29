# Fix MAC Critical Path: Line Buffer BRAM → mac_out_reg

## Problem

**-8 ns slack** at 150 MHz target (6.67 ns period) on the path:

```
line1_reg/CLKBWRCLK → [BRAM Tco] → s0_2 (shift reg) → window[0][2] (comb) →
  9× (subtraction + DSP multiply) + balanced adder tree → mac_out_reg[29]/D
```

The entire MAC computation — 9 multiply-subtracts + a 5-level adder tree — is a single combinational stage between the line buffer's registered window outputs and `mac_out_reg`.

## Root Cause

The `window` output of `line_buffer` is **purely combinational** — it's just a mux on shift registers ([line_buffer.sv:68–80](file:///e:/Zynq/Zynq.srcs/sources_1/new/line_buffer.sv#L68-L80)). So the real critical path is:

```
BRAM Tco (~2 ns) + shift_reg setup → window comb mux → 
  9× (9-bit subtract + 9×8 DSP multiply) + adder tree (~8–10 ns) → mac_out_reg setup
```

Total: ~12–14 ns, vs 6.67 ns period → **-5 to -8 ns slack**. Matches your report.

---

## Proposed Changes

### [conv_mac_array.sv](file:///e:/Zynq/Zynq.srcs/sources_1/new/conv_mac_array.sv)

Split the single combinational stage into **2 pipeline stages**:

| Stage | Logic | Critical path |
|-------|-------|--------------|
| **S1: Products** | 9× `(window[r][c] - zp) * weight[r][c]` — DSP multiplies | ~3–4 ns (DSP Tco) |
| **S2: Adder tree + output** | Balanced add of 9 registered products + `psum_in` → `mac_out` | ~3–4 ns (adder tree) |

#### Detailed changes:

**Stage 1** — register the 9 DSP products + `valid_in` + `psum_clear` + `psum_in`:
```diff
-// 1-cycle latency output regs (same as before)
-always_ff @(posedge clk or negedge rst_n) begin
-    if (!rst_n) begin
-        mac_out   <= '0;
-        valid_out <= 1'b0;
-    end else begin
-        valid_out <= valid_in;
-        if (valid_in) mac_out <= mac_next;
-    end
-end
+// ---- Stage 1 registers: capture DSP products ----
+logic signed [17:0] p00_r, p01_r, p02_r, ...;
+logic               valid_s1;
+logic               is_dw_s1, psum_clear_s1;
+logic signed [ACC_WIDTH-1:0] psum_in_s1;
+
+always_ff @(posedge clk or negedge rst_n) begin
+    if (!rst_n) begin ... end
+    else begin
+        valid_s1 <= valid_in;
+        p00_r <= p00; p01_r <= p01; ... p22_r <= p22;
+        is_dw_s1     <= is_depthwise;
+        psum_clear_s1 <= psum_clear;
+        psum_in_s1    <= psum_in;
+    end
+end
+
+// ---- Stage 2: adder tree + output register ----
+// (balanced add from registered products → mac_out)
```

> [!IMPORTANT]
> This adds **+1 clock of latency** to the MAC (total: 2 clocks from `valid_in` to `valid_out`, vs 1 today). Combined with the PPU pipeline, the total compute engine latency goes from 2 → 4 clocks. As confirmed in the earlier analysis, all parent modules count output pulses and tolerate arbitrary latency.

### No changes to other files

| Module | Why no change needed |
|--------|---------------------|
| [compute_engine.sv](file:///e:/Zynq/Zynq.srcs/sources_1/new/compute_engine.sv) | Pure structural wire-through; MAC `valid_out` feeds PPU `valid_in` |
| [dw_plane_run_axis.sv](file:///e:/Zynq/Zynq.srcs/sources_1/new/dw_plane_run_axis.sv) | Output FIFO counts `valid_out` pulses; latency-agnostic |
| [line_buffer.sv](file:///e:/Zynq/Zynq.srcs/sources_1/new/line_buffer.sv) | Upstream producer; unchanged |
| [ppu.sv](file:///e:/Zynq/Zynq.srcs/sources_1/new/ppu.sv) | Already pipelined in previous step; input contract unchanged |

---

## Verification Plan

### Automated — Existing Testbench

The existing testbench [final_tb.sv](file:///e:/Zynq/Zynq.srcs/sim_1/new/final_tb.sv) runs a full DW convolution layer through `dw_plane_run_axis`, comparing every output byte against a golden reference file. It will catch any functional regression from the pipelining change.

> [!NOTE]
> I cannot run Vivado simulation from this environment. After I make the edit, please run the `final_tb` simulation in Vivado to verify **PASS** output. The testbench already checks:
> - Pixel-by-pixel match against reference
> - Correct TLAST positioning
> - `done_out` assertion
