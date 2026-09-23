"""
q_sensitivity.py -- total ANALYSIS + VQ service versus Q, now that VQ runs on
the same PW engine.

The frozen model's pw_cycles() hard-codes Q, so the walk is re-expressed here
with Q threaded through. Every constant still comes from svc_model.py; nothing
is re-fitted.

DSP model: measured post-route, N_OC=Q=32 gives PW 132 DSP and DW 88 DSP for a
total of exactly 220/220 (PAPER_HW_EVIDENCE.md sec 18). The PW dual-MAC grid is
Q*(L/2) = 4Q multipliers, so PW(Q) = 4Q + 4 and TOTAL(Q) = 4Q + 92, which
reproduces 220 at Q=32. This assumes the PPUs stay where they are; the
manuscript already records that mapping PPUs to LUT changes this bound.
"""
import sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from svc_model import (pw_cyc_per_group, dw_cycles, F_CLK, L, CAP, ceil_div)
from vq_model import vq_cycles, vq_shape

SEL = [16, 48, 64]          # selected encoder transform C*
NPOS = 90 * 160


def walk_q(cout_list, Q, n_stride2=3, cin0=3, H0=720, W0=1280):
    H, W, cin = H0, W0, cin0
    out = []
    for j, cout in enumerate(cout_list):
        s = 2 if j < n_stride2 else 1
        Ho, Wo = (H + s - 1) // s, (W + s - 1) // s
        tdw = dw_cycles(cin, H, W)
        tpw = pw_cyc_per_group(cin, cout, Q=Q) * (Ho * Wo // L)
        out.append(dict(block=j + 1, cin=cin, cout=cout, t_dw=tdw, t_pw=tpw,
                        t_blk=max(tdw, tpw), bind="DW" if tdw >= tpw else "PW"))
        H, W, cin = Ho, Wo, cout
    return out


def dsp_total(Q):
    return 4 * Q + 92


print("Selected transform C* = %s, VQ M=8 K=16, %d positions, 100 MHz\n"
      % ("-".join(map(str, SEL)), NPOS))
print("%-4s %10s %10s %10s %10s %8s  %s"
      % ("Q", "analysis", "VQ", "serial", "fps(serial)", "DSP", "feasible"))
print("-" * 78)
rows = []
for Q in (16, 32, 48, 64):
    blocks = walk_q(SEL, Q)
    a_cyc = sum(b['t_blk'] for b in blocks)
    v_cyc, _ = vq_cycles(8, 16, Q=Q)
    a_ms, v_ms = a_cyc / F_CLK * 1e3, v_cyc / F_CLK * 1e3
    tot = a_ms + v_ms
    d = dsp_total(Q)
    feas = "yes" if d <= 220 else "NO (%d>220 DSP)" % d
    rows.append((Q, a_ms, v_ms, tot, d, feas, blocks))
    print("%-4d %10.4f %10.4f %10.4f %10.2f %8d  %s"
          % (Q, a_ms, v_ms, tot, 1000.0 / tot, d, feas))

print("\nPer-block binding at each Q (shows WHERE the analysis knee comes from):")
for Q, a_ms, v_ms, tot, d, feas, blocks in rows:
    print("  Q=%-3d %s   analysis=%.4f ms" %
          (Q, "  ".join("b%d:%s" % (b['block'], b['bind']) for b in blocks), a_ms))

print("\nVQ shape at each Q (why VQ scales differently from the transform):")
for Q in (16, 32, 48, 64):
    cin_mac, cout_total, batches, spb, dsub = vq_shape(8, 16, Q=Q)
    v_cyc, groups = vq_cycles(8, 16, Q=Q)
    print("  Q=%-3d subs/batch=%-2d cin_mac=%-3d cout=%-4d batches=%-3d "
          "%4d cyc/grp -> %8.4f ms"
          % (Q, spb, cin_mac, cout_total, batches, v_cyc // groups,
             v_cyc / F_CLK * 1e3))
