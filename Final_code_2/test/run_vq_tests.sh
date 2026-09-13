#!/bin/bash
# ============================================================================
# run_vq_tests.sh -- the whole VQ verification suite, both quantiser profiles.
#
#   ./run_vq_tests.sh          host-side tests only (gcc)
#   ./run_vq_tests.sh --rtl    also the xsim benches
#
# For --rtl, point XILINX_VIVADO at a Vivado install (or put xvlog on PATH):
#
#   XILINX_VIVADO=/c/SPROJ/Vivado/2020.2 ./run_vq_tests.sh --rtl
#
# NOTE the path. The real 2020.2 install is under C:/SPROJ, NOT C:/Xilinx --
# C:/Xilinx/Vivado/2020.2 has no bin/ and is not the install. See CLAUDE.md.
# 2025.1 at C:/Xilinx/2025.1/Vivado also works for SIMULATION, but it has no
# Zynq-7000 devices, so it must never supply a synthesis or timing number.
#
# PROFILES. Everything is built twice, from ONE source each time:
#   VQPW_PROFILE=1  DEPLOYED  M=4, K=64, Dsub=16   (default)
#   VQPW_PROFILE=0  LEGACY    M=8, K=16, Dsub=8    (regression)
# A change that breaks the legacy profile breaks the geometry the 2026-09-05
# board run measured, so it is worth keeping green.
# ============================================================================
set -u
cd "$(dirname "$0")"
SRC=../src
FAIL=0

run() {  # run <label> <cmd...>
  local label="$1"; shift
  printf '%-46s ' "$label"
  if out=$("$@" 2>&1); then
    echo "$out" | grep -qE "RESULT: PASS|0 checks failed|0 failures" \
      && echo "PASS" || { echo "PASS (no verdict line)"; }
  else
    echo "FAIL"; echo "$out" | tail -20; FAIL=1
  fi
}

echo "=== host-side ==="
for prof in 1 0; do
  tag=$([ "$prof" = 1 ] && echo DEPLOYED || echo LEGACY)
  gcc -O2 -Wall -I$SRC -DVQPW_PROFILE=$prof \
      -o vq_pw_golden_$prof.exe vq_pw_golden_test.c $SRC/vq_pw.c || FAIL=1
  run "golden model, $tag" ./vq_pw_golden_$prof.exe

  gcc -O2 -Wall -Istub -I$SRC -DVQPW_PROFILE=$prof \
      -o vq_pw_pl_prog_$prof.exe vq_pw_pl_prog_test.c \
      $SRC/vq_pw_pl.c $SRC/vq_pw.c stub/stub_defs.c || FAIL=1
  run "driver programming, $tag" ./vq_pw_pl_prog_$prof.exe

  gcc -O2 -Wall -I$SRC -DVQPW_PROFILE=$prof \
      -o rc_geom_$prof.exe rc_geometry_test.c $SRC/range_coder.c $SRC/vq_pw.c || FAIL=1
  run "range coder geometry, $tag" ./rc_geom_$prof.exe

  gcc -O2 -Wall -I$SRC -DVQPW_PROFILE=$prof \
      -o vq_slot_probe_$prof.exe vq_slot_probe_test.c $SRC/vq_pw.c || FAIL=1
  run "slot-probe premise, $tag" ./vq_slot_probe_$prof.exe

  gcc -O2 -Wall -I$SRC -DVQPW_PROFILE=$prof \
      -o vq_stale_arb_$prof.exe vq_stale_arbitration_test.c $SRC/vq_pw.c || FAIL=1
  run "stale-slot arbitration, $tag" ./vq_stale_arb_$prof.exe
done

# rANS bring-up, RANS_GUIDE.md section 12. Profile-independent: the coder does
# not read vq_pw.h. Stages 0 and 1 are byte-for-byte test vectors from the
# guide. Stage 2 is a round trip on SYNTHETIC tables -- it proves encoder and
# decoder are inverses, NOT compatibility, and does not stand in for stage 3.
gcc -std=c11 -O2 -Wall -Wextra -DRANS_DEBUG -I$SRC \
    -o rans_stage01.exe rans_stage01_test.c $SRC/rans.c || FAIL=1
run "rANS stages 0+1, test vectors A and B" ./rans_stage01.exe
gcc -std=c11 -O2 -Wall -Wextra -DRANS_DEBUG -I$SRC \
    -o rans_stage2.exe rans_stage2_test.c $SRC/rans.c || FAIL=1
run "rANS stage 2, round trip, synthetic tables" ./rans_stage2.exe
# Stage 3 SELF-CHECK only: section 3.5 hand vectors, ROM reconstruction, the
# 17-byte header, end-to-end payload and the mode-0 fallback. Acceptance needs
# the reference payload: ./rans_stage3.exe DIR (see the usage it prints).
gcc -std=c11 -O2 -Wall -Wextra -DRANS_DEBUG -I$SRC \
    -o rans_stage3.exe rans_stage3_test.c $SRC/rans.c || FAIL=1
