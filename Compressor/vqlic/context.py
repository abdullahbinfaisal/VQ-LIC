"""Neighbour-conditioned entropy coding: the context, the model, the rANS pair.

This is the coding half. `vqlic/context_fit.py` fits the tables and imports
these; `vqlic/codec.py` codes with them. **That sharing is the point** --
the fitter and the coder must agree on what a context IS, token for token, or a
stream decodes to silently different indices with nothing in it to notice.

WHAT IS AND IS NOT TRANSMITTED
------------------------------
Nothing conditional goes in the payload. The tables are fitted once, stored
beside the checkpoint, and held by BOTH ends -- the decoder forms each token's
context from neighbours it has already decoded, so conditioning costs zero bits.
That is why it beats a per-image histogram, and also why it has to generalize:
fit it on the evaluation set and you are measuring memorization.

WHY THE TABLES ARE PRE-QUANTIZED INTEGERS
-----------------------------------------
Encoder and decoder must agree on every symbol's frequency to the last integer;
rANS carries nothing that would detect disagreement, it just decodes different
symbols from that point on. So counts are renormalized to `1 << prob_bits` ONCE,
at fit time, and what ships is `uint16`. No float arithmetic runs at coding time
on either side, so no libm, summation order or device difference can
desynchronize them.

BORDERS
-------
Column 0 has no left neighbour and row 0 has no upper one. Rather than skip
those tokens -- which would quietly discount ~1/h of the grid -- they get an
explicit out-of-band context id `K`, so the conditioning alphabet is `K + 1` for
`left` and `(K + 1)^2` for `leftup`, and the borders are coded, and charged,
like everything else.
"""
from __future__ import annotations

import numpy as np

from vqlic.entropy import RANS_L, cdf_from_freqs

_BYTE = 0xFF
_SHIFT = 8

ORDERS = ("left", "leftup")
PROB_BITS = 16


def n_contexts(K, order):
    """Conditioning alphabet size. `K` itself is the out-of-bounds id."""
    return (K + 1) if order == "left" else (K + 1) * (K + 1)


def context_ids(idx_g, K, order):
    """`[h,w]` indices for ONE group -> `[h*w]` context ids, raster order.

    The single definition of the context, shared by the fitter and the encoder.
    Border tokens get the out-of-bounds id `K` rather than being skipped, so
    every token in the grid is coded and charged.
    """
    if order not in ORDERS:
        raise ValueError(f"unknown order {order!r}, expected one of {ORDERS}")
    h, w = idx_g.shape
    a = idx_g.astype(np.int64)
    left = np.full((h, w), K, dtype=np.int64)
    left[:, 1:] = a[:, :-1]
    if order == "left":
        return left.ravel()
    up = np.full((h, w), K, dtype=np.int64)
    up[1:, :] = a[:-1, :]
    return (left * (K + 1) + up).ravel()


def context_at(t, decoded, w, K, order):
    """The same context for token `t`, from a PARTIALLY decoded raster.

    The decoder's view: it cannot precompute contexts, it derives each from
    neighbours that landed a moment ago. `check_context_definitions` asserts
    this agrees with `context_ids` token for token -- if the two ever drift,
    streams decode to silently different indices.
    """
    x = t % w
    left = decoded[t - 1] if x else K
    if order == "left":
        return left
    up = decoded[t - w] if t >= w else K
    return left * (K + 1) + up


def check_context_definitions(K, order, seed=0):
    """`context_ids` (bulk) and `context_at` (incremental) must agree exactly.

    Cheap, and it is the one place a silent desynchronization between encode and
    decode could hide: the encoder uses the bulk form, the decoder the
    incremental one, and nothing in the stream would flag a disagreement.
    """
    rng = np.random.default_rng(seed)
    a = rng.integers(0, K, (7, 11)).astype(np.int32)
    bulk = context_ids(a, K, order)
    flat = a.ravel().tolist()
    inc = [context_at(t, flat, a.shape[1], K, order) for t in range(a.size)]
    if not np.array_equal(bulk, np.asarray(inc)):
        raise AssertionError(f"context definitions disagree for order={order}")


def quantize_to_total(weights, total, floor_one=True):
    """Integer frequencies summing to EXACTLY `total`, every entry >= 1.

    `quantize_freqs` is hardwired to a power-of-two total because that is what
    rANS needs for the whole table. This one takes an arbitrary total, because
    reconstructing a top-N table means distributing whatever mass the stored
    entries did not claim over the remaining symbols.

    Integer-only and deterministic, which is the requirement: encoder and
    decoder both run it and must land on the same array to the last unit.
    """
    w = np.asarray(weights, dtype=np.float64).ravel()
    n = w.size
    total = int(total)
    if floor_one and total < n:
        raise ValueError(f"cannot give {n} symbols >= 1 out of {total}")
    if w.sum() <= 0:
        w = np.ones(n)
    f = np.floor(w / w.sum() * total).astype(np.int64)
    if floor_one:
        np.maximum(f, 1, out=f)
    diff = total - int(f.sum())
    order = np.argsort(-w, kind="stable")
    if diff > 0:
        for j in range(diff):
            f[order[j % n]] += 1
    elif diff < 0:
        need, j = -diff, 0
        lo = 1 if floor_one else 0
        while need > 0:
            i = order[j % n]
            if f[i] > lo:
                f[i] -= 1
                need -= 1
            j += 1
    assert f.sum() == total
    return f


