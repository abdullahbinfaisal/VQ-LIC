"""
combined_system.py -- system-level combined analysis+VQ numbers, old vs new.

These are the comparison the paper actually needs and that no earlier script
produced: the OLD deployment overlapped the analysis transform and the DEDICATED
VQ engine on separate hardware, so its initiation interval was
max(T_analysis, T_VQ_dedicated). The NEW deployment shares one PW engine, so its
floor is the total PW busy time and nothing can go below it.

Dedicated VQ cost is from the RTL's own schedule statement (vq_engine.sv:56-59):
"exactly K cycles per BATCH of QUERY_LANES positions, with no bubble", with the
deployed QUERY_LANES=2 read out of the BD .xci. That is ANALYTICAL -- the block
has never been timed on hardware.
"""
import sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from svc_model import walk, F_CLK
from vq_model import vq_cycles

SEL = [16, 48, 64]
NPOS = 90 * 160
K_OLD, QL_OLD = 256, 2          # deployed dedicated-engine parameters
T_RC_MS = 1.9307                # MEASURED, degenerate input -- lower bound
T_MEAS_OTHER_MS = 18.8671       # MEASURED, schedule 16-32-32-32-64-64

blocks = list(walk(SEL))
a_cyc = sum(b['t_blk'] for b in blocks)
pw_cyc = sum(b['t_pw'] for b in blocks)
dw_cyc = sum(b['t_dw'] for b in blocks)
pw_slack = a_cyc * len(blocks) and sum(b['t_blk'] - b['t_pw'] for b in blocks)
dw_slack = sum(b['t_blk'] - b['t_dw'] for b in blocks)
v_cyc, _ = vq_cycles(8, 16)
ded_cyc = (NPOS // QL_OLD) * K_OLD

ms = lambda c: c / F_CLK * 1e3
fps = lambda c: F_CLK / c

print("Selected transform C* = %s, Q=32, L=8, 100 MHz\n" % "-".join(map(str, SEL)))
print("Component costs")
print("  T_analysis                 %9d cyc  %8.4f ms   MODELLED" % (a_cyc, ms(a_cyc)))
print("    of which PW busy         %9d cyc  %8.4f ms" % (pw_cyc, ms(pw_cyc)))
print("    of which DW busy         %9d cyc  %8.4f ms" % (dw_cyc, ms(dw_cyc)))
print("    PW idle slack            %9d cyc  %8.4f ms" % (pw_slack, ms(pw_slack)))
print("    DW idle slack            %9d cyc  %8.4f ms" % (dw_slack, ms(dw_slack)))
print("  T_VQ shared  (M=8,K=16)    %9d cyc  %8.4f ms   MODELLED" % (v_cyc, ms(v_cyc)))
print("  T_VQ dedicated (M=4,K=256) %9d cyc  %8.4f ms   ANALYTICAL" % (ded_cyc, ms(ded_cyc)))
print("  T_range_coder                        %8.4f ms   MEASURED (degenerate)" % T_RC_MS)

print("\nOLD deployment -- separate engines, VQ overlaps analysis")
old_ii = max(a_cyc, ded_cyc)
print("  II = max(T_analysis, T_VQ_ded) = max(%.4f, %.4f) = %8.4f ms -> %6.2f fps"
      % (ms(a_cyc), ms(ded_cyc), ms(old_ii), fps(old_ii)))
print("  BINDING: %s" % ("dedicated VQ" if ded_cyc > a_cyc else "analysis"))
old_ii_meas = max(T_MEAS_OTHER_MS * F_CLK / 1e3, ded_cyc)
print("  same, using the only MEASURED analysis (schedule 16-32-32-32-64-64):")
print("        max(%.4f, %.4f) = %8.4f ms -> %6.2f fps"
      % (T_MEAS_OTHER_MS, ms(ded_cyc), ms(old_ii_meas), fps(old_ii_meas)))

print("\nNEW deployment -- shared PW engine, VQ cannot overlap PW work")
floor = pw_cyc + v_cyc
ser = a_cyc + v_cyc
print("  II floor = total PW busy   %9d cyc  %8.4f ms -> %6.2f fps" % (floor, ms(floor), fps(floor)))
print("  II serial (no overlap)     %9d cyc  %8.4f ms -> %6.2f fps" % (ser, ms(ser), fps(ser)))
print("  VQ that cannot be hidden   %9d cyc  %8.4f ms" % (v_cyc - pw_slack, ms(v_cyc - pw_slack)))

print("\nSystem speedup, new vs old (C*)")
for label, new in (("at the II floor", floor), ("fully serial VQ", ser)):
    print("  %-18s %.4f -> %.4f ms   %.3fx   %6.2f -> %6.2f fps  (%+.1f%%)"
          % (label, ms(old_ii), ms(new), old_ii / new,
             fps(old_ii), fps(new), 100.0 * (fps(new) / fps(old_ii) - 1)))

print("\nWith range coding serialised (it runs on the PS and normally overlaps)")
full = ser + T_RC_MS * F_CLK / 1e3
print("  analysis + VQ + RC         %8.4f ms -> %6.2f fps   DERIVED" % (ms(full), fps(full)))
