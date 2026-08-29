# [compute_engine.sv](file:///e:/Zynq/Zynq.srcs/sources_1/new/compute_engine.sv) — Inefficiency Analysis & Throughput Report

## Architecture Summary

Top-level datapath chaining three sub-modules:

```
pixel_in → [Line Buffer] → 3×3 window → [Conv MAC Array] → acc → [PPU] → pixel_out
                                              ↑                       ↑
                                          weights/psum            residual inputs
```

The compute engine is a **structural wrapper** — it instantiates and wires `line_buffer`, `conv_mac_array`, and `ppu` with minimal glue logic.

---

## Current Throughput

| Metric | Value |
|---|---|
| **Pipeline depth** | **2 stages** (1 in MAC + 1 in PPU) |
| **Initiation interval** | **1 clock** (limited by line buffer `valid_out` rate) |
| **Latency** | **2 clocks** (MAC → PPU registers) |
| **Throughput** | **1 output pixel / clock** (when line buffer is producing) |

The **effective throughput** depends on the line buffer's `valid_out` rate, which is governed by image dimensions, padding, and stride. During steady-state streaming of a row, it produces 1 pixel/clock.

---

## Identified Inefficiencies

### 1. 🔴 Combined Comb Path Spans MAC + PPU Boundaries

**Problem:** The MAC's combinational output (`mac_result`) is registered once inside `conv_mac_array`, then fed into the PPU where another massive combinational block runs (multiply + 68-bit barrel shift + clamp) before the PPU's output register.

The **real critical path** of the entire engine is determined by the *slower* of:
- MAC critical path: ~8.5–10.5 ns (9 multiplies + adder tree)
- PPU critical path: ~15–25 ns (33×32-bit multiply + 68-bit rounding)

> [!CAUTION]
> The PPU stage is the **overall bottleneck**, capping the entire engine at **~40–65 MHz** regardless of how fast the MAC can go. The MAC's ~100 MHz capability is wasted.

**Fix:** Pipeline the PPU (as noted in the PPU analysis). The MAC is already reasonably pipelined for its complexity.

---

### 2. 🔴 `ppu_trigger` Timing Mismatch in Residual Mode

**Problem (lines 108-109):**
```systemverilog
logic ppu_trigger;
assign ppu_trigger = mode_residual ? valid_in : mac_valid_out;
```

In **residual mode**, `ppu_trigger = valid_in` — the PPU fires on the *raw* input valid, bypassing the line buffer and MAC pipeline entirely. But `conv_acc_in` is still wired to `mac_result`, which is stale or corresponds to a *different* cycle's data.

This works **only because** the PPU ignores `conv_acc_in` in residual mode and uses `res_a_in`/`res_b_in` instead. But:

- `mac_result` still toggles, causing unnecessary switching power
- If `mode_residual` transitions mid-stream without proper fencing, there's a race

**Fix:** Gate `conv_acc_in` to zero in residual mode to save dynamic power:
```systemverilog
.conv_acc_in(mode_residual ? '0 : mac_result),
```

---

### 3. 🟡 Line Buffer + MAC Run Unnecessarily in Residual Mode

**Problem:** When `mode_residual = 1`, the output comes from `res_a_in`/`res_b_in` through the PPU. But the line buffer and MAC array are still connected and clocked — any `valid_in` pulses will shift data through the line buffer and compute MAC products, wasting dynamic power for results that are never used.

**Fix:** Gate `valid_in` to the line buffer when in residual mode:
```systemverilog
.valid_in(valid_in & ~mode_residual),
```

Or clock-gate the MAC entirely in residual mode (more aggressive power savings).

---

### 4. 🟡 `MAX_IMG_WIDTH` Hardcoded to 2048

**Problem (line 65):**
```systemverilog
.MAX_IMG_WIDTH(2048),
```

The line buffer allocates BRAM for 2048-pixel-wide rows. If typical images are much smaller (e.g., 224×224 for MobileNet), this wastes BRAM. More importantly, this parameter is **hardcoded** rather than propagated from the compute engine's parameters.

**Fix:** Add a `MAX_IMG_WIDTH` parameter to `compute_engine` and pass it through:
```systemverilog
parameter int MAX_IMG_WIDTH = 2048
```

---

### 5. 🟡 No Backpressure / Flow Control

**Problem:** There is no `ready` handshake between stages. The design assumes the downstream consumer can always accept `pixel_out` every cycle. If the consumer stalls:
- `valid_out` pulses are lost
- Data is silently dropped

For a self-contained accelerator this may be fine, but for AXI-Stream integration (common on Zynq) a `tready` backpressure signal is essential.

**Fix:** Add `ready_in` / `ready_out` handshake signals, or at minimum a small output FIFO to absorb stalls.

---

### 6. 🟢 `consume_in` Is Just a Wire Pass-Through

```systemverilog
assign consume_in = lb_consume_in;
```

This is fine — it's a clean pass-through for testbench observability. No inefficiency, just noting it exists.

---

### 7. 🟢 `mac_debug_out` Always Driven

```systemverilog
assign mac_debug_out = mac_result;
```

For synthesis, if `mac_debug_out` is unconnected at the parent level, it will be optimized away. If it **is** connected (e.g., to an ILA), it adds a small fanout. Mark it for debug only:

```systemverilog
(* mark_debug = "true" *) output logic signed [ACC_WIDTH-1:0] mac_debug_out
```

---

## End-to-End Throughput Summary

| Scenario | Throughput | Bottleneck | Est. Fmax |
|---|---|---|---|
| **Current design** | 1 pixel/clk | PPU comb path | **~40–65 MHz** |
| **PPU pipelined (3-stage)** | 1 pixel/clk | MAC comb path | **~95–115 MHz** |
| **PPU + MAC pipelined** | 1 pixel/clk | Line buffer / fabric | **~150–200 MHz** |
| **+ Backpressure added** | 1 pixel/clk (sustained) | Consumer rate | same Fmax |

**Current effective throughput: ~40–65 Mpixels/s**, bottlenecked by the PPU.

---

## Recommended Priority

1. **Pipeline the PPU** — immediate 2–3× Fmax gain (the engine bottleneck)
2. **Gate MAC/line-buffer in residual mode** — free dynamic power savings
3. **Add backpressure** — required for robust AXI-Stream integration
4. **Parameterize `MAX_IMG_WIDTH`** — saves BRAM for smaller models
5. **Pipeline the MAC adder tree** — needed only once PPU is no longer the bottleneck
