// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Xanadu Quantum Technologies Inc.
//===----------------------------------------------------------------------===//
//
// hwhs_engine.v - the round FSM of hwhs-hsb
//
// One round: a 64 B request into HSB sensor-RX 0, a reply back from sensor-TX 0.
//
//                   hwhs_regs (PS AXI-Lite registers)
//                     | control          ^ status
//                     v                  |
//   slot RAM  --B--> +--------------------+ --sif_rx (request)--> HSB sensor-RX 0 ==> coprocessor
//   reply RAM <--B-- |    hwhs_engine     |
//   trace RAM <----- |                    | <--sif_tx (reply)---- HSB sensor-TX 0 <== coprocessor
//                    +--------------------+
//                       ^ s_axis      | m_axis
//                       | (request)   v (reply)
//                   transport_demo (the hw-loop's request source)
//
// FSM:
//   sw-loop (CPU writes slot RAM[RING_IDX], then CTRL.start):
//     IDLE --start--> RD -> RD2 -> SEND -> WAIT --reply--> DONE
//   hw-loop (transport_demo drives s_axis while demo_run is set):
//     IDLE --demo_run--> DEMO_RX -> SEND -> WAIT --reply--> DEMO_FWD -> DONE
//
// RTT (hif cycles):
//   sw-loop: from START accepted to the reply beat
//   hw-loop: from the s_axis handshake to the m_axis handshake
//
//===----------------------------------------------------------------------===//
`timescale 1ps / 1ps
`default_nettype none

