"""
test_service_model.py -- gate on the known-good values before anything else
runs. If these fail, stop: the model is wrong and every number downstream of it
is meaningless.

  python test_service_model.py
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import service_model as M

fails = 0


def chk(cond, what, got=None, want=None):
    global fails
    ok = bool(cond)
    print("  %-62s %s" % (what, "ok" if ok else "FAIL"))
    if not ok:
        fails += 1
        if got is not None:
            print("      got %s, want %s" % (got, want))


print("service_model.py -- specification gate")
print("=" * 74)

# ---------------------------------------------------------------------------
# 1. The two known-good values from the spec.
#    Deployed VQ mapping, (M,K,Dm) = (8,16,8), Q = 32:
#      c_in  = Dm * S where S = subs_per_batch = Q/K = 2  ->  16
#      c_out = M * K                                       -> 128
# ---------------------------------------------------------------------------
S      = 32 // 16          # subs_per_batch = Q / K
C_IN   = 8 * S             # Dm * S
C_OUT  = 8 * 16            # M * K
gs = M.group_service(C_IN, C_OUT, 32)
chk(gs == 136, "group_service(c_in=%d, c_out=%d, Q=32) == 136 cycles/group"
    % (C_IN, C_OUT), gs, 136)

NPOS   = 90 * 160          # the deployed latent
NGROUP = NPOS // M.L
tot = NGROUP * gs
chk(tot == 244800, "  x %d groups over the 90x160 latent == 244,800 cycles"
    % NGROUP, tot, 244800)

# ---------------------------------------------------------------------------
# 2. group_service internal consistency
# ---------------------------------------------------------------------------
chk(M.group_service(3, 16, 32) == 18, "group_service(3,16,32) == 18  (B=1 branch)")
chk(M.group_service(16, 48, 32) == 57, "group_service(16,48,32) == 57  (B=2)")
chk(M.group_service(48, 64, 32) == 124, "group_service(48,64,32) == 124 (B=2)")
chk(all(M.group_service(c, o, 32) > 0 for c in (1, 3, 8, 240)
        for o in (1, 16, 32, 33, 64, 128, 224)),
    "group_service positive across the feasible c_in/c_out range")

# ---------------------------------------------------------------------------
# 3. G = ceil(W/L), and the correction is free on every published width
# ---------------------------------------------------------------------------
chk(M.groups(1280) == 160 and M.groups(640) == 80
    and M.groups(320) == 40 and M.groups(160) == 20,
    "G exact on the deployed widths (1280/640/320/160)")
chk(M.groups(67) == 9, "G(67) == 9   ceiling, matches dw_fused_axi_order_tb", M.groups(67), 9)
chk(M.groups(1435) == 180, "G(1435) == 180  ceiling, matches dw_fused_axi_tb",
    M.groups(1435), 180)
bad = M.assert_ceiling_is_free()
chk(bad == [], "ceil(W/L) == W//L on every reference width -> no published "
               "number moves", bad, [])

# ---------------------------------------------------------------------------
# 4. The deployed transform reproduces the paper's total
# ---------------------------------------------------------------------------
rows = M.transform([16, 48, 64])
tot = sum(b["T_block"] for b in rows)
chk(abs(tot - 1344632) < 0.5,
    "deployed 16-48-64 total == 1,344,632 cycles", "%.0f" % tot, 1344632)
chk(abs(M.ms(tot) - 13.4463) < 1e-4,
    "                        == 13.4463 ms", "%.4f" % M.ms(tot), 13.4463)

# Per-block against the frozen svc_model.py, which computes max(T_DW, T_PW)
# only. Agreement here is a real check: it holds ONLY if the two DMA terms
# never bind, which is the claim being tested.
# binds returns the dict KEY, so "T_PW"/"T_DW"
EXPECT = [(351127, 518400, "T_PW"), (469300, 410400, "T_DW"), (356932, 223200, "T_DW")]
for b, (edw, epw, ebind) in zip(rows, EXPECT):
    chk(abs(b["T_DW"] - edw) < 0.5 and abs(b["T_PW"] - epw) < 0.5
        and b["binds"] == ebind,
        "block %d: T_DW=%d T_PW=%d binds=%s" % (b["block"], edw, epw, ebind),
        "%.0f/%.0f/%s" % (b["T_DW"], b["T_PW"], b["binds"]),
        "%d/%d/%s" % (edw, epw, ebind))

# ---------------------------------------------------------------------------
# 5. Integrality. Cycles are integers; a fractional service means the spec's
#    plain divisions do not land on the deployed geometry and the report would
#    be quoting a fractional cycle count.
# ---------------------------------------------------------------------------
frac = []
for b in rows:
    for k in ("T_read", "T_DW", "T_PW", "T_write"):
        if abs(b[k] - round(b[k])) > 1e-9:
            frac.append((b["block"], k, b[k]))
chk(frac == [], "every service is integral on the deployed transform", frac, [])

print()
print("%d checks failed" % fails)
print("RESULT: %s" % ("FAIL -- stop, do not run the sweep" if fails else "PASS"))
sys.exit(1 if fails else 0)
