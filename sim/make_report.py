"""
make_report.py -- Experiment B (per-block binding service), the board
cross-check, the constant sensitivity, and results/report.md.

Writes  results/per_block.csv
        results/report.md          (summary + the two LaTeX tables)

Reads   results/dw_sweep.csv       (Experiment A, optional)
        results/board_measurements.yaml  (optional; null entries -> predicted only)

NO FITTING. Where prediction and measurement disagree the disagreement is
printed. Nothing is tuned.

  python make_report.py
"""
import csv
import os
import sys
from datetime import date

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(ROOT, "model"))
import service_model as M

RES = os.path.join(ROOT, "results")
SWEEP_CSV = os.path.join(RES, "dw_sweep.csv")
BOARD_YML = os.path.join(RES, "board_measurements.yaml")
BLOCK_CSV = os.path.join(RES, "per_block.csv")
REPORT_MD = os.path.join(RES, "report.md")

PAPER_CYCLES = 1344632
PAPER_MS = 13.4463
SCHEDULE = [16, 48, 64]


# ---------------------------------------------------------------------------
def load_board():
    """Return {block: {...}} or {} if the file is absent/unusable."""
    if not os.path.exists(BOARD_YML):
        return {}, "absent"
    try:
        import yaml
    except ImportError:
        return {}, "pyyaml not installed"
    with open(BOARD_YML, encoding="utf-8") as fh:
        doc = yaml.safe_load(fh) or {}
    out = {}
    for b in (doc.get("blocks") or []):
        if b.get("cycles") is not None:
            out[int(b["block"])] = b
    if not out:
        return {}, "present but every entry is null"
    return out, "loaded"


def load_sweep():
    if not os.path.exists(SWEEP_CSV):
        return []
    with open(SWEEP_CSV, encoding="utf-8") as fh:
        return [r for r in csv.DictReader(fh)]


# ---------------------------------------------------------------------------
def sensitivity(base_total):
    """Step 4: perturb each constant by +/-1, report the change in total."""
    out = []
    for name in M.CONSTANT_NAMES:
        kw_name = {"R_SH": "r_sh", "DELTA_ACC": "d_acc", "DELTA_PPU": "d_ppu",
                   "DELTA_TR": "d_tr", "DELTA_FLUSH": "d_flush",
                   "DELTA_ROW": "d_row"}[name]
        base = getattr(M, name)
        row = {"constant": name, "value": base}
        for d in (-1, +1):
            rows = M.transform(SCHEDULE, **{kw_name: base + d})
            tot = sum(b["T_block"] for b in rows)
            row["d%+d_cycles" % d] = tot - base_total
            row["d%+d_pct" % d] = 100.0 * (tot - base_total) / base_total
        out.append(row)
    return out


