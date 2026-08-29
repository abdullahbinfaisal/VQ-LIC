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
    input  logic                    is_depthwise,
    input  logic [7:0]              pad_top,

    input  logic                    mode_residual,
    input  logic                    relu_en,
    input  logic [7:0]              zp_in,
    input  logic [7:0]              zp_out,

    input  logic [31:0]             mult_conv,
    input  logic [7:0]              shift_conv,
    input  logic signed [31:0]      bias_in,

    input  logic [31:0]             mult_res_a,
    input  logic [31:0]             mult_res_b,
    input  logic [7:0]              shift_res,
    input  logic [7:0]              zp_in_a,
    input  logic [7:0]              zp_in_b,

    input  logic                    valid_in,
    input  logic [DATA_WIDTH-1:0]   pixel_in,

    input  logic signed [DATA_WIDTH-1:0] weights [2:0][2:0],

    input  logic signed [ACC_WIDTH-1:0]  psum_in,
    input  logic                         psum_clear,

    input  logic [DATA_WIDTH-1:0]   res_a_in,
    input  logic [DATA_WIDTH-1:0]   res_b_in,

    output logic [DATA_WIDTH-1:0]   pixel_out,
    output logic                    valid_out,

    output logic signed [ACC_WIDTH-1:0] mac_debug_out,

    // NEW: tells TB when a real input pixel was consumed
    output logic                    consume_in
);

    logic [DATA_WIDTH-1:0]  lb_window [2:0][2:0];
    logic                   lb_valid_out;
    logic                   lb_consume_in;

    logic signed [ACC_WIDTH-1:0] mac_result;
    logic                        mac_valid_out;

    // 1) Line buffer
    line_buffer #(
        .DATA_WIDTH(DATA_WIDTH),
        .MAX_IMG_WIDTH(2048),
        .MAX_PAD(1)
    ) u_line_buf (
        .clk(clk),
        .rst_n(rst_n),
        .start_in(start_in),
        .img_width(img_width),
        .img_height(img_height),
        .stride_2(stride_2),
        .bypass_1x1(bypass_1x1),
        .pad_top(pad_top),
        .zp_in(zp_in),
        .pixel_in(pixel_in),
        .valid_in(valid_in),
        .window(lb_window),
        .valid_out(lb_valid_out),
        .done_out(done_out),
        .consume_in(lb_consume_in)
    );

    assign consume_in = lb_consume_in;

    // 2) MAC
    conv_mac_array #(
        .DATA_WIDTH(DATA_WIDTH),
        .ACC_WIDTH(ACC_WIDTH)
    ) u_mac (
        .clk(clk),
        .rst_n(rst_n),
        .is_depthwise(is_depthwise),
        .zp_in(zp_in),
        .window(lb_window),
        .valid_in(lb_valid_out),
        .weights(weights),
        .psum_in(psum_in),
        .psum_clear(psum_clear),
        .mac_out(mac_result),
        .valid_out(mac_valid_out)
    );

    assign mac_debug_out = mac_result;

    // 3) PPU
    logic ppu_trigger;
    assign ppu_trigger = mode_residual ? valid_in : mac_valid_out;

    ppu #(
        .DATA_WIDTH(DATA_WIDTH),
        .ACC_WIDTH(ACC_WIDTH)
    ) u_ppu (
        .clk(clk),
        .rst_n(rst_n),
        .mode_residual(mode_residual),
        .relu_en(relu_en),
        .mult_conv(mult_conv),
        .shift_conv(shift_conv),
        .bias_in(bias_in),
        .mult_res_a(mult_res_a),
        .mult_res_b(mult_res_b),
        .shift_res(shift_res),
        .zp_out(zp_out),
        .zp_in_a(zp_in_a),
        .zp_in_b(zp_in_b),
        .valid_in(ppu_trigger),
        .conv_acc_in(mac_result),
        .res_a_in(res_a_in),
        .res_b_in(res_b_in),
        .pixel_out(pixel_out),
        .valid_out(valid_out)
    );

endmodule
