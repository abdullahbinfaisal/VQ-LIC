`timescale 1ns/1ps
// ============================================================================
// tb_pw_vq.sv -- functional check of the PW-hosted VQ branch against the
//                golden model.
//
// Drives real latent groups through pw_pixel_major_core in VQ mode and
// compares the packed index beats byte-for-byte with vqpw_encode_frame().
// Vectors come from gen_vq_vectors.c, which links the SAME vq_pw.c the
// firmware uses -- so this checks the RTL against the reference, not against
// a second re-derivation of the reference.
//
// Configuration (option D):
//   cin_run     = 16   MAC length per batch and weight stride
//   vq_cin_load = 64   channels STREAMED per group (one contiguous DDR read)
//   cout_run    = 128  4 batches x 32 OC = 8 sub-codebooks x 16 codewords
//
// STATUS 2026-09-03: PASSES, 0 mismatches on all 256 beats, for all three
// scenarios -- 0 random, 1 tie storm, 2 INT8 extremes. It failed until the
// five PW datapath defects in commit cb16322 were fixed; nothing in the VQ
// branch itself changed. The tie-storm and corner scenarios are what make
// this more than a lucky pass: they pin the lowest-index tie rule and the
// 20-bit score width at their limits.
//
// Vectors are read from the CURRENT RUN DIRECTORY by fixed name (xsim.bat
// mangles plusargs); regenerate with
//   gen_vq_vectors <ngroups> <scenario> <dir>
// ============================================================================
module tb_pw_vq;

  localparam int DATA_WIDTH = 8;
  localparam int ACC_WIDTH  = 24;
  localparam int CIN_MAX    = 240;
  localparam int COUT_MAX   = 240;
  localparam int N_LANES    = 8;
  localparam int N_OC       = 32;
  localparam int PARAM_AW   = $clog2(COUT_MAX);
  localparam int W_AW       = $clog2((COUT_MAX/N_OC)*CIN_MAX);
  localparam int W_PER_BANK = 64;
  localparam int VQ_DIM     = 64;

  // Vectors are read from the CURRENT RUN DIRECTORY by fixed name; the runner
  // copies one scenario in at a time. xsim.bat mangles plusargs, so this is
  // deliberately argument-free.
  localparam int NG = 64;

  logic clk = 0, rst_n = 0;
  always #5 clk = ~clk;

  logic        start_in = 0, done_out;
  logic [31:0] tile_pixels;
  logic [11:0] cin_run  = 12'd16;
  logic [11:0] cout_run = 12'd128;
  logic [7:0]  zp_in = 8'd128, zp_out = 8'd128;
  logic        relu_en = 0;
  logic        vq_mode = 1'b1;
  logic [11:0] vq_cin_load = 12'd64;

  logic                vq_norm_we = 0;
  logic [6:0]          vq_norm_addr = 0;
  logic signed [19:0]  vq_norm_data = 0;

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
  logic [19:0] norm_mem   [0:127];
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
    .COUT_MAX(COUT_MAX), .N_LANES(N_LANES), .N_OC(N_OC), .USE_PW_VQ(1)
  ) dut (
    .clk(clk), .rst_n(rst_n), .start_in(start_in), .done_out(done_out),
    .tile_pixels(tile_pixels), .cin_run(cin_run), .cout_run(cout_run),
    .zp_in(zp_in), .zp_out(zp_out), .relu_en(relu_en),
    .vq_mode(vq_mode), .vq_cin_load(vq_cin_load),
    .vq_norm_we(vq_norm_we), .vq_norm_addr(vq_norm_addr), .vq_norm_data(vq_norm_data),
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
  // (valid_in && consume_in). An earlier version non-blocking-assigned
  // valid_in/pixel_in and then waited on the edge, which presented each beat
  // one cycle after the handshake that acknowledged it and made even ordinary
  // convolution accumulate zeros.
  int beat_i;
  always_comb begin
    valid_in = rst_n && (beat_i < NG*VQ_DIM);
    pixel_in = latent_mem[beat_i < NG*VQ_DIM ? beat_i : 0];
  end
  always_ff @(posedge clk) begin
    if (!rst_n) beat_i <= 0;
    else if (valid_in && consume_in) beat_i <= beat_i + 1;
  end

  int dbg4 = 0;
  always_ff @(posedge clk) begin
    if (rst_n && (dut.shadow_copy_trig || dut.sha_wr_en_e || dut.ppu_valid_in) && dbg4 < 14) begin
      $display("  SHA trig=%0b len=%0d idx=%0d wr_e=%0b wr_bank=%0b rd_bank=%0b acc00=%0d shard=%0d ppuv=%0b",
               dut.shadow_copy_trig, dut.shadow_copy_len, dut.shadow_copy_idx,
               dut.sha_wr_en_e, dut.sha_wr_bank, dut.sha_rd_bank,
               dut.acc[0][0], $signed(dut.sha_rd_data[23:0]), dut.ppu_valid_in);
      dbg4++;
    end
  end

  int dbg3 = 0;
  always_ff @(posedge clk) begin
    if (rst_n && dut.st == 3'd2 && dbg3 < 26) begin
      $display("  MAC t: acc00=%0d w_rr0=%0d pbpx0=%0d rd_d2=%0b first_ic=%0b",
               dut.acc[0][0], dut.w_rd_data_rr[0], dut.pb_pixel_r[0],
               dut.rd_issued_d2, dut.first_ic);
      dbg3++;
    end
  end

  int dbg2 = 0;
  always_ff @(posedge clk) begin
    if (rst_n && dut.rd_issued && dbg2 < 40) begin
      $display("  RD ic_idx=%0d w_base=%0d pb_idx=%0d w_addr=%0d st=%0d cb=%0d",
               dut.ic_idx, dut.w_addr_base, dut.pb_rd_index, dut.w_rd_addr,
               dut.st, dut.compute_buf);
      dbg2++;
    end
  end

  // ---- debug: first 20 PPU issues, the exact bus the VQ branch snoops ----
  int dbgn = 0;
  always_ff @(posedge clk) begin
    if (rst_n && dut.ppu_valid_in && dut.G_VQ.vq_k < 2 && dbgn < 24) begin
      $display("  DBG issue_idx=%0d oc_r=%0d batch_r=%0d k=%0d half=%0d norm=%0d acc0=%0d acc1=%0d score0=%0d",
               dut.ppu_issue_idx, dut.G_VQ.vq_oc_r, dut.G_VQ.vq_batch_r,
               dut.G_VQ.vq_k, dut.G_VQ.vq_half, dut.G_VQ.vq_norm_q,
               dut.ppu_acc_in[0], dut.ppu_acc_in[1], dut.G_VQ.vq_score[0]);
      dbgn++;
    end
  end

  // ---- output capture ----
  int nout = 0, nbad = 0;
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
      nout++;
    end
  end

  initial begin
    tile_pixels = NG * N_LANES;

    $readmemh("latent.hex",  latent_mem);
    $readmemh("weights.hex", w_mem);
    $readmemh("norms.hex",   norm_mem);
    $readmemh("expect.hex",  expect_mem);

    $display("\n=== PW-hosted VQ vs golden model ===");
    $display("groups=%0d  positions=%0d", NG, NG*N_LANES);
    $display("config : cin_run=%0d vq_cin_load=%0d cout_run=%0d N_OC=%0d",
             cin_run, vq_cin_load, cout_run, N_OC);

    repeat (4) @(posedge clk);
    rst_n = 1;
    repeat (4) @(posedge clk);

    // load the codeword-norm ROM
    for (int i = 0; i < 128; i++) begin
      @(posedge clk);
      vq_norm_we   <= 1'b1;
      vq_norm_addr <= i[6:0];
      vq_norm_data <= $signed(norm_mem[i]);
    end
    @(posedge clk) vq_norm_we <= 1'b0;
    repeat (4) @(posedge clk);

    @(posedge clk) start_in <= 1;
    @(posedge clk) start_in <= 0;

    // run to completion or timeout
    fork
      begin
        wait (nout >= 4*NG);
        repeat (50) @(posedge clk);
      end
      begin
        repeat (400000) @(posedge clk);
        $display("  TIMEOUT: only %0d of %0d beats emitted", nout, 4*NG);
      end
    join_any

    $display("\nbeats emitted = %0d / %0d, mismatches = %0d", nout, 4*NG, nbad);
    if (nout == 4*NG && nbad == 0)
      $display("RESULT: PASS -- RTL matches vqpw_encode_frame() exactly");
    else
      $display("RESULT: FAIL");
    $finish;
  end

endmodule
