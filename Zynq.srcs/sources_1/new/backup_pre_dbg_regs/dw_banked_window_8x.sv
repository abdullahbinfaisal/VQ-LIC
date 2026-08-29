`timescale 1ns / 1ps
// ============================================================================
// dw_banked_window_8x.sv — Group-major streaming REAL 3x3 depthwise windower,
// banked per channel.
// ============================================================================
//
// Replaces dw_seq_window_8x.sv, which only ever computed a 1x3 horizontal
// conv (top/bottom kernel rows tied to zp_in) -- wrong for this network's
// real, genuine 3x3 DW layers. See DW_PW_FUSION_PLAN_V2.md for history.
//
// REV 2 (2026-07-24, post-xsim-failure fix): rev 1 ported line_buffer_8x's
// horizontal halo trick verbatim ("use the next beat in the stream as the
// right-neighbor"), which is only valid when consecutive beats are
// horizontally adjacent -- true in line_buffer_8x's single-channel design,
// FALSE here, where consecutive beats are usually DIFFERENT CHANNELS
// (channel is the fastest-varying index in group-major order). xsim caught
// this: every failure was at a lane-0/lane-7 boundary tap, every center tap
// was correct.
//
// Fix: the VERTICAL context mechanism (banked line0/line1, addressed flat
// by (group,channel), row-parity ping-pong) is unchanged -- it tested
// clean. The HORIZONTAL halo now reuses dw_seq_window_8x's proven
// one-group-delayed-per-channel scheme (prev_group[c]/prev_last[c]) instead
// of "next beat", widened to buffer a channel's full pending 3-row vertical
// context (not just 1 row) so the two mechanisms compose correctly:
//   - line0/line1 (per (g,c) slot) hold this beat's own 3 vertical context
//     rows, exactly as rev 1.
//   - A NEW per-channel RAM (pend_ram) holds the PREVIOUS group's 3-row
//     context for that channel, not yet emitted -- emission is delayed by
//     exactly one group per channel, so the right-neighbor (this beat's own
//     lane-0 sample, in each of its 3 rows) is available when needed.
//   - A per-row horizontal flush pass (one beat per channel, right-context
//     = zp_in) completes the last group of every row, exactly mirroring
//     dw_seq_window_8x's S_FLUSH but now run once per row instead of once
//     per whole layer. The existing outer vertical flush ROW (r==H) is
//     still needed on top of this -- it supplies the bottom vertical
//     context for the last real row, a separate concern from the
//     per-row horizontal flush.
//
// INPUT/OUTPUT ORDER (group-major, channel-minor, matching PW's stream):
//   for row r in 0..H-1: for group g in 0..G-1: for channel c in 0..C-1:
//     one 8-sample beat
//   (plus one extra vertical-flush row, r==H, all zp_in)
//
// STORAGE: line0/line1 flat depth MAX_CG_PRODUCT (see DW_PW_FUSION_PLAN_V2.md
// SS5/SS7 -- C and G never peak simultaneously across this network's real
// layers). pend_ram is a separate, much smaller per-channel-only array
// (depth CIN_MAX, width 3*64 + 3*8 bits: 3 vertical context rows plus one
// left-continuity byte per row).
//
// SCOPE: stride-1 'same' conv only. The 4 real stride-2 DW layers
// (L08/16/24/38) stay on the legacy line_buffer_8x/compute_engine/
// dw_plane_run_axis path, untouched -- no stride_2/bypass_1x1 ports here.
// ============================================================================

module dw_banked_window_8x #(
    parameter int DATA_WIDTH     = 8,
    parameter int CIN_MAX        = 240,
    // Worst-case C x G product across every real DW layer in this network's
    // instr.bin (DW_PW_FUSION_PLAN_V2.md SS7) -- NOT CIN_MAX x G_MAX. Re-derive
    // this if the network schedule ever changes.
    parameter int MAX_CG_PRODUCT = 10800
)(
    input  logic                        clk,
    input  logic                        rst_n,

    input  logic                        start_in,     // 1-cycle pulse
    input  logic [11:0]                 cin_run,      // C  (channels)
    input  logic [11:0]                 n_groups,     // G  (8-sample groups per row)
    input  logic [11:0]                 img_width,    // real row width, unpadded samples
    input  logic [11:0]                 n_rows,       // H  (real rows)
    input  logic [DATA_WIDTH-1:0]       zp_in,

    // Group-major input stream (8 samples of current channel)
    input  logic [8*DATA_WIDTH-1:0]     pixel_in,
    input  logic                        valid_in,
    output logic                        consume_in,   // pop input FIFO this cycle

    // Window interface (identical shape to line_buffer_8x / dw_seq_window_8x
    // -- conv_mac_array / ppu / dw_fused_core need zero changes)
    output logic [DATA_WIDTH-1:0]       window [0:7][2:0][2:0],
    output logic [7:0]                  valid_out_vec,
    output logic [11:0]                 ch_out,       // channel index of emitted window
    output logic                        done_out
);

    localparam int SLOT_AW = $clog2(MAX_CG_PRODUCT);
    localparam int PB_AW   = (CIN_MAX <= 1) ? 1 : $clog2(CIN_MAX);

    // ------------------------------------------------------------
    // Banked vertical-context line buffers: flat depth MAX_CG_PRODUCT.
    // ------------------------------------------------------------
    (* ram_style = "block" *) logic [63:0] line0 [0:MAX_CG_PRODUCT-1];
    (* ram_style = "block" *) logic [63:0] line1 [0:MAX_CG_PRODUCT-1];

    // Per-channel pending (one-group-delayed) horizontal buffer: this
    // channel's previous group's 3 vertical context rows, not yet emitted,
    // plus one left-continuity byte per row (the last sample of the group
    // BEFORE the pending one -- dw_seq_window_8x's prev_last, tripled).
    (* ram_style = "block" *) logic [3*64+3*8-1:0] pend_ram [0:CIN_MAX-1];

    // ------------------------------------------------------------
    // Config latched at start_in
    // ------------------------------------------------------------
    logic [11:0] C_r, G_r, W_r, H_r;

    always_ff @(posedge clk) begin
        if (start_in) begin
            C_r <= cin_run;
            G_r <= n_groups;
            W_r <= img_width;
            H_r <= n_rows;
        end
    end

    // ------------------------------------------------------------
    // FSM counters: channel-fastest, then group (0..G inclusive -- G_r
    // itself is the per-row horizontal-flush slot), then row.
    // ------------------------------------------------------------
    // DRAIN_CYCLES: after a row's flush slot finishes, its later beats
    // (esp. the last channel processed) are still in flight through
    // Stage1->2, and their pend_ram write-backs don't commit until 4
    // cycles after their own Stage0 capture (verified: capture @T0 ->
    // s0_valid @T0+1 -> pend_ram read-issue @T0+2 -> write-back issue
    // @T0+3 -> commit visible @T0+4). Without a drain, the next row's own
    // g=0 writes to the same per-channel pend_ram race those in-flight
    // writes and can be silently clobbered by them, right at every row
    // boundary. 4 cycles is the minimum with a 1-cycle margin.
    localparam int DRAIN_CYCLES = 4;

    logic                running, toggle, draining, pending_finish;
    logic [11:0]         c_cnt, g_cnt, r_cnt;
    logic [SLOT_AW-1:0]  slot_idx;
    logic [3:0]          drain_cnt;

    logic real_row, real_group, need_input, advance;
    logic last_beat_of_group, last_beat_of_row;

    always_comb begin
        real_row           = running && (r_cnt < H_r);
        real_group         = (g_cnt < G_r);              // false only in the flush slot
        need_input         = real_group && real_row;
        advance            = running && !draining && (!need_input || valid_in);
        consume_in         = advance && need_input;
        last_beat_of_group = (c_cnt == C_r - 12'd1);
        last_beat_of_row   = last_beat_of_group && (g_cnt == G_r);
    end

    always_ff @(posedge clk) begin
        if (!rst_n) begin
            running        <= 1'b0;
            c_cnt          <= 12'd0;
            g_cnt          <= 12'd0;
            r_cnt          <= 12'd0;
            slot_idx       <= '0;
            done_out       <= 1'b0;
            draining       <= 1'b0;
            drain_cnt      <= '0;
            pending_finish <= 1'b0;
        end else if (start_in) begin
            running        <= 1'b1;
            c_cnt          <= 12'd0;
            g_cnt          <= 12'd0;
            r_cnt          <= 12'd0;
            slot_idx       <= '0;
            done_out       <= 1'b0;
            draining       <= 1'b0;
            drain_cnt      <= '0;
            pending_finish <= 1'b0;
        end else if (draining) begin
            done_out <= 1'b0;
            if (drain_cnt == 4'd0) begin
                draining <= 1'b0;
                if (pending_finish) begin
                    running  <= 1'b0;
                    done_out <= 1'b1;
                end
            end else begin
                drain_cnt <= drain_cnt - 4'd1;
            end
        end else if (advance) begin
            done_out <= 1'b0;
            if (last_beat_of_row) begin
                c_cnt          <= 12'd0;
                g_cnt          <= 12'd0;
                slot_idx       <= '0;
                draining       <= 1'b1;
                drain_cnt      <= DRAIN_CYCLES[3:0] - 4'd1;
                pending_finish <= (r_cnt == H_r);
                if (r_cnt != H_r) r_cnt <= r_cnt + 12'd1;
            end else if (last_beat_of_group) begin
                c_cnt <= 12'd0;
                g_cnt <= g_cnt + 12'd1;
                // BUG FIX (2026-07-24): this branch used to leave slot_idx
                // untouched, so the first beat of every new group reused
                // the previous group's flat vertical-bank address, and
                // every beat after that in the row stayed off by one slot
                // for the rest of the row. Must increment here too,
                // exactly like the plain per-beat branch below.
                if (real_group) slot_idx <= slot_idx + 1'b1;
            end else begin
                c_cnt <= c_cnt + 12'd1;
                if (real_group) slot_idx <= slot_idx + 1'b1;
            end
        end else begin
            done_out <= 1'b0;
        end
    end

    always_ff @(posedge clk) begin
        if (!rst_n)                           toggle <= 1'b0;
        else if (start_in)                    toggle <= 1'b0;
        else if (advance && last_beat_of_row)  toggle <= ~toggle;
    end

    // ==========================================
    // STAGE 0: vertical-bank read/write issue (only for real groups)
    // ==========================================
    logic [11:0] s0_g, s0_c, s0_row;
    logic        s0_valid, s0_is_flush;
    logic [63:0] s0_px_in;

    logic [63:0] line0_rd, line1_rd;
    logic [63:0] r2_in_raw;

    always_comb r2_in_raw = real_row ? pixel_in : {8{zp_in}};

    always_ff @(posedge clk) begin
        if (advance) begin
            if (real_group) begin
                line0_rd <= line0[slot_idx];
                line1_rd <= line1[slot_idx];
                if (toggle == 1'b0) line0[slot_idx] <= r2_in_raw;
                else                line1[slot_idx] <= r2_in_raw;
            end
            s0_g        <= g_cnt;
            s0_c        <= c_cnt;
            s0_row      <= r_cnt;
            s0_is_flush <= !real_group;
            s0_px_in    <= r2_in_raw;
        end

        if (!rst_n || start_in) s0_valid <= 1'b0;
        else if (advance)       s0_valid <= 1'b1;
        else                    s0_valid <= 1'b0;
    end

    // ==========================================
    // STAGE 1: row-parity mux -> this beat's OWN 3-row vertical context
    // (all zp if this is the per-row horizontal-flush slot)
    // ==========================================
    logic [63:0] R0_in, R1_in;
    logic [63:0] s1_R0, s1_R1, s1_R2;
    logic [11:0] s1_g, s1_c, s1_row;
    logic        s1_valid, s1_is_flush;

    always_comb begin
        if (s0_row[0] == 1'b0) begin
            R0_in = line0_rd;
            R1_in = line1_rd;
        end else begin
            R0_in = line1_rd;
            R1_in = line0_rd;
        end
    end

    always_ff @(posedge clk) begin
        if (s0_valid) begin
            if (s0_is_flush) begin
                s1_R0 <= {8{zp_in}};
                s1_R1 <= {8{zp_in}};
                s1_R2 <= {8{zp_in}};
            end else begin
                s1_R0 <= R0_in;
                s1_R1 <= R1_in;
                s1_R2 <= s0_px_in;
            end
            s1_g        <= s0_g;
            s1_c        <= s0_c;
            s1_row      <= s0_row;
            s1_is_flush <= s0_is_flush;
        end
        if (!rst_n || start_in) s1_valid <= 1'b0;
        else                    s1_valid <= s0_valid;
    end

    // ==========================================
    // STAGE 2: pend_ram read issue (per channel) + hold this beat's
    // vertical context one more cycle so both arrive together at Stage 3
    // ==========================================
    logic [3*64+3*8-1:0] pend_rd_data;
    logic [63:0]     s2_R0_hold, s2_R1_hold, s2_R2_hold;
    logic [11:0]     s2_g, s2_c, s2_row;
    logic            s2_valid, s2_is_flush;

    always_ff @(posedge clk) begin
        if (s1_valid) begin
            pend_rd_data <= pend_ram[s1_c[PB_AW-1:0]];
            s2_R0_hold   <= s1_R0;
            s2_R1_hold   <= s1_R1;
            s2_R2_hold   <= s1_R2;
            s2_g         <= s1_g;
            s2_c         <= s1_c;
            s2_row       <= s1_row;
            s2_is_flush  <= s1_is_flush;
        end
        if (!rst_n || start_in) s2_valid <= 1'b0;
        else                    s2_valid <= s1_valid;
    end

    // Decode the OLD pending value (the group about to be emitted, plus
    // its own left-continuity byte per row).
    logic [63:0] pend_R0, pend_R1, pend_R2;
    logic [7:0]  pend_L0, pend_L1, pend_L2;
    always_comb {pend_R0, pend_R1, pend_R2, pend_L0, pend_L1, pend_L2} = pend_rd_data;

    // write-back: this beat's own context becomes the new pending value for
    // its channel. New left-continuity byte = OLD pending group's own lane-7
    // sample (dw_seq_window_8x's prev_last<=prev_group[7] rule), or zp_in if
    // this is the row's first group for this channel (nothing real precedes
    // it -- dw_seq_window_8x's wb_first rule).
    logic [7:0] new_left0, new_left1, new_left2;
    always_comb begin
        new_left0 = (s2_g == 12'd0) ? zp_in : pend_R0[63:56];
        new_left1 = (s2_g == 12'd0) ? zp_in : pend_R1[63:56];
        new_left2 = (s2_g == 12'd0) ? zp_in : pend_R2[63:56];
    end

    always_ff @(posedge clk) begin
        if (s2_valid) begin
            pend_ram[s2_c[PB_AW-1:0]] <= {s2_R0_hold, s2_R1_hold, s2_R2_hold,
                                          new_left0, new_left1, new_left2};
        end
    end

    // ==========================================
    // STAGE 3: emit the PENDING group's window. Masking mirrors
    // line_buffer_8x's proven 10-position X_byte/is_x_valid scheme exactly
    // (k=0 is lane0's left neighbor, k=1..8 are lanes 0..7's own centers,
    // k=9 is lane7's right neighbor) -- each of the 3 tap columns for every
    // lane gets its OWN independent bounds check, not just the center's.
    // Emission is the group at s2_g - 1 (or G_r-1 during the flush slot,
    // where s2_g == G_r) -- suppressed for s2_g == 0 (no pending group
    // exists yet for that channel/row).
    // ==========================================
    logic emit_valid;
    logic signed [13:0] emit_g_s;
    always_comb begin
        emit_valid = s2_valid && (s2_is_flush || (s2_g != 12'd0));
        emit_g_s   = $signed({2'b0, s2_g}) - 14'sd1;
    end

    logic signed [13:0] Y;
    assign Y = $signed({2'b0, s2_row}) - 14'sd1;

    logic is_y_valid [0:2];
    always_comb begin
        is_y_valid[0] = (Y - 1 >= 0) && (Y - 1 < $signed({2'b0, H_r}));
        is_y_valid[1] = (Y     >= 0) && (Y     < $signed({2'b0, H_r}));
        is_y_valid[2] = (Y + 1 >= 0) && (Y + 1 < $signed({2'b0, H_r}));
    end

    logic signed [13:0] X_byte [0:9];
    logic is_x_valid [0:9];
    always_comb begin
        for (int k = 0; k < 10; k++) begin
            X_byte[k]     = emit_g_s * 8 + k - 1;
            is_x_valid[k] = (X_byte[k] >= 0) && (X_byte[k] < $signed({2'b0, W_r}));
        end
    end

    logic [7:0] raw_r0 [0:9], raw_r1 [0:9], raw_r2 [0:9];
    always_comb begin
        raw_r0[0] = pend_L0; raw_r1[0] = pend_L1; raw_r2[0] = pend_L2;
        for (int l = 0; l < 8; l++) begin
            raw_r0[l+1] = pend_R0[l*8 +: 8];
            raw_r1[l+1] = pend_R1[l*8 +: 8];
            raw_r2[l+1] = pend_R2[l*8 +: 8];
        end
        raw_r0[9] = s2_R0_hold[7:0]; raw_r1[9] = s2_R1_hold[7:0]; raw_r2[9] = s2_R2_hold[7:0];
    end

    logic [7:0] masked_r0 [0:9], masked_r1 [0:9], masked_r2 [0:9];
    always_comb begin
        for (int k = 0; k < 10; k++) begin
            masked_r0[k] = (is_y_valid[0] && is_x_valid[k]) ? raw_r0[k] : zp_in;
            masked_r1[k] = (is_y_valid[1] && is_x_valid[k]) ? raw_r1[k] : zp_in;
            masked_r2[k] = (is_y_valid[2] && is_x_valid[k]) ? raw_r2[k] : zp_in;
        end
    end

    logic [DATA_WIDTH-1:0] window_comb [0:7][2:0][2:0];
    logic [7:0]            valid_comb;

    always_comb begin
        for (int l = 0; l < 8; l++) begin
            window_comb[l][0][0] = masked_r0[l]; window_comb[l][0][1] = masked_r0[l+1]; window_comb[l][0][2] = masked_r0[l+2];
            window_comb[l][1][0] = masked_r1[l]; window_comb[l][1][1] = masked_r1[l+1]; window_comb[l][1][2] = masked_r1[l+2];
            window_comb[l][2][0] = masked_r2[l]; window_comb[l][2][1] = masked_r2[l+1]; window_comb[l][2][2] = masked_r2[l+2];
            valid_comb[l] = emit_valid && is_x_valid[l+1] && is_y_valid[1];
        end
    end

    always_ff @(posedge clk) begin
        if (!rst_n || start_in) begin
            valid_out_vec <= 8'd0;
            ch_out        <= 12'd0;
        end else begin
            valid_out_vec <= valid_comb;
            ch_out        <= s2_c;
            for (int i = 0; i < 8; i++)
                for (int r = 0; r < 3; r++)
                    for (int c = 0; c < 3; c++)
                        window[i][r][c] <= window_comb[i][r][c];
        end
    end

endmodule
`default_nettype wire
