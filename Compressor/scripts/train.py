#!/usr/bin/env python
"""Train one rate rung from scratch.

The implementation is `vqlic.train`; this only puts `Compressor/` on sys.path
first, so the package resolves when the script is invoked by path from another
working directory.

    # sanity: builds the model and prints its size, touches no data
    python scripts/train.py --dry-run

    # a rung. --name, --out-dir and the codebook shape all follow from --preset.
    # --max-steps counts BATCHES, so it moves with --batch-size.
    python scripts/train.py --preset bpp030 --train-dir /data/openimages/train

    # explicit flags always beat the preset
    python scripts/train.py --preset bpp030 --rate-beta-final 0.02

    # resume: picks up <out-dir>/<name>_last.pt and offsets the sequential
    # sampler so it continues through the corpus instead of re-reading the start
    python scripts/train.py --preset bpp030 --resume

`python scripts/train.py --help` lists every flag. The published checkpoints came
off the three presets `bpp010`, `bpp030` and `bpp040`.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from vqlic.train import main  # noqa: E402

if __name__ == "__main__":
    raise SystemExit(main())
