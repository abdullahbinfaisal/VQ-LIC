`timescale 1ns/1ps
// ============================================================================
// dw_fused_axi_order_tb.sv  (2026-07-27)
//
// PURPOSE: find the ONE-WORD STREAM OFFSET seen on hardware after the 32b->64b
// AXIS widening. Board evidence (H=2048 L0 loopback, [PROBE] output):
//   got[c0]==ref[c1] 98.2%,  got[c1]==ref[c2] 97.4%,
//   got[c2]==ref[c0] of the NEXT group (w+8) 93.8%
// Channel is the fastest-varying index, so that IS "the whole stream is offset
// by exactly one 64b word": correct[0] is never emitted, everything shifts up
// one, one extra word rides at the tail. Beat counts are nonetheless EXACT
// (consumed=written=produced=C*G*H), so nothing is lost at the FIFO level.
//
// METHOD: program the kernel as an IDENTITY (centre tap = 1, bias 0, unity
// requant, zp 0) so the DW is a byte-for-byte pass-through. The output stream
// must then equal the input stream exactly. This removes the need for a golden
// convolution model -- no requant/rounding to mismatch -- and isolates ORDERING
// from ARITHMETIC completely. Any offset shows up as an exact-equality failure
// at a known word index.
//
// A/B: compile with -d TB_AXIS_32 together with the pre-widening RTL in
// backup_pre_64bit/ to run the same test through the old 32b path. The 32b leg
// is expected to PASS (hardware scored 99.08% before the widening) and the 64b
// leg to show offset=+1. That difference is the bug.
//
//   A (64b, current RTL):   no define
//   B (32b, backup RTL):    +define+TB_AXIS_32
//
// PASS = TLAST seen AND received stream == transmitted stream at offset 0.
// ============================================================================
module dw_fused_axi_order_tb;

  // ---- small config: the offset is structural, it does not need real scale ----
  localparam int C = 3;
  localparam int W = 67;                        // not a multiple of 8 -> real tail
  localparam int H = 6;
  localparam int G = (W + 7) / 8;               // 9
  localparam logic [7:0] ZP_IN  = 8'h00;
  localparam logic [7:0] ZP_OUT = 8'h00;
  localparam logic       RELU   = 1'b0;

  localparam int TOT_WORDS   = H * G * C;       // 162 64b group-words
  localparam int TIMEOUT_CYC = 400000;

  // identity requant: out = ((x * MULT) >> SHIFT) + ZP_OUT, MULT = 1<<SHIFT
  localparam int         SHIFT_V = 16;
  localparam logic [31:0] MULT_V = 32'd1 << SHIFT_V;

  localparam [11:0] R_CTRL=12'h00, R_STATUS=12'h04, R_CIN=12'h08, R_NG=12'h0C,
                    R_ZP=12'h10, R_CH=12'h14, R_W0=12'h18, R_W1=12'h1C, R_W2=12'h20,
                    R_BIAS=12'h24, R_MULT=12'h28, R_SHIFT=12'h2C, R_IMGW=12'h30,
                    R_NROWS=12'h34, R_DBG_CONS=12'h38, R_DBG_WR=12'h3C,
                    R_DBG_PROD=12'h40, R_DBG_WIN=12'h44, R_DBG_FLAGS=12'h48;

  logic clk = 0, rstn = 0;
  always #5 clk = ~clk;

  logic [11:0] awaddr; logic awvalid, awready;
  logic [31:0] wdata;  logic [3:0] wstrb; logic wvalid, wready;
  logic [1:0]  bresp;  logic bvalid, bready;
  logic [11:0] araddr; logic arvalid, arready;
  logic [31:0] rdata;  logic [1:0] rresp; logic rvalid, rready;

`ifdef TB_AXIS_32
  localparam int AXIS_W = 32;
