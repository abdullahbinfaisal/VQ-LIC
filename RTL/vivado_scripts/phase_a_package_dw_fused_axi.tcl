# ============================================================================
# Phase A (Zynq.srcs version): package dw_fused_axi as a reusable IP,
# sourced from Zynq.srcs's OWN local ppu.sv/conv_mac_array.sv.
#
# This is the key difference from the TCSVT run: packaging against Zynq's
# own copies avoids re-introducing the exact bug we spent an hour chasing
# there -- TCSVT's local ppu.sv had drifted (use_dsp="no" vs "yes") from
# what PW's already-packaged IP was built with, causing a duplicate-module-
# with-different-contents conflict during top-level synthesis that likely
# caused the elaboration hang. Zynq.srcs has only ONE copy of ppu.sv on
# disk, and it's the one PW's existing packaged IP was originally sourced
# from, so this can't happen here.
#
# Run from the Vivado Tcl console with the Zynq project (C:\Users\Fahad\
# Zynq\Zynq.xpr) open. Overwrites the existing dw_fused_axi_1.0 package in
# the shared ip_repo (previously packaged from TCSVT's drifted sources) with
# a fresh one sourced from Zynq.srcs.
# ============================================================================

set src_dir   {C:/Users/Fahad/Zynq/Zynq.srcs/sources_1/new}
set ip_repo   {C:/Users/Fahad/ip_repo}
set pkg_dir   [file join $ip_repo dw_fused_axi_1.0]
set tmp_proj  {C:/Users/Fahad/ip_repo/_pkg_tmp_dw_fused_axi}

file delete -force $tmp_proj
file delete -force $pkg_dir

create_project -force pkg_dw_fused_axi_tmp $tmp_proj -part xc7z020clg484-1

add_files -norecurse [list \
    [file join $src_dir conv_mac_array.sv] \
    [file join $src_dir ppu.sv] \
    [file join $src_dir dw_banked_window_8x.sv] \
    [file join $src_dir dw_fused_core.sv] \
    [file join $src_dir dw_fused_axis.sv] \
    [file join $src_dir dw_fused_axi.sv] \
]
# dw_seq_window_8x.sv (1x3-only, wrong model -- see DW_PW_FUSION_PLAN_V2.md)
# deliberately dropped from the package 2026-07-24: dw_fused_core.sv no
# longer instantiates it. The file itself is left in sources_1/new,
# unreferenced, not deleted.
update_compile_order -fileset sources_1
set_property top dw_fused_axi [current_fileset]
update_compile_order -fileset sources_1

ipx::package_project -root_dir $pkg_dir -vendor xilinx.com -library user \
    -taxonomy /UserIP -import_files -set_current true -force

set core [ipx::current_core]
set_property VENDOR_DISPLAY_NAME {Fahad}    $core
set_property NAME dw_fused_axi              $core
set_property VERSION 1.0                    $core
set_property DISPLAY_NAME {Fused DW (stride-1) AXI4 wrapper} $core
set_property DESCRIPTION {Group-major streaming depthwise conv fused front-end for on-chip DW->PW hand-off} $core

puts "\n==== dw_fused_axi interface check ===="
foreach bi [ipx::get_bus_interfaces -of_objects $core] {
    puts "  [get_property NAME $bi]  (abstraction: [get_property ABSTRACTION_TYPE_NAME $bi])"
}
puts "Expect one AXI4LITE (s_axi) and two AXI4STREAM (s_axis, m_axis)."
puts "======================================\n"

ipx::create_xgui_files $core
ipx::update_checksums  $core
ipx::save_core         $core

close_project
puts "Phase A done: dw_fused_axi re-packaged from Zynq.srcs sources at $pkg_dir"
