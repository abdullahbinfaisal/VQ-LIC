# dw_fused_timing.xdc
#
# zp_in (packed into dw_fused_axi_0's reg_zp_relu) is a quasi-static config
# register: written once via AXI-Lite before start_in is pulsed for a layer
# run, then held stable for the entire run (thousands of clock cycles). It
# fans out combinationally into conv_mac_array's internal MAC pipeline
# registers -- the real timing requirement is "settled before start_in",
# not "within one clock cycle", so single-cycle STA is needlessly strict
# here.
#
# Added 2026-07-25 to close a residual setup violation left after
# implementation with aggressive place/route directives: WNS -0.021ns,
# 4 failing endpoints, all sourced from reg_zp_relu_reg bits. See
# DW_PW_FUSION_PLAN_V2.md for the fusion project's full history.
set_multicycle_path -setup -from [get_cells {hw_i/dw_fused_axi_0/inst/reg_zp_relu_reg[*]}] 4
set_multicycle_path -hold  -from [get_cells {hw_i/dw_fused_axi_0/inst/reg_zp_relu_reg[*]}] 3
