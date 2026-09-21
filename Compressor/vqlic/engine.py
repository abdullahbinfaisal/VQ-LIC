"""Training and validation loops.

Step-based, so `--max-steps`, resume and the LR horizon all agree, with per-epoch
summaries on top.

Everything a run emits is named after `cfg.name` and lands in `cfg.out_dir`:

    <name>_config.json   <name>_log.jsonl      <name>_curves.png
    <name>_last.pt       <name>_epoch_001.pt   <name>_final.pt
    previews/<name>_step_00010000.png
"""
from __future__ import annotations

import json
import math
import os
import random
import time

import numpy as np
import torch
import torch.nn.functional as F
from torch.optim import AdamW

from vqlic.checkpoint import load_checkpoint, save_checkpoint
from vqlic.codec import DOWNSAMPLE
from vqlic.losses import RateDistortionLoss
from vqlic.metrics import (
    ms_ssim_db, prior_cross_entropy_bpp, psnr_uint8, shannon_bpp_multi, to_uint8,
)
from vqlic.plots import plot_log
from vqlic.quantizer import format_util


# --------------------------------------------------------------------- schedule
def make_lr_lambda(total_steps, warmup_steps, lr, eta_min):
    """Closed-form cosine with optional linear warmup, as a LambdaLR multiplier.

    Closed-form on purpose. `CosineAnnealingLR` is recursive and serializes
    `T_max` into its state_dict, so resuming into a scheduler built for a
    different horizon silently reinstates the old one -- and once the step count
    passes `T_max` the cosine wraps and the LR climbs back toward its peak. A
    LambdaLR that reads only the step index cannot do that.
    """
    total_steps = max(int(total_steps), 1)
    floor = eta_min / lr if lr > 0 else 0.0

    def fn(step):
        if warmup_steps and step < warmup_steps:
            return (step + 1) / float(warmup_steps)
        t = min(max(step - warmup_steps, 0) / max(total_steps - warmup_steps, 1), 1.0)
        return floor + (1.0 - floor) * 0.5 * (1.0 + math.cos(math.pi * t))

    return fn


def fmt_hms(seconds):
    """Seconds -> "3h12m" / "12m40s" / "40s", for the ETA in the log line."""
    if not np.isfinite(seconds) or seconds < 0:
        return "?"
    s = int(seconds)
    if s >= 3600:
        return f"{s // 3600}h{(s % 3600) // 60:02d}m"
    if s >= 60:
        return f"{s // 60}m{s % 60:02d}s"
    return f"{s}s"


# ------------------------------------------------------------------- validation
@torch.no_grad()
def validate(model, loader, device, max_batches=8):
    """PSNR / MS-SSIM(dB) / bpp on the validation set.

    Runs under `model.eval()`, restored in a finally. This matters more than it
    looks: in train mode the quantizer runs its EMA update and dead-code
    resampling on validation batches too, so validating would move the codebook.
    `@torch.no_grad()` does not prevent that -- `_ema_update` is itself a no_grad
    function that mutates buffers directly.
    """
    was_training = model.training
    model.eval()
    psnrs, msds, bpps, xbpps = [], [], [], []
    try:
        for i, image in enumerate(loader):
            if max_batches and i >= max_batches:
                break
            if image is None:
                continue
            image = image.to(device, non_blocking=True)
            recon, _, indices, _ = model(image)
            B, _, H, W = image.shape
            bpps.append(shannon_bpp_multi(indices, B * H * W))
            xbpps.append(prior_cross_entropy_bpp(model.quantizer, indices, B * H * W))
            for b in range(B):
                o = to_uint8(image[b])
                r = to_uint8(recon[b])
                psnrs.append(psnr_uint8(o, r))
                try:
                    msds.append(ms_ssim_db(o, r)[0])
                except Exception:
                    pass          # below MS-SSIM's 161px floor
    finally:
        model.train(was_training)

    mean = lambda xs: float(np.mean(xs)) if xs else float("nan")
    return {"psnr": mean(psnrs), "ms_ssim_db": mean(msds),
            "bpp_empirical": mean(bpps), "bpp_prior": mean(xbpps),
            "n_images": len(psnrs)}


