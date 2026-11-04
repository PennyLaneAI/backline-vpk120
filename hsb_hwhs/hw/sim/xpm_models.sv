// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Xanadu Quantum Technologies Inc.
//
// xpm_models.sv - simulation models of the XPM primitives used in hw/rtl.

`timescale 1ps / 1ps

module xpm_memory_tdpram #(
  parameter integer MEMORY_SIZE        = 2048,
  parameter         MEMORY_PRIMITIVE   = "auto",
  parameter         CLOCKING_MODE      = "common_clock",
  parameter         MEMORY_INIT_FILE   = "none",
  parameter         MEMORY_INIT_PARAM  = "",
  parameter integer USE_MEM_INIT       = 0,
  parameter         WAKEUP_TIME        = "disable_sleep",
  parameter integer MESSAGE_CONTROL    = 0,
  parameter         ECC_MODE           = "no_ecc",
  parameter integer AUTO_SLEEP_TIME    = 0,
  parameter integer WRITE_DATA_WIDTH_A = 32,
  parameter integer READ_DATA_WIDTH_A  = 32,
  parameter integer BYTE_WRITE_WIDTH_A = 32,
  parameter integer ADDR_WIDTH_A       = 6,
  parameter         READ_RESET_VALUE_A = "0",
  parameter integer READ_LATENCY_A     = 2,
  parameter         WRITE_MODE_A       = "no_change",
  parameter integer WRITE_DATA_WIDTH_B = 32,
  parameter integer READ_DATA_WIDTH_B  = 32,
  parameter integer BYTE_WRITE_WIDTH_B = 32,
  parameter integer ADDR_WIDTH_B       = 6,
  parameter         READ_RESET_VALUE_B = "0",
  parameter integer READ_LATENCY_B     = 2,
  parameter         WRITE_MODE_B       = "no_change"
) (
  input  wire                                              sleep,
  input  wire                                              clka,
  input  wire                                              rsta,
  input  wire                                              ena,
  input  wire                                              regcea,
  input  wire [WRITE_DATA_WIDTH_A/BYTE_WRITE_WIDTH_A-1:0]  wea,
  input  wire [ADDR_WIDTH_A-1:0]                           addra,
  input  wire [WRITE_DATA_WIDTH_A-1:0]                     dina,
  input  wire                                              injectsbiterra,
  input  wire                                              injectdbiterra,
  output wire [READ_DATA_WIDTH_A-1:0]                      douta,
  output wire                                              sbiterra,
  output wire                                              dbiterra,
  input  wire                                              clkb,
  input  wire                                              rstb,
  input  wire                                              enb,
  input  wire                                              regceb,
  input  wire [WRITE_DATA_WIDTH_B/BYTE_WRITE_WIDTH_B-1:0]  web,
  input  wire [ADDR_WIDTH_B-1:0]                           addrb,
  input  wire [WRITE_DATA_WIDTH_B-1:0]                     dinb,
  input  wire                                              injectsbiterrb,
  input  wire                                              injectdbiterrb,
  output wire [READ_DATA_WIDTH_B-1:0]                      doutb,
  output wire                                              sbiterrb,
  output wire                                              dbiterrb
);
  // storage in units of the narrower port
  localparam integer WA = WRITE_DATA_WIDTH_A, WB = WRITE_DATA_WIDTH_B;
  localparam integer UW = (WA < WB) ? WA : WB;
  localparam integer DEPTH = MEMORY_SIZE / UW;
  localparam integer RA = WA / UW, RB = WB / UW;      // units per word
  localparam integer BA = BYTE_WRITE_WIDTH_A, BB = BYTE_WRITE_WIDTH_B;

  logic [UW-1:0] mem [0:DEPTH-1];
  initial for (int i = 0; i < DEPTH; i++) mem[i] = '0;

  logic [WA-1:0] qa [0:READ_LATENCY_A-1];
  logic [WB-1:0] qb [0:READ_LATENCY_B-1];

  always_ff @(posedge clka) begin
    if (ena) begin
      for (int u = 0; u < RA; u++)
        for (int b = 0; b < UW; b++)
          if (wea[(u * UW + b) / BA]) mem[addra * RA + u][b] <= dina[u * UW + b];
      for (int u = 0; u < RA; u++) qa[0][u * UW +: UW] <= mem[addra * RA + u];
    end
    for (int s = 1; s < READ_LATENCY_A; s++) if (regcea || s != READ_LATENCY_A - 1) qa[s] <= qa[s - 1];
    if (rsta) qa[READ_LATENCY_A - 1] <= '0;
  end

  always_ff @(posedge clka) begin   // common clock
    if (enb) begin
      for (int u = 0; u < RB; u++)
        for (int b = 0; b < UW; b++)
          if (web[(u * UW + b) / BB]) mem[addrb * RB + u][b] <= dinb[u * UW + b];
      for (int u = 0; u < RB; u++) qb[0][u * UW +: UW] <= mem[addrb * RB + u];
    end
    for (int s = 1; s < READ_LATENCY_B; s++) if (regceb || s != READ_LATENCY_B - 1) qb[s] <= qb[s - 1];
    if (rstb) qb[READ_LATENCY_B - 1] <= '0;
  end

  assign douta = qa[READ_LATENCY_A - 1];
  assign doutb = qb[READ_LATENCY_B - 1];
  assign sbiterra = 1'b0;
  assign dbiterra = 1'b0;
  assign sbiterrb = 1'b0;
  assign dbiterrb = 1'b0;
endmodule

module xpm_memory_sdpram #(
  parameter integer MEMORY_SIZE        = 2048,
  parameter         MEMORY_PRIMITIVE   = "auto",
  parameter         CLOCKING_MODE      = "common_clock",
  parameter         MEMORY_INIT_FILE   = "none",
  parameter         MEMORY_INIT_PARAM  = "",
  parameter integer USE_MEM_INIT       = 0,
  parameter         WAKEUP_TIME        = "disable_sleep",
  parameter integer MESSAGE_CONTROL    = 0,
  parameter         ECC_MODE           = "no_ecc",
  parameter integer AUTO_SLEEP_TIME    = 0,
  parameter integer WRITE_DATA_WIDTH_A = 32,
  parameter integer BYTE_WRITE_WIDTH_A = 32,
  parameter integer ADDR_WIDTH_A       = 6,
  parameter integer READ_DATA_WIDTH_B  = 32,
  parameter integer ADDR_WIDTH_B       = 6,
  parameter         READ_RESET_VALUE_B = "0",
  parameter integer READ_LATENCY_B     = 2,
  parameter         WRITE_MODE_B       = "no_change"
) (
  input  wire                                              sleep,
  input  wire                                              clka,
  input  wire                                              ena,
  input  wire [WRITE_DATA_WIDTH_A/BYTE_WRITE_WIDTH_A-1:0]  wea,
  input  wire [ADDR_WIDTH_A-1:0]                           addra,
  input  wire [WRITE_DATA_WIDTH_A-1:0]                     dina,
  input  wire                                              injectsbiterra,
  input  wire                                              injectdbiterra,
  input  wire                                              clkb,
  input  wire                                              rstb,
  input  wire                                              enb,
  input  wire                                              regceb,
  input  wire [ADDR_WIDTH_B-1:0]                           addrb,
  output wire [READ_DATA_WIDTH_B-1:0]                      doutb,
  output wire                                              sbiterrb,
  output wire                                              dbiterrb
);
  localparam integer W = WRITE_DATA_WIDTH_A, B = BYTE_WRITE_WIDTH_A;

  logic [W-1:0] mem [0:MEMORY_SIZE/W-1];
  initial for (int i = 0; i < MEMORY_SIZE / W; i++) mem[i] = '0;

  logic [W-1:0] qb [0:READ_LATENCY_B-1];

  always_ff @(posedge clka) begin   // common clock
    if (ena)
      for (int b = 0; b < W; b++)
        if (wea[b / B]) mem[addra][b] <= dina[b];
    if (enb) qb[0] <= mem[addrb];
    for (int s = 1; s < READ_LATENCY_B; s++) if (regceb || s != READ_LATENCY_B - 1) qb[s] <= qb[s - 1];
    if (rstb) qb[READ_LATENCY_B - 1] <= '0;
  end

  assign doutb = qb[READ_LATENCY_B - 1];
  assign sbiterrb = 1'b0;
  assign dbiterrb = 1'b0;
endmodule

module xpm_cdc_single #(
  parameter integer DEST_SYNC_FF   = 4,
  parameter integer INIT_SYNC_FF   = 0,
  parameter integer SIM_ASSERT_CHK = 0,
  parameter integer SRC_INPUT_REG  = 1
) (
  input  wire src_clk,
  input  wire src_in,
  input  wire dest_clk,
  output wire dest_out
);
  logic [DEST_SYNC_FF-1:0] s = '0;
  always_ff @(posedge dest_clk) s <= {s[DEST_SYNC_FF-2:0], src_in};
  assign dest_out = s[DEST_SYNC_FF-1];
endmodule

module xpm_cdc_array_single #(
  parameter integer DEST_SYNC_FF   = 4,
  parameter integer INIT_SYNC_FF   = 0,
  parameter integer SIM_ASSERT_CHK = 0,
  parameter integer SRC_INPUT_REG  = 1,
  parameter integer WIDTH          = 2
) (
  input  wire             src_clk,
  input  wire [WIDTH-1:0] src_in,
  input  wire             dest_clk,
  output wire [WIDTH-1:0] dest_out
);
  logic [WIDTH-1:0] s [DEST_SYNC_FF];
  initial for (int i = 0; i < DEST_SYNC_FF; i++) s[i] = '0;
  always_ff @(posedge dest_clk) begin
    s[0] <= src_in;
    for (int i = 1; i < DEST_SYNC_FF; i++) s[i] <= s[i - 1];
  end
  assign dest_out = s[DEST_SYNC_FF-1];
endmodule
