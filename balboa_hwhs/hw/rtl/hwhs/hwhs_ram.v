// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Xanadu Quantum Technologies Inc.
//===----------------------------------------------------------------------===//
//
// hwhs_ram.v - 64 B-per-entry true-dual-port RAM (slot RAM and reply RAM)
//
//   +----------------------------------------------------------------------+
//   | xpm_memory_tdpram - common clock (hif)                               |
//   |                                                                      |
//   | Port A (rw): PS -> axi_bram_ctrl (512b, READ_LATENCY 2) -> RAM       |
//   | Port B (rw): engine, one 512-bit entry per access                    |
//   +----------------------------------------------------------------------+
//
//===----------------------------------------------------------------------===//
`timescale 1ps / 1ps
`default_nettype none
(* DowngradeIPIdentifiedWarnings="yes" *)

module hwhs_ram #(
    parameter integer C_SLOT_AW   = 10,     // entries = 2^C_SLOT_AW (1024 x 64 B = 64 KB)
    parameter integer C_SLOT_DW   = 512,    // 64 B per entry
    parameter integer C_BRAM_DW   = 512,    // axi_bram_ctrl data width: one entry per PS beat
    parameter integer C_BRAM_AW   = 32,     // axi_bram_ctrl byte address
    parameter         C_PRIMITIVE = "auto"
) (
    input wire clk,

    // Port A: axi_bram_ctrl
    input  wire                     bram_rst,
    input  wire                     bram_en,
    input  wire [(C_BRAM_DW/8)-1:0] bram_we,
    input  wire [    C_BRAM_AW-1:0] bram_addr,
    input  wire [    C_BRAM_DW-1:0] bram_wrdata,
    output wire [    C_BRAM_DW-1:0] bram_rddata,

    // Port B: engine
    input  wire                 en_b,
    input  wire                 we_b,
    input  wire [C_SLOT_AW-1:0] addr_b,
    input  wire [C_SLOT_DW-1:0] din_b,
    output wire [C_SLOT_DW-1:0] dout_b
);

    localparam integer BYTE_LSB   = $clog2(C_BRAM_DW / 8);  // bram_addr bits inside an entry: 6
    localparam integer RD_LATENCY = 2;  // axi_bram_ctrl_slot/_reply READ_LATENCY and the engine's RD/RD2

    xpm_memory_tdpram #(
        .MEMORY_SIZE       (C_SLOT_DW * (1 << C_SLOT_AW)),
        .MEMORY_PRIMITIVE  (C_PRIMITIVE),
        .CLOCKING_MODE     ("common_clock"),
        .MEMORY_INIT_FILE  ("none"),
        .MEMORY_INIT_PARAM (""),
        .USE_MEM_INIT      (0),
        .WAKEUP_TIME       ("disable_sleep"),
        .MESSAGE_CONTROL   (0),
        .ECC_MODE          ("no_ecc"),
        .AUTO_SLEEP_TIME   (0),
        // Port A: PS
        .WRITE_DATA_WIDTH_A(C_BRAM_DW),
        .READ_DATA_WIDTH_A (C_BRAM_DW),
        .BYTE_WRITE_WIDTH_A(8),
        .ADDR_WIDTH_A      (C_SLOT_AW),
        .READ_RESET_VALUE_A("0"),
        .READ_LATENCY_A    (RD_LATENCY),
        .WRITE_MODE_A      ("no_change"),
        // Port B: engine, whole entries
        .WRITE_DATA_WIDTH_B(C_SLOT_DW),
        .READ_DATA_WIDTH_B (C_SLOT_DW),
        .BYTE_WRITE_WIDTH_B(C_SLOT_DW),
        .ADDR_WIDTH_B      (C_SLOT_AW),
        .READ_RESET_VALUE_B("0"),
        .READ_LATENCY_B    (RD_LATENCY),
        .WRITE_MODE_B      ("no_change")
    ) u_ram (
        .sleep         (1'b0),
        // Port A
        .clka          (clk),
        .rsta          (bram_rst),
        .ena           (bram_en),
        .regcea        (1'b1),
        .wea           (bram_we),
        .addra         (bram_addr[C_SLOT_AW+BYTE_LSB-1:BYTE_LSB]),
        .dina          (bram_wrdata),
        .injectsbiterra(1'b0),
        .injectdbiterra(1'b0),
        .douta         (bram_rddata),
        .sbiterra      (),
        .dbiterra      (),
        // Port B
        .clkb          (clk),
        .rstb          (1'b0),
        .enb           (en_b),
        .regceb        (1'b1),
        .web           (we_b),
        .addrb         (addr_b),
        .dinb          (din_b),
        .injectsbiterrb(1'b0),
        .injectdbiterrb(1'b0),
        .doutb         (dout_b),
        .sbiterrb      (),
        .dbiterrb      ()
    );

endmodule

`default_nettype wire