# ---------------------------------------------------------------------- preview
def save_preview(path, image, recon, n=4, header="", amplify=4.0):
    """Write an original / reconstruction / |error| grid as a PNG.

    Three rows per column so the failure MODE is visible, not just the score: the
    amplified error map is where you see ringing on edges, blocking at window
    seams, or colour drift that a PSNR number averages away.

    PIL rather than matplotlib, so this path needs no plotting backend at all --
    previews keep working on a box where `pip install matplotlib` was skipped.
    """
    from PIL import Image as PILImage, ImageDraw
    n = max(1, min(int(n), image.shape[0]))
    gap = 4
    cols, psnrs = [], []
    for b in range(n):
        o, r = to_uint8(image[b]), to_uint8(recon[b])
        e = np.clip(np.abs(o.astype(np.int16) - r.astype(np.int16)) * amplify,
                    0, 255).astype(np.uint8)
        sep = np.full((gap, o.shape[1], 3), 255, np.uint8)
        cols.append(np.concatenate([o, sep, r, sep, e], axis=0))
        psnrs.append(psnr_uint8(o, r))

    vsep = np.full((cols[0].shape[0], gap, 3), 255, np.uint8)
    grid = cols[0]
    for c in cols[1:]:
        grid = np.concatenate([grid, vsep, c], axis=1)

    top, bot = 15, 13
    canvas = PILImage.new("RGB", (grid.shape[1], grid.shape[0] + top + bot), "white")
    canvas.paste(PILImage.fromarray(grid), (0, top))
    draw = ImageDraw.Draw(canvas)
    draw.text((2, 3), header, fill="black")
    w = cols[0].shape[1]
    for i, p in enumerate(psnrs):
        draw.text((i * (w + gap) + 2, grid.shape[0] + top + 1),
                  f"PSNR {p:.2f} dB", fill="black")

    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    canvas.save(path)
    return path


def write_preview(cfg, model, step, fixed_batch, image, recon, bpp):
    """Preview PNG for `step`, preferring the fixed batch.

    A FIXED set of validation images means the PNGs written every --vis-every
    steps are the same content every time, so the sequence flips through like
    frames of an animation and progress is obvious. Falling back to the live
    training batch (different random crops each time) makes that much harder to
    read, so it is only the no-validation-set case.
    """
    path = cfg.file(f"step_{step:08d}.png", subdir="previews")
    rows = "rows: original / recon / |error| x4"
    if fixed_batch is None:
        head = f"{cfg.name}  step {step:,}  train batch  bpp {bpp:.4f}   {rows}"
        return save_preview(path, image, recon, n=cfg.vis_images, header=head)

    was_training = model.training
    model.eval()
    try:
        with torch.no_grad():
            r, _, _, bits = model(fixed_batch)
        head = (f"{cfg.name}  step {step:,}  val batch  "
                f"bpp {float(bits) / DOWNSAMPLE ** 2:.4f}   {rows}")
        return save_preview(path, fixed_batch, r, n=cfg.vis_images, header=head)
    finally:
        model.train(was_training)


def _fixed_preview_batch(cfg, val_loader, device):
    """First `--vis-images` validation images, kept on device for the whole run."""
    if val_loader is None or not cfg.vis_every:
        return None
    for batch in val_loader:
        if batch is not None:
            return batch[:cfg.vis_images].to(device)
    return None


