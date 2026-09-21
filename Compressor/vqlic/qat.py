"""INT8 quantization-aware training of the edge encoder.

The portable form of the `base_v3.ipynb` QAT cell. What that cell established and
this keeps:

* the **codebook is frozen** -- `quantizer.eval()`, which is what gates the EMA
  update inside `MultiCodebookEMAQuantizer.forward`, plus `requires_grad_(False)`
  on its buffers. Only the encoder (and, by default, the decoder) trains.
* `rate_beta` is restored from the fp32 run's `rate_beta_final`. It is a plain
  attribute, absent from the state_dict, and `build_model` hardcodes 0.0 -- so
  skipping this changes the assignment rule the codebook was fitted under.
* the eval runs the INT8 encoder and the quantizer on CPU with the decoder on
  GPU, because fbgemm has no CUDA kernels and moving a converted module to CUDA
  segfaults rather than raising.

What is new here:

* **the codebook is quantized to INT8 too**, so the assignment search is an
  integer dot product against an integer table -- the encoder side becomes
  integer end to end rather than an INT8 encoder feeding an fp32 nearest-
  neighbour search. See `quantizer.quantize_codebook` and `set_latent_quant` for
  the arithmetic, and `verify_int_search` for the end-of-run proof that the
  integer path picks the same codes as the float one.
* **crop-only training at 448** by default, so the decoder's 14-token attention
  windows see a 4x4 grid at native pixel scale instead of 224's 2x2. Full-res
  CLIC and Kodak are the deployment target and they are many windows wide.
* **the frozen prior is re-estimated once at the end**. QAT moves the encoder, so
  the indices it produces are not the ones the fp32 run's EMA prior was measured
  from -- and `prior_cross_entropy_bpp`, the publishable rate, is exactly the
  cross-entropy against that prior. Centroids never move; only the prior does.
* **observers and BN stats freeze at 90% of the budget**, so the encoder
  converges against final quantization params -- and so the int32 assignment
  bias, which is a function of the encoder's output scale and zero point, can be
  computed from values that have stopped moving.
* an images-seen budget with a cosine LR schedule over it, jsonl logging and
  curves shared with the fp32 trainer, and resumable checkpoints.

Ordering at the end of a run is load-bearing and goes: freeze observers -> train
out the remaining steps -> re-estimate the prior -> compute the int32 bias (it
depends on `log p`, so it must come after the prior) -> verify the integer search
-> save. `finalize` does exactly that, in that order.
"""
from __future__ import annotations

import copy
import json
import os
import random
import time

import numpy as np
import torch
import torch.nn.functional as F
from torch.ao.quantization import (
    disable_observer, get_default_qat_qconfig_mapping,
)
from torch.ao.quantization.fake_quantize import FakeQuantizeBase
from torch.ao.quantization.quantize_fx import convert_fx, prepare_qat_fx
from torch.optim import AdamW

try:                                                          # pragma: no cover
    from torch.ao.quantization.quantize_fx import fuse_fx
except ImportError:                                           # pragma: no cover
    from torch.ao.quantization.fx.fuse import fuse_fx

from vqlic.checkpoint import (
    apply_qat_payload, load_qat_payload, save_qat_checkpoint,
)
from vqlic.codec import DOWNSAMPLE, build_model, pad_to_multiple, unpad
from vqlic.config import CHECKPOINT_SHAPE_FIELDS, Config
from vqlic.engine import fmt_hms, make_lr_lambda
from vqlic.losses import RateDistortionLoss
from vqlic.metrics import (
    ms_ssim_db, prior_cross_entropy_bpp, psnr_uint8, shannon_bpp_multi, to_uint8,
)
from vqlic.plots import plot_log
from vqlic.quantizer import format_util

IMAGE_EXTS = (".png", ".jpg", ".jpeg", ".webp", ".bmp")


def usage_stats(usage):
    """`[G,K]` assignment counts -> the dict `vqlic.quantizer.format_util` renders.

    QAT has to count these itself. `MultiCodebookEMAQuantizer` maintains
    `code_usage` only `if self.track_usage and self.training`, and the quantizer
    is in `.eval()` for the whole run -- that is what freezes the codebook. So
    `quick_util()` and `utilization_stats()` read a histogram that never
    advances, and reporting them here would print a flat 0% active for a codebook
    that is in fact being used normally: a collapse warning about nothing.
    """
    usage = usage.detach().cpu()
    per = []
    for g in range(usage.shape[0]):
        u = usage[g]
        k = int(u.numel())
        active = int((u > 0).sum())
        tot = float(u.sum())
        if tot > 0:
            pr = u / tot
            ppl = float(torch.exp(-(pr * torch.log(pr + 1e-12)).sum()))
        else:
            ppl = 0.0
        per.append({"codebook": g, "active": active, "dead": k - active,
                    "util_pct": 100.0 * active / k,
                    "perplexity": ppl, "ppl_norm": ppl / k})
    total = int(usage.numel())
    active_all = sum(p["active"] for p in per)
    return {"per_codebook": per,
            "overall": {"active": active_all, "total": total,
                        "util_pct": 100.0 * active_all / total,
                        "mean_perplexity": float(np.mean(
                            [p["perplexity"] for p in per]))}}


