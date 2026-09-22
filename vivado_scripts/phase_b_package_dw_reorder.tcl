# ============================================================================
# Phase B (Zynq.srcs version): package dw_reorder as its own tiny IP,
# sourced from Zynq.srcs. Kept separate from dw_fused_axi on purpose (see
# chat) so that boundary stays independently ILA-probeable.
# ============================================================================

set src_dir   {C:/Users/Fahad/Zynq/Zynq.srcs/sources_1/new}
set ip_repo   {C:/Users/Fahad/ip_repo}
set pkg_dir   [file join $ip_repo dw_reorder_1.0]
set tmp_proj  {C:/Users/Fahad/ip_repo/_pkg_tmp_dw_reorder}

file delete -force $tmp_proj
file delete -force $pkg_dir

create_project -force pkg_dw_reorder_tmp $tmp_proj -part xc7z020clg484-1

add_files -norecurse [list [file join $src_dir dw_reorder.sv]]
update_compile_order -fileset sources_1
set_property top dw_reorder [current_fileset]
update_compile_order -fileset sources_1

ipx::package_project -root_dir $pkg_dir -vendor xilinx.com -library user \
    -taxonomy /UserIP -import_files -set_current true -force

set core [ipx::current_core]
set_property VENDOR_DISPLAY_NAME {Fahad}    $core
set_property NAME dw_reorder                $core
set_property VERSION 1.0                    $core
set_property DISPLAY_NAME {DW->PW decoupling FIFO} $core
set_property DESCRIPTION {Passthrough AXIS FIFO decoupling fused-DW and PW rates (no transpose needed -- both already group-major)} $core

puts "\n==== dw_reorder interface check ===="
foreach bi [ipx::get_bus_interfaces -of_objects $core] {
    puts "  [get_property NAME $bi]  (abstraction: [get_property ABSTRACTION_TYPE_NAME $bi])"
}
puts "Expect exactly two AXI4STREAM interfaces (S_AXIS, M_AXIS) -- no AXI4LITE."
puts "=====================================\n"

ipx::create_xgui_files $core
ipx::update_checksums  $core
ipx::save_core         $core

close_project
puts "Phase B done: dw_reorder re-packaged from Zynq.srcs sources at $pkg_dir"
