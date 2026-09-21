#!/usr/bin/env python
"""Load the codec, compress an image, decompress it, report what it cost.

This is the "does it work" script: one image in, a real payload out, the
reconstruction decoded back from those bytes, and the measured bpp and PSNR
printed. Nothing here is estimated -- `bpp` is `8 * len(payload) / pixels`, where
the payload is exactly what `NeuralImageCodec.compress` emits (header, any
transmitted table, and the context-coded index streams).

The codec is assembled the way every reported number is: INT8 QAT encoder, INT8
codebook, each codebook pruned to its top 64 codes, and the fitted left-neighbour
context model attached. `--float` swaps in the FP32 pre-QAT parent and its own
tables, which is the second curve on the RD figures.

    # round-trip one image at the middle rung
    python scripts/codec.py --rung bpp030 --image path/to/img.png

    # keep the bytes and the reconstruction
    python scripts/codec.py --rung bpp030 --image img.png \\
        --write-payload out.bin --write-recon out.png

    # the FP32 parent, for the INT8-vs-float delta
    python scripts/codec.py --rung bpp030 --image img.png --float

    # decode a payload written earlier -- no original needed
    python scripts/codec.py --rung bpp030 --decode out.bin --write-recon out.png

A payload is only decodable by the rung that wrote it: the context tables and the
pruned codebook are the decoder's half of the agreement, not part of the stream.
Decoding with the wrong rung does not raise, it produces garbage -- so the header
carries the shape and this script prints which weights it used.
"""
from __future__ import annotations

import argparse
import os
import sys

import torch

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from _common import (  # noqa: E402
    Formatter, RUNGS, assemble, load_rgb, norm_from_u8, score,
)
from vqlic.metrics import to_uint8  # noqa: E402


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=Formatter)
    ap.add_argument("--rung", default="bpp030", choices=RUNGS,
                    help="which rate rung to load")
    ap.add_argument("--image", default="", help="image to compress")
    ap.add_argument("--decode", default="", metavar="PAYLOAD",
                    help="decode an existing payload instead of compressing. "
                         "With --image as well, the reconstruction is scored "
                         "against it.")
    ap.add_argument("--float", action="store_true", dest="float_model",
                    help="the FP32 pre-QAT parent and its ctx_fp32_ tables, "
                         "instead of the INT8 deployment model")
    ap.add_argument("--keep-n", type=int, default=64,
                    help="codes kept per codebook; 64 is what is reported")
    ap.add_argument("--table", action="store_true",
                    help="fit this image's own index histogram and transmit it "
                         "in the payload, instead of coding against the frozen "
                         "prior. Sparse, so it costs ~0.0007 bpp at full "
                         "resolution -- but the reported numbers do not use it.")
    ap.add_argument("--crop", type=int, default=0,
                    help="centre-crop to NxN before encoding (0 = full image)")
    ap.add_argument("--device", default="cuda")
    ap.add_argument("--write-payload", default="", metavar="PATH",
                    help="write the compressed bytes here")
    ap.add_argument("--write-recon", default="", metavar="PATH",
                    help="write the decoded reconstruction here (PNG)")
    args = ap.parse_args()

    if not args.image and not args.decode:
        ap.error("pass --image to compress, or --decode to decode a payload")

    model, device, meta = assemble(args.rung, float_model=args.float_model,
                                   keep_n=args.keep_n, device=args.device)
    print()

    orig = load_rgb(args.image, crop=args.crop) if args.image else None

    with torch.no_grad():
        if args.decode:
            with open(args.decode, "rb") as fh:
                payload = fh.read()
            print(f"decoding {args.decode} ({len(payload)} bytes)")
            recon = to_uint8(model.decompress(payload))
            stats = None
        else:
            x = norm_from_u8(orig).to(device)
            payload, stats = model.compress_stats(x, build_table=args.table)
            recon = to_uint8(model.decompress(payload))

    h, w = recon.shape[:2]
    print(f"image: {os.path.basename(args.image or args.decode)} | {w}x{h}")
    print(f"payload: {len(payload)} bytes")

    if stats is not None:
        print(f"  bpp           {stats['bpp']:.4f}   (measured, "
              f"8 * {len(payload)} / {h * w})")
        print(f"  bpp_prior     {stats['bpp_prior']:.4f}   "
              f"(ideal coder against the frozen prior)")
        print(f"  bpp_empirical {stats['bpp_empirical']:.4f}   "
              f"(this image's own histogram -- optimistic, a decoder "
              f"cannot know it)")
        print(f"  mode          {stats['mode_name']}")

    if orig is not None:
        if orig.shape != recon.shape:
            print(f"\nnot scored: original is {orig.shape} but the "
                  f"reconstruction is {recon.shape}")
        else:
            psnr, msdb, msraw = score(orig, recon, device=device)
            print(f"  psnr          {psnr:.2f} dB")
            print(f"  ms_ssim       {msraw:.4f}  ({msdb:.2f} dB)")

    if args.write_payload:
        with open(args.write_payload, "wb") as fh:
            fh.write(payload)
        print(f"\npayload -> {args.write_payload}")
    if args.write_recon:
        from PIL import Image
        Image.fromarray(recon).save(args.write_recon)
        print(f"reconstruction -> {args.write_recon}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
