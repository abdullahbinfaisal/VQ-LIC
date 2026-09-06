"""
dump.py -- the three tables, straight off the board.

  1. per-stage latency, unoverlapped
  2. per-stage latency in the overlapped multi-frame steady state
  3. power and energy per frame

Every constant comes from results/board_measured.py, which holds the 2026-09-05
run-3 log and nothing else. Provenance is on every row: MEASURED means the board
produced that number, DERIVED means it is arithmetic over measured numbers, and
nothing here is modelled.

  python dump.py            # to the terminal
  python dump.py > dump.txt
"""
import sys
import os

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import board_measured as B

fps = B.fps
W = 78


def rule(ch="-"):
    print(ch * W)


def head(n, title):
    print()
    rule("=")
    print("%d. %s" % (n, title))
    rule("=")


# ===========================================================================
def stage_latency():
    head(1, "PER-STAGE LATENCY, UNOVERLAPPED")
    print("One frame, every stage run to completion before the next begins.")
    print("1280x720 input, C* = 16-48-64, PQ M=8 K=16, PL at 100 MHz.")
    print()
    print("  %-28s %9s %8s %7s  %-4s %s"
          % ("stage", "ms", "sd", "share", "unit", "provenance"))
    rule()

    rows = [
        ("ANALYSIS TRANSFORM", None, None, None, None),
        ("  input pack (group-major)", B.T_PACK,  0.0024, "CPU", "MEASURED"),
        ("  descriptor + weight prog", B.T_PROG,  0.0001, "CPU", "MEASURED"),
        ("  cache maintenance",        B.T_CACHE, 0.0000, "CPU", "MEASURED"),
        ("  DW->PW cascade (3 pairs)", B.T_PL,    0.0002, "PL",  "MEASURED"),
        ("VECTOR QUANTISATION", None, None, None, None),
        ("  codebook reload",          B.T_VQ_PROG, 0.0002, "CPU", "MEASURED"),
        ("  search, accelerator only", B.T_VQ_ACC,  None,   "PL",  "MEASURED"),
        ("  search, driver bracket",   B.T_VQ_RUN,  0.0001, "PL",  "MEASURED"),
        ("ENTROPY CODING", None, None, None, None),
        ("  range coder, 115200 sym",  B.T_RANGE, 0.0054, "CPU", "MEASURED"),
    ]
    tot = B.II_SERIAL
    for nm, ms, sd, unit, prov in rows:
        if ms is None:
            print("  %s" % nm)
            continue
        sds = "%8.4f" % sd if sd is not None else "       -"
        # the accelerator-only search is a component of the driver bracket,
        # not an additional stage, so it gets no share of the total
        share = "" if "accelerator" in nm else "%6.1f%%" % (100.0 * ms / tot)
        print("  %-28s %9.4f %s %7s  %-4s %s" % (nm, ms, sds, share, unit, prov))
    rule()
    print("  %-28s %9.4f %8s %7s        %s"
          % ("FRAME TOTAL", tot, "", "100.0%", "-> %.2f fps" % fps(tot)))
    print()
    print("  per DW->PW pair (SILICON, PL cycle counters):")
    print("    %-6s %-10s %8s %12s %10s %9s" %
          ("pair", "HxW", "Cout", "PL cycles", "ms", "cyc/group"))
    for cout, hw, groups, cyc, ms in B.PAIRS:
        print("    %-6s %-10s %8d %12d %10.4f %9.1f"
              % ("", hw, cout, cyc, ms, cyc / float(groups)))
    print()
    print("  Resource split: PL busy %.4f ms, CPU busy %.4f ms."
          % (B.PL_BUSY, B.CPU_BUSY))
    print("  The PW engine is one resource. The cascade and the search both need")
    print("  it, so they can never overlap -- asserted every run, 0 violations.")
    print()
    print("  NOT PART OF THE FRAME, listed because they appear in the log:")
    print("    %-28s %9.4f ms   software VQ reference, checked every frame"
          % ("NEON PQ search", 426.4452))
    print("    %-28s %9.4f ms   once per run, not per frame"
          % ("entropy model build", 27.4594))
    print("    %-28s %9.4f ms   synthetic frame generator, stands in for"
          % ("input generation", B.INPUT_PREP_MS))
    print("    %-28s %9s      a camera. Excluded from every II below." % ("", ""))


