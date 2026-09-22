# ============================================================================
# R_regen_bd_and_build.tcl — recover BD output products, then rebuild with the
# PW ppu_issue_idx fix and verify it reached the netlist.
#
#   vivado -mode batch -source vivado_scripts/R_regen_bd_and_build.tcl
#
# WHY THIS EXISTS
# P_rebuild_pw_ppu_fix.tcl purged Zynq.gen/sources_1/bd/hw and
# Zynq.ip_user_files/bd/hw WHOLESALE. That was too broad: it removed the
# generated output products of every IP in the BD, not just the PW ones. The
# doc's original remedy targeted only the stale duplicate-source subtree
# .../bd/hw/Zynq/. generate_target then failed with
#     ERROR: [Ipptcl 7-5] XIT evaluation error: key "si_properties" not known
#     ERROR: [IP_Flow 19-3541] Failed to generate IP 'smartconnect_0'
# because reset_target discarded parameter-propagation state while the BD still
# reported "already validated", so propagation was skipped and smartconnect's
# si_properties dict was never rebuilt. validate_bd_design -force rebuilds it.
#
# hw.bd itself lives in Zynq.srcs and was never touched, so this is recoverable.
# ============================================================================

set ROOT      C:/Users/Fahad/Zynq
set PROJ      $ROOT/Zynq.xpr
set OUT_XSA   $ROOT/noc16_cg2048.xsa

# N_OC on the PW BD cell. MUST equal PW_N_OC in main.c -- it drives
# cout_rounded, the bank select (oc % N_OC) and the weight BRAM offset
# ((oc / N_OC) * Cin). A mismatch misprograms weights SILENTLY, with no error
# and plausible-looking timing. Set to "" to leave the BD parameter untouched.
set SET_N_OC  16
set MARKER    "N_OC+1"
set JOBS      8

proc die {msg} {
    puts "\n########################################################"
    puts "# ABORT: $msg"
    puts "########################################################\n"
    exit 1
}

open_project $PROJ

# ---- fix must be in the file the PW IP actually declares ----
# CORRECTED 2026-07-30. This is NOT the TCSVT fork. The PW IP the project
# builds is packaged at C:/Users/Fahad/Zynq/Zynq.srcs/component.xml
# (<spirit:name>pw_single_oc_axis_axi</spirit:name>), and Zynq.srcs IS a loaded
# user IP repository while TCSVT/ip_repo is NOT. That component.xml declares
# src/pw_pixel_major_core.sv, i.e. Zynq.srcs/src/ -- NOT sources_1/new/ and NOT
# TCSVT. Note the packaging is MIXED: the AXIS shell comes from
# sources_1/new/pw_single_oc_axis.sv (the live tree) while the core comes from
# src/. Editing the live-tree core alone changes NOTHING in the netlist.
# An earlier note claiming "the PW IP builds from TCSVT" was wrong; a build
# checked against the TCSVT copy passed pre-flight and was still stale.
set IP_SRC $ROOT/Zynq.srcs/src/pw_pixel_major_core.sv
set fh [open $IP_SRC r]; set ip_txt [read $fh]; close $fh
if {[string first $MARKER $ip_txt] < 0} { die "fix marker '$MARKER' not in $IP_SRC" }
puts "OK: fix present in the REAL IP source ($IP_SRC)"

update_ip_catalog -rebuild -scan_changes

# ---- narrow purge: shared source copies only ----
# ipshared holds copies of IP sources; deleting it forces a re-copy from the
# packaged IP. Do NOT extend this to .../bd/hw itself -- that removes every
# IP's generated output products and the smartconnects then fail to regenerate.
foreach d [list $ROOT/Zynq.gen/sources_1/bd/hw/ipshared \
                $ROOT/Zynq.gen/sources_1/bd/hw/Zynq \
                $ROOT/Zynq.ip_user_files/bd/hw/ipshared \
                $ROOT/Zynq.ip_user_files/bd/hw/Zynq] {
    if {[file exists $d]} {
        puts "purging: $d"
        if {[catch {file delete -force $d} e]} { die "could not delete $d ($e)" }
    }
}

# ---- rebuild parameter propagation, then regenerate ----
set bd [get_files hw.bd]
if {$bd eq ""} { die "hw.bd not found" }

# reset_target FIRST: deleting files on disk does NOT tell Vivado its output
# products are stale -- it tracks generation state internally, so a purge alone
# leaves generate_target believing everything is current and it regenerates
# NOTHING (observed 2026-07-30: the guard then found no copy to check at all).
reset_target all $bd

# ...then rebuild parameter propagation, which reset_target invalidates. Without
# this the smartconnects fail with `key "si_properties" not known in dictionary`
# because the BD still reports "already validated" and skips propagation.
open_bd_design $bd

if {$SET_N_OC ne ""} {
    set pwcell [get_bd_cells -quiet pw_single_oc_axis_axi_0]
    if {$pwcell eq ""} { die "pw_single_oc_axis_axi_0 not found in the BD" }
    set was [get_property CONFIG.N_OC $pwcell]
    set_property CONFIG.N_OC $SET_N_OC $pwcell
    set now [get_property CONFIG.N_OC $pwcell]
    if {$now ne $SET_N_OC} { die "N_OC did not take: asked $SET_N_OC, read back $now" }
    puts "OK: N_OC $was -> $now on pw_single_oc_axis_axi_0"
}

if {[catch {validate_bd_design -force} e]} { die "validate_bd_design failed: $e" }
save_bd_design
close_bd_design [get_bd_designs hw]

if {[catch {generate_target all [get_files hw.bd]} e]} {
    die "generate_target still failing: $e"
}
catch {export_ip_user_files -of_objects [get_files hw.bd] -no_script -sync -force -quiet}

# ---- guard: every generated copy of the PW core must carry the fix ----
proc findall {dir name acc} {
    upvar $acc out
    foreach f [glob -nocomplain -directory $dir *] {
        if {[file isdirectory $f]} { findall $f $name out
        } elseif {[string equal [file tail $f] $name]} { lappend out $f }
    }
}
set copies [list]
foreach base [list $ROOT/Zynq.gen $ROOT/Zynq.ip_user_files] {
    if {[file exists $base]} { findall $base pw_pixel_major_core.sv copies }
}
if {[llength $copies] == 0} { die "no generated copy of pw_pixel_major_core.sv -- generate_target produced nothing" }
foreach f $copies {
    set fh [open $f r]; set t [read $fh]; close $fh
    if {[string first $MARKER $t] < 0} { die "STALE generated copy WITHOUT the fix: $f" }
    puts "OK: fix present in $f"
}

# ---- rebuild ----
reset_run synth_1
launch_runs impl_1 -to_step write_bitstream -jobs $JOBS
wait_on_run impl_1
if {[get_property PROGRESS [get_runs impl_1]] ne "100%"} {
    die "impl_1 did not finish: [get_property STATUS [get_runs impl_1]]"
}

# ---- netlist-level verification ----
set fh [open $ROOT/Zynq.runs/synth_1/runme.log r]; set log [read $fh]; close $fh
if {[string first "Synth 8-2490" $log] >= 0} { die "duplicate module definition -- a stale copy may have won" }
foreach line [split $log "\n"] {
    if {[string match "*Synth 8-6157*pw_pixel_major_core*" $line]} { puts "SYNTHESIZED: $line" }
}

write_hw_platform -fixed -include_bit -force -file $OUT_XSA
puts "\nwrote $OUT_XSA"
puts "bitstream: $ROOT/Zynq.runs/impl_1/hw_wrapper.bit"
puts "DONE"
