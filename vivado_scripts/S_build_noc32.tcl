# ============================================================================
# S_build_noc32.tcl — build the N_OC=32 operating point.
#
#   vivado -mode batch -source vivado_scripts/S_build_noc32.tcl
#
# Derived from R_regen_bd_and_build.tcl, which remains the reference flow for
# N_OC=16. The ordered BD recovery sequence below is unchanged and the comments
# there explain WHY each step is where it is — do not reorder.
#
# WHAT THIS BUILD CONTAINS (four changes, deliberately bundled — see below)
#   1. PW N_OC 16 -> 32                     (BD parameter)
#   2. PW partial-batch drain               (src/pw_pixel_major_core.sv)
#   3. PW v3 shadow double-buffer + Option C (same file, already present)
#   4. DW conv_mac_array USE_DSP 1 -> 0     (sources_1/new/dw_fused_core.sv)
#
# WHY BUNDLED, against the usual one-change-per-build rule: N_OC=32 needs 128
# DSPs for the PW grid, so the hard floor is PW 128+16+2 + DW 72+16 = 234 > 220.
# The build is infeasible unless DW's MACs move to fabric in the SAME build.
# The attribution handle is WNS: it must stay positive despite the grid doubling.
#
# PREDICTED (simulation + OOC synthesis, DSP_ALLOCATION_ANALYSIS.md §6-7)
#   DSP        162 / 220        (PW 128 mul + 16 PPU + 2 shell + DW 16 PPU)
#   LUT      ~32,400 / 53,200   (61%)
#   BRAM       73.5 / 140       unchanged — measured invariant in N_OC
#   cyc/group  26 / 55 / 71 / 71 / 109 / 173   for blocks 0..5
#   HW        ~20.76 ms         frame ~28.2 ms, ~35.4 fps
#
# ⚠️ AFTER THIS BUILD the XSA name CHANGES to noc32_cg2048.xsa. The Vitis
# platform must be repointed (§27 C: back up vitis-comp.json, copy the XSA into
# Final_2/hw/, edit BOTH fields). A new name is used deliberately so a forgotten
# repoint fails LOUDLY with a missing file rather than silently running the old
# N_OC=16 bitstream against a new ELF.
# ============================================================================

set ROOT      C:/Users/Fahad/Zynq
set PROJ      $ROOT/Zynq.xpr
set OUT_XSA   $ROOT/noc32_cg2048.xsa

set SET_N_OC  32
set JOBS      8

# Markers proving each change reached the file the IP actually declares.
# Chosen to match real code, not the comments that discuss it.
set PW_MARKERS [list "N_OC+1" "ppu_drain_len" "sha_wr_bank" "ppu_in_flight" \
                     "u_shadow_bram_o" "sha_rd_parity" \
                     {$signed(first_ic ? '0 : acc[oci][p*2])} \
                     {+ $signed({1'b0, p_packed_reg[oci][p][15]})} \
                     "rd_issued_q" "sha_snap" "copy_idx_now" "sha_copy_bank_r" \
                     "in_xfer" "DEFECT P1"]
# 2026-09-03: the five new core markers are the arithmetic defects fixed in
# commit cb16322 (W1 copy data/address skew, W2 copy bank, W3 copy racing the
# next batch, R1 shadow read address, L1 input handshake, M1 missing MAC stage).
# Before that commit THIS declared copy was the stale one -- five weeks behind
# sources_1/new, with none of them -- which is exactly the trap this block
# exists to catch. Without these markers a build silently synthesises a core
# that computes the wrong convolution.
set DW_MARKERS [list ".USE_DSP(0)) u_mac"]
# 2026-08-08: the PW SHELL now carries the block-2 over-consume fix (in_occ +
# unconditional in_rd_en). It is a different file from the core and was not
# previously guarded, so a stale copy would silently reinstate a bug that
# corrupts block 2 in every configuration.
# 2026-09-03: in_rd_en is no longer unconditional and the in_occ guard is gone.
# The core used to latch a beat one cycle before its own registered consume_in
# popped it (DEFECT L1); in_occ was an attempt to treat that symptom here. With
# the core latching only on a real transfer, in_rd_en pops exactly the word
# latched this cycle, so the old marker deliberately no longer matches.
set SH_MARKERS [list "assign in_rd_en      = core_consume_in && core_valid_in;" \
                     "assign core_valid_in = !in_empty;"]
