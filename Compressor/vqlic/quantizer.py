"""Multi-codebook (product) EMA vector quantizer with rate-aware ECVQ assignment.

Ported from base_v3.ipynb. base_v4.ipynb carries a byte-identical copy of this
class, so the only substantive change is the DEAD-CODE REVIVAL rule, taken from
base_RVQ.ipynb -- see `_ema_update` for the full reasoning and the measurement
that motivated it.
"""
from __future__ import annotations

import math

import numpy as np
import torch
import torch.nn as nn
import torch.nn.functional as F


# Fixed-point precision of the int32 assignment bias (see `set_latent_quant`).
#
# The bias is a rounded float, and rounding it coarsely moves the decision
# boundary between two codes -- enough to flip the argmin for a token sitting
# near a Voronoi face. Measured on 6 Kodak images (147,456 token-groups) at the
# bpp030 rung, against a float64 reference:
#
#     fractional bits    tokens assigned differently from float64
#            4                        10   (0.0068%)
#            6                         2   (0.0014%)
#            8                         0
#           12                         0
#
# -- so this is worth getting right, and 4 (the first thing tried) was not
# enough. Rather than hardcode 8, `set_latent_quant` spends whatever precision
# the int32 budget allows: it picks the largest shift for which the WORST-CASE
# comparison still has `CB_INT32_SAFETY`x headroom below 2^31, capped here.
# The cap exists because past ~12 bits there is nothing left to buy -- the
# integer search already agrees with float64 exactly -- and the headroom is
# better spent as headroom.
CB_BIAS_FRAC_BITS_CAP = 12
CB_INT32_SAFETY = 4.0

# What a checkpoint predating the adaptive choice used, and the fallback for a
# quantizer whose tables were restored without a recorded value.
CB_BIAS_FRAC_BITS = 4


