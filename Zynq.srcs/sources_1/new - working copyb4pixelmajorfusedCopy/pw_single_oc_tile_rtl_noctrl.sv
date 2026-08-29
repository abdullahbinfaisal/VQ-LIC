`timescale 1ns/1ps

module pw_single_oc_tile_rt_noctrl #(
  parameter int DATA_WIDTH      = 8,
  parameter int ACC_WIDTH       = 32,
  parameter int CIN_MAX         = 240,
  parameter int TILE_PIXELS_MAX = 16384,
  parameter int N_LANES         = 4,
  parameter int N_OC            = 2
)(
  input  logic                              clk,
  input  logic                              rst_n,

  input  logic                              start_in,
  output logic                              done_out,

  input  logic [31:0]                       tile_pixels,
  input  logic [11:0]                       cin_run,

  input  logic [7:0]                        zp_in,
  input  logic [7:0]                        zp_out,
  input  logic                              relu_en,

  // Per-OC parameters
  input  logic signed [31:0]                bias_in    [0:N_OC-1],
  input  logic [31:0]                       mult_conv  [0:N_OC-1],
  input  logic [7:0]                        shift_conv [0:N_OC-1],

  // Per-OC weights
  input  logic signed [DATA_WIDTH-1:0]      w_ic [0:N_OC-1][0:CIN_MAX-1],

  input  logic                              valid_in,
  input  logic [N_LANES*DATA_WIDTH-1:0]     pixel_in,
  output logic                              consume_in,

  input  logic                              out_stall,

  output logic [$clog2(CIN_MAX)-1:0]        ic_sel_dbg,

  output logic [N_LANES*DATA_WIDTH-1:0]     pixel_out,
  output logic                              valid_out
);

  // ------------------------------------------------------------
  // Address / depth calculations
  // ------------------------------------------------------------
  localparam int LANE_SHIFT = $clog2(N_LANES);
  localparam int BANK_DEPTH = TILE_PIXELS_MAX / N_LANES;
  localparam int BANK_AW    = (BANK_DEPTH <= 1) ? 1 : $clog2(BANK_DEPTH);

  wire [31:0] tile_groups = tile_pixels >> LANE_SHIFT;

  typedef enum logic [1:0] {
    S_IDLE = 2'd0,
    S_ACC  = 2'd1,
    S_PPU  = 2'd2,
    S_DONE = 2'd3
  } state_t;

  state_t st;

  // ------------------------------------------------------------
  // Counters / pipeline registers
  // ------------------------------------------------------------
  logic [$clog2(CIN_MAX)-1:0] ic_sel;
  logic [31:0]                grp_idx;

  logic                            s1_valid;
  logic signed [DATA_WIDTH-1:0]    s1_weight [0:N_OC-1];
  logic                            acc_draining;

  logic [BANK_AW-1:0]         idx_q;
  logic [DATA_WIDTH-1:0]      pixel_q  [0:N_LANES-1];
  logic                       first_ic_q;

  assign ic_sel_dbg = ic_sel;

  // ------------------------------------------------------------
  // PSUM RAM: N_OC x N_LANES banked SDPRAM
  // ------------------------------------------------------------
  logic [BANK_AW-1:0]            psum_raddr;
  logic                          psum_re;
  logic signed [ACC_WIDTH-1:0]   psum_rdata [0:N_OC-1][0:N_LANES-1];

  logic [BANK_AW-1:0]            psum_waddr;
  logic                          psum_we;
  logic signed [ACC_WIDTH-1:0]   psum_wdata [0:N_OC-1][0:N_LANES-1];

  generate
    for (genvar oc = 0; oc < N_OC; oc++) begin : G_OC
      for (genvar g = 0; g < N_LANES; g++) begin : G_LANE
        xpm_memory_sdpram #(
          .ADDR_WIDTH_A        (BANK_AW),
          .ADDR_WIDTH_B        (BANK_AW),
          .AUTO_SLEEP_TIME     (0),
          .BYTE_WRITE_WIDTH_A  (ACC_WIDTH),
          .CLOCKING_MODE       ("common_clock"),
          .ECC_MODE            ("no_ecc"),
          .MEMORY_INIT_FILE    ("none"),
          .MEMORY_INIT_PARAM   ("0"),
          .MEMORY_OPTIMIZATION ("true"),
          .MEMORY_PRIMITIVE    ("block"),
          .MEMORY_SIZE         (BANK_DEPTH * ACC_WIDTH),
          .MESSAGE_CONTROL     (0),
          .READ_DATA_WIDTH_B   (ACC_WIDTH),
          .READ_LATENCY_B      (1),
          .READ_RESET_VALUE_B  ("0"),
          .RST_MODE_A          ("SYNC"),
          .RST_MODE_B          ("SYNC"),
          .SIM_ASSERT_CHK      (0),
          .USE_MEM_INIT        (0),
          .WAKEUP_TIME         ("disable_sleep"),
          .WRITE_DATA_WIDTH_A  (ACC_WIDTH),
          .WRITE_MODE_B        ("no_change")
        ) u_psum_ram (
          .clka   (clk), .ena  (1'b1), .wea (psum_we),
          .addra  (psum_waddr), .dina (psum_wdata[oc][g]),
          .injectsbiterra (1'b0), .injectdbiterra (1'b0),
          .clkb   (clk), .enb  (psum_re), .rstb (1'b0), .regceb (1'b1),
          .addrb  (psum_raddr), .doutb (psum_rdata[oc][g]),
          .sbiterrb (), .dbiterrb (), .sleep (1'b0)
        );
      end
    end
  endgenerate

  // ------------------------------------------------------------
  // MAC: shared act_diff, per-OC weight broadcast
  // ------------------------------------------------------------
  logic signed [8:0]           act_diff   [0:N_LANES-1];
  logic signed [16:0]          prod       [0:N_OC-1][0:N_LANES-1];
  logic signed [ACC_WIDTH-1:0] prod_ext   [0:N_OC-1][0:N_LANES-1];
  logic signed [ACC_WIDTH-1:0] accum_next [0:N_OC-1][0:N_LANES-1];

  generate
    for (genvar g = 0; g < N_LANES; g++) begin : G_ACT
      always_comb
        act_diff[g] = $signed({1'b0, pixel_q[g]}) - $signed({1'b0, zp_in});
    end
    for (genvar oc = 0; oc < N_OC; oc++) begin : G_MAC_OC
      for (genvar g = 0; g < N_LANES; g++) begin : G_MAC_LANE
        always_comb begin
          prod[oc][g]       = act_diff[g] * s1_weight[oc];
          prod_ext[oc][g]   = $signed(prod[oc][g]);
          accum_next[oc][g] = prod_ext[oc][g] + (first_ic_q ? '0 : psum_rdata[oc][g]);
        end
      end
    end
  endgenerate

  // ------------------------------------------------------------
  // N_LANES PPUs (reused across OCs in sequential S_PPU)
  // ------------------------------------------------------------
  logic                              ppu_valid_in;
  logic signed [ACC_WIDTH-1:0]       ppu_acc_in    [0:N_LANES-1];
  logic [DATA_WIDTH-1:0]             ppu_pixel_out [0:N_LANES-1];
  logic [N_LANES-1:0]                ppu_valid_out_vec;

  // Select params for current OC being post-processed
  logic [$clog2(N_OC>1?N_OC:2)-1:0] oc_ppu_idx;
  logic signed [31:0]                ppu_bias;
  logic [31:0]                       ppu_mult;
  logic [7:0]                        ppu_shift;

  always_comb begin
    ppu_bias  = bias_in[oc_ppu_idx];
    ppu_mult  = mult_conv[oc_ppu_idx];
    ppu_shift = shift_conv[oc_ppu_idx];
  end

  generate
    for (genvar g = 0; g < N_LANES; g++) begin : G_PPU
      ppu #(.DATA_WIDTH(DATA_WIDTH), .ACC_WIDTH(ACC_WIDTH)) u_ppu (
        .clk(clk), .rst_n(rst_n),
        .mode_residual(1'b0), .relu_en(relu_en),
        .mult_conv(ppu_mult), .shift_conv(ppu_shift), .bias_in(ppu_bias),
        .mult_res_a(32'd0), .mult_res_b(32'd0), .shift_res(8'd0),
        .zp_out(zp_out), .zp_in_a(8'd0), .zp_in_b(8'd0),
        .valid_in(ppu_valid_in), .conv_acc_in(ppu_acc_in[g]),
        .res_a_in(8'd0), .res_b_in(8'd0),
        .pixel_out(ppu_pixel_out[g]), .valid_out(ppu_valid_out_vec[g])
      );
    end
  endgenerate

  wire ppu_valid_out = ppu_valid_out_vec[0];

  generate
    for (genvar g = 0; g < N_LANES; g++) begin : G_PX_OUT
      assign pixel_out[g*DATA_WIDTH +: DATA_WIDTH] = ppu_pixel_out[g];
    end
  endgenerate
  assign valid_out = ppu_valid_out;

  logic [31:0] ppu_issue_idx;
  logic        ppu_rd_en_d1;
  logic        ppu_rd_en_d2;
  logic [31:0] out_cnt;

  // ------------------------------------------------------------
  // Main controller
  // ------------------------------------------------------------
  integer li, oci;
  always_ff @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin
      st <= S_IDLE; done_out <= 1'b0; consume_in <= 1'b0;
      ic_sel <= '0; grp_idx <= 32'd0;
      s1_valid <= 1'b0; acc_draining <= 1'b0;
      for (oci=0; oci<N_OC; oci++) s1_weight[oci] <= '0;
      idx_q <= '0; first_ic_q <= 1'b0;
      for (li=0; li<N_LANES; li++) pixel_q[li] <= '0;
      psum_raddr <= '0; psum_re <= 1'b0;
      psum_waddr <= '0; psum_we <= 1'b0;
      for (oci=0; oci<N_OC; oci++)
        for (li=0; li<N_LANES; li++) psum_wdata[oci][li] <= '0;
      ppu_issue_idx <= 32'd0; ppu_rd_en_d1 <= 1'b0; ppu_rd_en_d2 <= 1'b0;
      out_cnt <= 32'd0; oc_ppu_idx <= '0;
      ppu_valid_in <= 1'b0;
      for (li=0; li<N_LANES; li++) ppu_acc_in[li] <= '0;
    end else begin
      done_out <= 1'b0; consume_in <= 1'b0;
      psum_re <= 1'b0; psum_we <= 1'b0;
      ppu_valid_in <= 1'b0;
      for (li=0; li<N_LANES; li++) ppu_acc_in[li] <= '0;

      case (st)

        S_IDLE: begin
          if (start_in) begin
            ic_sel <= '0; grp_idx <= 32'd0;
            s1_valid <= 1'b0; acc_draining <= 1'b0;
            for (oci=0; oci<N_OC; oci++) s1_weight[oci] <= '0;
            ppu_issue_idx <= 32'd0; ppu_rd_en_d1 <= 1'b0;
            ppu_rd_en_d2 <= 1'b0; out_cnt <= 32'd0; oc_ppu_idx <= '0;
            st <= ((tile_pixels == 0) || (cin_run == 0)) ? S_DONE : S_ACC;
          end
        end

        // ----------------------------------------------------------
        // S_ACC: N_OC × N_LANES parallel accumulations per clock
        // ----------------------------------------------------------
        S_ACC: begin
          // Stage B: writeback all OC × lane banks
          if (s1_valid) begin
            psum_waddr <= idx_q;
            psum_we    <= 1'b1;
            for (oci=0; oci<N_OC; oci++)
              for (li=0; li<N_LANES; li++)
                psum_wdata[oci][li] <= accum_next[oci][li];
          end

          // Stage A: consume 4 pixels, broadcast to all OC MACs
          if (!acc_draining && valid_in) begin
            consume_in <= 1'b1;
            s1_valid   <= 1'b1;
            idx_q      <= grp_idx[BANK_AW-1:0];
            for (li=0; li<N_LANES; li++)
              pixel_q[li] <= pixel_in[li*DATA_WIDTH +: DATA_WIDTH];
            first_ic_q <= (ic_sel == '0);
            for (oci=0; oci<N_OC; oci++)
              s1_weight[oci] <= w_ic[oci][ic_sel];
            psum_raddr <= grp_idx[BANK_AW-1:0];
            psum_re    <= 1'b1;

            if ((grp_idx + 1) == tile_groups) begin
              grp_idx <= 32'd0;
              if (($unsigned(ic_sel) + 1) == cin_run)
                acc_draining <= 1'b1;
              else
                ic_sel <= ic_sel + 1'b1;
            end else
              grp_idx <= grp_idx + 32'd1;
          end else
            s1_valid <= 1'b0;

          if (acc_draining && !s1_valid) begin
            acc_draining <= 1'b0;
            ppu_issue_idx <= 32'd0; ppu_rd_en_d1 <= 1'b0;
            ppu_rd_en_d2 <= 1'b0; out_cnt <= 32'd0; oc_ppu_idx <= '0;
            st <= S_PPU;
          end
        end

        // ----------------------------------------------------------
        // S_PPU: sequential per OC (OC0 first, then OC1, ...)
        //   PPU params switch via oc_ppu_idx MUX.
        //   Pipeline drains naturally between OC switches.
        // ----------------------------------------------------------
        S_PPU: begin
          if (!out_stall) begin
            ppu_rd_en_d2 <= ppu_rd_en_d1;

            if (ppu_issue_idx < tile_groups) begin
              psum_raddr    <= ppu_issue_idx[BANK_AW-1:0];
              psum_re       <= 1'b1;
              ppu_rd_en_d1  <= 1'b1;
              ppu_issue_idx <= ppu_issue_idx + 32'd1;
            end else
              ppu_rd_en_d1  <= 1'b0;

            ppu_valid_in <= ppu_rd_en_d2;
            if (ppu_rd_en_d2)
              for (li=0; li<N_LANES; li++)
                ppu_acc_in[li] <= psum_rdata[oc_ppu_idx][li];

            if (ppu_valid_out) begin
              out_cnt <= out_cnt + 32'd1;
              if ((out_cnt + 1) == tile_groups) begin
                if (($unsigned(oc_ppu_idx) + 1) == N_OC)
                  st <= S_DONE;
                else begin
                  oc_ppu_idx    <= oc_ppu_idx + 1'b1;
                  ppu_issue_idx <= 32'd0;
                  ppu_rd_en_d1  <= 1'b0;
                  ppu_rd_en_d2  <= 1'b0;
                  out_cnt       <= 32'd0;
                end
              end
            end
          end
        end

        S_DONE: begin done_out <= 1'b1; st <= S_IDLE; end
        default: st <= S_IDLE;
      endcase
    end
  end

endmodule

`default_nettype wire