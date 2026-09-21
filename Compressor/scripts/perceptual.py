"""Perceptual metrics for the rate-distortion bench: LPIPS, BRISQUE, NIQE, PI, TReS.

Metrics that disagree with PSNR on purpose. PSNR and MS-SSIM reward getting the
pixels right; these reward looking right, which for a codec at 0.1 bpp is not the
same thing. `METRICS` is the registry -- direction, whether the original is
needed, and measured cost -- and every consumer reads those facts from it.

* **LPIPS** (full reference, lower better) -- distance between VGG16 features of
  the original and the reconstruction. `piq`'s implementation with the standard
  Zhang et al. linear weights. The near-universal perceptual metric in the
  neural-compression literature, which is an argument for reporting it whatever
  else is reported.
* **BRISQUE** (no reference, lower better) -- an SVR over MSCN statistics, scored
  on the reconstruction ALONE. Roughly 0-100.
* **NIQE** (no reference, lower better) -- multivariate Gaussian fit to NSS
  features. Half of PI, and carried separately because when PI and NIQE disagree
  it is the Ma half doing it.
* **PI** (no reference, lower better) -- the PIRM-2018 perceptual index,
  `0.5 * ((10 - Ma) + NIQE)`. Not in `DEFAULT_METRICS`, on cost alone: Ma/NRQM is
  ~0.8 s per full-resolution image even after the float32 fix below, and
  profiling shows no single hotspot left to remove -- it is a MATLAB
  transliteration whose cost is spread over a steerable pyramid, three SVDs,
  twelve eigendecompositions, a NumPy interp and 27 SSIM calls. That is about an
  hour added to a full anchor sweep, so ask for it by name.
* **TReS** (no reference, **HIGHER better**) -- a ResNet-50 plus transformer head
  trained on KonIQ-10k with a relative-ranking and self-consistency objective,
  predicting a MOS on the KonIQ scale. The KonIQ weights are pinned, not
  pyiqa's default, because the FLIVE-trained variant is a different scale and the
  two are not interchangeable. At 0.08 s per full-resolution image it is cheap
  enough to leave on.

**On choosing which to report.** Picking the subset that flatters the codec is
metric cherry-picking, and it is a worse problem than choosing a favourable
example image because it moves the headline claim rather than an illustration.
Measure all of them, state the criterion the reported pair was chosen by, and
keep the rest available. Two criteria that survive review, neither of which is
"whichever we win": whether the metric is VALID here at all -- a no-reference
score that rates a compressed image better than the uncompressed original is out
of its training distribution and cannot carry a headline -- and whether it is
MONOTONE in rate on the anchors, since a metric that wanders as bitrate rises is
measuring something other than quality.

Two things to keep in mind when reading the no-reference numbers. First, they judge
an image on its own, so a codec that hallucinates plausible texture scores BETTER
than one that blurs, even where the texture is wrong -- that is the point of them,
but it means BRISQUE and PI can improve while fidelity gets worse. Second, they are
calibrated on natural photographs with traditional distortions; neural-codec output
at very low rate is outside that training distribution, so treat the absolute values
as a ranking aid, not a measurement. The pristine originals are scored too, and
`reference_scores` in the results is the floor those two curves are heading for.

pyiqa is imported one arch module at a time rather than through
`pyiqa.create_metric`. The factory path drags in `pyiqa.data`, which imports
`datasets`, which imports TensorFlow -- and this environment's TF and torch abort
the process the moment both are loaded. The arch classes are the same code without
the registry.
"""
from __future__ import annotations

import contextlib
import functools

import numpy as np
import torch

# One registry per metric, because three things have to agree about every metric
# and used to be spelled out in three places: which way is better (plot_rd points
# the axis by it), whether it needs the original (only a no-reference metric can
# be scored on the pristine images to give the curves a floor), and what it costs
# (which is what makes a metric opt-in or not).
#
#   lower       True if smaller is better
#   reference   True if it needs the ORIGINAL as well as the reconstruction
#   label       how a figure names it
#   cost        seconds per full-resolution image on this box, measured
#
# `tres` is the odd one out on direction: TReS predicts a MOS on the KonIQ-10k
# scale, so HIGHER is better, where every other perceptual metric here is a
# distance or a defect score. That is exactly the sort of detail that silently
# inverts a figure, which is why direction lives in the registry rather than in
# whichever plotting script happens to remember it.
METRICS = {
    "lpips":   {"lower": True,  "reference": True,
                "label": "LPIPS (VGG)", "cost": 0.02},
    "brisque": {"lower": True,  "reference": False,
                "label": "BRISQUE", "cost": 0.01},
    "niqe":    {"lower": True,  "reference": False,
                "label": "NIQE", "cost": 0.05},
    "pi":      {"lower": True,  "reference": False,
                "label": "Perceptual Index", "cost": 0.80},
    "tres":    {"lower": False, "reference": False,
                "label": "TReS (KonIQ MOS)", "cost": 0.08},
}

# Fidelity metrics live in lib.metrics, but plot_rd needs their direction from
# the same table so a caller can ask about any metric name uniformly.
LOWER_BETTER = {k: v["lower"] for k, v in METRICS.items()}
LOWER_BETTER.update({"psnr": False, "ms_ssim_db": False})

