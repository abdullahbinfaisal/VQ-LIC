"""Corpora, weight paths and the metric helpers every script here shares.

Both halves of an RD plot -- our rungs and the classical anchors -- take their
PSNR and MS-SSIM from `vqlic.metrics`, the same functions the training loop uses.
A benchmark whose anchors and model are scored by different implementations is
not a benchmark, so there is one copy of `score()` and everything calls it.

Paths are environment variables rather than constants, because the only thing
less portable than a hardcoded path is a hardcoded path on someone else's
machine:

    VQLIC_WEIGHTS   checkpoints and context tables  (default: ../weights)
    VQLIC_KODAK     Kodak 24                        (default: ../data/kodak)
    VQLIC_CLIC      CLIC2017 validation             (default: ../data/clic)
    VQLIC_BPGENC    bpgenc binary, for the anchors  (default: bpgenc on PATH)
    VQLIC_BPGDEC    bpgdec binary                   (default: bpgdec on PATH)

See the README for where to get the weights and the corpora.
"""
from __future__ import annotations

import argparse
import os
import sys

import numpy as np
from PIL import Image

_HERE = os.path.dirname(os.path.abspath(__file__))
COMPRESSOR = os.path.dirname(_HERE)
if COMPRESSOR not in sys.path:
    sys.path.insert(0, COMPRESSOR)

from vqlic.metrics import ms_ssim_db, psnr_uint8  # noqa: E402  (needs sys.path)


def _env_dir(var, *default_parts):
    return os.environ.get(var) or os.path.join(COMPRESSOR, *default_parts)


WEIGHTS_DIR = _env_dir("VQLIC_WEIGHTS", "weights")
BPGENC = os.environ.get("VQLIC_BPGENC", "bpgenc")
BPGDEC = os.environ.get("VQLIC_BPGDEC", "bpgdec")

IMAGE_EXTS = (".png", ".jpg", ".jpeg", ".bmp", ".tif", ".tiff")

# ------------------------------------------------------------------ datasets
#
# Two evaluation corpora, selected with `--dataset`. They are NOT interchangeable
# and a figure must never mix them:
#
# * **kodak** -- the standard 24-image Kodak benchmark, every image 768x512 or
#   512x768. Essentially every learned-compression paper reports it, so it is the
#   set that makes these numbers comparable to published ones.
# * **clic** -- 59 CLIC2017 validation images, 751-2048 px. The wider,
#   higher-resolution corpus the ladder was tuned against.
#
# The corpus travels in the output filename through `--tag`, so a Kodak sweep and
# a CLIC sweep coexist without one silently standing in for the other.
DATASETS = {
    "kodak": (_env_dir("VQLIC_KODAK", "data", "kodak"), "Kodak",
              "{n} images at native 768x512 / 512x768"),
    "clic": (_env_dir("VQLIC_CLIC", "data", "clic"), "CLIC",
             "{n} images at native resolution (751-2048 px)"),
}
DEFAULT_DATASET = "kodak"


def dataset_images(name=DEFAULT_DATASET):
    """Sorted absolute paths to the full-resolution images of one corpus.

    Sorted by filename, so `--limit N` means the same N images every run and the
    per-image rows of one invocation line up with the next.
    """
    if name not in DATASETS:
        raise SystemExit(f"unknown dataset {name!r}; choose from {list(DATASETS)}")
    d = DATASETS[name][0]
    if not os.path.isdir(d):
        raise SystemExit(
            f"dataset {name!r} directory does not exist: {d}\n"
            f"Set the corresponding VQLIC_* environment variable, or see the "
            f"README for how to fetch it.")
    names = sorted(f for f in os.listdir(d) if f.lower().endswith(IMAGE_EXTS))
    if not names:
        raise SystemExit(f"no images in {d}")
    return [os.path.join(d, n) for n in names]


def dataset_label(name=DEFAULT_DATASET):
    """The corpus name as a figure title should print it, e.g. "Kodak"."""
    return DATASETS.get(name, (None, name.upper(), ""))[1]


def dataset_subtitle(name, n):
    """The resolution note under a figure title, with the image count filled in."""
    tpl = DATASETS.get(name, (None, None, "{n} images"))[2]
    return tpl.format(n=n)


def add_dataset_arg(ap):
    """The `--dataset` flag, defined once so every script spells it the same."""
    ap.add_argument("--dataset", default=DEFAULT_DATASET, choices=list(DATASETS),
                    help="evaluation corpus. Use a distinct --tag per dataset: "
                         "the tag is what keeps a Kodak sweep and a CLIC sweep "
                         "from overwriting each other.")
    return ap


