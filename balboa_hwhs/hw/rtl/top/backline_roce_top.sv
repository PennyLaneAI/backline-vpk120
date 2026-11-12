// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Xanadu Quantum Technologies Inc.
//===----------------------------------------------------------------------===//
//
// backline_roce_top.sv - VPK120 top: MRMAC 100G + BALBOA RoCE v2 + hwhs-balboa
//
//   QSFP-DD 100G, to the coprocessor
//      |
//   i_mrmac_0_exdes   AMD's MRMAC example design: the 100G MAC
//      |
//   i_platform        block design: CIPS (A72), LPDDR4, clocks, BRAM controllers,
//      |              transport_demo, and the MAC <-> net clock width and clock converters
//   u_pad             frames to the 60-byte minimum
//   u_net             BALBOA (Coyote's network_stack: ARP, ICMP, IP, RoCE v2)
//      |
//   u_hwhs            hwhs-balboa: registers, round engine, RAMs, retransmission buffer
//
// The MAC side is 384 bit @ 390.625 MHz, everything from the converters down is 64 B @ net
// clock (NET_MHZ, the clock BALBOA's HLS cores are synthesized for).
//
//===----------------------------------------------------------------------===//

`ifndef BACKLINE_BUILD_ID
`define BACKLINE_BUILD_ID 0
`endif
`ifndef NET_CLK_FREQ
`define NET_CLK_FREQ 250000000
`endif

`timescale 1ps/1ps
`default_nettype none

(* DowngradeIPIdentifiedWarnings="yes" *)
module backline_roce_top
  import lynxTypes::*;
