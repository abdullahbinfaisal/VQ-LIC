`timescale 1ns/1ps

module pw_single_oc_axis_axi #(
  parameter int DATA_WIDTH         = 8,
  parameter int ACC_WIDTH          = 24,  // 24b sufficient for INT8: max=240×127×255=7.76M < 2^23
  parameter int CIN_MAX            = 240,
  // COUT_MAX. This is the param-BRAM depth AND, via COUT_MAX/N_OC, the
  // number of weight batches. The division FLOORS, so the real ceiling on
  // c_out is (COUT_MAX/N_OC)*N_OC: at 240 with N_OC=32 that is 224 and the
  // top 16 channels of the param BRAM are unreachable. 256 makes the two
  // agree and is free -- W_AW stays $clog2(8*240)=11 and PARAM_AW stays
  // $clog2(256)=8, so no port widens and no BRAM primitive grows (weights
  // 1920x8 and bias 256x32 both still fit one BRAM18 per bank).
  parameter int COUT_MAX           = 256,
  parameter int TILE_PIXELS_MAX    = 16384,
  parameter int IN_FIFO_DEPTH      = 2048,
  parameter int OUT_FIFO_DEPTH     = 4096,
  parameter int S_AXIS_DATA_WIDTH  = 32,
  parameter int N_LANES            = 16,
  parameter int N_OC               = 20,
  // USE_PW_VQ: compile in PW-hosted vector quantisation. 0 = this IP is
  // exactly what it was before the feature existed. See vq_pw.h.
  parameter int USE_PW_VQ          = 0,
  // ---- VQ geometry (see pw_pixel_major_core.sv for the derivation) ----
  //   deployed M=4,K=64,Dm=16 : 64 / 256 / 21
  //   legacy   M=8,K=16,Dm=8  : 16 / 128 / 20
  parameter int VQ_K               = 64,
  parameter int VQ_NORM_D          = 256,
  parameter int VQ_SCORE_W         = 21,
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
  output logic                              m_axis_tlast,

  // ---- VQ-mode stream pair (USE_PW_VQ only; tie off / leave open otherwise)
  // In VQ mode the engine's input is the LATENT read from DDR rather than the
  // DW engine's output, and its output is the packed index stream rather than
  // feature maps. Those are different DMA channels, so the engine needs a
  // second stream pair and a mux -- the alternative, an AXIS switch in the
  // block design, would add an IP, an address segment and a control path for
  // what is three lines of logic.
  //
  // Which pair is live follows reg_vq_ctrl[0], the SAME bit that puts the core
  // in VQ mode, so the stream selection and the datapath mode cannot disagree.
  input  logic [S_AXIS_DATA_WIDTH-1:0]      s_axis_vq_tdata,
  input  logic                              s_axis_vq_tvalid,
  output logic                              s_axis_vq_tready,
  input  logic                              s_axis_vq_tlast,

  output logic [M_AXIS_DATA_WIDTH-1:0]      m_axis_vq_tdata,
  output logic                              m_axis_vq_tvalid,
  input  logic                              m_axis_vq_tready,
  output logic                              m_axis_vq_tlast
);

  // ============================================================
  // Address map
  // ============================================================
  localparam logic [C_S_AXI_ADDR_WIDTH-1:0] ADDR_CTRL        = 12'h000; // W: bit0=start, bit1=clear done, bit2=clear error
  localparam logic [C_S_AXI_ADDR_WIDTH-1:0] ADDR_STATUS      = 12'h004; // R: bit0=done_sticky, bit1=busy
  localparam logic [C_S_AXI_ADDR_WIDTH-1:0] ADDR_TILE_PIXELS = 12'h008; // RW
  localparam logic [C_S_AXI_ADDR_WIDTH-1:0] ADDR_CIN_RUN     = 12'h00C; // RW [11:0]
  localparam logic [C_S_AXI_ADDR_WIDTH-1:0] ADDR_ZP_RELU     = 12'h010; // RW [7:0]=zp_in [15:8]=zp_out [16]=relu_en
  localparam logic [C_S_AXI_ADDR_WIDTH-1:0] ADDR_BIAS        = 12'h014; // W: writes bias_bram[reg_param_addr]
  localparam logic [C_S_AXI_ADDR_WIDTH-1:0] ADDR_MULT        = 12'h018; // W: writes mult_bram[reg_param_addr]
  localparam logic [C_S_AXI_ADDR_WIDTH-1:0] ADDR_SHIFT       = 12'h01C; // W: writes shift_bram[reg_param_addr]
  localparam logic [C_S_AXI_ADDR_WIDTH-1:0] ADDR_STATUS2     = 12'h020; // R: sticky error flags
                                                                        //    [5] = cfg_err: last start REFUSED
  localparam logic [C_S_AXI_ADDR_WIDTH-1:0] ADDR_OC_SEL      = 12'h024; // RW: weight bank select (0..N_OC-1)
  localparam logic [C_S_AXI_ADDR_WIDTH-1:0] ADDR_COUT_RUN    = 12'h028; // RW: total output channels
  localparam logic [C_S_AXI_ADDR_WIDTH-1:0] ADDR_W_BRAM_OFF  = 12'h02C; // RW: base offset in weight BRAM
  localparam logic [C_S_AXI_ADDR_WIDTH-1:0] ADDR_PARAM_ADDR  = 12'h030; // RW: address for bias/mult/shift BRAM writes
  localparam logic [C_S_AXI_ADDR_WIDTH-1:0] ADDR_VQ_CTRL     = 12'h034; // RW: [0]=vq_mode, [23:12]=vq_cin_load  (USE_PW_VQ only)
  localparam logic [C_S_AXI_ADDR_WIDTH-1:0] ADDR_VQ_NORM     = 12'h038; // W: [7:0]=OC, [31:12]=||v_k||^2 (USE_PW_VQ only)
  localparam logic [C_S_AXI_ADDR_WIDTH-1:0] ADDR_W_BASE      = 12'h100; // W: weight BRAM data at offset w_bram_off + idx

  // ============================================================
  // Weight / Param BRAM sizing
  // ============================================================
  localparam int W_OC_BATCHES = COUT_MAX / N_OC;
  localparam int W_DEPTH      = W_OC_BATCHES * CIN_MAX;
  localparam int W_AW         = (W_DEPTH <= 1) ? 1 : $clog2(W_DEPTH);
  localparam int PARAM_AW     = (COUT_MAX <= 1) ? 1 : $clog2(COUT_MAX);
  localparam int VQ_AW        = (VQ_NORM_D <= 1) ? 1 : $clog2(VQ_NORM_D);

  // ============================================================
  // AXI-Lite internals
  // ============================================================
  logic [C_S_AXI_ADDR_WIDTH-1:0] axi_awaddr;
  logic [C_S_AXI_ADDR_WIDTH-1:0] axi_araddr;
  logic                          aw_en;

  wire axil_rstn = s_axi_aresetn;

  wire [C_S_AXI_ADDR_WIDTH-1:0] wr_addr = {axi_awaddr[C_S_AXI_ADDR_WIDTH-1:2], 2'b00};
  wire [C_S_AXI_ADDR_WIDTH-1:0] rd_addr = {axi_araddr[C_S_AXI_ADDR_WIDTH-1:2], 2'b00};

  function automatic int unsigned weight_idx(input logic [C_S_AXI_ADDR_WIDTH-1:0] addr);
    weight_idx = (addr - ADDR_W_BASE) >> 2;
  endfunction

  // ============================================================
  // User registers
  // ============================================================
  logic [31:0] reg_tile_pixels;
  logic [31:0] reg_cin_run;
  logic [31:0] reg_cout_run;
  logic [31:0] reg_zp_relu;
  logic [31:0] reg_oc_sel;       // weight bank select (0..N_OC-1)
  logic [31:0] reg_w_bram_off;   // base offset into selected weight BRAM bank
  logic [31:0] reg_param_addr;   // global OC address for bias/mult/shift writes
  logic [31:0] reg_vq_ctrl;      // [0]=vq_mode, [23:12]=vq_cin_load (USE_PW_VQ)

  // ------------------------------------------------------------------
  // STREAM SELECT. At USE_PW_VQ = 0 vq_stream is a constant 0, every
  // ternary below folds to its else-arm and this IP is port-for-port and
  // gate-for-gate what it was before the VQ pair existed -- the same
  // elaboration-time folding the core relies on.
  //
  // The unselected slave is held NOT ready and the unselected master NOT
  // valid, so an idle producer on the other pair simply stalls; nothing is
  // dropped and no beat can cross between the two paths.
  // ------------------------------------------------------------------
  wire vq_stream = (USE_PW_VQ != 0) && reg_vq_ctrl[0];

  wire [S_AXIS_DATA_WIDTH-1:0] core_s_tdata  = vq_stream ? s_axis_vq_tdata  : s_axis_tdata;
  wire                         core_s_tvalid = vq_stream ? s_axis_vq_tvalid : s_axis_tvalid;
  wire                         core_s_tlast  = vq_stream ? s_axis_vq_tlast  : s_axis_tlast;
  wire                         core_s_tready;
  assign s_axis_tready    = vq_stream ? 1'b0 : core_s_tready;
  assign s_axis_vq_tready = vq_stream ? core_s_tready : 1'b0;

  wire [M_AXIS_DATA_WIDTH-1:0] core_m_tdata;
  wire                         core_m_tvalid, core_m_tlast;
  wire                         core_m_tready = vq_stream ? m_axis_vq_tready : m_axis_tready;
  assign m_axis_tdata     = core_m_tdata;
  assign m_axis_tlast     = core_m_tlast;
  assign m_axis_tvalid    = vq_stream ? 1'b0 : core_m_tvalid;
  assign m_axis_vq_tdata  = core_m_tdata;
  assign m_axis_vq_tlast  = core_m_tlast;
  assign m_axis_vq_tvalid = vq_stream ? core_m_tvalid : 1'b0;

`ifndef SYNTHESIS
  // vq_mode is programmed before start_in and must not move under a live
  // stream: flipping it mid-transfer would strand a beat in whichever FIFO
  // was mid-handshake. Software sets it once per run; this catches a driver
  // that does not.
  logic vq_stream_d;
  always_ff @(posedge s_axi_aclk) begin
    vq_stream_d <= vq_stream;
    if (s_axi_aresetn && (vq_stream != vq_stream_d))
      a_vq_stream_stable: assert (!core_s_tvalid && !core_m_tvalid)
        else $error("vq_mode changed while a stream was active");
  end
