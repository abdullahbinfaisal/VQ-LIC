"""
synthesis.py -- implementation results for the bitstream currently on the board.

PROVENANCE, checked rather than assumed. This project has already shipped one
XSA built from the wrong parameter binding, so the identity of "the current
bitstream" is verified, not inferred:

  Zynq.runs/impl_1/hw_wrapper.bit   4,045,670 B  md5 d6bb43a7...71e8ea
  pwvq_noc32.xsa :: pwvq_noc32.bit  4,045,670 B  md5 d6bb43a7...71e8ea   IDENTICAL
  Final_2/hw/pwvq_noc32.xsa         same file, 2026-09-04 15:44

  synth_1 log: "Parameter USE_PW_VQ bound to: 32'sb...0001"   (three instances)

So impl_1's reports describe the bitstream the board ran for runs 1-3.

  python synthesis.py
"""

PART   = "xc7z020clg484-1"
TOOL   = "Vivado v.2020.2 (win64) Build 3064766"
DATE   = "2026-09-04 05:22 (utilization), 05:31 (timing), 05:32 (bitstream)"
DESIGN = "hw_wrapper"

# ---- timing, post-route ---------------------------------------------------
CLK_NAME   = "clk_fpga_0"
CLK_PERIOD = 10.000     # ns
CLK_MHZ    = 100.000
WNS   =  0.077          # ns
TNS   =  0.000
WHS   =  0.012
THS   =  0.000
WPWS  =  3.750
TPWS  =  0.000
EP_SETUP_FAIL = 0
EP_SETUP_TOT  = 100426
EP_HOLD_FAIL  = 0
EP_HOLD_TOT   = 100426
EP_PW_FAIL    = 0
EP_PW_TOT     = 35629
TIMING_MET    = True    # "All user specified timing constraints are met."

# ---- utilization, post-place ----------------------------------------------
#  name, used, available
UTIL = [
    ("Slice LUTs",            34358, 53200),
    ("  LUT as Logic",        32823, 53200),
    ("  LUT as Memory",        1535, 17400),
    ("    Distributed RAM",     920, None),
    ("    Shift Register",      615, None),
    ("Slice Registers (FF)",  32354, 106400),
    ("F7 Muxes",               1393, 26600),
    ("F8 Muxes",                288, 13300),
    ("Block RAM Tile",         82.5, 140),
    ("  RAMB36E1",               62, 140),
    ("  RAMB18E1",               41, 280),
    ("DSP48E1",                 220, 220),
    ("Bonded IOB",                0, 200),
    ("BUFGCTRL",                  1, 32),
    ("MMCME2_ADV",                0, 4),
    ("PLLE2_ADV",                 0, 4),
]

# ---- Vivado's own power estimate ------------------------------------------
# VECTORLESS, confidence Medium, no SAIF and no simulation activity file. It
# assumes default toggle rates, so it is not a prediction of what this design
# draws -- see the comparison against the board below.
VIV_TOTAL   = 2.530
VIV_DYNAMIC = 2.352
VIV_STATIC  = 0.178
VIV_TJ      = 54.2      # C
VIV_TJA     = 11.5      # C/W
VIV_COMPONENTS = [      # W
    ("Clocks",      0.085),
    ("Slice Logic", 0.186),
    ("Signals",     0.307),
    ("Block RAM",   0.020),
    ("DSPs",        0.177),
    ("PS7",         1.577),
    ("Static",      0.178),
]
VIV_PL_DYN = 0.085 + 0.186 + 0.307 + 0.020 + 0.177   # everything but PS7 and static

# Board, run 3, phase B (with the VQ), regulator OUTPUT power.
BOARD_PL_RAILS = 0.2689   # VCCINT + VCCAUX + VCCBRAM
BOARD_TOTAL    = 2.0359