module hwhs_engine #(
    parameter integer C_SLOT_AW  = 10,   // slot/reply ring: 2^C_SLOT_AW slots
    parameter integer C_BEAT_DW  = 512,  // 64 B: one slot, request or reply
    parameter integer C_TRACE_AW = 20,   // RTT trace depth: 2^20 entries = 2 MB
    parameter integer C_TRACE_DW = 16    // one trace entry: RTT in clocks, saturating
) (
    input wire clk,
    input wire rstn,

    // CONTROL (hwhs_regs)
    input  wire                   start,       // W1P
    input  wire                   abort,       // level
    input  wire                   soft_reset,  // level
    input  wire                   ring_wr,
    input  wire [C_SLOT_AW-1:0]   ring_wdata,
    input  wire                   seq_wr,
    input  wire [31:0]            seq_wdata,
    input  wire [C_SLOT_AW:0]     ring_len,
    input  wire                   seq_stamp,
    input  wire [31:0]            reply_to,
    input  wire [31:0]            seq_off,
    input  wire                   demo_run,
    input  wire                   trace_clear, // level: hold the trace empty

    // STATUS (hwhs_regs)
    output reg  [ 3:0]            sts_state,
    output reg                    sts_busy,
    output reg                    sts_done,
    output reg                    sts_reply_seen,
    output reg                    sts_timeout,
    output wire [C_SLOT_AW-1:0]   sts_ring_idx,
    output wire [31:0]            sts_next_seq,
    output reg  [31:0]            sts_last_seq,
    output reg  [31:0]            sts_round_cnt,
    output reg  [31:0]            sts_done_cnt,    // rounds finished, with a reply or timed out
    output reg  [63:0]            sts_rtt,
    output reg  [31:0]            sts_stale_cnt,
    output reg  [31:0]            sts_timeout_cnt,
    output reg  [31:0]            sts_tx_cnt,
    output reg  [31:0]            sts_rx_cnt,
    output reg  [31:0]            sts_last_rd,
    output reg  [31:0]            sts_trace_cnt,
    output reg                    sts_trace_full,
    output reg                    sts_trace_sat,

    // Slot RAM (read) and reply RAM (write), port B
    output wire [C_SLOT_AW-1:0]   slot_addr,
    input  wire [C_BEAT_DW-1:0]   slot_dout,
    output reg                    reply_we,
    output wire [C_SLOT_AW-1:0]   reply_addr,
    output wire [C_BEAT_DW-1:0]   reply_din,

    // hw-loop: transport_demo
    input  wire [C_BEAT_DW-1:0]   s_axis_tdata,
    input  wire                   s_axis_tvalid,
    output reg                    s_axis_tready,
    output wire [C_BEAT_DW-1:0]   m_axis_tdata,
    output reg                    m_axis_tvalid,
    input  wire                   m_axis_tready,

    // HSB sensor-RX 0 (request), single beat, tlast = 1, tkeep = all
    output reg  [C_BEAT_DW-1:0]   sif_rx_tdata,
    output reg                    sif_rx_tvalid,
    input  wire                   sif_rx_tready,

    // HSB sensor-TX 0 (reply), never back-pressured
    input  wire [C_BEAT_DW-1:0]   sif_tx_tdata,
    input  wire                   sif_tx_tvalid,
    input  wire                   sif_tx_tlast,

    // RTT trace (hh_rtt_trace port A)
    output reg                    trace_we,
    output reg  [C_TRACE_AW-1:0]  trace_addr,
    output reg  [C_TRACE_DW-1:0]  trace_din        // rtt in clocks, saturating
);

    localparam [3:0] S_IDLE     = 4'd0,
                     S_RD       = 4'd1,
                     S_RD2      = 4'd2,
                     S_SEND     = 4'd3,
                     S_WAIT     = 4'd4,
                     S_DEMO_RX  = 4'd5,
                     S_DEMO_FWD = 4'd6,
                     S_DONE     = 4'd7;

    wire rst = !rstn || soft_reset;

    // seq dword lane inside a beat: SEQ_OFF is a byte offset into the beat
    localparam integer BEAT_BYTES = C_BEAT_DW / 8;       // 64 B
    localparam integer BEAT_LSB   = $clog2(BEAT_BYTES);  // 6: byte-offset bits inside a beat
    localparam integer DW_LSB     = 2;                   // a dword is 4 B
    localparam integer SEQ_DW_W   = BEAT_LSB - DW_LSB;   // 4: picks one of the 16 dwords

    wire [SEQ_DW_W-1:0] seq_dw = seq_off[BEAT_LSB-1:DW_LSB];

    // stamp the seq into a beat
    function [C_BEAT_DW-1:0] stamp(input [C_BEAT_DW-1:0] beat, input [SEQ_DW_W-1:0] dw,
                                   input [31:0] seq);
        begin
            stamp            = beat;
            stamp[dw*32+:32] = seq;
        end
    endfunction

    // ------------------------------------------------------------ //
    // State
    // ------------------------------------------------------------ //
    reg  [3:0]            state;
    reg  [63:0]           free_cnt;     // free-running hif cycle count: the RTT clock
    reg  [C_SLOT_AW-1:0]  ring_idx;     // slot of this round (or of the next one)
    reg  [31:0]           next_seq;     // seq the next request carries
    reg                   start_pend;   // a START not taken yet
    reg                   demo_act;     // this round came from transport_demo
    reg                   round_ok;     // this round ended with its reply
    reg  [31:0]           to_cnt;       // cycles spent in WAIT
    reg                   rx_in_pkt;    // inside a multi-beat sensor-TX packet
    reg  [63:0]           t_start;      // free_cnt when the round started
    reg  [31:0]           exp_seq;      // seq the reply must carry
    reg  [C_BEAT_DW-1:0]  reply_beat;   // the reply, or the zero beat of a timed-out demo round
    reg                   trace_push;   // hand this round's RTT to the trace writer
    reg  [C_TRACE_DW-1:0] trace_value;
    reg  [C_TRACE_AW-1:0] trace_ptr;

    wire [C_SLOT_AW-1:0] ring_next = ({1'b0, ring_idx} + 1'b1 >= ring_len) ? {C_SLOT_AW{1'b0}}
                                                                           : ring_idx + 1'b1;

    assign sts_ring_idx = ring_idx;
    assign sts_next_seq = next_seq;
    assign slot_addr    = ring_idx;     // port B reads this slot all the time; RD/RD2 wait out its latency
    assign reply_addr   = ring_idx;     // the reply goes to the request's slot
    assign reply_din    = reply_beat;
    assign m_axis_tdata = reply_beat;

    // ------------------------------------------------------------ //
    // Events
    // ------------------------------------------------------------ //
    wire go          = !abort;
    wire idle        = go && (state == S_IDLE);
    wire sw_begin    = idle && (start || start_pend);                 // a sw-loop round starts
    wire hw_begin    = idle && !(start || start_pend) && demo_run;    // offer s_axis one request
    wire hw_stop     = go && (state == S_DEMO_RX) && !demo_run;       // demo ended before a request came
    wire hw_take     = go && (state == S_DEMO_RX) && demo_run && s_axis_tready && s_axis_tvalid;
    wire req_load    = (go && (state == S_RD2)) || hw_take;           // the request beat is formed
    wire req_sent    = go && (state == S_SEND) && sif_rx_tvalid && sif_rx_tready;

    wire [31:0] rx_seq = sif_tx_tdata[seq_dw*32+:32];
    wire        rx_sop = sif_tx_tvalid && !rx_in_pkt;                 // first beat of a sensor-TX packet

    wire got_reply   = go && (state == S_WAIT) && rx_sop && (rx_seq == exp_seq);
    wire timed_out   = (reply_to != 32'd0) && (to_cnt >= reply_to);
    wire got_timeout = go && (state == S_WAIT) && !got_reply && timed_out;
    wire round_over  = got_reply || got_timeout;                      // WAIT ends
    wire fwd_done    = go && (state == S_DEMO_FWD) && m_axis_tvalid && m_axis_tready;
    wire round_end   = go && (state == S_DONE);

    // ------------------------------------------------------------ //
    // FSM
    // ------------------------------------------------------------ //
    always @(posedge clk) begin
        if (rst || abort) state <= S_IDLE;
        else case (state)
            S_IDLE:     state <= sw_begin ? S_RD : hw_begin ? S_DEMO_RX : S_IDLE;
            S_RD:       state <= S_RD2;    // slot RAM read latency: hwhs_ram RD_LATENCY = 2
            S_RD2:      state <= S_SEND;
            S_DEMO_RX:  state <= hw_stop ? S_IDLE : hw_take ? S_SEND : S_DEMO_RX;
            S_SEND:     if (req_sent)   state <= S_WAIT;
            S_WAIT:     if (round_over) state <= demo_act ? S_DEMO_FWD : S_DONE;
            S_DEMO_FWD: if (fwd_done)   state <= S_DONE;
            S_DONE:     state <= S_IDLE;
            default:    state <= S_IDLE;
        endcase
    end

    // ------------------------------------------------------------ //
    // Request path: slot RAM or s_axis -> sif_rx
    // ------------------------------------------------------------ //
    wire [C_BEAT_DW-1:0] req_src = (state == S_DEMO_RX) ? s_axis_tdata : slot_dout;

    always @(posedge clk) begin
        if (rst) begin
            start_pend    <= 1'b0;
            demo_act      <= 1'b0;
            s_axis_tready <= 1'b0;
            sif_rx_tvalid <= 1'b0;
            next_seq      <= 32'd1;
            ring_idx      <= {C_SLOT_AW{1'b0}};
        end else begin
            if (abort || sw_begin) start_pend <= 1'b0;
            else if (start)        start_pend <= 1'b1;

            if (sw_begin)      demo_act <= 1'b0;
            else if (hw_begin) demo_act <= 1'b1;
            else if (hw_stop)  demo_act <= 1'b0;

            if (abort || hw_stop || hw_take) s_axis_tready <= 1'b0;
            else if (hw_begin)               s_axis_tready <= 1'b1;

            // abort drops a pending request without its handshake
            // TODO: should respect AXIS standard
            if (abort || req_sent) sif_rx_tvalid <= 1'b0;
            else if (req_load)     sif_rx_tvalid <= 1'b1;

            if (req_sent && seq_stamp) next_seq <= next_seq + 32'd1;
            else if (seq_wr)           next_seq <= seq_wdata;

            if (round_end)    ring_idx <= ring_next;
            else if (ring_wr) ring_idx <= ring_wdata;
        end
    end

    always @(posedge clk) begin
        if (sw_begin || hw_take) t_start <= free_cnt;
        if (req_load) begin
            sif_rx_tdata <= seq_stamp ? stamp(req_src, seq_dw, next_seq) : req_src;
            exp_seq      <= seq_stamp ? next_seq : req_src[seq_dw*32+:32];
        end
    end

    // ------------------------------------------------------------ //
    // Reply path: sif_tx -> reply RAM, m_axis
    // ------------------------------------------------------------ //
    always @(posedge clk) begin
        if (rst) begin
            rx_in_pkt     <= 1'b0;
            to_cnt        <= 32'd0;
            round_ok      <= 1'b0;
            reply_we      <= 1'b0;
            m_axis_tvalid <= 1'b0;
        end else begin
            if (sif_tx_tvalid) rx_in_pkt <= !sif_tx_tlast;

            if (req_sent)                     to_cnt <= 32'd0;
            else if (go && (state == S_WAIT)) to_cnt <= to_cnt + 32'd1;

            if (got_reply)        round_ok <= 1'b1;
            else if (got_timeout) round_ok <= 1'b0;

            reply_we <= got_reply;    // one cycle, while ring_idx still points at this round

            // abort drops a pending reply without its handshake
            // TODO: should respect AXIS standard
            if (abort || fwd_done)           m_axis_tvalid <= 1'b0;
            else if (round_over && demo_act) m_axis_tvalid <= 1'b1;
        end
    end

    always @(posedge clk) begin
        if (got_reply)        reply_beat <= sif_tx_tdata;
        else if (got_timeout) reply_beat <= {C_BEAT_DW{1'b0}};
    end

    // ------------------------------------------------------------ //
    // Status and statistics
    // ------------------------------------------------------------ //
    always @(posedge clk) begin
        if (rst) begin
            free_cnt        <= 64'd0;
            sts_state       <= S_IDLE;
            sts_busy        <= 1'b0;
            sts_done        <= 1'b0;
            sts_reply_seen  <= 1'b0;
            sts_timeout     <= 1'b0;
            sts_rtt         <= 64'd0;
            sts_last_seq    <= 32'd0;
            sts_last_rd     <= 32'd0;
            sts_round_cnt   <= 32'd0;
            sts_done_cnt    <= 32'd0;
            sts_timeout_cnt <= 32'd0;
            sts_stale_cnt   <= 32'd0;
            sts_tx_cnt      <= 32'd0;
            sts_rx_cnt      <= 32'd0;
            trace_push      <= 1'b0;
        end else begin
            free_cnt  <= free_cnt + 64'd1;
            sts_state <= state;

            if (sw_begin || hw_begin)                       sts_busy <= 1'b1;
            else if (abort || hw_stop || round_end || idle) sts_busy <= 1'b0;

            // per-round flags, cleared when a round takes its request
            if (sw_begin || hw_take) begin
                sts_done       <= 1'b0;
                sts_reply_seen <= 1'b0;
                sts_timeout    <= 1'b0;
            end else begin
                if (round_end)   sts_done       <= 1'b1;
                if (got_reply)   sts_reply_seen <= 1'b1;
                if (got_timeout) sts_timeout    <= 1'b1;
            end

            // RTT: sw-loop at the reply beat, hw-loop at the m_axis handshake
            if (got_reply && !demo_act)    sts_rtt <= free_cnt - t_start;
            else if (fwd_done && round_ok) sts_rtt <= free_cnt - t_start;

            if (req_sent) begin
                sts_tx_cnt   <= sts_tx_cnt + 32'd1;
                sts_last_seq <= exp_seq;
            end
            if (got_timeout)           sts_timeout_cnt <= sts_timeout_cnt + 32'd1;
            if (round_end)             sts_done_cnt    <= sts_done_cnt + 32'd1;
            if (round_end && round_ok) sts_round_cnt   <= sts_round_cnt + 32'd1;

            // every sensor-TX packet counts; one that does not end the round is stale
            if (rx_sop) begin
                sts_rx_cnt  <= sts_rx_cnt + 32'd1;
                sts_last_rd <= rx_seq;
                if (!got_reply) sts_stale_cnt <= sts_stale_cnt + 32'd1;
            end

            trace_push <= round_end && round_ok;
        end
    end

    always @(posedge clk) begin
        if (round_end)   // saturate: an RTT that needs more than C_TRACE_DW bits is all ones
            trace_value <= (|sts_rtt[63:C_TRACE_DW]) ? {C_TRACE_DW{1'b1}} : sts_rtt[C_TRACE_DW-1:0];
    end

    // ------------------------------------------------------------ //
    // RTT trace writer: one entry per completed round, until full
    // ------------------------------------------------------------ //
    wire trace_wr = trace_push && !sts_trace_full;

    always @(posedge clk) begin
        if (rst || trace_clear) begin
            trace_we       <= 1'b0;
            trace_ptr      <= {C_TRACE_AW{1'b0}};
            sts_trace_cnt  <= 32'd0;
            sts_trace_full <= 1'b0;
            sts_trace_sat  <= 1'b0;
        end else begin
            trace_we <= trace_wr;
            if (trace_wr) begin
                sts_trace_cnt <= sts_trace_cnt + 32'd1;
                if (trace_value == {C_TRACE_DW{1'b1}}) sts_trace_sat <= 1'b1;
                if (trace_ptr == {C_TRACE_AW{1'b1}})   sts_trace_full <= 1'b1;
                else                                   trace_ptr <= trace_ptr + 1'b1;
            end
        end
    end

    always @(posedge clk) begin
        if (trace_wr) begin
            trace_addr <= trace_ptr;
            trace_din  <= trace_value;
        end
    end

endmodule

`default_nettype wire
