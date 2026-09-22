# DSP Allocation and Parallelism Axes — Analytical Model

**Compiled 2026-08-08.** Companion to `PAPER_HW_EVIDENCE.md` §28, which holds the
raw measurements. This document derives the model, proves the two structural
results that determine the answer, enumerates the reachable operating points, and
selects one.

> **Provenance.** Every resource number is MEASURED (routed checkpoint, or
> out-of-context synthesis for `xc7z020clg484-1`). Every cycle number is
> SIMULATED (xsim on the real RTL), validated against the currently flashed
> bitstream to <1% on five of six blocks. **Nothing in §6 has been built.**

---

## 1. Why this analysis exists

The paper's central claim is that the design is shaped by a hard 220-DSP ceiling.
A reviewer will therefore ask two questions that the evidence package could not
previously answer:

1. Why does the depthwise engine hold **53% of the MAC DSPs** while performing
   **18.2% of the MACs**?
2. Why is `N_LANES=8, N_OC=16` the right point rather than an arbitrary one?

Both are answerable, and the answers are more interesting than "we ran out of
DSPs". The short version: **DW's allocation is set by rate-matching to the memory
port, not by its share of the arithmetic**, and **the parallelism axes have
different algebraic signatures — one divides every cost term, one has an interior
optimum, and one is a threshold with no benefit past it.**

---

## 2. Notation

| symbol | meaning |
|---|---|
| `L` | `N_LANES` — pixels per beat, shared by DW and PW |
| `Q` | `N_OC` — output channels computed in parallel by PW |
| `K` | DW kernel unroll: taps evaluated per lane per cycle, `K ∈ {1,3,9}` |
| `P_j` | PW output pixels in block `j` |
| `c_in,j`, `c_out,j` | PW channel counts |
| `M_j` | DW MACs in block `j` |
| `I_j`, `O_j` | DDR bytes read / written for block `j` |
| `B_j` | `⌈c_out,j / Q⌉` — output-channel batches |
| `Q_last,j` | `c_out,j − (B_j−1)·Q` — valid channels in the final batch |
| `D_DW`, `D_PW` | DSPs in the DW MAC array / PW MAC grid |

Workload (720p ImageEncoderLite, six depthwise-separable blocks):

| j | `c_in` | `c_out` | `P_j` | `I_j` (B) | `O_j` (B) | `M_j` |
|---|---|---|---|---|---|---|
| 0 | 3 | 16 | 230,400 | 2,764,800 | 3,686,400 | 6,220,800 |
| 1 | 16 | 32 | 57,600 | 3,686,400 | 1,843,200 | 8,294,400 |
| 2 | 32 | 32 | 14,400 | 1,843,200 | 460,800 | 4,147,200 |
| 3 | 32 | 32 | 14,400 | 460,800 | 460,800 | 4,147,200 |
| 4 | 32 | 64 | 14,400 | 460,800 | 921,600 | 4,147,200 |
| 5 | 64 | 64 | 14,400 | 921,600 | 921,600 | 8,294,400 |

---

## 3. The resource model (MEASURED)

### 3.1 DSP cost laws

| function | DSPs | note |
|---|---|---|
| DW MAC array | `L·K` | 1 MAC/DSP — **no packing** |
| DW PPU requant | `2L` | 2 DSPs per PPU, confirmed by OOC synthesis |
| PW MAC grid | `Q·L/2` | **2 MAC/DSP** — two 9-bit activations packed into one 25-bit operand |
| PW accumulators | 0 … `Q·L` | **opportunistic**, see 3.3 |
| PW PPU requant | `2L` | |
| PW shell | 2 | `tile_groups × cout_run` |

Reconciliation at the built point `L=8, Q=16, K=9`: DW `72+16 = 88`; PW
`64 + 50 + 16 + 2 = 132`; total **220** ✅ matches the device exactly.

### 3.2 The packing asymmetry

PW obtains 2 MAC/DSP; DW obtains 1. This is not an oversight — it follows from
the dataflow. PW is output-stationary with a broadcast activation, so two
activations can share one weight in a single 25×18 multiply. DW is
input-stationary with a per-channel kernel: each of the 9 taps has a *different*
weight and a *different* activation, so there is nothing to share.

Consequence, measured on the built design:

| | DW | PW |
|---|---|---|
| MACs/frame | 35.25 M (18.2%) | 158.5 M (81.8%) |
| MAC DSPs | 72 (53%) | 64 (47%) |
| achieved MAC/cycle | 13.5 of 72 | 54.1 of 128 |
| utilisation | 18.8% | 42.3% |
| **MAC per DSP per frame** | **0.49 M** | **2.48 M** |

**PW extracts 5.1× more work per DSP** — 2× from packing, 2.5× from utilisation.
§5.3 explains why the allocation is nonetheless correct.

### 3.3 Accumulators are an opportunistic mapping, not a requirement

50 DSPs in the built design implement `acc[oci][p] <= (first_ic ? '0 : acc) + …`
inside DSP48 ALUs, with `first_ic` driving OPMODE. They are named `p_0_out__*`,
and `p_0_out__42` is the endpoint of the §19 critical path — which is also why
that path has **zero logic levels**: it terminates on a DSP control pin.

They are free to relinquish. OOC synthesis at `L=8, Q=32` yields `acc = 0` with
**no source change**: Vivado stops absorbing accumulators once the multiply grid
alone needs 128 of 220. Raising `Q` therefore releases them automatically.

### 3.4 The price of a DSP

