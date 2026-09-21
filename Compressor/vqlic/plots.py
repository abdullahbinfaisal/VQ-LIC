"""Loss curves rendered straight from a run's log.jsonl.

Written as a PNG into the run directory rather than served over a port, because
the thing being watched is usually a headless cloud box: `scp` one file, or open
it in the RunPod/Jupyter file browser, and it is current as of the last
`--plot-every` steps. `scripts/plot_log.py` re-renders the same figure from a
downloaded log at any time.

matplotlib is imported lazily and its absence is not fatal -- the training path
never depends on a plotting backend.
"""
from __future__ import annotations

import json
import os

import numpy as np

# Validated 5-colour categorical palette (blue, orange, green, amber, pink).
PALETTE = ["#2a78d6", "#eb6834", "#1baf7a", "#eda100", "#e87ba4"]
GRID = {"color": "#d9d9d9", "linewidth": 0.6}

_warned = False


def _pyplot():
    global _warned
    try:
        import matplotlib
        matplotlib.use("Agg")          # no display on a cloud box
        import matplotlib.pyplot as plt
        return plt
    except Exception as e:             # pragma: no cover - environment dependent
        if not _warned:
            print(f"note: curves disabled ({e.__class__.__name__}: {e}). "
                  f"`pip install matplotlib` to enable, or pass --plot-every 0.")
            _warned = True
        return None


def load_log(path):
    """Split a log.jsonl into (train_records, val_records), both step-sorted."""
    train, val = [], []
    if not os.path.exists(path):
        return train, val
    with open(path, "r", encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            try:
                rec = json.loads(line)
            except json.JSONDecodeError:
                continue               # a truncated tail line after a hard kill
            (val if "val" in rec else train).append(rec)
    train.sort(key=lambda r: r.get("step", 0))
    val.sort(key=lambda r: r.get("step", 0))
    return train, val


def _series(records, key, sub=None):
    """(steps, values) for one field, skipping records that lack it."""
    xs, ys = [], []
    for r in records:
        src = r.get(sub, {}) if sub else r
        v = src.get(key)
        if v is None:
            continue
        xs.append(r.get("step", 0))
        ys.append(float(v))
    return np.asarray(xs, float), np.asarray(ys, float)


def _smooth(y, k):
    """Trailing moving average, NaN-padded so it stays aligned with x."""
    if k <= 1 or len(y) < k:
        return y
    c = np.convolve(y, np.ones(k) / k, mode="valid")
    return np.concatenate([np.full(k - 1, np.nan), c])


def plot_log(log_path, out_path=None, title="", smooth=9, dpi=110):
    """Render the six diagnostic panels. Returns the path written, or None.

    Raw per-interval values are drawn faint with a moving average on top; the
    per-interval loss of a codec bounces by more than its trend over any 50-step
    window, so the raw line alone does not answer "is it going down".
    """
    plt = _pyplot()
    if plt is None:
        return None
    train, val = load_log(log_path)
    if not train and not val:
        return None
    out_path = out_path or os.path.splitext(log_path)[0] + "_curves.png"

    fig, axes = plt.subplots(2, 3, figsize=(15, 7.5))
    fig.suptitle(f"{title or os.path.basename(log_path)}"
                 f"    ({len(train)} log points, {len(val)} validations)",
                 fontsize=11, x=0.01, ha="left")

    def line(ax, xs, ys, label, color, raw=True):
        if len(xs) == 0:
            return
        if raw and smooth > 1 and len(ys) >= smooth:
            ax.plot(xs, ys, color=color, linewidth=0.8, alpha=0.22)
            ax.plot(xs, _smooth(ys, smooth), color=color, linewidth=1.6,
                    label=label)
        else:
            ax.plot(xs, ys, color=color, linewidth=1.6, marker="o" if len(xs) < 30
                    else None, markersize=3, label=label)

    def frame(ax, title_, ylabel, legend=True):
        ax.set_title(title_, fontsize=9.5, loc="left", color="#333333")
        ax.set_ylabel(ylabel, fontsize=8.5)
        ax.set_xlabel("step", fontsize=8.5)
        ax.grid(True, **GRID)
        ax.set_axisbelow(True)
        for s in ("top", "right"):
            ax.spines[s].set_visible(False)
        for s in ("left", "bottom"):
            ax.spines[s].set_color("#bbbbbb")
        ax.tick_params(labelsize=8, colors="#555555", length=3)
        if legend and ax.get_legend_handles_labels()[0]:
            ax.legend(fontsize=8, frameon=False)

    # 1 -- loss and its distortion component
    ax = axes[0][0]
    line(ax, *_series(train, "loss"), "total", PALETTE[0])
    line(ax, *_series(train, "dist"), "distortion", PALETTE[1])
    frame(ax, "loss (moving average over raw)", "loss")

    # 2 -- rate. Train bpp is this batch's own histogram entropy, so it reads low;
    # bpp_prior is the cross-entropy an actual coder pays. Both are plotted because
    # the GAP between them is the thing to watch.
    ax = axes[0][1]
    line(ax, *_series(train, "bpp"), "train (empirical)", PALETTE[0])
    line(ax, *_series(val, "bpp_empirical", sub="val"), "val (empirical)",
         PALETTE[2], raw=False)
    line(ax, *_series(val, "bpp_prior", sub="val"), "val (prior x-entropy)",
         PALETTE[1], raw=False)
    frame(ax, "rate", "bpp")

    # 3 -- quality. Both series are dB, so they share one axis honestly.
    ax = axes[0][2]
    line(ax, *_series(val, "psnr", sub="val"), "PSNR", PALETTE[0], raw=False)
    line(ax, *_series(val, "ms_ssim_db", sub="val"), "MS-SSIM", PALETTE[2],
         raw=False)
    frame(ax, "validation quality", "dB")

    # 4 -- codebook utilization, the collapse alarm
    ax = axes[1][0]
    xs, ys = _series(train, "util")
    line(ax, xs, 100.0 * ys, "codes used this epoch", PALETTE[2])
    ax.set_ylim(0, 105)
    frame(ax, "codebook utilization", "% of G x K")

    # 5 -- rate pressure vs rate_beta. rp approaching 1 means the ECVQ tilt is the
    # same size as the distortion it competes with, i.e. assignment has stopped
    # being about geometry.
    ax = axes[1][1]
    line(ax, *_series(train, "rate_beta"), "rate_beta", PALETTE[3])
    line(ax, *_series(train, "rate_pressure"), "rate pressure", PALETTE[1])
    frame(ax, "rate pressure (watch for -> 1)", "")

    # 6 -- learning rate
    ax = axes[1][2]
    line(ax, *_series(train, "lr"), "lr", PALETTE[0], raw=False)
    frame(ax, "learning rate", "lr")

    fig.tight_layout(rect=(0, 0, 1, 0.965))
    os.makedirs(os.path.dirname(os.path.abspath(out_path)), exist_ok=True)
    tmp = out_path + ".tmp.png"
    fig.savefig(tmp, dpi=dpi, facecolor="white")
    plt.close(fig)
    os.replace(tmp, out_path)          # never leave a half-written PNG
    return out_path
