"""BPG / JPEG / JPEG 2000 / WebP anchors on full-resolution CLIC.

Every anchor is measured the honest way: encode to a real file, read its size off
disk for the rate, decode it back and score the decoded pixels. No proxy rate
models -- container and header bytes are part of what these formats cost.

Configuration, and why:

* **BPG** 4:4:4 at `BPG_M` (currently `-m 8`, bpgenc's own default). No chroma
  subsampling. `-m` is encoder EFFORT, 1=fast..9=slow, and 8 is what bpgenc
  uses when not told otherwise -- which is also what CompressAI's published
  `bpg_444_x265_ycbcr` curves were produced at, so our BPG anchor and theirs
  are the same configuration and can be cross-checked against each other.
  This repo previously swept `-m 9`; that is one notch more effort on the same
  codec, so an -m 9 curve and an -m 8 curve must never share a figure.
* **JPEG** 4:2:0, `optimize=True` (optimal Huffman tables). Standard libjpeg.
* **JPEG 2000** irreversible 9/7 wavelet, rate-targeted, so its points land on
  chosen bpp values instead of wherever a quality index happens to fall.
* **WebP** quality sweep at the default `method=4`.

Work is parallel over (image, codec, quality) jobs across processes. Each worker
scores its own reconstruction on CPU with a single torch thread -- 32 single-thread
workers beat one 32-thread MS-SSIM by a wide margin, and it avoids shuttling
decoded 2048px frames back to the parent.

Writes results/classical.json.
"""
from __future__ import annotations

import argparse
import hashlib
import io
import json
import os
import shutil
import subprocess
import tempfile
import threading
import time
from collections import defaultdict
from concurrent.futures import ProcessPoolExecutor

import numpy as np
from PIL import Image

from _common import (
    BPGDEC, BPGENC, add_dataset_arg, add_out_dir_arg, bpp_from_bytes,
    dataset_images, dataset_label, load_rgb, out_dir, score,
)

# --- operating points -------------------------------------------------------
# Chosen to bracket 0-0.5 bpp with a couple of points beyond it, so the curves are
# anchored at both ends of the plotted window instead of being extrapolated to it.
JPEG_Q = [1, 2, 3, 4, 5, 6, 8, 10, 12, 15, 20, 25, 30, 40, 50]
WEBP_Q = [0, 1, 2, 3, 5, 8, 12, 18, 25, 35, 45, 55, 65, 75]
J2K_BPP = [0.02, 0.04, 0.06, 0.08, 0.10, 0.15, 0.20, 0.25, 0.30,
           0.35, 0.40, 0.45, 0.50, 0.60, 0.70]
BPG_Q = [51, 50, 49, 48, 47, 46, 45, 44, 42, 40, 38, 36, 34, 32, 30]

# bpgenc -m: encoder effort, 1=fast .. 9=slow, default 8. Kept as a constant
# because it is the one BPG setting that has to match whatever curve we are
# comparing against, and a hardcoded literal is how that silently drifts.
BPG_M = 8

# JPEG 2000 is the one anchor addressed by target RATE rather than by a quality
# index, so its sweep has to know how many pixels that rate is spread over. On a
# 224x224 crop the JP2 container alone is ~270 bytes = 0.043 bpp, so a 0.02 bpp
# request is not merely missed, it is unreachable -- OpenJPEG emits its floor-size
# file and the result decodes at ~13 dB. Asking for rates the format cannot express
# would put a cliff on the curve that says nothing about JPEG 2000's efficiency.
J2K_BPP_SMALL = [0.06, 0.08, 0.10, 0.15, 0.20, 0.25, 0.30,
                 0.35, 0.40, 0.45, 0.50, 0.60, 0.70]
# Achieved rate may exceed the request by this much before the point is dropped as
# "the encoder could not operate here". Logged, never silent.
J2K_RATE_TOL = 1.15


def _decode_array(data, fmt):
    with Image.open(io.BytesIO(data)) as im:
        return np.asarray(im.convert("RGB"), dtype=np.uint8)


def codec_jpeg(img, q, tmpdir):
    buf = io.BytesIO()
    img.save(buf, "JPEG", quality=int(q), optimize=True, subsampling=2)
    data = buf.getvalue()
    return len(data), _decode_array(data, "JPEG")


def codec_webp(img, q, tmpdir):
    buf = io.BytesIO()
    img.save(buf, "WEBP", quality=int(q), method=4)
    data = buf.getvalue()
    return len(data), _decode_array(data, "WEBP")


def codec_jp2(img, target_bpp, tmpdir):
    """OpenJPEG via Pillow, rate-targeted.

    `quality_mode='rates'` takes a COMPRESSION RATIO, not a quality index. Raw RGB
    is 24 bpp, so the ratio for a target rate is 24/target. The achieved size is
    still measured from the file -- OpenJPEG's rate control is close but not exact.
    """
    buf = io.BytesIO()
    img.save(buf, "JPEG2000", quality_mode="rates",
             quality_layers=[24.0 / float(target_bpp)], irreversible=True)
    data = buf.getvalue()
    return len(data), _decode_array(data, "JPEG2000")


