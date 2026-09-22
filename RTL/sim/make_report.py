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
def residual_finding(ok):
    """Characterise the residual at BOTH boundaries. THIS DOES NOT CHANGE THE
    MODEL -- it states what the measurement says."""
    from collections import Counter

    def rc(c, G):
        return c * (G + M.DELTA_FLUSH) + M.DELTA_ROW

    s1 = [r for r in ok if r["stride"] == "1"]
    s2 = [r for r in ok if r["stride"] == "2"]
    off_core1 = sorted(set(int(r["abs_err"]) for r in s1))

    # AXIS-boundary offsets
    ax1 = Counter(int(r["axis_cycles"]) - int(r["predicted_cycles"]) for r in s1)
    ax2 = Counter(int(r["axis_cycles"]) - int(r["predicted_cycles"]) for r in s2)

    dev = []
    hit1 = hit2 = 0
    for r in ok:
        H, W, C, G = (int(r[k]) for k in ("H", "W", "c_in", "G"))
        m = int(r["measured_cycles"])
        exp = ((H + 1) if r["stride"] == "1" else H) * rc(C, G) + 19
        if m == exp:
            hit1 += (r["stride"] == "1")
            hit2 += (r["stride"] == "2")
        else:
            dev.append((H, W, C, r["stride"], G, m - exp))

    L = []
    A = L.append
    A("### The disagreement, and what it is")
    A("")
    A("**No constant was changed and no correction term was added.** What "
      "follows describes the residual; it does not repair it.")
    A("")
    A("#### At the AXIS boundary the model needs no stride term")
    A("")
    A("Measured at the AXIS ports -- first `s_axis_tvalid && s_axis_tready` to "
      "last `m_axis_tvalid && m_axis_tready` -- the residual is a **constant**:")
    A("")
    A("| stride | n | `axis_cycles - T_DW` |")
    A("|---|---|---|")
    A("| 1 | %d | %s |" % (len(s1), ", ".join("**%+d** (x%d)" % (k, v)
                                              for k, v in sorted(ax1.items()))))
    A("| 2 | %d | %s |" % (len(s2), ", ".join("**%+d** (x%d)" % (k, v)
                                              for k, v in sorted(ax2.items()))))
    A("")
    A("Zero variance on stride 1, and one exception on stride 2 -- the "
      "degenerate `H=1` case. **`T_DW` is exact as a rate model at this "
      "boundary**, off only by a fixed pipeline latency, and it needs no "
      "stride term to be so.")
    A("")
    A("#### At the core boundary it does need one")
    A("")
    A("At stride 1 the core residual is a single value across all %d "
      "configurations: **%+d cycles**, independent of `H`, `W` and `c_in` -- "
      "the datapath fill/drain (%d stages, `1 + 9 + 8`, per the RTL's own "
      "comment) that a throughput-only model does not contain."
      % (len(s1), off_core1[0] if len(off_core1) == 1 else 0, 18))
    A("")
    A("At stride 2 it is not constant. It is independent of `H` -- for a given "
      "`(G, c_in)` the same value appears at every height -- and equals "
      "exactly one row's work:")
    A("")
    A("```")
    A("  stride 1:  core  ==  (H+1) * [c_in*(G+1) + 4]  +  19")
    A("  stride 2:  core  ==   H    * [c_in*(G+1) + 4]  +  19")
    A("```")
    A("")
    A("exact on **%d of %d** stride-1 and **%d of %d** stride-2 configurations."
      % (hit1, len(s1), hit2, len(s2)))
    A("")
    if dev:
        A("The %d exceptions:" % len(dev))
        A("")
        A("| H | W | c_in | stride | G | residual |")
        A("|---|---|---|---|---|---|")
        for H, W, C, st, G, d in dev:
            A("| %d | %d | %d | %s | %d | %+d |" % (H, W, C, st, G, d))
        A("")
        odd = [d for d in dev if d[4] % 2 == 1]
        if odd and all(d[5] == -d[2] for d in odd):
            A("`W=67` and `W=100` are the only configurations in the sweep with "
              "an **odd `G`**, and both miss by exactly `-c_in`. Stride 2 "
              "decimates groups in pairs, so an odd `G` leaves a half pair. "
              "`W=1279` and `W=1435` are also ragged but have even `G` and land "
              "exactly. **So the ragged-width question splits in two:** "
              "`G = ceil(W/L)` is the right group count and the ceiling "
              "correction is what makes those configurations land at all; the "
              "residual then depends on the **parity** of `G`, not on whether "
              "`W` divides `L`. With exact division these four would have been "
              "wrong by a whole group per channel per row instead.")
            A("")
        if [d for d in dev if d[0] == 1]:
            A("`H=1` at stride 2 is degenerate -- a 3x3 window over one row -- "
              "and is listed for completeness, not as a usable configuration.")
            A("")
    A("#### Reading the two together")
    A("")
    A("The two boundaries do not disagree about the engine's rate. They "
      "disagree about **where the window starts and stops**. The AXIS window "
      "spans the whole input stream; the core window ends at the last "
      "`valid_out`, and at stride 2 the core stops emitting one row before the "
      "input runs out, because only even output rows survive. That one row is "
      "the entire stride-2 residual, and it is why the core number is *lower* "
      "than `T_DW` while the AXIS number is a constant *above* it.")
    A("")
    A("**Which boundary should the paper quote?** Blocks are composed over "
      "AXIS -- DW streams into PW through those ports -- so the AXIS window is "
      "the service the composed system actually sees, and that is the one "
      "`T_DW` predicts to a constant. The core-level result is reported "
      "because it was asked for and because it localises the difference, not "
      "because the model is wrong.")
    A("")
    A("**Hypothesis for the one row.** The `(H+1)` form counts a "
      "vertical-flush row pass beyond the `H` real rows. At stride 1 that pass "
      "emits and the core window contains it. At stride 2 it produces no "
      "surviving output row, so the core window closes before it -- consistent "
      "with a residual of exactly one row, independent of `H`. Confirming that "
      "requires the windower's row FSM, which was deliberately out of scope.")
    return "\n".join(L)


