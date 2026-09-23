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


---

# VQ comparison audit -- dedicated PQ engine vs shared pointwise engine

**Question asked:** is the comparison between the standalone/dedicated VQ engine
and the shared-pointwise VQ engine technically fair and correctly interpreted?

**Verdict:** the comparison as it stands is *not* fair, but the standing
criticism (128 vs 256 products/cycle, "not resource-normalised") identifies the
wrong defect. That objection is arithmetically correct and analytically
irrelevant. Three other asymmetries are each individually larger than the
1.152 ms difference the table reports.

Nothing in this section is fitted. Every figure is either read from a shipped
artefact, produced by the frozen `model/service_model.py`, or measured on the
board; the provenance is stated per number.

## 1. What the dedicated engine actually is

From `vq_accel.xsa` -> `hw.hwh` -- i.e. from the shipped bitstream, not from a
recollection of the design intent:

```
vq_pq_axi_0 : M=4, K=256, DSUB=16, QUERY_LANES=2, LANES=8,
              NCH=64, GROUP_BUFS=8, SQ_STYLE=1
```

Datapath width is `QUERY_LANES(2) x M(4) x DSUB(16) = 128` multipliers. One
codeword index `k` is broadcast per cycle and swept `k = 0..255`, with two query
positions in flight. `SQ_STYLE=1` means `||c_k||^2` is precomputed and folded
into the score, so the per-cycle work is the inner product alone:

```
argmin_k ||z - c_k||^2  ==  argmin_k ( ||c_k||^2 - 2<z, c_k> )
```

Latency:

```
(NPOS / QUERY_LANES) x K = (14400 / 2) x 256 = 1,843,200 cyc = 18.4320 ms
```

Useful products: `14400 x 4 x 256 x 16 = 235,929,600`, over 1,843,200 cycles =
**128.0 products/cycle against 128 multipliers -> 100.0% MAC utilisation.**
No stalls, no bubbles. The schedule *is* its own roofline.

## 2. What the shared PW engine is when it does VQ

VQ is mapped as a 1x1 convolution with block-diagonal weight packing:
`c_in = DSUB = 16`, `c_out = M*K = 1024`, `Q = N_OC = 32`, `L = 8` lanes.

```
group_service(16, 1024, 32) = 1088 cyc/group
1088 x 1800 groups          = 1,958,400 cyc = 19.5840 ms
```

Useful products are identical (235,929,600), so the shared engine achieves
`235,929,600 / 1,958,400 = 120.5` products/cycle against `L(8) x Q(32) = 256`
INT8 lanes -> **47.1% MAC utilisation**.

## 3. Is the standing criticism right?

**The arithmetic is right. The inference drawn from it is wrong.**

The criticism: the shared engine has 256 products/cycle of capacity against the
dedicated engine's 128, so a shared engine losing by 6.2% is really losing by
more than 2x per multiplier, and the table hides that. Every one of those
numbers is correct.

But the argument presumes the shared engine is MAC-bound at this configuration.
It is not. Decompose `group_service(16, 1024, 32)`:

| term | value |
|---|---|
| `B = ceil(c_out/Q) = ceil(1024/32)` | 32 output batches |
| output-path term `c_out + d_ppu*B = 1024 + 2*32` | **1088** |
| accumulate/MAC term `c_in + d_acc + (B-2)*D + max(D + d_tr, Gamma)` | 1080 |
| `group_service = max(1088, 1080)` | **1088** |

The binding term is the **output path**, not the MAC array. 1024 codeword
scores must leave the PPU one per cycle, and the PPU is one-wide. MAC-busy time
is only `B x c_in = 32 x 16 = 512` of 1088 cycles -- the array is idle 53% of
the time *by construction*, waiting on drain.

The practical consequence: **doubling the multipliers would not move 19.584 ms
at all**, and halving them would not move it either until the MAC term crossed
1088. So "the shared engine has twice the arithmetic and still loses" is a true
sentence that describes nothing causal. A per-multiplier normalisation would
produce a number (2.12x worse per MAC) that no design change could act on.

The correct interpretation is stronger and falls straight out of the frozen
service model: **PQ at large K is an output-bandwidth problem, and a shared
convolution datapath narrows exactly the wrong resource.**

## 4. The three asymmetries that do matter

None of them is about multiplier count; each is larger than the 1.152 ms the
table reports.

### (a) Provenance -- measured against modelled

| | value | how obtained |
|---|---|---|
| dedicated | 18.4320 ms | analytical |
| dedicated | 18.433 ms | RTL simulation, 1,843,339 cyc |
| dedicated | **18.435 ms** | **silicon**, block's own cycle counter (`vq_pl.h`) |
| shared | 19.5840 ms | **modelled only** |

The shared engine has never executed (M=4, K=256, D=16) -- not on silicon, not
in simulation. The table currently places a measured number and a modelled
number in adjacent cells with no distinction drawn. This is the single most
reviewer-exposed line in the comparison.

### (b) The modelled configuration is not executable in one pass

`c_out = M*K = 1024` against `PW_COUT_HW_MAX = 224` (7 batches of Q=32). The
shared engine physically cannot hold 1024 output channels, so it needs
**5 passes**. The measured per-pass driver bracket is 0.3714 ms:

```
5 passes x 0.3714 ms = +1.857 ms
```

None of which is in the model. The paper reports a single-pass model figure for
a configuration the hardware cannot run single-pass.

### (c) Codebook residency -- once per model vs once per frame

The dedicated block owns its codebook RAM. `vq_pl_load_codebook()` is
documented "once per model, not timed", and the analysis convolution never
touches that memory.

The shared engine stores the codebook in the shared weight BRAM, which the
analysis convolution **overwrites every frame**. It must therefore be reloaded
every frame:

```
16,384 weight entries + 1,024 norms + 6 = 17,414 writes
17,414 x 0.2208 us = 3.8451 ms per frame
```

That single term is 3.3x the difference the table reports.

### Corrected per-frame comparison at (M=4, K=256, D=16)

| | ms | provenance |
|---|---|---|
| dedicated | **18.435** | measured on silicon |
| shared: search | 19.584 | modelled |
| shared: 5-pass driver overhead | +1.857 | measured bracket |
| shared: per-frame codebook reload | +3.845 | measured write rate |
| **shared total** | **25.286** | modelled + measured |

**+37.2%, not +6.2%.** The honest number is six times the reported one, and it
favours the paper's argument -- which is precisely why it is worth fixing
rather than defending the current table.

## 5. Dedicated VQ resource cost, and coexistence on the device

Both builds post-route, xc7z020clg484-1, Vivado 2020.2. `vq_impl_util3.rpt`
(Aug 30 00:38, the build matching `vq_accel.xsa`) against `Zynq.runs/impl_1`
(Sep 4):

| | dedicated build | shared build | delta |
|---|---|---|---|
| Slice LUTs | 40,073 (75.3%) | 34,358 (64.6%) | -5,715 |
| Slice Registers | 34,329 (32.3%) | 32,354 (30.4%) | -1,975 |
| Block RAM Tile | 91.5 (65.4%) | 82.5 (58.9%) | -9.0 |
| DSP48E1 | 220 (100%) | 220 (100%) | **0** |

Read the DSP row carefully: **the dedicated VQ engine consumes zero DSP48E1s.**
Its 128 multipliers are LUT-based -- INT8 x INT8 maps to LUT fabric cheaply.
Both builds sit at 220/220 because the convolution engine saturates the DSP
column either way.

**Precision caveat, to be respected in the paper.** The *absolute* cost of
`vq_pq_axi_0` is NOT ESTABLISHED: no hierarchical utilisation report survives
for the Aug 30 build. What is established is the net two-build delta above.
State it as a delta, never as "the VQ engine costs 5,715 LUTs" -- the two builds
differ in more than the VQ block.

## 6. Is the 220/220-DSP feasibility argument the stronger one?

**No. As stated it is false, and it is the most dangerous claim in the section.**

The proposed argument -- that the dedicated VQ engine could not coexist with
the convolution engine because DSPs are exhausted at 220/220 -- is refuted by
the repository itself. The dedicated build routed, met timing, and shipped an
XSA at 220/220 DSP *with the VQ engine present*. VQ was feasible without
sharing. A reviewer who asks for the utilisation reports finds this in a minute.

The defensible version, nearly as strong:

> Sharing returns 5,715 LUTs, 1,975 registers and 9 BRAM tiles at zero DSP
> cost, moving LUT occupancy from 75.3% to 64.6%. On a device already at
> 220/220 DSP48E1, that LUT headroom is what admits the entropy-coding and
> packing logic; the dedicated engine fits, but leaves the design no room to
> grow.

True, checkable, and it makes the same point about the design without the false
impossibility claim.

## 7. Recommended Table III structure

Add a provenance column and split the shared engine's cost into its actual
components. Drop the framing that these are two equal-throughput
implementations of one function -- they are not, and pretending otherwise is
what creates the unfairness.

| VQ mapping (M=4, K=256, D=16) | ms/frame | source |
|---|---|---|
| Dedicated PQ engine, 128 LUT multipliers | 18.435 | measured, on-chip counter |
| Shared PW engine -- search only | 19.584 | analytical model |
| Shared PW engine -- + 5-pass driver overhead | 21.441 | model + measured bracket |
| Shared PW engine -- + per-frame codebook reload | 25.286 | model + measured write rate |
| **Resource returned by sharing** | -5,715 LUT, -1,975 FF, -9 BRAM, +/-0 DSP | post-route, Vivado 2020.2 |

### LaTeX form

