"""Learned-codec anchors: Balle hyperprior, Minnen MBT2018, MCUCoder.

The classical anchors (`eval_classical.py`) are the wrong comparison on their own
-- every one of them predates learned compression. These three are the learned
baselines the paper is actually argued against, and like the classical anchors
they do not depend on which of our checkpoints is being evaluated, so they belong
in `results/anchors/` and get measured ONCE.

    balle      bmshj2018_hyperprior   Balle et al., ICLR 2018 (scale hyperprior)
    mbt        mbt2018                Minnen et al., NeurIPS 2018 (joint AR)
    mcucoder   MCUCoder               Hojjat et al. -- the MCU-class competitor

MEASURED THE SAME WAY AS EVERYTHING ELSE
----------------------------------------
Rate is the **real bitstream**: `net.compress()` bytes for the CompressAI models,
Huffman-coded channel bytes for MCUCoder. Not `-log2 p` of the latents, which runs
~0.5% optimistic and is not what an anchor is for -- `--rate estimated` exists to
cross-check, and records itself in the output so a curve cannot be mistaken for
the measured one. PSNR and MS-SSIM come from `common.score`, i.e. the same
`lib.metrics` functions the training loop and every other bench script use, on
uint8 reconstructions. LPIPS and BRISQUE come from `scripts/perceptual.py` and are
scored inline on the decoded reconstruction, the same way `exp_keepn.py` scores
ours -- so the perceptual figures can put these anchors and our rungs on one
axis. `--perceptual ""` turns them off.

Every bpp divides by the ORIGINAL pixel count, never the padded one. The
CompressAI nets need a multiple of 64 and MCUCoder a multiple of 8; dividing by
the padded count would understate the rate and slide the whole curve left.

THE 0.5 bpp WINDOW
------------------
The figures plot 0-0.5 bpp, so a quality level whose corpus-mean rate lands at or
above `--max-bpp` is recorded in `dropped` and not in the curve. Quality indices
are swept in ascending order and `--stop-past-max` (default on) ends that model's
sweep at the first point past the window, so no time is spent measuring -- or
downloading -- points that cannot be plotted. Rate rises monotonically with
quality index for both CompressAI families and with channel count for MCUCoder,
so this cannot skip a point that would have been inside the window.

COMPRESSAI'S DECODER IS UNSTABLE HERE, AND THAT IS WHY THIS SCRIPT IS PARANOID
------------------------------------------------------------------------------
In this environment `net.decompress()` fails at random on roughly one image in
eight. The failure is not an exception: the bitstream is byte-identical across
repeats, `forward` is always right, and the returned `x_hat` simply contains
NaNs, which `clamp(0, 1)` does not remove and `astype(uint8)` turns into
undefined bytes. Measured on Kodak at bmshj2018 q2, three of 24 images broke on
one pass and a different one on the next -- enough to move a corpus mean by more
than a dB, silently, in the direction that flatters us. Occasionally the ANS
extension takes the whole process down instead, with no Python traceback.

Three defences, all cheap:

* **every real decode is verified** against `forward`, which is the synthesis
  transform over the quantized latent and therefore exactly what a correct decode
  must reproduce. Agreement is bimodal and not a judgement call -- a good decode
  lands at 102 dB to infinity, a broken one at 10-13 dB -- so a decode below
  `--verify-db` is re-coded up to `--verify-retries` times.
* **finiteness is checked before the uint8 cast**, on the reference as well as
  the decode, so a NaN reconstruction can never agree with another NaN
  reconstruction and pass. An image that never verifies is recorded in `errors`
  and excluded from the mean rather than dragging it down.
* **results are written after every curve point**, atomically, and `--resume`
  (default) skips points already in the output file. A hard crash five hours into
  the MBT sweep then costs one point, not the run.

REQUIREMENTS
------------
Needs `compressai` (the Balle and Minnen anchors and their pretrained
checkpoints) and `dahuffman` (MCUCoder's per-channel Huffman tables). Neither is
required by the codec itself, so neither is in requirements.txt:

    pip install compressai dahuffman
    python scripts/eval_learned.py --dataset kodak

MCUCODER'S CODEC IS CALIBRATED, NOT LEARNED
-------------------------------------------
MCUCoder ships an encoder whose 12 latent channels are sent progressively; the
transmitted alphabet is a per-channel min/max scaling to uint8 followed by a
6-bit step (`x // 4`), and the entropy coder is a per-channel Huffman table. Both
the scaling range and the tables are fitted on training images -- side
information held by both peers, like our context tables -- so `--mcu-calib-dir`
images are fitted first and then reused for every rate point. Rate points are
`k = 1..12` transmitted channels, with the rest replaced by `rate_less`'s zeros.
Its symbols are exact by construction (the decoder dequantizes what the encoder
quantized, and the Huffman code is prefix-free and lossless), so it needs no
decode verification -- and it has never produced one.

Two deliberate deviations from the reference notebook, both to avoid measuring
something other than the codec:

* **no eval-time noise.** `MCUCoder.forward` adds U(-0.01, 0.01) to the latent
  when `not self.training`, which makes every measurement stochastic. This script
  drives the encoder, the quantizer and the decoder directly, so the only
  perturbation is the quantization the codec actually performs.
* **the Huffman table covers all 64 symbols** (`range(64)`), not the reference's
  `range(63)`. A symbol absent from the table is unencodable, and symbol 63
  occurs on full-resolution images even when it never appeared in calibration.
"""
from __future__ import annotations

