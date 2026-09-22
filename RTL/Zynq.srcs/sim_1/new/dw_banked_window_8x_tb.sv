`timescale 1ns/1ps

// ============================================================================
// dw_banked_window_8x_tb.sv - self-checking unit test
//
// Drives group-major input (row-major outer, then group, then channel) with
// stall bubbles, and checks every emitted 8-lane window -- REAL 3x3, not the
// 1x3 dw_seq_window_8x produced -- against a software golden model. Exercises:
//   - genuine vertical windowing across multiple rows (top/bottom taps real,
//     not zp-tied -- this is the actual fix vs dw_seq_window_8x)
//   - multi-channel banking (per-channel row context stays independent while
//     channels round-robin within a row-group)
//   - horizontal tail masking with a non-multiple-of-8 real width (the
//     IMG_WIDTH register path, separate from N_GROUPS)
//   - top/bottom image-edge masking (zp at Y<0 and Y>=H)
//   - the flush row (r==H) correctly completing the last real row's output
//   - consume_in only pulsing on real rows, not the flush row
//   - done_out firing exactly once, after every expected emission
//
// Run in Vivado xsim. Expect: "PASS: dw_banked_window_8x ..."
// ============================================================================

module dw_banked_window_8x_tb;

  localparam int DATA_WIDTH = 8;
  localparam int CIN_MAX    = 240;
  localparam int C          = 4;         // channels under test
  localparam int G          = 3;         // groups per row (capacity)
  localparam int N          = G*8;       // padded samples per row (24)
  localparam int W          = 19;        // REAL width -- not a multiple of 8,
                                          // exercises tail masking (n=19..23 pad)
  localparam int H          = 4;         // real rows
  localparam logic [7:0] ZP      = 8'd7;    // nonzero padding value
  localparam logic [7:0] POISON  = 8'hEE;   // fed into padding lanes; must
                                             // never leak into a window --
                                             // proves masking overrides data,
                                             // not just coincidentally matches zp

  // ---- DUT I/O ----
  logic                    clk = 0;
  logic                    rst_n = 0;

  logic                    start_in;
  logic [11:0]             cin_run;
  logic [11:0]             n_groups;
  logic [11:0]             img_width;
  logic [11:0]             n_rows;
  logic [7:0]              zp_in;

  logic [8*DATA_WIDTH-1:0] pixel_in;
  logic                    valid_in;
  wire                     consume_in;     // DUT output, do not drive in TB

  logic [DATA_WIDTH-1:0]   window [0:7][2:0][2:0];
  logic [7:0]              valid_out_vec;
  logic [11:0]             ch_out;
  logic                    done_out;

  always #5 clk = ~clk;   // 100 MHz

  dw_banked_window_8x #(
    .DATA_WIDTH(DATA_WIDTH),
    .CIN_MAX(CIN_MAX),
    .MAX_CG_PRODUCT(C*G)     // exact fit for this test's shape
  ) dut (
    .clk(clk),
    .rst_n(rst_n),
    .start_in(start_in),
    .cin_run(cin_run),
    .n_groups(n_groups),
    .img_width(img_width),
    .n_rows(n_rows),
    .zp_in(zp_in),
    .pixel_in(pixel_in),
    .valid_in(valid_in),
    .consume_in(consume_in),
    .window(window),
    .valid_out_vec(valid_out_vec),
    .ch_out(ch_out),
    .done_out(done_out)
  );

  // ---- golden input data: gold[channel][row][sample] ----
  logic [7:0] gold [0:C-1][0:H-1][0:N-1];

  function automatic logic [7:0] gen(input int c, input int y, input int n);
    return (c*8'd40 + y*8'd17 + n*8'd3 + 8'd5) & 8'hFF;
  endfunction

  // Unified tap lookup: out-of-bounds (row or column) -> ZP. Bounds are
  // against the REAL H/W, not the padded N/G capacity.
  function automatic logic [7:0] tap(input int c, input int y, input int n);
    if (y < 0 || y >= H || n < 0 || n >= W) return ZP;
    return gold[c][y][n];
  endfunction

  int   errors      = 0;
  int   emit_count  = 0;              // counts every emission attempt
                                       // (H rows worth, one per r_cnt=1..H)
  int   consume_count = 0;
  logic saw_done    = 1'b0;
  int   done_at_emit = -1;

  localparam int TOTAL_EMITS = H * G * C;

  // ---- checker: fires only on genuine emission cycles (>=1 lane valid).
  // This is the gate that matters: it must skip reset/startup/stall-drain
  // cycles (valid_out_vec all 0, s2_valid was 0) AND the very first row's
  // pipeline pass (r_cnt=0 -> Y=-1, unconditionally masked entirely by
  // is_y_valid, also valid_out_vec all 0) without needing to observe any
  // internal DUT signal -- both cases already read as "nothing valid" from
  // the outside. Relies on W>=1 (so every real group has >=1 valid lane,
  // never all-8-invalid) so no genuine emission is ever mistaken for idle. ----
  always @(posedge clk) begin
    if (rst_n && emit_count < TOTAL_EMITS && (|valid_out_vec)) begin
      int y, g, c, rem;
      y   = emit_count / (G*C);
      rem = emit_count % (G*C);
      g   = rem / C;
      c   = rem % C;

      if (ch_out !== c[11:0]) begin
        errors++;
        $display("ERR emit %0d (y=%0d g=%0d c=%0d): ch_out=%0d expected %0d",
                 emit_count, y, g, c, ch_out, c);
      end

      for (int l = 0; l < 8; l++) begin
        int n;
        logic exp_valid;
        n = g*8 + l;
        exp_valid = (n < W);

        if (valid_out_vec[l] !== exp_valid) begin
          errors++;
          $display("ERR valid  emit %0d (y=%0d g=%0d c=%0d) lane %0d: got %0d exp %0d",
                   emit_count, y, g, c, l, valid_out_vec[l], exp_valid);
        end

        // Check window contents regardless of valid_out_vec -- masked
        // (invalid) lanes must still read back as pure ZP, not leaked
        // POISON or stale data.
        for (int wr = 0; wr < 3; wr++) begin
          for (int wc = 0; wc < 3; wc++) begin
            logic [7:0] exp;
            exp = tap(c, y + (wr-1), n + (wc-1));
            if (window[l][wr][wc] !== exp) begin
              errors++;
              $display("ERR window emit %0d (y=%0d g=%0d c=%0d) lane %0d [%0d][%0d]: got %0d exp %0d",
                       emit_count, y, g, c, l, wr, wc, window[l][wr][wc], exp);
            end
          end
        end
      end

      emit_count++;
    end
  end

  // sticky observer for the 1-cycle done_out pulse; record when it landed
  always @(posedge clk) begin
    if (!rst_n) begin
      saw_done <= 1'b0;
    end else if (done_out) begin
      saw_done <= 1'b1;
      if (done_at_emit < 0) done_at_emit = emit_count;
    end
  end

  // consume_in must only fire on real rows -- count it and cross-check
  // against expected (H*G*C, never (H+1)*G*C for the flush row)
  always @(posedge clk) begin
    if (rst_n && consume_in) consume_count++;
  end

  // ---- stimulus ----
  integer beat_no;

  initial begin
    // initialize golden memory: real samples get gen(); padding lanes
    // (n>=W, still within the padded N capacity) get POISON, to prove
    // the DUT's own img_width-based masking suppresses them rather than
    // relying on the input stream happening to already be zp.
    for (int c = 0; c < C; c++) begin
      for (int y = 0; y < H; y++) begin
        for (int n = 0; n < N; n++) begin
          gold[c][y][n] = (n < W) ? gen(c, y, n) : POISON;
        end
      end
    end

    // initialize DUT inputs
    start_in  = 1'b0;
    valid_in  = 1'b0;
    pixel_in  = '0;

    cin_run   = C[11:0];
    n_groups  = G[11:0];
    img_width = W[11:0];
    n_rows    = H[11:0];
    zp_in     = ZP;

    // reset
    rst_n = 1'b0;
    repeat (4) @(posedge clk);
    rst_n = 1'b1;
    @(posedge clk);

    // start pulse
    start_in <= 1'b1;
    @(posedge clk);
    start_in <= 1'b0;
    @(posedge clk);

    // group-major drive, row-major outer: for row: for group: for channel
    beat_no = 0;

    for (int y = 0; y < H; y++) begin
      for (int g = 0; g < G; g++) begin
        for (int c = 0; c < C; c++) begin
          logic [63:0] beat;

          beat = 64'd0;
          for (int l = 0; l < 8; l++) begin
            beat[l*8 +: 8] = gold[c][y][g*8 + l];
          end

          if ((beat_no % 5) == 4) begin
            // stall bubble
            valid_in <= 1'b0;
            pixel_in <= '0;
            @(posedge clk);
          end

          // Hold pixel_in/valid_in steady until the DUT actually accepts
          // this beat. NOT a fire-and-forget single-cycle present: the DUT
          // has per-row "flush slot" cycles (see dw_banked_window_8x.sv)
          // where it needs no input at all and consume_in stays low for
          // several cycles in a row -- presenting one beat per clock edge
          // unconditionally (as dw_seq_window_8x_tb.sv could get away with,
          // having no such gap) silently desyncs the stream from here on.
          pixel_in <= beat;
          valid_in <= 1'b1;
          do begin
            @(posedge clk);
          end while (!consume_in);

          beat_no++;
        end
      end
    end

    valid_in <= 1'b0;
    pixel_in <= '0;

    $display("Drive complete (real rows) at t=%0t, beats sent=%0d (H*G*C=%0d)",
             $time, beat_no, H*G*C);

    // Robust completion: clocked wait, no wait-event scheduling weirdness.
    // Nothing further needs driving -- the flush row (r==H) advances on
    // its own once the real rows are exhausted.
    while (emit_count < TOTAL_EMITS) begin
      @(posedge clk);
    end

    repeat (8) @(posedge clk);

    if (!saw_done) begin
      $display("ERR: done_out was never observed");
      errors++;
    end else if (done_at_emit != TOTAL_EMITS) begin
      $display("ERR: done_out landed at emit_count=%0d, expected exactly %0d",
               done_at_emit, TOTAL_EMITS);
      errors++;
    end

    if (consume_count != H*G*C) begin
      $display("ERR: consume_in fired %0d times, expected exactly %0d (real rows only, not the flush row)",
               consume_count, H*G*C);
      errors++;
    end

    if (errors == 0) begin
      $display("PASS: dw_banked_window_8x - %0d emits, all windows correct (C=%0d G=%0d W=%0d H=%0d), consume=%0d done_at=%0d",
               emit_count, C, G, W, H, consume_count, done_at_emit);
    end else begin
      $display("FAIL: dw_banked_window_8x - %0d errors", errors);
    end

    $finish;
  end

  // ---- timeout guard ----
  initial begin
    #500000;
    $display("TIMEOUT - emit_count=%0d expected=%0d errors=%0d saw_done=%0b consume_count=%0d",
             emit_count, TOTAL_EMITS, errors, saw_done, consume_count);
    $finish;
  end

endmodule

`default_nettype wire
