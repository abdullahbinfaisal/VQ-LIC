`timescale 1ns/1ps
// ============================================================================
// dw_fused_core.sv — Fused group-major depthwise datapath (stride-1, 1-D)
// ============================================================================
//
// dw_seq_window_8x  →  8× conv_mac_array  →  8× ppu, with per-channel weights and
// PPU params supplied from on-chip RAMs addressed by the windower's `ch_out` tag.
//
// Group-major in (8 samples of channel c) → group-major out (8 results of ch c).
//
// CHANNEL-TAG PIPELINE ALIGNMENT
//   T    : windower emits window(c), ch_out=c, vvec=FF.
//          weight RAM read issued @c  (1-cyc latency).
//   T+1  : weights(c) ready; window registered (window_d). Both enter the MACs
//          together with valid. conv_mac_array skews window AND weights per-tap,
//          so each window meets its own channel's weights automatically.
//   T+10 : MAC result valid (conv_mac_array latency = 9 from its input @T+1).
//          param RAM read was issued @T+9 (ch_out delayed 9) so bias/mult/shift
//          for channel c land here, aligned with the MAC result entering the PPU.
//   T+18 : PPU output valid (ppu latency = 8). pixel_out = 8 results of channel c.
//
// Weights/params are loaded once before `start_in` via the *_wr_* ports
// (driven by the AXI-Lite shell, indexed by channel).
// ============================================================================

module dw_fused_core #(
    parameter int DATA_WIDTH = 8,
    parameter int ACC_WIDTH  = 32,
    parameter int CIN_MAX    = 240
)(
    input  logic                        clk,
    input  logic                        rst_n,

    input  logic                        start_in,
    output logic                        done_out,      // ~ last output beat emitted

    input  logic [11:0]                 cin_run,       // C
    input  logic [11:0]                 n_groups,      // G
    input  logic [7:0]                  zp_in,
    input  logic [7:0]                  zp_out,
    input  logic                        relu_en,

    // group-major input (8 samples of current channel)
    input  logic [8*DATA_WIDTH-1:0]     pixel_in,
    input  logic                        valid_in,
    output logic                        consume_in,

    // group-major output (8 results of current channel)
    output logic [8*DATA_WIDTH-1:0]     pixel_out,
    output logic                        valid_out,

    // ---- weight RAM load: 9 signed weights {w8..w0} for one channel ----
    input  logic                        w_wr_en,
    input  logic [11:0]                 w_wr_ch,
    input  logic [9*DATA_WIDTH-1:0]     w_wr_data,

    // ---- PPU param RAM load: bias/mult/shift for one channel ----
    input  logic                        p_wr_en,
    input  logic [11:0]                 p_wr_ch,
    input  logic signed [31:0]          p_bias,
    input  logic [31:0]                 p_mult,
    input  logic [7:0]                  p_shift
);

    localparam int PB_AW   = (CIN_MAX <= 1) ? 1 : $clog2(CIN_MAX);
    localparam int MAC_LAT = 9;   // conv_mac_array valid latency

    // ------------------------------------------------------------
    // Windower
    // ------------------------------------------------------------
    logic [DATA_WIDTH-1:0] win [0:7][2:0][2:0];
    logic [7:0]            win_vvec;
    logic [11:0]           win_ch;
    logic                  win_done;

    dw_seq_window_8x #(.DATA_WIDTH(DATA_WIDTH), .CIN_MAX(CIN_MAX)) u_win (
        .clk(clk), .rst_n(rst_n), .start_in(start_in),
        .cin_run(cin_run), .n_groups(n_groups), .zp_in(zp_in),
        .pixel_in(pixel_in), .valid_in(valid_in), .consume_in(consume_in),
        .window(win), .valid_out_vec(win_vvec), .ch_out(win_ch), .done_out(win_done)
    );

    wire win_valid = win_vvec[0];   // all 8 lanes valid together (stride-1)

    // ------------------------------------------------------------
    // Weight RAM (9 weights/channel), registered read addressed by win_ch
    // ------------------------------------------------------------
    (* ram_style = "block" *) logic [9*DATA_WIDTH-1:0] wram [0:CIN_MAX-1];
    logic [9*DATA_WIDTH-1:0] wram_q;
    always_ff @(posedge clk) begin
        if (w_wr_en) wram[w_wr_ch[PB_AW-1:0]] <= w_wr_data;
        wram_q <= wram[win_ch[PB_AW-1:0]];     // 1-cycle latency -> ready @T+1
    end

    // ------------------------------------------------------------
    // Stage T -> T+1: register window + valid (align to weight RAM latency)
    // ------------------------------------------------------------
    logic [DATA_WIDTH-1:0] win_d [0:7][2:0][2:0];
    logic                  vld_d;
    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) vld_d <= 1'b0;
        else        vld_d <= win_valid;
    end
    always_ff @(posedge clk) begin
        for (int i = 0; i < 8; i++)
            for (int r = 0; r < 3; r++)
                for (int c = 0; c < 3; c++)
                    win_d[i][r][c] <= win[i][r][c];
    end

    // unpack RAM weights into [2:0][2:0]
    logic signed [DATA_WIDTH-1:0] wts [2:0][2:0];
    always_comb
        for (int k = 0; k < 9; k++)
            wts[k/3][k%3] = $signed(wram_q[k*DATA_WIDTH +: DATA_WIDTH]);

    // ------------------------------------------------------------
    // 8× MAC (shared weights = current channel's 3x3)
    // ------------------------------------------------------------
    logic signed [ACC_WIDTH-1:0] mac_out [0:7];
    logic [7:0]                  mac_vld_vec;

    genvar i;
    generate
        for (i = 0; i < 8; i++) begin : G_MAC
            conv_mac_array #(.DATA_WIDTH(DATA_WIDTH), .ACC_WIDTH(ACC_WIDTH), .USE_DSP(0)) u_mac (
                .clk(clk), .rst_n(rst_n), .zp_in(zp_in),
                .window(win_d[i]), .valid_in(vld_d), .weights(wts),
                .mac_out(mac_out[i]), .valid_out(mac_vld_vec[i])
            );
        end
    endgenerate
    wire mac_valid = mac_vld_vec[0];

    // ------------------------------------------------------------
    // Channel tag delayed by MAC_LAT so PPU params land with the MAC result.
    //   ch_pipe[0] <= win_ch @T+1 ; ch_pipe[MAC_LAT-1] = win_ch @ T+MAC_LAT
    //   param read issued there (1-cyc) -> params ready @ T+MAC_LAT+1 = T+10.
    // ------------------------------------------------------------
    logic [11:0] ch_pipe [0:MAC_LAT-1];
    always_ff @(posedge clk) begin
        ch_pipe[0] <= win_ch;
        for (int s = 1; s < MAC_LAT; s++) ch_pipe[s] <= ch_pipe[s-1];
    end

    // ------------------------------------------------------------
    // PPU param RAM, registered read
    // ------------------------------------------------------------
    localparam int PRAM_W = 32 + 32 + 8;   // {bias, mult, shift}
    (* ram_style = "block" *) logic [PRAM_W-1:0] pram [0:CIN_MAX-1];
    logic [PRAM_W-1:0] pram_q;
    always_ff @(posedge clk) begin
        if (p_wr_en) pram[p_wr_ch[PB_AW-1:0]] <= {p_bias, p_mult, p_shift};
        pram_q <= pram[ch_pipe[MAC_LAT-1][PB_AW-1:0]];   // ready @ T+10 with mac_valid
    end

    // pram_q = { bias[71:40], mult[39:8], shift[7:0] }
    wire signed [31:0] ppu_bias  = $signed(pram_q[40 +: 32]);
    wire        [31:0] ppu_mult  = pram_q[8  +: 32];
    wire        [7:0]  ppu_shift = pram_q[7:0];

    // ------------------------------------------------------------
    // 8× PPU (shared params = current channel)
    // ------------------------------------------------------------
    logic [DATA_WIDTH-1:0] ppu_px [0:7];
    logic [7:0]            ppu_vld_vec;
    generate
        for (i = 0; i < 8; i++) begin : G_PPU
            ppu #(.DATA_WIDTH(DATA_WIDTH), .ACC_WIDTH(ACC_WIDTH)) u_ppu (
                .clk(clk), .rst_n(rst_n), .relu_en(relu_en),
                .mult_conv(ppu_mult[23:0]), .shift_conv(ppu_shift), .bias_in(ppu_bias),
                .zp_out(zp_out),
                .valid_in(mac_valid), .conv_acc_in(mac_out[i]),
                .pixel_out(ppu_px[i]), .valid_out(ppu_vld_vec[i])
            );
            assign pixel_out[i*DATA_WIDTH +: DATA_WIDTH] = ppu_px[i];
        end
    endgenerate
    assign valid_out = ppu_vld_vec[0];

    // ------------------------------------------------------------
    // done_out: windower done delayed by total datapath latency (1+9+8 = 18)
    //   (informational; the AXIS shell counts output beats for TLAST)
    // ------------------------------------------------------------
    localparam int TOT_LAT = 1 + MAC_LAT + 8;
    logic [TOT_LAT-1:0] done_sr;
    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) done_sr <= '0;
        else        done_sr <= {done_sr[TOT_LAT-2:0], win_done};
    end
    assign done_out = done_sr[TOT_LAT-1];

endmodule
`default_nettype wire
