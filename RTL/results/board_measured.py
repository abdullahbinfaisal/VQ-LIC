"""
board_measured.py -- what the board measured, PW-hosted VQ.

Three runs, all 2026-09-05:
  run 1  first silicon for the PW-hosted VQ. Correctness, and the stage
         decomposition. Its T_RANGE was read off #STSUM by hand.
  run 2  reporting bugs fixed, plus a pipelined pass with entropy coding driven
         from inside the analysis cascade's DMA waits.
  run 3  input double-buffered, so the pack runs in those waits too. THIS IS
         THE AUTHORITATIVE RUN: every number below comes from it unless the
         comment says otherwise.

RUN 1's T_RANGE OF 5.275 ms WAS WRONG and is not carried here. It came from a
run whose entropy model was degenerate, so the coder was doing a different job:
mostly 16-bit symbols into an overflowing buffer, where rc_put() discards
instead of storing. Anything quoting 5.275 predates run 2.

RUN CONDITIONS, which bound what may be claimed:
  * SYNTHETIC input. The DIV2K frames are not on the card as raw pixels, so
    ep_synth_frame_planar generated every frame. T_HOST and the VQ search are
    data-independent and stay valid. T_RANGE is weakly data-dependent, and the
    coded SIZE is not a rate result at all.
  * SYNTHETIC codebook (deterministic xorshift). No trained weights exist, so
    no rate or distortion claim can be made from this run. Timing only.
  * serial pass 80 timed frames, 3 warm-up discarded, 20 calibration frames;
    pipelined pass 40 timed frames plus one untimed priming frame.
"""

F_CLK = 100e6

# ---------------------------------------------------------------------------
# 1. Analysis transform, C* = 16-48-64, three DW->PW pairs.  MEASURED
# ---------------------------------------------------------------------------
PAIRS = [  # Cout,  HxW,      groups, PL cycles, ms
    (16, "360x640", 28800, 519919, 5.1992),
    (48, "180x320",  7200, 470288, 4.7029),
    (64,  "90x160",  1800, 357989, 3.5799),
]
T_PL       = 13.4820   # sd 0.0002, cv 0.00%.  Identical across runs 2 and 3.
T_PL_MODEL = 13.4463   # svc_model.walk([16,48,64])

T_PACK  = 5.0712    # sd 0.0024   host input packing
T_PROG  = 1.0936    # sd 0.0001   descriptor/weight programming, 3 pairs
T_CACHE = 0.0005
T_HOST  = 19.6477   # sd 0.0024   the whole analysis phase, CPU + PL
T_GAP   = 0.0005    # unattributed; the decomposition closes to 0.5 us

# T_PACK was 5.0127 in run 2 and 5.0712 in run 3: +1.2% on a step whose only
# code change was gaining a row range. Not explained. It is consistent with
# instruction-cache alignment moving under a rebuild -- 58 us on a 5 ms
# memory-bound NEON loop -- and it does not touch the conclusion, because in
# the deployed path this step is hidden and its duration stops mattering.

# ---------------------------------------------------------------------------
# 2. VQ on the shared PW engine.  MEASURED
# ---------------------------------------------------------------------------
T_VQ_PROG         = 0.4834   # codebook reload, per frame  (#STSUM vqprog)
T_VQ_PROG_ASSUMED = 0.327    # the old guess: 0.15 us per AXI-lite write
US_PER_AXI_WRITE  = 0.4818 * 1000.0 / (32 * 64 + 128 + 6)

T_VQ_RUN   = 2.6402   # driver bracket, serial pass    (#STSUM vqrun)
T_VQ_RUN_P = 2.8219   # driver bracket, pipelined pass -- see note below
T_VQ_ACC   = 2.4505   # accelerator only: start -> DMA idle (last_run_ms)
T_VQ_MODEL = 2.4480   # vq_model.vq_cycles(8, 16) -- compare against T_VQ_ACC