def add_out_dir_arg(ap, default="results"):
    """Where an invocation writes its JSON."""
    ap.add_argument("--out-dir", default=os.path.join(COMPRESSOR, default),
                    help="directory for output JSON (created if missing)")
    return ap


def out_dir(path):
    os.makedirs(path, exist_ok=True)
    print(f"[out dir] {path}")
    return path


# ------------------------------------------------------------------- weights
#
# The reported ladder: each rung's FP32 parent, the INT8 QAT encoder trained from
# it, and the context table fitted on its code distribution. One list, because
# this used to live in three scripts and that is three chances for a figure to be
# drawn from a checkpoint the neighbouring figure did not use.
#
# A float model and its INT8 finetune emit different code distributions, so the
# `ctx_` and `ctx_fp32_` tables are not interchangeable -- one table cannot price
# both models.
RUNGS = ["bpp010", "bpp030", "bpp040"]


def weight(name):
    """A file in the weights directory, checked to exist with a useful error."""
    path = os.path.join(WEIGHTS_DIR, name)
    if not os.path.exists(path):
        raise SystemExit(
            f"missing weight file: {path}\n"
            f"Download the checkpoints and context tables first (see the "
            f"README), or point VQLIC_WEIGHTS at where they already live.")
    return path


def rung_paths(rung, float_tables=False):
    """`(fp32_parent, qat_encoder, context_table)` for one rung."""
    prefix = "ctx_fp32_" if float_tables else "ctx_"
    return (weight(f"{rung}_fp32.pt"),
            weight(f"{rung}_cbint8_qat_final.pt"),
            weight(f"{prefix}{rung}_keep64_left_top16.npz"))


def train_images(limit):
    """The first `limit` OpenImages train images, for fitting context tables.

    The same corpus every table in this repo was fitted on, and held out from
    Kodak and CLIC by construction. Sorted, so `--limit N` is reproducible.
    """
    d = os.environ.get("VQLIC_TRAIN") or os.path.join(COMPRESSOR, "data", "train")
    if not os.path.isdir(d):
        raise SystemExit(
            f"training corpus not found: {d}\n"
            f"Set VQLIC_TRAIN to an OpenImages train directory. Only needed for "
            f"fitting context tables -- the fitted ones ship with the weights.")
    names = sorted(f for f in os.listdir(d)
                   if f.lower().endswith((".jpg", ".jpeg", ".png")))
    if not names:
        raise SystemExit(f"no images in {d}")
    return [os.path.join(d, n) for n in names[:limit]]


def assemble(rung, float_model=False, keep_n=64, device="cuda", quiet=False,
             attach_context=True):
    """One rung name -> a codec configured exactly as the reported results are.

    Four steps, in this order:

    1. load the checkpoint, restoring `rate_beta` (`vqlic.load`);
    2. place it -- an INT8 encoder stays on the CPU, the decoder does not;
    3. prune each codebook to its top `keep_n` codes by prior mass, which is
       what makes the dropped codes unreachable rather than merely unlikely;
    4. attach the context model, so `compress` codes each index against its
       left neighbour instead of against the marginal prior.

    Step 3 before step 4 is not arbitrary: the context tables were fitted on the
    code distribution of the *pruned* model, so attaching them to an unpruned
    one prices a distribution that model does not emit.

    `float_model=True` selects the FP32 pre-QAT parent and its own `ctx_fp32_`
    tables -- the two curves the RD figures show.

    `attach_context=False` stops after step 3, for the one caller that cannot do
    step 4: the script that *fits* the tables, which needs the pruned model to
    count code occurrences before any table exists.

    Returns `(model, device, meta)`.
    """
    from vqlic.context import ContextModel
    from vqlic.keepn import install_keep_mask, keep_topn
    from vqlic.load import load_fp32, load_qat_int8, place

    fp32, qat, ctx_path = (rung_paths(rung, float_tables=float_model)
                           if attach_context else
                           (weight(f"{rung}_fp32.pt"),
                            weight(f"{rung}_cbint8_qat_final.pt"), None))

    if float_model:
        model, cfg, _ck = load_fp32(fp32)
        info = {"quantized": False, "rate_beta": cfg.rate_beta_final}
    else:
        model, cfg, info = load_qat_int8(fp32, qat)

    model, device = place(model, device, info.get("quantized", False))

    q = model.quantizer
    prior_w = q.cluster_size.detach().cpu().numpy().copy()
    install_keep_mask(q, keep_topn(prior_w, keep_n))

    ctx_model = None
    if attach_context:
        ctx_model = ContextModel.load(ctx_path)
        model.attach_context(ctx_model)

    meta = {
        "rung": rung,
        "model": "fp32_pre_qat" if float_model else "int8_qat_cbint8",
        "checkpoint": os.path.basename(fp32 if float_model else qat),
        "context_table": os.path.basename(ctx_path) if ctx_path else None,
        "keep_n": keep_n,
        "codebooks": int(q.G),
        "codebook_size": int(q.K),
        "rate_beta": info.get("rate_beta"),
        "device": str(device),
        "qat": {k: v for k, v in info.items() if k != "rate_beta"},
    }
    if not quiet:
        print(f"{rung}: {meta['model']} | {meta['checkpoint']}")
        print(f"  keepN={keep_n}/{q.K} x G={q.G} | rate_beta={meta['rate_beta']}")
        if ctx_model is not None:
            print(f"  context: {meta['context_table']} | {ctx_model.summary()}")
        print(f"  device: {device}")
    return model, device, meta


