// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Xanadu Quantum Technologies Inc.
//
// network_stack_sim.sv - network_stack for tb_hwhs_balboa: the same ports as the vendored
// network_stack.sv, but only roce_stack (vendored, unmodified) inside, with its IP-level
// streams on s_axis_net / m_axis_net. The IP handler, ARP, ICMP and MAC/IP encoder are HLS
// cores with no simulation model here; the RDMA interfaces, which is what hwhs and
// balboa_net drive, go straight to roce_stack as in network_stack.sv. Simulation only.

`timescale 1ns / 1ps

module network_stack
  import lynxTypes::*;
(
    input  wire                 nclk,
    input  wire                 nresetn,

    metaIntf.s                  s_arp_lookup_request,
    metaIntf.s                  s_set_ip_addr,
    metaIntf.s                  s_set_mac_addr,
    output net_stat_t           m_net_stats,

    metaIntf.s                  s_rdma_qp_interface,
    metaIntf.s                  s_rdma_conn_interface,
    metaIntf.s                  s_rdma_sq,
    metaIntf.m                  m_rdma_ack,
    metaIntf.m                  m_rdma_rd_req,
    metaIntf.m                  m_rdma_wr_req,
    AXI4S.s                     s_axis_rdma_rd_req,
    AXI4S.s                     s_axis_rdma_rd_rsp,
    AXI4S.m                     m_axis_rdma_wr,

    metaIntf.m                  m_rdma_mem_rd_cmd,
    metaIntf.m                  m_rdma_mem_wr_cmd,
    metaIntf.s                  s_rdma_mem_rd_sts,
    metaIntf.s                  s_rdma_mem_wr_sts,
    AXI4S.s                     s_axis_rdma_mem_rd,
    AXI4S.m                     m_axis_rdma_mem_wr,

    AXI4S.s                     s_axis_net,
    AXI4S.m                     m_axis_net
);

  // network_stack.sv byte-swaps the IP it is given; the same here
  logic [31:0] ip_addr = 32'h0;
  logic [47:0] mac_addr = 48'h0;
  logic [31:0] arp_lookups = 0;
  always_ff @(posedge nclk) begin
    if (s_set_ip_addr.valid)
      ip_addr <= {s_set_ip_addr.data[7:0], s_set_ip_addr.data[15:8], s_set_ip_addr.data[23:16],
                  s_set_ip_addr.data[31:24]};
    if (s_set_mac_addr.valid) mac_addr <= s_set_mac_addr.data;
    if (s_arp_lookup_request.valid && s_arp_lookup_request.ready) arp_lookups <= arp_lookups + 1;
  end
  assign s_set_ip_addr.ready        = 1'b1;
  assign s_set_mac_addr.ready       = 1'b1;
  assign s_arp_lookup_request.ready = 1'b1;

  logic [31:0] rx_frames = 0, tx_frames = 0;
  always_ff @(posedge nclk) begin
    if (!nresetn) begin
      rx_frames <= 0;
      tx_frames <= 0;
    end else begin
      if (s_axis_net.tvalid && s_axis_net.tready && s_axis_net.tlast) rx_frames <= rx_frames + 1;
      if (m_axis_net.tvalid && m_axis_net.tready && m_axis_net.tlast) tx_frames <= tx_frames + 1;
    end
  end
  always_comb begin
    m_net_stats                = '0;
    m_net_stats.rx_pkg_counter = rx_frames;
    m_net_stats.tx_pkg_counter = tx_frames;
  end

  roce_stack inst_roce_stack (
    .nclk                    (nclk),
    .nresetn                 (nresetn),
    .s_axis_rx               (s_axis_net),
    .m_axis_tx               (m_axis_net),
    .s_rdma_qp_interface     (s_rdma_qp_interface),
    .s_rdma_conn_interface   (s_rdma_conn_interface),
    .s_rdma_sq               (s_rdma_sq),
    .m_rdma_ack              (m_rdma_ack),
    .m_rdma_rd_req           (m_rdma_rd_req),
    .m_rdma_wr_req           (m_rdma_wr_req),
    .s_axis_rdma_rd_req      (s_axis_rdma_rd_req),
    .s_axis_rdma_rd_rsp      (s_axis_rdma_rd_rsp),
    .m_axis_rdma_wr          (m_axis_rdma_wr),
    .local_ip_address        (ip_addr),
    .m_rdma_mem_rd_cmd       (m_rdma_mem_rd_cmd),
    .m_rdma_mem_wr_cmd       (m_rdma_mem_wr_cmd),
    .s_rdma_mem_rd_sts       (s_rdma_mem_rd_sts),
    .s_rdma_mem_wr_sts       (s_rdma_mem_wr_sts),
    .s_axis_rdma_mem_rd      (s_axis_rdma_mem_rd),
    .m_axis_rdma_mem_wr      (m_axis_rdma_mem_wr),
    .ibv_rx_pkg_count_valid  (),
    .ibv_rx_pkg_count_data   (),
    .ibv_tx_pkg_count_valid  (),
    .ibv_tx_pkg_count_data   (),
    .crc_drop_pkg_count_valid(),
    .crc_drop_pkg_count_data (),
    .psn_drop_pkg_count_valid(),
    .psn_drop_pkg_count_data (),
    .retrans_count_valid     (),
    .retrans_count_data      ()
  );

endmodule
