`timescale 1ns / 1ps

module line_buffer_8x #(
    parameter int DATA_WIDTH     = 8,
    parameter int MAX_IMG_WIDTH  = 2048
)(
    input  logic                        clk,
    input  logic                        rst_n,

    input  logic                        start_in,      // 1-cycle pulse
    input  logic [11:0]                 img_width,     // Real Width
    input  logic [11:0]                 img_height,    // Real Height
    input  logic                        stride_2,
    input  logic                        bypass_1x1,
    input  logic [DATA_WIDTH-1:0]       zp_in,

    input  logic [8*DATA_WIDTH-1:0]     pixel_in,      // 8 pixels at a time
    input  logic                        valid_in,

    output logic [DATA_WIDTH-1:0]       window [0:7][2:0][2:0], // 8 sliding windows
    output logic [7:0]                  valid_out_vec,          // 8 valid flags
    output logic                        done_out,

    output logic                        consume_in
);

    // BRAM dimensions
    localparam int WORDS_PER_ROW = MAX_IMG_WIDTH / 8; 

    // BRAMs for storing rows
    (* ram_style = "block" *) logic [63:0] line0 [0:WORDS_PER_ROW-1];
    (* ram_style = "block" *) logic [63:0] line1 [0:WORDS_PER_ROW-1];

    logic [12:0] W_div_8;
    logic        running;
    logic [12:0] col, row;
    
    // Derived configurations
    always_ff @(posedge clk) begin
        if (start_in) begin
            W_div_8 <= (img_width + 12'd7) >> 3;
        end
    end

    // FSM
    logic need_dma;
    logic advance;

    always_comb begin
        need_dma   = (col < W_div_8) && (row < img_height);
        advance    = running && (!need_dma || valid_in);
        consume_in = advance && need_dma;
    end

    // Synchronous reset (was async): keeps col/row/done_out off the BRAM's
    // async-control path -> clears REQP-1839 on line0/line1. Behaviorally
    // identical in sim since rst_n is held low for many cycles before start_in.
    always_ff @(posedge clk) begin
        if (!rst_n) begin
            running  <= 1'b0;
            col      <= 13'd0;
            row      <= 13'd0;
            done_out <= 1'b0;
        end else if (start_in) begin
            running  <= 1'b1;
            col      <= 13'd0;
            row      <= 13'd0;
            done_out <= 1'b0;
        end else if (advance) begin
            if (col == W_div_8) begin
                col <= 13'd0;
                if (row == img_height) begin
                    running  <= 1'b0;
                    done_out <= 1'b1;
                end else begin
                    row <= row + 13'd1;
                end
            end else begin
                col <= col + 13'd1;
            end
        end else begin
            done_out <= 1'b0;
        end
    end

    // ==========================================
    // STAGE 0: BRAM Read/Write Issue
    // ==========================================
    logic [12:0] s1_col, s1_row;
    logic        s1_valid;
    logic [63:0] s1_px_in;
    
    logic [63:0] line0_rd, line1_rd;
    logic [63:0] r2_in_raw;

    always_comb r2_in_raw = need_dma ? pixel_in : {8{zp_in}};

    logic toggle;
    // Synchronous reset (was async): toggle drives bank write-select / parity
    // mux; keeping it off the BRAM async-control path clears REQP-1839.
    always_ff @(posedge clk) begin
        if (!rst_n) toggle <= 1'b0;
        else if (start_in) toggle <= 1'b0;
        else if (advance && col == W_div_8) toggle <= ~toggle;
    end

    always_ff @(posedge clk) begin
        if (advance) begin
            // Read
            line0_rd <= line0[col];
            line1_rd <= line1[col];
            
            // Write
            if (col < W_div_8) begin
                if (toggle == 1'b0) line0[col] <= r2_in_raw;
                else                line1[col] <= r2_in_raw;
            end

            s1_col   <= col;
            s1_row   <= row;
            s1_px_in <= r2_in_raw;
        end
        
        if (!rst_n || start_in) s1_valid <= 1'b0;
        else if (advance)       s1_valid <= 1'b1;
        else                    s1_valid <= 1'b0;
    end

    // ==========================================
    // STAGE 1: BRAM Data Arrives -> Shift Regs
    // ==========================================
    logic [63:0] R0_in, R1_in, R2_in;
    logic [63:0] R0_prev, R1_prev, R2_prev;
    logic [7:0]  R0_left, R1_left, R2_left;

    logic [12:0] s2_col, s2_row;
    logic        s2_valid;

    always_comb begin
        if (s1_row[0] == 1'b0) begin
            // Even rows: line1 has oldest (R0), line0 has newest written (which we don't read directly)
            R0_in = line1_rd;
            R1_in = line0_rd;
            R2_in = s1_px_in;
        end else begin
            // Odd rows
            R0_in = line0_rd;
            R1_in = line1_rd;
            R2_in = s1_px_in;
        end
    end

    always_ff @(posedge clk) begin
        if (s1_valid) begin
            R0_prev <= R0_in;
            R1_prev <= R1_in;
            R2_prev <= R2_in;

            R0_left <= R0_prev[63:56];
            R1_left <= R1_prev[63:56];
            R2_left <= R2_prev[63:56];

            s2_col <= s1_col;
            s2_row <= s1_row;
        end

        if (!rst_n || start_in) s2_valid <= 1'b0;
        else                    s2_valid <= s1_valid;
    end

    // ==========================================
    // STAGE 2: Window Extraction & Padding
    // ==========================================
    // ==========================================
    // STAGE 2: Window Extraction & Padding (combinational)
    //   Renamed to _comb to allow Stage 3 registration below.
    // ==========================================
    logic signed [13:0] Y;
    always_comb Y = bypass_1x1 ? $signed({1'b0, s2_row}) : ($signed({1'b0, s2_row}) - 14'sd1);

    logic is_y_valid [0:2];
    always_comb begin
        is_y_valid[0] = bypass_1x1 ? 1'b0 : (Y - 1 >= 0) && (Y - 1 < $signed({2'b0, img_height}));
        is_y_valid[1] = (Y >= 0) && (Y < $signed({2'b0, img_height}));
        is_y_valid[2] = bypass_1x1 ? 1'b0 : (Y + 1 >= 0) && (Y + 1 < $signed({2'b0, img_height}));
    end

    logic signed [13:0] X_byte [0:9];
    logic is_x_valid [0:9];
    always_comb begin
        for (int k = 0; k < 10; k++) begin
            X_byte[k]    = $signed({1'b0, s2_col}) * 8 + k - 1;
            is_x_valid[k] = (X_byte[k] >= 0) && (X_byte[k] < $signed({2'b0, img_width}));
        end
    end

    logic [7:0] raw_r0 [0:9];
    logic [7:0] raw_r1 [0:9];
    logic [7:0] raw_r2 [0:9];
    always_comb begin
        raw_r0[0] = R0_left; raw_r1[0] = R1_left; raw_r2[0] = R2_left;
        for (int i = 0; i < 8; i++) begin
            raw_r0[i+1] = R0_prev[i*8 +: 8];
            raw_r1[i+1] = R1_prev[i*8 +: 8];
            raw_r2[i+1] = R2_prev[i*8 +: 8];
        end
        raw_r0[9] = R0_in[7:0]; raw_r1[9] = R1_in[7:0]; raw_r2[9] = R2_in[7:0];
    end

    logic [7:0] masked_r0 [0:9];
    logic [7:0] masked_r1 [0:9];
    logic [7:0] masked_r2 [0:9];
    always_comb begin
        for (int k = 0; k < 10; k++) begin
            masked_r0[k] = (is_y_valid[0] && is_x_valid[k]) ? raw_r0[k] : zp_in;
            masked_r1[k] = (is_y_valid[1] && is_x_valid[k]) ? raw_r1[k] : zp_in;
            masked_r2[k] = (is_y_valid[2] && is_x_valid[k]) ? raw_r2[k] : zp_in;
        end
    end

    // Combinational window / valid (Stage 2 outputs, NOT yet registered)
    logic [DATA_WIDTH-1:0] window_comb [0:7][2:0][2:0];
    logic [7:0]            valid_comb;

    always_comb begin
        logic center_x_valid, center_y_valid, is_x_even, is_y_even, stride_ok;
        for (int i = 0; i < 8; i++) begin
            if (bypass_1x1) begin
                window_comb[i][0][0] = '0; window_comb[i][0][1] = '0; window_comb[i][0][2] = '0;
                window_comb[i][1][0] = '0; window_comb[i][1][1] = masked_r1[i+1]; window_comb[i][1][2] = '0;
                window_comb[i][2][0] = '0; window_comb[i][2][1] = '0; window_comb[i][2][2] = '0;
            end else begin
                window_comb[i][0][0] = masked_r0[i];   window_comb[i][0][1] = masked_r0[i+1]; window_comb[i][0][2] = masked_r0[i+2];
                window_comb[i][1][0] = masked_r1[i];   window_comb[i][1][1] = masked_r1[i+1]; window_comb[i][1][2] = masked_r1[i+2];
                window_comb[i][2][0] = masked_r2[i];   window_comb[i][2][1] = masked_r2[i+1]; window_comb[i][2][2] = masked_r2[i+2];
            end
            center_x_valid = (X_byte[i+1] >= 0) && (X_byte[i+1] < $signed({2'b0, img_width}));
            center_y_valid = (Y >= 0) && (Y < $signed({2'b0, img_height}));
            is_x_even      = (X_byte[i+1][0] == 1'b0);
            is_y_even      = (Y[0] == 1'b0);
            stride_ok      = stride_2 ? (is_x_even && is_y_even) : 1'b1;
            valid_comb[i]  = s2_valid && center_x_valid && center_y_valid && stride_ok;
        end
    end

    // ==========================================
    // STAGE 3: Register window + valid_out_vec
    //   Breaks the 10-CARRY4 path:
    //     s2_row_reg → Y → is_y_valid comparators → masked_r MUX → DSP-A
    //   After this register, the DSP AREG sees clean fabric-FF data,
    //   no carry chain in the setup path.
    // ==========================================
    always_ff @(posedge clk) begin
        if (!rst_n || start_in) begin
            valid_out_vec <= 8'd0;
        end else begin
            valid_out_vec <= valid_comb;
            for (int i = 0; i < 8; i++)
                for (int r = 0; r < 3; r++)
                    for (int c = 0; c < 3; c++)
                        window[i][r][c] <= window_comb[i][r][c];
        end
    end

endmodule
`default_nettype wire
