`timescale 1ns/1ps
// ============================================================================
// tb_pw_axi_vq.sv -- VQ at the TOP of the IP: AXI-lite programming, the VQ
//                    stream pair, and the mux that selects it.
//
// WHAT THIS COVERS THAT NOTHING ELSE DOES
//   tb_pw_vq.sv drives the bare core and proves the VQ ARITHMETIC. It does not
//   touch the AXI-lite register file, the codeword-norm write port, the weight
//   load path, or the s_axis_vq / m_axis_vq mux that the block design is about
//   to be rewired around. Those are exactly the pieces a BD retarget depends
//   on, so they get their own check before the BD is touched.
//
//   Everything here goes through the real register map -- weights via
//   ADDR_OC_SEL/ADDR_W_BASE, norms via ADDR_VQ_NORM, mode via ADDR_VQ_CTRL --
//   so a wrong offset or a missing write pulse fails here rather than on
//   hardware.
//
// THE MUX IS CHECKED IN BOTH DIRECTIONS
//   With vq_mode = 1 the latent must be accepted on s_axis_vq and the indices
//   must leave on m_axis_vq, while the ordinary pair stays quiet: s_axis_tready
//   low and m_axis_tvalid low for the whole run. Both are asserted continuously,
//   not sampled at the end, so a mux that leaks even one beat fails.
//
// Vectors: latent.hex / weights.hex / norms.hex / expect.hex from
// gen_vq_vectors.c, read from the run directory by fixed name.
// ============================================================================
module tb_pw_axi_vq;

  localparam int DATA_WIDTH = 8;
  localparam int ACC_WIDTH  = 24;
  localparam int CIN_MAX    = 240;
  localparam int COUT_MAX   = 240;
  localparam int N_LANES    = 8;
  localparam int N_OC       = 32;
  localparam int W_PER_BANK = 64;
  localparam int VQ_DIM     = 64;
  localparam int NG         = 64;      // groups in the vector set

  localparam [11:0] ADDR_CTRL        = 12'h000;
  localparam [11:0] ADDR_TILE_PIXELS = 12'h008;
  localparam [11:0] ADDR_CIN_RUN     = 12'h00C;
  localparam [11:0] ADDR_ZP_RELU     = 12'h010;
  localparam [11:0] ADDR_OC_SEL      = 12'h024;
  localparam [11:0] ADDR_COUT_RUN    = 12'h028;
  localparam [11:0] ADDR_W_BRAM_OFF  = 12'h02C;
  localparam [11:0] ADDR_VQ_CTRL     = 12'h034;
  localparam [11:0] ADDR_VQ_NORM     = 12'h038;
  localparam [11:0] ADDR_W_BASE      = 12'h100;

  logic clk = 0, rstn = 0;
  always #5 clk = ~clk;

  // ---- AXI-lite ----
  logic [11:0] awaddr;  logic awvalid;  logic awready;
  logic [31:0] wdata;   logic [3:0] wstrb = 4'hF; logic wvalid; logic wready;
  logic [1:0]  bresp;   logic bvalid;   logic bready = 1'b1;
  logic [11:0] araddr = '0; logic arvalid = 0; logic arready;
  logic [31:0] rdata;   logic [1:0] rresp; logic rvalid; logic rready = 1'b1;

  // ---- ordinary stream pair (must stay idle in VQ mode) ----
  logic [63:0] s_tdata = '0; logic s_tvalid = 0; logic s_tready; logic s_tlast = 0;
  logic [63:0] m_tdata;      logic m_tvalid;     logic m_tready = 1'b1; logic m_tlast;

  // ---- VQ stream pair ----
  logic [63:0] sv_tdata; logic sv_tvalid; logic sv_tready; logic sv_tlast;
  logic [63:0] mv_tdata; logic mv_tvalid; logic mv_tready = 1'b1; logic mv_tlast;

  logic [63:0] latent_mem [0:VQ_DIM*1800-1];
  logic [7:0]  w_mem      [0:N_OC*W_PER_BANK-1];
  logic [19:0] norm_mem   [0:127];
  logic [63:0] expect_mem [0:4*1800-1];

  pw_single_oc_axis_axi #(
    .USE_PW_VQ(1),
    .DATA_WIDTH(DATA_WIDTH), .ACC_WIDTH(ACC_WIDTH),
    .CIN_MAX(CIN_MAX), .COUT_MAX(COUT_MAX),
    .TILE_PIXELS_MAX(32768), .IN_FIFO_DEPTH(2048), .OUT_FIFO_DEPTH(4096),
    .S_AXIS_DATA_WIDTH(64), .N_LANES(N_LANES), .N_OC(N_OC),
    .M_AXIS_DATA_WIDTH(64)
  ) dut (
    .s_axi_aclk(clk), .s_axi_aresetn(rstn),
    .s_axi_awaddr(awaddr), .s_axi_awvalid(awvalid), .s_axi_awready(awready),
    .s_axi_wdata(wdata), .s_axi_wstrb(wstrb), .s_axi_wvalid(wvalid), .s_axi_wready(wready),
    .s_axi_bresp(bresp), .s_axi_bvalid(bvalid), .s_axi_bready(bready),
    .s_axi_araddr(araddr), .s_axi_arvalid(arvalid), .s_axi_arready(arready),
    .s_axi_rdata(rdata), .s_axi_rresp(rresp), .s_axi_rvalid(rvalid), .s_axi_rready(rready),
    .s_axis_tdata(s_tdata), .s_axis_tvalid(s_tvalid), .s_axis_tready(s_tready), .s_axis_tlast(s_tlast),
    .m_axis_tdata(m_tdata), .m_axis_tvalid(m_tvalid), .m_axis_tready(m_tready), .m_axis_tlast(m_tlast),
    .s_axis_vq_tdata(sv_tdata), .s_axis_vq_tvalid(sv_tvalid),
    .s_axis_vq_tready(sv_tready), .s_axis_vq_tlast(sv_tlast),
    .m_axis_vq_tdata(mv_tdata), .m_axis_vq_tvalid(mv_tvalid),
    .m_axis_vq_tready(mv_tready), .m_axis_vq_tlast(mv_tlast)
  );

  // ---- AXI-lite single write ----
  // The shell commits a write only when all four handshake signals are high in
  // the SAME cycle:
  //     slv_reg_wren = awready && awvalid && wready && wvalid
  // so both valids must be held until that cycle and dropped together. An
  // earlier version of this task retired the two channels independently, which
  // let a write land with only one valid still asserted -- the register was
  // silently not written, and with ~2,200 programming writes per run that
  // corrupted the weight image and the norm table.
  task automatic wr(input [11:0] a, input [31:0] d);
    begin
      @(posedge clk);
      awaddr <= a; awvalid <= 1'b1; wdata <= d; wvalid <= 1'b1;
      forever begin
        @(posedge clk);
        // read back the values that were live during the cycle just ended
        if (awready && wready) break;
      end
      awvalid <= 1'b0; wvalid <= 1'b0;
      while (!bvalid) @(posedge clk);
      @(posedge clk);
    end
  endtask

  // ---- VQ input stream: one beat per channel, 64 per group ----
  // NOT presented until the engine has been started. pw_single_oc_axis.sv:88
  // resets the input FIFO on start_in (RST_STRETCH cycles), deliberately, so a
  // run cannot inherit beats from an aborted predecessor. Anything streamed
  // before start is therefore FLUSHED: an earlier version of this bench filled
  // the FIFO during AXI programming and lost the 9 beats that were sitting in
  // it, so the core resumed at latent index 10 and every result was wrong while
  // the weights and norms still verified. The real driver has the same
  // obligation -- start the engine, then kick the DMA.
  logic started = 1'b0;
  int beat_i;
  always_comb begin
    sv_tvalid = rstn && started && (beat_i < NG*VQ_DIM);
    sv_tdata  = latent_mem[beat_i < NG*VQ_DIM ? beat_i : 0];
    sv_tlast  = (beat_i == NG*VQ_DIM - 1);
  end
  always_ff @(posedge clk) begin
    if (!rstn) beat_i <= 0;
    else if (sv_tvalid && sv_tready) beat_i <= beat_i + 1;
  end

  // ---- does the weight BRAM hold what we programmed? ----
  // Checks the data the CORE actually receives against the vector file, on
  // every read, rather than poking into xpm internals.
  int wchk = 0, wbad = 0;
  logic [10:0] w_addr_d;
  logic        w_en_d;
  always_ff @(posedge clk) begin
    w_addr_d <= dut.core_w_rd_addr;
    w_en_d   <= dut.core_w_rd_en;
    if (rstn && w_en_d) begin
      for (int oc = 0; oc < N_OC; oc++) begin
        wchk++;
        if (dut.w_rd_data[oc] !== $signed(w_mem[oc*W_PER_BANK + int'(w_addr_d)])) begin
          if (wbad < 6)
            $display("  WBAD oc=%0d addr=%0d got=%0d exp=%0d",
                     oc, w_addr_d, dut.w_rd_data[oc],
                     $signed(w_mem[oc*W_PER_BANK + int'(w_addr_d)]));
          wbad++;
        end
      end
    end
  end

  // ---- is the latent stream intact where the CORE sees it? ----
  // The wrapper's input FIFO sits between s_axis_vq and the core. If a beat is
  // duplicated or dropped there, every downstream number is wrong while the
  // weights and norms still verify -- which is exactly the symptom.
  int icnt = 0, ibad = 0;
  always_ff @(posedge clk) begin
    if (rstn && dut.u_pw.core_valid_in && dut.u_pw.core_consume_in) begin
      if (icnt < NG*VQ_DIM) begin
        if (dut.u_pw.core_pixel_in !== latent_mem[icnt]) begin
          if (ibad < 6) begin
            int found = -1;
            for (int j = 0; j < NG*VQ_DIM; j++)
              if (found < 0 && dut.u_pw.core_pixel_in === latent_mem[j]) found = j;
            $display("  IBAD beat %0d: core saw %016x (= file index %0d), expected file index %0d",
                     icnt, dut.u_pw.core_pixel_in, found, icnt);
          end
          ibad++;
        end
      end
      icnt++;
    end
  end

  // ---- the ordinary pair must never move while vq_mode is set ----
  int leak_ready = 0, leak_valid = 0;
  always_ff @(posedge clk) begin
    if (rstn && dut.reg_vq_ctrl[0]) begin
      if (s_tready) leak_ready++;
      if (m_tvalid) leak_valid++;
    end
  end

  // ---- capture the VQ output ----
  int nout = 0, nbad = 0, nlast = 0;
  always_ff @(posedge clk) begin
    if (rstn && mv_tvalid && mv_tready) begin
      if (nout < 4*NG) begin
        if (mv_tdata !== expect_mem[nout]) begin
          if (nbad < 5)
            $display("  MISMATCH beat %0d: got %016x exp %016x",
                     nout, mv_tdata, expect_mem[nout]);
          nbad++;
        end
      end else begin
        if (nbad < 5) $display("  EXTRA beat %0d", nout);
        nbad++;
      end
      if (mv_tlast) nlast++;
      nout++;
    end
  end

  initial begin
    $readmemh("latent.hex",  latent_mem);
    $readmemh("weights.hex", w_mem);
    $readmemh("norms.hex",   norm_mem);
    $readmemh("expect.hex",  expect_mem);

    $display("\n=== PW-hosted VQ through the real AXI-lite register map ===");
    $display("groups=%0d  expected index beats=%0d", NG, 4*NG);

    awvalid = 0; wvalid = 0;
    repeat (8) @(posedge clk);
    rstn = 1;
    repeat (8) @(posedge clk);

    // geometry
    wr(ADDR_TILE_PIXELS, NG*N_LANES);
    wr(ADDR_CIN_RUN,     32'd16);      // MAC window per batch
    wr(ADDR_COUT_RUN,    32'd128);     // 8 sub-codebooks x 16 codewords
    wr(ADDR_ZP_RELU,     32'h0000_8080);  // zp_in = zp_out = 128, relu off

    // weights: bank per OC slot, 64 entries each
    for (int oc = 0; oc < N_OC; oc++) begin
      wr(ADDR_OC_SEL,     oc);
      wr(ADDR_W_BRAM_OFF, 32'd0);
      for (int a = 0; a < W_PER_BANK; a++)
        wr(ADDR_W_BASE + 12'(a*4), {24'd0, w_mem[oc*W_PER_BANK + a]});
    end

    // codeword norms: [6:0] = absolute OC, [31:12] = ||v_k||^2
    for (int i = 0; i < 128; i++)
      wr(ADDR_VQ_NORM, {norm_mem[i], 5'd0, i[6:0]});

    // VQ mode on: [0] = vq_mode, [23:12] = vq_cin_load
    wr(ADDR_VQ_CTRL, {8'd0, 12'd64, 11'd0, 1'b1});
    repeat (4) @(posedge clk);

    // ---- what actually landed in the engine ----
    $display("  PROG tile_pixels=%0d cin_run=%0d cout_run=%0d zp_in=%0d zp_out=%0d vq_ctrl=%08x",
             dut.reg_tile_pixels, dut.reg_cin_run[11:0], dut.reg_cout_run[11:0],
             dut.reg_zp_relu[7:0], dut.reg_zp_relu[15:8], dut.reg_vq_ctrl);
    $display("  NORM rom[0..3] = %0d %0d %0d %0d   (file: %0d %0d %0d %0d)",
             $signed(dut.u_pw.u_core.G_VQ.vq_norm[0]), $signed(dut.u_pw.u_core.G_VQ.vq_norm[1]),
             $signed(dut.u_pw.u_core.G_VQ.vq_norm[2]), $signed(dut.u_pw.u_core.G_VQ.vq_norm[3]),
             $signed(norm_mem[0]), $signed(norm_mem[1]),
             $signed(norm_mem[2]), $signed(norm_mem[3]));
    $display("  NORM rom[127] = %0d   (file: %0d)",
             $signed(dut.u_pw.u_core.G_VQ.vq_norm[127]), $signed(norm_mem[127]));

    wr(ADDR_CTRL, 32'h1);   // start -- this also flushes the input FIFO
    repeat (16) @(posedge clk);   // let fifo_rst deassert before streaming
    started = 1'b1;

    fork
      begin wait (nout >= 4*NG); repeat (200) @(posedge clk); end
      begin repeat (600000) @(posedge clk);
            $display("  TIMEOUT: %0d of %0d beats", nout, 4*NG); end
    join_any

    $display("\nindex beats = %0d / %0d, mismatches = %0d, tlast = %0d",
             nout, 4*NG, nbad, nlast);
    $display("  CORE st=%0d grp_idx=%0d tile_groups=%0d  input beats taken=%0d of %0d",
             dut.u_pw.u_core.st, dut.u_pw.u_core.grp_idx,
             dut.u_pw.u_core.tile_groups_r, beat_i, NG*VQ_DIM);
    $display("  vq_mode_r=%0b vq_cin_load_r=%0d cin_run_r=%0d cout_run_r=%0d",
             dut.u_pw.u_core.vq_mode_r, dut.u_pw.u_core.vq_cin_load_r,
             dut.u_pw.u_core.cin_run, dut.u_pw.u_core.cout_run_r);
    $display("  WEIGHT reads checked=%0d bad=%0d ; INPUT beats at core=%0d bad=%0d", wchk, wbad, icnt, ibad);
    $display("ordinary pair while vq_mode=1: s_axis_tready high %0d cyc, m_axis_tvalid high %0d cyc",
             leak_ready, leak_valid);
    if (nout == 4*NG && nbad == 0 && nlast == 1 && leak_ready == 0 && leak_valid == 0)
      $display("RESULT: PASS -- AXI-lite programming, VQ stream pair and mux all correct");
    else
      $display("RESULT: FAIL");
    $finish;
  end

endmodule
