#!/usr/bin/env python3
"""
Roofline diagram — DW/PW accelerator, ImageEncoderLite @720p on ZC702.

All inputs below are MEASURED on hardware (2026-07-30) except where marked.
Everything plotted is derived from those raw numbers, not hardcoded, so the
figure regenerates correctly if a measurement is corrected.

    python roofline.py            # writes roofline.png / roofline.pdf
    python roofline.py --show     # also opens a window

BANDWIDTH CONVENTION -- read this before changing BW_COMBINED
-------------------------------------------------------------
A 64-bit AXI HP port on Zynq-7000 has INDEPENDENT read and write channels, so
at 100 MHz it sustains 0.8 GB/s in EACH direction simultaneously (1.6 GB/s
aggregate per port). This design uses HP0's read channel (axi_dma_0 MM2S) and
HP1's write channel (axi_dma_1 S2MM), giving 0.8 GB/s read + 0.8 GB/s write.

Arithmetic intensity here is MACs / (read + write bytes), i.e. COMBINED
traffic, so the matching roof is the COMBINED rate: 1.6 GB/s.

Plotting combined traffic against a 0.8 GB/s roof is a units error -- it puts
blocks 1 and 2 above the line even though neither direction exceeds 0.8 GB/s
(block 1 peaks at 662 MB/s read / 331 MB/s write). The 0.8 GB/s line is drawn
as a labelled reference so the distinction is explicit.

If you would rather plot per-direction, set AI = MACs / read_bytes and use
BW_ONEWAY as the roof, and produce a second figure for writes.
"""

import argparse

import matplotlib
import matplotlib.pyplot as plt
import numpy as np

# ----------------------------------------------------------------------------
# MEASURED INPUTS
# ----------------------------------------------------------------------------
PL_CLOCK_HZ = 100e6

# Compute ceilings (DERIVED from the architecture, see PAPER_HW_EVIDENCE §4):
#   PW MAC grid  N_OC x N_LANES/2 = 16 x 4 = 64 DSP, 2 packed MACs each = 128/cyc
#   DW MACs      8 lanes x 9 taps = 72 DSP, 1 MAC each                  =  72/cyc
PW_MACS_PER_CYCLE = 128
DW_MACS_PER_CYCLE = 72

# Memory ceilings: 64-bit HP port @100 MHz, independent read/write channels
BW_ONEWAY_GBs   = 64 / 8 * PL_CLOCK_HZ / 1e9      # 0.8 GB/s per direction
BW_COMBINED_GBs = 2 * BW_ONEWAY_GBs               # 1.6 GB/s read+write
BW_DDR_GBs      = 32 / 8 * 1066.67e6 / 1e9        # 4.27 GB/s, DDR3-1066 x32

# Per-block MEASURED data.
#   mac_dw / mac_pw : MACs per frame (DW = C*Hout*Wout*9, PW = Cin*Cout*px)
#   rd_B / wr_B     : MM2S and S2MM bytes per frame
#   hw_ms           : warm per-block HW time, mean of 100 frames
BLOCKS = [
    # name        mac_dw      mac_pw       rd_B      wr_B     hw_ms
    ("b0 3->16",   6_220_800, 11_059_200, 2_764_800, 3_686_400, 10.38),
    ("b1 16->32",  8_294_400, 29_491_200, 3_686_400, 1_843_200,  5.57),
    ("b2 32->32",  4_147_200, 14_745_600, 1_843_200,   460_800,  2.68),
    ("b3 32->32",  4_147_200, 14_745_600,   460_800,   460_800,  1.87),
    ("b4 32->64",  4_147_200, 29_491_200,   460_800,   921_600,  3.24),
    ("b5 64->64",  8_294_400, 58_982_400,   921_600,   921_600,  5.55),
]

HW_WINDOW_MS = 29.30     # sum of per-block HW time
FRAME_MS     = 36.261    # end-to-end, mean of 100 frames


# ----------------------------------------------------------------------------
def derive():
    peak_mac_s = (PW_MACS_PER_CYCLE + DW_MACS_PER_CYCLE) * PL_CLOCK_HZ / 1e9
    peak_pw    = PW_MACS_PER_CYCLE * PL_CLOCK_HZ / 1e9
    peak_dw    = DW_MACS_PER_CYCLE * PL_CLOCK_HZ / 1e9

    pts, tot_mac, tot_B = [], 0, 0
    for name, mdw, mpw, rd, wr, ms in BLOCKS:
        mac, byt = mdw + mpw, rd + wr
        pts.append({
            "name": name,
            "ai":   mac / byt,                       # MAC per byte
            "perf": mac / (ms * 1e-3) / 1e9,         # GMAC/s
            "rd_MBs": rd / (ms * 1e-3) / 1e6,
            "wr_MBs": wr / (ms * 1e-3) / 1e6,
        })
        tot_mac += mac
        tot_B   += byt

    system = {
        "ai":        tot_mac / tot_B,
        "perf_hw":   tot_mac / (HW_WINDOW_MS * 1e-3) / 1e9,
        "perf_frame": tot_mac / (FRAME_MS * 1e-3) / 1e9,
        "mac": tot_mac, "bytes": tot_B,
    }
    return peak_mac_s, peak_pw, peak_dw, pts, system


