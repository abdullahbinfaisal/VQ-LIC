# Hardware-Aware Channel-Schedule Sensitivity

**Draft 5 — 2026-08-16.** Supersedes drafts 1–4; see §0.

**RD DATA NOW EXISTS (§4.6).** `8-32-64-64-64-64` measured **worse** for
compression than the deployed `16-32-32-32-64-64`. This supplies the constraint
whose absence made the optimisation ill-posed, and it **reverses the draft-4
recommendation**. Revised candidate: **`16-32-64-64-64-64`** — early capacity
bit-identical to deployed, +56% arithmetic, +11.7 points utilisation (§4.6.3).

The document supports a **hardware-efficiency sensitivity analysis** plus **one
RD comparison**. It still does not support a Pareto-optimality claim, and the RD
experiment is **confounded** until the §4.6.1 ablations are run.

> **SUPERSEDED 2026-09-02 — retained verbatim for provenance.** The paragraph
> immediately below was written when Route A had been built and timing-closed but
> had never been run on the board. That was true at the time. It is no longer
> true, and it must not be cited. See "Route A silicon validation" beneath it.

**Authoritative cost model: FULL drain** (`cyc/grp = B·cin + Q_last + 6B + 1`),
which is the law matching the 22.03 ms board measurement. Route A (half drain) is
**built and timing-closed but not board-measured**; results under it are labelled
throughout and must not be quoted as silicon-validated.

### Route A silicon validation (2026-09-02) — the note above is now stale

Route A has since been board-measured. The realized bitstream implements the
**half-drain** shadow transfer (`R_sh = 2` output channels/cycle, so the final
batch drains in `⌈Q_last/2⌉` cycles), and silicon agrees with it, not with the
full-drain law. Measured cycles/group on **PW-bound blocks only** (the blocks
where the PW law actually sets the time; DW-bound blocks cannot arbitrate it):

| block | measured cyc/grp | half drain (Route A) | err | full drain (old) | err |
|---|---|---|---|---|---|
| `16-48-64` b1 (cin=3, cout=16)   |  18.05 |  18 | **+0.3%** |  26 | −30.6% |
| 6-block b4 (cin=32, cout=32)     |  55.97 |  54 | **+3.7%** |  71 | −21.2% |
| 6-block b5 (cin=32, cout=64)     |  93.97 |  92 | **+2.1%** | 109 | −13.8% |
| 6-block b6 (cin=64, cout=64)     | 158.44 | 156 | **+1.6%** | 173 | −8.4% |

Full drain over-predicts by 8–31%. The 22.03 ms board measurement that motivated
the full-drain note **predates both the Route A build and the MM2S burst-size fix**
(`c_mm2s_burst_size` 8 → 256), so it cannot arbitrate the current hardware and is
not evidence against Route A. It is left in the history below as measured.

**Consequence for draft 3.** The draft-3 error recorded in §0 was diagnosed as
"used the Route A half-drain term against full-drain silicon". The *arithmetic*
error stands — draft 3 mixed a half-drain law with a calibration constant derived
from a full-drain measurement. The *attribution* was half wrong: the law draft 3
used was the correct one for the hardware as now built; the calibration constant
was the stale half. Neither the 1.2527 nor the 1.0615 calibration should be used,
because the validated model is uncalibrated — no empirical correction factor is
applied at all.

**DW row count.** Two DW expressions circulate. The superseded form
`T_DW = H·(c_in(G+1)+4)` gives **13.4136 ms** for `16-48-64`; the final RTL-derived
form `T_DW = (H+1)·(c_in(G+1)+4)` gives **13.4463 ms**. The `(H+1)` form is
correct — the windower runs one extra vertical-flush row pass beyond the `H` real
rows (`r_cnt` advances to `H_r` inclusive) — and it is also the better predictor
on the two DW-bound blocks of `16-48-64` (+0.21% / +0.30%, versus +0.49% / +0.85%).
**Quote 13.4463 ms, not 13.4136 ms.**

### Hardware feasibility constraint (2026-09-02)

`dw_banked_window_8x` allocates `line0`/`line1` at flat depth
`MAX_CG_PRODUCT = 2048` (RTL default; not overridden in `hw.bd`). `slot_idx`
resets at every row boundary and increments once per real-group beat, so a row
consumes exactly `G_j · c_in,j` slots. Overflow **wraps and aliases silently —
wrong data, no error**. The search formulation must therefore carry

>   `G_j · c_in,j ≤ 2048`  for every block j

which, with the realized 3×stride-2 front end (`G` = 160, 80, 40, then 20),
reduces to a per-position cap on the channel schedule itself:

>   `c_out,1 ≤ 25`,  `c_out,2 ≤ 51`,  `c_out,j ≤ 102` for `j ≥ 3`

