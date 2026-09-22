`timescale 1ns / 1ps

module ppu #(
    parameter int DATA_WIDTH = 8,
    parameter int ACC_WIDTH  = 32
)(
    input  logic                    clk,
    input  logic                    rst_n,

    input  logic                    relu_en,

    input  logic [23:0]             mult_conv,  // capped 24-bit: saves 2 DSPs per PPU
    input  logic [7:0]              shift_conv,
    input  logic signed [31:0]      bias_in,

    input  logic [7:0]              zp_out,

    input  logic                    valid_in,
    input  logic signed [31:0]      conv_acc_in,

    output logic [7:0]              pixel_out,
    output logic                    valid_out
);

    // ============================================================
    // STAGE 1: Multiply
    //   Conv:     acc_biased = conv_acc_in + bias_in
    //             conv_prod_wide = acc_biased * mult_conv   (33x32)
    // ============================================================
    // ============================================================
    // STAGE 0: Pre-Adder (Combines conv_acc + bias)
    //   Registers inputs to multiplier to map perfectly to DSP
    //   AREG/BREG, breaking the critical path!
    // ============================================================
    // ------------------------------------------------------------
    // DECLARATION INITIALISERS (2026-08-08) -- the `= '0` on every datapath
    // register below.
    //
    // These registers deliberately have NO reset (only the valid_* chain does),
    // to keep them out of the async-reset fanout cone and avoid recovery-time
    // violations. The cost was that SIMULATION started them X, and because this
    // pipeline is not valid-gated on the data path that X reached every output:
    // a golden-data check read 51,200/51,200 lane-values unknown, with ZERO
    // numeric mismatches -- i.e. no real error, but no verifiable data either.
    // That is why this design has never had a data check (§21).
    //
    // Xilinx FPGAs honour SystemVerilog declaration initialisers as the flop's
    // power-up state (they become the INIT attribute), which is exactly what the
    // silicon already did implicitly. So this is SYNTHESISABLE, changes NOTHING
    // on hardware, and makes simulation match the device.
    //
    // Preferred over the alternatives: xelab --initreg is unsupported in this
    // xsim, and testbench deposits silently DO NOTHING for signals inside
    // generate blocks (all the PPU instances are), which was verified.
    //
    // ⚠️ ppu.sv is declared by BOTH IPs, and the DW IP declares it TWICE
    // (ip_repo/dw_fused_axi_1.0/src/ and the live tree). Keep both byte-identical
    // -- see the double-declaration trap in §27.
    // ------------------------------------------------------------
    logic                valid_s0;
    logic signed [32:0]  acc_biased_s0 = '0;
    logic [23:0]         mult_conv_s0 = '0;
    logic                relu_en_s0 = '0;
    logic [7:0]          shift_conv_s0 = '0;
    logic [7:0]          zp_out_s0 = '0;

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            valid_s0 <= 1'b0;
        end else begin
            valid_s0 <= valid_in;
        end
    end

    always_ff @(posedge clk) begin
        // Conv inputs
        acc_biased_s0    <= conv_acc_in + bias_in;
        mult_conv_s0     <= mult_conv;
        shift_conv_s0    <= shift_conv;

        // Control
        relu_en_s0       <= relu_en;
        zp_out_s0        <= zp_out;
    end

    // ---- Stage 1a pipeline registers (MREG) ----
    logic                valid_s1a;
    (* use_dsp = "yes" *) logic signed [57:0]  conv_prod_wide_s1a = '0;
    logic [7:0]          shift_conv_s1a = '0;
    logic                relu_en_s1a = '0;
    logic [7:0]          zp_out_s1a = '0;

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            valid_s1a         <= 1'b0;
        end else begin
            valid_s1a         <= valid_s0;
        end
    end

    always_ff @(posedge clk) begin
        conv_prod_wide_s1a <= acc_biased_s0 * $signed({1'b0, mult_conv_s0});
        shift_conv_s1a     <= shift_conv_s0;
        relu_en_s1a        <= relu_en_s0;
        zp_out_s1a         <= zp_out_s0;
    end

    // ---- Stage 1b pipeline registers (PREG) ----
    logic                valid_s1;
    logic signed [57:0]  conv_prod_wide_s1 = '0;
    logic [7:0]          shift_conv_s1 = '0;
    logic                relu_en_s1 = '0;
    logic [7:0]          zp_out_s1 = '0;

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            valid_s1          <= 1'b0;
        end else begin
            valid_s1          <= valid_s1a;
        end
    end

    always_ff @(posedge clk) begin
        conv_prod_wide_s1 <= conv_prod_wide_s1a;
        shift_conv_s1     <= shift_conv_s1a;
        relu_en_s1        <= relu_en_s1a;
        zp_out_s1         <= zp_out_s1a;
    end

    // ============================================================
    // STAGE 2 (NEW): Absolute value
    //   Computes abs(x) (68-bit negate is the big carry chain that
    //   dominated the old critical path).
    // ============================================================
    logic [57:0] conv_ax;  // abs of 58-bit conv product
    logic        conv_sign;

    always_comb begin
        conv_sign = conv_prod_wide_s1[57];
        conv_ax   = conv_sign ? (~conv_prod_wide_s1 + 58'd1) : conv_prod_wide_s1;
    end

    // ---- Stage 2 pipeline registers ----
    logic                valid_s2;
    logic [57:0]         sel_ax_s2 = '0;
    logic                sel_sign_s2 = '0;
    logic [7:0]          sel_shift_s2 = '0;
    logic                relu_en_s2 = '0;
    logic [7:0]          zp_out_s2 = '0;

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            valid_s2     <= 1'b0;
        end else begin
            valid_s2     <= valid_s1;
        end
    end

    always_ff @(posedge clk) begin
        sel_ax_s2    <= conv_ax;
        sel_sign_s2  <= conv_sign;
        sel_shift_s2 <= shift_conv_s1;
        relu_en_s2   <= relu_en_s1;
        zp_out_s2    <= zp_out_s1;
    end

    // ============================================================
    // STAGE 3a-pre: Barrel shift (variable right-shift of 58-bit value)
    //   Computes q3_pre = sel_ax_s2 >> sh3 and the rounding terms.
    //   Does NOT apply the +1 increment ??? that is deferred to Stage 3a
    //   so each stage has at most a ~29-bit carry chain.
    // ============================================================
    logic [57:0]  q3_pre;       // shifted quotient (before rounding increment)
    logic         inc3_pre;     // 1 if we need to round up
    logic         shift_zero_pre;
    logic         shift_gone_pre;

    always_comb begin : comb_3a_pre
        int sh3;
        logic [57:0] mask3, r3, half3;

        sh3 = sel_shift_s2;

        // defaults
        q3_pre       = '0;
        mask3        = '0;
        r3           = '0;
        half3        = '0;
        inc3_pre     = 1'b0;
        shift_zero_pre = 1'b0;
        shift_gone_pre = 1'b0;

        if (sh3 <= 0) begin
            shift_zero_pre = 1'b1;
            q3_pre         = sel_ax_s2;   // no shift; pass through
        end else if (sh3 >= 58) begin
            shift_gone_pre = 1'b1;
        end else begin
            q3_pre = sel_ax_s2 >> sh3;
            mask3  = (58'd1 << sh3) - 58'd1;
            r3     = sel_ax_s2 & mask3;
            half3  = (58'd1 << (sh3 - 1));
            inc3_pre = (r3 > half3) || ((r3 == half3) && q3_pre[0]);
        end
    end

    // ---- Stage 3a-pre pipeline registers ----
    logic                valid_s3apre;
    logic [57:0]         q3_pre_r = '0;
    logic                inc3_pre_r = '0;
    logic                shift_zero_pre_r = '0;
    logic                shift_gone_pre_r = '0;
    logic                sel_sign_s3apre = '0;
    logic                relu_en_s3apre = '0;
    logic [7:0]          zp_out_s3apre = '0;

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) valid_s3apre <= 1'b0;
        else        valid_s3apre <= valid_s2;
    end

    always_ff @(posedge clk) begin
        q3_pre_r        <= q3_pre;
        inc3_pre_r      <= inc3_pre;
        shift_zero_pre_r <= shift_zero_pre;
        shift_gone_pre_r <= shift_gone_pre;
        sel_sign_s3apre  <= sel_sign_s2;
        relu_en_s3apre   <= relu_en_s2;
        zp_out_s3apre    <= zp_out_s2;
    end

    // ============================================================
    // STAGE 3a: Apply rounding increment + truncate to 32 bits
    //   Input is already shifted (q3_pre_r); only a narrow +1 adder
    //   with carry-save is needed here ??? breaks the 58-bit chain.
    // ============================================================
    logic [31:0]  rounded_q;
    logic         shift_zero;
    logic         shift_gone;

    always_comb begin : comb_3a
        logic [57:0] q3_rounded;
        q3_rounded  = q3_pre_r + {57'd0, inc3_pre_r};  // narrow +1
        shift_zero  = shift_zero_pre_r;
        shift_gone  = shift_gone_pre_r;

        if (shift_zero_pre_r)
            rounded_q = q3_pre_r[31:0];   // no shift: pass low 32 bits
        else if (shift_gone_pre_r)
            rounded_q = '0;               // fully shifted away
        else
            rounded_q = q3_rounded[31:0];
    end

    // ---- Stage 3a pipeline registers ----
    logic                valid_s3a;
    logic [31:0]         rounded_q_s3a = '0;
    logic                shift_zero_s3a = '0;
    logic                shift_gone_s3a = '0;
    logic                sel_sign_s3a = '0;
    logic                relu_en_s3a = '0;
    logic [7:0]          zp_out_s3a = '0;

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            valid_s3a      <= 1'b0;
        end else begin
            valid_s3a      <= valid_s3apre;
        end
    end

    always_ff @(posedge clk) begin
        rounded_q_s3a  <= rounded_q;
        shift_zero_s3a <= shift_zero;
        shift_gone_s3a <= shift_gone;
        sel_sign_s3a   <= sel_sign_s3apre;
        relu_en_s3a    <= relu_en_s3apre;
        zp_out_s3a     <= zp_out_s3apre;
    end

    // ============================================================
    // STAGE 3b: Sign restore + zero-point addition
    //   Uses registered rounded_q from Stage 3a.
    // ============================================================
    logic signed [31:0] pre_clamp;

    always_comb begin
        pre_clamp = '0;

        if (shift_gone_s3a) begin
            pre_clamp = $signed({24'd0, zp_out_s3a});
        end else if (shift_zero_s3a) begin
            if (sel_sign_s3a)
                pre_clamp = $signed({24'd0, zp_out_s3a}) - $signed(rounded_q_s3a);
            else
                pre_clamp = $signed(rounded_q_s3a) + $signed({24'd0, zp_out_s3a});
        end else begin
            if (sel_sign_s3a)
                pre_clamp = $signed({24'd0, zp_out_s3a}) - $signed(rounded_q_s3a);
            else
                pre_clamp = $signed(rounded_q_s3a) + $signed({24'd0, zp_out_s3a});
        end
    end

    // ---- Stage 3b pipeline registers ----
    logic                valid_s3;
    logic signed [31:0]  pre_clamp_s3 = '0;
    logic                relu_en_s3 = '0;
    logic [7:0]          zp_out_s3 = '0;

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            valid_s3     <= 1'b0;
        end else begin
            valid_s3     <= valid_s3a;
        end
    end

    always_ff @(posedge clk) begin
        pre_clamp_s3 <= pre_clamp;
        relu_en_s3   <= relu_en_s3a;
        zp_out_s3    <= zp_out_s3a;
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
        end else begin
            valid_out <= valid_s3;
        end
    end

    always_ff @(posedge clk) begin
        if (valid_s3) pixel_out <= clamped_val;
    end

endmodule

`default_nettype wire