# ------------------------------------------------------------------ train loop
def train(cfg, model, train_loader, val_loader=None):
    device = torch.device(cfg.device if torch.cuda.is_available()
                          or cfg.device == "cpu" else "cpu")
    if str(device) != cfg.device:
        print(f"note: requested device {cfg.device!r} unavailable, using {device}")
    if device.type == "cuda":
        # Input shapes are fixed (a handful of them under --multiscale), so let
        # cuDNN autotune once per shape instead of using its portable defaults.
        torch.backends.cudnn.benchmark = True

    model.to(device)
    criterion = RateDistortionLoss(
        lambda_rate=cfg.lambda_rate, beta_commit=cfg.beta_commit, alpha=cfg.alpha,
        data_range=2.0, channels=3, ms_ssim=cfg.ms_ssim_loss).to(device)
    optimizer = AdamW(model.parameters(), lr=cfg.lr, weight_decay=cfg.weight_decay)

    steps_per_epoch = len(train_loader)
    total_steps = cfg.max_steps or cfg.epochs * steps_per_epoch
    scheduler = torch.optim.lr_scheduler.LambdaLR(
        optimizer, make_lr_lambda(total_steps, cfg.lr_warmup, cfg.lr, cfg.eta_min))
    scaler = torch.amp.GradScaler("cuda", enabled=cfg.amp and device.type == "cuda")

    global_step, start_epoch = 0, 0
    resume_path = cfg.resume
    if resume_path == "auto":
        cand = cfg.file("last.pt")
        resume_path = cand if os.path.exists(cand) else ""
    if resume_path:
        global_step, start_epoch, _ = load_checkpoint(
            resume_path, model, optimizer, scheduler, scaler, device=device)
        print(f"resumed from {resume_path} at step {global_step}, epoch {start_epoch}")
        if not cfg.shuffle:
            # With shuffle=False the order is deterministic, so restarting the
            # loader at index 0 would re-train on the same leading images every
            # time and never reach the tail of the corpus. The loader is rebuilt
            # below at the right offset.
            print("  (sequential order: the caller must rebuild the loader with "
                  "start_index -- see train.py)")

    os.makedirs(cfg.out_dir, exist_ok=True)
    cfg.save(cfg.file("config.json"))
    log_path = cfg.file("log.jsonl")
    curve_path = cfg.file("curves.png")

    bpt_ceiling = cfg.bits_per_token_ceiling()
    print(f"\nrun {cfg.name!r}"
          + (f" (preset {cfg.preset})" if cfg.preset else "")
          + f" -> {cfg.out_dir}")
    print(f"training: {total_steps:,} steps "
          f"({steps_per_epoch:,} per epoch x {cfg.epochs} epochs requested), "
          f"batch {cfg.batch_size}, device {device}")
    print(f"rate ceiling: {cfg.num_codebooks} x log2({cfg.codebook_size}) = "
          f"{bpt_ceiling:.0f} bits/token = {bpt_ceiling / DOWNSAMPLE**2:.4f} bpp")
    print(f"rate_beta ramps 0 -> {cfg.rate_beta_final} over "
          f"{cfg.rate_beta_warmup:,} steps")
    print(f"logging: every {cfg.log_every} steps -> {os.path.basename(log_path)} | "
          f"previews every {cfg.vis_every or '-'} | "
          f"curves every {cfg.plot_every or '-'} -> "
          f"{os.path.basename(curve_path)}\n")

    # Multi-resolution training. The loader crops at max(scales); each batch is
    # downsampled here to a random scale so the decoder sees a range of token
    # counts. With windowed attention that means every window is still the
    # training-time size -- only the NUMBER of windows changes, which is exactly
    # the invariance we need at full resolution.
    scales = cfg.scale_sizes()
    rng = random.Random(cfg.seed)
    if scales:
        print(f"multi-resolution training over {scales} "
              f"(token grids {[s // DOWNSAMPLE for s in scales]}, "
              f"crops taken at {max(scales)})")

    preview_batch = _fixed_preview_batch(cfg, val_loader, device)

    model.train()
    start_step = global_step
    t_run = t_start = time.time()
    epoch = start_epoch
    run = {"loss": 0.0, "dist": 0.0, "bpp": 0.0, "n": 0}

    while global_step < total_steps:
        model.quantizer.reset_usage()
        epoch_seen = 0

        for image in train_loader:
            if global_step >= total_steps:
                break
            if image is None:                 # whole batch failed to load
                continue
            image = image.to(device, non_blocking=True)

            if scales:
                s = rng.choice(scales)
                if s != image.shape[-1]:
                    # antialias matters: without it the downsample aliases and the
                    # codec is trained to reproduce the aliasing.
                    image = F.interpolate(image, size=(s, s), mode="bilinear",
                                          align_corners=False, antialias=True)

            # --- rate_beta ramp, exactly base_v3's schedule ---
            if cfg.rate_beta_warmup > 0 and global_step < cfg.rate_beta_warmup:
                model.quantizer.rate_beta = (
                    cfg.rate_beta_final * global_step / cfg.rate_beta_warmup)
            else:
                model.quantizer.rate_beta = cfg.rate_beta_final

            optimizer.zero_grad(set_to_none=True)
            amp_ctx = torch.autocast("cuda", enabled=scaler.is_enabled())
            with amp_ctx:
                recon, vq_commit, indices, bits_per_token = model(image)
                loss, distortion, _ = criterion(recon, image, vq_commit,
                                                bits_per_token)

            if scaler.is_enabled():
                scaler.scale(loss).backward()
                if cfg.grad_clip:
                    scaler.unscale_(optimizer)
                    torch.nn.utils.clip_grad_norm_(model.parameters(), cfg.grad_clip)
                scaler.step(optimizer)
                scaler.update()
            else:
                loss.backward()
                if cfg.grad_clip:
                    torch.nn.utils.clip_grad_norm_(model.parameters(), cfg.grad_clip)
                optimizer.step()
            scheduler.step()

            B, _, H, W = image.shape
            raw_bpp = shannon_bpp_multi(indices, B * H * W)
            run["loss"] += float(loss)
            run["dist"] += float(distortion)
            run["bpp"] += raw_bpp
            run["n"] += 1
            global_step += 1
            epoch_seen += 1

            # ------------------------------------------------------- logging
            if cfg.log_every and global_step % cfg.log_every == 0:
                n = max(run["n"], 1)
                done = max(global_step - start_step, 1)
                elapsed = time.time() - t_run
                rec = {
                    "name": cfg.name, "step": global_step, "epoch": epoch,
                    "loss": run["loss"] / n, "dist": run["dist"] / n,
                    "bpp": run["bpp"] / n,
                    "util": model.quantizer.quick_util(),
                    "rate_beta": model.quantizer.rate_beta,
                    "rate_pressure": model.quantizer.rate_pressure(),
                    "lr": scheduler.get_last_lr()[0],
                    "imgs_per_s": (n * cfg.batch_size)
                                  / max(time.time() - t_start, 1e-9),
                    "elapsed_s": elapsed,
                    "eta_s": elapsed / done * max(total_steps - global_step, 0),
                }
                print(f"[{cfg.name}] {global_step:>7}/{total_steps} "
                      f"| L {rec['loss']:.4f} | D {rec['dist']:.4f} "
                      f"| bpp {rec['bpp']:.4f} | util {100*rec['util']:.0f}% "
                      f"| beta {rec['rate_beta']:.3f} | rp {rec['rate_pressure']:.3f} "
                      f"| lr {rec['lr']:.2e} | {rec['imgs_per_s']:.0f} img/s "
                      f"| eta {fmt_hms(rec['eta_s'])}")
                with open(log_path, "a", encoding="utf-8") as f:
                    f.write(json.dumps(rec) + "\n")
                run = {"loss": 0.0, "dist": 0.0, "bpp": 0.0, "n": 0}
                t_start = time.time()

            if cfg.vis_every and global_step % cfg.vis_every == 0:
                p = write_preview(cfg, model, global_step, preview_batch,
                                  image, recon, raw_bpp)
                print(f"  preview -> {p}")

            if cfg.plot_every and global_step % cfg.plot_every == 0:
                if plot_log(log_path, curve_path, title=cfg.name):
                    print(f"  curves  -> {curve_path}")

            if cfg.ckpt_every and global_step % cfg.ckpt_every == 0:
                save_checkpoint(cfg.file("last.pt"), model, optimizer, scheduler,
                                scaler, global_step, epoch, cfg,
                                save_full_model=cfg.save_full_model)

            if cfg.val_every and val_loader is not None \
                    and global_step % cfg.val_every == 0:
                m = validate(model, val_loader, device, cfg.val_batches)
                print(f"  [val step {global_step}] PSNR {m['psnr']:.3f} dB "
                      f"| MS-SSIM {m['ms_ssim_db']:.2f} dB "
                      f"| bpp {m['bpp_empirical']:.4f} (prior {m['bpp_prior']:.4f}) "
                      f"| n={m['n_images']}")
                with open(log_path, "a", encoding="utf-8") as f:
                    f.write(json.dumps({"name": cfg.name, "step": global_step,
                                        "epoch": epoch, "val": m}) + "\n")

        # ------------------------------------------------------- epoch summary
        epoch += 1
        stats = model.quantizer.utilization_stats()
        prior = model.quantizer.prior_stats()
        print(f"\nepoch {epoch} done ({epoch_seen:,} steps, {global_step:,} total)")
        print("codebook utilization:")
        print(format_util(stats))
        print(f"  prior entropy: {prior['bits_per_token']:.3f} bits/token "
              f"({prior['bits_per_token'] / DOWNSAMPLE**2:.4f} bpp equivalent) "
              f"of {bpt_ceiling:.0f} available")
        if stats["overall"]["util_pct"] < cfg.collapse_warn * 100:
            print(f"  WARNING utilization below {cfg.collapse_warn*100:.0f}% -- the "
                  f"codebook is collapsing. Lower --rate-beta-final, raise "
                  f"--revive-size, or drop to a smaller --preset.")

        if val_loader is not None:
            m = validate(model, val_loader, device, cfg.val_batches)
            print(f"  [val epoch {epoch}] PSNR {m['psnr']:.3f} dB "
                  f"| MS-SSIM {m['ms_ssim_db']:.2f} dB "
                  f"| bpp {m['bpp_empirical']:.4f} (prior {m['bpp_prior']:.4f})")
            with open(log_path, "a", encoding="utf-8") as f:
                f.write(json.dumps({"name": cfg.name, "step": global_step,
                                    "epoch": epoch, "val": m}) + "\n")

        save_checkpoint(cfg.file(f"epoch_{epoch:03d}.pt"), model, optimizer,
                        scheduler, scaler, global_step, epoch, cfg,
                        save_full_model=cfg.save_full_model)
        save_checkpoint(cfg.file("last.pt"), model, optimizer, scheduler, scaler,
                        global_step, epoch, cfg,
                        save_full_model=cfg.save_full_model)
        if cfg.plot_every:
            plot_log(log_path, curve_path, title=cfg.name)
        print()

    save_checkpoint(cfg.file("final.pt"), model, optimizer, scheduler, scaler,
                    global_step, epoch, cfg, save_full_model=cfg.save_full_model)
    if cfg.plot_every:
        plot_log(log_path, curve_path, title=cfg.name)
    print(f"done: {global_step:,} steps in {fmt_hms(time.time() - t_run)} "
          f"-> {cfg.file('final.pt')}")
    return model
