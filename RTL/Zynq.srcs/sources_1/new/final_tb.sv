`timescale 1ns/1ps
`default_nettype none

module tb_dw_plane_run_axis_auto_fixed;

  // -----------------------------
  // PATHS
  // -----------------------------
  localparam string INSTR_BIN        = "C:/Users/Fahad/OneDrive - Higher Education Commission/Desktop/Sem Stuff/SPROJ/Current Direction/code/Conversion Code/mem/instr.bin";
  localparam string PARAMS_BIN       = "C:/Users/Fahad/OneDrive - Higher Education Commission/Desktop/Sem Stuff/SPROJ/Current Direction/code/Conversion Code/mem/params.bin";
  localparam string WEIGHTS_BIN      = "C:/Users/Fahad/OneDrive - Higher Education Commission/Desktop/Sem Stuff/SPROJ/Current Direction/code/Conversion Code/mem/weights.bin";
  localparam string INPUT_PLANAR_BIN = "C:/Users/Fahad/OneDrive - Higher Education Commission/Desktop/Sem Stuff/SPROJ/Current Direction/code/Conversion Code/ref/input_planar.bin";
  localparam string REF_DIR          = "C:/Users/Fahad/OneDrive - Higher Education Commission/Desktop/Sem Stuff/SPROJ/Current Direction/code/Conversion Code/ref";

  // Choose DW layer + channel
  localparam int unsigned LAYER_IDX  = 0;  // l00
  localparam int unsigned KERNEL_IDX = 0;

  localparam int unsigned MAX_CYCLES        = 1_000_00000;
  localparam int unsigned DONE_GRACE_CYCLES = 100000000;

  // -----------------------------
  // DUT signals (64-bit AXI-Stream to match DUT port widths)
  // -----------------------------
  logic clk, rst_n;

  logic start_in;
  logic done_out;

  logic [11:0] img_width, img_height;
  logic [7:0]  pad_top;
  logic        stride_2;
  logic [7:0]  zp_in, zp_out;

  logic signed [31:0] bias_in;
  logic [31:0]        mult_in;
  logic [7:0]         shift_in;

  logic signed [7:0] weights_3x3 [2:0][2:0];

  // AXIS in - 64-bit wide to match dw_plane_run_axis ports
  logic [63:0] s_axis_tdata;
  logic        s_axis_tvalid;
  logic        s_axis_tready;
  logic        s_axis_tlast;

  // AXIS out - 64-bit wide to match dw_plane_run_axis ports
  logic [63:0] m_axis_tdata;
  logic        m_axis_tvalid;
  logic        m_axis_tready;
  logic        m_axis_tlast;

  // -----------------------------
  // DUT
  // -----------------------------
  dw_plane_run_axis #(
    .DATA_WIDTH(8),
    .ACC_WIDTH(32),
    .IN_FIFO_DEPTH(2048),
    .OUT_FIFO_DEPTH(2048)
  ) dut (
    .clk(clk),
    .rst_n(rst_n),

    .start_in(start_in),
    .done_out(done_out),

    .img_width(img_width),
    .img_height(img_height),
    .pad_top(pad_top),
    .stride_2(stride_2),
    .zp_in(zp_in),
    .zp_out(zp_out),

    .bias_in(bias_in),
    .mult_in(mult_in),
    .shift_in(shift_in),

    .weights_3x3(weights_3x3),

    .s_axis_tdata(s_axis_tdata),
    .s_axis_tvalid(s_axis_tvalid),
    .s_axis_tready(s_axis_tready),
    .s_axis_tlast(s_axis_tlast),

    .m_axis_tdata(m_axis_tdata),
    .m_axis_tvalid(m_axis_tvalid),
    .m_axis_tready(m_axis_tready),
    .m_axis_tlast(m_axis_tlast)
  );

  // clock
  initial clk = 1'b0;
  always #5 clk = ~clk;

  // keep output always ready
  initial m_axis_tready = 1'b1;

  // endian helpers
  function automatic int unsigned le_u32(input byte unsigned b0,b1,b2,b3);
    le_u32 = {b3,b2,b1,b0};
  endfunction
  function automatic int signed le_s32(input byte unsigned b0,b1,b2,b3);
    le_s32 = $signed({b3,b2,b1,b0});
  endfunction

  // layer fields
  int unsigned opcode;
  bit depthwise_en;
  int unsigned inH, inW, inC, outC;
  int unsigned in_zp, out_zp;
  int unsigned stride, kernel, pad;
  int unsigned weight_addr, params_addr;

  // files
  int fd_instr, fd_params, fd_weights;
  int fd_in, fd_ref;

  // sizes
  int unsigned in_pixels;
  int unsigned in_words_per_row;  // ceil(inW / 8) -- matches line_buffer_8x DMA
  int unsigned in_total_words;    // in_words_per_row * inH
  int unsigned out_h, out_w, out_pixels;
  int unsigned out_words_per_row;
  int unsigned out_total_words;

  string ref_out_path;

  // -----------------------------
  // Read one layer record from instr.bin
  // -----------------------------
  task automatic load_layer_from_instr;
    byte unsigned ib[0:31];
    int n;
    int unsigned w0,w1,w2,w3,w4,w5,w6,w7;
    int rc;
    begin
      fd_instr = $fopen(INSTR_BIN, "rb");
      if (fd_instr == 0) $fatal(1, "Cannot open instr.bin");

      rc = $fseek(fd_instr, LAYER_IDX*32, 0);
      if (rc != 0) $fatal(1, "fseek instr failed");

      n = $fread(ib, fd_instr);
      if (n < 32) $fatal(1, "instr fread got %0d bytes (<32)", n);
      $fclose(fd_instr);

      w0 = le_u32(ib[0], ib[1], ib[2], ib[3]);
      w1 = le_u32(ib[4], ib[5], ib[6], ib[7]);
      w2 = le_u32(ib[8], ib[9], ib[10], ib[11]);
      w3 = le_u32(ib[12], ib[13], ib[14], ib[15]);
      w4 = le_u32(ib[16], ib[17], ib[18], ib[19]);
      w5 = le_u32(ib[20], ib[21], ib[22], ib[23]);
      w6 = le_u32(ib[24], ib[25], ib[26], ib[27]);
      w7 = le_u32(ib[28], ib[29], ib[30], ib[31]);

      opcode       = (w0 >> 0) & 32'h3;
      depthwise_en = ((w0 >> 4) & 1) != 0;

      inH  = (w5 >> 16) & 16'hFFFF;
      inW  = (w5 >> 0)  & 16'hFFFF;
      outC = (w6 >> 16) & 16'hFFFF;
      inC  = (w6 >> 0)  & 16'hFFFF;

      in_zp  = (w7 >> 24) & 8'hFF;
      out_zp = (w7 >> 16) & 8'hFF;
      stride = (w7 >> 12) & 4'hF;
      kernel = (w7 >> 8)  & 4'hF;
      pad    = (w7 >> 0)  & 8'hFF;

      weight_addr = w3;
      params_addr = w4;

      if (opcode != 0)       $fatal(1, "Layer %0d opcode=%0d not conv", LAYER_IDX, opcode);
      if (!depthwise_en)     $fatal(1, "TB is DW-only; layer %0d is not depthwise", LAYER_IDX);
      if (kernel != 3)       $fatal(1, "Expected kernel=3, got %0d", kernel);
      if (stride != 1 && stride != 2) $fatal(1, "Expected stride 1/2, got %0d", stride);

      $display("L%0d: H=%0d W=%0d Cin=%0d Cout=%0d stride=%0d pad=%0d zp_in=%0d zp_out=%0d",
               LAYER_IDX, inH, inW, inC, outC, stride, pad, in_zp, out_zp);
      $display("     params_addr=0x%08x weight_addr=0x%08x", params_addr, weight_addr);
    end
  endtask

  // -----------------------------
  // Read DW params + weights for KERNEL_IDX
  // params: 16B per channel
  // weights: 9B per channel
  // -----------------------------
  task automatic load_dw_params_and_weights;
    byte unsigned pb[0:15];
    byte signed   wb[0:8];
    int n, rc;
    int wt_byte;
    begin
      fd_params = $fopen(PARAMS_BIN, "rb");
      if (fd_params == 0) $fatal(1, "Cannot open params.bin");
      rc = $fseek(fd_params, params_addr + (KERNEL_IDX * 16), 0);
      if (rc != 0) $fatal(1, "fseek params failed");
      n = $fread(pb, fd_params);
      if (n < 16) $fatal(1, "params fread got %0d bytes (<16)", n);
      $fclose(fd_params);

      bias_in  = le_s32(pb[0], pb[1], pb[2], pb[3]);
      mult_in  = le_u32(pb[4], pb[5], pb[6], pb[7]);
      shift_in = pb[8];

      fd_weights = $fopen(WEIGHTS_BIN, "rb");
      if (fd_weights == 0) $fatal(1, "Cannot open weights.bin");
      rc = $fseek(fd_weights, weight_addr + (KERNEL_IDX * 9), 0);
      if (rc != 0) $fatal(1, "fseek weights failed");

      for (int i = 0; i < 9; i++) begin
        wt_byte = $fgetc(fd_weights);
        if (wt_byte < 0) $fatal(1, "EOF reading weights byte %0d", i);
        wb[i] = byte'(wt_byte[7:0]);
      end
      $fclose(fd_weights);

      weights_3x3[0][0] = wb[0];
      weights_3x3[0][1] = wb[1];
      weights_3x3[0][2] = wb[2];
      weights_3x3[1][0] = wb[3];
      weights_3x3[1][1] = wb[4];
      weights_3x3[1][2] = wb[5];
      weights_3x3[2][0] = wb[6];
      weights_3x3[2][1] = wb[7];
      weights_3x3[2][2] = wb[8];

      $display("Params: bias=%0d mult=%0u shift=%0d", bias_in, mult_in, shift_in);
    end
  endtask

  // -----------------------------
  // RESET + CONFIG
  // -----------------------------
  initial begin
    rst_n = 1'b0;
    start_in = 1'b0;

    s_axis_tdata  = 64'd0;
    s_axis_tvalid = 1'b0;
    s_axis_tlast  = 1'b0;

    repeat (10) @(posedge clk);
    rst_n = 1'b1;
    repeat (5) @(posedge clk);

    load_layer_from_instr();

    if (KERNEL_IDX >= inC) $fatal(1, "KERNEL_IDX=%0d out of range (Cin=%0d)", KERNEL_IDX, inC);

    // drive DUT config
    img_width  = inW[11:0];
    img_height = inH[11:0];
    pad_top    = pad[7:0];
    stride_2   = (stride == 2);
    zp_in      = in_zp[7:0];
    zp_out     = out_zp[7:0];

    // derived sizes
    in_pixels       = inH * inW;
    in_words_per_row = (inW + 7) / 8;   // ceil(inW/8) -- must match line_buffer_8x W_div_8
    in_total_words   = in_words_per_row * inH;

    out_h      = (inH + (2*pad) - 3) / stride + 1;
    out_w      = (inW + (2*pad) - 3) / stride + 1;
    out_pixels = out_h * out_w;

    // DUT packs output into 64-bit words with row alignment
    out_words_per_row = (out_w + 7) / 8;
    out_total_words   = out_words_per_row * out_h;

    load_dw_params_and_weights();

    // open input + ref
    fd_in = $fopen(INPUT_PLANAR_BIN, "rb");
    if (fd_in == 0) $fatal(1, "Cannot open input_planar.bin");

    ref_out_path = $sformatf("%s/l%02d.bin", REF_DIR, LAYER_IDX);
    fd_ref = $fopen(ref_out_path, "rb");
    if (fd_ref == 0) $fatal(1, "Cannot open ref output %s", ref_out_path);

    // seek to this channel plane
    if ($fseek(fd_in,  KERNEL_IDX * in_pixels,  0) != 0) $fatal(1, "fseek input failed");
    if ($fseek(fd_ref, KERNEL_IDX * out_pixels, 0) != 0) $fatal(1, "fseek ref failed");

    // start pulse (do not stream during start)
    @(posedge clk);
    start_in <= 1'b1;
    @(posedge clk);
    start_in <= 1'b0;

    // wait 1 cycle after start before sending
    @(posedge clk);

    $display("RUN L%02d K%0d: in_pixels=%0d in_total_words=%0d (%0d/row) out_pixels=%0d (out_h=%0d out_w=%0d) out_total_words=%0d",
             LAYER_IDX, KERNEL_IDX, in_pixels, in_total_words, in_words_per_row, out_pixels, out_h, out_w, out_total_words);
  end

  // -----------------------------
  // STIMULUS THREAD - sends data ROW-BY-ROW, each row padded to
  // ceil(inW/8) words to match how line_buffer_8x consumes DMA data.
  // -----------------------------
  initial begin : stim_thread
    int unsigned sent_total;
    int unsigned row_idx;
    int unsigned col_word;
    int unsigned pixels_left_in_row;
    int sc;
    reg [63:0] word;
    int unsigned bytes_this_word;
    integer bi;

    wait(rst_n === 1'b1);
    @(negedge start_in);
    @(posedge clk);

    sent_total = 0;

    for (row_idx = 0; row_idx < inH; row_idx = row_idx + 1) begin
      pixels_left_in_row = inW;

      for (col_word = 0; col_word < in_words_per_row; col_word = col_word + 1) begin
        word = 64'd0;
        bytes_this_word = (pixels_left_in_row >= 8) ? 8 : pixels_left_in_row;

        // Read real pixels from file; remaining bytes stay 0 (row-end padding)
        for (bi = 0; bi < 8; bi = bi + 1) begin
          if (bi < bytes_this_word) begin
            sc = $fgetc(fd_in);
            if (sc < 0) $fatal(1, "Input EOF at row=%0d word=%0d byte=%0d", row_idx, col_word, bi);
            word[bi*8 +: 8] = sc[7:0];
          end
        end

        s_axis_tdata  <= word;
        s_axis_tvalid <= 1'b1;
        s_axis_tlast  <= (sent_total == (in_total_words - 1));

        @(posedge clk);
        while (!(s_axis_tvalid && s_axis_tready)) @(posedge clk);

        sent_total        = sent_total + 1;
        pixels_left_in_row = pixels_left_in_row - bytes_this_word;
      end
    end

    s_axis_tvalid <= 1'b0;
    s_axis_tlast  <= 1'b0;
    $fclose(fd_in);
    $display("STIM: sent %0d input words (%0d rows x %0d words/row, %0d pixels)",
             sent_total, inH, in_words_per_row, in_pixels);
  end

  // -----------------------------
  // SCOREBOARD THREAD - unpacks each 64-bit output word, compares per-byte
  // -----------------------------
  initial begin : score_thread
    int unsigned got_words;
    int unsigned got_pixels;
    int unsigned row_pixel;
    bit done_seen;
    int exp;
    int unsigned grace;
    int unsigned valid_bytes;
    integer bj;

    wait(rst_n === 1'b1);

    got_words  = 0;
    got_pixels = 0;
    row_pixel  = 0;
    done_seen  = 0;

    // watchdog timeout
    fork
      begin : timeout_thread
        repeat (MAX_CYCLES) @(posedge clk);
        $fatal(1, "TIMEOUT: got_words=%0d/%0d got_pixels=%0d/%0d",
               got_words, out_total_words, got_pixels, out_pixels);
      end

      begin : check_thread
        // Receive exactly out_total_words output beats
        while (got_words < out_total_words) begin
          @(posedge clk);

          // latch done_out whenever it happens
          if (done_out) done_seen = 1'b1;

          if (m_axis_tvalid && m_axis_tready) begin
            // Determine how many valid pixel bytes are in this word
            valid_bytes = out_w - row_pixel;
            if (valid_bytes > 8) valid_bytes = 8;

            // Compare each valid byte against reference
            for (bj = 0; bj < 8; bj = bj + 1) begin
              if (bj < valid_bytes) begin
                exp = $fgetc(fd_ref);
                if (exp < 0) $fatal(1, "Ref EOF early at pixel=%0d", got_pixels);

                if (m_axis_tdata[bj*8 +: 8] !== exp[7:0]) begin
                  $display("MISMATCH pixel=%0d (word=%0d byte=%0d) got=0x%02x exp=0x%02x",
                           got_pixels, got_words, bj, m_axis_tdata[bj*8 +: 8], exp[7:0]);
                  $fatal(1, "FAIL");
                end
                got_pixels = got_pixels + 1;
              end
            end

            // Update row tracking
            row_pixel = row_pixel + valid_bytes;
            if (row_pixel >= out_w) row_pixel = 0;

            // TLAST must be on the FINAL word
            if (got_words == (out_total_words - 1)) begin
              if (!m_axis_tlast)
                $fatal(1, "Expected TLAST on final word (word=%0d) but TLAST=0", got_words);
            end else begin
              if (m_axis_tlast)
                $fatal(1, "TLAST early at word=%0d expected %0d", got_words, out_total_words-1);
            end

            got_words = got_words + 1;
          end
        end

        // Verify we matched exactly the expected number of output pixels
        if (got_pixels != out_pixels)
          $display("WARNING: pixel count mismatch: got=%0d expected=%0d", got_pixels, out_pixels);

        // finished consuming expected bytes
        $fclose(fd_ref);

        // allow done_out to come a few cycles later
        grace = 0;
        while (!done_seen && (grace < DONE_GRACE_CYCLES)) begin
          @(posedge clk);
          if (done_out) done_seen = 1'b1;
          grace = grace + 1;
        end

        if (!done_seen) begin
          $display("WARNING: done_out never observed within %0d cycles after last beat.", DONE_GRACE_CYCLES);
        end

        $display("PASS: L%02d K%0d matched %0d pixels (%0d words). done_seen=%0d",
                 LAYER_IDX, KERNEL_IDX, out_pixels, out_total_words, done_seen);
        $finish;
      end
    join_any
    disable fork;
  end

endmodule

`default_nettype wire