# 2026-08-08: ppu.sv now carries declaration initialisers on its 34 reset-less
# datapath registers (Xilinx maps these to the flop INIT attribute, making the
# power-up-to-zero behaviour the design already relied on explicit). ppu.sv is the
# WORST file in the tree for staleness: the PW IP declares it once and the DW IP
# declares it TWICE, so there are four source paths and several generated copies.
set PPU_MARKERS [list "acc_biased_s0 = '0;" "pre_clamp_s3 = '0;"]

proc die {msg} {
    puts "\n########################################################"
    puts "# ABORT: $msg"
    puts "########################################################\n"
    exit 1
}
proc slurp {f} { set fh [open $f r]; set t [read $fh]; close $fh; return $t }
proc check_markers {f markers what} {
    set t [slurp $f]
    foreach m $markers {
        if {[string first $m $t] < 0} { die "$what: marker '$m' NOT in $f" }
    }
    puts "OK: all [llength $markers] markers present in $f"
}

open_project $PROJ

# ---- pre-flight: markers in the files the IPs actually declare ----
# PW core: Zynq.srcs/component.xml declares src/pw_pixel_major_core.sv (NOT
# sources_1/new, NOT TCSVT). Packaging is MIXED — the AXIS shell comes from
# sources_1/new. Editing the live-tree core alone changes NOTHING.
check_markers $ROOT/Zynq.srcs/src/pw_pixel_major_core.sv $PW_MARKERS "PW core"
# PW shell: declared ONCE in Zynq.srcs/component.xml (unlike the DW sources), so
# sources_1/new is the single authority for it.
check_markers $ROOT/Zynq.srcs/sources_1/new/pw_single_oc_axis.sv $SH_MARKERS "PW shell"
# ppu.sv: shared by both IPs. Check the live tree (PW's declared path AND the DW's
# second declared path) and the DW IP's own copy.
check_markers $ROOT/Zynq.srcs/sources_1/new/ppu.sv $PPU_MARKERS "PPU (live tree)"
check_markers C:/Users/Fahad/ip_repo/dw_fused_axi_1.0/src/ppu.sv $PPU_MARKERS "PPU (packaged DW IP)"

# DW core: THE DOUBLE-DECLARATION TRAP, and it is not in Zynq at all.
# The DW IP is packaged at C:/Users/Fahad/ip_repo/dw_fused_axi_1.0 (outside the
# project tree, absent from §27's paths table until 2026-08-08), and its
# component.xml declares EVERY source file TWICE:
#     ../../Zynq/Zynq.srcs/sources_1/new/dw_fused_core.sv   (live tree)
#     src/dw_fused_core.sv                                  (the IP's own copy)
# Which one synthesises is a coin flip that has genuinely flipped between two
# consecutive builds. generate_target copies from the PACKAGED IP, so editing
# only the live tree changes NOTHING -- that is precisely how the first attempt
# at this build died (the guard below caught it).
# Both declared paths are therefore checked, AND asserted byte-identical.
set DW_LIVE $ROOT/Zynq.srcs/sources_1/new/dw_fused_core.sv
set DW_IP   C:/Users/Fahad/ip_repo/dw_fused_axi_1.0/src/dw_fused_core.sv
check_markers $DW_LIVE $DW_MARKERS "DW core (live tree)"
if {![file exists $DW_IP]} { die "DW IP source missing: $DW_IP" }
check_markers $DW_IP $DW_MARKERS "DW core (packaged IP)"
if {[slurp $DW_LIVE] ne [slurp $DW_IP]} {
    die "DW core copies DIVERGE.\n#   live: $DW_LIVE\n#   ip  : $DW_IP\n#   Both are declared by the IP; a divergence means the build outcome\n#   depends on which one synthesis happens to pick. Sync them."
}
puts "OK: DW core copies byte-identical across both declared paths"

