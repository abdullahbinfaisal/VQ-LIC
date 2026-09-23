# ============================================================================
# Phase C (Zynq.srcs version): replace DW_conv_accel_0 with
# dw_fused_axi_0 -> dw_reorder_0 in Zynq's own live block design.
#
# This bakes in every fix discovered during the TCSVT run, so this should go
# straight through without the interactive back-and-forth that run needed:
#   - axi_dma_0 reconfigured to 32-bit (was 64-bit, sized for the legacy DW;
#     dw_fused_axi_0's AXIS ports are fixed 32-bit in the RTL, not a param)
#   - assign_bd_address run unconditionally after the cell churn (PW's own
#     AXI-Lite segment lost its assignment last time as a side effect of
#     deleting DW_conv_accel_0 and shuffling interconnect ports)
#   - connect_bd_net uses the correct object forms throughout (no double-
#     wrapped get_bd_nets bug)
#   - dw_reorder_0/rst_n is expected to auto-connect via ASSOCIATED_RESET
#     propagation from the clk connection (confirmed harmless last time,
#     not re-checked here -- validate_bd_design will catch it if it doesn't)
#
# NOTE: no reset_target/generate_target/upgrade_ip dance needed here, unlike
# the TCSVT run -- this is the FIRST time dw_fused_axi_0 is being added to
# THIS block design, not a re-package of an already-instantiated stale IP.
#
# PW (axi_dma_1 / pw_single_oc_axis_axi_0) is untouched, same as before --
# this still only wires the DW-only loop-back-to-DRAM test path.
# ============================================================================

# ---- 0. Repo path + catalog refresh ----
set ip_repo {C:/Users/Fahad/ip_repo}
set cur_repos [get_property ip_repo_paths [current_project]]
if {[lsearch -exact $cur_repos $ip_repo] == -1} {
    set_property ip_repo_paths [concat $cur_repos $ip_repo] [current_project]
}
update_ip_catalog -rebuild

set found_fused   [get_ipdefs -filter {NAME == dw_fused_axi}]
set found_reorder [get_ipdefs -filter {NAME == dw_reorder}]
if {$found_fused eq "" || $found_reorder eq ""} {
    error "dw_fused_axi or dw_reorder not found in IP catalog after update_ip_catalog -- re-check Phase A/B output before continuing."
}
puts "Found in catalog: $found_fused / $found_reorder"

# ---- 1. Open the active design ----
open_bd_design {C:/Users/Fahad/Zynq/Zynq.srcs/sources_1/bd/hw/hw.bd}
set bd [current_bd_design]
puts "Editing block design: [get_property NAME $bd]"

# ---- 2. Remove the legacy DW core ----
if {[llength [get_bd_cells -quiet DW_conv_accel_0]]} {
    delete_bd_objs [get_bd_cells DW_conv_accel_0]
    puts "Removed DW_conv_accel_0"
} else {
    puts "DW_conv_accel_0 not found -- already removed? continuing."
}

# ---- 3. Add the new cells ----
create_bd_cell -type ip -vlnv xilinx.com:user:dw_fused_axi:1.0 dw_fused_axi_0
create_bd_cell -type ip -vlnv xilinx.com:user:dw_reorder:1.0  dw_reorder_0
set_property CONFIG.DATA_W {32} [get_bd_cells dw_reorder_0]

# ---- 4. Wire the AXIS data path in DW_conv_accel_0's old place ----
connect_bd_intf_net [get_bd_intf_pins axi_dma_0/M_AXIS_MM2S] \
                     [get_bd_intf_pins dw_fused_axi_0/s_axis]
connect_bd_intf_net [get_bd_intf_pins dw_fused_axi_0/m_axis] \
                     [get_bd_intf_pins dw_reorder_0/s_axis]
connect_bd_intf_net [get_bd_intf_pins dw_reorder_0/m_axis] \
                     [get_bd_intf_pins axi_dma_0/S_AXIS_S2MM]

# ---- 5. AXI-Lite control path ----
apply_bd_automation -rule xilinx.com:bd_rule:axi4 \
    -config { Master {/processing_system7_0/M_AXI_GP0} Clk {Auto} } \
    [get_bd_intf_pins dw_fused_axi_0/S_AXI]

# ---- 6. Clock/reset for dw_reorder_0 ----
connect_bd_net [get_bd_pins dw_reorder_0/clk] [get_bd_pins dw_fused_axi_0/s_axi_aclk]
set rst_net [get_bd_nets -quiet -filter {NAME =~ "*rst_ps7_0_50M*peripheral_aresetn*"}]
if {[llength $rst_net] && ![llength [get_bd_nets -quiet -of_objects [get_bd_pins dw_reorder_0/rst_n]]]} {
    connect_bd_net [get_bd_pins dw_reorder_0/rst_n] $rst_net
} else {
    puts "dw_reorder_0/rst_n already connected (or reset net not found by name -- check manually if validate_bd_design complains)."
}

# ---- 7. Known-needed fixes, applied up front this time ----
assign_bd_address

set_property CONFIG.c_m_axis_mm2s_tdata_width {32} [get_bd_cells axi_dma_0]
set_property CONFIG.c_s_axis_s2mm_tdata_width {32} [get_bd_cells axi_dma_0]

# ---- 8. Validate + save ----
regenerate_bd_layout
validate_bd_design
save_bd_design

puts "\nPhase C done. If validate_bd_design reported errors, STOP and paste"
puts "the error text back rather than pushing to synthesis. If clean:"
puts "Generate Block Design, confirm dw_fused_axi_0/S_AXI's assigned base"
puts "address in the Address Editor (expect it reused DW_BASE_ADDR's old"
puts "0x43C00000 slot, same as it did in TCSVT, but confirm), then Generate"
puts "Bitstream."