import argparse
import json
import os
import sys
import time

import numpy as np
import torch
import torch.nn.functional as F

_HERE = os.path.dirname(os.path.abspath(__file__))
for _p in (os.path.dirname(_HERE), _HERE):
    if _p not in sys.path:
        sys.path.insert(0, _p)

from _common import (  # noqa: E402
    add_dataset_arg, add_out_dir_arg, bpp_from_bytes, dataset_images,
    dataset_label, load_rgb, out_dir, score,
)
from vqlic.metrics import psnr_uint8 as psnr_u8  # noqa: E402

CURVE_NAME = {
    "balle": "Balle hyperprior",
    "mbt": "Minnen MBT2018",
    "mcucoder": "MCUCoder",
}
LABEL_KEY = {"balle": "q", "mbt": "q", "mcucoder": "channels"}

# MCUCoder is a third-party baseline with its own licence, so it is not vendored
# here: clone it and point VQLIC_MCU_ROOT at the `mcu/` directory inside.
#     git clone https://github.com/ds-kiel/MCUCoder
MCU_ROOT = os.environ.get("VQLIC_MCU_ROOT", "")
MCU_WEIGHTS = (os.path.join(MCU_ROOT, "weights", "MCUCoder1M300k196MSSSIM.pth")
               if MCU_ROOT else "")
MCU_CHANNELS = 12
MCU_STEP = 4                      # 255 // 4 -> 64 symbols
MCU_SYMBOLS = 256 // MCU_STEP


# ------------------------------------------------------------------- tensors
def to_tensor(u8):
    return torch.from_numpy(u8.copy()).permute(2, 0, 1)[None].float().div_(255.0)


def to_u8(x):
    return (x.clamp(0, 1)[0].permute(1, 2, 0).cpu().numpy() * 255.0
            ).round().astype(np.uint8)


def recon_u8(x, h, w):
    """Crop to the original size and cast, reporting whether it was finite.

    The finiteness check is the load-bearing half: a NaN reconstruction survives
    `clamp(0, 1)` and casts to undefined bytes, so without this a NaN decode
    could be compared against a NaN reference, agree perfectly, and be recorded
    as a verified measurement.
    """
    x = x[..., :h, :w]
    ok = bool(torch.isfinite(x).all())
    return to_u8(x), ok


