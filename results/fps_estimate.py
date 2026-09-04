"""
fps_estimate.py -- estimated frame rate for the CURRENT design.

Current design = analysis transform 16-48-64 on the DW+PW cascade, VQ M=8/K=16
on the SAME PW engine, entropy coding rebuilt for that geometry, frame loop
software-pipelined so CPU work runs under the PL search.

EVERY NUMBER HERE IS AN ESTIMATE. Nothing in this file has been measured on the
board: the bitstream containing VQ was built today and has never been run. The
provenance of each input is stated so the weak ones are visible.

  T_analysis   MODELLED   frozen service model, results/svc_model.py
  T_VQ         MODELLED   frozen model applied to the VQ shape, vq_model.py
  T_vq_prog    ESTIMATED  2,176 AXI-lite writes; the weakest input, see below
  T_range      SCALED     measured 1.9307 ms on the OLD geometry, x1.88 from
                          the host benchmark ratio -- and that measurement was
                          on a DEGENERATE stream, so it is a LOWER bound
  T_pack       MEASURED   5.1047 ms, host input preparation

SUPERSEDED 2026-09-04 -- see results/overlap_comparison.py.

This file treats T_pack as CPU work available to hide under the VQ search. It
is not. The measured 5.1047 ms happens INSIDE edge_run_six_pairs, before the
search exists, and the measured decomposition shows host work is SERIAL with
the PL (pack + prog + cache + pl sums to t_host with a 1 us residual). So the
analysis phase is PL + host, ~19.77 ms rather than the 13.4463 ms used below,
and the numbers here are optimistic by roughly 6 ms per frame.

What edge_one_pipelined actually moved under the search is
ep_synth_frame_planar -- synthetic INPUT generation -- whose cost has never
been measured on the board.

Corrected estimate: ~44 fps, not ~50. overlap_comparison.py carries it, along
with the comparison against the dedicated engine that this file does not make.
"""
import sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from svc_model import walk, F_CLK
from vq_model import vq_cycles

SEL = [16, 48, 64]

# ---- inputs ---------------------------------------------------------------
blocks   = list(walk(SEL))
A_MS     = sum(b['t_blk'] for b in blocks) / F_CLK * 1e3      # 13.4463
VQ_MS    = vq_cycles(8, 16)[0] / F_CLK * 1e3                  # 2.4480

# Codebook reload: 32 banks x 64 weights + 128 norms + ~6 geometry = 2,182
# AXI-lite writes. The PS7 GP port takes roughly 0.1-0.2 us per posted write at
# 100 MHz; 0.15 us is the midpoint. THIS IS THE WEAKEST NUMBER IN THE FILE and
# it is the one to measure first -- vq_pw_pl_last_prog_ms() reports it directly.
N_PROG_WRITES = 32 * 64 + 128 + 6
US_PER_WRITE  = 0.15
PROG_MS       = N_PROG_WRITES * US_PER_WRITE / 1000.0

RANGE_OLD_MS  = 1.9307          # MEASURED, degenerate stream, lower bound
RANGE_RATIO   = 1.88            # host benchmark, both geometries, same code
RANGE_MS      = RANGE_OLD_MS * RANGE_RATIO

PACK_MS       = 5.1047          # MEASURED


def fps(ms): return 1000.0 / ms


print("Current design: C* = %s, VQ M=8 K=16 on the shared PW engine\n"
      % "-".join(map(str, SEL)))
print("  %-34s %9s   %s" % ("stage", "ms", "provenance"))
print("  " + "-" * 66)
print("  %-34s %9.4f   MODELLED (frozen model)"     % ("analysis (DW+PW cascade)", A_MS))
print("  %-34s %9.4f   ESTIMATED (%d writes x %.2f us)"
      % ("VQ codebook reload", PROG_MS, N_PROG_WRITES, US_PER_WRITE))
print("  %-34s %9.4f   MODELLED (frozen model)"     % ("VQ search", VQ_MS))
print("  %-34s %9.4f   SCALED from measured x%.2f"  % ("range coding", RANGE_MS, RANGE_RATIO))
print("  %-34s %9.4f   MEASURED"                    % ("input preparation (CPU)", PACK_MS))

# ---- engine occupancy is the floor ---------------------------------------
# The PW engine is the only resource VQ and the analysis both need. Its busy
# time cannot be overlapped away by anything.
pw_busy = sum(b['t_pw'] for b in blocks) / F_CLK * 1e3 + VQ_MS

print("\nSerial, no overlap (every stage back to back)")
ser = A_MS + PROG_MS + VQ_MS + RANGE_MS + PACK_MS
print("  II = %.4f ms -> %.2f fps" % (ser, fps(ser)))

print("\nPipelined, CPU work hidden under the PL search (edge_one_pipelined)")
# What can hide: range coding of frame f-1 and input prep for frame f+1, both
# pure CPU. What cannot: the analysis and the codebook reload, both of which
# need the PW engine or its weight BRAM.
cpu_avail = RANGE_MS + PACK_MS
hidden    = min(VQ_MS, cpu_avail)
exposed   = VQ_MS - hidden
pipe      = A_MS + PROG_MS + exposed + max(0.0, cpu_avail - VQ_MS)
print("  CPU work available to hide under the search : %.4f ms" % cpu_avail)
print("  of the %.4f ms search, hidden %.4f, exposed %.4f" % (VQ_MS, hidden, exposed))
print("  CPU work left over after the search ends    : %.4f ms" % max(0.0, cpu_avail - VQ_MS))
print("  II = %.4f ms -> %.2f fps" % (pipe, fps(pipe)))

print("\nFloor set by the shared engine (unreachable with this driver)")
print("  total PW busy = %.4f ms -> %.2f fps" % (pw_busy, fps(pw_busy)))
print("  Unreachable because software can only hand the engine whole jobs; it")
print("  cannot interleave VQ into the analysis's PW idle gaps.")

print("\nSensitivity of the pipelined estimate to the weakest input")
for us in (0.05, 0.10, 0.15, 0.30, 0.50):
    pm = N_PROG_WRITES * us / 1000.0
    ii = A_MS + pm + exposed + max(0.0, cpu_avail - VQ_MS)
    print("  %.2f us/AXI write -> reload %5.3f ms -> II %.3f ms -> %.2f fps"
          % (us, pm, ii, fps(ii)))

print("\nIf range coding is slower on real data than on the degenerate stream")
for mult in (1.0, 2.0, 4.0, 8.0):
    rc = RANGE_MS * mult
    ca = rc + PACK_MS
    hid = min(VQ_MS, ca)
    ii = A_MS + PROG_MS + (VQ_MS - hid) + max(0.0, ca - VQ_MS)
    print("  range x%.0f = %6.3f ms -> II %.3f ms -> %.2f fps" % (mult, rc, ii, fps(ii)))
