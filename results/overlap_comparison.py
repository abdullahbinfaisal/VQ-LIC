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
RELOAD  = (32 * 64 + 128 + 6) * 0.15 / 1000.0   # codebook reload      0.327 ESTIMATED

fps = lambda ms: 1000.0 / ms

print("Analysis phase (host-inclusive, because host work is SERIAL with the PL)")
print("  PL   %7.4f ms  MODELLED" % PL_MS)
print("  pack %7.4f ms  MEASURED" % PACK_MS)
print("  prog %7.4f ms  MEASURED 2.4363 over 6 pairs, scaled to %d" % (PROG_MS, len(SEL)))
print("  ---- %7.4f ms  total" % ANALYSIS)

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
# End-to-end estimate for the CURRENT design, including entropy coding.
# This is the number to quote for "what frame rate will the board give".
# ---------------------------------------------------------------------------
RANGE_MS = 1.9307 * 1.88     # measured on the OLD geometry, scaled by the
                             # host benchmark ratio. The measurement was taken
                             # on a DEGENERATE stream, so this is a LOWER bound.
VQ_TOT = SHR_NEW + RELOAD

print("\nEnd-to-end, current design (analysis + reload + search + entropy)")
ser = ANALYSIS + VQ_TOT + RANGE_MS
print("  serial                                : %6.3f ms -> %6.2f fps" % (ser, fps(ser)))

# Only range coding overlaps: it is CPU work on frame f-1's indices, needing
# neither the PW engine nor any buffer in flight. Input preparation does NOT
# count -- edge_prepare2() already does it outside the frame.
pipe = ANALYSIS + RELOAD + max(SHR_NEW, RANGE_MS)
print("  pipelined (entropy under the search)  : %6.3f ms -> %6.2f fps" % (pipe, fps(pipe)))
print("  the search is %s by entropy coding (%.3f vs %.3f ms)"
      % ("fully hidden" if RANGE_MS >= SHR_NEW else "only partly hidden",
         SHR_NEW, RANGE_MS))
print("  overlap is worth %6.2f -> %6.2f fps (%.2fx)" % (fps(ser), fps(pipe), ser / pipe))

print("\n  For reference, the old dedicated engine at the same analysis and")
print("  entropy cost: II = max(%.3f, %.3f) + %.3f = %.3f ms -> %.2f fps"
      % (ANALYSIS, DED_OLD, RANGE_MS, max(ANALYSIS, DED_OLD) + RANGE_MS,
         fps(max(ANALYSIS, DED_OLD) + RANGE_MS)))
print("  (its VQ overlapped the analysis; its entropy stage did not overlap")
print("   anything, because the CPU was the thing driving both.)")
