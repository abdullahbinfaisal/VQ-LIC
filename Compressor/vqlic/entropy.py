"""Static multi-symbol range coder (rANS) and bit packing for the payload.

This is what turns the codec's rate from an estimate into a measurement. Until
now `vqlic/metrics.py` reported what an ideal coder *would* pay against a
distribution; nothing produced a bitstream, so side information was invisible and
the number could not be checked. Everything here produces real bytes.

WHY rANS AND NOT AN ARITHMETIC CODER
------------------------------------
No carry propagation. An arithmetic coder's low/range update can carry through an
arbitrary run of pending output bytes, and getting that exactly right -- and
exactly symmetric on the decode side -- is where implementations go wrong. rANS
folds each symbol into a single integer state with a division and a multiply, so
encode and decode are visibly inverse operations and the round-trip test either
passes bit-exactly or fails loudly. Layout follows ryg's `rans_byte.h`:
32-bit state, `RANS_L = 1<<23`, byte-at-a-time renormalization.

WHY NO DEPENDENCY
-----------------
The shipped coder is static and context-free, with G symbol encodes per token
and no probability update. Importing `constriction` or
`torchac` would make that claim unauditable -- the thing being described would be
somebody else's compiled blob. This is NumPy only, no torch, so it is also
testable without building a model.

ENCODE IS BACKWARD, DECODE IS FORWARD
-------------------------------------
rANS is a stack: the last symbol encoded is the first decoded. `rans_encode`
therefore walks `symbols` in reverse and appends bytes, then reverses the buffer
once at the end -- equivalent to ryg writing backwards into a fixed buffer, but
without needing to know the output size in advance. The four state bytes are
appended last so that, after the reversal, they are the first thing the decoder
reads.
"""
from __future__ import annotations

import numpy as np

# Lower bound of the state interval. The state lives in [RANS_L, RANS_L << 8),
# so it stays under 2**31 and Python never has to promote it past a machine word.
RANS_L = 1 << 23
_RENORM_SHIFT = 8
_BYTE = 0xFF


# ------------------------------------------------------------------ frequencies
def quantize_freqs(counts, prob_bits=12):
    """Counts -> integer frequencies summing to EXACTLY `1 << prob_bits`.

    Two invariants, both load-bearing:

    * **The sum is exact.** rANS derives the symbol from `state & (total-1)`, so a
      total that is not `1 << prob_bits` silently decodes garbage rather than
      raising.
    * **Every frequency is >= 1.** A symbol with frequency 0 occupies no slot in
      the inverse-CDF table and cannot be encoded at all. On the frozen prior that
      would be a crash waiting for the first out-of-distribution image to pick a
      code the training corpus never used, which is exactly the case the fallback
      in `codec.compress` exists to survive -- so it must not crash first.

    Flooring loses at most K-1 counts and clamping to 1 can add at most K, so the
    residual is bounded by K and settling it with a short loop is cheap. It is
    spread across the largest bins rather than dumped on the single largest,
    which keeps the quantized distribution closer to the one it came from.
    """
    counts = np.asarray(counts, dtype=np.float64).ravel()
    K = counts.size
    total = 1 << prob_bits
    if K > total:
        raise ValueError(f"{K} symbols cannot each hold >= 1 of {total} slots; "
                         f"raise prob_bits above {int(np.ceil(np.log2(K)))}")

    # An all-zero histogram is not a bug: `cluster_size` is zero on a freshly
    # initialized quantizer, and a group can legitimately see no tokens on a
    # 1-token image. Uniform is the right answer for both.
    if counts.sum() <= 0:
        counts = np.ones(K, dtype=np.float64)

    freqs = np.floor(counts / counts.sum() * total).astype(np.int64)
    np.maximum(freqs, 1, out=freqs)

    diff = int(total - freqs.sum())
    if diff > 0:
        # Hand the surplus to the bins with the most actual mass.
        order = np.argsort(-counts, kind="stable")
        for j in range(diff):
            freqs[order[j % K]] += 1
    elif diff < 0:
        # Reclaim from the largest, never taking one below 1. This terminates:
        # if every frequency were 1 the sum would be K <= total, so diff < 0
        # guarantees at least one bin above 1, and each pass removes at least one.
        order = np.argsort(-freqs, kind="stable")
        need = -diff
        j = 0
        while need > 0:
            i = order[j % K]
            if freqs[i] > 1:
                freqs[i] -= 1
                need -= 1
            j += 1

    assert freqs.sum() == total and freqs.min() >= 1
    return freqs.astype(np.int64)


