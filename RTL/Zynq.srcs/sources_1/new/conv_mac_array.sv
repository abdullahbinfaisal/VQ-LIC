`timescale 1ns / 1ps

module conv_mac_array #(
    parameter int DATA_WIDTH = 8,
    parameter int ACC_WIDTH  = 32,
    parameter int USE_DSP    = 1
)(
    input  logic                    clk,
    input  logic                    rst_n,

    // Configuration
    input  logic [DATA_WIDTH-1:0]   zp_in,

    // Data Inputs
    input  logic [DATA_WIDTH-1:0]   window [2:0][2:0],
    input  logic                    valid_in,

    // Weights
    input  logic signed [DATA_WIDTH-1:0] weights [2:0][2:0],

    // Output
    output logic signed [ACC_WIDTH-1:0]  mac_out,
    output logic                         valid_out
);

    // signed zp once
    logic signed [8:0] zp_s;
    always_comb zp_s = $signed({1'b0, zp_in});

    // Flatten the 3x3 inputs to 1D arrays for easier generate loops
    logic [DATA_WIDTH-1:0]        flat_window [0:8];
    logic signed [DATA_WIDTH-1:0] flat_weights [0:8];

    always_comb begin
        for (int r = 0; r < 3; r++) begin
            for (int c = 0; c < 3; c++) begin
                flat_window[r*3 + c]  = window[r][c];
                flat_weights[r*3 + c] = weights[r][c];
            end
        end
    end

    // ================================================================
    // Input Skewing Shift Registers
    // ================================================================
    logic [DATA_WIDTH-1:0]        skewed_window [0:8];
    logic signed [DATA_WIDTH-1:0] skewed_weights [0:8];

    generate
        for (genvar i = 0; i < 9; i++) begin : G_SKEW
            if (i == 0) begin
                assign skewed_window[0]  = flat_window[0];
                assign skewed_weights[0] = flat_weights[0];
            end else begin
                // Shift register of depth 'i'
                logic [DATA_WIDTH-1:0]        win_sr [1:i];
                logic signed [DATA_WIDTH-1:0] wgt_sr [1:i];

                always_ff @(posedge clk) begin
                    win_sr[1] <= flat_window[i];
                    wgt_sr[1] <= flat_weights[i];
                    for (int j = 2; j <= i; j++) begin
                        win_sr[j] <= win_sr[j-1];
                        wgt_sr[j] <= wgt_sr[j-1];
                    end
                end

                assign skewed_window[i]  = win_sr[i];
                assign skewed_weights[i] = wgt_sr[i];
            end
        end
    endgenerate

    localparam int CAS_WIDTH = (USE_DSP != 0) ? 48 : ACC_WIDTH;

    // ================================================================
    // DSP P-Cascade Chain
    // ================================================================
    // A 48-bit register maps nicely to the P-register of the DSP48E1.
    // If USE_DSP is 0, we scale down the width to CAS_WIDTH (ACC_WIDTH) to save fabric logic.
    (* use_dsp = (USE_DSP != 0) ? "yes" : "no" *) logic signed [CAS_WIDTH-1:0] p_cas [0:8];

    always_ff @(posedge clk) begin
        // Stage 0: pure multiply
        p_cas[0] <= ($signed({1'b0, skewed_window[0]}) - zp_s) * skewed_weights[0];

        // Stages 1-8: multiply-accumulate with previous DSP's P_out
        p_cas[1] <= p_cas[0] + ($signed({1'b0, skewed_window[1]}) - zp_s) * skewed_weights[1];
        p_cas[2] <= p_cas[1] + ($signed({1'b0, skewed_window[2]}) - zp_s) * skewed_weights[2];
        p_cas[3] <= p_cas[2] + ($signed({1'b0, skewed_window[3]}) - zp_s) * skewed_weights[3];
        p_cas[4] <= p_cas[3] + ($signed({1'b0, skewed_window[4]}) - zp_s) * skewed_weights[4];
        p_cas[5] <= p_cas[4] + ($signed({1'b0, skewed_window[5]}) - zp_s) * skewed_weights[5];
        p_cas[6] <= p_cas[5] + ($signed({1'b0, skewed_window[6]}) - zp_s) * skewed_weights[6];
        p_cas[7] <= p_cas[6] + ($signed({1'b0, skewed_window[7]}) - zp_s) * skewed_weights[7];
        p_cas[8] <= p_cas[7] + ($signed({1'b0, skewed_window[8]}) - zp_s) * skewed_weights[8];
    end

    // ================================================================
    // Output assignment
    // ================================================================
    // The final accumulated result is ready after 9 clock cycles
    assign mac_out = p_cas[8][ACC_WIDTH-1:0];

    // Shift register for valid_out (delay by 9 cycles)
    // Must be 10 bits: shift into [0] each cycle, read from [9] after 9 cycles.
    // A 9-bit register only provides 8 cycles of delay because {sr[7:0], in}
    // drops bit [8] on every shift — bit [8] is never written.
    logic [9:0] valid_sr;
    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            valid_sr  <= '0;
        end else begin
            valid_sr  <= {valid_sr[8:0], valid_in};
        end
    end

    // valid_out appears exactly 9 cycles after valid_in
    assign valid_out = valid_sr[9];

endmodule
`default_nettype wire