BPG_TRIES = 25


def _run_retry(cmd, tries=BPG_TRIES):
    """Run `cmd`, retrying on a crash.

    The bundled x265 in bpgenc 0.9.8 crashes nondeterministically (exit
    0xC00000FF) on roughly a third of invocations on this machine, on identical
    input and at any -m level -- so it is a race inside the encoder, not something
    the arguments can avoid. Retrying is the cheap fix: an encode is well under a
    second, so a handful of attempts costs nothing and drives the residual failure
    rate to effectively zero. Output is deterministic when it DOES succeed, so a
    retried encode is the same bitstream, not a different operating point.
    """
    last = None
    for _ in range(tries):
        r = subprocess.run(cmd, capture_output=True)
        if r.returncode == 0:
            return r
        last = r
    raise RuntimeError(f"{os.path.basename(cmd[0])} failed {tries}x, last rc="
                       f"{last.returncode} err={last.stderr.decode(errors='replace')[:200]}")


def codec_bpg(img, q, tmpdir):
    """bpgenc/bpgdec round trip through temp files (the tools have no pipe mode).

    The source PNG is written once per IMAGE and reused across that image's
    quality points, which is worth doing -- it is 15 PNG encodes of a 2048px
    frame otherwise. The cache is keyed on a hash of the pixels, not on a fixed
    `src.png`: with a fixed name the reuse is only correct while a tmpdir holds
    exactly one image, and a caller that sweeps several images through one
    tmpdir silently re-encodes the FIRST one for every later image. That is a
    wrong number, not a crash -- it only surfaced here because Kodak's portrait
    images have a different shape than its landscape ones, so `score` caught a
    mismatch that same-shape images would have passed straight through.
    """
    tag = hashlib.blake2b(img.tobytes(), digest_size=8).hexdigest()
    src = os.path.join(tmpdir, f"src_{tag}.png")
    bit = os.path.join(tmpdir, f"out_{tag}_{q}.bpg")
    dec = os.path.join(tmpdir, f"dec_{tag}_{q}.png")
    if not os.path.exists(src):
        # Written to a unique name and os.replace'd, which is atomic. Not
        # belt-and-braces: eval_perceptual.py runs eight threads over the same
        # image through one tmpdir, so a plain `img.save(src)` lets one thread
        # hand bpgenc a half-written PNG. The retry loop below would mask it as
        # a slow encode, or record it as a failed one.
        staged = f"{src}.{os.getpid()}.{threading.get_ident()}"
        img.save(staged, "PNG")
        os.replace(staged, src)
    _run_retry([BPGENC, "-q", str(int(q)), "-f", "444", "-m", str(BPG_M), "-o", bit, src])
    n_bytes = os.path.getsize(bit)
    _run_retry([BPGDEC, "-o", dec, bit])
    arr = np.asarray(Image.open(dec).convert("RGB"), dtype=np.uint8)
    for p in (bit, dec):
        try:
            os.remove(p)
        except OSError:
            pass
    return n_bytes, arr


CODECS = {
    "BPG": (codec_bpg, BPG_Q),
    "JPEG": (codec_jpeg, JPEG_Q),
    "JPEG 2000": (codec_jp2, J2K_BPP),
    "WebP": (codec_webp, WEBP_Q),
}


