"""End-to-end codec pipeline, the payload format, and the padding helpers."""
from __future__ import annotations

import math
import struct

import numpy as np
import torch
import torch.nn as nn
import torch.nn.functional as F

from vqlic.decoder import SelfAttentionDecoder
from vqlic.encoder import ImageEncoder
from vqlic.context import (context_at, context_ids, rans_decode_ctx,
                         rans_encode_ctx)
from vqlic.entropy import (bitmap_size, pack_bitmap, pack_uints, packed_size,
                         quantize_freqs, quantize_freqs_support, rans_decode,
                         rans_encode, unpack_bitmap, unpack_uints)
from vqlic.quantizer import MultiCodebookEMAQuantizer

# The encoder downsamples by 8. The quantizer reports bits per LATENT TOKEN, so
# converting to bits per pixel divides by this squared.
DOWNSAMPLE = 8

# ----------------------------------------------------------- payload format
#
# 17-byte little-endian header, then the body. Self-describing on purpose:
# `decompress` takes a payload and a model and needs nothing else -- no
# out-of-band width, no remembering which flag the encoder ran with.
_MAGIC = b"NIC1"
_HEADER = "<4sBBHBII"          # magic, mode, G, K, prob_bits, H, W
HEADER_BYTES = struct.calcsize(_HEADER)

MODE_RAW = 0                   # fixed-length indices, no coder
MODE_PRIOR = 1                 # coded against the frozen EMA prior
MODE_TABLE = 2                 # coded against a per-image table, table included
MODE_CONTEXT = 3               # coded against a shipped neighbour-conditioned model

MODE_NAMES = {MODE_RAW: "raw", MODE_PRIOR: "prior", MODE_TABLE: "table",
              MODE_CONTEXT: "context"}

# Frequency resolution, per mode, because the two modes pay for it differently.
#
# PRIOR never transmits its table -- the decoder rebuilds it from the model's own
# `cluster_size` -- so resolution there is FREE and there is no reason to be
# stingy. Measured on 4 CLIC images: 12 -> 16 bits takes the bpp010 rung from
# 0.10077 to 0.09837 bpp (-2.4%) and bpp030 from 0.23098 to 0.22975 (-0.5%), for
# zero extra bytes. 14 and 16 are indistinguishable, and 16 is the ceiling a
# 32-bit rANS state with byte renormalization allows, so take 16.
#
# TABLE pays `prob_bits` for every occupied entry, so resolution is a real trade
# there. Measured at bpp010: sparse tables cost 0.07942 bpp at 12 bits and
# 0.07946 at 16 -- quantization loss is already down to +0.00006 bpp at 12, so
# the extra bits buy nothing and cost side information. Stay at 12.
PROB_BITS_PRIOR = 16
PROB_BITS_TABLE = 12


def payload_info(payload):
    """Header fields plus total size, without decoding anything.

    Lets the bench report which mode each image landed in, and its rate, without
    paying for a decoder pass.
    """
    if len(payload) < HEADER_BYTES:
        raise ValueError(f"payload of {len(payload)} bytes is shorter than the "
                         f"{HEADER_BYTES}-byte header")
    magic, mode, G, K, prob_bits, H, W = struct.unpack(
        _HEADER, payload[:HEADER_BYTES])
    if magic != _MAGIC:
        raise ValueError(f"bad magic {magic!r}, expected {_MAGIC!r}")
    if mode not in MODE_NAMES:
        raise ValueError(f"unknown mode {mode}")
    h, w = token_grid(H, W)
    return {"mode": mode, "mode_name": MODE_NAMES[mode], "num_codebooks": G,
            "codebook_size": K, "prob_bits": prob_bits, "height": H, "width": W,
            "tokens": h * w, "bytes": len(payload),
            "bpp": 8.0 * len(payload) / float(H * W)}


