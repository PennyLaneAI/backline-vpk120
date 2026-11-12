// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Xanadu Quantum Technologies Inc.
//
// axis_interconnect_512.sv - the two TX stream merges network_stack.sv instantiates:
// axis_interconnect_512_4to1 (ICMP, UDP, TCP, RoCE) and axis_interconnect_512_2to1 (ARP, IP).
//
// They take the place of Vivado AXI4-Stream Interconnect cores, which Vivado does not offer for
// Versal, with the same ports. A packet, once started, goes out whole; between packets the inputs
// take turns. The output is registered: one cycle from input to output, a beat every cycle. All
// ports run on ACLK; the per-port clocks and resets and ARB_REQ_SUPPRESS are not used.

`timescale 1ns / 1ps

module axis_pkt_merge #(
  parameter int N = 2
) (
  input  logic                clk,
  input  logic                rstn,

  input  logic [N-1:0]        s_tvalid,
  output logic [N-1:0]        s_tready,
  input  logic [N-1:0][511:0] s_tdata,
  input  logic [N-1:0][63:0]  s_tkeep,
  input  logic [N-1:0]        s_tlast,

  output logic                m_tvalid,
  input  logic                m_tready,
  output logic [511:0]        m_tdata,
  output logic [63:0]         m_tkeep,
  output logic                m_tlast
);

  localparam int W = $clog2(N);

  logic         busy;   // a packet is going through, from input sel
  logic [W-1:0] sel;    // also where the next turn starts from
  logic [W-1:0] pick;   // between packets: the first input with data after sel
  logic         any;
  logic [W-1:0] cur;

  always_comb begin
    pick = sel;
    any  = 1'b0;
    for (int i = 1; i <= N; i++) begin
      if (!any && s_tvalid[(int'(sel) + i) % N]) begin
        pick = W'((int'(sel) + i) % N);
        any  = 1'b1;
      end
    end
  end

  assign cur = busy ? sel : pick;

  wire grant = busy || any;
  wire room  = !m_tvalid || m_tready;
  wire take  = grant && room && s_tvalid[cur];

  always_comb begin
    s_tready      = '0;
    s_tready[cur] = grant && room;
  end

  always_ff @(posedge clk) begin
    if (!rstn) begin
      busy     <= 1'b0;
      sel      <= W'(N - 1);
      m_tvalid <= 1'b0;
    end else if (take) begin
      busy     <= !s_tlast[cur];
      sel      <= cur;
      m_tvalid <= 1'b1;
    end else if (m_tready) begin
      m_tvalid <= 1'b0;
    end
  end

  always_ff @(posedge clk) begin
    if (take) begin
      m_tdata <= s_tdata[cur];
      m_tkeep <= s_tkeep[cur];
      m_tlast <= s_tlast[cur];
    end
  end

endmodule

module axis_interconnect_512_4to1 (
  input  logic         ACLK,
  input  logic         ARESETN,

  input  logic         S00_AXIS_ACLK,
  input  logic         S01_AXIS_ACLK,
  input  logic         S02_AXIS_ACLK,
  input  logic         S03_AXIS_ACLK,
  input  logic         S00_AXIS_ARESETN,
  input  logic         S01_AXIS_ARESETN,
  input  logic         S02_AXIS_ARESETN,
  input  logic         S03_AXIS_ARESETN,

  input  logic         S00_AXIS_TVALID,
  output logic         S00_AXIS_TREADY,
  input  logic [511:0] S00_AXIS_TDATA,
  input  logic [63:0]  S00_AXIS_TKEEP,
  input  logic         S00_AXIS_TLAST,

  input  logic         S01_AXIS_TVALID,
  output logic         S01_AXIS_TREADY,
  input  logic [511:0] S01_AXIS_TDATA,
  input  logic [63:0]  S01_AXIS_TKEEP,
  input  logic         S01_AXIS_TLAST,

  input  logic         S02_AXIS_TVALID,
  output logic         S02_AXIS_TREADY,
  input  logic [511:0] S02_AXIS_TDATA,
  input  logic [63:0]  S02_AXIS_TKEEP,
  input  logic         S02_AXIS_TLAST,

  input  logic         S03_AXIS_TVALID,
  output logic         S03_AXIS_TREADY,
  input  logic [511:0] S03_AXIS_TDATA,
  input  logic [63:0]  S03_AXIS_TKEEP,
  input  logic         S03_AXIS_TLAST,

  input  logic         M00_AXIS_ACLK,
  input  logic         M00_AXIS_ARESETN,
  output logic         M00_AXIS_TVALID,
  input  logic         M00_AXIS_TREADY,
  output logic [511:0] M00_AXIS_TDATA,
  output logic [63:0]  M00_AXIS_TKEEP,
  output logic         M00_AXIS_TLAST,

  input  logic         S00_ARB_REQ_SUPPRESS,
  input  logic         S01_ARB_REQ_SUPPRESS,
  input  logic         S02_ARB_REQ_SUPPRESS,
  input  logic         S03_ARB_REQ_SUPPRESS
);

  axis_pkt_merge #(.N(4)) merge (
    .clk      (ACLK),
    .rstn     (ARESETN),
    .s_tvalid ({S03_AXIS_TVALID, S02_AXIS_TVALID, S01_AXIS_TVALID, S00_AXIS_TVALID}),
    .s_tready ({S03_AXIS_TREADY, S02_AXIS_TREADY, S01_AXIS_TREADY, S00_AXIS_TREADY}),
    .s_tdata  ({S03_AXIS_TDATA,  S02_AXIS_TDATA,  S01_AXIS_TDATA,  S00_AXIS_TDATA}),
    .s_tkeep  ({S03_AXIS_TKEEP,  S02_AXIS_TKEEP,  S01_AXIS_TKEEP,  S00_AXIS_TKEEP}),
    .s_tlast  ({S03_AXIS_TLAST,  S02_AXIS_TLAST,  S01_AXIS_TLAST,  S00_AXIS_TLAST}),
    .m_tvalid (M00_AXIS_TVALID),
    .m_tready (M00_AXIS_TREADY),
    .m_tdata  (M00_AXIS_TDATA),
    .m_tkeep  (M00_AXIS_TKEEP),
    .m_tlast  (M00_AXIS_TLAST)
  );

endmodule

module axis_interconnect_512_2to1 (
  input  logic         ACLK,
  input  logic         ARESETN,

  input  logic         S00_AXIS_ACLK,
  input  logic         S01_AXIS_ACLK,
  input  logic         S00_AXIS_ARESETN,
  input  logic         S01_AXIS_ARESETN,

  input  logic         S00_AXIS_TVALID,
  output logic         S00_AXIS_TREADY,
  input  logic [511:0] S00_AXIS_TDATA,
  input  logic [63:0]  S00_AXIS_TKEEP,
  input  logic         S00_AXIS_TLAST,

  input  logic         S01_AXIS_TVALID,
  output logic         S01_AXIS_TREADY,
  input  logic [511:0] S01_AXIS_TDATA,
  input  logic [63:0]  S01_AXIS_TKEEP,
  input  logic         S01_AXIS_TLAST,

  input  logic         M00_AXIS_ACLK,
  input  logic         M00_AXIS_ARESETN,
  output logic         M00_AXIS_TVALID,
  input  logic         M00_AXIS_TREADY,
  output logic [511:0] M00_AXIS_TDATA,
  output logic [63:0]  M00_AXIS_TKEEP,
  output logic         M00_AXIS_TLAST,

  input  logic         S00_ARB_REQ_SUPPRESS,
  input  logic         S01_ARB_REQ_SUPPRESS
);

  axis_pkt_merge #(.N(2)) merge (
    .clk      (ACLK),
    .rstn     (ARESETN),
    .s_tvalid ({S01_AXIS_TVALID, S00_AXIS_TVALID}),
    .s_tready ({S01_AXIS_TREADY, S00_AXIS_TREADY}),
    .s_tdata  ({S01_AXIS_TDATA,  S00_AXIS_TDATA}),
    .s_tkeep  ({S01_AXIS_TKEEP,  S00_AXIS_TKEEP}),
    .s_tlast  ({S01_AXIS_TLAST,  S00_AXIS_TLAST}),
    .m_tvalid (M00_AXIS_TVALID),
    .m_tready (M00_AXIS_TREADY),
    .m_tdata  (M00_AXIS_TDATA),
    .m_tkeep  (M00_AXIS_TKEEP),
    .m_tlast  (M00_AXIS_TLAST)
  );

endmodule