# The other five DW sources are declared twice as well. A divergence in any of
# them is the same trap, so verify the whole set.
foreach f {conv_mac_array.sv dw_banked_window_8x.sv dw_fused_axi.sv dw_fused_axis.sv ppu.sv} {
    set a $ROOT/Zynq.srcs/sources_1/new/$f
    set b C:/Users/Fahad/ip_repo/dw_fused_axi_1.0/src/$f
    if {![file exists $b]} { die "DW IP source missing: $b" }
    if {[slurp $a] ne [slurp $b]} { die "DW source diverges between declared paths: $f" }
}
puts "OK: all 6 DW sources in sync between live tree and packaged IP"

update_ip_catalog -rebuild -scan_changes

# ---- narrow purge: shared source copies only ----
foreach d [list $ROOT/Zynq.gen/sources_1/bd/hw/ipshared \
                $ROOT/Zynq.gen/sources_1/bd/hw/Zynq \
                $ROOT/Zynq.ip_user_files/bd/hw/ipshared \
                $ROOT/Zynq.ip_user_files/bd/hw/Zynq] {
    if {[file exists $d]} {
        puts "purging: $d"
        if {[catch {file delete -force $d} e]} { die "could not delete $d ($e)" }
    }
}

set bd [get_files hw.bd]
if {$bd eq ""} { die "hw.bd not found" }

# reset_target BEFORE regenerating: deleting files on disk does not tell Vivado
# its output products are stale.
reset_target all $bd
open_bd_design $bd

set pwcell [get_bd_cells -quiet pw_single_oc_axis_axi_0]
if {$pwcell eq ""} { die "pw_single_oc_axis_axi_0 not found in the BD" }
set was [get_property CONFIG.N_OC $pwcell]
set_property CONFIG.N_OC $SET_N_OC $pwcell
set now [get_property CONFIG.N_OC $pwcell]
if {$now ne $SET_N_OC} { die "N_OC did not take: asked $SET_N_OC, read back $now" }
puts "OK: N_OC $was -> $now on pw_single_oc_axis_axi_0"

# reset_target invalidates parameter propagation; without -force the BD reports
# "already validated", skips it, and smartconnect fails on si_properties.
if {[catch {validate_bd_design -force} e]} { die "validate_bd_design failed: $e" }
save_bd_design
close_bd_design [get_bd_designs hw]

if {[catch {generate_target all [get_files hw.bd]} e]} { die "generate_target failed: $e" }
catch {export_ip_user_files -of_objects [get_files hw.bd] -no_script -sync -force -quiet}

# ---- guard: EVERY generated copy of both cores must carry its markers ----
proc findall {dir name acc} {
    upvar $acc out
    foreach f [glob -nocomplain -directory $dir *] {
        if {[file isdirectory $f]} { findall $f $name out
        } elseif {[string equal [file tail $f] $name]} { lappend out $f }
    }
}
foreach {fname markers what} [list \
        pw_pixel_major_core.sv $PW_MARKERS "PW core" \
        pw_single_oc_axis.sv   $SH_MARKERS "PW shell" \
        ppu.sv                 $PPU_MARKERS "PPU" \
        dw_fused_core.sv       $DW_MARKERS "DW core"] {
    set copies [list]
    foreach base [list $ROOT/Zynq.gen $ROOT/Zynq.ip_user_files] {
        if {[file exists $base]} { findall $base $fname copies }
    }
    if {[llength $copies] == 0} { die "no generated copy of $fname -- generate_target produced nothing" }
    foreach f $copies { check_markers $f $markers "$what generated copy" }
}

# ---- rebuild ----
reset_run synth_1
launch_runs impl_1 -to_step write_bitstream -jobs $JOBS
wait_on_run impl_1
if {[get_property PROGRESS [get_runs impl_1]] ne "100%"} {
    die "impl_1 did not finish: [get_property STATUS [get_runs impl_1]]"
}