def pad_to_multiple(x, m):
    """Replicate-pad to a multiple of `m`. Returns (padded, (h, w)) ORIGINAL."""
    h, w = x.shape[-2:]
    H, W = -(-h // m) * m, -(-w // m) * m
    return F.pad(x, (0, W - w, 0, H - h), mode="replicate"), (h, w)


def corpus_point(rows, label_key, label, extra=()):
    """Per-image rows -> one curve point, means over the corpus.

    `extra` names the perceptual metrics to average alongside the fixed four, so
    adding one to `--perceptual` needs no edit here.
    """
    def m(key):
        vals = [r[key] for r in rows if r.get(key) is not None
                and np.isfinite(r[key])]
        return float(np.mean(vals)) if vals else None
    pt = {label_key: label, "n_images": len(rows), "bpp": m("bpp"),
          "psnr": m("psnr"), "ms_ssim_db": m("ms_ssim_db"),
          "ms_ssim": m("ms_ssim")}
    pt.update({k: m(k) for k in extra})
    return pt


def perceptual_summary(pt, keys):
    """The perceptual means as a printable tail, empty when none were scored."""
    parts = [f"{k} {pt[k]:.4f}" for k in keys if pt.get(k) is not None]
    return ("  " + "  ".join(parts)) if parts else ""


# ------------------------------------------------------------------ the sink
class Sink:
    """Accumulates curve points and rewrites the anchors file after each one.

    Same layout `eval_classical.py` writes, so `plot_rd.py`'s reader needs no
    reshaping: `curves` keyed by codec name, each a list of points sorted by
    rate. Written through a temp file and `os.replace`, so a crash mid-write
    cannot leave a truncated anchor where the next run expects a valid one --
    and this dependency does crash.
    """

    def __init__(self, path, meta):
        self.path, self.meta = path, meta
        self.curves, self.dropped, self.per_image, self.errors = {}, [], [], []

    def load(self):
        """Adopt an existing file, so a resumed run keeps what was measured."""
        if not os.path.exists(self.path):
            return False
        with open(self.path, encoding="utf-8") as f:
            old = json.load(f)
        self.curves = old.get("curves", {})
        self.dropped = old.get("dropped", [])
        self.per_image = old.get("per_image", [])
        self.errors = old.get("errors", [])
        return True

    def stop_above(self, model):
        """The label at which this model's sweep already left the window.

        Points ABOVE it are outside the figure and need not be measured. Points
        BELOW it must still be attempted, because "the sweep ended" is not the
        same as "every point below the end succeeded" -- a corrupt checkpoint or
        a crash leaves a gap, and treating the sweep as complete would make that
        gap permanent and silent.
        """
        ends = [d["point"] for d in self.dropped
                if isinstance(d, dict) and d.get("model") == model
                and d.get("ended_sweep")]
        return min(ends) if ends else None

    def measured(self, model):
        """Label values already recorded for this model, curve plus dropped."""
        key = LABEL_KEY[model]
        done = {p[key] for p in self.curves.get(CURVE_NAME[model], [])
                if key in p}
        done |= {d["point"] for d in self.dropped
                 if isinstance(d, dict) and d.get("model") == model}
        return done

    def add_point(self, model, pt):
        pts = self.curves.setdefault(CURVE_NAME[model], [])
        pts.append(pt)
        pts.sort(key=lambda p: p["bpp"])
        self.save()

    def drop_point(self, model, label, bpp, max_bpp, ended_sweep):
        self.dropped.append({"model": model, "point": label, "bpp": bpp,
                             "reason": f"at or past the {max_bpp} bpp window",
                             "ended_sweep": bool(ended_sweep)})
        self.save()

    def save(self):
        payload = dict(self.meta)
        payload.update({"curves": self.curves, "dropped": self.dropped,
                        "errors": self.errors, "per_image": self.per_image})
        tmp = self.path + ".tmp"
        with open(tmp, "w", encoding="utf-8") as f:
            json.dump(payload, f, indent=2)
        os.replace(tmp, self.path)


# --------------------------------------------------------------- compressai
def build_compressai(arch, quality, device, metric="mse"):
    """One pretrained CompressAI model, with its rANS CDFs materialized.

    The published state dict carries the quantized CDF buffers, but `compress()`
    reads them directly and fails obscurely when they are empty, so they are
    forced. (It is not the cause of the decoder instability above -- forcing the
    update leaves the CDF shape identical, and the failures happen either way.)
    Weights download to the torch hub cache on first use.
    """
    from compressai.zoo import bmshj2018_hyperprior, mbt2018
    zoo = {"balle": bmshj2018_hyperprior, "mbt": mbt2018}
    net = zoo[arch](quality=quality, metric=metric, pretrained=True,
                    progress=True).eval().to(device)
    net.update(force=True)
    return net


def checkpoint_url(arch, quality, metric="mse"):
    """The published URL for one zoo checkpoint, or "" if it cannot be resolved.

    Read out of CompressAI's own table rather than reconstructed, because the
    filename carries a hash suffix that torch.hub derives the cache name from.
    """
    try:
        import compressai.zoo.image as zi
        key = {"balle": "bmshj2018-hyperprior", "mbt": "mbt2018"}[arch]
        return zi.model_urls[key][metric][quality]
    except Exception:
        return ""


def cache_path(arch, quality, metric="mse"):
    """Where torch.hub keeps that checkpoint, so a corrupt one can be named."""
    url = checkpoint_url(arch, quality, metric)
    if not url:
        return ""
    return os.path.join(torch.hub.get_dir(), "checkpoints",
                        os.path.basename(url))


@torch.no_grad()
def code_compressai(net, orig, device, rate, verify_db=45.0, retries=4):
    """One image through one net -> a dict with the rate, the recon and timings."""
    x, (h, w) = pad_to_multiple(to_tensor(orig).to(device), 64)
    npx = orig.shape[0] * orig.shape[1]

    if rate == "estimated":
        t0 = time.perf_counter()
        out = net(x)
        bits = sum(float(-torch.log2(l).sum())
                   for l in out["likelihoods"].values())
        t_enc = time.perf_counter() - t0
        rec, ok = recon_u8(out["x_hat"], h, w)
        return {"bpp": bits / npx, "recon": rec, "bytes": None,
                "t_enc": t_enc, "t_dec": 0.0, "verify_db": None,
                "attempts": 1, "verified": ok,
                "why": "" if ok else "forward reconstruction is not finite"}

    ref, ref_ok = recon_u8(net(x)["x_hat"], h, w)
    if not ref_ok:
        return {"bpp": None, "recon": None, "bytes": None, "t_enc": 0.0,
                "t_dec": 0.0, "verify_db": None, "attempts": 0,
                "verified": False,
                "why": "forward reconstruction is not finite, so there is no "
                       "reference to verify a decode against"}

    out = None
    for attempt in range(1, retries + 2):
        t0 = time.perf_counter()
        enc = net.compress(x)
        t_enc = time.perf_counter() - t0
        nbytes = sum(len(s[0]) for s in enc["strings"])
        t0 = time.perf_counter()
        dec = net.decompress(enc["strings"], enc["shape"])
        t_dec = time.perf_counter() - t0
        rec, ok = recon_u8(dec["x_hat"], h, w)
        agree = psnr_u8(ref, rec) if ok else float("nan")
        good = bool(ok and agree >= verify_db)
        out = {"bpp": bpp_from_bytes(nbytes, orig.shape[0], orig.shape[1]),
               "recon": rec, "bytes": nbytes, "t_enc": t_enc, "t_dec": t_dec,
               "verify_db": None if not ok else agree, "attempts": attempt,
               "verified": good,
               "why": "" if good else ("decode is not finite (NaN)" if not ok
                                       else f"decode agrees with forward at "
                                            f"only {agree:.2f} dB")}
        if good or verify_db <= 0:
            break
    return out


def sweep_compressai(arch, args, images, device, sink, scorer=None):
    """Ascending quality sweep, stopping at the first point past the window."""
    pkeys = tuple(scorer.keys) if scorer is not None else ()
    already = sink.measured(arch)
    ceiling = sink.stop_above(arch)
    todo = [q for q in args.qualities
            if q not in already and (ceiling is None or q <= ceiling)]
    if not todo:
        print(f"  nothing to do: {sorted(already)} already measured"
              + (f", window closed at q{ceiling}" if ceiling else ""))
        return
    print(f"  to measure: {todo}"
          + (f" (q>{ceiling} is outside the window)" if ceiling else ""))
    for q in args.qualities:
        if q not in todo:
            continue
        try:
            net = build_compressai(arch, q, device, metric=args.metric)
        except Exception as exc:
            sink.errors.append({"model": arch, "q": q, "error": repr(exc),
                                "cache_file": cache_path(arch, q, args.metric)})
            sink.save()
            print(f"  q{q}: BUILD FAILED {exc!r}")
            cf = cache_path(arch, q, args.metric)
            if cf:
                have = (f"{os.path.getsize(cf):,} bytes"
                        if os.path.exists(cf) else "missing")
                print(f"       cached file: {cf} ({have})")
                print(f"       re-download: {checkpoint_url(arch, q, args.metric)}")
            continue
        rows, t0 = [], time.time()
        for i, path in enumerate(images, 1):
            orig = load_rgb(path, crop=args.crop)
            name = os.path.basename(path)
            try:
                r = code_compressai(net, orig, device, args.rate,
                                    verify_db=args.verify_db,
                                    retries=args.verify_retries)
            except Exception as exc:
                sink.errors.append({"model": arch, "q": q, "file": name,
                                    "error": repr(exc)})
                continue
            if not r["verified"]:
                sink.errors.append({"model": arch, "q": q, "file": name,
                                    "attempts": r["attempts"],
                                    "error": f"unverified: {r['why']}"})
                print(f"      [{i:>3}/{len(images)}] {name[:34]:<34} "
                      f"EXCLUDED after {r['attempts']} attempt(s): {r['why']}")
                continue
            psnr, msdb, msraw = score(orig, r["recon"])
            row = {"file": name, "model": arch, "q": q, "bpp": r["bpp"],
                   "psnr": psnr, "ms_ssim_db": msdb, "ms_ssim": msraw,
                   "bytes": r["bytes"], "t_enc": r["t_enc"],
                   "t_dec": r["t_dec"], "verify_db": r["verify_db"],
                   "attempts": r["attempts"]}
            if scorer is not None:
                # Scored on the reconstruction this payload actually decodes to,
                # which is how exp_keepn scores ours -- not on a second forward
                # pass, whose pixels are not the ones a decoder would produce.
                row.update(scorer.score(orig, r["recon"]))
            rows.append(row)
            sink.per_image.append(row)
            if args.progress:
                retry = "" if r["attempts"] == 1 \
                    else f"  (retried {r['attempts'] - 1}x)"
                print(f"      [{i:>3}/{len(images)}] {name[:34]:<34} "
                      f"bpp {r['bpp']:.4f}  psnr {psnr:.2f}  "
                      f"enc {r['t_enc']:.1f}s dec {r['t_dec']:.1f}s{retry}")
        del net
        if device.type == "cuda":
            torch.cuda.empty_cache()
        if not rows:
            sink.save()
            continue
        pt = corpus_point(rows, "q", q, extra=pkeys)
        pt["seconds"] = round(time.time() - t0, 1)
        pt["retried"] = sum(1 for r in rows if r["attempts"] > 1)
        print(f"  q{q}: bpp {pt['bpp']:.4f}  PSNR {pt['psnr']:.3f}  "
              f"MS-SSIM {pt['ms_ssim_db']:.2f} dB"
              f"{perceptual_summary(pt, pkeys)}  ({pt['seconds']:.0f}s, "
              f"n={pt['n_images']}, {pt['retried']} retried)")
        if pt["bpp"] >= args.max_bpp:
            sink.drop_point(arch, q, pt["bpp"], args.max_bpp,
                            args.stop_past_max)
            print(f"       past {args.max_bpp} bpp -- dropped"
                  + ("; ending this sweep" if args.stop_past_max else ""))
            if args.stop_past_max:
                return
            continue
        sink.add_point(arch, pt)


# ----------------------------------------------------------------- mcucoder
def load_mcucoder(weights, device, root=MCU_ROOT):
    if root not in sys.path:
        sys.path.insert(0, root)
    from mcucoder.model import MCUCoder
    model = MCUCoder()
    model.load_state_dict(torch.load(weights, map_location="cpu"))
    return model.eval().to(device)


@torch.no_grad()
def mcu_calibrate(model, paths, crop, device):
    """Per-channel min/max and Huffman tables, fitted on training images.

    Mirrors the reference `create_codec`: scale each channel to uint8 by its own
    observed range, drop to 6 bits, and fit one Huffman table per channel over
    the pooled calibration symbols. Every symbol 0..63 is seeded once so that a
    symbol which never appeared in calibration is still encodable.
    """
    from dahuffman import HuffmanCodec

    zs = []
    for path in paths:
        x = to_tensor(load_rgb(path, crop=crop)).to(device)
        x, _ = pad_to_multiple(x, 8)
        zs.append(model.encoder(x).detach().cpu())
    setting = {"min": {}, "max": {}, "codec": {}}
    for c in range(MCU_CHANNELS):
        data = torch.cat([z[:, c].reshape(-1) for z in zs])
        lo, hi = float(data.min()), float(data.max())
        q = ((data - lo) / (hi - lo) * 255).to(torch.uint8)
        q = (q // MCU_STEP).numpy()
        q = np.append(q, np.arange(MCU_SYMBOLS, dtype=q.dtype))
        setting["min"][c], setting["max"][c] = lo, hi
        setting["codec"][c] = HuffmanCodec.from_data(q)
    return setting


def mcu_quantize(z_c, lo, hi):
    """One channel -> 6-bit symbols (uint8), the transmitted representation."""
    q = ((z_c - lo) / (hi - lo) * 255).clamp(0, 255).to(torch.uint8)
    return q // MCU_STEP


@torch.no_grad()
def code_mcucoder(model, orig, codec, keep, device):
    """One image at `keep` transmitted channels -> (bpp, recon_u8, bytes, ok)."""
    x, (h, w) = pad_to_multiple(to_tensor(orig).to(device), 8)
    z = model.encoder(x)
    zq = torch.zeros_like(z)
    nbytes = 0
    for c in range(keep):
        lo, hi = codec["min"][c], codec["max"][c]
        sym = mcu_quantize(z[:, c], lo, hi)
        nbytes += len(codec["codec"][c].encode(sym.reshape(-1).cpu().numpy()))
        deq = (sym.to(torch.float32) * MCU_STEP) / 255.0 * (hi - lo) + lo
        zq[:, c] = deq
    rec, ok = recon_u8(model.decoder(zq), h, w)
    return bpp_from_bytes(nbytes, orig.shape[0], orig.shape[1]), rec, nbytes, ok


def sweep_mcucoder(args, images, device, sink, scorer=None):
    pkeys = tuple(scorer.keys) if scorer is not None else ()
    already = sink.measured("mcucoder")
    ceiling = sink.stop_above("mcucoder")
    todo = [k for k in range(1, MCU_CHANNELS + 1)
            if k not in already and (ceiling is None or k <= ceiling)]
    if not todo:
        print(f"  nothing to do: {sorted(already)} already measured"
              + (f", window closed at {ceiling}/12" if ceiling else ""))
        return
    model = load_mcucoder(args.mcu_weights, device, args.mcu_root)
    calib = sorted(f for f in os.listdir(args.mcu_calib_dir)
                   if f.lower().endswith((".png", ".jpg", ".jpeg")))
    calib = [os.path.join(args.mcu_calib_dir, f)
             for f in calib[:args.mcu_calib_images]]
    if not calib:
        raise SystemExit(f"no calibration images in {args.mcu_calib_dir}")
    t0 = time.time()
    codec = mcu_calibrate(model, calib, args.mcu_calib_crop, device)
    print(f"  calibrated on {len(calib)} images at "
          f"{args.mcu_calib_crop or 'native'} px ({time.time() - t0:.0f}s)")

    for keep in todo:
        rows, t0 = [], time.time()
        for i, path in enumerate(images, 1):
            orig = load_rgb(path, crop=args.crop)
            name = os.path.basename(path)
            try:
                bpp, rec, nbytes, ok = code_mcucoder(model, orig, codec, keep,
                                                     device)
            except Exception as exc:
                sink.errors.append({"model": "mcucoder", "channels": keep,
                                    "file": name, "error": repr(exc)})
                continue
            if not ok:
                sink.errors.append({"model": "mcucoder", "channels": keep,
                                    "file": name,
                                    "error": "reconstruction is not finite"})
                continue
            psnr, msdb, msraw = score(orig, rec)
            row = {"file": name, "model": "mcucoder", "channels": keep,
                   "bpp": bpp, "psnr": psnr, "ms_ssim_db": msdb,
                   "ms_ssim": msraw, "bytes": nbytes}
            if scorer is not None:
                row.update(scorer.score(orig, rec))
            rows.append(row)
            sink.per_image.append(row)
            if args.progress:
                print(f"      [{i:>3}/{len(images)}] {name[:34]:<34} "
                      f"bpp {bpp:.4f}  psnr {psnr:.2f}")
        if not rows:
            sink.save()
            continue
        pt = corpus_point(rows, "channels", keep, extra=pkeys)
        pt["seconds"] = round(time.time() - t0, 1)
        print(f"  {keep:>2}/12 channels: bpp {pt['bpp']:.4f}  PSNR "
              f"{pt['psnr']:.3f}  MS-SSIM {pt['ms_ssim_db']:.2f} dB"
              f"{perceptual_summary(pt, pkeys)}  "
              f"({pt['seconds']:.0f}s, n={pt['n_images']})")
        if pt["bpp"] >= args.max_bpp:
            sink.drop_point("mcucoder", keep, pt["bpp"], args.max_bpp,
                            args.stop_past_max)
            print(f"       past {args.max_bpp} bpp -- dropped"
                  + ("; ending this sweep" if args.stop_past_max else ""))
            if args.stop_past_max:
                return
            continue
        sink.add_point("mcucoder", pt)


# --------------------------------------------------------------------- main
def build_argparser():
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--models", default="balle,mbt,mcucoder",
                    help="comma list from balle,mbt,mcucoder "
                         "(default: %(default)s)")
    ap.add_argument("--qualities", default="1,2,3,4,5,6,7,8",
                    help="CompressAI quality indices, ascending "
                         "(default: %(default)s)")
    ap.add_argument("--metric", default="mse", choices=["mse", "ms-ssim"],
                    help="which pretrained variant to pull (default %(default)s)")
    ap.add_argument("--rate", default="real", choices=["real", "estimated"],
                    help="'real' codes the actual bitstream and measures its "
                         "bytes; 'estimated' uses -log2 p of the latents, which "
                         "is ~0.5%% optimistic (default %(default)s)")
    ap.add_argument("--max-bpp", type=float, default=0.5,
                    help="drop points at or above this corpus-mean rate "
                         "(default %(default)s)")
    ap.add_argument("--stop-past-max", action=argparse.BooleanOptionalAction,
                    default=True,
                    help="end a model's sweep at the first point past the "
                         "window instead of measuring the rest "
                         "(default: --stop-past-max)")
    ap.add_argument("--verify-db", type=float, default=45.0,
                    help="a real decode must reproduce the forward "
                         "reconstruction to at least this PSNR or it is "
                         "re-coded (0 disables). Agreement is bimodal: good "
                         "decodes land above 100 dB, broken ones near 12 "
                         "(default %(default)s)")
    ap.add_argument("--verify-retries", type=int, default=4,
                    help="re-code attempts before an image is excluded and "
                         "recorded as an error (default %(default)s)")
    ap.add_argument("--perceptual", default="lpips,brisque",
                    help="perceptual metrics scored on each reconstruction, "
                         "from scripts/perceptual.py's registry (lpips, brisque, "
                         "niqe, pi, tres). Empty disables. LPIPS costs 0.02 s "
                         "and BRISQUE 0.01 s per full-resolution image, which is "
                         "free next to the coding itself "
                         "(default: %(default)s)")
    ap.add_argument("--resume", action=argparse.BooleanOptionalAction,
                    default=True,
                    help="keep what an existing anchors file already holds and "
                         "skip those points; this dependency crashes hard "
                         "occasionally (default: --resume)")
    ap.add_argument("--limit", type=int, default=0,
                    help="first N images of the corpus only (smoke tests)")
    ap.add_argument("--crop", type=int, default=0,
                    help="centre-crop to NxN (0 = full resolution)")
    ap.add_argument("--device", default="cuda")
    ap.add_argument("--mbt-device", default="",
                    help="override --device for mbt2018 only. Its entropy coder "
                         "is sequential CPU either way, and 'cpu' measured 45 s "
                         "against 65 s per CLIC image for identical output")
    ap.add_argument("--progress", action="store_true",
                    help="print a line per image")
    ap.add_argument("--tag", default="",
                    help="writes learned_<tag>.json into --out-dir "
                         "(default: <dataset>_full, or <dataset>_c<crop>)")
    ap.add_argument("--mcu-root", default=MCU_ROOT,
                    help="the MCUCoder checkout. Defaults to $VQLIC_MCU_ROOT.")
    ap.add_argument("--mcu-weights", default=MCU_WEIGHTS)
    ap.add_argument("--mcu-calib-dir",
                    default=os.environ.get("VQLIC_TRAIN", ""),
                    help="images the per-channel ranges and Huffman tables are "
                         "fitted on -- training data, held out from both eval "
                         "corpora. Defaults to $VQLIC_TRAIN.")
    ap.add_argument("--mcu-calib-images", type=int, default=64,
                    help="how many calibration images (default %(default)s, the "
                         "reference notebook's number)")
    ap.add_argument("--mcu-calib-crop", type=int, default=448,
                    help="crop for the calibration pass; 0 = native resolution "
                         "(default %(default)s)")
    add_dataset_arg(ap)
    add_out_dir_arg(ap)
    return ap


def main(argv=None):
    args = build_argparser().parse_args(argv)
    models = [m.strip() for m in args.models.split(",") if m.strip()]
    unknown = [m for m in models if m not in CURVE_NAME]
    if unknown:
        raise SystemExit(f"unknown model(s) {unknown}; "
                         f"choose from {list(CURVE_NAME)}")
    args.qualities = [int(q) for q in args.qualities.split(",") if q.strip()]

    device = torch.device(args.device if torch.cuda.is_available() else "cpu")
    scorer = None
    pkeys = [m.strip() for m in args.perceptual.split(",") if m.strip()]
    if pkeys:
        from perceptual import PerceptualScorer
        # On --device even when mbt2018 codes on the CPU: the scorer has nothing
        # to do with the entropy coder, and LPIPS is a VGG16 forward.
        scorer = PerceptualScorer(str(device), metrics=pkeys).warmup()
        print(f"perceptual: {','.join(scorer.keys)} on {scorer.device}")
    images = dataset_images(args.dataset)
    if args.limit:
        images = images[:args.limit]
    tag = args.tag or (f"{args.dataset}_c{args.crop}" if args.crop
                       else f"{args.dataset}_full")
    out_path = os.path.join(out_dir(args.out_dir), f"learned_{tag}.json")

    sink = Sink(out_path, {
        "n_images": len(images), "crop": args.crop, "dataset": args.dataset,
        "config": {"rate": args.rate, "metric": args.metric,
                   "max_bpp": args.max_bpp, "qualities": args.qualities,
                   "stop_past_max": bool(args.stop_past_max),
                   "verify_db": args.verify_db,
                   "verify_retries": args.verify_retries,
                   "device": str(device),
                   "mbt_device": args.mbt_device or str(device),
                   "mcu_weights": args.mcu_weights,
                   "mcu_calib_dir": args.mcu_calib_dir,
                   "mcu_calib_images": args.mcu_calib_images,
                   "mcu_calib_crop": args.mcu_calib_crop,
                   "mcu_step": MCU_STEP,
                   "perceptual": list(scorer.keys) if scorer else []}})
    if args.resume and sink.load():
        have = {k: len(v) for k, v in sink.curves.items() if v}
        print(f"resuming {os.path.basename(out_path)}: {have or 'no points yet'}")

    where = f"{args.crop}x{args.crop} centre crop" if args.crop \
        else "full resolution"
    print(f"{len(images)} {dataset_label(args.dataset)} images at {where} | "
          f"{args.rate} rate | window < {args.max_bpp} bpp | device {device}\n")

    t_all = time.time()
    for m in models:
        print(f"== {CURVE_NAME[m]}")
        if m == "mcucoder":
            sweep_mcucoder(args, images, device, sink, scorer)
        else:
            dev = device
            if m == "mbt" and args.mbt_device:
                dev = torch.device(args.mbt_device)
            sweep_compressai(m, args, images, dev, sink, scorer)
        print()

    sink.save()
    for name, pts in sink.curves.items():
        print(f"{name}: {len(pts)} point(s) inside the window")
        for p in pts:
            k = p.get("q", p.get("channels"))
            extra = "  ".join(
                f"{m} {p[m]:.4f}" for m in ("lpips", "brisque", "niqe", "pi",
                                            "tres") if p.get(m) is not None)
            print(f"   {k:<4} bpp {p['bpp']:.4f}  PSNR {p['psnr']:6.2f}  "
                  f"MS-SSIM {p['ms_ssim_db']:6.2f} dB  {extra}  "
                  f"(n={p['n_images']})")
    for d in sink.dropped:
        print(f"dropped: {d}")
    if sink.errors:
        print(f"{len(sink.errors)} error(s); first few:")
        for e in sink.errors[:5]:
            print(f"   {e}")
    print(f"\nwrote {out_path}  ({time.time() - t_all:.0f}s this invocation)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
