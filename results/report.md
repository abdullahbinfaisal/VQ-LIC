# Depthwise service-model validation

Generated 2026-09-07 by `sim/make_report.py`. Model: `model/service_model.py`, written from the equations alone.

**No constant was fitted.** Where prediction and measurement disagree the disagreement is reported as the result.

## Experiment A -- DW schedule sweep, RTL vs `T_DW`

Window: core = first cycle with `valid_in && consume_in` to the last cycle with `valid_out`, inclusive. AXIS = first `s_axis_tvalid && s_axis_tready` to last `m_axis_tvalid && m_axis_tready`.

`98` configurations, `98` usable.

| statistic | value |
|---|---|
| max abs error | 1953 cycles |
| mean abs error | 500.7 cycles |
| max pct error | 29.651% |
| mean pct error | 1.3534% |
| max abs error per row | 51.0000 cycles/row |
| mean abs error per row | 4.4205 cycles/row |

Distinct absolute errors, stride 1: `[19]`

Distinct absolute errors, stride 2: `20 values`

Full table: `results/dw_sweep.csv`.

### The disagreement, and what it is

**No constant was changed and no correction term was added.** What follows describes the residual; it does not repair it.

#### At the AXIS boundary the model needs no stride term

Measured at the AXIS ports -- first `s_axis_tvalid && s_axis_tready` to last `m_axis_tvalid && m_axis_tready` -- the residual is a **constant**:

| stride | n | `axis_cycles - T_DW` |
|---|---|---|
| 1 | 24 | **+36** (x24) |
| 2 | 74 | **+33** (x73), **+36** (x1) |

Zero variance on stride 1, and one exception on stride 2 -- the degenerate `H=1` case. **`T_DW` is exact as a rate model at this boundary**, off only by a fixed pipeline latency, and it needs no stride term to be so.

#### At the core boundary it does need one

