`timescale 1ns/1ps

module pw_pixel_major_core #(
  parameter int DATA_WIDTH      = 8,
  parameter int ACC_WIDTH       = 32,
  parameter int CIN_MAX         = 240,
  parameter int COUT_MAX        = 240,
  parameter int N_LANES         = 8,
  parameter int N_OC            = 5,
  // USE_PW_VQ: compile in the PW-hosted vector-quantisation mode. 0 = the
  // engine is exactly what it was before this feature existed (see the
  // bit-identity note at the vq_mode_r declaration below).
  parameter int USE_PW_VQ        = 0
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

  // ---- VQ mode (USE_PW_VQ only; tie low otherwise) ----
  // vq_mode      : 1 = nearest-codeword search, 0 = ordinary convolution
  // vq_cin_load  : channels STREAMED per group (64 = the full latent vector).
  //                cin_run keeps its ordinary meaning of MAC length and weight
  //                stride, which in VQ mode is 16 = 2 sub-codebooks x DSUB.
  input  logic                              vq_mode,
  input  logic [11:0]                       vq_cin_load,
  // Codeword-norm ROM write port. ||v_k||^2 for absolute OC vq_norm_addr.
  // A dedicated ROM, NOT the param-BRAM bias path: see the note at the VQ
  // branch below for why the bias path is unsafe for the last OC of a batch.
  input  logic                              vq_norm_we,
  input  logic [6:0]                        vq_norm_addr,
  input  logic signed [19:0]                vq_norm_data,

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
  localparam int PB_WIDTH     = N_LANES * DATA_WIDTH;

  // Registered at start - removes division from critical path
  logic [31:0] tile_groups_r;
  // ============================================================
  // VQ MODE  (see Final_code_2/src/vq_pw.h for the full mapping)
  //
  // The PW MAC already computes SUM_ic (uint8 act - uint8 zp_in) * int8 w.
  // With the codebook stored PRE-CENTRED as v = cq - 128 that is exactly the
  // dot product the nearest-codeword search needs, so the MAC, the DSP
  // packing, the accumulators, the shadow copy and the drain FSM are ALL
  // untouched. Only two things differ in VQ mode:
  //
  //   1. cin_load = 64  -- the stream still delivers the whole latent vector
  //      for the group (one contiguous DDR read), while cin_run = 16 remains
  //      the per-batch MAC length.
  //   2. the pb_ram READ window starts at w_addr_base instead of 0.
  //      w_addr_base already resets to 0 at every group boundary and advances
  //      by cin_run per OC batch, so in VQ mode it ALREADY equals
  //      16 * oc_batch_idx -- the exact window base each batch needs. No new
  //      counter, no multiplier, no extra state.
  //
  // BIT-IDENTITY WITH ORDINARY CONVOLUTION: USE_PW_VQ is an elaboration-time
  // constant. At USE_PW_VQ = 0 both ternaries below fold to their else-arms,
  // which are character-for-character the expressions that were in the RTL
  // before this feature, and vq_mode_r/vq_cin_load_r lose all readers and are
  // stripped. Conv mode cannot change; there is no path by which it could.
  logic                vq_mode_r;
  logic [11:0]         vq_cin_load_r;

  logic [11:0] cout_batches_r;
  // PARTIAL-BATCH DRAIN (2026-08-07). Previously the drain always issued N_OC
  // beats, so a layer whose cout is not a multiple of N_OC paid for padding
  // channels that were computed and discarded. Block 0 (cout=16) at N_OC=32
  // measured 42.02 cyc/group against 26.02 at N_OC=16 -- exactly the 16 extra
  // drain cycles. Draining only the valid channels removes that penalty and
  // decouples the choice of N_OC from the layer's cout.
  logic [11:0] cout_run_r;
  logic [11:0] ppu_drain_len;   // valid output channels in the batch being drained
  // The acc->shadow copy must be bounded the same way. Bounding only the drain
  // is not enough: the copy runs N_OC cycles and S_COMPUTE/S_WAIT_PPU both gate
  // on !shadow_copying, so an unbounded copy just becomes the new limiter
  // (measured: cyc/group stayed at 42.01 with the drain bounded but the copy not).
  logic [11:0] shadow_copy_len;
  // Copy now moves TWO OCs per cycle (even/odd shadow halves), so it runs for
  // ceil(shadow_copy_len / 2) cycles. This is the term that was 46% of
  // per-group time and fully exposed in S_WAIT_PPU.
  wire  [11:0] copy_pairs = ($unsigned(shadow_copy_len) + 12'd1) >> 1;
  logic [7:0]  zp_in_r;
  logic [7:0]  zp_out_r;
  logic        relu_en_r;
  // Per-lane replicated zp_in copies - each drives only N_OC DSP pre-adders
  // (not max_fanout attribute which Vivado may ignore on arrays; explicit replication is guaranteed)
  (* dont_touch = "true" *) logic [7:0] zp_in_lane [0:N_LANES-1];
  generate
    for (genvar g = 0; g < N_LANES; g++) begin : G_ZP_REPLICATE
      always_ff @(posedge clk or negedge rst_n)
        if (!rst_n) zp_in_lane[g] <= '0;
        else if (start_in) zp_in_lane[g] <= zp_in;
    end
  endgenerate

  // ------------------------------------------------------------
  // FSM types
  // ------------------------------------------------------------
  // Main FSM
  typedef enum logic [2:0] {
    S_IDLE       = 3'd0,
    S_LOAD_FIRST = 3'd1,   // load very first pixel group into buffer A
    S_COMPUTE    = 3'd2,   // MAC accumulation (Cin cycles per batch)
    S_BATCH_DONE = 3'd3,   // batch complete - trigger PPU, setup next
    S_WAIT_PPU   = 3'd4,   // wait for last PPU drain + group transition
    S_DONE       = 3'd5
  } state_t;
  state_t st;

  // PPU background sub-FSM
  typedef enum logic [1:0] {
    P_IDLE       = 2'd0,
    P_PARAM_WAIT = 2'd1,   // 1-cycle wait for param BRAM read latency
    P_DRAIN      = 2'd2    // feed shadow_acc to PPU, count outputs
  } ppu_st_t;
  ppu_st_t ppu_st;

  // Load background sub-FSM
  typedef enum logic [1:0] {
    L_IDLE    = 2'd0,
    L_LOADING = 2'd1,      // loading next group from input FIFO
    L_READY   = 2'd2       // next group fully loaded
  } load_st_t;
  load_st_t load_st;

  // ------------------------------------------------------------
  // Double Pixel Buffer BRAMs (A and B)
  //   Compute reads from active buffer (compute_buf)
  //   Load writes to inactive buffer (!compute_buf)
  // ------------------------------------------------------------
  logic compute_buf;   // 0 = buffer A active, 1 = buffer B active

  // Buffer A ports
  logic [PB_AW-1:0]   pb_a_wr_addr, pb_a_rd_addr;
  logic                pb_a_wr_en,   pb_a_rd_en;
  logic [PB_WIDTH-1:0] pb_a_wr_data, pb_a_rd_data;

  // Buffer B ports
  logic [PB_AW-1:0]   pb_b_wr_addr, pb_b_rd_addr;
  logic                pb_b_wr_en,   pb_b_rd_en;
  logic [PB_WIDTH-1:0] pb_b_wr_data, pb_b_rd_data;

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
  ) u_pixel_buf_a (
    .clka  (clk), .ena (1'b1), .wea (pb_a_wr_en),
    .addra (pb_a_wr_addr), .dina (pb_a_wr_data),
    .injectsbiterra(1'b0), .injectdbiterra(1'b0),
    .clkb  (clk), .enb (pb_a_rd_en), .rstb (1'b0), .regceb (1'b1),
    .addrb (pb_a_rd_addr), .doutb (pb_a_rd_data),
    .sbiterrb(), .dbiterrb(), .sleep(1'b0)
  );

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
  ) u_pixel_buf_b (
    .clka  (clk), .ena (1'b1), .wea (pb_b_wr_en),
    .addra (pb_b_wr_addr), .dina (pb_b_wr_data),
    .injectsbiterra(1'b0), .injectdbiterra(1'b0),
    .clkb  (clk), .enb (pb_b_rd_en), .rstb (1'b0), .regceb (1'b1),
    .addrb (pb_b_rd_addr), .doutb (pb_b_rd_data),
    .sbiterrb(), .dbiterrb(), .sleep(1'b0)
  );

  // Compute-side read data mux: select active buffer
  wire [PB_WIDTH-1:0] pb_rd_data = compute_buf ? pb_b_rd_data : pb_a_rd_data;

  // ------------------------------------------------------------
  // Accumulators: N_OC � N_LANES
  // shadow_acc replaced by BRAM to eliminate 10,240 FFs
  // ------------------------------------------------------------
  // DECLARATION INITIALISER (2026-09-03) -- same rationale as ppu.sv's, see the
  // long note there. acc has no reset (deliberately, to stay out of the async
  // reset fanout cone), so SIMULATION started it X and that X reached every
  // output, which is why this core has never had a data check. Xilinx honours
  // SV declaration initialisers as the flop INIT attribute, i.e. the power-up
  // state the silicon already had. Synthesisable, changes NOTHING on hardware,
  // and lets tb_pw_vq.sv actually check data.
  logic signed [ACC_WIDTH-1:0] acc [0:N_OC-1][0:N_LANES-1] = '{default:'0};

  // Shadow BRAM: depth=N_OC, width=N_LANES*ACC_WIDTH
  //
  // DOUBLE BUFFERING WAS TRIED AND REVERTED (2026-07-30). It fits for free --
  // width sets the BRAM primitive count, and depth 16->32 of 512 costs nothing
  // -- but it does not help, because the copy and the drain ALREADY overlap by
  // chasing: P_PARAM_WAIT costs 1 cycle, then P_DRAIN reads index 0 (written
  // first) advancing 1/cycle behind a copy also advancing 1/cycle, so the drain
  // never catches up and there was no stall to remove. Measured in sim
  // (2880 groups): baseline 77.0 cyc/grp at cin=16/2 batches, double-buffered
  // 76.0 -- and 88.0 if S_BATCH_DONE additionally waits for the copy, which
  // BREAKS the chase. The real overhead is elsewhere; see S_WAIT_PPU.
  // ------------------------------------------------------------------
  // SPLIT SHADOW: EVEN / ODD OUTPUT-CHANNEL BANKS (2026-08-09)
  //
  // WHY. Measured phase residency showed S_WAIT_PPU is 46% of per-group time on
  // blocks 2/3 and is PURE waiting on this copy, which overlaps compute 0.00
  // cycles for single-batch groups. The copy ran one OC per cycle, so it cost
  // Q_last cycles per group and every one of them was exposed.
  //
  // Splitting the store into even-OC and odd-OC halves lets the copy write TWO
  // OCs per cycle, halving it to ceil(Q_last/2). The DRAIN still consumes one OC
  // per cycle -- it alternates halves -- so the output rate is unchanged.
  //
  // Deliberately NOT the alternative (double-buffering acc): that would put a
  // mux on the accumulator feedback, which is the first_ic -> DSP OPMODE
  // critical path, on a design closing at WNS +0.119 ns. This touches only the
  // copy-write and drain-read paths and cannot affect that path.
  //
  // Both halves share ONE address bus: for issue j the drain wants OC j, which
  // lives at index j>>1 in half (j&1). Since j and j+1 share the same j>>1 when
  // j is even, addressing both halves identically and selecting the output by
  // parity costs nothing and keeps the existing pre-read structure intact.
  // ------------------------------------------------------------------
  localparam int SA_IDX_W  = (N_OC > 1) ? $clog2(N_OC) : 1;
  // per-half index: N_OC/2 entries per bank
  localparam int SA_HIDX_W = (N_OC > 2) ? $clog2(N_OC/2) : 1;
  localparam int SA_DEPTH  = 2 * ((N_OC > 2) ? (N_OC/2) : 1);   // per half
  localparam int SA_AW     = SA_HIDX_W + 1;                     // {bank, halfidx}
  localparam int SA_DW     = N_LANES * ACC_WIDTH;

  logic [SA_AW-1:0]  sha_wr_addr;
  logic [SA_DW-1:0]  sha_wr_data_e, sha_wr_data_o;
  logic              sha_wr_en_e,   sha_wr_en_o;
  logic [SA_AW-1:0]  sha_rd_addr;
  logic              sha_rd_en;
  logic [SA_DW-1:0]  sha_rd_data_e, sha_rd_data_o;
  logic              sha_rd_parity;      // registered: which half issue j wants
  logic [SA_DW-1:0]  sha_rd_data;        // muxed, drives ppu_acc_in as before

  assign sha_rd_data = sha_rd_parity ? sha_rd_data_o : sha_rd_data_e;

  // shadow_copy_idx now counts HALF-PAIRS: cycle i copies OC 2i and OC 2i+1
  logic [SA_HIDX_W-1:0] shadow_copy_idx;
  logic              sha_wr_bank, sha_rd_bank;
  logic              shadow_copying;
  logic              shadow_copy_trig;  // 1-cycle pulse from main FSM -> shadow block

  genvar _g;
  generate
    for (_g = 0; _g < N_LANES; _g++) begin : G_SHA_PACK
      assign sha_wr_data_e[_g*ACC_WIDTH +: ACC_WIDTH] =
               acc[{shadow_copy_idx, 1'b0}][_g];   // OC 2i
      assign sha_wr_data_o[_g*ACC_WIDTH +: ACC_WIDTH] =
               acc[{shadow_copy_idx, 1'b1}][_g];   // OC 2i+1
    end
  endgenerate

  xpm_memory_sdpram #(
    .ADDR_WIDTH_A        (SA_AW),
    .ADDR_WIDTH_B        (SA_AW),
    .AUTO_SLEEP_TIME     (0),
    .BYTE_WRITE_WIDTH_A  (SA_DW),
    .CLOCKING_MODE       ("common_clock"),
    .ECC_MODE            ("no_ecc"),
    .MEMORY_INIT_FILE    ("none"),
    .MEMORY_INIT_PARAM   ("0"),
    .MEMORY_OPTIMIZATION ("true"),
    .MEMORY_PRIMITIVE    ("block"),
    .MEMORY_SIZE         (SA_DEPTH * SA_DW),
    .MESSAGE_CONTROL     (0),
    .READ_DATA_WIDTH_B   (SA_DW),
    .READ_LATENCY_B      (1),
    .READ_RESET_VALUE_B  ("0"),
    .RST_MODE_A          ("SYNC"),
    .RST_MODE_B          ("SYNC"),
    .SIM_ASSERT_CHK      (0),
    .USE_MEM_INIT        (0),
    .WAKEUP_TIME         ("disable_sleep"),
    .WRITE_DATA_WIDTH_A  (SA_DW),
    .WRITE_MODE_B        ("no_change")
  ) u_shadow_bram_e (
    .clka  (clk), .ena (1'b1), .wea (sha_wr_en_e),
    .addra (sha_wr_addr), .dina (sha_wr_data_e),
    .injectsbiterra(1'b0), .injectdbiterra(1'b0),
    .clkb  (clk), .enb (sha_rd_en), .rstb (1'b0), .regceb (1'b1),
    .addrb (sha_rd_addr), .doutb (sha_rd_data_e),
    .sbiterrb(), .dbiterrb(), .sleep(1'b0)
  );

  // ODD half — identical geometry, same address bus, own write enable so a
  // ragged final pair (odd Q_last) can write the even OC without the odd one.
  xpm_memory_sdpram #(
    .ADDR_WIDTH_A        (SA_AW),
    .ADDR_WIDTH_B        (SA_AW),
    .AUTO_SLEEP_TIME     (0),
    .BYTE_WRITE_WIDTH_A  (SA_DW),
    .CLOCKING_MODE       ("common_clock"),
    .ECC_MODE            ("no_ecc"),
    .MEMORY_INIT_FILE    ("none"),
    .MEMORY_INIT_PARAM   ("0"),
    .MEMORY_OPTIMIZATION ("true"),
    .MEMORY_PRIMITIVE    ("block"),
    .MEMORY_SIZE         (SA_DEPTH * SA_DW),
    .MESSAGE_CONTROL     (0),
    .READ_DATA_WIDTH_B   (SA_DW),
    .READ_LATENCY_B      (1),
    .READ_RESET_VALUE_B  ("0"),
    .RST_MODE_A          ("SYNC"),
    .RST_MODE_B          ("SYNC"),
    .SIM_ASSERT_CHK      (0),
    .USE_MEM_INIT        (0),
    .WAKEUP_TIME         ("disable_sleep"),
    .WRITE_DATA_WIDTH_A  (SA_DW),
    .WRITE_MODE_B        ("no_change")
  ) u_shadow_bram_o (
    .clka  (clk), .ena (1'b1), .wea (sha_wr_en_o),
    .addra (sha_wr_addr), .dina (sha_wr_data_o),
    .injectsbiterra(1'b0), .injectdbiterra(1'b0),
    .clkb  (clk), .enb (sha_rd_en), .rstb (1'b0), .regceb (1'b1),
    .addrb (sha_rd_addr), .doutb (sha_rd_data_o),
    .sbiterrb(), .dbiterrb(), .sleep(1'b0)
  );

  // ------------------------------------------------------------
  // ------------------------------------------------------------
  // MAC pipeline (4-stage: BRAM read ? pixel reg ? multiply reg ? accumulate)
  //   Stage 1: BRAM read issue
  //   Stage 2: Register BRAM output (pb_pixel_r and w_rd_data_r)
  //   Stage 3: subtract + multiply ? prod_reg
  //   Stage 4: accumulate from prod_reg
  // ------------------------------------------------------------
  // Channels STREAMED per group. Ordinary convolution loads exactly the
  // channels it MACs; VQ mode loads all 64 and MACs a 16-wide window of them.
  wire [11:0] cin_load = ((USE_PW_VQ != 0) && vq_mode_r) ? vq_cin_load_r : cin_run;

  logic signed [DATA_WIDTH-1:0] w_rd_data_r   [0:N_OC-1]; // Fixes 1-cycle weight alignment bug
  logic signed [DATA_WIDTH-1:0] w_rd_data_rr  [0:N_OC-1]; // stage-2.5 registered weights for DSP path
  logic signed [24:0]           a_packed_r    [0:(N_LANES/2)-1]; // stage-2.5 registered packed activations
  logic signed [32:0] p_packed_reg [0:N_OC-1][0:(N_LANES/2)-1]; // Dual-MAC packed product register
  logic                         mul_valid;  // p_packed_reg contains valid data
  logic [DATA_WIDTH-1:0]        pb_pixel_r [0:N_LANES-1];  // registered BRAM output (raw)
  logic                         rd_issued_d1;  // delayed rd_issued (BRAM data registered)
  logic                         rd_issued_d2;  // delayed rd_issued_d1 (a_packed_r valid)

  // Unpack pixel buffer read data into per-lane values (combinational, used for registration)
  logic [DATA_WIDTH-1:0] pb_pixel [0:N_LANES-1];
  generate
    for (genvar g = 0; g < N_LANES; g++) begin : G_PB_UNPACK
      assign pb_pixel[g] = pb_rd_data[g*DATA_WIDTH +: DATA_WIDTH];
    end
  endgenerate

  // Dual-MAC Combinational Packing
  logic signed [9:0]  act1_adj [0:(N_LANES/2)-1];
  logic signed [24:0] a_packed [0:(N_LANES/2)-1];
  logic signed [32:0] p_packed_comb [0:N_OC-1][0:(N_LANES/2)-1];
  logic signed [8:0]  act_diff [0:N_LANES-1];

  always_comb begin
    for (int i = 0; i < N_LANES; i++) begin
      act_diff[i] = $signed({1'b0, pb_pixel_r[i]}) - $signed({1'b0, zp_in_lane[i]});
    end
    for (int p = 0; p < N_LANES/2; p++) begin
      act1_adj[p] = $signed(act_diff[p*2 + 1]) - $signed({1'b0, act_diff[p*2][8]});
      a_packed[p] = $signed({act1_adj[p][8:0], {7{act_diff[p*2][8]}}, act_diff[p*2]});
    end
    for (int oc = 0; oc < N_OC; oc++) begin
      for (int p = 0; p < N_LANES/2; p++) begin
        // Use stage-2.5 registered operands so the multiply sees clean flip-flop inputs
        // and Vivado maps it to a DSP48 P-register rather than fabric CARRY4 chains.
        p_packed_comb[oc][p] = a_packed_r[p] * w_rd_data_rr[oc];
      end
    end
  end

  // Generate block to map each element to a dedicated DSP multiplier/register
  generate
    for (genvar oc = 0; oc < N_OC; oc++) begin : G_DSP_OC
      for (genvar p = 0; p < N_LANES/2; p++) begin : G_DSP_P
        (* use_dsp = "yes" *) logic signed [32:0] p_reg;
        always_ff @(posedge clk) begin
          if (rd_issued_d2) begin
            p_reg <= p_packed_comb[oc][p];
          end
        end
        assign p_packed_reg[oc][p] = p_reg;
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

  // PPU params (latched from param BRAM)
  logic signed [31:0] ppu_bias_q;
  logic [23:0]        ppu_mult_q;  // 24-bit: INT8 mult always fits, saves DSPs
  logic [7:0]         ppu_shift_q;

  generate
    for (genvar g = 0; g < N_LANES; g++) begin : G_PPU
      ppu #(.DATA_WIDTH(DATA_WIDTH), .ACC_WIDTH(ACC_WIDTH)) u_ppu (
        .clk(clk), .rst_n(rst_n),
        .relu_en(relu_en_r),
        .mult_conv(ppu_mult_q), .shift_conv(ppu_shift_q), .bias_in(ppu_bias_q),
        .zp_out(zp_out_r),
        .valid_in(ppu_valid_in), .conv_acc_in(ppu_acc_in[g]),
        .pixel_out(ppu_pixel_out[g]), .valid_out(ppu_valid_out_vec[g])
      );
    end
  endgenerate

  wire ppu_valid_out = ppu_valid_out_vec[0];


  // ============================================================
  // VQ BRANCH  (USE_PW_VQ only)
  //
  // Snoops the PPU issue bus READ-ONLY. Nothing here can back-pressure or
  // otherwise perturb the MAC, the accumulators or the drain FSM. In VQ mode
  // the PPU still runs and its results are simply discarded by the output mux;
  // in conv mode this whole block loses its readers and is stripped.
  //
  //   score_k = ||v_k||^2 - 2*acc_k        20 bits signed (derived in vq_pw.c)
  //
  // WHY A DEDICATED NORM ROM AND NOT THE PARAM-BRAM BIAS PATH
  //   The obvious reuse is to carry ||v_k||^2 on ppu_bias_q, which is already
  //   read per OC at exactly the right cycle. tb_pw_bias_align.sv shows that
  //   is NOT safe: the drain's bias latch is gated by
  //   (ppu_issue_idx + 1) < ppu_drain_len, so the LAST OC of every batch is
  //   presented with the SECOND-TO-LAST OC's bias. Convolution has never
  //   noticed because this project programs bias == 0 for every channel
  //   (surr_fill_dummy_params), but VQ would silently mis-score k = 15 of
  //   every sub-codebook. Rather than modify the validated drain FSM, the VQ
  //   branch carries its own 128-entry norm ROM. 128 x 20 bits is distributed
  //   RAM; the drain FSM is untouched.
  //
  // OC/BATCH SHADOWS
  //   ppu_valid_in and ppu_acc_in are REGISTERED at issue, and ppu_issue_idx
  //   increments on the same edge, so at the valid cycle the accumulator
  //   belongs to OC (ppu_issue_idx - 1). A free-running one-cycle shadow gives
  //   that index with no subtractor and no FSM change. ppu_oc_batch is
  //   shadowed for the same reason: the last issue releases the drain to
  //   P_IDLE, so S_BATCH_DONE can overwrite ppu_oc_batch on the very cycle the
  //   last OC is presented.
  //
  // TIES: strict less-than keeps the LOWEST codeword index, matching
  // vqpw_search_sub() and the retired vq_engine.sv comparator.
  // ============================================================
  localparam int VQ_K       = 16;
  localparam int VQ_KW      = 4;
  localparam int VQ_SCORE_W = 20;
  localparam int VQ_NORM_D  = 128;
  localparam int VQ_BEATS   = N_LANES / 2;   // two 32-bit positions per beat

  logic [N_LANES*DATA_WIDTH-1:0] vq_pixel_out;
  logic                          vq_valid_out;
  wire  [N_LANES*DATA_WIDTH-1:0] ppu_pixel_bus;

  generate
    for (genvar g = 0; g < N_LANES; g++) begin : G_PX_PACK
      assign ppu_pixel_bus[g*DATA_WIDTH +: DATA_WIDTH] = ppu_pixel_out[g];
    end
  endgenerate

  generate
  if (USE_PW_VQ != 0) begin : G_VQ
    logic [$clog2(N_OC>1?N_OC+1:2)-1:0] vq_oc_r;
    logic [11:0]                        vq_batch_r;
    always_ff @(posedge clk) begin
      vq_oc_r    <= ppu_issue_idx;
      vq_batch_r <= ppu_oc_batch;
    end

    (* ram_style = "distributed" *) logic signed [VQ_SCORE_W-1:0] vq_norm [0:VQ_NORM_D-1];
    always_ff @(posedge clk) if (vq_norm_we) vq_norm[vq_norm_addr] <= vq_norm_data;
    wire [6:0] vq_norm_ra = {vq_batch_r[1:0], vq_oc_r[4:0]};
    wire signed [VQ_SCORE_W-1:0] vq_norm_q = vq_norm[vq_norm_ra];

    wire [VQ_KW-1:0] vq_k     = vq_oc_r[VQ_KW-1:0];
    wire             vq_half  = vq_oc_r[VQ_KW];
    wire             vq_first = (vq_k == {VQ_KW{1'b0}});
    wire             vq_lastk = (vq_k == {VQ_KW{1'b1}});
    wire [2:0]       vq_m     = {vq_batch_r[1:0], vq_half};
    wire             vq_go    = vq_mode_r && ppu_valid_in;

    logic signed [VQ_SCORE_W-1:0] vq_best  [0:N_LANES-1];
    logic [VQ_KW-1:0]             vq_bestk [0:N_LANES-1];
    logic [31:0]                  vq_word  [0:N_LANES-1];
    wire signed [VQ_SCORE_W-1:0]  vq_score [0:N_LANES-1];
    wire [N_LANES-1:0]            vq_take;
    wire [VQ_KW-1:0]              vq_curk  [0:N_LANES-1];

    for (genvar g = 0; g < N_LANES; g++) begin : G_VQ_LANE
      wire signed [31:0] acc32 = 32'(ppu_acc_in[g]);
      wire signed [31:0] sc32  = 32'(vq_norm_q) - (acc32 <<< 1);
      assign vq_score[g] = sc32[VQ_SCORE_W-1:0];
      assign vq_take[g]  = vq_first || (vq_score[g] < vq_best[g]);
      assign vq_curk[g]  = vq_take[g] ? vq_k : vq_bestk[g];
`ifndef SYNTHESIS
      // The 20-bit truncation must be exact -- that is the derivation in
      // vq_pw.c holding on real data, checked every single compare.
      always_ff @(posedge clk)
        if (rst_n && vq_go)
          a_vq_score_fits: assert (sc32 === 32'(vq_score[g]))
            else $error("VQ score %0d does not fit %0d bits", sc32, VQ_SCORE_W);
`endif
    end

    logic       vq_pending, vq_emit_busy;
    logic [$clog2(VQ_BEATS)-1:0] vq_beat;

    always_ff @(posedge clk) begin
      if (!rst_n) begin
        vq_pending <= 1'b0; vq_emit_busy <= 1'b0; vq_beat <= '0;
        for (int i = 0; i < N_LANES; i++) begin
          vq_best[i] <= '0; vq_bestk[i] <= '0; vq_word[i] <= 32'd0;
        end
      end else begin
        if (vq_go) begin
          for (int i = 0; i < N_LANES; i++) begin
            if (vq_take[i]) vq_best[i] <= vq_score[i];
            vq_bestk[i] <= vq_curk[i];
            if (vq_lastk) vq_word[i][{vq_m, 2'b00} +: VQ_KW] <= vq_curk[i];
          end
          // last codeword of the last sub-codebook of the last batch: the
          // whole 32-bit word for every lane is complete on this edge.
          if (vq_lastk && vq_half &&
              ($unsigned(vq_batch_r) + 1 == $unsigned(cout_batches_r)))
            vq_pending <= 1'b1;
        end

        if (vq_pending && !vq_emit_busy) begin
          vq_emit_busy <= 1'b1;
          vq_beat      <= '0;
          vq_pending   <= 1'b0;
        end else if (vq_emit_busy && !out_stall) begin
          if ($unsigned(vq_beat) + 1 == VQ_BEATS) vq_emit_busy <= 1'b0;
          vq_beat <= vq_beat + 1'b1;
        end
      end
    end

    // Position 2i occupies the LOW 32 bits so the S2MM writes it to the lower
    // address: little-endian, matching vqpw_encode_frame()'s idx_out[pos*4].
    assign vq_pixel_out = {vq_word[{vq_beat, 1'b1}], vq_word[{vq_beat, 1'b0}]};
    assign vq_valid_out = vq_emit_busy && !out_stall;
  end else begin : G_NO_VQ
    assign vq_pixel_out = '0;
    assign vq_valid_out = 1'b0;
  end
  endgenerate

  // ---- output ownership mux ----
  // Exactly one of the two sources drives the output FIFO, selected by the
  // per-run latched mode. In conv builds the ternaries fold to ppu_*.
  assign pixel_out = ((USE_PW_VQ != 0) && vq_mode_r) ? vq_pixel_out : ppu_pixel_bus;
  assign valid_out = ((USE_PW_VQ != 0) && vq_mode_r) ? vq_valid_out : ppu_valid_out;

  // ------------------------------------------------------------
  // Counters
  // ------------------------------------------------------------
  // Main FSM
  logic [31:0]                        grp_idx;
  logic [$clog2(CIN_MAX)-1:0]        ic_idx;
  logic [11:0]                        oc_batch_idx;
  logic [W_AW-1:0]                    w_addr_base;

  // pb_ram READ index. In VQ mode the batch's window base is w_addr_base.
  wire [PB_AW-1:0] pb_rd_index = ((USE_PW_VQ != 0) && vq_mode_r)
                               ? (w_addr_base[PB_AW-1:0] + ic_idx[PB_AW-1:0])
                               : ic_idx[PB_AW-1:0];

  logic                               rd_issued;
  logic                               first_ic;
  // mul_valid is declared above with prod_reg

  // Load sub-FSM
  logic [$clog2(CIN_MAX)-1:0]        load_ic_idx;
  logic                               next_grp_ready;

  // PPU sub-FSM
  // BUG FIX 2026-07-30 -- was $clog2(N_OC), which cannot REPRESENT N_OC when
  // N_OC is a power of two: at N_OC=8 this was [2:0], max value 7, so the
  // P_DRAIN guard `$unsigned(ppu_issue_idx) < N_OC` could never go false and
  // the drain issued PPU beats forever. It stopped only when ppu_out_cnt
  // caught up, i.e. after N_OC + PPU_LATENCY(9) issues: 17 beats/batch at
  // N_OC=8, 25 at N_OC=16, both measured in sim. Every group therefore emitted
  // ~2x its beats, produced_cnt hit the predicted total_groups_r at roughly
  // half the input, TLAST fired early, and S2MM completed a garbage-tailed
  // transfer looking perfectly healthy (Idle=1, IOC=1, no errors) while DW was
  // left holding undelivered input -> "MM2S timeout".
  // N_OC=30 was immune purely because 30 is NOT a power of two: $clog2(30)=5
  // bits holds 30 and 31, so the guard worked. That is the only reason every
  // build before N_OC=8 was correct, and why N_OC=16 is NOT a viable hedge.
  // $clog2(N_OC+1) guarantees the terminal value is representable.
  logic [$clog2(N_OC>1?N_OC+1:2)-1:0]  ppu_issue_idx;
  logic [31:0]                        ppu_out_cnt;

  // OPTION C (2026-08-07): the drain context is released when the last beat has
  // been ISSUED, not when it has RETIRED, so a batch's outputs can still be
  // inside the PPU pipeline after its successor has begun issuing. ppu_out_cnt
  // is a per-batch counter and can no longer answer "is the run finished"; this
  // occupancy counter can. +1 per ppu_valid_in, -1 per ppu_valid_out, maintained
  // unconditionally outside the FSM so it stays correct across batch boundaries
  // and while out_stall is held. Max occupancy is N_OC + PPU depth (~9).
  logic [7:0]                         ppu_in_flight;
  logic [11:0]                        ppu_oc_batch;  // which OC batch PPU is draining

  // ------------------------------------------------------------
  // Main Controller
  //   Three concurrent processes in one always_ff:
  //   1. Main FSM (compute scheduling)
  //   2. Load sub-FSM (background pixel preload)
  //   3. PPU sub-FSM (background accumulator drain)
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
      rd_issued     <= 1'b0;
      rd_issued_d1  <= 1'b0;
      rd_issued_d2  <= 1'b0;
      mul_valid     <= 1'b0;
      first_ic      <= 1'b1;
      // Removed datapath array resets to fix high-fanout recovery violations
      compute_buf     <= 1'b0;
      load_st         <= L_IDLE;
      load_ic_idx     <= '0;
      next_grp_ready  <= 1'b0;
      ppu_st          <= P_IDLE;
      ppu_valid_in    <= 1'b0;
      ppu_issue_idx   <= '0;
      ppu_out_cnt     <= 32'd0;
      ppu_in_flight   <= 8'd0;
      ppu_drain_len   <= 12'd0;
      cout_run_r      <= 12'd0;
      ppu_oc_batch    <= 12'd0;
      param_rd_addr   <= '0;
      param_rd_en     <= 1'b0;
      // ppu_bias_q / ppu_mult_q / ppu_shift_q intentionally omitted from async reset.
      // They are always loaded from BRAM in P_PARAM_WAIT before ppu_valid_in fires,
      // so their value at reset time is irrelevant. Including them drove the CLR pin
      // of e.g. ppu_mult_q_reg[11] via a 9.2 ns routed path (fo=35270 reset net),
      // causing a Recovery violation identical to the one already fixed above.
      // shadow_copying/shadow_copy_idx/sha_wr_en: synchronous reset (separate block)
      shadow_copy_trig <= 1'b0;
      sha_rd_en       <= 1'b0;
      sha_wr_bank <= 1'b0; sha_rd_bank <= 1'b0;


      pb_a_wr_en <= 1'b0; pb_a_wr_addr <= '0; pb_a_wr_data <= '0;
      pb_a_rd_en <= 1'b0; pb_a_rd_addr <= '0;
      pb_b_wr_en <= 1'b0; pb_b_wr_addr <= '0; pb_b_wr_data <= '0;
      pb_b_rd_en <= 1'b0; pb_b_rd_addr <= '0;
    end else begin
      // Default deasserts (active-high pulses cleared every cycle)
      done_out     <= 1'b0;
      consume_in   <= 1'b0;
      w_rd_en      <= 1'b0;
      ppu_valid_in <= 1'b0;
      param_rd_en  <= 1'b0;
      pb_a_wr_en   <= 1'b0;
      pb_b_wr_en   <= 1'b0;
      pb_a_rd_en   <= 1'b0;
      pb_b_rd_en   <= 1'b0;
      sha_rd_en        <= 1'b0;
      shadow_copy_trig <= 1'b0;  // default deassert; set in S_COMPUTE when batch done

      // OPTION C: PPU pipeline occupancy. Maintained here, outside the FSM,
      // because a batch's outputs retire after its drain context has already
      // been handed to the next batch. ppu_valid_in is the registered issue
      // strobe the PPU samples this edge, so incrementing on it counts exactly
      // the beats the PPU accepted.
      if (ppu_valid_in && !ppu_valid_out)
        ppu_in_flight <= ppu_in_flight + 8'd1;
      else if (!ppu_valid_in && ppu_valid_out)
        ppu_in_flight <= ppu_in_flight - 8'd1;

      // ========================================================
      // [1] MAIN FSM
      // ========================================================
      case (st)

        // --------------------------------------------------------
        // S_IDLE: wait for start
        // --------------------------------------------------------
        S_IDLE: begin
          if (start_in) begin
            // Latch derived constants (removes division from critical path)
            tile_groups_r  <= tile_pixels >> LANE_SHIFT;
            // ceil, not floor: with partial-batch drain N_OC may exceed cout,
            // and floor would give zero batches and emit nothing.
            cout_batches_r <= (cout_run + N_OC - 1) / N_OC;
            cout_run_r     <= cout_run;
            zp_in_r        <= zp_in;
            zp_out_r       <= zp_out;
            vq_mode_r      <= (USE_PW_VQ != 0) ? vq_mode : 1'b0;
            vq_cin_load_r  <= vq_cin_load;
            relu_en_r      <= relu_en;
            /* start each run on a known bank so a previous run's parity
             * cannot carry over (2026-07-30 double buffer) */


            grp_idx        <= 32'd0;
            ic_idx         <= '0;
            oc_batch_idx   <= 12'd0;
            w_addr_base    <= '0;
            rd_issued      <= 1'b0;
            first_ic       <= 1'b1;
            compute_buf    <= 1'b0;
            load_st        <= L_IDLE;
            ppu_st         <= P_IDLE;
            next_grp_ready <= 1'b0;
            ppu_out_cnt    <= 32'd0;
            // Defensive: S_DONE already guarantees the pipeline drained, but a
            // run must never inherit occupancy from an aborted predecessor.
            // Placed after the unconditional maintenance above so it wins.
            ppu_in_flight  <= 8'd0;
            if ((tile_pixels == 0) || (cin_run == 0) || (cout_run == 0))
              st <= S_DONE;
            else
              st <= S_LOAD_FIRST;
          end
        end

        // --------------------------------------------------------
        // S_LOAD_FIRST: load very first pixel group into buffer A
        //   (no compute to overlap with yet)
        // --------------------------------------------------------
        S_LOAD_FIRST: begin
          if (valid_in) begin
            consume_in   <= 1'b1;
            pb_a_wr_en   <= 1'b1;
            pb_a_wr_addr <= ic_idx[PB_AW-1:0];
            pb_a_wr_data <= pixel_in;

            if (($unsigned(ic_idx) + 1) == cin_load) begin
              // First group loaded - setup compute
              ic_idx       <= '0;
              oc_batch_idx <= 12'd0;
              w_addr_base  <= '0;
              rd_issued    <= 1'b0;
              first_ic     <= 1'b1;
              compute_buf  <= 1'b0;  // compute reads buffer A
              // acc clear NOT needed: first_ic=1 makes S_COMPUTE
              // use '0 on the first valid MAC (line 457)
              // Start background preload of group 1 into buffer B
              if ((grp_idx + 1) < tile_groups_r) begin
                load_st        <= L_LOADING;
                load_ic_idx    <= '0;
                next_grp_ready <= 1'b0;
              end
              st <= S_COMPUTE;
            end else begin
              ic_idx <= ic_idx + 1'b1;
            end
          end
        end

        // --------------------------------------------------------
        // S_COMPUTE: MAC accumulation for current OC batch
        //   Cin cycles reading pixel buffer + weight BRAM
        //   Load sub-FSM and PPU sub-FSM run concurrently
        // --------------------------------------------------------
        S_COMPUTE: begin
          // Pipeline stage 4: accumulate from registered multiply output
          if (mul_valid) begin
            for (oci = 0; oci < N_OC; oci++) begin
              for (int p = 0; p < N_LANES/2; p++) begin
                // SIGNEDNESS FIX (2026-08-25) -- PACK_SIGNED_FIX
                // The unsized literal '0 is UNSIGNED, so `first_ic ? '0 : acc`
                // has unsigned type and forces the whole add into unsigned
                // context, ZERO-extending a negative product instead of
                // sign-extending it. xsim: -84 was accumulated as +65452.
                // $signed() on the ternary restores signed context.
                acc[oci][p*2]     <= $signed(first_ic ? '0 : acc[oci][p*2])
                                     + $signed(p_packed_reg[oci][p][15:0]);
                // DUAL-MAC BORROW FIX (2026-08-25) -- PACK_BORROW_FIX
                // p[15:0] is a SIGNED 16-bit field; when negative it borrows 1
                // from the high field, so p[32:16] reads a1*w - 1. Adding the
                // low field's sign bit back cancels the borrow exactly.
                acc[oci][p*2 + 1] <= $signed(first_ic ? '0 : acc[oci][p*2 + 1])
                                     + $signed(p_packed_reg[oci][p][32:16])
                                     + $signed({1'b0, p_packed_reg[oci][p][15]});
              end
            end
            first_ic <= 1'b0;
          end

          // Pipeline stage 3: multiply (now in DSP48) and register result.
          // Triggered one cycle later than before (rd_issued_d2) because the new
          // stage 2.5 below captures a_packed_r/w_rd_data_rr first.
          if (rd_issued_d2) begin
            mul_valid <= 1'b1;
          end else begin
            mul_valid <= 1'b0;
          end

          // Pipeline stage 2.5: register packed activations and weights.
          // Breaks the timing-critical path: previously a_packed (from
          // pb_pixel_r and zp_in_lane via two 9-bit subtracts + packing)
          // fed the multiply in the same cycle, causing 17 logic levels
          // and -2.7 ns slack. Now only registered signals enter the DSP48.
          rd_issued_d2 <= rd_issued_d1;

          // Pipeline stage 2: register BRAM output (data arrived from read)
          rd_issued_d1 <= rd_issued;

          // Pipeline stage 1: issue BRAM reads
          if ($unsigned(ic_idx) < cin_run) begin
            // Read from active compute buffer
            if (!compute_buf) begin
              pb_a_rd_en   <= 1'b1;
              pb_a_rd_addr <= pb_rd_index;
            end else begin
              pb_b_rd_en   <= 1'b1;
              pb_b_rd_addr <= pb_rd_index;
            end
            w_rd_en    <= 1'b1;
            w_rd_addr  <= w_addr_base + ic_idx;
            rd_issued  <= 1'b1;
            ic_idx     <= ic_idx + 1'b1;
          end else begin
            rd_issued <= 1'b0;
            if (!rd_issued && !rd_issued_d1 && !rd_issued_d2 && !mul_valid) begin
              // DOUBLE-BUFFER CHANGE 2026-07-30. This used to require
              // `ppu_st == P_IDLE`, i.e. batch N+1 could not finish its compute
              // until batch N's PPU drain had fully RETIRED (N_OC issues plus
              // ~9 cycles of PPU pipeline tail). That was the measured
              // ~12.3 cycles/batch serialisation.
              //
              // With two banks the copy targets the bank the PPU is NOT
              // draining, so it can proceed immediately. The only remaining
              // requirement is that the copy engine itself is free.
              // S_BATCH_DONE still gates the DRAIN on P_IDLE, which is what
              // keeps at most one drain in flight and makes 2 banks sufficient.
              if (!shadow_copying) begin
                // Trigger shadow block to copy acc -> shadow BRAM
                shadow_copy_trig <= 1'b1;
                shadow_copy_len  <= (($unsigned(cout_run_r) - ($unsigned(oc_batch_idx) * N_OC)) < N_OC)
                                    ? ($unsigned(cout_run_r) - ($unsigned(oc_batch_idx) * N_OC))
                                    : N_OC;
                st <= S_BATCH_DONE;
              end
            end
          end
        end

        // --------------------------------------------------------
        // S_BATCH_DONE: trigger PPU drain + decide next step
        //   Waits for previous PPU drain to complete (if still busy)
        // --------------------------------------------------------
        S_BATCH_DONE: begin
          if (ppu_st == P_IDLE) begin
            sha_rd_bank <= sha_wr_bank;
            sha_wr_bank <= ~sha_wr_bank;
            // Hand the just-filled bank to the PPU and flip the write bank so
            // the NEXT batch's copy lands in the one that is now free.


            // Previous PPU drain (if any) is complete - safe to trigger new one
            // Issue first param BRAM read for this batch
            ppu_st        <= P_PARAM_WAIT;
            ppu_oc_batch  <= oc_batch_idx;
            // Valid channels remaining in this batch: full N_OC except possibly
            // the last, which carries cout_run - oc_batch_idx*N_OC.
            ppu_drain_len <= (($unsigned(cout_run_r) - ($unsigned(oc_batch_idx) * N_OC)) < N_OC)
                             ? ($unsigned(cout_run_r) - ($unsigned(oc_batch_idx) * N_OC))
                             : N_OC;
            ppu_issue_idx <= '0;
            ppu_out_cnt   <= 32'd0;
            param_rd_en   <= 1'b1;
            param_rd_addr <= oc_batch_idx * N_OC;

            if (($unsigned(oc_batch_idx) + 1) < cout_batches_r) begin
              // Not the last batch - start next batch immediately
              oc_batch_idx <= oc_batch_idx + 12'd1;
              w_addr_base  <= w_addr_base + cin_run;
              ic_idx       <= '0;
              rd_issued    <= 1'b0;
              first_ic     <= 1'b1;
              // acc clear NOT needed: first_ic handles it
              st <= S_COMPUTE;
            end else begin
              // Last batch of this group - wait for PPU to finish
              st <= S_WAIT_PPU;
            end
          end
          // else: PPU still busy from previous batch - wait here
        end

        // --------------------------------------------------------
        // S_WAIT_PPU: wait for last batch's PPU drain, then
        //   either swap buffers for next group or finish
        // --------------------------------------------------------
        S_WAIT_PPU: begin
          if (!shadow_copying) begin
            // Last batch's PPU drain is complete.
            //
            // BUG FIX 2026-07-30 -- grp_idx used to increment HERE,
            // unconditionally, outside both branches below. When the next pixel
            // group was not yet preloaded the FSM correctly stayed in
            // S_WAIT_PPU, but the group counter kept advancing ONE PER CLOCK
            // while no input was consumed and no output produced. It free-ran
            // to tile_groups_r and fell into S_DONE, so PW reported done with
            // most of its input undelivered, stopped draining DW, DW's out FIFO
            // filled, DW throttled, and MM2S could never complete.
            //
            // The "(rare)" in the old comment below was true only at N_OC=30,
            // where PW spent max(cin_run, cout_rounded) = 30 cycles per group
            // and DW stayed comfortably ahead. At N_OC=8 that drops to 8
            // cycles, PW outruns DW, and the not-ready path became the COMMON
            // case. Measured on hardware (surrogate pair 0, 720p, N_OC=8): DW
            // stalled at consumed=44431 of 86400 with out_prog_full=1 while PW
            // already read STATUS=done.
            //
            // grp_idx must advance only when a group is actually retired.
            // Both branches below still test the pre-increment value, so the
            // terminal condition and the (grp_idx + 2) preload lookahead keep
            // exactly their original meaning.
            if ((grp_idx + 1) >= tile_groups_r) begin
              // All groups done -- but under Option C the drain context is
              // released at issue completion, so the final batch's last beats
              // may still be inside the PPU pipeline. Wait for the pipeline to
              // empty before pulsing done, otherwise done leads the data.
              // Costs a one-off ~9 cycles per RUN, not per batch.
              if (ppu_in_flight == 8'd0) begin
                grp_idx <= grp_idx + 32'd1;
                st <= S_DONE;
              end
            end else if (next_grp_ready) begin
              // Next group is preloaded - swap buffers and continue
              grp_idx      <= grp_idx + 32'd1;
              compute_buf  <= !compute_buf;
              oc_batch_idx <= 12'd0;
              w_addr_base  <= '0;
              ic_idx       <= '0;
              rd_issued    <= 1'b0;
              first_ic     <= 1'b1;
              // acc clear NOT needed: first_ic handles it
              // Start preloading next-next group (if any)
              next_grp_ready <= 1'b0;
              if ((grp_idx + 2) < tile_groups_r) begin
                load_st     <= L_LOADING;
                load_ic_idx <= '0;
              end else begin
                load_st <= L_IDLE;
              end
              st <= S_COMPUTE;
            end
            // else: next group not ready yet -- wait here WITHOUT advancing
            // grp_idx. This is the normal input-starved path, not a rare one.
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

      // ========================================================
      // [2] LOAD SUB-FSM (runs concurrently with main FSM)
      //   Preloads next pixel group into inactive buffer
      //   Only active during S_COMPUTE / S_BATCH_DONE / S_WAIT_PPU
      // ========================================================
      case (load_st)
        L_IDLE:  begin end  // nothing to do
        L_LOADING: begin
          if (valid_in) begin
            consume_in <= 1'b1;
            // Write to inactive buffer (opposite of compute_buf)
            if (compute_buf) begin
              // Compute reads B ? load writes A
              pb_a_wr_en   <= 1'b1;
              pb_a_wr_addr <= load_ic_idx[PB_AW-1:0];
              pb_a_wr_data <= pixel_in;
            end else begin
              // Compute reads A ? load writes B
              pb_b_wr_en   <= 1'b1;
              pb_b_wr_addr <= load_ic_idx[PB_AW-1:0];
              pb_b_wr_data <= pixel_in;
            end

            if (($unsigned(load_ic_idx) + 1) == cin_load) begin
              next_grp_ready <= 1'b1;
              load_st        <= L_READY;
            end else begin
              load_ic_idx <= load_ic_idx + 1'b1;
            end
          end
        end
        L_READY: begin end  // hold until main FSM resets us
        default: load_st <= L_IDLE;
      endcase

      // ========================================================
      // [3] PPU SUB-FSM (runs concurrently with main FSM)
      //   Drains shadow_acc through PPU instances in background
      //   Triggered from S_BATCH_DONE, runs during S_COMPUTE
      // ========================================================
      case (ppu_st)
        P_IDLE: begin end  // waiting for trigger

        // 1-cycle wait for param BRAM read latency
        // Also issue shadow BRAM pre-read for OC 0 (data arrives 1st cycle of P_DRAIN)
        P_PARAM_WAIT: begin
          ppu_bias_q  <= param_bias_data;
          ppu_mult_q  <= param_mult_data[23:0];  // upper 8b ignored (INT8 quant fits in 24b)
          ppu_shift_q <= param_shift_data;
          // Pre-read OC 1 params
          if (1 < $unsigned(ppu_drain_len)) begin
            param_rd_en   <= 1'b1;
            param_rd_addr <= ppu_oc_batch * N_OC + 1;
          end
          // Pre-read shadow BRAM OC 0 (1-cycle BRAM latency ? data ready at P_DRAIN cycle 0)
          sha_rd_en     <= 1'b1;
          sha_rd_addr   <= {sha_rd_bank, {SA_HIDX_W{1'b0}}};
          sha_rd_parity <= 1'b0;                     // issue 0 -> even half
          ppu_st <= P_DRAIN;
        end

        // Feed shadow BRAM accumulators to PPU, one OC per cycle
        P_DRAIN: begin
          if (!out_stall) begin
            if ($unsigned(ppu_issue_idx) < $unsigned(ppu_drain_len)) begin
              ppu_valid_in <= 1'b1;
              // Unpack sha_rd_data (arrived from pre-read issued previous cycle)
              for (li = 0; li < N_LANES; li++)
                ppu_acc_in[li] <= $signed(sha_rd_data[li*ACC_WIDTH +: ACC_WIDTH]);
              ppu_issue_idx <= ppu_issue_idx + 1'b1;

              // OPTION C (2026-08-07): release the drain context on ISSUE
              // completion. This used to wait for ppu_out_cnt == N_OC, i.e. for
              // the last beat to RETIRE, which cost the ~9-cycle PPU pipeline
              // tail on every batch while S_BATCH_DONE sat blocked on P_IDLE.
              // Safe because the PPU latches bias/mult/shift WITH the data at
              // its stage 0, so the parameters may change as soon as the last
              // valid_in has been sampled; and because the pipeline is strictly
              // in-order, so the next batch's outputs cannot overtake this
              // batch's. Shadow banks stay safe with 2 banks: a retiring beat
              // has already read the shadow BRAM.
              if (($unsigned(ppu_issue_idx) + 1) == $unsigned(ppu_drain_len))
                ppu_st <= P_IDLE;

              // Pre-read NEXT OC from shadow BRAM (data arrives next cycle)
              if (($unsigned(ppu_issue_idx) + 1) < $unsigned(ppu_drain_len)) begin
                // OC j lives at half-index j>>1 in half (j&1). Both halves share
                // this address bus; sha_rd_parity selects which one the muxed
                // sha_rd_data presents, registered so it lands with the data.
                sha_rd_en     <= 1'b1;
                sha_rd_addr   <= {sha_rd_bank,
                                  SA_HIDX_W'(($unsigned(ppu_issue_idx) + 1) >> 1)};
                sha_rd_parity <= ($unsigned(ppu_issue_idx) + 1) & 1'b1;
              end

              // Param pipeline: latch arriving, pre-read next
              if (($unsigned(ppu_issue_idx) + 1) < $unsigned(ppu_drain_len)) begin
                ppu_bias_q  <= param_bias_data;
                ppu_mult_q  <= param_mult_data[23:0];
                ppu_shift_q <= param_shift_data;
                if (($unsigned(ppu_issue_idx) + 2) < $unsigned(ppu_drain_len)) begin
                  param_rd_en   <= 1'b1;
                  param_rd_addr <= ppu_oc_batch * N_OC + ppu_issue_idx + 2;
                end
              end
            end

            // Count PPU outputs. Retained for debug only -- the P_IDLE exit is
            // now driven by issue completion above, and run completion by
            // ppu_in_flight. This counter no longer gates anything.
            if (ppu_valid_out)
              ppu_out_cnt <= ppu_out_cnt + 32'd1;
          end
        end

        default: ppu_st <= P_IDLE;
      endcase

    end
  end

  // Synchronous-only datapath registers to map optimally to the DSP48 input pipeline
  always_ff @(posedge clk) begin
    if (rd_issued) begin
      for (int li = 0; li < N_LANES; li++) begin
        pb_pixel_r[li] <= pb_pixel[li];
      end
      for (int oci = 0; oci < N_OC; oci++) begin
        w_rd_data_r[oci] <= w_rd_data[oci];
      end
    end
    if (rd_issued_d1) begin
      for (int p = 0; p < N_LANES/2; p++) begin
        a_packed_r[p] <= a_packed[p];
      end
      for (int oci = 0; oci < N_OC; oci++) begin
        w_rd_data_rr[oci] <= w_rd_data_r[oci];
      end
    end
  end

  // ---------------------------------------------------------------
  // shadow_copy_idx / shadow_copying / sha_wr_en: synchronous reset only
  //   Owned entirely by this block. main FSM issues shadow_copy_trig
  //   to start a copy; this block runs it to completion.
  //   Using synchronous reset removes these FFs from the async-reset
  //   fanout cone, eliminating recovery-time violations on CLR pins.
  // ---------------------------------------------------------------
  always_ff @(posedge clk) begin
    if (!rst_n) begin
      shadow_copying  <= 1'b0;
      shadow_copy_idx <= '0;
      sha_wr_en_e     <= 1'b0;
      sha_wr_en_o     <= 1'b0;
    end else begin
      sha_wr_en_e <= 1'b0;  // default deassert
      sha_wr_en_o <= 1'b0;
      if (shadow_copy_trig) begin
        // Latch trigger; start copy from idx 0
        shadow_copying  <= 1'b1;
        shadow_copy_idx <= '0;
      end else if (shadow_copying) begin
        // Two OCs per cycle: 2i into the even half, 2i+1 into the odd half.
        // The odd write is suppressed on a ragged final pair, i.e. when
        // Q_last is odd and OC 2i+1 does not exist.
        sha_wr_en_e <= 1'b1;
        sha_wr_en_o <= (({shadow_copy_idx, 1'b1}) < $unsigned(shadow_copy_len));
        sha_wr_addr <= {sha_wr_bank, shadow_copy_idx};
        // sha_wr_data_e/o driven combinationally from acc[2i] / acc[2i+1]
        if ($unsigned(shadow_copy_idx) >= (copy_pairs - 1)) begin
          shadow_copying  <= 1'b0;
          shadow_copy_idx <= '0;
        end else begin
          shadow_copy_idx <= shadow_copy_idx + 1'b1;
        end
      end
    end
  end

`ifndef SYNTHESIS
  // Step 10 safety properties. The MAC must never read a pb_ram entry the load
  // did not write, which is exactly "the batch windows tile the loaded region".
  always_ff @(posedge clk) begin
    if (rst_n) begin
      a_vq_off_when_not_compiled: assert (!((USE_PW_VQ == 0) && vq_mode))
        else $error("vq_mode asserted but USE_PW_VQ = 0");
      if (vq_mode_r && (st != S_IDLE)) begin
        a_vq_window_inside_load: assert ($unsigned(w_addr_base) + $unsigned(cin_run)
                                         <= $unsigned(cin_load))
          else $error("VQ window %0d+%0d overruns the %0d channels loaded",
                      w_addr_base, cin_run, cin_load);
        a_vq_load_fits_pb: assert ($unsigned(cin_load) <= CIN_MAX)
          else $error("vq_cin_load %0d exceeds CIN_MAX %0d", cin_load, CIN_MAX);
      end
    end
  end
`endif

endmodule

`default_nettype wire
