// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Xanadu Quantum Technologies Inc.
//
// hwhs_engine_sva.sv - assertions on hwhs_engine, bound into every instance.
// Simulation only: hw/sim/run.sh compiles it with --assert; Vivado never reads it.
//
//   a_sq_hold, a_tx_hold          AXI-Stream: once VALID is up, VALID and the data hold until
//                                 the handshake, abort included (a post is owed to BALBOA)
//   a_m_axis_hold                 the same for the reply to transport_demo. abort breaks this
//                                 today (it drops TVALID without a handshake), so no test
//                                 aborts with a reply pending.
//   a_post_owed                   no round leaves IDLE while a post is owed
//   a_reply_we                    reply RAM is written only for the reply that matched
//                                 this round's seq
//   a_wait_timeout                WAIT is left the cycle after REPLY_TO runs out
//   a_send_bounded, a_fwd_bounded SEND and DEMO_FWD end within MAX_STALL cycles; neither
//                                 has a timeout of its own yet

module hwhs_engine_sva #(
    parameter int C_BEAT_DW = 512,
    parameter int MAX_STALL = 100000
) (
    input logic                 clk,
    input logic                 rst,
    input logic                 in_idle,
    input logic                 in_send,
    input logic                 in_wait,
    input logic                 in_fwd,
    input logic                 post_busy,
    input logic                 sq_valid,
    input logic                 sq_ready,
    input logic [63:0]          sq_raddr,
    input logic                 tx_tvalid,
    input logic                 tx_tready,
    input logic [C_BEAT_DW-1:0] tx_tdata,
    input logic                 m_axis_tvalid,
    input logic                 m_axis_tready,
    input logic [C_BEAT_DW-1:0] m_axis_tdata,
    input logic                 reply_we,
    input logic                 got_reply,
    input logic [31:0]          to_cnt,
    input logic [31:0]          reply_to
);

    a_sq_hold: assert property (@(posedge clk) disable iff (rst)
        sq_valid && !sq_ready |=> sq_valid && $stable(sq_raddr))
        else $error("sq: VALID or the address changed before the handshake");

    a_tx_hold: assert property (@(posedge clk) disable iff (rst)
        tx_tvalid && !tx_tready |=> tx_tvalid && $stable(tx_tdata))
        else $error("tx: TVALID or TDATA changed before the handshake");

    a_m_axis_hold: assert property (@(posedge clk) disable iff (rst)
        m_axis_tvalid && !m_axis_tready |=> m_axis_tvalid && $stable(m_axis_tdata))
        else $error("m_axis: TVALID or TDATA changed before the handshake");

    a_post_owed: assert property (@(posedge clk) disable iff (rst)
        in_idle && post_busy |=> in_idle)
        else $error("a round started while a post was owed");

    a_reply_we: assert property (@(posedge clk) disable iff (rst)
        reply_we |-> $past(got_reply))
        else $error("reply RAM written without a matching reply");

    a_wait_timeout: assert property (@(posedge clk) disable iff (rst)
        in_wait && reply_to != 0 && to_cnt >= reply_to |=> !in_wait)
        else $error("WAIT outlived REPLY_TO (%0d)", reply_to);

    // consecutive cycles in the current SEND or DEMO_FWD visit
    int unsigned stall;
    logic        was_send, was_fwd;
    always_ff @(posedge clk) begin
        was_send <= in_send;
        was_fwd  <= in_fwd;
        stall    <= (rst || !((in_send && was_send) || (in_fwd && was_fwd))) ? 0 : stall + 1;
    end

    a_send_bounded: assert property (@(posedge clk) disable iff (rst)
        in_send |-> stall < MAX_STALL)
        else $error("SEND stuck for %0d cycles: BALBOA never took the request", stall);

    a_fwd_bounded: assert property (@(posedge clk) disable iff (rst)
        in_fwd |-> stall < MAX_STALL)
        else $error("DEMO_FWD stuck for %0d cycles: transport_demo never took the reply", stall);

endmodule

bind hwhs_engine hwhs_engine_sva #(.C_BEAT_DW(C_BEAT_DW)) u_sva (
    .clk          (clk),
    .rst          (rst),
    .in_idle      (state == S_IDLE),
    .in_send      (state == S_SEND),
    .in_wait      (state == S_WAIT),
    .in_fwd       (state == S_DEMO_FWD),
    .post_busy    (sts_post_busy),
    .sq_valid     (sq_valid),
    .sq_ready     (sq_ready),
    .sq_raddr     (sq_raddr),
    .tx_tvalid    (tx_tvalid),
    .tx_tready    (tx_tready),
    .tx_tdata     (tx_tdata),
    .m_axis_tvalid(m_axis_tvalid),
    .m_axis_tready(m_axis_tready),
    .m_axis_tdata (m_axis_tdata),
    .reply_we     (reply_we),
    .got_reply    (got_reply),
    .to_cnt       (to_cnt),
    .reply_to     (reply_to)
);
