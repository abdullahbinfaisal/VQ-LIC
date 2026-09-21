"""Checkpoint I/O.

Writes go to a temp file and are then os.replace'd, so a crash mid-save cannot
leave a truncated checkpoint where the resume path expects a valid one.
"""
from __future__ import annotations

import os

import torch

from vqlic.config import Config


def save_checkpoint(path, model, optimizer=None, scheduler=None, scaler=None,
                    step=0, epoch=0, cfg=None, save_full_model=False):
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    payload = {
        "model": model.state_dict(),
        "step": int(step),
        "epoch": int(epoch),
        "config": cfg.to_dict() if cfg is not None else None,
        "torch_version": torch.__version__,
    }
    if optimizer is not None:
        payload["optimizer"] = optimizer.state_dict()
    if scheduler is not None:
        payload["scheduler"] = scheduler.state_dict()
    if scaler is not None:
        payload["scaler"] = scaler.state_dict()

    tmp = path + ".tmp"
    torch.save(payload, tmp)
    os.replace(tmp, path)

    if save_full_model:
        tmp = path + ".full.tmp"
        torch.save(model, tmp)
        os.replace(tmp, path.replace(".pt", ".full.pt"))
    return path


def load_checkpoint(path, model, optimizer=None, scheduler=None, scaler=None,
                    device="cpu", strict=True):
    """Restore into live objects. Returns (step, epoch, saved_config_or_None)."""
    ckpt = torch.load(path, map_location=device, weights_only=False)
    if "model" not in ckpt:
        raise RuntimeError(f"{path} has no 'model' key -- not a checkpoint written "
                           f"by save_checkpoint")

    missing, unexpected = model.load_state_dict(ckpt["model"], strict=strict)
    if missing or unexpected:
        print(f"  state_dict: {len(missing)} missing, {len(unexpected)} unexpected")

    if optimizer is not None and "optimizer" in ckpt:
        optimizer.load_state_dict(ckpt["optimizer"])
    if scaler is not None and "scaler" in ckpt:
        scaler.load_state_dict(ckpt["scaler"])
    if scheduler is not None and "scheduler" in ckpt:
        # Carry ONLY the position, never the horizon. A recursive scheduler such as
        # CosineAnnealingLR serializes T_max, so restoring the whole dict into a
        # scheduler built for a different horizon silently reinstates the old one --
        # and past T_max the cosine wraps and the LR climbs back toward its peak.
        # (The LambdaLR here is closed-form, but keep the discipline.)
        sd = scheduler.state_dict()
        for k in ("last_epoch", "_step_count"):
            if k in ckpt["scheduler"]:
                sd[k] = ckpt["scheduler"][k]
        scheduler.load_state_dict(sd)

    saved_cfg = None
    if ckpt.get("config"):
        try:
            saved_cfg = Config.from_dict(ckpt["config"])
        except Exception as e:
            print(f"  could not rebuild saved config: {e}")

    return int(ckpt.get("step", 0)), int(ckpt.get("epoch", 0)), saved_cfg


# ---------------------------------------------------------------------- QAT
# The QAT payload keeps the key names the base_v3 notebook cell wrote
# (`encoder_state_dict`, `decoder_state_dict`, `optimizer_state_dict`,
# `images_seen`, `epoch`) so the four checkpoints listed in
# the published ladder stay loadable by the same code path, and adds
# what INT8-codebook QAT needs on top:
#
#   quantizer_state_dict  the QUANTIZED codebook. Without it, eval would rebuild
#                         the quantizer from the fp32 `--fp32` companion and
#                         search an fp32 codebook with an encoder trained against
#                         an int8 one -- a mismatch that produces plausible
#                         numbers rather than an error, which is the worst kind.
#   qat                   the integer tables (int8 codebook, per-group scale,
#                         int32 assignment bias) and the encoder output qparams
#                         they were derived from. Recomputable from the state
#                         dicts, but stored so a consumer does not have to know
#                         how, and so a mismatch is detectable.
QAT_META_KEY = "qat"


def qat_tables(quantizer):
    """The integer assignment tables off a quantizer, or None if not quantized."""
    if not getattr(quantizer, "cb_quantized", False):
        return None
    return {
        "codebook_quantized": True,
        "codebook_bits": int(quantizer.cb_bits),
        "cb_int8": quantizer.cb_int8.detach().cpu().clone(),
        "cb_scale": quantizer.cb_scale.detach().cpu().clone(),
        "cb_bias": (None if quantizer.cb_bias is None
                    else quantizer.cb_bias.detach().cpu().clone()),
        "bias_frac_bits": int(quantizer.cb_bias_frac_bits),
        "latent_scale": float(quantizer.cb_latent_scale),
        "latent_zero_point": int(quantizer.cb_latent_zero_point),
        "rate_beta": float(quantizer.rate_beta),
    }


