`timescale 1ns/1ps
// ============================================================================
// dw_fused_axi_tb.sv  (2026-07-25)
// Integration TB: drive dw_fused_axi ONLY through its AXI-Lite + AXIS ports,
// exactly like the ARM + MM2S/S2MM DMAs do on hardware. Reproduces the real
// Stage-1 loopback sequence at the REAL failing scale:
//     C=3, G=180, W=1435, H=4, MAX_CG_PRODUCT=10800
// i.e. the config the standalone windower TB (dw_banked_window_8x_tb) never
// covered -- it ran windower-only at G=3/W=19/MAX_CG_PRODUCT=12, and never
// touched the FIFOs, the throttle, the hold register, the TLAST path, or the
// AXI-Lite config/weight-load path.
//
// WHY: on hardware the core consumes input (MM2S completes) but emits ZERO
// output and never asserts TLAST -> S2MM times out. Does that reproduce in
// behavioral sim?
//   - Reproduces here  -> integration/scale RTL bug: debug it in the waveform,
//                         stop burning FPGA rebuilds.
//   - Passes here      -> sim-vs-synth divergence (BRAM read/write collision
//                         the override comment flagged): only the on-chip HW
//                         debug regs / a post-synth timing sim can catch it.
//
// PASS = m_axis emits EXP_OUT_BEATS beats AND asserts TLAST.
// HANG = TLAST never arrives; TB dumps the SAME on-chip debug registers the
//        board prints (consumed/written/produced/win_state/flags).
//
// To run: add to sim_1, set dw_fused_axi_tb as simulation top, launch xsim.
// Behavioral models of xpm_fifo_sync / inferred BRAM are used (see caveat).
// ============================================================================
module dw_fused_axi_tb;

  // ---- config under test (mirror the H=4 board run; set H=1 for that case) ----
  localparam int C = 3;
  localparam int W = 1435;
  localparam int H = 4;
  localparam int G = (W + 7) / 8;               // 180
  localparam logic [7:0] ZP_IN  = 8'h00;        // ZP_RELU=0x00004600
  localparam logic [7:0] ZP_OUT = 8'h46;
  localparam logic       RELU   = 1'b0;

  localparam int TOT_IN_BEATS  = H * G * C * 2;  // 4320 (2x 32b beats / group)
  localparam int EXP_OUT_BEATS = H * G * C * 2;  // DW is 1:1 spatial at stride 1
  localparam int TIMEOUT_CYC   = 800000;

  // register map (matches dw_fused_axi.sv)
  localparam [11:0] R_CTRL=12'h00, R_STATUS=12'h04, R_CIN=12'h08, R_NG=12'h0C,
                    R_ZP=12'h10, R_CH=12'h14, R_W0=12'h18, R_W1=12'h1C, R_W2=12'h20,
                    R_BIAS=12'h24, R_MULT=12'h28, R_SHIFT=12'h2C, R_IMGW=12'h30,
                    R_NROWS=12'h34, R_DBG_CONS=12'h38, R_DBG_WR=12'h3C,
                    R_DBG_PROD=12'h40, R_DBG_WIN=12'h44, R_DBG_FLAGS=12'h48;

  // ---- clock / reset ----
  logic clk = 0;
  logic rstn = 0;
  always #5 clk = ~clk;                          // 100 MHz

  // ---- AXI-Lite ----
  logic [11:0] awaddr; logic awvalid; logic awready;
  logic [31:0] wdata;  logic [3:0] wstrb; logic wvalid; logic wready;
  logic [1:0]  bresp;  logic bvalid; logic bready;
  logic [11:0] araddr; logic arvalid; logic arready;
  logic [31:0] rdata;  logic [1:0] rresp; logic rvalid; logic rready;

  // ---- AXIS ----
  logic [31:0] s_tdata; logic s_tvalid; logic s_tready; logic s_tlast;
  logic [31:0] m_tdata; logic m_tvalid; logic m_tready; logic m_tlast;

  logic start_pulse_out;

  logic [31:0] tmp;
  logic        timed_out = 0;

  // ---- DUT ----
  dw_fused_axi dut (
    .s_axi_aclk(clk), .s_axi_aresetn(rstn),
    .s_axi_awaddr(awaddr), .s_axi_awvalid(awvalid), .s_axi_awready(awready),
    .s_axi_wdata(wdata), .s_axi_wstrb(wstrb), .s_axi_wvalid(wvalid), .s_axi_wready(wready),
    .s_axi_bresp(bresp), .s_axi_bvalid(bvalid), .s_axi_bready(bready),
    .s_axi_araddr(araddr), .s_axi_arvalid(arvalid), .s_axi_arready(arready),
    .s_axi_rdata(rdata), .s_axi_rresp(rresp), .s_axi_rvalid(rvalid), .s_axi_rready(rready),
    .s_axis_tdata(s_tdata), .s_axis_tvalid(s_tvalid), .s_axis_tready(s_tready), .s_axis_tlast(s_tlast),
    .m_axis_tdata(m_tdata), .m_axis_tvalid(m_tvalid), .m_axis_tready(m_tready), .m_axis_tlast(m_tlast),
    .start_pulse_out(start_pulse_out)
  );

  // ------------------------------------------------------------------------
  // AXI-Lite master tasks
  // ------------------------------------------------------------------------
  task automatic axil_write(input [11:0] addr, input [31:0] data);
    begin
      @(posedge clk);
      awaddr <= addr; awvalid <= 1'b1;
      wdata  <= data; wstrb <= 4'hF; wvalid <= 1'b1;
      bready <= 1'b1;
      do @(posedge clk); while (!(awready && wready));
      awvalid <= 1'b0; wvalid <= 1'b0;
      do @(posedge clk); while (!bvalid);
      bready <= 1'b0;
    end
  endtask

  task automatic axil_read(input [11:0] addr, output [31:0] data);
    begin
      @(posedge clk);
      araddr <= addr; arvalid <= 1'b1; rready <= 1'b1;
      do @(posedge clk); while (!arready);
      arvalid <= 1'b0;
      do @(posedge clk); while (!rvalid);
      data = rdata;
      rready <= 1'b0;
    end
  endtask

  // ------------------------------------------------------------------------
  // Output sink: model S2MM (always ready), count beats, catch TLAST
  // ------------------------------------------------------------------------
  int out_beats = 0;
  bit got_tlast = 0;
  always @(posedge clk) begin
    if (!rstn) begin
      out_beats <= 0;
      got_tlast <= 0;
    end else if (m_tvalid && m_tready) begin
      out_beats <= out_beats + 1;
      if (m_tlast) got_tlast <= 1'b1;
    end
  end

  // count accepted input beats (observability only)
  int in_beats = 0;
  always @(posedge clk) begin
    if (!rstn)                       in_beats <= 0;
    else if (s_tvalid && s_tready)   in_beats <= in_beats + 1;
  end

  // group-major input word generator. beat b -> group gc=b/2, half=b%2.
  // Values are a readable byte pattern; irrelevant to whether the core hangs.
  function automatic [31:0] gen_word(input int b);
    logic [7:0] byte0, byte1, byte2, byte3;
    int base;
    begin
      base  = b * 4;
      byte0 = (base + 0) & 8'hFF;
      byte1 = (base + 1) & 8'hFF;
      byte2 = (base + 2) & 8'hFF;
      byte3 = (base + 3) & 8'hFF;
      gen_word = {byte3, byte2, byte1, byte0};
    end
  endfunction

  bit input_go = 0;

  // continuous input streamer (holds tvalid high across beats; only advances
  // when tready). Started after start_pulse, mirroring main.c ordering.
  initial begin
    s_tvalid = 1'b0; s_tdata = 32'd0; s_tlast = 1'b0;
    wait (input_go);
    for (int b = 0; b < TOT_IN_BEATS; b++) begin
      s_tdata  = gen_word(b);
      s_tvalid = 1'b1;
      s_tlast  = (b == TOT_IN_BEATS-1);     // core ignores this, but drive it
      @(posedge clk);
      while (!s_tready) @(posedge clk);      // hold until accepted
    end
    s_tvalid = 1'b0; s_tlast = 1'b0;
  end

  // ------------------------------------------------------------------------
  // Debug-register dump (same fields the board prints)
  // ------------------------------------------------------------------------
  task automatic dump_dbg;
    logic [31:0] dc, dw, dp, ws, fl;
    begin
      axil_read(R_DBG_CONS,  dc);
      axil_read(R_DBG_WR,    dw);
      axil_read(R_DBG_PROD,  dp);
      axil_read(R_DBG_WIN,   ws);
      axil_read(R_DBG_FLAGS, fl);
      $display("[TB] DBG consumed=%0d written=%0d produced=%0d", dc, dw, dp);
      $display("[TB] DBG win_state=0x%08X (running=%0d draining=%0d r_cnt=%0d g_cnt=%0d c_cnt=%0d)",
               ws, ws[31], ws[30], ws[29:24], ws[23:12], ws[11:0]);
      $display("[TB] DBG flags=0x%08X (core_done_seen=%0d all_written=%0d out_full_ever=%0d out_prog_full=%0d in_empty=%0d)",
               fl, fl[0], fl[1], fl[2], fl[3], fl[4]);
    end
  endtask

  // ------------------------------------------------------------------------
  // Main sequence
  // ------------------------------------------------------------------------
  initial begin
    awvalid=0; wvalid=0; bready=0; arvalid=0; rready=0;
    awaddr=0; wdata=0; wstrb=0; araddr=0;
    m_tready = 1'b1;                          // S2MM keeps up

    rstn = 1'b0;
    repeat (10) @(posedge clk);
    rstn = 1'b1;
    repeat (5) @(posedge clk);

    // per-channel weights/params (real L0 K0 values; values don't affect hang)
    for (int c = 0; c < C; c++) begin
      axil_write(R_CH,   c);
      axil_write(R_W0,   32'hC4FE1004);       // {w3,w2,w1,w0} = {-60,-2,16,4}
      axil_write(R_W1,   32'hD6800AE5);       // {w7,w6,w5,w4} = {-42,-128,10,-27}
      axil_write(R_W2,   32'h0000000B);       // w8=11 -> commits 9 weights
      axil_write(R_BIAS, 32'hFFFFE7BF);       // -6209
      axil_write(R_MULT, 32'd4029303);
      axil_write(R_SHIFT,32'd31);             // -> commits {bias,mult,shift}
    end

    // config
    axil_write(R_CIN,   C);
    axil_write(R_NG,    G);
    axil_write(R_IMGW,  W);
    axil_write(R_NROWS, H);
    axil_write(R_ZP,    {15'd0, RELU, ZP_OUT, ZP_IN});

    // readback sanity (mirrors the board's [DWF-L0] readback line)
    axil_read(R_CIN,   tmp); $display("[TB] readback CIN=%0d (exp %0d)",   tmp, C);
    axil_read(R_NG,    tmp); $display("[TB] readback NG=%0d (exp %0d)",     tmp, G);
    axil_read(R_IMGW,  tmp); $display("[TB] readback IMGW=%0d (exp %0d)",   tmp, W);
    axil_read(R_NROWS, tmp); $display("[TB] readback NROWS=%0d (exp %0d)",  tmp, H);

    axil_read(R_STATUS, tmp);
    $display("[TB] pre-start STATUS=0x%08X (done=%0d busy=%0d)", tmp, tmp[0], tmp[1]);

    // pulse start (bit0), like main.c
    axil_write(R_CTRL, 32'h1);
    axil_write(R_CTRL, 32'h0);
    axil_read(R_STATUS, tmp);
    $display("[TB] post-start STATUS=0x%08X (busy=%0d) -- expect busy=1", tmp, tmp[1]);

    // begin streaming input
    input_go = 1'b1;

    // wait for TLAST or timeout
    fork
      begin : watchdog
        repeat (TIMEOUT_CYC) @(posedge clk);
        timed_out = 1'b1;
      end
    join_none
    wait (got_tlast || timed_out);
    disable watchdog;

    repeat (4) @(posedge clk);
    if (got_tlast)
      $display("[TB] PASS: TLAST seen. out_beats=%0d (exp %0d), in_beats=%0d/%0d",
               out_beats, EXP_OUT_BEATS, in_beats, TOT_IN_BEATS);
    else
      $display("[TB] HANG: no TLAST (timeout). out_beats=%0d (exp %0d), in_beats=%0d/%0d",
               out_beats, EXP_OUT_BEATS, in_beats, TOT_IN_BEATS);

    axil_read(R_STATUS, tmp);
    $display("[TB] final STATUS=0x%08X (done=%0d busy=%0d)", tmp, tmp[0], tmp[1]);
    dump_dbg();

    $finish;
  end

endmodule
`default_nettype wire
