// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Xanadu Quantum Technologies Inc.
//
// tb_hwhs_engine.sv - hwhs_engine with models of the RAMs, HSB and transport_demo.
// Run with hw/sim/run.sh.

`timescale 1ps / 1ps

module tb_hwhs_engine;
  localparam int SLOT_AW  = 4;     // 16 slots
  localparam int SLOTS    = 1 << SLOT_AW;
  localparam int BEAT_DW  = 512;
  localparam int TRACE_AW = 3;     // 8 entries
  localparam int TRACE_DW = 16;
  localparam int SEQ_OFF  = 12;
  localparam int LANE     = SEQ_OFF / 4;
  localparam logic [3:0] S_IDLE = 4'd0, S_SEND = 4'd3, S_WAIT = 4'd4, S_DEMO_RX = 4'd5,
                         S_DEMO_FWD = 4'd6, S_DONE = 4'd7;

  logic clk = 1'b0;
  always #2000 clk = ~clk;
  int unsigned cyc = 0;
  always @(posedge clk) cyc <= cyc + 1;

  int errors = 0;
  task automatic check(input bit cond, input string what);
    if (!cond) begin
      errors++;
      $display("  FAIL: %s", what);
    end
  endtask

  // --------------------------------------------------------------------------------------
  // DUT
  // --------------------------------------------------------------------------------------
  logic                rstn = 1'b0, start = 1'b0, abort = 1'b0, soft_reset = 1'b0;
  logic                ring_wr = 1'b0, seq_wr = 1'b0, seq_stamp = 1'b1, demo_run = 1'b0;
  logic                trace_clear = 1'b1;
  logic [SLOT_AW-1:0]  ring_wdata = '0;
  logic [SLOT_AW:0]    ring_len = SLOTS;
  logic [31:0]         seq_wdata = '0, reply_to = 32'd2000, seq_off = SEQ_OFF;

  wire  [3:0]          sts_state;
  wire                 sts_busy, sts_done, sts_reply_seen, sts_timeout;
  wire  [SLOT_AW-1:0]  sts_ring_idx;
  wire  [31:0]         sts_next_seq, sts_last_seq, sts_round_cnt, sts_done_cnt, sts_stale_cnt;
  wire  [31:0]         sts_timeout_cnt, sts_tx_cnt, sts_rx_cnt, sts_last_rd, sts_trace_cnt;
  wire  [63:0]         sts_rtt;
  wire                 sts_trace_full, sts_trace_sat;

  wire  [SLOT_AW-1:0]  slot_addr, reply_addr;
  logic [BEAT_DW-1:0]  slot_dout;
  wire                 reply_we;
  wire  [BEAT_DW-1:0]  reply_din;
  logic [BEAT_DW-1:0]  s_axis_tdata = '0;
  logic                s_axis_tvalid = 1'b0;
  wire                 s_axis_tready;
  wire  [BEAT_DW-1:0]  m_axis_tdata;
  wire                 m_axis_tvalid;
  logic                m_axis_tready = 1'b0;
  wire  [BEAT_DW-1:0]  sif_rx_tdata;
  wire                 sif_rx_tvalid;
  logic                sif_rx_tready = 1'b1;
  logic [BEAT_DW-1:0]  sif_tx_tdata = '0;
  logic                sif_tx_tvalid = 1'b0, sif_tx_tlast = 1'b0;
  wire                 trace_we;
  wire  [TRACE_AW-1:0] trace_addr;
  wire  [TRACE_DW-1:0] trace_din;

  hwhs_engine #(
    .C_SLOT_AW (SLOT_AW),
    .C_BEAT_DW (BEAT_DW),
    .C_TRACE_AW(TRACE_AW),
    .C_TRACE_DW(TRACE_DW)
  ) u_dut (.*);

  // --------------------------------------------------------------------------------------
  // RAMs (slot RAM READ_LATENCY 2)
  // --------------------------------------------------------------------------------------
  logic [BEAT_DW-1:0]  slot_mem  [SLOTS];
  logic [BEAT_DW-1:0]  reply_mem [SLOTS];
  logic [TRACE_DW-1:0] trace_mem [1 << TRACE_AW];
  logic [BEAT_DW-1:0]  slot_q;
  always @(posedge clk) begin
    slot_q    <= slot_mem[slot_addr];
    slot_dout <= slot_q;
    if (reply_we) reply_mem[reply_addr] <= reply_din;
    if (trace_we) trace_mem[trace_addr] <= trace_din;
  end

  // --------------------------------------------------------------------------------------
  // HSB model: echoes each request after reply_delay cycles
  // --------------------------------------------------------------------------------------
  int                   reply_delay = 50, reply_beats = 1, drop_next = 0, late_next = 0;
  int                   late_delay = 0;
  logic [BEAT_DW-1:0]   last_req = '0;
  logic [4*BEAT_DW-1:0] pq_data [$];      // queued replies, up to 4 beats
  int                   pq_n    [$];
  int unsigned          pq_at   [$];

  task automatic hsb_send(input logic [4*BEAT_DW-1:0] beats, input int n, input int unsigned at);
    pq_data.push_back(beats);
    pq_n.push_back(n);
    pq_at.push_back(at);
  endtask

  always @(posedge clk) begin
    if (sif_rx_tvalid && sif_rx_tready) begin
      last_req = sif_rx_tdata;
      if (drop_next > 0) begin
        drop_next--;
      end else if (late_next > 0) begin
        late_next--;
        hsb_send({4{sif_rx_tdata}}, reply_beats, cyc + 32'(late_delay));
      end else begin
        hsb_send({4{sif_rx_tdata}}, reply_beats, cyc + 32'(reply_delay));
      end
    end
  end

  int                   tx_pos = 0, tx_n = 0;
  logic [4*BEAT_DW-1:0] tx_pkt;
  always @(negedge clk) begin
    if (tx_pos == 0)
      for (int q = 0; q < pq_at.size(); q++)
        if (cyc >= pq_at[q]) begin
          tx_pkt = pq_data[q];
          tx_n   = pq_n[q];
          pq_data.delete(q);
          pq_n.delete(q);
          pq_at.delete(q);
          tx_pos = 1;
          break;
        end
    if (tx_pos > 0) begin
      sif_tx_tvalid = 1'b1;
      sif_tx_tdata  = tx_pkt[(tx_pos - 1) * BEAT_DW +: BEAT_DW];
      sif_tx_tlast  = (tx_pos == tx_n);
      tx_pos        = (tx_pos == tx_n) ? 0 : tx_pos + 1;
    end else begin
      sif_tx_tvalid = 1'b0;
      sif_tx_tlast  = 1'b0;
    end
  end

  // --------------------------------------------------------------------------------------
  // transport_demo and register writes
  // --------------------------------------------------------------------------------------
  task automatic tick(input int n);
    repeat (n) @(negedge clk);
  endtask

  task automatic demo_send(input logic [BEAT_DW-1:0] d);
    @(negedge clk);
    s_axis_tdata  = d;
    s_axis_tvalid = 1'b1;
    while (!s_axis_tready) @(negedge clk);
    @(negedge clk);
    s_axis_tvalid = 1'b0;
  endtask

  task automatic demo_recv(input int hold, output logic [BEAT_DW-1:0] d);
    tick(hold);
    m_axis_tready = 1'b1;
    while (!m_axis_tvalid) @(negedge clk);
    d = m_axis_tdata;
    @(negedge clk);
    m_axis_tready = 1'b0;
  endtask

  task automatic pulse_start();
    @(negedge clk);
    start = 1'b1;
    @(negedge clk);
    start = 1'b0;
  endtask

  task automatic write_ring_idx(input int v);
    @(negedge clk);
    ring_wr = 1'b1;
    ring_wdata = SLOT_AW'(v);
    @(negedge clk);
    ring_wr = 1'b0;
  endtask

  task automatic write_next_seq(input logic [31:0] v);
    @(negedge clk);
    seq_wr = 1'b1;
    seq_wdata = v;
    @(negedge clk);
    seq_wr = 1'b0;
  endtask

  // --------------------------------------------------------------------------------------
  // sw-loop rounds
  // --------------------------------------------------------------------------------------
  function automatic logic [BEAT_DW-1:0] req(input int i);
    logic [BEAT_DW-1:0] q = '0;
    q[63:0]    = 64'hCAFE_0000_0000_0000 + 64'(i);
    q[159:128] = 32'(i);
    return q;                  // seq stamped by the engine
  endfunction

  task automatic wait_done(input logic [31:0] target, input int limit, input string what);
    int t = 0;
    while (sts_done_cnt < target && t < limit) begin
      @(negedge clk);
      t++;
    end
    check(sts_done_cnt >= target, $sformatf("%s: the round did not finish", what));
  endtask

  task automatic sw_round(input int i, input string what, output bit timed_out);
    logic [31:0] done0, to0;
    done0 = sts_done_cnt;
    to0   = sts_timeout_cnt;
    slot_mem[sts_ring_idx] = req(i);
    tick(3);                   // slot RAM latency
    pulse_start();
    wait_done(done0 + 1, 400000, what);
    timed_out = (sts_timeout_cnt != to0);
  endtask

  task automatic sw_round_ok(input int i, input string what);
    bit to;
    int idx;
    idx = int'(sts_ring_idx);
    sw_round(i, what, to);
    check(!to, {what, ": unexpected timeout"});
    check(reply_mem[idx][63:0] == req(i)[63:0], {what, ": reply value"});
    check(reply_mem[idx][LANE*32 +: 32] == sts_last_seq, {what, ": reply carries the sent seq"});
  endtask

  // --------------------------------------------------------------------------------------
  // Test
  // --------------------------------------------------------------------------------------
  initial begin
    bit                 to;
    logic [BEAT_DW-1:0] beat;
    logic [31:0]        done0, seq0, stale0, rx0, tx0, rounds0, to0;

    for (int i = 0; i < SLOTS; i++) begin
      slot_mem[i]  = '0;
      reply_mem[i] = '0;
    end
    tick(5);
    rstn = 1'b1;
    tick(5);

    $display("-- sw-loop rounds");
    for (int i = 0; i < 3; i++) sw_round_ok(i, $sformatf("round %0d", i));
    check(sts_round_cnt == 3 && sts_tx_cnt == 3 && sts_rx_cnt == 3, "3 rounds, 3 requests, 3 replies");
    check(sts_last_seq == 3 && sts_next_seq == 4, "seqs 1..3 stamped");
    check(sts_ring_idx == 3, "RING_IDX advanced to 3");
    check(sts_rtt >= 64'(reply_delay) && sts_rtt < 64'(reply_delay + 20), $sformatf("RTT %0d", sts_rtt));
    $display("  RTT %0d cycles (the HSB model answers after %0d)", sts_rtt, reply_delay);

    $display("-- START while busy is kept; STARTs during one round coalesce");
    reply_delay = 200;
    done0 = sts_done_cnt;
    slot_mem[sts_ring_idx] = req(10);
    slot_mem[(int'(sts_ring_idx) + 1) % SLOTS] = req(11);
    tick(3);
    pulse_start();
    tick(20);
    check(u_dut.state == S_WAIT, "the first round is in WAIT");
    pulse_start();
    tick(5);
    pulse_start();
    wait_done(done0 + 2, 5000, "START while busy");
    tick(600);
    check(sts_done_cnt == done0 + 2, $sformatf("two rounds ran, not %0d", sts_done_cnt - done0));
    reply_delay = 50;

    $display("-- the ring wraps at RING_LEN");
    @(negedge clk);
    ring_len = 3;
    write_ring_idx(0);
    begin
      int seen [4];
      for (int i = 0; i < 4; i++) begin
        seen[i] = int'(sts_ring_idx);
        sw_round_ok(20 + i, $sformatf("wrap round %0d", i));
      end
      check(seen[0] == 0 && seen[1] == 1 && seen[2] == 2 && seen[3] == 0,
            $sformatf("slots %0d %0d %0d %0d", seen[0], seen[1], seen[2], seen[3]));
    end
    @(negedge clk);
    ring_len = SLOTS;

    $display("-- seq_stamp = 0: the payload's own seq is expected back");
    @(negedge clk);
    seq_stamp = 1'b0;
    begin
      int idx;
      logic [BEAT_DW-1:0] q;
      idx  = int'(sts_ring_idx);
      seq0 = sts_next_seq;
      to0  = sts_timeout_cnt;
      q = req(30);
      q[LANE*32 +: 32] = 32'h1234_5678;
      slot_mem[idx] = q;
      tick(3);
      done0 = sts_done_cnt;
      pulse_start();
      wait_done(done0 + 1, 5000, "seq_stamp 0");
      check(sts_timeout_cnt == to0 && sts_last_seq == 32'h1234_5678, "matched on the payload's seq");
      check(sts_next_seq == seq0, "NEXT_SEQ untouched");
      check(reply_mem[idx][LANE*32 +: 32] == 32'h1234_5678, "the reply carries it");
    end
    @(negedge clk);
    seq_stamp = 1'b1;

    $display("-- HSB holds sif_rx_tready low: the request waits, unchanged");
    @(negedge clk);
    sif_rx_tready = 1'b0;
    begin
      int idx;
      idx = int'(sts_ring_idx);
      slot_mem[idx] = req(40);
      tick(3);
      done0 = sts_done_cnt;
      pulse_start();
      tick(40);
      check(u_dut.state == S_SEND && sif_rx_tvalid, "waiting in SEND");
      @(negedge clk);
      sif_rx_tready = 1'b1;
      wait_done(done0 + 1, 5000, "back-pressure");
      check(reply_mem[idx][63:0] == req(40)[63:0], "reply after the back-pressure");
      check(sts_rtt >= 64'(40 + reply_delay), $sformatf("RTT %0d includes the wait", sts_rtt));
    end

    $display("-- a 3-beat reply: one packet, the round ends on its first beat");
    reply_beats = 3;
    rx0    = sts_rx_cnt;
    stale0 = sts_stale_cnt;
    sw_round_ok(50, "3-beat reply");
    tick(5);
    check(sts_rx_cnt == rx0 + 1 && sts_stale_cnt == stale0,
          $sformatf("rx +%0d, stale +%0d", sts_rx_cnt - rx0, sts_stale_cnt - stale0));
    reply_beats = 1;

    $display("-- a stray packet whose second beat carries the seq does not end the round");
    reply_delay = 300;
    begin
      int idx;
      logic [BEAT_DW-1:0] b0, b1;
      idx = int'(sts_ring_idx);
      slot_mem[idx] = req(60);
      tick(3);
      done0  = sts_done_cnt;
      rx0    = sts_rx_cnt;
      stale0 = sts_stale_cnt;
      pulse_start();
      tick(20);
      b1 = last_req;                                  // seq in beat 1
      b0 = last_req;
      b0[LANE*32 +: 32] = last_req[LANE*32 +: 32] + 32'd7;
      b0[63:0] = 64'hBAD;
      hsb_send({{(2*BEAT_DW){1'b0}}, b1, b0}, 2, cyc);
      wait_done(done0 + 1, 5000, "stray packet");
      check(sts_stale_cnt == stale0 + 1 && sts_rx_cnt == rx0 + 2,
            $sformatf("stray: stale +%0d, rx +%0d", sts_stale_cnt - stale0, sts_rx_cnt - rx0));
      check(reply_mem[idx][63:0] == req(60)[63:0], "the real reply was kept");
    end
    reply_delay = 50;

    $display("-- request lost: timeout");
    drop_next = 1;
    begin
      int idx;
      idx     = int'(sts_ring_idx);
      rounds0 = sts_round_cnt;
      reply_mem[idx] = '0;
      sw_round(70, "lost request", to);
      check(to && sts_timeout && sts_round_cnt == rounds0, "timed out, not counted as a round");
      check(reply_mem[idx] == '0, "reply RAM untouched");
      check(int'(sts_ring_idx) == (idx + 1) % SLOTS, "RING_IDX still advanced");
    end

    $display("-- late reply: arrives after the timeout and counts as stale");
    late_next  = 1;
    late_delay = 2600;          // REPLY_TO is 2000
    stale0     = sts_stale_cnt;
    sw_round(71, "late reply", to);
    check(to, "the late round timed out");
    sw_round_ok(72, "the round after it");
    tick(2700);
    check(sts_stale_cnt == stale0 + 1, $sformatf("late reply: stale +%0d", sts_stale_cnt - stale0));

    $display("-- abort frees a round that would wait forever");
    @(negedge clk);
    reply_to = 32'd0;
    drop_next = 1;
    begin
      int idx;
      idx   = int'(sts_ring_idx);
      done0 = sts_done_cnt;
      to0   = sts_timeout_cnt;
      slot_mem[idx] = req(80);
      tick(3);
      pulse_start();
      tick(3000);
      check(u_dut.state == S_WAIT && sts_busy, "stuck in WAIT with REPLY_TO = 0");
      @(negedge clk);
      abort = 1'b1;
      tick(3);
      check(u_dut.state == S_IDLE && !sts_busy, "abort: back in IDLE");
      pulse_start();                                  // dropped under abort
      tick(3);
      @(negedge clk);
      abort = 1'b0;
      tick(200);
      check(sts_done_cnt == done0 && sts_timeout_cnt == to0, "abort ends no round; the START under it is dropped");
      check(int'(sts_ring_idx) == idx, "abort keeps RING_IDX");
    end
    @(negedge clk);
    reply_to = 32'd2000;
    sw_round_ok(81, "the round after abort");

    $display("-- abort while HSB holds sif_rx_tready low: the request goes out first");
    @(negedge clk);
    sif_rx_tready = 1'b0;
    begin
      int idx;
      idx    = int'(sts_ring_idx);
      done0  = sts_done_cnt;
      stale0 = sts_stale_cnt;
      seq0   = sts_next_seq;
      slot_mem[idx] = req(82);
      tick(3);
      pulse_start();
      tick(20);
      check(u_dut.state == S_SEND && sif_rx_tvalid, "waiting in SEND");
      @(negedge clk);
      abort = 1'b1;
      @(negedge clk);
      abort = 1'b0;                                   // a pulse, as the driver writes it
      tick(20);
      check(u_dut.state == S_SEND && sif_rx_tvalid && sts_busy, "abort waits in SEND");
      @(negedge clk);
      sif_rx_tready = 1'b1;
      tick(3);
      check(u_dut.state == S_IDLE && !sts_busy, "then back in IDLE");
      check(last_req[63:0] == req(82)[63:0] && sts_next_seq == seq0 + 1, "the request went out with its seq");
      tick(reply_delay + 10);
      check(sts_done_cnt == done0 && sts_stale_cnt == stale0 + 1, "its reply is stale; no round ended");
      check(int'(sts_ring_idx) == idx, "abort keeps RING_IDX");
    end
    sw_round_ok(83, "the round after abort in SEND");

    $display("-- CPU writes during a round take effect at once");
    reply_delay = 200;
    begin
      int idx;
      idx   = int'(sts_ring_idx);
      done0 = sts_done_cnt;
      to0   = sts_timeout_cnt;
      slot_mem[idx] = req(90);
      tick(3);
      pulse_start();
      tick(20);
      write_next_seq(32'd1000);
      wait_done(done0 + 1, 5000, "NEXT_SEQ mid-round");
      check(sts_timeout_cnt == to0, "NEXT_SEQ written mid-round: this round still matched");
      sw_round_ok(91, "the round after the NEXT_SEQ write");
      check(sts_last_seq == 32'd1000, "the next round used the written seq");
    end
    begin
      int idx, moved;
      idx   = int'(sts_ring_idx);
      moved = (idx + 5) % SLOTS;
      done0 = sts_done_cnt;
      reply_mem[idx]   = '0;
      reply_mem[moved] = '0;
      slot_mem[idx] = req(92);
      tick(3);
      pulse_start();
      tick(20);
      write_ring_idx(moved);
      wait_done(done0 + 1, 5000, "RING_IDX mid-round");
      check(reply_mem[moved][63:0] == req(92)[63:0] && reply_mem[idx] == '0,
            "RING_IDX written mid-round: the reply lands in the new slot");
      check(int'(sts_ring_idx) == (moved + 1) % SLOTS, "and the ring goes on from there");
    end
    reply_delay = 50;

    $display("-- a CPU write and the FSM in the same cycle: the FSM wins");
    begin
      int idx;
      idx = int'(sts_ring_idx);
      slot_mem[idx] = req(93);
      tick(3);
      pulse_start();
      while (u_dut.state != S_DONE) @(negedge clk);
      ring_wr = 1'b1;
      ring_wdata = SLOT_AW'(9);
      @(negedge clk);
      ring_wr = 1'b0;
      check(int'(sts_ring_idx) == (idx + 1) % SLOTS, $sformatf("DONE beats a RING_IDX write (%0d)", sts_ring_idx));
    end
    begin
      int idx;
      idx   = int'(sts_ring_idx);
      seq0  = sts_next_seq;
      done0 = sts_done_cnt;
      slot_mem[idx] = req(94);
      tick(3);
      pulse_start();
      while (!(u_dut.state == S_SEND && sif_rx_tvalid && sif_rx_tready)) @(negedge clk);
      seq_wr = 1'b1;
      seq_wdata = 32'd5000;
      @(negedge clk);
      seq_wr = 1'b0;
      wait_done(done0 + 1, 5000, "NEXT_SEQ vs SEND");
      check(sts_next_seq == seq0 + 1, "SEND's advance beats a NEXT_SEQ write");
    end

    $display("-- soft_reset: back to the reset state");
    @(negedge clk);
    soft_reset = 1'b1;
    tick(3);
    check(sts_round_cnt == 0 && sts_done_cnt == 0 && sts_tx_cnt == 0 && sts_rx_cnt == 0 &&
          sts_stale_cnt == 0 && sts_timeout_cnt == 0, "counters cleared");
    check(sts_next_seq == 1 && sts_ring_idx == 0 && u_dut.state == S_IDLE && !sts_busy,
          "NEXT_SEQ 1, RING_IDX 0, IDLE");
    @(negedge clk);
    soft_reset = 1'b0;
    sw_round_ok(100, "the round after soft_reset");
    check(sts_last_seq == 1, "seq restarts at 1");

    $display("-- hw-loop: transport_demo slow to take the reply");
    @(negedge clk);
    demo_run = 1'b1;
    rounds0 = sts_round_cnt;
    demo_send(req(200));
    demo_recv(reply_delay + 40, beat);
    check(beat[63:0] == req(200)[63:0], "hw-loop reply");
    tick(3);
    check(sts_round_cnt == rounds0 + 1, "hw-loop round counted");
    check(sts_rtt >= 64'(reply_delay + 30), $sformatf("hw-loop RTT %0d includes the m_axis wait", sts_rtt));
    $display("  hw-loop RTT %0d cycles", sts_rtt);

    $display("-- hw-loop timeout: a zero beat goes back");
    drop_next = 1;
    to0 = sts_timeout_cnt;
    demo_send(req(201));
    demo_recv(0, beat);
    check(beat == '0 && sts_timeout_cnt == to0 + 1, "zero beat on timeout");

    $display("-- demo stops while the engine offers s_axis");
    tick(5);
    check(u_dut.state == S_DEMO_RX && s_axis_tready, "offering s_axis");
    @(negedge clk);
    demo_run = 1'b0;
    tick(3);
    check(u_dut.state == S_IDLE && !sts_busy && !s_axis_tready, "back in IDLE");

    $display("-- abort in the cycle s_axis hands over a request: the request is kept");
    @(negedge clk);
    demo_run = 1'b1;
    tick(3);
    check(u_dut.state == S_DEMO_RX && s_axis_tready, "offering s_axis");
    tx0 = sts_tx_cnt;
    s_axis_tdata  = req(203);
    s_axis_tvalid = 1'b1;
    abort         = 1'b1;
    @(negedge clk);                                   // the handshake and the abort, same edge
    s_axis_tvalid = 1'b0;
    abort         = 1'b0;
    demo_run      = 1'b0;                             // hwhs.v clears demo_run on abort
    tick(5);
    check(sts_tx_cnt == tx0 + 1 && last_req[63:0] == req(203)[63:0], "the request went to HSB");
    check(u_dut.state == S_IDLE && !sts_busy, "then back in IDLE");
    tick(reply_delay + 10);                           // its reply comes back stale

    $display("-- abort while transport_demo holds m_axis_tready low: the reply goes back first");
    @(negedge clk);
    demo_run = 1'b1;
    demo_send(req(204));
    while (u_dut.state != S_DEMO_FWD) @(negedge clk);
    abort = 1'b1;
    @(negedge clk);
    abort    = 1'b0;
    demo_run = 1'b0;
    tick(20);
    check(u_dut.state == S_DEMO_FWD && m_axis_tvalid, "abort waits in DEMO_FWD");
    demo_recv(0, beat);
    check(beat[63:0] == req(204)[63:0], "the reply was handed over");
    tick(3);
    check(u_dut.state == S_IDLE && !sts_busy && !s_axis_tready, "then back in IDLE");

    $display("-- RTT trace: saturation, then full");
    @(negedge clk);
    trace_clear = 1'b0;
    reply_to = 32'd0;
    reply_delay = 70000;
    sw_round_ok(300, "a 70000-cycle round");
    tick(3);
    check(sts_rtt > 64'd65535 && sts_trace_cnt == 1 && sts_trace_sat && trace_mem[0] == 16'hFFFF,
          $sformatf("RTT %0d is traced as 0xFFFF", sts_rtt));
    reply_delay = 50;
    @(negedge clk);
    reply_to = 32'd2000;
    for (int i = 0; i < 8; i++) sw_round_ok(301 + i, $sformatf("trace round %0d", i));
    tick(3);
    check(sts_trace_cnt == 8 && sts_trace_full, $sformatf("full at %0d entries, the 9th dropped", sts_trace_cnt));
    check(trace_mem[7] != 16'd0 && trace_mem[7] != 16'hFFFF, "the last entry is an RTT");

    if (errors == 0) begin
      $display("PASS");
      $finish;
    end else begin
      $fatal(1, "FAILED: %0d check(s)", errors);
    end
  end

  initial begin
    repeat (2000000) @(posedge clk);
    $fatal(1, "FAIL: global timeout");
  end
endmodule
