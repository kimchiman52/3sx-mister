//============================================================================
//
//  Menu for MiSTer.
//  Copyright (C) 2017-2020 Sorgelig
//
//
//  This program is free software; you can redistribute it and/or modify it
//  under the terms of the GNU General Public License as published by the Free
//  Software Foundation; either version 2 of the License, or (at your option)
//  any later version.
//
//  This program is distributed in the hope that it will be useful, but WITHOUT
//  ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
//  FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
//  more details.
//
//  You should have received a copy of the GNU General Public License along
//  with this program; if not, write to the Free Software Foundation, Inc.,
//  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
//
//============================================================================

module emu
(
	//Master input clock
	input         CLK_50M,

	//Async reset from top-level module.
	//Can be used as initial reset.
	input         RESET,

	//Must be passed to hps_io module
	inout  [48:0] HPS_BUS,

	//Base video clock. Usually equals to CLK_SYS.
	output        CLK_VIDEO,

	//Multiple resolutions are supported using different CE_PIXEL rates.
	//Must be based on CLK_VIDEO
	output        CE_PIXEL,

	//Video aspect ratio for HDMI. Most retro systems have ratio 4:3.
	//if VIDEO_ARX[12] or VIDEO_ARY[12] is set then [11:0] contains scaled size instead of aspect ratio.
	output [12:0] VIDEO_ARX,
	output [12:0] VIDEO_ARY,

	output  [7:0] VGA_R,
	output  [7:0] VGA_G,
	output  [7:0] VGA_B,
	output        VGA_HS,
	output        VGA_VS,
	output        VGA_DE,    // = ~(VBlank | HBlank)
	output        VGA_F1,
	output [1:0]  VGA_SL,
	output        VGA_SCALER, // Force VGA scaler
	output        VGA_DISABLE, // analog out is off

	input  [11:0] HDMI_WIDTH,
	input  [11:0] HDMI_HEIGHT,
	output        HDMI_FREEZE,
	output        HDMI_BLACKOUT,
	output        HDMI_BOB_DEINT,

`ifdef MISTER_FB
	// Use framebuffer in DDRAM
	// FB_FORMAT:
	//    [2:0] : 011=8bpp(palette) 100=16bpp 101=24bpp 110=32bpp
	//    [3]   : 0=16bits 565 1=16bits 1555
	//    [4]   : 0=RGB  1=BGR (for 16/24/32 modes)
	//
	// FB_STRIDE either 0 (rounded to 256 bytes) or multiple of pixel size (in bytes)
	output        FB_EN,
	output  [4:0] FB_FORMAT,
	output [11:0] FB_WIDTH,
	output [11:0] FB_HEIGHT,
	output [31:0] FB_BASE,
	output [13:0] FB_STRIDE,
	input         FB_VBL,
	input         FB_LL,
	output        FB_FORCE_BLANK,

`ifdef MISTER_FB_PALETTE
	// Palette control for 8bit modes.
	// Ignored for other video modes.
	output        FB_PAL_CLK,
	output  [7:0] FB_PAL_ADDR,
	output [23:0] FB_PAL_DOUT,
	input  [23:0] FB_PAL_DIN,
	output        FB_PAL_WR,
`endif
`endif

	output        LED_USER,  // 1 - ON, 0 - OFF.

	// b[1]: 0 - LED status is system status OR'd with b[0]
	//       1 - LED status is controled solely by b[0]
	// hint: supply 2'b00 to let the system control the LED.
	output  [1:0] LED_POWER,
	output  [1:0] LED_DISK,

	// I/O board button press simulation (active high)
	// b[1]: user button
	// b[0]: osd button
	output  [1:0] BUTTONS,

	input         CLK_AUDIO, // 24.576 MHz
	output [15:0] AUDIO_L,
	output [15:0] AUDIO_R,
	output        AUDIO_S,   // 1 - signed audio samples, 0 - unsigned
	output  [1:0] AUDIO_MIX, // 0 - no mix, 1 - 25%, 2 - 50%, 3 - 100% (mono)

	//ADC
	inout   [3:0] ADC_BUS,

	//SD-SPI
	output        SD_SCK,
	output        SD_MOSI,
	input         SD_MISO,
	output        SD_CS,
	input         SD_CD,

	//High latency DDR3 RAM interface
	//Use for non-critical time purposes
	output        DDRAM_CLK,
	input         DDRAM_BUSY,
	output  [7:0] DDRAM_BURSTCNT,
	output [28:0] DDRAM_ADDR,
	input  [63:0] DDRAM_DOUT,
	input         DDRAM_DOUT_READY,
	output        DDRAM_RD,
	output [63:0] DDRAM_DIN,
	output  [7:0] DDRAM_BE,
	output        DDRAM_WE,

	//SDRAM interface with lower latency
	output        SDRAM_CLK,
	output        SDRAM_CKE,
	output [12:0] SDRAM_A,
	output  [1:0] SDRAM_BA,
	inout  [15:0] SDRAM_DQ,
	output        SDRAM_DQML,
	output        SDRAM_DQMH,
	output        SDRAM_nCS,
	output        SDRAM_nCAS,
	output        SDRAM_nRAS,
	output        SDRAM_nWE,

`ifdef MISTER_DUAL_SDRAM
	//Secondary SDRAM
	//Set all output SDRAM_* signals to Z ASAP if SDRAM2_EN is 0
	input         SDRAM2_EN,
	output        SDRAM2_CLK,
	output [12:0] SDRAM2_A,
	output  [1:0] SDRAM2_BA,
	inout  [15:0] SDRAM2_DQ,
	output        SDRAM2_nCS,
	output        SDRAM2_nCAS,
	output        SDRAM2_nRAS,
	output        SDRAM2_nWE,
`endif

	input         UART_CTS,
	output        UART_RTS,
	input         UART_RXD,
	output        UART_TXD,
	output        UART_DTR,
	input         UART_DSR,

	// Open-drain User port.
	// 0 - D+/RX
	// 1 - D-/TX
	// 2..6 - USR2..USR6
	// Set USER_OUT to 1 to read from USER_IN.
	input   [6:0] USER_IN,
	output  [6:0] USER_OUT,

	input         OSD_STATUS,

	// Native video active signal for sys_top.v vsync routing
	output        NATIVE_VID_ACTIVE
);

