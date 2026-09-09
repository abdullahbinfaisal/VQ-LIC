"""
vq_model.py -- VQ service model for the SHARED PW engine.

Built entirely on the FROZEN analysis model in svc_model.py: it imports
pw_cyc_per_group() unchanged and only supplies the (cin, cout, groups) that
VQ mode presents to that same engine. No constant is re-fitted here.

WHY THE PW MODEL APPLIES UNCHANGED. In VQ mode the core still runs the
ordinary load/compute/drain schedule -- the VQ branch snoops ppu_valid_in
and ppu_acc_in and never alters MAC scheduling or the drain (see
pw_pixel_major_core.sv, generate block G_VQ). So a VQ "group" costs exactly
pw_cyc_per_group(cin_mac, cout_total). What VQ changes is only what those
numbers MEAN:

    cout_total = M * K        one PW output channel per codeword
    subs_per_batch = Q // K   sub-codebooks that fit in one batch of Q
                              output channels (0 if K > Q)
    cin_mac    = Dsub * max(1, subs_per_batch)
                              the MAC window: block-diagonal packing lets
                              subs_per_batch sub-codebooks share a batch,
                              each on its own Dsub input channels
    groups     = NPOS / L

The block-diagonal packing is the only reason K < Q helps: with K=16 and
Q=32 two sub-codebooks ride in one batch, so M=8 needs 4 batches, not 8.

DEPLOYED (2026-09-09): M=4, K=64, Dsub=16. K now EXCEEDS Q, so one
sub-codebook spans two batches, cout_total = 256 and B = 8. The per-group cost
272 is CONFIRMED AGAINST THE RTL, not only modelled -- tb_pw_vq reports
272.00 cycles/group, min 272, max 272, over 63 gaps. The legacy M=8/K=16 point
reports 136, and 2*136 = 272 exactly, because both are bound by the output
drain term c_out + delta_ppu*B, which is additive in both c_out and B.
"""
import sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from svc_model import pw_cyc_per_group, F_CLK, L, Q as Q_DEFAULT

NPOS = 90 * 160          # 14,400 latent positions, 90x160 latent map
DIM = 64                 # latent channels entering VQ


def vq_shape(M, K, Q=Q_DEFAULT, dim=DIM):
    """(cin_mac, cout_total, batches, subs_per_batch) for one VQ configuration."""
    dsub = dim // M
    subs_per_batch = max(1, Q // K)          # K > Q  -> one sub-codebook spans
    if K > Q:                                #          several batches
        subs_per_batch = 1
    cin_mac = dsub * subs_per_batch
    cout_total = M * K
    batches = -(-cout_total // Q)
    return cin_mac, cout_total, batches, subs_per_batch, dsub


def vq_cycles(M, K, Q=Q_DEFAULT, npos=NPOS, lanes=L, dim=DIM):
    cin_mac, cout_total, _, _, _ = vq_shape(M, K, Q, dim)
    groups = npos // lanes
    return groups * pw_cyc_per_group(cin_mac, cout_total, Q=Q), groups


def vq_ms(M, K, Q=Q_DEFAULT):
    cyc, _ = vq_cycles(M, K, Q)
    return cyc / F_CLK * 1e3


def report(M, K, Q=Q_DEFAULT, label=""):
    cin_mac, cout_total, batches, spb, dsub = vq_shape(M, K, Q)
    cyc, groups = vq_cycles(M, K, Q)
    per_grp = cyc // groups
    print("  %-28s M=%-3d K=%-4d Dsub=%-3d | cin_mac=%-3d cout=%-5d B=%-3d "
          "subs/batch=%d | %4d cyc/grp x %d grp = %9d cyc = %8.4f ms"
          % (label, M, K, dsub, cin_mac, cout_total, batches, spb,
             per_grp, groups, cyc, cyc / F_CLK * 1e3))
    return cyc


if __name__ == "__main__":
    print("VQ on the SHARED PW engine (L=%d, Q=%d, %.0f MHz, %d positions)"
          % (L, Q_DEFAULT, F_CLK / 1e6, NPOS))
    print()
    print("DEPLOYED, superseded and legacy configurations on the shared engine:")
    report(4, 64,  label="DEPLOYED  M=4  K=64")
    report(8, 16,  label="legacy    M=8  K=16")
    report(4, 256, label="original  M=4  K=256")
    print()
    print("Full M,K grid at 32 bits/position (M*log2(K) == 32) -- the")
    print("iso-rate family the selected point was chosen from:")
    print()
    for M, K in [(2, 65536), (4, 256), (8, 16), (16, 4), (32, 2)]:
        if 64 % M:
            continue
        bits = M * (K.bit_length() - 1)
        if bits != 32:
            continue
        report(M, K, label="M=%d K=%d" % (M, K))
    print()
    print("Other M,K on the grid (NOT iso-rate; listed for completeness):")
    for M, K in [(8, 32), (8, 64), (8, 256), (4, 16), (4, 32),
                 (4, 64), (16, 16), (16, 8), (8, 8), (2, 256)]:
        if 64 % M:
            continue
        report(M, K, label="M=%d K=%d (%d bit/pos)"
               % (M, K, M * (K.bit_length() - 1)))
