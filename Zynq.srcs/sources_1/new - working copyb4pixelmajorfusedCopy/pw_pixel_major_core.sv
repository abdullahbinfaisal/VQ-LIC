`timescale 1ns/1ps

module pw_pixel_major_core #(
  parameter int DATA_WIDTH      = 8,
  parameter int ACC_WIDTH       = 32,
  parameter int CIN_MAX         = 240,
  parameter int COUT_MAX        = 240,
  parameter int N_LANES         = 8,
  parameter int N_OC            = 5
)(
  input  logic                              clk,
  input  logic                              rst_n,

  input  logic                              start_in,
  output logic                              done_out,

  input  logic [31:0]                       tile_pixels,
  input  logic [11:0]                       cin_run,
  input  logic [11:0]                       cout_run,    // total output channels

  input  logic [7:0]                        zp_in,
  input  logic [7:0]                        zp_out,
  input  logic                              relu_en,

  // Weight BRAM read interface (all N_OC banks, 1-cycle latency)
  output logic [$clog2((COUT_MAX/N_OC)*CIN_MAX)-1:0] w_rd_addr,
  output logic                              w_rd_en,
  input  logic signed [DATA_WIDTH-1:0]      w_rd_data  [0:N_OC-1],

  // Param BRAM read interface (bias/mult/shift, 1-cycle latency)
  output logic [$clog2(COUT_MAX)-1:0]       param_rd_addr,
  output logic                              param_rd_en,
  input  logic signed [31:0]                param_bias_data,
  input  logic [31:0]                       param_mult_data,
  input  logic [7:0]                        param_shift_data,

  // Pixel input from input FIFO
  input  logic                              valid_in,
  input  logic [N_LANES*DATA_WIDTH-1:0]     pixel_in,
  output logic                              consume_in,

  // Pixel output to output FIFO
  output logic [N_LANES*DATA_WIDTH-1:0]     pixel_out,
  output logic                              valid_out,

  // Backpressure from output FIFO
  input  logic                              out_stall
);

  // ------------------------------------------------------------
  // Constants
  // ------------------------------------------------------------
  localparam int LANE_SHIFT   = $clog2(N_LANES);
  localparam int W_DEPTH      = (COUT_MAX / N_OC) * CIN_MAX;
  localparam int W_AW         = (W_DEPTH <= 1) ? 1 : $clog2(W_DEPTH);
  localparam int PB_AW        = (CIN_MAX <= 1) ? 1 : $clog2(CIN_MAX);
  localparam int PARAM_AW     = (COUT_MAX <= 1) ? 1 : $clog2(COUT_MAX);

  wire [31:0] tile_groups  = tile_pixels >> LANE_SHIFT;
  wire [11:0] cout_batches = cout_run / N_OC;

  // ------------------------------------------------------------
  // FSM
  // ------------------------------------------------------------
  typedef enum logic [2:0] {
    S_IDLE     = 3'd0,
    S_LOAD_GRP = 3'd1,
    S_COMPUTE  = 3'd2,
    S_PPU_LOAD = 3'd3,   // pre-read params before PPU
    S_PPU      = 3'd4,
    S_DONE     = 3'd5
  } state_t;

  state_t st;

  // ------------------------------------------------------------
  // Counters
  // ------------------------------------------------------------
  logic [31:0]        grp_idx;       // pixel group index
  logic [$clog2(CIN_MAX)-1:0] ic_idx;  // input channel index
  logic [11:0]        oc_batch_idx;  // output channel batch
  logic [W_AW-1:0]    w_addr_base;   // weight BRAM base for current batch

  // Compute pipeline
  logic               rd_issued;     // BRAM read was issued last cycle
  logic               first_ic;      // first IC (clear accumulator)

  // PPU
  logic [$clog2(N_OC>1?N_OC:2)-1:0] ppu_issue_idx;
  logic [31:0]        ppu_out_cnt;   // outputs received from PPU this batch

  // Latched params for PPU (from BRAM, read 1 cycle early)
  logic signed [31:0] ppu_bias_q;
  logic [31:0]        ppu_mult_q;
  logic [7:0]         ppu_shift_q;
  logic               ppu_params_valid;

  // ------------------------------------------------------------
  // Pixel buffer BRAM: CIN_MAX × (N_LANES * DATA_WIDTH)
  // Write during S_LOAD_GRP, Read during S_COMPUTE
  // ------------------------------------------------------------
  localparam int PB_WIDTH = N_LANES * DATA_WIDTH;

  logic [PB_AW-1:0]   pb_wr_addr;
  logic                pb_wr_en;
  logic [PB_WIDTH-1:0] pb_wr_data;
  logic [PB_AW-1:0]   pb_rd_addr;
  logic                pb_rd_en;
  logic [PB_WIDTH-1:0] pb_rd_data;

  xpm_memory_sdpram #(
    .ADDR_WIDTH_A        (PB_AW),
    .ADDR_WIDTH_B        (PB_AW),
    .AUTO_SLEEP_TIME     (0),
    .BYTE_WRITE_WIDTH_A  (PB_WIDTH),
    .CLOCKING_MODE       ("common_clock"),
    .ECC_MODE            ("no_ecc"),
    .MEMORY_INIT_FILE    ("none"),
    .MEMORY_INIT_PARAM   ("0"),
    .MEMORY_OPTIMIZATION ("true"),
    .MEMORY_PRIMITIVE    ("auto"),
    .MEMORY_SIZE         (CIN_MAX * PB_WIDTH),
    .MESSAGE_CONTROL     (0),
    .READ_DATA_WIDTH_B   (PB_WIDTH),
    .READ_LATENCY_B      (1),
    .READ_RESET_VALUE_B  ("0"),
    .RST_MODE_A          ("SYNC"),
    .RST_MODE_B          ("SYNC"),
    .SIM_ASSERT_CHK      (0),
    .USE_MEM_INIT        (0),
    .WAKEUP_TIME         ("disable_sleep"),
    .WRITE_DATA_WIDTH_A  (PB_WIDTH),
    .WRITE_MODE_B        ("read_first")
  ) u_pixel_buf (
    .clka  (clk), .ena (1'b1), .wea (pb_wr_en),
    .addra (pb_wr_addr), .dina (pb_wr_data),
    .injectsbiterra(1'b0), .injectdbiterra(1'b0),
    .clkb  (clk), .enb (pb_rd_en), .rstb (1'b0), .regceb (1'b1),
    .addrb (pb_rd_addr), .doutb (pb_rd_data),
    .sbiterrb(), .dbiterrb(), .sleep(1'b0)
  );

  // ------------------------------------------------------------
  // Accumulator registers: N_OC × N_LANES
  // ------------------------------------------------------------
  logic signed [ACC_WIDTH-1:0] acc [0:N_OC-1][0:N_LANES-1];

  // MAC pipeline stage (1-cycle latency from BRAM read)
  logic signed [8:0]            act_diff  [0:N_LANES-1];
  logic signed [DATA_WIDTH:0]   prod      [0:N_OC-1][0:N_LANES-1];
  logic signed [ACC_WIDTH-1:0]  prod_ext  [0:N_OC-1][0:N_LANES-1];

  // Unpack pixel buffer read data into per-lane values
  logic [DATA_WIDTH-1:0] pb_pixel [0:N_LANES-1];
  generate
    for (genvar g = 0; g < N_LANES; g++) begin : G_PB_UNPACK
      assign pb_pixel[g] = pb_rd_data[g*DATA_WIDTH +: DATA_WIDTH];
    end
  endgenerate

  // Compute act_diff and products (combinational, used when rd_issued)
  generate
    for (genvar g = 0; g < N_LANES; g++) begin : G_ACT_DIFF
      always_comb
        act_diff[g] = $signed({1'b0, pb_pixel[g]}) - $signed({1'b0, zp_in});
    end
    for (genvar oc = 0; oc < N_OC; oc++) begin : G_MAC_OC
      for (genvar g = 0; g < N_LANES; g++) begin : G_MAC_LANE
        always_comb begin
          prod[oc][g]     = act_diff[g] * w_rd_data[oc];
          prod_ext[oc][g] = $signed(prod[oc][g]);
        end
      end
    end
  endgenerate

  // ------------------------------------------------------------
  // N_LANES PPUs (reused across OCs sequentially)
  // ------------------------------------------------------------
  logic                              ppu_valid_in;
  logic signed [ACC_WIDTH-1:0]       ppu_acc_in    [0:N_LANES-1];
  logic [DATA_WIDTH-1:0]             ppu_pixel_out [0:N_LANES-1];
  logic [N_LANES-1:0]                ppu_valid_out_vec;

  generate
    for (genvar g = 0; g < N_LANES; g++) begin : G_PPU
      ppu #(.DATA_WIDTH(DATA_WIDTH), .ACC_WIDTH(ACC_WIDTH)) u_ppu (
        .clk(clk), .rst_n(rst_n),
        .mode_residual(1'b0), .relu_en(relu_en),
        .mult_conv(ppu_mult_q), .shift_conv(ppu_shift_q), .bias_in(ppu_bias_q),
        .mult_res_a(32'd0), .mult_res_b(32'd0), .shift_res(8'd0),
        .zp_out(zp_out), .zp_in_a(8'd0), .zp_in_b(8'd0),
        .valid_in(ppu_valid_in), .conv_acc_in(ppu_acc_in[g]),
        .res_a_in(8'd0), .res_b_in(8'd0),
        .pixel_out(ppu_pixel_out[g]), .valid_out(ppu_valid_out_vec[g])
      );
    end
  endgenerate

  wire ppu_valid_out = ppu_valid_out_vec[0];

  // Pack PPU output into pixel_out bus
  generate
    for (genvar g = 0; g < N_LANES; g++) begin : G_PX_OUT
      assign pixel_out[g*DATA_WIDTH +: DATA_WIDTH] = ppu_pixel_out[g];
    end
  endgenerate
  assign valid_out = ppu_valid_out;

  // ------------------------------------------------------------
  // Main Controller
  // ------------------------------------------------------------
  integer li, oci;

  always_ff @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin
      st            <= S_IDLE;
      done_out      <= 1'b0;
      consume_in    <= 1'b0;
      grp_idx       <= 32'd0;
      ic_idx        <= '0;
      oc_batch_idx  <= 12'd0;
      w_addr_base   <= '0;
      w_rd_addr     <= '0;
      w_rd_en       <= 1'b0;
      pb_wr_addr    <= '0;
      pb_wr_en      <= 1'b0;
      pb_wr_data    <= '0;
      pb_rd_addr    <= '0;
      pb_rd_en      <= 1'b0;
      rd_issued     <= 1'b0;
      first_ic      <= 1'b1;
      for (oci = 0; oci < N_OC; oci++)
        for (li = 0; li < N_LANES; li++)
          acc[oci][li] <= '0;
      ppu_valid_in  <= 1'b0;
      ppu_issue_idx <= '0;
      ppu_out_cnt   <= 32'd0;
      ppu_params_valid <= 1'b0;
      param_rd_addr <= '0;
      param_rd_en   <= 1'b0;
      ppu_bias_q    <= '0;
      ppu_mult_q    <= '0;
      ppu_shift_q   <= '0;
      for (li = 0; li < N_LANES; li++)
        ppu_acc_in[li] <= '0;
    end else begin
      // Default deasserts
      done_out     <= 1'b0;
      consume_in   <= 1'b0;
      pb_wr_en     <= 1'b0;
      pb_rd_en     <= 1'b0;
      w_rd_en      <= 1'b0;
      ppu_valid_in <= 1'b0;
      param_rd_en  <= 1'b0;
      ppu_params_valid <= 1'b0;

      case (st)

        // --------------------------------------------------------
        // S_IDLE: wait for start
        // --------------------------------------------------------
        S_IDLE: begin
          if (start_in) begin
            grp_idx      <= 32'd0;
            ic_idx       <= '0;
            oc_batch_idx <= 12'd0;
            w_addr_base  <= '0;
            rd_issued    <= 1'b0;
            first_ic     <= 1'b1;
            ppu_out_cnt  <= 32'd0;
            if ((tile_pixels == 0) || (cin_run == 0) || (cout_run == 0))
              st <= S_DONE;
            else
              st <= S_LOAD_GRP;
          end
        end

        // --------------------------------------------------------
        // S_LOAD_GRP: buffer one pixel group (Cin beats from input)
        // --------------------------------------------------------
        S_LOAD_GRP: begin
          if (valid_in) begin
            consume_in <= 1'b1;
            pb_wr_en   <= 1'b1;
            pb_wr_addr <= ic_idx[PB_AW-1:0];
            pb_wr_data <= pixel_in;

            if (($unsigned(ic_idx) + 1) == cin_run) begin
              ic_idx       <= '0;
              oc_batch_idx <= 12'd0;
              w_addr_base  <= '0;
              rd_issued    <= 1'b0;
              first_ic     <= 1'b1;
              // Clear accumulators for first batch
              for (oci = 0; oci < N_OC; oci++)
                for (li = 0; li < N_LANES; li++)
                  acc[oci][li] <= '0;
              st <= S_COMPUTE;
            end else begin
              ic_idx <= ic_idx + 1'b1;
            end
          end
        end

        // --------------------------------------------------------
        // S_COMPUTE: accumulate N_OC channels for current OC batch
        //   Cin cycles read from pixel_buf and weight BRAM
        // --------------------------------------------------------
        S_COMPUTE: begin
          // Pipeline stage 2: accumulate (when BRAM data from previous cycle is ready)
          if (rd_issued) begin
            for (oci = 0; oci < N_OC; oci++)
              for (li = 0; li < N_LANES; li++)
                acc[oci][li] <= (first_ic ? '0 : acc[oci][li]) + prod_ext[oci][li];
            first_ic <= 1'b0;
          end

          // Pipeline stage 1: issue BRAM reads
          if ($unsigned(ic_idx) < cin_run) begin
            pb_rd_en   <= 1'b1;
            pb_rd_addr <= ic_idx[PB_AW-1:0];
            w_rd_en    <= 1'b1;
            w_rd_addr  <= w_addr_base + ic_idx;
            rd_issued  <= 1'b1;
            ic_idx     <= ic_idx + 1'b1;
          end else begin
            rd_issued <= 1'b0;
            if (!rd_issued) begin
              // Both pipeline stages drained — accumulation complete
              // Pre-read params for first PPU OC
              param_rd_en   <= 1'b1;
              param_rd_addr <= oc_batch_idx * N_OC;
              ppu_issue_idx <= '0;
              ppu_out_cnt   <= 32'd0;
              st            <= S_PPU_LOAD;
            end
          end
        end

        // --------------------------------------------------------
        // S_PPU_LOAD: 1-cycle wait for param BRAM read latency
        // --------------------------------------------------------
        S_PPU_LOAD: begin
          // Latch the params that arrived from the BRAM read
          ppu_bias_q  <= param_bias_data;
          ppu_mult_q  <= param_mult_data;
          ppu_shift_q <= param_shift_data;
          ppu_params_valid <= 1'b1;
          // Pre-read next OC params (OC+1)
          if (ppu_issue_idx + 1 < N_OC) begin
            param_rd_en   <= 1'b1;
            param_rd_addr <= oc_batch_idx * N_OC + ppu_issue_idx + 1;
          end
          st <= S_PPU;
        end

        // --------------------------------------------------------
        // S_PPU: feed accumulators to PPU, N_OC at a time
        // --------------------------------------------------------
        S_PPU: begin
          if (!out_stall) begin
            // Issue PPU input for current OC within batch
            if ($unsigned(ppu_issue_idx) < N_OC && ppu_params_valid) begin
              ppu_valid_in <= 1'b1;
              for (li = 0; li < N_LANES; li++)
                ppu_acc_in[li] <= acc[ppu_issue_idx][li];

              ppu_issue_idx <= ppu_issue_idx + 1'b1;

              // Pre-read params for next OC (if any more in this batch)
              if (ppu_issue_idx + 1 < N_OC) begin
                // Latch current params that arrived
                ppu_bias_q  <= param_bias_data;
                ppu_mult_q  <= param_mult_data;
                ppu_shift_q <= param_shift_data;
                // Issue read for the one after next
                if (ppu_issue_idx + 2 < N_OC) begin
                  param_rd_en   <= 1'b1;
                  param_rd_addr <= oc_batch_idx * N_OC + ppu_issue_idx + 2;
                end
              end
            end

            // Count PPU outputs
            if (ppu_valid_out) begin
              ppu_out_cnt <= ppu_out_cnt + 32'd1;
              if ((ppu_out_cnt + 1) == N_OC) begin
                // This OC batch is done
                oc_batch_idx <= oc_batch_idx + 12'd1;
                if (($unsigned(oc_batch_idx) + 1) == cout_batches) begin
                  // All OC batches done for this pixel group
                  grp_idx <= grp_idx + 32'd1;
                  if ((grp_idx + 1) == tile_groups)
                    st <= S_DONE;
                  else begin
                    // Next pixel group
                    ic_idx <= '0;
                    st     <= S_LOAD_GRP;
                  end
                end else begin
                  // Next OC batch — replay pixel buffer
                  ic_idx      <= '0;
                  w_addr_base <= w_addr_base + cin_run;
                  rd_issued   <= 1'b0;
                  first_ic    <= 1'b1;
                  // Clear accumulators
                  for (oci = 0; oci < N_OC; oci++)
                    for (li = 0; li < N_LANES; li++)
                      acc[oci][li] <= '0;
                  st <= S_COMPUTE;
                end
              end
            end
          end
        end

        // --------------------------------------------------------
        // S_DONE: pulse done
        // --------------------------------------------------------
        S_DONE: begin
          done_out <= 1'b1;
          st       <= S_IDLE;
        end

        default: st <= S_IDLE;
      endcase
    end
  end

endmodule

`default_nettype wire