def plot(show=False):
    peak, peak_pw, peak_dw, pts, sysp = derive()
    ridge = peak / BW_COMBINED_GBs          # AI where the roofs meet

    fig, ax = plt.subplots(figsize=(8.2, 5.6))
    ai = np.logspace(-0.6, 2.3, 600)        # ~0.25 .. 200 MAC/byte

    # --- memory roofs (sloped) and compute roof (flat) ----------------------
    for bw, style, col, lab in (
        (BW_COMBINED_GBs, "-",  "#1f77b4", f"DMA combined {BW_COMBINED_GBs:.1f} GB/s"),
        (BW_ONEWAY_GBs,   "--", "#7fb3d5", f"one direction {BW_ONEWAY_GBs:.1f} GB/s (reference)"),
        (BW_DDR_GBs,      ":",  "#aec7e8", f"DDR3 peak {BW_DDR_GBs:.2f} GB/s"),
    ):
        ax.plot(ai, np.minimum(bw * ai, peak), style, color=col, lw=1.8, label=lab)

    ax.axhline(peak, color="#d62728", lw=2.0,
               label=f"compute roof {peak:.1f} GMAC/s (200 MAC/cyc @100 MHz)")
    for p, lab in ((peak_pw, f"PW only {peak_pw:.1f}"), (peak_dw, f"DW only {peak_dw:.1f}")):
        ax.axhline(p, color="#d62728", lw=0.9, ls=":", alpha=0.55)
        ax.text(ai[-1], p * 1.03, lab, ha="right", va="bottom",
                fontsize=7.5, color="#d62728")

    ax.axvline(ridge, color="grey", lw=0.8, ls="-.", alpha=0.7)
    ax.text(ridge * 1.06, peak * 0.30, f"ridge {ridge:.1f} MAC/B",
            rotation=90, fontsize=7.5, color="grey", va="center")

    # --- per-block points ---------------------------------------------------
    cmap = plt.get_cmap("viridis")
    for i, p in enumerate(pts):
        c = cmap(i / max(1, len(pts) - 1))
        ax.plot(p["ai"], p["perf"], "o", ms=8, color=c, mec="k", mew=0.6, zorder=5)
        ax.annotate(p["name"], (p["ai"], p["perf"]),
                    textcoords="offset points", xytext=(8, -3),
                    fontsize=8, color="k")

    # --- system points ------------------------------------------------------
    ax.plot(sysp["ai"], sysp["perf_hw"], "*", ms=19, color="#ff7f0e",
            mec="k", mew=0.7, zorder=6,
            label=f"SYSTEM, HW window  ({sysp['perf_hw']:.2f} GMAC/s)")
    ax.plot(sysp["ai"], sysp["perf_frame"], "P", ms=12, color="#8c564b",
            mec="k", mew=0.7, zorder=6,
            label=f"SYSTEM, full frame ({sysp['perf_frame']:.2f} GMAC/s)")

    ax.set_xscale("log"); ax.set_yscale("log")
    ax.set_xlabel("Arithmetic intensity  [MAC / byte of DRAM traffic]")
    ax.set_ylabel("Performance  [GMAC/s]")
    ax.set_title("Roofline — DW/PW accelerator, ImageEncoderLite @720p, ZC702 (xc7z020, 100 MHz)")
    ax.grid(True, which="both", alpha=0.25, lw=0.5)
    ax.set_ylim(0.5, peak * 2.2)
    ax.legend(loc="lower right", fontsize=7.6, framealpha=0.93)
    fig.tight_layout()

    for ext in ("png", "pdf"):
        fig.savefig(f"roofline.{ext}", dpi=300)
    print("wrote roofline.png / roofline.pdf")

    # --- console table, so the figure can be checked against numbers --------
    print(f"\ncompute roof {peak:.1f} GMAC/s  (PW {peak_pw:.1f} + DW {peak_dw:.1f})")
    print(f"memory roofs: combined {BW_COMBINED_GBs:.1f}, one-way {BW_ONEWAY_GBs:.1f}, "
          f"DDR {BW_DDR_GBs:.2f} GB/s")
    print(f"ridge point {ridge:.2f} MAC/byte\n")
    print(f"{'block':<11}{'AI':>8}{'GMAC/s':>9}{'% roof':>9}"
          f"{'rd MB/s':>10}{'wr MB/s':>10}{'bound':>10}")
    for p in pts:
        roof = min(BW_COMBINED_GBs * p["ai"], peak)
        print(f"{p['name']:<11}{p['ai']:>8.2f}{p['perf']:>9.2f}"
              f"{100 * p['perf'] / roof:>8.0f}%{p['rd_MBs']:>10.0f}{p['wr_MBs']:>10.0f}"
              f"{('memory' if p['ai'] < ridge else 'compute'):>10}")
    for lab, v in (("SYSTEM hw", sysp["perf_hw"]), ("SYSTEM frame", sysp["perf_frame"])):
        roof = min(BW_COMBINED_GBs * sysp["ai"], peak)
        print(f"{lab:<11}{sysp['ai']:>8.2f}{v:>9.2f}{100 * v / roof:>8.0f}%")

    if show:
        plt.show()


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--show", action="store_true")
    a = ap.parse_args()
    matplotlib.use("Agg") if not a.show else None
    plot(a.show)
