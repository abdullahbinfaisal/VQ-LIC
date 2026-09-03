`timescale 1ns/1ps
// Elaboration wrapper: instantiates the PW IP with the VQ mode compiled IN,
// so both build configurations are checked, not just the default.
module vq_build_wrapper (input logic clk, input logic rstn);
  pw_single_oc_axis_axi #(
    .DATA_WIDTH(8), .ACC_WIDTH(24), .CIN_MAX(240), .COUT_MAX(240),
    .TILE_PIXELS_MAX(32768), .IN_FIFO_DEPTH(2048), .OUT_FIFO_DEPTH(8192),
    .S_AXIS_DATA_WIDTH(64), .N_LANES(8), .N_OC(32), .M_AXIS_DATA_WIDTH(64),
    .USE_PW_VQ(1)
  ) u (
    .s_axi_aclk(clk), .s_axi_aresetn(rstn),
    .s_axi_awaddr('0), .s_axi_awvalid(1'b0), .s_axi_awready(),
    .s_axi_wdata('0), .s_axi_wstrb('0), .s_axi_wvalid(1'b0), .s_axi_wready(),
    .s_axi_bresp(), .s_axi_bvalid(), .s_axi_bready(1'b0),
    .s_axi_araddr('0), .s_axi_arvalid(1'b0), .s_axi_arready(),
    .s_axi_rdata(), .s_axi_rresp(), .s_axi_rvalid(), .s_axi_rready(1'b0),
    .s_axis_tdata('0), .s_axis_tvalid(1'b0), .s_axis_tready(), .s_axis_tlast(1'b0),
    .m_axis_tdata(), .m_axis_tvalid(), .m_axis_tready(1'b0), .m_axis_tlast()
  );
endmodule