`16-64-64-64` violates this at block 3 (`c_out,2 = 64 > 51`, i.e.
`40 × 64 = 2560 > 2048`) and is **not hardware-feasible on the realized
accelerator**. It must not be treated as an ordinary feasible NAS point. It is
the only violating topology among every candidate recorded in this repository
(see `results/hw_feasibility_audit.csv`).

---

## 0. Revision history

| draft | claim | status |
|---|---|---|
| 1 | "the schedule is near-optimal for this hardware" | withdrawn — no minimisation performed |
| 2 | "the question is unanswerable without RD" | wrong objective — utilisation is a ratio, non-degenerate |
| 3 | "strictly dominated"; "marginal rate halves at the `C2=32` crossover" | **both withdrawn — see below** |
| 4 | "`8-32-64-64-64-64` is the capacity-maximal choice" | **superseded by RD measurement** — it is worse for compression. §4.6 |
| 4 | "Σ`cin_j` ≤ 240 residency constraint" | **retracted** — RAMs are reloaded per layer, no layer base. §4.1 |

### What draft 3 got wrong

1. **Enumerated with the wrong cycle law.** Draft 3 used the Route A half-drain
   term `⌈Q_last/2⌉` while calibrating against a board number measured on
   full-drain hardware. Calibration was therefore 1.2527 when it should have been
   **1.0615**. Every frame-time projection and fps figure in draft 3 is void.
2. **The crossover claim does not survive the authoritative model.** At `C2=32`
   with `C1=8`, full drain gives b1 read = write = 230,400 cyc but **PW = 338,400**
   — b1 is *PW-bound*, so the read/write crossover is not the binding transition.
   Under Route A, PW = 223,200 and the claim holds. **It is conditional on a
   measurement that has not been made.**
3. **"Strictly dominated" ignored two axes** — weight count (+61.4%) and
   coefficient-programming time. See §4.
4. **Block-0 saving stated as 1.73 ms.** Wrong derivation: it used the *write*
   (230,400 cyc) as the floor and ignored the RGB *read* (345,600 cyc). Under the
   built RTL the saving is **0.576 ms**.
5. **Ordinal ranks ("rank 786") were tie-sensitive.** Report the count strictly
   better instead.
6. **Called the quantity "arithmetic intensity".** It is not — it divides by
   `max(read, write)`, not total bytes. Renamed **directional service intensity**.
7. **Claimed outputs were never validated.** Obsolete — outputs have since been
   validated. (B3/B4 remain untimed, a separate limitation.)

---

## 1. Directional service intensity — the robust analytical result

This section is **model-independent**: it uses only transfer volumes, so it holds
under either drain law.

With reads and writes served on overlapping directions, define

$$I_{\mathrm{svc}} = \frac{c_{in}c_{out}P_{out}}{\max(c_{in}P_{in},\; c_{out}P_{out})}$$

> **Note on naming.** This is *not* ordinary arithmetic intensity (ops ÷ total
> bytes). It divides by the *larger* of the two directional transfers, modelling
> overlapped service. Call it **directional service intensity** in the paper to
> avoid collision with the existing usage.

Closed forms:

| stride | $I_{\mathrm{svc}}$ |
|---|---|
| 1 | $\min(c_{in},\,c_{out})$ |
| 2 | $c_{in}c_{out}/\max(4c_{in},\,c_{out})$ |

and in both cases

$$\boxed{I_{\mathrm{svc}} \le c_{in}}$$

### 1.1 The saturation condition — necessary, NOT sufficient

⚠️ Draft 3 said "a block saturates iff `cin ≥ 32`". That is wrong in the "if"
direction. The correct statement:

> A block can reach the compute–communication ridge only if $c_{in} \ge Q$, **and**
> only when $c_{out}$ is large enough — at stride $s$, additionally
> $c_{out} \ge s^2 c_{in}$.

Counterexample from the deployed schedule itself: **b2 has `cin=32 ≥ Q` yet
$I_{\mathrm{svc}} = 8$**, because `cout=32` and it needs 128 at stride 2.

And reaching the ridge still does not give 100% MAC-array utilisation — batch,
drain and group-control overheads remain.

| blk | `cin` | `cout` | $s$ | $I_{\mathrm{svc}}$ | ceiling ($=c_{in}$) |
|---|---|---|---|---|---|
| b0 | 3 | 16 | 2 | 3 | **3** — structural, RGB |
| b1 | 16 | 32 | 2 | 8 | **16** |
| b2 | 32 | 32 | 2 | **8** | 32 — *needs `cout=128`* |
| b3 | 32 | 32 | 1 | 32 | 32 |
| b5 | 64 | 64 | 1 | 64 | 64 |

## 2. The crossover rule — robust, and the cleanest exportable result

