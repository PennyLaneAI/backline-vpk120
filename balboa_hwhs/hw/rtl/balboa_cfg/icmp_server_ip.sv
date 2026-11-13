// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Xanadu Quantum Technologies Inc.
//===----------------------------------------------------------------------===//
//
// icmp_server_ip.sv - answers ICMP echo requests (ping) for network_stack.sv
//
// network_stack.sv instantiates fpga-network-stack's icmp_server HLS core (Xilinx, BSD-3)
// under this name. That core is not vendored; this module takes its place with the same
// ports. It answers echo requests only: the reply is the request with the IP
// addresses swapped, ICMP type 0 and the ICMP checksum adjusted for that (RFC 1624). The IP
// checksum does not change, since swapping the addresses keeps their sum. Everything else
// (other ICMP types, IP options, UDP and TTL inputs) is dropped.
//
// The stream is the IP packet, 8 bytes per beat, byte 0 in bits [7:0]:
//
//   beat 0  bytes  0..7   version/IHL ... flags/fragment
//   beat 1  bytes  8..15  TTL, protocol, checksum, source address
//   beat 2  bytes 16..23  destination address, ICMP type, code, checksum
//   beat 3+ the rest of the ICMP message, unchanged
//
// The first three beats are held until beat 2 shows the type, then go out rewritten.
//
//===----------------------------------------------------------------------===//
`timescale 1ns / 1ps

module icmp_server_ip (
  input  logic        ap_clk,
  input  logic        ap_rst_n,

  input  logic        s_axis_TVALID,
  output logic        s_axis_TREADY,
  input  logic [63:0] s_axis_TDATA,
  input  logic [7:0]  s_axis_TKEEP,
  input  logic        s_axis_TLAST,

  input  logic        udpIn_TVALID,
  output logic        udpIn_TREADY,
  input  logic [63:0] udpIn_TDATA,
  input  logic [7:0]  udpIn_TKEEP,
  input  logic        udpIn_TLAST,

  input  logic        ttlIn_TVALID,
  output logic        ttlIn_TREADY,
  input  logic [63:0] ttlIn_TDATA,
  input  logic [7:0]  ttlIn_TKEEP,
  input  logic        ttlIn_TLAST,

  output logic        m_axis_TVALID,
  input  logic        m_axis_TREADY,
  output logic [63:0] m_axis_TDATA,
  output logic [7:0]  m_axis_TKEEP,
  output logic        m_axis_TLAST
);

  typedef enum logic [2:0] {S_H0, S_H1, S_H2, S_E0, S_E1, S_E2, S_FWD, S_DROP} state_t;
  state_t state;

  logic [63:0] b0, b1, b2;
  logic [7:0]  k2;
  logic        last2;            // the packet ended in beat 2

  assign udpIn_TREADY = 1'b1;    // network_stack ties these inputs off
  assign ttlIn_TREADY = 1'b1;

  wire s_fire = s_axis_TVALID && s_axis_TREADY;
  wire m_fire = m_axis_TVALID && m_axis_TREADY;

  // an echo request with a plain 20-byte IP header
  wire echo_req = (b0[7:0] == 8'h45) && (s_axis_TDATA[39:32] == 8'd8);

  // ICMP checksum for type 8 -> 0: one's-complement add of 0x0800 (bytes 22, 23 are big-endian)
  wire [15:0] cs_in  = {s_axis_TDATA[55:48], s_axis_TDATA[63:56]};
  wire [16:0] cs_sum = {1'b0, cs_in} + 17'h0800;
  wire [15:0] cs_out = cs_sum[15:0] + {15'd0, cs_sum[16]};

  always_ff @(posedge ap_clk) begin
    if (!ap_rst_n) begin
      state <= S_H0;
    end else begin
      case (state)
        S_H0: if (s_fire) begin
          b0    <= s_axis_TDATA;
          state <= s_axis_TLAST ? S_H0 : S_H1;
        end
        S_H1: if (s_fire) begin
          b1    <= s_axis_TDATA;
          state <= s_axis_TLAST ? S_H0 : S_H2;
        end
        S_H2: if (s_fire) begin
          if (echo_req) begin
            b1[63:32] <= s_axis_TDATA[31:0];                  // source <- destination
            b2        <= {cs_out[7:0], cs_out[15:8], s_axis_TDATA[47:40], 8'd0, b1[63:32]};
            k2        <= s_axis_TKEEP;
            last2     <= s_axis_TLAST;
            state     <= S_E0;
          end else begin
            state <= s_axis_TLAST ? S_H0 : S_DROP;
          end
        end
        S_E0:   if (m_fire) state <= S_E1;
        S_E1:   if (m_fire) state <= S_E2;
        S_E2:   if (m_fire) state <= last2 ? S_H0 : S_FWD;
        S_FWD:  if (s_fire && s_axis_TLAST) state <= S_H0;
        S_DROP: if (s_fire && s_axis_TLAST) state <= S_H0;
        default: state <= S_H0;
      endcase
    end
  end

  always_comb begin
    s_axis_TREADY = 1'b0;
    m_axis_TVALID = 1'b0;
    m_axis_TDATA  = s_axis_TDATA;
    m_axis_TKEEP  = s_axis_TKEEP;
    m_axis_TLAST  = s_axis_TLAST;
    case (state)
      S_H0, S_H1, S_H2, S_DROP: s_axis_TREADY = 1'b1;
      S_E0: begin
        m_axis_TVALID = 1'b1;
        m_axis_TDATA  = b0;
        m_axis_TKEEP  = 8'hFF;
        m_axis_TLAST  = 1'b0;
      end
      S_E1: begin
        m_axis_TVALID = 1'b1;
        m_axis_TDATA  = b1;
        m_axis_TKEEP  = 8'hFF;
        m_axis_TLAST  = 1'b0;
      end
      S_E2: begin
        m_axis_TVALID = 1'b1;
        m_axis_TDATA  = b2;
        m_axis_TKEEP  = k2;
        m_axis_TLAST  = last2;
      end
      S_FWD: begin
        m_axis_TVALID = s_axis_TVALID;
        s_axis_TREADY = m_axis_TREADY;
      end
      default: ;
    endcase
  end

endmodule
