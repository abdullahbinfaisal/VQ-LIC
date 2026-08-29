`timescale 1ns/1ps

module pw_single_oc_axis_axi #(
  parameter int DATA_WIDTH         = 8,
  parameter int ACC_WIDTH          = 32,
  parameter int CIN_MAX            = 240,
  parameter int TILE_PIXELS_MAX    = 16384,
  parameter int IN_FIFO_DEPTH      = 2048,
  parameter int OUT_FIFO_DEPTH     = 4096,
  parameter int S_AXIS_DATA_WIDTH  = 32,
  parameter int N_LANES            = 4,
  parameter int N_OC               = 2,
  parameter int M_AXIS_DATA_WIDTH  = N_LANES * DATA_WIDTH,

  parameter integer C_S_AXI_DATA_WIDTH = 32,
  parameter integer C_S_AXI_ADDR_WIDTH = 12
)(
  // ------------------------------------------------------------
  // Global clock/reset
  // ------------------------------------------------------------
  input  logic                              s_axi_aclk,
  input  logic                              s_axi_aresetn,

  // ------------------------------------------------------------
  // AXI4-Lite slave
  // ------------------------------------------------------------
  input  logic [C_S_AXI_ADDR_WIDTH-1:0]     s_axi_awaddr,
  input  logic                              s_axi_awvalid,
  output logic                              s_axi_awready,

  input  logic [C_S_AXI_DATA_WIDTH-1:0]     s_axi_wdata,
  input  logic [(C_S_AXI_DATA_WIDTH/8)-1:0] s_axi_wstrb,
  input  logic                              s_axi_wvalid,
  output logic                              s_axi_wready,

  output logic [1:0]                        s_axi_bresp,
  output logic                              s_axi_bvalid,
  input  logic                              s_axi_bready,

  input  logic [C_S_AXI_ADDR_WIDTH-1:0]     s_axi_araddr,
  input  logic                              s_axi_arvalid,
  output logic                              s_axi_arready,

  output logic [C_S_AXI_DATA_WIDTH-1:0]     s_axi_rdata,
  output logic [1:0]                        s_axi_rresp,
  output logic                              s_axi_rvalid,
  input  logic                              s_axi_rready,

  // ------------------------------------------------------------
  // AXI-Stream input (from DMA MM2S)
  // ------------------------------------------------------------
  input  logic [S_AXIS_DATA_WIDTH-1:0]      s_axis_tdata,
  input  logic                              s_axis_tvalid,
  output logic                              s_axis_tready,
  input  logic                              s_axis_tlast,

  // ------------------------------------------------------------
  // AXI-Stream output (to DMA S2MM)
  // ------------------------------------------------------------
  output logic [M_AXIS_DATA_WIDTH-1:0]      m_axis_tdata,
  output logic                              m_axis_tvalid,
  input  logic                              m_axis_tready,
  output logic                              m_axis_tlast

  // optional:
  // ,output logic                          irq
);

  // ============================================================
  // Address map
  // ============================================================
  localparam logic [C_S_AXI_ADDR_WIDTH-1:0] ADDR_CTRL        = 12'h000; // W: bit0=start pulse, bit1=clear done, bit2=clear error sticky
  localparam logic [C_S_AXI_ADDR_WIDTH-1:0] ADDR_STATUS      = 12'h004; // R: bit0=done_sticky, bit1=busy
  localparam logic [C_S_AXI_ADDR_WIDTH-1:0] ADDR_TILE_PIXELS = 12'h008; // RW
  localparam logic [C_S_AXI_ADDR_WIDTH-1:0] ADDR_CIN_RUN     = 12'h00C; // RW [11:0]
  localparam logic [C_S_AXI_ADDR_WIDTH-1:0] ADDR_ZP_RELU     = 12'h010; // RW [7:0]=zp_in [15:8]=zp_out [16]=relu_en
  localparam logic [C_S_AXI_ADDR_WIDTH-1:0] ADDR_BIAS        = 12'h014; // RW
  localparam logic [C_S_AXI_ADDR_WIDTH-1:0] ADDR_MULT        = 12'h018; // RW
  localparam logic [C_S_AXI_ADDR_WIDTH-1:0] ADDR_SHIFT       = 12'h01C; // RW [7:0]
  localparam logic [C_S_AXI_ADDR_WIDTH-1:0] ADDR_STATUS2     = 12'h020; // R: sticky error flags
  localparam logic [C_S_AXI_ADDR_WIDTH-1:0] ADDR_OC_SEL      = 12'h024; // RW: selects OC bank for weight/param writes
  localparam logic [C_S_AXI_ADDR_WIDTH-1:0] ADDR_W_BASE      = 12'h100; // RW weight[i] at 0x100 + 4*i

  // ============================================================
  // AXI-Lite internals
  // ============================================================
  logic [C_S_AXI_ADDR_WIDTH-1:0] axi_awaddr;
  logic [C_S_AXI_ADDR_WIDTH-1:0] axi_araddr;
  logic                          aw_en;

  wire axil_rstn = s_axi_aresetn;

  // aligned addresses
  wire [C_S_AXI_ADDR_WIDTH-1:0] wr_addr = {axi_awaddr[C_S_AXI_ADDR_WIDTH-1:2], 2'b00};
  wire [C_S_AXI_ADDR_WIDTH-1:0] rd_addr = {axi_araddr[C_S_AXI_ADDR_WIDTH-1:2], 2'b00};

  function automatic int unsigned weight_idx(input logic [C_S_AXI_ADDR_WIDTH-1:0] addr);
    weight_idx = (addr - ADDR_W_BASE) >> 2;
  endfunction

  // ============================================================
  // User registers â€” banked by oc_sel for per-OC storage
  // ============================================================
  logic [31:0] reg_tile_pixels;
  logic [31:0] reg_cin_run;
  logic [31:0] reg_zp_relu;
  logic [31:0] reg_oc_sel;

  // Per-OC banked registers (selected by reg_oc_sel during writes)
  logic [31:0] reg_bias    [0:N_OC-1];
  logic [31:0] reg_mult    [0:N_OC-1];
  logic [31:0] reg_shift   [0:N_OC-1];
  logic [31:0] reg_weights [0:N_OC-1][0:CIN_MAX-1];

  logic        busy;
  logic        done_sticky;

  logic        start_pulse;
  logic        clear_done_pulse;
  logic        clear_err_pulse;

  logic        in_overflow;
  logic        in_underflow;
  logic        out_overflow;
  logic        out_underflow;

  logic        in_overflow_sticky;
  logic        in_underflow_sticky;
  logic        out_overflow_sticky;
  logic        out_underflow_sticky;
  logic        start_while_busy_sticky;

  // OC bank select index (clamped to valid range)
  wire [$clog2(N_OC>1?N_OC:2)-1:0] oc_wr_sel = reg_oc_sel[$clog2(N_OC>1?N_OC:2)-1:0];

  // Per-OC weight and param arrays for the core
  logic signed [DATA_WIDTH-1:0] w_ic       [0:N_OC-1][0:CIN_MAX-1];
  logic signed [31:0]           bias_arr   [0:N_OC-1];
  logic [31:0]                  mult_arr   [0:N_OC-1];
  logic [7:0]                   shift_arr  [0:N_OC-1];

  genvar gi, goc;
  generate
    for (goc = 0; goc < N_OC; goc++) begin : G_OC_PARAMS
      for (gi = 0; gi < CIN_MAX; gi++) begin : G_W
        assign w_ic[goc][gi] = reg_weights[goc][gi][DATA_WIDTH-1:0];
      end
      assign bias_arr[goc]  = $signed(reg_bias[goc]);
      assign mult_arr[goc]  = reg_mult[goc];
      assign shift_arr[goc] = reg_shift[goc][7:0];
    end
  endgenerate

  // ============================================================
  // AXI-Lite write address channel
  // ============================================================
  always_ff @(posedge s_axi_aclk) begin
    if (!axil_rstn) begin
      s_axi_awready <= 1'b0;
      axi_awaddr    <= '0;
      aw_en         <= 1'b1;
    end else begin
      if (!s_axi_awready && s_axi_awvalid && s_axi_wvalid && aw_en) begin
        s_axi_awready <= 1'b1;
        axi_awaddr    <= s_axi_awaddr;
        aw_en         <= 1'b0;
      end else if (s_axi_bready && s_axi_bvalid) begin
        aw_en         <= 1'b1;
        s_axi_awready <= 1'b0;
      end else begin
        s_axi_awready <= 1'b0;
      end
    end
  end

  // ============================================================
  // AXI-Lite write data channel
  // ============================================================
  always_ff @(posedge s_axi_aclk) begin
    if (!axil_rstn) begin
      s_axi_wready <= 1'b0;
    end else begin
      if (!s_axi_wready && s_axi_wvalid && s_axi_awvalid && aw_en) begin
        s_axi_wready <= 1'b1;
      end else begin
        s_axi_wready <= 1'b0;
      end
    end
  end

  wire slv_reg_wren = s_axi_wready && s_axi_wvalid && s_axi_awready && s_axi_awvalid;

  // ============================================================
  // Start / clear pulse generation
  // ============================================================
  always_ff @(posedge s_axi_aclk) begin
    if (!axil_rstn) begin
      start_pulse      <= 1'b0;
      clear_done_pulse <= 1'b0;
      clear_err_pulse  <= 1'b0;
    end else begin
      start_pulse      <= 1'b0;
      clear_done_pulse <= 1'b0;
      clear_err_pulse  <= 1'b0;

      if (slv_reg_wren && (wr_addr == ADDR_CTRL) && s_axi_wstrb[0]) begin
        if (s_axi_wdata[0] && !busy)
          start_pulse <= 1'b1;
        if (s_axi_wdata[1])
          clear_done_pulse <= 1'b1;
        if (s_axi_wdata[2])
          clear_err_pulse <= 1'b1;
      end
    end
  end

  // ============================================================
  // Register writes â€” per-OC regs go to bank[oc_wr_sel]
  // ============================================================
  integer wi, woc;
  always_ff @(posedge s_axi_aclk) begin
    if (!axil_rstn) begin
      reg_tile_pixels <= 32'd0;
      reg_cin_run     <= 32'd0;
      reg_zp_relu     <= 32'd0;
      reg_oc_sel      <= 32'd0;

      for (woc = 0; woc < N_OC; woc = woc + 1) begin
        reg_bias[woc]  <= 32'd0;
        reg_mult[woc]  <= 32'd0;
        reg_shift[woc] <= 32'd0;
        for (wi = 0; wi < CIN_MAX; wi = wi + 1)
          reg_weights[woc][wi] <= 32'd0;
      end

    end else begin
      if (slv_reg_wren) begin
        if (wr_addr == ADDR_TILE_PIXELS)
          reg_tile_pixels <= s_axi_wdata;
        else if (wr_addr == ADDR_CIN_RUN)
          reg_cin_run <= s_axi_wdata;
        else if (wr_addr == ADDR_ZP_RELU)
          reg_zp_relu <= s_axi_wdata;
        else if (wr_addr == ADDR_OC_SEL)
          reg_oc_sel <= s_axi_wdata;
        else if (wr_addr == ADDR_BIAS)
          reg_bias[oc_wr_sel] <= s_axi_wdata;
        else if (wr_addr == ADDR_MULT)
          reg_mult[oc_wr_sel] <= s_axi_wdata;
        else if (wr_addr == ADDR_SHIFT)
          reg_shift[oc_wr_sel] <= s_axi_wdata;
        else if ((wr_addr >= ADDR_W_BASE) &&
                 (wr_addr < (ADDR_W_BASE + (CIN_MAX*4))))
          reg_weights[oc_wr_sel][weight_idx(wr_addr)] <= s_axi_wdata;
      end
    end
  end

  // ============================================================
  // AXI-Lite write response
  // ============================================================
  always_ff @(posedge s_axi_aclk) begin
    if (!axil_rstn) begin
      s_axi_bvalid <= 1'b0;
      s_axi_bresp  <= 2'b00;
    end else begin
      if (slv_reg_wren && !s_axi_bvalid) begin
        s_axi_bvalid <= 1'b1;
        s_axi_bresp  <= 2'b00; // OKAY
      end else if (s_axi_bvalid && s_axi_bready) begin
        s_axi_bvalid <= 1'b0;
      end
    end
  end

  // ============================================================
  // AXI-Lite read address channel
  // ============================================================
  always_ff @(posedge s_axi_aclk) begin
    if (!axil_rstn) begin
      s_axi_arready <= 1'b0;
      axi_araddr    <= '0;
    end else begin
      if (!s_axi_arready && s_axi_arvalid) begin
        s_axi_arready <= 1'b1;
        axi_araddr    <= s_axi_araddr;
      end else begin
        s_axi_arready <= 1'b0;
      end
    end
  end

  // ============================================================
  // AXI-Lite read data channel
  // ============================================================
  always_comb begin
    s_axi_rdata = 32'd0;

    if (rd_addr == ADDR_STATUS) begin
      s_axi_rdata[0] = done_sticky;
      s_axi_rdata[1] = busy;
    end else if (rd_addr == ADDR_TILE_PIXELS) begin
      s_axi_rdata = reg_tile_pixels;
    end else if (rd_addr == ADDR_CIN_RUN) begin
      s_axi_rdata = reg_cin_run;
    end else if (rd_addr == ADDR_ZP_RELU) begin
      s_axi_rdata = reg_zp_relu;
    end else if (rd_addr == ADDR_OC_SEL) begin
      s_axi_rdata = reg_oc_sel;
    end else if (rd_addr == ADDR_BIAS) begin
      s_axi_rdata = reg_bias[oc_wr_sel];
    end else if (rd_addr == ADDR_MULT) begin
      s_axi_rdata = reg_mult[oc_wr_sel];
    end else if (rd_addr == ADDR_SHIFT) begin
      s_axi_rdata = reg_shift[oc_wr_sel];
    end else if (rd_addr == ADDR_STATUS2) begin
      s_axi_rdata[0] = in_overflow_sticky;
      s_axi_rdata[1] = in_underflow_sticky;
      s_axi_rdata[2] = out_overflow_sticky;
      s_axi_rdata[3] = out_underflow_sticky;
      s_axi_rdata[4] = start_while_busy_sticky;
    end else if ((rd_addr >= ADDR_W_BASE) &&
                 (rd_addr < (ADDR_W_BASE + (CIN_MAX*4)))) begin
      s_axi_rdata = reg_weights[oc_wr_sel][weight_idx(rd_addr)];
    end
  end

  always_ff @(posedge s_axi_aclk) begin
    if (!axil_rstn) begin
      s_axi_rvalid <= 1'b0;
      s_axi_rresp  <= 2'b00;
    end else begin
      if (s_axi_arready && s_axi_arvalid && !s_axi_rvalid) begin
        s_axi_rvalid <= 1'b1;
        s_axi_rresp  <= 2'b00; // OKAY
      end else if (s_axi_rvalid && s_axi_rready) begin
        s_axi_rvalid <= 1'b0;
      end
    end
  end

  // ============================================================
  // PW datapath instance
  // ============================================================
  logic pw_done_out;

  pw_single_oc_axis #(
    .DATA_WIDTH(DATA_WIDTH), .ACC_WIDTH(ACC_WIDTH), .CIN_MAX(CIN_MAX),
    .TILE_PIXELS_MAX(TILE_PIXELS_MAX), .IN_FIFO_DEPTH(IN_FIFO_DEPTH),
    .OUT_FIFO_DEPTH(OUT_FIFO_DEPTH), .S_AXIS_DATA_WIDTH(S_AXIS_DATA_WIDTH),
    .N_LANES(N_LANES), .N_OC(N_OC), .M_AXIS_DATA_WIDTH(M_AXIS_DATA_WIDTH)
  ) u_pw (
    .clk(s_axi_aclk), .rst_n(s_axi_aresetn),
    .start_in(start_pulse), .done_out(pw_done_out),
    .tile_pixels(reg_tile_pixels), .cin_run(reg_cin_run[11:0]),
    .zp_in(reg_zp_relu[7:0]), .zp_out(reg_zp_relu[15:8]), .relu_en(reg_zp_relu[16]),
    .bias_in(bias_arr), .mult_conv(mult_arr), .shift_conv(shift_arr),
    .w_ic(w_ic),
    .s_axis_tdata(s_axis_tdata), .s_axis_tvalid(s_axis_tvalid),
    .s_axis_tready(s_axis_tready), .s_axis_tlast(s_axis_tlast),
    .m_axis_tdata(m_axis_tdata), .m_axis_tvalid(m_axis_tvalid),
    .m_axis_tready(m_axis_tready), .m_axis_tlast(m_axis_tlast),
    .in_overflow_out(in_overflow), .in_underflow_out(in_underflow),
    .out_overflow_out(out_overflow), .out_underflow_out(out_underflow)
  );

  // ============================================================
  // Busy / done sticky
  // ============================================================
  always_ff @(posedge s_axi_aclk) begin
    if (!axil_rstn) begin
      busy        <= 1'b0;
      done_sticky <= 1'b0;
    end else begin
      if (start_pulse) begin
        busy        <= 1'b1;
        done_sticky <= 1'b0;
      end else begin
        if (pw_done_out) begin
          busy        <= 1'b0;
          done_sticky <= 1'b1;
        end
        if (clear_done_pulse)
          done_sticky <= 1'b0;
      end
    end
  end

  // ============================================================
  // Error/status sticky bits
  // ============================================================
  always_ff @(posedge s_axi_aclk) begin
    if (!axil_rstn) begin
      in_overflow_sticky      <= 1'b0;
      in_underflow_sticky     <= 1'b0;
      out_overflow_sticky     <= 1'b0;
      out_underflow_sticky    <= 1'b0;
      start_while_busy_sticky <= 1'b0;
    end else begin
      if (clear_err_pulse) begin
        in_overflow_sticky      <= 1'b0;
        in_underflow_sticky     <= 1'b0;
        out_overflow_sticky     <= 1'b0;
        out_underflow_sticky    <= 1'b0;
        start_while_busy_sticky <= 1'b0;
      end else begin
        if (in_overflow)
          in_overflow_sticky <= 1'b1;
        if (in_underflow)
          in_underflow_sticky <= 1'b1;
        if (out_overflow)
          out_overflow_sticky <= 1'b1;
        if (out_underflow)
          out_underflow_sticky <= 1'b1;

        if (slv_reg_wren && (wr_addr == ADDR_CTRL) && s_axi_wstrb[0] && s_axi_wdata[0] && busy)
          start_while_busy_sticky <= 1'b1;
      end
    end
  end

  // optional IRQ
  // assign irq = done_sticky | in_overflow_sticky | in_underflow_sticky
  //                     | out_overflow_sticky | out_underflow_sticky | start_while_busy_sticky;

endmodule

`default_nettype wire