def quantize_freqs_support(counts, prob_bits=12):
    """Like `quantize_freqs`, but only the symbols that actually OCCUR get mass.

    Returns `(freqs, support)`: `freqs` is full length `K` with zeros off the
    support, `support` is the sorted indices where `counts > 0`.

    This is the right table for a *transmitted* per-image histogram, and using
    the dense `quantize_freqs` there was costing more than the table itself.
    Measured on CLIC at the bpp010 rung, G=4, K=256, prob_bits=12: only ~65 of
    the 1024 codes occur in a given image, so the dense builder's `>= 1` clamp
    hands 959 never-occurring codes one slot each -- 23% of the 4096 slots -- and
    every real symbol pays for it. That was +0.0089 bpp against an own-entropy of
    0.0786, i.e. **twice the cost of the 1536-byte table**. Restricting the mass
    to the support drops it to +0.00006 bpp and shrinks the table at the same
    time, because only the occupied entries are sent.

    The `>= 1` clamp still holds ON the support, for the same reason as before:
    an occurring symbol with frequency 0 is unencodable. What goes away is the
    clamp on symbols that provably cannot occur, and it can go away precisely
    because this table is fitted to the data it will code. Do NOT use this for
    the frozen prior -- there, a code the training corpus never touched can still
    turn up in the next image, and that is the crash `quantize_freqs` prevents.
    """
    counts = np.asarray(counts, dtype=np.float64).ravel()
    freqs = np.zeros(counts.size, dtype=np.int64)
    support = np.flatnonzero(counts > 0)
    if support.size == 0:
        # No symbol occurs -- a legitimate case on a degenerate image. Give
        # symbol 0 the whole interval so the table is still a valid one.
        freqs[0] = 1 << prob_bits
        return freqs, np.zeros(1, dtype=np.int64)
    freqs[support] = quantize_freqs(counts[support], prob_bits)
    return freqs, support


def cdf_from_freqs(freqs):
    """Exclusive prefix sums, length K+1, `cdf[-1] == sum(freqs)`."""
    cdf = np.zeros(len(freqs) + 1, dtype=np.int64)
    np.cumsum(freqs, out=cdf[1:])
    return cdf


def inverse_cdf(freqs):
    """slot -> symbol, one entry per slot in `1 << prob_bits`.

    `np.repeat` is exactly the right primitive: symbol s owns freqs[s]
    consecutive slots, which is the definition of the table.
    """
    return np.repeat(np.arange(len(freqs), dtype=np.int32), freqs)