`else
  localparam int AXIS_W = 64;
`endif
  logic [AXIS_W-1:0] s_tdata; logic s_tvalid, s_tready, s_tlast;
  logic [AXIS_W-1:0] m_tdata; logic m_tvalid, m_tready, m_tlast;

  logic start_pulse_out;
  logic [31:0] tmp;
  logic timed_out = 0;

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
  // AXI-Lite
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
  // Expected stream: mirrors pack_input_group_major() in main.c --
  // for h, for g, for c: 8 lanes, lane l = column g*8+l, ZP_IN past W.
  // Each byte encodes its own word index so a mismatch is self-describing.
  // ------------------------------------------------------------------------
  logic [63:0] exp_word [0:TOT_WORDS-1];
  logic [63:0] rx_word  [0:TOT_WORDS+8];
  int rx_count = 0;

  function automatic [7:0] px(input int k, input int lane);
    // distinctive, non-repeating within a 32-word span, never 8'h00 so a
    // dropped/zero word cannot masquerade as valid data
    px = 8'h01 + (((k * 8) + lane) % 8'hFE);
  endfunction

  initial begin
    int k;
    k = 0;
    for (int h = 0; h < H; h++)
      for (int g = 0; g < G; g++)
        for (int c = 0; c < C; c++) begin
          for (int l = 0; l < 8; l++) begin
            int col = g*8 + l;
            exp_word[k][l*8 +: 8] = (col < W) ? px(k, l) : ZP_IN;
          end
          k++;
        end
  end

  // ------------------------------------------------------------------------
  // Input driver
  // ------------------------------------------------------------------------
  bit input_go = 0;
  int in_words = 0;

  initial begin
    s_tvalid = 1'b0; s_tdata = '0; s_tlast = 1'b0;
    wait (input_go);
    for (int k = 0; k < TOT_WORDS; k++) begin
`ifdef TB_AXIS_32
      for (int half = 0; half < 2; half++) begin
        s_tdata  = exp_word[k][half*32 +: 32];
        s_tvalid = 1'b1;
        s_tlast  = (k == TOT_WORDS-1) && (half == 1);
        @(posedge clk);
        while (!s_tready) @(posedge clk);
      end
`else
      s_tdata  = exp_word[k];
      s_tvalid = 1'b1;
      s_tlast  = (k == TOT_WORDS-1);
      @(posedge clk);
      while (!s_tready) @(posedge clk);
`endif
      in_words++;
    end
    s_tvalid = 1'b0; s_tlast = 1'b0;
  end

  // ------------------------------------------------------------------------
  // Output sink -- reassemble 64b words (32b leg takes two beats per word)
  // ------------------------------------------------------------------------
  int  out_beats = 0;
  bit  got_tlast = 0;
`ifdef TB_AXIS_32
  bit         rx_half = 0;
  logic [31:0] rx_lo;
`endif

  always @(posedge clk) begin
    if (!rstn) begin
      out_beats <= 0; got_tlast <= 0; rx_count <= 0;
`ifdef TB_AXIS_32
      rx_half <= 0;
`endif
    end else if (m_tvalid && m_tready) begin
      out_beats <= out_beats + 1;
      if (m_tlast) got_tlast <= 1'b1;
`ifdef TB_AXIS_32
      if (rx_half == 0) begin
        rx_lo   <= m_tdata;
        rx_half <= 1;
      end else begin
        if (rx_count <= TOT_WORDS+8) rx_word[rx_count] <= {m_tdata, rx_lo};
        rx_count <= rx_count + 1;
        rx_half  <= 0;
      end
