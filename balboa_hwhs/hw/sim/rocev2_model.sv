// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Xanadu Quantum Technologies Inc.
//
// rocev2_model.sv - behavioral stand-in for BALBOA's rocev2 HLS core, user side only.
// Simulation only (tb_hwhs_balboa); the module has the HLS core's name and ports.
//
// Every word on the HLS core's AXI-Stream ports is packed as Vitis HLS packs the structs
// (aggregate compact=bit, first member in the low bits), as fpga-network-stack's
// ib_transport_protocol.hpp declares them:
//
//   txMeta     [31:0] op_code [47:32] qpn [48] host [49] lst [55:50] offs [119:56] raddr
//              [183:120] laddr [215:184] len [247:216] imm
//   memCmd     [31:0] op_code [47:32] qpn [48] lst [96:49] addr [100:97] dst [102:101] strm
//              [130:103] len [131] actv [132] host [138:133] offs
//   ackMeta    [31:0] op_code [47:32] qpn [48] host [52:49] dst [54:53] strm [55] lst
//   qpContext  [31:0] newState [55:32] qp_num [79:56] remote_psn [103:80] local_psn
//              [135:104] r_key [183:136] virtual_address
//   ifConnReq  [15:0] qpn [39:16] remote_qpn [167:40] remote_ip_address [183:168] remote_udp_port
//
// What it does, the way the HLS core does it toward roce_stack:
//   - a work request (RC_RDMA_WRITE_ONLY) -> a memory read command with actv = 1, then the
//     payload is read, then a packet goes out on m_axis_tx_data and, ack_delay cycles later,
//     the coprocessor's ACK comes back on m_axis_rx_ack_meta (lst = 1)
//   - retransmit(offs, len): a memory read command with actv = 0 for a WRITE sent before;
//     the data read back must be that WRITE's payload
//   - remote_read(len): a memory read command for an RDMA READ response (actv = 1)
//   - inject_write(...): a WRITE from the coprocessor: memory write command, then its data
// The packet on m_axis_tx_data is the IPv4/UDP/BTH/RETH header the core would put in front
// of the payload (no ICRC: roce_stack's icrc.sv appends it).
//
// The queues are only touched in procedural code; every output is a register loaded from
// them, so no continuous assignment depends on a queue (Verilator does not re-evaluate those
// when a queue changes).

