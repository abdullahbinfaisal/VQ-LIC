"""
bind_check.py -- which engine binds each fused DW->PW pair, model vs board.

WHAT THIS IS FOR. The latency diagram draws the analysis phase as three
DW->PW blocks. Drawing them as opaque boxes hides the one thing a fused
accelerator is supposed to demonstrate: that the two cores run concurrently and
only the slower one costs time. The service model already claims which core
that is, per pair, and the claim is not uniform -- it flips between pair 1 and
pair 2. That has never been checked on silicon.

WHY NOT FROM THE DONE BITS. CASCADE_ENGINE_SPLIT samples both cores' done-sticky
STATUS bits and reports PW_busy - DW_busy. That cannot answer this question at
any sampling rate. The cores are coupled by an AXIS link with backpressure:

  * PW slow  -> PW backpressures DW -> DW cannot finish early -> both done late
  * DW slow  -> PW starves          -> PW finishes just after DW's last beat

Either way the two done bits land within a pipeline depth of each other, so the
measured "tail" is the drain in both cases and the binding engine is invisible.
Worse, the existing report would print "0.00 ms of DW work runs underneath PW",
which reads as a finding and is an artefact.

WHAT DOES WORK. DW's output FIFO. If it ever reached prog_full, PW could not
drain what DW produced, so PW binds. If it never did, PW always kept up, so DW
binds. One sticky bit (DWF_REG_DBG_FLAGS bit 2, out_full_ever), one AXI-Lite
read per pair after the hardware window closes. Binary per pair, not a
duration: it says WHICH core binds, not by how much.

So the diagram gets: measured pair totals, a measured binding engine, and a
modelled margin. Two of those three are silicon.

  python bind_check.py                     # what the model predicts
  python bind_check.py <log.txt>           # and what the board said
"""
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from svc_model import walk, F_CLK

SEL = [16, 48, 64]


def model_rows():
    rows = []
    for i, b in enumerate(walk(SEL)):
        dw = b["t_dw"] / F_CLK * 1e3
        pw = b["t_pw"] / F_CLK * 1e3
        blk = b["t_blk"] / F_CLK * 1e3
        rows.append({
            "pair": i + 1,
            "hxw": "%dx%d" % (b["Ho"], b["Wo"]),
            "t_dw": dw, "t_pw": pw, "t_blk": blk,
            "bind": b["bind"],
            "slack": abs(pw - dw),
        })
    return rows


def parse_board(path):
    """Pull the [PAIR] bind table out of a run log. Returns {pair: verdict}."""
    out = {}
    pat = re.compile(r"^\[PAIR\]\s+(\d+)\s+([\d.]+)\s+(\d+)\s+(\d+)\s+(\S+)")
    seen_header = False
    for line in open(path, encoding="utf-8", errors="replace"):
        if "which engine binds each fused pair" in line:
            seen_header = True
            continue
        if not seen_header:
            continue
        m = pat.match(line.strip())
        if m:
            out[int(m.group(1))] = {
                "hw_ms": float(m.group(2)),
                "nfull": int(m.group(3)),
                "nframe": int(m.group(4)),
                "bind": m.group(5),
            }
        elif line.startswith("[PAIR]  out_full"):
            break
    return out


def main():
    rows = model_rows()
    board = parse_board(sys.argv[1]) if len(sys.argv) > 1 else {}

    print("Which engine binds each fused DW->PW pair, C* = %s"
          % "-".join(map(str, SEL)))
    print("=" * 76)
    print("  %-4s %-9s %8s %8s %8s %8s %6s   %s"
          % ("pair", "HxW", "t_dw ms", "t_pw ms", "blk ms", "slack", "model", "board"))
    print("-" * 76)
    agree = disagree = unknown = 0
    for r in rows:
        b = board.get(r["pair"])
        if b is None:
            bs = "-"
            unknown += 1
        else:
            ok = (b["bind"] == r["bind"])
            bs = "%-5s %s (%d/%d frames)" % (
                b["bind"], "AGREE" if ok else "*** DISAGREE ***",
                b["nfull"], b["nframe"])
            if ok:
                agree += 1
            else:
                disagree += 1
        print("  %-4d %-9s %8.4f %8.4f %8.4f %8.4f %6s   %s"
              % (r["pair"], r["hxw"], r["t_dw"], r["t_pw"], r["t_blk"],
                 r["slack"], r["bind"], bs))
    print("-" * 76)
    tot = sum(r["t_blk"] for r in rows)
    hid = sum(min(r["t_dw"], r["t_pw"]) for r in rows)
    print("  modelled analysis %.4f ms; %.4f ms of it is the faster core running"
          % (tot, hid))
    print("  underneath the slower one and costing nothing. That is the fusion,")
    print("  and it is %.0f%% of the analysis phase." % (100.0 * hid / tot))
    print()

    if not board:
        print("No log given, so the board column is empty. Run:")
        print("  python bind_check.py <run log>")
        print()

    print("WHAT THE DIAGRAM SHOULD SHOW")
    print("  The binding engine is NOT the same for every pair:")
    for r in rows:
        other = "PW" if r["bind"] == "DW" else "DW"
        print("    pair %d  %-9s %s binds, %s hides %.4f ms underneath it"
              % (r["pair"], r["hxw"], r["bind"], other, min(r["t_dw"], r["t_pw"])))
    print("  Three identical opaque boxes would misrepresent that. The bar")
    print("  lengths are measured, the binding engine is measured, and only the")
    print("  split inside each bar is modelled -- label it that way.")

    if board:
        print()
        if disagree:
            print("  %d of %d pairs DISAGREE with the model. The model's t_dw/t_pw"
                  % (disagree, len(rows)))
            print("  split is what is being tested here, and it did not hold. Do not")
            print("  draw the modelled split until this is understood.")
        elif unknown:
            print("  %d pairs had no board verdict." % unknown)
        else:
            print("  All %d pairs agree with the model. The split has never been" % agree)
            print("  checked before; it is checked now, and the diagram can carry it")
            print("  with the margin labelled MODELLED and the binding engine")
            print("  labelled MEASURED.")


if __name__ == "__main__":
    main()
