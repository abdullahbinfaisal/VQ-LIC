`timescale 1ns/1ps
// ============================================================================
// dw_stride2_tb.sv — self-checking TB for dw_banked_window_8x stride 1 and 2.
//
// Feeds a group-major stream, captures every emitted 8-lane window, and
// compares each of the 72 taps against a golden model computed straight from
// the pixel function. Checks emission COUNT and ORDER as well as tap values,
// so an off-by-one in group pairing or row parity fails loudly rather than
// producing plausible-looking garbage.
// ============================================================================
module dw_stride2_tb;

    localparam int C  = 8;
    localparam int W  = 64;
    localparam int G  = W/8;      // 4  (even -> stride-2 legal)
    localparam int H  = 16;
    localparam int ZP = 8'd7;

    logic clk = 0, rst_n = 0;
    always #5 clk = ~clk;

    logic        start_in, stride2;
    logic [11:0] cin_run, n_groups, img_width, n_rows;
    logic [63:0] pixel_in;
    logic        valid_in, consume_in;
    logic [7:0]  window [0:7][2:0][2:0];
    logic [7:0]  valid_out_vec;
    logic [11:0] ch_out;
    logic        done_out;
    logic [31:0] dbg_state, dbg_cfg;

    dw_banked_window_8x #(.DATA_WIDTH(8), .CIN_MAX(240), .MAX_CG_PRODUCT(10800)) dut (
        .clk(clk), .rst_n(rst_n), .start_in(start_in),
        .cin_run(cin_run), .n_groups(n_groups), .img_width(img_width),
        .n_rows(n_rows), .stride2(stride2), .zp_in(ZP),
        .pixel_in(pixel_in), .valid_in(valid_in), .consume_in(consume_in),
        .window(window), .valid_out_vec(valid_out_vec), .ch_out(ch_out),
        .done_out(done_out), .dbg_state(dbg_state), .dbg_cfg(dbg_cfg)
    );

    // Pixel function: deliberately mixes c, y and x so any tap taken from the
    // wrong channel, row or column disagrees.
    function automatic [7:0] px(input int c, input int y, input int x);
        px = 8'(((y * W + x) * 7 + c * 53 + 11) % 251 + 1);
    endfunction

    // Golden tap, with zero-padding to zp outside the real image.
    function automatic [7:0] gold(input int c, input int y, input int x);
        gold = (y < 0 || y >= H || x < 0 || x >= W) ? ZP : px(c, y, x);
    endfunction

    // ---------------- input feed ----------------
    // fed/emitted/errors are driven ONLY by the clocked block below; the
    // stimulus process resets them via clr rather than assigning directly
    // (xsim rejects mixed procedural drivers on the same variable).
    bit clr;
    int fed;      // beat index in group-major order
    always_comb begin
        automatic int r = fed / (G * C);
        automatic int g = (fed / C) % G;
        automatic int c = fed % C;
        for (int l = 0; l < 8; l++)
            pixel_in[l*8 +: 8] = px(c, r, g*8 + l);
    end

    // ---------------- capture + check ----------------
    int emitted, errors;
    int STRIDE, H_OUT, G_OUT;

    task automatic check_emission(input int k);
        automatic int c     = k % C;
        automatic int m     = (k / C) % G_OUT;
        automatic int r_out = k / (C * G_OUT);
        automatic int cy, cx;
        automatic logic [7:0] exp;
        if (ch_out != c) begin
            $error("emission %0d: ch_out=%0d expected %0d", k, ch_out, c);
            errors++;
            return;
        end
        for (int l = 0; l < 8; l++) begin
            cy = r_out * STRIDE;
            cx = (m*8 + l) * STRIDE;
            for (int kr = 0; kr < 3; kr++)
                for (int kc = 0; kc < 3; kc++) begin
                    exp = gold(c, cy + kr - 1, cx + kc - 1);
                    if (window[l][kr][kc] !== exp) begin
                        $error("emission %0d (r_out=%0d m=%0d c=%0d) lane %0d tap[%0d][%0d]: got %0d expected %0d",
                               k, r_out, m, c, l, kr, kc, window[l][kr][kc], exp);
                        errors++;
                    end
                end
        end
    endtask

    always_ff @(posedge clk) begin
        if (clr) begin
            fed     <= 0;
            emitted <= 0;
            errors  <= 0;
        end else begin
            if (consume_in) fed <= fed + 1;
            if (rst_n && valid_out_vec[0]) begin
                check_emission(emitted);
                emitted <= emitted + 1;
            end
        end
    end

    // ---------------- run ----------------
    task automatic run(input bit s2, input string name);
        automatic int expect_n;
        STRIDE = s2 ? 2 : 1;
        H_OUT  = s2 ? H/2 : H;
        G_OUT  = s2 ? G/2 : G;
        expect_n = H_OUT * G_OUT * C;

        clr = 1; @(posedge clk); @(negedge clk); clr = 0;
        stride2 = s2;
        cin_run = C; n_groups = G; img_width = W; n_rows = H;
        valid_in = 1;
        @(negedge clk); start_in = 1; @(negedge clk); start_in = 0;

        wait (done_out);
        repeat (40) @(posedge clk);

        $display("[%s] emitted=%0d expected=%0d fed=%0d errors=%0d",
                 name, emitted, expect_n, fed, errors);
        if (emitted != expect_n) begin
            $error("[%s] emission COUNT wrong: %0d vs %0d", name, emitted, expect_n);
            errors++;
        end
        if (errors == 0) $display("[%s] PASS", name);
        else             $display("[%s] FAIL (%0d errors)", name, errors);
    endtask

    int total_err;
    initial begin
        start_in = 0; valid_in = 0; stride2 = 0; clr = 0;
        cin_run = 0; n_groups = 0; img_width = 0; n_rows = 0;
        total_err = 0;
        repeat (10) @(posedge clk);
        rst_n = 1;
        repeat (5) @(posedge clk);

        run(1'b0, "stride1");
        total_err += errors;
        repeat (20) @(posedge clk);

        rst_n = 0; repeat (5) @(posedge clk); rst_n = 1; repeat (5) @(posedge clk);

        run(1'b1, "stride2");
        total_err += errors;

        if (total_err == 0) $display("\n=== ALL PASS ===");
        else                $display("\n=== FAILED: %0d errors ===", total_err);
        $finish;
    end

    initial begin
        #2000000;
        $display("TIMEOUT: emitted=%0d fed=%0d", emitted, fed);
        $finish;
    end

endmodule