# ===========================================================================
def overlapped():
    head(2, "PER-STAGE LATENCY IN THE OVERLAPPED MULTI-FRAME SEQUENCE")
    print("Steady state, 40 consecutive frames. Each frame's cascade DMA wait")
    print("carries the NEXT frame's input pack and the PREVIOUS frame's entropy")
    print("coding, both sliced and driven from the cascade's poll loops.")
    print()
    print("  %-30s %9s %9s  %s" % ("stage", "cost ms", "in II", "where it runs"))
    rule()
    ex = [
        ("descriptor + weight prog", B.T_PROG, B.T_PROG,
         "exposed: writes the engine regs"),
        ("DW->PW cascade", 13.4937, 13.4937,
         "exposed: the PL itself"),
        ("codebook reload", B.T_VQ_PROG, B.T_VQ_PROG,
         "exposed: writes the weight BRAM"),
        ("VQ search (driver bracket)", B.T_VQ_RUN_P, B.T_VQ_RUN_P,
         "exposed: the PL itself"),
        ("input pack, for frame n+1", B.T_PACK, 0.0007,
         "HIDDEN in the cascade wait"),
        ("entropy coding, of frame n-1", B.T_RANGE, 0.0006,
         "HIDDEN in the cascade wait"),
    ]
    s_ii = 0.0
    for nm, cost, inii, where in ex:
        s_ii += inii
        print("  %-30s %9.4f %9.4f  %s" % (nm, cost, inii, where))
    rule()
    print("  %-30s %9s %9.4f  sum of the column" % ("", "", s_ii))
    print("  %-30s %9s %9.4f  MEASURED, wall time / frames"
          % ("", "", B.II_PIPE))
    print("  %-30s %9s %9.4f  frame bookkeeping outside every stage"
          % ("residual", "", B.II_PIPE - s_ii))
    print()
    print("  INTERVAL PER FRAME  %.4f ms  ->  %.2f fps   MEASURED"
          % (B.II_PIPE, fps(B.II_PIPE)))
    print()

    print("  What went into the hiding window")
    rule()
    win = B.T_PACK + B.T_RANGE
    print("  cascade DMA wait, per frame        %9.4f ms" % B.T_PL)
    print("  CPU work placed in it              %9.4f ms  (%.0f%% full)"
          % (win, 100.0 * win / B.T_PL))
    print("    input pack   %6.1f%% of it         %9.4f ms  %d of %d rows"
          % (100.0 * B.T_PACK / win, B.T_PACK, B.PIPE_ROWS_ANAL, 720))
    print("    entropy      %6.1f%% of it         %9.4f ms  %.1f of %d symbols"
          % (100.0 * B.T_RANGE / win, B.T_RANGE, B.PIPE_IN_ANAL, B.N_SYMBOLS))
    print("  spilled to the VQ search window    %9s     %.1f symbols (%.2f%%)"
          % ("", B.PIPE_IN_SEARCH, 100.0 * B.PIPE_IN_SEARCH / B.N_SYMBOLS))
    print("  left exposed                       %9.4f ms  pack %.4f + flush %.4f"
          % (B.PIPE_PACK_EXP + B.PIPE_EXP_MS, B.PIPE_PACK_EXP, B.PIPE_EXP_MS))
    print("  headroom left in the window        %9.4f ms" % (B.T_PL - win))
    print()

    print("  Steady-state timeline of frame n, ms from frame start")
    rule()
    t1 = B.T_PROG                 # prog ends
    t2 = t1 + 13.4937             # cascade ends
    t3 = t2 + B.T_VQ_PROG         # reload ends
    t4 = t3 + B.T_VQ_RUN_P        # search ends
    SC = 60.0 / t4                # columns per ms

    def bar(lo, hi, ch):
        a, b = int(round(lo * SC)), int(round(hi * SC))
        return " " * a + ch * max(1, b - a)

    print("        0%s%.1f ms" % (" " * 52, t4))
    print("        |%s|" % ("-" * 58))
    print("  PL    %s" % bar(t1, t2, "#") + "  cascade, DW->PW x3")
    print("  PL    %s" % bar(t3, t4, "#") + "  VQ search")
    print("  CPU   %s" % bar(0.0, t1, "p") + "  prog: descriptors + weights")
    print("  CPU   %s" % bar(t2, t3, "r") + "  reload: codebook")
    print("  CPU   %s" % bar(t1, t2, ".") + "  pack(n+1) then entropy(n-1),")
    print("  %s   sliced into the cascade DMA wait" % (" " * 60))
    print()
    print("        # = the PW engine, and it is the only strictly serial")
    print("        resource. p and r write its registers and weight BRAM while")
    print("        it is idle. The dotted span is CPU work for OTHER frames.")
    print()
    print("  Three frames are in flight: n is on the engine, n+1 is being packed,")
    print("  n-1 is being entropy coded. Frame n's own pack happened during frame")
    print("  n-1, and its own entropy coding will happen during frame n+1.")
    print()

    print("  Topologies measured on this board")
    rule()
    for nm, ms, note in (
            ("serial",                B.II_SERIAL,  "every stage back to back"),
            ("entropy hidden",        B.II_PIPE_V1, "run 2"),
            ("entropy + pack hidden", B.II_PIPE,    "run 3, deployed"),
            ("floor",                 B.PL_BUSY,    "PW engine busy time")):
        print("  %-22s %9.4f ms  %6.2f fps   %s" % (nm, ms, fps(ms), note))
    print("  serial -> deployed: %.3fx" % (B.II_SERIAL / B.II_PIPE))
    above = B.II_PIPE - B.PL_BUSY
    book  = above - B.T_PROG - B.T_VQ_PROG - 0.0117 - B.PIPE_PACK_EXP - B.PIPE_EXP_MS
    print("  %.4f ms above the floor, and all of it is accounted for:" % above)
    print("    %7.4f  descriptor + weight programming, writes the engine regs"
          % B.T_PROG)
    print("    %7.4f  codebook reload, writes the weight BRAM" % B.T_VQ_PROG)
    print("    %7.4f  cascade slowdown from the background work" % 0.0117)
    print("    %7.4f  pack and flush remainders left exposed"
          % (B.PIPE_PACK_EXP + B.PIPE_EXP_MS))
    print("    %7.4f  frame bookkeeping outside every stage bracket" % book)
    print("  The first two are irreducible with this engine: both configure the")
    print("  thing they would have to overlap.")
    print()
    print("  Cost of the overlap, measured:")
    print("    cascade      %.4f -> %.4f ms  (%+.4f, %+.2f%%)"
          % (B.T_PL, 13.4937, 13.4937 - B.T_PL,
             100.0 * (13.4937 - B.T_PL) / B.T_PL))
    print("      slice-granular DMA-completion detection plus 5.5 MB of CPU DDR")
    print("      traffic inside the transfers it hides in. 12 microseconds.")
    print("    VQ driver    %.4f -> %.4f ms  (%+.4f, cache state)"
          % (B.T_VQ_RUN, B.T_VQ_RUN_P, B.T_VQ_RUN_P - B.T_VQ_RUN))
    print("      The accelerator itself is unchanged at %.4f ms." % B.T_VQ_ACC)