```latex
\begin{tabular}{lrl}
\toprule
VQ mapping ($M{=}4$, $K{=}256$, $D{=}16$) & ms/frame & source \\
\midrule
Dedicated PQ engine, 128 LUT multipliers   & 18.435 & measured, on-chip counter \\
Shared PW engine -- search only            & 19.584 & analytical model \\
\quad + 5-pass driver overhead             & 21.441 & model + measured bracket \\
\quad + per-frame codebook reload          & 25.286 & model + measured write rate \\
\midrule
\multicolumn{3}{l}{Resource returned by sharing:
  $-5{,}715$ LUT, $-1{,}975$ FF, $-9$ BRAM, $\pm 0$ DSP (post-route)} \\
\bottomrule
\end{tabular}
```

## 8. Recommended prose

> At the legacy configuration ($M{=}4$, $K{=}256$, $D{=}16$), a dedicated
> product-quantisation engine is faster than the shared pointwise datapath by a
> substantial margin. The dedicated engine issues 128 INT8 products per cycle
> and sustains exactly that rate -- its schedule is bubble-free, giving
> 18.435 ms measured from the block's own cycle counter. The shared engine,
> mapping the same search as a $1{\times}1$ convolution with
> $c_{\mathrm{out}} = MK = 1024$, is limited not by arithmetic but by its output
> path: the per-group service cost is
> $\max(c_{\mathrm{out}} + \delta_{\mathrm{ppu}}B,\,\cdot) = 1088$ cycles, of
> which only 512 are MAC-active. Additional multipliers would not reduce this.
> The shared mapping additionally requires five output passes, since
> $c_{\mathrm{out}} = 1024$ exceeds the 224-channel output capacity, and a
> per-frame codebook reload, since the analysis convolution overwrites the
> shared weight memory. Accounting for both, the shared mapping costs 25.286 ms
> per frame against 18.435 ms measured.

> We nonetheless adopt the shared mapping, for two reasons. First, sharing
> returns 5,715 LUTs, 1,975 registers and 9 BRAM tiles -- reducing LUT
> occupancy from 75.3\% to 64.6\% on a device whose DSP column is already fully
> committed at 220/220 -- and that headroom is what accommodates the entropy
> coder and packing logic. Second, the deployed configuration is $M{=}8$,
> $K{=}16$, $D{=}8$, not $M{=}4$, $K{=}256$, $D{=}16$; at $K{=}16$ the
> output-path bottleneck that dominates the legacy configuration disappears,
> and the shared engine is no longer the limiting stage. The legacy comparison
> is therefore reported as a worst case for sharing, not as the operating point.

That second paragraph is what actually saves the design decision, and it is
currently absent from the paper.

## 9. Smallest change that makes the comparison defensible

Add the provenance column (measured vs modelled) and a footnote stating that
the shared-engine figure is search-only, excluding multi-pass and
codebook-reload overhead. Two edits, and the misrepresentation is gone.
Everything in sections 7 and 8 above is improvement; that much is the minimum.

## 10. Open items this audit surfaced

1. `PAPER_HW_EVIDENCE.md` (2,309 lines) contains **zero** occurrences of "VQ".
   The dedicated engine is entirely undocumented in the evidence file, so a
   reviewer request for its provenance currently has nothing to point at.
2. The 3.845 ms reload figure assumes the codebook travels the same weight-write
   path as convolution weights. If a faster bulk path exists that this audit did
   not find, the figure drops -- though not below roughly 1 ms.
3. The dedicated engine's absolute resource cost remains unestablished (no
   surviving hierarchical report for the Aug 30 build). Only the two-build delta
   is citable.

---

# Deployed VQ audit -- M=4, K=64, Dm=16 on the shared PW engine

Supersedes the M=8/K=16 assumptions in the preceding section for the *selected*
quantiser. The preceding section's dedicated-vs-shared analysis at M=4/K=256 is
unaffected and still stands.

Every fact below is from `pw_pixel_major_core.sv`, `pw_single_oc_axis_axi.sv`,
`Zynq.gen/sources_1/bd/hw/hw_handoff/hw.hwh` (the shipped bitstream's parameter
handoff), the drivers `vq_pw_pl.c` / `vq_pw.c` / `main.c`, or board measurement
in `results/board_measured.py`. Where a number is model-derived it is labelled.

## 0. Headline -- the binding constraint is not the one we were looking for

`PW_COUT_HW_MAX = 224` is **not** what stops M=4/K=64. The shipped VQ argmin and
index-packing datapath is hardwired to **K=16** and **c_out <= 128**. The engine
cannot execute this quantiser in one pass, in two passes, or in any number of
passes, without an RTL change.

## 1. What the shipped hardware actually is

`hw.hwh`, instance `hw_pw_single_oc_axis_axi_0_0`:

```
N_LANES=8  N_OC=32  CIN_MAX=240  COUT_MAX=240  ACC_WIDTH=24
TILE_PIXELS_MAX=32768  USE_PW_VQ=1
S_AXIS_DATA_WIDTH=64  M_AXIS_DATA_WIDTH=64
```

Derived in `pw_single_oc_axis_axi.sv:113-116`:

```systemverilog
localparam int W_OC_BATCHES = COUT_MAX / N_OC;   // 240/32 = 7  (integer division)
localparam int W_DEPTH      = W_OC_BATCHES * CIN_MAX;  // 7*240 = 1680 per bank
localparam int W_AW         = $clog2(W_DEPTH);         // 11
localparam int PARAM_AW     = $clog2(COUT_MAX);        // 8
```

so `c_out <= W_OC_BATCHES * N_OC = 224` in **convolution** mode.

### The VQ branch is far more constrained than that

`pw_pixel_major_core.sv:635-735`, inside `generate if (USE_PW_VQ != 0)`:

```systemverilog
localparam int VQ_K       = 16;
localparam int VQ_KW      = 4;
localparam int VQ_SCORE_W = 20;
localparam int VQ_NORM_D  = 128;

wire [VQ_KW-1:0] vq_k     = vq_oc_r[VQ_KW-1:0];   // codeword index = issue_idx[3:0]
wire             vq_half  = vq_oc_r[VQ_KW];
wire             vq_first = (vq_k == 0);          // running-best RESET
wire             vq_lastk = (vq_k == 4'hF);       // commit
wire [2:0]       vq_m     = {vq_batch_r[1:0], vq_half};
wire [6:0]       vq_norm_ra = {vq_batch_r[1:0], vq_oc_r[4:0]};

if (vq_lastk) vq_word[i][{vq_m, 2'b00} +: VQ_KW] <= vq_curk[i];
```

and the AXI-lite norm port, `pw_single_oc_axis_axi.sv:450`:

```systemverilog
vq_norm_addr <= s_axi_wdata[6:0];    // SEVEN bits -> 128 norms, hardwired
```

Five independent hard limits, each of which M=4/K=64 violates:

| # | RTL fact | limit imposed | M=4,K=64 needs |
|---|---|---|---|
| 1 | `vq_k = vq_oc_r[3:0]`, `vq_first = (vq_k==0)` | argmin window is exactly 16 codewords; the running best **resets every 16 issues** | one argmin over 64 |
| 2 | `vq_word[i][{vq_m,2'b00} +: 4]` | 4-bit nibble fields, 8 per 32-bit word | 6-bit fields |
| 3 | `vq_norm_ra` = 7 bits, `VQ_NORM_D=128`, AXI `wdata[6:0]` | 128 codeword norms | 256 |
| 4 | `vq_m = {vq_batch_r[1:0], vq_half}` | batch index 2 bits -> batches 0..3 -> **c_out <= 128 in VQ mode** | 8 batches, c_out=256 |
| 5 | `VQ_SCORE_W = 20` | score fits +/-524,287; tight max at Dsub=8 is +391,168 | Dsub=16 doubles it to **+782,336** -> 21 bits |

Limit 5 is worth spelling out because it is silent. `vq_pw.c:9-19` derives the
20-bit width for Dsub=8 from the per-dimension maximum of
`||v||^2 - 2u.v = -2uv + v^2`, which is 48,896 (u=+127, v=-128). At Dsub=8 that
is `8 x 48,896 = 391,168`, and 20-bit signed holds it. At **Dsub=16** it is
`16 x 48,896 = 782,336`, which needs 21-bit signed. The accumulator itself is
fine (`ACC_WIDTH=24` holds +/-262,144), but the score register truncates.

**There is no runtime guard.** `pw_pixel_major_core.sv:962` checks only
`tile_pixels==0 || cin_run==0 || cout_run==0`. The `$error` on
`cout_run > COUT_MAX` exists **only** in `pw_pixel_major_core_SIMCOPY.sv:779`,
which is not the synthesised file. Programming `COUT_RUN=256` on the shipped
bitstream aliases silently -- norms wrap mod 128, batches 4..7 alias onto 0..3,
and the frame comes out wrong with no status bit set. Same failure class as
`MAX_CG_PRODUCT`.

## 2. Two physical passes, or another mechanism?

**Neither, on the shipped engine.** Splitting c_out=256 into 224+32 or 128+128
does not help, because limits 1, 2, 3 and 5 are per-codeword and per-symbol, not
per-pass. Concretely, the argmin is reset by `vq_first = (vq_oc_r[3:0]==0)`, so
a two-pass split still produces four separate argmins over 16 codewords each
instead of one argmin over 64, and the packer still writes 4-bit nibbles. A
software merge of partial minima is impossible: the engine emits only the
winning **index**, never the score.

**On a minimally modified engine**, 128+128 is the right split, not 224+32:

- 128+128 aligns to the batch boundary (4 + 4 batches of N_OC=32) and to the
  sub-codebook boundary (2 + 2 sub-codebooks of 64 codewords).
- 224+32 splits *inside* codeword 224's sub-codebook -- sub-codebook 3 would
  straddle the pass boundary, so its running minimum would have to survive a
  full engine restart. It cannot; `w_addr_base` and the PPU state reset in
  `S_IDLE` (line 950).

## 3. Exact search cycles per pass, from the real scheduler

`vq_model.py` shows `K > Q` forces `subs_per_batch = 1`, so
`cin_mac = Dsub = 16`, `c_out = M*K = 256`, `B = ceil(256/32) = 8`.

The frozen `pw_cyc_per_group(cin, cout, Q)` from `svc_model.py`, evaluated with
no modification:

```
one logical pass   gs(16, 256, 32) = 272 cyc/group
two passes 128+128 gs(16, 128, 32) = 136 cyc/group each
```

with `B=8, Q_last=32, d = max(cin+d_acc, Q+d_ppu) = max(22,34) = 34`:

```
gs(16,256,32) = max( 256 + 2*8,  16+6 + 6*34 + max(34+1, 16+16+6) )
              = max( 272,        22 + 204 + 38 = 264 )  = 272   <- DRAIN-bound
gs(16,128,32) = max( 128 + 2*4,  16+6 + 2*34 + max(35, 38) )
              = max( 136,        22 + 68 + 38 = 128 )   = 136   <- DRAIN-bound
```

**The split is exactly free in group service: `2 x 136 = 272`.** That is not a
coincidence. Both expressions are bound by the output-drain term
`c_out + delta_ppu*B`, which is additive in both `c_out` and `B`, so cutting the
run in half and paying the per-batch drain overhead twice costs precisely what
paying it once over eight batches costs. The MAC term (264 vs 2x128=256) is
slack in both cases and never binds.

For contrast, the 224+32 split is **not** free: `gs(16,224,32) + gs(16,32,32)
= 238 + 38 = 276 cyc/group`, +1.5%, because the 32-channel remainder pass pays a
full fill/drain for one batch.

Restart/fill/drain effects **outside** the steady-state group loop:

- Per-pass input DMA is `921,600 B / 8 B/cyc = 115,200 cyc = 1.152 ms`, well
  under the 244,800-cycle compute, so `T_read` never binds. Two passes re-stream
  the whole latent -- 1,843,200 B from DDR instead of 921,600 -- which costs DDR
  bandwidth and power but **no latency**.
- `S_LOAD_FIRST` fill is one group of `vq_cin_load = 64` beats, and the final
  drain is `ceil(Q_last/R_sh) + delta_acc = 16+6 = 22` cycles. Both are under
  100 cycles against 244,800 -- below the measurement floor and below the
  model's own 0.10% agreement with silicon.

Model credibility for exactly this shape: at the shipped M=8/K=16,
`gs(16,128,32) x 1800 = 244,800 cyc = 2.4480 ms` against **2.4505 ms measured**
on the accelerator (`T_VQ_ACC`), an error of **-0.10%**. The 136 cyc/group figure
used above is therefore silicon-validated, not merely asserted.

## 4. Total search latency, 90x160 latent at 100 MHz

```
groups = 14,400 positions / 8 lanes = 1,800
```

| mapping | cyc/group | total cycles | ms |
|---|---|---|---|
| one pass, c_out=256 (needs COUT_MAX>=256) | 272 | 489,600 | **4.8960** |
| two passes, 128+128 | 136 + 136 | 489,600 | **4.8960** |
| two passes, 224+32 | 238 + 38 | 496,800 | 4.9680 |

**4.8960 ms** either way, for the 128+128 split.

## 5. Host/driver overhead for the required number of passes

Measured, `board_measured.py`, run 3, 80 timed frames:

```
T_VQ_RUN_P = 2.8219 ms   driver bracket, pipelined pass
T_VQ_ACC   = 2.4505 ms   accelerator only, start -> DMA idle
-> per-invocation driver overhead = 0.3714 ms
```

That 0.3714 ms is cache maintenance (`Xil_DCacheInvalidateRange` over 57,600 B),
DMA arm/disarm, and the poll loop's last iteration. It is per **invocation**, so
two passes cost **0.7428 ms** and one pass costs **0.3714 ms**.

Plus per-pass register config: `TILE_PIXELS, CIN_RUN, COUT_RUN, ZP_RELU,
VQ_CTRL, CTRL`, and the `VQ_CTRL` clear in `vq_pw_pl_finish` -- 7 writes per
pass, 0.0015 ms each.

## 6. Must the codebook be reloaded every frame?

**Yes. Confirmed by direct evidence, not inferred.**

The analysis cascade programs PW convolution weights through the *same* register
window the codebook uses. `main.c:3488-3490`:

```c
pw_write_reg(PW_REG_OC_SEL,      oc % PW_N_OC);
pw_write_reg(PW_REG_W_BRAM_OFF, (oc / PW_N_OC) * Cin_pw);
for (int ic = 0; ic < Cin_pw; ic++) pw_write_weight(ic, w_ic[ic]);
```

and `vq_pw_pl.c:105-113` writes the codebook to `PW_REG_OC_SEL` /
`PW_REG_W_BRAM_OFF` / `PW_REG_W_BASE` -- byte for byte the same 32 weight banks.
There is one weight memory. The three analysis blocks overwrite it every frame
(`T_PROG = 1.0936 ms` measured), so the codebook must be rewritten every frame.

This is confirmed empirically: `vq_pw_pl_load_codebook()` is called per frame and
`T_VQ_PROG = 0.4834 ms` is a *per-frame* accumulator in `#STSUM`, not a one-time
cost. Contrast the dedicated block, whose `vq_pl_load_codebook()` is documented
in `vq_pl.h` as "once per model, not timed" -- it owns private codeword RAM.

## 7. Exact reload volume and time for M=4, K=64, Dm=16

### Calibration of the AXI-lite write rate

From the measured M=8/K=16 reload, whose write count is exactly known from
`vq_pw_pl.c`:

```
32 banks x (OC_SEL + W_BRAM_OFF + 64 weight writes) + 128 norms = 2,240 writes
T_VQ_PROG = 0.4834 ms  ->  0.21580 us per AXI-lite write
```

**Independent cross-check on a different code path.** The analysis cascade's own
programming is `sum(cin*cout) = 3*16 + 16*48 + 48*64 = 3,888` weight writes,
`2*sum(cout) = 256` bank selects, `3*sum(cout) = 384` bias/mult/shift = 4,528
writes. At 0.21580 us that predicts 0.9772 ms against a measured
`T_PROG = 1.0936 ms`, leaving 0.1164 ms for the DW descriptors and DMA setup
that `T_PROG` also covers. The rate is consistent across two unrelated paths.

### Volume

| item | count | derivation |
|---|---|---|
| codeword coefficients | **4,096** | `M*K*Dm = 4*64*16`; = 32 banks x 128 entries |
| codeword norms | **256** | `M*K` |
| bank config (`OC_SEL`, `W_BRAM_OFF`) | 64 | 32 banks x 2 |
| **reload subtotal** | **4,416** | |
| per-pass run config | 7 per pass | `TILE_PIXELS, CIN_RUN, COUT_RUN, ZP_RELU, VQ_CTRL, CTRL, clear` |

Per bank the weight image is `B * cin_mac = 8 * 16 = 128` entries, comfortably
inside `W_DEPTH = 1680`. Weight *storage* is not a constraint here; only the
`W_OC_BATCHES` batch count and the VQ-mode limits are.

### Time

```
4,416 x 0.21580 us = 0.9530 ms  codebook + norm reload, per frame
    7 x 0.21580 us = 0.0015 ms  per pass run config
```

That is **1.97x** the shipped M=8/K=16 reload (0.4834 ms), tracking the 2x growth
in `M*K*Dm` (4,096 vs 2,048 coefficients).

## 8. Complete deployed VQ latency, M=4 K=64 Dm=16

On a **minimally modified** engine (the shipped one cannot run this at all --
see section 1):

| component | ms | provenance |
|---|---|---|
| PL search service (2 x 2.4480) | 4.8960 | model, validated to -0.10% at M=8/K=16 |
| multi-pass driver overhead (2 x 0.3714) | 0.7428 | measured bracket |
| codebook + norm reload (4,416 writes) | 0.9530 | measured write rate |
| per-pass run config (2 x 7 writes) | 0.0030 | measured write rate |
| **TOTAL, two passes** | **6.5948** | |
| **TOTAL, one pass (COUT_MAX 240->256)** | **6.2219** | saves 0.3729 ms, 5.65% |

For reference, the shipped M=8/K=16 costs `0.4834 + 2.8219 = 3.3053 ms`
measured end to end. **M=4/K=64 is very nearly exactly 2x that**, and the rate
falls from 32 to 24 bits/position (0.500 -> 0.375 bpp at 1280x720). Whether that
trade is worth 3.29 ms/frame is an RD question this audit cannot answer -- there
are still no trained weights in the repository.

Not included, and correctly so: the second pass re-reads 921,600 B of latent from
DDR. It is fully covered by compute (115,200 read cycles against 244,800 compute
cycles per pass) so it adds no latency, but it does add DDR traffic and power
that the single-pass mapping would not.

## 9. Is 4.896 ms a valid physical PL search latency?

**It is an ideal service number, and on the shipped engine it is not
realizable at all.** Precisely:

- **As a service model figure it is correct**, and unusually well founded: the
  same `pw_cyc_per_group` predicts the shipped VQ shape to -0.10% against
  silicon, and 4.896 ms holds under the 128+128 split exactly (`2 x 136 = 272`),
  so it is not an artefact of assuming a single pass.
- **It is not a complete PL latency.** It excludes the 0.7428 ms of driver
  overhead the two passes require and the 0.9530 ms per-frame codebook reload.
  The honest PL-plus-host figure is **6.5948 ms**.
- **It is not achievable on the implemented engine at all.** `USE_PW_VQ=1` gives
  a K=16 argmin with 4-bit index packing and a 128-entry norm ROM. Quoting
  4.896 ms as a measured or achievable number for the current bitstream would be
  incorrect.

So: **valid as a model number for a modified engine; not a physical latency for
the implemented one.**

## 10. Can PW_COUT_HW_MAX be raised 224 -> 256 trivially?

### What sets 224

Not DSPs, not `Q`, not the MAC array. It is one integer division:

```systemverilog
localparam int W_OC_BATCHES = COUT_MAX / N_OC;         // 240/32 = 7, remainder 16 WASTED
localparam int W_DEPTH      = W_OC_BATCHES * CIN_MAX;  // 1680 bytes per weight bank
```

`COUT_MAX = 240` was chosen for `CIN_MAX = 240` symmetry and happens not to be a
multiple of `N_OC = 32`. The floor throws away 16 output channels. The physical
storage constraint is the per-bank weight BRAM depth and the param BRAM depth.

### Cost of COUT_MAX 240 -> 256

| | COUT_MAX=240 | COUT_MAX=256 | change |
|---|---|---|---|
| `W_OC_BATCHES` | 7 | 8 | +1 |
| `W_DEPTH` (bytes/bank) | 1,680 | 1,920 | +240 |
| `W_AW` = `$clog2(W_DEPTH)` | **11** | **11** | **none** |
| weight bits/bank | 13,440 | 15,360 | both fit one BRAM18 (2048x9 = 18,432 b) |
| `PARAM_AW` = `$clog2(COUT_MAX)` | **8** | **8** | **none** |
| bias/mult BRAM bits | 7,680 | 8,192 | both fit one BRAM18 |
| DSP48E1 | 220 | 220 | **none** |
| `N_OC` / `Q` | 32 | 32 | **none** |

**Yes -- trivially, for convolution mode.** No port width changes
(`w_rd_addr`, `param_rd_addr` are declared in terms of these `$clog2`
expressions and both stay put), no new BRAM primitive, no DSP, no change to `Q`
or the MAC array. It is a one-token edit to the IP parameter plus a re-run of
synthesis and P&R. The FSM already supports it: `cout_batches_r` and
`oc_batch_idx` are 12-bit, and `w_addr_base` is `W_AW`-wide.

### But it does not deliver M=4, K=64 on its own

Raising `COUT_MAX` fixes limit 4's *convolution*-mode ceiling. It does nothing
for the VQ-mode limits. To run M=4/K=64 the VQ branch also needs:

| change | scope | cost |
|---|---|---|
| `VQ_KW` 4 -> 6, `vq_k = {vq_batch_r[0], vq_oc_r[4:0]}` so a 64-codeword sweep spans two batches | ~6 lines | comparator unchanged; `vq_bestk` +2 b x 8 lanes = +16 FF |
| `vq_first`/`vq_lastk` retimed to the 64-codeword boundary | 2 lines | `vq_best` already persists across batches -- no new state |
| `vq_word[i][6*vq_m +: 6]` instead of nibble indexing | 1 line | 24 of 32 bits used; output word format unchanged |
| `VQ_NORM_D` 128 -> 256, `vq_norm_addr` 7 -> 8 b (core port **and** `s_axi_wdata[7:0]`) | 3 lines | distributed RAM ~40 -> ~80 LUTRAM |
| `VQ_SCORE_W` 20 -> 21 | 1 line | +1 b x 8 lanes on comparator and best register |

All five are small, none touches the MAC array, the DSP count, `Q`, or the
scheduling FSM -- so the 4.896 ms service figure survives them unchanged. But
they are a bitstream change, requiring re-synthesis, re-place-and-route, timing
closure at 100 MHz, and re-running `tb_pw_vq.sv` / `tb_pw_axi_vq.sv` plus the
`vq_pw_pl_verify()` bit-exactness check against `vqpw_encode_frame()`. Calling
it "trivial" would be right about the logic and wrong about the project cost.

### Is eliminating the second pass worth it?

Doing `COUT_MAX -> 256` *in addition to* the VQ changes buys one thing: a single
invocation instead of two, saving **0.3729 ms/frame (5.65%)** of the 6.5948 ms
total, plus 921,600 B/frame of DDR read traffic. Since the VQ changes are needed
regardless and `COUT_MAX` is a free parameter edit in the same rebuild, there is
no reason not to take it. It also removes the silent-aliasing hazard at
`cout_run` in (225..256), which is currently a real trap for anyone programming
a 256-channel run on this engine.

### One thing to fix in the same pass

Add the `cout_run > COUT_MAX` guard that exists only in
`pw_pixel_major_core_SIMCOPY.sv:779` to the synthesised core, as a sticky status
bit. Today an over-range `cout_run` produces a wrong frame with no indication.

---

# M=4, K=64, Dm=16 on the shared PW engine -- implementation

The audit above concluded that the shipped VQ datapath could not execute this
quantiser at any number of passes. This section records the change that makes
it execute, in ONE invocation, and the evidence that it is correct.

Everything is parameterised rather than retargeted: the same RTL source and the
same C source build both the deployed M=4/K=64/Dm=16 geometry and the legacy
M=8/K=16/Dm=8 one, and both are proved on every run.

## 1. The five limits, and what replaced each

| limit (shipped) | replacement | note |
|---|---|---|
| `VQ_K = 16` hardcoded | parameter `VQ_K` | 64 deployed, 16 legacy |
| `vq_k = vq_oc_r[3:0]`, argmin resets every 16 | `vq_abs = {batch, oc}`, `vq_k = vq_abs[VQ_KW-1:0]` | one expression covers both |
| `vq_word[i][{vq_m,2'b00} +: 4]` nibbles | `vq_word[i][VQ_KW*vq_m +: VQ_KW]` | 6-bit fields at K=64 |
| `VQ_NORM_D = 128`, 7-bit AXI address | parameter `VQ_NORM_D`, `$clog2(VQ_NORM_D)`-bit address | 256 norms, 8-bit field |
| `VQ_SCORE_W = 20` | parameter `VQ_SCORE_W` | 21 at Dm=16 -- see §3 |
| `vq_m = {vq_batch_r[1:0], vq_half}` capping VQ c_out at 128 | `vq_m = vq_abs[VQ_AW-1:VQ_KW]` | c_out to VQ_NORM_D |
| no synthesised geometry check | `cfg_err` + `STATUS2[5]` | see §5 |

### The indexing generalisation is one expression

Write `abs = batch*N_OC + oc` for the absolute output channel. Then for any
power-of-two K,

```
k = abs mod K = abs[VQ_KW-1:0]      the codeword inside its sub-codebook
m = abs div K = abs[VQ_AW-1:VQ_KW]  which sub-codebook
```

and the argmin resets exactly every K issues (`vq_first = (vq_k == 0)`) and
commits on the last of them. Specialising:

```
K=16, N_OC=32 : k = abs[3:0] = oc[3:0],          m = {batch[1:0], oc[4]}
K=64, N_OC=32 : k = abs[5:0] = {batch[0], oc[4:0]}, m = batch[2:1]
```

The K=16 case is character-for-character the old `vq_k` / `vq_m`, which is why
the legacy regression is bit-identical. The K=64 case spreads ONE sub-codebook
across TWO batches, and the running minimum survives that boundary because
nothing but `vq_first` clears it.

## 2. The one thing that is not a rename: the activation window

`w_addr_base` used to serve two purposes at once -- the weight BRAM read
address AND the pb_ram activation window base. At K <= N_OC that coincidence
holds, because every batch opens a new input window. At K > N_OC the two
DIVERGE:

- the WEIGHT address must advance every batch (each batch holds different
  codewords), and
- the ACTIVATION window must HOLD STILL for `VQ_BPS = K/N_OC` batches, because
  those codewords all belong to one sub-codebook and read the same Dm
  dimensions.

One counter cannot do both, so VQ mode gets `vq_pb_base`, stepped only on a
sub-codebook boundary:

```
K=16, N_OC=32, VQ_BPS=1 : 0, 16, 32, 48            (every batch -- as before)
K=64, N_OC=32, VQ_BPS=2 : 0, 0, 16, 16, 32, 32, 48, 48
```

At `VQ_BPS = 1` the step condition is constant-true and `vq_pb_base` tracks
`w_addr_base` exactly, so the legacy geometry is untouched. Cost: one
`PB_AW`-wide register and adder.

## 3. Score dynamic range -- the bound depends on Dm, not on K

Both `u = zq - 128` and `v = cq - 128` lie in `[-128, +127]`. Per dimension,
`v^2 - 2uv` is maximised at `u=+127, v=-128` (`16384 + 2*127*128 = 48,896`) and
minimised at `v = u = -128` (`16384 - 32768 = -16,384`). Over Dm dimensions,

```
score in [ -16384*Dm , +48896*Dm ]      and  2^(N-1) > 48896*Dm
```

| Dm | max score | needs | VQ_SCORE_W |
|---|---|---|---|
| 8 (legacy) | 391,168 | 2^19 = 524,288 | **20** |
| 16 (deployed) | 782,336 | 2^20 = 1,048,576 | **21** |