# ------------------------------------------------------------------- metrics
def center_crop(img, size):
    """Centre crop a PIL image to size x size, matching `vqlic.dataset`.

    torchvision's CenterCrop rounds the offset DOWN ((H - size) // 2), so this
    reproduces it exactly -- a 224 evaluation has to see the same pixels the
    training-time validation loop saw. Images smaller than `size` on a side are
    reflect-padded first, as `CenterCrop` pads (with zeros) rather than failing.
    """
    if not size:
        return img
    w, h = img.size
    if w < size or h < size:
        a = np.asarray(img.convert("RGB"))
        ph, pw = max(0, size - h), max(0, size - w)
        a = np.pad(a, ((0, ph), (0, pw), (0, 0)), mode="reflect")
        img = Image.fromarray(a)
        w, h = img.size
    left, top = (w - size) // 2, (h - size) // 2
    return img.crop((left, top, left + size, top + size))


def load_rgb(path, crop=0):
    """Image as a uint8 HWC array, optionally centre-cropped to `crop` px."""
    img = Image.open(path).convert("RGB")
    if crop:
        img = center_crop(img, crop)
    return np.asarray(img, dtype=np.uint8)


def norm_from_u8(arr):
    """uint8 HWC -> `[1,3,H,W]` in [-1,1], identical to the loader's transform.

    Same ops in the same order as `normalize(to_tensor(img), 0.5, 0.5)`: a
    division by 255, then a subtract and a divide. Written out rather than fused
    into `x/127.5 - 1` so the float32 rounding matches the training path --
    assignment near a Voronoi boundary is decided on the last bits of the latent,
    so this is not pedantry.
    """
    import torch
    t = torch.from_numpy(np.ascontiguousarray(arr))
    t = t.permute(2, 0, 1)[None].float().div_(255.0)
    return t.sub_(0.5).div_(0.5)


def score(orig_u8, recon_u8, device=None):
    """(PSNR dB, MS-SSIM dB, raw MS-SSIM) for one reconstruction."""
    if orig_u8.shape != recon_u8.shape:
        raise ValueError(f"shape mismatch {orig_u8.shape} vs {recon_u8.shape}")
    p = psnr_uint8(orig_u8, recon_u8)
    d, raw = ms_ssim_db(orig_u8, recon_u8, device=device)
    return p, d, raw


def bpp_from_bytes(n_bytes, height, width):
    """File size -> bits per pixel. This is the real rate the anchors pay."""
    return 8.0 * n_bytes / float(height * width)


def resolve_table(mode, crop):
    """Whether to transmit a per-image frequency table, from --table and --crop.

    One function because several scripts ask the question, and a figure drawn
    from a run that answered it differently than the run beside it is a silent
    inconsistency rather than a visible one.
    """
    return mode == "on" or (mode == "auto" and not crop)


def interp_curve(pts, key, x):
    """An anchor's corpus RD curve at rate `x`, linearly interpolated.

    Returns None outside the measured range rather than extrapolating: an anchor
    whose sweep never reached that rate has no comparable number there, and
    inventing one by extending the last segment is how a plausible-looking figure
    ends up resting on a value nobody measured.
    """
    pts = sorted([p for p in pts if p.get(key) is not None],
                 key=lambda p: p["bpp"])
    for a, b in zip(pts, pts[1:]):
        if a["bpp"] <= x <= b["bpp"]:
            t = (x - a["bpp"]) / (b["bpp"] - a["bpp"])
            return a[key] + t * (b[key] - a[key])
    return None


class Formatter(argparse.ArgumentDefaultsHelpFormatter,
                argparse.RawDescriptionHelpFormatter):
    """Keeps the module docstring's layout in --help and still shows defaults."""