# The search's DRIVER bracket costs 0.18 ms more in a pipelined frame than in a
# serial one, while the accelerator itself is unchanged at 2.4505 ms. The frame
# reads 57,600 extra bytes and ping-pongs both the index and input buffers, so
# the cache state the driver's prep and finish see is not the same. MEASURED in
# both pipelined runs (2.8272, 2.8219), not explained. It is 1% of the frame
# period and it is already inside the reported II.

# Correctness, not timing: the gate everything else stands on.
VQ_MISMATCHES     = 0   # of 57,600 index bytes, vs vqpw_encode_frame()
STEXCL_VIOLATIONS = 0   # DW_PW and VQ_RUN never overlapped, BOTH passes
RC_ROUNDTRIP_BAD  = 0   # of 4,608,000 bytes over 80 frames

# ---------------------------------------------------------------------------
# 3. Entropy coding, M=8 K=16 geometry.  MEASURED
# ---------------------------------------------------------------------------
T_RANGE       = 6.7181   # sd 0.0054, cv 0.08%, min 6.7054, max 6.7315
T_RANGE_EST   = 1.9307 * 1.88   # 3.630 ms, the pre-run estimate
N_SYMBOLS     = 115200          # 14,400 positions x 8 sub-codebooks
NS_PER_SYMBOL = T_RANGE * 1e6 / N_SYMBOLS

RANGE_BYTES   = 7247.6   # mean, serial pass
RANGE_BPP     = 0.0629   # sd 0.0002
FIXED_BPP     = 0.5000
MODEL_ENTROPY = [0.5041, 0.5051, 0.5044, 0.5030,
                 0.5025, 0.5044, 0.5025, 0.5025]   # bits/sym, SYNTHETIC data

# ---------------------------------------------------------------------------
# 4. Pipelined, run 2: entropy coding inside the cascade's DMA waits.
# ---------------------------------------------------------------------------
II_PIPE_V1     = 22.9135   # ms/frame
PRED_II_PIPE   = 22.72     # predicted before run 2: +0.85%

# ---------------------------------------------------------------------------
# 5. Pipelined, run 3: entropy coding AND input packing in those waits.
#    MEASURED, 40 frames, same session as the serial pass above.
# ---------------------------------------------------------------------------
II_PIPE        = 17.9024   # ms/frame, wall time over the pass / frames
PRED_II_DBUF   = 17.89     # predicted before the run: +0.07%

PIPE_DWPW      = 14.5878   # sd 0.0023, the analysis phase with the pack gone
PIPE_ROWS_ANAL = 720       # of 720 input rows packed inside the analysis
PIPE_PACK_EXP  = 0.0002    # ms of packing left exposed
PIPE_IN_ANAL   = 115174.4  # symbols coded in the analysis window
PIPE_IN_SEARCH = 25.6      # spilled into the VQ search window
PIPE_EXPOSED   = 0.0       # symbols left over
PIPE_EXP_MS    = 0.0003    # the 4-byte flush; it depends on the last symbol
PIPE_BYTES     = 7248.3    # matches the serial pass -- the coder is unchanged

# The 25.6 symbols that spilled into the search window are the design working
# as intended rather than a shortfall. The pack runs FIRST because it has a
# hard deadline, so when the window is tight it is the entropy coder that
# spills -- and it spills into the next window rather than into the frame.
# 25.6 of 115,200 is 0.02%: the margin is thin and it held.

INPUT_PREP_MS  = 71.3258   # ep_synth_frame_planar. Stands in for a camera that
                           # does not exist; excluded from II for that reason.

II_PWRPIPE = 22.9631   # edge_power_measure()'s loop. NOT the deployed path any
                       # more: it packs inside the cascade and does no entropy
                       # coding, so it now measures the run-2 topology. See the
                       # power section.

