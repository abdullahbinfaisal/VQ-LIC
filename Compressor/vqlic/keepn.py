"""Codebook pruning: restrict the quantizer to the top-N codes per codebook.

The deployed configuration keeps 64 of each codebook's 256 codes. That is not a
post-hoc filter on the indices -- it changes what the quantizer is allowed to
assign, so the rate is charged against a prior normalized over exactly the codes
that can occur. Doing it the other way round (prune the table, leave the
assignment alone) prices a distribution the encoder does not emit.

`install_keep_mask` is reversible so one loaded model can be measured at several
N without being rebuilt.
"""
from __future__ import annotations

import numpy as np
import torch

__all__ = ["keep_topn", "install_keep_mask", "remove_keep_mask"]


def keep_topn(weights, n):
    """Boolean `[G,K]` keeping the largest entries of each row.

    `n` is either one number applied to every codebook, or one per codebook.
    The per-codebook form matters: the codebooks of this model do not use their
    256 codes to remotely the same depth, so a single N is set by the greediest
    of them and wastes the others.

    Ties break by index, which only matters deep in a tail where the codes
    involved carry indistinguishable mass.
    """
    w = np.asarray(weights, dtype=np.float64)
    G, K = w.shape
    ns = [int(n)] * G if np.isscalar(n) else [int(x) for x in n]
    if len(ns) != G:
        raise SystemExit(f"expected 1 or {G} values for N, got {len(ns)}")
    if any(x < 1 or x > K for x in ns):
        raise SystemExit(f"every N must lie in 1..{K}, got {ns}")
    keep = np.zeros((G, K), dtype=bool)
    for g in range(G):
        keep[g, np.argsort(-w[g], kind="stable")[:ns[g]]] = True
    return keep


def install_keep_mask(q, keep):
    """Make every code outside `keep` unreachable, and return an undo token.

    Two edits, and they have to agree or the rate would be charged against a
    different prior than the one the assignment used:

    1. `cluster_size` is zeroed on the dropped codes. That is what
       `prior_counts()` (the rANS table) and `metrics.prior_cross_entropy_bpp`
       read, so both now normalize over the survivors.
    2. `_prior_logp` is overridden to return `-inf` there, which is what makes
       the code unreachable: the quantizer's cost is `dist - beta*logp`, so the
       cost becomes `+inf` inside its own forward.

    Step 2 is the load-bearing one and it needs `rate_beta != 0`; zeroing
    `cluster_size` alone would only make a dropped code expensive, not
    impossible, and `eps/total` is a finite handicap the argmin can still choose
    to pay.
    """
    if not q.rate_beta:
        raise SystemExit(
            "rate_beta is 0, so the assignment ignores the prior and a -inf log "
            "p cannot mask a code out. This needs the ECVQ rung the checkpoint "
            "was trained as -- check that the loader restored rate_beta_final.")
    dev = q.cluster_size.device
    mask = torch.as_tensor(keep, dtype=torch.bool, device=dev)
    undo = q.cluster_size.detach().clone()

    q._keep_mask = mask
    q.cluster_size.mul_(mask.to(q.cluster_size.dtype))

    def _prior_logp(g, _q=q):
        p = _q.cluster_size[g] + _q.eps
        p = p / p.sum()
        return torch.log(p).masked_fill(~_q._keep_mask[g], float("-inf"))

    # Set on the INSTANCE, so removing it restores the class method untouched.
    q._prior_logp = _prior_logp
    return undo


def remove_keep_mask(q, undo):
    """Undo `install_keep_mask`, restoring the full codebook."""
    q.cluster_size.copy_(undo)
    for attr in ("_prior_logp", "_keep_mask"):
        if attr in q.__dict__:
            del q.__dict__[attr]
