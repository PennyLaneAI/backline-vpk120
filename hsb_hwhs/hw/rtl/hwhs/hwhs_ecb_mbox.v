// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Xanadu Quantum Technologies Inc.
//===----------------------------------------------------------------------===//
//
// hwhs_ecb_mbox.v - the door into HSB's register space
//
// HSB's registers are reached only through ECB packets (UDP 8192) on a host
// port.
//   - Port 0 goes to the MRMAC (100G to the coprocessor)
//   - Port 1 is wired here for ECB mailbox
//
//   TX: software writes one frame (<= 256 B) to the TX buffer, sets
//       ECB_TX_LEN, then ECB_CTRL.send; it enters port 1 as 64 B beats.
//   RX: holds HSB's ECB reply until software sets ECB_CTRL.release.

//   TX buffer 64 x 32 bit -- 64 B beats --> HSB i_hif_axis[1]
//   RX buffer 4 x 512 bit <-- 64 B beats -- HSB o_hif_axis[1]
//
//===----------------------------------------------------------------------===//
`timescale 1ps / 1ps
`default_nettype none

module hwhs_ecb_mbox (
    input wire clk,
    input wire rstn,

    // registers
    input  wire        tx_we,
    input  wire [ 5:0] tx_waddr,
    input  wire [31:0] tx_wdata,
    input  wire        send,
    input  wire        release_rx,
    input  wire [ 8:0] tx_len,      // bytes
    input  wire [15:0] ecb_port,
    input  wire [ 5:0] rx_raddr,
    output wire [31:0] rx_rdata,
    output reg         tx_busy,
    output reg         rx_valid,
    output reg         rx_trunc,
    output reg  [ 8:0] rx_len,
    output reg  [31:0] rx_cnt,
    output reg  [31:0] drop_cnt,
    output reg  [31:0] ovf_cnt,

    // to HSB host port 1 RX (i_hif_axis[1])
    output reg  [511:0] m_tdata,
    output reg  [ 63:0] m_tkeep,
    output reg          m_tlast,
    output reg          m_tvalid,
    input  wire         m_tready,

    // from HSB host port 1 TX (o_hif_axis[1]); tready is 1 in the top
    input  wire [511:0] s_tdata,
    input  wire [ 63:0] s_tkeep,
    input  wire         s_tlast,
    input  wire         s_tvalid
);

    // ------------------------------------------------------------ //
    // Buffers: 4 beats x 64 B each
    // ------------------------------------------------------------ //
    reg  [ 31:0] txw [0:63];
    reg  [511:0] rxb [0:3];
    wire [511:0] tx_beat [0:3];

    genvar b, w;
    generate
        for (b = 0; b < 4; b = b + 1) begin : g_beat
            for (w = 0; w < 16; w = w + 1) begin : g_word
                assign tx_beat[b][w*32+:32] = txw[b*16+w];
            end
        end
    endgenerate

    always @(posedge clk) begin
        if (tx_we) txw[tx_waddr] <= tx_wdata;
    end

    assign rx_rdata = rxb[rx_raddr[5:4]][rx_raddr[3:0]*32+:32];

    // ------------------------------------------------------------ //
    // TX: stream ceil(tx_len / 64) beats
    //     tkeep (mask) trimmed on the last beat
    // ------------------------------------------------------------ //
    reg  [1:0] tx_beat_idx;
    reg  [1:0] tx_last_idx;
    wire [5:0] tx_tail     = tx_len[5:0];                       // bytes in the last beat, 0 = 64
    wire [2:0] tx_beats_m1 = tx_len[8:6] - {2'b00, (tx_tail == 6'd0)};   // ceil(len/64) - 1
    wire [63:0] tx_keep_last = (tx_tail == 6'd0) ? {64{1'b1}} : ({64{1'b1}} >> (7'd64 - {1'b0, tx_tail}));

    always @(posedge clk) begin
        if (!rstn) begin
            tx_busy     <= 1'b0;
            tx_beat_idx <= 2'd0;
            tx_last_idx <= 2'd0;
            m_tdata     <= 512'd0;
            m_tkeep     <= 64'd0;
            m_tlast     <= 1'b0;
            m_tvalid    <= 1'b0;
        end else if (!tx_busy) begin
            if (send && (tx_len != 9'd0) && (tx_len <= 9'd256)) begin
                tx_busy     <= 1'b1;
                tx_beat_idx <= 2'd0;
                tx_last_idx <= tx_beats_m1[1:0];
                m_tdata     <= tx_beat[0];
                m_tlast     <= (tx_len <= 9'd64);
                m_tkeep     <= (tx_len <= 9'd64) ? tx_keep_last : {64{1'b1}};
                m_tvalid    <= 1'b1;
            end
        end else if (m_tvalid && m_tready) begin
            if (m_tlast) begin
                m_tvalid <= 1'b0;
                m_tlast  <= 1'b0;
                tx_busy  <= 1'b0;
            end else begin
                tx_beat_idx <= tx_beat_idx + 2'd1;
                m_tdata     <= tx_beat[tx_beat_idx + 2'd1];
                m_tlast     <= (tx_beat_idx + 2'd1 == tx_last_idx);
                m_tkeep     <= (tx_beat_idx + 2'd1 == tx_last_idx) ? tx_keep_last : {64{1'b1}};
            end
        end
    end

    // ------------------------------------------------------------ //
    // RX: keep one ECB reply frame
    // ------------------------------------------------------------ //
    // first beat: EtherType = 0x0800 (bytes 12-13): IPV4,
    //             UDP = 17 (byte 23),
    //             UDP source port = ecb_port (bytes 34-35, big-endian)
    wire is_ecb = (s_tdata[12*8+:8] == 8'h08) && (s_tdata[13*8+:8] == 8'h00) &&
                  (s_tdata[23*8+:8] == 8'h11) &&
                  (s_tdata[34*8+:8] == ecb_port[15:8]) && (s_tdata[35*8+:8] == ecb_port[7:0]);

    reg       rx_in_pkt;
    reg       rx_take;      // this packet is being kept
    reg [2:0] rx_beats;     // beats stored so far

    // bytes in a beat (tkeep is contiguous from bit 0)
    function [6:0] keep_bytes(input [63:0] keep);
        integer i;
        begin
            keep_bytes = 7'd0;
            for (i = 0; i < 64; i = i + 1) keep_bytes = keep_bytes + keep[i];
        end
    endfunction

    wire       rx_sop  = s_tvalid && !rx_in_pkt;
    wire       take    = rx_sop ? (is_ecb && !rx_valid) : rx_take;

    always @(posedge clk) begin
        if (!rstn) begin
            rx_in_pkt <= 1'b0;
            rx_take   <= 1'b0;
            rx_beats  <= 3'd0;
            rx_valid  <= 1'b0;
            rx_trunc  <= 1'b0;
            rx_len    <= 9'd0;
            rx_cnt    <= 32'd0;
            drop_cnt  <= 32'd0;
            ovf_cnt   <= 32'd0;
        end else begin
            if (release_rx) begin
                rx_valid <= 1'b0;
                rx_trunc <= 1'b0;
                rx_len   <= 9'd0;
            end

            if (s_tvalid) begin
                rx_in_pkt <= !s_tlast;
                if (rx_sop) begin
                    rx_take  <= is_ecb && !rx_valid;
                    rx_beats <= 3'd0;
                    if (!is_ecb)       drop_cnt <= drop_cnt + 32'd1;
                    else if (rx_valid) ovf_cnt  <= ovf_cnt + 32'd1;
                end

                if (take) begin
                    if ((rx_sop ? 3'd0 : rx_beats) < 3'd4) begin
                        rxb[rx_sop ? 2'd0 : rx_beats[1:0]] <= s_tdata;
                        rx_len   <= (rx_sop ? 9'd0 : rx_len) + {2'b00, keep_bytes(s_tkeep)};
                        rx_beats <= (rx_sop ? 3'd0 : rx_beats) + 3'd1;
                    end else begin
                        rx_trunc <= 1'b1;
                    end
                    if (s_tlast) begin
                        rx_valid <= 1'b1;
                        rx_cnt   <= rx_cnt + 32'd1;
                    end
                end
            end
        end
    end

endmodule

`default_nettype wire