# --------------------------------------------------------------------- config
def config_for_qat(args, ckpt_config):
    """Resolve the run config: CLI, with the model shape from the checkpoint.

    QAT continues a trained model, so the fields in `CHECKPOINT_SHAPE_FIELDS` are
    taken from that checkpoint unconditionally -- `build_model` has to reproduce
    the module tree `load_state_dict(strict=True)` expects, and a `--num-layers`
    on the command line that disagreed would be a confusing crash at best. A note
    is printed for any field where the command line asked for something else, so
    the override is visible rather than silent.
    """
    from vqlic.config import config_from_args

    cfg = config_from_args(args)
    overridden = []
    for name in CHECKPOINT_SHAPE_FIELDS:
        if name not in ckpt_config:
            continue
        want, have = ckpt_config[name], getattr(cfg, name)
        if want != have:
            overridden.append(f"{name}: {have} -> {want}")
        setattr(cfg, name, want)
    if overridden:
        print("model shape taken from the checkpoint, not the command line:")
        for line in overridden:
            print(f"    {line}")
    # __post_init__ validated the CLI values; re-run it on the merged ones.
    cfg.__post_init__()
    return cfg


# ------------------------------------------------------------------- assembly
def load_fp32_codec(path):
    """The fp32 `vqlic` checkpoint -> (model, config dict, provenance dict)."""
    ck = torch.load(path, map_location="cpu", weights_only=False)
    if "model" not in ck or "config" not in ck:
        raise SystemExit(
            f"{path} is not a vqlic checkpoint (needs 'model' and 'config'). The "
            f"base_v3 full-pickle checkpoints are not supported here -- see "
            f"the base_v3 lineage; this loader does not handle those.")
    cfg = Config.from_dict(ck["config"])
    model = build_model(cfg).eval()
    model.load_state_dict(ck["model"], strict=True)
    # build_model hardcodes rate_beta=0.0 because the fp32 trainer ramps it, and
    # rate_beta is a plain attribute rather than a buffer, so it is NOT in the
    # state_dict. Restoring the run's end value is what keeps the ECVQ assignment
    # rule the same as the one this codebook and decoder were trained under.
    model.quantizer.rate_beta = cfg.rate_beta_final
    return model, ck["config"], {
        "path": path, "step": ck.get("step"), "epoch": ck.get("epoch"),
        "rate_beta": model.quantizer.rate_beta,
    }


def prepare_encoder(encoder_fp32, image_size, backend="fbgemm"):
    """fp32 encoder -> FX graph module with fake-quant observers inserted."""
    enc = copy.deepcopy(encoder_fp32).eval()
    try:
        enc = fuse_fx(enc)
    except Exception as exc:                                  # pragma: no cover
        # Not fatal: without fusion, conv and BN are observed separately, which
        # costs a little accuracy but still trains and still converts.
        print(f"note: fuse_fx failed ({exc}); continuing unfused")
    example = torch.randn(1, 3, image_size, image_size)
    return prepare_qat_fx(enc, get_default_qat_qconfig_mapping(backend), example)


def freeze_codebook(model, train_decoder=True):
    """Freeze the quantizer; leave encoder (and optionally decoder) trainable.

    `.eval()` on the quantizer is the operative line, and it is easy to
    misread as cosmetic: `MultiCodebookEMAQuantizer` runs `_ema_update` and its
    dead-code resampling from inside `forward`, gated on `self.training` and NOT
    on `torch.no_grad()`. Leaving it in train mode would keep moving the
    centroids -- off the integer lattice, and out from under the decoder.
    """
    for p in model.quantizer.parameters():
        p.requires_grad_(False)
    model.quantizer.eval()
    for p in model.decoder.parameters():
        p.requires_grad_(train_decoder)
    return model


