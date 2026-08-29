`timescale 1ns / 1ps
// ============================================================================
// dw_seq_window_8x.sv — Group-major streaming 1-D depthwise windower
// ============================================================================
//
// Drop-in replacement for line_buffer_8x INSIDE the fused DW path. It produces
// the SAME 8-lane `window[0:7][2:0][2:0]` interface, so conv_mac_array and ppu
// are reused unchanged. The two unused window rows (r=0, r=2) are driven with
// zp_in, so (window-zp)*w = 0 for them and the effective op is a 1x3 conv on the
// middle row {left, center, right}.
//
// INPUT ORDER (group-major / channel-minor, matching PW's stream):
//   for g in 0..G-1:            // G = n_groups = ceil(W/8)
//     for c in 0..C-1:          // C = cin_run
//       beat = 8 samples of channel c at sequence positions [8g .. 8g+7]
//
// OUTPUT ORDER (same group-major order, 1 group of latency):
//   for g in 0..G-1: for c in 0..C-1: one 8-lane window (output group g, ch c)
//   -> 8x conv_mac_array -> 8x ppu -> 8-sample beat = PW input order.
//
// HALO (per channel, across the channel interleave):
//   out[c][8g+l] needs in[c][8g+l-1] (left) and in[c][8g+l+1] (right).
//     lane 0 left  = last sample of channel c's PREVIOUS group   (prev_last[c])
//     lane 7 right = first sample of channel c's NEXT group       (curr[0])
//   So output group g-1 is emitted when input group g of the same channel
//   arrives (1-group delay); the final group g=G-1 is emitted in a FLUSH phase
//   with right-pad = zp_in. prev_last is seeded with zp_in (left padding).
//
// STORAGE: one (8+64)-bit x CIN_MAX SDP BRAM holding {prev_last, prev_group}.
//
// SCOPE: stride-1 'same' conv (v1). Stride-2 is handled elsewhere (v2) —
//   see DW_PW_FUSION_PLAN.md.
// ============================================================================

module dw_seq_window_8x #(
    parameter int DATA_WIDTH = 8,
    parameter int CIN_MAX    = 240
)(
    input  logic                        clk,
    input  logic                        rst_n,

    input  logic                        start_in,     // 1-cycle pulse
    input  logic [11:0]                 cin_run,      // C  (channels)
    input  logic [11:0]                 n_groups,     // G  (8-sample groups along sequence)
    input  logic [DATA_WIDTH-1:0]       zp_in,

    // Group-major input stream (8 samples of current channel)
    input  logic [8*DATA_WIDTH-1:0]     pixel_in,
    input  logic                        valid_in,
    output logic                        consume_in,   // pop input FIFO this cycle

    // Window interface (identical shape to line_buffer_8x)
    output logic [DATA_WIDTH-1:0]       window [0:7][2:0][2:0],
    output logic [7:0]                  valid_out_vec,
    output logic [11:0]                 ch_out,       // channel index of emitted window
    output logic                        done_out
);

    localparam int PB_AW   = (CIN_MAX <= 1) ? 1 : $clog2(CIN_MAX);
    localparam int GROUP_W = 8 * DATA_WIDTH;          // 64
    localparam int RAM_W   = GROUP_W + DATA_WIDTH;    // 72: {prev_last, prev_group}

    // Config latched at start_in
    logic [11:0] cin_r, ngrp_r;

    // ------------------------------------------------------------
    // {prev_last[c], prev_group[c]} BRAM (port A write, port B read)
    // ------------------------------------------------------------
    logic [PB_AW-1:0] ram_wr_addr, ram_rd_addr;
    logic             ram_wr_en,   ram_rd_en;
    logic [RAM_W-1:0] ram_wr_data, ram_rd_data;

    xpm_memory_sdpram #(
        .ADDR_WIDTH_A        (PB_AW),
        .ADDR_WIDTH_B        (PB_AW),
        .AUTO_SLEEP_TIME     (0),
        .BYTE_WRITE_WIDTH_A  (RAM_W),
        .CLOCKING_MODE       ("common_clock"),
        .ECC_MODE            ("no_ecc"),
        .MEMORY_INIT_FILE    ("none"),
        .MEMORY_INIT_PARAM   ("0"),
        .MEMORY_OPTIMIZATION ("true"),
        .MEMORY_PRIMITIVE    ("block"),
        .MEMORY_SIZE         (CIN_MAX * RAM_W),
        .MESSAGE_CONTROL     (0),
        .READ_DATA_WIDTH_B   (RAM_W),
        .READ_LATENCY_B      (1),
        .READ_RESET_VALUE_B  ("0"),
        .RST_MODE_A          ("SYNC"),
        .RST_MODE_B          ("SYNC"),
        .SIM_ASSERT_CHK      (0),
        .USE_MEM_INIT        (0),
        .WAKEUP_TIME         ("disable_sleep"),
        .WRITE_DATA_WIDTH_A  (RAM_W),
        .WRITE_MODE_B        ("read_first")
    ) u_halo_ram (
        .clka  (clk), .ena (1'b1), .wea (ram_wr_en),
        .addra (ram_wr_addr), .dina (ram_wr_data),
        .injectsbiterra(1'b0), .injectdbiterra(1'b0),
        .clkb  (clk), .enb (ram_rd_en), .rstb (1'b0), .regceb (1'b1),
        .addrb (ram_rd_addr), .doutb (ram_rd_data),
        .sbiterrb(), .dbiterrb(), .sleep(1'b0)
    );

    // ------------------------------------------------------------
    // FSM
    // ------------------------------------------------------------
    typedef enum logic [1:0] {S_IDLE, S_STREAM, S_FLUSH, S_DONE} st_t;
    st_t st;

    logic [11:0] in_ch, in_grp;   // group-major iteration over the input stream
    logic [11:0] fl_ch;           // flush channel counter

    // S0 -> S1 pipeline registers (the beat captured last cycle)
    logic               s1_valid;     // emit a window this cycle
    logic               s1_is_flush;  // emit comes from flush phase (right-pad = zp)
    logic [11:0]        s1_ch;
    logic [GROUP_W-1:0] s1_curr;      // current group samples (lane-7 right halo source)

    // write-back pipeline (store curr as new prev_group, update prev_last)
    logic               wb_pending;
    logic [11:0]        wb_ch;
    logic [GROUP_W-1:0] wb_curr;
    logic               wb_first;     // captured group was g==0 -> prev_last := zp

    // FWFT pop: combinational, same cycle the beat is read
    assign consume_in = (st == S_STREAM) && valid_in;

    // ------------------------------------------------------------
    // BRAM read address: stream reads in_ch, flush reads fl_ch
    // ------------------------------------------------------------
    always_comb begin
        ram_rd_en   = 1'b0;
        ram_rd_addr = '0;
        if (st == S_STREAM && valid_in) begin
            ram_rd_en   = 1'b1;
            ram_rd_addr = in_ch[PB_AW-1:0];
        end else if (st == S_FLUSH) begin
            ram_rd_en   = 1'b1;
            ram_rd_addr = fl_ch[PB_AW-1:0];
        end
    end

    // ------------------------------------------------------------
    // Window build (S1, combinational): ram_rd_data = {prev_last, prev_group}
    // ------------------------------------------------------------
    logic [DATA_WIDTH-1:0] pg [0:7];   // prev_group samples
    logic [DATA_WIDTH-1:0] pl;         // prev_last (lane-0 left tap)
    always_comb begin
        pl = ram_rd_data[GROUP_W +: DATA_WIDTH];
        for (int l = 0; l < 8; l++)
            pg[l] = ram_rd_data[l*DATA_WIDTH +: DATA_WIDTH];
    end

    always_comb begin
        logic [DATA_WIDTH-1:0] left, center, right;
        for (int l = 0; l < 8; l++) begin
            center = pg[l];
            left   = (l == 0) ? pl : pg[l-1];
            if (l == 7) right = s1_is_flush ? zp_in : s1_curr[0 +: DATA_WIDTH];
            else        right = pg[l+1];

            // middle row = real 1x3 taps; top/bottom rows = zp (zero contribution)
            window[l][0][0] = zp_in; window[l][0][1] = zp_in;  window[l][0][2] = zp_in;
            window[l][1][0] = left;  window[l][1][1] = center; window[l][1][2] = right;
            window[l][2][0] = zp_in; window[l][2][1] = zp_in;  window[l][2][2] = zp_in;
        end
    end

    assign valid_out_vec = s1_valid ? 8'hFF : 8'h00;
    assign ch_out        = s1_ch;

    // ------------------------------------------------------------
    // Single sequential block: FSM + counters + S1 regs + write-back
    // ------------------------------------------------------------
    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            st          <= S_IDLE;
            cin_r       <= '0;  ngrp_r <= '0;
            in_ch       <= '0;  in_grp <= '0;  fl_ch <= '0;
            s1_valid    <= 1'b0; s1_is_flush <= 1'b0; s1_ch <= '0; s1_curr <= '0;
            wb_pending  <= 1'b0; wb_ch <= '0;  wb_curr <= '0; wb_first <= 1'b0;
            ram_wr_en   <= 1'b0; ram_wr_addr <= '0; ram_wr_data <= '0;
            done_out    <= 1'b0;
        end else begin
            // default deasserts
            s1_valid    <= 1'b0;
            s1_is_flush <= 1'b0;
            wb_pending  <= 1'b0;
            ram_wr_en   <= 1'b0;
            done_out    <= 1'b0;

            // ---- write-back: execute the store scheduled last cycle ----
            // ram_rd_data now holds the OLD {prev_last,prev_group} for wb_ch.
            if (wb_pending) begin
                ram_wr_en   <= 1'b1;
                ram_wr_addr <= wb_ch[PB_AW-1:0];
                ram_wr_data <= { (wb_first ? zp_in
                                            : ram_rd_data[7*DATA_WIDTH +: DATA_WIDTH]),
                                 wb_curr };
            end

            case (st)
                S_IDLE: begin
                    if (start_in) begin
                        cin_r  <= cin_run;
                        ngrp_r <= n_groups;
                        in_ch  <= '0;
                        in_grp <= '0;
                        st     <= S_STREAM;
                    end
                end

                // accept group-major beats; emit output group (in_grp-1)
                S_STREAM: begin
                    if (valid_in) begin
                        // capture for S1 emit
                        s1_ch    <= in_ch;
                        s1_curr  <= pixel_in;
                        s1_valid <= (in_grp != 12'd0);

                        // schedule write-back of this beat
                        wb_pending <= 1'b1;
                        wb_ch      <= in_ch;
                        wb_curr    <= pixel_in;
                        wb_first   <= (in_grp == 12'd0);

                        // advance group-major counters
                        if (in_ch + 12'd1 == cin_r) begin
                            in_ch <= '0;
                            if (in_grp + 12'd1 == ngrp_r) begin
                                st    <= S_FLUSH;
                                fl_ch <= '0;
                            end else begin
                                in_grp <= in_grp + 12'd1;
                            end
                        end else begin
                            in_ch <= in_ch + 12'd1;
                        end
                    end
                end

                // emit the final group (g=G-1) for every channel, right halo = zp
                S_FLUSH: begin
                    s1_ch       <= fl_ch;
                    s1_valid    <= 1'b1;
                    s1_is_flush <= 1'b1;
                    if (fl_ch + 12'd1 == cin_r) st <= S_DONE;
                    else                        fl_ch <= fl_ch + 12'd1;
                end

                S_DONE: begin
                    done_out <= 1'b1;
                    st       <= S_IDLE;
                end

                default: st <= S_IDLE;
            endcase
        end
    end

endmodule
`default_nettype wire
