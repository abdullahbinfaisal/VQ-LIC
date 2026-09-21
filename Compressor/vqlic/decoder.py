"""Server-side decoder: windowed transformer over the token grid, then synthesis.

Two things this fixes relative to base_v3.

**Resolution generalization.** base_v3 attended globally with absolute sinusoidal
position encoding computed for the training grid. Trained at 224 (28x28 = 784
tokens) and run on a 2048x1365 image (~43,520 tokens) that breaks twice: the
encoding is evaluated at positions never seen in training, and softmax over 55x
more keys dilutes attention toward the global mean. Both depend only on token
count, not image content, which is exactly the observed symptom -- 224 crops fine,
resized-to-224 fine, full resolution bad. It is also computationally hopeless: the
n^2 term goes from ~1.9 GMAC at 224 to ~5.8 TMAC at full resolution.

**Parameter allocation.** In base_v3, 94% of the decoder refined 784 tokens while
6% synthesized 150,528 pixels -- the final RGB projection was 432 parameters, and
there was no refinement at full resolution at all.
"""
from __future__ import annotations

import math

import torch
import torch.nn as nn
import torch.nn.functional as F


def sinusoidal_pos_2d(h, w, dim, device, dtype=torch.float32):
    """Fixed 2D sinusoidal position encoding, flattened to [h*w, dim].

    Half the channels encode the row index and half the column, each split again
    into sin/cos pairs -- hence `dim % 4 == 0`.
    """
    assert dim % 4 == 0, f"pos-enc dim must be divisible by 4, got {dim}"
    pe = torch.zeros(h, w, dim, device=device, dtype=dtype)
    d = dim // 2
    div = torch.exp(torch.arange(0, d, 2, device=device, dtype=dtype)
                    * (-math.log(10000.0) / d))
    y = torch.arange(h, device=device, dtype=dtype).unsqueeze(1)
    x = torch.arange(w, device=device, dtype=dtype).unsqueeze(1)
    pe[..., 0:d:2] = torch.sin(y * div).unsqueeze(1).expand(h, w, -1)
    pe[..., 1:d:2] = torch.cos(y * div).unsqueeze(1).expand(h, w, -1)
    pe[..., d::2] = torch.sin(x * div).unsqueeze(0).expand(h, w, -1)
    pe[..., d + 1::2] = torch.cos(x * div).unsqueeze(0).expand(h, w, -1)
    return pe.view(h * w, dim)


