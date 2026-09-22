#!/usr/bin/env bash
# =============================================================================
# update_platform_vq.sh -- repoint the Final_2 Vitis platform at the VQ XSA.
#
# WHY
#   Final_code_2 builds against platform "Final_2", which currently points at
#   noc32_cg2048.xsa -- the PRE-VQ fabric (bitstream md5 21357a97...). Building
#   and running without this step loads a bitstream with no VQ block in it.
#   vq_pl_init() catches that (the ID register will not read 0x56513032) and
#   refuses to report timing, but the platform must be repointed before any
#   hardware VQ measurement means anything.
#
# METHOD
#   This follows the method already used in this project -- see the existing
#   vitis-comp.json.bak_pre_*_repoint backups. A repoint is exactly:
#     1. copy the new .xsa into Final_2/hw/
#     2. edit "xsa" and "xsaPathInPlatform" in Final_2/vitis-comp.json
#     3. rebuild the platform in Vitis, then rebuild Final_code_2
#   Step 3 is left to Vitis: the toolchain here is Vitis 2025.1 (unified flow)
#   while Vivado is 2020.2, and driving a GUI-managed workspace from a script
#   is more likely to corrupt it than to help.
#
# Usage:  bash update_platform_vq.sh [--dry-run] [--verify]
#   --dry-run  report what would change, touch nothing
#   --verify   after rebuilding in Vitis, confirm the platform bitstream
#              now matches impl_1
#
# Close Vitis before running: it holds locks on the workspace.
# =============================================================================
set -u

ZYNQ=/c/Users/Fahad/Zynq
XSA_SRC="$ZYNQ/vq_accel.xsa"
# Name the XSA after the bitstream it carries. A same-named file replaced in
# place is exactly how the ipshared copy went stale during this build; a
# content-derived name removes any chance of a cached path being reused.
BIT_MD5=$(md5sum "$ZYNQ/Zynq.runs/impl_1/hw_wrapper.bit" 2>/dev/null | cut -c1-8)
XSA_NAME="vq_accel_${BIT_MD5:-unknown}.xsa"
PLAT="$ZYNQ/Final_2"
COMP="$PLAT/vitis-comp.json"
HWDIR="$PLAT/hw"
IMPL_BIT="$ZYNQ/Zynq.runs/impl_1/hw_wrapper.bit"
PLAT_BIT="$PLAT/export/Final_2/hw/Final_2.bit"

MODE=apply
case "${1:-}" in
  --dry-run) MODE=dry ;;
  --verify)  MODE=verify ;;
  "")        MODE=apply ;;
  *) echo "unknown option $1"; exit 2 ;;
esac

say () { printf '[PLAT] %s\n' "$*"; }
die () { printf '[PLAT] ERROR: %s\n' "$*" >&2; exit 1; }

md5of () { [ -f "$1" ] && md5sum "$1" | cut -d' ' -f1 || echo "(missing)"; }

# ---- report current state ---------------------------------------------------
[ -f "$COMP" ] || die "missing $COMP -- is the platform still called Final_2?"
CUR=$(grep -oE '"xsa": *"[^"]*"' "$COMP" | sed 's/.*\\\\//; s/"$//')
say "platform currently points at : $CUR"
say "impl_1 bitstream md5         : $(md5of "$IMPL_BIT")"
say "platform bitstream md5       : $(md5of "$PLAT_BIT")"

if [ "$MODE" = verify ]; then
  A=$(md5of "$IMPL_BIT"); B=$(md5of "$PLAT_BIT")
  if [ "$A" = "$B" ] && [ "$A" != "(missing)" ]; then
    say "MATCH -- the platform carries the VQ fabric. Safe to measure."
    exit 0
  fi
  say "MISMATCH -- platform has not been rebuilt against the new XSA yet."
  say "Do NOT trust any PL VQ timing until these match."
  exit 3
fi

[ -f "$XSA_SRC" ] || die "missing $XSA_SRC -- run the Vivado build first"
[ -f "$IMPL_BIT" ] || die "missing $IMPL_BIT -- write_bitstream has not run"

if [ "$MODE" = dry ]; then
  say "would copy   : $XSA_SRC -> $HWDIR/$XSA_NAME"
  say "would set    : \"xsa\": \"C:\\\\Users\\\\Fahad\\\\Zynq\\\\Final_2\\\\hw\\\\$XSA_NAME\""
  say "would set    : \"xsaPathInPlatform\": \"Final_2\\\\hw\\\\$XSA_NAME\""
  say "dry run, nothing changed"
  exit 0
fi

# ---- apply ------------------------------------------------------------------
STAMP=$(date +%Y%m%d_%H%M%S)
cp "$COMP" "$COMP.bak_pre_vq_repoint_$STAMP" || die "cannot back up $COMP"
say "backed up    : $COMP.bak_pre_vq_repoint_$STAMP"

cp "$XSA_SRC" "$HWDIR/$XSA_NAME" || die "cannot copy XSA into $HWDIR"
say "copied       : $HWDIR/$XSA_NAME"

# rewrite exactly the two fields, preserving the doubled backslashes
sed -i \
  -e "s|\"xsa\": \"[^\"]*\"|\"xsa\": \"C:\\\\\\\\Users\\\\\\\\Fahad\\\\\\\\Zynq\\\\\\\\Final_2\\\\\\\\hw\\\\\\\\$XSA_NAME\"|" \
  -e "s|\"xsaPathInPlatform\": \"[^\"]*\"|\"xsaPathInPlatform\": \"Final_2\\\\\\\\hw\\\\\\\\$XSA_NAME\"|" \
  "$COMP" || die "sed failed on $COMP"

NEW=$(grep -oE '"xsa": *"[^"]*"' "$COMP")
say "now reads    : $NEW"
grep -q "$XSA_NAME" "$COMP" || die "repoint did not take -- restore the .bak and do it in the GUI"

cat <<EOF

[PLAT] Repoint done. Remaining steps, in Vitis (2025.1):
[PLAT]   1. open the workspace, let it reload Final_2
[PLAT]   2. build the Final_2 platform   (regenerates the BSP; the address map
[PLAT]      gained vq_pq_axi_0 @ 0x43C20000 and axi_dma_2 @ 0x40420000)
[PLAT]   3. clean + build Final_code_2
[PLAT]   4. program the board with the NEW bitstream, then run
[PLAT]
[PLAT] Confirm before trusting any number:
[PLAT]   bash update_platform_vq.sh --verify
[PLAT] and in the serial log look for:
[PLAT]   [EDGE] PL VQ vs scalar reference: 0 mismatches of 57600 ... -> PASS

EOF
