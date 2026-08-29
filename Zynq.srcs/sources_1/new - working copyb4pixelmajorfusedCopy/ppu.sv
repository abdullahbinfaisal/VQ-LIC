`timescale 1ns / 1ps

module ppu #(
    parameter int DATA_WIDTH = 8,
    parameter int ACC_WIDTH  = 32
)(
    input  logic                    clk,
    input  logic                    rst_n,

    input  logic                    mode_residual,
    input  logic                    relu_en,

    input  logic [31:0]             mult_conv,
    input  logic [7:0]              shift_conv,
    input  logic signed [31:0]      bias_in,

    input  logic [31:0]             mult_res_a,
    input  logic [31:0]             mult_res_b,
    input  logic [7:0]              shift_res,

    input  logic [7:0]              zp_out,
    input  logic [7:0]              zp_in_a,
    input  logic [7:0]              zp_in_b,

    input  logic                    valid_in,
    input  logic signed [31:0]      conv_acc_in,

    input  logic [7:0]              res_a_in,
    input  logic [7:0]              res_b_in,

    output logic [7:0]              pixel_out,
    output logic                    valid_out
);

    // ============================================================
    // STAGE 1: Multiply
    //   Conv:     acc_biased = conv_acc_in + bias_in
    //             conv_prod_wide = acc_biased * mult_conv   (33x32)
    //   Residual: diff -> prod_a, prod_b -> sum             (9x32 x 2)
    // ============================================================
    logic signed [32:0]  acc_biased;
    (* use_dsp = "yes" *) logic signed [67:0] conv_prod_wide;

    logic signed [8:0]   diff_a, diff_b;
    (* use_dsp = "yes" *) logic signed [41:0] prod_a_wide, prod_b_wide;
    logic signed [67:0]  sum_res_wide;

    always_comb begin
        // Conv multiply
        acc_biased     = conv_acc_in + bias_in;
        conv_prod_wide = acc_biased * $signed({1'b0, mult_conv});

        // Residual multiply
        diff_a       = $signed({1'b0, res_a_in}) - $signed({1'b0, zp_in_a});
        diff_b       = $signed({1'b0, res_b_in}) - $signed({1'b0, zp_in_b});
        prod_a_wide  = $signed(diff_a) * $signed({1'b0, mult_res_a});
        prod_b_wide  = $signed(diff_b) * $signed({1'b0, mult_res_b});
        sum_res_wide = $signed(prod_a_wide) + $signed(prod_b_wide);
    end

    // ---- Stage 1 pipeline registers ----
    logic                valid_s1;
    logic signed [67:0]  conv_prod_wide_s1;
    logic signed [67:0]  sum_res_wide_s1;
    logic [7:0]          shift_conv_s1;
    logic [7:0]          shift_res_s1;
    logic                mode_residual_s1;
    logic                relu_en_s1;
    logic [7:0]          zp_out_s1;

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            valid_s1          <= 1'b0;
            conv_prod_wide_s1 <= '0;
            sum_res_wide_s1   <= '0;
            shift_conv_s1     <= '0;
            shift_res_s1      <= '0;
            mode_residual_s1  <= 1'b0;
            relu_en_s1        <= 1'b0;
            zp_out_s1         <= '0;
        end else begin
            valid_s1          <= valid_in;
            conv_prod_wide_s1 <= conv_prod_wide;
            sum_res_wide_s1   <= sum_res_wide;
            shift_conv_s1     <= shift_conv;
            shift_res_s1      <= shift_res;
            mode_residual_s1  <= mode_residual;
            relu_en_s1        <= relu_en;
            zp_out_s1         <= zp_out;
        end
    end

    // ============================================================
    // STAGE 2 (NEW): Absolute value + mode selection
    //   Computes abs(x) for both paths (68-bit negate is the big
    //   carry chain that dominated the old critical path).
    //   Also selects conv vs residual path early to halve
    //   downstream logic.
    // ============================================================
    logic [67:0] conv_ax, res_ax;
    logic        conv_sign, res_sign;
    logic [67:0] sel_ax;
    logic        sel_sign;
    logic [7:0]  sel_shift;

    always_comb begin
        conv_sign = conv_prod_wide_s1[67];
        conv_ax   = conv_sign ? (~conv_prod_wide_s1 + 68'd1) : conv_prod_wide_s1;

        res_sign  = sum_res_wide_s1[67];
        res_ax    = res_sign ? (~sum_res_wide_s1 + 68'd1) : sum_res_wide_s1;

        sel_ax    = mode_residual_s1 ? res_ax    : conv_ax;
        sel_sign  = mode_residual_s1 ? res_sign  : conv_sign;
        sel_shift = mode_residual_s1 ? shift_res_s1 : shift_conv_s1;
    end

    // ---- Stage 2 pipeline registers ----
    logic                valid_s2;
    logic [67:0]         sel_ax_s2;
    logic                sel_sign_s2;
    logic [7:0]          sel_shift_s2;
    logic                relu_en_s2;
    logic [7:0]          zp_out_s2;

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            valid_s2     <= 1'b0;
            sel_ax_s2    <= '0;
            sel_sign_s2  <= 1'b0;
            sel_shift_s2 <= '0;
            relu_en_s2   <= 1'b0;
            zp_out_s2    <= '0;
        end else begin
            valid_s2     <= valid_s1;
            sel_ax_s2    <= sel_ax;
            sel_sign_s2  <= sel_sign;
            sel_shift_s2 <= sel_shift;
            relu_en_s2   <= relu_en_s1;
            zp_out_s2    <= zp_out_s1;
        end
    end

    // ============================================================
    // STAGE 3: Barrel shift + ties-to-even rounding + sign
    //          restore + zero-point addition
    // ============================================================
    logic [67:0]        q3, mask3, r3, half3;
    logic               inc3;
    logic signed [31:0] scaled_val;
    logic signed [31:0] pre_clamp;
    int                 sh3;

    always_comb begin
        sh3 = sel_shift_s2;

        // defaults
        q3 = '0; mask3 = '0; r3 = '0; half3 = '0;
        inc3 = 1'b0;
        scaled_val = '0;
        pre_clamp  = '0;

        if (sh3 <= 0) begin
            // No shift: value used directly
            scaled_val = sel_sign_s2 ? -$signed(sel_ax_s2[31:0])
                                     :  $signed(sel_ax_s2[31:0]);
            pre_clamp  = scaled_val + $signed({24'd0, zp_out_s2});
        end else if (sh3 >= 68) begin
            // Shifted entirely away
            pre_clamp = $signed({24'd0, zp_out_s2});
        end else begin
            q3    = sel_ax_s2 >> sh3;
            mask3 = (68'd1 << sh3) - 68'd1;
            r3    = sel_ax_s2 & mask3;
            half3 = (68'd1 << (sh3 - 1));

            inc3 = (r3 > half3) || ((r3 == half3) && q3[0]);
            if (inc3) q3 = q3 + 68'd1;

            // Sign restore + zp add (merged into one add/sub)
            if (sel_sign_s2)
                pre_clamp = $signed({24'd0, zp_out_s2}) - $signed(q3[31:0]);
            else
                pre_clamp = $signed(q3[31:0]) + $signed({24'd0, zp_out_s2});
        end
    end

    // ---- Stage 3 pipeline registers ----
    logic                valid_s3;
    logic signed [31:0]  pre_clamp_s3;
    logic                relu_en_s3;
    logic [7:0]          zp_out_s3;

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            valid_s3     <= 1'b0;
            pre_clamp_s3 <= '0;
            relu_en_s3   <= 1'b0;
            zp_out_s3    <= '0;
        end else begin
            valid_s3     <= valid_s2;
            pre_clamp_s3 <= pre_clamp;
            relu_en_s3   <= relu_en_s2;
            zp_out_s3    <= zp_out_s2;
        end
    end

    // ============================================================
    // STAGE 4: Clamp [0,255] + ReLU -> output register
    // ============================================================
    logic [7:0] clamped_val;
    logic       neg;
    logic       gt255;

    always_comb begin
        neg   = pre_clamp_s3[31];
        gt255 = (~pre_clamp_s3[31]) && (|pre_clamp_s3[30:8]);

        if (neg)         clamped_val = 8'd0;
        else if (gt255)  clamped_val = 8'd255;
        else             clamped_val = pre_clamp_s3[7:0];

        if (relu_en_s3 && (clamped_val < zp_out_s3))
            clamped_val = zp_out_s3;
    end

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            valid_out <= 1'b0;
            pixel_out <= 8'd0;
        end else begin
            valid_out <= valid_s3;
            if (valid_s3) pixel_out <= clamped_val;
        end
    end

endmodule

`default_nettype wire