def deployed_three_way(sweep):
    """model T_DW vs DW-alone RTL vs fused block on the board."""
    idx = {}
    for r in sweep:
        if r["status"] != "OK":
            continue
        idx[(int(r["H"]), int(r["W"]), int(r["c_in"]), int(r["stride"]))] = r
    want = ((720, 1280, 3), (360, 640, 16), (180, 320, 48))
    if not all((h, w, c, 2) in idx for h, w, c in want):
        return ""
    L = []
    A = L.append
    A("### `T_DW` against DW alone and against the fused block")
    A("")
    A("| blk | H x W in | binds | model `T_DW` | DW alone (RTL) | fused block (board) | model vs DW alone |")
    A("|---|---|---|---|---|---|---|")
    binds = ["PW", "DW", "DW"]
    board = [519919, 470288, 357989]
    for i, (h, w, c) in enumerate(want):
        r = idx[(h, w, c, 2)]
        dw, mdl = int(r["measured_cycles"]), int(r["predicted_cycles"])
        A("| %d | %dx%d | %s | %d | %d | %d | %+.2f%% |"
          % (i + 1, h, w, binds[i], mdl, dw, board[i], 100.0 * (mdl - dw) / dw))
    A("")
    A("> **Superseded by Item 1 below.** The cancellation described in this "
      "paragraph is an artifact of the core boundary; at the AXIS boundary the "
      "two terms have the same sign and add. Kept for the record.")
    A("")
    A("On the two DW-bound blocks the model **over**-predicts the depthwise "
      "engine by +0.27% and +0.55%, while **under**-predicting the fused "
      "block by -0.21% and -0.30%. The signs are opposite, so the extra row "
      "the model charges partly stands in for the DW->PW fusion overhead it "
      "does not model. The block-level agreement is therefore better than the "
      "depthwise model deserves on its own, and should not be presented as "
      "evidence that `T_DW` is correct.")
    return "\n".join(L)


