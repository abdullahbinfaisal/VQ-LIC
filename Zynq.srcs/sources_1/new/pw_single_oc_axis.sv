`timescale 1ns/1ps
module pw_single_oc_axis #(
  parameter int DATA_WIDTH        = 8,
  parameter int ACC_WIDTH         = 32,
  parameter int CIN_MAX           = 240,
  parameter int COUT_MAX          = 240,
  // TILE_PIXELS_MAX does NOT constrain tile_pixels. `tile_pixels` is a 32-bit
  // runtime register and tile_groups_r is 32-bit, so the datapath handles any
  // value that fits in 32 bits -- the encoder's block 0 runs 230,400 pixels
  // against the old 32,768 setting with exact beat counts, which is what first
  // exposed this. The parameter's ONLY effect is supplying the default for
  // OUT_FIFO_DEPTH below, and the BD overrides that explicitly (8192), so in
  // the built design this parameter is inert.
  // Raised 32,768 -> 262,144 on 2026-07-30 purely so the name stops implying a
  // ceiling that does not exist; it covers block 0's 230,400 with headroom.
  // Do NOT couple new logic to it without re-checking OUT_FIFO_DEPTH.
  parameter int TILE_PIXELS_MAX   = 262144,
  parameter int IN_FIFO_DEPTH     = 2048,
  parameter int OUT_FIFO_DEPTH    = TILE_PIXELS_MAX,
  parameter int S_AXIS_DATA_WIDTH = 32,
  parameter int N_LANES           = 16,
  parameter int N_OC              = 20,
  parameter int M_AXIS_DATA_WIDTH = N_LANES * DATA_WIDTH
)(
  input  logic                         clk,
  input  logic                         rst_n,

  input  logic                         start_in,
  output logic                         done_out,

  input  logic [31:0]                  tile_pixels,
  input  logic [11:0]                  cin_run,
  input  logic [11:0]                  cout_run,

  input  logic [7:0]                   zp_in,
  input  logic [7:0]                   zp_out,
  input  logic                         relu_en,

  // Weight BRAM read interface (from AXI wrapper BRAMs)
  output logic [$clog2((COUT_MAX/N_OC)*CIN_MAX)-1:0] w_rd_addr,
  output logic                         w_rd_en,
  input  logic signed [DATA_WIDTH-1:0] w_rd_data [0:N_OC-1],

  // Param BRAM read interface (from AXI wrapper BRAMs)
  output logic [$clog2(COUT_MAX)-1:0]  param_rd_addr,
  output logic                         param_rd_en,
  input  logic signed [31:0]           param_bias_data,
  input  logic [31:0]                  param_mult_data,
  input  logic [7:0]                   param_shift_data,

  // AXI-Stream slave (input from DMA)
  input  logic [S_AXIS_DATA_WIDTH-1:0] s_axis_tdata,
  input  logic                         s_axis_tvalid,
  output logic                         s_axis_tready,
  input  logic                         s_axis_tlast,

  // AXI-Stream master (output to DMA)
  output logic [M_AXIS_DATA_WIDTH-1:0] m_axis_tdata,
  output logic                         m_axis_tvalid,
  input  logic                         m_axis_tready,
  output logic                         m_axis_tlast,

  output logic                         in_overflow_out,
  output logic                         in_underflow_out,
  output logic                         out_overflow_out,
  output logic                         out_underflow_out
);

  logic _unused_tlast;
  always_comb _unused_tlast = s_axis_tlast;

  // FIFO reset stretcher
  localparam int RST_STRETCH = 5;
  logic [2:0] fifo_rst_cnt;
  logic       fifo_rst;

  always_ff @(posedge clk or negedge rst_n) begin
    if (!rst_n)          fifo_rst_cnt <= RST_STRETCH[2:0];
    else if (start_in)   fifo_rst_cnt <= RST_STRETCH[2:0];
    else if (fifo_rst_cnt != 3'd0) fifo_rst_cnt <= fifo_rst_cnt - 3'd1;
  end
  assign fifo_rst = !rst_n || (fifo_rst_cnt != 3'd0);

  // Width / count calculations
  localparam int CORE_DATA_W  = N_LANES * DATA_WIDTH;
  localparam int LANE_SHIFT   = $clog2(N_LANES);
  localparam int IN_WR_CNT_W  = (IN_FIFO_DEPTH <= 1) ? 1 : $clog2(IN_FIFO_DEPTH+1);
  localparam int IN_RD_DEPTH  = IN_FIFO_DEPTH * S_AXIS_DATA_WIDTH / CORE_DATA_W;
  localparam int IN_RD_CNT_W  = (IN_RD_DEPTH  <= 1) ? 1 : $clog2(IN_RD_DEPTH+1);
  localparam int OUT_CNT_W    = (OUT_FIFO_DEPTH <= 1) ? 1 : $clog2(OUT_FIFO_DEPTH+1);
  localparam int OUT_DATA_W   = CORE_DATA_W + 1;

  // Three-stage pipeline to keep the 32???12 multiply off the critical path.
  //   Cycle 1 (start_in):     latch tile_groups (cheap shift) and cout_run
  //   Cycle 2 (start_in_d1):  compute two 22-bit partial products via DSP48
  //                            pp_lo = tile_groups[15:0]  * cout_run
  //                            pp_hi = tile_groups[31:16] * cout_run
  //   Cycle 3 (start_in_d2):  total_groups = pp_lo + (pp_hi << 16)
  //   Each stage is now ?6 CARRY4 deep ? timing closure at 100 MHz.
  logic [31:0] tile_groups_r;
  logic [11:0] cout_run_r;
  logic        start_in_d1, start_in_d2;

  (* use_dsp = "yes" *) logic [27:0] pp_lo_r;   // 16+12 = 28 bits
  (* use_dsp = "yes" *) logic [27:0] pp_hi_r;   // 16+12 = 28 bits

  logic [31:0] total_groups_r;

  always_ff @(posedge clk) begin
    if (!rst_n) begin
      start_in_d1    <= 1'b0;
      start_in_d2    <= 1'b0;
      tile_groups_r  <= 32'd0;
      cout_run_r     <= 12'd0;
      pp_lo_r        <= 28'd0;
      pp_hi_r        <= 28'd0;
      total_groups_r <= 32'd0;
    end else begin
      start_in_d1 <= start_in;
      start_in_d2 <= start_in_d1;

      if (start_in) begin                              // Cycle 1: cheap latches
        tile_groups_r <= tile_pixels >> LANE_SHIFT;
        cout_run_r    <= cout_run;
      end

      if (start_in_d1) begin                           // Cycle 2: two narrow DSP multiplies
        pp_lo_r <= tile_groups_r[15:0]  * cout_run_r;
        pp_hi_r <= tile_groups_r[31:16] * cout_run_r;
      end

      if (start_in_d2)                                 // Cycle 3: cheap add + shift
        total_groups_r <= {4'd0, pp_lo_r} + ({4'd0, pp_hi_r} << 16);
    end
  end


  // ---- Input FIFO ----
  logic                    in_full, in_empty;
  logic [CORE_DATA_W-1:0]  in_dout;
  logic                    in_wr_en, in_rd_en;
  logic [IN_WR_CNT_W-1:0] in_wr_count;
  logic [IN_RD_CNT_W-1:0] in_rd_count;
  logic                    in_overflow, in_underflow;

  logic                    core_valid_in;
  logic [CORE_DATA_W-1:0]  core_pixel_in;
  logic                    core_consume_in;

  assign in_wr_en      = s_axis_tvalid && s_axis_tready;
  assign s_axis_tready = (!in_full) && (!fifo_rst);
  assign core_pixel_in = in_dout;

  // ------------------------------------------------------------------
  // FIXED 2026-08-08 -- the "block 2 anomaly" (SS21). Present in EVERY build
  // shipped before this date; it silently corrupted block 2's data.
  //
  // consume_in is REGISTERED: the core samples valid_in at cycle T and the pop
  // lands on the T+1 edge. While a consume is in flight, in_empty at T still
  // reads 0 for a word that is about to be taken. If that was the LAST word, the
  // core sees a stale "not empty", decides to consume again, and advances its
  // channel index for a beat it never receives -- in_rd_en below then gates the
  // read and the beat silently never existed.
  //
  // TRACED to a single event, cascade_tb cin=32 cout=32 stride2 (DW-bound):
  //   [TR] OVER-CONSUME cyc=433: consume_in=1 while valid_in=0, empty=1, rden=0
  //        (core has counted 31)
  // ONE occurrence, in S_LOAD_FIRST, at channel 31 of the FIRST group. From then
  // on the core runs permanently one beat ahead of the stream, every group is
  // shifted by a channel, and DW is left holding the surplus with STATUS=busy.
  // Only reachable when PW's input actually runs dry, i.e. only in a DW-bound
  // block -- hence block 2 alone, and hence 1 beat at N_OC=16 vs 60 at N_OC=32.
  //
  // THE FIX IS THE TWO ASSIGNMENTS BELOW, AND BOTH ARE REQUIRED.
  //   in_rd_en unconditional  - a consume always retires the word latched last
  //                             cycle, so the pop must always happen. Gating it
  //                             on core_valid_in is what stranded the beat.
  //   in_occ threshold        - a word the in-flight read is about to take must
  //                             not be offered again.
  // Verified: reproducer 1023/1024 -> 1024/1024; the N_OC=16 DW-bound case, i.e.
  // the original block-2 anomaly, also passes; PW-bound shapes are BIT-IDENTICAL
  // in cycle count (22908, 12316), so this costs no throughput where PW binds.
  // Stride-2 shapes gain 0.3-2.3% cycles - that is the core correctly waiting
  // instead of consuming a phantom beat.
  //
  // FOUR FIXES THAT DO NOT WORK, recorded so they are not retried:
  //  1. in_rd_en = core_consume_in ALONE          - bit-identical, valid still wrong
  //  2. in_occ threshold ALONE                    - oscillates at half rate, because
  //                                                 a gated consume does not pop and
  //                                                 threshold/occupancy disagree
  //  3. !(core_consume_in && in_almost_empty)     - dramatically worse (1024 counted
  //     with USE_ADV_FEATURES "0F0F"                vs 535 arrived); almost_empty is
  //                                                 derived from the LAGGED count
  //  4. a skid buffer                             - would cap input at 1 beat/2 cyc
  // Both empty and rd_data_count LAG: at cyc 433 rd_data_count read 1 while empty
  // was already 1. That is why occupancy must be maintained here, from the
  // handshakes, rather than taken from the FIFO's own status flags.
  // ------------------------------------------------------------------
  // FIX (waveform-derived, see above): a word that the in-flight read is about
  // to take must not be offered again. in_occ is an EXACT, un-lagged occupancy
  // maintained from the handshakes themselves -- the FIFO's own empty and
  // rd_data_count both lag and cannot be used for this (at cyc 433 rd_data_count
  // read 1 while empty was already 1, which is what broke the almost_empty
  // attempt). !in_empty is retained because in FWFT it is what guarantees dout
  // is actually presenting a word; in_occ alone would go high before the
  // fall-through completes.
  //   in_occ  = words held at the START of this cycle
  //   the read happening THIS cycle claims one of them
  // so a new latch is only legal when strictly more than that remain.
  // core_consume_in is a registered core output, so there is no combinational
  // loop through in_rd_en.
  // Costs no throughput: while streaming, in_occ is large and this is always 1.
  localparam int OCC_W = IN_RD_CNT_W + 1;
  logic [OCC_W-1:0] in_occ;
  always_ff @(posedge clk) begin
    if (fifo_rst) in_occ <= '0;
    else          in_occ <= in_occ + (in_wr_en ? OCC_W'(1) : OCC_W'(0))
                                   - ((in_rd_en && (in_occ != '0)) ? OCC_W'(1) : OCC_W'(0));
  end
  // NOTE: assumes one FIFO write == one core word, i.e. S_AXIS_DATA_WIDTH ==
  // CORE_DATA_W. True for N_LANES=8 (both 64b). At N_LANES=16 the core reads
  // 128b against 64b writes and this counter must be rescaled.
  initial if (S_AXIS_DATA_WIDTH != CORE_DATA_W)
    $error("in_occ assumes S_AXIS_DATA_WIDTH == CORE_DATA_W (got %0d vs %0d)",
           S_AXIS_DATA_WIDTH, CORE_DATA_W);

  // in_rd_en is UNCONDITIONAL. A consume always retires the word the core
  // latched on the previous cycle, so the pop must always happen -- gating it on
  // core_valid_in is what stranded the beat in the first place. Making it
  // unconditional ALONE does nothing (tried: bit-identical), and the occupancy
  // guard ALONE oscillates at half rate, because a gated consume does not pop
  // and so the threshold and in_occ disagree. Both are required together.
  assign in_rd_en      = core_consume_in;
  assign core_valid_in = !in_empty &&
                         ($unsigned(in_occ) > (core_consume_in ? 32'd1 : 32'd0));

  xpm_fifo_sync #(
    .DOUT_RESET_VALUE("0"), .ECC_MODE("no_ecc"), .FIFO_MEMORY_TYPE("block"),
    .FIFO_READ_LATENCY(0), .FIFO_WRITE_DEPTH(IN_FIFO_DEPTH), .FULL_RESET_VALUE(1),
    .PROG_EMPTY_THRESH(10), .PROG_FULL_THRESH(10),
    .RD_DATA_COUNT_WIDTH(IN_RD_CNT_W), .READ_DATA_WIDTH(CORE_DATA_W),
    .READ_MODE("fwft"), .SIM_ASSERT_CHK(0), .USE_ADV_FEATURES("0707"),
    .WAKEUP_TIME(0), .WRITE_DATA_WIDTH(S_AXIS_DATA_WIDTH),
    .WR_DATA_COUNT_WIDTH(IN_WR_CNT_W)
  ) u_in_fifo (
    .rst(fifo_rst), .wr_clk(clk), .wr_en(in_wr_en), .din(s_axis_tdata),
    .full(in_full), .prog_full(), .wr_data_count(in_wr_count),
    .rd_en(in_rd_en), .dout(in_dout), .empty(in_empty),
    .prog_empty(), .rd_data_count(in_rd_count),
    .data_valid(), .overflow(in_overflow), .underflow(in_underflow),
    .almost_full(), .almost_empty(), .wr_ack(),
    .sleep(1'b0), .injectsbiterr(1'b0), .injectdbiterr(1'b0),
    .sbiterr(), .dbiterr()
  );

  // ---- Core ----
  logic                       core_done;
  logic [CORE_DATA_W-1:0]     core_pixel_out;
  logic                       core_valid_out;
  logic                       out_full;       // forward-declared for backpressure
  logic                       out_prog_full;  // stall point WITH headroom -- see below

  // ------------------------------------------------------------------
  // BUG FIX 2026-07-27 -- PW hung forever in the DW->PW cascade.
  //
  // out_stall used to be driven by out_full. The core only gates its PPU
  // *issue* on out_stall (pw_pixel_major_core.sv P_DRAIN, ~line 705), but the
  // PPU is a pipeline: by the time out_full asserts, several beats are already
  // in flight, and they land on out_wr_en = core_valid_out && !out_full, which
  // DROPS them. produced_cnt (which counts out_wr_en) then falls permanently
  // short of total_groups_r, so will_last never fires, TLAST never asserts,
  // the S2MM DMA never completes and done_out never sets -> PW busy forever.
  //
  // The loss is invisible to STATUS2: xpm's overflow only asserts on
  // (wr_en && full), and out_wr_en is already gated by !out_full, so
  // out_overflow_sticky stays 0 through the whole failure.
  //
  // Observed on hardware 2026-07-27 (H=64 cascade): PW emitted exactly 65,280
  // beats = 2176 complete pixel groups of 30, then stopped dead with the S2MM
  // healthy and still armed (SR=0, Halted=0) -- i.e. the DMA would have taken
  // more and PW had nothing to give.
  //
  // Same failure mode dw_fused_axis.sv already carries a fix for (see its
  // out_prog_full throttle and the PROG_FULL_THRESH comment: "once throttled,
  // that many beats are already committed and will still land"). Fix here is
  // the same shape: stall the core at OUT_FIFO_DEPTH - OUT_PF_MARGIN so the
  // in-flight beats have somewhere to go.
  //
  // NOTE: this makes the predicted TLAST count correct rather than making it
  // robust. If a beat is ever dropped for some other reason the hang returns.
  // The durable fix is dw_plane_run_axis.sv's approach -- carry the last-beat
  // flag through the FIFO with its data instead of predicting a total.
  // ------------------------------------------------------------------
  localparam int OUT_PF_MARGIN = 64;   // > PPU issue->output pipeline depth
  localparam int OUT_PF_THRESH = (OUT_FIFO_DEPTH > (OUT_PF_MARGIN + 8))
                               ? (OUT_FIFO_DEPTH - OUT_PF_MARGIN)
                               : (OUT_FIFO_DEPTH / 2);

  pw_pixel_major_core #(
    .DATA_WIDTH(DATA_WIDTH), .ACC_WIDTH(ACC_WIDTH), .CIN_MAX(CIN_MAX),
    .COUT_MAX(COUT_MAX), .N_LANES(N_LANES), .N_OC(N_OC)
  ) u_core (
    .clk(clk), .rst_n(rst_n), .start_in(start_in), .done_out(core_done),
    .tile_pixels(tile_pixels), .cin_run(cin_run), .cout_run(cout_run),
    .zp_in(zp_in), .zp_out(zp_out), .relu_en(relu_en),
    .w_rd_addr(w_rd_addr), .w_rd_en(w_rd_en), .w_rd_data(w_rd_data),
    .param_rd_addr(param_rd_addr), .param_rd_en(param_rd_en),
    .param_bias_data(param_bias_data), .param_mult_data(param_mult_data),
    .param_shift_data(param_shift_data),
    .valid_in(core_valid_in), .pixel_in(core_pixel_in),
    .consume_in(core_consume_in),
    .pixel_out(core_pixel_out), .valid_out(core_valid_out),
    .out_stall(out_prog_full)      /* was out_full -- see BUG FIX note above */
  );

  // ---- Output FIFO ----
  logic                   out_empty;
  logic [OUT_DATA_W-1:0]  out_dout;
  logic                   out_wr_en, out_rd_en;
  logic [OUT_CNT_W-1:0]   out_wr_count, out_rd_count;
  logic                   out_overflow, out_underflow;

  logic [31:0]            produced_cnt;
  wire                    will_last;
  wire                    last_beat_accepted;

  assign will_last = (total_groups_r != 0) && (produced_cnt == (total_groups_r - 1));
  assign out_wr_en = core_valid_out && !out_full;
  assign out_rd_en = m_axis_tvalid && m_axis_tready;
  assign m_axis_tvalid = !out_empty;
  assign m_axis_tdata  = out_dout[CORE_DATA_W-1:0];
  assign m_axis_tlast  = out_dout[CORE_DATA_W];
  assign last_beat_accepted = m_axis_tvalid && m_axis_tready && m_axis_tlast;

  xpm_fifo_sync #(
    .DOUT_RESET_VALUE("0"), .ECC_MODE("no_ecc"), .FIFO_MEMORY_TYPE("block"),
    .FIFO_READ_LATENCY(0), .FIFO_WRITE_DEPTH(OUT_FIFO_DEPTH), .FULL_RESET_VALUE(1),
    .PROG_EMPTY_THRESH(10), .PROG_FULL_THRESH(OUT_PF_THRESH),
    .RD_DATA_COUNT_WIDTH(OUT_CNT_W), .READ_DATA_WIDTH(OUT_DATA_W),
    .READ_MODE("fwft"), .SIM_ASSERT_CHK(0), .USE_ADV_FEATURES("0707"),
    .WAKEUP_TIME(0), .WRITE_DATA_WIDTH(OUT_DATA_W),
    .WR_DATA_COUNT_WIDTH(OUT_CNT_W)
  ) u_out_fifo (
    .rst(fifo_rst), .wr_clk(clk), .wr_en(out_wr_en),
    .din({will_last, core_pixel_out}),
    .full(out_full), .prog_full(out_prog_full), .wr_data_count(out_wr_count),
    .rd_en(out_rd_en), .dout(out_dout), .empty(out_empty),
    .prog_empty(), .rd_data_count(out_rd_count),
    .data_valid(), .overflow(out_overflow), .underflow(out_underflow),
    .almost_full(), .almost_empty(), .wr_ack(),
    .sleep(1'b0), .injectsbiterr(1'b0), .injectdbiterr(1'b0),
    .sbiterr(), .dbiterr()
  );

  assign in_overflow_out  = in_overflow;
  assign in_underflow_out = in_underflow;
  assign out_overflow_out = out_overflow;
  assign out_underflow_out= out_underflow;

  // Track produced groups for TLAST
  always_ff @(posedge clk or negedge rst_n) begin
    if (!rst_n)        produced_cnt <= 32'd0;
    else if (start_in) produced_cnt <= 32'd0;
    else if (out_wr_en) produced_cnt <= produced_cnt + 32'd1;
  end

  // Done: after last output beat accepted
  always_ff @(posedge clk or negedge rst_n) begin
    if (!rst_n)        done_out <= 1'b0;
    else if (start_in) done_out <= 1'b0;
    else if ((tile_pixels == 0) || (cin_run == 0) || (cout_run == 0))
      done_out <= core_done;
    else done_out <= last_beat_accepted;
  end

endmodule

`default_nettype wire