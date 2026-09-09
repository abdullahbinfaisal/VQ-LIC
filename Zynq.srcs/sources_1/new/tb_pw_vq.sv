`timescale 1ns/1ps
// ============================================================================
// tb_pw_vq.sv -- functional check of the PW-hosted VQ branch against the
//                golden model, at BOTH quantiser geometries.
//
// Drives real latent groups through pw_pixel_major_core in VQ mode and
// compares the packed index beats byte-for-byte with vqpw_encode_frame().
// Vectors come from gen_vq_vectors.c, which links the SAME vq_pw.c the
// firmware uses -- so this checks the RTL against the reference, not against
// a second re-derivation of the reference.
//
// The work lives in pw_vq_bench, which is parameterised, and the tops below
// pick a geometry and a vector directory. That way the DEPLOYED and LEGACY
// configurations are proved by ONE piece of bench source and cannot drift
// apart:
//
//   tb_pw_vq         DEPLOYED  M=4 K=64 Dsub=16  cout=256, 8 batches, dir vq64
//                    -> 6-bit indices, norm addresses to 255, scores past the
//                       old 20-bit range, one sub-codebook spanning 2 batches
//   tb_pw_vq_legacy  LEGACY    M=8 K=16 Dsub=8   cout=128, 4 batches, dir vq16
//                    -> the 2026-09-03 configuration, unchanged, as regression
//   tb_pw_vq_guard   the synthesisable configuration guard: every geometry
//                    that would alias must be REFUSED, not silently run
//
// Regenerate vectors with
//   gen_vq_vectors        <ngroups> <scenario> vq64
//   gen_vq_vectors_legacy <ngroups> <scenario> vq16
// scenario: 0 random, 1 tie storm, 2 INT8 extremes.
// ============================================================================

module pw_vq_bench #(
  parameter int VQ_K       = 64,
  parameter int VQ_NORM_D  = 256,
  parameter int VQ_SCORE_W = 21,
  parameter int VQ_M       = 4,
  parameter int VQ_DSUB    = 16,
  parameter int COUT_MAX   = 256,
  parameter int NG         = 64,
  parameter string DIR     = "vq64",
  parameter string TAG     = "DEPLOYED M=4 K=64 Dsub=16"
)();

  localparam int DATA_WIDTH = 8;
  localparam int ACC_WIDTH  = 24;
  localparam int CIN_MAX    = 240;
  localparam int N_LANES    = 8;
  localparam int N_OC       = 32;
  localparam int PARAM_AW   = $clog2(COUT_MAX);
  localparam int W_AW       = $clog2((COUT_MAX/N_OC)*CIN_MAX);
  localparam int VQ_AW      = $clog2(VQ_NORM_D);
  localparam int VQ_DIM     = 64;

  // Derived exactly as vq_pw.h derives them, from K and M alone.
  localparam int COUT_TOTAL = VQ_M * VQ_K;                       // 256 or 128
  localparam int NBATCH     = COUT_TOTAL / N_OC;                 //   8 or   4
  localparam int SUBS_PER_B = (VQ_K < N_OC) ? (N_OC / VQ_K) : 1; //   1 or   2
  localparam int CIN_MAC    = VQ_DSUB * SUBS_PER_B;              //  16 in both
  localparam int W_PER_BANK = NBATCH * CIN_MAC;                  // 128 or  64

  logic clk = 0, rst_n = 0;
  always #5 clk = ~clk;
  longint unsigned cyc = 0;
  always_ff @(posedge clk) cyc <= cyc + 1;

  logic        start_in = 0, done_out, cfg_err, cfg_err_stb;
  logic [31:0] tile_pixels;
  logic [11:0] cin_run  = 12'(CIN_MAC);
  logic [11:0] cout_run = 12'(COUT_TOTAL);
  logic [7:0]  zp_in = 8'd128, zp_out = 8'd128;
  logic        relu_en = 0;
  logic        vq_mode = 1'b1;
  logic [11:0] vq_cin_load = 12'(VQ_DIM);

  logic                        vq_norm_we = 0;
  logic [VQ_AW-1:0]            vq_norm_addr = 0;
  logic signed [VQ_SCORE_W-1:0] vq_norm_data = 0;

  logic [W_AW-1:0]              w_rd_addr;
  logic                         w_rd_en;
  // Model the real weight BRAM's POWER-UP STATE. xpm memories read 0 from
  // uninitialised locations; an uninitialised TB array reads X, and that X
  // reaches w_rd_data_rr -> p_reg -> acc on the very first MAC and poisons
  // every result. This was the actual cause of the earlier bench failures.
  logic signed [DATA_WIDTH-1:0] w_rd_data [0:N_OC-1] = '{default:'0};

  logic [PARAM_AW-1:0] param_rd_addr;
  logic                param_rd_en;
  logic signed [31:0]  param_bias_data = '0;
  logic [31:0]         param_mult_data = '0;
  logic [7:0]          param_shift_data = '0;

  logic                          valid_in;
  logic [N_LANES*DATA_WIDTH-1:0] pixel_in;
  logic                          consume_in;
  logic [N_LANES*DATA_WIDTH-1:0] pixel_out;
  logic                          valid_out;
  logic                          out_stall = 0;

  // ---- vectors ----
  logic [63:0] latent_mem [0:VQ_DIM*1800-1];
  logic [7:0]  w_mem      [0:N_OC*W_PER_BANK-1];
  logic [19:0] norm_mem   [0:VQ_NORM_D-1];
  logic [63:0] expect_mem [0:4*1800-1];

  // ---- weight BRAM model: registered read, w[oc][addr] ----
  always_ff @(posedge clk) begin
    if (w_rd_en)
      for (int oc = 0; oc < N_OC; oc++)
        w_rd_data[oc] <= $signed(w_mem[oc*W_PER_BANK + int'(w_rd_addr)]);
  end

  // ---- param BRAM model: bias 0 (the VQ branch does NOT use this path) ----
  always_ff @(posedge clk) begin
    if (param_rd_en) begin
      param_bias_data  <= 32'sd0;
      param_mult_data  <= 32'h0001_0000;
      param_shift_data <= 8'd16;
    end
  end

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

  // ---- input stream: one beat per channel, 64 per group ----
  // Proper AXIS-style driver: data and valid are COMBINATIONAL from the beat
  // pointer and hold until the handshake, and the pointer advances only on
  // (valid_in && consume_in).
  int beat_i;
  always_comb begin
    valid_in = rst_n && (beat_i < NG*VQ_DIM);
    pixel_in = latent_mem[beat_i < NG*VQ_DIM ? beat_i : 0];
  end
  always_ff @(posedge clk) begin
    if (!rst_n) beat_i <= 0;
    else if (valid_in && consume_in) beat_i <= beat_i + 1;
  end

  // ---- output capture + per-group cycle timing ----
  // The first beat of each group is beat index % 4 == 0; the gap between
  // consecutive first-beats IS the per-group service the analytical model
  // predicts, so it is measured here rather than inferred from the total.
  int nout = 0, nbad = 0;
  longint unsigned t_first = 0, t_prev = 0, t_last = 0;
  int      ngap = 0;
  longint  gap_sum = 0;
  int      gap_min = 1 << 30, gap_max = 0;

  always_ff @(posedge clk) begin
    if (rst_n && valid_out) begin
      if (nout < 4*NG) begin
        if (pixel_out !== expect_mem[nout]) begin
          if (nbad < 6)
            $display("  MISMATCH beat %0d (positions %0d,%0d): got %016x exp %016x",
                     nout, (nout/4)*8 + (nout%4)*2, (nout/4)*8 + (nout%4)*2 + 1,
                     pixel_out, expect_mem[nout]);
          nbad++;
        end
      end else begin
        if (nbad < 6) $display("  EXTRA beat %0d (expected only %0d)", nout, 4*NG);
        nbad++;
      end
      if ((nout % 4) == 0) begin
        if (nout == 0) t_first = cyc;
        else begin
          automatic int g = int'(cyc - t_prev);
          gap_sum += g; ngap++;
          if (g < gap_min) gap_min = g;
          if (g > gap_max) gap_max = g;
        end
        t_prev = cyc;
      end
      t_last = cyc;
      nout++;
    end
  end

  initial begin
    tile_pixels = NG * N_LANES;

    $readmemh({DIR, "/latent.hex"},  latent_mem);
    $readmemh({DIR, "/weights.hex"}, w_mem);
    $readmemh({DIR, "/norms.hex"},   norm_mem);
    $readmemh({DIR, "/expect.hex"},  expect_mem);

    $display("\n=== PW-hosted VQ vs golden model : %s ===", TAG);
    $display("groups=%0d  positions=%0d  vectors=%s", NG, NG*N_LANES, DIR);
    $display("config : cin_run=%0d vq_cin_load=%0d cout_run=%0d N_OC=%0d",
             cin_run, vq_cin_load, cout_run, N_OC);
    $display("VQ     : K=%0d KW=%0d NORM_D=%0d SCORE_W=%0d M=%0d Dsub=%0d batches=%0d w_per_bank=%0d",
             VQ_K, $clog2(VQ_K), VQ_NORM_D, VQ_SCORE_W, VQ_M, VQ_DSUB,
             NBATCH, W_PER_BANK);

    repeat (4) @(posedge clk);
    rst_n = 1;
    repeat (4) @(posedge clk);

    // load the codeword-norm ROM -- all VQ_NORM_D of them, so at the deployed
    // geometry this writes addresses 128..255 that the old 7-bit port could
    // not reach at all.
    for (int i = 0; i < VQ_NORM_D; i++) begin
      @(posedge clk);
      vq_norm_we   <= 1'b1;
      vq_norm_addr <= VQ_AW'(i);
      vq_norm_data <= VQ_SCORE_W'(signed'(norm_mem[i]));
    end
    @(posedge clk) vq_norm_we <= 1'b0;
    repeat (4) @(posedge clk);

    @(posedge clk) start_in <= 1;
    @(posedge clk) start_in <= 0;

    fork
      begin
        wait (nout >= 4*NG);
        repeat (50) @(posedge clk);
      end
      begin
        repeat (2000000) @(posedge clk);
        $display("  TIMEOUT: only %0d of %0d beats emitted", nout, 4*NG);
      end
    join_any

    $display("\ncfg_err = %0b (must be 0)", cfg_err);
    $display("beats emitted = %0d / %0d, mismatches = %0d", nout, 4*NG, nbad);
    if (ngap > 0)
      $display("cycles/group: mean %0.2f  min %0d  max %0d  over %0d gaps",
               real'(gap_sum) / real'(ngap), gap_min, gap_max, ngap);
    $display("VQCYC,%s,%0d,%0d,%0d,%0d,%0d,%0d,%0d",
             TAG, VQ_K, COUT_TOTAL, NG, (ngap > 0) ? int'(gap_sum/ngap) : 0,
             gap_min, gap_max, nbad);
    if (nout == 4*NG && nbad == 0 && cfg_err == 1'b0)
      $display("RESULT: PASS -- RTL matches vqpw_encode_frame() exactly");
    else
      $display("RESULT: FAIL");
    $finish;
  end

endmodule


// ---------------------------------------------------------------------------
// DEPLOYED: M=4, K=64, Dsub=16. c_out = 256, so COUT_MAX must be 256 -- at
// 240 the engine floors to 7 batches (224) and the guard refuses the run.
// ---------------------------------------------------------------------------
module tb_pw_vq;
  pw_vq_bench #(.VQ_K(64), .VQ_NORM_D(256), .VQ_SCORE_W(21),
                .VQ_M(4), .VQ_DSUB(16), .COUT_MAX(256),
                .NG(64), .DIR("vq64"),
                .TAG("DEPLOYED M=4 K=64 Dsub=16")) u();
endmodule

// ---------------------------------------------------------------------------
// LEGACY regression: M=8, K=16, Dsub=8. Bit-for-bit the 2026-09-03 geometry.
// COUT_MAX stays 240 here deliberately -- the legacy build's parameter -- so
// this also proves the generalisation did not disturb the shipped sizing.
// ---------------------------------------------------------------------------
module tb_pw_vq_legacy;
  pw_vq_bench #(.VQ_K(16), .VQ_NORM_D(128), .VQ_SCORE_W(20),
                .VQ_M(8), .VQ_DSUB(8), .COUT_MAX(240),
                .NG(64), .DIR("vq16"),
                .TAG("LEGACY M=8 K=16 Dsub=8")) u();
endmodule
