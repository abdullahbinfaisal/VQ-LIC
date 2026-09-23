# Shared PW-hosted VQ (MEASURED, 2026-09-11 board run) against the dedicated
# vq_pq_axi engine at the deployed geometry (PROJECTED from measured parts).
T_CLK = 1e-5  # ms per PL cycle, 100 MHz (10 ns)

# ---- measured, pipelined pass, 40 frames ----
II_sh   = 20.8405
an      = 14.5902   # analysis stage, PL + layer programming
prog    = 1.0925    # PS layer programming inside it
rl      = 0.9603    # codebook reload (shared engine only)
vq_br   = 5.2770    # shared VQ driver bracket
vq_acc  = 4.8983    # shared accelerator elapsed
pkc, enc = 5.7873, 5.3730
tails   = II_sh - an - rl - vq_br          # exposed tails + unattributed
en_end  = 13.4724   # entropy of N-1 finishes this far into iteration N
pk0     = 0.0461
E2E_sh  = 62.4697
P_B     = 2.0567    # W, board with VQ
drv     = vq_br - vq_acc                   # driver overhead, assumed equal for the dedicated block

# ---- dedicated engine, deployed geometry M=4 K=64, QUERY_LANES=2 ----
# measured law at K=256: 1,843,339 cyc = 1800 groups x (8/2 x K) + 139 fill
fill = 1843339 - 1800 * 4 * 256
acc_d = (1800 * 4 * 64 + fill) * T_CLK
br_d  = acc_d + drv
print("fill cycles", fill, " dedicated accel K=64 %.4f ms  bracket %.4f ms" % (acc_d, br_d))
print("shared accel %.4f  bracket %.4f  reload %.4f  drv %.4f  tails %.4f" % (vq_acc, vq_br, rl, drv, tails))
print("dedicated accel at K=256 measured 18.435 -> law check %.4f" % ((1800 * 4 * 256 + fill) * T_CLK))

idle_an = an - prog
need = pkc + enc
print("CPU in analysis wait: idle %.4f  needed %.4f  spare %.4f" % (idle_an, need, idle_an - need))

def row(name, ii, e2e):
    print("  %-34s II %8.4f ms  %6.2f fps  dII %+8.4f (%+5.1f%%)  E2E %7.2f  dE2E %+7.2f  E %6.2f mJ" %
          (name, ii, 1000 / ii, ii - II_sh, 100 * (ii - II_sh) / II_sh, e2e, e2e - E2E_sh, P_B * ii))

print()
row("shared, MEASURED", II_sh, E2E_sh)
# D1: drop-in. Same serial order on the PL, no reload, dedicated search time.
ii1 = an + br_d + tails
e1 = (ii1 - pk0) + ii1 + ii1          # payload assembled at frame end, as the harness does today
e1b = (ii1 - pk0) + ii1 + en_end      # payload assembled when coding ends
row("D1 dedicated, serial (no overlap)", ii1, e1)
print("     D1 with payload assembled at coding end: E2E %.2f" % e1b)
# D2: VQ of N on its own engine while analysis of N+1 runs. The analysis stage
# binds; the VQ finishes br_d into the next iteration, before entropy of N
# starts in today's measured order (en start 6.44 ms).
ii2 = an + tails
e2 = (ii2 - pk0) + ii2 + ii2
e2b = (ii2 - pk0) + ii2 + en_end
row("D2 dedicated, VQ overlaps analysis", ii2, e2)
print("     D2 with payload assembled at coding end: E2E %.2f" % e2b)
print("     D2 check: VQ(N) done %.3f ms into N+1; entropy(N) starts 6.444 ms -> %s" %
      (br_d, "OK" if br_d < 6.444 else "LATE"))
print("     power held at the measured %.4f W in D1/D2 -- the added fabric is NOT in it" % P_B)
