`timescale 1ns / 1ps

module conv_mac_array #(
    parameter int DATA_WIDTH = 8,
    parameter int ACC_WIDTH  = 32
)(
    input  logic                    clk,
    input  logic                    rst_n,

    // Configuration
    input  logic                    is_depthwise,
    input  logic [DATA_WIDTH-1:0]   zp_in,

    // Data Inputs
    input  logic [DATA_WIDTH-1:0]   window [2:0][2:0],
    input  logic                    valid_in,

    // Weights
    input  logic signed [DATA_WIDTH-1:0] weights [2:0][2:0],

    // Partial Sum (used for PW accumulation)
    input  logic signed [ACC_WIDTH-1:0]  psum_in,
    input  logic                         psum_clear,

    // Output
    output logic signed [ACC_WIDTH-1:0]  mac_out,
    output logic                         valid_out
);

    // signed zp once
    logic signed [8:0] zp_s;
    always_comb zp_s = $signed({1'b0, zp_in});

    // ================================================================
    // Stage 1 registers: DSP products computed directly in always_ff
    //   Maps to DSP48 with MREG=1 (registered multiply output)
    // ================================================================
    (* dont_touch = "yes" *) logic signed [17:0] p00_r;
    (* dont_touch = "yes" *) logic signed [17:0] p01_r;
    (* dont_touch = "yes" *) logic signed [17:0] p02_r;
    (* dont_touch = "yes" *) logic signed [17:0] p10_r;
    (* dont_touch = "yes" *) logic signed [17:0] p11_r;
    (* dont_touch = "yes" *) logic signed [17:0] p12_r;
    (* dont_touch = "yes" *) logic signed [17:0] p20_r;
    (* dont_touch = "yes" *) logic signed [17:0] p21_r;
    (* dont_touch = "yes" *) logic signed [17:0] p22_r;
    (* dont_touch = "yes" *) logic               valid_s1;
    (* dont_touch = "yes" *) logic               is_dw_s1;
    (* dont_touch = "yes" *) logic               psum_clear_s1;
    (* dont_touch = "yes" *) logic signed [ACC_WIDTH-1:0] psum_in_s1;

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            p00_r <= '0; p01_r <= '0; p02_r <= '0;
            p10_r <= '0; p11_r <= '0; p12_r <= '0;
            p20_r <= '0; p21_r <= '0; p22_r <= '0;
            valid_s1      <= 1'b0;
            is_dw_s1      <= 1'b0;
            psum_clear_s1 <= 1'b0;
            psum_in_s1    <= '0;
        end else begin
            valid_s1      <= valid_in;
            is_dw_s1      <= is_depthwise;
            psum_clear_s1 <= psum_clear;
            psum_in_s1    <= psum_in;

            if (is_depthwise) begin
                // 3x3 depthwise: all taps (subtract + multiply -> DSP MREG)
                p00_r <= ($signed({1'b0,window[0][0]}) - zp_s) * weights[0][0];
                p01_r <= ($signed({1'b0,window[0][1]}) - zp_s) * weights[0][1];
                p02_r <= ($signed({1'b0,window[0][2]}) - zp_s) * weights[0][2];

                p10_r <= ($signed({1'b0,window[1][0]}) - zp_s) * weights[1][0];
                p11_r <= ($signed({1'b0,window[1][1]}) - zp_s) * weights[1][1];
                p12_r <= ($signed({1'b0,window[1][2]}) - zp_s) * weights[1][2];

                p20_r <= ($signed({1'b0,window[2][0]}) - zp_s) * weights[2][0];
                p21_r <= ($signed({1'b0,window[2][1]}) - zp_s) * weights[2][1];
                p22_r <= ($signed({1'b0,window[2][2]}) - zp_s) * weights[2][2];
            end else begin
                // 1x1 pointwise: ONLY center tap
                p00_r <= '0; p01_r <= '0; p02_r <= '0;
                p10_r <= '0;              p12_r <= '0;
                p20_r <= '0; p21_r <= '0; p22_r <= '0;
                p11_r <= ($signed({1'b0,window[1][1]}) - zp_s) * weights[1][1];
            end
        end
    end

    // ================================================================
    // Stage 2: balanced adder tree from registered products
    // ================================================================
    logic signed [ACC_WIDTH-1:0] s0, s1, s2, s3, s4;
    logic signed [ACC_WIDTH-1:0] tree_sum;
    logic signed [ACC_WIDTH-1:0] mac_next;

    always_comb begin
        // defaults (avoid latch inference)
        s0 = '0; s1 = '0; s2 = '0; s3 = '0; s4 = '0;
        tree_sum = '0;
        mac_next = '0;

        if (is_dw_s1) begin
            s0 = $signed(p00_r) + $signed(p01_r);
            s1 = $signed(p02_r) + $signed(p10_r);
            s2 = $signed(p11_r) + $signed(p12_r);
            s3 = $signed(p20_r) + $signed(p21_r);
            s4 = $signed(p22_r);

            tree_sum = ( (s0 + s1) + (s2 + s3) ) + s4;
            mac_next = tree_sum;  // DW ignores psum
        end else begin
            tree_sum = $signed(p11_r);
            mac_next = tree_sum + (psum_clear_s1 ? '0 : psum_in_s1);
        end
    end

    // Stage 2 output register
    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            mac_out   <= '0;
            valid_out <= 1'b0;
        end else begin
            valid_out <= valid_s1;
            if (valid_s1) mac_out <= mac_next;
        end
    end

endmodule
