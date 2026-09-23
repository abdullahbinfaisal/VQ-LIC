# ============================================================================
# P_rebuild_pw_ppu_fix.tcl — rebuild the PW IP after the ppu_issue_idx width
# fix, with a HARD GUARD that the fix actually reached the synthesized netlist.
#
#   vivado -mode batch -source vivado_scripts/P_rebuild_pw_ppu_fix.tcl
#
# CLOSE THE VIVADO GUI FIRST. This opens Zynq.xpr and resets synth_1/impl_1;
# running it while another process holds the project can corrupt it.
#
# WHY THE GUARD EXISTS (DW_PW_FUSION_PLAN_V2.md §0, §0.1): this project has
# twice drawn conclusions from builds whose RTL never reached the netlist,
# because a stale generated copy won over the edited source. Both times the
# tell was a "bit-identical" result misread as evidence about the hypothesis.
# So this script FAILS LOUDLY rather than producing a bitstream you might
# trust. Never relax the guard to get a build through.
#
# The fix under test (pw_pixel_major_core.sv):
#   logic [$clog2(N_OC>1?N_OC+1:2)-1:0] ppu_issue_idx;   // was $clog2(N_OC)
# $clog2(N_OC) cannot represent N_OC when N_OC is a power of two, so the
# P_DRAIN issue guard never went false at N_OC=8/16.
# ============================================================================

set ROOT      C:/Users/Fahad/Zynq
set PROJ      $ROOT/Zynq.xpr
set OUT_XSA   $ROOT/noc8_ppufix.xsa
set MARKER    "N_OC+1"
set JOBS      8

proc die {msg} {
    puts "\n########################################################"
    puts "# ABORT: $msg"
    puts "########################################################\n"
    exit 1
}

# ---------------------------------------------------------------- 1. open
if {![file exists $PROJ]} { die "project not found: $PROJ" }
open_project $PROJ

# ------------------------------------------------- 2. source-of-truth check
# The PW IP builds from the TCSVT fork, NOT the live Zynq tree. Verify the fix
# is in the file the IP actually declares before spending 40 minutes on it.
set IP_SRC $ROOT/../TCSVT/ip_repo/pw_single_oc_axis_axi_1.0/src/pw_pixel_major_core.sv
if {![file exists $IP_SRC]} { die "PW IP source not found: $IP_SRC" }
set fh [open $IP_SRC r]; set ip_txt [read $fh]; close $fh
if {[string first $MARKER $ip_txt] < 0} {
    die "fix marker '$MARKER' NOT in the IP source $IP_SRC -- nothing to build"
}
puts "OK: fix present in IP source (TCSVT fork)"

# ---------------------------------------------------- 3. purge stale copies
update_ip_catalog -rebuild -scan_changes

set bd [get_files hw.bd]
if {$bd eq ""} { die "hw.bd not found in project" }
reset_target all $bd

# DO NOT widen these paths. Deleting all of .../bd/hw (rather than just the
# stale duplicate-source subtree .../bd/hw/Zynq and the shared IP sources)
# removes the generated output products of EVERY IP in the BD, and the Xilinx
# smartconnects then fail to regenerate with
#   ERROR: [Ipptcl 7-5] XIT evaluation error: key "si_properties" not known
# taking BD HDL generation down with them. Tried 2026-07-30; recovery needed
# validate_bd_design -force (see R_regen_bd_and_build.tcl).
foreach d [list $ROOT/Zynq.gen/sources_1/bd/hw/Zynq \
                $ROOT/Zynq.gen/sources_1/bd/hw/ipshared \
                $ROOT/Zynq.ip_user_files/bd/hw/Zynq \
                $ROOT/Zynq.ip_user_files/bd/hw/ipshared] {
    if {[file exists $d]} {
        puts "purging generated dir: $d"
        if {[catch {file delete -force $d} e]} { die "could not delete $d ($e)" }
    }
}

# ------------------------------------------------------- 4. regenerate + guard
generate_target all $bd
catch {export_ip_user_files -of_objects $bd -no_script -sync -force -quiet}

# EVERY generated copy of the PW core must carry the fix.
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
if {[llength $copies] == 0} { die "no generated copy of pw_pixel_major_core.sv found -- generate_target did not run?" }
foreach f $copies {
    set fh [open $f r]; set t [read $fh]; close $fh
    if {[string first $MARKER $t] < 0} {
        die "STALE generated copy WITHOUT the fix: $f"
    }
    puts "OK: fix present in $f"
}

# ------------------------------------------------------------- 5. rebuild
reset_run synth_1
launch_runs impl_1 -to_step write_bitstream -jobs $JOBS
wait_on_run impl_1

if {[get_property PROGRESS [get_runs impl_1]] ne "100%"} {
    die "impl_1 did not finish: [get_property STATUS [get_runs impl_1]]"
}

# --------------------------------------------- 6. netlist-level verification
set synth_log $ROOT/Zynq.runs/synth_1/runme.log
if {![file exists $synth_log]} { die "synth log missing: $synth_log" }
set fh [open $synth_log r]; set log [read $fh]; close $fh

# (a) no duplicate module definitions (the DW double-declaration failure mode)
if {[string first "Synth 8-2490" $log] >= 0} {
    die "duplicate module definition in synthesis -- a stale copy may have won"
}
# (b) which pw_pixel_major_core was ACTUALLY synthesized
foreach line [split $log "\n"] {
    if {[string match "*Synth 8-6157*pw_pixel_major_core*" $line]} {
        puts "SYNTHESIZED: $line"
    }
}

# ------------------------------------------------------------- 7. export
write_hw_platform -fixed -include_bit -force -file $OUT_XSA
puts "\nwrote $OUT_XSA"
puts "bitstream: $ROOT/Zynq.runs/impl_1/hw_wrapper.bit"
puts "\nNEXT: repoint Final_2/vitis-comp.json at the new XSA, then verify with"
puts "  unzip -p <xsa> hw.hwh | grep -o 'N_OC\" VALUE=\"\[0-9\]*\"'"
puts "DONE"
