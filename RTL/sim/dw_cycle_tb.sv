// ============================================================================
// dw_cycle_tb.sv -- cycle-count the DW engine across a configuration sweep.
//
// PURPOSE. Validate T_DW = (H+1)*(c_in*(G+delta_flush) + delta_row) against
// silicon RTL. The model was written from the equations alone; this bench is
// the independent measurement. A disagreement is the result -- nothing here
// may be tuned to make them agree.
//
// TWO DUTs, SAME STIMULUS, INDEPENDENT WINDOWS.
//   A  dw_fused_core   the engine. PRIMARY.
//   B  dw_fused_axis   the same core inside its 2048-deep AXIS FIFOs.
//      Reported as a separate column so the shell's contribution is visible
//      rather than folded into the engine's number.
//
// THE MEASUREMENT WINDOW, stated exactly because the report has to.
//   core:  first cycle with (valid_in && consume_in)  -- the first input beat
//          the core ACCEPTS, not the first offered --
//          to the last cycle with valid_out high.  Inclusive: last - first + 1.
//   axis:  first cycle with (s_axis_tvalid && s_axis_tready)
//          to the last cycle with (m_axis_tvalid && m_axis_tready).
//
// THE BENCH MUST NOT BE THE BOTTLENECK, or it measures itself. Input is
// offered every cycle until the beat count is met; output is accepted every
// cycle (m_axis_tready tied high). dw_fused_core has no backpressure input at
// all, so at the core level there is nothing to get wrong; at the AXIS level a
// lazy consumer would let out_prog_full throttle core_valid_in and inflate the
// number, which is exactly why tready is tied high.
//
// CONFIGURATIONS come from sim/dw_configs.txt at RUN time, so the sweep can
// change without re-elaboration. Format, one per line, '#' comments ignored:
//     H  W  c_in  stride2
//
// CAPACITY. dw_banked_window_8x has NO BOUNDS CHECK on its line buffers:
// exceeding MAX_CG_PRODUCT=2048 makes slot_idx wrap and the buffers silently
// alias -- wrong data, no error, and a timing number that means nothing. The
// bench refuses any config with n_groups*cin_run > 2048 and says so.
// ============================================================================
`timescale 1ns / 1ps

module dw_cycle_tb;

  localparam int CW             = 64;      // core / AXIS data width
  localparam int MAX_CG_PRODUCT = 2048;    // dw_banked_window_8x line buffers
  localparam int TIMEOUT_CYC    = 20_000_000;
  localparam int QUIET_CYC      = 256;     // silence that ends a run

  // identity kernel: w4 = 1 (centre tap), everything else 0
  localparam logic [71:0] W_IDENTITY = 72'h00_00_00_00_01_00_00_00_00;
  localparam int          SHIFT_V    = 16;
  localparam logic [31:0] MULT_V     = 32'd1 << SHIFT_V;

  logic clk = 0, rst_n = 0;
  always #5 clk = ~clk;                    // 100 MHz

  // ---- shared configuration ------------------------------------------------
  logic        start_in;
  logic [11:0] cin_run, n_groups, img_width, n_rows;
  logic        stride2;
  logic [7:0]  zp_in, zp_out;
  logic        relu_en;
  logic        w_wr_en, p_wr_en;
  logic [11:0] w_wr_ch, p_wr_ch;
  logic [71:0] w_wr_data;
  logic signed [31:0] p_bias;
  logic [31:0] p_mult;
  logic [7:0]  p_shift;

  // ---- DUT A: the core ----------------------------------------------------
  logic [CW-1:0] a_pixel_in,  a_pixel_out;
  logic          a_valid_in,  a_consume_in, a_valid_out, a_done;

  dw_fused_core #(.DATA_WIDTH(8), .ACC_WIDTH(32), .CIN_MAX(240)) u_core (
    .clk(clk), .rst_n(rst_n), .start_in(start_in), .done_out(a_done),
    .cin_run(cin_run), .n_groups(n_groups), .img_width(img_width),
    .n_rows(n_rows), .stride2(stride2), .zp_in(zp_in), .zp_out(zp_out),
    .relu_en(relu_en),
    .pixel_in(a_pixel_in), .valid_in(a_valid_in), .consume_in(a_consume_in),
    .pixel_out(a_pixel_out), .valid_out(a_valid_out),
    .dbg_win_state(), .dbg_win_cfg(),
    .w_wr_en(w_wr_en), .w_wr_ch(w_wr_ch), .w_wr_data(w_wr_data),
    .p_wr_en(p_wr_en), .p_wr_ch(p_wr_ch), .p_bias(p_bias),
    .p_mult(p_mult), .p_shift(p_shift)
  );

  // ---- DUT B: the same core inside the AXIS shell --------------------------
  logic [CW-1:0] b_sdata, b_mdata;
  logic          b_svalid, b_sready, b_mvalid, b_mready, b_mlast, b_done;

  dw_fused_axis #(.DATA_WIDTH(8), .ACC_WIDTH(32), .CIN_MAX(240),
                  .IN_FIFO_DEPTH(2048), .OUT_FIFO_DEPTH(2048)) u_axis (
    .clk(clk), .rst_n(rst_n), .start_in(start_in), .done_out(b_done),
    .cin_run(cin_run), .n_groups(n_groups), .img_width(img_width),
    .n_rows(n_rows), .stride2(stride2), .zp_in(zp_in), .zp_out(zp_out),
    .relu_en(relu_en),
    .w_wr_en(w_wr_en), .w_wr_ch(w_wr_ch), .w_wr_data(w_wr_data),
    .p_wr_en(p_wr_en), .p_wr_ch(p_wr_ch), .p_bias(p_bias),
    .p_mult(p_mult), .p_shift(p_shift),
    .s_axis_tdata(b_sdata), .s_axis_tvalid(b_svalid),
    .s_axis_tready(b_sready), .s_axis_tlast(1'b0),
    .m_axis_tdata(b_mdata), .m_axis_tvalid(b_mvalid),
    .m_axis_tready(b_mready), .m_axis_tlast(b_mlast),
    .dbg_consumed(), .dbg_written(), .dbg_produced(),
    .dbg_win_state(), .dbg_flags(), .dbg_win_cfg()
  );

  // ---- run state ----------------------------------------------------------
  int unsigned cyc;                 // free-running, zeroed at each start pulse
  longint      a_first, a_last, b_first, b_last;
  int unsigned a_sent, b_sent, a_out, b_out;
  int unsigned tot_beats;
  bit          running;
  int unsigned quiet;
  bit          a_done_seen, b_done_seen;

  assign a_pixel_in = {8{8'hA5}};
  assign b_sdata    = {8{8'h5A}};
  assign b_mready   = 1'b1;                       // never backpressure
  assign a_valid_in = running && (a_sent < tot_beats);
  assign b_svalid   = running && (b_sent < tot_beats);

  always_ff @(posedge clk) begin
    if (!rst_n) begin
      cyc <= 0; a_sent <= 0; b_sent <= 0; a_out <= 0; b_out <= 0;
      a_first <= -1; a_last <= -1; b_first <= -1; b_last <= -1; quiet <= 0;
      a_done_seen <= 0; b_done_seen <= 0;
    end else if (running) begin
      cyc <= cyc + 1;

      if (a_valid_in && a_consume_in) begin
        a_sent <= a_sent + 1;
        if (a_first < 0) a_first <= cyc;
      end
      if (a_valid_out) begin a_out <= a_out + 1; a_last <= cyc; end

      if (b_svalid && b_sready) begin
        b_sent <= b_sent + 1;
        if (b_first < 0) b_first <= cyc;
      end
      if (b_mvalid && b_mready) begin b_out <= b_out + 1; b_last <= cyc; end

      // a run ends after both cores go quiet
      // done_out is a ONE-CYCLE PULSE (done_sr[TOT_LAT-1]), not a sticky
      // level, so it has to be latched -- testing it live can never
      // coincide with the quiet window and the run spins to timeout.
      if (a_done) a_done_seen <= 1'b1;
      if (b_done) b_done_seen <= 1'b1;

      if (a_valid_out || (b_mvalid && b_mready)) quiet <= 0;
      else                                       quiet <= quiet + 1;
    end
  end

  // ---- helpers ------------------------------------------------------------
  task automatic do_reset();
    begin
      running = 0; start_in = 0; rst_n = 0;
      w_wr_en = 0; p_wr_en = 0; w_wr_ch = 0; p_wr_ch = 0;
      w_wr_data = '0; p_bias = 0; p_mult = MULT_V; p_shift = SHIFT_V;
      repeat (32) @(posedge clk);
      rst_n = 1;
      repeat (16) @(posedge clk);
    end
  endtask

  task automatic load_channel(input int ch);
    begin
      @(posedge clk);
      w_wr_ch <= ch[11:0]; w_wr_data <= W_IDENTITY; w_wr_en <= 1'b1;
      @(posedge clk); w_wr_en <= 1'b0;
      p_wr_ch <= ch[11:0]; p_bias <= 32'sd0; p_mult <= MULT_V;
      p_shift <= SHIFT_V[7:0]; p_wr_en <= 1'b1;
      @(posedge clk); p_wr_en <= 1'b0;
    end
  endtask

  // ---- sweep --------------------------------------------------------------
  int fd, code, nrun, nskip;
  string line;
  int H, W, C, S, G;
  longint a_cyc, b_cyc;

  initial begin
    nrun = 0; nskip = 0;
    zp_in = 8'h00; zp_out = 8'h00; relu_en = 1'b0;

    fd = $fopen("dw_configs.txt", "r");
    if (fd == 0) begin
      $display("[TB] FATAL: cannot open dw_configs.txt");
      $finish;
    end

    $display("DWCSV,h,w,cin,stride,G,in_beats,core_cycles,axis_cycles,axis_minus_core,core_out_beats,axis_out_beats,status");

    while (!$feof(fd)) begin
      code = $fgets(line, fd);
      if (code <= 0) continue;
      if ($sscanf(line, "%d %d %d %d", H, W, C, S) != 4) continue;

      G = (W + 7) / 8;                       // ceiling, matches the RTL's port

      if (G * C > MAX_CG_PRODUCT) begin
        $display("DWCSV,%0d,%0d,%0d,%0d,%0d,0,0,0,0,0,0,SKIP_CG_%0d_OVER_%0d",
                 H, W, C, S, G, G*C, MAX_CG_PRODUCT);
        nskip++;
        continue;
      end

      do_reset();
      for (int ch = 0; ch < C; ch++) load_channel(ch);

      @(posedge clk);
      cin_run   <= C[11:0];
      n_groups  <= G[11:0];
      img_width <= W[11:0];
      n_rows    <= H[11:0];
      stride2   <= (S == 2);
      tot_beats  = H * C * G;
      @(posedge clk);

      // start pulse; counters clear on the same edge the run begins
      running = 1;
      start_in <= 1'b1;
      @(posedge clk);
      start_in <= 1'b0;

      // wait for both to finish, or time out
      begin
        int unsigned guard;
        guard = 0;
        while (guard < TIMEOUT_CYC) begin
          @(posedge clk);
          guard++;
          if (a_done_seen && b_done_seen && (quiet > QUIET_CYC)) break;
          // fallback: if a done pulse is ever missed the run still ends
          // once the input is fully consumed and both outputs are quiet.
          if ((a_sent == tot_beats) && (b_sent == tot_beats)
              && (quiet > 16 * QUIET_CYC)) break;
        end

        a_cyc = (a_first >= 0 && a_last >= a_first) ? (a_last - a_first + 1) : -1;
        b_cyc = (b_first >= 0 && b_last >= b_first) ? (b_last - b_first + 1) : -1;

        $display("DWCSV,%0d,%0d,%0d,%0d,%0d,%0d,%0d,%0d,%0d,%0d,%0d,%s",
                 H, W, C, S, G, tot_beats, a_cyc, b_cyc,
                 (a_cyc > 0 && b_cyc > 0) ? (b_cyc - a_cyc) : 0,
                 a_out, b_out,
                 (guard >= TIMEOUT_CYC)         ? "TIMEOUT" :
                 (!a_done_seen || !b_done_seen) ? "NO_DONE" :
                 (a_sent != tot_beats)  ? "INPUT_SHORT" : "OK");
        nrun++;
      end
      running = 0;
    end

    $fclose(fd);
    $display("[TB] %0d configurations run, %0d skipped for capacity", nrun, nskip);
    $finish;
  end

endmodule
