"""Checkpoint -> ready-to-code model, for both the FP32 and INT8 QAT paths.

Every script that measures something loads a checkpoint, and a checkpoint is not
a model until two things have happened that `load_state_dict` does not do:

1. `rate_beta` is restored. It is a plain attribute, absent from the state_dict,
   and `build_model` hardcodes `0.0`. Leaving it at zero swaps the
   entropy-constrained assignment for plain nearest-neighbour -- which does not
   raise, does not warn, and inflates the measured rate by up to 2.6x while
   changing which codes are used at all.
2. `eval()` is called. In train mode the quantizer runs `_ema_update`, a
   `@torch.no_grad` function that mutates buffers directly, so wrapping the call
   site in `torch.no_grad()` does NOT stop it and the codebook drifts from one
   image to the next.

Both used to be open-coded in each benchmark script, which is one chance per
script for a number to be quietly drawn from a different model than the number
beside it. This module is the only copy.
"""
from __future__ import annotations

import copy

import torch
import torch.nn as nn
from torch.ao.quantization import get_default_qat_qconfig_mapping
from torch.ao.quantization.quantize_fx import convert_fx, prepare_qat_fx

try:
    from torch.ao.quantization.quantize_fx import fuse_fx
except Exception:                                             # pragma: no cover
    from torch.ao.quantization.fx.fuse import fuse_fx

from vqlic.checkpoint import QAT_META_KEY, apply_qat_payload, load_qat_payload
from vqlic.codec import build_model
from vqlic.config import Config

__all__ = ["load_fp32", "load_qat_int8", "CpuEncoderBridge", "place"]


def load_fp32(path):
    """An FP32 checkpoint -> `(model, cfg, raw_checkpoint)`.

    The model is in eval mode with `rate_beta` restored from its own config, so
    the assignment is the one the checkpoint was trained with.
    """
    ck = torch.load(path, map_location="cpu", weights_only=False)
    if "model" not in ck or "config" not in ck:
        raise SystemExit(
            f"{path} is not a vqlic checkpoint (needs 'model' and 'config').")
    cfg = Config.from_dict(ck["config"])
    model = build_model(cfg)
    model.load_state_dict(ck["model"], strict=True)
    model.quantizer.rate_beta = cfg.rate_beta_final
    model.eval()
    model.quantizer.track_usage = False
    return model, cfg, ck


def load_qat_int8(fp32_path, qat_path=None):
    """FP32 parent + QAT encoder -> `(model, cfg, info)`, the reported model.

    With `qat_path` the encoder is rebuilt through `fuse_fx` -> `prepare_qat_fx`,
    the saved prepared weights are loaded, and `convert_fx` produces the real
    INT8 module; the trained decoder is loaded alongside.

    A QAT checkpoint also carries its **quantizer**, and that matters: the
    codebook may itself be INT8 and its prior re-estimated, so it is not the
    codebook the FP32 parent holds. It is restored when present, and a payload
    whose metadata claims a quantized codebook it cannot supply is refused
    rather than silently measured against a codebook its encoder never saw.

    Without `qat_path` you get the plain FP32 model -- the baseline the QAT run
    is trying to match.

    Note the INT8 encoder pins the job to CPU: fbgemm has no CUDA kernels, and
    moving a converted module to CUDA segfaults rather than raising.
    """
    model, cfg, ck = load_fp32(fp32_path)

    info = {
        "num_codebooks": cfg.num_codebooks,
        "codebook_size": cfg.codebook_size,
        "rate_beta": cfg.rate_beta_final,
        "fp32_step": int(ck.get("step", 0)),
        "fp32_epoch": int(ck.get("epoch", 0)),
        "quantized": False,
        "qat_epoch": 0,
        "qat_images_seen": 0,
    }

    if qat_path:
        qck = load_qat_payload(qat_path)
        prepared = prepare_qat_fx(
            fuse_fx(copy.deepcopy(model.encoder).eval()),
            get_default_qat_qconfig_mapping("fbgemm"),
            torch.randn(1, 3, 224, 224),
        )
        model.encoder = prepared
        restored = apply_qat_payload(qck, model)
        model.encoder = convert_fx(prepared.cpu().eval()).eval()
        meta = qck.get(QAT_META_KEY) or {}
        info.update(
            quantized=any(getattr(v, "dtype", None) in (torch.qint8, torch.quint8)
                          for v in model.encoder.state_dict().values()),
            qat_epoch=int(qck.get("epoch", 0)),
            qat_images_seen=int(qck.get("images_seen", 0)),
            int8_missing=restored["encoder_missing"],
            int8_unexpected=restored["encoder_unexpected"],
            codebook_quantized=bool(restored["codebook_quantized"]),
            codebook_bits=int(meta.get("codebook_bits", 0)),
            quantizer_restored=bool(restored["quantizer"]),
            prior_reestimated=bool(qck.get("prior_reestimated", False)),
        )
        if restored["encoder_missing"] or restored["encoder_unexpected"]:
            raise SystemExit(
                f"{qat_path}: prepared-encoder state does not match a freshly "
                f"prepared encoder ({len(restored['encoder_missing'])} missing, "
                f"{len(restored['encoder_unexpected'])} unexpected). The QAT run "
                f"prepared it differently -- check the QAT backend and the FP32 "
                f"checkpoint it came from.")

    # Re-assert after apply_qat_payload, which loads modules that may arrive in
    # train mode.
    model.eval()
    model.quantizer.track_usage = False
    return model, cfg, info


class CpuEncoderBridge(nn.Module):
    """Run the INT8 encoder on the CPU, hand its output to `out_device`.

    fbgemm has no CUDA kernels, so a `convert_fx`-ed encoder cannot leave the
    CPU -- moving it segfaults rather than raising. Everything downstream can:
    the quantizer's argmin over G*K codes and the six-layer decoder are the bulk
    of the arithmetic. `codec._prepare` sends its input to
    `quantizer.embed.device`, so wrapping the encoder is the whole of what it
    takes to split the model across both devices.
    """

    def __init__(self, encoder, out_device):
        super().__init__()
        self.encoder = encoder
        self.out_device = out_device

    def forward(self, x):
        return self.encoder(x.to("cpu")).to(self.out_device)


def place(model, want, quantized):
    """Move a loaded model onto `want`, and return the device to feed it.

    For an FP32 model that is just `.to(device)`. For an INT8 one the encoder
    stays on the CPU and the quantizer and decoder move, which is what keeps the
    GPU from idling through the expensive half of the work.

    Falls back to CPU when CUDA is not available, so the same command line runs
    on a laptop.
    """
    if not torch.cuda.is_available():
        want = "cpu"
    device = torch.device(want)
    if not quantized:
        return model.to(device), device
    if device.type != "cpu":
        model.quantizer.to(device)
        model.decoder.to(device)
        model.encoder = CpuEncoderBridge(model.encoder, device)
    return model, device