class ContextModel:
    """Per-group marginal plus a sparse set of conditional tables.

    Frequencies are stored as `uint16` holding `freq - 1`, so a full `1 << 16`
    interval on one symbol still fits.
    """

    def __init__(self, G, K, order, prob_bits, marginal, ids, freqs, top_n=0,
                 keep_n=0):
        self.G, self.K = int(G), int(K)
        self.order = order
        self.top_n = int(top_n)
        # The EFFECTIVE alphabet. Tables stay K-wide so the coder can index by
        # raw code id, but under keepN only `keep_n` codes can occur, so only
        # `keep_n + 1` contexts can -- and that, not K, is what a fixed
        # allocation has to be sized for.
        self.keep_n = int(keep_n)
        self.prob_bits = int(prob_bits)
        self.marginal = [np.asarray(m, dtype=np.int64) for m in marginal]
        self.ids = [np.asarray(i, dtype=np.int64) for i in ids]
        self.freqs = [np.asarray(f, dtype=np.uint16) for f in freqs]
        self._cache, self._marg = {}, {}
        for g in range(self.G):
            if self.ids[g].size and not np.all(np.diff(self.ids[g]) > 0):
                raise ValueError(f"group {g} context ids not sorted/unique")
            if self.freqs[g].shape != (self.ids[g].size, self.K):
                raise ValueError(f"group {g}: {self.ids[g].size} ids vs freqs "
                                 f"{self.freqs[g].shape}")

    # -------------------------------------------------------- vectorized rate
    def freq_of(self, g, ctx, cur):
        """`freq` for every (ctx, cur) pair at once. No Python loop.

        This is what makes the exact-rate sweep cheap: one `searchsorted` per
        group per image resolves which tokens hit a conditional table and which
        fall back, and the rest is fancy indexing.
        """
        ids = self.ids[g]
        out = self.marginal[g][cur].astype(np.int64)
        if ids.size:
            j = np.searchsorted(ids, ctx)
            j_clipped = np.minimum(j, ids.size - 1)
            hit = ids[j_clipped] == ctx
            if hit.any():
                rows = self.freqs[g][j_clipped[hit]].astype(np.int64) + 1
                out[hit] = rows[np.arange(rows.shape[0]), cur[hit]]
        return out

    # --------------------------------------------------------- coding lookup
    def _lists(self, freqs):
        f = [int(v) for v in freqs]
        return f, [int(v) for v in cdf_from_freqs(np.asarray(f, dtype=np.int64))]

    def marginal_table(self, g):
        if g not in self._marg:
            self._marg[g] = self._lists(self.marginal[g])
        return self._marg[g]

    def table(self, g, ctx):
        """`(freqs_list, cdf_list)` for one group and one context, cached.

        Cached because a full-resolution image touches the same few thousand
        contexts 2.4M times, and rebuilding a 257-entry CDF each time would
        dominate.
        """
        key = (g, int(ctx))
        hit = self._cache.get(key)
        if hit is not None:
            return hit
        ids = self.ids[g]
        j = int(np.searchsorted(ids, ctx))
        if j < ids.size and ids[j] == ctx:
            out = self._lists(self.freqs[g][j].astype(np.int64) + 1)
        else:
            out = self.marginal_table(g)
        self._cache[key] = out
        return out

    # ------------------------------------------------------------- reporting
    def n_tables(self):
        return [int(i.size) for i in self.ids]

    def nbytes(self, worst_case=False):
        """What has to SHIP, which is not what is held in memory here.

        In top-N mode the rate is computed from a reconstructed dense table
        (fast to index), but only `top_n` symbol/frequency pairs per context
        would actually be stored -- 3 bytes each, one for the symbol and two for
        the frequency. The escape mass is derived, not stored: it is whatever
        `1 << prob_bits` minus the stored frequencies leaves.

        `worst_case` sizes a FIXED allocation of `n_contexts(K, order)` tables
        per group instead of the ones this rung happened to populate, and drops
        the sparse id index, which a fixed allocation does not need because the
        context IS the array offset. That is the number to budget firmware
        against: a rung whose codebook is used more evenly populates more
        contexts, and an image sized on this rung's sparse count would not fit
        it. Reported alongside the observed size rather than instead of it,
        because the observed one is what this rung's tables actually occupy.
        """
        per_ctx = (3 * self.top_n) if self.top_n else (2 * self.K)
        marg = sum(m.size * 2 for m in self.marginal)
        if worst_case:
            n = n_contexts(self.keep_n or self.K, self.order)
            return int(self.G * n * per_ctx + marg)
        ids = sum(i.size * 4 for i in self.ids)
        tables = sum(i.size * per_ctx for i in self.ids)
        return int(ids + tables + marg)

    def summary(self):
        top = f" top{self.top_n}" if self.top_n else " dense"
        return (f"order={self.order}{top} K={self.K} G={self.G} "
                f"prob_bits={self.prob_bits} | tables/group {self.n_tables()} "
                f"| {self.nbytes() / 1024:.1f} KB to ship "
                f"({self.nbytes(worst_case=True) / 1024:.1f} KB budgeted)")

    def save(self, path):
        # top_n is persisted: without it a reloaded model reports its DENSE size
        # even though its rows are truncated reconstructions, which is how a
        # 12.7 KB table comes back claiming 108 KB.
        blob = {"order": np.array(self.order), "K": self.K, "G": self.G,
                "prob_bits": self.prob_bits, "top_n": self.top_n,
                "keep_n": self.keep_n}
        for g in range(self.G):
            blob[f"marginal_{g}"] = self.marginal[g]
            blob[f"ids_{g}"] = self.ids[g]
            blob[f"freqs_{g}"] = self.freqs[g]
        np.savez_compressed(path, **blob)
        return path

    @classmethod
    def load(cls, path):
        z = np.load(path, allow_pickle=False)
        G, K = int(z["G"]), int(z["K"])
        # Models fitted before top_n was persisted load as dense; their rows are
        # still the truncated reconstructions, so only the SIZE report differs.
        top_n = int(z["top_n"]) if "top_n" in z.files else 0
        keep_n = int(z["keep_n"]) if "keep_n" in z.files else 0
        return cls(G, K, str(z["order"]), int(z["prob_bits"]),
                   [z[f"marginal_{g}"] for g in range(G)],
                   [z[f"ids_{g}"] for g in range(G)],
                   [z[f"freqs_{g}"] for g in range(G)], top_n, keep_n)


