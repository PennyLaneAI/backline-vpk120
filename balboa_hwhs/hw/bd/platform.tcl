# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Xanadu Quantum Technologies Inc.
#
# platform.tcl
#
#   cips_block            AMD's CIPS hierarchy (amd_procs.tcl), plus M_AXI_LPD enabled
#   axi_noc_0/1/2         AMD's LPDDR4 NoC (amd_procs.tcl), PS ports only
#   clk_wizard_net        the net clock (BALBOA, hwhs, transport_demo) from pl0
#   axis_* (rx)           MRMAC 48 B @ 390.625 -> 64 B -> net clock          -> NET_RX_AXIS
#   axis_* (tx)           NET_TX_AXIS -> 390.625 -> 48 B, packet FIFO        -> MRMAC_TX_AXIS
#   smartconnect_hwhs     FPD 0xA400_0000 -> HWHS_AXI (AXI-Lite in the net clock domain)
#   smartconnect_lpd      LPD -> slot / reply / trace / demo BRAM controllers
#   axi_bram_ctrl_*       PS side of the hwhs RAMs (exported) and of the demo table
#   transport_demo_0      the hw-loop syndrome pacer (module reference, unchanged)
#   axi_gpio_id/_ctl      build marker, BALBOA reset and status, all in the pl0 domain
#
# The receive side has no packet FIFO: BALBOA's IP handler takes a frame as it comes, and a
# store-and-forward stage would add a frame's time to every round. The transmit side keeps
# one, since the MAC must not run dry inside a frame.
#
# PS address map:
#   0xA400_0000  64K  hwhs registers                   (HWHS_AXI)
#   0xA404_0000  64K  MRMAC                            (MRMAC_AXI)
#   0xA405_0000  64K  axi_gpio_id   ch1 BUILD_ID, ch2 net clock in Hz
#   0xA406_0000  64K  cips_block/axi_gpio_gt_reset_mask (AMD)
#   0xA407_0000  64K  cips_block/gtctrl                 (AMD)
#   0xA408_0000  64K  cips_block/axi_gpio_debug         (AMD)
#   0xA409_0000  64K  axi_gpio_ctl  ch1 out [0]=BALBOA reset, ch2 in = status
#   0x8000_0000   1M  transport_demo table   (LPD)
#   0x9100_0000  64K  slot RAM               (LPD)
#   0x9200_0000   2M  RTT trace RAM          (LPD)
#   0x9300_0000  64K  reply RAM              (LPD)

