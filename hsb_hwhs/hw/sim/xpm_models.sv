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
  // storage in units as wide as the narrower port; a port's word spans UNITS_PER_WORD_* units
  localparam integer UNIT_WIDTH = (WRITE_DATA_WIDTH_A < WRITE_DATA_WIDTH_B) ? WRITE_DATA_WIDTH_A
                                                                             : WRITE_DATA_WIDTH_B;
  localparam integer UNITS            = MEMORY_SIZE / UNIT_WIDTH;
  localparam integer UNITS_PER_WORD_A = WRITE_DATA_WIDTH_A / UNIT_WIDTH;
  localparam integer UNITS_PER_WORD_B = WRITE_DATA_WIDTH_B / UNIT_WIDTH;

  logic [UNIT_WIDTH-1:0] mem [0:UNITS-1];
  initial for (int i = 0; i < UNITS; i++) mem[i] = '0;

  // read pipelines; the last stage drives the output
  logic [WRITE_DATA_WIDTH_A-1:0] read_pipe_a [0:READ_LATENCY_A-1];
  logic [WRITE_DATA_WIDTH_B-1:0] read_pipe_b [0:READ_LATENCY_B-1];

  always_ff @(posedge clka) begin
    if (ena) begin
      for (int unit = 0; unit < UNITS_PER_WORD_A; unit++)
        for (int i = 0; i < UNIT_WIDTH; i++)
          if (wea[(unit * UNIT_WIDTH + i) / BYTE_WRITE_WIDTH_A])
            mem[addra * UNITS_PER_WORD_A + unit][i] <= dina[unit * UNIT_WIDTH + i];
      for (int unit = 0; unit < UNITS_PER_WORD_A; unit++)
        read_pipe_a[0][unit * UNIT_WIDTH +: UNIT_WIDTH] <= mem[addra * UNITS_PER_WORD_A + unit];
    end
    for (int stage = 1; stage < READ_LATENCY_A; stage++)
      if (regcea || stage != READ_LATENCY_A - 1) read_pipe_a[stage] <= read_pipe_a[stage - 1];
    if (rsta) read_pipe_a[READ_LATENCY_A - 1] <= '0;
  end

  always_ff @(posedge clka) begin   // common clock
    if (enb) begin
      for (int unit = 0; unit < UNITS_PER_WORD_B; unit++)
        for (int i = 0; i < UNIT_WIDTH; i++)
          if (web[(unit * UNIT_WIDTH + i) / BYTE_WRITE_WIDTH_B])
            mem[addrb * UNITS_PER_WORD_B + unit][i] <= dinb[unit * UNIT_WIDTH + i];
      for (int unit = 0; unit < UNITS_PER_WORD_B; unit++)
        read_pipe_b[0][unit * UNIT_WIDTH +: UNIT_WIDTH] <= mem[addrb * UNITS_PER_WORD_B + unit];
    end
    for (int stage = 1; stage < READ_LATENCY_B; stage++)
      if (regceb || stage != READ_LATENCY_B - 1) read_pipe_b[stage] <= read_pipe_b[stage - 1];
    if (rstb) read_pipe_b[READ_LATENCY_B - 1] <= '0;
  end

  assign douta = read_pipe_a[READ_LATENCY_A - 1];
  assign doutb = read_pipe_b[READ_LATENCY_B - 1];
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
  localparam integer WORDS = MEMORY_SIZE / WRITE_DATA_WIDTH_A;

  logic [WRITE_DATA_WIDTH_A-1:0] mem [0:WORDS-1];
  initial for (int i = 0; i < WORDS; i++) mem[i] = '0;

  // read pipeline; the last stage drives the output
  logic [WRITE_DATA_WIDTH_A-1:0] read_pipe_b [0:READ_LATENCY_B-1];

  always_ff @(posedge clka) begin   // common clock
    if (ena)
      for (int i = 0; i < WRITE_DATA_WIDTH_A; i++)
        if (wea[i / BYTE_WRITE_WIDTH_A]) mem[addra][i] <= dina[i];
    if (enb) read_pipe_b[0] <= mem[addrb];
    for (int stage = 1; stage < READ_LATENCY_B; stage++)
      if (regceb || stage != READ_LATENCY_B - 1) read_pipe_b[stage] <= read_pipe_b[stage - 1];
    if (rstb) read_pipe_b[READ_LATENCY_B - 1] <= '0;
  end

  assign doutb = read_pipe_b[READ_LATENCY_B - 1];
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
  logic [DEST_SYNC_FF-1:0] sync = '0;
  always_ff @(posedge dest_clk) sync <= {sync[DEST_SYNC_FF-2:0], src_in};
  assign dest_out = sync[DEST_SYNC_FF-1];
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
  logic [WIDTH-1:0] sync [DEST_SYNC_FF];
  initial for (int i = 0; i < DEST_SYNC_FF; i++) sync[i] = '0;
  always_ff @(posedge dest_clk) begin
    sync[0] <= src_in;
    for (int i = 1; i < DEST_SYNC_FF; i++) sync[i] <= sync[i - 1];
  end
  assign dest_out = sync[DEST_SYNC_FF-1];
endmodule
