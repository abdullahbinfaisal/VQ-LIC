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

