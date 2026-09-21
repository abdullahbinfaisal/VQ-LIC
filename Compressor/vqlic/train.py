"""CLI entrypoint.

    python train.py --train-dir /data/openimages/train [--flags]
    python -m vqlic.train --train-dir /data/openimages/train [--flags]

`--list-presets` prints the bitrate ladder and `--print-config` the resolved
settings; neither touches any data.
"""
from __future__ import annotations

import os
import random
import sys

import numpy as np
import torch

from vqlic import presets
from vqlic.codec import build_model
from vqlic.config import build_argparser, config_from_args
from vqlic.dataset import build_trainloader, build_valloader
from vqlic.engine import train
from vqlic.metrics import model_size_table


def set_seed(seed):
    random.seed(seed)
    np.random.seed(seed)
    torch.manual_seed(seed)
    torch.cuda.manual_seed_all(seed)


def peek_step(path):
    """Read just the step counter out of a checkpoint.

    Needed before the loader exists: with `shuffle=False` the sampler has to be
    offset to the resumed position, and the loader is constructed before
    `engine.train` performs the real restore.
    """
    if not path or not os.path.exists(path):
        return 0
    try:
        ckpt = torch.load(path, map_location="cpu", weights_only=False)
        return int(ckpt.get("step", 0))
    except Exception as e:
        print(f"warning: could not read step from {path}: {e}")
        return 0


def main(argv=None):
    args = build_argparser(argv).parse_args(argv)

    if args.list_presets:
        print(presets.describe())
        return 0

    cfg = config_from_args(args)

    if args.print_config:
        print(cfg.summary())
        return 0

    if not cfg.train_dir:
        print("\nerror: --train-dir is required (or set NIC_TRAIN_DIR).\n"
              "       run with --print-config to inspect settings without data.",
              file=sys.stderr)
        return 2

    set_seed(cfg.seed)
    os.makedirs(cfg.out_dir, exist_ok=True)

    # -------------------------------------------------------------- resume point
    resume_path = cfg.resume
    if resume_path == "auto":
        cand = cfg.file("last.pt")
        resume_path = cand if os.path.exists(cand) else ""
        cfg.resume = resume_path
    resumed_step = peek_step(resume_path)
    start_index = (resumed_step * cfg.batch_size) if not cfg.shuffle else 0


    torch.backends.cudnn.benchmark = True
    torch.set_float32_matmul_precision("high")   # TF32
    # ------------------------------------------------------------------- loaders
    print()
    train_loader = build_trainloader(
        cfg.train_dir, batch_size=cfg.batch_size, image_size=cfg.image_size,
        num_workers=cfg.num_workers, shuffle=cfg.shuffle, seed=cfg.seed,
        start_index=start_index, scales=cfg.scale_sizes(),
        file_list=cfg.file_list, limit=cfg.limit_images,
        crop_only=cfg.crop_only)
    val_loader = None
    if cfg.val_dir:
        val_loader = build_valloader(
            cfg.val_dir, batch_size=cfg.batch_size, image_size=cfg.image_size,
            num_workers=0, resize_short=cfg.val_resize_short or None)

    # --------------------------------------------------------------------- model
    model = build_model(cfg)
    print()
    print(model_size_table(model))
    print()
    print(model.encoder.summary(cfg.image_size))
    print()

    train(cfg, model, train_loader, val_loader)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
