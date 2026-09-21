"""Fitting the context tables that `vqlic.context` codes with.

`vqlic/context.py` is the coding half -- given a table, encode and decode against
it. This is the other half: accumulate `n(context, code)` over a training corpus
and turn those counts into the tables that ship.

The two are deliberately separate files because they run at different times and
in different places. Fitting happens once, offline, over thousands of images.
Coding happens per image, on the edge, against a ROM the fit produced.

THE LOCKED CONFIGURATION
------------------------
    order      left        65 contexts per group, not leftup's 4,225
    keep_n     64          keepN pruning, so the alphabet is 64 codes + border
    min_count  512         observations before a context earns its own table
    top_n      16          store 16 (symbol, freq) pairs; tail from the marginal
    prob_bits  16          fitted resolution, cannot be changed after the fact

`left` and `top_n` are what make this fit an edge ROM budget at all: `leftup`
dense was 5.6 MB. See `CONTEXT_CODEC.md` for the format the hardware implements.
"""
from __future__ import annotations

import numpy as np

from vqlic.context import ContextModel, context_ids, quantize_to_total
from vqlic.entropy import quantize_freqs

PROB_BITS = 16
MIN_COUNT = 512        # observations before a context earns its own table
ALPHA = 1.0            # marginal pseudo-counts mixed into every conditional

# The edge already ships the codebook: G*K*d = 4*256*16 = 16,384 values, ~16 KB
# at int8, against 4.9 KB of encoder weights. So ~21 KB is the ROM a context
# table has to be judged against -- not the weight count alone. A dense
# conditional table at 512 B per context blows through that after 64 contexts.
EDGE_ROM_BYTES = 4900 + 4 * 256 * 16

__all__ = ["ContextCounter", "truncate_to_top_n", "PROB_BITS", "MIN_COUNT",
           "ALPHA", "EDGE_ROM_BYTES"]


class ContextCounter:
    """Accumulates `n(ctx, cur)` over a corpus, sparsely.

    A dense `[(K+1)^2, K]` array is 16.9M entries per group -- 540 MB at int64
    for G=4 -- and the vast majority of contexts never occur. Counts are held as
    a sorted `(key, count)` pair and merged in batches, so memory tracks the
    contexts that actually appear rather than the ones that could.
    """

    FLUSH = 128

    def __init__(self, G, K, order, keep_n=0):
        self.G, self.K, self.order = int(G), int(K), order
        # Recorded, not enforced: it only affects how a fixed-allocation ROM
        # budget is computed, never the counts or the coded rate.
        self.keep_n = int(keep_n)
        self.keys = [np.zeros(0, dtype=np.int64) for _ in range(self.G)]
        self.counts = [np.zeros(0, dtype=np.int64) for _ in range(self.G)]
        self.marg = [np.zeros(self.K, dtype=np.int64) for _ in range(self.G)]
        self._buf = [[] for _ in range(self.G)]
        self._n = 0
        self.tokens = 0
        self.images = 0

    def add(self, idx):
        """One image's `[h, w, G]` index map."""
        for g in range(self.G):
            a = idx[..., g]
            ctx = context_ids(a, self.K, self.order)
            cur = a.astype(np.int64).ravel()
            self._buf[g].append(ctx * self.K + cur)
            self.marg[g] += np.bincount(cur, minlength=self.K)
        self.tokens += idx.shape[0] * idx.shape[1]
        self.images += 1
        self._n += 1
        if self._n >= self.FLUSH:
            self.flush()

    def flush(self):
        for g in range(self.G):
            if not self._buf[g]:
                continue
            nk, nc = np.unique(np.concatenate(self._buf[g]),
                               return_counts=True)
            k = np.concatenate([self.keys[g], nk])
            c = np.concatenate([self.counts[g], nc])
            uk, inv = np.unique(k, return_inverse=True)
            self.keys[g] = uk
            self.counts[g] = np.bincount(inv, weights=c).astype(np.int64)
            self._buf[g] = []
        self._n = 0

    def save_counts(self, path):
        """The sparse `(key, count)` pairs and marginals, before any table is
        built. Saving these is what makes a size/rate sweep cheap: min_count and
        top-N are pure post-processing, so a whole Pareto curve costs no
        re-encoding of the training corpus."""
        self.flush()
        blob = {"K": self.K, "G": self.G, "order": np.array(self.order),
                "tokens": self.tokens, "images": self.images}
        for g in range(self.G):
            blob[f"keys_{g}"] = self.keys[g]
            blob[f"counts_{g}"] = self.counts[g]
            blob[f"marg_{g}"] = self.marg[g]
        np.savez_compressed(path, **blob)
        return path

    @classmethod
    def from_counts(cls, path):
        z = np.load(path, allow_pickle=False)
        c = cls(int(z["G"]), int(z["K"]), str(z["order"]))
        c.tokens = int(z["tokens"])
        c.images = int(z["images"])
        for g in range(c.G):
            c.keys[g] = z[f"keys_{g}"]
            c.counts[g] = z[f"counts_{g}"]
            c.marg[g] = z[f"marg_{g}"]
        return c

    def build(self, prob_bits=PROB_BITS, min_count=MIN_COUNT, alpha=ALPHA,
              top_n=0):
        """Accumulated counts -> a `ContextModel` ready to code with."""
        self.flush()
        G, K = self.G, self.K
        marginal, ids_out, freqs_out = [], [], []
        for g in range(G):
            m = self.marg[g].astype(np.float64)
            marginal.append(quantize_freqs(m if m.sum() > 0 else np.ones(K),
                                           prob_bits))
            pm = (m + 1.0) / (m.sum() + K)

            keys, cnts = self.keys[g], self.counts[g]
            ctxs = keys // K
            curs = keys % K
            # keys are sorted, so contexts are contiguous: one pass, no dict.
            bounds = np.flatnonzero(np.diff(ctxs)) + 1
            starts = np.concatenate([[0], bounds])
            ends = np.concatenate([bounds, [keys.size]])

            marg_f = marginal[-1]
            kept, rows = [], []
            for s, e in zip(starts.tolist(), ends.tolist()):
                if cnts[s:e].sum() < min_count:
                    continue
                counts = alpha * pm.copy()
                counts[curs[s:e]] += cnts[s:e]
                f = quantize_freqs(counts, prob_bits)
                if top_n:
                    f = truncate_to_top_n(f, marg_f, top_n, prob_bits)
                kept.append(int(ctxs[s]))
                rows.append((f - 1).astype(np.uint16))
            ids_out.append(np.asarray(kept, dtype=np.int64))
            freqs_out.append(np.asarray(rows, dtype=np.uint16
                                        ).reshape(len(rows), K))
        return ContextModel(G, K, self.order, prob_bits, marginal,
                            ids_out, freqs_out, top_n=top_n,
                            keep_n=getattr(self, "keep_n", 0))