def window_partition(x, ws):
    """[B,H,W,C] -> [B*nW, ws*ws, C]. Window index row-major, batch outermost."""
    B, H, W, C = x.shape
    x = x.view(B, H // ws, ws, W // ws, ws, C)
    return x.permute(0, 1, 3, 2, 4, 5).contiguous().view(-1, ws * ws, C)


def window_reverse(win, ws, B, H, W):
    """Inverse of `window_partition`."""
    C = win.shape[-1]
    x = win.view(B, H // ws, W // ws, ws, ws, C)
    return x.permute(0, 1, 3, 2, 4, 5).contiguous().view(B, H, W, C)


class SynthesisHead(nn.Module):
    """/8 token features -> full-resolution RGB.

    Sub-pixel convolution rather than ConvTranspose2d (same cost, no checkerboard
    risk), a refinement block at every resolution, and two at full resolution.
    Channel ladder is latent//2 -> //4 -> //8, so at latent_dim=256 it runs
    128 @ /4, 64 @ /2, 32 @ /1 and totals ~1.75M parameters.

    Parameter count is not a constraint on the server side, and base_v3 badly
    under-spent here: a single transposed conv per 2x step, nothing at full
    resolution, and a 432-parameter final projection producing every pixel.
    """

    def __init__(self, latent_dim=256, out_channels=3):
        super().__init__()
        c1, c2, c3 = latent_dim // 2, latent_dim // 4, latent_dim // 8

        def up(cin, cout):
            return [nn.Conv2d(cin, cout * 4, 3, padding=1),
                    nn.PixelShuffle(2), nn.ReLU(inplace=True)]

        def refine(c, n=1):
            layers = []
            for _ in range(n):
                layers += [nn.Conv2d(c, c, 3, padding=1), nn.ReLU(inplace=True)]
            return layers

        self.net = nn.Sequential(
            *up(latent_dim, c1), *refine(c1),        # -> /4
            *up(c1, c2), *refine(c2),                # -> /2
            *up(c2, c3), *refine(c3, 2),             # -> /1
            nn.Conv2d(c3, out_channels, 3, padding=1),
            nn.Tanh(),                               # output in [-1, 1]
        )

    def forward(self, x):
        return self.net(x)


class SelfAttentionDecoder(nn.Module):
    """code space -> windowed transformer over tokens -> synthesis to full res.

    `window_size` is in TOKENS. 0 (or a value covering the whole grid) gives global
    attention. Any smaller value makes the operation resolution-invariant: at 224
    the 28x28 grid holds (28/ws)^2 windows, and at full resolution it holds more
    windows of *identical* size, so nothing the network sees leaves its training
    distribution. Windows shift by half their width on odd layers so information
    crosses window boundaries.
    """

    def __init__(self, embedding_dim=64, latent_dim=256, out_channels=3,
                 nhead=8, num_layers=6, window_size=14):
        super().__init__()
        self.latent_dim = latent_dim
        self.window_size = int(window_size)

        self.in_proj = nn.Conv2d(embedding_dim, latent_dim, 1)

        layer = nn.TransformerEncoderLayer(
            d_model=latent_dim, nhead=nhead, dim_feedforward=4 * latent_dim,
            batch_first=True, norm_first=True)
        # The terminal LayerNorm is not optional: nn.TransformerEncoder defaults to
        # norm=None, but these layers are norm_first (pre-LN), and a pre-LN stack
        # without a final norm lets the residual stream's scale grow with depth.
        # enable_nested_tensor=False: the nested-tensor fast path is incompatible
        # with norm_first, so it silently declines and warns on every construction.
        # It would be unusable here regardless -- forward() drives the layers by
        # hand to shift the window grid, so the container never runs its own loop.
        self.transformer = nn.TransformerEncoder(
            layer, num_layers=num_layers, norm=nn.LayerNorm(latent_dim),
            enable_nested_tensor=False)

        self.decoder = SynthesisHead(latent_dim, out_channels)
        self._pe_cache = {}

    def _pos(self, H, W, C, device, dtype):
        """Position encoding, tiled with period `window_size`.

        Tiling is what removes the extrapolation failure: a token's encoding depends
        only on its offset WITHIN a window, so the set of position vectors is
        identical at 28x28 and at 256x170 (196 distinct vectors either way, for
        ws=14). Global attention keeps the original whole-grid encoding, which does
        grow with the grid.
        """
        ws = self.window_size
        key = (H, W, C, ws, str(device), str(dtype))
        if key not in self._pe_cache:
            if len(self._pe_cache) > 16:
                self._pe_cache.clear()
            if ws <= 0 or (ws >= H and ws >= W):
                pe = sinusoidal_pos_2d(H, W, C, device, dtype).view(1, H, W, C)
            else:
                base = sinusoidal_pos_2d(ws, ws, C, device, dtype).view(ws, ws, C)
                pe = base.repeat(-(-H // ws), -(-W // ws), 1)[:H, :W].unsqueeze(0)
            self._pe_cache[key] = pe
        return self._pe_cache[key]

    def _attend(self, x, layer, shift):
        """One transformer layer over [B,H,W,C], windowed.

        Shifting is done by asymmetric PADDING rather than `torch.roll`. Rolling
        would let a token on the left edge attend to one on the right edge, which
        for an image is simply wrong, and suppressing that needs Swin's [L,L]
        region mask -- a large tensor to materialize on every layer. Padding
        top-left by ws//2 shifts the window grid with no wrap-around, and validity
        then needs only a [B*nW, L] key-padding mask.
        """
        B, H, W, C = x.shape
        ws = self.window_size
        if ws <= 0 or (ws >= H and ws >= W):
            return layer(x.reshape(B, H * W, C)).reshape(B, H, W, C)

        off = ws // 2 if shift else 0
        ph, pw = (-(H + off)) % ws, (-(W + off)) % ws
        if off or ph or pw:
            xp = F.pad(x.permute(0, 3, 1, 2), (off, pw, off, ph))
            xp = xp.permute(0, 2, 3, 1).contiguous()
        else:
            xp = x
        Hp, Wp = xp.shape[1], xp.shape[2]

        valid = xp.new_zeros(1, Hp, Wp, 1)
        valid[:, off:off + H, off:off + W, :] = 1.0
        mask = (window_partition(valid, ws)[..., 0] < 0.5).repeat(B, 1)
        # A fully padded window would leave softmax with no keys and emit NaN. Its
        # output is cropped away, so leave it unmasked.
        mask[mask.all(dim=1)] = False

        win = layer(window_partition(xp, ws), src_key_padding_mask=mask)
        xp = window_reverse(win, ws, B, Hp, Wp)
        return xp[:, off:off + H, off:off + W, :]

    def forward(self, quantized_latents):
        q = self.in_proj(quantized_latents)                  # [B,C,h,w]
        B, C, H, W = q.shape
        x = q.permute(0, 2, 3, 1) + self._pos(H, W, C, q.device, q.dtype)

        # Layers are iterated by hand rather than calling self.transformer(...) so
        # the window grid can shift on alternate layers. The container is still
        # nn.TransformerEncoder, so state_dict keys stay transformer.layers.<i>.*.
        for i, layer in enumerate(self.transformer.layers):
            x = self._attend(x, layer, shift=bool(i % 2))
        x = self.transformer.norm(x)

        return self.decoder(x.permute(0, 3, 1, 2).contiguous())
