// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Xanadu Quantum Technologies Inc.
//
// tb_hwhs_balboa.sv - hwhs-balboa against BALBOA's own RTL.
//
//   AXI-Lite / BRAM tasks --> hwhs (registers, engine, RAMs, retransmission buffer)
//                               |  ^
//                               v  |  plain ports
//                             balboa_net (struct packing, as on the board)
//                               |  ^
//                               v  |  Coyote interfaces
//                             network_stack_sim -> roce_stack (vendored: rdma_flow,
//                               |  ^                rdma_mux_retrans, icrc, queues)
//                               v  |
//                             rocev2_model (the HLS core's user side, bit-exact AXIS words)
//                               |  ^
//                             the coprocessor: echoes each request's first 16 bytes
//
// The testbench programs hwhs the way the A72 driver does (sw/lib/hwhs/hwhs.c): addresses,
// the QP and connection contexts packed into the context registers, the engine registers;
// it then runs sw-loop rounds through the slot and reply RAMs. It checks what reaches the
// HLS core field by field (contexts, work requests, payloads), the packet that leaves
// roce_stack (icrc.sv appends 4 bytes), the ACK path through rdma_flow, rdma_flow holding
// work requests once 16 WRITEs are unacknowledged, a retransmission read back from
// hwhs_retx_ram, a READ answered with zeros, a stale WRITE, a timeout and a stack reset.
//
// Build and run with hw/sim/run.sh (Verilator). Exit status 0 = all checks passed.