# ============================================================ rANS, per token
def rans_encode_ctx(symbols, tables, ctx_ids, prob_bits):
    """Encode where every symbol has its OWN frequency table.

    Encoding walks BACKWARD, which is fine: the encoder holds the whole index
    map before it starts, so every context is already known. The decoder is the
    side that must discover them, in forward order as each neighbour lands.
    That asymmetry is what lets an autoregressive model work with rANS at all.
    """
    x_max_base = (RANS_L >> prob_bits) << _SHIFT
    out = bytearray()
    x = RANS_L
    syms = symbols.tolist() if isinstance(symbols, np.ndarray) else list(symbols)
    ctxs = ctx_ids.tolist() if isinstance(ctx_ids, np.ndarray) else list(ctx_ids)
    if len(syms) != len(ctxs):
        raise ValueError(f"{len(syms)} symbols but {len(ctxs)} contexts")

    for t in range(len(syms) - 1, -1, -1):
        freqs_l, cdf_l = tables(ctxs[t])
        s = syms[t]
        f = freqs_l[s]
        x_max = x_max_base * f
        while x >= x_max:
            out.append(x & _BYTE)
            x >>= _SHIFT
        x = ((x // f) << prob_bits) + (x % f) + cdf_l[s]

    for _ in range(4):
        out.append(x & _BYTE)
        x >>= _SHIFT
    out.reverse()
    return bytes(out)


def symbol_from_cdf(cdf_l, slot, K):
    """Largest `s` with `cdf_l[s] <= slot`. Binary search, not a lookup table.

    The static coder inverts the CDF with a `1 << prob_bits`-slot table built
    once per stream. A context coder cannot: a different table per token means
    materializing a 65,536-slot array for thousands of contexts, which would
    dominate everything. Eight compares at K=256 is the right trade.
    """
    lo, hi = 0, K
    while lo + 1 < hi:
        mid = (lo + hi) >> 1
        if cdf_l[mid] <= slot:
            lo = mid
        else:
            hi = mid
    return lo


def rans_decode_ctx(data, n_symbols, context_of, tables, prob_bits):
    """Inverse of `rans_encode_ctx`. `context_of(t, decoded)` -> context id."""
    if n_symbols == 0:
        return np.zeros(0, dtype=np.int32)
    if len(data) < 4:
        raise ValueError(f"stream of {len(data)} bytes holds no rANS state")

    mask = (1 << prob_bits) - 1
    x = (data[0] << 24) | (data[1] << 16) | (data[2] << 8) | data[3]
    p = 4
    out = [0] * n_symbols
    for t in range(n_symbols):
        freqs_l, cdf_l = tables(context_of(t, out))
        slot = x & mask
        s = symbol_from_cdf(cdf_l, slot, len(freqs_l))
        out[t] = s
        x = freqs_l[s] * (x >> prob_bits) + slot - cdf_l[s]
        while x < RANS_L:
            x = (x << _SHIFT) | data[p]
            p += 1
    return np.asarray(out, dtype=np.int32)