# ---------------------------------------------------------------------------
# 6. Power, duty-matched interleaved A/B.  MEASURED IN RUN 3.
#
# SUPERSEDED BY CONSTRUCTION, PENDING RUN 4. These numbers are a correct
# measurement of the WRONG TOPOLOGY: edge_power_measure() drove a loop that
# packed inside the cascade and never entropy-coded, i.e. the run-2
# pipeline, while the deployed one is run 3. The A/B DELTA survives that --
# both phases ran the same loop and only the VQ differed -- so the < 14.6 mW
# bound stands. The ABSOLUTE mean power does not transfer: it was taken with
# the CPU spinning through DMA waits that the deployed pipeline fills with
# 11.8 ms of packing and entropy coding, and a busy CPU draws more.
#
# Fixed on 2026-09-06: the loop now runs the deployed topology and
# cross-checks its own period against the pipelined pass. Re-run to replace
# PWR_A/PWR_B and every energy figure below.
# ---------------------------------------------------------------------------
PWR_TOPOLOGY_STALE = True   # set False once run 4 refreshes the rails below
PWR_A     = 2.0342   # +- 0.0052 W, pipelined loop with the VQ never started
PWR_B     = 2.0359   # +- 0.0051 W, same loop with the VQ overlapped
PWR_DELTA = +0.0017  # +- 0.0073 W
PWR_BOUND = 0.0146   # 2 SE: the VQ costs LESS THAN this. Not zero -- a bound.
PWR_SAMPLES = 656    # per phase, over 4,896 frames per phase
PWR_NOTE  = ("regulator OUTPUT power: excludes conversion losses and the "
             "unmonitored 5 V USB rail. NOT 12 V input power.")

# Per-rail means, W.  (name, group, A, B)
PWR_RAILS = [
    ("VCCINT",   "PL",   0.2325, 0.2322),
    ("VCCPINT",  "PS",   0.3591, 0.3595),
    ("VCCAUX",   "PL",   0.0283, 0.0292),
    ("VCCPAUX",  "PS",   0.1111, 0.1158),
    ("VCCADJ",   "MISC", 0.0406, 0.0363),
    ("VCC1V5PS", "DDR",  0.4737, 0.4747),
    ("VCC_MIO",  "PS",   0.0111, 0.0110),
    ("VCCBRAM",  "PL",   0.0088, 0.0075),
    ("VCC3V3",   "MISC", 0.7480, 0.7450),
    ("VCC2V5",   "MISC", 0.0210, 0.0247),
]

# THREE runs, three signs: -0.0046, +0.0048, +0.0017 W, every one inside 2 SE
# of zero. That is not a puzzle to resolve, it is what "not resolved" looks
# like, and three independent draws make the point better than any one of them.
#
# CAVEAT THAT MATTERS FOR ENERGY. edge_power_measure() drives its own loop, and
# that loop still packs inside the cascade and never entropy-codes. So the mean
# power above was measured while the CPU spins through the DMA waits, and the
# deployed path now fills those waits with work. Energy per frame at 17.9024 ms
# is therefore DERIVED from a power figure taken under a lighter CPU load: the
# per-frame work is identical and the frame is shorter, so the direction is
# right, but the magnitude is not measured. Pointing the A/B at the deployed
# loop is the fix, and it has not been done.


def fps(ms):
    return 1000.0 / ms


SERIAL_STAGES = [("analysis: host packing", T_PACK,    "CPU"),
                 ("analysis: programming",  T_PROG,    "CPU"),
                 ("analysis: PL cascade",   T_PL,      "PL "),
                 ("VQ codebook reload",     T_VQ_PROG, "CPU"),
                 ("VQ search",              T_VQ_RUN,  "PL "),
                 ("entropy coding",         T_RANGE,   "CPU")]
II_SERIAL = sum(t for _n, t, _w in SERIAL_STAGES)
PL_BUSY   = T_PL + T_VQ_RUN_P          # the floor: one engine, two jobs
CPU_BUSY  = T_PACK + T_PROG + T_VQ_PROG + T_RANGE


