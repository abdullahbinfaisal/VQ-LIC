# ============================================================================
# V_verify_netlist_and_export.tcl — prove the ppu_issue_idx fix is in the
# SYNTHESIZED NETLIST (not just in a source file), then export the XSA.
#
#   vivado -mode batch -source vivado_scripts/V_verify_netlist_and_export.tcl
#
# The fix widens ppu_issue_idx from $clog2(N_OC) to $clog2(N_OC+1) bits. At
# N_OC=8 that is 3 bits -> 4 bits. Counting the flops of that register in the
# post-synth netlist is the only check that cannot be fooled by a stale source
# copy, a wrong IP repository, or a guard that validated the wrong file --
# all three of which happened on 2026-07-30 before this existed.
#
# EXPECT: 4 flops (bits 0..3). 3 flops means the OLD RTL is in the netlist.
# ============================================================================

set ROOT    C:/Users/Fahad/Zynq
set PROJ    $ROOT/Zynq.xpr
set OUT_XSA $ROOT/noc16_cg2048.xsa
# EXPECT is derived from the build's actual N_OC below: the fix makes
# ppu_issue_idx $clog2(N_OC+1) bits, so 4 flops at N_OC=8 and 5 at N_OC=16.
set EXPECT  0

proc die {msg} {
    puts "\n########################################################"
    puts "# ABORT: $msg"
    puts "########################################################\n"
    exit 1
}

open_project $PROJ

puts "IMPL: STATUS=[get_property STATUS [get_runs impl_1]]  PROGRESS=[get_property PROGRESS [get_runs impl_1]]"
puts "TIMING: WNS=[get_property STATS.WNS [get_runs impl_1]] ns  WHS=[get_property STATS.WHS [get_runs impl_1]] ns"

# Derive the expected width from this build's N_OC: ceil(log2(N_OC+1)).
set noc_cfg [get_property CONFIG.N_OC [get_ips -quiet hw_pw_single_oc_axis_axi_0_0]]
if {$noc_cfg eq ""} { die "could not read CONFIG.N_OC from the PW IP" }
set EXPECT 0
set v [expr {$noc_cfg + 1}]
while {(1 << $EXPECT) < $v} { incr EXPECT }
puts "N_OC = $noc_cfg  ->  ppu_issue_idx should be \$clog2(N_OC+1) = $EXPECT bits"

open_run synth_1 -name netcheck

set cells [get_cells -hier -filter {NAME =~ "*ppu_issue_idx_reg*"}]
set n [llength $cells]
puts "\n--- ppu_issue_idx flops in netlist: $n ---"
foreach c $cells { puts "  $c" }

if {$n == 0} { die "ppu_issue_idx not found in netlist -- cannot verify" }
if {$n != $EXPECT} {
    die "ppu_issue_idx has $n flops, expected $EXPECT. The OLD (pre-fix) RTL is in the netlist."
}
puts "OK: ppu_issue_idx is $n bits -> the \$clog2(N_OC+1) fix IS in the netlist"

# (EXPECT is derived from CONFIG.N_OC above, so no hardcoded N_OC check here.)
# REMINDER: PW_N_OC in Final_code_2/src/main.c must equal this build's N_OC.
puts "REMINDER: set PW_N_OC = $noc_cfg in main.c before running on hardware."

close_design

write_hw_platform -fixed -include_bit -force -file $OUT_XSA
puts "\nwrote $OUT_XSA"
puts "DONE"
