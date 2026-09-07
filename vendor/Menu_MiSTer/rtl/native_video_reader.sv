//============================================================================
//
//  Native Video DDR3 Reader
//
//  Reads RGB565 pixel data from DDR3 and outputs decoded RGB888 pixels
//  synchronized to the video timing generator via a dual-clock FIFO.
//
//  Line-on-demand design: reads one scanline at a time from DDR3, triggered
//  by the display consuming a line. Two lines are preloaded during vblank.
//
//  The FIFO is 64-bit wide (raw DDR3 beats). Each 64-bit word contains
//  4 RGB565 pixels. Beat reception is 1-cycle (no data loss). RGB565-to-
//  RGB888 decode happens on the read side, extracting 4 pixels per pop.
//
//  DDR3 Memory Map (physical addresses):
//    0x3A000000 + 0x000   : Control word (frame_counter[31:2], active_buffer[1:0])
//    0x3A000000 + 0x100   : Buffer 0 (384x224 RGB565 = 172,032 bytes)
//    0x3A000000 + 0x2A200 : Buffer 1
//
//  Clock domains:
//    Write side: ddr_clk (clk_sys, 100 MHz)
//    Read side:  clk_vid (CLK_VIDEO, 31.1538 MHz) with ce_pix divide-by-4 (7.7885 MHz)
//
//  Copyright (C) 2026 3S-ARM Project
//  Licensed under GNU General Public License v2+
//
//============================================================================

module native_video_reader (
    // DDR3 Avalon-MM master (directly drives DDRAM_ ports)
    input  wire        ddr_clk,         // DDRAM_CLK (= clk_sys = 100 MHz)
    input  wire        ddr_busy,        // DDRAM_BUSY
    output reg   [7:0] ddr_burstcnt,    // DDRAM_BURSTCNT
    output reg  [28:0] ddr_addr,        // DDRAM_ADDR
    input  wire [63:0] ddr_dout,        // DDRAM_DOUT
    input  wire        ddr_dout_ready,  // DDRAM_DOUT_READY
    output reg         ddr_rd,          // DDRAM_RD
    output wire [63:0] ddr_din,         // DDRAM_DIN (unused, tie to 0)
    output wire  [7:0] ddr_be,          // DDRAM_BE (all 1s for reads)
    output wire        ddr_we,          // DDRAM_WE (unused, tie to 0)

    // Pixel output (clk_vid domain)
    input  wire        clk_vid,         // video clock (31.1538 MHz, CLK_VIDEO)
    input  wire        ce_pix,          // pixel enable (divide-by-4, ~7.7885 MHz)
    input  wire        reset,           // active high reset

    // Timing inputs (from native_video_timing, clk_vid domain)
    input  wire        de,              // data enable
    input  wire        hblank,
    input  wire        vblank,
    input  wire        new_frame,       // pulse at start of vblank
    input  wire        new_line,        // pulse at start of hblank
    input  wire  [8:0] vcount,          // current line number

    // Pixel output
    output reg   [7:0] r_out,
    output reg   [7:0] g_out,
    output reg   [7:0] b_out,

    // Enable/status
    input  wire        enable,          // master enable from ARM config
    output wire        frame_ready      // indicates valid data being output
);

// Unused DDR3 write signals
assign ddr_din = 64'd0;
assign ddr_be  = 8'hFF;
assign ddr_we  = 1'b0;

// =========================================================================
// DDR3 Address Constants (29-bit qword addresses = physical >> 3)
// =========================================================================
localparam [28:0] CTRL_ADDR   = 29'h07400000;  // 0x3A000000 >> 3
localparam [28:0] BUF0_ADDR   = 29'h07400020;  // 0x3A000100 >> 3
localparam [28:0] BUF1_ADDR   = 29'h07405440;  // 0x3A02A200 >> 3
localparam [7:0]  LINE_BURST  = 8'd96;         // 768 bytes / 8 = 96 beats
localparam [28:0] LINE_STRIDE = 29'd96;        // 96 qword addresses per line
localparam [8:0]  V_ACTIVE    = 9'd224;