def fusion_overhead(sweep, board, rows):
    """ITEM 1. Separate the model's own offset from the DW->PW fusion cost.
    Arithmetic over measurements already taken; the only modelled quantities
    are T_DW and T_block themselves."""
    idx = {}
    for r in sweep:
        if r["status"] == "OK":
            idx[(int(r["H"]), int(r["W"]), int(r["c_in"]), int(r["stride"]))] = r
    need = [(b["H"], b["W"], b["c_in"], b["s"]) for b in rows]
    if not all(k in idx for k in need):
        return ""

    def ax(b):
        return int(idx[(b["H"], b["W"], b["c_in"], b["s"])]["axis_cycles"])

    L = []
    A = L.append
    A("## Item 1 -- fusion overhead, separated from model error")
    A("")
    A("`a` is the model. `b` is the DW engine **alone**, measured in "
      "simulation at the **AXIS** boundary on the identical geometry -- the "
      "boundary blocks are actually composed over. `c` is the four-way max. "
      "`d` is the fused block on silicon.")
    A("")
    A("| | blk 1 | blk 2 | blk 3 |")
    A("|---|---|---|---|")
    A("| geometry (H x W in) | %s |"
      % " | ".join("%dx%d" % (b["H"], b["W"]) for b in rows))
    A("| c_in / c_out | %s |"
      % " | ".join("%d / %d" % (b["c_in"], b["c_out"]) for b in rows))
    A("| **a** `T_DW` predicted | %s |" % " | ".join("%d" % b["T_DW"] for b in rows))
    A("| **b** DW alone, AXIS, measured | %s |" % " | ".join("%d" % ax(b) for b in rows))
    A("| **c** `T_block` predicted | %s |" % " | ".join("%d" % b["T_block"] for b in rows))
    A("| &nbsp;&nbsp;binds | %s |"
      % " | ".join("**%s**" % b["binds"].replace("T_", "") for b in rows))
    A("| **d** fused block, silicon | %s |"
      % " | ".join("%d" % board[b["block"]]["cycles"] for b in rows))
    A("")

    A("### e. implied fusion overhead")
    A("")
    ov = {}
    for b in rows:
        if b["binds"] == "T_DW":
            ov[b["block"]] = board[b["block"]]["cycles"] - ax(b)
    A("**Blocks 2 and 3 (DW binds).** `d - b` is a difference of two "
      "measurements, so it is what the fused pair costs over the depthwise "
      "engine running alone:")
    A("")
    A("| blk | d | b | overhead | as % of block |")
    A("|---|---|---|---|---|")
    for b in rows:
        if b["block"] in ov:
            A("| %d | %d | %d | **%+d** | %.3f%% |"
              % (b["block"], board[b["block"]]["cycles"], ax(b), ov[b["block"]],
                 100.0 * ov[b["block"]] / board[b["block"]]["cycles"]))
    A("")
    A("**Block 1 (PW binds).** `b` is not the binding term, so `d - b` is not "
      "a fusion overhead and is not reported as one. The comparable residual "
      "is `d - T_PW` = %+d, but `T_PW` is a **model**, not a measurement, so "
      "that number mixes PW model error with fusion cost and cannot separate "
      "them. **Isolating fusion overhead on block 1 needs a PW-alone RTL "
      "measurement, which this study does not have.**"
      % (board[1]["cycles"] - int(rows[0]["T_PW"])))
    A("")

    A("### The block-level error decomposes exactly")
    A("")
    A("| blk | `T_block - d` | `T_DW - b` | `b - d` |")
    A("|---|---|---|---|")
    for b in rows:
        if b["block"] in ov:
            A("| %d | %+d | %+d | %+d |"
              % (b["block"], b["T_block"] - board[b["block"]]["cycles"],
                 b["T_DW"] - ax(b), ax(b) - board[b["block"]]["cycles"]))
    A("")
    A("**This corrects a statement made earlier in this report.** The earlier "
      "text said the model's extra row and the unmodelled fusion overhead have "
      "*opposite* signs and partly cancel, so the block agreement flattered "
      "the model. That was an artifact of comparing against the **core** "
      "boundary. At the AXIS boundary -- the correct one, since blocks compose "
      "over AXIS -- both terms are **negative and simply add**: the model sits "
      "a constant -33 cycles below the depthwise engine, and fusion adds ~1,000 "
      "more. There is no cancellation, and the block-level agreement is not "
      "luck.")
    A("")

    A("### Constant, or does it scale?")
    A("")
    if len(ov) >= 2:
        ks = sorted(ov)
        b2, b3 = rows[ks[0] - 1], rows[ks[1] - 1]
        o2, o3 = ov[ks[0]], ov[ks[1]]
        P2 = (b2["H"] * b2["W"]) // (b2["s"] ** 2)
        P3 = (b3["H"] * b3["W"]) // (b3["s"] ** 2)
        A("| quantity | blk %d | blk %d | ratio |" % (ks[0], ks[1]))
        A("|---|---|---|---|")
        A("| **overhead (cyc)** | %d | %d | **%.3f** |" % (o2, o3, o2 / float(o3)))
        A("| output pixels | %d | %d | %.2f |" % (P2, P3, P2 / float(P3)))
        A("| c_in | %d | %d | %.2f |" % (b2["c_in"], b3["c_in"],
                                         b2["c_in"] / float(b3["c_in"])))
        A("| c_out | %d | %d | %.2f |" % (b2["c_out"], b3["c_out"],
                                          b2["c_out"] / float(b3["c_out"])))
        A("| G | %d | %d | %.2f |" % (b2["G"], b3["G"], b2["G"] / float(b3["G"])))
        A("")
        k = (o2 - o3) / float(P2 - P3)
        A("**Approximately constant.** Output volume changes by %.0fx between "
          "the two blocks while the overhead changes by %.0f%%, in the "
          "*opposite* direction. A per-beat cost is therefore excluded: "
          "fitting `overhead = F + k*P_out` gives `k = %.6f` cycles per output "
          "pixel, **negative and unphysical**. `c_in` moves %.0fx the other "
          "way, `c_out` %.2fx and `G` %.0fx, and none of them tracks it either."
          % (P2 / float(P3), 100.0 * abs(o2 / float(o3) - 1.0), k,
             b3["c_in"] / float(b2["c_in"]), b3["c_out"] / float(b2["c_out"]),
             b2["G"] / float(b3["G"])))
        A("")
        A("**What two points cannot settle.** A fixed handshake cost and a "
          "*weak* per-beat cost cannot be separated -- two usable measurements, "
          "two free parameters. A *strong* per-beat cost is ruled out by the "
          "ratio; anything smaller is not resolvable, and no trend is fitted "
          "to two points. Block 1 supplies no third point because PW binds "
          "there.")
        A("")
        A("### One sentence, or a real gap?")
        A("")
        lo, hi = min(o2, o3), max(o2, o3)
        p2 = 100.0 * o2 / board[ks[0]]["cycles"]
        p3 = 100.0 * o3 / board[ks[1]]["cycles"]
        A("**One sentence, with a stated bound.** The model omits a per-block "
          "DW->PW fusion overhead of about %d-%d cycles, %.2f-%.2f%% of a "
          "block, which on the available evidence does not scale with output "
          "volume, channel count or group count. It is a real omission -- once "
          "the constant pipeline offset is removed it is the *whole* of the "
          "block-level residual -- but it is small, one-directional and "
          "bounded. What would make it a real gap is a schedule where it stops "
          "being roughly constant, and two points cannot say where that is."
          % (lo, hi, min(p2, p3), max(p2, p3)))
    return "\n".join(L)


