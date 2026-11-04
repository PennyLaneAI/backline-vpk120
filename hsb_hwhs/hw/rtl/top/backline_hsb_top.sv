// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Xanadu Quantum Technologies Inc.
//===----------------------------------------------------------------------===//
//
// backline_hsb_top.sv - VPK120 top: NVIDIA HSB
//
//   u_hsb             NVIDIA HSB (HOLOLINK_top):
//                       - host port 0 is the 100G MAC (hif0_*)
//                       - host port 1 is hwhs's ECB mailbox (ecb_*)
//                       - sensor-RX/TX 0 go to hwhs (sif0_*)
//
// 64 B @ hif for HSB IP
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
  // clocks and reset
  input  wire                       apb_clk,         // HSB control plane
  input  wire                       hif_clk,         // HSB host/sensor clock
  input  wire                       ptp_clk,         // HSB PTP timer
  input  wire                       hsb_sys_rst,     // HSB i_sys_rst, apb domain
  output wire                       hsb_init_done,   // HSB o_init_done, apb domain

  // host port 0 <-> the 100G MAC
  input  wire [`HOST_WIDTH-1:0]     hif0_rx_tdata,
  input  wire [`HOSTKEEP_WIDTH-1:0] hif0_rx_tkeep,
  input  wire [`HOSTKEEP_WIDTH-1:0] hif0_rx_tuser,   // frame error, per byte
  input  wire                       hif0_rx_tlast,
  input  wire                       hif0_rx_tvalid,
  output wire                       hif0_rx_tready,
  output wire [`HOST_WIDTH-1:0]     hif0_tx_tdata,
  output wire [`HOSTKEEP_WIDTH-1:0] hif0_tx_tkeep,
  output wire                       hif0_tx_tlast,
  output wire                       hif0_tx_tvalid,
  input  wire                       hif0_tx_tready,

  // host port 1 <-> hwhs ECB mailbox
  input  wire [`HOST_WIDTH-1:0]     ecb_rx_tdata,
  input  wire [`HOSTKEEP_WIDTH-1:0] ecb_rx_tkeep,
  input  wire                       ecb_rx_tlast,
  input  wire                       ecb_rx_tvalid,
  output wire                       ecb_rx_tready,
  output wire [`HOST_WIDTH-1:0]     ecb_tx_tdata,
  output wire [`HOSTKEEP_WIDTH-1:0] ecb_tx_tkeep,
  output wire                       ecb_tx_tlast,
  output wire                       ecb_tx_tvalid,   // always accepted, no tready

  // sensor RX/TX 0 <-> hwhs
  input  wire [`DATAPATH_WIDTH-1:0] sif0_rx_tdata,
  input  wire [`DATAKEEP_WIDTH-1:0] sif0_rx_tkeep,
  input  wire                       sif0_rx_tlast,
  input  wire                       sif0_rx_tvalid,
  output wire                       sif0_rx_tready,
  output wire [`DATAPATH_WIDTH-1:0] sif0_tx_tdata,
  output wire [`DATAKEEP_WIDTH-1:0] sif0_tx_tkeep,
  output wire                       sif0_tx_tlast,
  output wire                       sif0_tx_tvalid,
  input  wire                       sif0_tx_tready
);

  // HSB host port of each link
  localparam int HIF_MAC = 0;   // MRMAC 100G to the coprocessor
  localparam int HIF_ECB = 1;   // the A72's ECB mailbox in hwhs

  localparam logic [31:0] BUILD_ID = `BACKLINE_BUILD_ID;

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
  assign hif_rx_tvalid[HIF_ECB] = ecb_rx_tvalid;
  assign hif_rx_tlast [HIF_ECB] = ecb_rx_tlast;
  assign hif_rx_tdata [HIF_ECB] = ecb_rx_tdata;
  assign hif_rx_tkeep [HIF_ECB] = ecb_rx_tkeep;
  assign hif_rx_tuser [HIF_ECB] = '0;
  assign ecb_rx_tready          = hif_rx_tready[HIF_ECB];

  assign ecb_tx_tvalid          = hif_tx_tvalid[HIF_ECB];
  assign ecb_tx_tlast           = hif_tx_tlast [HIF_ECB];
  assign ecb_tx_tdata           = hif_tx_tdata [HIF_ECB];
  assign ecb_tx_tkeep           = hif_tx_tkeep [HIF_ECB];
  assign hif_tx_tready[HIF_ECB] = 1'b1;

  // sensor RX/TX 0 <-> hwhs
  assign sif_rx_tvalid[0] = sif0_rx_tvalid;
  assign sif_rx_tlast [0] = sif0_rx_tlast;
  assign sif_rx_tdata [0] = sif0_rx_tdata;
  assign sif_rx_tkeep [0] = sif0_rx_tkeep;
  assign sif_rx_tuser [0] = '0;
  assign sif0_rx_tready   = sif_rx_tready[0];

  assign sif0_tx_tvalid   = sif_tx_tvalid[0];
  assign sif0_tx_tlast    = sif_tx_tlast [0];
  assign sif0_tx_tdata    = sif_tx_tdata [0];
  assign sif0_tx_tkeep    = sif_tx_tkeep [0];
  assign sif_tx_tready[0] = sif0_tx_tready;

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

endmodule

`default_nettype wire
