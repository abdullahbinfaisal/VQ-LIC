#!/usr/bin/env python
"""Rate-distortion evaluation of the ladder over a corpus -> `ours_<tag>.json`.

Runs the same round trip `scripts/codec.py` does, over every image of a corpus
and every rung of the ladder, and writes the corpus means with the per-image rows
behind them. This is where the reported numbers come from.

`bpp` is measured, never estimated: `8 * len(payload) / pixels`, with the payload
being what `compress` actually emits. `bpp_prior` (an ideal coder against the
frozen prior) and `bpp_empirical` (this image's own histogram) are recorded
alongside so the JSON carries the comparison instead of asking a reader to trust
one of them -- `bpp_empirical` in particular is optimistic, since a decoder
cannot know the histogram of an image it has not received yet.

Each rung is verified, not assumed: after every image the realised indices are
checked against the keep mask, and a single code outside it fails the run. A
non-zero violation count means the numbers beside it were measured under a mask
that did not hold, which is worse than no numbers.

    # the reported curve, both models, on Kodak
    python scripts/eval.py --dataset kodak --tag kodak

    # smoke test first -- two images, one rung
    python scripts/eval.py --dataset kodak --rungs bpp030 --limit 2 --tag smoke

    # the INT8 deployment model only
    python scripts/eval.py --dataset clic --models int8 --tag clic

Writes `ours_<tag>_final.json` (INT8 QAT) and `ours_<tag>_float.json` (the FP32
pre-QAT parents) into `--out-dir`.
"""
from __future__ import annotations

import argparse
import json
import os
import sys
import time

import numpy as np
import torch

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from _common import (  # noqa: E402
    Formatter, RUNGS, add_dataset_arg, add_out_dir_arg, assemble,
    dataset_images, dataset_label, load_rgb, norm_from_u8, out_dir, score,
)
from vqlic.metrics import to_uint8  # noqa: E402

BPP_CEILING = 0.5     # the rate range the RD figures are drawn over

VARIANTS = {
    "int8": ("final", True, "INT8 QAT + INT8 codebook + keepN=64 + left context"),
    "float": ("float", False, "FP32 pre-QAT + keepN=64 + left context"),
}


@torch.no_grad()
def evaluate_rung(model, device, images, originals, crop, keep_n,
                  build_table=False, progress=False):
    """One assembled codec over a corpus -> (per-image rows, violations, seen).

    `violations` must come back 0. It is the check that the `-inf` prior really
    did make the dropped codes unreachable, verified on each image's own realised
    indices rather than trusted.
    """
    q = model.quantizer
    keep_t = (q._keep_mask if hasattr(q, "_keep_mask")
              else torch.ones(q.G, q.K, dtype=torch.bool, device=device))
    rows, violations = [], 0
    # The realised alphabet, accumulated over the corpus. Pruning to N codes does
    # not oblige the encoder to use all N, and at small N it does not.
    seen = np.zeros((q.G, q.K), dtype=np.int64)

    for i, path in enumerate(images, 1):
        orig = originals[path]
        x = norm_from_u8(orig).to(device)

        payload, stats = model.compress_stats(x, build_table=build_table)
        recon = to_uint8(model.decompress(payload))
        psnr, msdb, msraw = score(orig, recon, device=device)
        rows.append({
            "file": os.path.basename(path),
            "bpp": stats["bpp"], "psnr": psnr,
            "ms_ssim_db": msdb, "ms_ssim": msraw,
            "bpp_prior": stats["bpp_prior"],
            "bpp_empirical": stats["bpp_empirical"],
            "mode": stats["mode_name"],
            "bytes": len(payload),
        })

        # Verification on this image's own realised indices.
        xp, _H, _W, _ph, _pw = model._prepare(x)
        _, _, idx, _ = q(model.encoder(xp))
        flat = idx.reshape(-1, q.G)
        for g in range(q.G):
            violations += int((~keep_t[g][flat[:, g]]).sum())
            seen[g] += np.bincount(flat[:, g].cpu().numpy(), minlength=q.K)

        if progress:
            r = rows[-1]
            print(f"      [{i:3d}/{len(images)}] {r['file'][:34]:<34} "
                  f"bpp {r['bpp']:.4f} psnr {r['psnr']:.2f}", flush=True)

    return rows, violations, seen


