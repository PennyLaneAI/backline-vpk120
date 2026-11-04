## SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
## SPDX-License-Identifier: Apache-2.0
##
## Taken from NVIDIA's RFSoC 4x2 reference constraints
## (fpga/pynq/rfsoc-pynq/build/constraints/constraints.xdc @ EA_CUDAQ_10_26 a4b2059)

set dram_rams [get_cells -quiet -hierarchical -regexp {.*(u_ram_player|ipv4_buffer|fcs_buffer|hdr_buffer|u_axis_inp_buffer|u_tx_stream_buffer|ecb_rdwr_ctrl_inst|u_evt_int|u_ptp_ingress|i2c_ctrl_inst|spi_ctrl_inst|gen_data_gen_inst\[0\]|gen_data_gen_inst\[1\]|gen_pack_inst\[0\]|u_dp_pkt_ts|u_config_ram|u_rx_ls_parseri_1/u_axis_buffer|DISTRIBUTED_RAM\.lut_mem).*(blk_mem|lut_mem).* }]
set_property RAM_STYLE DISTRIBUTED $dram_rams