| source | LUT per DSP freed | evidence |
|---|---|---|
| PW accumulators → fabric | **36.6** | core `L8/Q16`: 144→80 DSP for +2,343 LUT |
| DW MACs → fabric | **88.8** | §23 ablation: 72 DSP for 6,390 LUT |
| PPUs → fabric | **533** | `ppu` OOC: 2 DSP for +1,067 LUT |

**PPUs are the worst source by 15×.** Narrowing them (bias 32→24 b, mult
24→18 b) drops the fabric cost to 512 LUT at no precision cost — 17 mantissa bits
against a 1/255 output step — but even then they remain the most expensive place
to find multipliers.

### 3.5 Measured core resources (OOC, `xc7z020clg484-1`)

| `L` | `Q` | LUT | FF | DSP | BRAM | mul | acc | PPU |
|---|---|---|---|---|---|---|---|---|
| 8 | 16 | 7,291 | 4,381 | 144 | 6 | 64 | 64 | 16 |
| 8 | 16 † | 9,634 | 6,474 | 80 | 6 | 64 | 0 | 16 |
| 8 | 32 | 15,001 | 10,552 | 144 | 6 | 128 | 0 | 16 |
| 8 | 48 | 20,170 | 14,669 | 208 | 6 | 192 | 0 | 16 |
| 16 | 16 | 18,754 | 12,583 | 160 | 12 | 128 | 0 | 32 |
| 16 | 24 | 25,401 | 16,893 | 220 | 12 | 188 | 0 | 32 |

† accumulators forced to fabric. **BRAM is invariant in `Q`** (6 at Q = 16, 32
and 48): the weight memory is `(COUT_MAX/Q)·CIN_MAX` deep across `Q` banks, so
capacity is constant and only banking changes. BRAM **doubles** with `L`, because
`SA_DW = L·ACC_WIDTH` and the pixel buffers both widen.

---

## 4. The cycle model (SIMULATED, exact)

### 4.1 Form

$$\boxed{\ \text{cyc/group} \;=\; B\,c_{in} \;+\; Q_{last} \;+\; 6B \;+\; 1\ }$$

The four terms, in order: **compute** (one cycle per input channel per batch);
**the one final drain that does not overlap** the next batch's compute;
**per-batch overhead**; **per-group overhead**.

Fitted once and then checked against every configuration simulated — residuals
below 0.05 cycles across `Q ∈ {8,16,32}` and 20+ shapes:

| `Q` | shape | model | simulated |
|---|---|---|---|
| 8 | cin16 cout32 | `4·16+8+24+1` = 97 | 97.03 |
| 8 | cin64 cout64 | `8·64+8+48+1` = 569 | 569.05 |
| 16 | cin3 cout16 | `1·3+16+6+1` = 26 | 26.02 |
| 16 | cin64 cout64 | `4·64+16+24+1` = 297 | 297.05 |
| 32 | cin3 cout16 † | `1·3+16+6+1` = 26 | 26.02 |
| 32 | cin32 cout64 | `2·32+32+12+1` = 109 | 109.03 |
| 32 | cin64 cout64 | `2·64+32+12+1` = 173 | 173.05 |

† with partial-batch drain. Without it `Q_last = Q = 32` and the result is 42.02
— exactly 16 wasted drain cycles.

### 4.2 `cyc/group` is independent of `L`

The `ic` loop costs one cycle per input channel and the drain one cycle per
output channel, **regardless of how many pixels ride in each beat**. So `L` acts
purely through the group count `P_j/L`, and PW time scales as `1/L` exactly.

### 4.3 Validation against the running bitstream

Simulating the pre-v3 core (`bak_pre_v3_2026-07-30` — what is actually flashed)
against the 2026-08-07 board run:

| blk | hardware | simulated | error |
|---|---|---|---|
| 0 | 36.1 | 36.01 | −0.3% |
| 1 | 77.3 | 77.02 | −0.4% |
| 2 | 149.0 | 103.03 | *DW-bound* |
| 3 | 104.0 | 103.03 | −0.9% |
| 4 | 180.0 | 179.03 | −0.5% |
| 5 | 308.4 | 307.05 | −0.4% |

Block 2 is not a miss. The simulation models PW alone and returns 103.03 —
**identical to block 3**, as it must, since blocks 2 and 3 have identical PW
configurations. Hardware reads 149.0 only because DW binds there. This
reproduces §16's diagnosis from the opposite direction.

---

## 5. Reduction to DSP variables, and two structural results

Substituting `D_PW = Q·L/2` and `B = c_out/Q`:

$$T_{PW,j} \;=\; \underbrace{\frac{\text{MAC}_j}{2D_{PW}}}_{\text{compute}}
\;+\; \underbrace{\frac{P_j\,Q_{last}}{L}}_{\text{final drain}}
\;+\; \underbrace{\frac{3P_j\,c_{out,j}}{D_{PW}}}_{\text{per-batch}}
\;+\; \underbrace{\frac{P_j}{L}}_{\text{per-group}}$$

### 5.1 Theorem 1 — DW's design freedom is a single number

DW time is `M_j/(L·K)`. It depends only on the **product**, never on `L` and `K`
separately. There is therefore no DW *shape* question — only a DW *budget*
question. Any `(L,K)` with the same product performs identically.

### 5.2 Theorem 2 — lanes strictly dominate output channels

Hold `D_PW = QL/2` constant and raise `L`, lowering `Q` to compensate:

- term 1 `MAC/(2D_PW)` — **unchanged**
- term 3 `3P·c_out/D_PW` — **unchanged**
- term 2 `P·Q_last/L` — falls as `1/L²` (since `Q ∝ 1/L`)
- term 4 `P/L` — falls as `1/L`