def summarize(rows):
    """Corpus means, plus a count of which coding mode each image landed in."""
    keys = ("bpp", "psnr", "ms_ssim_db", "ms_ssim", "bpp_prior", "bpp_empirical")
    out = {k: float(np.mean([r[k] for r in rows])) for k in keys}
    modes = {}
    for r in rows:
        modes[r["mode"]] = modes.get(r["mode"], 0) + 1
    out["modes"] = modes
    out["n_images"] = len(rows)
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=Formatter)
    ap.add_argument("--rungs", default=",".join(RUNGS),
                    help="comma-separated rungs to evaluate")
    ap.add_argument("--models", default="int8,float",
                    help="which of the two curves to measure: `int8` (the "
                         "deployment model) and/or `float` (its FP32 parent)")
    ap.add_argument("--keep-n", type=int, default=64,
                    help="codes kept per codebook; 64 is what is reported")
    ap.add_argument("--table", action="store_true",
                    help="transmit a per-image histogram instead of coding "
                         "against the frozen prior. Not what is reported.")
    ap.add_argument("--crop", type=int, default=0,
                    help="centre-crop every image to NxN (0 = full resolution)")
    ap.add_argument("--limit", type=int, default=0, help="first N images only")
    ap.add_argument("--device", default="cuda")
    ap.add_argument("--tag", default="eval",
                    help="writes ours_<tag>_final.json / ours_<tag>_float.json")
    ap.add_argument("--progress", action="store_true", help="per-image lines")
    add_dataset_arg(ap)
    add_out_dir_arg(ap)
    args = ap.parse_args()

    rungs = [r.strip() for r in args.rungs.split(",") if r.strip()]
    models = [m.strip() for m in args.models.split(",") if m.strip()]
    for m in models:
        if m not in VARIANTS:
            raise SystemExit(f"unknown model {m!r}; choose from {list(VARIANTS)}")

    images = dataset_images(args.dataset)
    if args.limit:
        images = images[:args.limit]
    dest = out_dir(args.out_dir)

    where = f"{args.crop}x{args.crop} centre crop" if args.crop else "full resolution"
    print(f"{len(images)} {dataset_label(args.dataset)} images at {where}\n")

    # Decoded once and shared by every rung and both models: the PNGs are up to
    # 2048 px and decoding one is tens of milliseconds, which is a serial cost
    # for nothing when six configurations want the same pixels.
    print("decoding originals ...", flush=True)
    originals = {p: load_rgb(p, crop=args.crop) for p in images}

    for name in models:
        suffix, int8, variant = VARIANTS[name]
        result = {}
        print(f"\n=== {variant} ===")
        for rung in rungs:
            t0 = time.time()
            model, device, meta = assemble(
                rung, float_model=not int8, keep_n=args.keep_n,
                device=args.device)
            rows, violations, seen = evaluate_rung(
                model, device, images, originals, args.crop, args.keep_n,
                build_table=args.table, progress=args.progress)
            if violations:
                raise SystemExit(
                    f"{rung}: {violations} index assignments landed outside the "
                    f"keep mask. The mask did not hold, so these numbers belong "
                    f"to a codec that is not the one being described -- refusing "
                    f"to write them.")

            means = summarize(rows)
            result[rung] = {
                "variant": variant,
                "dataset": args.dataset,
                "dataset_label": dataset_label(args.dataset),
                "crop": args.crop,
                "keep_n": args.keep_n,
                "codes_used": [int((seen[g] > 0).sum()) for g in range(seen.shape[0])],
                "bpp_ceiling": BPP_CEILING,
                # The context tables are side information held by both peers and
                # never transmitted, so the payload carries no table at all.
                "build_table": bool(args.table),
                "table_bpp": 0.0,
                "per_image": rows,
                **meta,
                **means,
            }
            print(f"  {rung:<8} {means['bpp']:.4f} bpp  psnr {means['psnr']:.2f}"
                  f"  ms-ssim {means['ms_ssim_db']:.2f} dB"
                  f"  modes {means['modes']}  [{time.time() - t0:.0f}s]")
            del model

        path = os.path.join(dest, f"ours_{args.tag}_{suffix}.json")
        with open(path, "w", encoding="utf-8") as fh:
            json.dump(result, fh, indent=1)
        print(f"  -> {path}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