def _work(job):
    """One (image, codec) pair over that codec's quality points.

    Deliberately fine-grained: a job is one image and ONE codec, so a crash-retry
    storm inside BPG cannot stall the JPEG/WebP/JP2 work, and a job that does die
    takes ~15 encodes with it rather than ~60.
    """
    import torch
    torch.set_num_threads(1)

    path, cname, crop = job
    orig = load_rgb(path, crop=crop)
    H, W = orig.shape[:2]
    img = Image.fromarray(orig)
    fn, points = CODECS[cname]
    out = []
    tmpdir = tempfile.mkdtemp(prefix="rdbench_")
    try:
        for q in points:
            try:
                n_bytes, recon = fn(img, q, tmpdir)
            except Exception as e:                        # noqa: BLE001
                out.append({"codec": cname, "q": q,
                            "file": os.path.basename(path), "error": str(e)})
                continue
            # Guard rather than let a silent size mismatch poison the mean.
            if recon.shape != orig.shape:
                out.append({"codec": cname, "q": q,
                            "file": os.path.basename(path),
                            "error": f"shape {recon.shape} != {orig.shape}"})
                continue
            psnr, msd, msraw = score(orig, recon, device=None)
            out.append({
                "codec": cname, "q": q, "file": os.path.basename(path),
                "height": H, "width": W, "bytes": int(n_bytes),
                "bpp": bpp_from_bytes(n_bytes, H, W),
                "psnr": psnr, "ms_ssim_db": msd, "ms_ssim": msraw,
            })
    finally:
        shutil.rmtree(tmpdir, ignore_errors=True)
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--workers", type=int, default=max(1, (os.cpu_count() or 8) - 2))
    ap.add_argument("--limit", type=int, default=0, help="first N images only")
    ap.add_argument("--codecs", default=",".join(CODECS))
    ap.add_argument("--crop", type=int, default=0,
                    help="centre-crop to NxN before encoding (0 = full resolution)")
    ap.add_argument("--tag", default="full", help="results/classical_<tag>.json")
    add_dataset_arg(ap)
    add_out_dir_arg(ap)
    ap.add_argument("--out", default="",
                    help="explicit output path, bypassing --out-dir")
    args = ap.parse_args()

    names = [c.strip() for c in args.codecs.split(",") if c.strip()]
    bad = [c for c in names if c not in CODECS]
    if bad:
        raise SystemExit(f"unknown codec(s) {bad}; choose from {list(CODECS)}")

    if args.crop and args.crop <= 512:
        CODECS["JPEG 2000"] = (codec_jp2, J2K_BPP_SMALL)
        print(f"note: {args.crop}px crop -- JPEG 2000 sweep starts at "
              f"{J2K_BPP_SMALL[0]} bpp (container overhead makes lower "
              f"rates unreachable)\n")

    images = dataset_images(args.dataset)
    if args.limit:
        images = images[:args.limit]
    # Anchors, not run output: these do not depend on which checkpoint is
    # being evaluated, and they are the most expensive measurement in the repo.
    out_path = args.out or os.path.join(out_dir(args.out_dir),
                                        f"classical_{args.tag}.json")

    n_pts = sum(len(CODECS[c][1]) for c in names)
    where = f"{args.crop}x{args.crop} centre crop" if args.crop else "full resolution"
    print(f"{len(images)} {dataset_label(args.dataset)} images ({where}) x "
          f"{n_pts} operating points "
          f"({', '.join(names)}) = {len(images)*n_pts:,} encodes "
          f"on {args.workers} workers\n")

    jobs = [(p, c, args.crop) for p in images for c in names]
    t0, records, done = time.time(), [], 0
    with ProcessPoolExecutor(max_workers=args.workers) as ex:
        for res in ex.map(_work, jobs):
            records.extend(res)
            done += 1
            el = time.time() - t0
            nerr = sum(1 for r in records if "error" in r)
            print(f"  {done:>4}/{len(jobs)} jobs  {el:.0f}s elapsed, "
                  f"eta {el/done*(len(jobs)-done):.0f}s, {nerr} failed")

    # Aggregate: for each codec and quality point, average bpp and each metric over
    # the corpus. This is the standard RD-curve convention -- one curve point per
    # ENCODER SETTING, not per image -- so every point is measured on all 59 images.
    errors = [r for r in records if "error" in r]
    ok = [r for r in records if "error" not in r]
    agg = defaultdict(lambda: defaultdict(list))
    for r in ok:
        agg[r["codec"]][r["q"]].append(r)

    curves, dropped = {}, []
    for cname in names:
        pts = []
        for q in CODECS[cname][1]:
            rs = agg[cname].get(q, [])
            if not rs:
                continue
            m = lambda k: sum(r[k] for r in rs) / len(rs)
            pt = {"q": q, "n_images": len(rs), "bpp": m("bpp"),
                  "psnr": m("psnr"), "ms_ssim_db": m("ms_ssim_db"),
                  "ms_ssim": m("ms_ssim")}
            # JPEG 2000 is rate-targeted, so a large overshoot means the encoder
            # hit its floor rather than operating at the requested point.
            if cname == "JPEG 2000" and pt["bpp"] > J2K_RATE_TOL * float(q):
                dropped.append(f"{cname} target {q} bpp -> achieved "
                               f"{pt['bpp']:.4f} ({pt['bpp']/float(q):.2f}x), "
                               f"{pt['psnr']:.2f} dB: rate not reachable")
                continue
            pts.append(pt)
        pts.sort(key=lambda p: p["bpp"])
        curves[cname] = pts

    for d in dropped:
        print(f"dropped: {d}")
        print(f"\n{cname}:")
        for p in pts:
            print(f"   q={p['q']:<6} bpp {p['bpp']:.4f}  PSNR {p['psnr']:6.2f}  "
                  f"MS-SSIM {p['ms_ssim_db']:6.2f} dB  (n={p['n_images']})")

    if errors:
        print(f"\n{len(errors)} failed encodes:")
        for e in errors[:10]:
            print(f"   {e['codec']} q={e['q']} {e['file']}: {e['error'][:90]}")

    with open(out_path, "w", encoding="utf-8") as f:
        json.dump({"curves": curves, "n_images": len(images), "crop": args.crop,
                   "dataset": args.dataset,
                   "errors": errors, "dropped": dropped, "per_image": ok},
                  f, indent=2)
    print(f"\nwrote {out_path}  ({time.time()-t0:.0f}s total)")


if __name__ == "__main__":
    main()