# ---- netlist verification ----
set log [slurp $ROOT/Zynq.runs/synth_1/runme.log]
if {[string first "Synth 8-2490" $log] >= 0} { die "duplicate module definition -- a stale copy may have won" }

open_run impl_1
set wns [get_property SLACK [get_timing_paths -delay_type max]]
set whs [get_property SLACK [get_timing_paths -delay_type min]]
set dsp [llength [get_cells -hier -filter {REF_NAME =~ DSP48*}]]
set lut [llength [get_cells -hier -filter {REF_NAME =~ LUT*}]]
set ram [llength [get_cells -hier -filter {REF_NAME =~ RAMB*}]]

puts "\n================ BUILD RESULT ================"
puts [format "WNS   %+0.3f ns   (was +0.321 at N_OC=16)" $wns]
puts [format "WHS   %+0.3f ns   (was +0.012 -- period-independent, watch it)" $whs]
puts [format "DSP   %d / 220     (predicted 162)" $dsp]
puts [format "LUT   %d / 53200   (predicted ~32400)" $lut]
puts [format "RAMB  %d          (predicted unchanged from 73.5 tiles)" $ram]

# DSP by function, so we can see whether the accumulators were released
array set cnt {}
foreach c [get_cells -hier -filter {REF_NAME =~ DSP48*}] {
    set n [get_property NAME $c]
    if {[regexp {u_mac|conv_mac|p_cas} $n]}            { set k DW_MAC \
    } elseif {[regexp {G_DSP_OC|p_reg|p_packed} $n]}   { set k PW_MUL \
    } elseif {[regexp {p_0_out} $n]}                   { set k PW_ACC \
    } elseif {[regexp {G_PPU|u_ppu|conv_prod} $n]}     { set k PPU \
    } else                                             { set k OTHER }
    if {[info exists cnt($k)]} { incr cnt($k) } else { set cnt($k) 1 }
}
puts "-- DSP by function --"
foreach k [lsort [array names cnt]] { puts [format "   %-8s %4d" $k $cnt($k)] }
puts "=============================================="

if {$wns < 0} {
    puts "\n⚠️  TIMING FAILED. Do NOT program this bitstream."
    puts "   This is the first_ic question becoming real. Levers, in order"
    puts "   (DSP_ALLOCATION_ANALYSIS.md §8.3):"
    puts "     1. extend dw_fused_timing.xdc multicycle from reg_zp_relu_reg\[*\]"
    puts "        to reg_cin_run / reg_n_groups / reg_img_width / reg_n_rows"
    puts "     2. raise implementation replication effort via -directive"
    puts "     3. restructure so the first input channel WRITES instead of"
    puts "        accumulating, removing first_ic from the DSP control path"
    puts "   Report the failing path before changing anything:"
    puts "     report_timing -max_paths 5 -path_type full_clock_expanded"
    die "WNS negative ($wns ns)"
}
if {$dsp > 220} { die "DSP over budget: $dsp" }

write_hw_platform -fixed -include_bit -force -file $OUT_XSA
puts "\nwrote $OUT_XSA"
puts "bitstream: $ROOT/Zynq.runs/impl_1/hw_wrapper.bit"
puts ""
puts "NEXT (§27 C) — Vitis platform repoint, both fields together:"
puts "  1. cp $ROOT/Final_2/vitis-comp.json{,.bak_pre_noc32_repoint}"
puts "  2. cp $OUT_XSA $ROOT/Final_2/hw/noc32_cg2048.xsa"
puts "  3. edit $ROOT/Final_2/vitis-comp.json — BOTH xsa fields"
puts "  4. set PW_N_OC 32 in Final_code_2/src/main.c  <-- MUST match CONFIG.N_OC"
puts "     a mismatch misprograms weights SILENTLY with plausible timing"
puts "  5. program hw_wrapper.bit EXPLICITLY (JTAG Run leaves stale fabric)"
puts "DONE"
