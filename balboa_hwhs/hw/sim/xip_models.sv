// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Xanadu Quantum Technologies Inc.
//
// xip_models.sv - simulation models of the Vivado IP the vendored BALBOA RTL instantiates by
// module name (hw/scripts/balboa_ips.tcl creates the real ones with these names). Simulation
// only; Vivado never reads this file.
//
//   axis_data_fifo_*         first-word-fall-through FIFO of the configured depth
//   axis_register_slice_*    full register slice: one cycle of latency, no bubbles
//
// The real FIFOs take a few cycles more from input to output; nothing here depends on that.

`timescale 1ps / 1ps

module xip_fifo #(
  parameter int DW    = 8,
  parameter int DEPTH = 32
) (
  input  logic          clk,
  input  logic          rstn,
  input  logic          s_valid,
  output logic          s_ready,
  input  logic [DW-1:0] s_data,
  output logic          m_valid,
  input  logic          m_ready,
  output logic [DW-1:0] m_data
);
  localparam int AW = $clog2(DEPTH);
  logic [DW-1:0] mem [DEPTH];
  logic [AW:0]   wp, rp;
  wire           full  = (wp[AW] != rp[AW]) && (wp[AW-1:0] == rp[AW-1:0]);
  wire           empty = (wp == rp);

  assign s_ready = rstn && !full;
  assign m_valid = !empty;
  assign m_data  = mem[rp[AW-1:0]];

  always_ff @(posedge clk) begin
    if (!rstn) begin
      wp <= '0;
      rp <= '0;
    end else begin
      if (s_valid && s_ready) begin
        mem[wp[AW-1:0]] <= s_data;
        wp <= wp + 1'b1;
      end
      if (m_valid && m_ready) rp <= rp + 1'b1;
    end
  end
endmodule

module xip_slice #(
  parameter int DW = 8
) (
  input  logic          clk,
  input  logic          rstn,
  input  logic          s_valid,
  output logic          s_ready,
  input  logic [DW-1:0] s_data,
  output logic          m_valid,
  input  logic          m_ready,
  output logic [DW-1:0] m_data
);
  xip_fifo #(.DW(DW), .DEPTH(2)) u_q (.*);
endmodule

// ---- FIFOs ---------------------------------------------------------------------------------

`define XIP_META_FIFO(NAME, W_, D_)                                                       \
module NAME (                                                                                 \
  input  logic          s_axis_aresetn,                                                       \
  input  logic          s_axis_aclk,                                                          \
  input  logic          s_axis_tvalid,                                                        \
  output logic          s_axis_tready,                                                        \
  input  logic [W_-1:0] s_axis_tdata,                                                         \
  output logic          m_axis_tvalid,                                                        \
  input  logic          m_axis_tready,                                                        \
  output logic [W_-1:0] m_axis_tdata                                                          \
);                                                                                            \
  xip_fifo #(.DW(W_), .DEPTH(D_)) u_q (                                                    \
    .clk(s_axis_aclk), .rstn(s_axis_aresetn),                                                 \
    .s_valid(s_axis_tvalid), .s_ready(s_axis_tready), .s_data(s_axis_tdata),                  \
    .m_valid(m_axis_tvalid), .m_ready(m_axis_tready), .m_data(m_axis_tdata));                 \
endmodule

`define XIP_DATA_FIFO(NAME, D_)                                                           \
module NAME (                                                                                 \
  input  logic         s_axis_aresetn,                                                        \
  input  logic         s_axis_aclk,                                                           \
  input  logic         s_axis_tvalid,                                                         \
  output logic         s_axis_tready,                                                         \
  input  logic [511:0] s_axis_tdata,                                                          \
  input  logic [63:0]  s_axis_tkeep,                                                          \
  input  logic         s_axis_tlast,                                                          \
  output logic         m_axis_tvalid,                                                         \
  input  logic         m_axis_tready,                                                         \
  output logic [511:0] m_axis_tdata,                                                          \
  output logic [63:0]  m_axis_tkeep,                                                          \
  output logic         m_axis_tlast                                                           \
);                                                                                            \
  xip_fifo #(.DW(577), .DEPTH(D_)) u_q (                                                   \
    .clk(s_axis_aclk), .rstn(s_axis_aresetn),                                                 \
    .s_valid(s_axis_tvalid), .s_ready(s_axis_tready),                                         \
    .s_data({s_axis_tlast, s_axis_tkeep, s_axis_tdata}),                                      \
    .m_valid(m_axis_tvalid), .m_ready(m_axis_tready),                                         \
    .m_data({m_axis_tlast, m_axis_tkeep, m_axis_tdata}));                                     \
endmodule

`XIP_META_FIFO(axis_data_fifo_meta_96, 96, 32)
`XIP_META_FIFO(axis_data_fifo_meta_128, 128, 32)
`XIP_DATA_FIFO(axis_data_fifo_512, 512)
`XIP_DATA_FIFO(axis_data_fifo_512_cc_tx, 16)

// ---- register slices -----------------------------------------------------------------------

`define XIP_META_SLICE(NAME, W_)                                                             \
module NAME (                                                                                 \
  input  logic          aclk,                                                                 \
  input  logic          aresetn,                                                              \
  input  logic          s_axis_tvalid,                                                        \
  output logic          s_axis_tready,                                                        \
  input  logic [W_-1:0] s_axis_tdata,                                                         \
  output logic          m_axis_tvalid,                                                        \
  input  logic          m_axis_tready,                                                        \
  output logic [W_-1:0] m_axis_tdata                                                          \
);                                                                                            \
  xip_slice #(.DW(W_)) u_s (                                                                  \
    .clk(aclk), .rstn(aresetn),                                                               \
    .s_valid(s_axis_tvalid), .s_ready(s_axis_tready), .s_data(s_axis_tdata),                  \
    .m_valid(m_axis_tvalid), .m_ready(m_axis_tready), .m_data(m_axis_tdata));                 \
endmodule

`XIP_META_SLICE(axis_register_slice_meta_32, 32)
`XIP_META_SLICE(axis_register_slice_meta_56, 56)

module axis_register_slice_512 (
  input  logic         aclk,
  input  logic         aresetn,
  input  logic         s_axis_tvalid,
  output logic         s_axis_tready,
  input  logic [511:0] s_axis_tdata,
  input  logic [63:0]  s_axis_tkeep,
  input  logic         s_axis_tlast,
  output logic         m_axis_tvalid,
  input  logic         m_axis_tready,
  output logic [511:0] m_axis_tdata,
  output logic [63:0]  m_axis_tkeep,
  output logic         m_axis_tlast
);
  xip_slice #(.DW(577)) u_s (
    .clk(aclk), .rstn(aresetn),
    .s_valid(s_axis_tvalid), .s_ready(s_axis_tready), .s_data({s_axis_tlast, s_axis_tkeep, s_axis_tdata}),
    .m_valid(m_axis_tvalid), .m_ready(m_axis_tready), .m_data({m_axis_tlast, m_axis_tkeep, m_axis_tdata}));
endmodule
