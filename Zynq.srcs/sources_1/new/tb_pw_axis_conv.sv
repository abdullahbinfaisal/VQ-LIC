`timescale 1ns/1ps
// ============================================================================
// tb_pw_axis_conv.sv -- ORDINARY CONVOLUTION at the pw_single_oc_axis level,
//                       with the real input/output FIFOs and real backpressure.
//
// THE QUESTION THIS ANSWERS
//   tb_pw_vq.sv (bare core) failed, and so did the same harness in convolution
//   mode. Two explanations were live: (a) the bare-core harness does not
//   reproduce the core's real environment -- notably out_stall, which gates
//   P_DRAIN and was tied low -- or (b) the PW arithmetic is genuinely wrong and
//   has never been caught, because ppu.sv records that this design "has never
//   had a data check".
//
//   This bench settles it. It drives plain convolution through the FIFOs, so
//   out_stall and the load/compute/drain interleaving are the real ones.
//     PASS -> the bare-core harness was at fault; rebuild the VQ bench here.
//     FAIL -> a real, never-detected bug in the PW datapath, which outranks
//             the VQ port entirely.
//
// IDENTITY PPU: bias = 0, mult = 1<<16, shift = 16, relu off, and operands
// small enough that the output clamp never engages, so out = acc + 128
// exactly and any mismatch is in the MAC / accumulator / shadow path.
// See gen_conv_vectors.c.
// ============================================================================
module tb_pw_axis_conv;

  localparam int DATA_WIDTH = 8;
  localparam int ACC_WIDTH  = 24;
  localparam int CIN_MAX    = 240;
  localparam int COUT_MAX   = 240;
  localparam int N_LANES    = 8;
  localparam int N_OC       = 32;
  localparam int W_AW       = $clog2((COUT_MAX/N_OC)*CIN_MAX);
  localparam int PARAM_AW   = $clog2(COUT_MAX);

  localparam int NG   = 64;    // groups
  localparam int CIN  = 16;
  localparam int COUT = 32;

  logic clk = 0, rst_n = 0;
  always #5 clk = ~clk;

  logic        start_in = 0, done_out;
  logic [31:0] tile_pixels = NG * N_LANES;
  logic [11:0] cin_run  = CIN;
  logic [11:0] cout_run = COUT;
  logic [7:0]  zp_in = 8'd128, zp_out = 8'd128;
  logic        relu_en = 1'b0;

  logic [W_AW-1:0]              w_rd_addr;
  logic                         w_rd_en;
  // Model the real weight BRAM's POWER-UP STATE. xpm memories read 0 from
  // uninitialised locations; an uninitialised TB array reads X, and that X
  // reaches w_rd_data_rr -> p_reg -> acc on the very first MAC and poisons
  // every result. This was the actual cause of the earlier bench failures.
  logic signed [DATA_WIDTH-1:0] w_rd_data [0:N_OC-1] = '{default:'0};
  logic [PARAM_AW-1:0]          param_rd_addr;
  logic                         param_rd_en;
  logic signed [31:0]           param_bias_data = '0;
  logic [31:0]                  param_mult_data = '0;
  logic [7:0]                   param_shift_data = '0;

  logic [63:0] s_axis_tdata;
  logic        s_axis_tvalid, s_axis_tready, s_axis_tlast;
  logic [63:0] m_axis_tdata;
  logic        m_axis_tvalid, m_axis_tlast;
  logic        m_axis_tready;

  // ---- vectors ----
  logic [63:0] lat_mem [0:NG*CIN-1];
  logic [7:0]  w_mem   [0:COUT*CIN-1];
  logic [63:0] exp_mem [0:NG*COUT-1];

  // ---- weight BRAM model: 1-cycle registered read, bank per OC ----
  always_ff @(posedge clk) begin
    if (w_rd_en)
      for (int oc = 0; oc < N_OC; oc++)
        w_rd_data[oc] <= $signed(w_mem[oc*CIN + int'(w_rd_addr)]);
  end

  // ---- param BRAM model: the identity requantiser ----
  always_ff @(posedge clk) begin
    if (param_rd_en) begin
      param_bias_data  <= 32'sd0;
      param_mult_data  <= 32'h0001_0000;
      param_shift_data <= 8'd16;
    end
  end

  pw_single_oc_axis #(
    .USE_PW_VQ(1),                 // compiled in, but vq_mode is LOW here:
                                   // this also proves the VQ build does not
                                   // disturb ordinary convolution.
    .DATA_WIDTH(DATA_WIDTH), .ACC_WIDTH(ACC_WIDTH),
    .CIN_MAX(CIN_MAX), .COUT_MAX(COUT_MAX),
    .TILE_PIXELS_MAX(32768), .IN_FIFO_DEPTH(2048), .OUT_FIFO_DEPTH(8192),
    .S_AXIS_DATA_WIDTH(64), .N_LANES(N_LANES), .N_OC(N_OC),
    .M_AXIS_DATA_WIDTH(64)
  ) dut (
    .clk(clk), .rst_n(rst_n), .start_in(start_in), .done_out(done_out),
    .tile_pixels(tile_pixels), .cin_run(cin_run), .cout_run(cout_run),
    .zp_in(zp_in), .zp_out(zp_out), .relu_en(relu_en),
    .vq_mode(1'b0), .vq_cin_load(12'd0),
    .vq_norm_we(1'b0), .vq_norm_addr(7'd0), .vq_norm_data(20'sd0),
    .w_rd_addr(w_rd_addr), .w_rd_en(w_rd_en), .w_rd_data(w_rd_data),
    .param_rd_addr(param_rd_addr), .param_rd_en(param_rd_en),
    .param_bias_data(param_bias_data), .param_mult_data(param_mult_data),
    .param_shift_data(param_shift_data),
    .s_axis_tdata(s_axis_tdata), .s_axis_tvalid(s_axis_tvalid),
    .s_axis_tready(s_axis_tready), .s_axis_tlast(s_axis_tlast),
    .m_axis_tdata(m_axis_tdata), .m_axis_tvalid(m_axis_tvalid),
    .m_axis_tready(m_axis_tready), .m_axis_tlast(m_axis_tlast)
  );

  // ---- AXIS input driver: data/valid combinational from the pointer ----
  int beat_i;
  always_comb begin
    s_axis_tvalid = rst_n && (beat_i < NG*CIN);
    s_axis_tdata  = lat_mem[beat_i < NG*CIN ? beat_i : 0];
    s_axis_tlast  = (beat_i == NG*CIN - 1);
  end
  always_ff @(posedge clk) begin
    if (!rst_n) beat_i <= 0;
    else if (s_axis_tvalid && s_axis_tready) beat_i <= beat_i + 1;
  end

  int dbgp = 0;
  always_ff @(posedge clk) begin
    if (rst_n && dut.u_core.st == 3'd2 && dbgp < 24) begin
      $display("  P: acc=%0d p_pk=%0d w_rr=%0d a_pk_r=%0d pbpx=%0d d2=%0b fic=%0b",
               dut.u_core.acc[0][0], dut.u_core.p_packed_reg[0][0],
               dut.u_core.w_rd_data_rr[0], dut.u_core.a_packed_r[0],
               dut.u_core.pb_pixel_r[0], dut.u_core.rd_issued_d2, dut.u_core.first_ic);
      dbgp++;
    end
  end

  int dbgs = 0;
  always_ff @(posedge clk) begin
    if (rst_n && dut.u_core.shadow_copy_trig && dbgs < 10) begin
      $display("  S: trig=%0b copyidx=%0d wr_e=%0b wrbank=%0b rdbank=%0b rdaddr=%0h shrd=%0d issue=%0d acc00=%0d",
               dut.u_core.shadow_copy_trig, dut.u_core.shadow_copy_idx,
               dut.u_core.sha_wr_en_e, dut.u_core.sha_wr_bank, dut.u_core.sha_rd_bank,
               dut.u_core.sha_rd_addr, $signed(dut.u_core.sha_rd_data[23:0]),
               dut.u_core.ppu_issue_idx, dut.u_core.acc[0][0]);
      dbgs++;
    end
  end

  int dbgo = 0;
  always_ff @(posedge clk) begin
    if (rst_n && dut.core_valid_out && dbgo < 6) begin
      $display("  O: acc00=%0d ppu_acc0=%0d core_px=%016x ppu_bias=%0d ppu_mult=%0h ppu_sh=%0d",
               dut.u_core.acc[0][0], dut.u_core.ppu_acc_in[0], dut.core_pixel_out,
               dut.u_core.ppu_bias_q, dut.u_core.ppu_mult_q, dut.u_core.ppu_shift_q);
      dbgo++;
    end
  end

  // ---- AXIS output sink, always ready ----
  assign m_axis_tready = 1'b1;

  int nout = 0, nbad = 0, nlast = 0, nshift = 0;
  always_ff @(posedge clk) begin
    if (rst_n && m_axis_tvalid && m_axis_tready) begin
      // Hypothesis: the shadow double-buffer makes the drain of batch N read
      // the bank batch N-1 filled, so the stream lags by one group.
      if (nout >= COUT && nout < NG*COUT &&
          m_axis_tdata === exp_mem[nout - COUT]) nshift++;
      if (nout < NG*COUT) begin
        if (m_axis_tdata !== exp_mem[nout]) begin
          if (nbad < 6)
            $display("  MISMATCH beat %0d (group %0d, oc %0d): got %016x exp %016x",
                     nout, nout/COUT, nout%COUT, m_axis_tdata, exp_mem[nout]);
          nbad++;
        end
      end else begin
        if (nbad < 6) $display("  EXTRA beat %0d (expected only %0d)", nout, NG*COUT);
        nbad++;
      end
      if (m_axis_tlast) nlast++;
      nout++;
    end
  end

  initial begin
    $readmemh("conv_latent.hex",  lat_mem);
    $readmemh("conv_weights.hex", w_mem);
    $readmemh("conv_expect.hex",  exp_mem);

    $display("\n=== PW convolution at the AXIS level (real FIFOs, real backpressure) ===");
    $display("groups=%0d cin=%0d cout=%0d  expected beats=%0d", NG, CIN, COUT, NG*COUT);

    repeat (4) @(posedge clk);
    rst_n = 1;
    repeat (4) @(posedge clk);
    @(posedge clk) start_in <= 1;
    @(posedge clk) start_in <= 0;

    fork
      begin wait (nout >= NG*COUT); repeat (50) @(posedge clk); end
      begin
        repeat (600000) @(posedge clk);
        $display("  TIMEOUT: only %0d of %0d beats", nout, NG*COUT);
      end
    join_any

    $display("\nbeats = %0d / %0d, mismatches = %0d, tlast seen = %0d",
             nout, NG*COUT, nbad, nlast);
    $display("beats matching a ONE-GROUP-LAGGED reference: %0d / %0d",
             nshift, (NG-1)*COUT);
    if (nout == NG*COUT && nbad == 0 && nlast == 1)
      $display("RESULT: PASS -- the PW datapath is arithmetically correct here");
    else
      $display("RESULT: FAIL");
    $finish;
  end

endmodule