// Deadlock timeout: ~1M cycles at 100 MHz = ~10 ms
localparam [19:0] TIMEOUT_MAX = 20'hF_FFFF;

// =========================================================================
// Enable synchronizer (slow signal, 2-FF CDC to ddr_clk)
// =========================================================================
reg [1:0] enable_sync;
always @(posedge ddr_clk) begin
    if (reset)
        enable_sync <= 2'b0;
    else
        enable_sync <= {enable_sync[0], enable};
end
wire enable_ddr = enable_sync[1];

// =========================================================================
// CDC: new_frame from clk_vid (31.1538 MHz, ce_pix gated) to ddr_clk (100 MHz)
// Pulse is one 31.1538 MHz cycle wide (~32 ns), safely captured at 100 MHz.
// =========================================================================
reg [1:0] new_frame_sync;
always @(posedge ddr_clk) begin
    if (reset)
        new_frame_sync <= 2'b0;
    else
        new_frame_sync <= {new_frame_sync[0], new_frame};
end
wire new_frame_ddr = ~new_frame_sync[1] & new_frame_sync[0];

// =========================================================================
// CDC: new_line from clk_vid (31.1538 MHz, ce_pix gated) to ddr_clk (100 MHz)
// =========================================================================
reg [1:0] new_line_sync;
always @(posedge ddr_clk) begin
    if (reset)
        new_line_sync <= 2'b0;
    else
        new_line_sync <= {new_line_sync[0], new_line};
end
wire new_line_ddr = ~new_line_sync[1] & new_line_sync[0];

// =========================================================================
// CDC: vblank from clk_vid to ddr_clk (level signal, 2-FF sync)
// Used to prevent new_line triggers during vblank.
// =========================================================================
reg [1:0] vblank_sync;
always @(posedge ddr_clk) begin
    if (reset)
        vblank_sync <= 2'b0;
    else
        vblank_sync <= {vblank_sync[0], vblank};
end
wire vblank_ddr = vblank_sync[1];

