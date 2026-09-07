//============================================================================
//
//  Native Video Top-Level Wrapper
//
//  Instantiates the timing generator and DDR3 reader, providing a clean
//  interface to menu.sv. Runs on CLK_VIDEO (31.1538 MHz) with integer
//  divide-by-4 ce_pix for 7.7885 MHz effective pixel rate.
//
//  Copyright (C) 2026 3S-ARM Project
//  Licensed under GNU General Public License v2+
//
//============================================================================

module native_video_top (
    input  wire        clk_sys,          // system clock (100 MHz) for DDR3
    input  wire        clk_vid,          // video clock (31.1538 MHz, CLK_VIDEO)
    input  wire        ce_pix,           // pixel enable (divide-by-4, ~7.7885 MHz)
    input  wire        reset,

    // OSD position offsets (two's complement, passed to timing generator)
    input  wire signed [3:0] h_offset,   // -8 to +7 pixels
    input  wire signed [3:0] v_offset,   // -8 to +7 lines

    // DDR3 Avalon-MM master
    input  wire        ddr_busy,
    output wire  [7:0] ddr_burstcnt,
    output wire [28:0] ddr_addr,
    input  wire [63:0] ddr_dout,
    input  wire        ddr_dout_ready,
    output wire        ddr_rd,
    output wire [63:0] ddr_din,
    output wire  [7:0] ddr_be,
    output wire        ddr_we,

    // Video output (active in clk_vid domain)
    output wire  [7:0] vga_r,
    output wire  [7:0] vga_g,
    output wire  [7:0] vga_b,
    output wire        vga_hs,
    output wire        vga_vs,
    output wire        vga_de,
    output wire        vga_hblank,
    output wire        vga_vblank,

    // Control
    input  wire        enable,           // from ARM: activate native video
    output wire        active,           // module is outputting valid video
    output wire        vsync_out         // active-high vsync for frame sync
);

// =========================================================================
// Timing Generator
// Runs on clk_vid (31.1538 MHz) with ce_pix gating at ~7.7885 MHz.
// H/V counters only advance on ce_pix pulses.
// =========================================================================
wire        tim_hsync;
wire        tim_vsync;
wire        tim_hblank;
wire        tim_vblank;
wire        tim_de;
wire [9:0]  tim_hcount;
wire [8:0]  tim_vcount;
wire        tim_new_frame;
wire        tim_new_line;

native_video_timing timing (
    .clk       (clk_vid),
    .ce_pix    (ce_pix),
    .reset     (reset),

    .h_offset  (h_offset),
    .v_offset  (v_offset),

    .hsync     (tim_hsync),
    .vsync     (tim_vsync),
    .hblank    (tim_hblank),
    .vblank    (tim_vblank),
    .de        (tim_de),
    .hcount    (tim_hcount),
    .vcount    (tim_vcount),
    .new_frame (tim_new_frame),
    .new_line  (tim_new_line)
);

// =========================================================================
// DDR3 Pixel Reader
// Write side: ddr_clk (100 MHz)
// Read side: clk_vid (31.1538 MHz) with ce_pix gating at ~7.7885 MHz
// =========================================================================
wire [7:0]  reader_r, reader_g, reader_b;
wire        reader_frame_ready;

native_video_reader reader (
    // DDR3 interface (clk_sys domain)
    .ddr_clk        (clk_sys),
    .ddr_busy       (ddr_busy),
    .ddr_burstcnt   (ddr_burstcnt),
    .ddr_addr       (ddr_addr),
    .ddr_dout       (ddr_dout),
    .ddr_dout_ready (ddr_dout_ready),
    .ddr_rd         (ddr_rd),
    .ddr_din        (ddr_din),
    .ddr_be         (ddr_be),
    .ddr_we         (ddr_we),

    // Pixel output (clk_vid domain with ce_pix gating)
    .clk_vid        (clk_vid),
    .ce_pix         (ce_pix),
    .reset          (reset),

    // Timing signals
    .de             (tim_de),
    .hblank         (tim_hblank),
    .vblank         (tim_vblank),
    .new_frame      (tim_new_frame),
    .new_line       (tim_new_line),
    .vcount         (tim_vcount),

    // Pixel output
    .r_out          (reader_r),
    .g_out          (reader_g),
    .b_out          (reader_b),

    // Control
    .enable         (enable),
    .frame_ready    (reader_frame_ready)
);

// =========================================================================
// Output assignments
// =========================================================================

// Video output: reader provides pixel data, timing provides sync.
//
// native_video_reader registers r_out/g_out/b_out from the `de` it was given,
// so pixel data lags the raw timing strobes by one ce_pix. Retime the strobes
// to match. Without this the first DE sample carries the reader's blanking
// black and the last two source columns are lost: 382 lands outside DE, and
// 383 is never emitted at all because the blanking branch fires first.
//
// The blanking strobes reset asserted: during reset the safe state is "blanked",
// not "displaying". Resetting them low lets hs_de go high for one ce_pix.
reg tim_hsync_q;
reg tim_vsync_q;
reg tim_de_q;
reg tim_hblank_q;
reg tim_vblank_q;

always @(posedge clk_vid) begin
    if (reset) begin
        tim_hsync_q  <= 1'b0;
        tim_vsync_q  <= 1'b0;
        tim_de_q     <= 1'b0;
        tim_hblank_q <= 1'b1;
        tim_vblank_q <= 1'b1;
    end
    else if (ce_pix) begin
        tim_hsync_q  <= tim_hsync;
        tim_vsync_q  <= tim_vsync;
        tim_de_q     <= tim_de;
        tim_hblank_q <= tim_hblank;
        tim_vblank_q <= tim_vblank;
    end
end

assign vga_r  = reader_r;
assign vga_g  = reader_g;
assign vga_b  = reader_b;
assign vga_hs = tim_hsync_q;
assign vga_vs = tim_vsync_q;
assign vga_de = tim_de_q;
assign vga_hblank = tim_hblank_q;
assign vga_vblank = tim_vblank_q;

// active: module is enabled and outputting valid frame data
assign active    = enable & reader_frame_ready;
assign vsync_out = tim_vsync_q;

endmodule