# ---------------------------------------------------------------------------
def main():
    rows = M.transform(SCHEDULE)
    total = sum(b["T_block"] for b in rows)
    board, board_state = load_board()
    sweep = load_sweep()

    # ---- per_block.csv ----------------------------------------------------
    fields = ["block", "H_in", "W_in", "c_in", "c_out", "stride", "G",
              "T_read_cyc", "T_DW_cyc", "T_PW_cyc", "T_write_cyc",
              "binds", "T_block_cyc", "T_block_ms",
              "measured_cyc", "measured_ms", "abs_err_cyc", "pct_err"]
    with open(BLOCK_CSV, "w", newline="", encoding="utf-8") as fh:
        w = csv.DictWriter(fh, fieldnames=fields)
        w.writeheader()
        for b in rows:
            m = board.get(b["block"])
            rec = {"block": b["block"], "H_in": b["H"], "W_in": b["W"],
                   "c_in": b["c_in"], "c_out": b["c_out"], "stride": b["s"],
                   "G": b["G"],
                   "T_read_cyc": int(b["T_read"]), "T_DW_cyc": int(b["T_DW"]),
                   "T_PW_cyc": int(b["T_PW"]), "T_write_cyc": int(b["T_write"]),
                   "binds": b["binds"], "T_block_cyc": int(b["T_block"]),
                   "T_block_ms": round(M.ms(b["T_block"]), 5),
                   "measured_cyc": m["cycles"] if m else "",
                   "measured_ms": round(m["cycles"] / M.F_CLK * 1e3, 5) if m else "",
                   "abs_err_cyc": int(b["T_block"] - m["cycles"]) if m else "",
                   "pct_err": round(100.0 * (b["T_block"] - m["cycles"])
                                    / m["cycles"], 4) if m else ""}
            w.writerow(rec)

    # ---- report.md --------------------------------------------------------
    L = []
    A = L.append
    A("# Depthwise service-model validation")
    A("")
    A("Generated %s by `sim/make_report.py`. Model: `model/service_model.py`, "
      "written from the equations alone." % date.today().isoformat())
    A("")
    A("**No constant was fitted.** Where prediction and measurement disagree "
      "the disagreement is reported as the result.")
    A("")

    # ---- Experiment A -----------------------------------------------------
    A("## Experiment A -- DW schedule sweep, RTL vs `T_DW`")
    A("")
    if not sweep:
        A("`results/dw_sweep.csv` not present -- run `sim/analyse_sweep.py`.")
        A("")
    else:
        ok = [r for r in sweep if r["status"] == "OK" and r["measured_cycles"]
              and int(r["measured_cycles"]) > 0]
        A("Window: core = first cycle with `valid_in && consume_in` to the last "
          "cycle with `valid_out`, inclusive. AXIS = first "
          "`s_axis_tvalid && s_axis_tready` to last "
          "`m_axis_tvalid && m_axis_tready`.")
        A("")
        A("`%d` configurations, `%d` usable." % (len(sweep), len(ok)))
        A("")
        if ok:
            errs = [int(r["abs_err"]) for r in ok]
            pcts = [abs(float(r["pct_err"])) for r in ok]
            rows_ = [abs(float(r["err_per_row"])) for r in ok]
            A("| statistic | value |")
            A("|---|---|")
            A("| max abs error | %d cycles |" % max(abs(e) for e in errs))
            A("| mean abs error | %.1f cycles |"
              % (sum(abs(e) for e in errs) / len(errs)))
            A("| max pct error | %.3f%% |" % max(pcts))
            A("| mean pct error | %.4f%% |" % (sum(pcts) / len(pcts)))
            A("| max abs error per row | %.4f cycles/row |" % max(rows_))
            A("| mean abs error per row | %.4f cycles/row |"
              % (sum(rows_) / len(rows_)))
            A("")
            s1 = sorted(set(int(r["abs_err"]) for r in ok if r["stride"] == "1"))
            s2 = sorted(set(int(r["abs_err"]) for r in ok if r["stride"] == "2"))
            A("Distinct absolute errors, stride 1: `%s`" % (s1 if len(s1) < 12 else
                                                            "%d values" % len(s1)))
            A("")
            A("Distinct absolute errors, stride 2: `%s`" % (s2 if len(s2) < 12 else
                                                            "%d values" % len(s2)))
            A("")
            A("Full table: `results/dw_sweep.csv`.")
            A("")

    # ---- Experiment B -----------------------------------------------------
    A("## Experiment B -- per-block binding service, deployed transform")
    A("")
    A("16-48-64, L=8, Q=32, 720p input, three stride-2 DW-PW blocks.")
    A("")
    A("| blk | H x W in | c_in | c_out | T_read | T_DW | T_PW | T_write | binds | T_block | ms |")
    A("|---|---|---|---|---|---|---|---|---|---|---|")
    for b in rows:
        A("| %d | %dx%d | %d | %d | %d | %d | %d | %d | **%s** | %d | %.4f |"
          % (b["block"], b["H"], b["W"], b["c_in"], b["c_out"],
             b["T_read"], b["T_DW"], b["T_PW"], b["T_write"],
             b["binds"].replace("T_", ""), b["T_block"], M.ms(b["T_block"])))
    A("| | | | | | | | | **total** | **%d** | **%.4f** |" % (total, M.ms(total)))
    A("")
    d = total - PAPER_CYCLES
    A("Cross-check against the paper's %s cycles / %.4f ms: **%s** (%+d cycles)."
      % ("{:,}".format(PAPER_CYCLES), PAPER_MS,
         "exact" if d == 0 else "DISCREPANCY", d))
    A("")

    # DMA binding
    dma = [b for b in rows if b["binds"] in ("T_read", "T_write")]
    A("### Do the DMA terms ever bind?")
    A("")
    if not dma:
        A("**No.** Neither `T_read` nor `T_write` binds in any of the three "
          "deployed blocks. Margins to the binding service:")
        A("")
        A("| blk | binds | T_block | T_read | headroom | T_write | headroom |")
        A("|---|---|---|---|---|---|---|")
        for b in rows:
            A("| %d | %s | %d | %d | %.2f%% | %d | %.2f%% |"
              % (b["block"], b["binds"].replace("T_", ""), b["T_block"],
                 b["T_read"], 100.0 * (b["T_block"] - b["T_read"]) / b["T_block"],
                 b["T_write"], 100.0 * (b["T_block"] - b["T_write"]) / b["T_block"]))
        A("")
        worst = min(rows, key=lambda b: (b["T_block"] - b["T_read"]) / b["T_block"])
        A("The tightest margin is block %d, where `T_read` is only **%.2f%%** "
          "below the binding service. It never binds, but it is not far off, "
          "so the statement to make in the paper is that the DMA terms do not "
          "bind *on this schedule* rather than that they cannot."
          % (worst["block"],
             100.0 * (worst["T_block"] - worst["T_read"]) / worst["T_block"]))
    else:
        A("**Yes** -- in block(s) %s. Report this."
          % ", ".join(str(b["block"]) for b in dma))
    A("")

    # ---- board comparison -------------------------------------------------
    A("### Predicted vs measured, per block")
    A("")
    if not board:
        A("`results/board_measurements.yaml` is %s, so this is a "
          "**predicted-only** table." % board_state)
        A("")
    else:
        A("Measured figures are the FUSED DW+PW pair per block. Within a block "
          "DW streams into PW with no DDR round trip, so the pair is the block "
          "and is the correct target for `max(T_read, T_DW, T_PW, T_write)`.")
        A("")
        A("| blk | binding service | predicted | measured | error | pct |")
        A("|---|---|---|---|---|---|")
        tm = 0
        for b in rows:
            m = board.get(b["block"])
            if not m:
                A("| %d | %s | %d | *null* | | |"
                  % (b["block"], b["binds"].replace("T_", ""), b["T_block"]))
                continue
            tm += m["cycles"]
            e = b["T_block"] - m["cycles"]
            A("| %d | %s | %d | %d | %+d | %+.3f%% |"
              % (b["block"], b["binds"].replace("T_", ""), b["T_block"],
                 m["cycles"], e, 100.0 * e / m["cycles"]))
        if tm:
            A("| **sum** | | **%d** | **%d** | **%+d** | **%+.3f%%** |"
              % (total, tm, total - tm, 100.0 * (total - tm) / tm))
            A("")
            A("%.4f ms predicted against %.5f ms measured."
              % (M.ms(total), tm / M.F_CLK * 1e3))
            A("")
            pcts = [100.0 * (b["T_block"] - board[b["block"]]["cycles"])
                    / board[b["block"]]["cycles"] for b in rows if b["block"] in board]
            same = all(p < 0 for p in pcts) or all(p > 0 for p in pcts)
            A("**Per-block errors: %s.** Range %+.3f%% to %+.3f%%, aggregate "
              "%+.3f%%. %s"
              % (", ".join("%+.3f%%" % p for p in pcts), min(pcts), max(pcts),
                 100.0 * (total - tm) / tm,
                 "All the same sign and of similar magnitude, so the aggregate "
                 "figure is representative rather than the result of "
                 "over- and under-prediction cancelling."
                 if same else
                 "**The signs differ, so the aggregate hides cancellation. "
                 "Quote the per-block figures, not the sum.**"))
    A("")

    # ---- Step 4 -----------------------------------------------------------
    A("## Constant sensitivity (+/-1)")
    A("")
    A("Change in total predicted latency for the deployed transform.")
    A("")
    A("| constant | value | -1 cycles | -1 pct | +1 cycles | +1 pct |")
    A("|---|---|---|---|---|---|")
    for s in sensitivity(total):
        A("| `%s` | %d | %+d | %+.3f%% | %+d | %+.3f%% |"
          % (s["constant"], s["value"], s["d-1_cycles"], s["d-1_pct"],
             s["d+1_cycles"], s["d+1_pct"]))
    A("")
    A("A constant with zero sensitivity does not appear in ANY binding service "
      "on this schedule, so this transform cannot validate it at all -- "
      "neither can the board. Say that rather than implying it was checked.")
    A("")

    # ---- LaTeX ------------------------------------------------------------
    A("## LaTeX tables")
    A("")
    A("### Table 1 -- DW sweep")
    A("")
    A("```latex")
    A(r"\begin{tabular}{rrrrrrr}")
    A(r"\toprule")
    A(r"$H$ & $W$ & $c_{\mathrm{in}}$ & $G$ & predicted & measured & err (\%) \\")
    A(r"\midrule")
    if sweep:
        show = [r for r in sweep if r["status"] == "OK"][:14]
        for r in show:
            A(r"%s & %s & %s & %s & %s & %s & %+.3f \\"
              % (r["H"], r["W"], r["c_in"], r["G"], r["predicted_cycles"],
                 r["measured_cycles"], float(r["pct_err"])))
    else:
        A(r"\multicolumn{7}{c}{sweep not yet run} \\")
    A(r"\bottomrule")
    A(r"\end{tabular}")
    A("```")
    A("")
    A("### Table 2 -- per-block binding service")
    A("")
    A("```latex")
    A(r"\begin{tabular}{rrrrrrlrr}")
    A(r"\toprule")
    A(r"blk & $T_{\mathrm{read}}$ & $T_{\mathrm{DW}}$ & $T_{\mathrm{PW}}$ & "
      r"$T_{\mathrm{write}}$ & binds & $T_{\mathrm{block}}$ & meas. & err (\%) \\")
    A(r"\midrule")
    for b in rows:
        m = board.get(b["block"])
        if m:
            e = 100.0 * (b["T_block"] - m["cycles"]) / m["cycles"]
            A(r"%d & %d & %d & %d & %d & %s & %d & %d & %+.3f \\"
              % (b["block"], b["T_read"], b["T_DW"], b["T_PW"], b["T_write"],
                 b["binds"].replace("T_", ""), b["T_block"], m["cycles"], e))
        else:
            A(r"%d & %d & %d & %d & %d & %s & %d & --- & --- \\"
              % (b["block"], b["T_read"], b["T_DW"], b["T_PW"], b["T_write"],
                 b["binds"].replace("T_", ""), b["T_block"]))
    A(r"\midrule")
    if board:
        tm = sum(m["cycles"] for m in board.values())
        A(r"total & & & & & & %d & %d & %+.3f \\"
          % (total, tm, 100.0 * (total - tm) / tm))
    else:
        A(r"total & & & & & & %d & --- & --- \\" % total)
    A(r"\bottomrule")
    A(r"\end{tabular}")
    A("```")
    A("")

    with open(REPORT_MD, "w", encoding="utf-8") as fh:
        fh.write("\n".join(L) + "\n")

    print("wrote %s" % BLOCK_CSV)
    print("wrote %s" % REPORT_MD)
    print("board_measurements.yaml: %s" % board_state)
    print("total predicted %d cyc (%.4f ms), paper %d -> %+d"
          % (total, M.ms(total), PAPER_CYCLES, total - PAPER_CYCLES))
    return 0


if __name__ == "__main__":
    sys.exit(main())