class MultiCodebookEMAQuantizer(nn.Module):
    """Product VQ with EMA codebook updates and optional entropy-constrained
    (rate-aware) assignment.

    * `embedding_dim` is split into `num_codebooks` groups of `d` dims each, so
      `num_codebooks=1` is a single large codebook of dimension `embedding_dim`.
      base_v3 runs with G=1, K=128, d=128.
    * A factorized prior P(k) is estimated from the EMA `cluster_size` and used
      both for the rate estimate and, when `rate_beta > 0`, for the assignment
      rule `argmin(dist - rate_beta * log P(k))`.
    * Dead codes are re-seeded to fight collapse.

    The four knobs below are CLASS attributes on purpose. Checkpoints in this
    lineage were written with `torch.save(model)` (a full pickle), and unpickling
    an nn.Module restores its `__dict__` without ever calling `__init__` -- so an
    instance loaded from an old checkpoint has a `__dict__` that predates these
    names. Declaring them on the class lets such an instance resolve them by
    ordinary attribute lookup instead of raising AttributeError from inside
    `_ema_update`.
    """

    # cluster_size handed to a revived code. MUST sit comfortably above
    # dead_thresh -- see _ema_update.
    revive_size = 10.0
    # revived codes are drawn from the worst-quantized fraction of the batch
    # rather than uniformly at random.
    revive_pool_frac = 0.10
    # scale rate_beta by the batch's mean nearest-neighbour squared error, making
    # it dimensionless. False reproduces base_v3, which used rate_beta in
    # absolute units.
    rate_beta_norm = False

    # --- INT8 codebook / integer assignment search -------------------------
    # State written by `quantize_codebook` and `set_latent_quant`. Declared here
    # for the same unpickling reason as the four knobs above, and held as PLAIN
    # attributes rather than registered buffers for two specific reasons:
    #
    # * a persistent buffer would appear in `state_dict()`, and every existing
    #   consumer loads this lineage's checkpoints with `strict=True`
    #   (every script under `scripts/`), so a new key would turn all
    #   four archived rungs into load errors;
    # * a non-persistent buffer stays out of the state_dict but is still counted
    #   by `nn.Module.buffers()`, which `vqlic.metrics.model_size_table` sums into
    #   its "FULL MODEL" row -- a reporting change nobody asked for.
    #
    # The cost is that `.to(device)` does not move them; `int_search` moves what
    # it needs onto the input's device instead.
    cb_quantized = False
    cb_bits = 0
    cb_int8 = None                 # [G,K,d] int8 -- the transmitted table
    cb_scale = None                # [G] float32 -- one scale per codebook group
    cb_bias = None                 # [G,K] int64 -- |c|^2 and the ECVQ tilt, fused
    cb_latent_scale = 0.0          # the encoder's output quantization scale, s_z
    cb_latent_zero_point = 0       # ... and its zero point, zp_z
    cb_bias_frac_bits = CB_BIAS_FRAC_BITS   # fixed-point shift used by cb_bias

    def __init__(self, embedding_dim=64, num_codebooks=4, codebook_size=256,
                 commitment_cost=0.25, decay=0.99, rate_beta=0.0,
                 dead_thresh=1.0, revive_size=None, revive_pool_frac=None,
                 rate_beta_norm=None):
        super().__init__()
        assert embedding_dim % num_codebooks == 0, \
            f"embedding_dim {embedding_dim} not divisible by num_codebooks {num_codebooks}"
        self.G = num_codebooks
        self.K = codebook_size
        self.d = embedding_dim // num_codebooks
        self.commitment_cost = commitment_cost
        self.decay = decay
        self.eps = 1e-5
        self.rate_beta = rate_beta
        self.dead_thresh = dead_thresh
        self.track_usage = True

        if revive_size is not None:
            self.revive_size = float(revive_size)
        if revive_pool_frac is not None:
            self.revive_pool_frac = float(revive_pool_frac)
        if rate_beta_norm is not None:
            self.rate_beta_norm = bool(rate_beta_norm)

        self.register_buffer("embed", torch.randn(self.G, self.K, self.d))
        self.register_buffer("cluster_size", torch.zeros(self.G, self.K))
        self.register_buffer("ema_w", self.embed.clone())
        self.register_buffer("code_usage", torch.zeros(self.G, self.K))

        # Diagnostics, refreshed every forward. Held as 0-dim device tensors (not
        # buffers, not floats) so the hot path never triggers a device sync --
        # .item() is called only by the logger.
        self._stat_dist = None   # mean nearest-neighbour squared error
        self._stat_bits = None   # mean bits/token charged by the prior

    # ------------------------------------------------------------------ prior
    def _prior_logp(self, g):
        p = self.cluster_size[g] + self.eps
        p = p / p.sum()
        return torch.log(p)

    # ---------------------------------------------------------------- forward
    def forward(self, inputs):
        """Returns (quantized, commit_loss, indices, bits_per_token).

        NOTE the units of the 4th value: bits per LATENT TOKEN, not bits per
        pixel. Converting to true bpp needs the encoder's spatial downsampling
        factor, which this module does not know, so `NeuralImageCodec` does that
        division. base_v3 named this `bpp` and consumed it as though it already
        were bpp, which overstates the rate by (downsample factor)^2 = 64x. The
        NAME is corrected here; the VALUE fed to the loss is left unchanged so
        `lambda_rate` keeps the meaning it had during base_v3 training. See
        `vqlic.losses.RateDistortionLoss`.

        ALWAYS RUNS IN FP32, autocast or not. Disabling autocast is not enough on
        its own -- `inputs` arrives already fp16, cast by the encoder's own convs
        while autocast was active, so the `.float()` below is what actually
        prevents `Half @ float` against the fp32 `embed` buffer.

        Keeping this block in fp32 is right on the numerics anyway:

        * distance is computed by the expansion |z|^2 - 2 z.c + |c|^2, which
          subtracts quantities of similar magnitude. In fp16 that cancellation eats
          most of the mantissa, and an argmin over the result is what assigns each
          token to a code -- assignment near cell boundaries would become
          noise-driven.
        * `cluster_size` accumulates per-batch hit counts, which reach thousands at
          batch 32+. fp16's spacing at 2048 is already 1.0, so an EMA of counts
          could no longer represent the increments `dead_thresh` compares against.

        The cast is differentiable, so gradients still reach an fp16 encoder, and
        autocast re-casts the fp32 tensors returned here at the decoder's first op.
        """
        inputs = inputs.float()
        with torch.autocast(inputs.device.type, enabled=False):
            B, D, H, W = inputs.shape
            x = inputs.permute(0, 2, 3, 1).contiguous()
            groups = x.view(B, H, W, self.G, self.d)

            quantized = torch.empty_like(groups)
            all_indices = torch.empty(B, H, W, self.G, dtype=torch.long,
                                    device=inputs.device)
            total_bits = inputs.new_zeros(())
            commitment = inputs.new_zeros(())
            stat_dist = inputs.new_zeros(())

            for g in range(self.G):
                zg = groups[..., g, :].reshape(-1, self.d)
                cb = self.embed[g]

                dist = (zg.pow(2).sum(1, keepdim=True)
                        - 2 * zg @ cb.t()
                        + cb.pow(2).sum(1))

                logp = self._prior_logp(g)
                if self.rate_beta:
                    # rate_beta trades nats against a squared error in ABSOLUTE
                    # units, so its effective strength depends on the scale of the
                    # latents. rate_beta_norm divides that out.
                    beta = self.rate_beta
                    if self.rate_beta_norm:
                        beta = beta * dist.min(1).values.mean().detach()
                    cost = dist - beta * logp.unsqueeze(0)
                else:
                    cost = dist
                idx = cost.argmin(1)

                enc = F.one_hot(idx, self.K).type_as(zg)
                zq = enc @ cb

                bits = -(logp[idx] / math.log(2.0))
                total_bits = total_bits + bits.sum()
                commitment = commitment + F.mse_loss(zq.detach(), zg, reduction="mean")
                stat_dist = stat_dist + dist.min(1).values.mean().detach()

                # Usage tracking is a TRAINING diagnostic. Gating on self.training
                # keeps validation passes from polluting the histogram the
                # utilization log reads.
                if self.track_usage and self.training:
                    self.code_usage[g] += torch.bincount(
                        idx, minlength=self.K).to(self.code_usage.dtype)

                if self.training:
                    self._ema_update(g, enc, zg, idx)

                zq_st = zg + (zq - zg).detach()
                quantized[..., g, :] = zq_st.view(B, H, W, self.d)
                all_indices[..., g] = idx.view(B, H, W)

            quantized = quantized.view(B, H, W, D).permute(0, 3, 1, 2).contiguous()
            loss = self.commitment_cost * commitment
            bits_per_token = total_bits / (B * H * W)
            self._stat_dist = stat_dist / self.G
            self._stat_bits = (bits_per_token / self.G).detach()
            return quantized, loss, all_indices, bits_per_token

    # ----------------------------------------------------------------- lookup
    @torch.no_grad()
    def lookup(self, indices):
        """Indices -> quantized latents. The exact inverse of `forward`'s layout.

        `[B,H,W,G] -> [B,D,H,W]`, which is what the decoder consumes.

        This has to mirror `forward` term for term. There, channel `c` of the
        output comes from group `c // d`, dimension `c % d` -- because `forward`
        fills a `[B,H,W,G,d]` buffer and then does `view(B,H,W,D)`, so `G` is the
        outer of the two collapsed axes. Getting that transposed would not raise;
        it would silently reconstruct a different image, which is why the
        round-trip test in `tests/test_codec.py` compares against `forward`
        rather than checking shapes.

        Used by `NeuralImageCodec.decompress`, and separate from `forward` because
        the decode side has no encoder and no input latents -- it has integers off
        the wire and nothing else.

        This gathers rows; `forward` instead computes `one_hot(idx) @ codebook`,
        because it needs the one-hot for `_ema_update` anyway. Mathematically the
        same vector, but a different float accumulation order, so the two differ
        in the last bit or two (~6e-8). That is why the round-trip test compares
        indices exactly and pixels with a tolerance. Note the gather is also what
        the deployed decoder would do, and what the edge budget charges --
        the `K x d` matmul is a training-time artifact, not part of the codec.
        """
        if indices.dim() != 4 or indices.shape[-1] != self.G:
            raise ValueError(f"expected [B,H,W,{self.G}] indices, "
                             f"got {tuple(indices.shape)}")
        B, H, W, G = indices.shape
        idx = indices.to(self.embed.device).long()
        out = self.embed.new_empty((B, H, W, G, self.d))
        for g in range(G):
            out[..., g, :] = self.embed[g][idx[..., g]]
        return out.view(B, H, W, G * self.d).permute(0, 3, 1, 2).contiguous()

    @torch.no_grad()
    def prior_counts(self):
        """The frozen prior as per-group counts, `[G,K]`.

        `cluster_size + eps`, which is exactly what `_prior_logp` normalizes and
        what `vqlic.metrics.prior_cross_entropy_bpp` charges against. The entropy
        coder has to agree with both or the measured payload would not be
        comparable to the estimate it replaces.
        """
        return (self.cluster_size + self.eps).detach().cpu().numpy()

    # ----------------------------------------------- INT8 codebook / int search
    @torch.no_grad()
    def quantize_codebook(self, bits=8):
        """Quantize the codebook to `bits`-bit symmetric integers, IN PLACE.

        Only meaningful on a FROZEN codebook: `_ema_update` would immediately pull
        the centroids back off the integer lattice, so the caller must have put
        this module in `.eval()` first (which is what gates the EMA). Called on a
        live training codebook this would be silently undone.

        One scale per codebook GROUP, `max|embed[g]| / (2^(bits-1) - 1)`, and
        symmetric (no zero point). Both choices are about what the deployed argmin
        has to do rather than about reconstruction error:

        * **per-group, not per-code.** A per-code scale fits each centroid a
          little tighter, but then comparing two codes' costs means multiplying
          each by its own scale -- a fixed-point multiply per code inside the
          argmin. With one scale per group the scale is common to every candidate
          and divides out of the comparison entirely, so the argmin is a pure
          int32 compare. The rows of a trained codebook have similar norms
          anyway, so the tighter fit buys very little.
        * **symmetric.** An asymmetric codebook would put a zero point inside
          `|c_k|^2`, which is the term already being precomputed; keeping it
          symmetric leaves that constant exact.

        `embed` is REPLACED by the dequantized values rather than kept alongside
        the integer table. That is the whole trick: `lookup()`,
        `NeuralImageCodec.compress`, `decompress`, the decoder's input and this
        module's own float search then all see the same vectors with no further
        code changes, so the encoder and decoder cannot drift apart by one of them
        being taught about the quantization and another not. The integer table in
        `cb_int8` is an exact re-encoding of what `embed` now holds, not a second
        source of truth.

        Idempotent: the values are already on the lattice and `max|embed|` is
        unchanged, so a second call is a no-op. `tests/test_qat.py` asserts that.

        Returns a dict of stats for the log.
        """
        if self.training:
            raise RuntimeError(
                "quantize_codebook() requires eval mode: the EMA update in "
                "forward() runs on every training forward and would pull the "
                "centroids straight back off the integer lattice. Call "
                "quantizer.eval() first -- that is also what freezes the "
                "codebook for QAT.")
        if bits < 2 or bits > 8:
            raise ValueError(f"bits must be in [2, 8], got {bits}")

        qmax = 2 ** (bits - 1) - 1
        before = self.embed.clone()
        # amax over (K, d) -> one scale per group.
        amax = self.embed.abs().amax(dim=(1, 2)).clamp_min(1e-12)
        scale = amax / qmax
        q = torch.round(self.embed / scale.view(-1, 1, 1)).clamp_(-qmax, qmax)

        self.embed.copy_(q * scale.view(-1, 1, 1))
        # ema_w is embed * cluster_size by construction (see _ema_update). Nothing
        # reads it while the codebook is frozen, but leaving it describing the
        # PRE-quantization centroids means a later unfreeze would snap every
        # centroid back on its first EMA step.
        self.ema_w.copy_(self.embed * self.cluster_size.unsqueeze(-1))

        self.cb_int8 = q.to(torch.int8)
        self.cb_scale = scale.clone()
        self.cb_bits = int(bits)
        self.cb_quantized = True
        self.cb_bias = None          # invalid until set_latent_quant() runs

        rel = ((self.embed - before).pow(2).sum()
               / before.pow(2).sum().clamp_min(1e-12))
        return {"bits": int(bits), "scale": [float(v) for v in scale],
                "rel_sq_err": float(rel),
                "max_abs": [float(v) for v in amax]}

    @torch.no_grad()
    def set_latent_quant(self, scale, zero_point, frac_bits=None):
        """Record the encoder's output quantization params and fuse the int32 bias.

        This is what makes the assignment search integer end to end. `forward`
        picks `argmin_k(|z|^2 - 2 z.c_k + |c_k|^2 - beta*log p_k)`. The `|z|^2`
        term is the same for every k, so it drops out of the argmin. Substituting
        `z = s_z (q_z - zp)` and `c_k = s_c q_k` and dividing through by the
        positive constant `2 s_z s_c` (which cannot change an argmin) leaves

            argmin_k ( bias_k - q_z.q_k )

            bias_k = s_c |q_k|^2 / (2 s_z)  +  zp * sum(q_k)
                     - beta * log p_k / (2 s_z s_c)

        so the ONLY per-token work is an integer dot product; everything else is
        a constant vector computed once, here. Three things worth noting:

        * **the ECVQ rate tilt is free.** `-beta*log p_k` is per-code and
          token-independent, so it folds into the same constant that `|c_k|^2`
          already required. Keeping ECVQ therefore costs the deployed argmin
          nothing, and dropping it would change the assignment rule the frozen
          codebook and the decoder were trained under.
        * **the activation zero point is free too.** `q_z` is quint8 (see
          `vqlic.qat`), so the cross term `-2 s_z s_c * zp * sum(q_k)` appears --
          but `sum(q_k)` is again per-code, so it joins the same constant.
        * **`rate_beta_norm` must be off.** With it on, `beta` is rescaled every
          batch by the mean nearest-neighbour error, so `bias_k` would not be a
          constant at all and there would be nothing to precompute. All four
          archived rungs were trained with it False.

        `frac_bits` is the fixed-point precision of `bias_k`; the comparison is
        then `bias_k - (q_z.q_k << frac_bits)`. Left as None it is chosen to be
        as large as the int32 budget allows -- see `CB_BIAS_FRAC_BITS_CAP` for
        the measurement that motivates spending precision here at all.

        Call this AFTER the fake-quant observers are frozen -- `bias_k` is a
        function of `s_z` and `zp`, so a bias computed while the observers are
        still moving describes quantization params the deployed encoder will not
        have. `vqlic.qat` sequences it that way.
        """
        if not self.cb_quantized:
            raise RuntimeError("call quantize_codebook() before set_latent_quant()")
        if self.rate_beta and self.rate_beta_norm:
            raise RuntimeError(
                "set_latent_quant() needs rate_beta_norm=False: with it on, "
                "beta is rescaled by each batch's mean quantization error, so "
                "the ECVQ tilt is not a per-code constant and cannot be folded "
                "into a precomputed bias. All four archived rungs are False.")

        s_z = float(scale)
        zp = int(zero_point)
        if s_z <= 0:
            raise ValueError(f"latent scale must be positive, got {s_z}")

        # The int8 tables are plain attributes, not buffers, so `.to(device)` on
        # this module does not move them -- and after `apply_qat_payload` they
        # arrive from a checkpoint on CPU while `cluster_size` is on the training
        # device. Pin everything to the prior's device rather than assuming.
        dev = self.cluster_size.device
        qi = self.cb_int8.to(dev).to(torch.int64)
        s_c = self.cb_scale.to(dev).to(torch.float64)
        norm2 = qi.pow(2).sum(-1).to(torch.float64)               # [G,K]
        row_sum = qi.sum(-1).to(torch.float64)                    # [G,K]

        bias = norm2 * (s_c / (2.0 * s_z)).unsqueeze(1) + zp * row_sum
        if self.rate_beta:
            logp = torch.stack([self._prior_logp(g) for g in range(self.G)])
            bias = bias - (self.rate_beta * logp.to(dev).to(torch.float64)
                           / (2.0 * s_z * s_c.unsqueeze(1)))

        # Worst case the deployed comparison has to represent. `q_z` is quint8, so
        # |q_z.q_k| <= 255 * max_k sum_j|q_kj|, and the two terms of
        # `bias_k - (q_z.q_k << F)` can have opposite signs, so they add.
        lim = 2 ** 31 - 1
        dot_max = 255.0 * float(qi.abs().sum(-1).max())
        span = float(bias.abs().max()) + dot_max
        if frac_bits is None:
            budget = lim / CB_INT32_SAFETY
            frac_bits = int(math.floor(math.log2(max(budget / max(span, 1e-9),
                                                     1.0))))
            frac_bits = max(0, min(CB_BIAS_FRAC_BITS_CAP, frac_bits))
        frac_bits = int(frac_bits)

        self.cb_bias = torch.round(bias * (1 << frac_bits)).to(torch.int64)
        self.cb_bias_frac_bits = frac_bits
        self.cb_latent_scale = s_z
        self.cb_latent_zero_point = zp

        worst = int(math.ceil(span * (1 << frac_bits)))
        if worst > lim:
            raise OverflowError(
                f"the worst-case assignment comparison needs {worst:,} > int32 "
                f"max, so a deployment targeting int32 would wrap. Pass a "
                f"smaller frac_bits, or lower rate_beta.")
        return {"latent_scale": s_z, "latent_zero_point": zp,
                "bias_frac_bits": frac_bits,
                "bias_absmax": int(self.cb_bias.abs().max()),
                "compare_absmax": worst, "int32_headroom": lim / max(worst, 1)}

    @torch.no_grad()
    def quantize_latents(self, latents):
        """Float latents -> the quint8 codes `q_z` the deployed encoder emits.

        `[B,D,H,W]` float -> `[B,D,H,W]` int64. Uses the params recorded by
        `set_latent_quant`, so this is the same mapping the converted INT8
        encoder applies at its output. Provided so a test can drive `int_search`
        from ordinary float latents rather than having to reach inside a
        converted FX graph.
        """
        if not self.cb_quantized or self.cb_bias is None:
            raise RuntimeError("call quantize_codebook() and set_latent_quant() first")
        q = torch.round(latents.detach().float() / self.cb_latent_scale)
        return (q + self.cb_latent_zero_point).clamp_(0, 255).to(torch.int64)

    @torch.no_grad()
    def int_search(self, q_latents):
        """Pure-integer assignment. `[B,D,H,W]` quint8 codes -> `[B,H,W,G]` indices.

        The deployed encoder-side search, and the reference `tests/test_qat.py`
        checks `forward` against: same layout, same grouping, same tie-breaking
        (`argmin` takes the lowest index), but no float anywhere.

        Accumulated in int64 on whatever device the input is on, because torch has
        no integer matmul on CUDA -- but every intermediate is asserted to fit
        int32, which is the claim that matters for the hardware. At G=4, K=256,
        d=16, quint8 activations and int8 codes the raw dot product is bounded by
        255 * 127 * 16 = 518,160, and `set_latent_quant` chose the fixed-point
        shift so that the full comparison keeps a factor of `CB_INT32_SAFETY`
        below 2^31. The check below is what would catch a codebook shape, a
        `frac_bits` override or a `rate_beta` that ate that margin anyway.
        """
        if not self.cb_quantized or self.cb_bias is None:
            raise RuntimeError("call quantize_codebook() and set_latent_quant() first")
        if q_latents.dim() != 4:
            raise ValueError(f"expected [B,D,H,W], got {tuple(q_latents.shape)}")
        B, D, H, W = q_latents.shape
        if D != self.G * self.d:
            raise ValueError(f"expected D={self.G * self.d}, got {D}")

        dev = q_latents.device
        cb = self.cb_int8.to(dev).to(torch.int64)
        bias = self.cb_bias.to(dev)

        # Same permute/view chain as forward, so group g really is channels
        # [g*d, (g+1)*d) and these indices are interchangeable with forward's.
        x = q_latents.to(torch.int64).permute(0, 2, 3, 1).contiguous()
        groups = x.view(B, H, W, self.G, self.d)

        out = torch.empty(B, H, W, self.G, dtype=torch.long, device=dev)
        lim = 2 ** 31 - 1
        for g in range(self.G):
            qz = groups[..., g, :].reshape(-1, self.d)
            dot = qz @ cb[g].t()                                  # [N,K] int64
            cost = bias[g].unsqueeze(0) - (dot << self.cb_bias_frac_bits)
            worst = int(cost.abs().max())
            if worst > lim:
                raise OverflowError(
                    f"group {g}: |cost| reached {worst:,} > int32 max. The "
                    f"integer search this verifies would wrap on 32-bit "
                    f"hardware; rebuild the bias with a smaller frac_bits.")
            out[..., g] = cost.argmin(1).view(B, H, W)
        return out

    @torch.no_grad()
    def set_prior_counts(self, counts, floor=1.0):
        """Replace the frozen prior with freshly measured index counts.

        `counts` is `[G,K]`. Needed because QAT moves the encoder: the codebook is
        quantized and frozen, but the latents landing on it are not the ones the
        EMA prior was estimated from, and `vqlic.metrics.prior_cross_entropy_bpp` --
        the publishable rate, and what `scripts/eval.py` reports -- is exactly
        the cross-entropy of realised indices against this prior. A stale prior
        shows up as a rate regression that has nothing to do with INT8.

        Re-estimating is legitimate rather than a fudge: the prior is side
        information both peers hold, transmitted once with the model, not per
        image. What is NOT legitimate is moving the centroids, which is why this
        touches `cluster_size` only.

        Two details:

        * **the total mass per group is preserved.** The prior itself is
          scale-free (`_prior_logp` normalizes), but `cluster_size`'s absolute
          magnitude is what `dead_thresh` and `prior_stats()['live']` are
          calibrated against, so rescaling to the previous total keeps those
          readings comparable across the run.
        * **add-one smoothing** (`floor`). A code that happened to win zero
          tokens over the measurement pass would otherwise get probability
          `eps/total`, i.e. ~17 bits, and any test image that does hit it pays
          that. `codec.compress` has a raw-mode fallback for exactly this
          pathology, but it is better not to manufacture it: one count per code
          over a pass of tens of millions of tokens is numerically invisible and
          removes the cliff.
        """
        counts = torch.as_tensor(counts, dtype=torch.float64,
                                 device=self.cluster_size.device)
        if tuple(counts.shape) != (self.G, self.K):
            raise ValueError(f"expected counts [{self.G},{self.K}], "
                             f"got {tuple(counts.shape)}")
        if float(counts.sum()) <= 0:
            raise ValueError("counts are all zero -- the measurement pass saw "
                             "no tokens")

        old_mass = self.cluster_size.to(torch.float64).sum(dim=1, keepdim=True)
        # A freshly built quantizer has cluster_size all zeros and thus no mass to
        # preserve; fall back to the observed token count for something sane.
        old_mass = torch.where(old_mass > 0, old_mass,
                               counts.sum(dim=1, keepdim=True).clamp_max(1e4))
        c = counts + float(floor)
        new = c / c.sum(dim=1, keepdim=True) * old_mass

        before = self.cluster_size.clone()
        self.cluster_size.copy_(new.to(self.cluster_size.dtype))
        self.ema_w.copy_(self.embed * self.cluster_size.unsqueeze(-1))

        def _ent(cs):
            p = cs.to(torch.float64) + self.eps
            p = p / p.sum(dim=1, keepdim=True)
            return float(-(p * torch.log2(p.clamp_min(1e-12))).sum())

        return {"entropy_before": _ent(before), "entropy_after": _ent(new),
                "live_before": int((before > self.dead_thresh).sum()),
                "live_after": int((self.cluster_size > self.dead_thresh).sum()),
                "tokens": int(counts.sum())}

    # ------------------------------------------------------------- EMA update
    @torch.no_grad()
    def _ema_update(self, g, enc, zg, idx):
        cs = enc.sum(0)
        self.cluster_size[g].mul_(self.decay).add_(cs, alpha=1 - self.decay)
        n = self.cluster_size[g].sum()
        self.cluster_size[g] = ((self.cluster_size[g] + self.eps)
                                / (n + self.K * self.eps) * n)
        dw = enc.t() @ zg
        self.ema_w[g].mul_(self.decay).add_(dw, alpha=1 - self.decay)
        self.embed[g] = self.ema_w[g] / self.cluster_size[g].unsqueeze(1)

        dead = self.cluster_size[g] < self.dead_thresh
        n_dead = int(dead.sum())
        if n_dead == 0 or zg.shape[0] < n_dead:
            return

        # ------------------------- THE COLD-START FIX ------------------------
        # WHERE to re-seed from.
        #   Uniform random draws almost always land in a dense region already
        #   owned by a live neighbour: the new centroid wins ~0 assignments and,
        #   with rate_beta > 0, additionally carries a -beta*log p handicap for
        #   being rare, so it dies again immediately. Drawing from the
        #   worst-quantized tail puts it where the live codebook actually has
        #   error. A random pick WITHIN that tail keeps the n_dead new centroids
        #   from all collapsing onto the same outlier.
        err = (zg - self.embed[g][idx]).pow(2).sum(1)
        pool = min(max(n_dead, int(self.revive_pool_frac * zg.shape[0])),
                   zg.shape[0])
        cand = torch.topk(err, pool).indices
        samp = zg[cand[torch.randperm(pool, device=zg.device)[:n_dead]]]

        # HOW MUCH MASS to give it.
        #   base_v3 set cluster_size to exactly dead_thresh (1.0). One EMA step
        #   later that is 0.99*1.0 + 0.01*hits, so a revived code had to win >= 2
        #   of ~12.5k tokens in the very NEXT batch or it was discarded and
        #   re-randomised -- every step, forever. That is why 3 of the 4 base
        #   codebooks in nic_v3_e1_14999.pt ended at 12/30/18 live entries out of
        #   256, with the rest pinned at exactly cluster_size == 1.0.
        #   revive_size buys ~ln(revive_size)/ln(1/decay) ~= 230 steps of grace
        #   at decay=0.99 for the new centroid to attract a basin.
        self.embed[g][dead] = samp
        self.ema_w[g][dead] = samp * self.revive_size
        self.cluster_size[g][dead] = self.revive_size

    # ------------------------------------------------- utilization / reporting
    @torch.no_grad()
    def reset_usage(self):
        self.code_usage.zero_()

    @torch.no_grad()
    def quick_util(self):
        active = (self.code_usage > 0).float().sum()
        return (active / (self.G * self.K)).item()

    @torch.no_grad()
    def utilization_stats(self):
        """Utilization from the observed assignment histogram. Empty until some
        training batches have run -- see `prior_stats` for a version readable at
        any time."""
        per = []
        for g in range(self.G):
            u = self.code_usage[g]
            tot = u.sum()
            active = int((u > 0).sum())
            if tot > 0:
                p = u / tot
                ent = -(p * torch.log(p + 1e-12)).sum()
                ppl = float(torch.exp(ent))
            else:
                ppl = 0.0
            per.append({"codebook": g, "active": active, "dead": self.K - active,
                        "util_pct": 100.0 * active / self.K,
                        "perplexity": ppl, "ppl_norm": ppl / self.K})
        active_all = sum(p["active"] for p in per)
        agg = {"active": active_all, "total": self.G * self.K,
               "util_pct": 100.0 * active_all / (self.G * self.K),
               "mean_perplexity": float(np.mean([p["perplexity"] for p in per]))}
        return {"per_codebook": per, "overall": agg}

    @torch.no_grad()
    def prior_stats(self):
        """Codebook health straight from the EMA prior. Readable without having
        run any batches, unlike code_usage -- so it works on a fresh load.

        `bits_per_token` here is the ENTROPY of the prior. The rate an entropy
        coder actually pays is the CROSS-ENTROPY of realised indices against it,
        which is higher on out-of-distribution data. Do not quote this as a bpp.
        """
        per = []
        for g in range(self.G):
            raw = self.cluster_size[g]
            # `+ eps` matches _prior_logp, and keeps a freshly initialized model
            # (cluster_size all zeros) from reporting nan instead of a uniform prior.
            cs = raw + self.eps
            p = cs / cs.sum()
            ent = float(-(p * torch.log2(p + 1e-12)).sum())
            live = int((raw > self.dead_thresh).sum())
            per.append({"codebook": g, "live": live, "K": self.K,
                        "live_pct": 100.0 * live / self.K, "entropy_bits": ent})
        return {"per_codebook": per,
                "bits_per_token": sum(p["entropy_bits"] for p in per)}

    @torch.no_grad()
    def rate_pressure(self):
        """Size of the ECVQ rate tilt RELATIVE to the distortion it competes
        with, from the last forward's statistics.

        This is the number that explains a collapsing codebook: once
        -rate_beta*log p is comparable to the nearest-neighbour error, assignment
        stops being about geometry and the prior feeds back on itself -- rich
        codes get cheaper, so they get richer. Returns 0.0 before the first
        forward or when rate_beta == 0.
        """
        if self._stat_dist is None or not self.rate_beta:
            return 0.0
        dist = float(self._stat_dist)
        beta = self.rate_beta * (dist if self.rate_beta_norm else 1.0)
        rate_cost = beta * math.log(2.0) * float(self._stat_bits)
        return rate_cost / max(dist, 1e-12)


def format_util(stats):
    """Render `utilization_stats()` as the multi-line block base_v3 printed."""
    lines = []
    for p in stats["per_codebook"]:
        lines.append(f"  CB{p['codebook']}: {p['active']:>3}/{p['active']+p['dead']:<3} "
                     f"active ({p['util_pct']:5.1f}%) | dead {p['dead']:>3} "
                     f"| perplexity {p['perplexity']:6.1f}")
    o = stats["overall"]
    lines.append(f"  OVERALL: {o['util_pct']:.1f}% active "
                 f"| mean perplexity {o['mean_perplexity']:.1f}")
    return "\n".join(lines)
