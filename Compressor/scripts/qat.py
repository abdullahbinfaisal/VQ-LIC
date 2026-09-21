#!/usr/bin/env python
"""INT8 quantization-aware finetune of a trained rung, codebook included.

This is the step that produces the **published** encoder. Training with
`scripts/train.py` gives you the FP32 parent; this converts its encoder to INT8
through `fuse_fx` -> `prepare_qat_fx` -> `convert_fx`, quantizes the codebook to
INT8 alongside, and re-estimates the coding prior under the quantized assignment.

The implementation is `vqlic.qat`; this only puts `Compressor/` on sys.path
first.

    # the published recipe: INT8 encoder + INT8 codebook + prior re-estimation
    python scripts/qat.py \
        --qat-from weights/bpp030_fp32.pt \
        --name bpp030_cbint8 --out-dir weights \
        --qat-quantize-codebook --qat-codebook-bits 8 \
        --train-dir /data/openimages/train

    # resume a preempted run
    python scripts/qat.py --qat-resume weights/bpp030_cbint8_qat_last.pt \
        --train-dir /data/openimages/train

Order matters and `vqlic.qat` sequences it: observers freeze before the codebook
is quantized, and the prior is re-estimated last, against the assignment the
INT8 codebook actually produces. A prior estimated before that step prices a
distribution the deployed model does not emit.

`python scripts/qat.py --help` lists every flag.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from vqlic.qat import main  # noqa: E402

if __name__ == "__main__":
    raise SystemExit(main())
