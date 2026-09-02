#!/usr/bin/env python3
"""
analyze_oos.py -- join the frozen analytical service model against the silicon
                  measurements produced by oos_validation.c.

    python analyze_oos.py <board_log.txt>

Reads the #BLOCKCSV / #CANDCSV lines out of a raw serial log and writes:
    service_model_oos_validation.csv     per-block predicted vs measured
    service_model_oos_summary.csv        per-candidate predicted vs measured

STEP 10 -- DO NOT FIT THE MODEL. This script contains no free parameters, no
regression against the measurements, and no correction factor. Every predicted
value comes from svc_model.py, which was frozen before any measurement was
taken. If agreement is poor, that is the result.
"""
import sys, csv, io, re
from svc_model import predict, walk, pw_cyc_per_group, F_CLK, Q, ceil_div

SCHED = {
    "16-48-64":          [16, 48, 64],
    "16-32-32-32-64-64": [16, 32, 32, 32, 64, 64],
    "16-16-16-16-32-64": [16, 16, 16, 16, 32, 64],
    "16-16-48-32-64":    [16, 16, 48, 32, 64],
    "16-16-64-64":       [16, 16, 64, 64],
    "16-32-48-64-32-64": [16, 32, 48, 64, 32, 64],
}
CONTROLS = {"CTRL-A", "CTRL-B"}


def parse(path):
    blocks, cands = [], []
    for ln in io.open(path, encoding="utf-8", errors="replace"):
        ln = ln.strip()
        if ln.startswith("#BLOCKCSV,") and not ln.startswith("#BLOCKCSV,role"):
            f = ln.split(",")
            blocks.append(dict(role=f[1], candidate=f[2], nblocks=int(f[3]),
                               block=int(f[4]), cout=int(f[5]), hout=int(f[6]),
                               wout=int(f[7]), groups=int(f[8]), frames=int(f[9]),
                               meas_ms=float(f[11]), min_ms=float(f[12]),
                               max_ms=float(f[13]), sd_ms=float(f[14])))
        elif ln.startswith("#CANDCSV,") and not ln.startswith("#CANDCSV,role"):
            f = ln.split(",")
            cands.append(dict(role=f[1], candidate=f[2], nblocks=int(f[3]),
                              frames=int(f[4]), meas_ms=float(f[5]),
                              min_ms=float(f[6]), max_ms=float(f[7]),
                              sd_ms=float(f[8]), sum_blocks_ms=float(f[9])))
    return blocks, cands


def pct(a, b):
    return 100.0 * (a / b - 1.0) if b else float("nan")


