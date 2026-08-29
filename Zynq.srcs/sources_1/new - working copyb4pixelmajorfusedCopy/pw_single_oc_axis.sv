`timescale 1ns/1ps
module pw_single_oc_axis #(
  parameter int DATA_WIDTH        = 8,
  parameter int ACC_WIDTH         = 32,
  parameter int CIN_MAX           = 240,
  parameter int TILE_PIXELS_MAX   = 2048*16,
  parameter int IN_FIFO_DEPTH     = 2048,
  parameter int OUT_FIFO_DEPTH    = TILE_PIXELS_MAX,
  parameter int S_AXIS_DATA_WIDTH = 32,
  parameter int N_LANES           = 4,
  parameter int N_OC              = 2,
  parameter int M_AXIS_DATA_WIDTH = N_LANES * DATA_WIDTH
)(
  input  logic                         clk,
  input  logic                         rst_n,

  input  logic                         start_in,
  output logic                         done_out,

  input  logic [31:0]                  tile_pixels,
  input  logic [11:0]                  cin_run,

  input  logic [7:0]                   zp_in,
  input  logic [7:0]                   zp_out,
  input  logic                         relu_en,

  input  logic signed [31:0]           bias_in    [0:N_OC-1],
  input  logic [31:0]                  mult_conv  [0:N_OC-1],
  input  logic [7:0]                   shift_conv [0:N_OC-1],

  input  logic signed [DATA_WIDTH-1:0] w_ic [0:N_OC-1][0:CIN_MAX-1],

  input  logic [S_AXIS_DATA_WIDTH-1:0] s_axis_tdata,
  input  logic                         s_axis_tvalid,
  output logic                         s_axis_tready,
  input  logic                         s_axis_tlast,

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

  wire [31:0] tile_groups  = tile_pixels >> LANE_SHIFT;
  wire [31:0] total_groups = tile_groups * N_OC;

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
  assign core_valid_in = !in_empty;
  assign core_pixel_in = in_dout;
  assign in_rd_en      = core_consume_in && core_valid_in;

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
  logic [$clog2(CIN_MAX)-1:0] ic_sel_dbg;
  logic [CORE_DATA_W-1:0]     core_pixel_out;
  logic                       core_valid_out;
  logic                       out_full;  // forward-declared for backpressure

  pw_single_oc_tile_rt_noctrl #(
    .DATA_WIDTH(DATA_WIDTH), .ACC_WIDTH(ACC_WIDTH), .CIN_MAX(CIN_MAX),
    .TILE_PIXELS_MAX(TILE_PIXELS_MAX), .N_LANES(N_LANES), .N_OC(N_OC)
  ) u_core (
    .clk(clk), .rst_n(rst_n), .start_in(start_in), .done_out(core_done),
    .tile_pixels(tile_pixels), .cin_run(cin_run),
    .zp_in(zp_in), .zp_out(zp_out), .relu_en(relu_en),
    .bias_in(bias_in), .mult_conv(mult_conv), .shift_conv(shift_conv),
    .w_ic(w_ic),
    .valid_in(core_valid_in), .pixel_in(core_pixel_in),
    .consume_in(core_consume_in),
    .out_stall(out_full),
    .ic_sel_dbg(ic_sel_dbg),
    .pixel_out(core_pixel_out), .valid_out(core_valid_out)
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

  assign will_last = (total_groups != 0) && (produced_cnt == (total_groups - 1));
  assign out_wr_en = core_valid_out && !out_full;
  assign out_rd_en = m_axis_tvalid && m_axis_tready;
  assign m_axis_tvalid = !out_empty;
  assign m_axis_tdata  = out_dout[CORE_DATA_W-1:0];
  assign m_axis_tlast  = out_dout[CORE_DATA_W];
  assign last_beat_accepted = m_axis_tvalid && m_axis_tready && m_axis_tlast;

  xpm_fifo_sync #(
    .DOUT_RESET_VALUE("0"), .ECC_MODE("no_ecc"), .FIFO_MEMORY_TYPE("block"),
    .FIFO_READ_LATENCY(0), .FIFO_WRITE_DEPTH(OUT_FIFO_DEPTH), .FULL_RESET_VALUE(1),
    .PROG_EMPTY_THRESH(10), .PROG_FULL_THRESH(10),
    .RD_DATA_COUNT_WIDTH(OUT_CNT_W), .READ_DATA_WIDTH(OUT_DATA_W),
    .READ_MODE("fwft"), .SIM_ASSERT_CHK(0), .USE_ADV_FEATURES("0707"),
    .WAKEUP_TIME(0), .WRITE_DATA_WIDTH(OUT_DATA_W),
    .WR_DATA_COUNT_WIDTH(OUT_CNT_W)
  ) u_out_fifo (
    .rst(fifo_rst), .wr_clk(clk), .wr_en(out_wr_en),
    .din({will_last, core_pixel_out}),
    .full(out_full), .prog_full(), .wr_data_count(out_wr_count),
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
    else if ((tile_pixels == 0) || (cin_run == 0)) done_out <= core_done;
    else done_out <= last_beat_accepted;
  end

endmodule

`default_nettype wire