assign ADC_BUS  = 'Z;
assign {UART_RTS, UART_DTR} = 0;
assign {SD_SCK, SD_MOSI, SD_CS} = 'Z;

assign DDRAM_CLK = clk_sys;

// CE_PIXEL: divide CLK_VIDEO (31.1538 MHz) by 4 for ~7.7885 MHz effective pixel rate.
// Integer divider = zero pixel timing jitter.
reg [1:0] ce_div;
wire ce_pix_div4 = (ce_div == 2'd0);
wire ce_pix2x = (ce_div[0] == 1'b0);
always @(posedge CLK_VIDEO) begin
	if (RESET) ce_div <= 2'd0;
	else ce_div <= ce_div + 2'd1;
end
assign CE_PIXEL = ce_pix_div4;

assign VGA_SL = 0;
assign VGA_F1 = 0;
// CPS3 was designed for 4:3 CRT monitors.  Tells ASCAL to pillarbox on 16:9.
// "Full" (status[12]=1) sends ARX=0/ARY=0 so the scaler fills the display
// without resampling — needed for clean pixels when vga_scaler=1 routes
// analog output through ASCAL.  Has no effect on direct analog (vga_scaler=0).
wire ar_full = status[12];

// --- Vertical crop / integer scale (active on HDMI only, via video_freak) ---
wire [11:0] vcrop_size = status[32] ? 12'd216 : 12'd0;

// Crop offset: {vcopt,1'b0} doubles the 4-bit index to get even pixel offsets.
// CONF_STR options: 0, 2, 4, 6, 8, 10, -12, -10, -8, -6, -4, -2
wire [3:0] vcopt = status[36:33];
reg  [4:0] vcrop_off;
always @(posedge CLK_VIDEO) begin
	vcrop_off <= (vcopt < 6) ? {vcopt,1'b0} : ({vcopt,1'b0} - 5'd24);
end

wire [1:0] vscale = status[38:37]; // 2 bits: menu exposes 4 of 5 SCALE modes; zero-extends to video_freak's 3-bit port

// Remap OSD index to signed 4-bit scale for hsize module.
// CONF_STR assigns sequential indices 0-8 but the hsize summand uses
// scale[3] as sign.  Indices 5-8 (negative labels) need their high bit set.
wire [3:0] hsize_raw = status[42:39];
reg  [3:0] hsize_scale;
always @(*) begin
    case (hsize_raw)
        4'd5:    hsize_scale = 4'b1100; // -4
        4'd6:    hsize_scale = 4'b1101; // -3
        4'd7:    hsize_scale = 4'b1110; // -2
        4'd8:    hsize_scale = 4'b1111; // -1
        default: hsize_scale = hsize_raw;
    endcase
end
wire       hsize_enable = (hsize_scale != 4'd0);

wire [11:0] arx_in = ar_full ? 12'd0 : 12'd4;
wire [11:0] ary_in = ar_full ? 12'd0 : 12'd3;

video_freak video_freak
(
	.CLK_VIDEO   (CLK_VIDEO),
	.CE_PIXEL    (CE_PIXEL),
	.VGA_VS      (VGA_VS),
	.HDMI_WIDTH  (HDMI_WIDTH),
	.HDMI_HEIGHT (HDMI_HEIGHT),
	.VGA_DE      (VGA_DE),
	.VIDEO_ARX   (VIDEO_ARX),
	.VIDEO_ARY   (VIDEO_ARY),

	.VGA_DE_IN   (vga_de_raw),
	.ARX         (arx_in),
	.ARY         (ary_in),
	.CROP_SIZE   (vcrop_size),
	.CROP_OFF    (vcrop_off),
	.SCALE       (vscale)
);
assign VGA_SCALER= 0;
assign VGA_DISABLE = 0;

assign AUDIO_MIX = 0;
assign HDMI_FREEZE = 0;
assign HDMI_BLACKOUT = 0;

assign LED_DISK = 0;
assign LED_POWER[1]= 1;
assign BUTTONS = 0;

reg  [26:0] act_cnt;
always @(posedge clk_sys) act_cnt <= act_cnt + 1'd1; 
assign LED_USER    = FB ? led[0] : act_cnt[26]  ? act_cnt[25:18]  > act_cnt[7:0]  : act_cnt[25:18]  <= act_cnt[7:0];

wire [26:0] act_cnt2 = {~act_cnt[26],act_cnt[25:0]};
assign LED_POWER[0]= FB ? led[2] : act_cnt2[26] ? act_cnt2[25:18] > act_cnt2[7:0] : act_cnt2[25:18] <= act_cnt2[7:0];


`include "build_id.v" 
localparam CONF_STR = {
	"MENU;UART31250,MIDI;",
	// Top row by the maintainer's explicit choice: the fastest way into a
	// game sits first. Selecting it signals the RUNNING game to jump
	// straight into a live training match with the last-used characters
	// and settings — no restart, no menus: the HPS menu intercepts the
	// OSD select directly (main-mister-full-menu.patch, bit==15 ->
	// quick_training_signal() = SIGRTMIN+5 to the child) and then
	// dismisses the OSD itself; the bit's value is never read by RTL or
	// wrapper. Bit 15 was the retired "SA Quality" row's bit (removed
	// with the other SA-quality rows), so stale 3S-ARM.CFG files may
	// carry it set — safe for a T row for exactly the [31] reasons above,
	// but do NOT reuse [15] for an O-value field without the [30]-style
	// seed-after-CFG defense. NOT [2-9]: despite looking free in this
	// string, upstream MENU-core handling owns [0], [3:1] and [4], and
	// this file itself reads [4] (PAL), [5] (FB), [8:6] (led) and [9]
	// (NATIVE_VID).
	"T[15],Quick Training;",
	"T[29],Play Online;",
	// Top-level by design: watching replays is a primary activity, so it
	// sits with Play Online rather than inside a page.
	// Selecting this row starts the weekly-best shuffle viewer, which plays
	// the cached .3sr set back to back. The row is UNCONDITIONAL by design:
	// with an empty or stale cache it still appears and the viewer plays
	// whatever is on disk, so there is no dynamic-menu logic to get wrong.
	// Bit 31 was V-Position's top bit through release v20260416 (stale CFGs
	// may carry it set). Safe for a T: the HPS menu intercepts the OSD
	// select directly (main-mister-full-menu.patch, bit==31) and the bit's
	// value is never read by RTL or wrapper. Do NOT reuse [31] for an
	// O-value field without the [30]-style seed-after-CFG defense.
	"T[31],Watch Replays;",
	"T[23],Button Check;",
	"-;",
	// Pages. Reordering rows is presentation-only: the HPS wrapper keys
	// every option and trigger on its BIT NUMBER, never on menu position
	// (thirdsarm_wrapper.cpp's user_io_status_get/set calls all take a
	// literal "[n]" / "[hi:lo]"). The P<n> syntax below is the one proven
	// by 57ec219d, which shipped a "P1,Performance;" page in this same
	// config string.
	"P1,Game;",
	// Balance request. Bit 48 is deliberately BRAND NEW: it has never
	// appeared in any CONF_STR in this tree's history, in any
	// user_io_status_* call in the wrapper, or in upstream Main_MiSTer's
	// MENU-core handling, so no existing 3S-ARM.CFG can carry it set.
	// Every reuse candidate is contaminated: [30] is the retired
	// ex-"Arcade Balance" bit AND V-Position's middle bit before
	// v20260416, [15]/[18:16] are the retired SA-quality rows, [31] is
	// now Watch Replays and [47] is Language.
	//
	// This row is a REQUEST, not an outcome. It writes the `balance` key
	// in the game config (index 0 -> "arcade", index 1 -> "ps2") and the
	// game resolves it once at boot (ArcadeBalance_Init(),
	// src/arcade/arcade_balance.c), so a change applies on the NEXT game
	// launch. Asking for Arcade on a device with no CPS3 romset -- or one
	// whose 20-character adaptation fails -- still boots PS2, with the
	// reason logged. The "Balance:" status row directly below reports
	// what the game actually resolved.
	//
	// The row is deliberately NOT locked out when no ROM is present.
	// Locking it would mean either overwriting the player's stored
	// preference with `ps2` -- silently losing their choice the day they
	// install the romset -- or new status_menumask plumbing driven from
	// RTL, which cannot see whether a ROM exists on the HPS filesystem.
	// Keeping the request live and reporting the outcome underneath is
	// the whole design; do not "fix" it into a lockout.
	"P1O[48],Balance,Arcade,PS2;",
	"P1O[13],Game Mode,Console,Arcade;",
	// Bit 47 is brand new: it has never appeared in any CONF_STR in this
	// tree's history, in any user_io_status_* call in the wrapper, or in
	// upstream Main_MiSTer's MENU-core handling (which owns [0], [3:1] and
	// [4]), so no existing 3S-ARM.CFG can carry it set. That is why it was
	// chosen over the tempting reuse candidates: [30] (ex-Arcade Balance,
	// and V-Position's middle bit before v20260416) and [15]/[18:16]
	// (the retired SA-quality rows). [31] is now IN USE by the replay
	// browser trigger above. The wrapper still seeds [47] from the game config after the
	// CFG load, in the same block as [14], because the game config is
	// authoritative for every wrapper-owned option.
	"P1O[47],Language,English,Japanese;",
	// Bit 14 was "SA Activation"'s bit (a retired super-effect-quality OSD
	// row) before that row was removed, so old 3S-ARM.CFG files may carry
	// it set. Safe only because the wrapper seeds [14] from the game
	// config AFTER CFG load (thirdsarm_wrapper.cpp) — keep that ordering
	// if this bit ever moves.
	"P1O[14],BGM Type,Arranged,Original;",
	"P1O[24],Hold to Pause,Off,On;",
	"P1O[11:10],FPS Counter,Off,FPS,Debug;",
	// Live status row: what the game actually RESOLVED, as opposed to the
	// P1O[48] toggle above, which is only the REQUEST. Deliberately last on
	// the page and rendered inverted (menu.cpp, via
	// tools/mister-wrapper/main-mister-full-menu.patch) so it reads as a
	// footer rather than as another setting -- it sat mid-list under the
	// toggle before and looked like a broken option row.
	// NOT an option and deliberately without a status bit. menu.cpp
	// substitutes the live value from
	// /media/fat/games/3s-arm/balance.status into any text row whose label
	// is exactly "Balance:" (thirdsarm_balance_status_line() in
	// thirdsarm_wrapper.cpp) -- keep the label byte-identical if this row
	// ever moves again, and keep it LAST so the inverted styling does not
	// read as a selected row mid-list. See docs/mister-wrapper.md
	// "Balance Status Line".
	"P1-,Balance:;",
	"P2,Video;",
	"P2O[12],Aspect Ratio,4:3,Full;",
	"P2O[32],Vertical Crop,Disabled,216p(5x);",
	"P2O[36:33],Crop Offset,0,2,4,6,8,10,-12,-10,-8,-6,-4,-2;",
	"P2O[38:37],Scale,Normal,V-Integer,Narrower HV-Integer,Wider HV-Integer;",
	"P2O[42:39],H Size,0,+1,+2,+3,+4,-4,-3,-2,-1;",
	"P2O[28:25],H Position,0,+1,+2,+3,+4,+5,+6,+7,-8,-7,-6,-5,-4,-3,-2,-1;",
	"P2O[46:43],V Position,0,+1,+2,+3,+4,+5,+6,+7,-8,-7,-6,-5,-4,-3,-2,-1;",
	// Overclock is the only System row today. The page stays: netplay
	// diagnostics / status rows land here next.
	"P3,System;",
	"P3O[20:19],Overclock,Stock,1000MHz,1200MHz;",
	"-;",
	"T[21],Reset to Default;",
	"T[22],Restart;",
	"-;",
	"J1,LP,MP,HP,LK,MK,HK,Select,Start;",
	"jn,Y,X,L,B,A,R,Select,Start;",
	"V,v",`BUILD_DATE
};

wire forced_scandoubler;
// Width tracks the highest bit the CONF_STR above declares ([48], Balance).
// [47] (Language) and [48] (Balance) are HPS-side only — nothing in this
// file reads them — but keeping the wire wide enough to cover the whole
// config string preserves the invariant that every declared bit exists
// here. hps_io drives a 128-bit status; the connection simply truncates.
wire [48:0] status;

hps_io #(.CONF_STR(CONF_STR), .CONF_STR_BRAM(1)) hps_io
(
	.clk_sys(clk_sys),
	.HPS_BUS(HPS_BUS),
	.forced_scandoubler(forced_scandoubler),
	.status(status),
	.status_menumask(cfg)
);

////////////////////   CLOCKS   ///////////////////
wire locked, clk_sys;
wire clk_pix;   // Dedicated video PLL: 31.1538 MHz (CLK_VIDEO, divided by 4 for 7.7885 MHz pixels)
pll pll
(
	.refclk(CLK_50M),
	.rst(0),
	.outclk_0(clk_sys),
	.locked(locked)
);

pll_video pll_vid
(
	.refclk(CLK_50M),
	.rst(0),
	.outclk_0(clk_pix),
	.locked()
);

assign CLK_VIDEO = clk_pix;

// --- Native video control ---
wire NATIVE_VID = status[9];
assign NATIVE_VID_ACTIVE = NATIVE_VID;


/////////////////////   SDRAM   ///////////////////
//
// Helper functionality:
//    SDRAM and DDR3 RAM are being cleared while this core is working.
//    some cores behave incorrectly if started with non-clean RAM.

sdram sdr
(
	.*,
	.init(~locked),
	.clk(clk_sys),
	.addr(sdram_addr),
	.wtbt(3),
	.dout(sdram_dout),
	.din(sdram_din),
	.rd(sdram_rd),
	.we(sdram_we),
	.ready(sdram_ready)
);

reg  [26:0] sdram_addr;
wire        sdram_ready;
wire [15:0] sdram_dout;
reg  [15:0] sdram_din;
reg         sdram_we;
reg         sdram_rd;
reg  [15:0] cfg = 0;

always @(posedge clk_sys) begin
	reg [4:0] state = 0;

	sdram_rd <= 0;
	sdram_we <= 0;

	if(RESET) begin
		state <= 0;
		cfg <= 0;
	end
	else begin
		case(state)
			0: if(sdram_ready) begin
					cfg <= 0;
					state      <= state+1'd1;
				end
			1: begin
					sdram_addr <= 'h4000000;
					sdram_din  <= 3128;
					sdram_we   <= 1;
					state      <= state+1'd1;
				end
			2: state <= state+1'd1;
			3: if(sdram_ready) begin
					sdram_addr <= 'h2000000;
					sdram_din  <= 2064;
					sdram_we   <= 1;
					state      <= state+1'd1;
				end
			4: state <= state+1'd1;
			5: if(sdram_ready) begin
					sdram_addr <= 'h0000000;
					sdram_din  <= 1032;
					sdram_we   <= 1;
					state      <= state+1'd1;
				end
			6: state <= state+1'd1;
			7: if(sdram_ready) begin
					sdram_addr <= 'h1000000;
					sdram_din  <= 12345;
					sdram_we   <= 1;
					state      <= state+1'd1;
				end
			8: state <= state+1'd1;
			9: if(sdram_ready) begin
					sdram_addr <= 'h4000000;
					sdram_rd   <= 1;
					state      <= state+1'd1;
				end
			10: state <= state+1'd1;
			11: if(sdram_ready) begin
					cfg[2]     <= (sdram_dout == 3128);
					sdram_addr <= 'h2000000;
					sdram_rd   <= 1;
					state      <= state+1'd1;
				end
			12: state <= state+1'd1;
			13: if(sdram_ready) begin
					cfg[1]     <= (sdram_dout == 2064);
					sdram_addr <= 'h0000000;
					sdram_rd   <= 1;
					state      <= state+1'd1;
				end
			14: state <= state+1'd1;
			15: if(sdram_ready) begin
					cfg[0]     <= (sdram_dout == 1032);
					cfg[15]    <= 1;
					state      <= state+1'd1;
				end
			16: begin
					sdram_addr <= addr[24:0];
					sdram_din  <= 0;
					sdram_we   <= we;
				end
		endcase
	end
end

// --- DDR3 port sharing: old ddram (SDRAM clear) + native video reader ---
wire  [7:0] old_ddr_burstcnt;
wire [28:0] old_ddr_addr;
wire        old_ddr_rd;
wire [63:0] old_ddr_din;
wire  [7:0] old_ddr_be;
wire        old_ddr_we;

ddram ddr
(
	.DDRAM_CLK(clk_sys),
	.DDRAM_BUSY(DDRAM_BUSY),
	.DDRAM_BURSTCNT(old_ddr_burstcnt),
	.DDRAM_ADDR(old_ddr_addr),
	.DDRAM_DOUT(DDRAM_DOUT),
	.DDRAM_DOUT_READY(DDRAM_DOUT_READY & ~use_nv),
	.DDRAM_RD(old_ddr_rd),
	.DDRAM_DIN(old_ddr_din),
	.DDRAM_BE(old_ddr_be),
	.DDRAM_WE(old_ddr_we),
	.reset(RESET),
	.addr(addr),
	.dout(),
	.din(0),
	.we(we),
	.rd(0),
	.ready()
);

// Native video DDR3 signals
wire  [7:0] nv_ddr_burstcnt;
wire [28:0] nv_ddr_addr;
wire        nv_ddr_rd;

// Priority mux: native video reader takes over after boot (cfg[15])
wire use_nv = NATIVE_VID & cfg[15];

assign DDRAM_BURSTCNT = use_nv ? nv_ddr_burstcnt : old_ddr_burstcnt;
assign DDRAM_ADDR     = use_nv ? nv_ddr_addr     : old_ddr_addr;
assign DDRAM_RD       = use_nv ? nv_ddr_rd       : old_ddr_rd;
assign DDRAM_DIN      = use_nv ? 64'd0           : old_ddr_din;
assign DDRAM_BE       = use_nv ? 8'hFF           : old_ddr_be;
assign DDRAM_WE       = use_nv ? 1'b0            : old_ddr_we;

reg        we;
reg [28:0] addr = 0;

always @(posedge clk_sys) begin
	reg [4:0] cnt = 9;

	if(~RESET & cfg[15]) begin
		cnt <= cnt + 1'b1;
		we <= &cnt;
		if(cnt == 8) addr <= addr + 1'd1;
	end
end

////////////////////////////  MT32pi  ////////////////////////////////// 

//
// Pin | USB Name | Signal
// ----+----------+--------------
// 0   | D+       | I/O I2C_SDA / RX (midi in)
// 1   | D-       | O   TX (midi out)
// 2   | TX-      | I   I2S_WS (1 == right)
// 3   | GND_d    | I   I2C_SCL
// 4   | RX+      | I   I2S_BCLK
// 5   | RX-      | I   I2S_DAT
// 6   | TX+      | -   none
//

reg [15:0] mt32_i2s_r, mt32_i2s_l;
wire midi_rx;

assign AUDIO_L = mt32_i2s_l;
assign AUDIO_R = mt32_i2s_r;
assign AUDIO_S = 1;

assign USER_OUT[0]   = 1;
assign USER_OUT[1]   = UART_RXD;
assign USER_OUT[6:2] = '1;
assign UART_TXD      = midi_rx;


//
// crossed/straight cable selection
//

generate
genvar i;
for(i = 0; i<2; i++) begin : clk_rate
	wire clk_in = i ? USER_IN[6] : USER_IN[4];
	reg [4:0] cnt;
	always @(posedge CLK_AUDIO) begin : clkr
		reg       clk_sr, clk, old_clk;
		reg [4:0] cnt_tmp;

		clk_sr <= clk_in;
		if (clk_sr == clk_in) clk <= clk_sr;

		if(~&cnt_tmp) cnt_tmp <= cnt_tmp + 1'd1;
		else cnt <= '1;

		old_clk <= clk;
		if(~old_clk & clk) begin
			cnt <= cnt_tmp;
			cnt_tmp <= 0;
		end
	end
end

reg crossed;
always @(posedge CLK_AUDIO) crossed <= (clk_rate[0].cnt <= clk_rate[1].cnt);
endgenerate

wire   i2s_ws   = crossed ? USER_IN[2] : USER_IN[5];
wire   i2s_data = crossed ? USER_IN[5] : USER_IN[2];
wire   i2s_bclk = crossed ? USER_IN[4] : USER_IN[6];
assign midi_rx  = crossed ? USER_IN[6] : USER_IN[4];

always @(posedge CLK_AUDIO) begin : i2s_proc
	reg [15:0] i2s_buf = 0;
	reg  [4:0] i2s_cnt = 0;
	reg        clk_sr;
	reg        i2s_clk = 0;
	reg        old_clk, old_ws;
	reg        i2s_next = 0;

	// Debounce clock
	clk_sr <= i2s_bclk;
	if (clk_sr == i2s_bclk) i2s_clk <= clk_sr;

	// Latch data and ws on rising edge
	old_clk <= i2s_clk;
	if (i2s_clk && ~old_clk) begin

		if (~i2s_cnt[4]) begin
			i2s_cnt <= i2s_cnt + 1'd1;
			i2s_buf[~i2s_cnt[3:0]] <= i2s_data;
		end

		// Word Select will change 1 clock before the new word starts
		old_ws <= i2s_ws;
		if (old_ws != i2s_ws) i2s_next <= 1;
	end

	if (i2s_next) begin
		i2s_next <= 0;
		i2s_cnt <= 0;
		i2s_buf <= 0;

		if (i2s_ws) mt32_i2s_l <= i2s_buf;
		else        mt32_i2s_r <= i2s_buf;
	end
	
	if (RESET) begin
		i2s_buf    <= 0;
		mt32_i2s_l <= 0;
		mt32_i2s_r <= 0;
	end
end

/////////////////////   VIDEO   ///////////////////

localparam lfsr_n = 63;

wire PAL = status[4];
wire FB  = status[5];
wire [2:0] led = status[8:6];

reg   [9:0] hc;
reg   [9:0] vc;
reg   [9:0] vvc;

reg  [lfsr_n:0] rnd_reg;
wire [lfsr_n:0] rnd;

wire  [5:0] rnd_c = {rnd_reg[0],rnd_reg[1],rnd_reg[2],rnd_reg[2],rnd_reg[2],rnd_reg[2]};

lfsr #(lfsr_n) random(rnd);

always @(posedge CLK_VIDEO) begin
	ce_pix <= ce_pix_div4;

	if(ce_pix) begin
		if(hc == 500) begin
			hc <= 0;
			if(vc == (PAL ? (forced_scandoubler ? 623 : 311) : (forced_scandoubler ? 523 : 261))) begin
				vc <= 0;
				vvc <= vvc + 9'd6;
			end else begin
				vc <= vc + 1'd1;
			end
		end else begin
			hc <= hc + 1'd1;
		end

		rnd_reg <= rnd;
	end
end

reg HBlank;
reg HSync;
reg VBlank;
reg VSync;

reg ce_pix;
always @(posedge CLK_VIDEO) begin
	if (hc == 384) HBlank <= 1;
		else if (hc == 0) HBlank <= 0;

	if (hc == 412) begin
		HSync <= 1;

		if(PAL) begin
			if(vc == (forced_scandoubler ? 609 : 280)) VSync <= 1;
				else if (vc == (forced_scandoubler ? 617 : 283)) VSync <= 0;

			if(vc == (forced_scandoubler ? 601 : 270)) VBlank <= 1;
				else if (vc == 0) VBlank <= 0;
		end
		else begin
			if(vc == (forced_scandoubler ? 490 : 224)) VSync <= 1;
				else if (vc == (forced_scandoubler ? 496 : 227)) VSync <= 0;

			if(vc == (forced_scandoubler ? 480 : 224)) VBlank <= 1;
				else if (vc == 0) VBlank <= 0;
		end
	end

	if (hc == 450) HSync <= 0;
end

reg  [7:0] cos_out;
wire [5:0] cos_g = cos_out[7:3]+6'd32;
cos cos(vvc + {vc>>forced_scandoubler, 2'b00}, cos_out);

wire [7:0] comp_v = (cos_g >= rnd_c) ? {cos_g - rnd_c, 2'b00} : 8'd0;

// --- Native video module ---
wire [7:0] nv_r, nv_g, nv_b;
wire       nv_hs, nv_vs, nv_de;
wire       nv_hblank, nv_vblank;
wire       nv_active;

native_video_top native_video
(
	.clk_sys        (clk_sys),
	.clk_vid        (CLK_VIDEO),
	.ce_pix         (ce_pix_div4),
	.reset          (RESET),

	// OSD position offsets (two's complement from status bits)
	.h_offset       ($signed(status[28:25])),
	.v_offset       ($signed(status[46:43])),

	// DDR3 interface (directly to mux)
	.ddr_busy       (DDRAM_BUSY),
	.ddr_burstcnt   (nv_ddr_burstcnt),
	.ddr_addr       (nv_ddr_addr),
	.ddr_dout       (DDRAM_DOUT),
	.ddr_dout_ready (DDRAM_DOUT_READY & use_nv),
	.ddr_rd         (nv_ddr_rd),
	.ddr_din        (),
	.ddr_be         (),
	.ddr_we         (),

	// Video output
	.vga_r          (nv_r),
	.vga_g          (nv_g),
	.vga_b          (nv_b),
	.vga_hs         (nv_hs),
	.vga_vs         (nv_vs),
	.vga_de         (nv_de),
	.vga_hblank     (nv_hblank),
	.vga_vblank     (nv_vblank),

	// Control
	.enable         (use_nv),
	.active         (nv_active),
	.vsync_out      ()
);

// --- Horizontal size adjustment (line-buffer scaler, adapted from jtframe_hsize) ---
wire [7:0] hs_nv_r, hs_nv_g, hs_nv_b;
wire       hs_nv_hs, hs_nv_vs, hs_nv_hb, hs_nv_vb;

hsize #(.COLORW(8)) hsize_inst (
	.clk      (CLK_VIDEO),
	.pxl_cen  (ce_pix_div4),
	.pxl2_cen (ce_pix2x),
	.scale    (hsize_scale),
	.enable   (hsize_enable),
	.r_in     (nv_r),  .g_in  (nv_g),  .b_in  (nv_b),
	.HS_in    (nv_hs), .VS_in (nv_vs),
	.HB_in    (nv_hblank), .VB_in (nv_vblank),
	.HS_out   (hs_nv_hs), .VS_out(hs_nv_vs),
	.HB_out   (hs_nv_hb), .VB_out(hs_nv_vb),
	.r_out    (hs_nv_r), .g_out(hs_nv_g), .b_out(hs_nv_b)
);

// Mux VGA outputs: native video path vs. existing menu pattern
// When NATIVE_VID_ACTIVE, output native video timing (hs/vs/de) so the CRT
// can lock onto valid sync immediately. Pixel data comes from nv_active
// (frame_ready); until then, output black.
// Raw DE before crop — this feeds into video_freak
wire hs_de = ~(hs_nv_hb | hs_nv_vb);
wire vga_de_raw = NATIVE_VID_ACTIVE ? hs_de : ~(HBlank | VBlank);
assign VGA_HS  = NATIVE_VID_ACTIVE ? hs_nv_hs : HSync;
assign VGA_VS  = NATIVE_VID_ACTIVE ? hs_nv_vs : VSync;
assign VGA_R   = nv_active ? hs_nv_r  : (NATIVE_VID_ACTIVE ? 8'd0 : comp_v);
assign VGA_G   = nv_active ? hs_nv_g  : (NATIVE_VID_ACTIVE ? 8'd0 : comp_v);
assign VGA_B   = nv_active ? hs_nv_b  : (NATIVE_VID_ACTIVE ? 8'd0 : comp_v);

endmodule