**At equal DSP cost, larger `L` is always at least as good and usually better.**
This is the algebraic reason `N_OC` saturates while `N_LANES` does not, and it is
the single most useful result in this document.

### 5.3 Corollary — the minimum DW allocation, and why 72 is correct

DW must merely avoid binding: `M_j/(L·K) ≤ T_j` for all `j`. At the built point
the binding block is b3, giving `L·K ≳ 50`.

But observe the measured DW MAC cycles at `L·K = 72` against the read floors:

| blk | DW MAC `M_j/72` | read floor `I_j/8` |
|---|---|---|
| 3 | 57,600 | 57,600 |
| 4 | 57,600 | 57,600 |
| 5 | 115,200 | 115,200 |

**Exactly equal on all three stride-1 blocks.** 72 MACs is precisely what
sustains one beat per cycle at stride 1 — `8 lanes × 9 taps` consumes an 8-pixel
beat and produces 8 outputs in the same cycle. **DW's allocation is
rate-matching to the 64-bit HP port, not a workload share.** That is the answer
to the reviewer's first question, and it is a design *result*, not an accident.

The corollary is that DW is over-provisioned on the stride-2 blocks — 81.8% of
its ingest — where at most 2 of 8 lanes produce valid outputs.

### 5.4 The axes, summarised

| axis | scope | DSP cost | algebraic signature | verdict |
|---|---|---|---|---|
| `L` | **both engines** | `K + Q/2` per lane | in **all four** denominators | the only axis that scales without limit |
| `Q` | PW only | `L/2` per channel | divides terms 1,3; **multiplies** term 2 | interior optimum |
| `K` | DW only | `L` per tap | threshold only | set to the least value that clears the rate constraint |
| input channels | PW | — | strictly sequential | not an axis |

`L` is *not* a per-engine axis: it is the beat width, shared by DW and PW.
Changing it changes both engines simultaneously — which is why it is expensive
and why it is the only thing that helps block 0.

---

## 6. The reachable operating points

### 6.1 The floor no allocation can beat

Block time is `max(read, DW MAC, PW, write)` — DW and PW run concurrently, which
*is* the fusion. Per-block DMA floors at 8 B/cycle per 64-bit HP port:

| blk | read | write | `max` |
|---|---|---|---|
| 0 | 345,600 | 460,800 | 460,800 |
| 1 | 460,800 | 230,400 | 460,800 |
| 2 | 230,400 | 57,600 | 230,400 |
| 3 | 57,600 | 57,600 | 57,600 |
| 4 | 57,600 | 115,200 | 115,200 |
| 5 | 115,200 | 115,200 | 115,200 |
| **Σ** | | | **1,440,000 cyc = 14.40 ms** |

### 6.2 Catalogue

DSP budget: `L·K + 2L + Q·L/2 + 2L + 2 ≤ 220` (DW MAC term drops to 0 in fabric).

| # | `L` | `Q` | `K` | DW MACs | DSP | LUT | BRAM | HW | fps | binds on |
|---|---|---|---|---|---|---|---|---|---|---|
| 1 | 8 | 16 | 9 | DSP | 220 | 18,280 | 73.5 | 24.5 ms | 30.6 | PW everywhere |
| 2 | 8 | 24 | 9 | DSP | 202 | ~25,000 | 73.5 | 22.37 ms | 33.4 | PW |
| 3 | 8 | 28 | 9 | DSP | 218 | ~28,000 | 73.5 | 22.01 ms | 33.8 | PW |
| **4** | **8** | **32** | **9** | **fabric** | **162** | **~32,400 (61%)** | **73.5** | **20.76 ms** | **35.4** | b0 PW, b1/b2 read |
| 5 | 8 | 40 | 9 | fabric | 194 | ~35,000 (66%) | 73.5 | 20.47 ms | 35.8 | same |
| 6 | 8 | 48 | 9 | fabric | 226 ✗ | — | — | — | — | infeasible |
| **7** | **16** | **16** | **3** | **fabric** | **194** | **~40,000 (75%)** | **~105** | **16.58 ms** | **41.6** | b0 write, b1/b2 read |
| 8 | 16 | 24 | 3 | fabric | 254 ✗ | — | — | — | — | infeasible |
| — | ∞ | ∞ | — | — | — | — | — | *14.40 ms* | *~48* | DMA only |

Notes:
- #4 needs DW's MACs in fabric because `72 + 34 + 128 = 234 > 220`. That move is
  **free in throughput** — the array is still `8×9 = 72` MAC/cycle either way —
  costs 6,390 LUT, and *restores* the ~4× setup-slack margin the DSP P-register
  had been providing (§23 ablation, in reverse).
- #5 buys 1.4% over #4 for 32 more DSPs. Poor marginal return; see 6.3.
- #7 caps at `Q=16`: OOC synthesis returns 220 DSP for the core alone at
  `L=16, Q=24`, before DW's PPUs.
- At `L=16`, `K=3` costs 0.03 ms against `K=9` while using 96 fewer fabric MACs
  (~8,500 LUT). `K=3` is the correct choice by Theorem 1.

### 6.3 Why the `L=8` family stalls at ~20.5 ms

Block 0 costs `26 cyc/group × 28,800 groups = 748,800 cycles = 7.49 ms`, which is
**36% of the frame**. Its `cyc/group` decomposes as `3 + 16 + 6 + 1`: only 3
cycles are compute. It is drain-dominated, and because `c_out = 16` caps
`Q_last` at 16, **no increase in `Q` can touch it** — the drain term `P·Q_last/L`
has no `Q` in it once `Q ≥ c_out`.