def output_fake_quant(prepared):
    """The FakeQuantize module feeding the prepared graph's output.

    This is where the encoder's output scale and zero point live, and the int32
    assignment bias is a function of both. Found by following the graph's output
    node back one step rather than by name, since the
    `activation_post_process_N` numbering depends on the graph.
    """
    mods = dict(prepared.named_modules())
    outputs = [n for n in prepared.graph.nodes if n.op == "output"]
    if outputs:
        src = outputs[-1].args[0]
        while isinstance(src, (tuple, list)) and src:
            src = src[0]
        if getattr(src, "op", "") == "call_module":
            m = mods.get(str(src.target))
            if isinstance(m, FakeQuantizeBase):
                return m
    # Fall back to the last fake-quant in graph order.
    last = None
    for n in prepared.graph.nodes:
        if n.op == "call_module":
            m = mods.get(str(n.target))
            if isinstance(m, FakeQuantizeBase):
                last = m
    if last is None:
        raise RuntimeError(
            "the prepared encoder has no output fake-quant, so the latent "
            "quantization params the integer assignment search needs cannot be "
            "read. Was prepare_qat_fx given a qconfig mapping?")
    return last


def latent_quant_params(prepared):
    """(scale, zero_point) of the encoder's INT8 output."""
    scale, zp = output_fake_quant(prepared).calculate_qparams()
    return float(scale.reshape(-1)[0]), int(zp.reshape(-1)[0])


def freeze_quant_params(prepared):
    """Stop the observers and the BN running stats.

    Two effects, both wanted at the same point in the run: the encoder spends its
    last steps training against the exact quantization grid it will be deployed
    with, and the output scale/zero point stop moving so the int32 assignment
    bias can be computed from them.
    """
    from torch.ao.nn.intrinsic.qat import freeze_bn_stats

    prepared.apply(disable_observer)
    prepared.apply(freeze_bn_stats)
    s, z = latent_quant_params(prepared)
    return {"latent_scale": s, "latent_zero_point": z}


def convert_int8_encoder(prepared):
    """A real INT8 encoder module, from a COPY, leaving `prepared` untouched.

    CPU-only by necessity, and the copy is what lets the eval run mid-training
    without disturbing the weights the optimizer holds.
    """
    return convert_fx(copy.deepcopy(prepared).cpu().eval()).eval()


# ----------------------------------------------------------------- evaluation
def eval_image_paths(root, limit=0):
    """Sorted absolute paths of the eval corpus.

    Sorted so `--qat-eval-images N` means the same N images at every checkpoint
    and the jsonl rows of one run line up with the next.
    """
    if not root:
        return []
    if not os.path.isdir(root):
        raise SystemExit(f"--qat-eval-dir does not exist: {root}")
    files = sorted(f for f in os.listdir(root)
                   if f.lower().endswith(IMAGE_EXTS))
    if limit:
        files = files[:limit]
    return [os.path.join(root, f) for f in files]


def load_image(path):
    """One image -> `[1,3,H,W]` float in [-1,1], at native resolution.

    Matches `vqlic.dataset`'s normalization, which has to agree with the decoder's
    Tanh and the loss's `data_range=2.0`.
    """
    from PIL import Image

    with Image.open(path) as im:
        arr = np.asarray(im.convert("RGB"), dtype=np.float32) / 255.0
    t = torch.from_numpy(arr).permute(2, 0, 1).unsqueeze(0)
    return t * 2.0 - 1.0


@torch.no_grad()
def evaluate(model, paths, decoder_device="cuda", int8=True):
    """Score the CURRENT weights on full-resolution images.

    Runs the same round trip `scripts/eval.py` runs -- reflect-pad to a
    multiple of the /8 downsample, encode, quantize, decode, unpad -- and reports
    both rate figures:

    * `bpp_prior` is `prior_cross_entropy_bpp`, the cross-entropy of the realised
      indices against the frozen prior. That is what an entropy coder actually
      pays and the number to publish.
    * `bpp_empirical` is each image's own histogram entropy. Optimistic, since a
      decoder cannot know the histogram of an image it has not received; kept
      because it is what the notebook cell logged.

    The live model is left exactly as found -- device placement included -- so a
    mid-training call cannot perturb the run.
    """
    if not paths:
        return None
    was_training = model.training
    quant, dec = model.quantizer, model.decoder
    quant_dev = quant.embed.device
    dec_dev = next(dec.parameters()).device
    gpu = torch.device(decoder_device if torch.cuda.is_available() else "cpu")

    if int8:
        encoder = convert_int8_encoder(model.encoder)
    else:
        encoder = copy.deepcopy(model.encoder).cpu().eval()
    quant.cpu().eval()
    dec.to(gpu).eval()

    rows = []
    try:
        for path in paths:
            image = load_image(path)
            padded, (ph, pw) = pad_to_multiple(image, multiple=DOWNSAMPLE)
            latents = encoder(padded)
            quantized, _, indices, _ = quant(latents)
            recon = unpad(dec(quantized.to(gpu)).cpu(), ph, pw)

            a, b = to_uint8(image), to_uint8(recon)
            npix = padded.shape[2] * padded.shape[3]
            row = {
                "image": os.path.basename(path),
                "psnr": psnr_uint8(a, b),
                "bpp_prior": prior_cross_entropy_bpp(quant, indices, npix),
                "bpp_empirical": shannon_bpp_multi(indices, npix),
            }
            # 5-scale MS-SSIM needs >= 161 px per side.
            if min(a.shape[:2]) >= 161:
                row["ms_ssim_db"], row["ms_ssim"] = ms_ssim_db(a, b)
            rows.append(row)
    finally:
        # Restore precisely the state the training loop expects, whatever
        # happened above.
        quant.to(quant_dev)
        dec.to(dec_dev)
        if was_training:
            model.train()
            dec.train(any(p.requires_grad for p in dec.parameters()))
            quant.eval()

    def mean(key):
        vals = [r[key] for r in rows if key in r and np.isfinite(r[key])]
        return float(np.mean(vals)) if vals else None

    return {"n_images": len(rows), "int8": bool(int8),
            "psnr": mean("psnr"), "ms_ssim_db": mean("ms_ssim_db"),
            "bpp_prior": mean("bpp_prior"),
            "bpp_empirical": mean("bpp_empirical"),
            "per_image": rows}


