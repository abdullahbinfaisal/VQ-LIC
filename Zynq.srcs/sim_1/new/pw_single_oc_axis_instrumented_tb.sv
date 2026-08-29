`timescale 1ns/1ps
// ============================================================================
// pw_single_oc_axis_tb.sv  (2026-07-27)
//
// WHY: in the on-chip DW->PW cascade, PW stops emitting partway through a run
// and hangs forever. Measured on hardware, with the S2MM DMA healthy and still
// armed (SR=0, Halted=0) -- i.e. the DMA would take more, PW has nothing to give:
//
//     H    tile_groups   total beats   emitted    outcome
//     8      1,440          43,200      43,200    COMPLETE
//     16     2,880          86,400      46,848    stall at 54%
//     64    11,520         345,600      65,280    stall at 19%
//
// The stall point is NOT fixed and NOT a constant fraction, so it is neither a
// hard counter ceiling nor a simple rate effect. Three hardware hypotheses have
// already failed (input cadence, out_full-vs-prog_full beat drops, absolute
// ceiling). This TB exists to stop guessing: it reproduces the stall in sim with
// the core's internal state directly observable.
//
// Drives pw_single_oc_axis (FIFOs + core, no AXI-Lite) with the exact BD
// parameters, models the weight/param BRAMs the AXI wrapper normally provides,
// and models the S2MM as an always-ready sink.
//
// PASS = all EXP_BEATS emitted and done_out seen.
// STALL = no m_axis beat for STALL_CYC cycles -> dump every internal counter.
//
// To run: add to sim_1, set pw_single_oc_axis_tb as simulation top, launch xsim.
// ============================================================================
module pw_single_oc_axis_tb;

  // ---- parameters: mirror the BD instance exactly ----
  localparam int DATA_WIDTH        = 8;
  localparam int ACC_WIDTH         = 24;
  localparam int CIN_MAX           = 240;
  localparam int COUT_MAX          = 240;
  localparam int TILE_PIXELS_MAX   = 32768;
  localparam int IN_FIFO_DEPTH     = 2048;
  localparam int OUT_FIFO_DEPTH    = 8192;
  localparam int S_AXIS_DATA_WIDTH = 64;
  localparam int N_LANES           = 8;
  localparam int N_OC              = 8;
  localparam int M_AXIS_DATA_WIDTH = 64;

  // ---- run config: the H=16 case, which stalls on hardware at 46,848 beats ----
  localparam int CIN_RUN     = 3;
  localparam int COUT_RUN    = 8;
  localparam int TILE_GROUPS = 2880;                    // H=16 -> 16*180
  localparam int TILE_PIXELS = TILE_GROUPS * N_LANES;   // 23,040
  localparam int EXP_IN_BEATS  = TILE_GROUPS * CIN_RUN; //  8,640
  localparam int EXP_OUT_BEATS = TILE_GROUPS * COUT_RUN;// 86,400

  // Set >0 to insert idle cycles between input beats (probes whether PW is
  // sensitive to being starved -- DW feeds it far more raggedly than a DMA).
  localparam int IN_GAP_CYCLES = 0;

  // Output backpressure model. With OUT_BURST=0 the sink is always ready,
  // which is what the first sim run used -- and it PASSED at the exact config
  // that stalls on hardware (H=16, 86,400 beats). The real S2MM is NOT always
  // ready: it accepts a burst then deasserts tready between bursts, at 4KB
  // boundaries and on DRAM refresh. Set OUT_BURST>0 to model that.
  //   OUT_BURST = beats accepted before pausing
  //   OUT_GAP   = cycles tready is held low
  localparam int OUT_BURST = 256;
  localparam int OUT_GAP   = 8;

  localparam int STALL_CYC = 20000;   // no output for this long => stalled
  localparam int MAX_CYC   = 4000000;

  logic clk = 0, rst_n = 0;
  always #5 clk = ~clk;

  logic        start_in, done_out;
  logic [31:0] tile_pixels;
  logic [11:0] cin_run, cout_run;
  logic [7:0]  zp_in, zp_out;
  logic        relu_en;

  logic [$clog2((COUT_MAX/N_OC)*CIN_MAX)-1:0] w_rd_addr;
  logic                                      w_rd_en;
  logic signed [DATA_WIDTH-1:0]              w_rd_data [0:N_OC-1];

  logic [$clog2(COUT_MAX)-1:0] param_rd_addr;
  logic                        param_rd_en;
  logic signed [31:0]          param_bias_data;
  logic [31:0]                 param_mult_data;
  logic [7:0]                  param_shift_data;

  logic [S_AXIS_DATA_WIDTH-1:0] s_tdata;
  logic                         s_tvalid, s_tready, s_tlast;
  logic [M_AXIS_DATA_WIDTH-1:0] m_tdata;
  logic                         m_tvalid, m_tready, m_tlast;
  logic in_ovf, in_unf, out_ovf, out_unf;

  pw_single_oc_axis #(
    .DATA_WIDTH(DATA_WIDTH), .ACC_WIDTH(ACC_WIDTH), .CIN_MAX(CIN_MAX),
    .COUT_MAX(COUT_MAX), .TILE_PIXELS_MAX(TILE_PIXELS_MAX),
    .IN_FIFO_DEPTH(IN_FIFO_DEPTH), .OUT_FIFO_DEPTH(OUT_FIFO_DEPTH),
    .S_AXIS_DATA_WIDTH(S_AXIS_DATA_WIDTH), .N_LANES(N_LANES), .N_OC(N_OC),
    .M_AXIS_DATA_WIDTH(M_AXIS_DATA_WIDTH)
  ) dut (
    .clk(clk), .rst_n(rst_n),
    .start_in(start_in), .done_out(done_out),
    .tile_pixels(tile_pixels), .cin_run(cin_run), .cout_run(cout_run),
    .zp_in(zp_in), .zp_out(zp_out), .relu_en(relu_en),
    .w_rd_addr(w_rd_addr), .w_rd_en(w_rd_en), .w_rd_data(w_rd_data),
    .param_rd_addr(param_rd_addr), .param_rd_en(param_rd_en),
    .param_bias_data(param_bias_data), .param_mult_data(param_mult_data),
    .param_shift_data(param_shift_data),
    .s_axis_tdata(s_tdata), .s_axis_tvalid(s_tvalid),
    .s_axis_tready(s_tready), .s_axis_tlast(s_tlast),
    .m_axis_tdata(m_tdata), .m_axis_tvalid(m_tvalid),
    .m_axis_tready(m_tready), .m_axis_tlast(m_tlast),
    .in_overflow_out(in_ovf), .in_underflow_out(in_unf),
    .out_overflow_out(out_ovf), .out_underflow_out(out_unf)
  );

  // ---- weight / param BRAM models (1-cycle read latency, as the wrapper) ----
  // These signals must be driven from HERE ONLY -- initialising them in the
  // main initial block as well is an "invalid combination of procedural
  // drivers" (VRFC 10-3818), so the reset values live in this block.
  always_ff @(posedge clk) begin
    if (!rst_n) begin
      for (int o = 0; o < N_OC; o++) w_rd_data[o] <= '0;
      param_bias_data  <= 32'sd0;
      param_mult_data  <= 32'h4000_0000;
      param_shift_data <= 8'd31;
    end else begin
      if (w_rd_en)
        for (int o = 0; o < N_OC; o++) w_rd_data[o] <= $signed(8'sd1);
      if (param_rd_en) begin
        param_bias_data  <= 32'sd0;
        param_mult_data  <= 32'h4000_0000;
        param_shift_data <= 8'd31;
      end
    end
  end

  // ---- input driver: group-major, cin_run words per pixel group ----
  int in_beats = 0;
  initial begin
    s_tvalid = 0; s_tdata = '0; s_tlast = 0;
    @(posedge rst_n);
    repeat (20) @(posedge clk);
    for (int g = 0; g < TILE_GROUPS; g++) begin
      for (int c = 0; c < CIN_RUN; c++) begin
        s_tdata  = {8'(g), 8'(c), 8'(g), 8'(c), 8'(g), 8'(c), 8'(g), 8'(c)};
        s_tvalid = 1'b1;
        s_tlast  = (g == TILE_GROUPS-1) && (c == CIN_RUN-1);
        @(posedge clk);
        while (!s_tready) @(posedge clk);
        in_beats++;
        if (IN_GAP_CYCLES > 0) begin
          s_tvalid = 1'b0;
          repeat (IN_GAP_CYCLES) @(posedge clk);
        end
      end
    end
    s_tvalid = 0; s_tlast = 0;
    $display("[TB] input driver finished: %0d beats (exp %0d) @%0t",
             in_beats, EXP_IN_BEATS, $time);
  end

  // ---- output sink: models S2MM, always ready ----
  int out_beats = 0;
  bit got_tlast = 0;
  always_ff @(posedge clk) begin
    if (!rst_n) begin out_beats <= 0; got_tlast <= 0; end
    else if (m_tvalid && m_tready) begin
      out_beats <= out_beats + 1;
      if (m_tlast) got_tlast <= 1'b1;
    end
  end

  // ---- internal state dump: the whole point of this TB ----
  task automatic dump_state(input string why);
    begin
      $display("---- %s @%0t ----", why, $time);
      $display("  out_beats=%0d / %0d   in_beats=%0d / %0d",
               out_beats, EXP_OUT_BEATS, in_beats, EXP_IN_BEATS);
      $display("  core: st=%0d  grp_idx=%0d / tile_groups_r=%0d",
               dut.u_core.st, dut.u_core.grp_idx, dut.u_core.tile_groups_r);
      $display("  core: ppu_st=%0d load_st=%0d next_grp_ready=%0b compute_buf=%0b",
               dut.u_core.ppu_st, dut.u_core.load_st,
               dut.u_core.next_grp_ready, dut.u_core.compute_buf);
      $display("  core: ic_idx=%0d load_ic_idx=%0d oc_batch_idx=%0d",
               dut.u_core.ic_idx, dut.u_core.load_ic_idx, dut.u_core.oc_batch_idx);
      $display("  axis: produced_cnt=%0d total_groups_r=%0d will_last=%0b",
               dut.produced_cnt, dut.total_groups_r, dut.will_last);
      $display("  fifo: in_empty=%0b out_full=%0b out_prog_full=%0b out_empty=%0b",
               dut.in_empty, dut.out_full, dut.out_prog_full, dut.out_empty);
      $display("  flags: in_ovf=%0b in_unf=%0b out_ovf=%0b out_unf=%0b",
               in_ovf, in_unf, out_ovf, out_unf);
      $display("  handshake: s_tvalid=%0b s_tready=%0b m_tvalid=%0b m_tready=%0b",
               s_tvalid, s_tready, m_tvalid, m_tready);
    end
  endtask

  // ---- stall watchdog ----
  int last_out = 0, idle_cyc = 0;
  bit stalled = 0;
  always_ff @(posedge clk) begin
    if (!rst_n) begin last_out <= 0; idle_cyc <= 0; end
    else if (!got_tlast && !stalled) begin
      if (out_beats != last_out) begin
        last_out <= out_beats;
        idle_cyc <= 0;
      end else begin
        idle_cyc <= idle_cyc + 1;
        if (idle_cyc == STALL_CYC) stalled <= 1'b1;
      end
    end
  end

  // periodic progress so a slow run is distinguishable from a dead one
  initial forever begin
    repeat (50000) @(posedge clk);
    if (!got_tlast)
      $display("[TB] progress: out_beats=%0d/%0d grp_idx=%0d st=%0d @%0t",
               out_beats, EXP_OUT_BEATS, dut.u_core.grp_idx, dut.u_core.st, $time);
  end

  // ---- output backpressure driver (models S2MM burst/pause) ----
  initial begin
    if (OUT_BURST == 0) begin
      m_tready = 1'b1;                       // always-ready sink
    end else begin
      m_tready = 1'b1;
      forever begin
        int accepted = 0;
        while (accepted < OUT_BURST) begin
          @(posedge clk);
          if (m_tvalid && m_tready) accepted++;
        end
        m_tready = 1'b0;
        repeat (OUT_GAP) @(posedge clk);
        m_tready = 1'b1;
      end
    end
  end

  // ---- main ----
  initial begin
    start_in = 0;
    tile_pixels = TILE_PIXELS; cin_run = CIN_RUN; cout_run = COUT_RUN;
    zp_in = 8'd70; zp_out = 8'd0; relu_en = 1'b0;
    /* w_rd_data / param_* are driven solely by the BRAM model above */

    rst_n = 0;
    repeat (10) @(posedge clk);
    rst_n = 1;
    repeat (5) @(posedge clk);

    $display("[TB] tile_pixels=%0d tile_groups=%0d cin=%0d cout=%0d -> exp_out=%0d beats",
             TILE_PIXELS, TILE_GROUPS, CIN_RUN, COUT_RUN, EXP_OUT_BEATS);

    start_in = 1; @(posedge clk); start_in = 0;

    fork
      begin : watch
        wait (stalled);
        dump_state("STALLED (no output for STALL_CYC)");
        $display("[TB] FAIL: stalled at %0d/%0d beats (%0d%%)",
                 out_beats, EXP_OUT_BEATS, (out_beats*100)/EXP_OUT_BEATS);
        $finish;
      end
      begin : good
        wait (got_tlast);
        repeat (10) @(posedge clk);
        dump_state("TLAST seen");
        if (out_beats == EXP_OUT_BEATS)
          $display("[TB] PASS: %0d beats, TLAST, done_out=%0b", out_beats, done_out);
        else
          $display("[TB] FAIL: TLAST at %0d beats, expected %0d",
                   out_beats, EXP_OUT_BEATS);
        $finish;
      end
      begin : timeout
        repeat (MAX_CYC) @(posedge clk);
        dump_state("GLOBAL TIMEOUT");
        $display("[TB] FAIL: global timeout");
        $finish;
      end
    join_any
    $finish;
  end

  // ---- instrumentation ----
  int n_batch, n_vin, n_vout, n_stall_vout, n_wr;
  bit prev_idle;
  always_ff @(posedge clk) begin
    if (!rst_n) begin
      n_batch<=0; n_vin<=0; n_vout<=0; n_stall_vout<=0; n_wr<=0; prev_idle<=1;
    end else begin
      prev_idle <= (dut.u_core.ppu_st == 2'd0);
      if ((dut.u_core.ppu_st == 2'd1) && prev_idle) n_batch <= n_batch + 1;
      if (dut.u_core.ppu_valid_in) n_vin  <= n_vin  + 1;
      if (dut.u_core.valid_out)    n_vout <= n_vout + 1;
      if (dut.u_core.valid_out && dut.u_core.out_stall) n_stall_vout <= n_stall_vout + 1;
      if (dut.out_wr_en) n_wr <= n_wr + 1;
    end
  end
  final $display("[MON] batches=%0d vin=%0d vout=%0d vout_during_stall=%0d fifo_writes=%0d grp_idx=%0d",
                 n_batch, n_vin, n_vout, n_stall_vout, n_wr, dut.u_core.grp_idx);
endmodule
`default_nettype wire
