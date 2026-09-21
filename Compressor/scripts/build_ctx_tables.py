#!/usr/bin/env python
"""Fit the context tables the codec codes against, and write them to weights/.

The fitted tables ship with the checkpoints, so you only need this to refit them
-- after retraining a rung, or to reproduce the fit from scratch. It needs the
OpenImages training corpus (`VQLIC_TRAIN`); nothing else here does.

Two passes in one run, per rung: encode `--limit` training images through the
pruned model and accumulate `n(context, code)`, then post-process those counts
into the shipped table. The raw counts are saved too, because `min_count` and
`top_n` are pure post-processing -- a whole size/rate sweep costs no re-encoding.

THE LOCKED CONFIGURATION, imported from `vqlic.context_fit` rather than restated
so it cannot drift from the reported one:

    order      left        65 contexts per group, not leftup's 4,225
    keep_n     64          keepN pruning: alphabet is 64 codes + border
    min_count  512         observations before a context earns its own table
    top_n      16          16 (symbol, freq) pairs; tail from the marginal
    prob_bits  16          fitted resolution, cannot be changed after the fact

    python scripts/build_ctx_tables.py                      # all three rungs
    python scripts/build_ctx_tables.py --float               # the fp32 parents
    python scripts/build_ctx_tables.py --rungs bpp030 --limit 8   # smoke test

`--float` fits the same tables against the FLOAT, pre-QAT parents, so a float
curve can be context-coded exactly as the INT8 one is and the gap between the two
curves is the QAT cost rather than a difference in entropy coding. Those tables
carry an `fp32_` infix: a float model and its INT8 finetune emit different code
distributions, and one table cannot price both.

WHY THE SURVIVING CODES DIFFER PER MODEL
----------------------------------------
keepN keeps each codebook's 64 highest-prior codes, read off
`quantizer.cluster_size` -- and `vqlic/qat.py::finalize` writes the re-estimated
prior INTO `cluster_size` at the end of every finetune (it preserves the
per-group mass; the centroids never move). So the survivors are selected by each
model's own current prior. Both the choice of survivors and the counts
conditioned on them are refitted here.

Sizes are reported two ways and the difference matters:

* **observed** -- what this rung's populated tables occupy, with a 4-byte sparse
  context index per table.
* **budget** -- a fixed `(keep_n + 1)` tables per group, no sparse index needed
  because the context IS the array offset. This is the number firmware is sized
  against, and it does not depend on which rung populated it.
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
    Formatter, RUNGS, WEIGHTS_DIR, assemble, load_rgb, train_images,
)
from vqlic.codec import pad_to_multiple  # noqa: E402
from vqlic.context import check_context_definitions, n_contexts  # noqa: E402
from vqlic.context_fit import (  # noqa: E402
    ALPHA, MIN_COUNT, PROB_BITS, ContextCounter,
)


@torch.no_grad()
def encode_indices(model, path):
    """One image path -> its `[h, w, G]` index map, and the pixel count."""
    a = load_rgb(path)
    x = torch.from_numpy(a.copy()).permute(2, 0, 1).float()
    x = x.div_(255.0).sub_(0.5).div_(0.5)[None]
    xp, _ = pad_to_multiple(x, 8)
    _, _, indices, _ = model.quantizer(model.encoder(xp))
    return (indices[0].cpu().numpy().astype(np.int32),
            a.shape[0] * a.shape[1])


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=Formatter)
    ap.add_argument("--rungs", default=",".join(RUNGS))
    ap.add_argument("--float", action="store_true", dest="float_model",
                    help="fit against the FP32 pre-QAT parents; writes the "
                         "tables with an fp32_ infix")
    ap.add_argument("--limit", type=int, default=4000,
                    help="training images to fit on; 4000 is what was reported")
    ap.add_argument("--order", default="left",
                    help="context definition. `left` is what ships; the "
                         "hardware implements it.")
    ap.add_argument("--keep-n", type=int, default=64)
    ap.add_argument("--top-n", type=int, default=16)
    ap.add_argument("--min-count", type=int, default=MIN_COUNT)
    ap.add_argument("--prob-bits", type=int, default=PROB_BITS)
    ap.add_argument("--alpha", type=float, default=ALPHA)
    ap.add_argument("--device", default="cuda")
    ap.add_argument("--out-dir", default=WEIGHTS_DIR,
                    help="where the tables are written")
    args = ap.parse_args()

    rungs = [r.strip() for r in args.rungs.split(",") if r.strip()]
    infix = "fp32_" if args.float_model else ""
    os.makedirs(args.out_dir, exist_ok=True)
    images = train_images(args.limit)
    report = {}

    print(f"order={args.order} keep_n={args.keep_n} top_n={args.top_n} "
          f"min_count={args.min_count} prob_bits={args.prob_bits}")
    print(f"fitting on {len(images)} training images "
          f"({'fp32 pre-QAT' if args.float_model else 'INT8 QAT'} models)\n")

    for rung in rungs:
        model, device, meta = assemble(
            rung, float_model=args.float_model, keep_n=args.keep_n,
            device=args.device, attach_context=False)
        q = model.quantizer

        # The encoder and the decoder must derive the same context id from the
        # same history -- the encoder uses the bulk form, the decoder the
        # incremental one, and nothing in the stream would flag a disagreement.
        check_context_definitions(q.K, args.order)

        counter = ContextCounter(q.G, q.K, args.order, keep_n=args.keep_n)
        t0 = time.time()
        for i, path in enumerate(images, 1):
            idx, _px = encode_indices(model, path)
            counter.add(idx)
            if i % 250 == 0 or i == len(images):
                rate = i / max(time.time() - t0, 1e-9)
                print(f"    [{i:5d}/{len(images)}] {rate:5.1f} img/s", flush=True)

        stem = f"{infix}{rung}_keep{args.keep_n}_{args.order}"
        cpath = counter.save_counts(
            os.path.join(args.out_dir, f"counts_{stem}.npz"))

        cm = counter.build(args.prob_bits, args.min_count, args.alpha,
                           args.top_n)
        tpath = os.path.join(args.out_dir,
                             f"ctx_{stem}_top{args.top_n}.npz")
        cm.save(tpath)

        nctx = n_contexts(args.keep_n or cm.K, args.order)
        report[rung] = {
            "file": os.path.basename(tpath),
            "counts_file": os.path.basename(cpath),
            "model": meta["model"], "checkpoint": meta["checkpoint"],
            "order": args.order, "keep_n": args.keep_n, "top_n": args.top_n,
            "min_count": args.min_count, "prob_bits": args.prob_bits,
            "G": cm.G, "K": cm.K, "contexts_per_group": nctx,
            "n_tables": cm.n_tables(),
            "observed_bytes": cm.nbytes(),
            "budget_bytes": cm.nbytes(worst_case=True),
            "training_images": len(images),
            "training_tokens": int(counter.tokens),
        }
        print(f"  {rung:<8} tables/group {str(cm.n_tables()):<22} "
              f"observed {cm.nbytes()/1024:>7.1f}K  "
              f"budget {cm.nbytes(worst_case=True)/1024:>7.1f}K  "
              f"ctx/grp {nctx}")
        print(f"  -> {tpath}\n")
        del model

    rpath = os.path.join(args.out_dir,
                         f"ctx_tables_{infix}{args.order}_top{args.top_n}.json")
    with open(rpath, "w", encoding="utf-8") as fh:
        json.dump(report, fh, indent=1)
    worst = max(r["budget_bytes"] for r in report.values())
    print(f"firmware budget across all rungs: {worst/1024:.1f} KB "
          f"(identical by construction -- a fixed allocation does not depend on "
          f"which rung populated it)")
    print(f"wrote {rpath}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