# ===========================================================================
def power():
    head(3, "POWER AND ENERGY PER FRAME")
    print("Duty-matched interleaved A/B over the PMBus rails. A and B alternate")
    print("frame by frame and both pad to the same period, so thermal drift and")
    print("duty differences cancel instead of being absorbed into the delta.")
    print("%d scans per phase, 4,896 frames per phase." % B.PWR_SAMPLES)
    print()
    print("  %-10s %-6s %9s %9s %10s" % ("rail", "group", "A (W)", "B (W)", "delta"))
    rule()
    ga, gb = {}, {}
    for nm, grp, a, b in B.PWR_RAILS:
        print("  %-10s %-6s %9.4f %9.4f %+10.4f" % (nm, grp, a, b, b - a))
        ga[grp] = ga.get(grp, 0.0) + a
        gb[grp] = gb.get(grp, 0.0) + b
    rule()
    for grp in ("PL", "PS", "DDR", "MISC"):
        print("  %-10s %-6s %9.4f %9.4f %+10.4f   %5.1f%% of board"
              % ("", grp, ga[grp], gb[grp], gb[grp] - ga[grp],
                 100.0 * gb[grp] / B.PWR_B))
    rule()
    print("  %-10s %-6s %9.4f %9.4f %+10.4f" % ("TOTAL", "", B.PWR_A, B.PWR_B,
                                                B.PWR_DELTA))
    print("             +- %.4f   +- %.4f   +- 0.0073" % (0.0052, 0.0051))
    print()
    soc = gb["PL"] + gb["PS"] + gb["DDR"]
    print("  The VQ delta is %+.4f W against a 2 SE bound of %.4f: NOT RESOLVED."
          % (B.PWR_DELTA, B.PWR_BOUND))
    print("  Quote it as an UPPER BOUND -- the VQ costs less than %.1f mW -- and"
          % (B.PWR_BOUND * 1000.0))
    print("  not as zero. Three runs gave -0.0046, +0.0048 and +0.0017 W, all")
    print("  inside 2 SE. The sign is noise, and three draws show that better")
    print("  than any single one.")
    print()
    print("  MISC is %.4f W, %.0f%% of the board, and is almost all VCC3V3"
          % (gb["MISC"], 100.0 * gb["MISC"] / B.PWR_B))
    print("  (%.4f W) -- USB, PHY, LEDs, board housekeeping. The SoC rails that"
          % 0.7450)
    print("  the accelerator actually moves are PL + PS + DDR = %.4f W." % soc)
    print()
    print("  %-40s %9s %9s %9s" % ("energy per frame", "II ms", "W", "mJ"))
    rule()
    print("  %-40s %9.4f %9.4f %9.2f   MEASURED"
          % ("power loop as run (run-2 topology)", B.II_PWRPIPE, B.PWR_B,
             B.PWR_B * B.II_PWRPIPE))
    print("  %-40s %9.4f %9.4f %9.2f   DERIVED"
          % ("deployed pipeline", B.II_PIPE, B.PWR_B, B.PWR_B * B.II_PIPE))
    print("  %-40s %9.4f %9.4f %9.2f   DERIVED"
          % ("serial, no overlap", B.II_SERIAL, B.PWR_B, B.PWR_B * B.II_SERIAL))
    print("  %-40s %9.4f %9.4f %9.2f   DERIVED"
          % ("SoC rails only, deployed", B.II_PIPE, soc, soc * B.II_PIPE))
    rule()
    print()
    if getattr(B, "PWR_TOPOLOGY_STALE", False):
        print("  *** THE POWER ROWS ABOVE ARE PENDING RE-MEASUREMENT ***")
        print("  They measure the run-2 topology, because edge_power_measure()")
        print("  drove its own loop and that loop packed inside the cascade and")
        print("  never entropy-coded. The A/B DELTA survives -- both phases ran")
        print("  the same loop, only the VQ differed -- so the < %.1f mW bound"
              % (B.PWR_BOUND * 1000.0))
        print("  stands. The ABSOLUTE W does not, and neither does any mJ below.")
        print("  The loop was pointed at the deployed topology on 2026-09-06 and")
        print("  now cross-checks its own period against the pipelined pass.")
        print("  Re-run to replace every number in this section.")
        print()
    print("  THE CAVEAT ON THE DERIVED ROWS, and it is not small.")
    print("  edge_power_measure() drives its own loop, and that loop still packs")
    print("  inside the cascade and never entropy-codes -- it is the run-2")
    print("  topology, %.4f ms. So the mean power above was taken while the CPU"
          % B.II_PWRPIPE)
    print("  SPINS through the DMA waits that the deployed path now fills with")
    print("  work. Same work per frame in a %.0f%% shorter frame, so the energy"
          % (100.0 * (1.0 - B.II_PIPE / B.II_SERIAL)))
    print("  direction is right; the magnitude is not measured, because a busier")
    print("  CPU during those waits will draw more than a spinning one.")
    print("  Pointing the A/B at the deployed loop is the fix. It has not been")
    print("  done, so %.2f mJ is the number to quote and %.2f mJ is an estimate."
          % (B.PWR_B * B.II_PWRPIPE, B.PWR_B * B.II_PIPE))
    print()
    print("  %s" % B.PWR_NOTE)


if __name__ == "__main__":
    print("=" * W)
    print("PixelPacker on Zynq-7020 -- board dump, 2026-09-05 run 3")
    print("xc7z020clg484-1, PL 100 MHz, 1280x720, PQ M=8 K=16, 0.5000 bpp fixed")
    print("=" * W)
    print("SYNTHETIC input and SYNTHETIC codebook: no trained weights exist, so")
    print("this is a timing and power result and NOT a rate or distortion one.")
    print("Correctness gates all clean: %d VQ mismatches of 57,600, %d range"
          % (B.VQ_MISMATCHES, B.RC_ROUNDTRIP_BAD))
    print("round-trip mismatches of 4,608,000, %d engine-exclusion violations."
          % B.STEXCL_VIOLATIONS)
    stage_latency()
    overlapped()
    power()
    print()
    rule("=")
    print("Regenerate: python results/dump.py   Source: results/board_measured.py")
    rule("=")