# ------------------------------------------------------------------------ rANS
def rans_encode(symbols, freqs, prob_bits=12):
    """Encode `symbols` against static `freqs`. Returns bytes.

    `freqs` must come from `quantize_freqs` at the same `prob_bits`.

    The Python-list conversions are not incidental. This loop runs once per
    symbol -- ~181k times for a 2.8 Mpixel image at G=4 -- and indexing a list is
    several times cheaper than indexing a NumPy array with a scalar, because the
    latter builds a 0-d array and a boxed scalar every time. rANS cannot be
    vectorized: the state carries serially, which is precisely the property
    `edge_budget.py` calls the unparallelizable stage.
    """
    freqs_l = [int(f) for f in freqs]
    cdf_l = [int(c) for c in cdf_from_freqs(freqs)]
    syms = symbols.tolist() if isinstance(symbols, np.ndarray) else list(symbols)

    x_max_base = (RANS_L >> prob_bits) << _RENORM_SHIFT
    out = bytearray()
    x = RANS_L

    for s in reversed(syms):
        f = freqs_l[s]
        x_max = x_max_base * f
        while x >= x_max:
            out.append(x & _BYTE)
            x >>= _RENORM_SHIFT
        x = ((x // f) << prob_bits) + (x % f) + cdf_l[s]

    # Final state, LSB first. After the reversal below it lands at the front of
    # the buffer MSB first, which is what `rans_decode` reads.
    for _ in range(4):
        out.append(x & _BYTE)
        x >>= _RENORM_SHIFT

    out.reverse()
    return bytes(out)


def rans_decode(data, n_symbols, freqs, prob_bits=12):
    """Inverse of `rans_encode`. Returns int32[n_symbols].

    `freqs` and `prob_bits` must be byte-identical to the encode side; there is
    nothing in the stream that would detect a mismatch, it simply decodes to
    different symbols. That is why the payload header in `vqlic/codec.py` carries
    `prob_bits` and either the table itself or enough to rebuild it.
    """
    if n_symbols == 0:
        return np.zeros(0, dtype=np.int32)
    if len(data) < 4:
        raise ValueError(f"stream of {len(data)} bytes cannot hold the rANS state")

    freqs_l = [int(f) for f in freqs]
    cdf_l = [int(c) for c in cdf_from_freqs(freqs)]
    inv_l = inverse_cdf(freqs).tolist()

    mask = (1 << prob_bits) - 1
    x = (data[0] << 24) | (data[1] << 16) | (data[2] << 8) | data[3]
    p = 4
    out = [0] * n_symbols

    for i in range(n_symbols):
        slot = x & mask
        s = inv_l[slot]
        out[i] = s
        x = freqs_l[s] * (x >> prob_bits) + slot - cdf_l[s]
        while x < RANS_L:
            x = (x << _RENORM_SHIFT) | data[p]
            p += 1

    return np.asarray(out, dtype=np.int32)


# ----------------------------------------------------------------- bit packing
def pack_uints(values, bits):
    """Pack unsigned ints at `bits` each, MSB-first, into bytes.

    Used for the raw-index fallback mode and for the transmitted frequency
    table. Vectorized rather than looped: the raw mode packs the same ~181k
    values the coder would have, and a Python bit loop there would dominate the
    whole compress call.

    The tail is zero-padded to a byte boundary, so `unpack_uints` needs the
    count -- which the caller always has, since the token grid is derived from
    the image dimensions in the header.
    """
    v = np.asarray(values, dtype=np.uint64).ravel()
    if bits <= 0 or bits > 32:
        raise ValueError(f"bits must be in 1..32, got {bits}")
    if v.size and int(v.max()) >= (1 << bits):
        raise ValueError(f"value {int(v.max())} does not fit in {bits} bits")
    shifts = np.arange(bits - 1, -1, -1, dtype=np.uint64)
    b = ((v[:, None] >> shifts) & np.uint64(1)).astype(np.uint8).ravel()
    return np.packbits(b).tobytes()


def unpack_uints(data, count, bits):
    """Inverse of `pack_uints`. Returns int64[count]."""
    if count == 0:
        return np.zeros(0, dtype=np.int64)
    need = (count * bits + 7) // 8
    if len(data) < need:
        raise ValueError(f"need {need} bytes for {count} x {bits} bits, "
                         f"got {len(data)}")
    b = np.unpackbits(np.frombuffer(data[:need], dtype=np.uint8))
    b = b[:count * bits].reshape(count, bits).astype(np.uint64)
    weights = (np.uint64(1) << np.arange(bits - 1, -1, -1, dtype=np.uint64))
    return (b * weights).sum(axis=1).astype(np.int64)


def pack_bitmap(mask):
    """Boolean mask -> `ceil(len(mask)/8)` bytes, MSB-first within each byte.

    How the support of a transmitted table is described. `K` bits is cheaper
    than any run-length or delta scheme at these sizes and needs no length
    field: the decoder knows `K` from the header, and the number of frequencies
    that follow is the bitmap's popcount.
    """
    return np.packbits(np.asarray(mask, dtype=bool)).tobytes()


def unpack_bitmap(data, count):
    """Inverse of `pack_bitmap`. Returns a bool array of length `count`."""
    need = bitmap_size(count)
    if len(data) < need:
        raise ValueError(f"need {need} bytes for a {count}-bit map, "
                         f"got {len(data)}")
    bits = np.unpackbits(np.frombuffer(data[:need], dtype=np.uint8))
    return bits[:count].astype(bool)


def bitmap_size(count):
    """Bytes `pack_bitmap` will produce for a mask of `count` entries."""
    return (count + 7) // 8


def packed_size(count, bits):
    """Bytes `pack_uints` will produce. Lets the caller price a mode without
    building it -- which is how `codec.compress` decides on the raw fallback."""
    return (count * bits + 7) // 8
