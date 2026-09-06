"""
analyse_sweep.py -- Experiment A. Join the RTL cycle counts against T_DW.

Reads  sim/dw_sweep_raw.log   (the DWCSV lines xsim printed)
Writes results/dw_sweep.csv

NOTHING HERE FITS ANYTHING. If measured disagrees with T_DW the disagreement is
reported as-is. No constant is adjusted, no correction term is introduced, and
no configuration is dropped for disagreeing.

  python analyse_sweep.py
"""
import csv
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(ROOT, "model"))
import service_model as M

RAW = os.path.join(HERE, "dw_sweep_raw.log")
OUT = os.path.join(ROOT, "results", "dw_sweep.csv")

FIELDS = ["H", "W", "c_in", "stride", "G", "in_beats",
          "predicted_cycles", "measured_cycles", "abs_err", "pct_err",
          "err_per_row", "axis_cycles", "axis_minus_core",
          "core_out_beats", "axis_out_beats", "status"]


def parse(path):
    rows = []
    for line in open(path, encoding="utf-8", errors="replace"):
        if not line.startswith("DWCSV,"):
            continue
        f = [x.strip() for x in line.strip().split(",")]
        if len(f) < 13 or f[1] == "h":
            continue
        try:
            H, W, C, S, G, beats = (int(f[i]) for i in range(1, 7))
            core, axis, delta = int(f[7]), int(f[8]), int(f[9])
            ob_core, ob_axis = int(f[10]), int(f[11])
        except ValueError:
            continue
        rows.append(dict(H=H, W=W, c_in=C, stride=S, G=G, in_beats=beats,
                         measured_cycles=core, axis_cycles=axis,
                         axis_minus_core=delta,
                         core_out_beats=ob_core, axis_out_beats=ob_axis,
                         status=f[12]))
    return rows


def main():
    if not os.path.exists(RAW):
        print("no %s -- run the sweep first" % RAW)
        return 1
    rows = parse(RAW)
    if not rows:
        print("no DWCSV rows in %s" % RAW)
        return 1

    for r in rows:
        p = M.t_dw(r["H"], r["W"], r["c_in"])
        r["predicted_cycles"] = int(p)
        if r["measured_cycles"] > 0:
            r["abs_err"] = r["measured_cycles"] - r["predicted_cycles"]
            r["pct_err"] = 100.0 * r["abs_err"] / float(r["predicted_cycles"])
            r["err_per_row"] = r["abs_err"] / float(r["H"] + 1)
        else:
            r["abs_err"] = r["pct_err"] = r["err_per_row"] = None

    with open(OUT, "w", newline="", encoding="utf-8") as fh:
        w = csv.DictWriter(fh, fieldnames=FIELDS, extrasaction="ignore")
        w.writeheader()
        for r in rows:
            w.writerow(r)

    ok = [r for r in rows if r["status"] == "OK" and r["measured_cycles"] > 0]
    bad = [r for r in rows if r["status"] != "OK"]

    print("Experiment A -- DW schedule sweep, RTL vs T_DW")
    print("=" * 78)
    print("  configurations run   : %d" % len(rows))
    print("  usable (status OK)   : %d" % len(ok))
    if bad:
        print("  NOT usable           : %d" % len(bad))
        for r in bad[:8]:
            print("      H=%d W=%d c_in=%d s=%d -> %s"
                  % (r["H"], r["W"], r["c_in"], r["stride"], r["status"]))
    if not ok:
        return 1

    def stats(sel, label):
        if not sel:
            return
        ae = [abs(r["abs_err"]) for r in sel]
        pe = [abs(r["pct_err"]) for r in sel]
        pr = [abs(r["err_per_row"]) for r in sel]
        sgn = set(1 if r["abs_err"] > 0 else (-1 if r["abs_err"] < 0 else 0)
                  for r in sel)
        print("  %-22s n=%-3d  |err| max %7d mean %9.1f cyc | "
              "pct max %6.2f%% mean %6.3f%% | per-row max %7.3f mean %7.3f | %s"
              % (label, len(sel), max(ae), sum(ae) / len(ae),
                 max(pe), sum(pe) / len(pe), max(pr), sum(pr) / len(pr),
                 "all +" if sgn == {1} else "all -" if sgn == {-1}
                 else "mixed sign"))

    print()
    stats(ok, "ALL")
    stats([r for r in ok if r["stride"] == 1], "stride 1")
    stats([r for r in ok if r["stride"] == 2], "stride 2")
    exact = [r for r in ok if r["W"] % M.L == 0]
    ragged = [r for r in ok if r["W"] % M.L != 0]
    stats(exact, "W divisible by 8")
    stats(ragged, "W NOT divisible by 8")

    # Is the stride-1 error a constant offset rather than a scaling error?
    s1 = [r for r in ok if r["stride"] == 1]
    if s1:
        offs = sorted(set(r["abs_err"] for r in s1))
        print()
        print("  stride-1 absolute error takes %d distinct value(s): %s"
              % (len(offs), offs[:10]))
        if len(offs) == 1:
            print("  -> a CONSTANT offset of %+d cycles, independent of H, W and"
                  % offs[0])
            print("     c_in. That is a fill/drain latency, not a rate error.")

    print()
    print("  wrote %s" % OUT)
    return 0


if __name__ == "__main__":
    sys.exit(main())