proc cr_bd_platform { net_mhz build_id } {

  set design_name platform
  create_bd_design $design_name
  current_bd_instance /

  set net_hz   [expr {round($net_mhz * 1000000.0)}]
  set pl0_hz   199999817
  set mrmac_hz 390625000

  # ==================================================================================
  # External interface ports
  # ==================================================================================
  set MRMAC_AXI [ create_bd_intf_port -mode Master -vlnv xilinx.com:interface:aximm_rtl:1.0 MRMAC_AXI ]
  set_property -dict [ list \
   CONFIG.ADDR_WIDTH {32} \
   CONFIG.DATA_WIDTH {32} \
   CONFIG.FREQ_HZ $pl0_hz \
   CONFIG.PROTOCOL {AXI4LITE} \
   ] $MRMAC_AXI

  set HWHS_AXI [ create_bd_intf_port -mode Master -vlnv xilinx.com:interface:aximm_rtl:1.0 HWHS_AXI ]
  set_property -dict [ list \
   CONFIG.ADDR_WIDTH {32} \
   CONFIG.DATA_WIDTH {32} \
   CONFIG.FREQ_HZ $net_hz \
   CONFIG.PROTOCOL {AXI4LITE} \
   ] $HWHS_AXI

  create_bd_intf_port -mode Master -vlnv xilinx.com:interface:lpddr4_rtl:1.0 ch0_lpddr4_trip1
  create_bd_intf_port -mode Master -vlnv xilinx.com:interface:lpddr4_rtl:1.0 ch1_lpddr4_trip1
  create_bd_intf_port -mode Master -vlnv xilinx.com:interface:lpddr4_rtl:1.0 ch0_lpddr4_trip2
  create_bd_intf_port -mode Master -vlnv xilinx.com:interface:lpddr4_rtl:1.0 ch1_lpddr4_trip2
  create_bd_intf_port -mode Master -vlnv xilinx.com:interface:lpddr4_rtl:1.0 ch0_lpddr4_trip3
  create_bd_intf_port -mode Master -vlnv xilinx.com:interface:lpddr4_rtl:1.0 ch1_lpddr4_trip3
  foreach n {lpddr4_clk1 lpddr4_clk2 lpddr4_clk3} {
    set p [ create_bd_intf_port -mode Slave -vlnv xilinx.com:interface:diff_clock_rtl:1.0 $n ]
    set_property CONFIG.FREQ_HZ {200000000} $p
  }

  # MRMAC side (exdes clocks).
  set MRMAC_RX_AXIS [ create_bd_intf_port -mode Slave -vlnv xilinx.com:interface:axis_rtl:1.0 MRMAC_RX_AXIS ]
  set_property -dict [ list \
   CONFIG.FREQ_HZ $mrmac_hz \
   CONFIG.HAS_TKEEP {1} \
   CONFIG.HAS_TLAST {1} \
   CONFIG.HAS_TREADY {1} \
   CONFIG.HAS_TSTRB {0} \
   CONFIG.LAYERED_METADATA {undef} \
   CONFIG.TDATA_NUM_BYTES {48} \
   CONFIG.TDEST_WIDTH {0} \
   CONFIG.TID_WIDTH {0} \
   CONFIG.TUSER_WIDTH {0} \
   ] $MRMAC_RX_AXIS

  set MRMAC_TX_AXIS [ create_bd_intf_port -mode Master -vlnv xilinx.com:interface:axis_rtl:1.0 MRMAC_TX_AXIS ]
  set_property CONFIG.FREQ_HZ $mrmac_hz $MRMAC_TX_AXIS

  # BALBOA side (net clock)
  set NET_RX_AXIS [ create_bd_intf_port -mode Master -vlnv xilinx.com:interface:axis_rtl:1.0 NET_RX_AXIS ]
  set_property CONFIG.FREQ_HZ $net_hz $NET_RX_AXIS

  set NET_TX_AXIS [ create_bd_intf_port -mode Slave -vlnv xilinx.com:interface:axis_rtl:1.0 NET_TX_AXIS ]
  set_property -dict [ list \
   CONFIG.FREQ_HZ $net_hz \
   CONFIG.HAS_TKEEP {1} \
   CONFIG.HAS_TLAST {1} \
   CONFIG.HAS_TREADY {1} \
   CONFIG.HAS_TSTRB {0} \
   CONFIG.LAYERED_METADATA {undef} \
   CONFIG.TDATA_NUM_BYTES {64} \
   CONFIG.TDEST_WIDTH {0} \
   CONFIG.TID_WIDTH {0} \
   CONFIG.TUSER_WIDTH {0} \
   ] $NET_TX_AXIS

  # transport_demo <-> hwhs (net clock)
  set DEMO_M_AXIS [ create_bd_intf_port -mode Master -vlnv xilinx.com:interface:axis_rtl:1.0 DEMO_M_AXIS ]
  set_property CONFIG.FREQ_HZ $net_hz $DEMO_M_AXIS

  set DEMO_S_AXIS [ create_bd_intf_port -mode Slave -vlnv xilinx.com:interface:axis_rtl:1.0 DEMO_S_AXIS ]
  set_property -dict [ list \
   CONFIG.FREQ_HZ $net_hz \
   CONFIG.HAS_TKEEP {0} \
   CONFIG.HAS_TLAST {1} \
   CONFIG.HAS_TREADY {1} \
   CONFIG.HAS_TSTRB {0} \
   CONFIG.LAYERED_METADATA {undef} \
   CONFIG.TDATA_NUM_BYTES {64} \
   CONFIG.TDEST_WIDTH {0} \
   CONFIG.TID_WIDTH {0} \
   CONFIG.TUSER_WIDTH {0} \
   ] $DEMO_S_AXIS

  # PS side of the hwhs RAMs
  foreach n {SLOT_BRAM REPLY_BRAM TRACE_BRAM} {
    set p [ create_bd_intf_port -mode Master -vlnv xilinx.com:interface:bram_rtl:1.0 $n ]
    set_property -dict [ list CONFIG.MASTER_TYPE {BRAM_CTRL} CONFIG.READ_WRITE_MODE {READ_WRITE} ] $p
  }

  # ==================================================================================
  # External ports
  # ==================================================================================
  set pl_clk [ create_bd_port -dir O -type clk pl_clk ]
  set_property -dict [ list CONFIG.ASSOCIATED_BUSIF {MRMAC_AXI} CONFIG.FREQ_HZ $pl0_hz ] $pl_clk
  create_bd_port -dir O -from 0 -to 0 -type rst pl_resetn

  set net_clk [ create_bd_port -dir O -type clk net_clk ]
  set_property -dict [ list \
   CONFIG.ASSOCIATED_BUSIF {HWHS_AXI:NET_RX_AXIS:NET_TX_AXIS:DEMO_M_AXIS:DEMO_S_AXIS} \
   CONFIG.ASSOCIATED_RESET {net_resetn} \
   CONFIG.FREQ_HZ $net_hz \
   ] $net_clk
  create_bd_port -dir O -from 0 -to 0 -type rst net_resetn
  create_bd_port -dir O clk_locked

  set cmac_rx_clk [ create_bd_port -dir I -type clk -freq_hz $mrmac_hz cmac_rx_clk ]
  set_property -dict [ list CONFIG.ASSOCIATED_BUSIF {MRMAC_RX_AXIS} CONFIG.ASSOCIATED_RESET {cmac_rx_resetn} ] $cmac_rx_clk
  set cmac_rx_resetn [ create_bd_port -dir I -type rst cmac_rx_resetn ]
  set_property CONFIG.POLARITY {ACTIVE_LOW} $cmac_rx_resetn
  set cmac_tx_clk [ create_bd_port -dir I -type clk -freq_hz $mrmac_hz cmac_tx_clk ]
  set_property -dict [ list CONFIG.ASSOCIATED_BUSIF {MRMAC_TX_AXIS} CONFIG.ASSOCIATED_RESET {cmac_tx_resetn} ] $cmac_tx_clk
  set cmac_tx_resetn [ create_bd_port -dir I -type rst cmac_tx_resetn ]
  set_property CONFIG.POLARITY {ACTIVE_LOW} $cmac_tx_resetn

  # GT control
  create_bd_port -dir O -from 7 -to 0 gt_line_rate
  create_bd_port -dir O -from 3 -to 0 gt_reset_all_in
  create_bd_port -dir O -from 2 -to 0 -type data gt_loopback
  create_bd_port -dir O -from 3 -to 0 -type data gt_reset_rx_datapath_in
  create_bd_port -dir O -from 3 -to 0 -type data gt_reset_tx_datapath_in
  create_bd_port -dir O -from 0 -to 0 -type data gt_rxcdrhold
  create_bd_port -dir O -from 5 -to 0 -type data gt_txprecursor
  create_bd_port -dir O -from 5 -to 0 -type data gt_txpostcursor
  create_bd_port -dir O -from 6 -to 0 -type data gt_txmaincursor
  create_bd_port -dir I -from 3 -to 0 -type data stat_mst_reset_done

  # BALBOA reset / status through axi_gpio_ctl
  create_bd_port -dir O -from 31 -to 0 net_ctl
  create_bd_port -dir I -from 31 -to 0 ctl_status

  # transport_demo control, driven by hwhs registers
  create_bd_port -dir I start_demo
  create_bd_port -dir I reset_demo
  create_bd_port -dir I -from 31 -to 0 syn_sum
  create_bd_port -dir I -from 63 -to 0 freq_num
  create_bd_port -dir I -from 31 -to 0 syn_depth
  create_bd_port -dir I -from 31 -to 0 reply_seq_off
  create_bd_port -dir I -from 31 -to 0 freq_span_mask
  create_bd_port -dir I -from 31 -to 0 lfsr_seed
  create_bd_port -dir O cmp_error
  create_bd_port -dir O cmp_valid
  create_bd_port -dir O demo_done

  # ==================================================================================
  # PS and DDR (AMD)
  # ==================================================================================
  create_hier_cell_cips_block [current_bd_instance .] cips_block

  # M_AXI_LPD carries the PL BRAM window at 0x8000_0000..0x9FFF_FFFF
  current_bd_instance cips_block
  create_bd_intf_pin -mode Master -vlnv xilinx.com:interface:aximm_rtl:1.0 M_AXI_LPD
  connect_bd_intf_net [get_bd_intf_pins M_AXI_LPD] [get_bd_intf_pins versal_cips_0/M_AXI_LPD]
  connect_bd_net [get_bd_pins versal_cips_0/pl0_ref_clk] [get_bd_pins versal_cips_0/m_axi_lpd_aclk]
  current_bd_instance /

  eval [amd_noc_script]

  # ==================================================================================
  # The net clock: BALBOA (its HLS cores are synthesized for it), hwhs, transport_demo
  # ==================================================================================
  set clk_wizard_net [ create_bd_cell -type ip -vlnv xilinx.com:ip:clk_wizard:1.0 clk_wizard_net ]
  set_property -dict [list \
    CONFIG.CLKOUT_DRIVES {BUFG,BUFG,BUFG,BUFG,BUFG,BUFG,BUFG} \
    CONFIG.CLKOUT_PORT {net_clk,clk_out2,clk_out3,clk_out4,clk_out5,clk_out6,clk_out7} \
    CONFIG.CLKOUT_REQUESTED_OUT_FREQUENCY [format "%.3f,100.000,100.000,100.000,100.000,100.000,100.000" $net_mhz] \
    CONFIG.CLKOUT_USED {true,false,false,false,false,false,false} \
    CONFIG.PRIM_SOURCE {Global_buffer} \
    CONFIG.RESET_TYPE {ACTIVE_HIGH} \
    CONFIG.USE_LOCKED {true} \
    CONFIG.USE_RESET {true} \
  ] $clk_wizard_net

  create_bd_cell -type ip -vlnv xilinx.com:ip:proc_sys_reset:5.0 proc_sys_reset_net

  # ==================================================================================
  # MRMAC <-> BALBOA
  # ==================================================================================
  set axis_fifo_rx_mac [ create_bd_cell -type ip -vlnv xilinx.com:ip:axis_data_fifo:2.0 axis_fifo_rx_mac ]
  set_property -dict [list \
    CONFIG.FIFO_DEPTH {256} \
  ] $axis_fifo_rx_mac

  set axis_dwidth_rx [ create_bd_cell -type ip -vlnv xilinx.com:ip:axis_dwidth_converter:1.1 axis_dwidth_rx ]
  set_property -dict [list \
    CONFIG.HAS_MI_TKEEP {1} \
    CONFIG.HAS_TKEEP {1} \
    CONFIG.HAS_TLAST {1} \
    CONFIG.M_TDATA_NUM_BYTES {64} \
    CONFIG.S_TDATA_NUM_BYTES {48} \
  ] $axis_dwidth_rx

  set axis_fifo_rx_cdc [ create_bd_cell -type ip -vlnv xilinx.com:ip:axis_data_fifo:2.0 axis_fifo_rx_cdc ]
  set_property -dict [list \
    CONFIG.FIFO_DEPTH {2048} \
    CONFIG.IS_ACLK_ASYNC {1} \
    CONFIG.HAS_TKEEP {1} \
    CONFIG.HAS_TLAST {1} \
    CONFIG.TDATA_NUM_BYTES {64} \
  ] $axis_fifo_rx_cdc

  set axis_fifo_tx_cdc [ create_bd_cell -type ip -vlnv xilinx.com:ip:axis_data_fifo:2.0 axis_fifo_tx_cdc ]
  set_property -dict [list \
    CONFIG.FIFO_DEPTH {512} \
    CONFIG.IS_ACLK_ASYNC {1} \
    CONFIG.HAS_TKEEP {1} \
    CONFIG.HAS_TLAST {1} \
    CONFIG.TDATA_NUM_BYTES {64} \
  ] $axis_fifo_tx_cdc

  set axis_dwidth_tx [ create_bd_cell -type ip -vlnv xilinx.com:ip:axis_dwidth_converter:1.1 axis_dwidth_tx ]
  set_property -dict [list \
    CONFIG.HAS_MI_TKEEP {1} \
    CONFIG.HAS_TKEEP {1} \
    CONFIG.HAS_TLAST {1} \
    CONFIG.M_TDATA_NUM_BYTES {48} \
    CONFIG.S_TDATA_NUM_BYTES {64} \
  ] $axis_dwidth_tx

  # MRMAC TX must not underrun inside a frame
  set axis_fifo_tx_pkt [ create_bd_cell -type ip -vlnv xilinx.com:ip:axis_data_fifo:2.0 axis_fifo_tx_pkt ]
  set_property CONFIG.FIFO_MODE {2} $axis_fifo_tx_pkt

  # ==================================================================================
  # PS -> hwhs / BRAM / GPIO
  # ==================================================================================
  set smartconnect_hwhs [ create_bd_cell -type ip -vlnv xilinx.com:ip:smartconnect:1.0 smartconnect_hwhs ]
  set_property -dict [list \
    CONFIG.NUM_CLKS {2} \
    CONFIG.NUM_MI {1} \
    CONFIG.NUM_SI {1} \
  ] $smartconnect_hwhs

  # S00: M_AXI_LPD, S01: transport_demo m_axi_0. Each BRAM controller has one PS address
  set smartconnect_lpd [ create_bd_cell -type ip -vlnv xilinx.com:ip:smartconnect:1.0 smartconnect_lpd ]
  set_property -dict [list \
    CONFIG.NUM_CLKS {2} \
    CONFIG.NUM_MI {4} \
    CONFIG.NUM_SI {2} \
  ] $smartconnect_lpd

  foreach n {axi_bram_ctrl_slot axi_bram_ctrl_reply} {
    set c [ create_bd_cell -type ip -vlnv xilinx.com:ip:axi_bram_ctrl:4.1 $n ]
    set_property -dict [list \
      CONFIG.DATA_WIDTH {512} \
      CONFIG.SINGLE_PORT_BRAM {1} \
      CONFIG.READ_LATENCY {2} \
    ] $c
  }

  # RTT trace read-back: 32-bit, two 16-bit entries per word
  set axi_bram_ctrl_trace [ create_bd_cell -type ip -vlnv xilinx.com:ip:axi_bram_ctrl:4.1 axi_bram_ctrl_trace ]
  set_property -dict [list \
    CONFIG.DATA_WIDTH {32} \
    CONFIG.SINGLE_PORT_BRAM {1} \
    CONFIG.READ_LATENCY {8} \
  ] $axi_bram_ctrl_trace

  # transport_demo table: port A read by transport_demo m_axi_1, port B by the PS and m_axi_0
  foreach n {axi_bram_ctrl_demo_a axi_bram_ctrl_demo_b} {
    set c [ create_bd_cell -type ip -vlnv xilinx.com:ip:axi_bram_ctrl:4.1 $n ]
    set_property -dict [list \
      CONFIG.DATA_WIDTH {512} \
      CONFIG.SINGLE_PORT_BRAM {1} \
      CONFIG.READ_LATENCY {4} \
    ] $c
  }
  set emb_mem_gen_demo [ create_bd_cell -type ip -vlnv xilinx.com:ip:emb_mem_gen:1.0 emb_mem_gen_demo ]
  set_property -dict [list \
    CONFIG.ADDR_WIDTH_A {20} \
    CONFIG.ADDR_WIDTH_B {20} \
    CONFIG.MEMORY_TYPE {True_Dual_Port_RAM} \
    CONFIG.MEMORY_PRIMITIVE {URAM} \
    CONFIG.READ_LATENCY_A {4} \
    CONFIG.READ_LATENCY_B {4} \
  ] $emb_mem_gen_demo

  set transport_demo_0 [ create_bd_cell -type module -reference transport_demo transport_demo_0 ]

  set axi_gpio_id [ create_bd_cell -type ip -vlnv xilinx.com:ip:axi_gpio:2.0 axi_gpio_id ]
  set_property -dict [list \
    CONFIG.C_ALL_INPUTS {1} \
    CONFIG.C_ALL_INPUTS_2 {1} \
    CONFIG.C_GPIO_WIDTH {32} \
    CONFIG.C_GPIO2_WIDTH {32} \
    CONFIG.C_IS_DUAL {1} \
  ] $axi_gpio_id

  set axi_gpio_ctl [ create_bd_cell -type ip -vlnv xilinx.com:ip:axi_gpio:2.0 axi_gpio_ctl ]
  set_property -dict [list \
    CONFIG.C_ALL_OUTPUTS {1} \
    CONFIG.C_DOUT_DEFAULT {0x00000000} \
    CONFIG.C_GPIO_WIDTH {32} \
    CONFIG.C_ALL_INPUTS_2 {1} \
    CONFIG.C_GPIO2_WIDTH {32} \
    CONFIG.C_IS_DUAL {1} \
  ] $axi_gpio_ctl

  set xlconstant_build_id [ create_bd_cell -type ip -vlnv xilinx.com:ip:xlconstant:1.1 xlconstant_build_id ]
  set_property -dict [list CONFIG.CONST_VAL $build_id CONFIG.CONST_WIDTH {32}] $xlconstant_build_id
  set xlconstant_net_hz [ create_bd_cell -type ip -vlnv xilinx.com:ip:xlconstant:1.1 xlconstant_net_hz ]
  set_property -dict [list CONFIG.CONST_VAL $net_hz CONFIG.CONST_WIDTH {32}] $xlconstant_net_hz
  set xlconstant_one [ create_bd_cell -type ip -vlnv xilinx.com:ip:xlconstant:1.1 xlconstant_one ]
  set xlconstant_zero [ create_bd_cell -type ip -vlnv xilinx.com:ip:xlconstant:1.1 xlconstant_zero ]
  set_property CONFIG.CONST_VAL {0} $xlconstant_zero

  # ==================================================================================
  # Interface connections
  # ==================================================================================
  # NoC (PS ports only)
  connect_bd_intf_net [get_bd_intf_ports ch0_lpddr4_trip1] [get_bd_intf_pins axi_noc_0/CH0_LPDDR4_0]
  connect_bd_intf_net [get_bd_intf_ports ch1_lpddr4_trip1] [get_bd_intf_pins axi_noc_0/CH1_LPDDR4_0]
  connect_bd_intf_net [get_bd_intf_ports ch0_lpddr4_trip3] [get_bd_intf_pins axi_noc_1/CH0_LPDDR4_0]
  connect_bd_intf_net [get_bd_intf_ports ch1_lpddr4_trip3] [get_bd_intf_pins axi_noc_1/CH1_LPDDR4_0]
  connect_bd_intf_net [get_bd_intf_ports ch0_lpddr4_trip2] [get_bd_intf_pins axi_noc_2/CH0_LPDDR4_0]
  connect_bd_intf_net [get_bd_intf_ports ch1_lpddr4_trip2] [get_bd_intf_pins axi_noc_2/CH1_LPDDR4_0]
  connect_bd_intf_net [get_bd_intf_ports lpddr4_clk1] [get_bd_intf_pins axi_noc_0/sys_clk0]
  connect_bd_intf_net [get_bd_intf_ports lpddr4_clk2] [get_bd_intf_pins axi_noc_2/sys_clk0]
  connect_bd_intf_net [get_bd_intf_ports lpddr4_clk3] [get_bd_intf_pins axi_noc_1/sys_clk0]
  connect_bd_intf_net [get_bd_intf_pins axi_noc_0/M00_INI] [get_bd_intf_pins axi_noc_2/S00_INI]
  connect_bd_intf_net [get_bd_intf_pins axi_noc_0/M01_INI] [get_bd_intf_pins axi_noc_2/S01_INI]
  connect_bd_intf_net [get_bd_intf_pins axi_noc_0/M02_INI] [get_bd_intf_pins axi_noc_2/S02_INI]
  connect_bd_intf_net [get_bd_intf_pins axi_noc_0/M03_INI] [get_bd_intf_pins axi_noc_2/S03_INI]
  connect_bd_intf_net [get_bd_intf_pins axi_noc_0/M04_INI] [get_bd_intf_pins axi_noc_1/S00_INI]
  connect_bd_intf_net [get_bd_intf_pins axi_noc_0/M05_INI] [get_bd_intf_pins axi_noc_1/S01_INI]
  connect_bd_intf_net [get_bd_intf_pins axi_noc_0/M06_INI] [get_bd_intf_pins axi_noc_1/S02_INI]
  connect_bd_intf_net [get_bd_intf_pins axi_noc_0/M07_INI] [get_bd_intf_pins axi_noc_1/S03_INI]
  connect_bd_intf_net [get_bd_intf_pins axi_noc_0/S00_AXI] [get_bd_intf_pins cips_block/PMC_NOC_AXI_0]
  connect_bd_intf_net [get_bd_intf_pins axi_noc_0/S01_AXI] [get_bd_intf_pins cips_block/NOC_LPD_AXI_0]
  connect_bd_intf_net [get_bd_intf_pins axi_noc_0/S02_AXI] [get_bd_intf_pins cips_block/FPD_CCI_NOC_0]
  connect_bd_intf_net [get_bd_intf_pins axi_noc_0/S03_AXI] [get_bd_intf_pins cips_block/FPD_CCI_NOC_1]
  connect_bd_intf_net [get_bd_intf_pins axi_noc_0/S04_AXI] [get_bd_intf_pins cips_block/FPD_CCI_NOC_2]
  connect_bd_intf_net [get_bd_intf_pins axi_noc_0/S05_AXI] [get_bd_intf_pins cips_block/FPD_CCI_NOC_3]

  # FPD masters of cips_block
  connect_bd_intf_net [get_bd_intf_ports MRMAC_AXI] [get_bd_intf_pins cips_block/M00_AXI_0]
  connect_bd_intf_net [get_bd_intf_pins cips_block/M00_AXI_1] [get_bd_intf_pins axi_gpio_id/S_AXI]
  connect_bd_intf_net [get_bd_intf_pins cips_block/M00_AXI_2] [get_bd_intf_pins axi_gpio_ctl/S_AXI]
  connect_bd_intf_net [get_bd_intf_pins cips_block/M00_AXI_3] [get_bd_intf_pins smartconnect_hwhs/S00_AXI]
  connect_bd_intf_net [get_bd_intf_pins smartconnect_hwhs/M00_AXI] [get_bd_intf_ports HWHS_AXI]

  # BRAM windows
  connect_bd_intf_net [get_bd_intf_pins cips_block/M_AXI_LPD] [get_bd_intf_pins smartconnect_lpd/S00_AXI]
  connect_bd_intf_net [get_bd_intf_pins transport_demo_0/m_axi_0] [get_bd_intf_pins smartconnect_lpd/S01_AXI]
  connect_bd_intf_net [get_bd_intf_pins smartconnect_lpd/M00_AXI] [get_bd_intf_pins axi_bram_ctrl_slot/S_AXI]
  connect_bd_intf_net [get_bd_intf_pins smartconnect_lpd/M01_AXI] [get_bd_intf_pins axi_bram_ctrl_reply/S_AXI]
  connect_bd_intf_net [get_bd_intf_pins smartconnect_lpd/M02_AXI] [get_bd_intf_pins axi_bram_ctrl_trace/S_AXI]
  connect_bd_intf_net [get_bd_intf_pins smartconnect_lpd/M03_AXI] [get_bd_intf_pins axi_bram_ctrl_demo_b/S_AXI]
  connect_bd_intf_net [get_bd_intf_pins transport_demo_0/m_axi_1] [get_bd_intf_pins axi_bram_ctrl_demo_a/S_AXI]
  connect_bd_intf_net [get_bd_intf_pins axi_bram_ctrl_slot/BRAM_PORTA] [get_bd_intf_ports SLOT_BRAM]
  connect_bd_intf_net [get_bd_intf_pins axi_bram_ctrl_reply/BRAM_PORTA] [get_bd_intf_ports REPLY_BRAM]
  connect_bd_intf_net [get_bd_intf_pins axi_bram_ctrl_trace/BRAM_PORTA] [get_bd_intf_ports TRACE_BRAM]
  connect_bd_intf_net [get_bd_intf_pins axi_bram_ctrl_demo_a/BRAM_PORTA] [get_bd_intf_pins emb_mem_gen_demo/BRAM_PORTA]
  connect_bd_intf_net [get_bd_intf_pins axi_bram_ctrl_demo_b/BRAM_PORTA] [get_bd_intf_pins emb_mem_gen_demo/BRAM_PORTB]

  # transport_demo <-> hwhs
  connect_bd_intf_net [get_bd_intf_pins transport_demo_0/m_axis] [get_bd_intf_ports DEMO_M_AXIS]
  connect_bd_intf_net [get_bd_intf_ports DEMO_S_AXIS] [get_bd_intf_pins transport_demo_0/s_axis]

  # MRMAC <-> BALBOA
  connect_bd_intf_net [get_bd_intf_ports MRMAC_RX_AXIS] [get_bd_intf_pins axis_fifo_rx_mac/S_AXIS]
  connect_bd_intf_net [get_bd_intf_pins axis_fifo_rx_mac/M_AXIS] [get_bd_intf_pins axis_dwidth_rx/S_AXIS]
  connect_bd_intf_net [get_bd_intf_pins axis_dwidth_rx/M_AXIS] [get_bd_intf_pins axis_fifo_rx_cdc/S_AXIS]
  connect_bd_intf_net [get_bd_intf_pins axis_fifo_rx_cdc/M_AXIS] [get_bd_intf_ports NET_RX_AXIS]
  connect_bd_intf_net [get_bd_intf_ports NET_TX_AXIS] [get_bd_intf_pins axis_fifo_tx_cdc/S_AXIS]
  connect_bd_intf_net [get_bd_intf_pins axis_fifo_tx_cdc/M_AXIS] [get_bd_intf_pins axis_dwidth_tx/S_AXIS]
  connect_bd_intf_net [get_bd_intf_pins axis_dwidth_tx/M_AXIS] [get_bd_intf_pins axis_fifo_tx_pkt/S_AXIS]
  connect_bd_intf_net [get_bd_intf_pins axis_fifo_tx_pkt/M_AXIS] [get_bd_intf_ports MRMAC_TX_AXIS]

  # ==================================================================================
  # Port connections
  # ==================================================================================
  # NoC clocks
  connect_bd_net [get_bd_pins cips_block/pmc_axi_noc_axi0_clk] [get_bd_pins axi_noc_0/aclk0]
  connect_bd_net [get_bd_pins cips_block/lpd_axi_noc_clk] [get_bd_pins axi_noc_0/aclk1]
  connect_bd_net [get_bd_pins cips_block/fpd_cci_noc_axi0_clk] [get_bd_pins axi_noc_0/aclk2]
  connect_bd_net [get_bd_pins cips_block/fpd_cci_noc_axi1_clk] [get_bd_pins axi_noc_0/aclk3]
  connect_bd_net [get_bd_pins cips_block/fpd_cci_noc_axi2_clk] [get_bd_pins axi_noc_0/aclk4]
  connect_bd_net [get_bd_pins cips_block/fpd_cci_noc_axi3_clk] [get_bd_pins axi_noc_0/aclk5]

  # GT control
  connect_bd_net [get_bd_pins cips_block/gt_line_rate] [get_bd_ports gt_line_rate]
  connect_bd_net [get_bd_pins cips_block/gt_reset_all_in] [get_bd_ports gt_reset_all_in]
  connect_bd_net [get_bd_pins cips_block/gt_loopback] [get_bd_ports gt_loopback]
  connect_bd_net [get_bd_pins cips_block/gt_reset_rx_datapath_in] [get_bd_ports gt_reset_rx_datapath_in]
  connect_bd_net [get_bd_pins cips_block/gt_reset_tx_datapath_in] [get_bd_ports gt_reset_tx_datapath_in]
  connect_bd_net [get_bd_pins cips_block/gt_rxcdrhold] [get_bd_ports gt_rxcdrhold]
  connect_bd_net [get_bd_pins cips_block/gt_txprecursor] [get_bd_ports gt_txprecursor]
  connect_bd_net [get_bd_pins cips_block/gt_txpostcursor] [get_bd_ports gt_txpostcursor]
  connect_bd_net [get_bd_pins cips_block/gt_txmaincursor] [get_bd_ports gt_txmaincursor]
  connect_bd_net [get_bd_ports stat_mst_reset_done] [get_bd_pins cips_block/stat_mst_reset_done]

  # no PL interrupts yet
  connect_bd_net [get_bd_pins xlconstant_zero/dout] [get_bd_pins cips_block/pl_ps_irq0] [get_bd_pins cips_block/pl_ps_irq1] [get_bd_pins cips_block/pl_ps_irq2] [get_bd_pins cips_block/pl_ps_irq3]

  # pl0 domain
  connect_bd_net [get_bd_pins cips_block/pl0_ref_clk_0] [get_bd_ports pl_clk] [get_bd_pins clk_wizard_net/clk_in1] [get_bd_pins axi_gpio_id/s_axi_aclk] [get_bd_pins axi_gpio_ctl/s_axi_aclk] [get_bd_pins smartconnect_hwhs/aclk] [get_bd_pins smartconnect_lpd/aclk]
  connect_bd_net [get_bd_pins cips_block/pl0_resetn_0] [get_bd_ports pl_resetn] [get_bd_pins axi_gpio_id/s_axi_aresetn] [get_bd_pins axi_gpio_ctl/s_axi_aresetn] [get_bd_pins proc_sys_reset_net/ext_reset_in]
  connect_bd_net [get_bd_pins cips_block/interconnect_aresetn] [get_bd_pins smartconnect_hwhs/aresetn] [get_bd_pins smartconnect_lpd/aresetn]
  connect_bd_net [get_bd_pins cips_block/peripheral_reset] [get_bd_pins clk_wizard_net/reset]

  # net clock domain
  connect_bd_net [get_bd_pins clk_wizard_net/net_clk] [get_bd_ports net_clk] [get_bd_pins proc_sys_reset_net/slowest_sync_clk] [get_bd_pins smartconnect_hwhs/aclk1] [get_bd_pins smartconnect_lpd/aclk1] [get_bd_pins axi_bram_ctrl_slot/s_axi_aclk] [get_bd_pins axi_bram_ctrl_reply/s_axi_aclk] [get_bd_pins axi_bram_ctrl_trace/s_axi_aclk] [get_bd_pins axi_bram_ctrl_demo_a/s_axi_aclk] [get_bd_pins axi_bram_ctrl_demo_b/s_axi_aclk] [get_bd_pins transport_demo_0/clk] [get_bd_pins axis_fifo_rx_cdc/m_axis_aclk] [get_bd_pins axis_fifo_tx_cdc/s_axis_aclk]
  connect_bd_net [get_bd_pins proc_sys_reset_net/peripheral_aresetn] [get_bd_ports net_resetn] [get_bd_pins axi_bram_ctrl_slot/s_axi_aresetn] [get_bd_pins axi_bram_ctrl_reply/s_axi_aresetn] [get_bd_pins axi_bram_ctrl_trace/s_axi_aresetn] [get_bd_pins axi_bram_ctrl_demo_a/s_axi_aresetn] [get_bd_pins axi_bram_ctrl_demo_b/s_axi_aresetn] [get_bd_pins transport_demo_0/resetn] [get_bd_pins axis_fifo_tx_cdc/s_axis_aresetn]
  connect_bd_net [get_bd_pins clk_wizard_net/locked] [get_bd_ports clk_locked] [get_bd_pins proc_sys_reset_net/dcm_locked]

  # MRMAC clocks
  connect_bd_net [get_bd_ports cmac_rx_clk] [get_bd_pins axis_fifo_rx_mac/s_axis_aclk] [get_bd_pins axis_dwidth_rx/aclk] [get_bd_pins axis_fifo_rx_cdc/s_axis_aclk]
  connect_bd_net [get_bd_ports cmac_rx_resetn] [get_bd_pins axis_fifo_rx_mac/s_axis_aresetn] [get_bd_pins axis_dwidth_rx/aresetn] [get_bd_pins axis_fifo_rx_cdc/s_axis_aresetn]
  connect_bd_net [get_bd_ports cmac_tx_clk] [get_bd_pins axis_fifo_tx_cdc/m_axis_aclk] [get_bd_pins axis_dwidth_tx/aclk] [get_bd_pins axis_fifo_tx_pkt/s_axis_aclk]
  connect_bd_net [get_bd_ports cmac_tx_resetn] [get_bd_pins axis_dwidth_tx/aresetn] [get_bd_pins axis_fifo_tx_pkt/s_axis_aresetn]

  # GPIOs
  connect_bd_net [get_bd_pins xlconstant_build_id/dout] [get_bd_pins axi_gpio_id/gpio_io_i]
  connect_bd_net [get_bd_pins xlconstant_net_hz/dout] [get_bd_pins axi_gpio_id/gpio2_io_i]
  connect_bd_net [get_bd_pins axi_gpio_ctl/gpio_io_o] [get_bd_ports net_ctl]
  connect_bd_net [get_bd_ports ctl_status] [get_bd_pins axi_gpio_ctl/gpio2_io_i]

  # demo table output-register enables
  connect_bd_net [get_bd_pins xlconstant_one/dout] [get_bd_pins emb_mem_gen_demo/regcea] [get_bd_pins emb_mem_gen_demo/regceb]

  # transport_demo control
  connect_bd_net [get_bd_ports start_demo] [get_bd_pins transport_demo_0/start_demo]
  connect_bd_net [get_bd_ports reset_demo] [get_bd_pins transport_demo_0/reset_demo]
  connect_bd_net [get_bd_ports syn_sum] [get_bd_pins transport_demo_0/syn_sum]
  connect_bd_net [get_bd_ports freq_num] [get_bd_pins transport_demo_0/freq_num]
  connect_bd_net [get_bd_ports syn_depth] [get_bd_pins transport_demo_0/syn_depth]
  connect_bd_net [get_bd_ports reply_seq_off] [get_bd_pins transport_demo_0/reply_seq_off]
  connect_bd_net [get_bd_ports freq_span_mask] [get_bd_pins transport_demo_0/freq_span_mask]
  connect_bd_net [get_bd_ports lfsr_seed] [get_bd_pins transport_demo_0/lfsr_seed]
  connect_bd_net [get_bd_pins transport_demo_0/cmp_error] [get_bd_ports cmp_error]
  connect_bd_net [get_bd_pins transport_demo_0/cmp_valid] [get_bd_ports cmp_valid]
  connect_bd_net [get_bd_pins transport_demo_0/demo_done] [get_bd_ports demo_done]

  # ==================================================================================
  # Address segments
  # ==================================================================================
  set fpd [get_bd_addr_spaces cips_block/versal_cips_0/M_AXI_FPD]
  set lpd [get_bd_addr_spaces cips_block/versal_cips_0/M_AXI_LPD]

  assign_bd_address -offset 0xA4000000 -range 0x00010000 -target_address_space $fpd [get_bd_addr_segs HWHS_AXI/Reg] -force
  assign_bd_address -offset 0xA4040000 -range 0x00010000 -target_address_space $fpd [get_bd_addr_segs MRMAC_AXI/Reg] -force
  assign_bd_address -offset 0xA4050000 -range 0x00010000 -target_address_space $fpd [get_bd_addr_segs axi_gpio_id/S_AXI/Reg] -force
  assign_bd_address -offset 0xA4060000 -range 0x00010000 -target_address_space $fpd [get_bd_addr_segs cips_block/axi_gpio_gt_reset_mask/S_AXI/Reg] -force
  assign_bd_address -offset 0xA4070000 -range 0x00010000 -target_address_space $fpd [get_bd_addr_segs cips_block/gtctrl/S_AXI/Reg] -force
  assign_bd_address -offset 0xA4080000 -range 0x00010000 -target_address_space $fpd [get_bd_addr_segs cips_block/axi_gpio_debug/S_AXI/Reg] -force
  assign_bd_address -offset 0xA4090000 -range 0x00010000 -target_address_space $fpd [get_bd_addr_segs axi_gpio_ctl/S_AXI/Reg] -force

  assign_bd_address -offset 0x80000000 -range 0x00100000 -target_address_space $lpd [get_bd_addr_segs axi_bram_ctrl_demo_b/S_AXI/Mem0] -force
  assign_bd_address -offset 0x91000000 -range 0x00010000 -target_address_space $lpd [get_bd_addr_segs axi_bram_ctrl_slot/S_AXI/Mem0] -force
  assign_bd_address -offset 0x92000000 -range 0x00200000 -target_address_space $lpd [get_bd_addr_segs axi_bram_ctrl_trace/S_AXI/Mem0] -force
  assign_bd_address -offset 0x93000000 -range 0x00010000 -target_address_space $lpd [get_bd_addr_segs axi_bram_ctrl_reply/S_AXI/Mem0] -force

  set demo0 [get_bd_addr_spaces transport_demo_0/m_axi_0]
  assign_bd_address -offset 0x80000000 -range 0x00100000 -target_address_space $demo0 [get_bd_addr_segs axi_bram_ctrl_demo_b/S_AXI/Mem0] -force
  exclude_bd_addr_seg -target_address_space $demo0 [get_bd_addr_segs axi_bram_ctrl_slot/S_AXI/Mem0]
  exclude_bd_addr_seg -target_address_space $demo0 [get_bd_addr_segs axi_bram_ctrl_reply/S_AXI/Mem0]
  exclude_bd_addr_seg -target_address_space $demo0 [get_bd_addr_segs axi_bram_ctrl_trace/S_AXI/Mem0]
  assign_bd_address -offset 0x80000000 -range 0x00100000 -target_address_space [get_bd_addr_spaces transport_demo_0/m_axi_1] [get_bd_addr_segs axi_bram_ctrl_demo_a/S_AXI/Mem0] -force

  # PS -> DDR
  eval [amd_ps_ddr_addr_script]

  validate_bd_design
  save_bd_design
  close_bd_design $design_name
}