METRIC_LABELS = {k: v["label"] for k, v in METRICS.items()}
METRIC_LABELS.update({"psnr": "PSNR (dB)", "ms_ssim_db": "MS-SSIM (dB)"})

# What `score()` returns unless asked otherwise, and the pair the paper reports.
#
# `niqe`, `pi` and `tres` remain available by name and are worth keeping recorded
# for an appendix, but none of them belongs in the default:
#
#   tres  FAILS the validity audit in this codec's operating range. Over a JPEG
#         ladder on Kodak it is ANTI-correlated with rate below 0.35 bpp
#         (Spearman -0.88, 6 reversals): it scores a 0.09 bpp JPEG at 77.6 and a
#         0.21 bpp JPEG at 62.0, preferring the image with less than half the
#         bits. It also moves the OPPOSITE way on our own rungs, which is the
#         tell -- it ranks artifact character rather than quality, and blocking
#         reads to it as sharpness.
#   pi    Passes the audit, so it is a legitimate alternative to BRISQUE, but it
#         costs 80x as much (Ma/NRQM at ~0.8 s per full-resolution image, about
#         20 min on a Kodak anchor sweep) and half of it is NIQE by construction.
#   niqe  Passes, and is the other half of PI. Same reasoning.
DEFAULT_METRICS = ("lpips", "brisque")
ALL_METRICS = tuple(METRICS)


def _to_tensor(u8, device):
    """uint8 HWC -> float NCHW in [0, 1] on `device`."""
    t = torch.from_numpy(np.ascontiguousarray(u8)).permute(2, 0, 1)[None]
    return t.to(device=device, dtype=torch.float32).div_(255.0)


# --------------------------------------------------------------------------
# Making Ma/NRQM affordable.
#
# Stock pyiqa runs NRQM in float64 (`nrqm_arch.nrqm` opens with `img.double()`).
# On a GeForce card that is the whole cost: a 4090 does FP64 at 1/64 of its FP32
# rate, so a 2048px CLIC image takes ~5.0 s of which almost all is the precision,
# not the arithmetic. Two changes bring it to ~0.8 s:
#
#   1. keep the pipeline in float32
#   2. memoise the 11x11 SSIM window, which `global_gsm` otherwise rebuilds on the
#      host and re-uploads 27 times per image
#
# Both were measured against the stock path on four full-resolution CLIC images:
# the window cache is bit-identical, and float32 moves NRQM by at most 7e-5, i.e.
# 4e-5 of PI -- three orders of magnitude below the second decimal anyone reads.
# Set `PerceptualScorer(fast_nrqm=False)` to run the stock float64 path.
#
# `torch.Tensor.double` is swapped for the duration of the NRQM call only. It is a
# global for that window, so scoring must stay on one thread -- `eval_perceptual`
# threads the codec work and scores on the main thread, which satisfies that.
# --------------------------------------------------------------------------
_FSPECIAL_PATCHED = False


class _CachedWindow:
    """Stands in for the tensor `fspecial` returns; `.to(x)` yields the cached one."""
    __slots__ = ("t",)

    def __init__(self, t):
        self.t = t

    def to(self, *_a, **_k):
        return self.t


def _patch_fspecial():
    global _FSPECIAL_PATCHED
    if _FSPECIAL_PATCHED:
        return
    import pyiqa.archs.ssim_arch as ssim_arch

    original = ssim_arch.fspecial

    @functools.lru_cache(maxsize=64)
    def build(size, sigma, channels, device):
        return original(size, sigma, channels).to(device)

    def fspecial(size=3, sigma=None, channels=1):
        # pyiqa always follows this with `.to(X)`, so the device is decided there;
        # cache per current device and let the stand-in hand the tensor over.
        dev = (torch.device("cuda", torch.cuda.current_device())
               if torch.cuda.is_available() else torch.device("cpu"))
        return _CachedWindow(build(size, sigma, channels, dev))

    ssim_arch.fspecial = fspecial
    _FSPECIAL_PATCHED = True


@contextlib.contextmanager
def _float32_double():
    """Make `Tensor.double()` a no-op, so pyiqa's NRQM stays in float32."""
    original = torch.Tensor.double
    torch.Tensor.double = lambda self: self.float()
    try:
        yield
    finally:
        torch.Tensor.double = original


