`timescale 1ns/1ps
// ============================================================================
// tb_pw_bias_align.sv -- does ppu_bias_q line up with ppu_acc_in?
//
// WHY THIS EXISTS
//   The PW-hosted VQ branch carries ||v_k||^2 on the per-OC param-BRAM bias
//   read, so bias must be aligned with the accumulator it is presented with.
//   Every measurement this project has ever taken used bias == 0 for every
//   channel (surr_fill_dummy_params), so a one-OC skew would be invisible in
//   convolution and fatal in VQ. Read the answer off silicon-accurate RTL
//   rather than off the source.
//
// METHOD
//   Model the param BRAM as the identity: bias[addr] = addr. Then ppu_bias_q
//   IS the absolute OC index of whatever bias the PPU is being handed. The
//   accumulator presented alongside it belongs to OC (ppu_issue_idx - 1),
//   because ppu_issue_idx is incremented in the same cycle ppu_valid_in is
//   registered. Aligned  <=>  ppu_bias_q == ppu_issue_idx - 1.
// ============================================================================
module tb_pw_bias_align;

  localparam int DATA_WIDTH = 8;
  localparam int ACC_WIDTH  = 24;
  localparam int CIN_MAX    = 240;
  localparam int COUT_MAX   = 240;
  localparam int N_LANES    = 8;
  localparam int N_OC       = 32;
  localparam int PARAM_AW   = $clog2(COUT_MAX);
  localparam int W_AW       = $clog2((COUT_MAX/N_OC)*CIN_MAX);

  logic clk = 0, rst_n = 0;
  always #5 clk = ~clk;

  logic        start_in = 0, done_out;
  logic [31:0] tile_pixels = 32'd8;   // exactly one group of 8 pixels
  logic [11:0] cin_run     = 12'd4;
  logic [11:0] cout_run    = 12'd32;  // one full batch
  logic [7:0]  zp_in = 8'd128, zp_out = 8'd128;
  logic        relu_en = 0;

  logic [W_AW-1:0]     w_rd_addr;
  logic                w_rd_en;
  logic signed [DATA_WIDTH-1:0] w_rd_data [0:N_OC-1];

  logic [PARAM_AW-1:0] param_rd_addr;
  logic                param_rd_en;
  logic signed [31:0]  param_bias_data;
  logic [31:0]         param_mult_data;
  logic [7:0]          param_shift_data;

  logic                        valid_in = 1;
  logic [N_LANES*DATA_WIDTH-1:0] pixel_in = {8{8'd129}}; // act_diff = +1
  logic                        consume_in;
  logic [N_LANES*DATA_WIDTH-1:0] pixel_out;
  logic                        valid_out;
  logic                        out_stall = 0;

  // ---- weight BRAM model: w[oc] = 1, so acc = cin_run * 1 * 1 = 4 ----
  always_ff @(posedge clk) begin
    for (int i = 0; i < N_OC; i++) w_rd_data[i] <= 8'sd1;
  end

  // ---- param BRAM model: IDENTITY. bias[addr] = addr. 1-cycle latency. ----
  always_ff @(posedge clk) begin
    if (param_rd_en) begin
      param_bias_data  <= $signed({24'd0, param_rd_addr});
      param_mult_data  <= 32'h0001_0000;
      param_shift_data <= 8'd16;
    end
  end

  pw_pixel_major_core #(
    .DATA_WIDTH(DATA_WIDTH), .ACC_WIDTH(ACC_WIDTH), .CIN_MAX(CIN_MAX),
    .COUT_MAX(COUT_MAX), .N_LANES(N_LANES), .N_OC(N_OC), .USE_PW_VQ(0)
  ) dut (
    .clk(clk), .rst_n(rst_n), .start_in(start_in), .done_out(done_out),
    .tile_pixels(tile_pixels), .cin_run(cin_run), .cout_run(cout_run),
    .zp_in(zp_in), .zp_out(zp_out), .relu_en(relu_en),
    .vq_mode(1'b0), .vq_cin_load(12'd0),
    .w_rd_addr(w_rd_addr), .w_rd_en(w_rd_en), .w_rd_data(w_rd_data),
    .param_rd_addr(param_rd_addr), .param_rd_en(param_rd_en),
    .param_bias_data(param_bias_data), .param_mult_data(param_mult_data),
    .param_shift_data(param_shift_data),
    .valid_in(valid_in), .pixel_in(pixel_in), .consume_in(consume_in),
    .pixel_out(pixel_out), .valid_out(valid_out), .out_stall(out_stall)
  );

  int nobs = 0, nskew = 0;
  int first_bias = -1, first_expect = -1;

  // Probe the PPU issue bus -- exactly the signals the VQ branch will snoop.
  always_ff @(posedge clk) begin
    if (rst_n && dut.ppu_valid_in) begin
      automatic int oc_presented = int'(dut.ppu_issue_idx) - 1;
      automatic int bias_seen    = int'(param_bias_data === 32'bx ? 0 : dut.ppu_bias_q);
      if (nobs < 8)
        $display("  issue_idx=%0d -> acc is OC %0d, ppu_bias_q = %0d %s",
                 int'(dut.ppu_issue_idx), oc_presented, bias_seen,
                 (bias_seen == oc_presented) ? "ALIGNED" : "<== SKEW");
      if (bias_seen != oc_presented) begin
        nskew++;
        if (first_bias < 0) begin first_bias = bias_seen; first_expect = oc_presented; end
      end
      nobs++;
    end
  end

  initial begin
    $display("\n=== PPU bias/accumulator alignment probe ===");
    repeat (4) @(posedge clk);
    rst_n = 1;
    repeat (4) @(posedge clk);
    @(posedge clk) start_in <= 1;
    @(posedge clk) start_in <= 0;

    repeat (4000) @(posedge clk);

    $display("\nobservations = %0d, skewed = %0d", nobs, nskew);
    if (nobs == 0)
      $display("RESULT: INCONCLUSIVE -- the drain never fired");
    else if (nskew == 0)
      $display("RESULT: ALIGNED -- ppu_bias_q matches the accumulator's OC");
    else
      $display("RESULT: SKEWED by %0d -- bias %0d presented with OC %0d's accumulator",
               first_bias - first_expect, first_bias, first_expect);
    $finish;
  end

endmodule