def report():
    print("BOARD, PW-hosted VQ -- 2026-09-05 run 3")
    print("=" * 72)
    print("Correctness gates")
    print("  PW VQ vs software reference : %d mismatches of 57,600" % VQ_MISMATCHES)
    print("  range coder round trip      : %d mismatches of 4,608,000" % RC_ROUNDTRIP_BAD)
    print("  DW_PW / VQ_RUN exclusion    : %d violations, both passes"
          % STEXCL_VIOLATIONS)

    print()
    print("Model and estimate against board")
    print("  %-26s %9s %9s %8s" % ("quantity", "predicted", "measured", "error"))
    for name, pred, meas, kind in (
            ("PL analysis",              T_PL_MODEL,        T_PL,      "MODEL"),
            ("VQ search (accelerator)",  T_VQ_MODEL,        T_VQ_ACC,  "MODEL"),
            ("VQ codebook reload",       T_VQ_PROG_ASSUMED, T_VQ_PROG, "guess"),
            ("entropy coding",           T_RANGE_EST,       T_RANGE,   "guess")):
        print("  %-26s %9.4f %9.4f %+7.2f%%  %s"
              % (name, pred, meas, 100.0 * (meas - pred) / pred, kind))
    print("  The two MODEL rows are the frozen service model, validated on")
    print("  silicon for the VQ shape as well as for the convolution shapes it")
    print("  was frozen on. The two guess rows were never models, and both were")
    print("  optimistic: %.3f us per AXI-lite write against the assumed 0.15,"
          % US_PER_AXI_WRITE)
    print("  and %.1f ns per coded symbol over %d symbols."
          % (NS_PER_SYMBOL, N_SYMBOLS))

    print()
    print("Where a frame goes, SERIAL (every row MEASURED)")
    for nm, t, w in SERIAL_STAGES:
        print("  %-24s %7.4f ms  %s  %5.1f%%" % (nm, t, w, 100.0 * t / II_SERIAL))
    print("  %-24s %7.4f ms       -> %.2f fps" % ("TOTAL", II_SERIAL, fps(II_SERIAL)))
    print("  PL busy %7.4f ms    CPU busy %7.4f ms" % (PL_BUSY, CPU_BUSY))

    print()
    print("Three topologies, all measured on the same board")
    rows = [("serial", II_SERIAL, "every stage back to back"),
            ("entropy hidden", II_PIPE_V1, "run 2: coder in the DMA waits"),
            ("entropy + pack hidden", II_PIPE, "run 3: input double-buffered"),
            ("floor", PL_BUSY, "PL busy: one engine, cannot go below")]
    for nm, ms, note in rows:
        print("  %-22s %7.4f ms -> %5.2f fps   %s" % (nm, ms, fps(ms), note))
    print("  serial -> deployed: %.3fx. Of the %.4f ms removed, %.4f was the"
          % (II_SERIAL / II_PIPE, II_SERIAL - II_PIPE, T_RANGE))
    print("  entropy stage and %.4f the input pack." % T_PACK)
    print("  %.4f ms above the floor: programming %.4f + reload %.4f, both of"
          % (II_PIPE - PL_BUSY, T_PROG, T_VQ_PROG))
    print("  which write the engine's own registers and weight BRAM while it is")
    print("  idle. Neither can overlap the thing it is configuring.")

    print()
    print("What the two CPU jobs did with the analysis window")
    print("  window (PL cascade)          %7.4f ms" % T_PL)
    print("  asked of it: packing %.4f + entropy %.4f = %.4f ms (%.0f%% full)"
          % (T_PACK, T_RANGE, T_PACK + T_RANGE,
             100.0 * (T_PACK + T_RANGE) / T_PL))
    print("  input rows packed in it      %d of %d  (%.0f%%)"
          % (PIPE_ROWS_ANAL, 720, 100.0 * PIPE_ROWS_ANAL / 720))
    print("  packing left exposed         %7.4f ms" % PIPE_PACK_EXP)
    print("  symbols coded in it          %9.1f of %d  (%.2f%%)"
          % (PIPE_IN_ANAL, N_SYMBOLS, 100.0 * PIPE_IN_ANAL / N_SYMBOLS))
    print("  symbols spilled to the search%9.1f            (%.2f%%)"
          % (PIPE_IN_SEARCH, 100.0 * PIPE_IN_SEARCH / N_SYMBOLS))
    print("  symbols left exposed         %9.1f" % PIPE_EXPOSED)
    print("  entropy left exposed         %7.4f ms against %.4f serial: %.2f%% hidden"
          % (PIPE_EXP_MS, T_RANGE, 100.0 * (T_RANGE - PIPE_EXP_MS) / T_RANGE))

    print()
    print("  What the overlap cost, measured")
    implied_pl = PIPE_DWPW - T_PROG - T_CACHE
    print("    analysis phase  %7.4f -> %7.4f ms: the pack is gone from it"
          % (T_HOST, PIPE_DWPW))
    print("    implied PL time %7.4f -> %7.4f ms  (%+.4f, %+.2f%%)"
          % (T_PL, implied_pl, implied_pl - T_PL,
             100.0 * (implied_pl - T_PL) / T_PL))
    print("      This is the whole cost of the interference: DMA-completion")
    print("      detection delayed by up to one slice, plus 5.5 MB of CPU DDR")
    print("      traffic running inside the transfers it hides in. 12 us. The")
    print("      contention worry was the main risk to this change and it did")
    print("      not materialise.")
    print("    VQ driver       %7.4f -> %7.4f ms  (%+.4f, cache state)"
          % (T_VQ_RUN, T_VQ_RUN_P, T_VQ_RUN_P - T_VQ_RUN))

    print()
    print("  Checks that this is real and not an accounting artefact")
    print("   1. II is wall time over the pass divided by frames. Not a sum of")
    print("      stages, so it cannot assume the stages are disjoint.")
    print("   2. Coded output unchanged: %.1f B serial, %.1f B pipelined, and"
          % (RANGE_BYTES, PIPE_BYTES))
    print("      the round trip is exact in both. A pipeline that dropped or")
    print("      corrupted work would not produce the same bitstream.")
    print("   3. #STSUM pack_ms falls to ~0.0007 in the pipelined pass: the")
    print("      cascade is no longer packing, which is what was claimed.")
    print("   4. The predictions were recorded before each run: %.2f vs %.4f"
          % (PRED_II_PIPE, II_PIPE_V1))
    print("      for run 2, %.2f vs %.4f for run 3 (+%.2f%%)."
          % (PRED_II_DBUF, II_PIPE, 100.0 * (II_PIPE - PRED_II_DBUF) / PRED_II_DBUF))

    print()
    print("Rate (SYNTHETIC codebook -- NOT a rate result)")
    print("  %.1f B/frame, %.5f bpp against %.4f fixed: %.1f%% smaller."
          % (RANGE_BYTES, RANGE_BPP, FIXED_BPP,
             100.0 * (1.0 - RANGE_BPP / FIXED_BPP)))
    print("  This says the coder works. It says nothing about the codec: the")
    print("  indices come from a random codebook over a synthetic latent.")

    print()
    print("Power and energy")
    print("  A, no VQ  %.4f W      B, with VQ  %.4f W   (run 2)" % (PWR_A, PWR_B))
    print("  delta %+.4f W -- NOT RESOLVED, |delta| < 2 SE." % PWR_DELTA)
    print("  Report the VQ as costing LESS THAN %.1f mW. Not as zero, and not"
          % (PWR_BOUND * 1000.0))
    print("  as positive: run 1 gave this delta the other sign.")
    print("  energy/frame, serial   : %6.2f mJ at %7.4f ms  DERIVED"
          % (PWR_B * II_SERIAL, II_SERIAL))
    print("  energy/frame, deployed : %6.2f mJ at %7.4f ms  DERIVED"
          % (PWR_B * II_PIPE, II_PIPE))
    print("  BOTH derived, and the second one needs care: the A/B loop still")
    print("  packs inside the cascade and never entropy-codes, so its mean")
    print("  power was taken while the CPU spins through the DMA waits that the")
    print("  deployed path now fills with work. Same work per frame in a %.0f%%"
          % (100.0 * (1.0 - II_PIPE / II_SERIAL)))
    print("  shorter frame, so the direction is right and the magnitude is not")
    print("  measured. Pointing edge_power_measure() at the deployed loop is the")
    print("  fix, and it has not been done.")
    print("  %s" % PWR_NOTE)


if __name__ == "__main__":
    report()
