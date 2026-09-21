"""The bitrate ladder: one named codebook shape per target bpp.

Rate in this codec is set by the codebook SHAPE, not by `rate_beta`:

    bits/token = num_codebooks * log2(codebook_size)     <- a hard ceiling
    bpp        = bits/token / 64                         <- /8 encoder, 64 px/token

`rate_beta` can only push the realised entropy *below* that ceiling, and pushing
it far below is the collapse regime. base_v3 ran 4x256 -- a 0.5 bpp ceiling -- at
rate_beta=0.3 and landed at 15.2 bits/token = 0.238 bpp. That is a 1024-entry
codebook doing the work of about 56 entries, and the checkpoints show exactly
that: 221/12/30/18 live of 256 per codebook, the rest pinned dead. Sizing the
codebook to the target rate and keeping rate_beta small is the stable way to hit a
bitrate, and it shrinks the edge LUT by the same factor.

Every rung keeps `num_codebooks=4`, so the group dimension stays 64/4 = 16 and
the encoder, the decoder and every tensor shape except the codebook are IDENTICAL
across the ladder -- only `codebook_size` moves. That is deliberate: it makes the
five points one architecture's rate-distortion curve rather than five different
models. `bpp010w` is the single exception, and it exists to A/B that choice.

Select one with `--preset <name>`; list them with `--list-presets`. Explicit flags
still win, so `--preset bpp020 --rate-beta-final 0.2` is a legal override.
"""
from __future__ import annotations

import math
from dataclasses import dataclass

# Realised entropy as a fraction of the ceiling. A codebook whose size matches the
# target rate ends up close to uniformly used, so its entropy lands near
# log2(K) -- near, not at, because the assignment histogram is never flat. 0.80 is
# the planning estimate behind `expected_bpp`; treat it as +-15% and trim with
# rate_beta once a run has 20k steps of measured bpp behind it.
UTIL_FRACTION = 0.80


@dataclass(frozen=True)
class Preset:
    codebook_size: int
    rate_beta_final: float
    num_codebooks: int = 4
    note: str = ""

    @property
    def bits_per_token(self) -> float:
        return self.num_codebooks * math.log2(self.codebook_size)

    @property
    def ceiling_bpp(self) -> float:
        return self.bits_per_token / 64.0

    @property
    def expected_bpp(self) -> float:
        return UTIL_FRACTION * self.ceiling_bpp

    def lut_params(self, embedding_dim: int = 64) -> int:
        # G * K * (embedding_dim // G) == K * embedding_dim, so the edge LUT depends
        # only on codebook_size -- not on how the dims are split across groups.
        return self.codebook_size * embedding_dim

    def overrides(self) -> dict:
        return {"num_codebooks": self.num_codebooks,
                "codebook_size": self.codebook_size,
                "rate_beta_final": self.rate_beta_final}


# ---------------------------------------------------------------------------
# EDIT HERE to add or retune a rung. Nothing else in the library holds a bitrate
# assumption.
#
# rate_beta_final is deliberately small at the low rungs and zero at the bottom
# two: rate pressure exists to prune an oversized codebook, and once the codebook
# is already the size of the target rate the entropy term buys nothing while still
# feeding the rich-get-richer loop. With K=4 there is nothing to prune and a
# collapse costs a quarter of the model's capacity.
# ---------------------------------------------------------------------------
PRESETS: dict[str, Preset] = {
    "bpp010": Preset(codebook_size=4, rate_beta_final=0.0,
                     note="2 bits/group; coarsest rung, expect visible blocking"),
    "bpp015": Preset(codebook_size=8, rate_beta_final=0.0,
                     note="3 bits/group"),
    "bpp020": Preset(codebook_size=16, rate_beta_final=0.10,
                     note="4 bits/group; matches base_v3's REALISED rate"),
    "bpp025": Preset(codebook_size=32, rate_beta_final=0.15,
                     note="5 bits/group"),
    "bpp030": Preset(codebook_size=64, rate_beta_final=0.20,
                     note="6 bits/group"),
    "bpp035": Preset(codebook_size=128, rate_beta_final=0.25,
                     note="7 bits/group"),
    "bpp040": Preset(codebook_size=256, rate_beta_final=0.30,
                     note="base_v3's codebook SHAPE, with revival fixed"),
    "bpp010w": Preset(codebook_size=16, rate_beta_final=0.0, num_codebooks=2,
                      note="same 8 bits/token as bpp010, spent as 2 groups of 32 "
                           "dims instead of 4 of 16 -- the A/B for how to split"),
    "bpp050": Preset(codebook_size=256, rate_beta_final=0.2),
    "bpp060": Preset(codebook_size=256, rate_beta_final=0.1),
}


def apply(name: str) -> dict:
    """Config overrides for a preset name. Raises with the valid list."""
    if name not in PRESETS:
        raise ValueError(f"unknown --preset {name!r}; choose one of "
                         f"{', '.join(PRESETS)}")
    return PRESETS[name].overrides()


def describe(embedding_dim: int = 64) -> str:
    """The ladder as a table, for --list-presets and the docs."""
    from vqlic.encoder import ImageEncoder
    enc = ImageEncoder(embedding_dim=embedding_dim).n_params()

    rows = [f"{'preset':<10}{'G':>3}{'K':>5}{'b/token':>9}{'ceiling':>9}"
            f"{'expect':>8}{'beta':>7}{'LUT':>9}{'edge':>9}  note",
            "-" * 118]
    for name, p in PRESETS.items():
        lut = p.lut_params(embedding_dim)
        rows.append(
            f"{name:<10}{p.num_codebooks:>3}{p.codebook_size:>5}"
            f"{p.bits_per_token:>9.0f}{p.ceiling_bpp:>9.4f}"
            f"{p.expected_bpp:>8.3f}{p.rate_beta_final:>7.2f}"
            f"{lut:>9,}{enc + lut:>9,}  {p.note}")
    rows += [
        "-" * 118,
        f"ceiling = G*log2(K)/64 (hard).  expect = {UTIL_FRACTION:.2f} x ceiling "
        f"(planning estimate, +-15%).",
        f"LUT = K x embedding_dim params.  edge = encoder ({enc:,}) + LUT, which is "
        f"everything that ships to the device.",
    ]
    return "\n".join(rows)
