"""
overlap_comparison.py -- did moving VQ onto the shared PW engine cost the
frame overlap the dedicated engine allowed?

Short answer: yes, and it is worth about 12% of the frame rate. The M,K change
does not recover it -- it prevents the shared design from being far worse.

WHY THE ANALYSIS PHASE IS PL + HOST, NOT PL ALONE. The measured decomposition
is complete to 1 us: t_pack 5.1047 + t_prog 2.4363 + t_cache 0.0009 + t_pl
18.8671 = 26.4090 against a measured t_host of 26.4100. The components SUM to
the total, so the CPU work is serial with the PL, not hidden under it. Any
frame-rate estimate that uses PL time alone is wrong by ~7 ms.

  (This is the error in the first version of fps_estimate.py: it treated the
   measured in-cascade t_pack as CPU work available to hide under the VQ
   search. It is not -- it happens inside edge_run_six_pairs, before the search
   exists. What edge_one_pipelined actually moved under the search is
   ep_synth_frame_planar, the synthetic INPUT generation, whose cost has never
   been measured.)
"""
import sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from svc_model import walk, F_CLK
from vq_model import vq_cycles

SEL = [16, 48, 64]
NPOS = 90 * 160

# ---- analysis -------------------------------------------------------------
PL_MS    = sum(b['t_blk'] for b in walk(SEL)) / F_CLK * 1e3   # 13.4463 MODELLED
PACK_MS  = 5.1047        # MEASURED. Frame-level: chained pairs are CASC_IN_PREPACKED
PROG_6   = 2.4363        # MEASURED over 6 pairs (12 layers)
PROG_MS  = PROG_6 * (len(SEL) / 6.0)   # scaled to 3 pairs -- an assumption
CACHE_MS = 0.0009
ANALYSIS = PL_MS + PACK_MS + PROG_MS + CACHE_MS

