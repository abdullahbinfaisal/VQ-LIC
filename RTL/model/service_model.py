"""
service_model.py -- the analytical service model, implemented from the written
equations alone.

INDEPENDENCE. This file was written from the specification text, not by reading
the RTL. In particular T_DW was implemented without opening the DW core's
scheduling FSM; the only RTL facts used are the core's INTERFACE PORTS, which
name the same quantities the equations do (cin_run, n_groups, img_width,
n_rows, stride2). The point of the exercise is an independent check, so a
disagreement between this file and simulation is a result, not a bug to be
tuned away.

FROZEN CONSTANTS. Do not fit these to anything. If measurement disagrees with
the model, the disagreement is the finding: report it, do not add a correction
term. The paper claims no fitted correction, so quietly fitting one would be
worse than a null result.

ONE DEVIATION FROM THE SPEC AS WRITTEN, and it is deliberate. The spec gives
G = W / L as exact division. The RTL takes n_groups as an input port and every
testbench in the repo computes it as ceil(W/L) -- dw_fused_axi_order_tb runs
W=67 with the comment "not a multiple of 8 -> real tail", and dw_fused_axi_tb
runs W=1435. So exact division is wrong for ragged widths and the ceiling is
used here. Every deployed and reference width (1280, 640, 320, 160) is a
multiple of 8, so no deployed number changes; assert_ceiling_is_free() proves
that rather than asserting it.
"""

from math import ceil

# ---------------------------------------------------------------------------
# Frozen constants
# ---------------------------------------------------------------------------
L           = 8
W_DMA       = 8         # bytes/cycle
F_CLK       = 100e6
R_SH        = 2
DELTA_ACC   = 6
DELTA_PPU   = 2
DELTA_TR    = 1
DELTA_FLUSH = 1
DELTA_ROW   = 4

CONSTANT_NAMES = ("R_SH", "DELTA_ACC", "DELTA_PPU",
                  "DELTA_TR", "DELTA_FLUSH", "DELTA_ROW")


def groups(W, lanes=L):
    """G. Ceiling, not exact division -- see the module docstring."""
    return -(-W // lanes)


# ---------------------------------------------------------------------------
# The four services
# ---------------------------------------------------------------------------
def t_read(H, W, c_in, w_dma=W_DMA):
    """Input DMA. P_in = H*W."""
    return H * W * c_in / float(w_dma)


def t_dw(H, W, c_in, lanes=L, d_flush=DELTA_FLUSH, d_row=DELTA_ROW):
    """T_DW = (H+1) * (c_in*(G + delta_flush) + delta_row)."""
    G = groups(W, lanes)
    return (H + 1) * (c_in * (G + d_flush) + d_row)


def t_write(H, W, c_out, s, w_dma=W_DMA):
    """Output DMA. P = P_in / s^2."""
    P = (H * W) / float(s * s)
    return P * c_out / float(w_dma)


def group_service(c_in, c_out, Q,
                  r_sh=R_SH, d_acc=DELTA_ACC, d_ppu=DELTA_PPU, d_tr=DELTA_TR):
    """Cycles the PW engine spends on one group of L pixels."""
    B      = ceil(c_out / float(Q))
    Q_last = c_out - (B - 1) * Q
    Gam    = c_in + ceil(Q_last / float(r_sh)) + d_acc
    if B == 1:
        return max(c_out + d_ppu, Gam)
    D = max(c_in + d_acc, Q + d_ppu)
    return max(c_out + d_ppu * B,
               c_in + d_acc + (B - 2) * D + max(D + d_tr, Gam))


def t_pw(H, W, c_in, c_out, s, Q, lanes=L, **kw):
    """T_PW = (P / L) * group_service. P is the OUTPUT pixel count."""
    P = (H * W) / float(s * s)
    return (P / float(lanes)) * group_service(c_in, c_out, Q, **kw)


def block(H, W, c_in, c_out, s, Q, lanes=L, w_dma=W_DMA, **kw):
    """All four services plus the binding one.

    T_block = max(T_read, T_DW, T_PW, T_write)
    """
    r = t_read(H, W, c_in, w_dma)
    d = t_dw(H, W, c_in, lanes, kw.get("d_flush", DELTA_FLUSH),
             kw.get("d_row", DELTA_ROW))
    p = t_pw(H, W, c_in, c_out, s, Q, lanes,
             r_sh=kw.get("r_sh", R_SH), d_acc=kw.get("d_acc", DELTA_ACC),
             d_ppu=kw.get("d_ppu", DELTA_PPU), d_tr=kw.get("d_tr", DELTA_TR))
    w = t_write(H, W, c_out, s, w_dma)
    services = {"T_read": r, "T_DW": d, "T_PW": p, "T_write": w}
    binder = max(services, key=lambda k: services[k])
    return dict(H=H, W=W, G=groups(W, lanes), c_in=c_in, c_out=c_out, s=s, Q=Q,
                Ho=-(-H // s), Wo=-(-W // s),
                T_read=r, T_DW=d, T_PW=p, T_write=w,
                T_block=services[binder], binds=binder,
                gs=group_service(c_in, c_out, Q,
                                 r_sh=kw.get("r_sh", R_SH),
                                 d_acc=kw.get("d_acc", DELTA_ACC),
                                 d_ppu=kw.get("d_ppu", DELTA_PPU),
                                 d_tr=kw.get("d_tr", DELTA_TR)))


def transform(cout_list, H0=720, W0=1280, c_in0=3, Q=32, n_stride2=3, **kw):
    """Walk a channel schedule. Each block's output geometry feeds the next."""
    H, W, c_in = H0, W0, c_in0
    out = []
    for j, c_out in enumerate(cout_list):
        s = 2 if j < n_stride2 else 1
        b = block(H, W, c_in, c_out, s, Q, **kw)
        b["block"] = j + 1
        out.append(b)
        H, W, c_in = b["Ho"], b["Wo"], c_out
    return out


def ms(cycles, f=F_CLK):
    return cycles / f * 1e3


# ---------------------------------------------------------------------------
# The ceiling correction is free on every width this project uses
# ---------------------------------------------------------------------------
REFERENCE_WIDTHS = (1280, 640, 320, 160)


def assert_ceiling_is_free(widths=REFERENCE_WIDTHS, lanes=L):
    """Prove, rather than assert, that ceil(W/L) == W//L on every deployed and
    reference width -- so the correction changes no published number."""
    bad = [w for w in widths if groups(w, lanes) != w // lanes]
    return bad


if __name__ == "__main__":
    rows = transform([16, 48, 64])
    print("Deployed transform 16-48-64, L=%d, Q=32, 720p in" % L)
    print("  %-3s %-11s %5s %5s %10s %10s %10s %10s %10s %7s"
          % ("blk", "HxW in", "c_in", "c_out", "T_read", "T_DW", "T_PW",
             "T_write", "T_block", "binds"))
    tot = 0.0
    for b in rows:
        tot += b["T_block"]
        print("  %-3d %-11s %5d %5d %10.0f %10.0f %10.0f %10.0f %10.0f %7s"
              % (b["block"], "%dx%d" % (b["H"], b["W"]), b["c_in"], b["c_out"],
                 b["T_read"], b["T_DW"], b["T_PW"], b["T_write"],
                 b["T_block"], b["binds"]))
    print("  %-38s %10.0f cyc = %.4f ms" % ("TOTAL", tot, ms(tot)))
    bad = assert_ceiling_is_free()
    print("  ceil(W/L) != W//L on reference widths: %s"
          % (bad if bad else "none -- the correction is free"))
