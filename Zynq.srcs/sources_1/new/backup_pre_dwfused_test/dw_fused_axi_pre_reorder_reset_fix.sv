`timescale 1ns/1ps
// ============================================================================
// dw_fused_axi.sv — AXI4-Lite wrapper for dw_fused_axis (fused stride-1 DW)
// ============================================================================
//
// Mirrors the proven AXI-Lite FSM from pw_single_oc_axis_axi.sv. Loads per-channel
// weights/params into the fused core via a CH_ADDR index register, then streams.
//
// Register map (offset : meaning)
//   0x00 CTRL       W  bit0=start, bit1=clear_done
//   0x04 STATUS     R  bit0=done_sticky, bit1=busy
//   0x08 CIN_RUN    RW C (channels)
//   0x0C N_GROUPS   RW G = ceil(W/8), groups per row
//   0x10 ZP_RELU    RW [7:0]=zp_in [15:8]=zp_out [16]=relu_en
//   0x14 CH_ADDR    RW channel index for weight/param loads
//   0x18 W0         RW {w3,w2,w1,w0}
//   0x1C W1         RW {w7,w6,w5,w4}
//   0x20 W2         W  {w8}    -> commits 9 weights to wram[CH_ADDR]
//   0x24 BIAS       RW
//   0x28 MULT       RW
//   0x2C SHIFT      W  [7:0]   -> commits {bias,mult,shift} to pram[CH_ADDR]
//   0x30 IMG_WIDTH  RW real, unpadded row width W (samples) -- NEW 2026-07-24,
//                       needed by dw_banked_window_8x's horizontal edge
//                       masking; N_GROUPS alone (ceil(W/8)) loses the
//                       remainder. See DW_PW_FUSION_PLAN_V2.md.
//   0x34 N_ROWS     RW H (real rows) -- NEW 2026-07-24, V1 implicitly
//                       assumed H=1; real DW layers need genuine vertical
//                       windowing across H rows.
// ============================================================================

module dw_fused_axi #(
    parameter int DATA_WIDTH     = 8,
    parameter int ACC_WIDTH      = 32,
    parameter int CIN_MAX        = 240,
    parameter int IN_FIFO_DEPTH  = 2048,
    parameter int OUT_FIFO_DEPTH = 2048,
    parameter integer C_S_AXI_DATA_WIDTH = 32,
    parameter integer C_S_AXI_ADDR_WIDTH = 12
)(
    input  logic                              s_axi_aclk,
    input  logic                              s_axi_aresetn,

    // AXI4-Lite slave
    input  logic [C_S_AXI_ADDR_WIDTH-1:0]     s_axi_awaddr,
    input  logic                              s_axi_awvalid,
    output logic                              s_axi_awready,
    input  logic [C_S_AXI_DATA_WIDTH-1:0]     s_axi_wdata,
    input  logic [(C_S_AXI_DATA_WIDTH/8)-1:0] s_axi_wstrb,
    input  logic                              s_axi_wvalid,
    output logic                              s_axi_wready,
    output logic [1:0]                        s_axi_bresp,
    output logic                              s_axi_bvalid,
    input  logic                              s_axi_bready,
    input  logic [C_S_AXI_ADDR_WIDTH-1:0]     s_axi_araddr,
    input  logic                              s_axi_arvalid,
    output logic                              s_axi_arready,
    output logic [C_S_AXI_DATA_WIDTH-1:0]     s_axi_rdata,
    output logic [1:0]                        s_axi_rresp,
    output logic                              s_axi_rvalid,
    input  logic                              s_axi_rready,

    // AXI-Stream input (from MM2S DMA, group-major, 32b)
    input  logic [31:0]                       s_axis_tdata,
    input  logic                              s_axis_tvalid,
    output logic                              s_axis_tready,
    input  logic                              s_axis_tlast,

    // AXI-Stream output (to PW / dw_reorder, group-major, 32b)
    output logic [31:0]                       m_axis_tdata,
    output logic                              m_axis_tvalid,
    input  logic                              m_axis_tready,
    output logic                              m_axis_tlast
);

    localparam logic [11:0] ADDR_CTRL     = 12'h000;
    localparam logic [11:0] ADDR_STATUS   = 12'h004;
    localparam logic [11:0] ADDR_CIN_RUN  = 12'h008;
    localparam logic [11:0] ADDR_N_GROUPS = 12'h00C;
    localparam logic [11:0] ADDR_ZP_RELU  = 12'h010;
    localparam logic [11:0] ADDR_CH_ADDR  = 12'h014;
    localparam logic [11:0] ADDR_W0       = 12'h018;
    localparam logic [11:0] ADDR_W1       = 12'h01C;
    localparam logic [11:0] ADDR_W2       = 12'h020;
    localparam logic [11:0] ADDR_BIAS     = 12'h024;
    localparam logic [11:0] ADDR_MULT     = 12'h028;
    localparam logic [11:0] ADDR_SHIFT    = 12'h02C;
    localparam logic [11:0] ADDR_IMG_WIDTH = 12'h030;
    localparam logic [11:0] ADDR_N_ROWS    = 12'h034;

    wire axil_rstn = s_axi_aresetn;

    logic [C_S_AXI_ADDR_WIDTH-1:0] axi_awaddr, axi_araddr;
    logic aw_en;
    wire [11:0] wr_addr = {axi_awaddr[11:2], 2'b00};
    wire [11:0] rd_addr = {axi_araddr[11:2], 2'b00};

    // user registers
    logic [31:0] reg_cin_run, reg_n_groups, reg_zp_relu, reg_ch_addr;
    logic [31:0] reg_w0, reg_w1, reg_bias, reg_mult;
    logic [31:0] reg_img_width, reg_n_rows;

    logic busy, done_sticky;
    logic start_pulse, clear_done_pulse;

    // ---- AW channel ----
    always_ff @(posedge s_axi_aclk) begin
        if (!axil_rstn) begin
            s_axi_awready <= 1'b0; axi_awaddr <= '0; aw_en <= 1'b1;
        end else if (!s_axi_awready && s_axi_awvalid && s_axi_wvalid && aw_en) begin
            s_axi_awready <= 1'b1; axi_awaddr <= s_axi_awaddr; aw_en <= 1'b0;
        end else if (s_axi_bready && s_axi_bvalid) begin
            aw_en <= 1'b1; s_axi_awready <= 1'b0;
        end else begin
            s_axi_awready <= 1'b0;
        end
    end

    // ---- W channel ----
    always_ff @(posedge s_axi_aclk) begin
        if (!axil_rstn) s_axi_wready <= 1'b0;
        else if (!s_axi_wready && s_axi_wvalid && s_axi_awvalid && aw_en) s_axi_wready <= 1'b1;
        else s_axi_wready <= 1'b0;
    end

    wire slv_reg_wren = s_axi_wready && s_axi_wvalid && s_axi_awready && s_axi_awvalid;

    // ---- start / clear pulses ----
    always_ff @(posedge s_axi_aclk) begin
        if (!axil_rstn) begin start_pulse <= 1'b0; clear_done_pulse <= 1'b0; end
        else begin
            start_pulse <= 1'b0; clear_done_pulse <= 1'b0;
            if (slv_reg_wren && (wr_addr == ADDR_CTRL) && s_axi_wstrb[0]) begin
                if (s_axi_wdata[0] && !busy) start_pulse <= 1'b1;
                if (s_axi_wdata[1])          clear_done_pulse <= 1'b1;
            end
        end
    end

    // ---- register writes ----
    always_ff @(posedge s_axi_aclk) begin
        if (!axil_rstn) begin
            reg_cin_run <= '0; reg_n_groups <= '0; reg_zp_relu <= '0; reg_ch_addr <= '0;
            reg_w0 <= '0; reg_w1 <= '0; reg_bias <= '0; reg_mult <= '0;
            reg_img_width <= '0; reg_n_rows <= '0;
        end else if (slv_reg_wren) begin
            case (wr_addr)
                ADDR_CIN_RUN  : reg_cin_run   <= s_axi_wdata;
                ADDR_N_GROUPS : reg_n_groups  <= s_axi_wdata;
                ADDR_ZP_RELU  : reg_zp_relu   <= s_axi_wdata;
                ADDR_CH_ADDR  : reg_ch_addr   <= s_axi_wdata;
                ADDR_W0       : reg_w0        <= s_axi_wdata;
                ADDR_W1       : reg_w1        <= s_axi_wdata;
                ADDR_BIAS     : reg_bias      <= s_axi_wdata;
                ADDR_MULT     : reg_mult      <= s_axi_wdata;
                ADDR_IMG_WIDTH: reg_img_width <= s_axi_wdata;
                ADDR_N_ROWS   : reg_n_rows    <= s_axi_wdata;
                default       : ;
            endcase
        end
    end

    // ---- weight / param commit pulses ----
    logic                    w_wr_en, p_wr_en;
    logic [9*DATA_WIDTH-1:0] w_wr_data;
    logic [7:0]              p_shift;
    always_comb begin
        // assemble {w8, w7..w4, w3..w0}
        w_wr_data = {s_axi_wdata[7:0], reg_w1, reg_w0};
        w_wr_en   = slv_reg_wren && (wr_addr == ADDR_W2);
        p_wr_en   = slv_reg_wren && (wr_addr == ADDR_SHIFT);
        p_shift   = s_axi_wdata[7:0];
    end

    // ---- write response ----
    always_ff @(posedge s_axi_aclk) begin
        if (!axil_rstn) begin s_axi_bvalid <= 1'b0; s_axi_bresp <= 2'b00; end
        else if (slv_reg_wren && !s_axi_bvalid) begin s_axi_bvalid <= 1'b1; s_axi_bresp <= 2'b00; end
        else if (s_axi_bvalid && s_axi_bready) s_axi_bvalid <= 1'b0;
    end

    // ---- AR channel ----
    always_ff @(posedge s_axi_aclk) begin
        if (!axil_rstn) begin s_axi_arready <= 1'b0; axi_araddr <= '0; end
        else if (!s_axi_arready && s_axi_arvalid) begin s_axi_arready <= 1'b1; axi_araddr <= s_axi_araddr; end
        else s_axi_arready <= 1'b0;
    end

    // ---- read data ----
    always_comb begin
        s_axi_rdata = 32'd0;
        case (rd_addr)
            ADDR_STATUS  : begin s_axi_rdata[0] = done_sticky; s_axi_rdata[1] = busy; end
            ADDR_CIN_RUN  : s_axi_rdata = reg_cin_run;
            ADDR_N_GROUPS : s_axi_rdata = reg_n_groups;
            ADDR_ZP_RELU  : s_axi_rdata = reg_zp_relu;
            ADDR_CH_ADDR  : s_axi_rdata = reg_ch_addr;
            ADDR_IMG_WIDTH: s_axi_rdata = reg_img_width;
            ADDR_N_ROWS   : s_axi_rdata = reg_n_rows;
            default       : s_axi_rdata = 32'd0;
        endcase
    end

    always_ff @(posedge s_axi_aclk) begin
        if (!axil_rstn) begin s_axi_rvalid <= 1'b0; s_axi_rresp <= 2'b00; end
        else if (s_axi_arready && s_axi_arvalid && !s_axi_rvalid) begin s_axi_rvalid <= 1'b1; s_axi_rresp <= 2'b00; end
        else if (s_axi_rvalid && s_axi_rready) s_axi_rvalid <= 1'b0;
    end

    // ---- fused DW datapath ----
    logic dw_done;

    dw_fused_axis #(
        .DATA_WIDTH(DATA_WIDTH), .ACC_WIDTH(ACC_WIDTH), .CIN_MAX(CIN_MAX),
        .IN_FIFO_DEPTH(IN_FIFO_DEPTH), .OUT_FIFO_DEPTH(OUT_FIFO_DEPTH)
    ) u_dw (
        .clk(s_axi_aclk), .rst_n(s_axi_aresetn),
        .start_in(start_pulse), .done_out(dw_done),
        .cin_run(reg_cin_run[11:0]), .n_groups(reg_n_groups[11:0]),
        .img_width(reg_img_width[11:0]), .n_rows(reg_n_rows[11:0]),
        .zp_in(reg_zp_relu[7:0]), .zp_out(reg_zp_relu[15:8]), .relu_en(reg_zp_relu[16]),
        .w_wr_en(w_wr_en), .w_wr_ch(reg_ch_addr[11:0]), .w_wr_data(w_wr_data),
        .p_wr_en(p_wr_en), .p_wr_ch(reg_ch_addr[11:0]),
        .p_bias(reg_bias), .p_mult(reg_mult), .p_shift(p_shift),
        .s_axis_tdata(s_axis_tdata), .s_axis_tvalid(s_axis_tvalid),
        .s_axis_tready(s_axis_tready), .s_axis_tlast(s_axis_tlast),
        .m_axis_tdata(m_axis_tdata), .m_axis_tvalid(m_axis_tvalid),
        .m_axis_tready(m_axis_tready), .m_axis_tlast(m_axis_tlast)
    );

    // ---- busy / done sticky ----
    always_ff @(posedge s_axi_aclk) begin
        if (!axil_rstn) begin busy <= 1'b0; done_sticky <= 1'b0; end
        else if (start_pulse) begin busy <= 1'b1; done_sticky <= 1'b0; end
        else begin
            if (dw_done) begin busy <= 1'b0; done_sticky <= 1'b1; end
            if (clear_done_pulse) done_sticky <= 1'b0;
        end
    end

endmodule
`default_nettype wire