Dm = 16 overflows the shipped 20-bit width. This is proved, not asserted, three
ways:

1. `vq_pw_golden_test` attains 782,336 exactly on a directed corner and checks
   that truncating it to 20 bits changes the value.
2. `a_vq_score_fits` in the RTL checks `sc32 === 32'(vq_score[g])` on **every
   compare**. At `VQ_SCORE_W = 21`, zero violations across all six data runs.
3. A **negative control** -- the deployed geometry elaborated with
   `VQ_SCORE_W = 20` -- fires **1,361 overflow assertions** on the INT8-extremes
   vectors, with scores like 586,751 against a 524,287 ceiling.

The accumulator is unaffected: `|acc| <= 16384*Dm = 262,144` at Dm=16, inside
the shipped `ACC_WIDTH = 24`. So is the AXI norm field: `||v_k||^2` is at most
262,144, which fits 20-bit signed, so `ADDR_VQ_NORM`'s DATA field is unchanged
and only its ADDRESS field widened, 7 -> 8 bits. Addresses 0..127 decode
identically, so the register map is backward compatible.

## 4. COUT_MAX 240 -> 256

`W_OC_BATCHES = COUT_MAX / N_OC` floors: 240/32 = 7, so the shipped ceiling on
c_out was 224 and the top 16 entries of the param BRAM were unreachable. 256
makes the two agree.

| | 240 | 256 | change |
|---|---|---|---|
| `W_OC_BATCHES` | 7 | 8 | +1 |
| `W_DEPTH` bytes/bank | 1,680 | 1,920 | +240 |
| `W_AW = $clog2(W_DEPTH)` | **11** | **11** | none |
| weight bits/bank | 13,440 | 15,360 | one BRAM18 (2048x9 = 18,432 b) either way |
| `PARAM_AW = $clog2(COUT_MAX)` | **8** | **8** | none |
| bias/mult BRAM bits | 7,680 | 8,192 | one BRAM18 either way |
| `N_OC` / `Q` | 32 | 32 | none |

No port widens (`w_rd_addr` and `param_rd_addr` are declared from those two
`$clog2` expressions), no BRAM primitive grows, `Q` is untouched. Section 8
reports the measured DSP effect.

## 5. The synthesised configuration guard

The `cout_run > COUT_MAX` check existed ONLY in
`pw_pixel_major_core_SIMCOPY.sv`, which is not the synthesised file. An
over-range geometry ran, aliased, and produced a wrong frame with nothing set
to say so. `S_IDLE` now refuses to start and raises `cfg_err`, surfaced at
`STATUS2[5]`; `vq_pw_pl_start()` reads it after every start.

Refused: `cout_run > (COUT_MAX/N_OC)*N_OC`; `cin_run > CIN_MAX`; and in VQ mode
`cout_run > VQ_NORM_D`, `vq_cin_load > CIN_MAX`, `cout_run` not a multiple of
`VQ_K`, and window schedules that would read past the loaded channels. A legal
start clears the flag, so it always describes the most recent start.

The window-fit condition is the one that is easy to get wrong: the number of
windows is NOT `cout_run/K`, it is `batches / VQ_BPS` -- at K <= N_OC every
batch opens a window, at K > N_OC only every VQ_BPS-th does. Both deployed
geometries need `4 x 16 = 64` channels, which is exactly `vq_cin_load`.

## 6. The mapping at M=4, K=64, Dm=16, and why no structural zeros

`vqpw_map_oc` gives, for batch b and channel oc: `m = b/2`,
`k = 32*(b%2) + oc`, window base `16*(b/2)`, and offset within the window
**always 0**. Since `Dm = 16 = cin_mac`, every one of the 16 weights each
output channel reads is a real codeword coefficient.

Confirmed rather than assumed: `vq_pw_golden_test` compares every entry of the
4,096-byte weight image against the codebook directly and counts zeros. It
finds **17 zeros in 4,096** -- consistent with a random int8 codebook producing
a 0 about 1 byte in 256, and nothing structural. The legacy image, by contrast,
is **1,026 of 2,048** zeros: block-diagonal packing wastes half the MAC there.

So the deployed mapping runs the VQ search at **100% MAC utilisation** of the
window it walks, against 50% for the legacy one.

## 7. Verification

### Software reference, both profiles

`vq_pw_golden_test` re-derives every index from the HOST-LOADED IMAGES using
the engine's own arithmetic -- window base `vqpw_batch_window(b)`, 16-tap MAC,
uint8-minus-uint8 pre-adder, argmin resetting every K and persisting across
batch boundaries, score truncated to `VQPW_SCORE_BITS` -- and compares against
`vqpw_encode_frame()`.

```
DEPLOYED M=4 K=64 Dsub=16 : 58 checks, 0 failures
LEGACY   M=8 K=16 Dsub=8  : 44 checks, 0 failures
```

Including: 3 full 14,400-position frames each, 0 mismatches; every (m,k) pair
of 256 observed with highest index **63**; norm address **255** used; the
transport word's unused bits [31:24] zero at every position; put/get
round-tripping 12,279 indices above 15; and a **cross-batch tie** -- codewords
31 and 32 of the same sub-codebook made identical -- resolving to the lower
index every time (k=32 chosen 0 times, k=31 chosen 41 times). That last case is
the one K > N_OC creates and K = 16 cannot.

### RTL against the software reference

`tb_pw_vq` drives real latent groups through `pw_pixel_major_core` and compares
the packed index beats byte-for-byte. Vectors come from `gen_vq_vectors`, which
links the SAME `vq_pw.c` the firmware uses.

| scenario | DEPLOYED | LEGACY |
|---|---|---|
| 0 random | PASS, 0 mismatches, 272.00 cyc/grp | PASS, 0 mismatches, 136.00 cyc/grp |
| 1 tie storm | PASS, 0 mismatches, 272.00 cyc/grp | PASS, 0 mismatches, 136.00 cyc/grp |
| 2 INT8 extremes | PASS, 0 mismatches, 272.00 cyc/grp | PASS, 0 mismatches, 136.00 cyc/grp |

256 of 256 beats every run, `cfg_err = 0`, zero assertion errors.

### RTL through the real AXI-lite register map

`tb_pw_axi_vq` programs the engine the way the driver does -- `OC_SEL` /
`W_BRAM_OFF` / `W_BASE` for weights, `ADDR_VQ_NORM` for norms, `ADDR_VQ_CTRL`
for mode -- and reads the norm ROM back.

```
DEPLOYED, all 3 scenarios : PASS, 256/256 beats, 0 mismatches,
                            NORM ROM entries wrong: 0 / 256,
                            262,144 weight reads checked, 0 bad, cfg_err = 0
LEGACY,   all 3 scenarios : PASS, 256/256 beats, 0 mismatches,
                            NORM ROM entries wrong: 0 / 128, cfg_err = 0
```

This is the only place the widened norm address is exercised end to end: the
deployed run writes addresses 128..255, which the shipped 7-bit port could not
express at all.

### The driver emits the image the RTL was proved against

`vq_pw_pl_prog_test` replays the driver's AXI-lite writes into a model of the
weight banks and norm ROM. Both profiles: **0 checks failed**. Every weight
entry and every norm written exactly once and matching
`vqpw_build_weights()` / `vqpw_build_norms()`.

### The configuration guard

`tb_pw_vq_guard`: **18 cases, 0 failures**. Each refused case raises `cfg_err`
AND emits zero beats; each legal case clears `cfg_err` and emits. Covered:
zero geometry, `cout_run` 288 and 320, `cin_run` 256, `vq_cin_load` 256 and 48,
`cout_run` 224 (not a multiple of K=64), and convolution-mode cases proving the
VQ-only rules do not leak into conv while `COUT_HW_MAX` and `CIN_MAX` still
apply there.

### Entropy coder

`rc_geometry_test` at the new geometry: RC_NMODEL=4, RC_NSYM=64, **0 checks
failed**, round trips exact including the 6-bit fields that straddle byte
boundaries. Legacy profile: also 0 failed.

## 8. Cycle count and latency

The per-group service is measured as the gap between successive first-beats of
a group, in steady state with no input starvation and no output stall:

| geometry | model `group_service` | RTL measured | over |
|---|---|---|---|
| M=4 K=64, c_out 256, B=8 | 272 | **272.00, min 272, max 272** | 63 gaps |
| M=8 K=16, c_out 128, B=4 | 136 | **136.00, min 136, max 136** | 63 gaps |

**`GroupService(16,256,32) = 272` matches the RTL exactly.** The frozen
`svc_model.pw_cyc_per_group` needed no adjustment.

`2 x 136 = 272` exactly, which is why the two-pass split the audit costed was
free in service terms: both forms are bound by the output drain
`c_out + delta_ppu*B`, additive in both terms.

For the 90x160 latent, 14,400 positions / 8 lanes = 1,800 groups:

```
272 cyc/group x 1,800 = 489,600 cycles = 4.8960 ms at 100 MHz
```

Complete per-frame cost, now a SINGLE invocation because COUT_MAX is 256:

| component | ms | provenance |
|---|---|---|
| PL search | 4.8960 | RTL-confirmed 272 cyc/group |
| driver bracket, one invocation | 0.3714 | measured (`T_VQ_RUN_P - T_VQ_ACC`) |
| codebook + norm reload, 4,416 writes | 0.9530 | measured 0.21580 us/write |
| run config, 7 writes | 0.0015 | measured |
| **total** | **6.2219** | |

against **6.5948 ms** for the two-pass arrangement the audit costed. Raising
COUT_MAX saves 0.3729 ms/frame (5.65%) and 921,600 B/frame of DDR re-read.

