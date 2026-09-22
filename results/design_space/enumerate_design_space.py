"""
enumerate_design_space.py -- full design-space enumeration with the FPGA
latency model, implemented from the written specification.

    python enumerate_design_space.py

Needs numpy, scipy, matplotlib (Anaconda base on this machine; the Vivado
Python has no matplotlib).

The model below is written from the spec text, NOT imported from
model/service_model.py. The frozen model is used only afterwards, as an
independent cross-check that both implementations agree on every sequence.
Nothing is fitted. All arithmetic is exact (Fraction); cycles are exact
integers on every geometry used here, and that is asserted.

Outputs, all in this directory:
    design_space_runA.csv, design_space_runB.csv
    scatter_mmac_runA.pdf/.png, scatter_weights_runA.pdf/.png  (and runB)
    summary.txt
"""
import csv, itertools, os, sys
from fractions import Fraction
from math import ceil

import numpy as np
from scipy.stats import spearmanr
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.lines import Line2D

HERE = os.path.dirname(os.path.abspath(__file__))

# ---------------------------------------------------------------------------
# Model constants (spec)
# ---------------------------------------------------------------------------
L, Q, W_DMA, R_SH = 8, 32, 8, 2
F_CLK = 100_000_000
D_ACC, D_PPU, D_TR, D_FLUSH, D_ROW = 6, 2, 1, 1, 4

H0, W0, C0 = 720, 1280, 3
N_STRIDE2 = 3
WIDTHS = (16, 32, 48, 64)
DEPTHS = (3, 4, 5, 6)
LAST_WIDTH = 64

# Hardware limit of the deployed bitstream (dw_banked_window_8x MAX_CG_PRODUCT).
# Not part of the latency model; reported as a column, never used to filter.
MAX_CG_PRODUCT = 2048