# ------------------------------------------------------------ prior / verify
@torch.no_grad()
def reestimate_prior(model, loader, target_images, device, floor=1.0):
    """Recount the index histogram with the codebook fixed, then install it.

    Uses the fake-quant encoder on `device` rather than a converted INT8 encoder
    on CPU. The two agree on the assignment of all but a vanishing fraction of
    tokens -- fake-quant applies the same quantization grid to weights and
    activations and differs only in accumulating the convolution in fp32 instead
    of exact int32 -- and the CPU path would be ~50x slower for a pass whose
    whole purpose is to average over tens of millions of tokens.
    """
    was_training = model.training
    model.eval()
    q = model.quantizer
    counts = torch.zeros(q.G, q.K, dtype=torch.float64)
    seen = 0
    t0 = time.time()
    try:
        for image in loader:
            if image is None:
                continue
            image = image.to(device, non_blocking=True)
            _, _, indices, _ = model(image)
            idx = indices.detach().cpu()
            for g in range(q.G):
                counts[g] += torch.bincount(idx[..., g].reshape(-1),
                                            minlength=q.K).to(torch.float64)
            seen += image.shape[0]
            if seen >= target_images:
                break
    finally:
        if was_training:
            model.train()
            model.decoder.train(any(p.requires_grad
                                    for p in model.decoder.parameters()))
            q.eval()

    stats = q.set_prior_counts(counts, floor=floor)
    stats["images"] = seen
    stats["seconds"] = time.time() - t0
    return stats


@torch.no_grad()
def verify_int_search(model, image, device):
    """Assert the integer search picks exactly what the float search picks.

    The end-of-run proof that the deployed encoder-side argmin is faithful, run
    on real latents rather than the synthetic ones `tests/test_qat.py` uses.
    Cheap, and the one check that would catch a bias computed from stale
    quantization params -- which is the specific mistake the freeze-then-finalize
    ordering exists to prevent.
    """
    q = model.quantizer
    if not q.cb_quantized or q.cb_bias is None:
        return None
    was_training = model.training
    model.eval()
    try:
        latents = model.encoder(image.to(device)).float().cpu()
        _, _, idx_float, _ = q.cpu()(latents)
        idx_int = q.int_search(q.quantize_latents(latents))
        agree = int((idx_float == idx_int).sum())
        total = int(idx_float.numel())
    finally:
        q.to(device)
        if was_training:
            model.train()
            model.decoder.train(any(p.requires_grad
                                    for p in model.decoder.parameters()))
            q.eval()
    return {"tokens": total, "agree": agree,
            "mismatch_frac": (total - agree) / max(total, 1)}


