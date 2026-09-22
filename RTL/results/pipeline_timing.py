"""
pipeline_timing.py -- what the frame initiation interval can be once VQ runs on
the SAME PW engine as the analysis transform.

The whole question is which resources are mutually exclusive:

  * The PW engine is ONE piece of hardware. Analysis-PW work and VQ work
    contend for it absolutely -- they cannot overlap by any amount.
  * The DW engine is separate hardware and is NOT used by VQ at all, so VQ can
    run underneath DW work.
  * In the frozen model a block costs max(t_dw, t_pw). Whenever a block is
    DW-bound the PW engine is idle for (t_dw - t_pw) cycles, and that idle time
    is exactly the budget VQ can be hidden in.

So the best achievable initiation interval, if the schedule interleaves VQ into
the PW engine's idle windows, is

    II = T_analysis + max(0, T_VQ - PW_slack)
    PW_slack = sum over blocks of (t_blk - t_pw)

and the worst (fully serial, VQ issued only after the whole frame) is

    II = T_analysis + T_VQ

Both are MODELLED. Neither has been measured: the OOS firmware that would time
the selected transform has never been run on the board, and no shared-engine
bitstream exists yet.
"""
import sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from svc_model import walk, F_CLK
from vq_model import vq_cycles

SEL = [16, 48, 64]

blocks = list(walk(SEL))
a_cyc = sum(b['t_blk'] for b in blocks)
pw_slack = sum(b['t_blk'] - b['t_pw'] for b in blocks)
dw_slack = sum(b['t_blk'] - b['t_dw'] for b in blocks)
v_cyc, _ = vq_cycles(8, 16)

print("Selected transform C* = %s, Q=32, L=8, 100 MHz\n" % "-".join(map(str, SEL)))
print("%-6s %6s %6s %9s %9s %9s %-5s %9s"
      % ("block", "cin", "cout", "t_dw", "t_pw", "t_blk", "bind", "PW idle"))
for b in blocks:
    print("%-6d %6d %6d %9d %9d %9d %-5s %9d"
          % (b['block'], b['cin'], b['cout'], b['t_dw'], b['t_pw'],
             b['t_blk'], b['bind'], b['t_blk'] - b['t_pw']))
print("-" * 72)
print("%-6s %6s %6s %9d %9d %9d %-5s %9d"
      % ("total", "", "", sum(b['t_dw'] for b in blocks),
         sum(b['t_pw'] for b in blocks), a_cyc, "", pw_slack))

print("\nT_analysis   = %9d cyc = %8.4f ms   MODELLED" % (a_cyc, a_cyc / F_CLK * 1e3))
print("T_VQ         = %9d cyc = %8.4f ms   MODELLED" % (v_cyc, v_cyc / F_CLK * 1e3))
print("PW idle slack= %9d cyc = %8.4f ms   the budget VQ can hide in" %
      (pw_slack, pw_slack / F_CLK * 1e3))
print("DW idle slack= %9d cyc = %8.4f ms" % (dw_slack, dw_slack / F_CLK * 1e3))

ser = a_cyc + v_cyc
best = a_cyc + max(0, v_cyc - pw_slack)
print("\nII (fully serial VQ)      = %9d cyc = %8.4f ms -> %6.2f fps" %
      (ser, ser / F_CLK * 1e3, F_CLK / ser))
print("II (VQ hidden in PW idle) = %9d cyc = %8.4f ms -> %6.2f fps" %
      (best, best / F_CLK * 1e3, F_CLK / best))
print("II (PW engine busy time)  = %9d cyc = %8.4f ms -> %6.2f fps   [lower bound:"
      % (sum(b['t_pw'] for b in blocks) + v_cyc,
         (sum(b['t_pw'] for b in blocks) + v_cyc) / F_CLK * 1e3,
         F_CLK / (sum(b['t_pw'] for b in blocks) + v_cyc)))
print("                                                              total PW work]")

for target in (68.2, 62.92):
    print("\n  %.2f fps would require II = %.4f ms = %d cycles"
          % (target, 1000.0 / target, int(F_CLK / target)))