def token_grid(height, width, downsample=DOWNSAMPLE):
    """Token grid for an image, matching what `pad_to_multiple` produces.

    Derived rather than stored in the header: the encoder is a fixed /8, so the
    grid is a function of the image size and storing it would let a payload
    disagree with itself.
    """
    return (-(-height // downsample), -(-width // downsample))


class NeuralImageCodec(nn.Module):
    """image -> encoder -> product VQ -> windowed transformer decoder -> recon.

    forward() returns (recon, commit_loss, indices, bits_per_token).

    The 4th value is bits per latent token, matching what the quantizer reports;
    `bits_to_bpp` converts it. Note that `lambda_rate` in the loss multiplies this
    bits-per-token figure rather than bpp -- a factor of 64 at /8 -- which is the
    historical meaning of that hyperparameter. See vqlic/losses.py.
    """

    def __init__(self, embedding_dim=64, latent_dim=256, num_codebooks=4,
                 codebook_size=256, rate_beta=0.0, in_channels=3,
                 nhead=8, num_layers=6, window_size=14,
                 commitment_cost=0.25, decay=0.99, dead_thresh=1.0,
                 revive_size=None, revive_pool_frac=None, rate_beta_norm=None):
        super().__init__()
        self.encoder = ImageEncoder(in_channels=in_channels,
                                    embedding_dim=embedding_dim)
        self.quantizer = MultiCodebookEMAQuantizer(
            embedding_dim=embedding_dim, num_codebooks=num_codebooks,
            codebook_size=codebook_size, rate_beta=rate_beta,
            commitment_cost=commitment_cost, decay=decay,
            dead_thresh=dead_thresh, revive_size=revive_size,
            revive_pool_frac=revive_pool_frac, rate_beta_norm=rate_beta_norm)
        self.decoder = SelfAttentionDecoder(
            embedding_dim=embedding_dim, latent_dim=latent_dim,
            out_channels=in_channels, nhead=nhead, num_layers=num_layers,
            window_size=window_size)
        # Set by `attach_context`. Not a submodule and not in the state_dict:
        # the tables are integer side information fitted after training and
        # shipped beside the checkpoint, not parameters. `None` keeps the codec
        # at exactly the three modes it had before.
        self.context = None

    def attach_context(self, model):
        """Ship a `vqlic.context.ContextModel` with this codec, or `None` to stop.

        Once attached, `compress` codes against it instead of the frozen EMA
        prior, and `decompress` requires the SAME model -- the payload carries
        no table, so a peer holding different tables decodes different indices
        with nothing in the stream to notice. The header's `prob_bits` is the
        one cross-check that survives, and `decompress` enforces it.
        """
        if model is not None:
            q = self.quantizer
            if (model.G, model.K) != (q.G, q.K):
                raise ValueError(
                    f"context model is G={model.G} K={model.K} but this codec "
                    f"is G={q.G} K={q.K}")
        self.context = model
        return self

    def forward(self, image):
        latents = self.encoder(image)
        quantized, vq_loss, indices, bits_per_token = self.quantizer(latents)
        recon = self.decoder(quantized)
        return recon, vq_loss, indices, bits_per_token

    @staticmethod
    def bits_to_bpp(bits_per_token, downsample=DOWNSAMPLE):
        """bits/latent-token -> bits/pixel."""
        return bits_per_token / float(downsample * downsample)

    # ------------------------------------------------------ compress / decompress
    @torch.no_grad()
    def compress(self, image, build_table=False, prob_bits=None):
        """One image -> one payload of bytes. The whole encode side.

        `image` is a float tensor `[1,3,H,W]` or `[3,H,W]` in **[-1, 1]** -- the
        normalization `vqlic/dataset.py` and `vqlic/engine.py` already use. Tensors
        only, deliberately: no PIL anywhere in this path, so it is usable from a
        dataloader, from a test, or from something that never touches a file.

        `build_table=False` codes the indices against the quantizer's frozen EMA
        prior. Nothing is transmitted about the distribution, but the payload is
        then only decodable by a peer holding this exact codebook state.

        `build_table=True` fits the histogram of *this image's* indices and
        transmits it alongside, trading side information for the gap between
        cross-entropy against a corpus prior and this image's own entropy.

        The table is **sparse**: a `K`-bit occupancy map per group, then a
        frequency only for the codes that occur. That is not a micro-optimization.
        An image touches ~65 of the 1024 codes at the bpp010 rung, so a dense
        table sends 959 useless entries and -- far worse -- must give each of them
        a frequency of at least 1, taxing every symbol that does occur. Measured
        on 4 CLIC images at bpp010 against an own-entropy of 0.07858 bpp: dense
        costs +0.00890 bpp of quantization loss plus 0.00445 bpp of table, for a
        0.09197 bpp payload; sparse costs +0.00006 plus 0.00074, for 0.07942 --
        a **13.6% lower rate**, and the gap widens as the rung drops. At 224x224,
        where the table amortizes over 784 tokens instead of 44k, it is the
        difference between 0.340 and 0.116 bpp.

        Whether the table beats the prior at all still depends on the token
        count, and the mode is recorded in the header so the bench measures which
        way it went instead of assuming.

        Either way, if the coded body comes out no smaller than fixed-length
        indices, the fixed-length form is emitted instead and the mode says
        `raw`. Coding CAN lose: cross-entropy against a mismatched prior is
        `H(p) + KL(p||q)` and unbounded above, so a token landing on a code the
        prior thinks is vanishingly rare costs far more than `log2(K)` bits. This
        replaces clipping the reported rate at a ceiling after the fact -- the
        decision belongs in the codec, where it can actually change the
        bitstream.
        """
        if self.training:
            raise RuntimeError(
                "compress() requires eval mode. The quantizer runs its EMA "
                "update and dead-code resampling on every forward while "
                "training, and torch.no_grad() does not prevent it -- so "
                "compressing here would move the codebook and desynchronize "
                "every payload written before it. Call model.eval() first.")

        payload, _ = self._compress(image, build_table, prob_bits)
        return payload

    @torch.no_grad()
    def compress_stats(self, image, build_table=False, prob_bits=None):
        """`(payload, stats)` -- `compress` plus what the payload cost and why.

        Exists for the benchmark, which reports the measured rate but also wants
        the two estimates it replaces (`bpp_prior`, `bpp_empirical`) side by side
        in the same JSON. Computing them here costs nothing, because the indices
        are already in hand; making the bench call `forward` again to get them
        would double the encode work.
        """
        if self.training:
            raise RuntimeError("compress_stats() requires eval mode; see compress()")
        return self._compress(image, build_table, prob_bits)

    def _prepare(self, image):
        """`image` -> (padded batch, H, W, ph, pw), with the input checks."""
        x = image
        if x.dim() == 3:
            x = x[None]
        if x.dim() != 4:
            raise ValueError(f"expected [1,3,H,W] or [3,H,W], got {tuple(x.shape)}")
        if x.shape[0] != 1:
            raise ValueError(
                f"one payload is one image; got a batch of {x.shape[0]}. Loop "
                f"over the batch -- the header carries a single H and W, and a "
                f"batched payload would not be independently decodable.")
        _, _, H, W = x.shape
        x = x.to(self.quantizer.embed.device).float()
        xp, (ph, pw) = pad_to_multiple(x, DOWNSAMPLE)
        return xp, H, W, ph, pw

    def _compress(self, image, build_table, prob_bits):
        ctx = None if build_table else self.context
        if prob_bits is None:
            # A context model carries its own resolution: its tables were
            # renormalized to `1 << prob_bits` at fit time and cannot be
            # requantized here without both ends disagreeing.
            prob_bits = (ctx.prob_bits if ctx is not None
                         else (PROB_BITS_TABLE if build_table else PROB_BITS_PRIOR))
        elif ctx is not None and prob_bits != ctx.prob_bits:
            raise ValueError(
                f"prob_bits={prob_bits} but the attached context model was "
                f"fitted at {ctx.prob_bits}; its integer tables cannot be "
                f"requantized without desynchronizing the decoder")
        xp, H, W, _ph, _pw = self._prepare(image)
        latents = self.encoder(xp)
        _, _, indices, _ = self.quantizer(latents)

        # Going through the quantizer's own forward rather than reimplementing
        # the assignment is what guarantees the payload holds the same indices
        # the RD curves were measured with -- ECVQ tilt, prior handicap and all.
        q = self.quantizer
        G, K = q.G, q.K
        idx = indices[0].cpu().numpy().astype(np.int64)      # [h,w,G]
        h, w = idx.shape[:2]
        n = h * w

        raw_bits = max(1, int(math.ceil(math.log2(K))))
        raw_bytes = packed_size(n * G, raw_bits)

        if ctx is not None:
            # Conditional coding. Nothing is transmitted: the decoder holds the
            # same tables and rebuilds each token's context from neighbours it
            # has already decoded, so the conditioning costs zero bits.
            mode = MODE_CONTEXT
            table = b""
            streams = []
            for g in range(G):
                cids = context_ids(idx[..., g], K, ctx.order)
                streams.append(rans_encode_ctx(
                    idx[..., g].ravel(),
                    lambda c, _g=g: ctx.table(_g, c),
                    cids, prob_bits))
        elif build_table:
            mode = MODE_TABLE
            freqs, parts = [], []
            for g in range(G):
                counts = np.bincount(idx[..., g].ravel(), minlength=K)
                f, _support = quantize_freqs_support(counts, prob_bits)
                freqs.append(f)
                # Occupancy map, then one frequency per occupied code. No length
                # field is needed: the popcount of the map IS the count that
                # follows, and the decoder has K from the header.
                #
                # Stored as freq-1. An occupied frequency is in 1..(1<<prob_bits)
                # inclusive, which is prob_bits+1 values; the top of that range
                # is reached whenever a group's support is a single code, which
                # is not exotic -- at the bpp010 rung one group already uses only
                # 12 of its 256 codes, and a small crop can easily take it to
                # one. Biasing by 1 makes the range exactly prob_bits bits wide.
                parts.append(pack_bitmap(f > 0)
                             + pack_uints(f[f > 0] - 1, prob_bits))
            table = b"".join(parts)
        else:
            mode = MODE_PRIOR
            counts = q.prior_counts()
            freqs = [quantize_freqs(counts[g], prob_bits) for g in range(G)]
            table = b""

        if mode != MODE_CONTEXT:
            streams = [rans_encode(idx[..., g].ravel(), freqs[g], prob_bits)
                       for g in range(G)]
        body = table + b"".join(struct.pack("<I", len(s)) + s for s in streams)

        if len(body) >= raw_bytes:
            mode = MODE_RAW
            table = b""
            # Group-major so a decoder can pull one group's plane without
            # touching the others, matching the per-group streams above.
            body = pack_uints(idx.transpose(2, 0, 1).ravel(), raw_bits)

        header = struct.pack(_HEADER, _MAGIC, mode, G, K, prob_bits, H, W)
        payload = header + body

        stats = {
            "mode": mode, "mode_name": MODE_NAMES[mode],
            "payload_bytes": len(payload),
            "bpp": 8.0 * len(payload) / float(H * W),
            "table_bytes": len(table),
            "table_bpp": 8.0 * len(table) / float(H * W),
            "header_bytes": HEADER_BYTES,
            "raw_bytes": raw_bytes,
            "tokens": n,
        }
        # The two estimates this measurement replaces, so the JSON carries the
        # comparison rather than asking the reader to trust one of them. Called
        # from `vqlic.metrics` rather than reimplemented here: these are the exact
        # functions the RD curves were built on, so `bpp` vs `bpp_prior` is a
        # like-for-like check on the coder and not on two different definitions.
        #
        # Imported lazily because `vqlic.metrics` pulls in pytorch_msssim, and
        # nothing about compressing an image needs MS-SSIM.
        from vqlic.metrics import prior_cross_entropy_bpp, shannon_bpp_multi
        stats["bpp_prior"] = prior_cross_entropy_bpp(q, indices, H * W)
        stats["bpp_empirical"] = shannon_bpp_multi(indices, H * W)
        return payload, stats

    @torch.no_grad()
    def decompress(self, payload):
        """One payload -> `[1,3,H,W]` float tensor in [-1, 1].

        The decoded **indices are bit-identical** to the ones `forward` assigns
        to the source image, and that is the guarantee that matters: the payload
        carries integers, and nothing downstream of them is stochastic.

        The reconstruction agrees with `forward` to float32 epsilon (~6e-8 on
        values in [-1, 1]) rather than bit-exactly, for one specific reason:
        `MultiCodebookEMAQuantizer.forward` materializes the quantized vectors as
        `one_hot(idx) @ codebook`, because it needs that one-hot for the EMA
        update, while `lookup` gathers rows directly. The two are mathematically
        identical and differ only in float accumulation order. Matching them
        bit-for-bit would mean making `lookup` do a `K x d` matmul per token to
        reproduce a rounding artifact, which is 4096x the arithmetic for no gain.
        `tests/test_codec.py` asserts exact equality on the indices and a tight
        tolerance on the pixels.
        """
        info = payload_info(payload)
        q = self.quantizer
        if info["num_codebooks"] != q.G or info["codebook_size"] != q.K:
            raise ValueError(
                f"payload was written by a G={info['num_codebooks']}, "
                f"K={info['codebook_size']} model; this one is G={q.G}, K={q.K}")

        G, K = q.G, q.K
        H, W = info["height"], info["width"]
        prob_bits = info["prob_bits"]
        h, w = token_grid(H, W)
        n = h * w
        body = payload[HEADER_BYTES:]

        if info["mode"] == MODE_RAW:
            raw_bits = max(1, int(math.ceil(math.log2(K))))
            flat = unpack_uints(body, n * G, raw_bits)
            idx = flat.reshape(G, h, w).transpose(1, 2, 0)
        else:
            off = 0
            if info["mode"] == MODE_TABLE:
                freqs = []
                for _g in range(G):
                    mask = unpack_bitmap(body[off:off + bitmap_size(K)], K)
                    off += bitmap_size(K)
                    ns = int(mask.sum())
                    nb = packed_size(ns, prob_bits)
                    f = np.zeros(K, dtype=np.int64)
                    f[mask] = unpack_uints(body[off:off + nb], ns, prob_bits) + 1
                    off += nb
                    freqs.append(f)
            elif info["mode"] == MODE_CONTEXT:
                # Nothing to read: the tables are side information both peers
                # hold. Decoded below, per token, because each context depends
                # on neighbours that have not been decoded yet.
                ctx = self.context
                if ctx is None:
                    raise ValueError(
                        "payload is MODE_CONTEXT but no context model is "
                        "attached; call attach_context() with the same model "
                        "that encoded it")
                if ctx.prob_bits != prob_bits:
                    raise ValueError(
                        f"payload was coded at prob_bits={prob_bits} but the "
                        f"attached context model is {ctx.prob_bits}")
                if (ctx.G, ctx.K) != (G, K):
                    raise ValueError(
                        f"payload is G={G} K={K} but the attached context "
                        f"model is G={ctx.G} K={ctx.K}")
            else:
                # PRIOR mode rebuilds the table from the model's own buffers, so
                # the payload is only decodable by a peer holding this codebook
                # state. That is the cost of not transmitting it.
                counts = q.prior_counts()
                freqs = [quantize_freqs(counts[g], prob_bits) for g in range(G)]

            planes = []
            for g in range(G):
                (ln,) = struct.unpack("<I", body[off:off + 4])
                off += 4
                if info["mode"] == MODE_CONTEXT:
                    ctx = self.context
                    planes.append(rans_decode_ctx(
                        body[off:off + ln], n,
                        lambda t, dec, _w=w: context_at(t, dec, _w, K,
                                                        ctx.order),
                        lambda c, _g=g: ctx.table(_g, c),
                        prob_bits))
                else:
                    planes.append(rans_decode(body[off:off + ln], n, freqs[g],
                                              prob_bits))
                off += ln
            idx = np.stack(planes, axis=-1).reshape(h, w, G)

        indices = torch.from_numpy(np.ascontiguousarray(idx)).long()[None]
        quantized = q.lookup(indices)
        recon = self.decoder(quantized)
        ph, pw = h * DOWNSAMPLE - H, w * DOWNSAMPLE - W
        return unpad(recon, ph, pw)


def build_model(cfg):
    """Construct the codec from a `vqlic.config.Config`."""
    return NeuralImageCodec(
        embedding_dim=cfg.embedding_dim,
        latent_dim=cfg.latent_dim,
        num_codebooks=cfg.num_codebooks,
        codebook_size=cfg.codebook_size,
        rate_beta=0.0,                 # ramped by the trainer
        in_channels=3,
        nhead=cfg.nhead,
        num_layers=cfg.num_layers,
        window_size=cfg.window_size,
        commitment_cost=cfg.commitment_cost,
        decay=cfg.decay,
        dead_thresh=cfg.dead_thresh,
        revive_size=cfg.revive_size,
        revive_pool_frac=cfg.revive_pool_frac,
        rate_beta_norm=cfg.rate_beta_norm,
    )


def pad_to_multiple(x, multiple=DOWNSAMPLE):
    """Reflect-pad H and W up to the nearest multiple. Returns (padded, (ph, pw))."""
    _, _, h, w = x.shape
    ph = (multiple - h % multiple) % multiple
    pw = (multiple - w % multiple) % multiple
    # F.pad order is (left, right, top, bottom)
    return F.pad(x, (0, pw, 0, ph), mode="reflect"), (ph, pw)


def unpad(x, ph, pw):
    """Undo `pad_to_multiple`."""
    h, w = x.shape[-2], x.shape[-1]
    return x[..., :h - ph if ph > 0 else h, :w - pw if pw > 0 else w]
