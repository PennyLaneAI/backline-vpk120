// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Xanadu Quantum Technologies Inc.
//===----------------------------------------------------------------------===//
//
// eth_pad.v - pad Ethernet frames to the 60-byte minimum (the MAC appends the FCS)
//
// BALBOA's ARP replies and requests are 42 bytes. Coyote pads frames in front of its MAC
// (an HLS core); a frame that short is always a single 64 B beat here, so it is enough to
// widen TKEEP of a single-beat frame to 60 bytes and zero the bytes it adds. Everything
// else passes through unchanged, with no register in between.
//
//===----------------------------------------------------------------------===//
`timescale 1ps / 1ps
`default_nettype none

module eth_pad #(
    parameter integer DW = 512
) (
    input  wire            clk,
    input  wire            rstn,
    input  wire [DW-1:0]   s_tdata,
    input  wire [DW/8-1:0] s_tkeep,
    input  wire            s_tlast,
    input  wire            s_tvalid,
    output wire            s_tready,
    output wire [DW-1:0]   m_tdata,
    output wire [DW/8-1:0] m_tkeep,
    output wire            m_tlast,
    output wire            m_tvalid,
    input  wire            m_tready
);

    localparam integer MIN_BYTES = 60;

    reg in_frame;   // past the first beat of a frame
    always @(posedge clk) begin
        if (!rstn)                     in_frame <= 1'b0;
        else if (s_tvalid && s_tready) in_frame <= !s_tlast;
    end

    // TKEEP is contiguous from byte 0, so byte 59 tells whether the frame has 60 bytes
    wire short = !in_frame && s_tlast && !s_tkeep[MIN_BYTES-1];

    genvar b;
    generate
        for (b = 0; b < DW / 8; b = b + 1) begin : g_byte
            assign m_tdata[b*8+:8] = (short && !s_tkeep[b]) ? 8'd0 : s_tdata[b*8+:8];
            assign m_tkeep[b]      = s_tkeep[b] || (short && b < MIN_BYTES);
        end
    endgenerate

    assign m_tlast  = s_tlast;
    assign m_tvalid = s_tvalid;
    assign s_tready = m_tready;

endmodule

`default_nettype wire
