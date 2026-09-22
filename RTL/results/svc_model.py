# =============================================================================
# svc_model.py -- FROZEN analytical service model + hardware feasibility audit
#   for the realized L=8, Q=32 reusable accelerator (bitstream unchanged).
#
# MODEL FREEZE (user decision, 2026-09-02). Do not fit any constant to
# measurements. Provenance of every constant is given inline.
#   * PW shadow transfer: Route A / HALF drain, R_sh = 2 output channels/cycle
#     -> drain of the final batch costs ceil(Q_last/2) cycles.
#     (Silicon-validated; supersedes the FULL-drain note in
#      CHANNEL_SCHEDULE_ANALYSIS.md, which predates board measurement.)
#   * T_DW uses the (H+1)-row form: the RTL runs one extra vertical-flush row
#     pass beyond the H real rows. The superseded H-row form yields 13.4136 ms
#     for 16-48-64; the final (H+1) form yields 13.4463 ms.
# =============================================================================

# ---- RTL-derived constants (STEP 2 list) ------------------------------------
DELTA_ACC   = 6      # PW accumulate pipeline depth
DELTA_PPU   = 2      # PPU (requant) latency
R_SH        = 2      # shadow-transfer rate, output channels / cycle
DELTA_TR    = 1      # batch transition
DELTA_FLUSH = 1      # final flush
DELTA_ROW   = 4      # DW per-row horizontal-flush overhead
L           = 8      # DW/PW lanes (pixels per beat)
Q           = 32     # PW output-channel parallelism (N_OC)
W_DMA       = 8      # AXIS width, bytes
F_CLK       = 100e6

# ---- Realized hardware capacities (from hw.bd / *.xci, NOT RTL defaults) -----
CAP = dict(
    MAX_CG_PRODUCT = 2048,   # dw_banked_window_8x line0/line1 flat depth
    DW_CIN_MAX     = 240,    # dw pend_ram / half_ram depth
    PW_CIN_MAX     = 240,    # pw weight/param BRAM
    PW_COUT_MAX    = 240,    # pw param BRAM address space
    PW_N_OC        = 32,
    PW_W_OC_BATCHES= 240//32,        # = 7  (integer division in RTL!)
    PW_COUT_HW_MAX = 32*(240//32),   # = 224 real max c_out, NOT 240
    PW_W_DEPTH     = (240//32)*240,  # = 1680 weight-BRAM words
    TILE_PIXELS_MAX= 32768,  # hw; software uses PW_TILE_PIXELS = 16384
    SW_TILE_PIXELS = 16384,
    N_LANES        = 8,
    DESC_FIELD_MAX = 4095,   # cin_run/n_groups/img_width/n_rows are [11:0]
    DMA_LEN_MAX    = (1<<26)-1,   # c_sg_length_width = 26
    ACC_WIDTH      = 24,     # pw accumulator: needs c_in*127*255 < 2^23
)

def ceil_div(a,b): return -(-a//b)

# ---- PW group service, Route A half drain -----------------------------------
def pw_cyc_per_group(cin, cout, Q=Q):
    B  = max(1, ceil_div(cout, Q))
    Ql = cout - (B-1)*Q
    d  = max(cin + DELTA_ACC, Q + DELTA_PPU)          # steady-state group period
    if B == 1:
        return max(cout + DELTA_PPU, cin + ceil_div(Ql, R_SH) + DELTA_ACC)
    return max(cout + DELTA_PPU*B,
               cin + DELTA_ACC + (B-2)*d
                   + max(d + DELTA_TR, cin + ceil_div(Ql, R_SH) + DELTA_ACC))

# ---- DW service, (H+1)-row form ---------------------------------------------
def dw_cycles(cin, H_in, W_in):
    G = W_in // L
    return (H_in + DELTA_FLUSH) * (cin * (G + 1) + DELTA_ROW)

def pw_cycles(cin, cout, H_out, W_out):
    return pw_cyc_per_group(cin, cout) * (H_out * W_out // L)

# ---- Whole-network walk ------------------------------------------------------
def walk(cout_list, n_stride2=3, cin0=3, H0=720, W0=1280):
    """Yield a dict per block. First n_stride2 blocks are stride 2 (the
    realized firmware convention: dw_str = {2,2,2,1,1,...})."""
    H, W, cin = H0, W0, cin0
    for j, cout in enumerate(cout_list):
        s   = 2 if j < n_stride2 else 1
        G   = W // L
        Ho, Wo = (H+s-1)//s, (W+s-1)//s
        tdw = dw_cycles(cin, H, W)
        tpw = pw_cycles(cin, cout, Ho, Wo)
        yield dict(block=j+1, H=H, W=W, G=G, stride=s, cin=cin, cout=cout,
                   Ho=Ho, Wo=Wo, cg=G*cin, t_dw=tdw, t_pw=tpw,
                   t_blk=max(tdw,tpw), bind="DW" if tdw>=tpw else "PW")
        H, W, cin = Ho, Wo, cout

def predict(cout_list, **kw):
    b = list(walk(cout_list, **kw))
    cyc = sum(x['t_blk'] for x in b)
    return b, cyc, cyc/F_CLK*1e3

# ---- Feasibility -------------------------------------------------------------
def check(b):
    """All hard runtime limits whose violation ALIASES/TRUNCATES silently."""
    C=CAP; B_batches = max(1, ceil_div(b['cout'], C['PW_N_OC']))
    tp = min(C['SW_TILE_PIXELS'], b['Ho']*b['Wo'])
    return [
      ("G*cin<=MAX_CG_PRODUCT", b['cg'],              C['MAX_CG_PRODUCT']),
      ("cin<=DW_CIN_MAX",       b['cin'],             C['DW_CIN_MAX']),
      ("cin<=PW_CIN_MAX",       b['cin'],             C['PW_CIN_MAX']),
      ("cout<=PW_COUT_HW_MAX",  b['cout'],            C['PW_COUT_HW_MAX']),
      ("batches<=W_OC_BATCHES", B_batches,            C['PW_W_OC_BATCHES']),
      ("B*cin<=PW_W_DEPTH",     B_batches*b['cin'],   C['PW_W_DEPTH']),
      ("tile_px<=TILE_PX_MAX",  tp,                   C['TILE_PIXELS_MAX']),
      ("descr fields<=4095",    max(b['cin'],b['G'],b['W'],b['H']), C['DESC_FIELD_MAX']),
      ("DW in bytes<=DMA_LEN",  b['H']*b['W']*b['cin'],  C['DMA_LEN_MAX']),
      ("PW out bytes<=DMA_LEN", tp*b['cout'],         C['DMA_LEN_MAX']),
      ("acc: cin*127*255<2^23", b['cin']*127*255,     (1<<23)-1),
      ("last tile %% N_LANES",  (b['Ho']*b['Wo']) % C['N_LANES'], 0),
    ]