`timescale 1ps / 1ps

module tb_hwhs_balboa;
  import lynxTypes::*;

  localparam int N_STATS = 13;
  localparam int SEQ_OFF = 12;
  localparam int SLOTS   = 16;     // RING_LEN for this test

  // hwhs register offsets (common/include/hwhs_regs.h)
  localparam logic [15:0] R_CTRL = 16'h000, R_STATUS = 16'h004, R_RING_IDX = 16'h008,
                          R_NEXT_SEQ = 16'h00C, R_RING_LEN = 16'h010, R_FLAGS = 16'h014,
                          R_LAST_SEQ = 16'h018, R_DONE_CNT = 16'h01C, R_REPLY_TO = 16'h030,
                          R_ROUND_CNT = 16'h034, R_RTT_LO = 16'h038, R_STALE_CNT = 16'h044,
                          R_TIMEOUT_CNT = 16'h048, R_TX_CNT = 16'h04C, R_RX_CNT = 16'h050,
                          R_NET_STATUS = 16'h064, R_NET_CTRL = 16'h080, R_NET_PEND = 16'h084,
                          R_LOCAL_IP = 16'h088, R_MAC_LO = 16'h08C, R_MAC_HI = 16'h090,
                          R_ARP_IP = 16'h094, R_LOCAL_QPN = 16'h098, R_REQ_LEN = 16'h09C,
                          R_RMT_BASE_LO = 16'h0A0, R_RMT_BASE_HI = 16'h0A4,
                          R_RMT_STRIDE = 16'h0D8, R_ACK_CNT = 16'h0DC, R_WR_CNT = 16'h0E0,
                          R_RD_CNT = 16'h0E4, R_RETX_WR = 16'h0E8, R_RETX_RD = 16'h0EC,
                          R_NET_STATS = 16'h0F0, R_QP_CTX = 16'h140, R_CONN_CTX = 16'h160;

  // the session
  localparam logic [31:0] FPGA_IP   = 32'hC0A8_0002;          // 192.168.0.2
  localparam logic [31:0] HOST_IP   = 32'hC0A8_0001;          // 192.168.0.1
  localparam logic [15:0] FPGA_QPN  = 16'd0;
  localparam logic [23:0] HOST_QPN  = 24'h00_0ABC;
  localparam logic [23:0] PSN       = 24'h12_3456;
  localparam logic [31:0] HOST_RKEY = 32'h1234_ABCD;
  localparam logic [63:0] RING_IOVA = 64'h0000_7F00_1234_0000;
  localparam logic [15:0] SRC_PORT  = 16'hC000;

  logic clk = 1'b0;
  always #2000 clk = ~clk;          // 250 MHz
  int unsigned cyc = 0;
  always @(posedge clk) cyc <= cyc + 1;

  int errors = 0;
  task automatic check(input bit cond, input string what);
    if (!cond) begin
      errors++;
      $display("  FAIL: %s", what);
    end
  endtask

  logic rstn = 1'b0, stack_rstn = 1'b0;

  // ------------------------------------------------------------------------------------------
  // DUT: hwhs + balboa_net
  // ------------------------------------------------------------------------------------------
  logic [15:0]  awaddr = '0, araddr = '0;
  logic         awvalid = 1'b0, wvalid = 1'b0, bready = 1'b1, arvalid = 1'b0, rready = 1'b1;
  logic [31:0]  wdata = '0;
  wire          awready, wready, bvalid, arready, rvalid;
  wire  [1:0]   bresp, rresp;
  wire  [31:0]  rdata;

  logic         slot_en = 1'b0, reply_en = 1'b0;
  logic [63:0]  slot_we = '0;
  logic [31:0]  slot_addr = '0, reply_addr = '0;
  logic [511:0] slot_wrdata = '0;
  wire  [511:0] slot_rddata, reply_rddata;

  wire                  set_ip_valid, set_mac_valid, arp_valid, arp_ready;
  wire                  qp_valid, qp_ready, conn_valid, conn_ready;
  wire [31:0]           set_ip, arp_ip;
  wire [47:0]           set_mac;
  wire [183:0]          qp_ctx, conn_ctx;
  wire [32*N_STATS-1:0] net_stats;
  wire                  sq_valid, sq_ready, tx_tlast, tx_tvalid, tx_tready;
  wire [15:0]           sq_qpn;
  wire [63:0]           sq_raddr;
  wire [27:0]           sq_len;
  wire [511:0]          tx_tdata;
  wire [63:0]           tx_tkeep;
  wire                  ack_valid, ack_ready;
  wire                  rd_valid, rd_ready, rd_tlast, rd_tvalid, rd_tready;
  wire [27:0]           rd_len;
  wire [511:0]          rd_tdata;
  wire [63:0]           rd_tkeep;
  wire                  wr_valid, wr_ready, wr_tlast, wr_tvalid, wr_tready;
  wire [47:0]           wr_vaddr;
  wire [27:0]           wr_len;
  wire [511:0]          wr_tdata;
  wire [63:0]           wr_tkeep;
  wire                  mem_wr_valid, mem_wr_ready, mem_wr_tlast, mem_wr_tvalid, mem_wr_tready;
  wire                  mem_rd_valid, mem_rd_ready, mem_rd_tlast, mem_rd_tvalid, mem_rd_tready;
  wire [95:0]           mem_wr_cmd, mem_rd_cmd;
  wire [511:0]          mem_wr_tdata, mem_rd_tdata;
  wire [63:0]           mem_rd_tkeep;

  wire [511:0]          net_tx_tdata;
  wire [63:0]           net_tx_tkeep;
  wire                  net_tx_tlast, net_tx_tvalid;
  wire                  net_rx_tready;

  hwhs #(
    .CLK_HZ        (250000000),
    .BUILD_ID      (32'h2611_0412),
    .TRACE_AW      (10),
    .N_STATS       (N_STATS),
    .RETX_OFFS_LSB (RDMA_MEM_SHIFT + $clog2(PMTU_BYTES))
  ) u_hwhs (
    .clk(clk), .rstn(rstn), .stack_rstn(stack_rstn),
    .s_axi_awaddr(awaddr), .s_axi_awvalid(awvalid), .s_axi_awready(awready),
    .s_axi_wdata(wdata), .s_axi_wstrb(4'hF), .s_axi_wvalid(wvalid), .s_axi_wready(wready),
    .s_axi_bresp(bresp), .s_axi_bvalid(bvalid), .s_axi_bready(bready),
    .s_axi_araddr(araddr), .s_axi_arvalid(arvalid), .s_axi_arready(arready),
    .s_axi_rdata(rdata), .s_axi_rresp(rresp), .s_axi_rvalid(rvalid), .s_axi_rready(rready),
    .slot_bram_rst(1'b0), .slot_bram_en(slot_en), .slot_bram_we(slot_we), .slot_bram_addr(slot_addr),
    .slot_bram_wrdata(slot_wrdata), .slot_bram_rddata(slot_rddata),
    .reply_bram_rst(1'b0), .reply_bram_en(reply_en), .reply_bram_we('0), .reply_bram_addr(reply_addr),
    .reply_bram_wrdata('0), .reply_bram_rddata(reply_rddata),
    .trace_bram_rst(1'b0), .trace_bram_en(1'b0), .trace_bram_we('0), .trace_bram_addr('0),
    .trace_bram_wrdata('0), .trace_bram_rddata(),
    .s_axis_tdata('0), .s_axis_tvalid(1'b0), .s_axis_tready(), .s_axis_tlast(1'b0),
    .m_axis_tdata(), .m_axis_tvalid(), .m_axis_tready(1'b1), .m_axis_tlast(),
    .start_demo(), .reset_demo(), .syn_sum(), .freq_num(), .syn_depth(), .reply_seq_off(),
    .freq_span_mask(), .lfsr_seed(), .cmp_error(1'b0), .cmp_valid(1'b0), .demo_done(1'b0),
    .set_ip_valid(set_ip_valid), .set_ip(set_ip), .set_mac_valid(set_mac_valid), .set_mac(set_mac),
    .arp_valid(arp_valid), .arp_ready(arp_ready), .arp_ip(arp_ip),
    .qp_valid(qp_valid), .qp_ready(qp_ready), .qp_ctx(qp_ctx),
    .conn_valid(conn_valid), .conn_ready(conn_ready), .conn_ctx(conn_ctx), .net_stats(net_stats),
    .sq_valid(sq_valid), .sq_ready(sq_ready), .sq_qpn(sq_qpn), .sq_raddr(sq_raddr), .sq_len(sq_len),
    .tx_tdata(tx_tdata), .tx_tkeep(tx_tkeep), .tx_tlast(tx_tlast), .tx_tvalid(tx_tvalid), .tx_tready(tx_tready),
    .ack_valid(ack_valid), .ack_ready(ack_ready),
    .rd_valid(rd_valid), .rd_ready(rd_ready), .rd_len(rd_len),
    .rd_tdata(rd_tdata), .rd_tkeep(rd_tkeep), .rd_tlast(rd_tlast), .rd_tvalid(rd_tvalid), .rd_tready(rd_tready),
    .wr_valid(wr_valid), .wr_ready(wr_ready),
    .wr_tdata(wr_tdata), .wr_tkeep(wr_tkeep), .wr_tlast(wr_tlast), .wr_tvalid(wr_tvalid), .wr_tready(wr_tready),
    .mem_wr_valid(mem_wr_valid), .mem_wr_ready(mem_wr_ready), .mem_wr_cmd(mem_wr_cmd),
    .mem_wr_tdata(mem_wr_tdata), .mem_wr_tlast(mem_wr_tlast), .mem_wr_tvalid(mem_wr_tvalid), .mem_wr_tready(mem_wr_tready),
    .mem_rd_valid(mem_rd_valid), .mem_rd_ready(mem_rd_ready), .mem_rd_cmd(mem_rd_cmd),
    .mem_rd_tdata(mem_rd_tdata), .mem_rd_tkeep(mem_rd_tkeep), .mem_rd_tlast(mem_rd_tlast),
    .mem_rd_tvalid(mem_rd_tvalid), .mem_rd_tready(mem_rd_tready),
    .net_status({31'd0, stack_rstn})
  );

  balboa_net #(.N_STATS(N_STATS)) u_net (
    .clk(clk), .rstn(stack_rstn),
    .rx_tdata('0), .rx_tkeep('0), .rx_tlast(1'b0), .rx_tvalid(1'b0), .rx_tready(net_rx_tready),
    .tx_tdata(net_tx_tdata), .tx_tkeep(net_tx_tkeep), .tx_tlast(net_tx_tlast), .tx_tvalid(net_tx_tvalid),
    .tx_tready(1'b1),
    .set_ip_valid(set_ip_valid), .set_ip(set_ip), .set_mac_valid(set_mac_valid), .set_mac(set_mac),
    .arp_valid(arp_valid), .arp_ready(arp_ready), .arp_ip(arp_ip),
    .qp_valid(qp_valid), .qp_ready(qp_ready), .qp_ctx(qp_ctx),
    .conn_valid(conn_valid), .conn_ready(conn_ready), .conn_ctx(conn_ctx), .stats(net_stats),
    .sq_valid(sq_valid), .sq_ready(sq_ready), .sq_qpn(sq_qpn), .sq_raddr(sq_raddr), .sq_len(sq_len),
    .sq_tdata(tx_tdata), .sq_tkeep(tx_tkeep), .sq_tlast(tx_tlast), .sq_tvalid(tx_tvalid), .sq_tready(tx_tready),
    .ack_valid(ack_valid), .ack_ready(ack_ready),
    .rd_valid(rd_valid), .rd_ready(rd_ready), .rd_len(rd_len),
    .rd_tdata(rd_tdata), .rd_tkeep(rd_tkeep), .rd_tlast(rd_tlast), .rd_tvalid(rd_tvalid), .rd_tready(rd_tready),
    .wr_valid(wr_valid), .wr_ready(wr_ready), .wr_vaddr(wr_vaddr), .wr_len(wr_len),
    .wr_tdata(wr_tdata), .wr_tkeep(wr_tkeep), .wr_tlast(wr_tlast), .wr_tvalid(wr_tvalid), .wr_tready(wr_tready),
    .mem_wr_valid(mem_wr_valid), .mem_wr_ready(mem_wr_ready), .mem_wr_cmd(mem_wr_cmd),
    .mem_wr_tdata(mem_wr_tdata), .mem_wr_tlast(mem_wr_tlast), .mem_wr_tvalid(mem_wr_tvalid), .mem_wr_tready(mem_wr_tready),
    .mem_rd_valid(mem_rd_valid), .mem_rd_ready(mem_rd_ready), .mem_rd_cmd(mem_rd_cmd),
    .mem_rd_tdata(mem_rd_tdata), .mem_rd_tkeep(mem_rd_tkeep), .mem_rd_tlast(mem_rd_tlast),
    .mem_rd_tvalid(mem_rd_tvalid), .mem_rd_tready(mem_rd_tready)
  );

  // the HLS model inside roce_stack
  `define MODEL u_net.u_stack.inst_roce_stack.rocev2_inst

  // ------------------------------------------------------------------------------------------
  // Bus tasks: AXI-Lite and the axi_bram_ctrl side of the slot and reply RAMs. Inputs change
  // on the falling edge.
  // ------------------------------------------------------------------------------------------
  task automatic tick(input int n);
    repeat (n) @(negedge clk);
  endtask

  task automatic reg_wr(input logic [15:0] a, input logic [31:0] d);
    @(negedge clk);
    awaddr = a; awvalid = 1'b1; wdata = d; wvalid = 1'b1;
    fork
      begin while (!awready) @(negedge clk); @(negedge clk); awvalid = 1'b0; end
      begin while (!wready) @(negedge clk); @(negedge clk); wvalid = 1'b0; end
    join
    while (!bvalid) @(negedge clk);
    @(negedge clk);
  endtask

  task automatic reg_rd(input logic [15:0] a, output logic [31:0] d);
    @(negedge clk);
    araddr = a; arvalid = 1'b1;
    while (!arready) @(negedge clk);
    @(negedge clk);
    arvalid = 1'b0;
    while (!rvalid) @(negedge clk);
    d = rdata;
    @(negedge clk);
  endtask

  task automatic slot_write(input int idx, input logic [511:0] d);
    @(negedge clk);
    slot_en = 1'b1; slot_we = '1; slot_addr = 32'(idx * 64); slot_wrdata = d;
    @(negedge clk);
    slot_en = 1'b0; slot_we = '0;
  endtask

  task automatic reply_read(input int idx, output logic [511:0] d);
    @(negedge clk);
    reply_en = 1'b1; reply_addr = 32'(idx * 64);
    tick(3);                       // hwhs_ram READ_LATENCY 2
    d = reply_rddata;
    reply_en = 1'b0;
  endtask

  // ------------------------------------------------------------------------------------------
  // The contexts, packed as sw/lib/hwhs/hwhs.c packs them (rdma_qp_ctx_t, rdma_qp_conn_t)
  // ------------------------------------------------------------------------------------------
  function automatic logic [31:0] lsb_first(input logic [31:0] ip);
    return {ip[7:0], ip[15:8], ip[23:16], ip[31:24]};
  endfunction

  function automatic logic [183:0] qp_words();
    automatic logic [183:0] q = '0;
    q[31:0]    = 32'd0;            // new_state, as Coyote's driver writes it
    q[55:32]   = 24'(FPGA_QPN);
    q[79:56]   = PSN;              // remote_psn: the PSN the stack sends with
    q[103:80]  = PSN;              // local_psn: the PSN it expects
    q[135:104] = HOST_RKEY;
    q[183:136] = RING_IOVA[47:0];
    return q;
  endfunction

  function automatic logic [183:0] conn_words();
    automatic logic [183:0] c = '0;
    c[15:0]    = FPGA_QPN;
    c[39:16]   = HOST_QPN;
    c[167:40]  = {4{lsb_first(HOST_IP)}};
    c[183:168] = SRC_PORT;
    return c;
  endfunction

  task automatic write_ctx(input logic [15:0] base, input logic [183:0] v);
    for (int i = 0; i < 6; i++) reg_wr(base + 16'(4 * i), (i == 5) ? {8'd0, v[160 +: 24]} : v[32*i +: 32]);
  endtask

  // ------------------------------------------------------------------------------------------
  // The coprocessor: for each WRITE that reaches the stack, the reply is a WRITE of the
  // request's first 16 bytes ({u64 value; u32 decoder_id; u32 seq}) after reply_delay cycles,
  // to address 0 of the FPGA. drop_next requests get no reply.
  // ------------------------------------------------------------------------------------------
  int reply_delay = 60, drop_next = 0, seen = 0;
  always @(negedge clk) begin
    if (`MODEL.sent.size() > seen) begin
      logic [511:0] req, rep;
      req = `MODEL.sent[seen].payload;
      seen++;
      if (drop_next > 0) begin
        drop_next--;
      end else begin
        rep = '0;
        rep[127:0] = req[127:0];
        fork
          automatic logic [511:0] r = rep;
          begin
            tick(reply_delay);
            `MODEL.inject_write(48'd0, r, 16, 1);
          end
        join_none
      end
    end
  end

  // frames leaving balboa_net: bytes per frame
  int frame_bytes [$];
  logic [7:0] frame_first [$];       // first payload byte of each frame (IP version/IHL)
  int cur_bytes = 0, fb;
  always @(negedge clk) begin                 // tready is always 1: every valid is a beat
    if (net_tx_tvalid) begin
      fb = cur_bytes;
      for (int i = 0; i < 64; i++) fb += int'(net_tx_tkeep[i]);
      if (cur_bytes == 0) frame_first.push_back(net_tx_tdata[7:0]);
      if (net_tx_tlast) begin
        frame_bytes.push_back(fb);
        cur_bytes = 0;
      end else begin
        cur_bytes = fb;
      end
    end
  end

  // ------------------------------------------------------------------------------------------
  // Rounds, the way hwhs_round() runs them
  // ------------------------------------------------------------------------------------------
  function automatic logic [511:0] req(input int i);
    automatic logic [511:0] q = '0;
    q[63:0]    = 64'hCAFE_0000_0000_0000 + 64'(i);
    q[159:128] = 32'(i);
    return q;
  endfunction

  task automatic sw_round(input int i, output bit ok, output bit timed_out, output logic [511:0] rep);
    logic [31:0] st, done0, to0, idx;
    reg_rd(R_RING_IDX, idx);
    reg_rd(R_TIMEOUT_CNT, to0);
    reg_rd(R_STATUS, st);
    done0 = {24'd0, st[31:24]};
    slot_write(int'(idx), req(i));
    reg_wr(R_CTRL, 32'h1);
    ok = 1'b0;
    for (int t = 0; t < 2000; t++) begin
      reg_rd(R_STATUS, st);
      if ({24'd0, st[31:24]} != done0) begin
        ok = 1'b1;
        break;
      end
    end
    begin
      logic [31:0] to1;
      reg_rd(R_TIMEOUT_CNT, to1);
      timed_out = (to1 != to0);
    end
    reply_read(int'(idx), rep);
  endtask

  // ------------------------------------------------------------------------------------------
  // Test
  // ------------------------------------------------------------------------------------------
  initial begin
    logic [31:0]  v, v2;
    logic [511:0] rep;
    bit           ok, to;

    tick(5);
    rstn = 1'b1;
    tick(5);
    stack_rstn = 1'b1;
    tick(20);

    $display("-- addresses and an ARP lookup");
    reg_wr(R_LOCAL_IP, FPGA_IP);
    reg_wr(R_MAC_LO, 32'hC648_4D8E);
    reg_wr(R_MAC_HI, 32'h0000_0293);
    reg_wr(R_ARP_IP, HOST_IP);
    reg_wr(R_NET_CTRL, 32'h7);                       // set_ip, set_mac, arp_lookup
    tick(5);
    reg_rd(R_NET_PEND, v);
    check(v == 0, $sformatf("NET_PEND 0x%0h after the pushes", v));
    check(u_net.u_stack.ip_addr == lsb_first(FPGA_IP), "the stack has the IP, first octet in [7:0]");
    check(u_net.u_stack.mac_addr == 48'h0293_C648_4D8E, "the stack got the MAC");
    check(u_net.u_stack.arp_lookups == 1, "one ARP lookup");
    check(u_net.arp_lookup.data == lsb_first(HOST_IP), "ARP lookup address in network order");

    $display("-- QP and connection contexts");
    write_ctx(R_QP_CTX, qp_words());
    write_ctx(R_CONN_CTX, conn_words());
    reg_wr(R_NET_CTRL, 32'h18);                      // qp_push, conn_push
    tick(5);
    reg_rd(R_NET_PEND, v);
    check(v == 0, "contexts taken");
    check(`MODEL.qp_pushes == 1 && `MODEL.conn_pushes == 1, "one push each");
    check(`MODEL.qp_num == 24'(FPGA_QPN) && `MODEL.remote_psn == PSN && `MODEL.local_psn == PSN &&
          `MODEL.r_key == HOST_RKEY && `MODEL.qp_ctx[183:136] == RING_IOVA[47:0],
          "qpContext: qp_num, PSNs, r_key, address");
    check(`MODEL.qp_ctx[31:0] == 0, "qpContext: newState 0");
    check(`MODEL.conn_qpn == FPGA_QPN && `MODEL.remote_qpn == HOST_QPN &&
          `MODEL.conn_ctx[167:40] == {4{lsb_first(HOST_IP)}} && `MODEL.src_port == SRC_PORT,
          "ifConnReq: qpn, remote qpn, remote IP (x4, first octet in [7:0]), UDP port");

    $display("-- the engine");
    reg_wr(R_CTRL, 32'h4);                           // soft reset
    reg_wr(R_CTRL, 32'h0);
    reg_wr(R_REPLY_TO, 32'd20000);
    reg_wr(R_RING_LEN, SLOTS);
    reg_wr(R_FLAGS, 32'h1);
    reg_wr(R_LOCAL_QPN, 32'(FPGA_QPN));
    reg_wr(R_REQ_LEN, 32'd64);
    reg_wr(R_RMT_BASE_LO, RING_IOVA[31:0]);
    reg_wr(R_RMT_BASE_HI, RING_IOVA[63:32]);
    reg_wr(R_RMT_STRIDE, 32'd6);

    $display("-- sw-loop rounds through BALBOA");
    for (int i = 0; i < 3; i++) begin
      sw_round(i, ok, to, rep);
      check(ok && !to, $sformatf("round %0d finished with its reply", i));
      check(rep[63:0] == req(i)[63:0] && rep[127:96] == 32'(i + 1),
            $sformatf("round %0d: reply value 0x%0h and seq %0d", i, rep[63:0], rep[127:96]));
    end
    tick(100);
    check(`MODEL.sent.size() == 3, $sformatf("three WRITEs reached the stack (%0d)", `MODEL.sent.size()));
    for (int i = 0; i < 3 && i < `MODEL.sent.size(); i++) begin
      check(`MODEL.sent[i].op == 32'h0A && `MODEL.sent[i].qpn == FPGA_QPN && `MODEL.sent[i].lst &&
            !`MODEL.sent[i].host && `MODEL.sent[i].len == 64 && `MODEL.sent[i].imm == 0,
            $sformatf("txMeta %0d: WRITE_ONLY, qpn, lst, len 64", i));
      check(`MODEL.sent[i].raddr == RING_IOVA + 64'(i * 64),
            $sformatf("txMeta %0d: raddr 0x%0h is slot %0d", i, `MODEL.sent[i].raddr, i));
      check(`MODEL.sent[i].offs == 6'(i), $sformatf("txMeta %0d: offs %0d from rdma_flow", i, `MODEL.sent[i].offs));
      check(`MODEL.sent[i].payload[63:0] == req(i)[63:0] &&
            `MODEL.sent[i].payload[SEQ_OFF*8 +: 32] == 32'(i + 1), $sformatf("payload %0d, seq stamped", i));
    end
    check(frame_bytes.size() == 3, $sformatf("three frames left roce_stack (%0d)", frame_bytes.size()));
    for (int i = 0; i < frame_bytes.size(); i++)
      check(frame_bytes[i] == 20 + 8 + 12 + 16 + 64 + 4 && frame_first[i] == 8'h45,
            $sformatf("frame %0d: IPv4, %0d bytes = headers + 64 + ICRC", i, frame_bytes[i]));
    reg_rd(R_ACK_CNT, v);
    check(v == 3, $sformatf("ACK_CNT %0d: the ACKs came through rdma_flow", v));
    reg_rd(R_RETX_WR, v);
    check(v == 3, $sformatf("RETX_WR %0d: a copy of every payload", v));
    reg_rd(R_TX_CNT, v);
    reg_rd(R_RX_CNT, v2);
    check(v == 3 && v2 == 3, "TX_CNT 3, RX_CNT 3");

    $display("-- a retransmission reads the copy back from hwhs_retx_ram");
    `MODEL.retransmit(6'd1, 28'd64);
    tick(100);
    check(`MODEL.retx_ok == 1 && `MODEL.retx_bad == 0, "the copy of the WRITE on offs 1");
    reg_rd(R_RETX_RD, v);
    check(v == 1, "RETX_RD 1");

    $display("-- a READ from the coprocessor is answered with zeros");
    `MODEL.remote_read(28'd100);
    tick(100);
    check(`MODEL.reads_done == 1 && `MODEL.read_beats == 2 && `MODEL.read_nonzero == 0, "two zero beats");
    reg_rd(R_RD_CNT, v);
    check(v == 1, "RD_CNT 1");

    $display("-- a WRITE that matches no round is stale");
    reg_rd(R_STALE_CNT, v);
    `MODEL.inject_write(48'd0, 512'hDEAD, 16, 1);
    tick(50);
    reg_rd(R_STALE_CNT, v2);
    check(v2 == v + 1, "STALE_CNT +1");

    $display("-- 16 WRITEs unacknowledged: rdma_flow holds the 17th until an ACK");
    `MODEL.auto_ack = 1'b0;
    reply_delay = 40;
    for (int i = 0; i < 16; i++) begin
      sw_round(100 + i, ok, to, rep);
      check(ok && !to, $sformatf("unacked round %0d", i));
    end
    begin
      logic [31:0] st;
      int n0;
      n0 = `MODEL.sent.size();
      reg_rd(R_RING_IDX, v);
      slot_write(int'(v), req(200));
      reg_wr(R_CTRL, 32'h1);
      tick(200);
      reg_rd(R_STATUS, st);
      check(`MODEL.sent.size() == n0 && st[8], $sformatf("17th held: post owed (STATUS 0x%0h)", st));
      `MODEL.auto_ack = 1'b1;
      while (`MODEL.ack_w_pending.size() > 0) begin
        `MODEL.ack_due.push_back(`MODEL.now);
        `MODEL.ack_w.push_back(`MODEL.ack_w_pending.pop_front());
      end
      tick(400);
      check(`MODEL.sent.size() == n0 + 1, "17th went out after the ACKs");
      reg_rd(R_ROUND_CNT, v);
      check(v == 3 + 16 + 1, $sformatf("ROUND_CNT %0d", v));
    end

    $display("-- a lost reply times out");
    drop_next = 1;
    reg_wr(R_REPLY_TO, 32'd2000);
    sw_round(300, ok, to, rep);
    check(ok && to, "the round ended with a timeout");
    sw_round(301, ok, to, rep);
    check(ok && !to && rep[63:0] == req(301)[63:0], "the next round is fine");

    $display("-- a stack reset resets the engine, the registers stay");
    @(negedge clk);
    stack_rstn = 1'b0;
    tick(10);
    stack_rstn = 1'b1;
    tick(10);
    reg_rd(R_ROUND_CNT, v);
    check(v == 0, "engine counters cleared");
    reg_rd(R_LOCAL_QPN, v);
    reg_rd(R_RMT_STRIDE, v2);
    check(v == 32'(FPGA_QPN) && v2 == 6, "registers kept");
    reg_rd(R_NET_STATS + 16'd4, v);
    check(v == 0, $sformatf("the stack's counters start over (tx frames %0d)", v));

    if (errors == 0) begin
      $display("PASS");
      $finish;
    end else begin
      $fatal(1, "FAILED: %0d check(s)", errors);
    end
  end

  initial begin
    repeat (400000) @(posedge clk);
    $fatal(1, "FAIL: global timeout");
  end
endmodule
