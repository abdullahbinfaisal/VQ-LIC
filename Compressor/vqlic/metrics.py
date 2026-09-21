"""Metrics and reporting helpers.

PSNR is computed in numpy rather than via skimage, so the training image needs
neither scikit-image nor OpenCV. Numerically identical to
`psnr_sk(..., data_range=255)` on uint8 input.
"""
from __future__ import annotations

import numpy as np
import torch
from pytorch_msssim import ms_ssim as _ms_ssim


def to_uint8(x):
    """[-1,1] tensor (CHW or BCHW) -> uint8 HWC numpy: *0.5 + 0.5, clamp, round."""
    if x.dim() == 4:
        x = x[0]
    x = (x.detach().cpu().float() * 0.5 + 0.5).clamp(0, 1)
    return (x.permute(1, 2, 0).numpy() * 255).round().astype(np.uint8)


def psnr_uint8(a, b):
    """PSNR between two uint8 arrays, data_range=255."""
    mse = np.mean((a.astype(np.float64) - b.astype(np.float64)) ** 2)
    if mse <= 0:
        return float("inf")
    return float(10.0 * np.log10(255.0 ** 2 / mse))


def ms_ssim_db(a_u8, b_u8, device=None):
    """MS-SSIM in dB: -10*log10(1 - MS-SSIM). Returns (dB, raw).

    dB is what compression papers quote, because raw MS-SSIM compresses everything
    interesting into 0.9-1.0. 5-scale MS-SSIM needs >= 161 px per side; below that
    pytorch_msssim raises, so callers guard.
    """
    ta = torch.from_numpy(a_u8).permute(2, 0, 1)[None].float().div_(255.0)
    tb = torch.from_numpy(b_u8).permute(2, 0, 1)[None].float().div_(255.0)
    if device is not None:
        ta, tb = ta.to(device), tb.to(device)
    with torch.no_grad():
        v = float(_ms_ssim(ta, tb, data_range=1.0, size_average=True))
    v = min(max(v, 0.0), 1.0 - 1e-10)
    return -10.0 * np.log10(1.0 - v), v


def shannon_bpp_multi(indices, total_pixels):
    """Empirical bpp: per-codebook entropy of the realised indices, summed.

    `indices` is [B,H,W,G] over the LATENT grid; `total_pixels` must be the pixel
    count of the images those tokens came from (B*H_img*W_img), which is what makes
    the result bits per PIXEL.

    This is the entropy of THIS batch's own histogram, so it is optimistic: a real
    coder pays the cross-entropy against a prior fixed in advance. Fine as a
    training signal, not a number to publish -- see `prior_cross_entropy_bpp`.
    """
    idx = indices.detach().cpu().numpy()
    total_bits, n_tokens = 0.0, idx[..., 0].size
    for g in range(idx.shape[-1]):
        flat = idx[..., g].ravel()
        _, c = np.unique(flat, return_counts=True)
        p = c / flat.size
        total_bits += -np.sum(p * np.log2(p + 1e-12)) * n_tokens
    # max(0) because the +1e-12 guard makes a degenerate histogram (every token on
    # one code, which is what an untrained encoder produces) come out around -1e-14,
    # and a negative bpp in the first log line reads like a bug.
    return max(0.0, float(total_bits / total_pixels))


def prior_cross_entropy_bpp(quantizer, indices, total_pixels):
    """Cross-entropy of realised indices against the quantizer's EMA prior, in bpp.

    This is what an entropy coder actually pays, and it is the number to report. It
    is >= `shannon_bpp_multi` whenever the data does not match the prior, which on
    out-of-distribution validation data it never quite does.
    """
    # `+ eps` mirrors the quantizer's own `_prior_logp`: the rate must be charged
    # against the same prior the quantizer assigns with, and on a freshly
    # initialized model cluster_size is all zeros, so a bare cs/cs.sum() is 0/0.
    cs = quantizer.cluster_size + getattr(quantizer, "eps", 1e-5)
    logp = torch.log2((cs / cs.sum(dim=1, keepdim=True)).clamp_min(1e-12))
    idx = indices.detach()
    bits = 0.0
    for g in range(idx.shape[-1]):
        bits += float(-logp[g][idx[..., g]].sum())
    return bits / float(total_pixels)


def model_size_table(model):
    """Parameter/size breakdown, splitting the edge cost (encoder + codebook LUT)
    from the decoder, which never ships to the device."""
    def count(params):
        params = list(params)
        return (sum(p.numel() for p in params),
                sum(p.numel() * p.element_size() for p in params))

    enc_n, enc_b = count(model.encoder.parameters())
    dec_n, dec_b = count(model.decoder.parameters())
    cb_n, cb_b = count(model.quantizer.buffers())
    lut = model.quantizer.embed
    lut_n, lut_b = lut.numel(), lut.numel() * lut.element_size()

    mb = lambda x: x / 1024 ** 2
    return "\n".join([
        f"{'Component':<28}{'Params':>14}{'FP32':>12}{'INT8':>12}",
        "-" * 66,
        f"{'Encoder (EDGE)':<28}{enc_n:>14,}{mb(enc_b):>10.2f}MB{mb(enc_b/4):>10.2f}MB",
        f"{'Codebook LUT (EDGE)':<28}{lut_n:>14,}{mb(lut_b):>10.2f}MB{mb(lut_b/4):>10.2f}MB",
        f"{'Decoder (server)':<28}{dec_n:>14,}{mb(dec_b):>10.2f}MB{mb(dec_b/4):>10.2f}MB",
        "-" * 66,
        f"{'EDGE TOTAL (enc + LUT)':<28}{enc_n+lut_n:>14,}"
        f"{mb(enc_b+lut_b):>10.2f}MB{mb((enc_b+lut_b)/4):>10.2f}MB",
        f"{'FULL MODEL':<28}{enc_n+dec_n+cb_n:>14,}"
        f"{mb(enc_b+dec_b+cb_b):>10.2f}MB{'-':>12}",
    ])
