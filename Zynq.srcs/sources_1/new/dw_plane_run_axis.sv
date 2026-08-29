`timescale 1ns/1ps

module dw_plane_run_axis #(
  parameter int DATA_WIDTH      = 8,
  parameter int ACC_WIDTH       = 32,
  parameter int IN_FIFO_DEPTH   = 2048,   // power-of-2 recommended
  parameter int OUT_FIFO_DEPTH  = 2048    // power-of-2 recommended
)(
  input  logic                    clk,
  input  logic                    rst_n,

  input  logic                    start_in,     // 1-cycle pulse
  output logic                    done_out,     // 1-cycle pulse when LAST output beat is ACCEPTED

  input  logic [11:0]             img_width,
  input  logic [11:0]             img_height,
  input  logic [7:0]              pad_top,
  input  logic                    stride_2,
  input  logic [7:0]              zp_in,
  input  logic [7:0]              zp_out,

  input  logic signed [31:0]      bias_in,
  input  logic [31:0]             mult_in,
  input  logic [7:0]              shift_in,

  input  logic signed [DATA_WIDTH-1:0] weights_3x3 [2:0][2:0],

  // AXI-Stream input (from DMA MM2S)
  input  logic [63:0]             s_axis_tdata,
  input  logic                    s_axis_tvalid,
  output logic                    s_axis_tready,
  input  logic                    s_axis_tlast,   // unused

  // AXI-Stream output (to DMA S2MM)
  output logic [63:0]             m_axis_tdata,
  output logic                    m_axis_tvalid,
  input  logic                    m_axis_tready,
  output logic                    m_axis_tlast
);

  // Silence "unused" warning
  logic _unused_tlast;
  always_comb _unused_tlast = s_axis_tlast;

  // ============================================================
  // Derived output pixel count (for TLAST generation)
  // NOTE: You said you never use (pad=0,stride=2), so this ceil()
  // version is OK for your current network. If you ever do, switch
  // to the exact conv formula.
  // ============================================================
  // ============================================================
  // Registered output dimensions — latched at start_in.
  // AXI registers (img_width/height/stride_2) are stable well before
  // start_in pulses, so registering here is safe and breaks the
  // combinational multiply path:
  //   slv_reg → out_words_per_row * out_h (13 CARRY4) → will_last → out_mem
  // ============================================================
  logic [11:0] out_w_r, out_h_r;
  logic [31:0] out_words_r;

  logic [31:0] words_per_row_r;
  logic [11:0] ch_r;
  logic        calc_stage2;

  always_ff @(posedge clk) begin
    if (start_in) begin
      logic [11:0] cw, ch;
      logic [31:0] words_per_row;
      if (stride_2) begin
        cw = (img_width  + 12'd1) >> 1;
        ch = (img_height + 12'd1) >> 1;
      end else begin
        cw = img_width;
        ch = img_height;
      end
      out_w_r     <= cw;
      out_h_r     <= ch;
      words_per_row = ({20'd0, cw} + 32'd7) >> 3;
      
      // Stage 1 pipeline: save operands
      words_per_row_r <= words_per_row;
      ch_r            <= ch;
      calc_stage2     <= 1'b1;
    end else begin
      calc_stage2     <= 1'b0;
    end
    
    // Stage 2 pipeline: compute multiplier
    if (calc_stage2) begin
      out_words_r <= words_per_row_r * ch_r;
    end
  end

  // Combinational out_w for eol check (uses registered out_w_r after start)
  // Keep raw combinational wires only for the eol row-counter (which is local
  // to the packer and does not drive RAM write data).
  wire [11:0] out_w = out_w_r;

  // ============================================================
  // Input FIFO (DMA -> FIFO -> core)
  // ============================================================
  localparam int IN_AW = $clog2(IN_FIFO_DEPTH);

  (* ram_style="distributed" *) logic [63:0] in_mem [0:IN_FIFO_DEPTH-1];
  logic [IN_AW-1:0] in_wr_ptr, in_rd_ptr;
  logic [IN_AW:0]   in_count;

  wire in_full  = (in_count == IN_FIFO_DEPTH);
  wire in_empty = (in_count == 0);

  // core interface
  logic                  valid_in;
  logic [8*DATA_WIDTH-1:0] pixel_in;
  logic                  consume_in;

  // We'll throttle input if output fifo is close to full (defined later)
  logic throttle_in;

  // AXI input handshake
  wire in_push = s_axis_tvalid && s_axis_tready;

  // Async read (show-ahead)
  wire [63:0] in_dout = in_mem[in_rd_ptr];

  // Pop exactly when core consumes a REAL pixel
  wire in_pop = consume_in && valid_in;

  // Optional: gate ready during start flush to avoid pushing during reset/flush
  // (TB already avoids this; HW DMA sometimes won't)
  assign s_axis_tready = (!in_full) && (!start_in);

  assign valid_in = (!in_empty) && (!throttle_in);
  assign pixel_in = in_dout;

  // RAM write (NO async reset)
  always_ff @(posedge clk) begin
    if (in_push) begin
      in_mem[in_wr_ptr] <= s_axis_tdata;
    end
  end

  // pointers/counters (async reset OK)
  always_ff @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin
      in_wr_ptr <= '0;
      in_rd_ptr <= '0;
      in_count  <= '0;
    end else if (start_in) begin
      in_wr_ptr <= '0;
      in_rd_ptr <= '0;
      in_count  <= '0;
    end else begin
      if (in_push) in_wr_ptr <= in_wr_ptr + 1'b1;
      if (in_pop)  in_rd_ptr <= in_rd_ptr + 1'b1;

      case ({in_push, in_pop})
        2'b10: in_count <= in_count + 1'b1;
        2'b01: in_count <= in_count - 1'b1;
        default: ;
      endcase
    end
  end

  // ============================================================
  // Core (unchanged)
  // ============================================================
  logic [8*DATA_WIDTH-1:0] pixel_out;
  logic                  valid_out;
  logic [7:0]            core_val_vec;
  logic                  core_done;

  compute_engine #(
    .DATA_WIDTH(DATA_WIDTH),
    .ACC_WIDTH (ACC_WIDTH)
  ) u_dw (
    .clk(clk),
    .rst_n(rst_n),

    .start_in(start_in),
    .done_out(core_done),

    .img_width (img_width),
    .img_height(img_height),
    .stride_2  (stride_2),
    .bypass_1x1(1'b0),
    .pad_top   (pad_top),

    .relu_en      (1'b0),

    .zp_in (zp_in),
    .zp_out(zp_out),

    .mult_conv(mult_in),
    .shift_conv(shift_in),
    .bias_in(bias_in),

    .valid_in(valid_in),
    .pixel_in(pixel_in),

    .weights(weights_3x3),

    .psum_in('0),
    .psum_clear(1'b1),

    .pixel_out(pixel_out),
    .valid_out(),           // ignored, using vec
    .valid_out_vec(core_val_vec),

    .mac_debug_out(),
    .consume_in(consume_in)
  );

  // ============================================================
  // Byte Packer (Valid byte extraction & Row Alignment)
  // ============================================================
  // ---- Stage P0: extract valid bytes from core output ----
  logic [63:0] extracted;
  logic [3:0]  extract_cnt;

  always_comb begin
    extracted   = '0;
    extract_cnt = 0;
    for (int i = 0; i < 8; i++) begin
      if (core_val_vec[i]) begin
        extracted[extract_cnt*8 +: 8] = pixel_out[i*8 +: 8];
        extract_cnt = extract_cnt + 1;
      end
    end
  end

  logic [11:0] row_pixel_cnt;
  logic        eol;
  always_comb eol = (extract_cnt > 0) && (row_pixel_cnt + {8'd0, extract_cnt} >= out_w);

  always_ff @(posedge clk or negedge rst_n) begin
    if (!rst_n)      row_pixel_cnt <= '0;
    else if (start_in) row_pixel_cnt <= '0;
    else if (extract_cnt > 0) begin
      if (eol) row_pixel_cnt <= '0;
      else     row_pixel_cnt <= row_pixel_cnt + {8'd0, extract_cnt};
    end
  end

  // ---- Pipeline register: P0 -> P1 ----
  // Register extracted bytes + count + eol before the 128-bit barrel shift.
  // This breaks the critical path:
  //   OLD: shift_cnt --> 128-bit shift/mask --> combined --> shift_reg  (15 CARRY4)
  //   NEW: shift_cnt --> [FF] --> 128-bit shift/mask --> [FF] --> shift_reg
  logic [63:0]  extracted_r;
  logic [3:0]   extract_cnt_r;
  logic         eol_r;
  logic         has_data_r;   // extract_cnt_r > 0

  always_ff @(posedge clk or negedge rst_n) begin
    if (!rst_n || start_in) begin
      extracted_r   <= '0;
      extract_cnt_r <= '0;
      eol_r         <= 1'b0;
      has_data_r    <= 1'b0;
    end else begin
      extracted_r   <= extracted;
      extract_cnt_r <= extract_cnt;
      eol_r         <= eol;
      has_data_r    <= (extract_cnt > 0);
    end
  end

  // ---- Stage P1: 128-bit shift / mask / combine (now from registered inputs) ----
  logic [127:0] shift_reg;
  logic [3:0]   shift_cnt;

  logic [127:0] next_shift_reg;
  logic [4:0]   next_shift_cnt;
  logic         push_packed;
  logic [63:0]  packed_data;
  logic         set_flush_pending;

  always_comb begin
    logic [127:0] shifted_ext;
    logic [127:0] mask;
    logic [127:0] combined;

    // CASE lookup instead of 128-bit arithmetic shift+subtract.
    // Eliminates the 15-CARRY4 chain: shift_cnt is 3-bit (0-7),
    // so all 8 cases are MUX constants -> ~3-4 LUT levels only.
    case (shift_cnt)
      4'd0: begin
        shifted_ext = {64'd0,          extracted_r};
        mask        = 128'h0;
      end
      4'd1: begin
        shifted_ext = {56'd0, extracted_r,  8'd0};
        mask        = 128'hFF;
      end
      4'd2: begin
        shifted_ext = {48'd0, extracted_r, 16'd0};
        mask        = 128'hFFFF;
      end
      4'd3: begin
        shifted_ext = {40'd0, extracted_r, 24'd0};
        mask        = 128'hFFFFFF;
      end
      4'd4: begin
        shifted_ext = {32'd0, extracted_r, 32'd0};
        mask        = 128'hFFFFFFFF;
      end
      4'd5: begin
        shifted_ext = {24'd0, extracted_r, 40'd0};
        mask        = 128'hFFFFFFFFFF;
      end
      4'd6: begin
        shifted_ext = {16'd0, extracted_r, 48'd0};
        mask        = 128'hFFFFFFFFFFFF;
      end
      4'd7: begin
        shifted_ext = { 8'd0, extracted_r, 56'd0};
        mask        = 128'hFFFFFFFFFFFFFF;
      end
      default: begin
        shifted_ext = '0;
        mask        = '0;
      end
    endcase

    combined = (shift_reg & mask) | shifted_ext;

    next_shift_cnt    = shift_cnt + extract_cnt_r;
    set_flush_pending = 1'b0;

    if (next_shift_cnt >= 8) begin
      push_packed    = has_data_r;
      packed_data    = combined[63:0];
      next_shift_reg = {64'd0, combined[127:64]};
      next_shift_cnt = next_shift_cnt - 5'd8;
      if (eol_r && next_shift_cnt > 0) set_flush_pending = 1'b1;
    end else if (eol_r && has_data_r) begin
      push_packed    = 1'b1;
      packed_data    = combined[63:0];
      next_shift_reg = '0;
      next_shift_cnt = '0;
    end else begin
      push_packed    = 1'b0;
      packed_data    = 64'd0;
      next_shift_reg = has_data_r ? combined : shift_reg;
    end
  end

  // ============================================================
  // Output FIFO Declarations (core -> FIFO -> DMA)
  // ============================================================
  localparam int OUT_AW = $clog2(OUT_FIFO_DEPTH);

  (* ram_style="distributed" *) logic [64:0] out_mem [0:OUT_FIFO_DEPTH-1]; // [64]=last, [63:0]=data
  logic [OUT_AW-1:0] out_wr_ptr, out_rd_ptr;
  logic [OUT_AW:0]   out_count;

  wire out_full  = (out_count == OUT_FIFO_DEPTH);
  wire out_empty = (out_count == 0);

  // Handle flush for EOL overflow or core_done
  logic flush_pending;
  logic push_flush;
  always_comb begin
    push_flush = flush_pending && (shift_cnt > 0) && !out_full;
  end

  always_ff @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin
      shift_reg     <= '0;
      shift_cnt     <= '0;
      flush_pending <= 1'b0;
    end else if (start_in) begin
      shift_reg     <= '0;
      shift_cnt     <= '0;
      flush_pending <= 1'b0;
    end else begin
      if (core_done || set_flush_pending) flush_pending <= 1'b1;
      
      if (push_flush) begin
        shift_reg     <= '0;
        shift_cnt     <= '0;
        flush_pending <= 1'b0;
      end else begin
        shift_reg <= next_shift_reg;
        shift_cnt <= next_shift_cnt[3:0];
      end
    end
  end

  // Throttle input when OUT FIFO near full (margin for pipeline)
  localparam int OUT_MARGIN = 32;
  always_comb throttle_in = (out_count >= (OUT_FIFO_DEPTH - OUT_MARGIN));

  // Tag TLAST on final produced word
  logic [31:0] produced_cnt;
  wire will_last = (out_words_r != 0) && (produced_cnt == (out_words_r - 1));

  wire out_push = (push_packed || push_flush) && !out_full;
  wire [63:0] out_push_data = push_flush ? shift_reg[63:0] : packed_data;

  wire out_pop  = m_axis_tvalid && m_axis_tready;

  wire [64:0] out_dout = out_mem[out_rd_ptr];

  assign m_axis_tvalid = !out_empty;
  assign m_axis_tdata  = out_dout[63:0];
  assign m_axis_tlast  = out_dout[64];

  // ? FIXED DONE: pulse exactly when LAST beat is accepted
  assign done_out = m_axis_tvalid && m_axis_tready && m_axis_tlast;

  // RAM write (NO async reset)
  always_ff @(posedge clk) begin
    if (out_push) begin
      out_mem[out_wr_ptr] <= {will_last, out_push_data};
    end
  end

  // pointers/counters (async reset OK)
  always_ff @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin
      out_wr_ptr   <= '0;
      out_rd_ptr   <= '0;
      out_count    <= '0;
      produced_cnt <= 32'd0;
    end else begin
      if (start_in) begin
        out_wr_ptr   <= '0;
        out_rd_ptr   <= '0;
        out_count    <= '0;
        produced_cnt <= 32'd0;
      end else begin
        if (out_push) begin
          out_wr_ptr   <= out_wr_ptr + 1'b1;
          produced_cnt <= produced_cnt + 32'd1;
        end

        if (out_pop) begin
          out_rd_ptr <= out_rd_ptr + 1'b1;
        end

        case ({out_push, out_pop})
          2'b10: out_count <= out_count + 1'b1;
          2'b01: out_count <= out_count - 1'b1;
          default: ;
        endcase
      end
    end
  end

endmodule

`default_nettype wire