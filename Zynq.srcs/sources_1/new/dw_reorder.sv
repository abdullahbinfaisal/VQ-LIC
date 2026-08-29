`timescale 1ns/1ps
// ============================================================================
// dw_reorder.sv — Fused DW->PW AXIS glue (group-major passthrough FIFO)
// ============================================================================
//
// In the fused (Plan B) pipeline the DW already emits data in PW's input order
// (group-major / channel-minor: for g: for c: 8 samples). So NO transpose is
// required here — this block is just a synchronous AXIS FIFO that decouples the
// DW master and PW slave rates and carries TLAST through.
//
//   (The channel-major -> group-major transpose lives in dw_pw_reorder.sv and is
//    only needed for the NON-fused / channel-stationary DW. It is unused here.)
//
// DATA_W defaults to 64 (8 INT8 samples per beat). Set PW's S_AXIS_DATA_WIDTH to
// match for a direct on-chip link.
//
// RESET (added 2026-07-25, chasing a real-hardware S2MM timeout): this FIFO
// used to only reset on the global system reset (rst_n), never between
// individual DW runs. If a run aborts partway (e.g. an earlier timeout), this
// FIFO can be left non-empty/stuck, silently corrupting or blocking the NEXT
// run's data -- including its TLAST beat, which Xilinx's AXI DMA S2MM channel
// (Direct Register mode) requires to ever consider a transfer complete. Now
// takes start_in and stretches its own internal reset for a few cycles every
// run, mirroring dw_fused_axis.sv's fifo_rst pattern exactly.
// ============================================================================

module dw_reorder #(
    parameter int DATA_W = 64,
    parameter int DEPTH  = 32        // small decoupling FIFO (PW has its own deep FIFO)
)(
    input  logic                 clk,
    input  logic                 rst_n,
    input  logic                 start_in,     // 1-cycle pulse, once per run

    // AXIS slave (from fused DW master)
    input  logic [DATA_W-1:0]    s_axis_tdata,
    input  logic                 s_axis_tvalid,
    output logic                 s_axis_tready,
    input  logic                 s_axis_tlast,

    // AXIS master (to PW slave)
    output logic [DATA_W-1:0]    m_axis_tdata,
    output logic                 m_axis_tvalid,
    input  logic                 m_axis_tready,
    output logic                 m_axis_tlast
);

    localparam int FW = DATA_W + 1;  // {tlast, tdata}

    // Per-run reset stretch (mirrors dw_fused_axis.sv's fifo_rst exactly) --
    // clears any data left over from a prior aborted run before this run's
    // first beat can be written.
    logic [2:0] frst_cnt;
    logic       fifo_rst;
    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n)        frst_cnt <= 3'd5;
        else if (start_in) frst_cnt <= 3'd5;
        else if (frst_cnt != 0) frst_cnt <= frst_cnt - 3'd1;
    end
    assign fifo_rst = !rst_n || (frst_cnt != 0);

    logic        full, empty;
    logic        wr_en, rd_en;
    logic [FW-1:0] din, dout;

    assign wr_en        = s_axis_tvalid && s_axis_tready;
    assign s_axis_tready = !full && !fifo_rst;
    assign din          = {s_axis_tlast, s_axis_tdata};

    assign rd_en        = m_axis_tvalid && m_axis_tready;
    assign m_axis_tvalid = !empty;
    assign m_axis_tdata  = dout[DATA_W-1:0];
    assign m_axis_tlast  = dout[DATA_W];

    xpm_fifo_sync #(
        .DOUT_RESET_VALUE   ("0"),
        .ECC_MODE           ("no_ecc"),
        .FIFO_MEMORY_TYPE   ("auto"),
        .FIFO_READ_LATENCY  (0),
        .FIFO_WRITE_DEPTH   (DEPTH),
        .FULL_RESET_VALUE   (1),
        .PROG_EMPTY_THRESH  (5),
        .PROG_FULL_THRESH   (5),
        .RD_DATA_COUNT_WIDTH(1),
        .READ_DATA_WIDTH    (FW),
        .READ_MODE          ("fwft"),
        .SIM_ASSERT_CHK     (0),
        .USE_ADV_FEATURES   ("0000"),
        .WAKEUP_TIME        (0),
        .WRITE_DATA_WIDTH   (FW),
        .WR_DATA_COUNT_WIDTH(1)
    ) u_fifo (
        .rst        (fifo_rst),
        .wr_clk     (clk),
        .wr_en      (wr_en),
        .din        (din),
        .full       (full),
        .prog_full  (),
        .wr_data_count(),
        .rd_en      (rd_en),
        .dout       (dout),
        .empty      (empty),
        .prog_empty (),
        .rd_data_count(),
        .data_valid (),
        .overflow   (),
        .underflow  (),
        .almost_full(),
        .almost_empty(),
        .wr_ack     (),
        .sleep      (1'b0),
        .injectsbiterr(1'b0),
        .injectdbiterr(1'b0),
        .sbiterr    (),
        .dbiterr    ()
    );

endmodule
`default_nettype wire