`endif
  logic        vq_norm_we;       // 1-cycle pulse on a write to ADDR_VQ_NORM
  logic [VQ_AW-1:0]   vq_norm_addr;
  logic signed [VQ_SCORE_W-1:0] vq_norm_data;

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
  logic        cfg_err_sticky;

  wire [$clog2(N_OC>1?N_OC:2)-1:0] oc_wr_sel = reg_oc_sel[$clog2(N_OC>1?N_OC:2)-1:0];

  // ============================================================
  // Weight BRAMs — N_OC banks, each W_OC_BATCHES * CIN_MAX deep
  // Write from AXI-Lite, read from core
  // ============================================================
  // Write signals (from AXI-Lite)
  logic [W_AW-1:0]         w_wr_addr;
  logic [DATA_WIDTH-1:0]   w_wr_data;
  logic [N_OC-1:0]         w_wr_en;

  // Read signals (from core via axis wrapper)
  logic [W_AW-1:0]                    core_w_rd_addr;
  logic                               core_w_rd_en;
  logic signed [DATA_WIDTH-1:0]       w_rd_data [0:N_OC-1];

  generate
    for (genvar oc = 0; oc < N_OC; oc++) begin : G_WBRAM
      xpm_memory_sdpram #(
        .ADDR_WIDTH_A        (W_AW),
        .ADDR_WIDTH_B        (W_AW),
        .AUTO_SLEEP_TIME     (0),
        .BYTE_WRITE_WIDTH_A  (DATA_WIDTH),
        .CLOCKING_MODE       ("common_clock"),
        .ECC_MODE            ("no_ecc"),
        .MEMORY_INIT_FILE    ("none"),
        .MEMORY_INIT_PARAM   ("0"),
        .MEMORY_OPTIMIZATION ("true"),
        .MEMORY_PRIMITIVE    ("block"),
        .MEMORY_SIZE         (W_DEPTH * DATA_WIDTH),
        .MESSAGE_CONTROL     (0),
        .READ_DATA_WIDTH_B   (DATA_WIDTH),
        .READ_LATENCY_B      (1),
        .READ_RESET_VALUE_B  ("0"),
        .RST_MODE_A          ("SYNC"),
        .RST_MODE_B          ("SYNC"),
        .SIM_ASSERT_CHK      (0),
        .USE_MEM_INIT        (0),
        .WAKEUP_TIME         ("disable_sleep"),
        .WRITE_DATA_WIDTH_A  (DATA_WIDTH),
        .WRITE_MODE_B        ("no_change")
      ) u_wbram (
        .clka  (s_axi_aclk), .ena (1'b1), .wea (w_wr_en[oc]),
        .addra (w_wr_addr),  .dina (w_wr_data),
        .injectsbiterra(1'b0), .injectdbiterra(1'b0),
        .clkb  (s_axi_aclk), .enb (core_w_rd_en), .rstb (1'b0), .regceb (1'b1),
        .addrb (core_w_rd_addr), .doutb (w_rd_data[oc]),
        .sbiterrb(), .dbiterrb(), .sleep(1'b0)
      );
    end
  endgenerate

  // ============================================================
  // Param BRAMs — bias (32b), mult (32b), shift (8b)
  // Write from AXI-Lite, read from core
  // ============================================================
  // Write signals
  logic [PARAM_AW-1:0]  param_wr_addr;
  logic                 param_bias_wr_en;
  logic                 param_mult_wr_en;
  logic                 param_shift_wr_en;
  logic [31:0]          param_wr_data32;
  logic [7:0]           param_wr_data8;

  // Read signals (from core)
  logic [PARAM_AW-1:0]  core_param_rd_addr;
  logic                 core_param_rd_en;
  logic signed [31:0]   param_bias_rd;
  logic [31:0]          param_mult_rd;
  logic [7:0]           param_shift_rd;

  // Bias BRAM
  xpm_memory_sdpram #(
    .ADDR_WIDTH_A(PARAM_AW), .ADDR_WIDTH_B(PARAM_AW),
    .AUTO_SLEEP_TIME(0), .BYTE_WRITE_WIDTH_A(32),
    .CLOCKING_MODE("common_clock"), .ECC_MODE("no_ecc"),
    .MEMORY_INIT_FILE("none"), .MEMORY_INIT_PARAM("0"),
    .MEMORY_OPTIMIZATION("true"), .MEMORY_PRIMITIVE("auto"),
    .MEMORY_SIZE(COUT_MAX * 32), .MESSAGE_CONTROL(0),
    .READ_DATA_WIDTH_B(32), .READ_LATENCY_B(1),
    .READ_RESET_VALUE_B("0"), .RST_MODE_A("SYNC"), .RST_MODE_B("SYNC"),
    .SIM_ASSERT_CHK(0), .USE_MEM_INIT(0),
    .WAKEUP_TIME("disable_sleep"), .WRITE_DATA_WIDTH_A(32),
    .WRITE_MODE_B("no_change")
  ) u_bias_bram (
    .clka(s_axi_aclk), .ena(1'b1), .wea(param_bias_wr_en),
    .addra(param_wr_addr), .dina(param_wr_data32),
    .injectsbiterra(1'b0), .injectdbiterra(1'b0),
    .clkb(s_axi_aclk), .enb(core_param_rd_en), .rstb(1'b0), .regceb(1'b1),
    .addrb(core_param_rd_addr), .doutb(param_bias_rd),
    .sbiterrb(), .dbiterrb(), .sleep(1'b0)
  );

  // Mult BRAM
  xpm_memory_sdpram #(
    .ADDR_WIDTH_A(PARAM_AW), .ADDR_WIDTH_B(PARAM_AW),
    .AUTO_SLEEP_TIME(0), .BYTE_WRITE_WIDTH_A(32),
    .CLOCKING_MODE("common_clock"), .ECC_MODE("no_ecc"),
    .MEMORY_INIT_FILE("none"), .MEMORY_INIT_PARAM("0"),
    .MEMORY_OPTIMIZATION("true"), .MEMORY_PRIMITIVE("auto"),
    .MEMORY_SIZE(COUT_MAX * 32), .MESSAGE_CONTROL(0),
    .READ_DATA_WIDTH_B(32), .READ_LATENCY_B(1),
    .READ_RESET_VALUE_B("0"), .RST_MODE_A("SYNC"), .RST_MODE_B("SYNC"),
    .SIM_ASSERT_CHK(0), .USE_MEM_INIT(0),
    .WAKEUP_TIME("disable_sleep"), .WRITE_DATA_WIDTH_A(32),
    .WRITE_MODE_B("no_change")
  ) u_mult_bram (
    .clka(s_axi_aclk), .ena(1'b1), .wea(param_mult_wr_en),
    .addra(param_wr_addr), .dina(param_wr_data32),
    .injectsbiterra(1'b0), .injectdbiterra(1'b0),
    .clkb(s_axi_aclk), .enb(core_param_rd_en), .rstb(1'b0), .regceb(1'b1),
    .addrb(core_param_rd_addr), .doutb(param_mult_rd),
    .sbiterrb(), .dbiterrb(), .sleep(1'b0)
  );

  // Shift BRAM
  xpm_memory_sdpram #(
    .ADDR_WIDTH_A(PARAM_AW), .ADDR_WIDTH_B(PARAM_AW),
    .AUTO_SLEEP_TIME(0), .BYTE_WRITE_WIDTH_A(8),
    .CLOCKING_MODE("common_clock"), .ECC_MODE("no_ecc"),
    .MEMORY_INIT_FILE("none"), .MEMORY_INIT_PARAM("0"),
    .MEMORY_OPTIMIZATION("true"), .MEMORY_PRIMITIVE("auto"),
    .MEMORY_SIZE(COUT_MAX * 8), .MESSAGE_CONTROL(0),
    .READ_DATA_WIDTH_B(8), .READ_LATENCY_B(1),
    .READ_RESET_VALUE_B("0"), .RST_MODE_A("SYNC"), .RST_MODE_B("SYNC"),
    .SIM_ASSERT_CHK(0), .USE_MEM_INIT(0),
    .WAKEUP_TIME("disable_sleep"), .WRITE_DATA_WIDTH_A(8),
    .WRITE_MODE_B("no_change")
  ) u_shift_bram (
    .clka(s_axi_aclk), .ena(1'b1), .wea(param_shift_wr_en),
    .addra(param_wr_addr), .dina(param_wr_data8),
    .injectsbiterra(1'b0), .injectdbiterra(1'b0),
    .clkb(s_axi_aclk), .enb(core_param_rd_en), .rstb(1'b0), .regceb(1'b1),
    .addrb(core_param_rd_addr), .doutb(param_shift_rd),
    .sbiterrb(), .dbiterrb(), .sleep(1'b0)
  );

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
  // Register writes + BRAM writes
  // ============================================================
  always_ff @(posedge s_axi_aclk) begin
    if (!axil_rstn) begin
      reg_tile_pixels <= 32'd0;
      reg_cin_run     <= 32'd0;
      reg_cout_run    <= 32'd0;
      reg_zp_relu     <= 32'd0;
      reg_oc_sel      <= 32'd0;
      reg_w_bram_off  <= 32'd0;
      reg_param_addr  <= 32'd0;
    end else begin
      if (slv_reg_wren) begin
        if (wr_addr == ADDR_TILE_PIXELS)
          reg_tile_pixels <= s_axi_wdata;
        else if (wr_addr == ADDR_CIN_RUN)
          reg_cin_run <= s_axi_wdata;
        else if (wr_addr == ADDR_COUT_RUN)
          reg_cout_run <= s_axi_wdata;
        else if (wr_addr == ADDR_ZP_RELU)
          reg_zp_relu <= s_axi_wdata;
        else if (wr_addr == ADDR_OC_SEL)
          reg_oc_sel <= s_axi_wdata;
        else if (wr_addr == ADDR_W_BRAM_OFF)
          reg_w_bram_off <= s_axi_wdata;
        else if (wr_addr == ADDR_PARAM_ADDR)
          reg_param_addr <= s_axi_wdata;
        else if (wr_addr == ADDR_VQ_CTRL)
          reg_vq_ctrl <= s_axi_wdata;
      end
    end
  end

  // Codeword-norm ROM write. One AXI-Lite write per entry, 128 entries total.
  always_ff @(posedge s_axi_aclk) begin
    if (!s_axi_aresetn) begin
      vq_norm_we <= 1'b0;
    end else begin
      vq_norm_we   <= slv_reg_wren && (wr_addr == ADDR_VQ_NORM) && (USE_PW_VQ != 0);
      // Address widened 7 -> VQ_AW bits (8 at VQ_NORM_D=256). Backward
      // compatible: addresses 0..127 decode exactly as before.
      vq_norm_addr <= s_axi_wdata[VQ_AW-1:0];
      // ||v_k||^2 stays a 20-bit field. It is non-negative and at most
      // 16384*Dm = 262144 at Dm=16, which fits 20-bit signed with room;
      // only the SCORE needed widening, so the register map is unchanged.
      vq_norm_data <= VQ_SCORE_W'(signed'(s_axi_wdata[31:12]));
    end
  end

  // Weight BRAM write: triggered when AXI-Lite writes to weight space
  always_comb begin
    w_wr_en   = '0;
    w_wr_addr = '0;
    w_wr_data = '0;
    if (slv_reg_wren &&
        (wr_addr >= ADDR_W_BASE) &&
        (wr_addr < (ADDR_W_BASE + (CIN_MAX * 4)))) begin
      w_wr_addr = reg_w_bram_off[W_AW-1:0] + weight_idx(wr_addr);
      w_wr_data = s_axi_wdata[DATA_WIDTH-1:0];
      w_wr_en[oc_wr_sel] = 1'b1;
    end
  end

  // Param BRAM writes: triggered when writing BIAS, MULT, or SHIFT
  always_comb begin
    param_wr_addr      = reg_param_addr[PARAM_AW-1:0];
    param_wr_data32    = s_axi_wdata;
    param_wr_data8     = s_axi_wdata[7:0];
    param_bias_wr_en   = 1'b0;
    param_mult_wr_en   = 1'b0;
    param_shift_wr_en  = 1'b0;

    if (slv_reg_wren) begin
      if (wr_addr == ADDR_BIAS)
        param_bias_wr_en = 1'b1;
      else if (wr_addr == ADDR_MULT)
        param_mult_wr_en = 1'b1;
      else if (wr_addr == ADDR_SHIFT)
        param_shift_wr_en = 1'b1;
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
        s_axi_bresp  <= 2'b00;
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
    end else if (rd_addr == ADDR_COUT_RUN) begin
      s_axi_rdata = reg_cout_run;
    end else if (rd_addr == ADDR_ZP_RELU) begin
      s_axi_rdata = reg_zp_relu;
    end else if (rd_addr == ADDR_OC_SEL) begin
      s_axi_rdata = reg_oc_sel;
    end else if (rd_addr == ADDR_W_BRAM_OFF) begin
      s_axi_rdata = reg_w_bram_off;
    end else if (rd_addr == ADDR_PARAM_ADDR) begin
      s_axi_rdata = reg_param_addr;
    end else if (rd_addr == ADDR_VQ_CTRL) begin
      s_axi_rdata = reg_vq_ctrl;
    end else if (rd_addr == ADDR_STATUS2) begin
      s_axi_rdata[0] = in_overflow_sticky;
      s_axi_rdata[1] = in_underflow_sticky;
      s_axi_rdata[2] = out_overflow_sticky;
      s_axi_rdata[3] = out_underflow_sticky;
      s_axi_rdata[4] = start_while_busy_sticky;
      s_axi_rdata[5] = cfg_err_sticky;
    end
  end

  always_ff @(posedge s_axi_aclk) begin
    if (!axil_rstn) begin
      s_axi_rvalid <= 1'b0;
      s_axi_rresp  <= 2'b00;
    end else begin
      if (s_axi_arready && s_axi_arvalid && !s_axi_rvalid) begin
        s_axi_rvalid <= 1'b1;
        s_axi_rresp  <= 2'b00;
      end else if (s_axi_rvalid && s_axi_rready) begin
        s_axi_rvalid <= 1'b0;
      end
    end
  end

  // ============================================================
  // PW datapath instance
  // ============================================================
  logic pw_done_out;
  logic pw_cfg_err;
  logic pw_cfg_err_stb;

  pw_single_oc_axis #(
    .DATA_WIDTH(DATA_WIDTH), .ACC_WIDTH(ACC_WIDTH), .CIN_MAX(CIN_MAX),
    .COUT_MAX(COUT_MAX), .TILE_PIXELS_MAX(TILE_PIXELS_MAX),
    .IN_FIFO_DEPTH(IN_FIFO_DEPTH), .OUT_FIFO_DEPTH(OUT_FIFO_DEPTH),
    .S_AXIS_DATA_WIDTH(S_AXIS_DATA_WIDTH),
    .N_LANES(N_LANES), .N_OC(N_OC), .M_AXIS_DATA_WIDTH(M_AXIS_DATA_WIDTH),
    .USE_PW_VQ(USE_PW_VQ),
    .VQ_K(VQ_K), .VQ_NORM_D(VQ_NORM_D), .VQ_SCORE_W(VQ_SCORE_W)
  ) u_pw (
    .clk(s_axi_aclk), .rst_n(s_axi_aresetn),
    .start_in(start_pulse), .done_out(pw_done_out),
    .tile_pixels(reg_tile_pixels), .cin_run(reg_cin_run[11:0]),
    .cout_run(reg_cout_run[11:0]),
    .zp_in(reg_zp_relu[7:0]), .zp_out(reg_zp_relu[15:8]), .relu_en(reg_zp_relu[16]),
    // VQ mode. Hard-tied off unless USE_PW_VQ is set, so a stray register
    // write cannot perturb convolution in a build that did not ask for VQ.
    .vq_mode    ((USE_PW_VQ != 0) ? reg_vq_ctrl[0]     : 1'b0),
    .vq_cin_load((USE_PW_VQ != 0) ? reg_vq_ctrl[23:12] : 12'd0),
    .vq_norm_we(vq_norm_we), .vq_norm_addr(vq_norm_addr),
    .vq_norm_data(vq_norm_data), .cfg_err(pw_cfg_err),
    .cfg_err_stb(pw_cfg_err_stb),

    // Weight BRAM read (from core)
    .w_rd_addr(core_w_rd_addr), .w_rd_en(core_w_rd_en),
    .w_rd_data(w_rd_data),

    // Param BRAM read (from core)
    .param_rd_addr(core_param_rd_addr), .param_rd_en(core_param_rd_en),
    .param_bias_data(param_bias_rd), .param_mult_data(param_mult_rd),
    .param_shift_data(param_shift_rd),

    .s_axis_tdata(core_s_tdata), .s_axis_tvalid(core_s_tvalid),
    .s_axis_tready(core_s_tready), .s_axis_tlast(core_s_tlast),
    .m_axis_tdata(core_m_tdata), .m_axis_tvalid(core_m_tvalid),
    .m_axis_tready(core_m_tready), .m_axis_tlast(core_m_tlast),
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
      cfg_err_sticky          <= 1'b0;
    end else begin
      if (clear_err_pulse) begin
        in_overflow_sticky      <= 1'b0;
        in_underflow_sticky     <= 1'b0;
        out_overflow_sticky     <= 1'b0;
        out_underflow_sticky    <= 1'b0;
        start_while_busy_sticky <= 1'b0;
        cfg_err_sticky          <= 1'b0;
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
        // The core refused a start because the geometry would alias. This
        // is the flag that replaces the SIMCOPY-only $error.
        //
        // FROM THE STROBE. Not from pw_cfg_err, which is a LEVEL the core
        // holds until the next start it accepts:
        //   - latching the level makes this bit UNCLEARABLE, because the write
        //     zeroes it for one cycle and the level sets it straight back;
        //   - latching the level's rising EDGE makes it clearable but MISSES a
        //     second refusal that follows a first with no accepted start in
        //     between, since the level never fell.
        // Both were observed. The strobe is one cycle per refused start, which
        // is exactly what a clearable latch needs.
        if (pw_cfg_err_stb)
          cfg_err_sticky <= 1'b1;
      end
    end
  end

endmodule

`default_nettype wire