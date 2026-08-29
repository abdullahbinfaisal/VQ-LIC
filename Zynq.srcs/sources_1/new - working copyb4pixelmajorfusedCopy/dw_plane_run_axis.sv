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
  input  logic [7:0]              s_axis_tdata,
  input  logic                    s_axis_tvalid,
  output logic                    s_axis_tready,
  input  logic                    s_axis_tlast,   // unused

  // AXI-Stream output (to DMA S2MM)
  output logic [7:0]              m_axis_tdata,
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
  logic [11:0] out_w, out_h;
  logic [31:0] out_pixels;

  always_comb begin
    if (stride_2) begin
      out_w = (img_width  + 12'd1) >> 1;  // ceil(W/2)
      out_h = (img_height + 12'd1) >> 1;  // ceil(H/2)
    end else begin
      out_w = img_width;
      out_h = img_height;
    end
    out_pixels = out_w * out_h;
  end

  // ============================================================
  // Input FIFO (DMA -> FIFO -> core)
  // ============================================================
  localparam int IN_AW = $clog2(IN_FIFO_DEPTH);

  (* ram_style="distributed" *) logic [7:0] in_mem [0:IN_FIFO_DEPTH-1];
  logic [IN_AW-1:0] in_wr_ptr, in_rd_ptr;
  logic [IN_AW:0]   in_count;

  wire in_full  = (in_count == IN_FIFO_DEPTH);
  wire in_empty = (in_count == 0);

  // core interface
  logic                  valid_in;
  logic [DATA_WIDTH-1:0] pixel_in;
  logic                  consume_in;

  // We'll throttle input if output fifo is close to full (defined later)
  logic throttle_in;

  // AXI input handshake
  wire in_push = s_axis_tvalid && s_axis_tready;

  // Async read (show-ahead)
  wire [7:0] in_dout = in_mem[in_rd_ptr];

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
  logic [DATA_WIDTH-1:0] pixel_out;
  logic                  valid_out;
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
    .is_depthwise(1'b1),
    .pad_top   (pad_top),

    .mode_residual(1'b0),
    .relu_en      (1'b0),

    .zp_in (zp_in),
    .zp_out(zp_out),

    .mult_conv(mult_in),
    .shift_conv(shift_in),
    .bias_in(bias_in),

    .mult_res_a(32'd0),
    .mult_res_b(32'd0),
    .shift_res (8'd0),
    .zp_in_a   (8'd0),
    .zp_in_b   (8'd0),

    .valid_in(valid_in),
    .pixel_in(pixel_in),

    .weights(weights_3x3),

    .psum_in('0),
    .psum_clear(1'b1),

    .res_a_in(8'd0),
    .res_b_in(8'd0),

    .pixel_out(pixel_out),
    .valid_out(valid_out),

    .mac_debug_out(),
    .consume_in(consume_in)
  );

  // ============================================================
  // Output FIFO (core -> FIFO -> DMA)
  // Store packed {last,data} as 9-bit RAM
  // ============================================================
  localparam int OUT_AW = $clog2(OUT_FIFO_DEPTH);

  (* ram_style="distributed" *) logic [8:0] out_mem [0:OUT_FIFO_DEPTH-1]; // [8]=last, [7:0]=data
  logic [OUT_AW-1:0] out_wr_ptr, out_rd_ptr;
  logic [OUT_AW:0]   out_count;

  wire out_full  = (out_count == OUT_FIFO_DEPTH);
  wire out_empty = (out_count == 0);

  // Throttle input when OUT FIFO near full (margin for pipeline)
  localparam int OUT_MARGIN = 32;
  always_comb throttle_in = (out_count >= (OUT_FIFO_DEPTH - OUT_MARGIN));

  // Tag TLAST on final produced pixel
  logic [31:0] produced_cnt;
  wire will_last = (out_pixels != 0) && (produced_cnt == (out_pixels - 1));

  wire out_push = valid_out && !out_full;
  wire out_pop  = m_axis_tvalid && m_axis_tready;

  wire [8:0] out_dout = out_mem[out_rd_ptr];

  assign m_axis_tvalid = !out_empty;
  assign m_axis_tdata  = out_dout[7:0];
  assign m_axis_tlast  = out_dout[8];

  // ? FIXED DONE: pulse exactly when LAST beat is accepted
  assign done_out = m_axis_tvalid && m_axis_tready && m_axis_tlast;

  // RAM write (NO async reset)
  always_ff @(posedge clk) begin
    if (out_push) begin
      out_mem[out_wr_ptr] <= {will_last, pixel_out};
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