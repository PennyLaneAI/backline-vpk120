// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Xanadu Quantum Technologies Inc.
//===----------------------------------------------------------------------===//
//
// backline_hsb_top.sv - VPK120 top: MRMAC 100G + NVIDIA HSB + hwhs-hsb
//
//   QSFP-DD 100G, to the coprocessor
//      |
//   i_mrmac_0_exdes   AMD's MRMAC example design: the 100G MAC
//      |
//   i_platform        block design: CIPS (A72), LPDDR4, clocks, BRAM controllers,
//      |              transport_demo, and the MAC <-> HSB width and clock converters
//   u_hsb             NVIDIA HSB (HOLOLINK_top):
//      |                - host port 0 is the MAC,
//      |                - host port 1 is hwhs's ECB mailbox
//      |                - sensor-RX/TX 0 go to hwhs
//   u_hwhs            hwhs-hsb: registers, round engine, RAMs, ECB mailbox
//
// The MAC side is 384 bit @ 390.625 MHz, everything from HSB down is 64 B @ hif.
//
//===----------------------------------------------------------------------===//

`include "HOLOLINK_def.svh"

`ifndef BACKLINE_BUILD_ID
`define BACKLINE_BUILD_ID 0
`endif

`timescale 1ps/1ps
`default_nettype none