(
  input  wire       gt_ref_clk_p,
  input  wire       gt_ref_clk_n,
  input  wire [3:0] gt_rxn_in,
  input  wire [3:0] gt_rxp_in,
  output wire       qsfpdd2_lpmodes_ls,
  output wire [3:0] gt_txn_out,
  output wire [3:0] gt_txp_out,

  input  wire lpddr4_clk1_clk_n,
  input  wire lpddr4_clk1_clk_p,
  input  wire lpddr4_clk2_clk_n,
  input  wire lpddr4_clk2_clk_p,
  input  wire lpddr4_clk3_clk_n,
  input  wire lpddr4_clk3_clk_p,

  output wire [5:0]  ch0_lpddr4_trip1_ca_a,
  output wire [5:0]  ch0_lpddr4_trip1_ca_b,
  output wire [0:0]  ch0_lpddr4_trip1_ck_c_a,
  output wire [0:0]  ch0_lpddr4_trip1_ck_c_b,
  output wire [0:0]  ch0_lpddr4_trip1_ck_t_a,
  output wire [0:0]  ch0_lpddr4_trip1_ck_t_b,
  output wire [0:0]  ch0_lpddr4_trip1_cke_a,
  output wire [0:0]  ch0_lpddr4_trip1_cke_b,
  output wire [0:0]  ch0_lpddr4_trip1_cs_a,
  output wire [0:0]  ch0_lpddr4_trip1_cs_b,
  inout  wire [1:0]  ch0_lpddr4_trip1_dmi_a,
  inout  wire [1:0]  ch0_lpddr4_trip1_dmi_b,
  inout  wire [15:0] ch0_lpddr4_trip1_dq_a,
  inout  wire [15:0] ch0_lpddr4_trip1_dq_b,
  inout  wire [1:0]  ch0_lpddr4_trip1_dqs_c_a,
  inout  wire [1:0]  ch0_lpddr4_trip1_dqs_c_b,
  inout  wire [1:0]  ch0_lpddr4_trip1_dqs_t_a,
  inout  wire [1:0]  ch0_lpddr4_trip1_dqs_t_b,
  output wire [0:0]  ch0_lpddr4_trip1_reset_n,

  output wire [5:0]  ch0_lpddr4_trip2_ca_a,
  output wire [5:0]  ch0_lpddr4_trip2_ca_b,
  output wire [0:0]  ch0_lpddr4_trip2_ck_c_a,
  output wire [0:0]  ch0_lpddr4_trip2_ck_c_b,
  output wire [0:0]  ch0_lpddr4_trip2_ck_t_a,
  output wire [0:0]  ch0_lpddr4_trip2_ck_t_b,
  output wire [0:0]  ch0_lpddr4_trip2_cke_a,
  output wire [0:0]  ch0_lpddr4_trip2_cke_b,
  output wire [0:0]  ch0_lpddr4_trip2_cs_a,
  output wire [0:0]  ch0_lpddr4_trip2_cs_b,
  inout  wire [1:0]  ch0_lpddr4_trip2_dmi_a,
  inout  wire [1:0]  ch0_lpddr4_trip2_dmi_b,
  inout  wire [15:0] ch0_lpddr4_trip2_dq_a,
  inout  wire [15:0] ch0_lpddr4_trip2_dq_b,
  inout  wire [1:0]  ch0_lpddr4_trip2_dqs_c_a,
  inout  wire [1:0]  ch0_lpddr4_trip2_dqs_c_b,
  inout  wire [1:0]  ch0_lpddr4_trip2_dqs_t_a,
  inout  wire [1:0]  ch0_lpddr4_trip2_dqs_t_b,
  output wire [0:0]  ch0_lpddr4_trip2_reset_n,

  output wire [5:0]  ch0_lpddr4_trip3_ca_a,
  output wire [5:0]  ch0_lpddr4_trip3_ca_b,
  output wire [0:0]  ch0_lpddr4_trip3_ck_c_a,
  output wire [0:0]  ch0_lpddr4_trip3_ck_c_b,
  output wire [0:0]  ch0_lpddr4_trip3_ck_t_a,
  output wire [0:0]  ch0_lpddr4_trip3_ck_t_b,
  output wire [0:0]  ch0_lpddr4_trip3_cke_a,
  output wire [0:0]  ch0_lpddr4_trip3_cke_b,
  output wire [0:0]  ch0_lpddr4_trip3_cs_a,
  output wire [0:0]  ch0_lpddr4_trip3_cs_b,
  inout  wire [1:0]  ch0_lpddr4_trip3_dmi_a,
  inout  wire [1:0]  ch0_lpddr4_trip3_dmi_b,
  inout  wire [15:0] ch0_lpddr4_trip3_dq_a,
  inout  wire [15:0] ch0_lpddr4_trip3_dq_b,
  inout  wire [1:0]  ch0_lpddr4_trip3_dqs_c_a,
  inout  wire [1:0]  ch0_lpddr4_trip3_dqs_c_b,
  inout  wire [1:0]  ch0_lpddr4_trip3_dqs_t_a,
  inout  wire [1:0]  ch0_lpddr4_trip3_dqs_t_b,
  output wire [0:0]  ch0_lpddr4_trip3_reset_n,

  output wire [5:0]  ch1_lpddr4_trip1_ca_a,
  output wire [5:0]  ch1_lpddr4_trip1_ca_b,
  output wire [0:0]  ch1_lpddr4_trip1_ck_c_a,
  output wire [0:0]  ch1_lpddr4_trip1_ck_c_b,
  output wire [0:0]  ch1_lpddr4_trip1_ck_t_a,
  output wire [0:0]  ch1_lpddr4_trip1_ck_t_b,
  output wire [0:0]  ch1_lpddr4_trip1_cke_a,
  output wire [0:0]  ch1_lpddr4_trip1_cke_b,
  output wire [0:0]  ch1_lpddr4_trip1_cs_a,
  output wire [0:0]  ch1_lpddr4_trip1_cs_b,
  inout  wire [1:0]  ch1_lpddr4_trip1_dmi_a,
  inout  wire [1:0]  ch1_lpddr4_trip1_dmi_b,
  inout  wire [15:0] ch1_lpddr4_trip1_dq_a,
  inout  wire [15:0] ch1_lpddr4_trip1_dq_b,
  inout  wire [1:0]  ch1_lpddr4_trip1_dqs_c_a,
  inout  wire [1:0]  ch1_lpddr4_trip1_dqs_c_b,
  inout  wire [1:0]  ch1_lpddr4_trip1_dqs_t_a,
  inout  wire [1:0]  ch1_lpddr4_trip1_dqs_t_b,
  output wire [0:0]  ch1_lpddr4_trip1_reset_n,

  output wire [5:0]  ch1_lpddr4_trip2_ca_a,
  output wire [5:0]  ch1_lpddr4_trip2_ca_b,
  output wire [0:0]  ch1_lpddr4_trip2_ck_c_a,
  output wire [0:0]  ch1_lpddr4_trip2_ck_c_b,
  output wire [0:0]  ch1_lpddr4_trip2_ck_t_a,
  output wire [0:0]  ch1_lpddr4_trip2_ck_t_b,
  output wire [0:0]  ch1_lpddr4_trip2_cke_a,
  output wire [0:0]  ch1_lpddr4_trip2_cke_b,
  output wire [0:0]  ch1_lpddr4_trip2_cs_a,
  output wire [0:0]  ch1_lpddr4_trip2_cs_b,
  inout  wire [1:0]  ch1_lpddr4_trip2_dmi_a,
  inout  wire [1:0]  ch1_lpddr4_trip2_dmi_b,
  inout  wire [15:0] ch1_lpddr4_trip2_dq_a,
  inout  wire [15:0] ch1_lpddr4_trip2_dq_b,
  inout  wire [1:0]  ch1_lpddr4_trip2_dqs_c_a,
  inout  wire [1:0]  ch1_lpddr4_trip2_dqs_c_b,
  inout  wire [1:0]  ch1_lpddr4_trip2_dqs_t_a,
  inout  wire [1:0]  ch1_lpddr4_trip2_dqs_t_b,
  output wire [0:0]  ch1_lpddr4_trip2_reset_n,

  output wire [5:0]  ch1_lpddr4_trip3_ca_a,
  output wire [5:0]  ch1_lpddr4_trip3_ca_b,
  output wire [0:0]  ch1_lpddr4_trip3_ck_c_a,
  output wire [0:0]  ch1_lpddr4_trip3_ck_c_b,
  output wire [0:0]  ch1_lpddr4_trip3_ck_t_a,
  output wire [0:0]  ch1_lpddr4_trip3_ck_t_b,
  output wire [0:0]  ch1_lpddr4_trip3_cke_a,
  output wire [0:0]  ch1_lpddr4_trip3_cke_b,
  output wire [0:0]  ch1_lpddr4_trip3_cs_a,
  output wire [0:0]  ch1_lpddr4_trip3_cs_b,
  inout  wire [1:0]  ch1_lpddr4_trip3_dmi_a,
  inout  wire [1:0]  ch1_lpddr4_trip3_dmi_b,
  inout  wire [15:0] ch1_lpddr4_trip3_dq_a,
  inout  wire [15:0] ch1_lpddr4_trip3_dq_b,
  inout  wire [1:0]  ch1_lpddr4_trip3_dqs_c_a,
  inout  wire [1:0]  ch1_lpddr4_trip3_dqs_c_b,
  inout  wire [1:0]  ch1_lpddr4_trip3_dqs_t_a,
  inout  wire [1:0]  ch1_lpddr4_trip3_dqs_t_b,
  output wire [0:0]  ch1_lpddr4_trip3_reset_n
);

  localparam logic [31:0] BUILD_ID = `BACKLINE_BUILD_ID;
  localparam int          N_STATS  = 13;

  // where rdma_mux_retrans puts a message's slot in its retransmission address
  localparam int RETX_OFFS_LSB = RDMA_MEM_SHIFT + $clog2(PMTU_BYTES);

  //------------------------------------------------------------------------------------------//
  // Clocks and resets
  //------------------------------------------------------------------------------------------//
  wire pl_clk, pl_resetn;        // CIPS pl0, 200 MHz: MRMAC AXI-Lite, GPIOs
  wire net_clk, net_resetn;      // BALBOA, hwhs, transport_demo
  wire clk_locked;

  //------------------------------------------------------------------------------------------//
  // MRMAC example design
  //------------------------------------------------------------------------------------------//
  wire [7:0]   gt_line_rate;
  wire [2:0]   gt_loopback;
  wire [5:0]   gt_txprecursor;
  wire [5:0]   gt_txpostcursor;
  wire [6:0]   gt_txmaincursor;
  wire         gt_rxcdrhold;
  wire [3:0]   gt_reset_tx_datapath_in;
  wire [3:0]   gt_reset_rx_datapath_in;
  wire [3:0]   gt_reset_all_in;
  wire [3:0]   stat_mst_reset_done;

  wire [31:0]  mrmac_axi_awaddr, mrmac_axi_wdata, mrmac_axi_araddr, mrmac_axi_rdata;
  wire         mrmac_axi_awvalid, mrmac_axi_awready, mrmac_axi_wvalid, mrmac_axi_wready;
  wire [1:0]   mrmac_axi_bresp, mrmac_axi_rresp;
  wire         mrmac_axi_bvalid, mrmac_axi_bready, mrmac_axi_arvalid, mrmac_axi_arready;
  wire         mrmac_axi_rvalid, mrmac_axi_rready;

  wire [383:0] rx_axis_tdata;
  wire [47:0]  rx_axis_tkeep;
  wire         rx_axis_tuser, rx_axis_tlast, rx_axis_tvalid;
  wire         rx_axis_clk, rx_axis_rst, rx_axis_rstn;
  wire [383:0] tx_axis_tdata;
  wire [47:0]  tx_axis_tkeep;
  wire         tx_axis_tlast, tx_axis_tvalid, tx_axis_tready;
  wire         tx_axis_clk, tx_axis_rst, tx_axis_rstn;
  wire [8:0]   stat_rx_pause_req;

  mrmac_0_exdes i_mrmac_0_exdes (
    .rx_axis_tdata                    ( rx_axis_tdata           ),
    .rx_axis_tkeep                    ( rx_axis_tkeep           ),
    .rx_axis_tuser                    ( rx_axis_tuser           ),
    .rx_axis_tlast                    ( rx_axis_tlast           ),
    .rx_axis_tvalid                   ( rx_axis_tvalid          ),
    .rx_axis_clk                      ( rx_axis_clk             ),
    .rx_axis_rst                      ( rx_axis_rst             ),
    .rx_axis_rstn                     ( rx_axis_rstn            ),
    .tx_axis_tdata                    ( tx_axis_tdata           ),
    .tx_axis_tkeep                    ( tx_axis_tkeep           ),
    .tx_axis_tlast                    ( tx_axis_tlast           ),
    .tx_axis_tvalid                   ( tx_axis_tvalid          ),
    .tx_axis_tready                   ( tx_axis_tready          ),
    .tx_axis_clk                      ( tx_axis_clk             ),
    .tx_axis_rst                      ( tx_axis_rst             ),
    .tx_axis_rstn                     ( tx_axis_rstn            ),
    // no flow control
    .ctl_tx_port0_ctl_tx_pause_req    ( 9'd0                    ),
    .ctl_tx_port0_ctl_tx_resend_pause ( 1'b0                    ),
    .stat_rx_port0_stat_rx_pause_req  ( stat_rx_pause_req       ),
    .gt_line_rate                     ( gt_line_rate            ),
    .gt_loopback                      ( gt_loopback             ),
    .gt_txprecursor                   ( gt_txprecursor          ),
    .gt_txpostcursor                  ( gt_txpostcursor         ),
    .gt_txmaincursor                  ( gt_txmaincursor         ),
    .gt_rxcdrhold                     ( gt_rxcdrhold            ),
    .gt_reset_rx_datapath_in          ( gt_reset_rx_datapath_in ),
    .gt_reset_tx_datapath_in          ( gt_reset_tx_datapath_in ),
    .s_axi_aclk                       ( pl_clk                  ),
    .s_axi_aresetn                    ( pl_resetn               ),
    .s_axi_awaddr                     ( mrmac_axi_awaddr        ),
    .s_axi_awvalid                    ( mrmac_axi_awvalid       ),
    .s_axi_awready                    ( mrmac_axi_awready       ),
    .s_axi_wdata                      ( mrmac_axi_wdata         ),
    .s_axi_wvalid                     ( mrmac_axi_wvalid        ),
    .s_axi_wready                     ( mrmac_axi_wready        ),
    .s_axi_bresp                      ( mrmac_axi_bresp         ),
    .s_axi_bvalid                     ( mrmac_axi_bvalid        ),
    .s_axi_bready                     ( mrmac_axi_bready        ),
    .s_axi_araddr                     ( mrmac_axi_araddr        ),
    .s_axi_arvalid                    ( mrmac_axi_arvalid       ),
    .s_axi_arready                    ( mrmac_axi_arready       ),
    .s_axi_rdata                      ( mrmac_axi_rdata         ),
    .s_axi_rresp                      ( mrmac_axi_rresp         ),
    .s_axi_rvalid                     ( mrmac_axi_rvalid        ),
    .s_axi_rready                     ( mrmac_axi_rready        ),
    .stat_mst_reset_done              ( stat_mst_reset_done     ),
    .gt_rxn_in                        ( gt_rxn_in               ),
    .gt_rxp_in                        ( gt_rxp_in               ),
    .gt_txn_out                       ( gt_txn_out              ),
    .gt_txp_out                       ( gt_txp_out              ),
    .gt_reset_all_in                  ( gt_reset_all_in         ),
    .gpo_0                            (                         ),
    .gt_ref_clk_p                     ( gt_ref_clk_p            ),
    .gt_ref_clk_n                     ( gt_ref_clk_n            ),
    .pl_clk                           ( pl_clk                  ),
    .pl_resetn                        ( pl_resetn               )
  );

  assign qsfpdd2_lpmodes_ls = 1'b0;

  //------------------------------------------------------------------------------------------//
  // BALBOA reset: the net clock's reset, or the A72 holding the stack in reset through
  // axi_gpio_ctl. The reset clears BALBOA's QP state, its ARP table and its PSNs.
  //------------------------------------------------------------------------------------------//
  wire [31:0] net_ctl;           // axi_gpio_ctl channel 1 (pl0 domain)
  wire        stack_reset_pl = net_ctl[0];   // 1: hold BALBOA in reset

  // Clock crossings use xpm_cdc so they carry their own timing constraints.
  wire stack_reset_net;
  xpm_cdc_single #(.DEST_SYNC_FF(3), .INIT_SYNC_FF(1), .SRC_INPUT_REG(0)) u_cdc_stack_reset (
    .src_clk(1'b0), .src_in(stack_reset_pl), .dest_clk(net_clk), .dest_out(stack_reset_net));

  logic stack_rstn = 1'b0;       // BALBOA's reset, net clock domain
  always_ff @(posedge net_clk) stack_rstn <= net_resetn && !stack_reset_net;

  wire stack_up_rx;
  xpm_cdc_single #(.DEST_SYNC_FF(3), .INIT_SYNC_FF(1), .SRC_INPUT_REG(0)) u_cdc_stack_up_rx (
    .src_clk(1'b0), .src_in(stack_rstn), .dest_clk(rx_axis_clk), .dest_out(stack_up_rx));

  wire [3:0] gt_done_net;
  xpm_cdc_array_single #(.DEST_SYNC_FF(3), .INIT_SYNC_FF(1), .SRC_INPUT_REG(0), .WIDTH(4)) u_cdc_gt_net (
    .src_clk(1'b0), .src_in(stat_mst_reset_done), .dest_clk(net_clk), .dest_out(gt_done_net));

  // hwhs NET_STATUS (0x64)
  wire [31:0] net_status = {24'd0, gt_done_net, 2'd0, stack_reset_net, stack_rstn};

  // axi_gpio_ctl channel 2 (0xA409_0008), in the pl0 domain: net clock signals cross with xpm_cdc
  // first (net_clk and pl0 come from one MMCM, so a plain path would be timed at 1 ns)
  wire stack_up_pl;
  xpm_cdc_single #(.DEST_SYNC_FF(3), .INIT_SYNC_FF(1), .SRC_INPUT_REG(0)) u_cdc_stack_up_pl (
    .src_clk(1'b0), .src_in(stack_rstn), .dest_clk(pl_clk), .dest_out(stack_up_pl));
  wire [31:0] ctl_status = {24'd0, stat_mst_reset_done, 1'b0, stack_reset_pl, stack_up_pl, clk_locked};

  //------------------------------------------------------------------------------------------//
  // MRMAC RX gate: MRMAC cannot be stalled, so whole frames are dropped at the MAC while
  // BALBOA is held in reset.
  //------------------------------------------------------------------------------------------//
  logic rx_in_pkt, rx_pass;
  always_ff @(posedge rx_axis_clk) begin
    if (rx_axis_rst) begin
      rx_in_pkt <= 1'b0;
      rx_pass   <= 1'b0;
    end
    else if (rx_axis_tvalid) begin
      if (!rx_in_pkt) rx_pass <= stack_up_rx;   // decided at the first beat
      rx_in_pkt <= !rx_axis_tlast;
    end
  end
  wire rx_gate_tvalid = rx_axis_tvalid && (rx_in_pkt ? rx_pass : stack_up_rx);

  //------------------------------------------------------------------------------------------//
  // Platform BD
  //------------------------------------------------------------------------------------------//
  // BALBOA <-> MRMAC, 64 B @ net clock
  wire [511:0] net_rx_tdata;
  wire [63:0]  net_rx_tkeep;
  wire         net_rx_tlast, net_rx_tvalid, net_rx_tready;
  wire [511:0] net_tx_tdata;
  wire [63:0]  net_tx_tkeep;
  wire         net_tx_tlast, net_tx_tvalid, net_tx_tready;

  // hwhs AXI-Lite
  wire [31:0]  hwhs_axi_awaddr, hwhs_axi_wdata, hwhs_axi_araddr, hwhs_axi_rdata;
  wire [3:0]   hwhs_axi_wstrb;
  wire [1:0]   hwhs_axi_bresp, hwhs_axi_rresp;
  wire         hwhs_axi_awvalid, hwhs_axi_awready, hwhs_axi_wvalid, hwhs_axi_wready;
  wire         hwhs_axi_bvalid, hwhs_axi_bready, hwhs_axi_arvalid, hwhs_axi_arready;
  wire         hwhs_axi_rvalid, hwhs_axi_rready;

  // axi_bram_ctrl ports
  wire         slot_bram_clk, slot_bram_rst, slot_bram_en;
  wire [63:0]  slot_bram_we;
  wire [31:0]  slot_bram_addr;
  wire [511:0] slot_bram_din, slot_bram_dout;
  wire         reply_bram_clk, reply_bram_rst, reply_bram_en;
  wire [63:0]  reply_bram_we;
  wire [31:0]  reply_bram_addr;
  wire [511:0] reply_bram_din, reply_bram_dout;
  wire         trace_bram_clk, trace_bram_rst, trace_bram_en;
  wire [3:0]   trace_bram_we;
  wire [31:0]  trace_bram_addr;
  wire [31:0]  trace_bram_din, trace_bram_dout;

  // transport_demo <-> hwhs
  wire [511:0] demo_tx_tdata, demo_rx_tdata;
  wire         demo_tx_tvalid, demo_tx_tready, demo_tx_tlast;
  wire         demo_rx_tvalid, demo_rx_tready, demo_rx_tlast;
  wire         start_demo, reset_demo, cmp_error, cmp_valid, demo_done;
  wire [31:0]  syn_sum, syn_depth, reply_seq_off, freq_span_mask, lfsr_seed;
  wire [63:0]  freq_num;

  platform_wrapper i_platform (
    .lpddr4_clk1_clk_n        ( lpddr4_clk1_clk_n        ),
    .lpddr4_clk1_clk_p        ( lpddr4_clk1_clk_p        ),
    .lpddr4_clk2_clk_n        ( lpddr4_clk2_clk_n        ),
    .lpddr4_clk2_clk_p        ( lpddr4_clk2_clk_p        ),
    .lpddr4_clk3_clk_n        ( lpddr4_clk3_clk_n        ),
    .lpddr4_clk3_clk_p        ( lpddr4_clk3_clk_p        ),
    .ch0_lpddr4_trip1_ca_a    ( ch0_lpddr4_trip1_ca_a    ),
    .ch0_lpddr4_trip1_ca_b    ( ch0_lpddr4_trip1_ca_b    ),
    .ch0_lpddr4_trip1_ck_c_a  ( ch0_lpddr4_trip1_ck_c_a  ),
    .ch0_lpddr4_trip1_ck_c_b  ( ch0_lpddr4_trip1_ck_c_b  ),
    .ch0_lpddr4_trip1_ck_t_a  ( ch0_lpddr4_trip1_ck_t_a  ),
    .ch0_lpddr4_trip1_ck_t_b  ( ch0_lpddr4_trip1_ck_t_b  ),
    .ch0_lpddr4_trip1_cke_a   ( ch0_lpddr4_trip1_cke_a   ),
    .ch0_lpddr4_trip1_cke_b   ( ch0_lpddr4_trip1_cke_b   ),
    .ch0_lpddr4_trip1_cs_a    ( ch0_lpddr4_trip1_cs_a    ),
    .ch0_lpddr4_trip1_cs_b    ( ch0_lpddr4_trip1_cs_b    ),
    .ch0_lpddr4_trip1_dmi_a   ( ch0_lpddr4_trip1_dmi_a   ),
    .ch0_lpddr4_trip1_dmi_b   ( ch0_lpddr4_trip1_dmi_b   ),
    .ch0_lpddr4_trip1_dq_a    ( ch0_lpddr4_trip1_dq_a    ),
    .ch0_lpddr4_trip1_dq_b    ( ch0_lpddr4_trip1_dq_b    ),
    .ch0_lpddr4_trip1_dqs_c_a ( ch0_lpddr4_trip1_dqs_c_a ),
    .ch0_lpddr4_trip1_dqs_c_b ( ch0_lpddr4_trip1_dqs_c_b ),
    .ch0_lpddr4_trip1_dqs_t_a ( ch0_lpddr4_trip1_dqs_t_a ),
    .ch0_lpddr4_trip1_dqs_t_b ( ch0_lpddr4_trip1_dqs_t_b ),
    .ch0_lpddr4_trip1_reset_n ( ch0_lpddr4_trip1_reset_n ),
    .ch0_lpddr4_trip2_ca_a    ( ch0_lpddr4_trip2_ca_a    ),
    .ch0_lpddr4_trip2_ca_b    ( ch0_lpddr4_trip2_ca_b    ),
    .ch0_lpddr4_trip2_ck_c_a  ( ch0_lpddr4_trip2_ck_c_a  ),
    .ch0_lpddr4_trip2_ck_c_b  ( ch0_lpddr4_trip2_ck_c_b  ),
    .ch0_lpddr4_trip2_ck_t_a  ( ch0_lpddr4_trip2_ck_t_a  ),
    .ch0_lpddr4_trip2_ck_t_b  ( ch0_lpddr4_trip2_ck_t_b  ),
    .ch0_lpddr4_trip2_cke_a   ( ch0_lpddr4_trip2_cke_a   ),
    .ch0_lpddr4_trip2_cke_b   ( ch0_lpddr4_trip2_cke_b   ),
    .ch0_lpddr4_trip2_cs_a    ( ch0_lpddr4_trip2_cs_a    ),
    .ch0_lpddr4_trip2_cs_b    ( ch0_lpddr4_trip2_cs_b    ),
    .ch0_lpddr4_trip2_dmi_a   ( ch0_lpddr4_trip2_dmi_a   ),
    .ch0_lpddr4_trip2_dmi_b   ( ch0_lpddr4_trip2_dmi_b   ),
    .ch0_lpddr4_trip2_dq_a    ( ch0_lpddr4_trip2_dq_a    ),
    .ch0_lpddr4_trip2_dq_b    ( ch0_lpddr4_trip2_dq_b    ),
    .ch0_lpddr4_trip2_dqs_c_a ( ch0_lpddr4_trip2_dqs_c_a ),
    .ch0_lpddr4_trip2_dqs_c_b ( ch0_lpddr4_trip2_dqs_c_b ),
    .ch0_lpddr4_trip2_dqs_t_a ( ch0_lpddr4_trip2_dqs_t_a ),
    .ch0_lpddr4_trip2_dqs_t_b ( ch0_lpddr4_trip2_dqs_t_b ),
    .ch0_lpddr4_trip2_reset_n ( ch0_lpddr4_trip2_reset_n ),
    .ch0_lpddr4_trip3_ca_a    ( ch0_lpddr4_trip3_ca_a    ),
    .ch0_lpddr4_trip3_ca_b    ( ch0_lpddr4_trip3_ca_b    ),
    .ch0_lpddr4_trip3_ck_c_a  ( ch0_lpddr4_trip3_ck_c_a  ),
    .ch0_lpddr4_trip3_ck_c_b  ( ch0_lpddr4_trip3_ck_c_b  ),
    .ch0_lpddr4_trip3_ck_t_a  ( ch0_lpddr4_trip3_ck_t_a  ),
    .ch0_lpddr4_trip3_ck_t_b  ( ch0_lpddr4_trip3_ck_t_b  ),
    .ch0_lpddr4_trip3_cke_a   ( ch0_lpddr4_trip3_cke_a   ),
    .ch0_lpddr4_trip3_cke_b   ( ch0_lpddr4_trip3_cke_b   ),
    .ch0_lpddr4_trip3_cs_a    ( ch0_lpddr4_trip3_cs_a    ),
    .ch0_lpddr4_trip3_cs_b    ( ch0_lpddr4_trip3_cs_b    ),
    .ch0_lpddr4_trip3_dmi_a   ( ch0_lpddr4_trip3_dmi_a   ),
    .ch0_lpddr4_trip3_dmi_b   ( ch0_lpddr4_trip3_dmi_b   ),
    .ch0_lpddr4_trip3_dq_a    ( ch0_lpddr4_trip3_dq_a    ),
    .ch0_lpddr4_trip3_dq_b    ( ch0_lpddr4_trip3_dq_b    ),
    .ch0_lpddr4_trip3_dqs_c_a ( ch0_lpddr4_trip3_dqs_c_a ),
    .ch0_lpddr4_trip3_dqs_c_b ( ch0_lpddr4_trip3_dqs_c_b ),
    .ch0_lpddr4_trip3_dqs_t_a ( ch0_lpddr4_trip3_dqs_t_a ),
    .ch0_lpddr4_trip3_dqs_t_b ( ch0_lpddr4_trip3_dqs_t_b ),
    .ch0_lpddr4_trip3_reset_n ( ch0_lpddr4_trip3_reset_n ),
    .ch1_lpddr4_trip1_ca_a    ( ch1_lpddr4_trip1_ca_a    ),
    .ch1_lpddr4_trip1_ca_b    ( ch1_lpddr4_trip1_ca_b    ),
    .ch1_lpddr4_trip1_ck_c_a  ( ch1_lpddr4_trip1_ck_c_a  ),
    .ch1_lpddr4_trip1_ck_c_b  ( ch1_lpddr4_trip1_ck_c_b  ),
    .ch1_lpddr4_trip1_ck_t_a  ( ch1_lpddr4_trip1_ck_t_a  ),
    .ch1_lpddr4_trip1_ck_t_b  ( ch1_lpddr4_trip1_ck_t_b  ),
    .ch1_lpddr4_trip1_cke_a   ( ch1_lpddr4_trip1_cke_a   ),
    .ch1_lpddr4_trip1_cke_b   ( ch1_lpddr4_trip1_cke_b   ),
    .ch1_lpddr4_trip1_cs_a    ( ch1_lpddr4_trip1_cs_a    ),
    .ch1_lpddr4_trip1_cs_b    ( ch1_lpddr4_trip1_cs_b    ),
    .ch1_lpddr4_trip1_dmi_a   ( ch1_lpddr4_trip1_dmi_a   ),
    .ch1_lpddr4_trip1_dmi_b   ( ch1_lpddr4_trip1_dmi_b   ),
    .ch1_lpddr4_trip1_dq_a    ( ch1_lpddr4_trip1_dq_a    ),
    .ch1_lpddr4_trip1_dq_b    ( ch1_lpddr4_trip1_dq_b    ),
    .ch1_lpddr4_trip1_dqs_c_a ( ch1_lpddr4_trip1_dqs_c_a ),
    .ch1_lpddr4_trip1_dqs_c_b ( ch1_lpddr4_trip1_dqs_c_b ),
    .ch1_lpddr4_trip1_dqs_t_a ( ch1_lpddr4_trip1_dqs_t_a ),
    .ch1_lpddr4_trip1_dqs_t_b ( ch1_lpddr4_trip1_dqs_t_b ),
    .ch1_lpddr4_trip1_reset_n ( ch1_lpddr4_trip1_reset_n ),
    .ch1_lpddr4_trip2_ca_a    ( ch1_lpddr4_trip2_ca_a    ),
    .ch1_lpddr4_trip2_ca_b    ( ch1_lpddr4_trip2_ca_b    ),
    .ch1_lpddr4_trip2_ck_c_a  ( ch1_lpddr4_trip2_ck_c_a  ),
    .ch1_lpddr4_trip2_ck_c_b  ( ch1_lpddr4_trip2_ck_c_b  ),
    .ch1_lpddr4_trip2_ck_t_a  ( ch1_lpddr4_trip2_ck_t_a  ),
    .ch1_lpddr4_trip2_ck_t_b  ( ch1_lpddr4_trip2_ck_t_b  ),
    .ch1_lpddr4_trip2_cke_a   ( ch1_lpddr4_trip2_cke_a   ),
    .ch1_lpddr4_trip2_cke_b   ( ch1_lpddr4_trip2_cke_b   ),
    .ch1_lpddr4_trip2_cs_a    ( ch1_lpddr4_trip2_cs_a    ),
    .ch1_lpddr4_trip2_cs_b    ( ch1_lpddr4_trip2_cs_b    ),
    .ch1_lpddr4_trip2_dmi_a   ( ch1_lpddr4_trip2_dmi_a   ),
    .ch1_lpddr4_trip2_dmi_b   ( ch1_lpddr4_trip2_dmi_b   ),
    .ch1_lpddr4_trip2_dq_a    ( ch1_lpddr4_trip2_dq_a    ),
    .ch1_lpddr4_trip2_dq_b    ( ch1_lpddr4_trip2_dq_b    ),
    .ch1_lpddr4_trip2_dqs_c_a ( ch1_lpddr4_trip2_dqs_c_a ),
    .ch1_lpddr4_trip2_dqs_c_b ( ch1_lpddr4_trip2_dqs_c_b ),
    .ch1_lpddr4_trip2_dqs_t_a ( ch1_lpddr4_trip2_dqs_t_a ),
    .ch1_lpddr4_trip2_dqs_t_b ( ch1_lpddr4_trip2_dqs_t_b ),
    .ch1_lpddr4_trip2_reset_n ( ch1_lpddr4_trip2_reset_n ),
    .ch1_lpddr4_trip3_ca_a    ( ch1_lpddr4_trip3_ca_a    ),
    .ch1_lpddr4_trip3_ca_b    ( ch1_lpddr4_trip3_ca_b    ),
    .ch1_lpddr4_trip3_ck_c_a  ( ch1_lpddr4_trip3_ck_c_a  ),
    .ch1_lpddr4_trip3_ck_c_b  ( ch1_lpddr4_trip3_ck_c_b  ),
    .ch1_lpddr4_trip3_ck_t_a  ( ch1_lpddr4_trip3_ck_t_a  ),
    .ch1_lpddr4_trip3_ck_t_b  ( ch1_lpddr4_trip3_ck_t_b  ),
    .ch1_lpddr4_trip3_cke_a   ( ch1_lpddr4_trip3_cke_a   ),
    .ch1_lpddr4_trip3_cke_b   ( ch1_lpddr4_trip3_cke_b   ),
    .ch1_lpddr4_trip3_cs_a    ( ch1_lpddr4_trip3_cs_a    ),
    .ch1_lpddr4_trip3_cs_b    ( ch1_lpddr4_trip3_cs_b    ),
    .ch1_lpddr4_trip3_dmi_a   ( ch1_lpddr4_trip3_dmi_a   ),
    .ch1_lpddr4_trip3_dmi_b   ( ch1_lpddr4_trip3_dmi_b   ),
    .ch1_lpddr4_trip3_dq_a    ( ch1_lpddr4_trip3_dq_a    ),
    .ch1_lpddr4_trip3_dq_b    ( ch1_lpddr4_trip3_dq_b    ),
    .ch1_lpddr4_trip3_dqs_c_a ( ch1_lpddr4_trip3_dqs_c_a ),
    .ch1_lpddr4_trip3_dqs_c_b ( ch1_lpddr4_trip3_dqs_c_b ),
    .ch1_lpddr4_trip3_dqs_t_a ( ch1_lpddr4_trip3_dqs_t_a ),
    .ch1_lpddr4_trip3_dqs_t_b ( ch1_lpddr4_trip3_dqs_t_b ),
    .ch1_lpddr4_trip3_reset_n ( ch1_lpddr4_trip3_reset_n ),

    // clocks and resets
    .pl_clk                   ( pl_clk                   ),
    .pl_resetn                ( pl_resetn                ),
    .net_clk                  ( net_clk                  ),
    .net_resetn               ( net_resetn               ),
    .clk_locked               ( clk_locked               ),

    // MRMAC AXI-Lite and GT control
    .MRMAC_AXI_araddr         ( mrmac_axi_araddr         ),
    .MRMAC_AXI_arprot         (                          ),
    .MRMAC_AXI_arready        ( mrmac_axi_arready        ),
    .MRMAC_AXI_arvalid        ( mrmac_axi_arvalid        ),
    .MRMAC_AXI_awaddr         ( mrmac_axi_awaddr         ),
    .MRMAC_AXI_awprot         (                          ),
    .MRMAC_AXI_awready        ( mrmac_axi_awready        ),
    .MRMAC_AXI_awvalid        ( mrmac_axi_awvalid        ),
    .MRMAC_AXI_bready         ( mrmac_axi_bready         ),
    .MRMAC_AXI_bresp          ( mrmac_axi_bresp          ),
    .MRMAC_AXI_bvalid         ( mrmac_axi_bvalid         ),
    .MRMAC_AXI_rdata          ( mrmac_axi_rdata          ),
    .MRMAC_AXI_rready         ( mrmac_axi_rready         ),
    .MRMAC_AXI_rresp          ( mrmac_axi_rresp          ),
    .MRMAC_AXI_rvalid         ( mrmac_axi_rvalid         ),
    .MRMAC_AXI_wdata          ( mrmac_axi_wdata          ),
    .MRMAC_AXI_wready         ( mrmac_axi_wready         ),
    .MRMAC_AXI_wstrb          (                          ),
    .MRMAC_AXI_wvalid         ( mrmac_axi_wvalid         ),
    .gt_line_rate             ( gt_line_rate             ),
    .gt_reset_all_in          ( gt_reset_all_in          ),
    .gt_loopback              ( gt_loopback              ),
    .gt_txprecursor           ( gt_txprecursor           ),
    .gt_txpostcursor          ( gt_txpostcursor          ),
    .gt_txmaincursor          ( gt_txmaincursor          ),
    .gt_rxcdrhold             ( gt_rxcdrhold             ),
    .gt_reset_rx_datapath_in  ( gt_reset_rx_datapath_in  ),
    .gt_reset_tx_datapath_in  ( gt_reset_tx_datapath_in  ),
    .stat_mst_reset_done      ( stat_mst_reset_done      ),

    // MRMAC 384b <-> BALBOA 512b
    .cmac_rx_clk              ( rx_axis_clk              ),
    .cmac_rx_resetn           ( rx_axis_rstn             ),
    .MRMAC_RX_AXIS_tdata      ( rx_axis_tdata            ),
    .MRMAC_RX_AXIS_tkeep      ( rx_axis_tkeep            ),
    .MRMAC_RX_AXIS_tlast      ( rx_axis_tlast            ),
    .MRMAC_RX_AXIS_tvalid     ( rx_gate_tvalid           ),
    .MRMAC_RX_AXIS_tready     (                          ),   // MRMAC RX cannot stall
    .cmac_tx_clk              ( tx_axis_clk              ),
    .cmac_tx_resetn           ( tx_axis_rstn             ),
    .MRMAC_TX_AXIS_tdata      ( tx_axis_tdata            ),
    .MRMAC_TX_AXIS_tkeep      ( tx_axis_tkeep            ),
    .MRMAC_TX_AXIS_tlast      ( tx_axis_tlast            ),
    .MRMAC_TX_AXIS_tvalid     ( tx_axis_tvalid           ),
    .MRMAC_TX_AXIS_tready     ( tx_axis_tready           ),
    .NET_RX_AXIS_tdata        ( net_rx_tdata             ),
    .NET_RX_AXIS_tkeep        ( net_rx_tkeep             ),
    .NET_RX_AXIS_tlast        ( net_rx_tlast             ),
    .NET_RX_AXIS_tvalid       ( net_rx_tvalid            ),
    .NET_RX_AXIS_tready       ( net_rx_tready            ),
    .NET_TX_AXIS_tdata        ( net_tx_tdata             ),
    .NET_TX_AXIS_tkeep        ( net_tx_tkeep             ),
    .NET_TX_AXIS_tlast        ( net_tx_tlast             ),
    .NET_TX_AXIS_tvalid       ( net_tx_tvalid            ),
    .NET_TX_AXIS_tready       ( net_tx_tready            ),

    // hwhs
    .HWHS_AXI_araddr          ( hwhs_axi_araddr          ),
    .HWHS_AXI_arprot          (                          ),
    .HWHS_AXI_arready         ( hwhs_axi_arready         ),
    .HWHS_AXI_arvalid         ( hwhs_axi_arvalid         ),
    .HWHS_AXI_awaddr          ( hwhs_axi_awaddr          ),
    .HWHS_AXI_awprot          (                          ),
    .HWHS_AXI_awready         ( hwhs_axi_awready         ),
    .HWHS_AXI_awvalid         ( hwhs_axi_awvalid         ),
    .HWHS_AXI_bready          ( hwhs_axi_bready          ),
    .HWHS_AXI_bresp           ( hwhs_axi_bresp           ),
    .HWHS_AXI_bvalid          ( hwhs_axi_bvalid          ),
    .HWHS_AXI_rdata           ( hwhs_axi_rdata           ),
    .HWHS_AXI_rready          ( hwhs_axi_rready          ),
    .HWHS_AXI_rresp           ( hwhs_axi_rresp           ),
    .HWHS_AXI_rvalid          ( hwhs_axi_rvalid          ),
    .HWHS_AXI_wdata           ( hwhs_axi_wdata           ),
    .HWHS_AXI_wready          ( hwhs_axi_wready          ),
    .HWHS_AXI_wstrb           ( hwhs_axi_wstrb           ),
    .HWHS_AXI_wvalid          ( hwhs_axi_wvalid          ),
    .SLOT_BRAM_addr           ( slot_bram_addr           ),
    .SLOT_BRAM_clk            ( slot_bram_clk            ),
    .SLOT_BRAM_din            ( slot_bram_din            ),
    .SLOT_BRAM_dout           ( slot_bram_dout           ),
    .SLOT_BRAM_en             ( slot_bram_en             ),
    .SLOT_BRAM_rst            ( slot_bram_rst            ),
    .SLOT_BRAM_we             ( slot_bram_we             ),
    .REPLY_BRAM_addr          ( reply_bram_addr          ),
    .REPLY_BRAM_clk           ( reply_bram_clk           ),
    .REPLY_BRAM_din           ( reply_bram_din           ),
    .REPLY_BRAM_dout          ( reply_bram_dout          ),
    .REPLY_BRAM_en            ( reply_bram_en            ),
    .REPLY_BRAM_rst           ( reply_bram_rst           ),
    .REPLY_BRAM_we            ( reply_bram_we            ),
    .TRACE_BRAM_addr          ( trace_bram_addr          ),
    .TRACE_BRAM_clk           ( trace_bram_clk           ),
    .TRACE_BRAM_din           ( trace_bram_din           ),
    .TRACE_BRAM_dout          ( trace_bram_dout          ),
    .TRACE_BRAM_en            ( trace_bram_en            ),
    .TRACE_BRAM_rst           ( trace_bram_rst           ),
    .TRACE_BRAM_we            ( trace_bram_we            ),
    .net_ctl                  ( net_ctl                  ),
    .ctl_status               ( ctl_status               ),

    // transport_demo
    .DEMO_M_AXIS_tdata        ( demo_tx_tdata            ),
    .DEMO_M_AXIS_tlast        ( demo_tx_tlast            ),
    .DEMO_M_AXIS_tready       ( demo_tx_tready           ),
    .DEMO_M_AXIS_tvalid       ( demo_tx_tvalid           ),
    .DEMO_S_AXIS_tdata        ( demo_rx_tdata            ),
    .DEMO_S_AXIS_tlast        ( demo_rx_tlast            ),
    .DEMO_S_AXIS_tready       ( demo_rx_tready           ),
    .DEMO_S_AXIS_tvalid       ( demo_rx_tvalid           ),
    .start_demo               ( start_demo               ),
    .reset_demo               ( reset_demo               ),
    .syn_sum                  ( syn_sum                  ),
    .freq_num                 ( freq_num                 ),
    .syn_depth                ( syn_depth                ),
    .reply_seq_off            ( reply_seq_off            ),
    .freq_span_mask           ( freq_span_mask           ),
    .lfsr_seed                ( lfsr_seed                ),
    .cmp_error                ( cmp_error                ),
    .cmp_valid                ( cmp_valid                ),
    .demo_done                ( demo_done                )
  );

  //------------------------------------------------------------------------------------------//
  // BALBOA
  //------------------------------------------------------------------------------------------//
  wire [511:0] stk_tx_tdata;
  wire [63:0]  stk_tx_tkeep;
  wire         stk_tx_tlast, stk_tx_tvalid, stk_tx_tready;

  eth_pad u_pad (
    .clk      ( net_clk       ),
    .rstn     ( stack_rstn    ),
    .s_tdata  ( stk_tx_tdata  ),
    .s_tkeep  ( stk_tx_tkeep  ),
    .s_tlast  ( stk_tx_tlast  ),
    .s_tvalid ( stk_tx_tvalid ),
    .s_tready ( stk_tx_tready ),
    .m_tdata  ( net_tx_tdata  ),
    .m_tkeep  ( net_tx_tkeep  ),
    .m_tlast  ( net_tx_tlast  ),
    .m_tvalid ( net_tx_tvalid ),
    .m_tready ( net_tx_tready )
  );

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

  balboa_net #(
    .N_STATS ( N_STATS )
  ) u_net (
    .clk           ( net_clk       ),
    .rstn          ( stack_rstn    ),
    .rx_tdata      ( net_rx_tdata  ),
    .rx_tkeep      ( net_rx_tkeep  ),
    .rx_tlast      ( net_rx_tlast  ),
    .rx_tvalid     ( net_rx_tvalid ),
    .rx_tready     ( net_rx_tready ),
    .tx_tdata      ( stk_tx_tdata  ),
    .tx_tkeep      ( stk_tx_tkeep  ),
    .tx_tlast      ( stk_tx_tlast  ),
    .tx_tvalid     ( stk_tx_tvalid ),
    .tx_tready     ( stk_tx_tready ),
    .set_ip_valid  ( set_ip_valid  ),
    .set_ip        ( set_ip        ),
    .set_mac_valid ( set_mac_valid ),
    .set_mac       ( set_mac       ),
    .arp_valid     ( arp_valid     ),
    .arp_ready     ( arp_ready     ),
    .arp_ip        ( arp_ip        ),
    .qp_valid      ( qp_valid      ),
    .qp_ready      ( qp_ready      ),
    .qp_ctx        ( qp_ctx        ),
    .conn_valid    ( conn_valid    ),
    .conn_ready    ( conn_ready    ),
    .conn_ctx      ( conn_ctx      ),
    .stats         ( net_stats     ),
    .sq_valid      ( sq_valid      ),
    .sq_ready      ( sq_ready      ),
    .sq_qpn        ( sq_qpn        ),
    .sq_raddr      ( sq_raddr      ),
    .sq_len        ( sq_len        ),
    .sq_tdata      ( tx_tdata      ),
    .sq_tkeep      ( tx_tkeep      ),
    .sq_tlast      ( tx_tlast      ),
    .sq_tvalid     ( tx_tvalid     ),
    .sq_tready     ( tx_tready     ),
    .ack_valid     ( ack_valid     ),
    .ack_ready     ( ack_ready     ),
    .rd_valid      ( rd_valid      ),
    .rd_ready      ( rd_ready      ),
    .rd_len        ( rd_len        ),
    .rd_tdata      ( rd_tdata      ),
    .rd_tkeep      ( rd_tkeep      ),
    .rd_tlast      ( rd_tlast      ),
    .rd_tvalid     ( rd_tvalid     ),
    .rd_tready     ( rd_tready     ),
    .wr_valid      ( wr_valid      ),
    .wr_ready      ( wr_ready      ),
    .wr_vaddr      ( wr_vaddr      ),
    .wr_len        ( wr_len        ),
    .wr_tdata      ( wr_tdata      ),
    .wr_tkeep      ( wr_tkeep      ),
    .wr_tlast      ( wr_tlast      ),
    .wr_tvalid     ( wr_tvalid     ),
    .wr_tready     ( wr_tready     ),
    .mem_wr_valid  ( mem_wr_valid  ),
    .mem_wr_ready  ( mem_wr_ready  ),
    .mem_wr_cmd    ( mem_wr_cmd    ),
    .mem_wr_tdata  ( mem_wr_tdata  ),
    .mem_wr_tlast  ( mem_wr_tlast  ),
    .mem_wr_tvalid ( mem_wr_tvalid ),
    .mem_wr_tready ( mem_wr_tready ),
    .mem_rd_valid  ( mem_rd_valid  ),
    .mem_rd_ready  ( mem_rd_ready  ),
    .mem_rd_cmd    ( mem_rd_cmd    ),
    .mem_rd_tdata  ( mem_rd_tdata  ),
    .mem_rd_tkeep  ( mem_rd_tkeep  ),
    .mem_rd_tlast  ( mem_rd_tlast  ),
    .mem_rd_tvalid ( mem_rd_tvalid ),
    .mem_rd_tready ( mem_rd_tready )
  );

  //------------------------------------------------------------------------------------------//
  // hwhs-balboa. Its registers are reset with the net clock only: a stack reset resets the
  // engine and the retransmission buffer but keeps the registers, so the A72 can push the
  // addresses and contexts again right after.
  //------------------------------------------------------------------------------------------//
  hwhs #(
    .CLK_HZ        ( `NET_CLK_FREQ    ),
    .BUILD_ID      ( BUILD_ID         ),
    .N_STATS       ( N_STATS          ),
    .RETX_OFFS_LSB ( RETX_OFFS_LSB    )
  ) u_hwhs (
    .clk               ( net_clk                  ),
    .rstn              ( net_resetn               ),
    .stack_rstn        ( stack_rstn               ),
    .s_axi_awaddr      ( hwhs_axi_awaddr[15:0]    ),
    .s_axi_awvalid     ( hwhs_axi_awvalid         ),
    .s_axi_awready     ( hwhs_axi_awready         ),
    .s_axi_wdata       ( hwhs_axi_wdata           ),
    .s_axi_wstrb       ( hwhs_axi_wstrb           ),
    .s_axi_wvalid      ( hwhs_axi_wvalid          ),
    .s_axi_wready      ( hwhs_axi_wready          ),
    .s_axi_bresp       ( hwhs_axi_bresp           ),
    .s_axi_bvalid      ( hwhs_axi_bvalid          ),
    .s_axi_bready      ( hwhs_axi_bready          ),
    .s_axi_araddr      ( hwhs_axi_araddr[15:0]    ),
    .s_axi_arvalid     ( hwhs_axi_arvalid         ),
    .s_axi_arready     ( hwhs_axi_arready         ),
    .s_axi_rdata       ( hwhs_axi_rdata           ),
    .s_axi_rresp       ( hwhs_axi_rresp           ),
    .s_axi_rvalid      ( hwhs_axi_rvalid          ),
    .s_axi_rready      ( hwhs_axi_rready          ),
    .slot_bram_rst     ( slot_bram_rst            ),
    .slot_bram_en      ( slot_bram_en             ),
    .slot_bram_we      ( slot_bram_we             ),
    .slot_bram_addr    ( slot_bram_addr           ),
    .slot_bram_wrdata  ( slot_bram_din            ),
    .slot_bram_rddata  ( slot_bram_dout           ),
    .reply_bram_rst    ( reply_bram_rst           ),
    .reply_bram_en     ( reply_bram_en            ),
    .reply_bram_we     ( reply_bram_we            ),
    .reply_bram_addr   ( reply_bram_addr          ),
    .reply_bram_wrdata ( reply_bram_din           ),
    .reply_bram_rddata ( reply_bram_dout          ),
    .trace_bram_rst    ( trace_bram_rst           ),
    .trace_bram_en     ( trace_bram_en            ),
    .trace_bram_we     ( trace_bram_we            ),
    .trace_bram_addr   ( trace_bram_addr          ),
    .trace_bram_wrdata ( trace_bram_din           ),
    .trace_bram_rddata ( trace_bram_dout          ),
    .s_axis_tdata      ( demo_tx_tdata            ),
    .s_axis_tvalid     ( demo_tx_tvalid           ),
    .s_axis_tready     ( demo_tx_tready           ),
    .s_axis_tlast      ( demo_tx_tlast            ),
    .m_axis_tdata      ( demo_rx_tdata            ),
    .m_axis_tvalid     ( demo_rx_tvalid           ),
    .m_axis_tready     ( demo_rx_tready           ),
    .m_axis_tlast      ( demo_rx_tlast            ),
    .start_demo        ( start_demo               ),
    .reset_demo        ( reset_demo               ),
    .syn_sum           ( syn_sum                  ),
    .freq_num          ( freq_num                 ),
    .syn_depth         ( syn_depth                ),
    .reply_seq_off     ( reply_seq_off            ),
    .freq_span_mask    ( freq_span_mask           ),
    .lfsr_seed         ( lfsr_seed                ),
    .cmp_error         ( cmp_error                ),
    .cmp_valid         ( cmp_valid                ),
    .demo_done         ( demo_done                ),
    .set_ip_valid      ( set_ip_valid             ),
    .set_ip            ( set_ip                   ),
    .set_mac_valid     ( set_mac_valid            ),
    .set_mac           ( set_mac                  ),
    .arp_valid         ( arp_valid                ),
    .arp_ready         ( arp_ready                ),
    .arp_ip            ( arp_ip                   ),
    .qp_valid          ( qp_valid                 ),
    .qp_ready          ( qp_ready                 ),
    .qp_ctx            ( qp_ctx                   ),
    .conn_valid        ( conn_valid               ),
    .conn_ready        ( conn_ready               ),
    .conn_ctx          ( conn_ctx                 ),
    .net_stats         ( net_stats                ),
    .sq_valid          ( sq_valid                 ),
    .sq_ready          ( sq_ready                 ),
    .sq_qpn            ( sq_qpn                   ),
    .sq_raddr          ( sq_raddr                 ),
    .sq_len            ( sq_len                   ),
    .tx_tdata          ( tx_tdata                 ),
    .tx_tkeep          ( tx_tkeep                 ),
    .tx_tlast          ( tx_tlast                 ),
    .tx_tvalid         ( tx_tvalid                ),
    .tx_tready         ( tx_tready                ),
    .ack_valid         ( ack_valid                ),
    .ack_ready         ( ack_ready                ),
    .rd_valid          ( rd_valid                 ),
    .rd_ready          ( rd_ready                 ),
    .rd_len            ( rd_len                   ),
    .rd_tdata          ( rd_tdata                 ),
    .rd_tkeep          ( rd_tkeep                 ),
    .rd_tlast          ( rd_tlast                 ),
    .rd_tvalid         ( rd_tvalid                ),
    .rd_tready         ( rd_tready                ),
    .wr_valid          ( wr_valid                 ),
    .wr_ready          ( wr_ready                 ),
    .wr_tdata          ( wr_tdata                 ),
    .wr_tkeep          ( wr_tkeep                 ),
    .wr_tlast          ( wr_tlast                 ),
    .wr_tvalid         ( wr_tvalid                ),
    .wr_tready         ( wr_tready                ),
    .mem_wr_valid      ( mem_wr_valid             ),
    .mem_wr_ready      ( mem_wr_ready             ),
    .mem_wr_cmd        ( mem_wr_cmd               ),
    .mem_wr_tdata      ( mem_wr_tdata             ),
    .mem_wr_tlast      ( mem_wr_tlast             ),
    .mem_wr_tvalid     ( mem_wr_tvalid            ),
    .mem_wr_tready     ( mem_wr_tready            ),
    .mem_rd_valid      ( mem_rd_valid             ),
    .mem_rd_ready      ( mem_rd_ready             ),
    .mem_rd_cmd        ( mem_rd_cmd               ),
    .mem_rd_tdata      ( mem_rd_tdata             ),
    .mem_rd_tkeep      ( mem_rd_tkeep             ),
    .mem_rd_tlast      ( mem_rd_tlast             ),
    .mem_rd_tvalid     ( mem_rd_tvalid            ),
    .mem_rd_tready     ( mem_rd_tready            ),
    .net_status        ( net_status               )
  );

endmodule

`default_nettype wire
