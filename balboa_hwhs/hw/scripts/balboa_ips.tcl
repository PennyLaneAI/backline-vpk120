# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Xanadu Quantum Technologies Inc.
#
# balboa_ips.tcl - the IP cores BALBOA's vendored RTL instantiates by module name, created
# with the configurations Coyote gives them (its scripts/ip_inst/common_infrastructure.tcl
# and network_infrastructure.tcl at 15e5f59), and the four HLS cores hw/scripts/build_hls.sh
# exports. Sourced by create_project.tcl, which sets ip_repo_paths to the HLS IP first.
#
# Coyote's two AXI4-Stream Interconnect cores have no Versal version and are not created here:
# hw/rtl/balboa_cfg/axis_interconnect_512.sv takes their place.

proc balboa_create_ips {} {
    # Vivado IP, as Coyote configures it. axis_data_fifo_512: depth n_outs (8) x PMTU (4096) / 64.
    set ips {
        axis_data_fifo          2.0 axis_data_fifo_512_cc_tx    {CONFIG.TDATA_NUM_BYTES {64} CONFIG.IS_ACLK_ASYNC {0} CONFIG.HAS_TKEEP {1} CONFIG.HAS_TLAST {1}}
        axis_data_fifo          2.0 axis_data_fifo_512          {CONFIG.TDATA_NUM_BYTES {64} CONFIG.FIFO_DEPTH {512} CONFIG.HAS_TKEEP {1} CONFIG.HAS_TLAST {1}}
        axis_data_fifo          2.0 axis_data_fifo_meta_96      {CONFIG.TDATA_NUM_BYTES {12} CONFIG.FIFO_DEPTH {32}}
        axis_data_fifo          2.0 axis_data_fifo_meta_128     {CONFIG.TDATA_NUM_BYTES {16} CONFIG.FIFO_DEPTH {32}}
        axis_register_slice     1.1 axis_register_slice_512     {CONFIG.TDATA_NUM_BYTES {64} CONFIG.REG_CONFIG {8} CONFIG.HAS_TKEEP {1} CONFIG.HAS_TLAST {1}}
        axis_register_slice     1.1 axis_register_slice_meta_32 {CONFIG.TDATA_NUM_BYTES {4} CONFIG.REG_CONFIG {8}}
        axis_register_slice     1.1 axis_register_slice_meta_56 {CONFIG.TDATA_NUM_BYTES {7} CONFIG.REG_CONFIG {8}}
        axis_dwidth_converter   1.1 axis_512_to_64_converter    {CONFIG.S_TDATA_NUM_BYTES {64} CONFIG.M_TDATA_NUM_BYTES {8} CONFIG.HAS_TLAST {1} CONFIG.HAS_TKEEP {1} CONFIG.HAS_MI_TKEEP {1}}
        axis_dwidth_converter   1.1 axis_64_to_512_converter    {CONFIG.S_TDATA_NUM_BYTES {8} CONFIG.M_TDATA_NUM_BYTES {64} CONFIG.HAS_TLAST {1} CONFIG.HAS_TKEEP {1} CONFIG.HAS_MI_TKEEP {1} CONFIG.TDEST_WIDTH {1}}
    }
    foreach {name version module config} $ips {
        create_ip -name $name -vendor xilinx.com -library ip -version $version -module_name $module
        set_property -dict $config [get_ips $module]
    }

    # BALBOA's HLS cores (build_hls.sh), under the module names network_stack.sv and
    # roce_stack.sv instantiate. Their OOC runs synthesize with resource sharing off: Vivado
    # 2024.2 with it on breaks the IPv4 checksum adders (ip_handler drops every IPv4 packet,
    # mac_ip_encode sends a wrong IP checksum).
    set hls {
        rocev2            ethz.systems.fpga 0.82 rocev2_ip
        ip_handler        ethz.systems.fpga 2.0  ip_handler_ip
        mac_ip_encode     ethz.systems.fpga 2.0  mac_ip_encode_ip
        arp_server_subnet ethz.systems.fpga 1.1  arp_server_subnet_ip
    }
    foreach {name vendor version module} $hls {
        create_ip -name $name -vendor $vendor -library hls -version $version -module_name $module
        generate_target all [get_ips $module]
        create_ip_run [get_ips $module]
        set_property STEPS.SYNTH_DESIGN.ARGS.RESOURCE_SHARING off [get_runs ${module}_synth_1]
    }
}
