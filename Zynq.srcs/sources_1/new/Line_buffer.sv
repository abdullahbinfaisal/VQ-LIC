`timescale 1ns / 1ps

module line_buffer #(
    parameter int DATA_WIDTH     = 8,
    parameter int MAX_IMG_WIDTH  = 2048,
    parameter int MAX_PAD        = 1
)(
    input  logic                        clk,
    input  logic                        rst_n,

    input  logic                        start_in,      // 1-cycle pulse
    input  logic [11:0]                 img_width,     // Real Width
    input  logic [11:0]                 img_height,    // Real Height
    input  logic                        stride_2,
    input  logic                        bypass_1x1,
    input  logic [7:0]                  pad_top,       // e.g. 1
    input  logic [DATA_WIDTH-1:0]       zp_in,

    input  logic [DATA_WIDTH-1:0]       pixel_in,
    input  logic                        valid_in,

    output logic [DATA_WIDTH-1:0]       window [2:0][2:0],
    output logic                        valid_out,
    output logic                        done_out,

    output logic                        consume_in
);

    localparam int MAX_PADDED_WIDTH = MAX_IMG_WIDTH + 2*MAX_PAD;

    // RAMs: keep them ONLY in posedge blocks (no async reset sensitivity)
    (* ram_style = "block" *) logic [DATA_WIDTH-1:0] line0 [0:MAX_PADDED_WIDTH-1];
    (* ram_style = "block" *) logic [DATA_WIDTH-1:0] line1 [0:MAX_PADDED_WIDTH-1];

    // shift regs
    logic [DATA_WIDTH-1:0] s0_0, s0_1, s0_2;
    logic [DATA_WIDTH-1:0] s1_0, s1_1, s1_2;
    logic [DATA_WIDTH-1:0] s2_0, s2_1, s2_2;

    logic        running;
    logic [12:0] gen_row;
    logic [12:0] gen_col;

    logic [7:0]  pad_r;
    logic [12:0] eff_w_r, eff_h_r;

    logic [DATA_WIDTH-1:0] px_gen;
    logic                  need_real_px;
    logic                  advance;

    // ------------------------------------------------------------
    // Combinational generator decisions
    // ------------------------------------------------------------
    always_comb begin
        need_real_px =
            (gen_row >= pad_r) && (gen_row < (pad_r + img_height)) &&
            (gen_col >= pad_r) && (gen_col < (pad_r + img_width));

        advance    = running && ( !need_real_px || valid_in );
        px_gen     = need_real_px ? pixel_in : zp_in;
        consume_in = advance && need_real_px;
    end

    // ------------------------------------------------------------
    // Window output
    // ------------------------------------------------------------
    integer rr, cc;
    always_comb begin
        for (rr=0; rr<3; rr++)
            for (cc=0; cc<3; cc++)
                window[rr][cc] = '0;

        if (bypass_1x1) begin
            window[1][1] = s2_2;
        end else begin
            window[0][0] = s0_0; window[0][1] = s0_1; window[0][2] = s0_2;
            window[1][0] = s1_0; window[1][1] = s1_1; window[1][2] = s1_2;
            window[2][0] = s2_0; window[2][1] = s2_1; window[2][2] = s2_2;
        end
    end

    function automatic logic output_valid_check(
        input logic [12:0] gr,
        input logic [12:0] gc,
        input logic [7:0]  p,
        input logic [11:0] H,
        input logic [11:0] W,
        input logic        s2
    );
        logic signed [13:0] center_r, center_c;
        begin
            center_r = $signed({1'b0, gr}) - 14'sd1 - $signed({6'd0, p});
            center_c = $signed({1'b0, gc}) - 14'sd1 - $signed({6'd0, p});

            if (center_r >= 0 && center_r < $signed({2'd0, H}) &&
                center_c >= 0 && center_c < $signed({2'd0, W})) begin
                if (s2) output_valid_check = (center_r[0] == 1'b0) && (center_c[0] == 1'b0);
                else    output_valid_check = 1'b1;
            end else begin
                output_valid_check = 1'b0;
            end
        end
    endfunction

    // ------------------------------------------------------------
    // Control regs (can keep async reset here) - NO RAM access here
    // ------------------------------------------------------------
    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            running   <= 1'b0;
            gen_row   <= 13'd0;
            gen_col   <= 13'd0;
            valid_out <= 1'b0;
            done_out  <= 1'b0;

            pad_r     <= 8'd0;
            eff_w_r   <= 13'd0;
            eff_h_r   <= 13'd0;

        end else begin
            done_out <= 1'b0;

            if (start_in) begin
                running   <= 1'b1;
                gen_row   <= 13'd0;
                gen_col   <= 13'd0;
                valid_out <= 1'b0;

                pad_r   <= (bypass_1x1) ? 8'd0 : pad_top;
                eff_w_r <= {1'b0, img_width}  + (((bypass_1x1) ? 8'd0 : pad_top) << 1);
                eff_h_r <= {1'b0, img_height} + (((bypass_1x1) ? 8'd0 : pad_top) << 1);

            end else if (advance) begin

                if (bypass_1x1) begin
                    if (gen_row < img_height && gen_col < img_width) begin
                        if (stride_2) valid_out <= (gen_row[0]==1'b0 && gen_col[0]==1'b0);
                        else          valid_out <= 1'b1;
                    end else begin
                        valid_out <= 1'b0;
                    end
                end else begin
                    valid_out <= output_valid_check(gen_row, gen_col, pad_r, img_height, img_width, stride_2);
                end

                // advance coordinates
                if (gen_col == (eff_w_r - 1)) begin
                    gen_col <= 13'd0;
                    if (gen_row == (eff_h_r - 1)) begin
                        gen_row  <= 13'd0;
                        running  <= 1'b0;
                        done_out <= 1'b1;
                    end else begin
                        gen_row <= gen_row + 13'd1;
                    end
                end else begin
                    gen_col <= gen_col + 13'd1;
                end

            end else begin
                valid_out <= 1'b0;
            end
        end
    end

    // ------------------------------------------------------------
    // RAM + shift regs (posedge ONLY) - BRAM inference friendly
    // - We reset/clear shift regs using rst_n/start_in, but RAM arrays
    //   are NEVER written/cleared in reset branches.
    // ------------------------------------------------------------
    always_ff @(posedge clk) begin
        if (!rst_n) begin
            // reset shift regs only (safe; does not touch RAM arrays)
            s0_0 <= '0; s0_1 <= '0; s0_2 <= '0;
            s1_0 <= '0; s1_1 <= '0; s1_2 <= '0;
            s2_0 <= '0; s2_1 <= '0; s2_2 <= '0;
        end else if (start_in) begin
            // clear shift regs at start of run (matches your intent)
            s0_0 <= '0; s0_1 <= '0; s0_2 <= '0;
            s1_0 <= '0; s1_1 <= '0; s1_2 <= '0;
            s2_0 <= '0; s2_1 <= '0; s2_2 <= '0;
        end else if (advance) begin
            if (bypass_1x1) begin
                // 1x1: just capture generated pixel
                s2_2 <= px_gen;
            end else begin
                // bottom row shift
                s2_0 <= s2_1; s2_1 <= s2_2; s2_2 <= px_gen;

                // middle/top shift from RAM taps (read-before-write behavior)
                s1_0 <= s1_1; s1_1 <= s1_2; s1_2 <= line0[gen_col];
                s0_0 <= s0_1; s0_1 <= s0_2; s0_2 <= line1[gen_col];

                // update RAM (read-first style)
                // IMPORTANT: do NOT gate RAM with async reset; only here.
                line0[gen_col] <= px_gen;
                line1[gen_col] <= line0[gen_col];
            end
        end
    end

endmodule

`default_nettype wire