def report():
    print("SYNTHESIS / IMPLEMENTATION -- bitstream currently on the board")
    print("=" * 74)
    print("  part    %s" % PART)
    print("  tool    %s" % TOOL)
    print("  design  %s, built %s" % (DESIGN, DATE))
    print("  bitstream identity VERIFIED: impl_1/hw_wrapper.bit is byte-identical")
    print("  to pwvq_noc32.bit inside the XSA the platform was built from, and")
    print("  the synth log binds USE_PW_VQ to 1.")

    print()
    print("TIMING, post-route")
    print("-" * 74)
    print("  %-22s %s at %.3f ns (%.1f MHz)" % ("clock", CLK_NAME, CLK_PERIOD, CLK_MHZ))
    print("  %-22s %+.3f ns" % ("WNS (setup slack)", WNS))
    print("  %-22s %+.3f ns" % ("WHS (hold slack)", WHS))
    print("  %-22s %+.3f ns" % ("WPWS (pulse width)", WPWS))
    print("  %-22s %.3f ns / %.3f ns / %.3f ns" % ("TNS / THS / TPWS", TNS, THS, TPWS))
    print("  %-22s %d of %d setup, %d of %d hold, %d of %d pulse-width"
          % ("failing endpoints", EP_SETUP_FAIL, EP_SETUP_TOT,
             EP_HOLD_FAIL, EP_HOLD_TOT, EP_PW_FAIL, EP_PW_TOT))
    print("  %-22s %s" % ("verdict", "all user specified timing constraints are met"
                          if TIMING_MET else "NOT MET"))
    print()
    print("  %.3f ns of setup slack on a %.0f ns period is %.1f%%. The design"
          % (WNS, CLK_PERIOD, 100.0 * WNS / CLK_PERIOD))
    print("  closes, and it does not close comfortably: 220 of 220 DSPs are in")
    print("  use, so there is no room to trade area for slack on this part.")

    print()
    print("UTILIZATION, post-place")
    print("-" * 74)
    print("  %-22s %9s %11s %8s" % ("site type", "used", "available", "util%"))
    for nm, used, avail in UTIL:
        if avail:
            print("  %-22s %9s %11d %7.2f%%"
                  % (nm, ("%.1f" % used) if isinstance(used, float) else used,
                     avail, 100.0 * used / avail))
        else:
            print("  %-22s %9s %11s %8s" % (nm, used, "", ""))
    print()
    print("  DSP is the binding resource at 100.00%. Everything else has room:")
    print("  LUT 64.6%, FF 30.4%, BRAM 58.9%. A larger analysis schedule or a")
    print("  wider VQ would need DSPs this part does not have.")

    print()
    print("VIVADO POWER ESTIMATE -- and why it is not the number to quote")
    print("-" * 74)
    for nm, w in VIV_COMPONENTS:
        print("  %-14s %6.3f W" % (nm, w))
    print("  %-14s %6.3f W  (dynamic %.3f + static %.3f)"
          % ("TOTAL on-chip", VIV_TOTAL, VIV_DYNAMIC, VIV_STATIC))
    print("  Tj %.1f C at TJA %.1f C/W. Confidence: MEDIUM." % (VIV_TJ, VIV_TJA))
    print()
    print("  VECTORLESS. No SAIF, no simulation activity file, so Vivado assumed")
    print("  default toggle rates rather than this design's. Against the board:")
    print()
    print("    PL fabric   Vivado %.3f W   board %.3f W   %.1fx over"
          % (VIV_PL_DYN, BOARD_PL_RAILS, VIV_PL_DYN / BOARD_PL_RAILS))
    print("    (board = VCCINT + VCCAUX + VCCBRAM at the regulator outputs)")
    print()
    print("  The two are not the same quantity -- on-chip against regulator")
    print("  output, and the board total also carries rails Vivado does not model")
    print("  at all, VCC3V3 board housekeeping being 0.745 W of it. But a %.1fx"
          % (VIV_PL_DYN / BOARD_PL_RAILS))
    print("  gap on the fabric is not accounted for by that. Quote the measured")
    print("  %.4f W; the estimate would overstate the PL by about 3x." % BOARD_TOTAL)


if __name__ == "__main__":
    report()
