// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Xanadu Quantum Technologies Inc.
//===----------------------------------------------------------------------===//
//
// balboa_net.sv - BALBOA's network stack, with plain ports
//
// Instantiates Coyote's network_stack (vendored unmodified: IP handler, ARP, ICMP, MAC/IP
// encode and roce_stack, i.e. BALBOA) and turns its SystemVerilog interfaces into plain
// valid/ready ports, so hwhs and the board top need no Coyote types. The packing of the
// lynxTypes structs happens here, by field name:
//
//   sq_*     a WRITE hwhs posts: dreq_t, req_1 = the remote side (RC_RDMA_WRITE_ONLY, QPN,
//            remote address, length), req_2.vaddr = the local address (unused by hwhs)
//   tx_*     its payload, which BALBOA's rdma_mux_retrans takes once the stack reads it
//   wr_*     a WRITE the coprocessor issues: req_t from m_rdma_wr_req, data from m_axis_rdma_wr
//   rd_*     a READ the coprocessor issues: req_t from m_rdma_rd_req, data to s_axis_rdma_rd_rsp
//   mem_*    BALBOA's retransmission buffer (Coyote's card memory)
//   stats    network_stack's m_net_stats, as 32-bit words (hwhs_regs NET_STATS)
//
// Everything runs on one clock: BALBOA's HLS cores are synthesized for it (NET_MHZ).
//
//===----------------------------------------------------------------------===//
`timescale 1ns / 1ps

module balboa_net
  import lynxTypes::*;
#(
  parameter int N_STATS = 13
) (
  input  logic                    clk,
  input  logic                    rstn,

  // Ethernet frames without FCS, 64 B beats
  input  logic [AXI_NET_BITS-1:0] rx_tdata,
  input  logic [AXI_NET_BITS/8-1:0] rx_tkeep,
  input  logic                    rx_tlast,
  input  logic                    rx_tvalid,
  output logic                    rx_tready,
  output logic [AXI_NET_BITS-1:0] tx_tdata,
  output logic [AXI_NET_BITS/8-1:0] tx_tkeep,
  output logic                    tx_tlast,
  output logic                    tx_tvalid,
  input  logic                    tx_tready,

  // configuration: addresses in host byte order (0xC0A80002 = 192.168.0.2), contexts raw
  input  logic                    set_ip_valid,
  input  logic [31:0]             set_ip,
  input  logic                    set_mac_valid,
  input  logic [47:0]             set_mac,
  input  logic                    arp_valid,
  output logic                    arp_ready,
  input  logic [31:0]             arp_ip,
  input  logic                    qp_valid,
  output logic                    qp_ready,
  input  logic [183:0]            qp_ctx,       // rdma_qp_ctx_t
  input  logic                    conn_valid,
  output logic                    conn_ready,
  input  logic [183:0]            conn_ctx,     // rdma_qp_conn_t
  output logic [32*N_STATS-1:0]   stats,

  // WRITEs posted by hwhs
  input  logic                    sq_valid,
  output logic                    sq_ready,
  input  logic [15:0]             sq_qpn,       // {vfid, pid}
  input  logic [63:0]             sq_raddr,
  input  logic [27:0]             sq_len,
  input  logic [AXI_NET_BITS-1:0] sq_tdata,
  input  logic [AXI_NET_BITS/8-1:0] sq_tkeep,
  input  logic                    sq_tlast,
  input  logic                    sq_tvalid,
  output logic                    sq_tready,

  // their ACKs
  output logic                    ack_valid,
  input  logic                    ack_ready,

  // READs from the coprocessor
  output logic                    rd_valid,
  input  logic                    rd_ready,
  output logic [27:0]             rd_len,
  input  logic [AXI_NET_BITS-1:0] rd_tdata,
  input  logic [AXI_NET_BITS/8-1:0] rd_tkeep,
  input  logic                    rd_tlast,
  input  logic                    rd_tvalid,
  output logic                    rd_tready,

  // WRITEs from the coprocessor
  output logic                    wr_valid,
  input  logic                    wr_ready,
  output logic [47:0]             wr_vaddr,
  output logic [27:0]             wr_len,
  output logic [AXI_NET_BITS-1:0] wr_tdata,
  output logic [AXI_NET_BITS/8-1:0] wr_tkeep,
  output logic                    wr_tlast,
  output logic                    wr_tvalid,
  input  logic                    wr_tready,

  // retransmission buffer
  output logic                    mem_wr_valid,
  input  logic                    mem_wr_ready,
  output logic [MEM_CMD_BITS-1:0] mem_wr_cmd,
  output logic [AXI_DDR_BITS-1:0] mem_wr_tdata,
  output logic                    mem_wr_tlast,
  output logic                    mem_wr_tvalid,
  input  logic                    mem_wr_tready,
  output logic                    mem_rd_valid,
  input  logic                    mem_rd_ready,
  output logic [MEM_CMD_BITS-1:0] mem_rd_cmd,
  input  logic [AXI_DDR_BITS-1:0] mem_rd_tdata,
  input  logic [AXI_DDR_BITS/8-1:0] mem_rd_tkeep,
  input  logic                    mem_rd_tlast,
  input  logic                    mem_rd_tvalid,
  output logic                    mem_rd_tready
);

  // ------------------------------------------------------------------------------------------
  // Interfaces of network_stack
  // ------------------------------------------------------------------------------------------
  AXI4S #(.AXI4S_DATA_BITS(AXI_NET_BITS)) axis_rx (.aclk(clk), .aresetn(rstn));
  AXI4S #(.AXI4S_DATA_BITS(AXI_NET_BITS)) axis_tx (.aclk(clk), .aresetn(rstn));

  metaIntf #(.STYPE(logic[ARP_LUP_REQ_BITS-1:0])) arp_lookup (.aclk(clk), .aresetn(rstn));
  metaIntf #(.STYPE(logic[IP_ADDR_BITS-1:0]))     ip_addr    (.aclk(clk), .aresetn(rstn));
  metaIntf #(.STYPE(logic[MAC_ADDR_BITS-1:0]))    mac_addr   (.aclk(clk), .aresetn(rstn));
  net_stat_t                                       net_stats;

  metaIntf #(.STYPE(rdma_qp_ctx_t))  qp_interface   (.aclk(clk), .aresetn(rstn));
  metaIntf #(.STYPE(rdma_qp_conn_t)) conn_interface (.aclk(clk), .aresetn(rstn));
  metaIntf #(.STYPE(dreq_t))         rdma_sq        (.aclk(clk), .aresetn(rstn));
  metaIntf #(.STYPE(ack_t))          rdma_ack       (.aclk(clk), .aresetn(rstn));
  metaIntf #(.STYPE(req_t))          rdma_rd_req    (.aclk(clk), .aresetn(rstn));
  metaIntf #(.STYPE(req_t))          rdma_wr_req    (.aclk(clk), .aresetn(rstn));
  AXI4S #(.AXI4S_DATA_BITS(AXI_NET_BITS)) axis_rdma_rd_req (.aclk(clk), .aresetn(rstn));
  AXI4S #(.AXI4S_DATA_BITS(AXI_NET_BITS)) axis_rdma_rd_rsp (.aclk(clk), .aresetn(rstn));
  AXI4S #(.AXI4S_DATA_BITS(AXI_NET_BITS)) axis_rdma_wr     (.aclk(clk), .aresetn(rstn));

  metaIntf #(.STYPE(logic[MEM_CMD_BITS-1:0])) mem_rd_cmd_i (.aclk(clk), .aresetn(rstn));
  metaIntf #(.STYPE(logic[MEM_CMD_BITS-1:0])) mem_wr_cmd_i (.aclk(clk), .aresetn(rstn));
  metaIntf #(.STYPE(logic[MEM_STS_BITS-1:0])) mem_rd_sts   (.aclk(clk), .aresetn(rstn));
  metaIntf #(.STYPE(logic[MEM_STS_BITS-1:0])) mem_wr_sts   (.aclk(clk), .aresetn(rstn));
  AXI4S #(.AXI4S_DATA_BITS(AXI_DDR_BITS)) axis_mem_rd (.aclk(clk), .aresetn(rstn));
  AXI4S #(.AXI4S_DATA_BITS(AXI_DDR_BITS)) axis_mem_wr (.aclk(clk), .aresetn(rstn));

  network_stack u_stack (
    .nclk                  (clk),
    .nresetn               (rstn),
    .s_arp_lookup_request  (arp_lookup),
    .s_set_ip_addr         (ip_addr),
    .s_set_mac_addr        (mac_addr),
    .m_net_stats           (net_stats),
    .s_rdma_qp_interface   (qp_interface),
    .s_rdma_conn_interface (conn_interface),
    .s_rdma_sq             (rdma_sq),
    .m_rdma_ack            (rdma_ack),
    .m_rdma_rd_req         (rdma_rd_req),
    .m_rdma_wr_req         (rdma_wr_req),
    .s_axis_rdma_rd_req    (axis_rdma_rd_req),
    .s_axis_rdma_rd_rsp    (axis_rdma_rd_rsp),
    .m_axis_rdma_wr        (axis_rdma_wr),
    .m_rdma_mem_rd_cmd     (mem_rd_cmd_i),
    .m_rdma_mem_wr_cmd     (mem_wr_cmd_i),
    .s_rdma_mem_rd_sts     (mem_rd_sts),
    .s_rdma_mem_wr_sts     (mem_wr_sts),
    .s_axis_rdma_mem_rd    (axis_mem_rd),
    .m_axis_rdma_mem_wr    (axis_mem_wr),
    .s_axis_net            (axis_rx),
    .m_axis_net            (axis_tx)
  );

  // ------------------------------------------------------------------------------------------
  // Network
  // ------------------------------------------------------------------------------------------
  assign axis_rx.tdata  = rx_tdata;
  assign axis_rx.tkeep  = rx_tkeep;
  assign axis_rx.tlast  = rx_tlast;
  assign axis_rx.tvalid = rx_tvalid;
  assign rx_tready      = axis_rx.tready;

  assign tx_tdata       = axis_tx.tdata;
  assign tx_tkeep       = axis_tx.tkeep;
  assign tx_tlast       = axis_tx.tlast;
  assign tx_tvalid      = axis_tx.tvalid;
  assign axis_tx.tready = tx_tready;

  // ------------------------------------------------------------------------------------------
  // Configuration. network_stack byte-swaps the IP and MAC it is given into the stack's
  // first-octet-in-bits-[7:0] order, so they go in as plain integers.
  // ------------------------------------------------------------------------------------------
  assign ip_addr.valid  = set_ip_valid;
  assign ip_addr.data   = set_ip;
  assign mac_addr.valid = set_mac_valid;
  assign mac_addr.data  = set_mac;

  // the ARP server takes the address it resolves in network order, first octet in [7:0]
  assign arp_lookup.valid = arp_valid;
  assign arp_lookup.data  = {arp_ip[7:0], arp_ip[15:8], arp_ip[23:16], arp_ip[31:24]};
  assign arp_ready        = arp_lookup.ready;

  assign qp_interface.valid   = qp_valid;
  assign qp_interface.data    = rdma_qp_ctx_t'(qp_ctx);
  assign qp_ready             = qp_interface.ready;
  assign conn_interface.valid = conn_valid;
  assign conn_interface.data  = rdma_qp_conn_t'(conn_ctx);
  assign conn_ready           = conn_interface.ready;

  assign stats = {
    31'd0, net_stats.axis_stream_down,   // word 12
    net_stats.roce_retrans_counter,      // 11
    net_stats.roce_psn_drop_counter,     // 10
    net_stats.ibv_tx_pkg_counter,        //  9
    net_stats.ibv_rx_pkg_counter,        //  8
    net_stats.roce_tx_pkg_counter,       //  7
    net_stats.roce_rx_pkg_counter,       //  6
    net_stats.icmp_tx_pkg_counter,       //  5
    net_stats.icmp_rx_pkg_counter,       //  4
    net_stats.arp_tx_pkg_counter,        //  3
    net_stats.arp_rx_pkg_counter,        //  2
    net_stats.tx_pkg_counter,            //  1
    net_stats.rx_pkg_counter             //  0
  };

  // ------------------------------------------------------------------------------------------
  // WRITEs hwhs posts: one RC_RDMA_WRITE_ONLY per request, as Coyote's dreq_rdma_parser_wr
  // hands roce_stack a single-packet WRITE (roce_stack takes req_1.vaddr as the remote
  // address and req_2.vaddr as the local one; rdma_flow fills req_1.offs)
  // ------------------------------------------------------------------------------------------
  dreq_t sq_req;
  always_comb begin
    sq_req              = '0;
    sq_req.req_1.opcode = OPCODE_BITS'(RC_RDMA_WRITE_ONLY);
    sq_req.req_1.mode   = 1'(RDMA_MODE_RAW);
    sq_req.req_1.rdma   = 1'b1;
    sq_req.req_1.remote = 1'b1;
    sq_req.req_1.vfid   = sq_qpn[PID_BITS +: DEST_BITS];
    sq_req.req_1.pid    = sq_qpn[PID_BITS-1:0];
    sq_req.req_1.last   = 1'b1;
    sq_req.req_1.vaddr  = sq_raddr[VADDR_BITS-1:0];
    sq_req.req_1.len    = sq_len;
    sq_req.req_1.actv   = 1'b1;
    sq_req.req_2.vaddr  = '0;
  end

  assign rdma_sq.valid = sq_valid;
  assign rdma_sq.data  = sq_req;
  assign sq_ready      = rdma_sq.ready;

  assign axis_rdma_rd_req.tdata  = sq_tdata;
  assign axis_rdma_rd_req.tkeep  = sq_tkeep;
  assign axis_rdma_rd_req.tlast  = sq_tlast;
  assign axis_rdma_rd_req.tvalid = sq_tvalid;
  assign sq_tready               = axis_rdma_rd_req.tready;

  assign ack_valid      = rdma_ack.valid;
  assign rdma_ack.ready = ack_ready;

  // ------------------------------------------------------------------------------------------
  // READs and WRITEs from the coprocessor
  // ------------------------------------------------------------------------------------------
  assign rd_valid          = rdma_rd_req.valid;
  assign rd_len            = rdma_rd_req.data.len;
  assign rdma_rd_req.ready = rd_ready;

  assign axis_rdma_rd_rsp.tdata  = rd_tdata;
  assign axis_rdma_rd_rsp.tkeep  = rd_tkeep;
  assign axis_rdma_rd_rsp.tlast  = rd_tlast;
  assign axis_rdma_rd_rsp.tvalid = rd_tvalid;
  assign rd_tready               = axis_rdma_rd_rsp.tready;

  assign wr_valid          = rdma_wr_req.valid;
  assign wr_vaddr          = rdma_wr_req.data.vaddr;
  assign wr_len            = rdma_wr_req.data.len;
  assign rdma_wr_req.ready = wr_ready;

  assign wr_tdata            = axis_rdma_wr.tdata;
  assign wr_tkeep            = axis_rdma_wr.tkeep;
  assign wr_tlast            = axis_rdma_wr.tlast;
  assign wr_tvalid           = axis_rdma_wr.tvalid;
  assign axis_rdma_wr.tready = wr_tready;

  // ------------------------------------------------------------------------------------------
  // Retransmission buffer. roce_stack ignores the status channels.
  // ------------------------------------------------------------------------------------------
  assign mem_wr_valid       = mem_wr_cmd_i.valid;
  assign mem_wr_cmd         = mem_wr_cmd_i.data;
  assign mem_wr_cmd_i.ready = mem_wr_ready;
  assign mem_rd_valid       = mem_rd_cmd_i.valid;
  assign mem_rd_cmd         = mem_rd_cmd_i.data;
  assign mem_rd_cmd_i.ready = mem_rd_ready;

  assign mem_rd_sts.valid = 1'b0;
  assign mem_rd_sts.data  = '0;
  assign mem_wr_sts.valid = 1'b0;
  assign mem_wr_sts.data  = '0;

  assign mem_wr_tdata       = axis_mem_wr.tdata;
  assign mem_wr_tlast       = axis_mem_wr.tlast;
  assign mem_wr_tvalid      = axis_mem_wr.tvalid;
  assign axis_mem_wr.tready = mem_wr_tready;

  assign axis_mem_rd.tdata  = mem_rd_tdata;
  assign axis_mem_rd.tkeep  = mem_rd_tkeep;
  assign axis_mem_rd.tlast  = mem_rd_tlast;
  assign axis_mem_rd.tvalid = mem_rd_tvalid;
  assign mem_rd_tready      = axis_mem_rd.tready;

endmodule