The entropy stage moves the other way: M drops 8 -> 4, so symbols per frame
halve, 115,200 -> **57,600**, on a 64-symbol alphabet instead of 16. `T_RANGE`
was measured at M=8/K=16 and does not carry over; it should fall, and must be
re-measured on the board.

Rate: `M * log2(K) = 24` bits per position, **0.375 bpp** at 1280x720, down
from 0.500 bpp at M=8/K=16. The transport word stays 32 bits with the top byte
structurally zero, so `idx_out` is still 57,600 B/frame and the S2MM length,
cache ranges and 4-beats-per-group output count are unchanged.

## 9. Synthesis

**Vivado 2020.2, `xc7z020clg484-1`, out-of-context, 10 ns constraint.** These
are the real tool and the real part, so the absolute numbers stand as well as
the deltas.

(Tool path: `C:/SPROJ/Vivado/2020.2/bin`. `C:/Xilinx/Vivado/2020.2` holds only
`data/`, `tps/` and `win64/` -- no `bin/` -- and is not the install. Vivado
2025.1 is present but has no Zynq-7000 devices, so it cannot target this part
and supplies nothing here. Recorded in `CLAUDE.md`.)

Four configurations, so each change is attributable separately:

| | orig | base | cout | deploy |
|---|---|---|---|---|
| RTL | **as shipped** | new | new | new |
| COUT_MAX | 240 | 240 | **256** | **256** |
| VQ_K / NORM_D / SCORE_W | 16 / 128 / 20 | 16 / 128 / 20 | 16 / 128 / 20 | **64 / 256 / 21** |

| resource | orig | base | cout | deploy | base−orig | cout−orig | deploy−orig |
|---|---|---|---|---|---|---|---|
| Slice LUTs | 13,163 | 12,700 | 12,702 | 13,046 | −463 | −461 | **−117** |
| LUT as Logic | 13,082 | 12,619 | 12,621 | 12,909 | −463 | −461 | −173 |
| LUT as Memory | 81 | 81 | 81 | 137 | 0 | 0 | **+56** |
| Slice Registers | 13,809 | 13,824 | 13,824 | 13,789 | +15 | +15 | **−20** |
| Block RAM Tile | 46 | 46 | 46 | 46 | **0** | **0** | **0** |
| RAMB36 / RAMB18 | 28 / 36 | 28 / 36 | 28 / 36 | 28 / 36 | 0 | 0 | 0 |
| **DSP48E1** | **220** | **220** | **220** | **220** | **0** | **0** | **0** |
| WNS (ns) | +1.608 | +1.841 | +1.841 | **+1.809** | +0.233 | +0.233 | **+0.201** |

Reading it:

- **DSP48E1 is 220 in all four.** The PW engine alone already saturates this
  device's DSP column, and nothing here moves it -- not COUT_MAX, not the VQ
  generalisation. That is the direct answer to "does raising COUT_MAX to 256
  change DSP usage": **no**, and it could not, because `Q = N_OC = 32` and
  `N_LANES = 8` are untouched and they are what set the MAC array size.
- **COUT_MAX 240 -> 256 costs +2 LUTs and nothing else** (cout vs base). Zero
  registers, zero BRAM, zero DSP, zero timing. Exactly as §4 predicted: `W_AW`
  stays 11, `PARAM_AW` stays 8, and both memories still fit one BRAM18 per
  bank. The audit's claim that this ceiling is free was correct.
- **Block RAM is unchanged at 46 tiles.** The norm ROM stays distributed RAM
  (`ram_style = "distributed"`), which is where the +56 LUT-as-Memory comes
  from: 128x20 -> 256x21 bits.
- **The deployed configuration uses 117 fewer LUTs and 20 fewer registers than
  the shipped RTL.** The packing mux shrinks from eight 4-bit fields per 32-bit
  word to four 6-bit fields per 24-bit word on each of 8 lanes, and that more
  than pays for the larger norm ROM. The attribution is inference; the
  measurement is not.
- **Timing does not regress.** +1.608 -> +1.809 ns, i.e. 0.2 ns MORE slack than
  the shipped RTL. The base/cout gain of +0.233 ns is on a path none of this
  touches (`oc_batch_idx -> shadow_copy_len`), so it is netlist perturbation,
  not an improvement to claim. The honest statement is: no regression.
- The deployed build's critical path is
  `u_ppu/sel_shift_s2 -> u_ppu/inc3_pre_r` -- the pre-existing requantiser
  rounding path, not anything added here.

### The guard had to be pipelined, and the cost of not doing it is measured

The first implementation put the geometry check directly in the
`start_in -> next-state` cone. Same tool, same part, deployed generics:

| guard placement | WNS | critical path |
|---|---|---|
| none (shipped RTL) | +1.608 | `oc_batch_idx -> shadow_copy_len` |
| in the S_IDLE next-state cone | **+0.066** | `reg_cout_run -> st_reg[2]` |
| registered, 2 stages (shipped here) | **+1.809** | `u_ppu` requantiser |

The in-cone version technically closes at synthesis, with 0.066 ns of the
10 ns period left, and it is the critical path of the whole IP. That is not a
margin worth carrying into place-and-route on a device already at 220/220 DSP.

Two separate causes were fixed. First, the window-fit test contains a multiply
and Vivado inferred a **DSP48E1** for it -- combinational DSP delay inside a
control path -- removed with `(* use_dsp = "no" *)` and by replacing the `ceil`
divide with a plain shift by `$clog2(max(N_OC, VQ_K))`. Second, even as LUTs
the comparison chain was long enough to bind, so it was split across two
registered stages.

The guard has no throughput role: it only has to be settled by the time
`start_in` arrives, and the geometry registers are written over AXI-lite many
transactions earlier. `a_cfg_decision_settled` asserts that rather than
assuming it. Two cycles of latency there are free; a safety check eating the
headroom of the datapath it protects would not have been.

### Does it still fit xc7z020clg484-1?

**Yes.** On the real part and the real tool:

| | deployed, this IP alone | device |
|---|---|---|
| Slice LUTs | 13,046 | 53,200 (24.5%) |
| Slice Registers | 13,789 | 106,400 (13.0%) |
| Block RAM Tile | 46 | 140 (32.9%) |
| DSP48E1 | 220 | 220 (**100%**) |

DSP is the binding resource and it does not move. LUTs and registers go down.
BRAM is unchanged. Timing has 1.809 ns of slack against 10 ns, 18% of the
period, on a path this change did not touch.

Out-of-context synthesis of one IP is a strong indicator, not a substitute for
the whole design at 220/220 DSP with the real floorplan. Section 10 does that.

## 10. Full block design, post-route

§9 was out-of-context synthesis of the PW IP alone. This is the whole design --
PW plus the DW engine, three DMAs, the interconnect and the PS7 -- taken all
the way to a bitstream.

**Vivado 2020.2, `xc7z020clg484-1`, top `hw_wrapper`, post-route.** Built on a
COPY of the project in scratch, so `Zynq.xpr` and its existing `impl_1` results
are untouched. The baseline column is the shipped design's own
`Zynq.runs/impl_1` reports (2026-09-04).

| resource | shipped (M=8/K=16) | new (M=4/K=64) | delta | % of device |
|---|---|---|---|---|
| Slice LUTs | 34,358 | **34,343** | **−15** | 64.55% |
| LUT as Logic | 32,823 | 32,749 | −74 | 61.56% |
| LUT as Memory | 1,535 | 1,594 | +59 | 9.16% |
| Slice Registers | 32,354 | **32,341** | **−13** | 30.40% |
| Block RAM Tile | 82.5 | **82.5** | **0** | 58.93% |
| **DSP48E1** | 220 | **220** | **0** | **100.00%** |
| WNS (ns) | +0.077 | **+0.166** | **+0.089** | |
| WHS (ns) | +0.012 | +0.013 | +0.001 | |
| TNS / THS | 0.000 / 0.000 | 0.000 / 0.000 | | |
| timed endpoints | 100,426 | 100,785 | +359 | |
| bitstream | written | **written** | | |

**It fits, it closes, and it builds.** `write_bitstream` completed.

The headline is how little moves. The design is 15 LUTs and 13 registers
SMALLER, BRAM is identical, and DSP is 220/220 in both -- the convolution
engine saturates the DSP column and the VQ changes never touched it, because
`Q = N_OC = 32` and `N_LANES = 8` are what size the MAC array and neither
changed. The +59 LUT-as-Memory is the norm ROM growing 128x20 -> 256x21 as
distributed RAM; the logic LUTs fall by more than that, mostly because the
index-packing mux shrinks from eight 4-bit fields per 32-bit word to four
6-bit fields per 24-bit word across 8 lanes.

**Timing is the number that matters here, and it improves.** The shipped design
closes with **0.077 ns** of margin on a 10 ns period -- 0.8%. That is tight
enough that a careless change would break it. This build closes at **0.166 ns**,
slightly better, with zero failing endpoints on either setup or hold. The
improvement is not something to claim credit for -- it is on paths this work
does not touch, and 0.089 ns is within the run-to-run variation of a design
this close to the edge. The honest statement is: **no timing regression, and
the margin is unchanged in practice.**

That 0.077 ns baseline is also the reason the configuration guard was
pipelined. §9 measured the in-cone version at +0.066 ns of slack out of context,
where the rest of the design is absent. Dropping a new critical path into a
full design that already has only 0.077 ns would not have closed.

