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
// TLAST: total 32-bit output beats = n_groups * cin_run * n_rows * 2. The
// n_rows factor is new (2026-07-24, dw_banked_window_8x real-3x3 swap, see
// DW_PW_FUSION_PLAN_V2.md) -- V1 implicitly assumed n_rows=1.
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
    input  logic [11:0]             img_width,
    input  logic [11:0]             n_rows,
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
    logic              out_prog_full;   // defined below, forward-used here

    assign in_wr_en      = s_axis_tvalid && s_axis_tready;
    assign s_axis_tready = (!in_full) && (!fifo_rst);
    // Throttle input consumption when the output FIFO is nearing full --
    // mirrors dw_plane_run_axis.sv's proven throttle_in. Without this,
    // dw_fused_core has no way to pause (no ready/backpressure input at
    // all): if out_full, core_valid_out beats are silently dropped rather
    // than stalling, since out_wr_en = core_valid_out && !out_full. Never
    // exercised by the xsim testbench (48 total emissions, nowhere near
    // the 2048-deep FIFOs) -- only shows up at real scale (L0 alone needs
    // 1,105,920 group-emissions).
    assign core_valid_in = !in_empty && !out_prog_full;
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
        .cin_run(cin_run), .n_groups(n_groups), .img_width(img_width), .n_rows(n_rows),
        .zp_in(zp_in), .zp_out(zp_out), .relu_en(relu_en),
        .pixel_in(core_pixel_in), .valid_in(core_valid_in), .consume_in(core_consume_in),
        .pixel_out(core_pixel_out), .valid_out(core_valid_out),
        .w_wr_en(w_wr_en), .w_wr_ch(w_wr_ch), .w_wr_data(w_wr_data),
        .p_wr_en(p_wr_en), .p_wr_ch(p_wr_ch), .p_bias(p_bias), .p_mult(p_mult), .p_shift(p_shift)
    );

    // ------------------------------------------------------------
    // Output FIFO: 64b write -> 32b read (serialize group into 2 beats)
    // ------------------------------------------------------------
    logic        out_full, fifo_empty;
    logic [31:0] fifo_dout;
    logic        out_wr_en, fifo_rd_en;

    assign out_wr_en = core_valid_out && !out_full;
    // m_axis / read side is driven by the output holding register below, not
    // straight off the FIFO -- see the TLAST section.

    // PROG_FULL_THRESH must clear dw_fused_core's own pipeline depth
    // (windower ~4 stages + conv_mac_array 9 + ppu 8 ~= 21 cycles): once
    // throttled, that many beats are already committed and will still
    // land. 32 gives comfortable margin (mirrors dw_plane_run_axis's
    // OUT_MARGIN=32 for the same reason).
    xpm_fifo_sync #(
        .DOUT_RESET_VALUE("0"), .ECC_MODE("no_ecc"), .FIFO_MEMORY_TYPE("block"),
        .FIFO_READ_LATENCY(0), .FIFO_WRITE_DEPTH(OUT_FIFO_DEPTH), .FULL_RESET_VALUE(1),
        .PROG_EMPTY_THRESH(10), .PROG_FULL_THRESH(32),
        .RD_DATA_COUNT_WIDTH(1), .READ_DATA_WIDTH(32),
        .READ_MODE("fwft"), .SIM_ASSERT_CHK(0), .USE_ADV_FEATURES("0000"),
        .WAKEUP_TIME(0), .WRITE_DATA_WIDTH(CORE_W), .WR_DATA_COUNT_WIDTH(1)
    ) u_out_fifo (
        .rst(fifo_rst), .wr_clk(clk), .wr_en(out_wr_en), .din(core_pixel_out),
        .full(out_full), .prog_full(out_prog_full), .wr_data_count(),
        .rd_en(fifo_rd_en), .dout(fifo_dout), .empty(fifo_empty),
        .prog_empty(), .rd_data_count(), .data_valid(), .overflow(), .underflow(),
        .almost_full(), .almost_empty(), .wr_ack(),
        .sleep(1'b0), .injectsbiterr(1'b0), .injectdbiterr(1'b0), .sbiterr(), .dbiterr()
    );

    // ------------------------------------------------------------
    // TLAST / done -- OBSERVED, not predicted (reworked 2026-07-25).
    //
    // The previous scheme predicted the total 32b beat count geometrically
    // (n_groups*cin_run*n_rows*2) and asserted TLAST when a read-side counter
    // hit it. That is fragile: dw_banked_window_8x's real emission count (one-
    // group emission delay, per-row horizontal-flush slot, extra vertical-flush
    // row, lane-0-gated writes, per-lane edge masking) does not cleanly equal
    // that product, and if the core emits even ONE fewer beat than predicted,
    // TLAST never asserts and the S2MM DMA (Direct Register mode, needs TLAST
    // to complete) hangs forever. The proven dw_plane_run_axis.sv never had
    // this failure because it tagged the last beat from what the datapath
    // ACTUALLY produced (a FIFO data bit read out with its beat), not a formula.
    //
    // Here we get the same robustness without a per-beat FIFO tag: a 1-deep
    // holding register sits between the out FIFO and m_axis. The true last beat
    // of the run is, by construction, the one still in the holding register
    // when (a) the core has finished writing -- all_written, which trails
    // core_done, itself >=18 cycles behind the final emitted beat -- AND (b)
    // nothing remains behind it in the FIFO (fifo_empty). No count is ever
    // computed or compared, so no windower emission-count subtlety can break it.
    //
    // Throughput: the holding register only stalls m_axis when the FIFO is
    // momentarily empty AND the core hasn't finished (we can't yet tell whether
    // the held beat is the last one). That is rare in practice -- the FIFO is
    // fed 2 read-beats per 64b group but drained at most 1 beat/cycle, so it
    // fills rather than empties mid-run. The stall matters only at the true end.
    // ------------------------------------------------------------

    // Writing is finished once the core's done pulse is seen. core_done
    // (= win_done delayed by the full 1+9+8 datapath latency) is strictly
    // after the last core_valid_out, hence after the last FIFO write.
    logic all_written;
    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n)         all_written <= 1'b0;
        else if (start_in)  all_written <= 1'b0;
        else if (core_done) all_written <= 1'b1;
    end

    // 1-deep output holding register.
    logic        hold_valid;
    logic [31:0] hold_data;

    // Present the held beat if more data sits behind it (more coming) OR the
    // core has finished (nothing more can come -> this held beat is the last).
    wire hold_present = hold_valid && (!fifo_empty || all_written);
    wire hold_is_last = hold_valid && all_written && fifo_empty;

    assign m_axis_tvalid = hold_present;
    assign m_axis_tdata  = hold_data;
    assign m_axis_tlast  = hold_is_last;

    wire m_axis_fire = m_axis_tvalid && m_axis_tready;

    // Pop the FIFO to (re)fill the holding register whenever the FIFO has data
    // and the register is empty or being consumed this cycle.
    assign fifo_rd_en = !fifo_empty && (!hold_valid || m_axis_fire);

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            hold_valid <= 1'b0;
            hold_data  <= 32'd0;
        end else if (start_in) begin
            hold_valid <= 1'b0;
            hold_data  <= 32'd0;
        end else begin
            if (fifo_rd_en) begin
                hold_valid <= 1'b1;
                hold_data  <= fifo_dout;
            end else if (m_axis_fire) begin
                hold_valid <= 1'b0;
            end
        end
    end

    assign done_out = m_axis_fire && m_axis_tlast;

endmodule
`default_nettype wire