// =========================================================================
// Reset synchronizer for clk_vid domain (async assert, sync deassert)
// =========================================================================
reg [1:0] reset_vid_sync;
always @(posedge clk_vid or posedge reset)
    if (reset) reset_vid_sync <= 2'b11;
    else       reset_vid_sync <= {reset_vid_sync[0], 1'b0};
wire reset_vid = reset_vid_sync[1];

// =========================================================================
// CDC: frame_ready from ddr_clk to clk_vid (2-FF synchronizer)
// =========================================================================
reg frame_ready_reg;
reg [1:0] frame_ready_sync;
always @(posedge clk_vid) begin
    if (reset_vid)
        frame_ready_sync <= 2'b0;
    else
        frame_ready_sync <= {frame_ready_sync[0], frame_ready_reg};
end
wire frame_ready_vid = frame_ready_sync[1];
assign frame_ready = frame_ready_vid;

// =========================================================================
// DDR3 Read State Machine (ddr_clk domain)
// =========================================================================
localparam [3:0] ST_IDLE         = 4'd0;
localparam [3:0] ST_POLL_CTRL    = 4'd1;
localparam [3:0] ST_WAIT_CTRL    = 4'd2;
localparam [3:0] ST_CHECK_CTRL   = 4'd3;
localparam [3:0] ST_READ_LINE    = 4'd4;
localparam [3:0] ST_WAIT_LINE    = 4'd5;
localparam [3:0] ST_LINE_DONE    = 4'd6;
localparam [3:0] ST_WAIT_DISPLAY = 4'd7;

reg  [3:0]  state;
reg  [31:0] ctrl_word;
reg  [29:0] prev_frame_counter;
reg         active_buffer;
reg  [28:0] buf_base_addr;
reg  [8:0]  cur_line;
reg  [6:0]  beat_count;
reg         first_frame_loaded;
reg  [4:0]  stale_vblank_count;
reg         preloading;
reg  [19:0] timeout_cnt;

// =========================================================================
// FIFO write: push raw 64-bit DDR3 beats directly on ddr_dout_ready.
// One beat = 4 RGB565 pixels. No multi-cycle decode on write side.
// =========================================================================
reg         fifo_wr;
reg  [63:0] fifo_wr_data;
wire        fifo_full;

// =========================================================================
// FIFO async clear
// =========================================================================
reg [3:0] fifo_aclr_cnt;
wire fifo_aclr_ddr_active = (fifo_aclr_cnt != 4'd0);
wire fifo_aclr = reset | fifo_aclr_ddr_active;

// =========================================================================
// Main state machine + FIFO write logic
// =========================================================================
always @(posedge ddr_clk) begin
    if (reset) begin
        state              <= ST_IDLE;
        ddr_rd             <= 1'b0;
        ddr_burstcnt       <= 8'd1;
        ddr_addr           <= 29'd0;
        ctrl_word          <= 32'd0;
        prev_frame_counter <= 30'd0;
        active_buffer      <= 1'b0;
        buf_base_addr      <= 29'd0;
        cur_line           <= 9'd0;
        beat_count         <= 7'd0;
        first_frame_loaded <= 1'b0;
        frame_ready_reg    <= 1'b0;
        stale_vblank_count <= 5'd0;
        preloading         <= 1'b0;
        timeout_cnt        <= 20'd0;
        fifo_wr            <= 1'b0;
        fifo_wr_data       <= 64'd0;
        fifo_aclr_cnt      <= 4'd0;
    end
    else begin
        // Default: deassert FIFO write each cycle
        fifo_wr <= 1'b0;

        // Count down FIFO clear hold timer
        if (fifo_aclr_cnt != 4'd0) fifo_aclr_cnt <= fifo_aclr_cnt - 4'd1;

        // Deassert DDR3 read request once accepted (not busy)
        if (!ddr_busy) ddr_rd <= 1'b0;

        // -----------------------------------------------------------
        // In ST_WAIT_LINE: capture EVERY DDR3 beat immediately.
        // This runs in parallel with the state machine below to ensure
        // no beats are missed. The beat is pushed into the 64-bit FIFO
        // in a single cycle.
        // -----------------------------------------------------------
        if (state == ST_WAIT_LINE && ddr_dout_ready) begin
            fifo_wr      <= 1'b1;
            fifo_wr_data <= ddr_dout;
            beat_count   <= beat_count + 7'd1;
            timeout_cnt  <= 20'd0;
        end

        case (state)
            ST_IDLE: begin
                if (enable_ddr && new_frame_ddr) begin
                    // NOTE: Do NOT clear the FIFO here. The previous frame's
                    // pixel data may still be needed if the ARM hasn't written
                    // a new frame yet. The FIFO is only cleared in CHECK_CTRL
                    // after confirming a new frame counter.
                    state <= ST_POLL_CTRL;
                end
            end

            ST_POLL_CTRL: begin
                if (!ddr_busy) begin
                    ddr_addr     <= CTRL_ADDR;
                    ddr_burstcnt <= 8'd1;
                    ddr_rd       <= 1'b1;
                    timeout_cnt  <= 20'd0;
                    state        <= ST_WAIT_CTRL;
                end
            end

            ST_WAIT_CTRL: begin
                if (ddr_dout_ready) begin
                    ctrl_word <= ddr_dout[31:0];
                    state     <= ST_CHECK_CTRL;
                    timeout_cnt <= 20'd0;
                end
                else if (timeout_cnt == TIMEOUT_MAX) begin
                    state <= ST_IDLE;
                end
                else begin
                    timeout_cnt <= timeout_cnt + 20'd1;
                end
            end

            ST_CHECK_CTRL: begin
                if (ctrl_word[31:2] != prev_frame_counter) begin
                    // New frame available -- NOW clear the FIFO and load it
                    prev_frame_counter <= ctrl_word[31:2];
                    active_buffer      <= ctrl_word[0];
                    stale_vblank_count <= 5'd0;
                    buf_base_addr      <= ctrl_word[0] ? BUF1_ADDR : BUF0_ADDR;
                    cur_line           <= 9'd0;
                    preloading         <= 1'b1;
                    fifo_aclr_cnt      <= 4'd8;
                    state              <= ST_READ_LINE;
                end
                else if (first_frame_loaded) begin
                    // Stale frame but we have a valid previous buffer --
                    // re-read the same buffer so the display shows the last
                    // good frame instead of going black. This handles the
                    // common case where ARM delivery drifts slightly behind
                    // the FPGA's vblank poll.
                    if (stale_vblank_count < 5'd30)
                        stale_vblank_count <= stale_vblank_count + 5'd1;
                    if (stale_vblank_count >= 5'd29)
                        frame_ready_reg <= 1'b0;
                    // Re-read previous buffer (buf_base_addr unchanged)
                    cur_line      <= 9'd0;
                    preloading    <= 1'b1;
                    fifo_aclr_cnt <= 4'd8;
                    state         <= ST_READ_LINE;
                end
                else begin
                    // No frame ever loaded -- just wait
                    state <= ST_IDLE;
                end
            end

            ST_READ_LINE: begin
                if (!ddr_busy && !fifo_aclr_ddr_active) begin
                    ddr_addr     <= buf_base_addr + (cur_line * LINE_STRIDE);
                    ddr_burstcnt <= LINE_BURST;
                    ddr_rd       <= 1'b1;
                    beat_count   <= 7'd0;
                    timeout_cnt  <= 20'd0;
                    state        <= ST_WAIT_LINE;
                end
            end

            ST_WAIT_LINE: begin
                // Beat capture is handled above (outside case).
                // Here we just check for completion or timeout.
                if (beat_count == LINE_BURST) begin
                    state <= ST_LINE_DONE;
                end
                else if (timeout_cnt == TIMEOUT_MAX) begin
                    state <= ST_IDLE;
                end
                else if (!ddr_dout_ready) begin
                    // Only increment timeout when no beat this cycle
                    timeout_cnt <= timeout_cnt + 20'd1;
                end
            end

            ST_LINE_DONE: begin
                cur_line <= cur_line + 9'd1;

                if (cur_line == V_ACTIVE - 9'd1) begin
                    first_frame_loaded <= 1'b1;
                    frame_ready_reg    <= 1'b1;
                    preloading         <= 1'b0;
                    state              <= ST_IDLE;
                end
                else if (preloading && cur_line < 9'd1) begin
                    state <= ST_READ_LINE;
                end
                else begin
                    preloading <= 1'b0;
                    state      <= ST_WAIT_DISPLAY;
                end
            end

            ST_WAIT_DISPLAY: begin
                // Only trigger on new_line when NOT in vblank (active display).
                // This prevents reading ahead during vblank which would overflow
                // the FIFO since the read side doesn't consume during vblank.
                if (cur_line < V_ACTIVE && new_line_ddr && !vblank_ddr) begin
                    state <= ST_READ_LINE;
                end
            end

            default: state <= ST_IDLE;
        endcase
    end
end

// =========================================================================
// Dual-Clock FIFO (Altera dcfifo primitive)
// 64-bit wide: stores raw DDR3 beats (4 RGB565 pixels per entry)
// Write side: ddr_clk (100 MHz) -- 1 beat per ddr_dout_ready cycle
// Read side: clk_vid (31.1538 MHz) -- pop 1 entry per 4 ce_pix cycles
// Depth 256: holds ~2.67 scanlines (96 beats/line * 2.67 = 256)
// =========================================================================
wire [63:0] fifo_rd_data;
wire        fifo_empty;
reg         fifo_rd;

dcfifo #(
    .intended_device_family ("Cyclone V"),
    .lpm_numwords           (256),
    .lpm_showahead          ("ON"),
    .lpm_type               ("dcfifo"),
    .lpm_width              (64),
    .lpm_widthu             (8),
    .overflow_checking      ("ON"),
    .rdsync_delaypipe       (4),
    .underflow_checking     ("ON"),
    .use_eab                ("ON"),
    .wrsync_delaypipe       (4)
) line_fifo (
    .aclr     (fifo_aclr),
    .data     (fifo_wr_data),
    .rdclk    (clk_vid),
    .rdreq    (fifo_rd),
    .wrclk    (ddr_clk),
    .wrreq    (fifo_wr),
    .q        (fifo_rd_data),
    .rdempty  (fifo_empty),
    .wrfull   (fifo_full),
    .eccstatus(),
    .rdfull   (),
    .rdusedw  (),
    .wrempty  (),
    .wrusedw  ()
);

// =========================================================================
// Pixel output (clk_vid domain with ce_pix gating)
//
// The FIFO holds 64-bit words (4 RGB565 pixels each). We pop one word
// every 4 ce_pix cycles and extract pixels sequentially.
// =========================================================================
reg  [63:0] pixel_word;      // Current 64-bit word being consumed
reg  [1:0]  pixel_sub;       // Sub-pixel index within word (0..3)
reg         pixel_word_valid; // We have a valid word to consume

// RGB565 decode from current sub-pixel
wire [15:0] cur_pix = pixel_word[{pixel_sub, 4'b0000} +: 16];
wire  [7:0] dec_r = {cur_pix[15:11], cur_pix[15:13]};
wire  [7:0] dec_g = {cur_pix[10:5],  cur_pix[10:9]};
wire  [7:0] dec_b = {cur_pix[4:0],   cur_pix[4:2]};

always @(posedge clk_vid) begin
    if (reset_vid) begin
        fifo_rd          <= 1'b0;
        r_out            <= 8'd0;
        g_out            <= 8'd0;
        b_out            <= 8'd0;
        pixel_word       <= 64'd0;
        pixel_sub        <= 2'd0;
        pixel_word_valid <= 1'b0;
    end
    else begin
        // Always deassert fifo_rd outside ce_pix to prevent overconsumption.
        // fifo_rd must be a single-cycle pulse on the clk_vid domain.
        fifo_rd <= 1'b0;

        if (ce_pix) begin
            if (de && frame_ready_vid) begin
                if (pixel_word_valid) begin
                    // Output current pixel from the word
                    r_out <= dec_r;
                    g_out <= dec_g;
                    b_out <= dec_b;

                    if (pixel_sub == 2'd3) begin
                        // Word exhausted. Do NOT prefetch the next word here --
                        // the show-ahead branch below loads it on the next active
                        // ce_pix. Prefetching pops a 97th word on the last pixel
                        // of the line, which the blanking branch then discards,
                        // shearing every line 4px further right than the last.
                        pixel_word_valid <= 1'b0;
                        pixel_sub        <= 2'd0;
                    end
                    else begin
                        pixel_sub <= pixel_sub + 2'd1;
                    end
                end
                else if (!fifo_empty) begin
                    // No valid word: load one from FIFO (show-ahead).
                    // pixel_sub advances to 1 because pixel 0 of this word is
                    // emitted below in the same cycle; leaving it at 0 re-emits
                    // pixel 0 on the next ce_pix and costs a column at the right.
                    pixel_word       <= fifo_rd_data;
                    pixel_word_valid <= 1'b1;
                    pixel_sub        <= 2'd1;
                    fifo_rd          <= 1'b1;
                    // Output first pixel immediately
                    r_out <= {fifo_rd_data[15:11], fifo_rd_data[15:13]};
                    g_out <= {fifo_rd_data[10:5],  fifo_rd_data[10:9]};
                    b_out <= {fifo_rd_data[4:0],   fifo_rd_data[4:2]};
                end
                else begin
                    // FIFO empty: output black
                    r_out <= 8'd0;
                    g_out <= 8'd0;
                    b_out <= 8'd0;
                end
            end
            else begin
                // Outside active display: output black, reset pixel state
                r_out            <= 8'd0;
                g_out            <= 8'd0;
                b_out            <= 8'd0;
                pixel_sub        <= 2'd0;
                pixel_word_valid <= 1'b0;
            end
        end
    end
end

endmodule