run "rANS stage 3 self-check, NOT acceptance" ./rans_stage3.exe
gcc -std=c11 -O2 -Wall -Wextra -DRANS_DEBUG -I$SRC \
    -o rans_edge.exe rans_edge_test.c $SRC/rans_edge.c $SRC/rans.c || FAIL=1
run "rANS edge stage glue, synthetic, lossless" ./rans_edge.exe
# The divide-free encoder the board runs must write the divide encoder bytes:
# exactness bound for every fs, every reachable quotient at its worst
# remainder, and whole streams in four slicing schedules.
gcc -std=c11 -O2 -Wall -Wextra -DRANS_DEBUG -I$SRC \
    -o rans_recip.exe rans_recip_test.c $SRC/rans.c || FAIL=1
run "rANS divide-free encoder, byte-identical" ./rans_recip.exe

if [ "${1:-}" = "--rtl" ]; then
  XB="${XILINX_VIVADO:-}"
  [ -n "$XB" ] && XB="$XB/bin" || XB="$(dirname "$(command -v xvlog 2>/dev/null)" 2>/dev/null)"
  if [ -z "$XB" ] || [ ! -x "$XB/xvlog" ]; then
    echo; echo "xsim not found -- set XILINX_VIVADO or put xvlog on PATH"; exit 1
  fi
  # ABSOLUTE: the benches are elaborated inside a temp directory, so a
  # relative path here would not resolve there.
  RTL=$(cd ../../Zynq.srcs/sources_1/new && pwd)
  D=$(mktemp -d); mkdir -p "$D/vq64" "$D/vq16"
  gcc -O2 -I$SRC -o gen64.exe gen_vq_vectors.c $SRC/vq_pw.c
  gcc -O2 -I$SRC -DVQPW_PROFILE=0 -o gen16.exe gen_vq_vectors.c $SRC/vq_pw.c
  GEN64=$(pwd)/gen64.exe; GEN16=$(pwd)/gen16.exe
  ( cd "$D" && "$GEN64" 64 0 vq64 >/dev/null )
  echo; echo "=== RTL (xsim) ==="
  ( cd "$D"
    "$XB/xvlog" -sv --nolog "$RTL/pw_pixel_major_core.sv" "$RTL/ppu.sv" \
        "$RTL/pw_single_oc_axis.sv" "$RTL/pw_single_oc_axis_axi.sv" \
        "$RTL/tb_pw_vq.sv" "$RTL/tb_pw_vq_guard.sv" "$RTL/tb_pw_axi_vq.sv" \
        > xvlog.txt 2>&1 ) || { echo "xvlog failed"; tail -20 "$D/xvlog.txt"; exit 1; }
  for sc in 0 1 2; do
    ( cd "$D" && "$GEN64" 64 $sc vq64 >/dev/null \
                && "$GEN16" 64 $sc vq16 >/dev/null )
    # tb_pw_axi_vq_stale parks one beat on the input across the start. That
    # models a previous run -- the analysis convolution shares this port --
    # and it is the only bench that ever reproduced the board's one-channel
    # offset. Without it, pw_single_oc_axis can regress silently.
    for top in tb_pw_vq tb_pw_vq_legacy tb_pw_axi_vq tb_pw_axi_vq_legacy tb_pw_axi_vq_stale tb_pw_axi_vq_gap; do
      ( cd "$D"
        "$XB/xelab" --nolog -debug off -O2 -L xpm "$top" -s "s_$top" >/dev/null 2>&1
        "$XB/xsim" --nolog "s_$top" -runall > "o_$top.txt" 2>&1 )
      printf '%-46s ' "scenario $sc, $top"
      if grep -q "RESULT: PASS" "$D/o_$top.txt"; then
        echo "PASS  $(grep -oE 'cycles/group: mean [0-9.]+' "$D/o_$top.txt")"
      else
        echo "FAIL"; tail -20 "$D/o_$top.txt"; FAIL=1
      fi
    done
  done
  ( cd "$D"
    "$XB/xelab" --nolog -debug off -O2 -L xpm tb_pw_vq_guard -s s_guard >/dev/null 2>&1
    "$XB/xsim" --nolog s_guard -runall > o_guard.txt 2>&1 )
  printf '%-46s ' "configuration guard"
  grep -q "0 failures" "$D/o_guard.txt" \
    && echo "PASS  $(grep -oE '[0-9]+ cases' "$D/o_guard.txt")" \
    || { echo "FAIL"; tail -25 "$D/o_guard.txt"; FAIL=1; }
  rm -rf "$D"
fi

echo
[ $FAIL -eq 0 ] && echo "ALL PASS" || echo "SOMETHING FAILED"
exit $FAIL