At stride 1 the core residual is a single value across all 24 configurations: **+19 cycles**, independent of `H`, `W` and `c_in` -- the datapath fill/drain (18 stages, `1 + 9 + 8`, per the RTL's own comment) that a throughput-only model does not contain.

At stride 2 it is not constant. It is independent of `H` -- for a given `(G, c_in)` the same value appears at every height -- and equals exactly one row's work:

```
  stride 1:  core  ==  (H+1) * [c_in*(G+1) + 4]  +  19
  stride 2:  core  ==   H    * [c_in*(G+1) + 4]  +  19
```

exact on **24 of 24** stride-1 and **71 of 74** stride-2 configurations.

The 3 exceptions:

| H | W | c_in | stride | G | residual |
|---|---|---|---|---|---|
| 24 | 67 | 3 | 2 | 9 | -3 |
| 24 | 100 | 3 | 2 | 13 | -3 |
| 1 | 160 | 8 | 2 | 20 | +172 |

`W=67` and `W=100` are the only configurations in the sweep with an **odd `G`**, and both miss by exactly `-c_in`. Stride 2 decimates groups in pairs, so an odd `G` leaves a half pair. `W=1279` and `W=1435` are also ragged but have even `G` and land exactly. **So the ragged-width question splits in two:** `G = ceil(W/L)` is the right group count and the ceiling correction is what makes those configurations land at all; the residual then depends on the **parity** of `G`, not on whether `W` divides `L`. With exact division these four would have been wrong by a whole group per channel per row instead.

`H=1` at stride 2 is degenerate -- a 3x3 window over one row -- and is listed for completeness, not as a usable configuration.

#### Reading the two together

The two boundaries do not disagree about the engine's rate. They disagree about **where the window starts and stops**. The AXIS window spans the whole input stream; the core window ends at the last `valid_out`, and at stride 2 the core stops emitting one row before the input runs out, because only even output rows survive. That one row is the entire stride-2 residual, and it is why the core number is *lower* than `T_DW` while the AXIS number is a constant *above* it.

**Which boundary should the paper quote?** Blocks are composed over AXIS -- DW streams into PW through those ports -- so the AXIS window is the service the composed system actually sees, and that is the one `T_DW` predicts to a constant. The core-level result is reported because it was asked for and because it localises the difference, not because the model is wrong.

**Hypothesis for the one row.** The `(H+1)` form counts a vertical-flush row pass beyond the `H` real rows. At stride 1 that pass emits and the core window contains it. At stride 2 it produces no surviving output row, so the core window closes before it -- consistent with a residual of exactly one row, independent of `H`. Confirming that requires the windower's row FSM, which was deliberately out of scope.

## Experiment B -- per-block binding service, deployed transform

16-48-64, L=8, Q=32, 720p input, three stride-2 DW-PW blocks.

| blk | H x W in | c_in | c_out | T_read | T_DW | T_PW | T_write | binds | T_block | ms |
|---|---|---|---|---|---|---|---|---|---|---|
| 1 | 720x1280 | 3 | 16 | 345600 | 351127 | 518400 | 460800 | **PW** | 518400 | 5.1840 |
| 2 | 360x640 | 16 | 48 | 460800 | 469300 | 410400 | 345600 | **DW** | 469300 | 4.6930 |
| 3 | 180x320 | 48 | 64 | 345600 | 356932 | 223200 | 115200 | **DW** | 356932 | 3.5693 |
| | | | | | | | | **total** | **1344632** | **13.4463** |

Cross-check against the paper's 1,344,632 cycles / 13.4463 ms: **exact** (+0 cycles).

### Do the DMA terms ever bind?

**No.** Neither `T_read` nor `T_write` binds in any of the three deployed blocks. Margins to the binding service:

| blk | binds | T_block | T_read | headroom | T_write | headroom |
|---|---|---|---|---|---|---|
| 1 | PW | 518400 | 345600 | 33.33% | 460800 | 11.11% |
| 2 | DW | 469300 | 460800 | 1.81% | 345600 | 26.36% |
| 3 | DW | 356932 | 345600 | 3.17% | 115200 | 67.72% |

The tightest margin is block 2, where `T_read` is only **1.81%** below the binding service. It never binds, but it is not far off, so the statement to make in the paper is that the DMA terms do not bind *on this schedule* rather than that they cannot.

### `T_DW` against DW alone and against the fused block

| blk | H x W in | binds | model `T_DW` | DW alone (RTL) | fused block (board) | model vs DW alone |
|---|---|---|---|---|---|---|
| 1 | 720x1280 | PW | 351127 | 350659 | 519919 | +0.13% |
| 2 | 360x640 | DW | 469300 | 468019 | 470288 | +0.27% |
| 3 | 180x320 | DW | 356932 | 354979 | 357989 | +0.55% |

> **Superseded by Item 1 below.** The cancellation described in this paragraph is an artifact of the core boundary; at the AXIS boundary the two terms have the same sign and add. Kept for the record.

On the two DW-bound blocks the model **over**-predicts the depthwise engine by +0.27% and +0.55%, while **under**-predicting the fused block by -0.21% and -0.30%. The signs are opposite, so the extra row the model charges partly stands in for the DW->PW fusion overhead it does not model. The block-level agreement is therefore better than the depthwise model deserves on its own, and should not be presented as evidence that `T_DW` is correct.

### Predicted vs measured, per block

Measured figures are the FUSED DW+PW pair per block. Within a block DW streams into PW with no DDR round trip, so the pair is the block and is the correct target for `max(T_read, T_DW, T_PW, T_write)`.

| blk | binding service | predicted | measured | error | pct |
|---|---|---|---|---|---|
| 1 | PW | 518400 | 519919 | -1519 | -0.292% |
| 2 | DW | 469300 | 470288 | -988 | -0.210% |
| 3 | DW | 356932 | 357989 | -1057 | -0.295% |
| **sum** | | **1344632** | **1348196** | **-3564** | **-0.264%** |

13.4463 ms predicted against 13.48196 ms measured.

**Per-block errors: -0.292%, -0.210%, -0.295%.** Range -0.295% to -0.210%, aggregate -0.264%. All the same sign and of similar magnitude, so the aggregate figure is representative rather than the result of over- and under-prediction cancelling.

## Constant sensitivity (+/-1)

Change in total predicted latency for the deployed transform.

| constant | value | -1 cycles | -1 pct | +1 cycles | +1 pct |
|---|---|---|---|---|---|
| `R_SH` | 2 | +201600 | +14.993% | +0 | +0.000% |
| `DELTA_ACC` | 6 | +0 | +0.000% | +0 | +0.000% |
| `DELTA_PPU` | 2 | -28800 | -2.142% | +28800 | +2.142% |
| `DELTA_TR` | 1 | +0 | +0.000% | +0 | +0.000% |
| `DELTA_FLUSH` | 1 | -14464 | -1.076% | +14464 | +1.076% |
| `DELTA_ROW` | 4 | -542 | -0.040% | +542 | +0.040% |

A constant with zero sensitivity does not appear in ANY binding service on this schedule, so this transform cannot validate it at all -- neither can the board. Say that rather than implying it was checked.

## Item 1 -- fusion overhead, separated from model error

`a` is the model. `b` is the DW engine **alone**, measured in simulation at the **AXIS** boundary on the identical geometry -- the boundary blocks are actually composed over. `c` is the four-way max. `d` is the fused block on silicon.

| | blk 1 | blk 2 | blk 3 |
|---|---|---|---|
| geometry (H x W in) | 720x1280 | 360x640 | 180x320 |
| c_in / c_out | 3 / 16 | 16 / 48 | 48 / 64 |
| **a** `T_DW` predicted | 351127 | 469300 | 356932 |
| **b** DW alone, AXIS, measured | 351160 | 469333 | 356965 |
| **c** `T_block` predicted | 518400 | 469300 | 356932 |
| &nbsp;&nbsp;binds | **PW** | **DW** | **DW** |
| **d** fused block, silicon | 519919 | 470288 | 357989 |

### e. implied fusion overhead

**Blocks 2 and 3 (DW binds).** `d - b` is a difference of two measurements, so it is what the fused pair costs over the depthwise engine running alone:

| blk | d | b | overhead | as % of block |
|---|---|---|---|---|
| 2 | 470288 | 469333 | **+955** | 0.203% |
| 3 | 357989 | 356965 | **+1024** | 0.286% |

**Block 1 (PW binds).** `b` is not the binding term, so `d - b` is not a fusion overhead and is not reported as one. The comparable residual is `d - T_PW` = +1519, but `T_PW` is a **model**, not a measurement, so that number mixes PW model error with fusion cost and cannot separate them. **Isolating fusion overhead on block 1 needs a PW-alone RTL measurement, which this study does not have.**

### The block-level error decomposes exactly

| blk | `T_block - d` | `T_DW - b` | `b - d` |
|---|---|---|---|
| 2 | -988 | -33 | -955 |
| 3 | -1057 | -33 | -1024 |

**This corrects a statement made earlier in this report.** The earlier text said the model's extra row and the unmodelled fusion overhead have *opposite* signs and partly cancel, so the block agreement flattered the model. That was an artifact of comparing against the **core** boundary. At the AXIS boundary -- the correct one, since blocks compose over AXIS -- both terms are **negative and simply add**: the model sits a constant -33 cycles below the depthwise engine, and fusion adds ~1,000 more. There is no cancellation, and the block-level agreement is not luck.

### Constant, or does it scale?

| quantity | blk 2 | blk 3 | ratio |
|---|---|---|---|
| **overhead (cyc)** | 955 | 1024 | **0.933** |
| output pixels | 57600 | 14400 | 4.00 |
| c_in | 16 | 48 | 0.33 |
| c_out | 48 | 64 | 0.75 |
| G | 80 | 40 | 2.00 |

**Approximately constant.** Output volume changes by 4x between the two blocks while the overhead changes by 7%, in the *opposite* direction. A per-beat cost is therefore excluded: fitting `overhead = F + k*P_out` gives `k = -0.001597` cycles per output pixel, **negative and unphysical**. `c_in` moves 3x the other way, `c_out` 1.33x and `G` 2x, and none of them tracks it either.

**What two points cannot settle.** A fixed handshake cost and a *weak* per-beat cost cannot be separated -- two usable measurements, two free parameters. A *strong* per-beat cost is ruled out by the ratio; anything smaller is not resolvable, and no trend is fitted to two points. Block 1 supplies no third point because PW binds there.

### One sentence, or a real gap?

**One sentence, with a stated bound.** The model omits a per-block DW->PW fusion overhead of about 955-1024 cycles, 0.20-0.29% of a block, which on the available evidence does not scale with output volume, channel count or group count. It is a real omission -- once the constant pipeline offset is removed it is the *whole* of the block-level residual -- but it is small, one-directional and bounded. What would make it a real gap is a schedule where it stops being roughly constant, and two points cannot say where that is.

## Item 2 -- the stride-2 row mechanism, confirmed against the RTL

Scope restriction lifted after `model/service_model.py` was committed unchanged, so the model's independence is banked and this read cannot retroactively affect it.

**Confirmed. The RTL states it in its own header comment.**

### The `(H+1)` row count

`dw_banked_window_8x.sv`, emission order:

```
  for row r in 0..H-1: for group g in 0..G-1: for channel c in 0..C-1:
    one 8-sample beat
  (plus one extra vertical-flush row, r==H, all zp_in)
```

and the control logic that implements it:

```systemverilog
real_row         = running && (r_cnt < H_r);   // input needed only r < H
real_group       = (g_cnt < G_r);              // false only in the flush slot
last_beat_of_row = last_beat_of_group && (g_cnt == G_r);
...
pending_finish  <= (r_cnt == H_r);             // terminate AFTER the r==H pass
if (r_cnt != H_r) r_cnt <= r_cnt + 12'd1;
```

`r_cnt` takes the values `0 .. H_r` inclusive -- **H+1 row passes** -- and the last is not an input row (`real_row` false), which is why it costs time without consuming beats. Two more model constants fall out of the same block:

| constant | value | RTL |
|---|---|---|
| `delta_flush` | 1 | `real_group = (g_cnt < G_r)` -- `g_cnt` runs `0..G`, the extra slot being the per-row horizontal flush |
| `delta_row` | 4 | `localparam int DRAIN_CYCLES = 4` -- the per-row pend_ram write-back drain |

### Why the core window closes one row early at stride 2

From the same header:

> Rows: only even Y survive (Y = s2_row - 1). **The r==H vertical-flush row emits Y=H-1, odd, so for even H it is discarded** -- harmless, and left in place rather than special-cased so the FSM is untouched.

That is the mechanism verbatim. The `r==H` pass still **runs** -- the FSM is identical at both strides, which is why the AXIS window, which spans the input stream and the engine's full execution, shows no stride dependence -- but at stride 2 it **emits nothing that survives**, so the last `valid_out` falls one row earlier and the core window closes one row short. Exactly the measured residual: one row, independent of `H`, on 71 of 74 configurations.

### Do the AXIS constants follow from the same mechanism?

Partly. The part that does not is stated rather than guessed at.

`dw_fused_core.sv` sets `TOT_LAT = 1 + MAC_LAT + 8 = 1 + 9 + 8 = 18` and delays `done_out` by that much. The measured core offset is **+19 = TOT_LAT + 1** -- datapath fill/drain plus the one cycle between accepting the first beat and the window's first counted cycle. The core constant is fully accounted for.

The AXIS constants are that plus the shell:

```
  stride 1:  +36  =  19 (core)  +  17 (AXIS FIFOs + output holding reg)
  stride 2:  +33  =  19 (core)  +  14
```

The 17-versus-14 difference is a **3-cycle constant** on the output side, uniform across every `c_in` from 3 to 64, so it is structural and not data-dependent. `m_axis` is driven from an output holding register rather than straight off the FIFO (for TLAST), and that path drains differently when the core emits at a quarter rate. **I have not localised those 3 cycles to a specific stage.** The honest statement: the AXIS offset is a constant per stride, its dominant term is the datapath latency the RTL declares, and a 3-cycle stride-dependent tail in the output path remains unexplained.

### A precondition the sweep tripped over -- and it is not a timing bug

The same header, for stride 2:

> **PRECONDITIONS for stride2 (checked by the driver, NOT by hardware):** `n_groups` must be EVEN, i.e. `img_width` a multiple of 16. Otherwise the final group of every row is an even-index group that never gets a partner, and **its 4 output pixels are silently dropped**.

That is exactly the `W=67` (`G=9`) and `W=100` (`G=13`) residual: `-c_in` cycles, one dropped beat per channel per row. So those two configurations are **functionally invalid, not merely three cycles off** -- the RTL drops output there and says the driver must prevent it. They are kept in `dw_sweep.csv` with this note rather than deleted: the timing is real and the reason they differ is now understood.

It also sharpens the ragged-width answer. `G = ceil(W/L)` is required and correct. Beyond that, stride 2 needs `G` **even**, which is stronger than `W` dividing `L`: `W=1279` and `W=1435` are ragged, have even `G`, and land exactly. Every deployed width (1280/640/320) is a multiple of 16, so all three deployed blocks satisfy the precondition.

## LaTeX tables

### Table 1 -- DW sweep

```latex
\begin{tabular}{rrrrrrr}
\toprule
$H$ & $W$ & $c_{\mathrm{in}}$ & $G$ & predicted & measured & err (\%) \\
\midrule
90 & 160 & 3 & 20 & 6097 & 6049 & -0.787 \\
180 & 160 & 3 & 20 & 12127 & 12079 & -0.396 \\
360 & 160 & 3 & 20 & 24187 & 24139 & -0.198 \\
720 & 160 & 3 & 20 & 48307 & 48259 & -0.099 \\
90 & 320 & 3 & 40 & 11557 & 11449 & -0.934 \\
180 & 320 & 3 & 40 & 22987 & 22879 & -0.470 \\
360 & 320 & 3 & 40 & 45847 & 45739 & -0.236 \\
720 & 320 & 3 & 40 & 91567 & 91459 & -0.118 \\
90 & 640 & 3 & 80 & 22477 & 22249 & -1.014 \\
180 & 640 & 3 & 80 & 44707 & 44479 & -0.510 \\
360 & 640 & 3 & 80 & 89167 & 88939 & -0.256 \\
720 & 640 & 3 & 80 & 178087 & 177859 & -0.128 \\
90 & 1280 & 3 & 160 & 44317 & 43849 & -1.056 \\
180 & 1280 & 3 & 160 & 88147 & 87679 & -0.531 \\
\bottomrule
\end{tabular}
```

### Table 2 -- per-block binding service

```latex
\begin{tabular}{rrrrrrlrr}
\toprule
blk & $T_{\mathrm{read}}$ & $T_{\mathrm{DW}}$ & $T_{\mathrm{PW}}$ & $T_{\mathrm{write}}$ & binds & $T_{\mathrm{block}}$ & meas. & err (\%) \\
\midrule
1 & 345600 & 351127 & 518400 & 460800 & PW & 518400 & 519919 & -0.292 \\
2 & 460800 & 469300 & 410400 & 345600 & DW & 469300 & 470288 & -0.210 \\
3 & 345600 & 356932 & 223200 & 115200 & DW & 356932 & 357989 & -0.295 \\
\midrule
total & & & & & & 1344632 & 1348196 & -0.264 \\
\bottomrule
\end{tabular}
```