`else
      if (rx_count <= TOT_WORDS+8) rx_word[rx_count] <= m_tdata;
      rx_count <= rx_count + 1;
`endif
    end
  end

  task automatic dump_dbg;
    logic [31:0] dc, dw, dp, ws, fl;
    begin
      axil_read(R_DBG_CONS,  dc);
      axil_read(R_DBG_WR,    dw);
      axil_read(R_DBG_PROD,  dp);
      axil_read(R_DBG_WIN,   ws);
      axil_read(R_DBG_FLAGS, fl);
      $display("[TB] DBG consumed=%0d written=%0d produced=%0d (expect %0d words)",
               dc, dw, dp, TOT_WORDS);
      $display("[TB] DBG win_state=0x%08X (running=%0d draining=%0d r_cnt=%0d g_cnt=%0d c_cnt=%0d)",
               ws, ws[31], ws[30], ws[29:24], ws[23:12], ws[11:0]);
      $display("[TB] DBG flags=0x%08X (core_done=%0d all_written=%0d out_full_ever=%0d)",
               fl, fl[0], fl[1], fl[2]);
    end
  endtask

  // ------------------------------------------------------------------------
  // Offset analysis -- the actual measurement
  // ------------------------------------------------------------------------
  task automatic analyse;
    int best_off, best_hits, hits, n, first_bad;
    begin
      n = (rx_count < TOT_WORDS) ? rx_count : TOT_WORDS;
      best_off = 0; best_hits = -1;
      for (int off = -2; off <= 2; off++) begin
        hits = 0;
        for (int i = 0; i < n; i++) begin
          if ((i + off >= 0) && (i + off < TOT_WORDS))
            if (rx_word[i] === exp_word[i + off]) hits++;
        end
        $display("[TB] offset %+0d : %0d/%0d words match", off, hits, n);
        if (hits > best_hits) begin best_hits = hits; best_off = off; end
      end
      $display("[TB] BEST OFFSET = %+0d (%0d/%0d)", best_off, best_hits, n);

      first_bad = -1;
      for (int i = 0; i < n; i++)
        if ((first_bad < 0) && (rx_word[i] !== exp_word[i])) first_bad = i;

      if (first_bad >= 0) begin
        $display("[TB] first mismatch at word %0d  (row %0d group %0d chan %0d)",
                 first_bad, first_bad/(G*C), (first_bad/C)%G, first_bad%C);
        for (int i = (first_bad > 2 ? first_bad-2 : 0); i < first_bad+4 && i < n; i++)
          $display("[TB]   word %0d: rx=%016h  exp=%016h%s",
                   i, rx_word[i], exp_word[i],
                   (rx_word[i] === exp_word[i]) ? "" : "   <-- MISMATCH");
      end

      if (best_off == 0 && best_hits == n && got_tlast)
        $display("[TB] ===== PASS: stream identical, no offset =====");
      else if (best_off != 0)
        $display("[TB] ===== FAIL: stream OFFSET BY %+0d WORDS =====", best_off);
      else
        $display("[TB] ===== FAIL: no clean offset -- data corrupt, not displaced =====");
    end
  endtask

  // ------------------------------------------------------------------------
  // Main
  // ------------------------------------------------------------------------
  initial begin
    awvalid=0; wvalid=0; bready=0; arvalid=0; rready=0;
    awaddr=0; wdata=0; wstrb=0; araddr=0;
    m_tready = 1'b1;

    rstn = 1'b0;
    repeat (10) @(posedge clk);
    rstn = 1'b1;
    repeat (5) @(posedge clk);

    $display("[TB] AXIS width = %0d, C=%0d W=%0d H=%0d G=%0d, %0d words",
             AXIS_W, C, W, H, G, TOT_WORDS);
    $display("[TB] IDENTITY kernel (centre tap=1, bias=0, mult=%0d shift=%0d, zp=0)",
             MULT_V, SHIFT_V);

    // identity kernel on every channel: w = 0 0 0 / 0 1 0 / 0 0 0
    for (int c = 0; c < C; c++) begin
      axil_write(R_CH,    c);
      axil_write(R_W0,    32'h00000000);   // {w3,w2,w1,w0} = 0,0,0,0
      axil_write(R_W1,    32'h00000001);   // {w7,w6,w5,w4} = 0,0,0,1  -> w4 = 1
      axil_write(R_W2,    32'h00000000);   // w8 = 0 -> commits the 9 weights
      axil_write(R_BIAS,  32'd0);
      axil_write(R_MULT,  MULT_V);
      axil_write(R_SHIFT, SHIFT_V);        // commits {bias,mult,shift}
    end

    axil_write(R_CIN,   C);
    axil_write(R_NG,    G);
    axil_write(R_IMGW,  W);
    axil_write(R_NROWS, H);
    axil_write(R_ZP,    {15'd0, RELU, ZP_OUT, ZP_IN});

    axil_read(R_CIN,   tmp); $display("[TB] readback CIN=%0d (exp %0d)",   tmp, C);
    axil_read(R_NG,    tmp); $display("[TB] readback NG=%0d (exp %0d)",    tmp, G);
    axil_read(R_IMGW,  tmp); $display("[TB] readback IMGW=%0d (exp %0d)",  tmp, W);
    axil_read(R_NROWS, tmp); $display("[TB] readback NROWS=%0d (exp %0d)", tmp, H);

    axil_write(R_CTRL, 32'h1);
    axil_write(R_CTRL, 32'h0);

    input_go = 1'b1;

    fork
      begin : watchdog
        repeat (TIMEOUT_CYC) @(posedge clk);
        timed_out = 1'b1;
      end
    join_none
    wait (got_tlast || timed_out);
    disable watchdog;

    repeat (8) @(posedge clk);

    if (!got_tlast)
      $display("[TB] HANG: no TLAST. out_beats=%0d rx_words=%0d", out_beats, rx_count);
    else
      $display("[TB] TLAST seen. out_beats=%0d rx_words=%0d (exp %0d)",
               out_beats, rx_count, TOT_WORDS);

    dump_dbg();
    analyse();
    $finish;
  end

endmodule
`default_nettype wire
