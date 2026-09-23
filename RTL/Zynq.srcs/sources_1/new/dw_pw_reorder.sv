`timescale 1ns/1ps
// ============================================================================
// dw_pw_reorder.sv — Band-Reorder Buffer (DW band-stationary → PW pixel-group)
// ============================================================================
//
// PURPOSE:
//   Sits between a band-stationary DW engine and the PW engine. DW emits one
//   output row at a time across all Cin channels (channel-major within each
//   row). PW expects data in pixel-group-major, channel-minor order. This
//   module performs the on-chip transpose using double-buffered BRAM.
//
// DATA FLOW:
//   DW writes: for each row:
//     for ch in 0..Cin-1:
//       for beat in 0..wpr-1:                  // wpr = W_out_padded / 8
//         → 8 pixels (64 bits)
//
//   PW reads:  for each row:
//     for group in 0..wpr-1:                   // one pixel-group = 8 cols
//       for ch in 0..Cin-1:
//         ← 8 pixels (64 bits)
//
//   Write order into BRAM:  addr = bank_base + ch*wpr + beat   (linear)
//   Read  order from BRAM:  addr = bank_base + ch*wpr + group  (transposed)
//
// DOUBLE BUFFERING:
//   Two banks (each BUFFER_DEPTH entries of DW_BEAT_W bits).
//   DW writes row N into one bank while PW reads row N-1 from the other.
//
// RESOURCE ESTIMATE (Zynq-7020, worst-case BUFFER_DEPTH=1680):
//   BRAM:  2 × 1680 × 64 bits = 215 Kb → ~6 BRAM36K
//   DSP:   1 (for beats_per_row multiply)
//   LUT:   ~200-400 (FSM + address generators)
//
// THROUGHPUT:
//   1 beat/cycle (pipelined) on both write and read sides.
//   Zero throughput overhead — matches DW output rate exactly.
//   Latency: 1 output row (initial fill before first read can begin).
//
// PARAMETERS:
//   DATA_WIDTH    — pixel width in bits (default 8 for INT8)
//   CIN_MAX       — maximum input channels (240 for MobileNet)
//   BUFFER_DEPTH  — max(cin_run × ceil(W_out_padded/8)) across all layers
//                   Default 1680 covers all MobileNet-v1 layers
//   DW_BEAT_W     — DW output beat width (8 × DATA_WIDTH = 64 bits)
//
// ============================================================================

module dw_pw_reorder #(
    parameter int DATA_WIDTH   = 8,
    parameter int CIN_MAX      = 240,
    parameter int BUFFER_DEPTH = 1680,
    parameter int DW_BEAT_W    = 8 * DATA_WIDTH   // 64 bits: 8 pixels per beat
)(
    input  logic                  clk,
    input  logic                  rst_n,

    // ---- Control ----
    input  logic                  start_in,       // 1-cycle pulse: begin new tile
    output logic                  done_out,       // 1-cycle pulse: tile complete

    // ---- Configuration (must be stable before start_in) ----
    input  logic [11:0]           cin_run,        // number of input channels
    input  logic [11:0]           w_out_beats,    // output width in 8-pixel beats (= W_out_padded / 8)
    input  logic [11:0]           n_rows,         // number of output rows to process

    // ---- DW-side AXI-Stream slave ----
    input  logic [DW_BEAT_W-1:0]  s_axis_tdata,
    input  logic                  s_axis_tvalid,
    output logic                  s_axis_tready,

    // ---- PW-side AXI-Stream master ----
    output logic [DW_BEAT_W-1:0]  m_axis_tdata,
    output logic                  m_axis_tvalid,
    input  logic                  m_axis_tready,
    output logic                  m_axis_tlast
);

    // ================================================================
    // Constants
    // ================================================================
    localparam int TOTAL_DEPTH = 2 * BUFFER_DEPTH;
    localparam int ADDR_W      = $clog2(TOTAL_DEPTH > 1 ? TOTAL_DEPTH : 2);

    // ================================================================
    // Configuration registers (latched at start_in)
    // ================================================================
    logic [11:0] cin_r;            // cin_run
    logic [11:0] wpr_r;           // words per row (= w_out_beats)
    logic [11:0] n_rows_r;        // total output rows

    // beats_per_row = cin_r × wpr_r — computed in T_CFG via registered multiply
    (* use_dsp = "yes" *) logic [23:0] beats_per_row_r;

    // ================================================================
    // BRAM — single SDP, double-buffered via address offset
    //   Bank 0: addresses [0, BUFFER_DEPTH)
    //   Bank 1: addresses [BUFFER_DEPTH, 2*BUFFER_DEPTH)
    // ================================================================
    logic              bram_wr_en;
    logic [ADDR_W-1:0] bram_wr_addr;
    logic [DW_BEAT_W-1:0] bram_wr_data;
    logic              bram_rd_en;
    logic [ADDR_W-1:0] bram_rd_addr;
    logic [DW_BEAT_W-1:0] bram_rd_data;

    xpm_memory_sdpram #(
        .ADDR_WIDTH_A        (ADDR_W),
        .ADDR_WIDTH_B        (ADDR_W),
        .AUTO_SLEEP_TIME     (0),
        .BYTE_WRITE_WIDTH_A  (DW_BEAT_W),
        .CLOCKING_MODE       ("common_clock"),
        .ECC_MODE            ("no_ecc"),
        .MEMORY_INIT_FILE    ("none"),
        .MEMORY_INIT_PARAM   ("0"),
        .MEMORY_OPTIMIZATION ("true"),
        .MEMORY_PRIMITIVE    ("block"),
        .MEMORY_SIZE         (TOTAL_DEPTH * DW_BEAT_W),
        .MESSAGE_CONTROL     (0),
        .READ_DATA_WIDTH_B   (DW_BEAT_W),
        .READ_LATENCY_B      (1),
        .READ_RESET_VALUE_B  ("0"),
        .RST_MODE_A          ("SYNC"),
        .RST_MODE_B          ("SYNC"),
        .SIM_ASSERT_CHK      (0),
        .USE_MEM_INIT        (0),
        .WAKEUP_TIME         ("disable_sleep"),
        .WRITE_DATA_WIDTH_A  (DW_BEAT_W),
        .WRITE_MODE_B        ("no_change")
    ) u_bram (
        .clka   (clk),
        .ena    (1'b1),
        .wea    (bram_wr_en),
        .addra  (bram_wr_addr),
        .dina   (bram_wr_data),
        .injectsbiterra (1'b0),
        .injectdbiterra (1'b0),
        .clkb   (clk),
        .enb    (bram_rd_en),
        .rstb   (1'b0),
        .regceb (1'b1),
        .addrb  (bram_rd_addr),
        .doutb  (bram_rd_data),
        .sbiterrb (),
        .dbiterrb (),
        .sleep  (1'b0)
    );

    // ================================================================
    // Bank management
    // ================================================================
    logic wr_bank;   // 0: write bank 0, read bank 1.  1: write bank 1, read bank 0.
    wire [ADDR_W-1:0] wr_base = wr_bank ? ADDR_W'(BUFFER_DEPTH) : '0;
    wire [ADDR_W-1:0] rd_base = wr_bank ? '0 : ADDR_W'(BUFFER_DEPTH);

    // ================================================================
    // FSM type declarations
    // ================================================================
    typedef enum logic [2:0] {
        T_IDLE      = 3'd0,
        T_CFG       = 3'd1,   // compute beats_per_row (1-cycle registered multiply)
        T_FILL0     = 3'd2,   // fill first row (no concurrent read)
        T_STEADY_GO = 3'd3,   // 1-cycle settle after go pulses
        T_STEADY    = 3'd4,   // concurrent write+read (main pipeline)
        T_LAST_GO   = 3'd5,   // 1-cycle settle before last read
        T_LAST_RD   = 3'd6    // drain last row (no concurrent write)
    } top_st_t;
    top_st_t top_st;

    typedef enum logic [1:0] {
        WR_IDLE   = 2'd0,
        WR_ACTIVE = 2'd1,
        WR_DONE   = 2'd2
    } wr_st_t;
    wr_st_t wr_st;

    typedef enum logic [1:0] {
        RD_IDLE   = 2'd0,
        RD_ACTIVE = 2'd1,
        RD_DONE   = 2'd2
    } rd_st_t;
    rd_st_t rd_st;

    // ================================================================
    // Write sub-FSM variables
    // ================================================================
    logic [ADDR_W-1:0] wr_cnt;        // linear beat counter within bank
    logic [23:0]       wr_remaining;  // beats remaining (down-counter)
    logic              wr_go;         // 1-cycle pulse from top FSM

    // ================================================================
    // Read sub-FSM variables
    // ================================================================
    logic [ADDR_W-1:0] rd_ch_base;    // accumulator: base addr for current channel
    logic [11:0]       rd_group;      // pixel group index (0..wpr-1)
    logic [11:0]       rd_ch;         // channel index (0..cin-1)
    logic              rd_all_issued; // all BRAM reads have been issued
    logic              rd_go;         // 1-cycle pulse from top FSM
    logic              rd_last_row;   // the current read row is the final output row

    // Read pipeline registers
    logic              pipe_valid;    // BRAM read was issued last cycle; data arriving
    logic              pipe_is_last;  // the beat in the pipe is the very last output beat

    // Output register (AXI-Stream master side)
    logic              out_valid;
    logic [DW_BEAT_W-1:0] out_data;
    logic              out_last;

    // Pipeline enable: advance when output register is free or being consumed
    wire pipe_en = !out_valid || m_axis_tready;

    // ================================================================
    // Top-level FSM variables
    // ================================================================
    logic [11:0] wr_row_idx;  // count of rows written so far

    // ================================================================
    // Output port assignments
    // ================================================================
    assign m_axis_tdata  = out_data;
    assign m_axis_tvalid = out_valid;
    assign m_axis_tlast  = out_valid && out_last;
    assign s_axis_tready = (wr_st == WR_ACTIVE) && !start_in;

    // done_out fires when the very last output beat is accepted by PW
    assign done_out = m_axis_tvalid && m_axis_tready && m_axis_tlast;

    // ================================================================
    // Main sequential block — all FSMs in one always_ff
    //
    // Ordering within the block matters for non-blocking assignment
    // priority (last write wins):
    //   [A] Write sub-FSM
    //   [B] Read sub-FSM + output pipeline
    //   [C] Top-level FSM (LAST — its overrides take priority)
    // ================================================================
    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            // ---- Top FSM ----
            top_st         <= T_IDLE;
            wr_bank        <= 1'b0;
            wr_row_idx     <= '0;
            wr_go          <= 1'b0;
            rd_go          <= 1'b0;
            // ---- Config ----
            cin_r          <= '0;
            wpr_r          <= '0;
            n_rows_r       <= '0;
            beats_per_row_r <= '0;
            // ---- Write sub-FSM ----
            wr_st          <= WR_IDLE;
            wr_cnt         <= '0;
            wr_remaining   <= '0;
            bram_wr_en     <= 1'b0;
            bram_wr_addr   <= '0;
            bram_wr_data   <= '0;
            // ---- Read sub-FSM ----
            rd_st          <= RD_IDLE;
            rd_ch_base     <= '0;
            rd_group       <= '0;
            rd_ch          <= '0;
            rd_all_issued  <= 1'b0;
            rd_last_row    <= 1'b0;
            pipe_valid     <= 1'b0;
            pipe_is_last   <= 1'b0;
            bram_rd_en     <= 1'b0;
            bram_rd_addr   <= '0;
            // ---- Output register ----
            out_valid      <= 1'b0;
            out_data       <= '0;
            out_last       <= 1'b0;
        end else begin

            // ============================================================
            // Defaults: deassert single-cycle pulses each cycle
            // ============================================================
            bram_wr_en <= 1'b0;
            bram_rd_en <= 1'b0;
            wr_go      <= 1'b0;
            rd_go      <= 1'b0;

            // ============================================================
            // [A] WRITE SUB-FSM
            //
            // Accepts DW beats on s_axis in linear order.
            // BRAM address = wr_base + wr_cnt (simple counter).
            // Down-counter wr_remaining tracks completion.
            // ============================================================
            case (wr_st)
                WR_IDLE: begin
                    if (wr_go) begin
                        wr_cnt       <= '0;
                        wr_remaining <= beats_per_row_r - 24'd1;
                        wr_st        <= WR_ACTIVE;
                    end
                end

                WR_ACTIVE: begin
                    if (s_axis_tvalid && !start_in) begin
                        bram_wr_en   <= 1'b1;
                        bram_wr_addr <= wr_base + wr_cnt;
                        bram_wr_data <= s_axis_tdata;

                        if (wr_remaining == 24'd0) begin
                            wr_st <= WR_DONE;
                        end else begin
                            wr_cnt       <= wr_cnt + 1'b1;
                            wr_remaining <= wr_remaining - 24'd1;
                        end
                    end
                end

                WR_DONE: begin
                    if (wr_go) begin
                        wr_cnt       <= '0;
                        wr_remaining <= beats_per_row_r - 24'd1;
                        wr_st        <= WR_ACTIVE;
                    end
                end

                default: wr_st <= WR_IDLE;
            endcase

            // ============================================================
            // [B] READ SUB-FSM + OUTPUT PIPELINE
            //
            // Reads BRAM in transposed order:
            //   for group in 0..wpr-1:  (pixel-group outer)
            //     for ch in 0..cin-1:   (channel inner)
            //       addr = rd_base + ch*wpr + group
            //
            // Address computed via accumulator (no runtime multiply):
            //   rd_ch_base starts at rd_base, increments by wpr_r each ch.
            //   addr = rd_ch_base + group
            //
            // 2-stage pipeline with stall:
            //   Stage 1: issue BRAM read (1-cycle latency)
            //   Stage 2: capture into output register
            //   pipe_en gates advancement; on stall, BRAM output holds.
            // ============================================================

            // --- Output register: consume on handshake when pipe isn't refilling ---
            if (rd_st != RD_ACTIVE && out_valid && m_axis_tready) begin
                out_valid <= 1'b0;
            end

            case (rd_st)
                RD_IDLE: begin
                    if (rd_go) begin
                        rd_ch_base    <= rd_base;
                        rd_group      <= '0;
                        rd_ch         <= '0;
                        rd_all_issued <= 1'b0;
                        pipe_valid    <= 1'b0;
                        rd_st         <= RD_ACTIVE;
                    end
                end

                RD_ACTIVE: begin
                    if (pipe_en) begin
                        // --- Stage 2: capture arriving BRAM data ---
                        out_valid <= pipe_valid;
                        if (pipe_valid) begin
                            out_data <= bram_rd_data;
                            out_last <= pipe_is_last;
                        end

                        // --- Stage 1: issue next BRAM read ---
                        if (!rd_all_issued) begin
                            bram_rd_en   <= 1'b1;
                            bram_rd_addr <= rd_ch_base + ADDR_W'(rd_group);
                            pipe_valid   <= 1'b1;
                            pipe_is_last <= rd_last_row &&
                                            (rd_group == wpr_r  - 12'd1) &&
                                            (rd_ch    == cin_r  - 12'd1);

                            // Advance counters: channel inner, group outer
                            if (rd_ch + 12'd1 == cin_r) begin
                                rd_ch      <= '0;
                                rd_ch_base <= rd_base;   // reset to bank base
                                if (rd_group + 12'd1 == wpr_r) begin
                                    rd_all_issued <= 1'b1;
                                end else begin
                                    rd_group <= rd_group + 12'd1;
                                end
                            end else begin
                                rd_ch      <= rd_ch + 12'd1;
                                rd_ch_base <= rd_ch_base + ADDR_W'(wpr_r);
                            end
                        end else begin
                            pipe_valid <= 1'b0;
                        end
                    end
                    // When !pipe_en: stall — BRAM output register holds data

                    // Transition to RD_DONE when pipeline is fully drained
                    if (rd_all_issued && !pipe_valid && pipe_en) begin
                        if (!out_valid || m_axis_tready) begin
                            rd_st <= RD_DONE;
                        end
                    end
                end

                RD_DONE: begin
                    if (rd_go) begin
                        rd_ch_base    <= rd_base;
                        rd_group      <= '0;
                        rd_ch         <= '0;
                        rd_all_issued <= 1'b0;
                        pipe_valid    <= 1'b0;
                        rd_st         <= RD_ACTIVE;
                    end
                end

                default: rd_st <= RD_IDLE;
            endcase

            // ============================================================
            // [C] TOP-LEVEL FSM (last — overrides sub-FSM state transitions)
            //
            // Orchestrates bank swapping and sub-FSM go/done handshaking.
            //
            // Flow:
            //   T_IDLE → T_CFG → T_FILL0 → [T_STEADY_GO → T_STEADY]* →
            //   T_LAST_GO → T_LAST_RD → T_IDLE
            //
            // *_GO states are 1-cycle waits that allow sub-FSMs to see
            // the go pulse and transition out of DONE before the top FSM
            // re-checks their state.
            // ============================================================
            case (top_st)
                // --------------------------------------------------------
                // T_IDLE: wait for start_in, latch configuration
                // --------------------------------------------------------
                T_IDLE: begin
                    if (start_in) begin
                        cin_r      <= cin_run;
                        wpr_r      <= w_out_beats;
                        n_rows_r   <= n_rows;
                        wr_bank    <= 1'b0;
                        wr_row_idx <= '0;
                        // Reset sub-FSMs (override [A]/[B] assignments)
                        wr_st      <= WR_IDLE;
                        rd_st      <= RD_IDLE;
                        out_valid  <= 1'b0;
                        pipe_valid <= 1'b0;
                        top_st     <= T_CFG;
                    end
                end

                // --------------------------------------------------------
                // T_CFG: compute beats_per_row (registered 12×12 multiply)
                //   and kick off the write sub-FSM for the first row
                // --------------------------------------------------------
                T_CFG: begin
                    beats_per_row_r <= {12'd0, cin_r} * {12'd0, wpr_r};
                    wr_go  <= 1'b1;
                    top_st <= T_FILL0;
                end

                // --------------------------------------------------------
                // T_FILL0: wait for first row to be written to bank 0
                // --------------------------------------------------------
                T_FILL0: begin
                    if (wr_st == WR_DONE) begin
                        wr_row_idx <= 12'd1;
                        if (n_rows_r == 12'd1) begin
                            // Single row: no more writes, just read
                            rd_go       <= 1'b1;
                            rd_last_row <= 1'b1;
                            top_st      <= T_LAST_GO;
                        end else begin
                            // Swap banks: write row 1 to bank 1, read row 0 from bank 0
                            wr_bank     <= 1'b1;
                            wr_go       <= 1'b1;
                            rd_go       <= 1'b1;
                            rd_last_row <= (n_rows_r == 12'd2);
                            top_st      <= T_STEADY_GO;
                        end
                    end
                end

                // --------------------------------------------------------
                // T_STEADY_GO: 1-cycle settle (sub-FSMs absorb go pulse)
                // --------------------------------------------------------
                T_STEADY_GO: begin
                    top_st <= T_STEADY;
                end

                // --------------------------------------------------------
                // T_STEADY: concurrent write+read; wait for both to finish
                // --------------------------------------------------------
                T_STEADY: begin
                    if (wr_st == WR_DONE && rd_st == RD_DONE) begin
                        wr_row_idx <= wr_row_idx + 12'd1;
                        if (wr_row_idx + 12'd1 >= n_rows_r) begin
                            // All rows written; read the last one
                            rd_go       <= 1'b1;
                            rd_last_row <= 1'b1;
                            top_st      <= T_LAST_GO;
                        end else begin
                            // More rows: swap banks, restart both
                            wr_bank     <= !wr_bank;
                            wr_go       <= 1'b1;
                            rd_go       <= 1'b1;
                            rd_last_row <= (wr_row_idx + 12'd2 >= n_rows_r);
                            top_st      <= T_STEADY_GO;
                        end
                    end
                end

                // --------------------------------------------------------
                // T_LAST_GO: 1-cycle settle before last read
                // --------------------------------------------------------
                T_LAST_GO: begin
                    top_st <= T_LAST_RD;
                end

                // --------------------------------------------------------
                // T_LAST_RD: wait for done_out (last beat accepted on m_axis)
                // --------------------------------------------------------
                T_LAST_RD: begin
                    if (done_out) begin
                        top_st <= T_IDLE;
                    end
                end

                default: top_st <= T_IDLE;
            endcase

        end // else (!rst_n)
    end // always_ff

endmodule

`default_nettype wire