Only `L` divides that term. This is Theorem 2 made concrete, and it is why the
jump from #4 to #7 is worth 20% while #4 → #5 is worth 1.4%.

---

## 7. Selected operating point

### 7.0 THE OPTIMUM — what it is, why it is optimal, and why we are not on it

> ## ⚠️ CORRECTED 2026-08-09 — THE OPTIMUM IS `N_LANES=8, N_OC=32`
>
> **§7.0.1–7.0.5 below contain an unfair comparison and their conclusion is
> superseded.** They compare `L=8,Q=32` **with** the copy overlap against
> `L=16,Q=16` **without** it. Applying the overlap to both, which is the only
> honest comparison:
>
> | blk | `L=8, Q=32` | `L=16, Q=16` (K=9) |
> |---|---|---|
> | 0 | 460,800 *(write-bound)* | 460,800 *(write-bound)* |
> | 1 | 460,800 *(read-bound)* | 460,800 *(read-bound)* |
> | 2 | 230,400 *(read-bound)* | 230,400 *(read-bound)* |
> | 3 | 70,200 | 69,300 |
> | 4 | 138,600 | 137,700 |
> | 5 | 253,800 | 252,900 |
> | **Σ** | **16.15 ms** | **16.12 ms** |
>
> **They tie to within 0.2%**, which is far inside the modelling uncertainty.
>
> ### Why they converge — and why this weakens Theorem 2
> Both have **`D_PW = 128`** (`32·8/2` and `16·16/2`), i.e. *identical* PW
> multiplier capacity, so the compute term `MAC/(2·D_PW)` is identical.
>
> Theorem 2 (§5.2) says lanes dominate output channels because the drain term
> `P·Q_last/L` falls as `1/L²` at fixed `D_PW`. **But that term IS the
> un-overlapped shadow copy.** Once it is overlapped, only the `P/L` per-group
> term still favours lanes, and it is worth ~1 cycle per group.
>
> **The copy overlap does not merely save 4.65 ms — it dissolves the reason to
> prefer more lanes.** Theorem 2's dominance was largely an artifact of the
> serialised drain, and that is a result worth stating in the paper: *the
> apparent advantage of spatial parallelism over channel parallelism in this
> architecture was a control-path artifact, not a fundamental property.*
>
> ### Consequence
> **`N_LANES=8, N_OC=32` + copy overlap is the best point in the enumerated
> space** `L ∈ {8,16} × N_OC ∈ {16,32}` at fixed `D_PW = 128`, evaluated with the
> cost model validated to 2.4% on silicon (§16). It is *not* a global optimum —
> the space is four points, holds the DSP split fixed, and holds the channel
> schedule fixed. State it that way in the paper: an optimum over a declared
> space is defensible; "global" is not. It
> ties the 16-lane configuration, and it is **already built and measured**
> (22.03 ms today; 16.15 ms projected with the overlap). `N_LANES=16` would cost
> three RTL projects — windower rewrite, MAC-cascade restructure, 128-bit
> datapath — plus BD and host changes, for ≤0.2%.
>
> Read §7.0.1–7.0.5 as the *analysis of the axes*, which remains valid and is
> what the paper needs. Do not read its conclusion as the build target.
>
> ### How the error was made — worth not repeating
> The superseded conclusion compared `L=8,Q=32` **with** the copy overlap against
> `L=16,Q=16` **without** it. The optimisation had been discovered while working
> on one configuration, so it stayed attached to that configuration — but a
> change to a *term in the cost model* applies to **every point in the space**.
>
> **Discipline: when an optimisation changes a model term, re-evaluate the whole
> design space, not the configuration you found it in.**
>
> The conclusion happened to survive (`L=8` is still right, because it is already
> built) but the stated reason was false. Correcting it rather than patching it
> is what exposed the `D_PW = 128` coincidence and, through it, the finding that
> Theorem 2's preference for lanes was a drain-schedule artifact.

This section answers the question the whole document exists for: *given a 64-bit
DMA, what is the best arrangement of `N_LANES` and `N_OC`, is the DSP split
between DW and PW optimal in that state, and why.*

#### 7.0.1 The optimum

