`timescale 1ns/1ps
// ============================================================================
// tb_pw_vq_guard.sv -- the synthesisable configuration guard.
//
// WHY THIS EXISTS. Before this change the only geometry checks in
// pw_pixel_major_core were three zero tests, and the cout_run > COUT_MAX check
// lived ONLY in pw_pixel_major_core_SIMCOPY.sv -- a file that is not
// synthesised. An over-range cout_run therefore RAN: the weight and norm
// addresses wrapped, the frame came out wrong, and nothing anywhere said so.
// That is the same silent-aliasing failure class as MAX_CG_PRODUCT.
//
// The guard is now in the synthesised S_IDLE and raises cfg_err instead of
// starting. This bench asserts, for each geometry below, that
//
//     cfg_err == expected   AND   (cfg_err -> zero output beats)
//
// The second half matters as much as the first: a guard that flags the error
// but runs anyway is no better than no guard.
//
// The engine is elaborated exactly as the DEPLOYED build:
//   COUT_MAX = 256 -> COUT_HW_MAX = (256/32)*32 = 256
//   VQ_K = 64, VQ_NORM_D = 256, CIN_MAX = 240
// ============================================================================
module tb_pw_vq_guard;

  localparam int DATA_WIDTH = 8;
  localparam int ACC_WIDTH  = 24;
  localparam int CIN_MAX    = 240;
  localparam int COUT_MAX   = 256;
  localparam int N_LANES    = 8;
  localparam int N_OC       = 32;
  localparam int VQ_K       = 64;
  localparam int VQ_NORM_D  = 256;
  localparam int VQ_SCORE_W = 21;
  localparam int PARAM_AW   = $clog2(COUT_MAX);
  localparam int W_AW       = $clog2((COUT_MAX/N_OC)*CIN_MAX);
  localparam int VQ_AW      = $clog2(VQ_NORM_D);

  logic clk = 0, rst_n = 0;
  always #5 clk = ~clk;

  logic        start_in = 0, done_out, cfg_err, cfg_err_stb;
  // One group of N_LANES pixels: enough to prove a legal geometry runs and
  // emits, short enough that every case retires immediately.
  logic [31:0] tile_pixels = 32'd8;
  logic [11:0] cin_run     = 12'd16;
  logic [11:0] cout_run    = 12'd256;
  logic [7:0]  zp_in = 8'd128, zp_out = 8'd128;
  logic        relu_en = 0;
  logic        vq_mode = 1'b1;
  logic [11:0] vq_cin_load = 12'd64;

  logic                         vq_norm_we   = 0;
  logic [VQ_AW-1:0]             vq_norm_addr = 0;
  logic signed [VQ_SCORE_W-1:0] vq_norm_data = 0;

  logic [W_AW-1:0]              w_rd_addr;
  logic                         w_rd_en;
  logic signed [DATA_WIDTH-1:0] w_rd_data [0:N_OC-1] = '{default:'0};
  logic [PARAM_AW-1:0]          param_rd_addr;
  logic                         param_rd_en;
  logic signed [31:0]           param_bias_data  = '0;
  logic [31:0]                  param_mult_data  = 32'h0001_0000;
  logic [7:0]                   param_shift_data = 8'd16;

  logic                          valid_in = 1'b1;   // always ready to feed
  logic [N_LANES*DATA_WIDTH-1:0] pixel_in = 64'h8080_8080_8080_8080;
  logic                          consume_in;
  logic [N_LANES*DATA_WIDTH-1:0] pixel_out;
  logic                          valid_out;
  logic                          out_stall = 0;

  pw_pixel_major_core #(
    .DATA_WIDTH(DATA_WIDTH), .ACC_WIDTH(ACC_WIDTH), .CIN_MAX(CIN_MAX),
    .COUT_MAX(COUT_MAX), .N_LANES(N_LANES), .N_OC(N_OC), .USE_PW_VQ(1),
    .VQ_K(VQ_K), .VQ_NORM_D(VQ_NORM_D), .VQ_SCORE_W(VQ_SCORE_W)
  ) dut (
    .clk(clk), .rst_n(rst_n), .start_in(start_in), .done_out(done_out),
    .tile_pixels(tile_pixels), .cin_run(cin_run), .cout_run(cout_run),
    .zp_in(zp_in), .zp_out(zp_out), .relu_en(relu_en),
    .vq_mode(vq_mode), .vq_cin_load(vq_cin_load),
    .vq_norm_we(vq_norm_we), .vq_norm_addr(vq_norm_addr),
    .vq_norm_data(vq_norm_data), .cfg_err(cfg_err), .cfg_err_stb(cfg_err_stb),
    .w_rd_addr(w_rd_addr), .w_rd_en(w_rd_en), .w_rd_data(w_rd_data),
    .param_rd_addr(param_rd_addr), .param_rd_en(param_rd_en),
    .param_bias_data(param_bias_data), .param_mult_data(param_mult_data),
    .param_shift_data(param_shift_data),
    .valid_in(valid_in), .pixel_in(pixel_in), .consume_in(consume_in),
    .pixel_out(pixel_out), .valid_out(valid_out), .out_stall(out_stall)
  );

  int beats = 0;
  always_ff @(posedge clk) if (rst_n && valid_out) beats <= beats + 1;

  int ncase = 0, nfail = 0;

  task automatic run_case(input string name,
                          input logic       vqm,
                          input logic [31:0] tp,
                          input logic [11:0] cin,
                          input logic [11:0] cout,
                          input logic [11:0] cload,
                          input logic        expect_err);
    begin
      // Re-arm from a known state. A previous refused run left st = S_DONE.
      @(posedge clk);
      vq_mode     <= vqm;
      tile_pixels <= tp;
      cin_run     <= cin;
      cout_run    <= cout;
      vq_cin_load <= cload;
      beats        = 0;
      @(posedge clk);
      @(posedge clk) start_in <= 1'b1;
      @(posedge clk) start_in <= 1'b0;
      // Wait for the run to retire. A REFUSED start reaches S_DONE on the next
      // edge, so this returns almost immediately; a legal one runs its single
      // group. Waiting rather than counting cycles is what makes the next case
      // start from S_IDLE -- S_IDLE is the only state that accepts start_in,
      // so a fixed delay would silently drop every case after a legal one.
      fork
        begin wait (done_out === 1'b1); @(posedge clk); end
        begin repeat (100000) @(posedge clk);
              $display("  (timeout waiting for done on %s)", name); end
      join_any
      disable fork;
      repeat (4) @(posedge clk);

      ncase++;
      if (cfg_err !== expect_err) begin
        nfail++;
        $display("  FAIL %-34s cfg_err=%0b expected %0b  (vq=%0b tp=%0d cin=%0d cout=%0d load=%0d)",
                 name, cfg_err, expect_err, vqm, tp, cin, cout, cload);
      end else if (expect_err && beats != 0) begin
        nfail++;
        $display("  FAIL %-34s refused but emitted %0d beats", name, beats);
      end else begin
        $display("  ok   %-34s cfg_err=%0b beats=%0d", name, cfg_err, beats);
      end

      // Let a refused run settle back to idle before the next case.
      repeat (10) @(posedge clk);
    end
  endtask

  initial begin
    $display("\n=== configuration guard: over-range geometry must be REFUSED ===");
    $display("COUT_MAX=%0d -> COUT_HW_MAX=%0d, CIN_MAX=%0d, VQ_K=%0d, VQ_NORM_D=%0d",
             COUT_MAX, (COUT_MAX/N_OC)*N_OC, CIN_MAX, VQ_K, VQ_NORM_D);

    repeat (4) @(posedge clk);
    rst_n = 1;
    repeat (4) @(posedge clk);

    // Fill the norm ROM so the score datapath never sees X. The guard cases do
    // not check index data, but an X score trips a_vq_score_fits and buries the
    // result under thousands of assertion lines.
    for (int i = 0; i < VQ_NORM_D; i++) begin
      @(posedge clk);
      vq_norm_we   <= 1'b1;
      vq_norm_addr <= VQ_AW'(i);
      vq_norm_data <= VQ_SCORE_W'(i);
    end
    @(posedge clk) vq_norm_we <= 1'b0;
    repeat (4) @(posedge clk);

    //        name                              vq  tile  cin  cout  load  err
    run_case("legal DEPLOYED 4x64",              1,   8,  16,  256,   64,  1'b0);
    run_case("zero tile_pixels",                 1,   0,  16,  256,   64,  1'b1);
    run_case("zero cin_run",                     1,   8,   0,  256,   64,  1'b1);
    run_case("zero cout_run",                    1,   8,  16,    0,   64,  1'b1);
    // cout_run beyond the weight batches. At COUT_MAX=256 the ceiling is 256,
    // so 288 (9 batches) must be refused. This is the case that used to alias.
    run_case("cout_run 288 > COUT_HW_MAX",       1,   8,  16,  288,   64,  1'b1);
    run_case("cout_run 320 > VQ_NORM_D",         1,   8,  16,  320,   64,  1'b1);
    run_case("cin_run 256 > CIN_MAX",            1,   8, 256,  256,   64,  1'b1);
    run_case("vq_cin_load 256 > CIN_MAX",        1,   8,  16,  256,  256,  1'b1);
    // cout_run must be a whole number of sub-codebooks: 192 = 3*64 is legal,
    // 224 = 3.5*64 is not -- a partial sub-codebook would commit an argmin
    // that never saw all K candidates.
    run_case("cout_run 192 = 3*K",               1,   8,  16,  192,   64,  1'b0);
    run_case("cout_run 224 not a multiple of K", 1,   8,  16,  224,   64,  1'b1);
    // The batch windows must fit inside the channels actually streamed:
    // M = 256/64 = 4 windows of cin_run = 16 need 64 channels, so 48 is short.
    run_case("vq_cin_load 48 < M*cin_run",       1,   8,  16,  256,   48,  1'b1);
    run_case("vq_cin_load 32 with cout 128",     1,   8,  16,  128,   32,  1'b0);
    // Convolution mode: the K-multiple and norm-depth rules must NOT apply,
    // but COUT_HW_MAX and CIN_MAX still must.
    run_case("conv cout_run 224 (legal)",        0,   8,  16,  224,   64,  1'b0);
    run_case("conv cout_run 256 (legal)",        0,   8,  16,  256,   64,  1'b0);
    run_case("conv cout_run 288 (refused)",      0,   8,  16,  288,   64,  1'b1);
    run_case("conv cin_run 240 (legal)",         0,   8, 240,  128,   64,  1'b0);
    run_case("conv cin_run 241 (refused)",       0,   8, 241,  128,   64,  1'b1);
    // A legal run after a refused one must clear cfg_err, so the flag always
    // describes the MOST RECENT start rather than latching forever.
    run_case("legal again, cfg_err must clear",  1,   8,  16,  256,   64,  1'b0);

    $display("\n%0d cases, %0d failures -> %s",
             ncase, nfail, (nfail == 0) ? "PASS" : "FAIL");
    $finish;
  end

endmodule