def cdiv(a, b):
    return -(-a // b)


def group_latency(cin, cout):
    B = cdiv(cout, Q)
    q_last = cout - (B - 1) * Q
    gamma = cin + cdiv(q_last, R_SH) + D_ACC
    if B == 1:
        return max(cout + D_PPU, gamma)
    delta = max(cin + D_ACC, Q + D_PPU)
    t_ret = cout + B * D_PPU
    t_acc = cin + D_ACC + (B - 2) * delta + max(delta + D_TR, gamma)
    return max(t_ret, t_acc)


def exact_int(x, what):
    x = Fraction(x)
    if x.denominator != 1:
        raise ValueError("%s is not an integer cycle count: %s" % (what, x))
    return int(x)


def block_latency(H, W, cin, cout, s):
    p_in = H * W
    P = Fraction(p_in, s * s)
    G = cdiv(W, L)
    services = {
        "READ":  exact_int(Fraction(p_in * cin, W_DMA), "Tread"),
        "DW":    (H + 1) * (cin * (G + D_FLUSH) + D_ROW),
        "PW":    exact_int(P / L * group_latency(cin, cout), "TPW"),
        "WRITE": exact_int(P * cout / W_DMA, "Twrite"),
    }
    # max, ties broken in spec order read, DW, PW, write
    bott = max(services, key=lambda k: services[k])
    macs = exact_int(P * (9 * cin + cin * cout), "MACs")
    return dict(cycles=services[bott], bott=bott, services=services,
                macs=macs, weights=9 * cin + cin * cout, cg=G * cin)


def encoder(seq):
    H, W, cin = H0, W0, C0
    blocks = []
    for j, cout in enumerate(seq):
        s = 2 if j < N_STRIDE2 else 1
        b = block_latency(H, W, cin, cout, s)
        blocks.append(b)
        H, W, cin = cdiv(H, s), cdiv(W, s), cout
    cyc = sum(b["cycles"] for b in blocks)
    return dict(
        seq="-".join(map(str, seq)), depth=len(seq), cycles=cyc,
        t_ms=cyc * 1000.0 / F_CLK,
        mmac=sum(b["macs"] for b in blocks) / 1e6,
        weights=sum(b["weights"] for b in blocks),
        bott="-".join(b["bott"] for b in blocks),
        block_cycles=[b["cycles"] for b in blocks],
        feasible=all(b["cg"] <= MAX_CG_PRODUCT for b in blocks),
        infeasible_at=";".join("b%d G*Cin=%d" % (i + 1, b["cg"])
                               for i, b in enumerate(blocks)
                               if b["cg"] > MAX_CG_PRODUCT),
    )


def parse(name):
    return [int(x) for x in name.split("-")]


# ---------------------------------------------------------------------------
# 1. Validation against the known values -- abort on any mismatch
# ---------------------------------------------------------------------------
def validate(log):
    ok = True

    def chk(label, got, want):
        nonlocal ok
        good = got == want
        ok &= good
        log("  %-46s got %-12s want %-12s %s" % (label, got, want,
                                                  "ok" if good else "MISMATCH"))

    log("VALIDATION (Q=32)")
    for name, t, mm, wt in [("16-48-64", "13.446", "120.269", 4491),
                            ("16-32-32-32-64-64", "17.695", "193.766", 10363),
                            ("16-16-64-64", "13.909", None, None),
                            ("16-16-48-32-64", "13.988", None, None),
                            ("16-16-16-16-32-64", "13.952", None, None)]:
        e = encoder(parse(name))
        chk(name + "  T_HW ms", "%.3f" % e["t_ms"], t)
        if mm:
            chk(name + "  MMAC", "%.3f" % e["mmac"], mm)
            chk(name + "  weights", e["weights"], wt)
    e = encoder(parse("16-48-64"))
    chk("16-48-64  per-block cycles", e["block_cycles"], [518400, 469300, 356932])
    chk("16-48-64  bottlenecks", e["bott"], "PW-DW-DW")

    log("")
    log("LABELING DISCREPANCY: which sequence gives 17.766 ms?")
    hits = []
    for name in ("16-48-48-64", "16-48-48-48-64"):
        e = encoder(parse(name))
        mark = "<-- 17.766" if "%.3f" % e["t_ms"] == "17.766" else ""
        if mark:
            hits.append(name)
        log("  %-20s %.4f ms  %8.3f MMAC  %6d weights  %s  %s"
            % (name, e["t_ms"], e["mmac"], e["weights"], e["bott"], mark))
    log("  -> 17.766 ms is %s" % (" and ".join(hits) if hits else "NEITHER"))
    return ok


# ---------------------------------------------------------------------------
# 2. Enumeration
# ---------------------------------------------------------------------------
def enumerate_run(first_fixed):
    rows = []
    for n in DEPTHS:
        firsts = (16,) if first_fixed else WIDTHS
        for f in firsts:
            for mid in itertools.product(WIDTHS, repeat=n - 2):
                rows.append(encoder([f, *mid, LAST_WIDTH]))
    return rows


def cross_check_frozen(rows, log):
    """Independent implementation vs the frozen repo model, every sequence."""
    sys.path.insert(0, os.path.join(HERE, "..", "..", "model"))
    import service_model as fm
    bad = 0
    for r in rows:
        bl = fm.transform(parse(r["seq"]), Q=Q)
        fc = [int(round(b["T_block"])) for b in bl]
        fb = "-".join(b["binds"].replace("T_", "").upper() for b in bl)
        if fc != r["block_cycles"] or fb != r["bott"]:
            bad += 1
            if bad <= 5:
                log("  DISAGREE %s: spec %s %s / frozen %s %s"
                    % (r["seq"], r["block_cycles"], r["bott"], fc, fb))
    log("  spec implementation vs model/service_model.py: %d of %d sequences "
        "disagree" % (bad, len(rows)))
    return bad == 0


def write_csv(rows, path):
    with open(path, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["sequence", "depth", "T_HW_ms", "cycles", "MMAC", "weights",
                    "bottlenecks", "block_cycles", "hw_feasible_on_bitstream",
                    "infeasible_at"])
        for r in sorted(rows, key=lambda r: (r["t_ms"], -r["mmac"])):
            w.writerow([r["seq"], r["depth"], "%.6f" % r["t_ms"], r["cycles"],
                        "%.6f" % r["mmac"], r["weights"], r["bott"],
                        "/".join(map(str, r["block_cycles"])),
                        int(r["feasible"]), r["infeasible_at"]])


# ---------------------------------------------------------------------------
# 3. Frontier: for each latency, the maximum capacity achievable
# ---------------------------------------------------------------------------
def frontier(rows, key):
    """Pareto set (min latency, max capacity). A point is on it iff no other
    sequence has latency <= and capacity >, or latency < and capacity >=."""
    pts = sorted(rows, key=lambda r: (r["t_ms"], -r[key]))
    front, best = [], -1.0
    for r in pts:
        if r[key] > best:
            front.append(r)
            best = r[key]
    return front


# ---------------------------------------------------------------------------
# 4. Plot
# ---------------------------------------------------------------------------
CANDIDATES = [  # (sequence, legend label, marker)
    ("16-32-32-32-64-64", "$C_{\\mathrm{ref}}$ 16-32-32-32-64-64", "*"),
    ("16-48-64",          "Selected 16-48-64",   "D"),
    ("16-16-64-64",       "16-16-64-64",         "s"),
    ("16-64-64-64",       "16-64-64-64",         "^"),
    ("16-16-48-32-64",    "16-16-48-32-64",      "v"),
    ("16-48-48-48-64",    "16-48-48-48-64",      "P"),
    ("16-32-48-64-32-64", "16-32-48-64-32-64",   "X"),
    ("16-16-16-16-32-64", "16-16-16-16-32-64",   "h"),
]
DEPTH_COLORS = {3: "#0072B2", 4: "#E69F00", 5: "#009E73", 6: "#CC79A7"}  # Okabe-Ito

plt.rcParams.update({
    "font.family": "serif",
    "font.serif": ["Times New Roman", "Times", "DejaVu Serif"],
    "mathtext.fontset": "stix",
    "font.size": 8, "axes.labelsize": 8, "axes.titlesize": 8,
    "xtick.labelsize": 7, "ytick.labelsize": 7, "legend.fontsize": 6.5,
    "axes.linewidth": 0.6, "xtick.major.width": 0.6, "ytick.major.width": 0.6,
    "pdf.fonttype": 42, "ps.fonttype": 42,      # embedded TrueType (IEEE)
    "savefig.bbox": "tight", "savefig.pad_inches": 0.02,
})


def draw_layer(ax, rows, key, by, fr, t_max, small=False):
    """Population (filled = runs on the current bitstream, open = exceeds the
    DW buffer), frontier, and the named candidates."""
    s = 10 if small else 6
    for d in DEPTHS:
        for feas in (True, False):
            rs = [r for r in rows if r["depth"] == d and r["feasible"] == feas]
            if not rs:
                continue
            x, y = [r["t_ms"] for r in rs], [r[key] for r in rs]
            if feas:
                ax.scatter(x, y, s=s, c=DEPTH_COLORS[d], alpha=0.6,
                           linewidths=0, zorder=2)
            else:
                ax.scatter(x, y, s=s, facecolors="none",
                           edgecolors=DEPTH_COLORS[d], alpha=0.6,
                           linewidths=0.45, zorder=2)
    fx = [r["t_ms"] for r in fr]
    fy = [r[key] for r in fr]
    ax.step(fx + [t_max], fy + [fy[-1]], where="post", color="black",
            lw=0.8, zorder=3)
    for seq, _, mk in CANDIDATES:
        if seq in by:
            r = by[seq]
            ax.scatter([r["t_ms"]], [r[key]], marker=mk,
                       s=(55 if mk == "*" else 24) * (1.5 if small else 1.0),
                       facecolors="white", edgecolors="black",
                       linewidths=0.8, zorder=4)


def plot(rows, key, ylabel, run, path_stem):
    by = {r["seq"]: r for r in rows}
    fr = frontier(rows, key)
    t_max = max(r["t_ms"] for r in rows)
    fig, ax = plt.subplots(figsize=(3.5, 3.1))       # IEEE single column
    draw_layer(ax, rows, key, by, fr, t_max)
    ax.set_xlabel("Encoder latency $T_{\\mathrm{HW}}$ (ms)")
    ax.set_ylabel(ylabel)
    ax.grid(True, lw=0.3, alpha=0.4, zorder=0)
    ax.set_axisbelow(True)

    # Run B separates into bands by first width; say so on the plot.
    firsts = sorted({int(r["seq"].split("-")[0]) for r in rows})
    if len(firsts) > 1:
        lo = min(r[key] for r in rows)
        span = max(r[key] for r in rows) - lo
        for w1 in firsts:
            band = [r for r in rows if int(r["seq"].split("-")[0]) == w1]
            ax.text(np.median([r["t_ms"] for r in band]), lo - 0.07 * span,
                    "$w_1$=%d" % w1, ha="center", va="top", fontsize=6.5)
        ax.set_ylim(lo - 0.14 * span, None)

    # Zoom on the cluster of candidates around C_ref, which overlap at full scale.
    clus = [by[s] for s in ("16-32-32-32-64-64", "16-32-48-64-32-64",
                            "16-48-48-48-64", "16-64-64-64") if s in by]
    if clus:
        xs, ys = [r["t_ms"] for r in clus], [r[key] for r in clus]
        dx = max(0.08, 0.35 * (max(xs) - min(xs)))
        dy = max(0.12 * (max(ys) - min(ys)), 0.04 * max(ys))
        axin = ax.inset_axes([0.07, 0.56, 0.36, 0.40])
        draw_layer(axin, rows, key, by, fr, t_max, small=True)
        axin.set_xlim(min(xs) - dx, max(xs) + dx)
        axin.set_ylim(min(ys) - dy, max(ys) + dy)
        axin.tick_params(labelsize=5.5, length=2, pad=1)
        for sp in axin.spines.values():
            sp.set_linewidth(0.5)
        ax.indicate_inset_zoom(axin, edgecolor="0.4", lw=0.5, alpha=1.0)

    depth_h = [Line2D([], [], ls="", marker="o", ms=3.5, mfc=DEPTH_COLORS[d],
                      mec="none", label="N = %d" % d) for d in DEPTHS]
    depth_h.append(Line2D([], [], color="black", lw=0.8, label="Frontier"))
    depth_h.append(Line2D([], [], ls="", marker="o", ms=3.5, mfc="none",
                          mec="0.35", mew=0.5, label="Exceeds DW buffer"))
    cand_h = [Line2D([], [], ls="", marker=mk, ms=6.5 if mk == "*" else 4.2,
                     mfc="white", mec="black", mew=0.8, label=lab)
              for seq, lab, mk in CANDIDATES if seq in by]
    fig.tight_layout()
    fig.legend(handles=depth_h, loc="lower center", ncol=3, frameon=False,
               bbox_to_anchor=(0.54, 0.985), columnspacing=1.0,
               handletextpad=0.3, labelspacing=0.2)
    fig.legend(handles=cand_h, loc="upper center", ncol=2, frameon=False,
               bbox_to_anchor=(0.54, 0.015), columnspacing=0.8,
               handletextpad=0.3, labelspacing=0.25)
    fig.savefig(path_stem + ".pdf")
    fig.savefig(path_stem + ".png", dpi=300)
    plt.close(fig)


# ---------------------------------------------------------------------------
# 5. Summary statistics
# ---------------------------------------------------------------------------
def summarize(rows, run, log):
    n = len(rows)
    t = np.array([r["t_ms"] for r in rows])
    mm = np.array([r["mmac"] for r in rows])
    wt = np.array([r["weights"] for r in rows])
    by = {r["seq"]: r for r in rows}
    log("")
    log("=" * 78)
    log("RUN %s: %d sequences (%d feasible on the current bitstream)"
        % (run, n, sum(r["feasible"] for r in rows)))
    log("=" * 78)
    log("latency range %.4f .. %.4f ms;  MMAC range %.3f .. %.3f"
        % (t.min(), t.max(), mm.min(), mm.max()))

    for seq in ("16-32-32-32-64-64", "16-48-64"):
        r = by[seq]
        faster = int((t < r["t_ms"] - 1e-9).sum())
        ties = int((np.abs(t - r["t_ms"]) < 1e-9).sum()) - 1
        more_mac = int((mm > r["mmac"] + 1e-9).sum())
        log("")
        log("%s: %.4f ms, %.3f MMAC, %d weights, %s"
            % (seq, r["t_ms"], r["mmac"], r["weights"], r["bott"]))
        log("  latency: %d of %d sequences are faster, %d tie -> rank %d; "
            "faster than %.1f%% of the space"
            % (faster, n, ties, faster + 1,
               100.0 * (t > r["t_ms"] + 1e-9).sum() / n))
        log("  MMAC   : %d of %d have more MACs -> above %.1f%% of the space"
            % (more_mac, n, 100.0 * (mm < r["mmac"] - 1e-9).sum() / n))

    for key, lab in (("mmac", "MMAC"), ("weights", "weights")):
        fr = frontier(rows, key)
        names = {r["seq"] for r in fr}
        log("")
        log("frontier (%s): %d points" % (lab, len(fr)))
        for r in fr:
            log("  %-20s %.4f ms  %9.3f MMAC  %6d w  %-18s %s"
                % (r["seq"], r["t_ms"], r["mmac"], r["weights"], r["bott"],
                   "" if r["feasible"] else "INFEASIBLE (" + r["infeasible_at"] + ")"))
        for seq in ("16-48-64", "16-32-32-32-64-64"):
            if seq in names:
                log("  %s IS on the %s frontier" % (seq, lab))
            else:
                r = by[seq]
                dom = [x for x in rows if x["t_ms"] <= r["t_ms"] + 1e-9
                       and x[key] > r[key] + 1e-9]
                dom.sort(key=lambda x: (-x[key], x["t_ms"]))
                log("  %s is NOT on the %s frontier; %d sequences are as fast "
                    "and larger, e.g. %s (%.4f ms, %s %s)"
                    % (seq, lab, len(dom), dom[0]["seq"], dom[0]["t_ms"],
                       ("%.3f" % dom[0][key]) if key == "mmac" else dom[0][key], lab))

    log("")
    log("latency spread at similar MMAC (window +/-5% around the reference):")
    for seq in ("16-48-64", "16-32-32-32-64-64"):
        c = by[seq]["mmac"]
        sel = [r for r in rows if abs(r["mmac"] - c) <= 0.05 * c]
        ts = [r["t_ms"] for r in sel]
        lo = min(sel, key=lambda r: r["t_ms"])
        hi = max(sel, key=lambda r: r["t_ms"])
        log("  around %-18s (%.1f MMAC): n=%-3d latency %.3f .. %.3f ms "
            "(x%.2f)  fastest %s, slowest %s"
            % (seq, c, len(sel), min(ts), max(ts), max(ts) / min(ts),
               lo["seq"], hi["seq"]))
    edges = np.linspace(mm.min(), mm.max(), 11)
    log("  10 equal-width MMAC bins:")
    for a, b in zip(edges[:-1], edges[1:]):
        m = (mm >= a) & (mm <= b)
        if m.sum() >= 2:
            log("    %7.1f-%7.1f MMAC  n=%-4d latency %.3f .. %.3f ms  (x%.2f)"
                % (a, b, m.sum(), t[m].min(), t[m].max(), t[m].max() / t[m].min()))

    rho_m, p_m = spearmanr(mm, t)
    rho_w, p_w = spearmanr(wt, t)
    log("")
    log("Spearman rho(MMAC, T_HW)    = %+.4f  (p = %.2e, n = %d)" % (rho_m, p_m, n))
    log("Spearman rho(weights, T_HW) = %+.4f  (p = %.2e, n = %d)" % (rho_w, p_w, n))
    log("bottleneck patterns: " + ", ".join(
        "%s x%d" % (k, v) for k, v in sorted(
            {b: sum(1 for r in rows if r["bott"] == b) for b in
             {r["bott"] for r in rows}}.items(), key=lambda kv: -kv[1])[:8]))


# ---------------------------------------------------------------------------
def main():
    out = []

    def log(s=""):
        print(s)
        out.append(s)

    log("Design-space enumeration, latency model from spec. L=%d Q=%d W_DMA=%d "
        "R_sh=%d, %d MHz" % (L, Q, W_DMA, R_SH, F_CLK // 1_000_000))
    log("")
    if not validate(log):
        log("")
        log("VALIDATION FAILED -- stopping before enumeration.")
        open(os.path.join(HERE, "summary.txt"), "w").write("\n".join(out) + "\n")
        sys.exit(1)
    log("  all validation values match")

    runA = enumerate_run(first_fixed=True)
    runB = enumerate_run(first_fixed=False)
    assert len(runA) == 340 and len(runB) == 1360, (len(runA), len(runB))
    log("")
    log("CROSS-CHECK against the frozen repo model (Run B covers Run A)")
    if not cross_check_frozen(runB, log):
        log("CROSS-CHECK FAILED -- stopping.")
        sys.exit(1)

    for run, rows in (("A", runA), ("B", runB)):
        write_csv(rows, os.path.join(HERE, "design_space_run%s.csv" % run))
        plot(rows, "mmac", "Useful MACs per frame (MMAC)", run,
             os.path.join(HERE, "scatter_mmac_run%s" % run))
        plot(rows, "weights", "Weights (no biases)", run,
             os.path.join(HERE, "scatter_weights_run%s" % run))
        summarize(rows, run, log)

    open(os.path.join(HERE, "summary.txt"), "w").write("\n".join(out) + "\n")


if __name__ == "__main__":
    main()