# ------------------------------------------------------------------ the loop
def train_qat(cfg, model, train_loader, prior_loader=None):
    """Run QAT. `model.encoder` must already be the FX-prepared encoder."""
    device = torch.device(cfg.device if torch.cuda.is_available()
                          or cfg.device == "cpu" else "cpu")
    if str(device) != cfg.device:
        print(f"note: requested device {cfg.device!r} unavailable, using {device}")
    if device.type == "cuda":
        torch.backends.cudnn.benchmark = True
    # fbgemm/qnnpack kernels are what the converted encoder runs on at eval time.
    torch.backends.quantized.engine = cfg.qat_backend

    model.to(device)
    freeze_codebook(model, train_decoder=cfg.qat_train_decoder)

    criterion = RateDistortionLoss(
        lambda_rate=cfg.lambda_rate, beta_commit=cfg.beta_commit, alpha=cfg.alpha,
        data_range=2.0, channels=3, ms_ssim=cfg.ms_ssim_loss).to(device)

    trainable = [p for p in model.parameters() if p.requires_grad]
    optimizer = AdamW(trainable, lr=cfg.lr, weight_decay=cfg.weight_decay)

    steps_per_epoch = max(len(train_loader), 1)
    if cfg.qat_max_images:
        total_steps = max(1, cfg.qat_max_images // cfg.batch_size)
    else:
        total_steps = cfg.epochs * steps_per_epoch
    budget_images = total_steps * cfg.batch_size
    # 2% of the budget, unless asked otherwise. A cosine that starts at full LR
    # on a fake-quant graph whose observers have only just been initialized is
    # the one place this schedule bites.
    warmup = cfg.lr_warmup or max(1, int(0.02 * total_steps))
    scheduler = torch.optim.lr_scheduler.LambdaLR(
        optimizer, make_lr_lambda(total_steps, warmup, cfg.lr, cfg.eta_min))

    # ------------------------------------------------------------ INT8 codebook
    cb_stats = None
    if cfg.qat_quantize_codebook:
        cb_stats = model.quantizer.quantize_codebook(cfg.qat_codebook_bits)

    # ------------------------------------------------------------------- resume
    images_seen, epoch, global_step = 0, 0, 0
    observers_frozen = False
    resume = cfg.qat_resume
    if resume == "auto":
        cand = cfg.file("qat_last.pt")
        resume = cand if os.path.exists(cand) else ""
    if resume:
        if not os.path.exists(resume):
            raise SystemExit(f"--qat-resume {resume} not found")
        payload = load_qat_payload(resume, map_location="cpu")
        info = apply_qat_payload(payload, model)
        model.to(device)
        images_seen = int(payload.get("images_seen", 0))
        epoch = int(payload.get("epoch", 0))
        global_step = int(payload.get("step", images_seen // cfg.batch_size))
        for key, obj in (("optimizer_state_dict", optimizer),
                         ("scheduler_state_dict", scheduler)):
            if payload.get(key) is not None:
                try:
                    obj.load_state_dict(payload[key])
                except Exception as exc:
                    print(f"  could not restore {key}: {exc}")
        print(f"resumed {resume} at {images_seen:,} images, epoch {epoch}: "
              f"encoder {len(info['encoder_missing'])} missing / "
              f"{len(info['encoder_unexpected'])} unexpected, decoder "
              f"{'yes' if info['decoder'] else 'no'}, quantizer "
              f"{'yes' if info['quantizer'] else 'no'}, codebook "
              f"{'INT8' if info['codebook_quantized'] else 'fp32'}")
        # A resumed run may already be past the freeze point.
        # The encoder state_dict carries each fake-quant's `observer_enabled`
        # flag, so a checkpoint saved past the freeze point restores already
        # disabled -- but BN's `freeze_bn_stats` is a module swap, not a buffer,
        # so it has to be reapplied. Recording it here also stops the training
        # loop from re-announcing a freeze that already happened.
        if payload.get("observers_frozen"):
            qp = freeze_quant_params(model.encoder)
            observers_frozen = True
            print(f"  observers were already frozen in this checkpoint "
                  f"(latent scale {qp['latent_scale']:.6g}, zero point "
                  f"{qp['latent_zero_point']})")

    os.makedirs(cfg.out_dir, exist_ok=True)
    cfg.save(cfg.file("qat_config.json"))
    log_path = cfg.file("qat_log.jsonl")
    curve_path = cfg.file("qat_curves.png")
    eval_paths = eval_image_paths(cfg.qat_eval_dir, cfg.qat_eval_images)

    freeze_at = (int(cfg.qat_freeze_observer_frac * budget_images)
                 if cfg.qat_freeze_observer_frac < 1.0 else None)

    print(f"\nQAT run {cfg.name!r} -> {cfg.out_dir}")
    print(f"  budget      {budget_images:,} images "
          f"({total_steps:,} steps at batch {cfg.batch_size}), device {device}")
    print(f"  data        {cfg.image_size}px "
          f"{'crop-only' if cfg.crop_only else 'crop + RandomResizedCrop'}, "
          f"token grid {cfg.image_size // DOWNSAMPLE} "
          f"= {cfg.image_size // DOWNSAMPLE // max(cfg.window_size, 1)}x"
          f"{cfg.image_size // DOWNSAMPLE // max(cfg.window_size, 1)} "
          f"windows of {cfg.window_size}")
    print(f"  trainable   encoder"
          + (" + decoder" if cfg.qat_train_decoder else " only")
          + f" ({sum(p.numel() for p in trainable):,} params); "
            f"codebook FROZEN")
    if cb_stats:
        print(f"  codebook    INT8 ({cb_stats['bits']} bit, per-group scale "
              f"{[round(v, 6) for v in cb_stats['scale']]}), "
              f"relative squared error {cb_stats['rel_sq_err']:.3e}")
    else:
        print("  codebook    fp32 (--no-qat-quantize-codebook)")
    print(f"  lr          {cfg.lr:.2e} cosine to {cfg.eta_min:.1e}, "
          f"warmup {warmup:,} steps")
    print(f"  observers   freeze at "
          + (f"{freeze_at:,} images ({100*cfg.qat_freeze_observer_frac:.0f}%)"
             if freeze_at else "never")
          + f" | rate_beta {model.quantizer.rate_beta}")
    print(f"  eval        {len(eval_paths)} full-res images from "
          f"{cfg.qat_eval_dir or '(none)'}, decoder on "
          f"{cfg.qat_eval_decoder_device}\n")

    def log(rec):
        with open(log_path, "a", encoding="utf-8") as f:
            f.write(json.dumps(rec) + "\n")

    def checkpoint(tag, run_eval=True):
        path = cfg.file(f"qat_resume_{tag}.pt")
        save_qat_checkpoint(path, model, optimizer, scheduler,
                            images_seen=images_seen, epoch=epoch,
                            step=global_step, cfg=cfg,
                            extra={"observers_frozen": observers_frozen})
        save_qat_checkpoint(cfg.file("qat_last.pt"), model, optimizer, scheduler,
                            images_seen=images_seen, epoch=epoch,
                            step=global_step, cfg=cfg,
                            extra={"observers_frozen": observers_frozen})
        print(f"  saved -> {path}")
        if run_eval and eval_paths:
            m = evaluate(model, eval_paths,
                         decoder_device=cfg.qat_eval_decoder_device)
            print(f"  [eval {tag}] PSNR {m['psnr']:.3f} dB | MS-SSIM "
                  f"{(m['ms_ssim_db'] or float('nan')):.2f} dB | bpp "
                  f"{m['bpp_prior']:.4f} (prior) / {m['bpp_empirical']:.4f} "
                  f"(empirical) | n={m['n_images']}")
            log({"name": cfg.name, "step": global_step, "epoch": epoch,
                 "images_seen": images_seen,
                 "val": {k: v for k, v in m.items() if k != "per_image"}})
        return path

    next_ckpt = ((images_seen // cfg.qat_ckpt_every_images) + 1) \
        * cfg.qat_ckpt_every_images

    model.train()
    model.quantizer.eval()
    if not cfg.qat_train_decoder:
        model.decoder.eval()

    run = {"loss": 0.0, "dist": 0.0, "bpp": 0.0, "n": 0}
    t_run = t_win = time.time()
    stop = False
    usage = torch.zeros(model.quantizer.G, model.quantizer.K, device=device)

    while not stop:
        usage.zero_()
        epoch_seen = 0
        for image in train_loader:
            if image is None:
                continue
            image = image.to(device, non_blocking=True)

            optimizer.zero_grad(set_to_none=True)
            recon, vq_commit, indices, bits_per_token = model(image)
            loss, distortion, _ = criterion(recon, image, vq_commit,
                                            bits_per_token)
            loss.backward()
            if cfg.grad_clip:
                torch.nn.utils.clip_grad_norm_(trainable, cfg.grad_clip)
            optimizer.step()
            scheduler.step()

            B, _, H, W = image.shape
            raw_bpp = shannon_bpp_multi(indices, B * H * W)
            for g in range(model.quantizer.G):
                usage[g] += torch.bincount(indices[..., g].reshape(-1),
                                           minlength=model.quantizer.K)
            images_seen += B
            epoch_seen += B
            global_step += 1
            run["loss"] += float(loss)
            run["dist"] += float(distortion)
            run["bpp"] += raw_bpp
            run["n"] += 1

            if freeze_at is not None and not observers_frozen \
                    and images_seen >= freeze_at:
                qp = freeze_quant_params(model.encoder)
                observers_frozen = True
                print(f"  observers + BN frozen at {images_seen:,} images: "
                      f"latent scale {qp['latent_scale']:.6g}, zero point "
                      f"{qp['latent_zero_point']}")
                log({"name": cfg.name, "step": global_step, "epoch": epoch,
                     "images_seen": images_seen, "event": "freeze_observers",
                     **qp})

            if cfg.log_every and global_step % cfg.log_every == 0:
                n = max(run["n"], 1)
                elapsed = time.time() - t_run
                rec = {
                    "name": cfg.name, "step": global_step, "epoch": epoch,
                    "images_seen": images_seen,
                    "loss": run["loss"] / n, "dist": run["dist"] / n,
                    "bpp": run["bpp"] / n,
                    "util": float((usage > 0).float().sum()
                                  / usage.numel()),
                    "rate_beta": model.quantizer.rate_beta,
                    "rate_pressure": model.quantizer.rate_pressure(),
                    "lr": scheduler.get_last_lr()[0],
                    "imgs_per_s": (n * cfg.batch_size)
                                  / max(time.time() - t_win, 1e-9),
                    "elapsed_s": elapsed,
                    "eta_s": (elapsed / max(images_seen, 1)
                              * max(budget_images - images_seen, 0)),
                }
                print(f"[{cfg.name}] {images_seen:>9,}/{budget_images:,} img "
                      f"| L {rec['loss']:.4f} | D {rec['dist']:.4f} "
                      f"| bpp {rec['bpp']:.4f} | util {100*rec['util']:.0f}% "
                      f"| lr {rec['lr']:.2e} | {rec['imgs_per_s']:.0f} img/s "
                      f"| eta {fmt_hms(rec['eta_s'])}")
                log(rec)
                run = {"loss": 0.0, "dist": 0.0, "bpp": 0.0, "n": 0}
                t_win = time.time()

            if cfg.plot_every and global_step % cfg.plot_every == 0:
                plot_log(log_path, curve_path, title=cfg.name)

            while images_seen >= next_ckpt:
                checkpoint(f"{next_ckpt}")
                next_ckpt += cfg.qat_ckpt_every_images

            if images_seen >= budget_images:
                stop = True
                break

        if epoch_seen == 0:
            raise SystemExit("the training loader yielded nothing -- check "
                             "--train-dir and --limit-images")

        epoch += 1
        stats = usage_stats(usage)
        print(f"\nepoch {epoch} {'ended (budget reached)' if stop else 'done'}"
              f" ({epoch_seen:,} images, {images_seen:,} total)")
        print("codebook utilization (frozen codebook, observed assignments):")
        print(format_util(stats))
        if stats["overall"]["util_pct"] < cfg.collapse_warn * 100:
            print(f"  WARNING only {stats['overall']['util_pct']:.1f}% of the "
                  f"codebook is being addressed. The codebook is frozen, so "
                  f"this is the ENCODER having drifted onto a few codes -- "
                  f"check --lr and --qat-codebook-bits.")
        log({"name": cfg.name, "step": global_step, "epoch": epoch,
             "images_seen": images_seen, "event": "epoch_end",
             "util_pct": stats["overall"]["util_pct"],
             "mean_perplexity": stats["overall"]["mean_perplexity"]})
        # `finalize` writes the final checkpoint and runs the final eval, so an
        # epoch-end save here would only duplicate them.
        if not stop:
            checkpoint(f"epoch_{epoch:02d}")
        print()

    finalize(cfg, model, prior_loader or train_loader, device,
             observers_frozen=observers_frozen, log=log,
             images_seen=images_seen, epoch=epoch, step=global_step,
             optimizer=optimizer, scheduler=scheduler, eval_paths=eval_paths)

    if cfg.plot_every:
        plot_log(log_path, curve_path, title=cfg.name)
    print(f"\nQAT done: {images_seen:,} images in "
          f"{fmt_hms(time.time() - t_run)}")
    return model


def finalize(cfg, model, prior_loader, device, observers_frozen, log,
             images_seen, epoch, step, optimizer=None, scheduler=None,
             eval_paths=()):
    """Freeze -> re-estimate prior -> fuse the int32 bias -> verify -> save.

    The order is the point. The bias depends on `log p` AND on the encoder's
    output scale, so it has to be computed after both have settled; verifying the
    integer search afterwards is what turns "the arithmetic should be equivalent"
    into a measurement.
    """
    print("\n--- finalizing ---")
    if not observers_frozen:
        qp = freeze_quant_params(model.encoder)
        print(f"observers + BN frozen: latent scale {qp['latent_scale']:.6g}, "
              f"zero point {qp['latent_zero_point']}")

    if cfg.qat_prior_images:
        st = reestimate_prior(model, prior_loader, cfg.qat_prior_images, device,
                              floor=cfg.qat_prior_floor)
        print(f"prior re-estimated over {st['images']:,} images "
              f"({st['tokens']:,} tokens, {fmt_hms(st['seconds'])}): entropy "
              f"{st['entropy_before']:.3f} -> {st['entropy_after']:.3f} "
              f"bits/token, live codes {st['live_before']} -> "
              f"{st['live_after']}")
        log({"name": cfg.name, "step": step, "images_seen": images_seen,
             "event": "reestimate_prior", **st})
    else:
        print("prior kept as the fp32 run left it (--qat-prior-images 0)")

    if model.quantizer.cb_quantized:
        s_z, zp = latent_quant_params(model.encoder)
        info = model.quantizer.set_latent_quant(s_z, zp)
        print(f"int32 assignment bias built from s_z={info['latent_scale']:.6g}, "
              f"zp={info['latent_zero_point']} "
              f"({info['bias_frac_bits']} fractional bits, |bias| max "
              f"{info['bias_absmax']:,}, int32 headroom "
              f"{info['int32_headroom']:.0f}x)")
        log({"name": cfg.name, "step": step, "images_seen": images_seen,
             "event": "latent_quant", **info})

        probe = next((im for im in prior_loader if im is not None), None)
        if probe is not None:
            v = verify_int_search(model, probe[:2], device)
            if v:
                print(f"integer search vs float search: {v['agree']:,}/"
                      f"{v['tokens']:,} tokens agree "
                      f"(mismatch {100 * v['mismatch_frac']:.4f}%)")
                log({"name": cfg.name, "step": step,
                     "images_seen": images_seen,
                     "event": "verify_int_search", **v})
                if v["mismatch_frac"] > 1e-3:
                    print("  WARNING integer and float assignment disagree on "
                          "more than 0.1% of tokens. The deployed argmin would "
                          "not reproduce these results -- check that the "
                          "observers were frozen before the bias was built.")

    path = save_qat_checkpoint(
        cfg.file("qat_final.pt"), model, optimizer, scheduler,
        images_seen=images_seen, epoch=epoch, step=step, cfg=cfg,
        extra={"observers_frozen": True, "prior_reestimated":
               bool(cfg.qat_prior_images)})
    save_qat_checkpoint(
        cfg.file("qat_last.pt"), model, optimizer, scheduler,
        images_seen=images_seen, epoch=epoch, step=step, cfg=cfg,
        extra={"observers_frozen": True, "prior_reestimated":
               bool(cfg.qat_prior_images)})
    print(f"final checkpoint -> {path}")

    if eval_paths:
        m = evaluate(model, eval_paths,
                     decoder_device=cfg.qat_eval_decoder_device)
        print(f"[eval final] PSNR {m['psnr']:.3f} dB | MS-SSIM "
              f"{(m['ms_ssim_db'] or float('nan')):.2f} dB | bpp "
              f"{m['bpp_prior']:.4f} (prior) / {m['bpp_empirical']:.4f} "
              f"(empirical) | n={m['n_images']}")
        log({"name": cfg.name, "step": step, "epoch": epoch,
             "images_seen": images_seen,
             "val": {k: v for k, v in m.items() if k != "per_image"}})
    return path


# -------------------------------------------------------------------- driver
def main(argv=None):
    from vqlic.config import build_qat_argparser
    from vqlic.dataset import build_trainloader
    from vqlic.metrics import model_size_table

    args = build_qat_argparser(argv).parse_args(argv)

    if args.list_presets:
        from vqlic import presets
        print(presets.describe())
        return 0

    if not args.qat_from:
        print("\nerror: --qat-from is required (an fp32 vqlic checkpoint).\n",
              flush=True)
        return 2

    codec, ckpt_cfg, prov = load_fp32_codec(args.qat_from)
    cfg = config_for_qat(args, ckpt_cfg)

    if args.print_config:
        print(cfg.summary())
        return 0

    if not cfg.train_dir:
        print("\nerror: --train-dir is required (or set NIC_TRAIN_DIR).\n"
              "       run with --print-config to inspect settings without data.")
        return 2

    random.seed(cfg.seed)
    np.random.seed(cfg.seed)
    torch.manual_seed(cfg.seed)
    torch.cuda.manual_seed_all(cfg.seed)
    torch.set_float32_matmul_precision("high")

    print(f"\nloaded {prov['path']} | step {prov['step']} | epoch "
          f"{prov['epoch']} | rate_beta {prov['rate_beta']}")
    print(model_size_table(codec))

    model = codec
    model.encoder = prepare_encoder(codec.encoder, cfg.image_size,
                                    backend=cfg.qat_backend)

    train_loader = build_trainloader(
        cfg.train_dir, batch_size=cfg.batch_size, image_size=cfg.image_size,
        num_workers=cfg.num_workers, shuffle=cfg.shuffle, seed=cfg.seed,
        scales=cfg.scale_sizes(), file_list=cfg.file_list,
        limit=cfg.limit_images, crop_only=cfg.crop_only)

    train_qat(cfg, model, train_loader)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