$$c_{in}s^2 P_{out} = c_{out}P_{out} \;\Longrightarrow\; \boxed{c_{out}^{\mathrm{cross}} = s^2 c_{in}}$$

- below it, input reads dominate output writes;
- at it, the two directional transfers balance;
- above it, widening the output directly increases communication time.

**This is general, exact, and independent of the drain model.** It is more
valuable to the paper than any of the schedule enumeration.

⚠️ **But whether the crossover is the *binding* constraint depends on the drain
model.** Under full drain, b1 at `C2=32` is PW-bound before the crossover binds.
Do not build a claim on the crossover being the frontier's slope break until
Route A is board-measured (Task #18).

## 3. Cost structure of the deployed schedule — model-independent facts

Every inter-block tensor is written once and read once, so a channel is paid
twice. Marginal cost per channel at 8 B/cycle:

| tensor | resolution | cost/channel |
|---|---|---|
| **C1** (b0→b1) | 360×640 | **0.576 ms** |
| C2 (b1→b2) | 180×320 | 0.072 ms |
| C5 (b4→b5) | 90×160 | 0.036 ms |

**C1 is 16× the price of a late channel**, and blocks 0–1 — which carry it — have
$I_{\mathrm{svc}}$ ceilings of 3 and 16 and so can never reach the ridge. Early
capacity is the most expensive and the least effective. Under the full-drain
model blocks 0–1 consume **58%** of modelled frame time.

### 3.1 `C3: 32 → 64` — the cleanest schedule-level result

b2 stays read-bound after the widening, under **both** drain laws:

| `C3` | b2 read | b2 write | b2 PW (full drain) | b2 time |
|---|---|---|---|---|
| 32 | 230,400 | 57,600 | 127,800 | **2.304 ms** |
| 64 | 230,400 | 115,200 | 196,200 | **2.304 ms** |

**Latency-neutral, +14.75 M MACs.** This survives every modelling choice in this
document and is the safest thing here to act on.

## 4. The candidate schedule — and why "dominated" was too strong

`8-32-64-64-64-64`, under the **authoritative full-drain** model:

| | current `16-32-32-32-64-64` | candidate `8-32-64-64-64-64` |
|---|---|---|
| modelled B1 time | 20.754 ms | **20.214 ms** (−0.540) |
| PW MACs | 158.5 M | **226.7 M** (+43.0%) |
| PW utilisation | 29.84% | **43.81%** |
| activation traffic | 18.432 MB | **16.589 MB** (−10.0%) |
| **conv weights** | **10,363** | **16,731 (+61.4%)** ⚠️ |

### The programming counter-axis — REAL as implemented, fixable in ~4 lines of RTL

**Fact-checked against `main.c` 2026-08-16: coefficients are NOT amortised.**
`hw_dw_pw_cascade_l0_l1` is called inside the per-frame loop (`main.c:5420`) and
unconditionally reprograms DW weights+params (`:3347`), PW params (`:3365`) and PW
weights (`:3376`) on every call. Counting AXI-Lite writes —
DW `C×6`, PW params `Cout×4`, PW weights `Cout×(2+Cin)`:

| schedule | writes/frame | cost |
|---|---|---|
| current | 11,266 | **2.44 ms** (⇒ 217 ns/write) |
| candidate | 17,802 | **3.86 ms (+1.42 ms)** |

So **as the code stands the candidate is net +0.88 ms SLOWER** (+1.42 programming
vs −0.54 compute). The objection is real.

**Both weight memories have the CAPACITY for all six layers, but the RTL addressing does not currently exploit it** (see §4.1 — both RAMs are indexed by channel-within-layer with no layer base, so layers alias). Capacity, if a layer base were added:

| | capacity | current | candidate |
|---|---|---|---|
| PW `W_DEPTH=(240/32)×240` | 1,680 | 275 (16%) | 459 (27%) |
| DW `wram[0:CIN_MAX-1]` | 240 | 179 | 235 |

The only blocker is that the PW inference read base `w_addr_base` is hardcoded to
`'0` at layer start (`pw_pixel_major_core.sv` :507, :595, :629, :811). The *write*
path already has a runtime offset (`PW_REG_W_BRAM_OFF`); the *read* path has no
layer base. Add `PW_REG_W_LAYER_BASE`, change those four assignments, do the same
for DW's `win_ch` index, and program all six layers once at boot.

> **Payoff (requires the layer-base RTL change, §4.1): 2.44 ms → microseconds — 8.3% of the frame, 33.8 → ~36.9 fps, with no
> schedule change and no retrain.** Larger than anything the channel schedule
> buys, and it removes the candidate's penalty entirely. **Task #19.**

### 4.1 ⚠️ RETRACTED — the "second independent reason" was not justified by the RTL

An earlier draft claimed weight residency imposes **Σ`cin_j` ≤ 240**, and that this
independently ruled out `8-48-64-64-64-64` (Σ = 251). **Both claims are withdrawn.**

**The DW weight RAM is reloaded per layer, not shared across layers.**
`dw_fused_core.sv:109-113`:

```systemverilog
(* ram_style = "block" *) logic [9*DATA_WIDTH-1:0] wram [0:CIN_MAX-1];
if (w_wr_en) wram[w_wr_ch[PB_AW-1:0]] <= w_wr_data;   // write: channel WITHIN layer
wram_q <= wram[win_ch[PB_AW-1:0]];                     // read:  channel WITHIN layer
```

`w_wr_ch` comes from `DWF_REG_CH_ADDR`, written as `c = 0…C−1` per layer
(`main.c:3355`); `win_ch` is the windower's `c_cnt`, also `0…C_r−1`. **Neither port
carries a layer base**, so layer *N*'s channel *c* occupies the same address as
layer *N−1*'s channel *c*. The RAM holds exactly one layer at a time.

**The constraint the current RTL actually imposes is therefore**

$$\max_j c_{\mathrm{in},j} \le \mathrm{CIN\_MAX} = 240$$

which is **vacuous over the search alphabet** (max element 128). It rules nothing
out, and in particular does **not** rule out `8-48-64-64-64-64`.

**The search never imposed either form.** No search script contains the constant
240 or any residency test — the constraint existed only in this document's prose.

The Σ form becomes correct **only** for the hoisted design (Task #19), and only
*after* the layer-base RTL change that hoist requires. It is forward-looking
design guidance, not a property of the built system, and it must not be cited as
corroborating the schedule choice. §4.2 stands on its own.

### 4.1a The residency limits that ARE justified by the current RTL

| resource | RTL | constraint | binds the search? |
|---|---|---|---|
| DW weight RAM | `wram[0:CIN_MAX-1]`, `dw_fused_core.sv:109` | `max_j c_in,j ≤ 240` | no (alphabet ≤ 128) |
| DW param RAM | `pram[0:CIN_MAX-1]`, `:195` | `max_j c_in,j ≤ 240` | no |
| PW weight BRAM | `w_addr_base` walks `0, c_in, …, (B−1)c_in`; `W_DEPTH=(COUT_MAX/N_OC)·CIN_MAX=1680` | `B_j·c_in,j ≤ 1680` | no (worst case 4×128 = 512) |
| PW param BRAM | `param_rd_addr ≤ c_out−1` | `c_out,j ≤ COUT_MAX = 240` | no |

> 🐛 **Latent overflow, outside the search space but worth fixing.** `W_DEPTH`
> uses integer division: `(240/32)·240 = 7·240 = 1680`. But `B = ⌈240/32⌉ = 8`, so
> a layer with `c_out = 240, c_in = 240` addresses up to `8·240−1 = 1919 > 1679`
> and wraps silently. Safe for every candidate here (`c_out ≤ 128 ⇒ B ≤ 4`), but
> `W_DEPTH` should use a ceiling.

### 4.2 ✅ Why this candidate — the robust justification

The draft-3 justification (*"A sits at block 1's read/write crossover, where the
marginal exchange rate halves"*) is **withdrawn** — it holds only under the
unmeasured Route A law (§0). The defensible reason is simpler and survives both
models:

> **`8-32-64-64-64-64` is the capacity-maximal monotone schedule that does not
> increase frame time relative to the deployed one.**

| model | current | candidate | Δ time | Δ MACs |
|---|---|---|---|---|
| full drain (authoritative) | 20.75 ms · 29.84% | **20.21 ms · 43.81%** | −0.54 ms | +43.0% |
| Route A (built, unmeasured) | 17.59 ms · 35.21% | **17.12 ms · 51.74%** | −0.47 ms | +43.0% |

**The same schedule is selected under both laws** — the next frontier point up,
`8-48-64-64-64-64`, exceeds the current frame time in both (21.44 / 19.42 ms). The
choice is therefore robust to the drain ambiguity that voids the rest of draft 3,
and does not depend on Task #18.

Neighbour worth knowing: under full drain A is *not* the utilisation peak —
`8-24-64-64-64-64` reaches 44.19% at 19.06 ms with 215.7 M MACs. A buys 11 M more
MACs for 1.15 ms and 0.4 points. Under Route A, A *is* the local peak (51.74%).

### 4.3 Counts strictly better (tie-safe), replacing ordinal ranks

At the current schedule's own time budget (20.75 ms, full drain, `C6=64`):

- feasible schedules: **3,635**
- **strictly better utilisation: 1,010**
- best at that budget: `8-16-64-96-64-64`, 49.91%, 263.6 M MACs

Report the count strictly better. Ordinal rank depends on arbitrary tie placement.

### 4.4 The situation AFTER the hoist (Task #19)

Hoisting weight programming out of the frame loop changes the ranking, because it
removes the only axis on which the candidate loses. Measured host budget:
frame 29.552 ms = HW 22.030 + host 7.522, of which **programming is 2.440 ms**.
Post-hoist the host term falls to **5.082 ms**. Calibration = 22.030 / 20.754 =
**1.0615** (measured board HW ÷ full-drain model).

| configuration | HW (ms) | frame (ms) | **fps** |
|---|---|---|---|
| current `16-32-32-32-64-64`, pre-hoist *(today)* | 22.030 | 29.552 | **33.84** |
| current, **post-hoist** | 22.030 | 27.112 | **36.88** |
| candidate `8-32-64-64-64-64`, pre-hoist | 21.457 | 30.394 | **32.90** ❌ |
| candidate, **post-hoist** | 21.457 | 26.539 | **37.68** ✅ |

Two things this table settles:

1. **Pre-hoist the candidate is a regression** — 32.90 fps against 33.84. Its
   −0.573 ms of HW is swamped by +1.416 ms of extra programming (17,802 vs 11,266
   AXI-Lite writes). Building it before Task #19 would make the system *slower*.
2. **The hoist alone is worth more than the schedule change.** +3.04 fps
   (33.84 → 36.88) with no model change, no retrain and no RD risk, versus
   +0.80 fps for the schedule on top of it.

**Ordering is therefore forced: Task #19 first, candidate second.**

Conditional on Task #18 confirming Route A, both improve again:

| configuration | HW (ms) | frame (ms) | fps |
|---|---|---|---|
| current, post-hoist + Route A | 18.667 | 23.749 | 42.11 |
| candidate, post-hoist + Route A | 18.170 | 23.252 | 43.01 |

⚠️ These two rows are **projections through an unmeasured drain model** and must
not be quoted as results.

### 4.5 Hardware configuration — confirmed `N_LANES = 8`, `N_OC = 32`

Verified 2026-08-16 against four independent sources:

| source | value |
|---|---|
| `sources_1/bd/hw/hw.bd` instance params | `N_LANES: 8`, `N_OC: 32` |
| `Final_code_2/src/main.c:95` | `PW_N_OC 32` |
| `S_build_noc32.tcl` assertion | `N_OC 32 -> 32 on pw_single_oc_axis_axi_0` |
| synthesis DSP ledger | `PW_MUL 128` = `N_OC × N_LANES / 2` ✓ |

The `N_OC = 5` and `N_LANES = 16` literals in `pw_pixel_major_core.sv` and
`pw_single_oc_axis.sv` are **unused module defaults** — the BD overrides both. Do
not read them as the built configuration.

This is the configuration every number in this document assumes: peak
`N_OC × N_LANES = 256` MAC/cycle, and the ridge at 32 MAC/byte.

## 4.6 ⚠️ RD MEASUREMENT — the candidate is WORSE for compression

**Measured: `8-32-64-64-64-64` compresses worse than `16-32-32-32-64-64`.**

This is the outcome §6 warned about — *"MAC count is not capacity and capacity is
not quality; a narrower first stage could damage information preservation
regardless of later width."* It is also the **most valuable data point in the
project**, because it supplies the RD constraint whose absence made the
optimisation ill-posed (§0 draft 2).

### 4.6.1 The experiment is confounded — resolve before building on it

`8-32-64-64-64-64` changed **three** variables at once: `C1` 16→8, `C3` 32→64,
`C4` 32→64. Attributing the regression to `C1` is a hypothesis, not a
measurement. Two ablations settle it:

| run | Δ latency | Δ MACs | tests |
|---|---|---|---|
| **`8-32-32-32-64-64`** | **−3.672 ms** | −20.3 M | `C1` alone — RD drop here confirms the hypothesis |
| **`16-32-64-32-64-64`** | +0.576 ms | +29.5 M | `C3` alone — RD held here means late widening is RD-free |
| `16-32-32-64-64-64` | +1.836 ms | +44.2 M | `C4` alone (optional third point) |

The first is **faster than the deployed schedule**, so it is a low-risk run.

### 4.6.2 The co-design result: hardware and RD oppose on the same variable

| | says about early channels (`C1`, `C2`) |
|---|---|
| **Hardware** | cost 16× a late channel; blocks 0–1 have `I_svc ≤ c_in` = 3 and 16, so they can **never** reach the ridge |
| **RD** | they carry the information — what b0/b1 discard, b3–b5 cannot reconstruct (data-processing inequality) |

**This opposition is the paper's finding.** The accelerator's cheapest capacity
is where the codec needs it least; the codec's most valuable capacity is where
the accelerator is least efficient. Note it **inverts** the naive reading of §1–3:
the service model alone says *narrow the front*; the correct co-design answer is
**freeze the front, widen the back**.

### 4.6.3 The regions are separable — unbundle them

| region | RD | hardware | action |
|---|---|---|---|
| `C1, C2` | **critical** | expensive, below ridge | **freeze at RD-determined value** |
| `C3, C4, C5` | cheap | b2 read-bound ⇒ output width *unpriced*; b3–b5 above ridge | **maximise** |

The candidate failed because it bundled a late-path gain with an early-path
sacrifice.

| schedule | T | MACs | util | early cap | frame +hoist | |
|---|---|---|---|---|---|---|
| `16-32-32-32-64-64` | 17.669 | 158.5 M | 35.04% | 40.6 M | 22.751 | deployed |
| **`16-32-64-64-64-64`** | 20.657 | **247.0 M** | **46.71%** | **40.6 M** | 25.739 | **early capacity IDENTICAL** |
| `12-32-64-64-64-64` | 18.626 | 236.9 M | 49.67% | 30.4 M (75%) | 23.708 | RD probe |
| `8-32-64-64-64-64` | 16.985 | 226.7 M | 52.14% | 20.3 M (50%) | 22.067 | **measured worse RD** |

> ### ✅ Revised recommendation: `16-32-64-64-64-64`
> `C1` and `C2` are **unchanged from the deployed schedule**, so early capacity is
> bit-identical at 40.6 M — under the measured hypothesis its RD should hold. It
> buys **+56% arithmetic and +11.7 points utilisation** for +2.99 ms, which the
> Task #19 hoist (2.44 ms) very nearly cancels → **net +0.55 ms**.

### 4.6.4 Formulation to publish

$$\max_{\mathbf{C}\in\mathcal{F}} U_{\mathrm{PW}}(\mathbf{C})
\quad\text{s.t.}\quad T_{\mathrm{svc}}(\mathbf{C})\le T_{\mathrm{budget}},\quad
E(\mathbf{C})\ge E_{\min},\qquad E(\mathbf{C})=\sum_{j\le 2}P_jC_{j-1}C_j$$

RTL-exact cost model, **empirical** RD constraint, falsifiable proxy resting on a
physical argument (DPI). `E_min` is calibrated by the ablation ladder — report `E`
as a **proxy**, never as "capacity = quality".

## 4.7 Are the candidates well matched to the engine? — and is `C1=12` awkward?

### 4.7.1 `C1=12` is not awkward; it is the most hardware-natural value available

Every quantisation the RTL imposes was checked. **None applies to the channel
dimension:**

| check | at `C1=12` |
|---|---|
| PW batching | `B=⌈12/32⌉=1`, `Q_last=12` — partial-batch drain (`:583`, `:715`) |
| shadow copy | `copy_pairs=⌈12/2⌉=6`; 12 is even so ragged-pair suppression never fires |
| runtime regs | `cin_run`/`cout_run` are 12-bit ports — no power-of-2 requirement |
| data layout | one beat = 1 channel × 8 pixels; the channel dim is **not** lane-quantised |
| DMA alignment | 12 × 230,400 = 2,764,800 B ⇒ 345,600 beats, integer |
| DW windower | iterates `c_cnt = 0…C−1`; no constraint on `C` |
| PPU stall regime | `c_in<28` matters only for `B≥2`; b1 has `B=1` |

Better: **12 is exactly b0's read/write balance point**, `c_out = s²·c_in = 4×3`:

| `C1` | b0 read | b0 write | |
|---|---|---|---|
| 8 | 345,600 | 230,400 | read-bound, write bandwidth idle |
| **12** | 345,600 | **345,600** | **balanced exactly** |
| 16 | 345,600 | 460,800 | write-bound — paying for bandwidth |

⚠️ **But that balance is not operative today.** b0 is *PW-overhead*-bound
(432,000 cyc vs a 345,600 DMA floor), so the DMA balance does not bind. **After
Task #16 removes b0's overhead it does bind**, and `C1=16` then costs
**+1.152 ms** over `C1=12` where today it costs 0.864 ms. The penalty for sitting
past the balance point *grows* once block 0 is fixed.

### 4.7.2 Per-block engine match

`16-32-64-64-64-64`:

| blk | `cin/cout` | `s` | `I_svc` | ceiling | vs balance `s²c_in` | verdict |
|---|---|---|---|---|---|---|
| b0 | 3/16 | 2 | 3 | 3 | `cout>12` write-bound | cannot reach ridge — structural (RGB) |
| b1 | 16/32 | 2 | 8 | 16 | `cout<64` read-bound | cannot reach ridge — `c_in<Q` |
| b2 | 32/64 | 2 | 16 | 32 | `cout<128` read-bound | **output width free up to 128** |
| b3 | 64/64 | 1 | 64 | 64 | **balanced** | **2× above ridge** |
| b4 | 64/64 | 1 | 64 | 64 | **balanced** | **2× above ridge** |
| b5 | 64/64 | 1 | 64 | 64 | **balanced** | **2× above ridge** |

**No block is in the PPU stall regime** (`Δ=max(c_in+6, Q+2)`; every `B≥2` block
has `c_in=64 ≥ 28`). The three late blocks sit *exactly* at the stride-1 balance
point and well above the ridge — this is as well-matched as the engine gets. The
two starved blocks are held at their RD-determined widths, not chosen for the
hardware. `12-32-64-64-64-64` is identical except b0 moves to *balanced*.

## 5. Block 0 — corrected

b0 is PW-bound while running the engine at 5–8%. But the floor is the **RGB
read**, not the write:

```
read  = 3 x 921,600 / 8 = 345,600 cyc = 3.456 ms   <-- FIXED, no schedule touches it
```

| | b0 PW | DMA floor | **saving if overhead removed** |
|---|---|---|---|
| current `C1=16`, Route A (built) | 518,400 | 460,800 | **0.576 ms** |
| current `C1=16`, full drain | 748,800 | 460,800 | 2.880 ms |
| candidate `C1=8`, Route A (built) | 403,200 | 345,600 | **0.576 ms** |

**On the built RTL the saving is 0.576 ms, not 1.73 ms.** Draft 3's figure came
from using the write as the floor while taking PW from the other drain model —
two errors. **Do not put 1.73 ms in the paper.**

## 6. Limits

- **Utilisation rewards doing more arithmetic**, not useful arithmetic. It answers
  *"which schedule keeps the hardware busier?"*, not *"which is best for the
  codec?"* RD remains necessary for architecture selection — the draft-2
  objection was only *partially* wrong.
- MAC count is not capacity and capacity is not quality. A narrower first stage
  could damage information preservation regardless of later width.
- The frontier holds strides, block count and latent width fixed.
- Route A is unmeasured; §7 numbers move if it is confirmed.

## 7. Paper framing

Add a subsection: **"Hardware-Aware Channel-Schedule Sensitivity"**

1. Derive the directional service-intensity ceiling $I_{\mathrm{svc}} \le c_{in}$.
2. Derive $c_{out}^{\mathrm{cross}} = s^2 c_{in}$.
3. Diagnose why early stages are inefficient (ceilings of 3 and 16; 58% of frame).
4. Show `C3: 32→64` uses otherwise-idle service capacity.
5. Present `8-32-64-64-64-64` as an **untrained candidate**, with weight count shown.
6. State that RD evaluation is required before changing the deployed model.

A latency-vs-PW-MAC plot is admissible **only** if labelled *"modelled
latency–PW-work frontier at fixed latent width"*, stating: fixed `C6=64`; fixed
strides and block count; the channel alphabet; the monotonicity restriction; the
exact drain model; model-only status; and **parameter count as marker size** — so
the candidate's +61.4% weights are visible.

### ❌ Do not write

| forbidden | why |
|---|---|
| "strictly dominated" | ignores weights and programming time |
| "this is not a rate–distortion trade" | it may be |
| "the selected schedule is capacity optimal" | no such result |
| "rate–distortion Pareto" / "model-quality frontier" | quality is on neither axis |
| "the best point" / "the knee" | every frontier point is optimal for some preference |
| "HW–SW co-optimisation" | means jointly searching RD *and* latency — not done. Say **hardware-aware architecture selection** |
| the 1.73 ms block-0 saving | wrong derivation; it is 0.576 ms |
| "rank 5,174 / 786" | tie-sensitive; use counts strictly better |
| "no RD data is needed" | needed for *selection*, not for occupancy |
| "measurement costs nothing" | see §8 |
| "outputs were never validated" | obsolete — they have been |
| "selected schedule" | it is a **candidate** until trained and evaluated |

## 8. Next steps — revised by the RD result

**Priority 0 — resolve the confound (§4.6.1).** Two training runs. Nothing built
on the RD result is safe until these land.
  1. `8-32-32-32-64-64` — `C1` alone. **3.672 ms faster** than deployed, so
     low-risk. RD drop here confirms `C1` is the cause.
  2. `16-32-64-32-64-64` — `C3` alone. +0.576 ms. RD held here means the
     late-path widening is RD-free, which is the whole basis of §4.6.3.
  3. *(optional)* `C1 ∈ {8,12,16}` at fixed tail, to calibrate `E_min` rather than
     assume it. `C1=12` is b0's exact read/write balance (§4.7.1).

**Priority 1 — hoist weight programming out of the frame loop** (Task #19).
+3.04 fps, no model change, no retrain, no RD risk. Independent of everything
above, and a prerequisite for any wider schedule (§4.4).

**Priority 2 — adopt `16-32-64-64-64-64`** *if and only if* step 0.2 shows the
late widening is RD-neutral. +56% arithmetic, +11.7 points utilisation, net
+0.55 ms after the hoist, early capacity untouched.

**Priority 3 — block-0 overhead** (Task #16). 0.576 ms on the built RTL. Note it
**changes the `C1` economics**: once b0 is DMA-bound rather than overhead-bound,
sitting past the balance point costs +1.152 ms instead of 0.864 (§4.7.1).

**Priority 4 — board-measure Route A** (Task #18). Gates the drain law and the
calibration. Does not gate the schedule choice.

**Standing:** `C3: 32→64` is latency-neutral at b2 under both drain laws (§3.1)
and is contained in the recommended schedule.

> ⚠️ **Residency constraint that Task #19 would INTRODUCE.** The current RTL
> reloads both weight RAMs per layer (§4.1), so today the only bound is
> `max_j cin_j ≤ 240`, vacuous over the alphabet. Keeping all six layers resident
> — which the hoist requires, and which needs the layer-base RTL change — would
> newly impose **Σ`cin_j` ≤ 240**. `16-32-64-64-64-64` sums to **3+16+32+64+64+64
> = 243 > 240**, so it would **not fit**; `12-32-64-64-64-64` sums to 239 and
> would. This is a consequence of the proposed change, not a property of the built
> system, and must never be cited as corroborating a schedule choice.

---

## 2026-09-03 — the frozen model still describes the OLD bitstream

The service model frozen on 2026-09-02 (Route A / half-drain, R_sh = 2, T_DW
on the (H+1) vertical-flush-row formulation) was calibrated and silicon-validated
against the bitstream as it stood on that date. On 2026-09-03 the first data
check this datapath has ever had (`tb_pw_axis_conv.sv`) found five defects in the
PW compute path, and fixing them changed the schedule slightly. Both facts are
recorded here; neither supersedes the other.

**The OOS silicon measurements remain valid as measurements.** They record how
long the accelerator takes to move a given number of beats. That is independent
of whether the beats are numerically correct, and the five defects did not change
any beat count — every shape emitted exactly the expected number of beats before
the fixes as well as after. Nothing in the frozen-model validation needs redoing
on account of the arithmetic.

**What did change is the predicted period, by about one cycle per group.**
Measured on identical vectors, pre-fix RTL vs post-fix RTL, no backpressure:

| c_in | c_out | groups | batches/grp | pre-fix | post-fix | delta |
|---|---|---|---|---|---|---|
| 16 | 32 | 64 | 1 | 26115 ns | 26135 ns | +2 cyc total |
| 8  | 32 | 32 | 1 | 11925 ns | 11945 ns | +2 cyc total |
| 16 | 30 | 48 | 1 | 19385 ns | 19405 ns | +2 cyc total |
| 16 | 48 | 40 | 2 | 23955 ns | 24365 ns | +41 cyc, ≈ +1/group |
| 32 | 64 | 48 | 2 | 45955 ns | 46455 ns | +50 cyc, ≈ +1/group |
| 64 | 64 | 24 | 2 | 39315 ns | 39575 ns | +26 cyc, ≈ +1/group |

Two of the fixes move the schedule in opposite directions and very nearly cancel:
the MAC pipeline gained the stage it was missing relative to the BRAM read latency
(+1 cycle per batch), while the shadow copy now begins on the trigger cycle rather
than one cycle later (−1 cycle per batch). Single-batch shapes cancel exactly; the
residual on multi-batch shapes is about one cycle per group, roughly 1.5% of a
group period.

**Consequence for the manuscript.** Any number reported from the frozen model
describes the bitstream the OOS runs were measured on, and should be attributed to
it. If the design is rebuilt with the arithmetic fixes, the model will under-predict
multi-batch layers by ≈1 cycle per group; that is a constant offset in a term the
model already carries (the `+6` in the PW group-period expression becomes `+7`,
against a copy that starts one cycle earlier), and it can be re-derived analytically
rather than re-fitted. Do not re-fit constants to measurements — the freeze holds.

**The correctness finding is separate and larger.** Before 2026-09-03 the PW engine
did not compute correct convolutions for any shape, and did not compute them for
multi-batch shapes even after four of the five fixes. Every rate/distortion or
accuracy number that depends on PW output is therefore invalid for all builds prior
to commit cb16322, on top of the already-recorded fact that no trained weights exist.
Throughput, utilisation and timing-closure results are unaffected.