`timescale 1ns / 1ps

module rocev2_ip (
  input  logic         ap_clk,
  input  logic         ap_rst_n,
  output logic         m_axis_dbg_0_TVALID,
  input  logic         m_axis_dbg_0_TREADY,
  output logic [103:0] m_axis_dbg_0_TDATA,
  output logic         m_axis_dbg_1_TVALID,
  input  logic         m_axis_dbg_1_TREADY,
  output logic [103:0] m_axis_dbg_1_TDATA,
  output logic         m_axis_dbg_2_TVALID,
  input  logic         m_axis_dbg_2_TREADY,
  output logic [103:0] m_axis_dbg_2_TDATA,
  input  logic         s_axis_rx_data_TVALID,
  output logic         s_axis_rx_data_TREADY,
  input  logic [511:0] s_axis_rx_data_TDATA,
  input  logic [63:0]  s_axis_rx_data_TKEEP,
  input  logic         s_axis_rx_data_TLAST,
  output logic         m_axis_tx_data_TVALID,
  input  logic         m_axis_tx_data_TREADY,
  output logic [511:0] m_axis_tx_data_TDATA,
  output logic [63:0]  m_axis_tx_data_TKEEP,
  output logic         m_axis_tx_data_TLAST,
  input  logic         s_axis_sq_meta_TVALID,
  output logic         s_axis_sq_meta_TREADY,
  input  logic [247:0] s_axis_sq_meta_TDATA,
  output logic         m_axis_mem_write_cmd_TVALID,
  input  logic         m_axis_mem_write_cmd_TREADY,
  output logic [143:0] m_axis_mem_write_cmd_TDATA,
  output logic         m_axis_mem_read_cmd_TVALID,
  input  logic         m_axis_mem_read_cmd_TREADY,
  output logic [143:0] m_axis_mem_read_cmd_TDATA,
  output logic         m_axis_mem_write_data_TVALID,
  input  logic         m_axis_mem_write_data_TREADY,
  output logic [511:0] m_axis_mem_write_data_TDATA,
  output logic [63:0]  m_axis_mem_write_data_TKEEP,
  output logic         m_axis_mem_write_data_TLAST,
  input  logic         s_axis_mem_read_data_TVALID,
  output logic         s_axis_mem_read_data_TREADY,
  input  logic [511:0] s_axis_mem_read_data_TDATA,
  input  logic [63:0]  s_axis_mem_read_data_TKEEP,
  input  logic         s_axis_mem_read_data_TLAST,
  input  logic         s_axis_qp_interface_TVALID,
  output logic         s_axis_qp_interface_TREADY,
  input  logic [183:0] s_axis_qp_interface_TDATA,
  input  logic         s_axis_qp_conn_interface_TVALID,
  output logic         s_axis_qp_conn_interface_TREADY,
  input  logic [183:0] s_axis_qp_conn_interface_TDATA,
  output logic         m_axis_rx_ack_meta_TVALID,
  input  logic         m_axis_rx_ack_meta_TREADY,
  output logic [55:0]  m_axis_rx_ack_meta_TDATA,
  input  logic [127:0] local_ip_address,
  output logic [31:0]  regIbvCountRx,
  output logic         regIbvCountRx_ap_vld,
  output logic [31:0]  regIbvCountTx,
  output logic         regIbvCountTx_ap_vld,
  output logic [31:0]  regCrcDropPkgCount,
  output logic         regCrcDropPkgCount_ap_vld,
  output logic [31:0]  regInvalidPsnDropCount,
  output logic         regInvalidPsnDropCount_ap_vld,
  output logic [31:0]  regRetransCount,
  output logic         regRetransCount_ap_vld
);

  localparam logic [31:0] OP_WRITE_ONLY     = 32'h0A;
  localparam logic [31:0] OP_READ_RESP_ONLY = 32'h10;
  localparam logic [31:0] OP_ACK            = 32'h11;

  // ---- contexts, as the stack received them ---------------------------------------------
  logic [183:0] qp_ctx, conn_ctx;
  int           qp_pushes = 0, conn_pushes = 0;
  wire  [23:0]  qp_num     = qp_ctx[55:32];
  wire  [23:0]  remote_psn = qp_ctx[79:56];     // the PSN this side sends with (state table)
  wire  [23:0]  local_psn  = qp_ctx[103:80];    // the PSN this side expects
  wire  [31:0]  r_key      = qp_ctx[135:104];
  wire  [15:0]  conn_qpn   = conn_ctx[15:0];
  wire  [23:0]  remote_qpn = conn_ctx[39:16];
  wire  [31:0]  remote_ip  = conn_ctx[71:40];   // first word of the four; first octet in [7:0]
  wire  [15:0]  src_port   = conn_ctx[183:168];

  assign s_axis_qp_interface_TREADY      = 1'b1;
  assign s_axis_qp_conn_interface_TREADY = 1'b1;
  always @(posedge ap_clk) begin
    if (s_axis_qp_interface_TVALID) begin
      qp_ctx <= s_axis_qp_interface_TDATA;
      qp_pushes <= qp_pushes + 1;
    end
    if (s_axis_qp_conn_interface_TVALID) begin
      conn_ctx <= s_axis_qp_conn_interface_TDATA;
      conn_pushes <= conn_pushes + 1;
    end
  end

  // ---- unused outputs ---------------------------------------------------------------------
  assign m_axis_dbg_0_TVALID = 1'b0;
  assign m_axis_dbg_0_TDATA  = '0;
  assign m_axis_dbg_1_TVALID = 1'b0;
  assign m_axis_dbg_1_TDATA  = '0;
  assign m_axis_dbg_2_TVALID = 1'b0;
  assign m_axis_dbg_2_TDATA  = '0;
  assign regIbvCountRx = '0;
  assign regIbvCountRx_ap_vld = 1'b0;
  assign regIbvCountTx = '0;
  assign regIbvCountTx_ap_vld = 1'b0;
  assign regCrcDropPkgCount = '0;
  assign regCrcDropPkgCount_ap_vld = 1'b0;
  assign regInvalidPsnDropCount = '0;
  assign regInvalidPsnDropCount_ap_vld = 1'b0;
  assign regRetransCount = '0;
  assign regRetransCount_ap_vld = 1'b0;

  int rx_frames = 0;
  assign s_axis_rx_data_TREADY = 1'b1;
  always @(posedge ap_clk)
    if (s_axis_rx_data_TVALID && s_axis_rx_data_TLAST) rx_frames <= rx_frames + 1;

  // ---- packing --------------------------------------------------------------------------
  function automatic logic [143:0] mem_cmd(input logic [31:0] op, input logic [15:0] qpn,
                                           input logic [47:0] addr, input logic [27:0] len,
                                           input logic actv, input logic [5:0] offs);
    automatic logic [143:0] c = '0;
    c[31:0]    = op;
    c[47:32]   = qpn;
    c[48]      = 1'b1;          // lst
    c[96:49]   = addr;
    c[100:97]  = 4'h0;          // dst: addr[51:48] of a 64-bit address
    c[102:101] = 2'b00;         // strm
    c[130:103] = len;
    c[131]     = actv;
    c[132]     = 1'b0;          // host
    c[138:133] = offs;
    return c;
  endfunction

  // ---- what the testbench reads back ----------------------------------------------------
  typedef struct {
    logic [15:0]  qpn;
    logic         host, lst;
    logic [5:0]   offs;
    logic [63:0]  raddr, laddr;
    logic [31:0]  len, imm, op;
    logic [23:0]  psn;
    logic [511:0] payload;      // the first beat
  } wqe_t;

  wqe_t        sent [$];                 // every WRITE that went out, in order
  logic [511:0] sent_by_offs [64];       // the payload of the last WRITE on each offs
  int          retx_ok = 0, retx_bad = 0, read_beats = 0, read_nonzero = 0, reads_done = 0;
  int          tx_pkts = 0, acks_sent = 0;
  int          ack_delay = 30;
  bit          auto_ack = 1'b1;

  // ---- work requests -> memory read commands ----------------------------------------------
  typedef enum {K_TX, K_RETX, K_READ} kind_t;
  typedef struct {
    kind_t       kind;
    wqe_t        w;
    int          beats;
  } rd_job_t;

  logic [143:0] rd_cmd_q [$];
  rd_job_t      rd_job_q [$];
  logic [23:0]  next_psn;
  bit           psn_init = 1'b0;

  assign s_axis_sq_meta_TREADY = ap_rst_n;

  always @(posedge ap_clk) begin
    if (s_axis_sq_meta_TVALID && s_axis_sq_meta_TREADY) begin
      automatic wqe_t         w;
      automatic rd_job_t      j;
      automatic logic [247:0] d = s_axis_sq_meta_TDATA;
      w.op    = d[31:0];
      w.qpn   = d[47:32];
      w.host  = d[48];
      w.lst   = d[49];
      w.offs  = d[55:50];
      w.raddr = d[119:56];
      w.laddr = d[183:120];
      w.len   = d[215:184];
      w.imm   = d[247:216];
      if (!psn_init) begin
        w.psn = remote_psn;
        next_psn <= remote_psn + 24'd1;
        psn_init <= 1'b1;
      end else begin
        w.psn = next_psn;
        next_psn <= next_psn + 24'd1;
      end
      j.kind  = K_TX;
      j.w     = w;
      j.beats = (int'(w.len) + 63) / 64;
      rd_cmd_q.push_back(mem_cmd(w.op, w.qpn, w.laddr[47:0], w.len[27:0], 1'b1, w.offs));
      rd_job_q.push_back(j);
    end
  end

  task automatic retransmit(input logic [5:0] offs, input logic [27:0] len);
    rd_job_t j;
    j.kind   = K_RETX;
    j.w.offs = offs;
    j.beats  = (int'(len) + 63) / 64;
    rd_cmd_q.push_back(mem_cmd(OP_WRITE_ONLY, qp_num[15:0], 48'd0, len, 1'b0, offs));
    rd_job_q.push_back(j);
  endtask

  task automatic remote_read(input logic [27:0] len);
    rd_job_t j;
    j.kind  = K_READ;
    j.beats = (int'(len) + 63) / 64;
    rd_cmd_q.push_back(mem_cmd(OP_READ_RESP_ONLY, qp_num[15:0], 48'h1000, len, 1'b1, 6'd0));
    rd_job_q.push_back(j);
  endtask

  initial m_axis_mem_read_cmd_TVALID = 1'b0;
  always @(posedge ap_clk)
    if (!m_axis_mem_read_cmd_TVALID || m_axis_mem_read_cmd_TREADY) begin
      m_axis_mem_read_cmd_TVALID <= (rd_cmd_q.size() > 0);
      if (rd_cmd_q.size() > 0) m_axis_mem_read_cmd_TDATA <= rd_cmd_q.pop_front();
    end

  // ---- memory read data: the payload of a WRITE, a retransmission copy, a READ response ----
  int           rd_beat = 0;
  logic [511:0] rd_first;
  logic [7:0]   tx_hdr [$];

  typedef struct {
    logic [511:0] data;
    logic [63:0]  keep;
    logic         last;
  } beat_t;
  beat_t        tx_q [$];
  int unsigned  ack_due [$];
  wqe_t         ack_w [$];
  wqe_t         ack_w_pending [$];   // WRITEs not acknowledged while auto_ack is off
  int unsigned  now = 0;
  always @(posedge ap_clk) now <= now + 1;

  function automatic void put_be(ref logic [7:0] b [$], input logic [63:0] v, input int n);
    for (int i = n - 1; i >= 0; i--) b.push_back(v[i*8 +: 8]);
  endfunction

  // IPv4 (20) + UDP (8) + BTH (12) + RETH (16) in front of the payload
  function automatic void build_packet(input wqe_t w);
    automatic logic [7:0] b [$];
    automatic int         ip_len = 20 + 8 + 12 + 16 + int'(w.len) + 4;   // + ICRC
    automatic beat_t      x;
    b.push_back(8'h45); b.push_back(8'h02);
    put_be(b, 64'(ip_len), 2);
    put_be(b, 0, 4);                                    // id, flags
    b.push_back(8'd64); b.push_back(8'd17);             // TTL, UDP
    put_be(b, 0, 2);                                    // checksum (not modelled)
    put_be(b, {32'd0, local_ip_address[7:0], local_ip_address[15:8], local_ip_address[23:16],
               local_ip_address[31:24]}, 4);
    put_be(b, {32'd0, remote_ip[7:0], remote_ip[15:8], remote_ip[23:16], remote_ip[31:24]}, 4);
    put_be(b, 64'(src_port), 2);
    put_be(b, 64'd4791, 2);
    put_be(b, 64'(ip_len - 20), 2);
    put_be(b, 0, 2);
    b.push_back(8'h0A); b.push_back(8'h40);             // BTH: WRITE_ONLY, solicited/pad 0
    put_be(b, 64'hFFFF, 2);                             // P_Key
    put_be(b, {32'd0, 8'd0, remote_qpn}, 4);
    put_be(b, {32'd0, 8'h80, w.psn}, 4);                // AckReq, PSN
    put_be(b, w.raddr, 8);
    put_be(b, 64'(r_key), 4);
    put_be(b, 64'(w.len), 4);
    for (int i = 0; i < int'(w.len); i++) b.push_back(w.payload[i*8 +: 8]);
    for (int i = 0; i < b.size(); i += 64) begin
      x.data = '0;
      x.keep = '0;
      for (int k = 0; k < 64 && i + k < b.size(); k++) begin
        x.data[k*8 +: 8] = b[i+k];
        x.keep[k] = 1'b1;
      end
      x.last = (i + 64 >= b.size());
      tx_q.push_back(x);
    end
  endfunction

  int rd_jobs = 0;                       // rd_job_q.size(), as a plain variable
  always @(posedge ap_clk) rd_jobs <= rd_job_q.size();
  assign s_axis_mem_read_data_TREADY = (rd_jobs > 0);

  always @(posedge ap_clk) begin
    if (s_axis_mem_read_data_TVALID && s_axis_mem_read_data_TREADY) begin
      automatic rd_job_t j = rd_job_q[0];
      if (rd_beat == 0) rd_first = s_axis_mem_read_data_TDATA;
      if (j.kind == K_READ) begin
        read_beats <= read_beats + 1;
        if (s_axis_mem_read_data_TDATA != '0) read_nonzero <= read_nonzero + 1;
      end
      if (rd_beat + 1 == j.beats) begin
        automatic logic [511:0] first = (rd_beat == 0) ? s_axis_mem_read_data_TDATA : rd_first;
        if (!s_axis_mem_read_data_TLAST) $error("rocev2 model: the last beat of a read has no TLAST");
        case (j.kind)
          K_TX: begin
            automatic wqe_t w = j.w;
            w.payload = first;
            sent.push_back(w);
            sent_by_offs[w.offs] = first;
            build_packet(w);
            tx_pkts <= tx_pkts + 1;
            if (auto_ack) begin
              ack_due.push_back(now + 32'(ack_delay));
              ack_w.push_back(w);
            end else begin
              ack_w_pending.push_back(w);
            end
          end
          K_RETX: begin
            if (first == sent_by_offs[j.w.offs]) retx_ok <= retx_ok + 1;
            else retx_bad <= retx_bad + 1;
          end
          K_READ: reads_done <= reads_done + 1;
          default: ;
        endcase
        void'(rd_job_q.pop_front());
        rd_beat <= 0;
      end else begin
        rd_beat <= rd_beat + 1;
      end
    end
  end

  // ---- the packet out --------------------------------------------------------------------
  initial m_axis_tx_data_TVALID = 1'b0;
  always @(posedge ap_clk)
    if (!m_axis_tx_data_TVALID || m_axis_tx_data_TREADY) begin
      m_axis_tx_data_TVALID <= (tx_q.size() > 0);
      if (tx_q.size() > 0) begin
        automatic beat_t x = tx_q.pop_front();
        m_axis_tx_data_TDATA <= x.data;
        m_axis_tx_data_TKEEP <= x.keep;
        m_axis_tx_data_TLAST <= x.last;
      end
    end

  // ---- the coprocessor's ACKs -------------------------------------------------------------
  initial m_axis_rx_ack_meta_TVALID = 1'b0;
  always @(posedge ap_clk) begin
    if (m_axis_rx_ack_meta_TVALID && m_axis_rx_ack_meta_TREADY) acks_sent <= acks_sent + 1;
    if (!m_axis_rx_ack_meta_TVALID || m_axis_rx_ack_meta_TREADY) begin
      if (ack_due.size() > 0 && now >= ack_due[0]) begin
        automatic wqe_t w = ack_w.pop_front();
        void'(ack_due.pop_front());
        m_axis_rx_ack_meta_TVALID <= 1'b1;
        m_axis_rx_ack_meta_TDATA  <= {1'b1, 2'b00, 4'h0, 1'b0, w.qpn, OP_ACK};   // lst, strm, dst, host, qpn, op
      end else begin
        m_axis_rx_ack_meta_TVALID <= 1'b0;
      end
    end
  end

  // ---- WRITEs from the coprocessor ---------------------------------------------------------
  logic [143:0] wr_cmd_q [$];
  beat_t        wr_q [$];

  task automatic inject_write(input logic [47:0] vaddr, input logic [511:0] data, input int len,
                              input int beats);
    beat_t x;
    wr_cmd_q.push_back(mem_cmd(OP_WRITE_ONLY, conn_qpn, vaddr, 28'(len), 1'b1, 6'd0));
    for (int i = 0; i < beats; i++) begin
      x.data = data;
      x.keep = (i == beats - 1 && len % 64 != 0) ? ~(64'hFFFF_FFFF_FFFF_FFFF << (len % 64)) : '1;
      x.last = (i == beats - 1);
      wr_q.push_back(x);
    end
  endtask

  initial begin
    m_axis_mem_write_cmd_TVALID  = 1'b0;
    m_axis_mem_write_data_TVALID = 1'b0;
  end
  always @(posedge ap_clk) begin
    if (!m_axis_mem_write_cmd_TVALID || m_axis_mem_write_cmd_TREADY) begin
      m_axis_mem_write_cmd_TVALID <= (wr_cmd_q.size() > 0);
      if (wr_cmd_q.size() > 0) m_axis_mem_write_cmd_TDATA <= wr_cmd_q.pop_front();
    end
    if (!m_axis_mem_write_data_TVALID || m_axis_mem_write_data_TREADY) begin
      m_axis_mem_write_data_TVALID <= (wr_q.size() > 0);
      if (wr_q.size() > 0) begin
        automatic beat_t x = wr_q.pop_front();
        m_axis_mem_write_data_TDATA <= x.data;
        m_axis_mem_write_data_TKEEP <= x.keep;
        m_axis_mem_write_data_TLAST <= x.last;
      end
    end
  end

endmodule