class PerceptualScorer:
    """Lazily-built LPIPS + BRISQUE + NIQE/PI, all on one device.

    Built once and reused: LPIPS pulls a VGG16 (~500 MB) and NRQM a bundle of SVR
    models, and rebuilding either per image would dominate the run.

    NIQE is returned alongside PI because PI is half NIQE -- when the two curves
    disagree it is the Ma half doing it, and that is worth being able to see.
    """

    def __init__(self, device="cuda", fast_nrqm=True, metrics=None):
        self.device = torch.device(
            device if (device != "cuda" or torch.cuda.is_available()) else "cpu")
        self.fast_nrqm = fast_nrqm
        keys = tuple(metrics) if metrics else DEFAULT_METRICS
        bad = [k for k in keys if k not in METRICS]
        if bad:
            raise ValueError(f"unknown metric(s) {bad}; choose from "
                             f"{list(METRICS)}")
        # Registry order, not the caller's: a results row and the table printed
        # from it should not depend on the order the flags were typed in.
        self.keys = tuple(k for k in METRICS if k in keys)
        self._lpips = None
        self._niqe = None
        self._pi = None
        self._tres = None
        if fast_nrqm:
            _patch_fspecial()

    def __contains__(self, key):
        return key in self.keys

    @property
    def no_reference(self):
        """The selected metrics that can also be scored on a pristine image."""
        return tuple(k for k in self.keys if not METRICS[k]["reference"])

    def _run_pi(self, x):
        """PI, through the float32 path unless the caller asked for stock pyiqa."""
        if not self.fast_nrqm:
            return float(self.pi(x))
        with _float32_double():
            return float(self.pi(x))

    # -- lazy model construction ---------------------------------------------
    @property
    def lpips(self):
        if self._lpips is None:
            import piq
            self._lpips = piq.LPIPS(reduction="none").to(self.device).eval()
        return self._lpips

    @property
    def niqe(self):
        if self._niqe is None:
            from pyiqa.archs.niqe_arch import NIQE
            self._niqe = NIQE().to(self.device).eval()
        return self._niqe

    @property
    def pi(self):
        if self._pi is None:
            from pyiqa.archs.nrqm_arch import PI
            self._pi = PI().to(self.device).eval()
        return self._pi

    @property
    def tres(self):
        """TReS on the KonIQ-10k weights, which is the variant papers quote.

        pyiqa also ships a FLIVE-trained set; they are different scales and are
        not interchangeable, so the choice is pinned here rather than left to
        pyiqa's default moving under us. Note the direction: this one is a
        quality score, so HIGHER is better.
        """
        if self._tres is None:
            from pyiqa.archs.tres_arch import TReS
            self._tres = TReS(train_dataset="koniq").to(self.device).eval()
        return self._tres

    def warmup(self, size=256):
        """Build the selected models up front, so the first image is not an outlier.

        Only the selected ones: TReS pulls a ResNet-50 plus a transformer head
        and NRQM a bundle of SVRs, and building a model nothing will call is
        pure latency on a run that did not ask for it.
        """
        x = torch.rand(1, 3, size, size, device=self.device)
        with torch.no_grad():
            if "lpips" in self.keys:
                self.lpips(x, x)
            if "brisque" in self.keys:
                _ = __import__("piq").brisque(x, data_range=1.0)
            if "niqe" in self.keys:
                self.niqe(x)
            if "pi" in self.keys:
                self._run_pi(x)
            if "tres" in self.keys:
                self.tres(x)
        return self

    # -- scoring --------------------------------------------------------------
    def _jobs(self, rec, ref, keys):
        """{key: thunk} for `keys`, one entry per selected metric.

        `ref` is None when scoring a pristine image, where only the
        no-reference metrics apply.
        """
        import piq
        all_jobs = {
            "lpips": lambda: float(self.lpips(rec, ref).mean()),
            "brisque": lambda: float(piq.brisque(rec, data_range=1.0,
                                                 reduction="none").mean()),
            "niqe": lambda: float(self.niqe(rec)),
            "pi": lambda: self._run_pi(rec),
            "tres": lambda: float(self.tres(rec)),
        }
        return {k: all_jobs[k] for k in keys}

    def _run(self, jobs):
        """Run each thunk under its own guard.

        Guarded independently and per metric: BRISQUE's SVR returns a NaN on a
        near-flat crop, and one bad number should cost that one cell rather than
        the whole image's row. A None here is what `mean_or_none` skips and what
        plot_rd draws a gap through.
        """
        out = {}
        for key, fn in jobs.items():
            try:
                v = fn()
                out[key] = v if np.isfinite(v) else None
            except Exception:                                    # noqa: BLE001
                out[key] = None
        return out

    @torch.no_grad()
    def score(self, orig_u8, recon_u8):
        """(original, reconstruction) as uint8 HWC -> one value per selected metric."""
        rec = _to_tensor(recon_u8, self.device)
        ref = _to_tensor(orig_u8, self.device)
        return self._run(self._jobs(rec, ref, self.keys))

    @torch.no_grad()
    def score_reference(self, orig_u8):
        """The selected NO-REFERENCE metrics on the pristine original.

        This is the floor those curves are heading for, and for TReS the ceiling
        -- without it there is no way to read "is BRISQUE 43 good?" off a panel.

        Full-reference metrics are omitted rather than reported at their
        degenerate value: LPIPS of an image against itself is 0.0 by
        construction, and a hard zero on the plot reads as a measurement when it
        is a tautology.
        """
        ref = _to_tensor(orig_u8, self.device)
        return self._run(self._jobs(ref, None, self.no_reference))


def mean_or_none(rows, key):
    """Mean over the rows that actually produced a number for `key`."""
    vals = [r[key] for r in rows if r.get(key) is not None]
    return sum(vals) / len(vals) if vals else None