def main(path):
    blocks, cands = parse(path)
    if not blocks:
        sys.exit("no #BLOCKCSV lines found in %s -- paste the full board log" % path)

    # ---------------- per-block join ----------------
    brows = []
    for b in blocks:
        sched = SCHED.get(b["candidate"])
        if sched is None:
            sys.exit("unknown candidate in log: %r" % b["candidate"])
        pb = list(walk(sched))[b["block"] - 1]
        pred_ms = pb["t_blk"] / F_CLK * 1e3
        nb = max(1, ceil_div(pb["cout"], Q))
        brows.append(dict(
            role=b["role"], candidate=b["candidate"], nblocks=b["nblocks"],
            block=b["block"], H_in=pb["H"], W_in=pb["W"], G=pb["G"],
            stride=pb["stride"], cin=pb["cin"], cout=pb["cout"],
            batches=nb, Q_last=pb["cout"] - (nb - 1) * Q,
            pw_cyc_per_group=pw_cyc_per_group(pb["cin"], pb["cout"]),
            groups_out=pb["Ho"] * pb["Wo"] // 8,
            pred_binding=pb["bind"],
            pred_T_DW_cyc=pb["t_dw"], pred_T_PW_cyc=pb["t_pw"],
            pred_cyc=pb["t_blk"], pred_ms=round(pred_ms, 6),
            meas_ms=b["meas_ms"], meas_min_ms=b["min_ms"], meas_max_ms=b["max_ms"],
            meas_sd_ms=b["sd_ms"], frames=b["frames"],
            meas_cyc_per_group=round(b["meas_ms"] * 1e-3 * F_CLK / (pb["Ho"] * pb["Wo"] // 8), 3),
            err_ms=round(b["meas_ms"] - pred_ms, 6),
            err_pct=round(pct(b["meas_ms"], pred_ms), 4)))
    with open("service_model_oos_validation.csv", "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=list(brows[0].keys())); w.writeheader(); w.writerows(brows)

    # ---------------- per-candidate join ----------------
    crows = []
    for c in cands:
        _, cyc, ms = predict(SCHED[c["candidate"]])
        crows.append(dict(
            role=c["role"], candidate=c["candidate"], nblocks=c["nblocks"],
            frames=c["frames"], pred_cyc=cyc, pred_ms=round(ms, 6),
            meas_ms=c["meas_ms"], meas_min_ms=c["min_ms"], meas_max_ms=c["max_ms"],
            meas_sd_ms=c["sd_ms"], sum_blocks_ms=c["sum_blocks_ms"],
            err_ms=round(c["meas_ms"] - ms, 6), err_pct=round(pct(c["meas_ms"], ms), 4),
            in_sample=("control" if c["role"] in CONTROLS else "out-of-sample")))
    with open("service_model_oos_summary.csv", "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=list(crows[0].keys())); w.writeheader(); w.writerows(crows)

    P = print
    P("\n" + "=" * 78)
    P(" SERVICE-MODEL OUT-OF-SAMPLE VALIDATION")
    P(" model frozen before measurement; no constant fitted, no correction applied")
    P("=" * 78)

    P("\n--- A. Candidate-level: predicted vs measured ---")
    P("%-8s %-22s %2s %10s %10s %9s %8s" % ("role", "candidate", "nb", "pred ms", "meas ms", "err ms", "err %"))
    for r in crows:
        P("%-8s %-22s %2d %10.4f %10.4f %+9.4f %+8.3f" %
          (r["role"], r["candidate"], r["nblocks"], r["pred_ms"], r["meas_ms"], r["err_ms"], r["err_pct"]))

    P("\n--- B. Per-block: predicted vs measured ---")
    P("%-8s %-22s %2s %4s %5s %8s %9s %9s %8s %s" %
      ("role", "candidate", "b", "cin", "cout", "bind", "pred ms", "meas ms", "err %", "cyc/grp meas"))
    for r in brows:
        P("%-8s %-22s %2d %4d %5d %8s %9.4f %9.4f %+8.3f %12.2f" %
          (r["role"], r["candidate"], r["block"], r["cin"], r["cout"], r["pred_binding"],
           r["pred_ms"], r["meas_ms"], r["err_pct"], r["meas_cyc_per_group"]))

    def stats(v):
        n = len(v)
        if not n: return (float("nan"),) * 4
        m = sum(v) / n
        sd = (sum((x - m) ** 2 for x in v) / n) ** 0.5
        return m, sd, min(v), max(v)

    P("\n--- C. Controls vs out-of-sample (is error inflated out of sample?) ---")
    P("%-14s %2s %9s %9s %9s %9s %9s" % ("group", "n", "mean %", "sd %", "min %", "max %", "max|%|"))
    for lab, sel in (("control", lambda r: r["in_sample"] == "control"),
                     ("out-of-sample", lambda r: r["in_sample"] != "control")):
        v = [r["err_pct"] for r in crows if sel(r)]
        m, sd, lo, hi = stats(v)
        P("%-14s %2d %+9.3f %9.3f %+9.3f %+9.3f %9.3f" % (lab, len(v), m, sd, lo, hi, max(abs(x) for x in v) if v else float("nan")))
    P("  If the out-of-sample row is not materially worse than the control row,")
    P("  the model generalises to topologies it was never tuned on.")

    P("\n--- D. Error vs network depth ---")
    P("%-8s %9s %9s" % ("nblocks", "n", "mean err %"))
    for nb in sorted({r["nblocks"] for r in crows}):
        v = [r["err_pct"] for r in crows if r["nblocks"] == nb]
        P("%-8d %9d %+9.3f" % (nb, len(v), sum(v) / len(v)))

    P("\n--- E. Binding-resource prediction (did the model pick the right bottleneck?) ---")
    P("%-8s %9s %9s %9s" % ("binding", "n blocks", "mean err %", "max|err| %"))
    for bind in ("DW", "PW"):
        v = [r["err_pct"] for r in brows if r["pred_binding"] == bind]
        if v:
            P("%-8s %9d %+9.3f %9.3f" % (bind, len(v), sum(v) / len(v), max(abs(x) for x in v)))
    P("  A block whose measured time falls BELOW the predicted binding term would")
    P("  mean the model chose the wrong bottleneck. Count of such blocks: %d"
      % sum(1 for r in brows if r["err_pct"] < -1.0))

    P("\n--- F. Residual structure ---")
    v = [r["err_pct"] for r in brows]
    m, sd, lo, hi = stats(v)
    P("  per-block residual: mean %+.3f%%  sd %.3f%%  range [%+.3f%%, %+.3f%%]  n=%d" % (m, sd, lo, hi, len(v)))
    v = [r["err_pct"] for r in crows]
    m2, sd2, lo2, hi2 = stats(v)
    P("  candidate residual: mean %+.3f%%  sd %.3f%%  range [%+.3f%%, %+.3f%%]  n=%d" % (m2, sd2, lo2, hi2, len(v)))
    P("  sign: %d of %d block residuals positive (measured slower than predicted)"
      % (sum(1 for r in brows if r["err_ms"] > 0), len(brows)))
    P("  A uniformly positive residual is expected and is NOT corrected for: the")
    P("  model counts accelerator service cycles only, while the measured window")
    P("  additionally contains DMA descriptor issue and completion polling.")
    P("  The scale factor that would null it is reported for information only")
    P("  and is NOT applied anywhere: %.5f" % (1.0 + m2 / 100.0))

    P("\nwrote service_model_oos_validation.csv (%d block rows)" % len(brows))
    P("wrote service_model_oos_summary.csv (%d candidate rows)" % len(crows))


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    main(sys.argv[1])
