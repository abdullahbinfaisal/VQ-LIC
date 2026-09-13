# All inputs copied from the 2026-09-11 board log (divide-free rANS build).
# serial pass, STAT means, 80 frames
pack, prog, cache, pl, gap = 5.0715, 1.0919, 0.0005, 13.4817, 0.0005
host = 19.6461
rl_s, vq_s, ent_s = 0.9599, 5.2828, 4.8061
pairs = (5.1990, 4.7028, 3.5799)
serial = host + rl_s + vq_s + ent_s
print("serial check host", round(pack + prog + cache + pl + gap, 4), host)
print("serial frame", round(serial, 4), "fps", round(1000 / serial, 2))
rows = [("input packing", pack), ("layer programming", prog),
        ("PL analysis hardware", pl), ("  pair1", pairs[0]), ("  pair2", pairs[1]), ("  pair3", pairs[2]),
        ("cache+gap", cache + gap), ("codebook reload", rl_s), ("VQ search bracket", vq_s),
        ("  accel", 4.8984), ("  driver overhead", vq_s - 4.8984), ("entropy", ent_s)]
for n, v in rows:
    print("  %-22s %8.4f  %5.1f%%" % (n, v, 100 * v / serial))
print("  PS total", round(pack + prog + cache + gap + rl_s + (vq_s - 4.8984) + ent_s, 4),
      "PL total", round(pl + 4.8984, 4))
print("rANS ns/sym now", round(ent_s / 57600 * 1e6, 1), "before", round(16.3265 / 57600 * 1e6, 1),
      "speedup", round(16.3265 / ent_s, 2))

# pipelined pass
II = 20.8405
an, rl, vq, pkx, enx = 14.5902, 0.9603, 5.2770, 0.0002, 0.0032
pkc, enc = 5.7873, 5.3730
expo = an + rl + vq + pkx + enx
print("\npipe exposed sum", round(expo, 4), "unattributed", round(II - expo, 4))
print("idle in analysis window", round(an - 1.0925, 4), "used", round(pkc + enc, 4),
      "left", round(an - 1.0925 - pkc - enc, 4))
print("slice overhead pack", round(5.7875 - pack, 4), round(100 * (5.7875 / pack - 1), 1),
      "entropy", round(5.3762 - ent_s, 4), round(100 * (5.3762 / ent_s - 1), 1))
print("saving vs serial", round(serial - II, 4), round(100 * (serial - II) / serial, 1),
      "fps", round(1000 / serial, 2), "->", round(1000 / II, 2))
print("pipe analysis vs serial", round(an - 14.5746, 4), "vq", round(vq - vq_s, 4))
print("payload early-assembly E2E", round(62.4697 - (20.8347 - 13.4724), 2))

# energy
T = 20.8287
A = dict(VCCINT=0.2183, VCCPINT=0.3596, VCCAUX=0.0334, VCCPAUX=0.1214, VCCADJ=0.0229,
         VCC1V5PS=0.4754, VCC_MIO=0.0118, VCCBRAM=0.0110, VCC3V3=0.7722, VCC2V5=0.0231)
B = dict(VCCINT=0.2174, VCCPINT=0.3591, VCCAUX=0.0343, VCCPAUX=0.1298, VCCADJ=0.0252,
         VCC1V5PS=0.4789, VCC_MIO=0.0125, VCCBRAM=0.0115, VCC3V3=0.7673, VCC2V5=0.0206)
grp = dict(PL=["VCCINT", "VCCAUX", "VCCBRAM"], PS=["VCCPINT", "VCCPAUX", "VCC_MIO"],
           DDR=["VCC1V5PS"], MISC=["VCC3V3", "VCCADJ", "VCC2V5"])
PB = 2.0567
print("\nrail sums A", round(sum(A.values()), 4), "B", round(sum(B.values()), 4))
for g, rs in grp.items():
    a = sum(A[r] for r in rs); b = sum(B[r] for r in rs)
    print("  %-4s A %.4f B %.4f d %+.4f  E_B %.2f mJ  %.1f%%" % (g, a, b, b - a, b * T, 100 * b / sum(B.values())))
    for r in rs:
        print("     %-9s B %.4f  %.2f mJ" % (r, B[r], B[r] * T))
print("E frame", round(PB * T, 2), "A-equiv", round(2.0491 * T, 2),
      "VQ bound mJ", round(0.0149 * T, 3), "pct", round(100 * 0.0149 * T / (PB * T), 2))
print("nJ/pixel", round(PB * T / (1280 * 720) * 1e6, 1))
E = PB * T
for n, t in [("analysis window", an), ("reload", rl), ("VQ search", vq), ("exposed tails+unattr", II - an - rl - vq)]:
    print("  alloc %-22s %.4f ms  %.2f mJ  %.1f%%" % (n, t, E * t / II, 100 * t / II))
print("serial at same power mJ", round(PB * serial, 2), "saved", round(PB * (serial - T), 2))