**`N_LANES = 16, N_OC = 16, DW kernel unroll K = 3, DW MACs in fabric.**
**194 of 220 DSPs. ≈16.58 ms of HW time, ≈42 fps.**

> ### ⚠️ NEITHER `N_LANES` NOR `K` IS A KNOB — read before costing this
>
> **`K` DOES NOT EXIST AS A PARAMETER.** It is an *analytical* variable used in
> §5 to explore the space. `conv_mac_array.sv` has exactly three parameters —
> `DATA_WIDTH`, `ACC_WIDTH`, `USE_DSP` — and the 3×3 kernel is **fully unrolled
> by hand** as nine explicit cascade stages, `p_cas[0]`…`p_cas[8]`. K=9 is baked
> into the structure. `K=3` means restructuring that cascade into a loop that
> accumulates across three cycles: a **third RTL change**, not a setting.
>
> **`N_LANES` is a data-layout contract, not a PW-private width.** It is declared
> in the PW core, but the DW→PW stream is group-major/channel-minor — one beat is
> *one channel × N_LANES adjacent pixels*. DW hardcodes 8
> (`dw_fused_axis.sv`: `localparam int CORE_W = 8*DATA_WIDTH`, `m_axis_tdata` a
> flat `[63:0]`), so it emits
> `(ch0,px0-7) (ch1,px0-7) (ch2,px0-7) … (ch0,px8-15) …`
>
> Setting `N_LANES=16` on the BD cell **elaborates, synthesises and runs — and is
> silently wrong.** The shell's input FIFO would assemble a 128-bit word from two
> consecutive 64-bit writes, but those are two different CHANNELS, not sixteen
> adjacent pixels; PW would read channel 1 as pixels 8–15 of channel 0. Same
> failure class as the block-2 bug: correct beat counts, wrong data.
>
> Three further couplings break with it: `M_AXIS_DATA_WIDTH` becomes 128 into a
> 64-bit S2MM; the chained pair-to-pair handoff assumes group size 8; and
> `pack_input_group_major` in `main.c` builds 8-pixel groups for block 0.
>
> **So this configuration is three RTL projects (windower, MAC cascade, datapath
> width) plus BD and host changes — not a rebuild.** Cost it accordingly.

DSP distribution in that state:

| function | DSPs | law |
|---|---|---|
| PW MAC grid (multiply) | **128** | `N_OC × N_LANES/2` — 2 MAC/DSP, packed |
| PW PPU requant | 32 | `N_LANES × 2` |
| DW PPU requant | 32 | `N_LANES × 2` |
| PW shell product | 2 | fixed |
| **DW MAC array** | **0** | `N_LANES × K` moved to fabric |
| **total** | **194 / 220** | 26 spare |

So the split is **PW 162, DW 32** — 84% of the multiplier budget to the engine
doing 81.8% of the MACs, after DW's array is removed from the DSP budget
entirely.

#### 7.0.2 Why each axis sits where it does

**`N_LANES` is maximised because it is the only axis in every denominator.**
By Theorem 2 (§5.2), at fixed `D_PW` raising `L` leaves the compute and per-batch
terms unchanged while shrinking the drain term as `1/L²` and the per-group term
as `1/L`. It is the only axis that reduces the *group count*, and every per-group
cost is multiplied by that count. Lanes therefore dominate output channels at
equal DSP cost — this is why `N_OC` saturates and `N_LANES` does not.

**`N_OC` is 16 rather than larger because `L` has already consumed the budget.**
At `L=16` the PPUs alone cost `4L = 64` DSPs, so the grid can have at most
`220 − 64 − 2 = 154`, i.e. `N_OC ≤ 19`. Within that, `N_OC = 18–19` models
1.3% better than 16 (16.36–16.47 vs 16.58 ms) — real but inside the modelling
uncertainty, and it costs the clean `cout`-alignment that keeps `Q_last` uniform.
**16 is chosen for the 1.3%, not against it.**

**`K` is 3 because DW is a threshold, not a throughput axis.** By Theorem 1
(§5.1) DW time depends only on the product `L·K`, so there is no DW *shape*
question. `K` only has to clear the rate constraint. At `L=16`, `K=3` gives
`L·K = 48` and DW binds on block 3 by 2,673 cycles (0.03 ms); `K=9` removes even
that but costs ~8,500 more LUT for 96 extra fabric MACs. **0.03 ms is not worth
8,500 LUT.**

**DW's MACs are in fabric because they are the cheapest DSPs in the design and
free to move.** Measured cost per DSP released: accumulators 36.6 LUT, DW MACs
88.8 LUT, PPUs **533 LUT** (§3.4). The move is *cycle-neutral* — the array is
still `L×K` MAC/cycle either way — and it is what makes `L=16` fit at all
(with DW's MACs on DSP the configuration needs 338).

#### 7.0.3 Is the DW/PW split optimal in that state? Yes, and for a specific reason

DW is at its **minimum non-binding allocation** and PW has everything else. That
is optimal because the two engines are not symmetric:

- PW extracts **2 MAC/DSP** (activation packing); DW extracts **1** — there is
  nothing to share in a depthwise kernel, since each of the 9 taps has a
  different weight *and* a different activation.
- Measured per-DSP productivity: PW **2.48 MMAC/DSP/frame** vs DW **0.49** —
  **5.1×**, being 2× packing and 2.5× utilisation.
- DW's requirement is a *rate*, not a share: 72 MACs at `L=8` is exactly what
  sustains one input beat per cycle at stride 1 (§5.3 — DW MAC cycles equal the
  read floor to the cycle on all three stride-1 blocks). Give DW more and it
  idles; give it less and it throttles the ingest.

So the correct rule is **"DW gets a rate, PW gets the remainder"**, and that is
what this state implements. It also answers the reviewer question directly: DW
holding 53% of the MAC DSPs in the *original* design was rate-matching to the
memory port, not a workload-proportional allocation — and the fix is not to
rebalance the ratio but to take DW's array off the DSP budget entirely.

#### 7.0.4 Why it is only *near*-optimal, and what actually binds

`L=16, N_OC=16` is within **1.3%** of the best reachable configuration and within
**15%** of a bound it cannot cross:

| config | HW |
|---|---|
| L=16 N_OC=16 | 16.58 ms |
| L=16 N_OC=19 | 16.36 ms |
| L=32 N_OC=8, narrowed PPUs | 16.21 ms |
| **DMA floor** | **14.40 ms ≈ 46 fps** |

**Blocks 0–2 are DMA-bound and contribute 11.52 ms — 70% of the frame — for
*any* `L` and `N_OC`.** Only blocks 3–5 respond to parallelism at all. That is
the honest ceiling: past `L=16` the parallelism axes are exhausted, and the next
lever is the **second HP port pair (HP2/HP3 are unused)**, which would halve the
floor.

#### 7.0.5 So why are we not running it?

Two reasons, and the second is the one that matters now.

**1. It is not reachable by parameter change.** There is no `N_LANES` parameter
anywhere in the DW path — `dw_banked_window_8x.sv`, `conv_mac_array.sv`,
`dw_fused_core.sv`, `dw_fused_axis.sv`. The windower is hardcoded to 8 lanes and
structurally so (the 10-position `X_byte`/`is_x_valid` halo scheme,
`s2_R*_hold[7:0]`, "one dense 8-lane beat per input group pair"). PW parameterises
cleanly — OOC synthesis at `L=16` works — but DW does not. `L=16` is a **rewrite
of the depthwise windower**, the most intricate module in the design and the one
still carrying the open left/right-8-column edge bug (§21).

**2. A cheaper change reaches the same performance.** Measured 2026-08-08:
`S_WAIT_PPU` is 46% of per-group time and is pure waiting on the `acc → shadow`
copy, which overlaps compute **0.00** cycles. Removing it takes the *built*
`L=8, N_OC=32` configuration from 20.76 → **16.11 ms** — **better than the
`L=16` optimum**, entirely inside the PW core, with no windower rewrite (§7.1b,
`PAPER_HW_EVIDENCE.md` §29.7).

| path | HW | cost |
|---|---|---|
| built, `L=8 N_OC=32` | 22.03 ms *(measured)* | — |
| **+ copy overlap** | **~16.1 ms** | PW core only |
| `L=16 N_OC=16` | ~16.5 ms | windower rewrite |

**That inverts the roadmap.** `N_LANES=16` is no longer the way to reach ~16 ms —
it is a way to go *past* it, and the DMA floor limits how far past (16.1 → 14.4 ms
at best, ~46 fps). Its expected value has dropped from "the 20% win" to "a
further ~10% after the copy work, for the largest rewrite in the project".

**Conclusion: the optimum described in §7.0.1 is correct as an analysis of the
parallelism axes, and is NOT the right build target.** The right target is the
built configuration plus the copy overlap, which lands at the same place for a
fraction of the risk.

### 7.1 Build target: configuration #4

**`N_LANES=8, N_OC=32, partial-batch drain, DW MACs → fabric`**

| | |
|---|---|
| DSP | 162 / 220 |
| LUT | ~32,400 / 53,200 (61%) |
| BRAM | 73.5 / 140 — **unchanged**, measured invariant in `Q` |
| HW datapath | 20.76 ms |
| frame | ~28.2 ms → **35.4 fps** (from 26.9 measured) |

**Reasoning.** The DSP allocation follows directly from §3.4 and §5.3: DW's MACs
are the cheap source at 88.8 LUT/DSP and are provably non-binding once moved,
while the PPUs at 533 LUT/DSP are left alone. Raising `Q` to 32 additionally
releases the 50 accumulator DSPs with no source change (§3.3). BRAM does not
move. No datapath width changes, so the shell, the BD and the DMA configuration
are untouched — this is a parameter change plus RTL already written and
simulated.

### 7.1a ✅ IMPLEMENTED 2026-08-09 — split shadow (Route A)

The group-boundary copy is now halved: the shadow store is split into even-OC and
odd-OC halves, the copy writes two channels per cycle, and the drain alternates
halves at one per cycle. Measured in simulation across all five encoder shapes,
`S_WAIT_PPU` and `shadow_copying` both **exactly halved**, saving ~15.6
cycles/group everywhere (e.g. cin32/cout32: **71.03 → 55.38**).

Projected from the measured 08-08 hardware baseline:
**HW 22.03 → 18.87 ms, frame 29.55 → 26.39 ms, 33.8 → ~37.9 fps.**

Route B (double-buffer `acc`, the other ~2 ms) is **not** done: it needs a mux on
the accumulator feedback, i.e. the `first_ic → DSP OPMODE` critical path, on a
design closing at WNS +0.119 ns. Route A cannot touch that path — which is why it
was chosen first.

> ⚠️ An **OC-ordering** error in this change yields correct beat counts with
> channels swapped, and the TB's all-`+1` weights make every channel identical —
> invisible. Structure and beat counts are verified; **values are not**. Make the
> TB weights channel-dependent (`w_rd_data[o] = o+1`) before trusting it, or
> validate on the board.

### 7.1b The cheaper path to #7's performance — no windower rewrite

Measured 2026-08-08 (`PAPER_HW_EVIDENCE.md` §29.7): `S_WAIT_PPU` is **46%** of
block 2/3's per-group time and is pure waiting on the `acc → shadow` copy, which
overlaps compute **0.00** cycles for single-batch groups. Eliminating it takes
**20.76 → 16.11 ms (~41.8 fps)** — *the same result as configuration #7* —
entirely inside the PW core.

That reorders the roadmap: **do this before #7, not after.** It also changes how
#7 should be justified, because the 20% previously attributed to lanes is largely
available without them.

### 7.2 Next target: configuration #7

**`N_LANES=16, N_OC=16, K=3`** at ~16.58 ms / **41.6 fps**, 194 DSP.

Deferred, not rejected. It requires a 128-bit internal AXIS, a widened windower,
a reworked testbench, and it roughly doubles core BRAM against 52.5% already
used. It is the correct next step once #4 is measured — and Theorem 2 says it is
the *only* direction with headroom left.

### 7.3 What would change the answer

| if… | then |
|---|---|
| LUT at `L=16` exceeds ~48k in a real build | #7 dies; #5 becomes the ceiling at 35.8 fps |
| BRAM at `L=16` exceeds 140 | same |
| `first_ic` fanout is not fixed first | neither #4 nor #7 closes timing at 128+ grid DSPs |
| the second HP port pair (HP2/HP3) is enabled | the 14.40 ms DMA floor halves and `Q`/`L` both regain headroom |
| a layer with `c_out` not a multiple of `Q` is added | partial-batch drain already handles it; without it, padding returns |

---

## 8. Pre-build verification — results

### 8.1 Full-chain DW→PW cascade ✅ PASS

No testbench in `sim_1` instantiated both engines: every PW result came from a
PW-only harness with an ideal driver, every DW result from a DW-only harness.
`scratchpad/sim/cascade_tb.sv` closes that gap — DW's output FIFO feeding PW's
input FIFO, with DW throttling on PW's `prog_full`.

| shape | `N_OC` | stride | DW consumed / written / produced | PW out | result |
|---|---|---|---|---|---|
| b0: cin3 cout16 | 32 | 2 | 384 / 96 / 96 | 512 / 512 | **PASS** |
| b1: cin16 cout32 | 16 | 2 | 2048 / 512 / 512 | 1024 / 1024 | **PASS** |
| b5: cin64 cout64 | 32 | 1 | 8192 / 8192 / 8192 | 8192 / 8192 | **PASS** |
| ragged: cin32 **cout48** | 32 | 1 | 4096 / 4096 / 4096 | 6144 / 6144 | **PASS** |

The critical check is `produced_cnt == total_groups_r` in every case. The shell
predicts TLAST from `tile_groups × cout_run`; partial-batch drain makes the core
emit exactly `cout_run` beats per group, so the two now agree **by construction**
where previously they diverged for any `cout` not a multiple of `N_OC`. The
ragged `cout=48` case (2 batches, `Q_last=16`) is the direct test and it passes.

> ### ⚠️ A TESTBENCH LESSON WORTH KEEPING
> The first two cascade runs FAILED, reporting DW starvation — `in_empty=1`,
> `consumed = 7,941` of 8,192, PW stalled at group 114 of 128. It was **the
> driver, not the RTL**. Both procedural handshake forms are lossy when `tready`
> deasserts:
> ```
> @(posedge clk); while(!tready) @(posedge clk);   // reads tready AFTER the edge
> wait(tready); @(posedge clk);                    // races if tready drops between
> ```
> Each silently dropped ~3% of beats. Only a fully synchronous driver — index
> advancing on an **observed** `tvalid && tready` at a clock edge — is correct.
> The PW-only harness never exposed this because its input is never throttled;
> in the cascade DW back-pressures continuously. **Any future cascade work must
> use the synchronous form**, and a "starvation" symptom here should be blamed on
> the harness before the RTL.

### 8.2 DW `USE_DSP(0)` ✅ MEASURED

`USE_DSP` is a synthesis attribute: the RTL is functionally identical and the
array is `N_LANES × K = 72` MAC/cycle either way, so the move is **cycle-neutral
by construction** — no simulation can distinguish it. The question is resources:

| DW core | LUT | FF | DSP | BRAM |
|---|---|---|---|---|
| `USE_DSP(1)` | 5,592 | 3,650 | **88** | 17 |
| `USE_DSP(0)` | 13,808 | 5,146 | **16** | 17 |
| delta | **+8,216** | +1,496 | **−72** | 0 |

114 LUT per DSP freed at synthesis, against 88.8 measured post-route in §23 —
implementation optimises some of it away. Either figure keeps DW's MACs far
cheaper than the PPUs (533 LUT/DSP). BRAM unchanged.

### 8.3 `first_ic` fanout ❌ **INCONCLUSIVE — two negative results, and a retraction**

**Retraction.** An earlier draft of this section reported a worst-case fanout of
**6,669 loads** at `N_OC=32` and called the fix a blocking prerequisite. **That
number does not reproduce and must not be quoted.** Post-synthesis load counts
across three OOC sessions:

| core | `N_OC` | drivers | loads | worst net | session |
|---|---|---|---|---|---|
| baseline | 16 | 37 | 2,181 | 128 | A, B |
| baseline | 32 | 6 | 6,674 | **6,669** | A (and C, clean) |
| baseline | 32 | 69 | 6,862 | **274** | B (after a Q=16 run) |
| `(* max_fanout=16 *)` | 32 | 421 | 7,090 | 417 | A |
| explicit replication | 32 | 69 | 6,862 | 274 | B |

The same source file yields 6,669 in two sessions and 274 in another, depending
only on what was synthesised before it. **The measurement is unreliable, not the
design.** 6,669 is also implausible on its face: `first_ic` should reach roughly
`N_OC × N_LANES` accumulator muxes plus `N_OC × N_LANES/2` DSP OPMODE pins, i.e.
~380 loads at `N_OC=32`. The 274 figure is the physically sensible one, and it is
~2.1× the `N_OC=16` value of 128 — exactly proportional to `N_OC`, which is what
theory predicts and nothing to be alarmed about.

**Explicit source-level replication does not work.** A `(* dont_touch = "yes" *)
logic [N_OC-1:0] first_ic_bank` with every assignment mirrored, feeding
`first_ic_bank[oci]` into bank `oci`'s accumulators, produced a **bit-identical
netlist** at `N_OC=32` (LUT 13,618, FF 8,187, DSP 220, worst net 274 — all
unchanged) and was marginally *worse* at `N_OC=16` (worst net 133 vs 128).
Vivado's redundancy removal merges functionally identical registers back together
regardless of `dont_touch`. **This change has been reverted** — it was a no-op
carrying a maintenance cost. `max_fanout` is likewise only partially effective.

**Conclusion, honestly stated:** whether `N_OC=32` closes timing cannot be
answered by post-synthesis queries. Fanout at that stage is pre-optimisation and,
as shown above, not even stable. The mechanism that actually fixed this at
`N_OC=16` was Vivado's own *implementation*-phase replication — the routed design
contains `first_ic_reg_rep__0` — combined with the `dw_fused_timing.xdc`
multicycle on quasi-static config registers.

So `first_ic` is **an unquantified risk, not a proven blocker**. The correct
action is to build and read WNS, with these levers held in reserve if it fails:
1. extend the XDC multicycle from `reg_zp_relu_reg[*]` to `reg_cin_run`,
   `reg_n_groups`, `reg_img_width`, `reg_n_rows` (free margin, §19 path 3);
2. `-directive` options that raise implementation-phase replication effort;
3. restructure so the accumulator clear is not a broadcast control at all — e.g.
   let the first input channel *write* rather than accumulate, removing `first_ic`
   from the DSP control path entirely. This is the only real fix if the others
   fall short, and it is a datapath change, not an attribute.

### 8.5 AS-BUILT — configuration #4, 2026-08-08 ✅ TIMING CLOSED

> **Final build of the day** (three builds, converged): WNS **+0.119 ns**,
> WHS **+0.014 ns**, LUT 28,509 / 53,200 (53.6%), FF 20,195 (19.0%),
> BRAM 73.5 / 140 (52.5%), DSP 220/220. All constraints met, zero failing
> endpoints. Contains `N_OC=32`, partial-batch drain, v3, Option C, DW
> `USE_DSP(0)`, the **P0** over-consume fix and the `ppu.sv` declaration
> initialisers. The last of those is bit-identical in timing and area to the
> build without it, confirming `INIT` attributes cost nothing.
>
> **Hold slack improved** +0.006 → +0.014 ns when the PW shell changed, so the
> 6 ps risk flagged on the first build is resolved.
>
> ⚠️ The board run of 2026-08-08 **FAILED** (pair 3 MM2S timeout) on the
> *pre-P0/P1* bitstream. That failure is diagnosed and fixed; see
> `PAPER_HW_EVIDENCE.md` §21 and §29.2. No throughput number from that run is
> usable.

`vivado_scripts/S_build_noc32.tcl`. Post-route, `impl_1`:

| | predicted | **measured** | |
|---|---|---|---|
| WNS | > 0 | **+0.126 ns** | was +0.321 at `N_OC=16`; 0 failing endpoints, TNS 0.000 |
| WHS | — | **+0.006 ns** | was +0.012 — **see risk below** |
| DSP | 162 floor | **220 / 220** | see breakdown |
| LUT | ~32,400 | **28,500 / 53,200 (53.6%)** | implementation beat the estimate |
| BRAM | unchanged | **73.5 / 140 (52.5%)** | **prediction correct** — invariant in `N_OC` |

"All user specified timing constraints are met."

DSP by function: `PW_MUL 128 + PPU 32 + PW_ACC 58 + shell 2 = 220`. **`DW_MAC` is
absent**, confirming the MACs went to fabric. The 162 figure was the hard *floor*,
not a prediction of the total: Vivado filled the remaining 58 with accumulators
because they were free (§3.3's opportunistic mapping, again). **The design is not
DSP-constrained** — those 58 are reclaimable at 36.6 LUT/DSP if ever needed.

Note the BRAM metric: the build script's raw primitive count reads 93
(54 RAMB36E1 + 39 RAMB18E1), which is **73.5 tiles** — identical to the
`N_OC=16` build. The mix shifted toward RAMB18 as the weight memory went from 16
to 32 banks, but total tiles did not move. Quote tiles, not primitives.

**`first_ic` was not the blocker.** Timing closes with the grid doubled from 64 to
128 DSPs. The retraction in §8.3 was correct and none of its levers were needed.

> ### ⚠️ RISK — HOLD SLACK IS 6 PICOSECONDS
> WHS fell +0.012 → **+0.006 ns**. Hold slack is **period-independent**: it cannot
> be recovered by slowing the clock, and it is the one margin that raising the
> clock neither helps nor hurts. It is MET with 0 failing endpoints and 0.000 ns
> total violation, so **this bitstream is valid**. But 6 ps is inside the noise of
> a re-place-and-route. Consequences:
> - do not re-run implementation casually and expect the same result;
> - re-check WHS after **any** future PW change, not just timing-focused ones;
> - the tightest hold path at `N_OC=16` was the PPU rounding→clamp stage (§19);
>   confirm whether that is still the binding one before touching the PPU.

### 8.4 Remaining

Everything in §6 is simulation plus synthesis estimates. One bitstream and one
board run converts it into a result.

**Revised build order** (§8.3 removed the prerequisite it had introduced):

1. `N_OC=32` + DW `USE_DSP(0)` in `R_regen_bd_and_build.tcl`.
2. Netlist check per §27 — confirm the change reached the **generated** IP copy.
3. Read **WNS before programming anything.** Current is +0.321 ns at 64 grid
   DSPs. If it stays positive at 128, `first_ic` was never the problem and §8.3's
   levers stay unused. If it goes negative, apply them in the order listed there
   — that is the point at which the fanout question becomes real and answerable.
4. Fresh bitstream, programmed explicitly.
5. Warm per-pair run against predicted 26 / 55 / 71 / 71 / 109 / 173 cyc/group
   and ~20.76 ms HW.

The RTL is at the verified state: v3 + Option C + partial-batch drain, cascade
PASS on four shapes, `src/` and `sources_1/new/` in sync, `first_ic` replication
reverted. Backups: `backup_pre_optionC/pw_pixel_major_core.sv.pre_firstic_rep_2026-08-08`.