(* DowngradeIPIdentifiedWarnings="yes" *)
module backline_hsb_top
  import HOLOLINK_pkg::*;
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

  // HSB host port of each link
  localparam int HIF_MAC = 0;   // MRMAC 100G to the coprocessor
  localparam int HIF_ECB = 1;   // the A72's ECB mailbox in hwhs

  localparam logic [31:0] BUILD_ID = `BACKLINE_BUILD_ID;

  //------------------------------------------------------------------------------------------//
  // Clocks and resets
  //------------------------------------------------------------------------------------------//
  wire pl_clk, pl_resetn;        // CIPS pl0, 200 MHz: MRMAC AXI-Lite, GPIOs
  wire hif_clk, hif_resetn;      // HSB host/sensor clock, hwhs, transport_demo
  wire apb_clk, apb_resetn;      // HSB control plane
  wire ptp_clk;                  // HSB PTP timer
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
  // HSB readiness, seen from the MRMAC RX clock and from hif
  //------------------------------------------------------------------------------------------//
  wire        hsb_init_done;     // HSB o_init_done (apb domain)
  wire [31:0] hsb_ctl;           // axi_gpio_ctl channel 1 (pl0 domain)
  wire        hsb_reset_pl = hsb_ctl[0];   // 1: hold HSB in reset

  // Clock crossings use xpm_cdc so they carry their own timing constraints.
  wire hsb_reset_apb;
  xpm_cdc_single #(.DEST_SYNC_FF(3), .INIT_SYNC_FF(1), .SRC_INPUT_REG(0)) u_cdc_hsb_reset (
    .src_clk(1'b0), .src_in(hsb_reset_pl), .dest_clk(apb_clk), .dest_out(hsb_reset_apb));

  logic hsb_sys_rst = 1'b1;   // HSB i_sys_rst, apb domain
  always_ff @(posedge apb_clk) hsb_sys_rst <= !apb_resetn || hsb_reset_apb;

  logic hsb_up = 1'b0;        // HSB is out of reset and initialized
  always_ff @(posedge apb_clk) hsb_up <= hsb_init_done && !hsb_sys_rst;

  wire hsb_up_rx;
  xpm_cdc_single #(.DEST_SYNC_FF(3), .INIT_SYNC_FF(1), .SRC_INPUT_REG(0)) u_cdc_hsb_up_rx (
    .src_clk(1'b0), .src_in(hsb_up), .dest_clk(rx_axis_clk), .dest_out(hsb_up_rx));

  wire [5:0] hsb_sts_hif;     // {stat_mst_reset_done, hsb_sys_rst, hsb_init_done} in hif
  xpm_cdc_array_single #(.DEST_SYNC_FF(3), .INIT_SYNC_FF(1), .SRC_INPUT_REG(0), .WIDTH(6)) u_cdc_sts_hif (
    .src_clk(1'b0), .src_in({stat_mst_reset_done, hsb_sys_rst, hsb_init_done}),
    .dest_clk(hif_clk), .dest_out(hsb_sts_hif));

  // hwhs HSB_STATUS (0x64)
  wire [31:0] hsb_status = {24'd0, hsb_sts_hif[5:2], 2'd0, hsb_sts_hif[1], hsb_sts_hif[0]};

  // axi_gpio_ctl channel 2 (0xA409_0008): the GPIO synchronizes its inputs to pl0
  wire [31:0] ctl_status = {24'd0, stat_mst_reset_done, 1'b0, hsb_sys_rst, hsb_init_done, clk_locked};

  //------------------------------------------------------------------------------------------//
  // MRMAC RX gate: MRMAC cannot be stalled, so whole frames are dropped at the MAC while HSB
  // is down or held in reset.
  //------------------------------------------------------------------------------------------//
  logic rx_in_pkt, rx_pass;
  always_ff @(posedge rx_axis_clk) begin
    if (rx_axis_rst) begin
      rx_in_pkt <= 1'b0;
      rx_pass   <= 1'b0;
    end
    else if (rx_axis_tvalid) begin
      if (!rx_in_pkt) rx_pass <= hsb_up_rx;   // decided at the first beat
      rx_in_pkt <= !rx_axis_tlast;
    end
  end
  wire rx_gate_tvalid = rx_axis_tvalid && (rx_in_pkt ? rx_pass : hsb_up_rx);

  //------------------------------------------------------------------------------------------//
  // Platform BD
  //------------------------------------------------------------------------------------------//
  // HSB host port 0 <-> MRMAC, 64 B @ hif
  wire [511:0] hif0_rx_tdata;
  wire [63:0]  hif0_rx_tkeep, hif0_rx_tuser;
  wire         hif0_rx_tlast, hif0_rx_tvalid, hif0_rx_tready;
  wire [511:0] hif0_tx_tdata;
  wire [63:0]  hif0_tx_tkeep;
  wire         hif0_tx_tlast, hif0_tx_tvalid, hif0_tx_tready;

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
    .hif_clk                  ( hif_clk                  ),
    .hif_resetn               ( hif_resetn               ),
    .apb_clk                  ( apb_clk                  ),
    .apb_resetn               ( apb_resetn               ),
    .ptp_clk                  ( ptp_clk                  ),
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

    // MRMAC 384b <-> HSB host port 0 512b
    .cmac_rx_clk              ( rx_axis_clk              ),
    .cmac_rx_resetn           ( rx_axis_rstn             ),
    .MRMAC_RX_AXIS_tdata      ( rx_axis_tdata            ),
    .MRMAC_RX_AXIS_tkeep      ( rx_axis_tkeep            ),
    .MRMAC_RX_AXIS_tlast      ( rx_axis_tlast            ),
    .MRMAC_RX_AXIS_tuser      ( {48{rx_axis_tuser}}      ),   // frame error on every byte, see platform.tcl
    .MRMAC_RX_AXIS_tvalid     ( rx_gate_tvalid           ),
    .MRMAC_RX_AXIS_tready     (                          ),   // MRMAC RX cannot stall
    .cmac_tx_clk              ( tx_axis_clk              ),
    .cmac_tx_resetn           ( tx_axis_rstn             ),
    .MRMAC_TX_AXIS_tdata      ( tx_axis_tdata            ),
    .MRMAC_TX_AXIS_tkeep      ( tx_axis_tkeep            ),
    .MRMAC_TX_AXIS_tlast      ( tx_axis_tlast            ),
    .MRMAC_TX_AXIS_tvalid     ( tx_axis_tvalid           ),
    .MRMAC_TX_AXIS_tready     ( tx_axis_tready           ),
    .HSB_RX_AXIS_tdata        ( hif0_rx_tdata            ),
    .HSB_RX_AXIS_tkeep        ( hif0_rx_tkeep            ),
    .HSB_RX_AXIS_tlast        ( hif0_rx_tlast            ),
    .HSB_RX_AXIS_tuser        ( hif0_rx_tuser            ),
    .HSB_RX_AXIS_tvalid       ( hif0_rx_tvalid           ),
    .HSB_RX_AXIS_tready       ( hif0_rx_tready           ),
    .HSB_TX_AXIS_tdata        ( hif0_tx_tdata            ),
    .HSB_TX_AXIS_tkeep        ( hif0_tx_tkeep            ),
    .HSB_TX_AXIS_tlast        ( hif0_tx_tlast            ),
    .HSB_TX_AXIS_tvalid       ( hif0_tx_tvalid           ),
    .HSB_TX_AXIS_tready       ( hif0_tx_tready           ),

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
    .hsb_ctl                  ( hsb_ctl                  ),
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
  // HSB
  //------------------------------------------------------------------------------------------//
  logic [47:0]                mac_addr  [`HOST_IF_INST-1:0];
  assign mac_addr[HIF_MAC] = 48'h0293_C648_4D8E;
  assign mac_addr[HIF_ECB] = 48'h0293_C648_4D8F;

  logic [`REG_INST-1:0]       apb_psel;
  logic                       apb_penable;
  logic [31:0]                apb_paddr;
  logic [31:0]                apb_pwdata;
  logic                       apb_pwrite;
  logic [31:0]                apb_prdata [`REG_INST-1:0];
  assign apb_prdata = '{default:32'h0};

  logic [`HOST_IF_INST-1:0]   hif_rx_tvalid, hif_rx_tlast, hif_rx_tready;
  logic [`HOST_WIDTH-1:0]     hif_rx_tdata [`HOST_IF_INST-1:0];
  logic [`HOSTKEEP_WIDTH-1:0] hif_rx_tkeep [`HOST_IF_INST-1:0];
  logic [`HOSTUSER_WIDTH-1:0] hif_rx_tuser [`HOST_IF_INST-1:0];
  logic [`HOST_IF_INST-1:0]   hif_tx_tvalid, hif_tx_tlast, hif_tx_tready;
  logic [`HOST_WIDTH-1:0]     hif_tx_tdata [`HOST_IF_INST-1:0];
  logic [`HOSTKEEP_WIDTH-1:0] hif_tx_tkeep [`HOST_IF_INST-1:0];
  logic [`HOSTUSER_WIDTH-1:0] hif_tx_tuser [`HOST_IF_INST-1:0];

  logic [`SENSOR_RX_IF_INST-1:0] sif_rx_tvalid, sif_rx_tlast, sif_rx_tready;
  logic [`DATAPATH_WIDTH-1:0]    sif_rx_tdata [`SENSOR_RX_IF_INST-1:0];
  logic [`DATAKEEP_WIDTH-1:0]    sif_rx_tkeep [`SENSOR_RX_IF_INST-1:0];
  logic [`DATAUSER_WIDTH-1:0]    sif_rx_tuser [`SENSOR_RX_IF_INST-1:0];
  logic [`SENSOR_TX_IF_INST-1:0] sif_tx_tvalid, sif_tx_tlast, sif_tx_tready;
  logic [`DATAPATH_WIDTH-1:0]    sif_tx_tdata [`SENSOR_TX_IF_INST-1:0];
  logic [`DATAKEEP_WIDTH-1:0]    sif_tx_tkeep [`SENSOR_TX_IF_INST-1:0];
  logic [`DATAUSER_WIDTH-1:0]    sif_tx_tuser [`SENSOR_TX_IF_INST-1:0];

  // host port 0 <-> MRMAC
  assign hif_rx_tvalid[HIF_MAC] = hif0_rx_tvalid;
  assign hif_rx_tlast [HIF_MAC] = hif0_rx_tlast;
  assign hif_rx_tdata [HIF_MAC] = hif0_rx_tdata;
  assign hif_rx_tkeep [HIF_MAC] = hif0_rx_tkeep;
  assign hif_rx_tuser [HIF_MAC] = |(hif0_rx_tuser & hif0_rx_tkeep);   // frame error, on the last beat
  assign hif0_rx_tready         = hif_rx_tready[HIF_MAC];

  assign hif0_tx_tvalid         = hif_tx_tvalid[HIF_MAC];
  assign hif0_tx_tlast          = hif_tx_tlast [HIF_MAC];
  assign hif0_tx_tdata          = hif_tx_tdata [HIF_MAC];
  assign hif0_tx_tkeep          = hif_tx_tkeep [HIF_MAC];
  assign hif_tx_tready[HIF_MAC] = hif0_tx_tready;

  // host port 1 <-> hwhs ECB mailbox
  assign hif_rx_tuser [HIF_ECB] = '0;
  assign hif_tx_tready[HIF_ECB] = 1'b1;

  assign sif_rx_tuser[0] = '0;

  HOLOLINK_top #(
    .BUILD_REV         ( {16'd0, BUILD_ID}  )   // HSB reg 0x80 = {BUILD_ID[15:0], 16'h2609}
  ) u_hsb (
    .i_sys_rst         ( hsb_sys_rst        ),
    .i_apb_clk         ( apb_clk            ),
    .o_apb_rst         (                    ),
    .o_apb_psel        ( apb_psel           ),
    .o_apb_penable     ( apb_penable        ),
    .o_apb_paddr       ( apb_paddr          ),
    .o_apb_pwdata      ( apb_pwdata         ),
    .o_apb_pwrite      ( apb_pwrite         ),
    .i_apb_pready      ( '1                 ),   // no user APB slaves yet
    .i_apb_prdata      ( apb_prdata         ),
    .i_apb_pserr       ( '0                 ),
    .i_mac_addr        ( mac_addr           ),
    .i_board_sn        ( '0                 ),
    .i_enum_vld        ( 1'b1               ),
    .o_init_done       ( hsb_init_done      ),
    .i_sif_rx_clk      ( hif_clk            ),
    .o_sif_rx_rst      (                    ),
    .i_sif_axis_tvalid ( sif_rx_tvalid      ),
    .i_sif_axis_tlast  ( sif_rx_tlast       ),
    .i_sif_axis_tdata  ( sif_rx_tdata       ),
    .i_sif_axis_tkeep  ( sif_rx_tkeep       ),
    .i_sif_axis_tuser  ( sif_rx_tuser       ),
    .o_sif_axis_tready ( sif_rx_tready      ),
    .i_sif_tx_clk      ( hif_clk            ),
    .o_sif_tx_rst      (                    ),
    .o_sif_axis_tvalid ( sif_tx_tvalid      ),
    .o_sif_axis_tlast  ( sif_tx_tlast       ),
    .o_sif_axis_tdata  ( sif_tx_tdata       ),
    .o_sif_axis_tkeep  ( sif_tx_tkeep       ),
    .o_sif_axis_tuser  ( sif_tx_tuser       ),
    .i_sif_axis_tready ( sif_tx_tready      ),
    .i_sif_event       ( 16'd0              ),
    .i_hif_clk         ( hif_clk            ),
    .o_hif_rst         (                    ),
    .i_hif_axis_tvalid ( hif_rx_tvalid      ),
    .i_hif_axis_tlast  ( hif_rx_tlast       ),
    .i_hif_axis_tdata  ( hif_rx_tdata       ),
    .i_hif_axis_tkeep  ( hif_rx_tkeep       ),
    .i_hif_axis_tuser  ( hif_rx_tuser       ),
    .o_hif_axis_tready ( hif_rx_tready      ),
    .o_hif_axis_tvalid ( hif_tx_tvalid      ),
    .o_hif_axis_tlast  ( hif_tx_tlast       ),
    .o_hif_axis_tdata  ( hif_tx_tdata       ),
    .o_hif_axis_tkeep  ( hif_tx_tkeep       ),
    .o_hif_axis_tuser  ( hif_tx_tuser       ),
    .i_hif_axis_tready ( hif_tx_tready      ),
    .o_spi_csn         (                    ),
    .o_spi_sck         (                    ),
    .i_spi_sdio        ( '{4'b0}            ),
    .o_spi_sdio        (                    ),
    .o_spi_oen         (                    ),
    .i_i2c_scl         ( '1                 ),
    .i_i2c_sda         ( '1                 ),
    .o_i2c_scl_en      (                    ),
    .o_i2c_sda_en      (                    ),
    .i_gpio            ( '0                 ),
    .o_gpio            (                    ),
    .o_gpio_dir        (                    ),
    .o_sw_sys_rst      (                    ),   // HSB resets itself from it internally
    .o_sw_sen_rst      (                    ),
    .i_ptp_clk         ( ptp_clk            ),
    .o_ptp_rst         (                    ),
    .o_ptp_sec         (                    ),
    .o_ptp_nanosec     (                    ),
    .o_pps             (                    )
  );

  //------------------------------------------------------------------------------------------//
  // hwhs-hsb
  //------------------------------------------------------------------------------------------//
  hwhs #(
    .CLK_HZ   ( `HIF_CLK_FREQ       ),
    .BUILD_ID ( BUILD_ID            )
  ) u_hwhs (
    .clk               ( hif_clk                  ),
    .rstn              ( hif_resetn               ),
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
    .sif_rx_tdata      ( sif_rx_tdata[0]          ),
    .sif_rx_tkeep      ( sif_rx_tkeep[0]          ),
    .sif_rx_tlast      ( sif_rx_tlast[0]          ),
    .sif_rx_tvalid     ( sif_rx_tvalid[0]         ),
    .sif_rx_tready     ( sif_rx_tready[0]         ),
    .sif_tx_tdata      ( sif_tx_tdata[0]          ),
    .sif_tx_tkeep      ( sif_tx_tkeep[0]          ),
    .sif_tx_tlast      ( sif_tx_tlast[0]          ),
    .sif_tx_tvalid     ( sif_tx_tvalid[0]         ),
    .sif_tx_tready     ( sif_tx_tready[0]         ),
    .ecb_rx_tdata      ( hif_rx_tdata[HIF_ECB]    ),
    .ecb_rx_tkeep      ( hif_rx_tkeep[HIF_ECB]    ),
    .ecb_rx_tlast      ( hif_rx_tlast[HIF_ECB]    ),
    .ecb_rx_tvalid     ( hif_rx_tvalid[HIF_ECB]   ),
    .ecb_rx_tready     ( hif_rx_tready[HIF_ECB]   ),
    .ecb_tx_tdata      ( hif_tx_tdata[HIF_ECB]    ),
    .ecb_tx_tkeep      ( hif_tx_tkeep[HIF_ECB]    ),
    .ecb_tx_tlast      ( hif_tx_tlast[HIF_ECB]    ),
    .ecb_tx_tvalid     ( hif_tx_tvalid[HIF_ECB]   ),
    .ecb_tx_tready     (                          ),   // always 1, tied above
    .hsb_status        ( hsb_status               )
  );

endmodule

`default_nettype wire