# ---- VQ, the three variants ----------------------------------------------
QL = 2                                   # dedicated engine QUERY_LANES, from the .xci
DED_OLD = (NPOS // QL) * 256 / F_CLK * 1e3      # M=4 K=256 dedicated  18.432 ANALYTICAL
DED_NEW = (NPOS // QL) *  16 / F_CLK * 1e3      # M=8 K=16  dedicated   1.152 ANALYTICAL
SHR_OLD = vq_cycles(4, 256)[0] / F_CLK * 1e3    # M=4 K=256 shared     19.584 MODELLED
SHR_NEW = vq_cycles(8,  16)[0] / F_CLK * 1e3    # M=8 K=16  shared      2.448 MODELLED

# MEASURED ON THE BOARD 2026-09-05, first run of the PW-hosted VQ bitstream.
# vq_pw_pl_last_run_ms() reported 2.4503 ms against SHR_NEW's modelled 2.4480:
# +0.09%. The service model is now validated on hardware for the VQ shape, not
# just for the convolution shapes it was frozen on.
SHR_NEW_MEAS = 2.4503

# Codebook reload. Was 0.327 ms ESTIMATED at an assumed 0.15 us per AXI-lite
# write and flagged as the weakest input in the whole estimate. Measured
# 0.4819 ms by vq_pw_pl_last_prog_ms(), i.e. 0.221 us per write over the same
# 2,182 writes -- 47% above the assumption, so the estimate was optimistic.
RELOAD  = 0.4819                                # codebook reload      MEASURED

fps = lambda ms: 1000.0 / ms

print("Analysis phase (host-inclusive, because host work is SERIAL with the PL)")
print("  PL   %7.4f ms  MODELLED" % PL_MS)
print("  pack %7.4f ms  MEASURED" % PACK_MS)
print("  prog %7.4f ms  MEASURED 2.4363 over 6 pairs, scaled to %d" % (PROG_MS, len(SEL)))
print("  ---- %7.4f ms  total" % ANALYSIS)

print("\nVQ on the shared engine, model against board (2026-09-05)")
print("  search  modelled %7.4f ms   measured %7.4f ms   %+.2f%%"
      % (SHR_NEW, SHR_NEW_MEAS, 100.0 * (SHR_NEW_MEAS - SHR_NEW) / SHR_NEW))
print("  reload  assumed  %7.4f ms   measured %7.4f ms   %+.2f%%"
      % (0.327, RELOAD, 100.0 * (RELOAD - 0.327) / 0.327))
print("  NOTE the rows below still use the MODELLED search, so the dedicated-")
print("  engine variants (which have no board measurement) stay comparable.")

rows = [
    ("A  dedicated engine, M=4 K=256, OVERLAPPED", DED_OLD, True,
     "the old deployment as designed"),
    ("B  dedicated engine, M=4 K=256, serial",     DED_OLD, False,
     "what the old deployment cost without overlap"),
    ("C  dedicated engine, M=8 K=16,  OVERLAPPED", DED_NEW, True,
     "hypothetical: new codebook on the old engine"),
    ("D  SHARED PW engine, M=4 K=256",             SHR_OLD + RELOAD, False,
     "shared engine WITHOUT the codebook change"),
    ("E  SHARED PW engine, M=8 K=16",              SHR_NEW + RELOAD, False,
     "the current design"),
]

print("\n%-44s %8s %8s   %s" % ("configuration", "VQ ms", "II ms", "fps"))
print("-" * 84)
res = {}
for name, vq, overlapped, _note in rows:
    ii = max(ANALYSIS, vq) if overlapped else ANALYSIS + vq
    res[name[0]] = ii
    print("%-44s %8.3f %8.3f   %6.2f" % (name, vq, ii, fps(ii)))

print("\nWhat each change is actually worth")
print("  overlap, at the OLD codebook (B -> A)   : %6.2f -> %6.2f fps  (%.2fx)"
      % (fps(res['B']), fps(res['A']), res['B'] / res['A']))
print("  sharing the engine (A -> E)             : %6.2f -> %6.2f fps  (%.0f%% loss)"
      % (fps(res['A']), fps(res['E']), 100.0 * (1.0 - res['A'] / res['E'])))
print("  the M,K change ON the shared engine (D -> E): %6.2f -> %6.2f fps  (%.2fx)"
      % (fps(res['D']), fps(res['E']), res['D'] / res['E']))
print("  the M,K change ON the dedicated engine (A -> C): %6.2f -> %6.2f fps  (no gain --"
      % (fps(res['A']), fps(res['C'])))
print("      VQ was ALREADY fully hidden by the analysis at %.3f ms)" % ANALYSIS)

print("""
READING THIS

  The dedicated engine's VQ ran on separate hardware, so its %.3f ms sat
  entirely inside the analysis phase's %.3f ms and cost the frame rate
  NOTHING. Overlap was worth %.2fx there precisely because VQ was large.

  On the shared engine no amount of buffering recovers that: VQ and the
  analysis contend for one PW engine, so the search is fully exposed and adds
  %.3f ms to every frame.

  The M,K change did not buy throughput. On the dedicated engine it buys
  nothing at all (A -> C), because VQ was already hidden. What it does is stop
  the shared design from being a disaster: at the old codebook the shared
  engine would be %.2f fps (D).

  So the honest framing for the paper is that engine sharing costs about %.0f%%
  of the frame rate and buys ~7,000 LUT, ~8,100 FF and 9 BRAM36. It is an
  area/throughput trade, not a speedup.
""" % (DED_OLD, ANALYSIS, res['B'] / res['A'], SHR_NEW + RELOAD,
       fps(res['D']), 100.0 * (1.0 - res['A'] / res['E'])))

print("Sensitivity: the analysis phase is the term that decides whether the")
print("dedicated engine's VQ was hidden at all.")
for extra in (-3.0, -1.5, 0.0, 1.5, 3.0):
    a = ANALYSIS + extra
    ii_a = max(a, DED_OLD)
    ii_e = a + SHR_NEW + RELOAD
    print("  analysis %6.3f ms -> A %6.2f fps, E %6.2f fps, sharing costs %4.1f%%"
          % (a, fps(ii_a), fps(ii_e), 100.0 * (1.0 - ii_a / ii_e)))

# ---------------------------------------------------------------------------
# End-to-end for the CURRENT design.
#
# SUPERSEDED 2026-09-05 BY MEASUREMENT. The board ran the PW-hosted VQ
# bitstream; results/board_measured.py carries every number and the estimates
# below are kept only to show what the pre-run guesses were worth.
#
#   analysis     19.7701 modelled   ->  19.5905 measured   -0.9%
#   VQ reload     0.4819 measured (was 0.327 assumed:      +47%)
#   VQ search     2.4480 modelled   ->   2.4503 measured   +0.09%
#   entropy       3.6297 estimated  ->   5.2750 measured   +45%
#
# The two MODELLED terms held. The two ESTIMATED terms did not, and both were
# optimistic. The serial frame period is 27.99 ms (35.73 fps), not the 26.33 ms
# this file predicted.
# ---------------------------------------------------------------------------
import board_measured as B

print("\nEnd-to-end, current design -- MEASURED 2026-09-05")
print("  this file predicted            : %6.3f ms -> %6.2f fps"
      % (ANALYSIS + SHR_NEW + RELOAD + 1.9307 * 1.88,
         fps(ANALYSIS + SHR_NEW + RELOAD + 1.9307 * 1.88)))
SER_MEAS = (B.T_HOST + B.T_VQ_PROG + B.T_VQ_RUN + B.T_RANGE)
print("  the board gave                 : %6.3f ms -> %6.2f fps"
      % (SER_MEAS, fps(SER_MEAS)))
print("  run results/board_measured.py for the full decomposition, the")
print("  overlap bounds and the power A/B.")

print("\n  The comparison this file exists to make is unaffected: the dedicated")
print("  engine's VQ was hidden under the analysis and the shared engine's is")
print("  not, so sharing still costs a frame-rate fraction. With the measured")
print("  analysis and the measured search that cost is:")
_ded = max(B.T_HOST, DED_OLD)              # dedicated: VQ hidden under analysis
_shr = B.T_HOST + B.T_VQ_PROG + B.T_VQ_RUN # shared: exposed, plus its reload
print("    dedicated (VQ hidden)        : %6.3f ms -> %6.2f fps" % (_ded, fps(_ded)))
print("    shared    (VQ exposed)       : %6.3f ms -> %6.2f fps" % (_shr, fps(_shr)))
print("    sharing costs %.1f%% of the frame rate, before entropy coding."
      % (100.0 * (1.0 - _ded / _shr)))
print("  NOTE the dedicated figure remains MODELLED: that engine was removed")
print("  from the design, so it can no longer be measured on this board.")

# ---------------------------------------------------------------------------
# 2026-09-05, run 3: the comparison above has REVERSED, and the reason is the
# same overlap that used to justify the dedicated engine.
# ---------------------------------------------------------------------------
_rest = B.II_PIPE - B.T_VQ_PROG - B.T_VQ_RUN_P   # frame minus what only the
                                                 # shared design has to pay
_ded  = max(_rest, DED_OLD)

print("\nWith the CPU work hidden, the dedicated engine LOSES")
print("  shared engine   MEASURED : %7.4f ms -> %5.2f fps" % (B.II_PIPE, fps(B.II_PIPE)))
print("  dedicated       MODELLED : max(%.4f, %.4f) = %7.4f ms -> %5.2f fps"
      % (_rest, DED_OLD, _ded, fps(_ded)))
print("    Its frame is the analysis side alone, %.4f ms -- no codebook reload" % _rest)
print("    and no shared search -- run against an %.3f ms VQ on its own" % DED_OLD)
print("    hardware, which it cannot overlap away.")
print("  the shared engine is %.1f%% FASTER, and ~7,000 LUT / ~8,100 FF /"
      % (100.0 * (_ded / B.II_PIPE - 1.0)))
print("  9 BRAM36 smaller.")
print("""
  WHY IT REVERSED. The dedicated engine's %.3f ms VQ hid under an analysis
  phase that was 19.65 ms because 6.16 ms of it was CPU work sitting in front
  of the PL. Moving that work into the cascade's DMA waits shrank the phase to
  %.4f ms, which is no longer wide enough to cover an 18.43 ms search. The
  overlap that made the dedicated engine look free is the same overlap that
  took its advantage away.

  So the trade is no longer area-for-throughput. On the measured pipeline the
  shared engine is smaller AND slightly faster, and the honest caveat is the
  direction of evidence: the shared figure is measured on silicon, the
  dedicated one is analytical for hardware that no longer exists in the design.
""" % (DED_OLD, _rest))