Power, post-route vector-less estimate: **2.531 W** total on-chip (2.354 W
dynamic, 0.178 W static). This is a Vivado ESTIMATE at default switching
activity, not a measurement -- the board's PMBus figures (2.0342 / 2.0359 W in
`board_measured.py`) are the real numbers and are not comparable to it. Treat
this only as a sanity check that nothing pathological happened.

### One defect this caught that nothing else could

The PW core source exists TWICE in the project:

```
Zynq.srcs/src/pw_pixel_major_core.sv            <- the IP PACKAGES THIS ONE
Zynq.srcs/sources_1/new/pw_pixel_major_core.sv  <- the one normally edited
```

Both are tracked in git and were byte-identical. `Zynq.srcs/component.xml`
lists `src/pw_pixel_major_core.sv`, while every other file of the IP comes from
`sources_1/new/`. The RTL work in this study edited only `sources_1/new/`, so
the packaged IP kept the OLD core -- and every check passed anyway, because
xsim and out-of-context synthesis both read `sources_1/new/` directly.

The full build failed immediately and unambiguously:

```
ERROR: [Synth 8-7136] In the module 'pw_pixel_major_core' ...
       parameter 'VQ_K' used as named parameter override, is a localparam
```

New wrapper, stale core. The two files are now back in sync, and the trap is
recorded in `CLAUDE.md`. The lesson generalises: **out-of-context synthesis of
an IP cannot detect that the IP is packaging different source than you edited.**
Only a full block-design build can.

# rANS entropy coder -- how it works

*Written 2026-09-14. Code: `Final_code_2/src/rans.[ch]`, `rans_edge.[ch]`,
`edge_harness.c` (commit 3978d91). Specification: `RANS_GUIDE.md` and
`CONTEXT_CODEC.md` from the CS team. There is no Python reference in this
repository, and byte-identical acceptance against the CS reference is still
open (section 8).*

## 1. What gets coded

The PW search writes one 32-bit transport word per latent position (90 x 160 =
14,400 positions) carrying four 6-bit hardware codes k, group g in bits
6g..6g+5. Each group is one independent stream of n = 14,400 tokens:

```
k (0..63) --k2id[g]--> original id (0..255) --> plane g, raster order
```

The coder works on ORIGINAL ids with 256-wide tables (spec 1). The per-group
k -> id map and the id -> slot map belong to the model export. Until that
exists, `re_synth_maps()` uses `id = (97k + 29g + 7) mod 256` and slot = k.
**Synthetic.**

## 2. Context

The context of token t is the id of its left neighbour in the same plane;
column 0 uses the border id K = 256. `slot_of_id` maps the context id to one of
65 stored slots (64 codes, border = slot 64). That is table addressing only --
it selects a table and never changes a coded symbol.

## 3. Tables

Per group, 65 slots, each `f[256]` then `cdf[256]` as uint16, adjacent so one
token's two lookups share cache lines. 16-bit probabilities, every f >= 1,
every slot sums to exactly 65,536 -- enforced at boot by `rans_tables_check()`.
`cdf[256]` (= 65,536) is never stored or read.

The rows are expanded at boot from the section-3 ROM:

| per group | bytes |
|---|---|
| 65 slots x (sym[16] + freq[16] as uint16 LE) | 3,120 |
| marginal, 256 x uint16 LE | 512 |
| **group** | **3,632** |
| **ROM, 4 groups** | **14,528** |

One slot (spec 3.4, `rans_reconstruct_row`):

1. the 16 stored ids take their stored frequencies;
2. escape = 65,536 - (sum of the 16);
3. the 240 unstored ids, in ascending id order, share the escape in proportion
   to their marginal, through `rans_quantize_to_total` (spec 3.5):
   `f_i = max(1, floor(w_i * M / sum(w)))` in 64-bit, then the remainder is
   settled walking the ids in descending weight (ties: ascending index) --
   +1 per visit while short, -1 per visit on any f > 1 while over;
4. cdf is the running sum, and the row must total 65,536.

A slot with no calibration data stores the marginal's own top 16 and so
reconstructs to the marginal exactly.

On-board RAM: dense rows 4 x 65 x 512 x 2 B = 266,240 B, plus the reciprocal
tables of section 5, 4 x 65 x 256 x 4 B = 266,240 B.

The on-board tables are FITTED at boot on calibration frames 1..20
(`re_fit_tables`, ~70 ms, outside every timed bracket): per-slot histograms,
the marginal, top 16 by count (then marginal, then lowest id), stored mass
`M16 = 65536 * n16 / (tot + 256)` clamped to [16, 65,024]. They are serialised
in the section-3 layout and expanded through the same `rans_rom_expand_group`
the real ROM will use. **Synthetic.**

## 4. The coder

32-bit state, `RANS_L = 2^23`, invariant `L <= x < 256 L`. Tokens are encoded
BACKWARD (t = n-1 .. 0), one state per group, starting at L.

Encoder, per token with frequency f and cumulative c:

```
x_max = 32768 * f                          (32768 = (L >> 16) << 8, derived)
while x >= x_max: emit x & 0xFF (writing backward); x >>= 8     renormalise FIRST
x = ((x / f) << 16) + (x mod f) + c
```

then a four-byte flush, least significant first and still written backward, so
the stream opens with the final state big-endian. Worst case 2n + 4 bytes.

Decoder (forward; `rans_decode_ctx`, used by the tests and the on-board round
trip):

```
x = first 4 bytes, big-endian
slot = x & 0xFFFF;  s = binary search of cdf   (never reads cdf[256])
x = f[s] * (x >> 16) + slot - cdf[s]           update FIRST
while x < L: x = (x << 8) | next byte
```

A valid stream is consumed exactly: the read pointer must land on its length.

## 5. Divide-free encoder -- what runs on the A9

The Zynq-7000 Cortex-A9 has no hardware divide. The reference encoder paid two
`__aeabi_uidivmod` calls per token (`x / f`, and the column `t mod w`):
16.3265 ms per frame, 283 ns per token, on the board.

`rans_enc_step` takes the reciprocal path whenever `T->recip` is set
(`rans_recip_fill` at boot):

```
sh = ceil(log2 f)             32 - clz(f - 1): one CLZ
m  = ceil(2^(31+sh) / f)      precomputed per slot and symbol
q  = mulhi(x, m) >> (sh - 1)  one UMULL
x  = x + c + q * (65536 - f)  = (q << 16) + (x - q f) + c
f = 1:  x = (x << 16) + c
```

q equals floor(x / f) for every x < 2^31 because m f - 2^(31+sh) < f <= 2^sh
(Granlund and Montgomery 1994, theorem 4.2), and after renormalisation
x < 32768 f < 2^31. The column is carried and counted down (one divide per
slice, not per token), and the table descriptor is copied into locals because
every byte written through the output buffer may alias it.

Evidence that it writes the same bytes (`test/rans_recip_test.c`):

- the bound holds for every f in 2..65,535;
- every reachable quotient, at remainder 0 and at f - 1: 4,294,836,224 checks,
  0 wrong;
- 240 random streams byte-identical across reference one-shot, fast one-shot,
  fast sliced, and slices alternating between the two paths -- all decode;
- poisoned streams stop on the same token with the same error code.

A9 code generation (arm-none-eabi-gcc -O3, the Vitis flags): the reciprocal loop
contains UMULL and CLZ and no divide call.

Board, MEASURED: **16.3265 -> 4.8061 ms per frame, 283 -> 83 ns per token**,
unpack and payload assembly included. `T->recip = NULL` (or
`re_set_fast_divide(0)`) restores the reference encoder.

## 6. Container (spec 7)

17-byte header, `struct <4sBBHBII`:

| offset | field | value |
|---|---|---|
| 0 | magic | `NIC1` |
| 4 | mode | 3 context, 0 raw |
| 5 | G | 4 |
| 6 | K | 256, uint16 LE |
| 8 | prob_bits | 16 |
| 9 | H | 720, uint32 LE |
| 13 | W | 1280, uint32 LE |

Mode 3 body: for each group, a uint32 LE length and then its stream. The writer
checks ceil(H/8) x ceil(W/8) = n and ceil(W/8) = w. Mode 0 body: the four id
planes concatenated, n bytes each, chosen when the mode-3 body would be
>= n x G bytes. The fallback is the guide's, unchanged.

## 7. The on-board stage

One resumable job per frame, `re_job_start` / `re_job_step(budget)` /
`re_job_finish`. Phase 1 unpacks transport into planes (4 budget units per
position), phase 2 codes the groups in order (1 unit per token), finish builds
the payload. Slicing is byte-identical to one-shot (`rans_edge_test`). Planes
and work buffers are static, so only ONE job may be live.

Where the work sits in the pipelined pass, frame N:

| window | PS work |
|---|---|
| analysis DMA waits of N (`CASCADE_BG`) | pack N+1 first, then entropy of N-1, 64-token slices |
| VQ poll loop of N | whatever entropy of N-1 is left |
| after the search | payload assembly |

The serial pass codes every frame one-shot and decodes it back to hardware k
(`re_job_verify`). The power loop runs the pipelined schedule; before 3978d91 it
finished the job ahead of the codebook reload, so its period was 30.83 ms
against the pass's 26.20 ms.

Board, 2026-09-11, pwvq_k64c, MEASURED: II 20.8405 ms (47.98 fps); all 57,600
tokens coded inside the analysis waits; 0.0032 ms of entropy exposed;
end-to-end 62.47 ms (composed from measured offsets); 42.84 mJ per frame;
round trip 0 mismatches.

## 8. Test ladder, and what is open

