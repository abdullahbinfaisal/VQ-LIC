# DW→PW On-Chip Fusion Plan — V2 (source of truth)

**Status: 2026-07-24. Supersedes `TCSVT\sources_1\new\DW_PW_FUSION_PLAN.md` and
`TCSVT\sources_1\new\FUSION_HANDOFF_2026-06-28.md` entirely.** Those two documents
described a **1-D** depthwise-separable model (`N samples × C channels`, `H=1`,
DW = 3-tap 1×3 conv). That was wrong — see §1. Do not consult them for facts about
the model; they're kept only as a postmortem reference for what went wrong.

This is the live project (`C:\Users\Fahad\Zynq\Zynq.srcs`), not `TCSVT`. As of
2026-07-22 the fusion work happens here directly — `TCSVT` is a stale fork with its
own further-along (and wrongly-scoped) `main.c`. Companion doc: `DESIGN_UNDERSTANDING.md`
(whole-design reference, correctly describes the 2D 3×3 datapath — see §2).

---

## 0. RESUME HERE — REAL ENCODER RUNS END-TO-END AT 27.6 fps (2026-07-30)

> ### THE HEADLINE NUMBER — 100-FRAME DISTRIBUTION (supersedes all 5-frame runs)
> **ImageEncoderLite (NeuralImageCodec encoder), 720p, 6 depthwise-separable
> blocks, ZC702:**
> ```
>   frames timed   100        warm-up discarded   3
>   mean           36.261 ms  std dev   0.002 ms  (0.005% of mean)
>   median         36.261 ms  min/max   36.254 / 36.266 ms
>   P95 / P99      36.264 / 36.266 ms   spread    0.012 ms
>   throughput     27.6 fps (mean)      27.6 fps (P99)
> ```
> ```
>   HW datapath   29.30 ms  80.8%
>   pack           4.51 ms  12.4%   (block 0 camera transpose only)
>   weight/param   2.44 ms   6.7%   (12 layers share the weight RAMs)
>   cache          0.00 ms   0.0%   (gm_in mapped NORM_NONCACHE)
>   -- driver sum 36.25 ms          outside driver 0.01 ms
> ```
> **P99 == mean to three decimals.** 0.005% CoV on a bare-metal single core with
> no OS, no interrupts and fixed DMA sizes. Worst-case throughput = mean
> throughput, which is the property a real-time claim actually needs. This is a
> reportable result in itself.
>
> ### FUSION TRAFFIC — 29.8% DDR REDUCTION (measured from beat counters)
> ```
>   intermediate (DW out)   3,916,792 B     avoided (write+read)  7,833,584 B
>   actual DMA             18,432,000 B     unfused would be     26,265,584 B
>   TRAFFIC REDUCTION            29.8%
> ```
> Implied channel count per block = 3.00 / 16.00 / 32.00 / 32.00 / 32.00 / 64.00
> — **DW pads nothing; block 0 streams exactly 3 channels.**
> ⚠️ An earlier draft claimed "~16.6 MB/frame saved". That was WRONG (it
> multiplied from PW output bytes). The correct figure is **7.83 MB**.
>
> ### POWER — VIVADO ESTIMATE, MEDIUM CONFIDENCE, NOT MEASURED
> ```
>   total 2.187 W = dynamic 2.024 + static 0.163      junction 50.2 C
>   PS7 1.573 W (71.9%)   PL fabric only 0.450 W (20.6%)
>   -> 4.89 GOP/s/W total | 23.8 PL-only | 79.3 mJ/frame | 409 pJ/MAC
> ```
> Confidence is Medium because **internal node activity is <25% specified**
> (vectorless, no SAIF) — and the PS7 figure that is 72% of the total is a
> generic model with no knowledge of the actual CPU workload. **PMBus
> measurement is still required before any GOP/s/W claim.**
>
> ### ⚠️ THE CLOCK-RATE PLAN IS BLOCKED BY A ROUTING PATH
> Post-route critical path:
> ```
>   first_ic_reg_rep__0/C -> p_0_out__42/OPMODE[4]   slack +0.321 ns
>   7.505 ns = logic 0.518 (7%) + NET 6.987 (93%)    LOGIC LEVELS: 0
> ```
> `first_ic` clears the PW accumulator and therefore drives the **OPMODE pins of
> all 64 DSP48s** in the 16×8 grid. At 125 MHz (8.000 ns) the projected slack is
> **≈ −1.7 ns**, and with **zero logic levels it cannot be pipelined**.
> Fixes, in order: replicate `first_ic` per DSP column/bank (as already done for
> `zp_in_lane`); extend the `dw_fused_timing.xdc` multicycle from
> `reg_zp_relu_reg[*]` to the other quasi-static config registers
> (`PS7_i -> reg_cin_run_reg` is 9.18 ns of a value written once per run).
> **Do NOT assume 125 MHz gives 1.25× throughput.** See task "Fix first_ic
> fanout before raising the PL clock".
>
> Hold is tightest in the PPU rounding→clamp stage at **+12 ps**. Hold slack is
> independent of clock period, so raising the frequency neither helps nor hurts
> it — but any re-place-and-route can flip it negative.
>
> ### ⚠️ ARM VQ MAY DOMINATE THE CODEC — CHECK BEFORE FRAMING THE PAPER
> Product quantisation spec: 64-dim embedding, M=4 codebooks, Dsub=16, K=256.
> Using `‖x−c‖² = ‖x‖² − 2x·c + ‖c‖²` with `‖c‖²` precomputed, cost is the dot
> product only: `M·K·Dsub = 16,384` MAC per position × 14,400 positions =
> **235.9 MMAC/frame — 1.22× the entire CNN (193.8 MMAC)**.
> ```
>   ARM A9 @667 MHz, 1 core:  2 MAC/cyc -> 176.9 ms | 4 -> 88.4 | 8 -> 44.2
>   t_codec = 36.27 + t_VQ    =>  80–213 ms  =>  5–12 fps
> ```
> Codebook is only **16 KB int8** so it is L1-resident; embeddings stream
> 921,600 B/frame; indices out 57,600 B/frame. **If this holds, 27.6 fps is a
> component figure, not the codec figure** — and moving PQ into PL is blocked by
> DSP already at 220/220.
>
> ### ⚠️ OPEN ANOMALY — block 2 `produced` = `written` − 1
> Block 2 reported DW `written=57,600` but `produced=57,599` in the capture pass
> — one 64-bit beat into the out FIFO that never reached PW. **Not reproducible**
> (the cold pass of the same run showed 57,600/57,600). Candidates: a benign
> read race between two AXI-Lite reads, or a genuinely stranded beat. PW output
> beats and S2MM bytes were both exactly correct, which favours the race — but
> off-by-one beat accounting has already been wrong twice here. Discriminator:
> read the counters twice in one pass. Do not quote fusion traffic to byte
> precision until resolved (aggregate effect is 8 B in 26 MB).
>
> ### PW OPTIMISATION — CORRECTED OVERHEAD MODEL AND A COMMITTED FIX (2026-07-30)
>
> **The earlier "~12.3 cycles per BATCH" figure was wrong.** Fitting blocks 3
> and 4 — identical `cin`, different batch counts — separates the two terms
> properly for the first time:
> ```
>   b3 (2 batches, cin=32) = 104.0      b4 (4 batches, cin=32) = 180.0
>   => 2A = 76  =>  A = 38 = cin + 6         B = 104 - 76 = 28
>   check b5 (4, cin=64): 4*70 + 28 = 308  vs measured 308.4  ✓
>   check b0 (1, cin=3) :     9 + 28 =  37  vs measured  36.1  ✓
>
>   cyc/group = batches*(cin_run + 6) + 28
> ```
> ```
>   compute   sum(batches*cin)          12.38 ms   42%
>   PER-GROUP 28 cyc x 43,200 groups    12.10 ms   41%   <-- DOMINANT
>   per-batch  6 cyc x 64,800 batches    3.89 ms   13%
> ```
> **Per-GROUP is the dominant overhead**, and 28 cycles is `S_WAIT_PPU` waiting
> for the last batch's drain to fully RETIRE (`N_OC` issues + ~9 cycles of PPU
> pipeline) plus the buffer swap. Block 0 is the extreme: 28,800 groups x 28 =
> **8.06 ms of its 10.38 ms**, against only 2.59 ms of actual compute.
>
> #### Shadow double-buffering: tried, failed, then succeeded for a different reason
> **Attempt 1 — double buffer alone: NO GAIN, and harmful as first written.**
> ```
>   config (2880-group TB)     baseline   dbl+copy-gate   dbl, no gate
>   cin=16 cout=32 (2 batches)   77.0        88.0 (+14%)     76.0
>   cin=32 cout=64 (4 batches)  179.0       179.0           179.0
> ```
> The copy and drain **already overlap by CHASING**: `P_PARAM_WAIT` costs 1
> cycle, then `P_DRAIN` reads index 0 (written first) advancing 1/cycle behind a
> copy also advancing 1/cycle, so the drain never catches up. There was no stall
> to remove. Adding `&& !shadow_copying` to `S_BATCH_DONE` *destroyed* that
> chase and cost 14%. **BRAM cost of double buffering is ZERO** — width sets the
> primitive count and depth goes 16->32 of 512 rows available.
>
> **Attempt 2 (v3) — COMMITTED. Double buffer + non-blocking group transition.**
> The costly wait was `S_WAIT_PPU`, not `S_COMPUTE`. Double buffering is what
> *enables* removing it: group N+1's first copy would otherwise clobber the bank
> group N is still draining.
> ```
>   S_WAIT_PPU:  if (ppu_st == P_IDLE)  ->  if (!shadow_copying)
>   S_COMPUTE :  if (ppu_st == P_IDLE)  ->  if (!shadow_copying)
>   S_BATCH_DONE keeps its P_IDLE gate, and now hands the bank over:
>       sha_rd_bank <= sha_wr_bank;  sha_wr_bank <= ~sha_wr_bank;
> ```
> ```
>   config                       baseline   v3      saving
>   cin=3  cout=16 (1 batch)       36.0     27.0    -25%
>   cin=16 cout=32 (2 batches)     77.0     61.0    -21%
>   cin=32 cout=64 (4 batches)    179.0    169.0    -5.6%
>   cin=64 cout=64 (4 batches)    308(m)   297.0    -3.6%
>   ALL PASS with exact beat counts.
>
>   projected: HW 29.30 -> ~24.9 ms, frame 36.26 -> ~31.8 ms, 27.6 -> ~31.4 fps
>   (b0 contributes 2.62 of the ~4.45 ms saved -- it has the most groups)
> ```
> **NOT YET BUILT.** Backup `pw_pixel_major_core.sv.bak_pre_v3_2026-07-30`.
>
> #### THE SIMULATION IS TRUSTWORTHY — use it instead of 40-minute builds
> The reconstructed baseline reproduces hardware to within **1 cycle/group**:
> ```
>   cin=3 /1 batch    hw 36.1   sim 36.0
>   cin=16/2 batches  hw 77.3   sim 77.0
>   cin=32/4 batches  hw 180.0  sim 179.0
> ```
> Harness: `sim_1/new/pw_single_oc_axis_instrumented_tb.sv`, set `N_OC`,
> `COUT_RUN`, `CIN_RUN`, `IN_GAP_CYCLES` at the top; TLAST timestamp / 10000 =
> cycles, / 2880 = cyc/group. **Evaluate every future PW change here first.**
>
> ### PLAN — REMAINING SERIALISATION AT `ppu_st == P_IDLE` (S_BATCH_DONE)
>
> After v3, `S_BATCH_DONE` still blocks until the previous drain has fully
> RETIRED. That is why v3 recovers only ~10-16 of the 28 per-group cycles.
> A drain occupies `N_OC` issue cycles **plus ~9 cycles of PPU pipeline tail**;
> the tail is pure dead time for the next batch.
>
> **KEY ENABLING FACT (verified in `ppu.sv`): the PPU latches its parameters
> WITH the data.** `acc_biased_s0 <= conv_acc_in + bias_in`,
> `mult_conv_s0 <= mult_conv`, `shift_conv_s0 <= shift_conv`, then
> `shift_conv_s1a <= shift_conv_s0` etc. down the pipeline. So `ppu_bias_q /
> ppu_mult_q / ppu_shift_q` may be changed for the next batch as soon as the
> previous batch's last `valid_in` has been SAMPLED — retirement is irrelevant.
> **Nothing in the datapath requires the P_IDLE wait.** It exists only because
> `ppu_out_cnt` is a single per-batch counter used as the exit condition.
>
> #### Option C (RECOMMENDED — cheapest, targets the ~9-cycle tail)
> Split issue from retire:
> - `S_BATCH_DONE` triggers the next drain when `ppu_issue_idx == N_OC`
>   (issuing complete) instead of `ppu_out_cnt == N_OC` (all retired).
> - Replace the per-batch `ppu_out_cnt` with a **global** output counter, and a
>   small `in_flight` counter (`+1` per `ppu_valid_in`, `-1` per
>   `ppu_valid_out`) so `done` can still be detected as
>   `issues_complete && in_flight == 0`.
> - The PPU pipeline is strictly FIFO and in-order, so outputs still emerge in
>   issue order; no reordering logic is needed.
> - Expected: removes ~9 cycles/batch. 64,800 batches x 9 = 583,200 cyc =
>   **~5.8 ms/frame**, on top of v3.
> - Risk: `done_out` / TLAST accounting is exactly the class of bug that has bitten
>   this design three times. Verify `vin == vout == fifo_writes` in the TB across
>   1/2/4-batch configs BEFORE building.
>
> #### Option B (fallback — two drain contexts)
> Duplicate `ppu_st`, `ppu_issue_idx`, `ppu_out_cnt`, `ppu_oc_batch` and the
> param registers; alternate contexts. Conceptually simpler, no counter
> redesign, but needs two param pre-read streams and roughly doubles the PPU
> control registers. Use only if Option C's counter rework proves fragile.
>
> #### Option A (structural — do NOT start without Option C measured)
> Make the whole PPU path credit-based: issue continuously while the shadow bank
> has data and `!out_stall`, carrying a batch tag alongside the data exactly as
> `dw_plane_run_axis.sv` carries its last-beat flag through the FIFO. This is
> the "observed, not predicted" pattern already applied twice here. Largest
> gain, largest risk, and it subsumes Option C.
>
> #### Ceiling
> With Option C on top of v3, per-group overhead should fall from 28 to roughly
> 10-12 cycles and per-batch from 6 to ~5. Projected HW ~19-20 ms, frame
> ~26-27 ms, **~37-38 fps** — which matches the independently derived
> `max(batches*cin, cout_rounded)` ideal of 19.58 ms. That is the floor for this
> architecture at 100 MHz; beyond it, blocks 1 and 2 become DW-input bound.
>
> ### NEXT SESSION: EXTRACT PAPER NUMBERS. NO CORRECTNESS TESTING (user's call).
> The full evidence package is in **`PAPER_HW_EVIDENCE.md`** (this directory),
> numbered §0–§25 to match the paper information request, with a master
> per-layer table at §0 and provenance tags (MEASURED / REPORT / DERIVED /
> MODEL) on every number.
>
> **Still open, in the order that unblocks the most:**
> 1. Block diagram (figure) — no new data needed
> 2. Op-count convention: MAC = 1 or 2 ops — one decision, blocks every comparison
> 3. PMBus power measurement — replaces a Medium-confidence estimate
> 4. ARM PQ kernel + timing (task #22)
> 5. `first_ic` fanout fix (task #24) — prerequisite for >30 fps
> 6. Block 2 anomaly (task #23)
> 7. Baseline comparison table
> 8. Correctness validation — deferred by explicit decision; **no accuracy,
>    PSNR, MS-SSIM or bitrate claim is supportable without it**
>
> **Gated (do not start until the baseline bitstream, reports and correctness
> logs are archived):** shadow-accumulator double buffering. Worth ~10 ms/frame
> and it is the only remaining PW lever, but it competes with the `first_ic` fix
> — both touch PW, and only one can be attributed cleanly per build.
>
> ### CONFIGURATION OF RECORD
> ```
>   board/device   ZC702, xc7z020clg484-1        tools  Vivado/Vitis 2020.2
>   PL clock       FCLK0 100 MHz                 XSA    noc16_cg2048.xsa (21:47)
>   PW N_OC 16, N_LANES 8   DW MACs on DSP (USE_DSP=1)   MAX_CG_PRODUCT 2048
>   main.c: SURR_MODEL_ENCODER 1, PW_N_OC 16, SURR_CHAIN_PAIRS 1,
>           SURR_GM_IN_NONCACHED 1, CASCADE_INSTRUMENT 0
>   LUT 18,280 (34.4%)  FF 15,750 (14.8%)  BRAM 73.5/140 (52.5%)
>   DSP 220/220 (100.0%)   WNS +0.321 ns   WHS +0.012 ns   all constraints met
>   per module: DW 6,043 LUT / 24 BRAM36 / 88 DSP
>               PW 7,164 LUT / 41 BRAM36 / 132 DSP
> ```
>
> ### MODEL (measured, not assumed)
> ```
>   blk  DW C  DW HxW      s  ->  PW Cin->Cout @ HxW     groups batches
>    0     3   720x1280    2      3 -> 16  @ 360x640     28800     1
>    1    16   360x640     2     16 -> 32  @ 180x320      7200     2
>    2    32   180x320     2     32 -> 32  @  90x160      1800     2
>    3    32    90x160     1     32 -> 32  @  90x160      1800     2
>    4    32    90x160     1     32 -> 64  @  90x160      1800     4
>    5    64    90x160     1     64 -> 64  @  90x160      1800     4
> ```
> Activation is ReLU6 in the model; the PPU implements plain ReLU (no upper
> clamp). BatchNorm folds into weights/bias at quantisation. Block 5's final PW
> has no activation.
>
> ### PER-BLOCK MEASURED vs PREDICTED (prediction written BEFORE the run)
> ```
>   blk  groups  cyc/grp  model  measured HW   predicted HW
>    0    28800    36.1     16      10.38 ms     10.40   exact
>    1     7200    77.3     32       5.57 ms      5.57   exact
>    2     1800   149.0     64       2.68 ms      2.30   +17%  <-- see below
>    3     1800   104.0     64       1.87 ms      1.97
>    4     1800   180.0    128       3.24 ms      3.56
>    5     1800   308.4    256       5.55 ms      5.87
>   TOTAL                            29.30 ms    29.67   -1.3%
>   FRAME                            36.27 ms    37.2    -2.6%
> ```
> **Block 2 is the one real miss, and it refines the model.** It was predicted
> DW-bound with total = max(DW, PW) = 230,400 cyc. Measured 268,264. Blocks 2
> and 3 have IDENTICAL PW configs (32->32, cin 32, 2 batches, 1800 groups) yet
> differ 149.0 vs 104.0 cyc/group -- the only difference is DW input volume
> (230,400 vs 57,600 beats). So when DW binds, the cascade costs
> **DW_beats + a PW pipeline tail**, NOT max(DW, PW). The tail here is ~37,900
> cycles. Use `max()` as a lower bound only.
>
> ### STRIDE PLUMBING CONFIRMED PER LAYER
> `consumed` = 4x `written` on blocks 0,1,2 (stride 2) and `consumed` =
> `written` on blocks 3,4,5 (stride 1) -- first time mixed strides have run, and
> the per-layer `ZP_RELU[17]` control is doing what it should.
>
> ### DERIVED QUANTITIES FOR THE PAPER
> ```
>   MACs/frame     DW  35,251,200 (18.2%) + PW 158,515,200 (81.8%)
>                  = 193,766,400  (193.8 MMAC/frame)
>   throughput     5.35 GMAC/s   (10.70 GOP/s if 1 MAC = 2 ops -- STATE THIS)
>   DMA traffic    MM2S 10,137,600 + S2MM 8,294,400 = 18,432,000 B (18.43 MB)
>                  + CPU pack traffic 5,529,600 B (2 x 2,764,800)
>   bandwidth      509 MB/s sustained; 629 MB/s across the 29.30 ms HW window
>   arith. int.    10.51 MAC/byte of DMA traffic
> ```
>
> ### ROOFLINE — CORRECTED CEILINGS (an earlier "44.0 GMAC/s / 12.2%" was WRONG)
> **Do NOT use 220 DSP x 2 MAC.** Only some DSPs are MACs, and only PW packs:
> ```
>   PW MAC grid  N_OC x N_LANES/2 = 16 x 4 = 64 DSP, 2 MAC each = 128 MAC/cyc
>   DW MACs      8 lanes x 9 taps         = 72 DSP, 1 MAC each =  72 MAC/cyc
>   (remaining ~84 DSP are PPUs, requant multipliers, address arithmetic)
>   COMPUTE ROOF = 200 MAC/cycle @100 MHz = 20.0 GMAC/s
>     of which PW 12.8 GMAC/s, DW 7.2 GMAC/s
> ```
> **MEMORY ROOF = the HP ports, not the measured traffic.** BD has HP0+HP1
> enabled, 64-bit each, clocked by FCLK0:
> ```
>   per port   64 b x 100 MHz = 800 MB/s      (HP0 reads / HP1 writes)
>   aggregate  1.6 GB/s        <-- the accelerator-side ceiling
>   DDR3 MT41J256M8, 32-bit, 533 MHz = 4.27 GB/s (NOT binding)
> ```
> **Operating point (a dot on the plot, never a roof):**
> ```
>   arithmetic intensity 10.51 MAC/byte    achieved 5.35 GMAC/s
>   reads  10.14 MB / 29.30 ms = 346 MB/s of 800  (43%)
>   writes  8.29 MB / 29.30 ms = 283 MB/s of 800  (35%)
>   combined 629 MB/s of 1600 (39%)
>   ridge point = 20.0 / 1.6 = 12.5 MAC/byte ; we sit at 10.51, just left of it
>   memory roof at our AI = 1.6 GB/s x 10.51 = 16.8 GMAC/s
>   achieved / applicable roof = 5.35 / 16.8 = 32%
> ```
> **Conclusion for the paper: the design is neither compute-roof-bound nor
> bandwidth-bound.** Both ports sit near 40%, and compute is at 27% of the MAC
> ceiling. The limiter is the per-batch drain serialisation (§ cost model) --
> a control-path result, not a resource result. That is a defensible and
> specific finding, and it points at a fix worth ~10 ms/frame.
>
> Per-engine efficiency, which is more informative than the aggregate:
> ```
>   PW  158.5 MMAC/frame x 27.6 fps = 4.37 GMAC/s of 12.8 = 34%
>   DW   35.3 MMAC/frame x 27.6 fps = 0.97 GMAC/s of  7.2 = 14%
> ```
>
> ### COST MODEL (validated across 3 N_OC values and 9 layer configs)
> ```
>   cyc/group = batches * (cin_run + 12.3) + 20.8      [N_OC=16]
>   batches   = cout_rounded / N_OC ,  groups = pixels / 8
>   if DW binds: total ~ DW_input_beats + PW pipeline tail
> ```
> ~12 cycles per BATCH plus ~21 per GROUP. The IDEAL, if batch N+1 overlapped
> batch N's drain, is `max(batches*cin, cout_rounded)` -- that is the target for
> the shadow-accumulator double-buffer fix, worth ~10 ms/frame here.
>
> ### CAVEATS THAT MUST APPEAR IN ANY WRITE-UP
> 1. **Outputs are NOT validated.** Weights/params are synthetic dummies; the
>    figures are timing only. The DW one-word output offset and the
>    left/right-8-column edge bug are both still open, and chaining propagates
>    them between blocks. Non-zero-point bytes were observed in blocks 1-5
>    (e.g. `90 FF FF D0`) where uniform 0x80 was expected -- consistent with
>    those known bugs compounding along the chain. **No accuracy, PSNR, or
>    task-quality claim can be made from this work as it stands.**
> 2. `pack` is real recurring work: a camera does not deliver group-major.
> 3. `prog` (2.44 ms) is per frame because the 12 layers share the DW/PW weight
>    RAMs and cannot all stay resident.
> 4. `chunk done` in the log is a BROKEN bracket -- it routinely exceeds
>    1 beat/PL-cycle, which is physically impossible. Never quote it.
> 5. Per-pair numbers in the COLD pass are inflated ~2.6x by blocking UART
>    inside the timed regions. Only the WARM per-pair table is usable.
> 6. DSP is at 100%: this design is device-limited on the 7z020, so no further
>    N_OC or N_LANES scaling is possible on this part.
>
> ---

## 0.1 (earlier) PW POWER-OF-TWO N_OC BUG FOUND AND FIXED; DW STRIDE-2 EXISTS (2026-07-30)

> ### THE HEADLINE: `N_OC` MUST NOT HAVE BEEN A POWER OF TWO
> The N_OC=8 build was flashed and **failed with an MM2S timeout**. Root cause,
> found by simulation and fixed:
>
> ```systemverilog
> // pw_pixel_major_core.sv, was:
> logic [$clog2(N_OC>1?N_OC:2)-1:0]  ppu_issue_idx;   // BROKEN
> ```
> `$clog2(N_OC)` cannot **represent** N_OC when N_OC is a power of two. At
> N_OC=8 this is `[2:0]`, max value 7, so the P_DRAIN issue guard
> `$unsigned(ppu_issue_idx) < N_OC` **can never go false**. The drain kept
> issuing PPU beats and stopped only when `ppu_out_cnt` caught up — i.e. after
> `N_OC + PPU_LATENCY(9)` issues.
>
> | N_OC | width | max | issues/batch | |
> |---|---|---|---|---|
> | 8  | 3 bits | 7  | **17** = 8+9  | broken |
> | 16 | 4 bits | 15 | **25** = 16+9 | broken |
> | 30 | 5 bits | 31 | 30            | correct |
>
> **Every build before this one was correct only because 30 is not a power of
> two.** `$clog2(30)=5` holds both 30 and 31, so the guard worked. This was a
> latent bug the design has been carrying, not something N_OC=8 introduced.
>
> **`N_OC=16` IS NOT A HEDGE** — §0.1 names it as the fallback if N_OC=8
> underperforms. It fails for exactly the same reason. Any power-of-two N_OC
> would have.
>
> **Fix:** `$clog2(N_OC+1)`, so the terminal value is always representable.
>
> ### WHERE THE PW IP ACTUALLY BUILDS FROM — EARLIER NOTE WAS WRONG
> **The PW IP does NOT build from the TCSVT fork.** It is packaged by
> **`Zynq\Zynq.srcs\component.xml`** (`<spirit:name>pw_single_oc_axis_axi</spirit:name>`)
> — a `component.xml` at the `Zynq.srcs` root. `Zynq.srcs` IS a loaded user IP
> repository; `C:\Users\Fahad\TCSVT\ip_repo` is **not** in the project's list.
> Its sources come from **TWO different folders**, which is the trap:
> ```
> core:  Zynq\Zynq.srcs\src\pw_pixel_major_core.sv          <-- NOT sources_1\new
> shell: Zynq\Zynq.srcs\sources_1\new\pw_single_oc_axis.sv
> shell: Zynq\Zynq.srcs\sources_1\new\pw_single_oc_axis_axi.sv
> ppu:   Zynq\Zynq.srcs\sources_1\new\ppu.sv
> ```
> Editing `sources_1\new\pw_pixel_major_core.sv` changes what humans read and
> what the testbenches compile, and changes **NOTHING** in the netlist.
>
> Proved 2026-07-30: the fix was first applied only to the TCSVT copy, a build
> guard checked that copy and PASSED, and the freshly regenerated
> `Zynq.gen\...\bd\hw\ipshared\93ae\src\` copy still came back WITHOUT the fix,
> matching `Zynq.srcs\src\`. The two were byte-identical beforehand, so hashing
> them would NOT have revealed which one builds — only editing one and seeing
> which content regenerated did. **Check the GENERATED copy, never the source
> you think is authoritative.**
>
> Fix now applied to `Zynq\Zynq.srcs\src\` (the real one), with the live-tree
> and TCSVT copies kept in sync for humans/sim. The PW `component.xml` declares
> each file once, so it does NOT have the DW double-declaration trap.
>
> ### THE FAILURE CHAIN (why it looked like a DMA bug)
> Over-issuing meant every pixel group emitted ~2x its beats, so `produced_cnt`
> reached the **predicted** `total_groups_r` at roughly half the input. TLAST
> fired early, S2MM completed a garbage-tailed transfer looking perfectly
> healthy (`Idle=1, IOC_Irq=1`, no errors, full byte count landed), PW went
> done and stopped draining DW, DW's out FIFO filled, DW throttled, and MM2S
> could never finish. **MM2S was the victim, not the cause.** The tell was in
> the DW debug flags: `in_empty=0, out_prog_full=1` — DW was throttled, not
> starved, which points downstream.
>
> This is the FOURTH instance of one bug class in this design: predicted DW
> TLAST, PW `out_full` beat drop, PW `grp_idx` free-run (below), and now this.
> **Control state advancing on a predicted or free-running count instead of on
> observed completion of work.** PW's TLAST is STILL predicted
> (`produced_cnt == total_groups_r - 1`, `pw_single_oc_axis.sv:236`); the
> 2026-07-27 change only made the flag TRAVEL with its beat through the FIFO.
> Making it observed remains outstanding and would have contained this.
>
> ### N_OC=16 BUILD + PER-BATCH MODEL CONFIRMED BY PREDICTION (2026-07-30 late)
> Build: `MAX_CG_PRODUCT` 1024->2048, `N_OC` 8->16, `noc16_cg2048.xsa`.
> ```
>                N_OC=8 (19:44)   N_OC=16 (21:47)
>   LUT           16,681 31.4%     18,280 34.4%
>   BRAM            69.5 49.6%       73.5 52.5%   (+4 = MAX_CG_PRODUCT 1024->2048)
>   DSP              170 77.3%        220 100.0%  <-- SATURATED, zero headroom
>   WNS           +0.305 ns         +0.321 ns     (improved)
>   WHS           +0.009 ns         +0.012 ns     (improved)
>   PW module        82 DSP           132 DSP
> ```
> **DSP is at 220/220.** Nothing further can be added on the DSP side -- no
> N_OC=32, no N_LANES change. Checked for multipliers spilling into LUTs; the
> `Synth 8-3936` hits are unrelated DMA register trimming, so the fit is real.
> Escape hatch if needed: revert DW to `USE_DSP(0)` to free 72, at ~6,400 LUTs
> and worse setup slack. Netlist verified: `ppu_issue_idx` = 5 flops
> (= `$clog2(17)`), `CONFIG.N_OC` = 16, zero `Synth 8-2490`.
>
> **THE PER-BATCH MODEL WAS CONFIRMED BY A FALSIFIABLE PREDICTION**, made and
> written down BEFORE the run, including a counterintuitive component:
> ```
>   pair   @N_OC=8   predicted@16   measured@16
>     0      8.08       ~10.37        10.38   <-- predicted to get SLOWER
>     1      3.83        ~2.59         2.97
>     2      3.53        ~2.43         2.41
>   total   15.44       ~15.39        15.76
>   fps      49.1         ~49.2         48.3
> ```
> Pair 0 gets slower because `Cout=8` pads to `cout_rounded=16`, doubling its
> output beats. No per-GROUP model predicts that. **N_OC=16 is a net LOSS for
> the legacy surrogate** (48.3 vs 49.1 fps) for exactly this reason -- and it
> does NOT transfer to the encoder, whose Couts are all multiples of 16.
>
> **REFINED COST MODEL.** Pair 1 missed by 15%, which is diagnostic: pairs 0 and
> 1 have identical batches (1) and cout_rounded (16) yet differ by 5.1 cyc/group,
> and their only difference is `cin`. So the `max(cin, DRAIN)` floor is WRONG --
> cin contributes linearly even below the drain length:
> ```
>   cyc/group = batches * (cin_run + 12.3) + 20.8          [N_OC=16]
>     pair 0  1*(3+12.3)+20.8  =  36.1   (measured  36.1)
>     pair 1  1*(8+12.3)+20.8  =  41.1   (measured  41.2)
>     pair 2  4*(16+12.3)+20.8 = 134.0   (measured 133.9)
> ```
> ~12 cycles per BATCH plus ~21 per GROUP.
>
> **Updated ImageEncoderLite projection** (refined model, anchored on six
> per-pair measurements across two N_OC values):
> ```
>   N_OC=8                        HW 49.69   frame 57.2   17.5 fps
>   N_OC=16  (fabric now)         HW 29.67   frame 37.2   26.9 fps
>   N_OC=16 + drain overlap       HW 19.58   frame 27.1   36.9 fps
> ```
>
> **Non-zero output bytes appeared for the first time** (`pair 2: ... C5 FF FF`).
> That is the EXPECTED structural mismatch, flagged before the run: at N_OC=16
> pair 0 emits 16 channels where pair 1's DW expects 8, so the chained data is
> misaligned and compounds. Not a new bug, and it does not affect timing (no
> data-dependent paths). It is also the first positive evidence that real data
> propagates through the chain -- everything was zero-point until now.
>
> ### ⚠️ CORRECTION — THE "25.6 cyc/GROUP OVERHEAD" IS WRONG. IT IS PER-BATCH.
> Everything below that quotes a per-GROUP overhead constant (22.8 at N_OC=30,
> 25.6 at N_OC=8) was **one constant fitted to one aggregate**. A per-pair warm
> instrumented run on 2026-07-30 disproves it:
> ```
>   pr  groups  cyc/grp  compute-model  "OVH"  batches
>    0   28800     28.1        8         20.1     1
>    1    7200     53.2       16         37.2     2
>    2    1800    195.9      128         67.9     8
> ```
> "OVH" tracks BATCHES, not groups. Mechanism, visible in
> `pw_pixel_major_core.sv`: **`S_COMPUTE` cannot exit until `ppu_st == P_IDLE`**,
> so batch N+1's compute cannot finish until batch N's PPU drain has fully
> RETIRED (N_OC issues + ~9 cycles of PPU pipeline tail). The drain is therefore
> serialised per batch.
>
> **Corrected cost model** (0.6% against the measured 15.44 ms, and within 4%
> on every individual pair):
> ```
>   cyc/group = batches * ( max(cin_run, DRAIN) + FIX ) + GRP
>   DRAIN ~ 17 (= N_OC + PPU latency)   FIX ~ 8   GRP ~ 3
>   IDEAL, if batch N+1 overlapped batch N's drain:
>   cyc/group = max(batches*cin_run, cout_rounded)      <- the old model, which
>                                                          is the TARGET, not
>                                                          the current behaviour
> ```
> Note `max(cin, 17)`: while `cin_run <= 17` the DRAIN sets the floor, so the
> per-batch cost is flat at ~25 cycles regardless of how little compute the
> batch does. Measured per-batch: 28.1 / 26.6 / 24.5 for cin = 3 / 8 / 16.
>
> **Consequences:**
> 1. Any estimate made with the per-group model is optimistic. The
>    ImageEncoderLite projection moves 38.71 -> **44.78 ms HW** (21.6 -> 19.1 fps).
> 2. **`N_OC=16` becomes the largest single lever**, because halving the batch
>    count halves a PER-BATCH cost: 19.1 -> 31.1 fps on its own.
> 3. Overlapping the drain (double-buffer the shadow accumulator so batch N+1
>    need not wait for `P_IDLE`) is worth 19.1 -> 28.4 fps on its own.
> 4. Together: **36.9 fps**. At N_OC=16 blocks 1 and 2 then become DW-INPUT
>    bound (DW must ingest at 1 beat/cycle), which caps further PW parallelism.
>
> **Process note:** this is the second time a single constant fitted to an
> aggregate has misled us today (the first was the PW over-production bug, where
> aggregate pass/fail hid a 17-vs-8 per-batch ratio for three rounds). Fit the
> model PER UNIT and check the residuals vary the way the mechanism predicts.
>
> ### NON-CACHEABLE gm_in — 49.1 fps (2026-07-30). CACHING IT WAS HARMFUL.
> `gm_in` mapped `NORM_NONCACHE` (0x11DE2) via `Xil_SetTlbAttributes`, 4x 1 MB
> sections at 0x33000000. `SURR_GM_IN_NONCACHED` in main.c.
> ```
>              chained     + noncached
>   pack        6.24          4.52     <-- FASTER, not slower
>   cache       8.88          0.00
>   HW         15.44         15.44
>   prog        0.41          0.41
>   FRAME      30.97         20.37     = 49.1 fps
> ```
> **The prediction was wrong in the useful direction.** Uncached writes were
> expected to cost 1.5-3x on `pack`; they were 1.4x CHEAPER. Mechanism: the
> Cortex-A9 L1 is **write-allocate**, so every store into a not-yet-cached
> `gm_in` line first READ 32 bytes from DRAM -- lines `pack` then overwrites
> entirely. That is ~2.76 MB/frame of pure-waste reads, plus cache pollution
> evicting the `raw_in` data `pack` streams through, plus the writeback. Normal
> non-cacheable skips all three; the write buffer merges and streams out.
>
> So the flush was not the only cost of caching that buffer -- **caching it was
> actively harmful**. Saving was 10.60 ms, not the 8.88 targeted.
>
> Use `NORM_NONCACHE`, NOT `DEVICE_MEMORY`/`STRONG_ORDERED`: Normal
> non-cacheable still permits write buffering and merging; device/strongly-
> ordered forbids it and would make `pack` far slower -- an easy way to
> "disprove" this accidentally.
>
> **HW is now 75.8% of the frame.** Remaining budget:
> ```
>   HW    15.44  75.8%   ~9.7 ms per-group overhead + ~5.8 ms compute
>   pack   4.52  22.2%   pair 0 camera transpose
>   prog   0.41   2.0%
> ```
> Biggest remaining lever is the PW per-group overhead (~9.7 ms, pure FSM work,
> no resources). A fabric repack IP would take the last 4.52 ms => ~15.9 ms
> (63 fps).
>
> ### REAL STRIDE-2 + CHAINED PIPELINE — 32.3 fps, no proxies left (2026-07-30)
> **This is the first end-to-end run of the surrogate as specified.** Every
> proxy and harness artifact is gone from the measured path.
> ```
>                     proxy      real s2    real s2 + chained
>   pack               2.53      12.07        6.24   (pair 0 only)
>   cache              4.58      17.77        8.88   (pair 0 only)
>   HW                15.43      15.44       15.44   (unchanged throughout)
>   prog               0.41       0.41        0.41
>   FRAME             22.96      45.69       30.97
>   fps                43.6       21.9        32.3
> ```
> Verified engaged: `IMGW=1280 NROWS=720 NG=160`, `PW TILE_PX=230400`,
> `consumed=345600` = **4x** `written=produced=86400`. DW ingests 5.53 MB/frame
> where the proxy fed it 1.38 MB.
>
> **43.6 fps was never real** — it ran every DW on a quarter of its input. The
> defensible figure is **32.3 fps**.
>
> **DW HIDES BEHIND PW — now measured, not asserted.** HW stayed at 15.44 ms
> while DW's input went 4x. §0.1's claim is confirmed.
>
> **CHAINING (`SURR_CHAIN_PAIRS`, main.c).** Pair p's PW output feeds pair p+1's
> DW directly. Legal because PW emits group-major/channel-minor with
> cout_rounded channels per pixel group -- byte-for-byte what the next DW
> consumes -- and at N_OC=8 cout_rounded == Cout, so the counts match exactly:
> ```
>   pair 0 out 230400 x 8  = 1,843,200 = pair 1 DW in 8 x 360 x 640
>   pair 1 out  57600 x 16 =   921,600 = pair 2 DW in 16 x 180 x 320
> ```
> Pairs 1..n-1 therefore need **no pack AND no cache flush** -- the CPU never
> touches those bytes (S2MM writes them; MM2S reads DRAM). Ping-pong buffers
> chainA/chainB so the DMA never reads and writes one region. Worth 14.7 ms/frame.
> The old behaviour is still available at `SURR_CHAIN_PAIRS 0` for comparison.
>
> **Per-group PW overhead re-confirmed at 25.6 cyc/group** (modelled 576,000
> cycles vs measured 1,544,000 over 37,800 groups) -- the same figure derived
> independently at N_OC=30 and N_OC=8. It is now 9.68 of the 15.44 ms HW.
>
> **Where the remaining time is, and the ceiling on each:**
> ```
>   HW      15.44  49.8%   of which ~9.7 ms is per-group overhead, ~5.8 compute
>   cache    8.88  28.7%   pair 0 flush only; ~3.2 ms/MB, L1+L2/PL310 per-line
>   pack     6.24  20.2%   pair 0 camera-side transpose -- genuine work
>   prog     0.41   1.3%
> ```
> Next levers: non-cacheable `gm_in` (removes the 8.88, may slow pack -- must be
> measured); a fabric repack IP (removes pack AND cache, ~15.9 ms => ~63 fps);
> PW per-group overhead (halving it is worth ~4.8 ms).
>
> **DRAM traffic per frame** (now meaningful for the bandwidth/roofline work):
> MM2S 5.53 MB read + S2MM 3.69 MB write = 9.22 MB DMA, plus ~5.53 MB of CPU
> pack traffic. ~597 MB/s across the 15.44 ms HW window.
>
> ### PACK OPTIMISATION — 43.6 fps (2026-07-30, software only)
> `pack_input_group_major` was copying ONE BYTE AT A TIME with a per-byte
> `(col < W)` bounds test that can only fail in a row's last group, and only
> when `W % 8 != 0` — never true here (1280/640/320/160). Replaced with an
> 8-byte move for full groups plus a tail loop for ragged widths; differential-
> tested against the original across all surrogate shapes and six ragged widths.
> Also enabled NEON (`-mfpu=neon -ftree-vectorize` in `UserConfig.cmake`) — the
> app had been building `-mfpu=vfpv3`, so GCC emitted no NEON at all. No
> `-ffast-math`: NEON SP on Cortex-A9 is not IEEE-754, and integer loops
> vectorise without it.
> ```
> pack 8.20 -> 2.53 ms      frame 28.65 -> 22.96 ms = 43.6 fps
> HW 15.43, cache 4.58, prog 0.41 -- ALL UNCHANGED (the controls held)
> ```
> NEON is NOT the lever here: after the loop fix this is an 8-byte-granular
> block scatter (dst sequential, src jumps H*W between channels), which is
> memory-bound. NEON has no scatter and vst2/3/4 interleave elements, not
> 8-byte blocks.
>
> ### RESOURCE/TIMING REBUILD (2026-07-30 19:44) — all three changes verified
> `MAX_CG_PRODUCT` 10800->1024, DW `USE_DSP(0)->(1)`, DW IP sources synced.
> ```
>                    before(17:42)      after(19:44)
>   BRAM tiles        129.5  92.5%      69.5  49.6%     -60 tiles
>   LUT              23,071  43.4%     16,681  31.4%    -6,390
>   DSP                 98   44.6%       170   77.3%    +72
>   WNS              +0.0777 ns        +0.305 ns        ~4x better
>   WHS              +0.0481 ns        +0.009 ns        <-- THIN, watch this
>   dw_fused_axi_0   12,426 LUT / 80 BRAM / 16 DSP
>                ->   6,037 LUT / 20 BRAM / 88 DSP
> ```
> DW->DSP paid off twice: it removed 6,390 LUTs AND improved setup slack ~4x
> (DSP48 P-register replaces a fabric adder chain). **Hold slack is now only
> +9 ps** — met, but that is the new margin to watch; hold cannot be fixed by
> slowing the clock.
>
> **DSP is now the binding constraint at 77%**, which settles the `N_LANES=16`
> question: PW at 16 lanes needs ~64 (MAC grid) + ~96 (16 PPUs) + 2 = ~162, and
> with DW's 88 that is ~250 of 220. Not possible on this device alongside the
> DW DSP change.
>
> **THE SOURCE SYNC PROVED NECESSARY THE SAME DAY.** This build synthesized the
> windower from `ipshared/c9c1/src/` — the DW IP's OWN copy — whereas the 17:42
> build used `bd/hw/Zynq/...sources_1/new/`. The duplicate-declaration coin flip
> genuinely flipped between two consecutive builds. Had the copies not been
> synced first, stride-2 would have silently vanished from this bitstream.
> `Synth 8-2490` count is now 0.
>
> ### CONFIRMED ON SILICON (2026-07-30 evening) — 34.9 fps, counts exact
> ```
> pair 0  consumed=written=produced=86400   (was 44431/44188/44149)
> pair 1  consumed=written=produced=57600
> pair 2  consumed=written=produced=28800
> MM2S complete, DW STATUS=0x1, PW STATUS=0x1, no timeout, all 3 pairs
>
> steady state 28.65 ms/frame = 34.9 fps   (was 34.26 ms = 29.2 fps)
> repeatability 28.64..28.65 over 5 frames
>
> WARM breakdown      was (N_OC=30)      now (N_OC=8)
>   HW datapath        21.04 ms 61.4%     15.43 ms 53.9%
>   pack                8.20 ms 23.9%      8.20 ms 28.6%   <- bit-identical
>   cache               4.60 ms 13.4%      4.60 ms 16.1%   <- bit-identical
>   weight/param        0.41 ms  1.2%      0.41 ms  1.4%   <- bit-identical
> ```
> Only `HW` moved, exactly as expected: the other three are CPU-side and
> N_OC-independent. **CPU-side work is now 46% of the frame.**
>
> **The output bytes are all 0x80 (= zp_out). That is CORRECT here** — the
> surrogate estimate runs with synthetic dummy weights (`main.c` ~3659:
> "Numerically meaningless; only the measured cycle counts matter"). But note
> what this run does and does not prove: **it validates FLOW, not DATA.** The
> bug just fixed produced a full-length, cleanly-completing, garbage-tailed
> stream. Exact beat counts are strong evidence the flow is right and no
> evidence at all that the arithmetic is. Software-golden validation is still
> the top open item.
>
> ### THE REAL BOTTLENECK IS NOW PER-GROUP OVERHEAD (~24 cyc/group)
> Predicted ~42 fps, got 34.9. The miss is quantifiable, and this is the most
> useful number from the run:
>
> | | modelled cyc | measured cyc | implied overhead | per group |
> |---|---|---|---|---|
> | N_OC=30 | 1,242,000 | 2,104,000 | 862,000 | **22.8** |
> | N_OC=8  |   576,000 | 1,543,000 | 967,000 | **25.6** |
>
> Across 37,800 groups/frame there is a **fixed ~24 cycles/group** that N_OC
> does not touch. At N_OC=30 useful work was ~33 cyc/group so overhead was ~41%;
> at N_OC=8 useful work is ~15 so overhead is ~63%. Efficiency vs model fell
> 59% -> 37% — not a regression, the same constant becoming a bigger share.
> This puts a number on §0.1 open item #4 for the first time.
>
> **Consequence: further N_OC reduction buys almost nothing.** Suspects are the
> per-group FSM transitions — `S_BATCH_DONE` -> `S_WAIT_PPU`, `P_PARAM_WAIT`,
> and the shadow-copy trigger.
>
> ### TWO BRACKETS THAT MUST NOT BE QUOTED
> 1. **`chunk done` fails the physical bound on all three pairs** — 230,400
>    beats/137,850 cyc = **1.67**, 115,200/98,889 = **1.16**, 115,200/45,113 =
>    **2.55** beats/PL-cycle. A 64-bit AXIS cannot exceed 1.0. It is still the
>    broken tail bracket of §0.1 lesson 1: it is printed immediately after
>    `MM2S complete` because it starts after MM2S finishes. The `HW` bracket's
>    own ratios (0.16/0.09/0.09) are physically sound.
> 2. **The per-pair HW numbers printed (14.07/12.53/13.25 ms) are COLD.** They
>    sum to 39.85 ms against a warm HW total of 15.43 — a 2.6x gap. There is
>    currently NO valid per-pair breakdown; a warm instrumented pass is needed.
>
> ### BUILD RESULT — FIX VERIFIED IN THE NETLIST (2026-07-30 17:42)
> ```
> ppu_issue_idx flops in netlist: 4   (hw_i/pw_single_oc_axis_axi_0/inst/u_pw/u_core/)
>   -> was 3 in the broken build; $clog2(N_OC+1) IS in the netlist
> N_OC in build = 8       impl_1 write_bitstream Complete, PROGRESS 100%
> WNS +0.0777 ns   WHS +0.0481 ns          <-- MET but very tight
> LUT 23071/53200 43.4%   FF 16231/106400 15.3%
> BRAM 129.5/140  92.5%   <-- nearly full   DSP 98/220 44.6%
> bitstream Zynq.runs/impl_1/hw_wrapper.bit  |  XSA noc8_ppufix.xsa
> ```
> **How to verify a width fix reached the netlist** (`V_verify_netlist_and_export.tcl`):
> `open_run synth_1` then count `get_cells -hier -filter {NAME =~ "*ppu_issue_idx_reg*"}`.
> Counting flops in the post-synth netlist cannot be fooled by a stale source, a
> wrong IP repository, or a guard that checked the wrong file — all three of
> which happened before this check existed. Grepping a source file cannot
> establish this; grepping the GENERATED copy is necessary but not sufficient.
>
> **WNS is down to +0.078 ns** (the 64b build was +0.125). BRAM at 92.5% is
> close to the device limit and stride-2's per-channel `half_ram` contributes.
> Freeze paper resource/timing numbers from THIS build, not the 12:55 one.
>
> ### DW STRIDE-2 IS ALREADY IN THIS BITSTREAM (dormant)
> Unintended but useful: the DW IP's duplicate-declaration means
> `dw_banked_window_8x` / `dw_fused_core` / `dw_fused_axis` / `dw_fused_axi` each
> had TWO definitions this build, and the **live-tree copy won** —
> `Synth 8-6157` shows all four synthesized from
> `Zynq.gen\...\bd\hw\Zynq\Zynq.srcs\sources_1\new\`, which carries stride-2.
> The DW IP's own `src\` copy (in `C:\Users\Fahad\ip_repo\dw_fused_axi_1.0\src\`)
> does NOT have stride-2 and lost. So the fabric contains the stride-2 path,
> **dormant** because `ZP_RELU[17]` defaults to 0 and `dw_fused_axi`'s external
> port list is unchanged.
> **Do not rely on this.** Which copy wins is not something to depend on — it
> went the other way on 2026-07-28. The durable fix (repackage the DW IP
> declaring its sources ONCE) is still not done and is now more urgent, because
> the two copies genuinely DIFFER in function rather than just in date.
>
> ### SIM REGRESSION (all pass, exact counts)
> Harness saved as **`sim_1\new\pw_single_oc_axis_instrumented_tb.sv`** (adapted
> from `pw_single_oc_axis_tb.sv`, which already had an `IN_GAP_CYCLES` knob;
> adds per-batch counters for `ppu_valid_in` / `valid_out` / FIFO writes — the
> counters that actually found this). Set `N_OC`, `COUT_RUN` and
> `IN_GAP_CYCLES` at the top. Runs in minutes, no board needed. The DW stride-2
> bench is **`sim_1\new\dw_banked_window_8x_stride2_tb.sv`** (set `C`/`W`/`H`
> at the top; checks stride 1 and 2 against a golden model in one run).
> Both were written in a session scratchpad and deliberately moved into the
> tree — that is how the F1/F2/L/I2/N Tcl scripts were lost.
> ```
> N_OC=8  cout=8  gap=0 : PASS  batches=2880 vin=vout=writes=23040  grp_idx=2880/2880
> N_OC=8  cout=8  gap=8 : PASS  batches=2880                23040
> N_OC=16 cout=16 gap=0 : PASS  batches=2880                46080
> N_OC=8  cout=16 gap=0 : PASS  batches=5760 (2/group)      46080
> N_OC=30 cout=30 gap=8 : PASS  batches=2880                86400   (no regression)
> ```
>
> ### ALSO FIXED — PW `grp_idx` free-run (real, but NOT the cause)
> `S_WAIT_PPU` incremented `grp_idx` outside both branches, so a wait advanced
> the group counter one per CLOCK. Fixed to increment only when a group is
> actually retired. **This did NOT fix the failure** — the fixed core produced
> bit-identical results, verified genuine by diffing the compiled file (not a
> stale-file artifact). Kept as hardening; N_OC=30 still passes with it.
>
> ### DW STRIDE-2 NOW EXISTS (open item #1 of §0.1 is CLOSED in RTL)
> `dw_banked_window_8x.sv` gained a `stride2` port. Group boundaries are at
> multiples of 8 and 8g is always even, so stride-2 centres are ALWAYS lanes
> 0,2,4,6 — the parity never shifts. Each input group yields 4 output pixels
> and one input-group PAIR yields 8, mapping to output x = 8m..8m+7 in order.
> The windower parks the even group's 4 windows in a per-channel `half_ram`
> (27 masked bytes + 4 x-valid bits) and releases them with the odd group's 4
> as ONE DENSE all-valid beat. That keeps `valid_out_vec` all-or-nothing, which
> is what `dw_fused_core.sv:99` (`win_valid = win_vvec[0]`) assumes, so
> conv_mac_array / ppu / the AXIS shell need ZERO change. Only even output rows
> survive (Y = s2_row-1). TLAST is observed on the DW side, so quartering the
> output beat count moves no constant.
> - Plumbed `stride2` via **`ZP_RELU[17]`** (0x10) — no new address, no
>   `component.xml` change, covered by the existing quasi-static XDC multicycle.
>   **Defaults to 0 = stride 1**, so a rebuild is behaviour-neutral.
> - **PRECONDITIONS the driver must enforce (hardware does not check):**
>   `n_groups` EVEN (img_width a multiple of 16) and `cin_run >= 2`. Surrogate
>   widths 1280/640/320 are all multiples of 16.
> - Verified in sim, self-checking against a golden model, all 72 taps per
>   emission, two configs: `C=3,W=32,H=8` and `C=8,W=64,H=16`. Correct emission
>   count, order, halos, padding and channel tags; stride-1 unregressed.
> - **NOT yet built or run on hardware.**
>
> ### THE BOARD IS A ZC702
> Confirmed from `zc702_board.jpg` inside the exported XSA. ZC702 exposes real
> rail measurement over PMBus, so **measured** power and a genuine energy-per-
> frame figure are achievable — not just `report_power` estimates. This matters
> for the paper; see the evidence checklist.
>
> ### PLATFORM REPOINT — THE RECURRING `vitis-comp.json` FAILURE
> `Final_2\vitis-comp.json` had `xsa` / `xsaPathInPlatform` naming
> `hw\hw_cascade3.xsa`, which `update_hw` had DELETED from `Final_2\hw\` (the
> Zynq-root copy survives). Repointed to `hw\noc8.xsa`; backup
> `vitis-comp.json.bak_pre_noc8_repoint`. Verify a repoint by checking the XSA
> actually carries the intended parameters — `unzip -p <x>.xsa hw.hwh | grep N_OC`.
>
> ### MISSING SCRIPTS — STILL GONE
> `F1_bd_cascade_mode.tcl`, `F2_bd_loopback_mode.tcl`, `L_rebuild_both_ips.tcl`,
> `I2_rebuild_pw_guarded.tcl`, `N_rebuild_noc8.tcl` exist NOWHERE on disk (whole
> profile searched). §0.1 references all of them but never recorded a path.
> **F2 is the prerequisite for ever observing the DW one-word offset.** When
> recreated, put them in `Zynq\vivado_scripts\` and record that path here.
>
> ### METHODOLOGY — I WAS WRONG THREE TIMES BEFORE INSTRUMENTING
> Chasing this bug, three plausible hypotheses were each contradicted by data:
> 1. **"PW is input-starved"** — killed by running gap=0: it fails identically
>    with a full-rate input.
> 2. **"`grp_idx` free-run is the cause"** — killed by the fixed core producing
>    bit-identical output.
> 3. **"`out_stall` gates the `ppu_valid_out` count"** — killed by instrumenting:
>    `vout_during_stall=0`, it never happens.
>
> The bug was found in one step once a counter was put on `ppu_valid_in` per
> batch (17 vs the expected 8). **Add the counter before theorising.** Aggregate
> pass/fail told us nothing for three rounds; one ratio told us everything.
> Corollary to §0.1's rules: a clean parameter bisect (30 passes, 16 and 8 fail,
> independent of input rate) localises a bug far faster than reading RTL.
>
> ### DO THIS NEXT
> 1. Rebuild the PW IP from the TCSVT fork, re-synth, re-impl, export XSA.
>    **Check the fix reached the netlist** before trusting any board result.
> 2. Reflash and re-run the surrogate estimate at N_OC=8. Expect HW ~21 -> ~12 ms,
>    frame ~24 ms, ~42 fps.
> 3. Then build stride-2 and set `ZP_RELU[17]` per DW layer in `main.c`.
> 4. Make PW's TLAST observed rather than predicted.

---

## 0.2 (earlier) TARGET CHANGED, PERF MEASURED (2026-07-28)

> ### THE HARDWARE TARGET IS NOW THE SURROGATE NETWORK AT 720p
> The old 8-layer network (§3.2 table, 1435x2048) is **debug-only** from now on —
> kept solely to reproduce faulty outputs. The deployment target is the
> **surrogate**, already defined in `Final_code_2/src/main.c`
> (`surr_build_schedule`, `SURR_IN_H=720`, `SURR_IN_W=1280`):
>
> | # | kind | Cin→Cout | H×W | stride |
> |---|---|---|---|---|
> | 0 | DW | 3→3 | 720×1280 | **2** |
> | 1 | PW | 3→8 | 360×640 | 1 |
> | 2 | DW | 8→8 | 360×640 | **2** |
> | 3 | PW | 8→16 | 180×320 | 1 |
> | 4 | DW | 16→16 | 180×320 | **2** |
> | 5 | PW | 16→64 | 90×160 | 1 |
>
> **Consequence: the S2MM 64MB problem is GONE.** Largest PW output is L1 at
> 230,400 px × cout_rounded = 6.9 MB (at N_OC=30) — far under the 64MB
> `c_sg_length_width=26` cap. No chunking, no TLAST-per-packet, no scatter-gather.
> (The `CASCADE_S2MM_CHUNK` code is still in the driver as a guard; harmless.)
>
> ### MEASURED PERFORMANCE — 29.2 fps, and where the time goes
> Steady state, 720p, 5 frames, warm, console silenced, ±0.01 ms repeatable:
> **34.26 ms/frame = 29.2 fps** (was 10.1 fps before the cache fix below).
>
> WARM per-frame breakdown (reconciles exactly; `outside driver` = 0.00):
> ```
>   HW datapath   21.04 ms  61.4%
>   pack           8.20 ms  23.9%
>   cache          4.60 ms  13.4%
>   weight/param   0.41 ms   1.2%
> ```
> Datapath efficiency vs the 1-beat/cycle floor: 12.42 ms floor / 21.04 measured
> = **59%**.
>
> ### MEASUREMENT METHODOLOGY — THREE WRONG NUMBERS, READ THIS BEFORE QUOTING ANY
> Every early figure this session was wrong, each for a different reason:
> 1. **"22 ms datapath"** — the `chunk done` bracket started AFTER MM2S completed,
>    so it measured only the tail (MM2S completes only once DW has consumed all
>    input, and DW is backpressured by PW, so PW is nearly done by then). It was
>    also inflated by a `cascade_frontier()` buffer scan sitting inside the timed
>    region. Tell-tale: pair 2 reported **155 cycles** for 1.3 MB.
> 2. **"~35 ms end-to-end / 29 fps"** — that was the `HW` sub-bracket presented as
>    an end-to-end figure. The actual measured wall clock at the time was 245 ms.
> 3. **"UART is ~64 us/char, worth ~9 ms/frame"** — wrong by ~100x. Removing the
>    hex dump saved 0.08 ms; the console is ~0.5 us/char. UART only mattered for
>    the cold-start pass, never for steady state.
>
> **Rules that came out of it:**
> - **Sanity-check every timing against a physical bound.** 864,000 output beats
>   cannot move in 0.74 ms — at 1 beat/PL-cycle that is >= 8.64 ms. The driver now
>   prints `beats/PL-cycle`; if it exceeds 1.0 the bracket is wrong.
> - **COLD-START PHASE NUMBERS DO NOT TRANSFER TO STEADY STATE.** Cold `prog` read
>   14.6 ms, warm it is **0.41 ms** (35x) — the cold cost was first-touch DRAM
>   reads of the weight blobs, NOT the ~1,600 AXI-Lite writes as first diagnosed.
>   A whole optimisation (batched / DMA weight loading) was proposed off that and
>   would have bought nothing. Use the WARM breakdown only.
> - Phases must reconcile to the measured mean before any of them is acted on.
>
> ### FIXES APPLIED THIS SESSION (all verified on hardware)
> 1. **DW dropped output beats under PW backpressure** — `dw_fused_axis.sv` out
>    FIFO had `USE_ADV_FEATURES("0000")`, which DISABLES `prog_full`, so the
>    `core_valid_in = !in_empty && !out_prog_full` throttle never engaged and
>    `out_wr_en = core_valid_out && !out_full` silently discarded beats.
>    Measured at H=16: `consumed=8640 written=4707` — 3,933 words destroyed.
>    Fixed with `USE_ADV_FEATURES("0002")` (prog_full only). See the detailed
>    section further down.
> 2. **Driver deadlock at full scale** — the MM2S wait ran to completion *before*
>    the S2MM chunk re-arm loop, so PW filled chunk 0, S2MM stopped accepting,
>    the backpressure chain stalled MM2S, and the code that would re-arm sat
>    after the wait it was blocked in. Now serviced concurrently.
> 3. **Cache maintenance was larger than the datapath** — full-buffer invalidate
>    of `pw_out` before AND after every run (6.9 MB, ~37 ms/frame). The
>    before-invalidate is only needed when the CPU has WRITTEN the buffer (the
>    sentinel fill), and the after-invalidate only needs the bytes actually read.
>    **This single change took 10.1 fps -> 29.2 fps.**
> 4. **`N_OC` 30 -> 8** (building as of 2026-07-28). 8 divides the surrogate's
>    Couts 8/16/64 **exactly**, so `cout_rounded == Cout` at every stage: no
>    padding waste, and **no inter-pair channel repack** (PW's output is then
>    group-major with exactly the channels the next DW consumes).
>    Modelled cycles/group `max(batches*cin, cout_rounded)` x groups:
>    N_OC=30 -> 1,242,000 | N_OC=16 -> 691,200 | **N_OC=8 -> 576,000**.
>    `PW_N_OC` in `main.c` **must** match `CONFIG.N_OC` in the BD — it drives
>    `cout_rounded`, the bank select `oc % N_OC` and the BRAM offset
>    `(oc / N_OC) * Cin`. A mismatch misprograms weights silently.
>    Caveat: pair 2 becomes compute-bound (8 batches x 16 cin = 128 cyc/group vs
>    64 emitted beats). If HW does not improve as modelled, that is where it is,
>    and **N_OC=16 is the hedge**.
>
> ### OPEN ITEMS, IN PRIORITY ORDER
> 1. **Stride-2 DW does not exist.** All three surrogate DW layers are stride-2;
>    `dw_banked_window_8x` is stride-1 only (its own SCOPE comment, ~line 51).
>    The stride-2 path lived in `DW_conv_accel_0`, deleted from the BD 2026-07-09.
>    **The surrogate cannot actually run end-to-end until this is added.** All
>    timing so far runs each DW at its PW's resolution as a cost proxy (valid for
>    timing: the windower's work is input-driven, and DW stays hidden behind PW
>    at every stage — pair 0's real DW is ~345,600 input words ~= 3.5 ms against
>    PW's ~20 ms).
> 2. **`pack` = 8.2 ms/frame (24%)** — software group-major packing.
>    **A camera does NOT deliver group-major** (Bayer / interleaved RGB / planar
>    YUV); converting is a real transpose. Options: a small AXIS repack IP in
>    front of DW, strided/SG DMA, or keep paying it on the CPU. Not free.
> 3. **DW one-word output offset** — still unfixed, still uncharacterised as to
>    cause. Corrupts any model. Not observable in cascade mode; needs BD script
>    F2 (loopback) to see DW output at all. Full characterisation further down.
> 4. PW datapath efficiency is 59% of the beat-rate floor; per-group overhead.
>
> ### CURRENT STATE
> - BD: **CASCADE mode** (`F1_bd_cascade_mode.tcl`). `axi_dma_0` MM2S -> DW -> PW
>   -> `axi_dma_1` S2MM. `axi_dma_0` S2MM and `axi_dma_1` MM2S are DISABLED, so
>   the DW loopback and standalone PW paths do NOT exist. `F2` reverses it.
> - `main.c` flags: `RUN_SURROGATE_ESTIMATE 1`, `DWF_CASCADE_TEST 0`,
>   `DWF_L0_LOOPBACK_TEST 0`, `CASCADE_INSTRUMENT 0`, `PW_N_OC 8`.
> - Entry point: `cnn_run_surrogate_cascade_estimate()` — cold-start pass (with
>   console, NOT the figure) then 5 silent warm frames + phase breakdown.
> - `CASCADE_INSTRUMENT 1` restores the sentinel fill / frontier stall detection /
>   landed scan. It is the only thing that reports WHERE output stopped — turn it
>   back on to debug a stall, off to measure.
> - Building: `N_rebuild_noc8.tcl` -> `hw_noc8.xsa`.
>
> ---

## 0.3 (earlier) RESUME HERE (mid-session handoff, 2026-07-25)

> **DW DATAPATH VALIDATED ON HARDWARE — ~99% CORRECT (2026-07-26).** The real
> **H=2048** L0 loopback runs end-to-end on silicon (`loopback complete`, no
> timeout at full scale) and the output compares against golden L00.BIN at
> **99.08% correct** (81,392 mismatches / 8,816,640 px). The whole datapath —
> vertical 3×3 windowing, MAC, PPU, quantization, weights, group-major order,
> DMA — is CORRECT. **Remaining bug: horizontal edge handling** — ~87% of the
> mismatches are the left 8 cols (first group, ~99% of left-edge px wrong) and
> right 8 cols (last group), plus a small bottom-row (vertical-flush) cluster;
> the interior is 99.88% correct. This is a windower boundary bug (left-continuity
> / `pend_ram` / first-group + flush-slot emission, and right-edge `is_x_valid`
> masking). **DEFERRED per user (2026-07-26): to be reproduced+fixed in the TB
> later** (adapt `dw_banked_window_8x_tb` to C=3/W=1435, or add value-check to
> `dw_fused_axi_tb`) — NOT blocking the fusion cascade.
>
> **Three real bugs were fixed to get the datapath working** (all in the working
> bitstream): (1) predicted→observed TLAST in `dw_fused_axis.sv`; (2) BRAM
> cross-port collision in `dw_banked_window_8x.sv` line buffers (deferred
> write-back, `wb_*`); (3) **the actual blocker** — the windower's config latch
> captured `C_r/G_r/H_r` wrong on silicon (`G_r=0/H_r=0` → null run, `consumed=0`);
> fixed by tracking config UNCONDITIONALLY instead of snapshotting on the
> `start_in` edge. **CRITICAL PROCESS LESSON:** the config fix looked like it
> "didn't work" for TWO rounds because JTAG "Run" only re-downloads the ELF and
> leaves the OLD bitstream in the fabric — always explicitly program
> `Zynq.runs/impl_1/hw_wrapper.bit` (NOT `Final_2/hw/reorder.bit`, a stale 07-24
> copy) before every hardware run. See agent memory [[always-program-fresh-bitstream]].
>
> **NOW IN PROGRESS — the actual fusion goal: cascade DW → PW on-chip.**
> Step 1 (RTL DONE, not yet built): **widened DW `s_axis`/`m_axis` from 32b to
> 64b** in `dw_fused_axis.sv` + `dw_fused_axi.sv` — both FIFOs are now symmetric
> 64b (dropped the 32↔64 serialization; the core was always 64b). This matches
> PW's 64b `s_axis`/`m_axis` so DW→PW needs no width bridge. Backup
> `backup_pre_64bit/`.
>
> **BD-state audit, 2026-07-27 (three corrections to the notes above):**
> 1. **The IP src was NOT synced** — `ip_repo/dw_fused_axi_1.0/src/dw_fused_axi.sv`
>    and `dw_fused_axis.sv` were still the 32b Jul-25 copies while the working-dir
>    files carried the 64b edit. **Synced 2026-07-27** (backups
>    `*.bak_pre_64bit_2026-07-27`). The other four src files were already identical.
>    Exactly the failure mode this doc warns about — re-check the diff before
>    every repackage.
> 2. **`dw_reorder` is already removed from `hw.bd`** — the staged removal Tcl
>    below was evidently run. `dw_fused_axi_0/m_axis` connects straight to
>    `axi_dma_0/S_AXIS_S2MM` (interface net `dw_fused_axi_0_m_axis`). There is no
>    `dw_reorder_0` cell. The widen-vs-remove decision is moot. Residual: a
>    dangling single-endpoint net `dw_fused_axi_0_start_pulse_out` (harmless).
> 3. **`axi_dma_0` needs FOUR width params at 64, not two.** Its memory-map side
>    is also at 32 (`c_m_axi_mm2s_data_width`/`c_m_axi_s2mm_data_width`), and
>    PG021 requires MM width ≥ stream width. `axi_dma_1` (the PW path) is already
>    64 on all four — use it as the reference config. PS HP0/HP1 are 64b and
>    smartconnect_0/1 adapt, so nothing downstream changes.
>
> **64b BUILD IS ON SILICON AND THE DATA IS WRONG (2026-07-27).** The BD/platform
> work is DONE (see below) and the 64b bitstream runs end-to-end: `loopback
> complete`, no timeout, and the debug counters are EXACT —
> `consumed=written=produced=1105920 = C*G*H`, delta `+0` on all three. So TLAST,
> the FIFOs, the DMA and the beat accounting are all correct at 64b. But the
> comparison is **8.47M mismatches / 8.82M px (96% wrong)**, vs 81,392 (0.92%)
> at 32b. The interior — 99.88% correct at 32b — is now ~100% wrong.
> **The corruption is DETERMINISTIC:** across four runs the left-edge count is
> bit-identical (`L(<8)=48802` every time, i.e. the known edge bug is unchanged)
> and the total varies by only ~4,000 px out of 8.4M (0.05%). An early reading
> that called this "non-deterministic" over-weighted that 0.05% jitter — it is a
> systematic data error, not a race.
> **DISPROVEN — the half-rate/cadence theory.** Hypothesis: at 32b, assembling a
> 64b group took 2 MM2S beats, so the core structurally never saw back-to-back
> groups; at 64b it does, exposing a hazard (`pend_ram` / `wb_*` spacing). A
> `DWF_HALF_RATE_DIAG` toggle gating `core_valid_in` was built and run — result
> UNCHANGED (8477601 / 8477980, if anything marginally worse). The core is not
> cadence-sensitive. The throttle is still in `dw_fused_axis.sv` behind a
> `` `define ``; **remove it on the next RTL build.**
> Also ruled out: buffer alignment/DRE (`DDR_SKIP0/1` are 16MB-aligned);
> byte order in `pack_input_group_major` (lane `l` at byte `l`, width-agnostic —
> and an asymmetric-FIFO half-swap would have broken the 32b run too); the
> `dw_fused_timing.xdc` multicycle (it applies only to `reg_zp_relu_reg[*]`, a
> genuinely quasi-static config register, valid at any width); timing (routed
> WNS +0.125ns / WHS +0.015ns, no failing endpoints — thin but met).
> ## CASCADE STALL — ROOT-CAUSED AND FIXED (2026-07-28)
>
> **Cause: DW silently DROPPED output beats under PW's backpressure.**
> `dw_fused_axis.sv`'s out FIFO had `USE_ADV_FEATURES("0000")`, which **disables
> `prog_full`**. `out_prog_full` was therefore tied low forever and the throttle
> `core_valid_in = !in_empty && !out_prog_full` NEVER ENGAGED — the
> `PROG_FULL_THRESH(32)` parameter and its long explanatory comment described a
> mechanism that had never run. The FIFO reached `full` and
> `out_wr_en = core_valid_out && !out_full` discarded beats.
> Harmless while a DMA was the only consumer (it drained faster than DW
> produced); fatal once PW became the consumer, because PW needs ~50 cycles per
> pixel group and backpressures hard.
> Measured at H=16 before the fix: `consumed=8640 written=4707 produced=4707`
> with `out_full_ever=1` — 3,933 of 8,640 words destroyed. PW then starved
> mid-run and hung forever waiting for words that no longer existed. H=8 passed
> only because DW's 2048-deep out FIFO + PW's 2048-deep in FIFO happen to hold
> that run's entire 4,320-word output.
> **Fix: `USE_ADV_FEATURES("0002")`** on the OUT fifo only (enables `prog_full`
> and nothing else; NOT "0707", which would also enable the data-count ports
> that are declared here with width 1). The IN fifo stays "0000" — its
> `prog_full` is unconnected.
> **VERIFIED 2026-07-28, H=16: `consumed=written=produced=8640`, 691,200 B
> landed, TLAST clean, both cores done, `returned 0`.**
> This also retires the cadence/throttle question — the drop was the cause.
> PW was exonerated by simulation (`sim_1/new/pw_single_oc_axis_tb.sv`): it
> completes the exact H=16 config, 86,400 beats and clean TLAST, when given the
> right number of input words.
> **Still open: the DW one-word output offset** (separate bug, unaffected by
> this, and not observable in cascade mode — needs BD script F2).
>
> ---
>
> ## CRITICAL BUILD TRAP — DW IP DECLARES ITS SOURCES TWICE (2026-07-28)
>
> `ip_repo\dw_fused_axi_1.0\component.xml` declares **both**:
> `src/*.sv` **and** `../../Zynq/Zynq.srcs/sources_1/new/*.sv` (which resolves to
> the live working dir). Vivado copies both into the generated area, hits a
> duplicate module definition, and **the stale cached copy wins**:
> ```
> CRITICAL WARNING [filemgmt 20-1741] File 'dw_fused_axis.sv' ... different contents
> CRITICAL WARNING [Synth 8-2490] overwriting previous definition of module dw_fused_axis
>   [...bd/hw/Zynq/Zynq.srcs/sources_1/new/dw_fused_axis.sv:23]
> INFO: [Synth 8-6157] synthesizing module 'dw_fused_axis' [...same stale path...]
> ```
> The stale copies live at `Zynq.gen\sources_1\bd\hw\Zynq\...` and
> `Zynq.ip_user_files\bd\hw\Zynq\...` and were **frozen at Jul 27 11:59** (the
> original 64b build) while `ipshared\c9c1\src\` and the working dir were current.
>
> **TWO EXPERIMENTS WERE THEREFORE INVALID — their conclusions are RETRACTED:**
> 1. The **half-rate throttle** (`DWF_HALF_RATE_DIAG`) build: the throttle was
>    never in the synthesized file. "Cadence hypothesis disproven" is **NOT
>    established** — the test never ran. The cadence question is OPEN.
> 2. The **`USE_ADV_FEATURES("0002")` prog_full** build: likewise never
>    synthesized. `prog_full` is **untested**, not disproven.
> Both produced "bit-identical" results, which was the *evidence of* the stale
> file, misread as evidence about the hypotheses.
> (The PW `out_prog_full` change WAS genuinely in the fabric — PW has only one
> generated copy — so that one really did have no effect.)
>
> **How to check before trusting ANY DW rebuild:**
> ```
> find Zynq.gen Zynq.ip_user_files -name dw_fused_axis.sv   # expect ALL to match the working copy
> grep -n "Synth 8-2490" Zynq.runs/synth_1/runme.log        # duplicate-definition warning
> grep -n "Synth 8-6157.*dw_fused_axis" .../runme.log       # which path was ACTUALLY synthesized
> ```
> Fix applied 2026-07-28: deleted `Zynq.gen\...\bd\hw\Zynq\` and
> `Zynq.ip_user_files\bd\hw\Zynq\` so they regenerate from live source, and
> `L_rebuild_both_ips.tcl` now aborts the build if any generated copy is stale.
> **DURABLE FIX (not yet done): repackage the DW IP declaring its sources ONCE**
> (`src/*.sv` only, drop the `../../Zynq/...` set) so the conflict cannot recur.
>
> **Also:** `I2_rebuild_pw_guarded.tcl` upgrades ONLY the PW IP. Reusing it for a
> DW-side fix leaves DW's output products stale. Use `L_rebuild_both_ips.tcl`.
>
> ---
>
> ## CASCADE IS WIRED AND RUNNING (2026-07-27, later)
>
> **BD is in CASCADE MODE:** `axi_dma_0 MM2S -> dw_fused_axi_0 -> pw_single_oc_axis_axi_0
> -> axi_dma_1 S2MM`, with `axi_dma_0`'s S2MM and `axi_dma_1`'s MM2S channels
> disabled (`c_include_s2mm=0` / `c_include_mm2s=0`). Scripts: `F1_bd_cascade_mode.tcl`
> to enter, **`F2_bd_loopback_mode.tcl` to go back** — the DW→DRAM loopback test
> does NOT exist in cascade mode, so F2 is required before any DW output can be
> observed again (i.e. before fixing the one-word offset).
> Driver: `hw_dw_pw_cascade_l0_l1()` in `Final_code_2/src/main.c`, behind
> `DWF_CASCADE_TEST` (mutually exclusive with `DWF_L0_LOOPBACK_TEST`), with
> `CASCADE_H_OVERRIDE` for fast pipeline-drain tests at small H.
>
> **NO transpose and NO PW RTL change were needed** — see the corrected §4 note.
> **S2MM must be CHUNKED:** `c_sg_length_width=26` caps one transfer at 64MB and
> L01's output is 88,473,600 B (~84.4MB), so the driver arms 32MB chunks.
>
> **PW BUG FOUND AND FIXED (2026-07-27): silent output-beat drop → permanent hang.**
> First cascade runs: DW completed (`STATUS=0x1`, so PW accepted all its input) but
> PW stayed busy forever and S2MM never completed. At H=64 the output frontier froze
> at **65,280 beats = exactly 2176 complete pixel groups of 30**, with the S2MM
> healthy and still armed (`SR=0x00000000`, Halted=0) — the DMA would have taken
> more; PW had nothing to give.
> Mechanism: `pw_single_oc_axis.sv` drove the core's `out_stall` from **`out_full`**.
> The core gates only its PPU *issue* on `out_stall` (`pw_pixel_major_core.sv`
> P_DRAIN ~line 705), but the PPU is a pipeline — by the time `out_full` asserts,
> beats already in flight emerge and hit `out_wr_en = core_valid_out && !out_full`,
> which **drops them**. `produced_cnt` then falls permanently short of
> `total_groups_r`, so `will_last` never fires, TLAST never asserts, S2MM never
> completes, `done_out` never sets. Invisible to `STATUS2`: xpm `overflow` only
> asserts on `wr_en && full`, and `out_wr_en` is already gated by `!out_full`.
> **This is the SAME bug class already fixed in DW** (predicted-count TLAST +
> silent drop). Fix applied: `out_stall` now comes from `prog_full` at
> `OUT_FIFO_DEPTH - 64` so in-flight beats have somewhere to land.
> **Durable follow-up:** make PW's TLAST *observed* (carry the last-beat flag
> through the FIFO with its data, as `dw_plane_run_axis.sv` does) instead of
> predicting a total — the margin fix makes the prediction correct, not robust.
>
> **TRAP — the PW IP does NOT build from this tree.** `pw_single_oc_axis_axi` is
> packaged at `C:\Users\Fahad\TCSVT\ip_repo\pw_single_oc_axis_axi_1.0\`, and its
> `component.xml` file paths resolve **relative to the IP folder**. The files that
> actually synthesize are `<IP>\sources_1\new\*.sv` and `<IP>\src\pw_pixel_major_core.sv`
> — NOT `Zynq\Zynq.srcs\sources_1\new\` and NOT `TCSVT\sources_1\new\`. There are
> three copies of each PW source and only the IP-internal one builds. See agent
> memory [[pw-ip-builds-from-tcsvt]].
> **Also: do NOT run `ipx::create_xgui_files` standalone** (`ipx::open_ipxact_file`
> with no project). Doing so on 2026-07-27 stripped `xgui/*.tcl` down to
> `Component_Name`, breaking the customization GUI ("does not support the current
> part"). Synthesis is unaffected (component.xml keeps all 42 parameters and the BD
> instance is configured from its `.xci`), but re-assert the PW parameters after any
> `upgrade_ip` — defaults are `N_LANES=16, N_OC=20, S_AXIS_DATA_WIDTH=32` and would
> silently break the 64b handoff. `I2_rebuild_pw_guarded.tcl` does this. To repair
> the GUI: Tools → Create and Package New IP → **Edit an existing packaged IP**, which
> opens a real packaging project with a part context.
>
> ---
>
> **ROOT CAUSE CHARACTERIZED (2026-07-27): THE OUTPUT CHANNELS ARE ROTATED BY
> ONE.** A cross-channel/row match matrix on a clean interior row (c=0,h=1024,
> w[64..1371), `[PROBE]` output) gives:
> `got[c0] == ref[c1]` at **95.10%**, `got[c1] == ref[c2]` at **96.40%**,
> `got[c2]` matches nothing (all cells ~1%). Diagonal cells are at chance.
> The hardware computes **fully correct** values and writes them one channel
> slot too low; the last channel receives what falls off the end. This is an
> ORDERING/INDEXING fault, NOT arithmetic:
> - the host convolution model reproduces golden at **1307/1307 = 100.00%**, so
>   weights/bias/mult/shift/zp/padding/requant are all confirmed correct;
> - **no** tap mask matches `got` (all ≤0.68%, chance 0.39%) — not dropped taps,
>   not a window-construction bug, not the left-continuity path;
> - `got` vs raw input = 0.22% — not a pass-through;
> - the "uniform +49.6 bias" that looked arithmetic was an artifact: channel 1's
>   output distribution simply sits higher than channel 0's.
> This also retro-explains the throttle null result — the throttle stretched the
> pipeline in TIME but left the SEQUENCE of channel indices identical, so a
> sequence off-by-one survives it untouched.
> **`dw_reorder` is NOT the cause** (removed in the same change window, so it was
> a suspect): `dw_reorder.sv` verified a pure passthrough —
> `din = {s_axis_tlast, s_axis_tdata}`, `m_axis_tdata = dout[DATA_W-1:0]`,
> symmetric, already `DATA_W=64`. No reordering despite the name.
> **Lead for the TB (not yet confirmed):** per row per channel the windower
> suppresses the `g==0` emission (`emit_valid = s2_valid && (s2_is_flush ||
> (s2_g != 0))`, `dw_banked_window_8x.sv:407`) and adds one flush-slot emission,
> netting exactly G words per channel per row — which is why the COUNTS are
> exact (`consumed=written=produced=C*G*H`, delta +0) while the ORDER is off.
> If that suppression/flush pairing is applied once per row rather than once per
> row PER CHANNEL, exactly one word is dropped at the row start and everything
> rotates by one channel — the observed symptom. `pend_ram`'s read/write
> alignment was traced and looks self-consistent (read `pend_ram[s1_c]` at s1,
> consumed at s2 where the index equals `s2_c`; write `pend_ram[s2_c]`), so the
> fault is more likely in the emit-valid/flush accounting than in pend_ram.
> **Cheap unblock if needed:** the data is CORRECT, just rotated — the DW→PW
> cascade could proceed with a one-channel rotation compensated downstream while
> the real fix is made.
>
> **NEXT:** a software-only corruption probe was added to
> `Final_code_2/src/main.c` (`[PROBE]` lines in `compare_tensor_vs_ref`) that
> shift-scans and lane-permutation-scans one clean mid-row, to tell SHIFTED vs
> PERMUTED-WITHIN-GROUP vs MISCOMPUTED apart. App rebuild only, no bitstream.
> Then: re-run `sim_1/new/dw_fused_axi_tb.sv` (written Jul 25 for the 32b
> interface — widen its `s_axis` driver to 64b). It PASSED at 32b, so if it
> fails at 64b the bug is visible offline in minutes instead of per 30-min build.
>
> **NOTE — `Final_code_2/src/main.c` is the file the app builds, and it has
> DIVERGED from `Zynq.srcs/sources_1/new/main.c`** (the working copy has
> `DWF_L0_LOOPBACK_TEST 0` vs `1`, lacks the `DWF_REG_DBG_*` defines, and has an
> older `compare_group_major_vs_ref` signature). Reconcile them.
>
> **BD steps (DONE 2026-07-27):** repackage `dw_fused_axi` (port widths changed) →
> `update_ip_catalog -rebuild -scan_changes` → `upgrade_ip [get_ips
> hw_dw_fused_axi_0_0]` → set the four `axi_dma_0` widths to 64 → validate/save →
> resynth/reimpl. Scripts A and B (2026-07-27) hold the exact Tcl.
> `main.c` needs NO change for the width (it's byte-count based; L0
> `tensor_bytes` = 3·2048·1440 = 8,847,360, an exact multiple of 8).
> Re-verify the H=2048 loopback at 64b, THEN do the DW→PW cascade
> wiring (DW `m_axis`→PW `s_axis`, PW `m_axis`→S2MM).
>
> --- (historical, how we got here) ---
> The hardware no-output hang was first mis-attributed to a **sim-vs-synth BRAM
> cross-port collision** in the windower's line buffers (a real latent-bug fix
> but NOT the blocker — the config-latch bug was). Chain of proof:
> 1. An integration TB (`sim_1/new/dw_fused_axi_tb.sv`) drives the whole
>    `dw_fused_axi` through AXI-Lite + AXIS at the REAL failing scale (C=3,
>    G=180, W=1435, H=4, MAX_CG_PRODUCT=10800) — the config the standalone
>    windower TB never covered. It **PASSES** in behavioral xsim: out_beats=4320,
>    TLAST seen, done=1, `written=2160`/`produced=4320`. So the RTL LOGIC is
>    correct at real scale.
> 2. The synth RAM-inference report (`Zynq.runs/synth_1/runme.log` ~line 9583)
>    shows `line0_reg`/`line1_reg` (and `pend_ram_reg`) inferred as 7-series
>    **true dual-port** BRAM: Port A writes, Port B reads, **both at the same
>    address (`slot_idx`) in the same cycle** (code: `dw_banked_window_8x.sv`
>    old lines 236-239). On silicon a cross-port same-address read+write returns
>    **INVALID** read data (write-mode settings do NOT apply across ports, UG473);
>    behavioral sim models it as clean read-first and hides it entirely.
>
> **FIX IMPLEMENTED (`dw_banked_window_8x.sv`, not yet built/tested):** deferred
> the line0/line1 write-back by one cycle (`wb_*` block) so the write hits
> `slot_idx-1` while the read hits `slot_idx` — always different addresses, no
> collision, identical stored data. `pend_ram` left as-is: its read (`s1_c`) and
> write (`s2_c`, one stage behind) differ for C≥2, and every DW layer here has
> C≥3, so it never actually collides (only a latent hazard for a hypothetical
> C=1 layer; its write timing is coupled to the tuned DRAIN_CYCLES so don't touch
> it casually). Backup: `backup_pre_collision_fix/`, IP-src synced.
> **Next: re-run `dw_fused_axi_tb` behavioral (must still PASS) → rebuild →
> reflash → H=4 test should now complete; the debug-reg build (if separately
> flashed) is a cross-check (expect HW `written`>0 now).**
> NOTE: post-synth functional sim of `dw_fused_axi` won't elaborate cleanly (no
> standalone synthesized netlist; behavioral RTL + netlist conflict on
> `conv_mac_array`) — didn't need it, the synth *report* gave the answer.

**Files to read, in order:** this doc in full → `dw_fused_axis.sv` (the fix lives
here now — the OBSERVED-TLAST rework, see below) → `dw_plane_run_axis.sv` (the
PROVEN datapath whose TLAST-in-FIFO approach we're mirroring — read its
`out_mem <= {will_last,...}` / `m_axis_tlast = out_dout[64]`) → `main.c` (search
`hw_dw_fused_l0_loopback_test`, `DWF_L0_TEST_H_OVERRIDE`, `DWF_REG_STATUS`) →
agent memory `dw-pw-fusion-project.md` for fuller history if needed.

**Where things stand:** the §5 redesign is synthesized, timing-closed, bitstream
written (2026-07-25 02:37). Boot-tested on real hardware:
- Real L0 (`H=2048, W=1435, C=3`): **MM2S timeout** (input side — separate issue,
  see below, NOT addressed by the current fix).
- Same test, `H` overridden to 1 (`DWF_L0_TEST_H_OVERRIDE=1` in `main.c`,
  diagnostic-only — data garbled by design, doesn't matter for a DMA-completion
  test): **MM2S completes, then hangs on S2MM timeout.**

**The `dw_reorder` per-run-reset theory (previous leading hypothesis) is DISPROVEN.**
The reset fix (a `start_in` port + `fifo_rst` stretch on `dw_reorder.sv`) was
repackaged, BD-wired (`dw_fused_axi_0/start_pulse_out → dw_reorder_0/start_in`),
resynthesized, re-implemented, re-exported, reflashed, and retested 2026-07-25. The
H=1 test **still S2MM-times-out** with the clean-FIFO fix in place. Since `dw_reorder`
is a pure passthrough FIFO that carries TLAST straight through (`din =
{s_axis_tlast, s_axis_tdata}` → `m_axis_tlast = dout[DATA_W]`), a *clean* one still
timing out proves TLAST was never arriving at its input. The bug is UPSTREAM, in
`dw_fused_axi_0` (`dw_fused_axis.sv`). `dw_reorder` is exonerated.

**Root cause (identified 2026-07-25):** `dw_fused_axis.sv` generated `m_axis_tlast`
by *predicting* the total beat count geometrically (`n_groups*cin_run*n_rows*2`) and
asserting TLAST when a read-side counter reached it. The proven `dw_plane_run_axis.sv`
never did this — it stores the last-beat flag IN the output FIFO with the data
(`out_mem <= {will_last, data}`, `m_axis_tlast = out_dout[64]`), so TLAST is welded
to a real beat. The fused windower `dw_banked_window_8x` has a non-trivial emission
count (one-group emission delay, per-row horizontal-flush slot, extra vertical-flush
row, lane-0-gated writes, per-lane edge masking) that the naive product does not
exactly equal. If actual emitted < predicted, `produced_cnt` never reaches the
target, TLAST never fires, S2MM (Direct Register mode, needs TLAST) hangs forever.
This is exactly why the `dw_reorder` reset fix was a no-op: the missing TLAST was
never generated in the first place.

**FIX IMPLEMENTED (2026-07-25), NOT yet built or board-tested:**
- `dw_fused_axis.sv` reworked to OBSERVED TLAST — removed the predicted-count
  machinery (`gc_prod_r`/`total_words_r`/`mult_v`/`produced_cnt`). Added a 1-deep
  output holding register between `u_out_fifo` and `m_axis`. The true last beat is
  the one held when the core has finished writing (`all_written`, set from
  `core_done` which trails the last write by the full 18-cycle datapath latency)
  AND the FIFO behind it is empty (`fifo_empty`). No count is computed; immune to
  the windower's emission-count subtlety. AXI-compliant (tvalid held until tready);
  the only stall is a momentary FIFO-empty before the core finishes, which is rare
  because the FIFO fills faster than it drains (2 write-beats/group, ≤1 read/cycle).
  Backup: `backup_pre_tlast_fix/dw_fused_axis.sv`.
- **IP src synced:** the packaged IP has its OWN copy at
  `C:\Users\Fahad\ip_repo\dw_fused_axi_1.0\src\dw_fused_axis.sv` (component.xml
  references `src/dw_fused_axis.sv`). That copy was updated to match the edit
  (old copy saved as `dw_fused_axis.sv.bak_pre_tlast_2026-07-25`). **The working-dir
  file alone is NOT enough — the IP src copy is what synthesis reads.**
- **`dw_reorder` removal STAGED (Tcl below), not yet run.** Since TLAST now rides
  the out-FIFO correctly and `dw_reorder` is a redundant 32-deep passthrough FIFO
  (the `dw_fused_axis` out FIFO is 2048-deep and already provides
  tvalid/tready/tlast), delete `dw_reorder_0` and wire `dw_fused_axi_0/m_axis`
  straight to `axi_dma_0/S_AXIS_S2MM`. This removes a moving part and the exonerated
  red-herring block. `start_pulse_out` on `dw_fused_axi_0` becomes an unconnected
  output — harmless, left in place (optional later cleanup).

**Clock-association fix (applied 2026-07-25):** the packaged `dw_fused_axi` IP
associated `s_axi_aclk` with only the AXI-Lite bus (`ASSOCIATED_BUSIF = s_axi`),
not the AXIS ports — so the BD threw two `BD 41-967` "not associated to any clock
pin" warnings on `s_axis`/`m_axis` (the old `DW_conv_accel` had all three:
`S00_AXI:s_axis:m_axis`). Cosmetic (single clock drives everything in RTL) but
fixed for parity via `ipx::associate_bus_interfaces -busif {s_axis,m_axis} -clock
s_axi_aclk` during repackage → value is now `s_axi:s_axis:m_axis`, both warnings
gone. Fold this into any future repackage of this IP.

**Rebuild sequence (after the changes above):** repackage `dw_fused_axi_1.0` (its
RTL changed — the IP src is already synced; also apply the clock-association fix
above in the same repackage) → run the `dw_reorder`-removal Tcl →
`upgrade_ip [get_ips hw_dw_fused_axi_0_0]` → resynth → reimpl (same aggressive
directives + `dw_fused_timing.xdc` multicycle, should still apply) → re-export XSA →
Vitis platform re-read → rebuild app → reflash → retest H=1 first (should now
complete S2MM), then real H=2048.

**`dw_reorder` removal Tcl — ALREADY RUN (confirmed 2026-07-27, see §0 audit).
Kept for the record; do not re-run:**
```tcl
open_bd_design [get_files hw.bd]
# delete the cell; its attached interface/clock/reset/start nets go with it
delete_bd_objs [get_bd_cells dw_reorder_0]
# wire the fused DW master straight to the S2MM DMA
connect_bd_intf_net [get_bd_intf_pins dw_fused_axi_0/m_axis] \
                    [get_bd_intf_pins axi_dma_0/S_AXIS_S2MM]
validate_bd_design
save_bd_design
```

**Scope note:** this fix targets the **S2MM/TLAST completion** path (the H=1
symptom). The **real-L0 H=2048 MM2S timeout** is a distinct INPUT-side problem
(MM2S never finishes delivering) — do not expect this fix to resolve it. Chase that
separately once H=1 completes end-to-end: suspects are the input-FIFO backpressure/
throttle (`out_prog_full` gating `core_valid_in`) stalling in a way that wedges the
core so it never drains MM2S, or an MM2S descriptor/length mismatch. If the core
wedges (never reaches `win_done`), note the new observed-TLAST scheme will also
never complete — correct behavior (surfaces the real stall) but a different bug.

**Also cheap, no rebuild:** power-cycle the board and re-run H=1 unmodified — but
per above this alone will NOT fix it (TLAST generation, not FIFO staleness, is the
cause), so this is now only a sanity check, not a candidate fix.

---

## 1. Postmortem — how the 1-D/H=1 blunder happened

`FUSION_HANDOFF_2026-06-28.md`'s network table listed, for L00–L07: `N (active) =
1435`, `N padded(pow2) = 2048`. Those are exactly `W=1435` and `H=2048` — the real
image's width and height, the same numbers the original `dw-l0-hardware-mismatch`
debugging session used and the same numbers the live board just printed
(`[RUN] L00 DW in=[3,2048,1435]`). Whoever wrote that table took the image's real
spatial dimensions and reframed them as a 1-D sequence's "active length" and
"power-of-2-padded length." `2048 = 2^11` looks like a plausible padded length,
which is probably what made the misreading feel right — but nothing in this
codebase pads to a power of two; every padding scheme here rounds up to a multiple
of 8 (`W_padded = (W+7)&~7` → 1440 for W=1435, not 2048). That inconsistency was a
catchable red flag and wasn't caught.

It also directly contradicted a document written **one day earlier**:
`DESIGN_UNDERSTANDING.md` (2026-06-27, explicitly "re-verified against the current
RTL") states plainly the design is a 2D **"MobileNet-style CNN accelerator"** with DW
as **"a direct streaming 3×3 depthwise stencil engine."** `FUSION_HANDOFF_2026-06-28.md`
opens the very next day asserting *"1-D signal model — NOT 2-D images"* with no
citation and no acknowledgment of the contradiction.

Both planning docs actually flagged the assumption themselves, and the flag was
never followed up:
- `DW_PW_FUSION_PLAN.md` §11: *"Confirm the runtime layer blob carries H=1,
  W=length (assumed from PW-group consistency)."*
- `FUSION_HANDOFF_2026-06-28.md` §7: *"Confirm which 3 of the 9 the ARM currently
  places the real taps in."*

Both of those turned out to be false — real L0 has `H=2048` (not 1), and all 9
weight taps are genuinely non-zero (`4 16 -2 -60 -27 10 -128 -42 11`, read off the
board's own console log). ~3 weeks of RTL design, xsim validation, BD wiring,
synthesis, and a bitstream got built on the unconfirmed assumption before this was
caught — triggered only by a runtime DMA timeout during the Stage-1 board test,
not by design review.

**What actually needed chaining DW→PW on-chip was never in question and doesn't
need to change**: `line_buffer_8x` already proves full 3×3 windowing needs zero
DRAM traffic (2 rows of on-chip BRAM per plane). The DRAM round trip in the old
design existed because DW and PW were wired to independent DMA/DRAM paths with the
ARM repacking between them — not because vertical windowing required it. See §4.

### 1.1 Verification protocol (apply from here on, no exceptions)

- [ ] Any claim about model shape (dimensionality, H/W/C, kernel size, stride) must
      cite either **(a)** a direct decode of `0:/MEM/INSTR.BIN` (or the confirmed
      mirror copy, see §3.1), or **(b)** actual board console output. Never a prior
      planning document, never "the plan says."
- [ ] Anything phrased "assumed," "confirm," or "TODO" in this doc is a **blocker**,
      not a note. Do not start RTL/software work downstream of an unconfirmed item —
      confirm it first and record the verification (what was checked, when, result)
      inline where the assumption was.
- [ ] If a new finding contradicts `DESIGN_UNDERSTANDING.md`, resolve the
      contradiction explicitly in writing before proceeding. A newer document does
      not silently override an older one just by being newer.
- [ ] Any BRAM/DSP/timing budget claim must cite the actual
      `Zynq.runs\synth_1\hw_wrapper_utilization_synth.rpt` (or later report), not a
      hand estimate. Hand estimates are fine as a first pass but must be labeled as
      such and checked before being treated as a go/no-go answer.

---

## 2. Corrected model description

Quantized **INT8 MobileNet-style depthwise-separable CNN**, genuine 2D images, on
**Zynq-7020** (XC7Z020: 140× BRAM36 / 630 KiB, 220 DSP, 106k FF). Confirmed by (a)
`DESIGN_UNDERSTANDING.md`'s RTL-reconciled description, (b) every DW layer's decoded
`kernel=3` field (§3), (c) L0's real, non-degenerate 9-weight kernel read off the
board, (d) the golden reference structure (`0:/REF/L%02d.BIN`, per-pixel exact
match required — a 1×3 approximation cannot produce this).

- DW: direct streaming 3×3 depthwise stencil (`line_buffer_8x` + 8×
  `conv_mac_array` + 8× `ppu`).
- PW: SIMD tiled 1×1 pointwise (`pw_pixel_major_core`, `N_LANES=8, N_OC=30`,
  confirmed against the live `hw.bd`).
- 42 layers, alternating DW/PW with 7 residual Adds, 4 stride-2 downsamples.
- Channels per stage: 3→30→60→90→120→180→240 (max 240).
- Spatial dims halve at each stride-2 layer: 2048×1435 → 1024×718 → 512×359 →
  256×180 → 128×90.

---

## 3. Real per-layer table (verified, not hand-copied)

Decoded directly from `C:\Users\Fahad\TCSVT\mem\instr.bin` (1344 bytes = 42 × 32-byte
records, matching the board's own `[SD] Loaded 0:/MEM/INSTR.BIN, size = 1344 bytes`).
Record layout, per `parse_layer_desc_from_instr()` in `main.c`:

| Word | Field |
|---|---|
| w[0] bit4 | depthwise flag (1=DW, 0=PW) |
| w[0] bit3 | relu_en |
| w[0] bit2 | bypass_1x1 |
| w[3] | weight_addr |
| w[4] | params_addr |
| w[5] | H = bits[31:16], W = bits[15:0] |
| w[6] | Cout = bits[31:16], Cin = bits[15:0] |
| w[7] | zp_in = bits[31:24], zp_out = bits[23:16], stride = bits[15:12], kernel = bits[11:8], pad = bits[7:0] |

### 3.1 Provenance note (read before trusting this table)

`TCSVT\mem\instr.bin` is a **local convenience copy**, not the live SD card. It's
trusted here because it matches the board on every checkable field: file size
(1344 bytes), and L0's `Cin=3, H=2048, W=1435, zp_in=0, zp_out=70` all match the
board's own console output and the original `dw-l0-hardware-mismatch` golden facts
exactly. **Re-verify this match if `INSTR.BIN` is ever regenerated** — don't assume
the local copy stays in sync.

### 3.2 Full table

| L | Kind | Cin | Cout | H | W | k | s | pad | zp_in | zp_out |
|---|---|---|---|---|---|---|---|---|---|---|
| 00 | DW | 3 | 3 | 2048 | 1435 | 3 | 1 | 1 | 0 | 70 |
| 01 | PW | 3 | 30 | 2048 | 1435 | 1 | 1 | 0 | 70 | 0 |
| 02 | DW | 30 | 30 | 2048 | 1435 | 3 | 1 | 1 | 0 | 54 |
| 03 | PW | 30 | 60 | 2048 | 1435 | 1 | 1 | 0 | 54 | 0 |
| 04 | DW | 60 | 60 | 2048 | 1435 | 3 | 1 | 1 | 0 | 63 |
| 05 | PW | 60 | 60 | 2048 | 1435 | 1 | 1 | 0 | 63 | 0 |
| 06 | DW | 60 | 60 | 2048 | 1435 | 3 | 1 | 1 | 0 | 70 |
| 07 | PW | 60 | 60 | 2048 | 1435 | 1 | 1 | 0 | 70 | 65 |
| 08 | DW | 60 | 60 | 2048 | 1435 | 3 | **2** | 1 | 0 | 105 |
| 09 | PW | 60 | 90 | 1024 | 718 | 1 | 1 | 0 | 105 | 0 |
| 10 | DW | 90 | 90 | 1024 | 718 | 3 | 1 | 1 | 0 | 73 |
| 11 | PW | 90 | 120 | 1024 | 718 | 1 | 1 | 0 | 73 | 0 |
| 12 | DW | 120 | 120 | 1024 | 718 | 3 | 1 | 1 | 0 | 53 |
| 13 | PW | 120 | 120 | 1024 | 718 | 1 | 1 | 0 | 53 | 0 |
| 14 | DW | 120 | 120 | 1024 | 718 | 3 | 1 | 1 | 0 | 84 |
| 15 | PW | 120 | 120 | 1024 | 718 | 1 | 1 | 0 | 84 | 100 |
| 16 | DW | 120 | 120 | 1024 | 718 | 3 | **2** | 1 | 0 | 73 |
| 17 | PW | 120 | 180 | 512 | 359 | 1 | 1 | 0 | 73 | 0 |
| 18 | DW | 180 | 180 | 512 | 359 | 3 | 1 | 1 | 0 | 56 |
| 19 | PW | 180 | 240 | 512 | 359 | 1 | 1 | 0 | 56 | 0 |
| 20 | DW | 240 | 240 | 512 | 359 | 3 | 1 | 1 | 0 | 68 |
| 21 | PW | 240 | 240 | 512 | 359 | 1 | 1 | 0 | 68 | 0 |
| 22 | DW | 240 | 240 | 512 | 359 | 3 | 1 | 1 | 0 | 49 |
| 23 | PW | 240 | 240 | 512 | 359 | 1 | 1 | 0 | 49 | 99 |
| 24 | DW | 240 | 240 | 512 | 359 | 3 | **2** | 1 | 0 | 63 |
| 25 | PW | 240 | 240 | 256 | 180 | 1 | 1 | 0 | 63 | 0 |
| 26 | DW | 240 | 240 | 256 | 180 | 3 | 1 | 1 | 0 | 58 |
| 27 | PW | 240 | 240 | 256 | 180 | 1 | 1 | 0 | 58 | 0 |
| 28 | DW | 240 | 240 | 256 | 180 | 3 | 1 | 1 | 0 | 67 |
| 29 | PW | 240 | 240 | 256 | 180 | 1 | 1 | 0 | 67 | 49 |
| 30 | DW | 240 | 240 | 256 | 180 | 3 | 1 | 1 | 0 | 62 |
| 31 | PW | 240 | 240 | 256 | 180 | 1 | 1 | 0 | 62 | 0 |
| 32 | DW | 240 | 240 | 256 | 180 | 3 | 1 | 1 | 0 | 68 |
| 33 | PW | 240 | 240 | 256 | 180 | 1 | 1 | 0 | 68 | 64 |
| 34 | DW | 240 | 240 | 256 | 180 | 3 | 1 | 1 | 0 | 75 |
| 35 | PW | 240 | 240 | 256 | 180 | 1 | 1 | 0 | 75 | 0 |
| 36 | DW | 240 | 240 | 256 | 180 | 3 | 1 | 1 | 0 | 45 |
| 37 | PW | 240 | 240 | 256 | 180 | 1 | 1 | 0 | 45 | 42 |
| 38 | DW | 240 | 240 | 256 | 180 | 3 | **2** | 1 | 0 | 78 |
| 39 | PW | 240 | 240 | 128 | 90 | 1 | 1 | 0 | 78 | 0 |
| 40 | DW | 240 | 240 | 128 | 90 | 3 | 1 | 1 | 0 | 68 |
| 41 | PW | 240 | 220 | 128 | 90 | 1 | 1 | 0 | 68 | 67 |

Residual adds (elementwise, ARM-side, layout-agnostic — unaffected by this plan):
L07, L15, L23, L29, L33, L37, L41 (L41's `Cout=220` needs the existing
`cout_rounded` compaction, per `dw-pw-fusion-project` memory).

---

## 4. New dataflow (V2 — real 3×3, on-chip DW→PW, no DRAM round trip)

Goal is unchanged from the original plan: eliminate the DW→DRAM→ARM-transpose→PW
round trip and run DW/PW concurrently on-chip. What changes is *how* — real vertical
windowing is kept, not dropped.

```
DRAM (group-major)
   │  MM2S DMA (for row r: for group g: for channel c: 8 samples)
   ▼
fused DW  ── banked windower (line_buffer_8x's 2-row technique, banked per channel
   │         instead of one shared pair — see §5) → 8× conv_mac_array (reused,
   │         unchanged) → 8× ppu (reused, unchanged)
   │  AXIS master, group-major (for row r: for group g: for channel c: 8 samples)
   ▼
dw_reorder.sv  (thin AXIS FIFO / skid + start-done sequencing — unchanged from V1)
   │  AXIS
   ▼
PW (pw_single_oc_axis) → group-major output
   │  S2MM DMA (group-major)
   ▼
DRAM (group-major)   ← next DW input, or ARM elementwise add at residual layers
```

> **CONFIRMED CORRECT 2026-07-27 (an intermediate note in this doc claimed the
> opposite for ~an hour; that claim was wrong and is retracted).** PW's core
> **does** consume group-major, so the paragraph below stands and the DW→PW
> handoff really is a FIFO, not a transpose. Verified in `pw_pixel_major_core.sv`
> in two independent FSM states: `S_LOAD_FIRST` (line ~474) and the steady-state
> `L_LOADING` (line ~650) each consume **`cin_run` consecutive 64b words per
> pixel group**, writing them to the pixel buffer at `pb_*_wr_addr = ic_idx` /
> `load_ic_idx` = 0..cin_run-1 — i.e. one word per input channel for the same 8
> pixels. That is exactly DW's `(g,c0)(g,c1)(g,c2)(g+1,c0)...` output order.
> `CORE_DATA_W = N_LANES*DATA_WIDTH = 64`, matching DW's widened `m_axis`.
> **No PW RTL change and no transpose buffer are needed for the cascade.**
>
> **BUT — a real discrepancy was found in the SOFTWARE path.** `main.c`'s
> "Input tensor layout note" claims *"The PW DMA hardware expects each transfer
> to be a contiguous tiled-planar slice: `bytes[ic * tile_pixels + p]`"*, and
> `pack_one_tile()` implements exactly that (all of channel 0's `tile_pixels`
> bytes, then channel 1's...). That is **planar**, and it does not match what the
> core ingests. Reading `pack_one_tile` is what produced the retracted claim
> above. Implication: the **DRAM-fed** PW path (`hw_pw_run_full_layer`) appears to
> feed planar data to a group-major core, which would make it wrong for
> `Cin > 1` with multi-group tiles. Not yet verified on hardware — the network
> run currently dies at L00 before PW executes, so it may have been latent.
> **Verify this separately; it does not block the cascade** (which bypasses the
> packing entirely by streaming DW→PW on-chip).

Canonical DRAM layout stays group-major everywhere (this part of the original
insight was correct and independent of the 1-D/2-D error): PW already consumes and
produces group-major, so if DW also emits group-major, the DW→PW handoff is a FIFO,
not a transpose, and residual adds stay a plain elementwise sum with no repack.

**Iteration order is now 3 levels instead of 2** (this is the actual structural
change from V1): `for row r in 0..H-1: for group g in 0..G-1: for channel c in
0..C-1: one 8-sample beat`. V1 only had the inner two levels (`H` was assumed 1).
The per-channel weight/param RAM + `CH_ADDR` load scheme from V1 (`dw_fused_axi.sv`)
is unaffected by this and stays as-is.

---

## 5. Windower redesign — grounded in the existing, proven RTL

Per explicit direction: don't re-derive this from scratch, adapt the module that
already does correct 3×3 windowing (`line_buffer_8x.sv`) rather than extending
`dw_seq_window_8x.sv`'s 1×3 halo scheme into something that ends up reinventing
`line_buffer_8x` anyway.

**What's reusable unchanged:**
- `line_buffer_8x`'s Stage 0–3 window/pad/stride-2 logic (its `line0`/`line1` BRAM
  read/write, row-parity mux, edge masking, stride-2 decimation) is proven correct
  and already emits the exact `window[0:7][2:0][2:0]` / `valid_out_vec` interface
  `conv_mac_array` / `ppu` expect.
- `conv_mac_array`, `ppu` — unchanged, reused as-is (already true in V1).
- `dw_fused_axi.sv`'s `CH_ADDR`-indexed weight/param RAM register map (§6) — this
  part of V1 was never the problem and doesn't need to change.

**What has to change, and why:**

1. **`line_buffer_8x`'s `line0`/`line1` (currently `[0:WORDS_PER_ROW-1]`, one shared
   pair reused sequentially across channels via `start_in` reset) must become
   *banked per channel* — one 2-row buffer per channel, addressed by the same kind
   of running channel counter `dw_fused_axi` already uses for `CH_ADDR`. This is
   required because channels now round-robin *within* each row-group (group-major
   order) instead of running to completion one at a time — each channel's vertical
   (row-to-row) context has to persist independently across that interleaving. Real
   cost: `2 × C × W_padded` bytes instead of a fixed 4 KiB. See §7 for the budget.

2. **The AXIS shell's byte-packer** (`dw_plane_run_axis.sv` Stage P0/P1: valid-lane
   extraction, 128-bit barrel shift/mask, edge compaction) is logically fine and
   reusable *within* one channel's contribution to one group, but the sequencing
   around it needs to change from "finish channel c's whole plane, then start
   channel c+1" to "finish channel c's contribution to group g, hand off to channel
   c+1 for the same group, cycle all C before advancing to group g+1, cycle all G
   before advancing to row r+1."

3. **Register map addition**: V1's `DWF_REG_N_GROUPS` only ever meant "groups per
   row" under the false `H=1` assumption. The real design needs an explicit row
   count. Proposed: add `DWF_REG_N_ROWS` (H) alongside the existing `CIN_RUN`/
   `N_GROUPS`; the datapath FSM gets an outer row loop wrapping the existing
   group/channel loop. Not yet implemented — open item for the RTL pass.

**Status (2026-07-24): implemented, not yet simulated or built.**
`dw_banked_window_8x.sv` written (flat `MAX_CG_PRODUCT`-addressed storage per
§7, no per-beat multiply — a free-running `slot_idx` resets on row-wrap
instead of recomputing `g*C+c`; no "overshoot tick," row transition fires
directly on `c==C-1 && g==G-1`). Wired into `dw_fused_core.sv` in place of
`dw_seq_window_8x` — confirmed by reading `dw_fused_core.sv` in full that
**no pipeline retiming was needed**: it only requires `window`/
`valid_out_vec`/`ch_out` to land together on one clock edge at the
windower's output, not any particular internal latency (the original plan's
"re-check channel-tag timing" open item is resolved this way, not by adding
delay stages). Two new registers added end-to-end (`dw_fused_axi.sv` →
`dw_fused_axis.sv` → `dw_fused_core.sv` → `main.c`): `DWF_REG_N_ROWS` (0x34,
H) and `DWF_REG_IMG_WIDTH` (0x30, real unpadded W — needed because
`N_GROUPS` alone loses the remainder needed for exact edge masking, found
only once the edge-masking logic was actually written, not anticipated up
front). `dw_fused_axis.sv`'s TLAST/beat-count math updated from
`n_groups*cin_run*2` to `n_groups*cin_run*n_rows*2`, pipelined over 2
registered-multiply stages to avoid a long 3-way combinational multiply.

**xsim: PASS (2026-07-24), after 3 real bugs found and fixed via the TB:**
`dw_banked_window_8x_tb.sv` (C=4, G=3, W=19 non-multiple-of-8, H=4) —
`48 emits, all windows correct, consume=48 done_at=48`, zero errors. Bugs
found along the way, in order:
1. Horizontal halo ported `line_buffer_8x`'s "next beat = next group"
   assumption verbatim — invalid under channel-interleaved group-major
   order (next beat is usually a different channel). Rewrote around
   `dw_seq_window_8x`'s proven per-channel one-group-delay scheme, widened
   to buffer 3 vertical rows instead of 1 (plus a left-continuity byte per
   row and full 10-position `line_buffer_8x`-style edge masking — two more
   sub-bugs caught on self-review before this even reached xsim).
2. No drain between a row's flush slot ending and the next row starting —
   late-flush-slot channels' `pend_ram` write-backs (committing 4 cycles
   after their own Stage-0 capture) raced the next row's fresh writes to
   the same address. Confirmed by `done_out` landing exactly 3 emissions
   early, matching the pipeline depth precisely. Fixed with a 4-cycle
   drain state gating Stage-0 capture at every row transition.
3. `slot_idx` (flat vertical-bank address) only incremented in the
   plain per-beat branch, not in the `last_beat_of_group` branch — every
   group boundary within a row caused the next group's first beat to
   reuse the previous group's address, with the offset compounding for
   the rest of the row. This was the actual data-corruption root cause;
   fix #2 (real, and necessary) did not on its own resolve it.
Also caught and fixed a testbench-only bug independent of the DUT: the TB
originally presented one beat per clock edge with no `consume_in`
handshake, which desynced against the DUT's new per-row flush-slot gap
(no analog in `dw_seq_window_8x`, which the TB's stimulus style was
copied from).

**Status (2026-07-25): synthesized, implemented, timing-closed, bitstream written.**
Repackaged `dw_fused_axi` and upgraded the `dw_fused_axi_0` BD instance
(cleared one BD-churn side effect along the way: `dw_reorder_0/rst_n` lost
its auto-derived reset connection during the upgrade, reconnected to the
shared `rst_ps7_0_50M/peripheral_aresetn` net). First synth+impl attempt
came back **failing timing** (WNS -0.441ns, 66 endpoints) — traced via the
per-instance hierarchical utilization report and the routed timing report,
not guessed at:
- `dw_banked_window_8x` itself: confirmed **0 DSPs**, 728 LUTs, 67 RAMB36
  (bigger than the ~38-tile hand estimate in §7 due to width×depth BRAM
  packing inefficiency at this scale, not a bug — still well under the
  device's BRAM limit).
- DSPs hit the device's hard 220/220 ceiling — all 16 of `dw_fused_axi_0`'s
  DSPs traced to its 8× `ppu` instances (`(* use_dsp = "yes" *)`,
  hardcoded in `ppu.sv`, pre-existing/untouched, shared with PW). User's
  call: proceed anyway, priority is functional correctness over DSP
  headroom.
- The 66 failing paths were NOT localized to the new module — they spanned
  `dw_fused_axi_0`'s AXI-Lite config registers, PW's own unmodified `ppu`
  pipeline, the reset synchronizer, and `smartconnect_1`'s internal FIFO,
  i.e. general placement/routing congestion from the utilization jump, not
  a logic-design flaw. Aggressive `place_design`/`phys_opt_design`/
  `route_design` directives (`Explore`/`AggressiveExplore`) cut it to 4
  endpoints, all sourced from `reg_zp_relu_reg` (the `zp_in` config
  register) fanning combinationally into `conv_mac_array`. A
  `set_multicycle_path` constraint on that register (`constrs_1/new/
  dw_fused_timing.xdc`) — legitimate, not a hack, since `zp_in` is quasi-
  static (settles once via AXI-Lite before `start_in`, stable for
  thousands of cycles per run) — closed it completely.
- **Final: WNS +0.246ns, TNS 0.000ns, 0 failing endpoints, WHS +0.011ns.
  All user-specified timing constraints met. Bitstream written**
  (`Zynq.runs/impl_1/hw_wrapper.bit`, 2026-07-25 02:37).

**Remaining before this is confirmed working (do not skip, per §1.1):**
- [x] Re-export XSA, update Vitis platform, rebuild, flash, run the Stage-1 test —
      done, but it doesn't pass yet. **See §0 for the current MM2S/S2MM timeout
      investigation** — leading hypothesis is `dw_reorder_0` missing a per-run
      reset, fix in progress, not yet board-tested.
- Stride-2 DW layers (L08/16/24/38) stay out of fusion scope for v1 (same
  deferral as the original plan, and this part of the reasoning wasn't wrong) —
  kept on the legacy `line_buffer_8x`/`compute_engine`/`dw_plane_run_axis`
  path, untouched.

---

## 6. Register map (V1 unchanged + 2 new registers, added 2026-07-24)

`dw_fused_axi`, AXI4-Lite, 32-bit, base = `DW_BASE_ADDR = 0x43C00000` (reuses the
slot the deleted legacy `DW_conv_accel_0` used to occupy — confirmed via `hw.bd`
address-segment grep, 2026-07-22).

| Off | Name | Dir | Meaning |
|---|---|---|---|
| 0x00 | CTRL | W | bit0=start (pulse); gates on !busy |
| 0x04 | STATUS | R | bit0=done_sticky, bit1=busy |
| 0x08 | CIN_RUN | RW | C (channels) |
| 0x0C | N_GROUPS | RW | G = ⌈W_padded/8⌉ (groups **per row** — see §5 item 3, `N_ROWS` still to be added) |
| 0x10 | ZP_RELU | RW | [7:0]=zp_in [15:8]=zp_out [16]=relu_en |
| 0x14 | CH_ADDR | RW | channel index for weight/param loads |
| 0x18 | W0 | RW | {w3,w2,w1,w0} |
| 0x1C | W1 | RW | {w7,w6,w5,w4} |
| 0x20 | W2 | W | {w8} — commits 9 weights to wram[CH_ADDR] |
| 0x24 | BIAS | RW | INT32 |
| 0x28 | MULT | RW | requant multiplier |
| 0x2C | SHIFT | W | [7:0] — commits {bias,mult,shift} to pram[CH_ADDR] |
| 0x30 | IMG_WIDTH | RW | real, unpadded row width W (samples) |
| 0x34 | N_ROWS | RW | H (real rows) |

All 9 weight taps are real for every DW layer now (§2, §3) — no more "rest multiply
zp→0" assumption from V1 §7.

---

## 7. BRAM / DSP budget (verified against real numbers, not estimated)

`2 × C × W_padded` bytes per DW layer, using the real table in §3.2
(`W_padded = (W+7) & ~7`):

| Layer(s) | C | W_padded | 2×C×W_padded |
|---|---|---|---|
| L00 | 3 | 1440 | 8,640 B |
| L02 | 30 | 1440 | 86,400 B |
| L04/06/08 | 60 | 1440 | 172,800 B |
| L10 | 90 | 720 | 129,600 B |
| L12/14/16 | 120 | 720 | 172,800 B |
| L18 | 180 | 360 | 129,600 B |
| L20/22/24 | 240 | 360 | 172,800 B |
| L26–L38 | 240 | 184 | 88,320 B |
| L40 | 240 | 96 | 46,080 B |

**Worst case: 172,800 bytes (~169 KiB)**, at every mid-stage stride-1 layer where
`C × W_padded ≈ 86,400` stays roughly constant — matches the original plan's own
"balanced across stages" note, so that part of the estimate was right even though
the model framing around it was wrong.

**Checked against the actual current bitstream** (`Zynq.runs\synth_1\
hw_wrapper_utilization_synth.rpt`, 2026-07-09 build): Block RAM Tile
**55/140 (39.3%)** used → 85 tiles (~382 KiB) free. 172,800 B ≈ 38 tiles needed.
**Fits comfortably, ~47 tiles of headroom remaining.**

DSPs are already at **204/220 (92.7%)** in the same report — tight, but that's from
the existing 8× `conv_mac_array` + 8× `ppu` lane count, which doesn't change under
this redesign (only the windower's front-end storage changes). Not a blocker for
this specific change, but flagged as a standing project-wide constraint for any
future additions.

---

## 8. Files

**Reused unchanged:** `conv_mac_array.sv`, `ppu.sv`, `line_buffer_8x.sv` (as the
adaptation source, see §5 — the file itself untouched, still used by the legacy
stride-2 path), `pw_single_oc_axis.sv`, `pw_single_oc_axis_axi.sv`,
`pw_pixel_major_core.sv`, `dw_reorder.sv`.

**New file:** `dw_banked_window_8x.sv` — replaces `dw_seq_window_8x.sv`
(1×3-only, wrong model) in `dw_fused_core.sv`'s instantiation. `dw_seq_window_8x.sv`
itself is left in the tree, unreferenced, not deleted.

**Modified 2026-07-24:** `dw_fused_core.sv` (windower swap, `n_rows`/`img_width`
ports threaded through — no pipeline retiming needed, see §5 status),
`dw_fused_axis.sv` (`n_rows`/`img_width` ports threaded through, TLAST/beat-count
math extended to `n_groups*cin_run*n_rows*2`), `dw_fused_axi.sv` (`DWF_REG_N_ROWS`/
`DWF_REG_IMG_WIDTH` registers added, §6). All three backed up to
`backup_pre_dwfused_test/` first, per [[backup-before-modifying-rtl]].

**Software:** live `main.c` has: `DWF_REG_*` defines (now including
`DWF_REG_N_ROWS`/`DWF_REG_IMG_WIDTH`, written by `hw_dw_fused_l0_loopback_test()`
as of 2026-07-24), `pack_input_group_major`/`unpack_group_major_to_channel_major`/
`compare_group_major_vs_ref`, `hw_dw_fused_l0_loopback_test()` (Stage-1 validation
— its stale "H!=1 unsupported" warning was removed 2026-07-24 now that
`dw_banked_window_8x` genuinely supports H>1; **still untested against real
hardware**, since nothing in §5's redesign has been simulated or synthesized yet),
and a hard guard disabling the now-dead legacy `hw_dw_run_plane()` call path
(`DW_LEGACY_HW_ENABLED`). See `dw-pw-fusion-project` agent memory for the full
session-by-session history.

**Superseded (postmortem reference only, do not use for facts):**
`TCSVT\sources_1\new\DW_PW_FUSION_PLAN.md`, `TCSVT\sources_1\new\
FUSION_HANDOFF_2026-06-28.md`.