def fsm_confirmation():
    """ITEM 2. Confirm or refute the stride-2 row hypothesis against the RTL."""
    L = []
    A = L.append
    A("## Item 2 -- the stride-2 row mechanism, confirmed against the RTL")
    A("")
    A("Scope restriction lifted after `model/service_model.py` was committed "
      "unchanged, so the model's independence is banked and this read cannot "
      "retroactively affect it.")
    A("")
    A("**Confirmed. The RTL states it in its own header comment.**")
    A("")
    A("### The `(H+1)` row count")
    A("")
    A("`dw_banked_window_8x.sv`, emission order:")
    A("")
    A("```")
    A("  for row r in 0..H-1: for group g in 0..G-1: for channel c in 0..C-1:")
    A("    one 8-sample beat")
    A("  (plus one extra vertical-flush row, r==H, all zp_in)")
    A("```")
    A("")
    A("and the control logic that implements it:")
    A("")
    A("```systemverilog")
    A("real_row         = running && (r_cnt < H_r);   // input needed only r < H")
    A("real_group       = (g_cnt < G_r);              // false only in the flush slot")
    A("last_beat_of_row = last_beat_of_group && (g_cnt == G_r);")
    A("...")
    A("pending_finish  <= (r_cnt == H_r);             // terminate AFTER the r==H pass")
    A("if (r_cnt != H_r) r_cnt <= r_cnt + 12'd1;")
    A("```")
    A("")
    A("`r_cnt` takes the values `0 .. H_r` inclusive -- **H+1 row passes** -- "
      "and the last is not an input row (`real_row` false), which is why it "
      "costs time without consuming beats. Two more model constants fall out "
      "of the same block:")
    A("")
    A("| constant | value | RTL |")
    A("|---|---|---|")
    A("| `delta_flush` | 1 | `real_group = (g_cnt < G_r)` -- `g_cnt` runs `0..G`, the extra slot being the per-row horizontal flush |")
    A("| `delta_row` | 4 | `localparam int DRAIN_CYCLES = 4` -- the per-row pend_ram write-back drain |")
    A("")
    A("### Why the core window closes one row early at stride 2")
    A("")
    A("From the same header:")
    A("")
    A("> Rows: only even Y survive (Y = s2_row - 1). **The r==H vertical-flush "
      "row emits Y=H-1, odd, so for even H it is discarded** -- harmless, and "
      "left in place rather than special-cased so the FSM is untouched.")
    A("")
    A("That is the mechanism verbatim. The `r==H` pass still **runs** -- the "
      "FSM is identical at both strides, which is why the AXIS window, which "
      "spans the input stream and the engine's full execution, shows no stride "
      "dependence -- but at stride 2 it **emits nothing that survives**, so "
      "the last `valid_out` falls one row earlier and the core window closes "
      "one row short. Exactly the measured residual: one row, independent of "
      "`H`, on 71 of 74 configurations.")
    A("")
    A("### Do the AXIS constants follow from the same mechanism?")
    A("")
    A("Partly. The part that does not is stated rather than guessed at.")
    A("")
    A("`dw_fused_core.sv` sets `TOT_LAT = 1 + MAC_LAT + 8 = 1 + 9 + 8 = 18` "
      "and delays `done_out` by that much. The measured core offset is "
      "**+19 = TOT_LAT + 1** -- datapath fill/drain plus the one cycle between "
      "accepting the first beat and the window's first counted cycle. The core "
      "constant is fully accounted for.")
    A("")
    A("The AXIS constants are that plus the shell:")
    A("")
    A("```")
    A("  stride 1:  +36  =  19 (core)  +  17 (AXIS FIFOs + output holding reg)")
    A("  stride 2:  +33  =  19 (core)  +  14")
    A("```")
    A("")
    A("The 17-versus-14 difference is a **3-cycle constant** on the output "
      "side, uniform across every `c_in` from 3 to 64, so it is structural and "
      "not data-dependent. `m_axis` is driven from an output holding register "
      "rather than straight off the FIFO (for TLAST), and that path drains "
      "differently when the core emits at a quarter rate. **I have not "
      "localised those 3 cycles to a specific stage.** The honest statement: "
      "the AXIS offset is a constant per stride, its dominant term is the "
      "datapath latency the RTL declares, and a 3-cycle stride-dependent tail "
      "in the output path remains unexplained.")
    A("")
    A("### A precondition the sweep tripped over -- and it is not a timing bug")
    A("")
    A("The same header, for stride 2:")
    A("")
    A("> **PRECONDITIONS for stride2 (checked by the driver, NOT by "
      "hardware):** `n_groups` must be EVEN, i.e. `img_width` a multiple of "
      "16. Otherwise the final group of every row is an even-index group that "
      "never gets a partner, and **its 4 output pixels are silently dropped**.")
    A("")
    A("That is exactly the `W=67` (`G=9`) and `W=100` (`G=13`) residual: "
      "`-c_in` cycles, one dropped beat per channel per row. So those two "
      "configurations are **functionally invalid, not merely three cycles "
      "off** -- the RTL drops output there and says the driver must prevent "
      "it. They are kept in `dw_sweep.csv` with this note rather than deleted: "
      "the timing is real and the reason they differ is now understood.")
    A("")
    A("It also sharpens the ragged-width answer. `G = ceil(W/L)` is required "
      "and correct. Beyond that, stride 2 needs `G` **even**, which is "
      "stronger than `W` dividing `L`: `W=1279` and `W=1435` are ragged, have "
      "even `G`, and land exactly. Every deployed width (1280/640/320) is a "
      "multiple of 16, so all three deployed blocks satisfy the precondition.")
    return "\n".join(L)


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
            A(residual_finding(ok))
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

    # ---- three-way, if the sweep has the deployed geometries --------------
    if sweep:
        tw = deployed_three_way(sweep)
        if tw:
            A(tw)
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
    if sweep and board:
        fo = fusion_overhead(sweep, board, rows)
        if fo:
            A(fo); A("")
    A(fsm_confirmation()); A("")

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
