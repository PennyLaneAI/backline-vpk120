// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Xanadu Quantum Technologies Inc.
//===----------------------------------------------------------------------===//
// hh_rtt_trace.v - the RTT trace RAM
//
// Each entry is a completed round rtt in clocks
// And it is saturated if it exceeds 16 bits
// It's supposed to be written by the engine, read by the PS after the run.
//
//   +----------------------------------------------------------------+
//   | xpm_memory_sdpram - common clock, 64-bit words of 4 entries    |
//   |                                                                |
//   | Port A (wr): engine -> one 16-bit lane of a word               |
//   | Port B (rd): 32-bit half of a word -> axi_bram_ctrl -> PS      |
//   +----------------------------------------------------------------+
//
//===----------------------------------------------------------------------===//

`timescale 1ps / 1ps
`default_nettype none
(* DowngradeIPIdentifiedWarnings="yes" *)

module hh_rtt_trace #(
    parameter integer C_TRACE_AW = 20,  // entries = 2^C_TRACE_AW (1M x 16b = 2 MB)
    parameter integer C_TRACE_DW = 16,  // rtt in cycles
    parameter integer C_BRAM_DW  = 32
) (
    input wire clk,

    // Port A: engine writes
    input wire                  trace_we,
    input wire [C_TRACE_AW-1:0] trace_addr,
    input wire [C_TRACE_DW-1:0] trace_din,

    // Port B: axi_bram_ctrl (same clock as clk)
    input wire                     bram_rst,
    input wire                     bram_clk,
    input wire                     bram_en,
    input wire [(C_BRAM_DW/8)-1:0] bram_we,

    // byte address: what axi_bram_ctrl drives
    input  wire [ C_TRACE_AW:0] bram_addr,
    input  wire [C_BRAM_DW-1:0] bram_wrdata,
    output reg  [C_BRAM_DW-1:0] bram_rddata
);
    localparam integer RAM_DW      = 2 * C_BRAM_DW;                    // 64
    localparam integer ENTRIES     = RAM_DW / C_TRACE_DW;              // 4 entries per RAM word
    localparam integer ENTRY_BYTES = C_TRACE_DW / 8;                   // 2 bytes per entry
    localparam integer ENTRY_SEL_W = $clog2(ENTRIES);                  // 2 bits for entry within a word
    localparam integer WORD_AW     = C_TRACE_AW - ENTRY_SEL_W;         // 18 bits for RAM word address
    localparam integer BYTE_AW     = C_TRACE_AW + $clog2(ENTRY_BYTES); // 21 bits for bram_addr width
    localparam integer HALF_BIT    = $clog2(C_BRAM_DW / 8);            // bram_addr[2] picks the upper/lower 32-bit half
    localparam integer RD_LATENCY  = 6;


    // Format: Engine writes 16-bit entry to the RAM word, and PS reads 32-bit half of the word
    //
    // 63        48 47       32 31       16 15        0
    // | entry 4w+3| entry 4w+2| entry 4w+1| entry 4w |
    // \__ upper 32 bit ______/ \__ lower 32 bit_____/


    // Port A: Engine writes
    // For timing at 250 MHz, we need to delay the write by one extra cycle.
    reg                  a_we;
    reg [C_TRACE_AW-1:0] a_addr;
    reg [C_TRACE_DW-1:0] a_din;
    always @(posedge clk) begin
        a_we   <= trace_we;
        a_addr <= trace_addr;
        a_din  <= trace_din;
    end

    // Get the byte write enable mask for the entry's own bytes.
    // If a_we is 1:
    // a_bwe =
    // 1111_1111 &
    // 0000_0011 << (addr[1:0] * 2)
    // ----------------------------
    // 0000_0011 << 0 = 0000_0011  (if addr[1:0] = 00)
    // 0000_0011 << 2 = 0000_1100  (if addr[1:0] = 01)
    // 0000_0011 << 4 = 0011_0000  (if addr[1:0] = 10)
    // 0000_0011 << 6 = 1100_0000  (if addr[1:0] = 11)
    wire [RAM_DW/8-1:0] a_bwe = {(RAM_DW/8){a_we}}
                              & ({{(RAM_DW/8-ENTRY_BYTES){1'b0}}, {ENTRY_BYTES{1'b1}}}
                                 << (a_addr[ENTRY_SEL_W-1:0] * ENTRY_BYTES));

    // Port B: PS reads
    reg                  b_en;
    reg [WORD_AW-1:0]    b_addr;
    reg [RD_LATENCY:0]   b_half;    // bram_addr[HALF_BIT], delayed alongside the read
    wire [RAM_DW-1:0]    b_word;
    always @(posedge bram_clk) begin
        b_en   <= bram_en;
        b_addr <= bram_addr[BYTE_AW-1:HALF_BIT+1];
        b_half <= {b_half[RD_LATENCY-1:0], bram_addr[HALF_BIT]};
        if (bram_rst) bram_rddata <= {C_BRAM_DW{1'b0}};
        else          bram_rddata <= b_half[RD_LATENCY] ? b_word[RAM_DW-1:C_BRAM_DW]
                                                        : b_word[C_BRAM_DW-1:0];
    end

    xpm_memory_sdpram #(
        .MEMORY_SIZE       (RAM_DW << WORD_AW),
        .MEMORY_PRIMITIVE  ("ultra"),
        .CLOCKING_MODE     ("common_clock"),
        .MEMORY_INIT_FILE  ("none"),
        .MEMORY_INIT_PARAM (""),
        .USE_MEM_INIT      (0),
        .WAKEUP_TIME       ("disable_sleep"),
        .MESSAGE_CONTROL   (0),
        .ECC_MODE          ("no_ecc"),
        .AUTO_SLEEP_TIME   (0),
        // Port A: engine write
        .WRITE_DATA_WIDTH_A(RAM_DW),
        .BYTE_WRITE_WIDTH_A(8),
        .ADDR_WIDTH_A      (WORD_AW),
        // Port B: PS read
        .READ_DATA_WIDTH_B (RAM_DW),
        .ADDR_WIDTH_B      (WORD_AW),
        .READ_RESET_VALUE_B("0"),
        .READ_LATENCY_B    (RD_LATENCY),
        .WRITE_MODE_B      ("read_first")
    ) u_trace_ram (
        .sleep         (1'b0),
        // Port A
        .clka          (clk),
        .ena           (a_we),
        .wea           (a_bwe),
        .addra         (a_addr[C_TRACE_AW-1:ENTRY_SEL_W]),
        .dina          ({ENTRIES{a_din}}),
        .injectsbiterra(1'b0),
        .injectdbiterra(1'b0),
        // Port B
        .clkb          (bram_clk),
        .rstb          (1'b0),
        .enb           (b_en),
        .regceb        (1'b1),
        .addrb         (b_addr),
        .doutb         (b_word),
        .sbiterrb      (),
        .dbiterrb      ()
    );

endmodule

`default_nettype wire
