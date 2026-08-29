`timescale 1ns / 1ps

module compute_engine #(
    parameter int DATA_WIDTH = 8,
    parameter int ACC_WIDTH  = 32
)(
    input  logic                    clk,
    input  logic                    rst_n,

    input  logic                    start_in,
    output logic                    done_out,

    input  logic [11:0]             img_width,
    input  logic [11:0]             img_height,
    input  logic                    stride_2,
    input  logic                    bypass_1x1,
    input  logic [7:0]              pad_top,

    input  logic                    relu_en,
    input  logic [7:0]              zp_in,
    input  logic [7:0]              zp_out,

    input  logic [31:0]             mult_conv,
    input  logic [7:0]              shift_conv,
    input  logic signed [31:0]      bias_in,

    input  logic                    valid_in,
    input  logic [8*DATA_WIDTH-1:0] pixel_in,

    input  logic signed [DATA_WIDTH-1:0] weights [2:0][2:0],

    input  logic signed [ACC_WIDTH-1:0]  psum_in,
    input  logic                         psum_clear,

    output logic [8*DATA_WIDTH-1:0] pixel_out,
    output logic                    valid_out,
    output logic [7:0]              valid_out_vec,

    output logic signed [ACC_WIDTH-1:0] mac_debug_out,

    // NEW: tells TB when a real input pixel was consumed
    output logic                    consume_in
);

    logic [DATA_WIDTH-1:0]  lb_window [0:7][2:0][2:0];
    logic [7:0]             lb_valid_out_vec;
    logic                   lb_consume_in;

    logic signed [ACC_WIDTH-1:0] mac_result [0:7];
    logic [7:0]                  mac_valid_out_vec;

    logic lb_done_out;
    // 1) Line buffer (8-pixel parallel)
    line_buffer_8x #(
        .DATA_WIDTH(DATA_WIDTH),
        .MAX_IMG_WIDTH(2048)
    ) u_line_buf (
        .clk(clk),
        .rst_n(rst_n),
        .start_in(start_in),
        .img_width(img_width),
        .img_height(img_height),
        .stride_2(stride_2),
        .bypass_1x1(bypass_1x1),
        .zp_in(zp_in),
        .pixel_in(pixel_in),
        .valid_in(valid_in),
        .window(lb_window),
        .valid_out_vec(lb_valid_out_vec),
        .done_out(lb_done_out),
        .consume_in(lb_consume_in)
    );

    // Delay line buffer done_out by 14 cycles to perfectly align with MAC+PPU pipeline
    logic [13:0] done_out_shift;
    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) done_out_shift <= '0;
        else done_out_shift <= {done_out_shift[12:0], lb_done_out};
    end
    assign done_out = done_out_shift[13];

    assign consume_in = lb_consume_in;

    // 2) 8 Parallel MACs
    generate
        for (genvar i = 0; i < 8; i++) begin : G_MAC
            conv_mac_array #(
                .DATA_WIDTH(DATA_WIDTH),
                .ACC_WIDTH(ACC_WIDTH),
                .USE_DSP(1)
            ) u_mac (
                .clk(clk),
                .rst_n(rst_n),
                .zp_in(zp_in),
                .window(lb_window[i]),
                .valid_in(lb_valid_out_vec[i]),
                .weights(weights),
                .mac_out(mac_result[i]),
                .valid_out(mac_valid_out_vec[i])
            );
        end
    endgenerate

    // 3) 8 Parallel PPUs
    logic [DATA_WIDTH-1:0] ppu_pixel_out [0:7];
    logic [7:0] ppu_valid_out_vec;

    generate
        for (genvar i = 0; i < 8; i++) begin : G_PPU

            ppu #(
                .DATA_WIDTH(DATA_WIDTH),
                .ACC_WIDTH(ACC_WIDTH)
            ) u_ppu (
                .clk(clk),
                .rst_n(rst_n),
                .relu_en(relu_en),
                .mult_conv(mult_conv[23:0]),
                .shift_conv(shift_conv),
                .bias_in(bias_in),
                .zp_out(zp_out),
                .valid_in(mac_valid_out_vec[i]),
                .conv_acc_in(mac_result[i]),
                .pixel_out(ppu_pixel_out[i]),
                .valid_out(ppu_valid_out_vec[i])
            );
            
            assign pixel_out[i*DATA_WIDTH +: DATA_WIDTH] = ppu_pixel_out[i];
        end
    endgenerate

    // Or across the valid vector (since they are in lockstep)
    assign valid_out = |ppu_valid_out_vec;
    assign valid_out_vec = ppu_valid_out_vec;
    assign mac_debug_out = mac_result[0];

endmodule