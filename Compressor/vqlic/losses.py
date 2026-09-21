"""Rate-distortion objective.

    J = D + lambda_rate * R + beta_commit * commitment
    D = alpha * (1 - SSIM) + (1 - alpha) * L1
"""
from __future__ import annotations

import torch.nn as nn


class RateDistortionLoss(nn.Module):
    """Three things worth knowing about this objective.

    **SSIM, not MS-SSIM, by default.** Single-scale, matching the lineage. Pass
    `ms_ssim=True` (`--ms-ssim-loss`) to switch deliberately.

    **`data_range=2.0`** because tensors live in [-1, 1]. That has to agree with the
    dataset normalization and the decoder's Tanh; changing one alone silently
    rescales the distortion term.

    **The rate term has no gradient path.** The bits come from `-log p[idx]`, where
    the prior is an EMA buffer and `idx` is an argmin -- neither is differentiable.
    It shifts the reported total but trains nothing. The knob that actually controls
    rate is the quantizer's `rate_beta` (the ECVQ assignment tilt). Note also that
    `bits_per_token` is per LATENT token, not per pixel, so `lambda_rate` is
    effectively 64x stronger than the same number applied to bpp.
    """

    def __init__(self, lambda_rate=0.1, beta_commit=1.0, alpha=0.84,
                 data_range=2.0, channels=3, ms_ssim=False):
        super().__init__()
        self.l1 = nn.L1Loss()
        if ms_ssim:
            from pytorch_msssim import MS_SSIM
            self.ssim = MS_SSIM(data_range=data_range, size_average=True,
                                channel=channels)
        else:
            from pytorch_msssim import SSIM
            self.ssim = SSIM(data_range=data_range, size_average=True,
                             channel=channels)
        self.lambda_rate = lambda_rate
        self.beta_commit = beta_commit
        self.alpha = alpha

    def forward(self, recon, original, vq_commit, bits_per_token):
        ssim_loss = 1.0 - self.ssim(recon, original)
        l1_loss = self.l1(recon, original)
        distortion = self.alpha * ssim_loss + (1.0 - self.alpha) * l1_loss
        total = (distortion
                 + self.lambda_rate * bits_per_token
                 + self.beta_commit * vq_commit)
        return total, distortion, bits_per_token