def save_qat_checkpoint(path, model, optimizer=None, scheduler=None,
                        images_seen=0, epoch=0, step=0, cfg=None, extra=None):
    """Write one QAT checkpoint. `model.encoder` is the FX-prepared encoder.

    Same temp-file-then-`os.replace` discipline as `save_checkpoint`: these are
    written every 50k images on a multi-day run, so a crash mid-write must not
    leave a truncated file where `--qat-resume auto` will look.
    """
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    payload = {
        "encoder_state_dict": model.encoder.state_dict(),
        "decoder_state_dict": model.decoder.state_dict(),
        "quantizer_state_dict": model.quantizer.state_dict(),
        "images_seen": int(images_seen),
        "epoch": int(epoch),
        "step": int(step),
        "config": cfg.to_dict() if cfg is not None else None,
        "torch_version": torch.__version__,
        QAT_META_KEY: qat_tables(model.quantizer),
    }
    if optimizer is not None:
        payload["optimizer_state_dict"] = optimizer.state_dict()
    if scheduler is not None:
        payload["scheduler_state_dict"] = scheduler.state_dict()
    if extra:
        payload.update(extra)

    tmp = path + ".tmp"
    torch.save(payload, tmp)
    os.replace(tmp, path)
    return path


def load_qat_payload(path, map_location="cpu"):
    """Read a QAT checkpoint, tolerating the notebook-era layout.

    The archived rungs were saved either as a bare encoder `state_dict` or as the
    5-key dict the cell wrote; both are normalized to the current shape here so
    one loader serves every vintage.
    """
    obj = torch.load(path, map_location=map_location, weights_only=False)
    if not isinstance(obj, dict) or "encoder_state_dict" not in obj:
        # A bare `torch.save(encoder.state_dict())`, as the cell's
        # `encoder_qat_prepared_*.pth` files are.
        return {"encoder_state_dict": obj, QAT_META_KEY: None}
    obj.setdefault(QAT_META_KEY, None)
    return obj


def apply_qat_payload(payload, model, strict_codebook=True):
    """Restore a QAT payload onto a live model built from the fp32 checkpoint.

    `model.encoder` must already be the FX-prepared encoder, built the same way
    the run built it, or the fake-quant parameter names will not line up.

    `strict_codebook` is the guard that matters. A payload whose `qat` metadata
    says the codebook was quantized, but which carries no `quantizer_state_dict`
    to prove it, cannot be evaluated correctly: the encoder was trained against
    an int8 codebook and the only codebook available is the fp32 one it was not
    trained against. Silently proceeding yields numbers that look reasonable, so
    this raises instead. Pass False only to deliberately measure that mismatch.

    Returns a dict describing what was restored, for the log.
    """
    enc_missing, enc_unexpected = model.encoder.load_state_dict(
        payload["encoder_state_dict"], strict=False)
    # The key NAMES, not counts: callers report `len(...)` of these and, when a
    # prepared encoder does not match, the names are what identifies which part
    # of the graph was prepared differently.
    info = {"encoder_missing": list(enc_missing),
            "encoder_unexpected": list(enc_unexpected),
            "decoder": False, "quantizer": False, "codebook_quantized": False}

    if payload.get("decoder_state_dict") is not None:
        model.decoder.load_state_dict(payload["decoder_state_dict"])
        info["decoder"] = True

    meta = payload.get(QAT_META_KEY) or {}
    if payload.get("quantizer_state_dict") is not None:
        model.quantizer.load_state_dict(payload["quantizer_state_dict"])
        info["quantizer"] = True
    elif meta.get("codebook_quantized") and strict_codebook:
        raise RuntimeError(
            "this QAT checkpoint was trained against an INT8-quantized "
            "codebook but carries no 'quantizer_state_dict', so the quantized "
            "codebook is gone. Evaluating it against the fp32 codebook from "
            "the --fp32 companion would silently mismatch encoder and "
            "codebook. Re-save the checkpoint, or pass strict_codebook=False "
            "to measure the mismatch deliberately.")

    if meta.get("codebook_quantized"):
        q = model.quantizer
        # These are plain attributes rather than buffers (see
        # MultiCodebookEMAQuantizer), so nothing else will ever move them --
        # place them where the rest of the quantizer already lives.
        dev = q.embed.device
        q.cb_int8 = meta["cb_int8"].to(dev)
        q.cb_scale = meta["cb_scale"].to(dev)
        q.cb_bias = (None if meta["cb_bias"] is None
                     else meta["cb_bias"].to(dev))
        q.cb_bits = int(meta["codebook_bits"])
        q.cb_latent_scale = float(meta["latent_scale"])
        q.cb_latent_zero_point = int(meta["latent_zero_point"])
        # Older payloads predate the adaptive choice and used the fixed 4.
        q.cb_bias_frac_bits = int(meta.get("bias_frac_bits", 4))
        q.cb_quantized = True
        info["codebook_quantized"] = True
        info["codebook_bits"] = q.cb_bits
        # The int8 table is meant to be an exact re-encoding of `embed`, not a
        # second source of truth (see quantizer.quantize_codebook). If the two
        # disagree, one of them came from a different run.
        deq = q.cb_int8.to(q.cb_scale.dtype) * q.cb_scale.view(-1, 1, 1)
        drift = float((deq - q.embed).abs().max())
        info["table_vs_embed_max_abs"] = drift
        if drift > 1e-5:
            raise RuntimeError(
                f"the int8 codebook table and `embed` disagree by {drift:.3g}; "
                f"they should be the same vectors. The payload's 'qat' tables "
                f"and its 'quantizer_state_dict' are from different runs.")
    return info
