`timescale 1ns/1ps
// ============================================================================
// dw_fused_axis.sv — AXIS shell around dw_fused_core (fused stride-1 DW)
// ============================================================================
//
//   32b AXIS in (group-major)            32b AXIS out (group-major) -> PW
//        │  in FIFO 32->64                     ▲  out FIFO 64->32
//        ▼                                     │
//   dw_fused_core (64b in/out) ──────────── pixel_out(64) + valid
//
// The DW input arrives group-major as 32-bit DMA beats and is reassembled to
// 64-bit (8 samples) by the input FIFO. The core's 64-bit group-major output is
// serialized back to 32-bit so PW's existing 32-bit slave needs no change.
//
// Config and weight/param RAM loads come in on direct ports (an AXI-Lite wrapper,
// dw_fused_axi.sv, drives them — mirrors pw_single_oc_axis / *_axi split).
//
// TLAST: total 32-bit output beats = n_groups * cin_run * 2.
// ============================================================================

module dw_fused_axis #(
    parameter int DATA_WIDTH     = 8,
    parameter int ACC_WIDTH      = 32,
    parameter int CIN_MAX        = 240,
    parameter int IN_FIFO_DEPTH  = 2048,
    parameter int OUT_FIFO_DEPTH = 2048
)(
    input  logic                    clk,
    input  logic                    rst_n,

    // ---- control / config (stable before start_in) ----
    input  logic                    start_in,
    output logic                    done_out,
    input  logic [11:0]             cin_run,
    input  logic [11:0]             n_groups,
    input  logic [7:0]              zp_in,
    input  logic [7:0]              zp_out,
    input  logic                    relu_en,

    // ---- weight / param RAM load ----
    input  logic                    w_wr_en,
    input  logic [11:0]             w_wr_ch,
    input  logic [9*DATA_WIDTH-1:0] w_wr_data,
    input  logic                    p_wr_en,
    input  logic [11:0]             p_wr_ch,
    input  logic signed [31:0]      p_bias,
    input  logic [31:0]             p_mult,
    input  logic [7:0]              p_shift,

    // ---- AXIS slave (DW input from MM2S DMA, group-major, 32b) ----
    input  logic [31:0]             s_axis_tdata,
    input  logic                    s_axis_tvalid,
    output logic                    s_axis_tready,
    input  logic                    s_axis_tlast,   // unused

    // ---- AXIS master (to PW / dw_reorder, group-major, 32b) ----
    output logic [31:0]             m_axis_tdata,
    output logic                    m_axis_tvalid,
    input  logic                    m_axis_tready,
    output logic                    m_axis_tlast
);

    localparam int CORE_W = 8*DATA_WIDTH;   // 64

    logic _unused_tlast;
    always_comb _unused_tlast = s_axis_tlast;

    // FIFO reset stretch on start
    logic [2:0] frst_cnt;
    logic       fifo_rst;
    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n)        frst_cnt <= 3'd5;
        else if (start_in) frst_cnt <= 3'd5;
        else if (frst_cnt != 0) frst_cnt <= frst_cnt - 3'd1;
    end
    assign fifo_rst = !rst_n || (frst_cnt != 0);

    // ------------------------------------------------------------
    // Input FIFO: 32b write -> 64b read (reassemble group)
    // ------------------------------------------------------------
    logic              in_full, in_empty;
    logic [CORE_W-1:0] in_dout;
    logic              in_wr_en, in_rd_en;

    logic              core_valid_in, core_consume_in;
    logic [CORE_W-1:0] core_pixel_in;

    assign in_wr_en      = s_axis_tvalid && s_axis_tready;
    assign s_axis_tready = (!in_full) && (!fifo_rst);
    assign core_valid_in = !in_empty;
    assign core_pixel_in = in_dout;
    assign in_rd_en      = core_consume_in && core_valid_in;

    xpm_fifo_sync #(
        .DOUT_RESET_VALUE("0"), .ECC_MODE("no_ecc"), .FIFO_MEMORY_TYPE("block"),
        .FIFO_READ_LATENCY(0), .FIFO_WRITE_DEPTH(IN_FIFO_DEPTH), .FULL_RESET_VALUE(1),
        .PROG_EMPTY_THRESH(10), .PROG_FULL_THRESH(10),
        .RD_DATA_COUNT_WIDTH(1), .READ_DATA_WIDTH(CORE_W),
        .READ_MODE("fwft"), .SIM_ASSERT_CHK(0), .USE_ADV_FEATURES("0000"),
        .WAKEUP_TIME(0), .WRITE_DATA_WIDTH(32), .WR_DATA_COUNT_WIDTH(1)
    ) u_in_fifo (
        .rst(fifo_rst), .wr_clk(clk), .wr_en(in_wr_en), .din(s_axis_tdata),
        .full(in_full), .prog_full(), .wr_data_count(),
        .rd_en(in_rd_en), .dout(in_dout), .empty(in_empty),
        .prog_empty(), .rd_data_count(), .data_valid(), .overflow(), .underflow(),
        .almost_full(), .almost_empty(), .wr_ack(),
        .sleep(1'b0), .injectsbiterr(1'b0), .injectdbiterr(1'b0), .sbiterr(), .dbiterr()
    );

    // ------------------------------------------------------------
    // Core
    // ------------------------------------------------------------
    logic [CORE_W-1:0] core_pixel_out;
    logic              core_valid_out;
    logic              core_done;

    dw_fused_core #(.DATA_WIDTH(DATA_WIDTH), .ACC_WIDTH(ACC_WIDTH), .CIN_MAX(CIN_MAX)) u_core (
        .clk(clk), .rst_n(rst_n), .start_in(start_in), .done_out(core_done),
        .cin_run(cin_run), .n_groups(n_groups), .zp_in(zp_in), .zp_out(zp_out), .relu_en(relu_en),
        .pixel_in(core_pixel_in), .valid_in(core_valid_in), .consume_in(core_consume_in),
        .pixel_out(core_pixel_out), .valid_out(core_valid_out),
        .w_wr_en(w_wr_en), .w_wr_ch(w_wr_ch), .w_wr_data(w_wr_data),
        .p_wr_en(p_wr_en), .p_wr_ch(p_wr_ch), .p_bias(p_bias), .p_mult(p_mult), .p_shift(p_shift)
    );

    // ------------------------------------------------------------
    // Output FIFO: 64b write -> 32b read (serialize group into 2 beats)
    // ------------------------------------------------------------
    logic        out_full, out_empty;
    logic [31:0] out_dout;
    logic        out_wr_en, out_rd_en;

    assign out_wr_en     = core_valid_out && !out_full;
    assign out_rd_en     = m_axis_tvalid && m_axis_tready;
    assign m_axis_tvalid = !out_empty;
    assign m_axis_tdata  = out_dout;

    xpm_fifo_sync #(
        .DOUT_RESET_VALUE("0"), .ECC_MODE("no_ecc"), .FIFO_MEMORY_TYPE("block"),
        .FIFO_READ_LATENCY(0), .FIFO_WRITE_DEPTH(OUT_FIFO_DEPTH), .FULL_RESET_VALUE(1),
        .PROG_EMPTY_THRESH(10), .PROG_FULL_THRESH(10),
        .RD_DATA_COUNT_WIDTH(1), .READ_DATA_WIDTH(32),
        .READ_MODE("fwft"), .SIM_ASSERT_CHK(0), .USE_ADV_FEATURES("0000"),
        .WAKEUP_TIME(0), .WRITE_DATA_WIDTH(CORE_W), .WR_DATA_COUNT_WIDTH(1)
    ) u_out_fifo (
        .rst(fifo_rst), .wr_clk(clk), .wr_en(out_wr_en), .din(core_pixel_out),
        .full(out_full), .prog_full(), .wr_data_count(),
        .rd_en(out_rd_en), .dout(out_dout), .empty(out_empty),
        .prog_empty(), .rd_data_count(), .data_valid(), .overflow(), .underflow(),
        .almost_full(), .almost_empty(), .wr_ack(),
        .sleep(1'b0), .injectsbiterr(1'b0), .injectdbiterr(1'b0), .sbiterr(), .dbiterr()
    );

    // ------------------------------------------------------------
    // TLAST / done: total 32b output beats = n_groups * cin_run * 2
    // ------------------------------------------------------------
    logic [23:0] total_words_r;   // n_groups * cin_run (registered multiply)
    logic        mult_v;
    always_ff @(posedge clk) begin
        if (!rst_n) begin total_words_r <= 24'd0; mult_v <= 1'b0; end
        else if (start_in) begin
            total_words_r <= n_groups * cin_run;   // 12x12 -> 24b
            mult_v <= 1'b1;
        end
    end
    wire [24:0] total_beats = {total_words_r, 1'b0};   // *2 (two 32b beats / group)

    logic [24:0] produced_cnt;
    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n)        produced_cnt <= 25'd0;
        else if (start_in) produced_cnt <= 25'd0;
        else if (out_rd_en) produced_cnt <= produced_cnt + 25'd1;
    end
    assign m_axis_tlast = m_axis_tvalid && mult_v && (total_beats != 0) &&
                          (produced_cnt == total_beats - 25'd1);

    assign done_out = m_axis_tvalid && m_axis_tready && m_axis_tlast;

endmodule
`default_nettype wire