| stage | test | proves |
|---|---|---|
| 0 / 1 | `rans_stage01_test.c` | guide vectors A and B byte-exact |
| 2 | `rans_stage2_test.c` | encoder and decoder are inverses on synthetic tables -- NOT compatibility |
| 3 | `rans_stage3_test.c` | spec 3.5 hand vectors, ROM reconstruction, header, payload, fallback -- a self-check |
| edge | `rans_edge_test.c` | unpack + rANS + payload lossless back to k; slicing byte-identical |
| fast | `rans_recip_test.c` | the divide-free encoder writes the reference bytes |

Open:

- **Stage 3 acceptance** -- byte-identical against the CS reference:
  `rans_stage3.exe DIR` with their `rom.bin`, `slots.bin`, `idx.bin`,
  `payload.bin`. Not run; no dump exists.
- **The spec-1 remap reading** (ids coded 256-wide, remap = addressing) awaits
  confirmation.
- **The real k -> id map and ROM** from the model export replace
  `re_synth_maps` / `re_fit_tables`. Until then payload bytes are not a rate.

# VQ -- current state, and dedicated vs shared

*Written 2026-09-14. Re-derive the bitstream identity before quoting it (see
`CLAUDE.md`).*

## 1. What is deployed

| | |
|---|---|
| engine | the shared PW engine, `USE_PW_VQ=1` |
| geometry | M=4, K=64, Dm=16; 24 bits/position; cin_mac 16, cout_run 256, 8 batches, 21-bit score |
| bitstream | pwvq_k64c (0e93c7f, the FIFO-flush fix); `Zynq.runs/impl_1/hw_wrapper.bit` md5 `ec66d74c588ac416d186db4b4521200d` on 2026-09-14 |
| correctness | vs `vqpw_encode_frame()` 0 mismatches of 57,600; channel map 128/128 exact; every codeword reachable; guard present |
| codebook | in the weight BRAM the analysis overwrites, so reloaded every frame |

Board, 2026-09-11, pipelined pass, MEASURED:

| | ms |
|---|---|
| codebook reload | 0.9603 |
| search, accelerator (272 cyc/group x 1,800) | 4.8983 |
| search, driver bracket | 5.2770 |
| analysis, 3 fused pairs (1.0925 of it layer programming) | 14.5902 |
| **frame interval** | **20.8405 (47.98 fps)** |
| DW_PW / VQ overlap violations | 0 |

Board power 2.0567 W with the VQ; the VQ increment is +0.0075 +- 0.0074 W, not
resolved, **< 14.9 mW**.

Full design post-route (Vivado 2020.2, xc7z020clg484-1, `impl_1` of
2026-09-10): LUT 33,975 (63.86%), FF 32,342 (30.40%), BRAM 82.5 (58.93%), DSP
220/220, WNS +0.122 ns, WHS +0.018 ns. Vector-less estimate 2.506 W, of which
the PW IP 0.391 W -- an estimate, not a measurement.

The dedicated engine (`vq_pq_axi_0`, M=4, K=256, QUERY_LANES=2) left the block
design in 62cbfbc (2026-09-04). Its IP lives at
`C:/Users/Fahad/ip_repo/vq_pq_axi_1.0`, outside this repository, and its driver
`vq_pl.[ch]` is untracked in `Final_code_2/src`. On silicon at K=256: 18.435 ms
from its own cycle counter, 1,024.1 cycles per group, codebook loaded once per
model.

## 2. Resources

Out-of-context synthesis, Vivado 2020.2, xc7z020clg484-1, 10 ns, 2026-09-14.
Reports and scripts: `results/vq_dedicated_vs_shared/`.

| | Slice LUT | LUT logic | LUT RAM | FF | BRAM36 | DSP | WNS (ns) |
|---|---|---|---|---|---|---|---|
| dedicated, K=64 | 8,312 | 8,238 | 74 | 8,561 | 9 | 0 | +4.200 |
| dedicated, K=256 | 8,323 | 8,241 | 82 | 8,595 | 9 | 0 | +4.119 |
| shared PW engine, VQ compiled in | 12,482 | 12,345 | 137 | 13,774 | 46 | 220 | +1.565 |
| shared PW engine, VQ compiled out | 12,181 | 12,156 | 25 | 13,277 | 46 | 220 | +1.608 |
| **shared VQ branch** | **+301** | **+189** | **+112** | **+497** | **0** | **0** | -0.043 |
| **dedicated K=64 / shared branch** | **27.6x** | 43.6x | 0.66x | **17.2x** | 9 vs 0 | 0 vs 0 | |

Where the difference comes from:

- The dedicated datapath is QUERY_LANES x M x DSUB = 2 x 4 x 16 = 128 INT8
  multipliers in LUTs, whatever K is; K only sets the sweep length, which is
  why K=64 and K=256 synthesise within 11 LUTs of each other. Its 9 BRAM36 are
  the codebook memories plus a group buffer, and they do not shrink at K=64.
- The shared branch reuses the PW MAC array and the weight BRAM. It adds the
  norm ROM (256 x 21 bits, distributed -- the +112 LUT RAM), the score/argmin
  path and the index packing. Its codebook costs no BRAM because it borrows
  the analysis weight memory, which is exactly why it is reloaded every frame.

Provenance notes:

- K=64 is not the IP as shipped: `vq_pq_top.sv:329` sliced a fixed 8 bits out
  of a KW-bit index and fails to elaborate below K=256. The scratch copy takes
  KW bits and lets the 8-bit field zero-extend (`copy_vq_k64.py`), which is
  identical at K=256. Synthesised only -- not simulated at K=64, never on the
  board at K=64.
- K=256 today (8,323 LUT / 8,595 FF) sits above the record in
  `ip_repo/.../doc/VQ_QUERYLANES_RESULTS.md` (8,074 / 8,138) on the same tool
  and part. The IP has changed since that record; not traced further.
- Every dedicated-engine batch lost its FIRST `synth_design` to a tool-side
  helper failure (`couldn't read file ... No error`) before elaboration; the
  second call in the same process always succeeded, and the PW batch was
  unaffected. The FAILED lines in `wns_results.txt` are those, not the design.

Full design, PROJECTED -- arithmetic on measured parts, not an in-context build:

| | shared, post-route (measured) | dedicated, OOC arithmetic | dedicated, in-context history |
|---|---|---|---|
| LUT | 33,975 (63.9%) | 41,986 (78.9%) | ~39,690 (74.6%) |
| FF | 32,342 (30.4%) | 40,406 (38.0%) | ~34,317 (32.3%) |
| BRAM36 | 82.5 (58.9%) | 91.5 (65.4%) | 91.5 (65.4%) |
| DSP | 220 (100%) | 220 | 220 |

"OOC arithmetic" is today's post-route design minus the shared branch plus the
dedicated K=64 engine; one extra AXI-lite interconnect port is not counted.
"In-context history" applies the difference between the 2026-08-30 dedicated
build (`vq_impl_util3.rpt`: 40,073 LUT / 34,329 FF / 91.5 BRAM) and the
2026-09-04 shared build (34,358 / 32,354 / 82.5); those builds differ in more
than the VQ. OOC counts overstate what survives in context, flip-flops most, so
the real cost most likely lies between the two columns. The 2026-08-30
dedicated build did route and meet timing at 220/220 DSP; today's design has
+0.122 ns of slack to absorb the extra logic.

## 3. Latency and energy

Shared: MEASURED. Dedicated: PROJECTED from measured parts
(`dedicated_projection.py`). The accelerator time comes from the K=256 silicon
law, 4K cycles per group plus 139 fill, which reproduces 18.4334 ms against the
18.435 ms measured. Driver overhead is ASSUMED equal to the shared engine's
0.3787 ms (no dedicated-era record). Energy is 2.0567 W x II, so the extra
fabric's power is NOT included.

| | shared (measured) | D1: dedicated, same serial order | D2: dedicated, VQ during the next analysis |
|---|---|---|---|
| codebook reload | 0.9603 | 0 | 0 |
| accelerator | 4.8983 | 4.6094 | 4.6094 |
| driver bracket | 5.2770 | 4.9881 | 4.9881 |
| **II** | **20.8405 (47.98 fps)** | **19.5913 (51.04 fps), -6.0%** | **14.6032 (68.48 fps), -29.9%** |
| end-to-end | 62.47 | 58.73 | 43.76 |
| energy per frame | 42.84 mJ | 40.29 mJ | 30.03 mJ |

At K=64 the dedicated search is only 5.9% faster than the shared one (256
against 272 cycles per group). D1 gains the reload and that 5.9%. D2 gains the
engine: the analysis becomes the only binding stage. Its CPU budget checks out
against the measured schedule -- 13.50 ms free in the analysis wait against
11.16 ms needed, and the VQ of N done 4.99 ms into N+1, before entropy of N
starts at 6.44 ms.

D2 is a projection until these hold on silicon:

1. analysis DMA traffic (HP0/HP1) concurrent with VQ traffic (HP2) on the one
   DDR controller -- never measured;
2. latent ping-pong, because the VQ of N reads the latent while N+1 is analysed;
3. an unpack variant for the dedicated IP's 8-bit index fields (32 bits per
   position, not the deployed 24-bit transport);
4. the K=64 variant simulated against the golden model, then run on the board.

**The one-line comparison.** Sharing costs the VQ +301 LUT, +497 FF and no
BRAM or DSP. A dedicated K=64 engine costs about 8.3k LUT, 8.6k FF and 9 BRAM36
(27.6x the LUTs), and buys 1.25 ms of frame interval run serially -- or about
6.2 ms if its concurrency holds on silicon.