def truncate_to_top_n(freqs, marg_freqs, top_n, prob_bits):
    """Keep the `top_n` largest frequencies; rebuild the tail from the marginal.

    This is the lever that actually fits a context model in edge ROM. A dense
    table is `K` frequencies -- 512 bytes at K=256 and uint16 -- but a
    distribution conditioned on (left, up) is savagely peaked: a handful of
    codes carry almost all the mass and the rest are barely distinguishable
    from the unconditional marginal, which is shipped anyway. So store

        top_n x (1 byte symbol + 2 byte frequency)

    and let the leftover mass spread over the remaining symbols in proportion
    to the marginal. At top_n=8 that is 24 bytes against 512, a 21x cut, and
    the tail it discards was already being predicted by the marginal.

    Both ends must reconstruct identically, hence `quantize_to_total` and
    integers throughout. What is returned here is the reconstruction, so the
    measured rate is what a decoder holding only the truncated form would pay
    -- not what the dense table would have paid.
    """
    K = freqs.size
    total = 1 << prob_bits
    keep = np.argsort(-freqs, kind="stable")[:top_n]
    out = np.zeros(K, dtype=np.int64)
    out[keep] = freqs[keep]

    tail = np.setdiff1d(np.arange(K), keep, assume_unique=True)
    mass = total - int(out[keep].sum())
    if mass < tail.size:
        # The stored entries claimed so much that the tail cannot keep >= 1
        # each. Reclaim from the largest -- an unencodable zero is worse than a
        # slightly mispriced peak.
        deficit = tail.size - mass
        order = keep[np.argsort(-freqs[keep], kind="stable")]
        j = 0
        while deficit > 0:
            i = order[j % order.size]
            if out[i] > 1:
                out[i] -= 1
                deficit -= 1
            j += 1
        mass = total - int(out[keep].sum())
    out[tail] = quantize_to_total(marg_freqs[tail], mass)
    assert out.sum() == total and out.min() >= 1
    return out
