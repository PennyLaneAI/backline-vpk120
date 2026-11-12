# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Xanadu Quantum Technologies Inc.
#
# create_project.tcl - create the backline-roce Vivado project (Vivado 2024.2).
#
#   vivado -mode batch -source hw/scripts/create_project.tcl -tclargs \
#       -amd build/amd/hw -hls build/hls/iprepo -out build/vivado [-net_mhz 250]
#
#   -amd      the "hw" directory of AMD's VPK120 reference design
#             (scripts/unpack-amd.sh puts it in build/amd/hw)
#   -hls      BALBOA's HLS cores as IP (hw/scripts/build_hls.sh puts them in build/hls/iprepo),
#             synthesized for the same net clock
#   -out      where the project goes (build/vivado/backline_roce/backline_roce.xpr)
#   -net_mhz  the net clock: BALBOA, hwhs, transport_demo

set repo    [file normalize [file join [file dirname [info script]] .. ..]]
set amd_hw  [file join $repo build amd hw]
set hls_ip  [file join $repo build hls iprepo]
set out_dir [file join $repo build vivado]
set net_mhz 250.0

for {set i 0} {$i < $::argc} {incr i} {
    set opt [lindex $::argv $i]
    switch -exact -- $opt {
        "-amd"     { incr i; set amd_hw  [file normalize [lindex $::argv $i]] }
        "-hls"     { incr i; set hls_ip  [file normalize [lindex $::argv $i]] }
        "-out"     { incr i; set out_dir [file normalize [lindex $::argv $i]] }
        "-net_mhz" { incr i; set net_mhz [lindex $::argv $i] }
        default    { error "create_project.tcl: unknown option $opt" }
    }
}

set proj_name backline_roce
set part      xcvp1202-vsva2785-2MP-e-S
set net_hz    [expr {round($net_mhz * 1000000.0)}]
set build_id  [scan [clock format [clock seconds] -format %y%m%d%H] %x]

foreach f [list vpk120m_ernic_ref_design.tcl hdl/mrmac_0_exdes_ernic.sv hdl/mrmac_0_axis_stream_mux.sv \
               clk_wizard_0.xci default.xdc v350_ddr4_single_lp0.xdc] {
    if {![file exists [file join $amd_hw $f]]} {
        error "missing AMD reference design file $amd_hw/$f (run scripts/unpack-amd.sh)"
    }
}
foreach core {rocev2 ip_handler mac_ip_encode arp_server_subnet} {
    set stamp [file join $hls_ip $core net_mhz]
    if {![file exists [file join $hls_ip $core component.xml]] || ![file exists $stamp]} {
        error "missing BALBOA HLS core $hls_ip/$core (run hw/scripts/build_hls.sh -net_mhz $net_mhz)"
    }
    set fh [open $stamp r]
    set core_mhz [string trim [read $fh]]
    close $fh
    if {$core_mhz != $net_mhz} {
        error "BALBOA HLS core $core was synthesized for $core_mhz MHz, not $net_mhz (run hw/scripts/build_hls.sh -net_mhz $net_mhz)"
    }
}

puts "INFO: repo=$repo amd=$amd_hw hls=$hls_ip out=$out_dir net=${net_mhz} MHz build_id=[format 0x%08x $build_id]"

#################################################
# Project
#################################################
create_project $proj_name [file join $out_dir $proj_name] -part $part -force
set obj [current_project]
set_property -name "board_part"                     -value "xilinx.com:vpk120:part0:1.2"  -objects $obj
set_property -name "default_lib"                    -value "xil_defaultlib"               -objects $obj
set_property -name "enable_vhdl_2008"               -value "1"                            -objects $obj
set_property -name "ip_cache_permissions"           -value "read write"                   -objects $obj
set_property -name "mem.enable_memory_map_generation" -value "1"                          -objects $obj
set_property -name "platform.board_id"              -value "vpk120"                       -objects $obj
set_property -name "revised_directory_structure"    -value "1"                            -objects $obj
set_property -name "simulator_language"             -value "Mixed"                        -objects $obj
set_property -name "xpm_libraries"                  -value "XPM_CDC XPM_FIFO XPM_MEMORY"  -objects $obj

set src [get_filesets sources_1]

# BALBOA's HLS cores
set_property ip_repo_paths [list $hls_ip] [current_project]
update_ip_catalog

#################################################
# Sources
#################################################
# AMD MRMAC example design
add_files -norecurse -fileset $src [list \
    [file join $amd_hw hdl mrmac_0_exdes_ernic.sv] \
    [file join $amd_hw hdl mrmac_0_axis_stream_mux.sv] ]
set_property file_type SystemVerilog [get_files {mrmac_0_exdes_ernic.sv mrmac_0_axis_stream_mux.sv}]
import_files -fileset $src [list [file join $amd_hw clk_wizard_0.xci]]
set_property generate_files_for_reference 0 [get_files clk_wizard_0.xci]
set_property registered_with_manager 1 [get_files clk_wizard_0.xci]
set_property synth_checkpoint_mode Singular [get_files clk_wizard_0.xci]

# BALBOA: Coyote's RTL (vendored, unmodified) with this build's lynxTypes package, which
# carries the defines that select BALBOA's parts (EN_RDMA, EN_STATS, VITIS_HLS)
set coyote [file join $repo third_party balboa coyote hw hdl]
add_files -norecurse -fileset $src [concat \
    [list [file join $repo hw rtl balboa_cfg lynx_pkg.sv] \
          [file join $coyote pkg axi_intf.sv] [file join $coyote pkg lynx_intf.sv]] \
    [list [file join $coyote common regs axis_reg.sv] [file join $coyote common regs axis_reg_array.sv] \
          [file join $coyote common regs meta_reg.sv] [file join $coyote common regs meta_reg_array.sv] \
          [file join $coyote common queues fifo.sv] [file join $coyote common queues meta_queue.sv] \
          [file join $coyote common queues queue_meta.sv] [file join $coyote common queues queue_stream.sv] \
          [file join $coyote common ram sp_ram_nc.sv]] \
    [lsort [glob [file join $coyote network rdma *.sv]]] \
    [list [file join $coyote network stack network_stack.sv]] ]
add_files -norecurse -fileset $src [glob [file join $coyote pkg *.svh]]
set_property file_type {Verilog Header} [get_files -of_objects $src *.svh]
set_property include_dirs [list [file join $coyote pkg]] $src

# BALBOA's stand-ins (icmp_server_ip, vio_ip, the axis_interconnect_512 merges), hwhs,
# transport_demo, the top
add_files -norecurse -fileset $src [concat \
    [list [file join $repo hw rtl balboa_cfg icmp_server_ip.sv] [file join $repo hw rtl balboa_cfg vio_ip.sv] \
          [file join $repo hw rtl balboa_cfg axis_interconnect_512.sv]] \
    [lsort [glob [file join $repo hw rtl hwhs *.v]]] \
    [list [file join $repo hw rtl demo transport_demo.v] \
          [file join $repo hw rtl top eth_pad.v] \
          [file join $repo hw rtl top balboa_net.sv] \
          [file join $repo hw rtl top backline_roce_top.sv]] ]

# The lynxTypes defines are repeated here so that no file depends on compile order for them
set_property verilog_define [list NET_CLK_FREQ=$net_hz BACKLINE_BUILD_ID=$build_id EN_RDMA EN_STATS VITIS_HLS] $src

# The IP cores the vendored RTL instantiates by name
source [file join $repo hw scripts balboa_ips.tcl]
balboa_create_ips

#################################################
# Block designs
#################################################
source [file join $repo hw bd amd_procs.tcl]
amd_open [file join $amd_hw vpk120m_ernic_ref_design.tcl]
amd_load_proc cr_bd_mrmac_0_core_support
# M_AXI_LPD on: the BRAM windows at 0x8000_0000 (platform.tcl adds the pin and clock)
amd_load_proc create_hier_cell_cips_block [list \
    "      PS_USE_M_AXI_FPD {1} \\\n" \
    "      PS_USE_M_AXI_FPD {1} \\\n      PS_USE_M_AXI_LPD {1} \\\n" ]
source [file join $repo hw bd platform.tcl]

cr_bd_mrmac_0_core_support ""
cr_bd_platform $net_mhz $build_id

foreach bd {mrmac_0_core_support platform} {
    set f [get_files $bd.bd]
    set_property REGISTERED_WITH_MANAGER 1 $f
    set_property SYNTH_CHECKPOINT_MODE "Hierarchical" $f
    add_files -norecurse -fileset $src [make_wrapper -fileset $src -files $f -top]
}

set_property top backline_roce_top $src
set_property top_auto_set 0 $src
update_compile_order -fileset $src

#################################################
# Constraints
#################################################
set con [get_filesets constrs_1]
import_files -fileset $con [list [file join $amd_hw default.xdc] [file join $amd_hw v350_ddr4_single_lp0.xdc]]
set_property used_in_synthesis 0 [get_files v350_ddr4_single_lp0.xdc]
# No constraints of our own: every crossing between the net clock, pl0 and the MAC clocks is
# an xpm_cdc or an IP that constrains itself.

# Resource sharing off, as for the HLS cores (balboa_ips.tcl)
set_property STEPS.SYNTH_DESIGN.ARGS.RESOURCE_SHARING off [get_runs synth_1]

# Explore directives plus post-route phys_opt, as in the HSB build
set_property strategy Performance_ExplorePostRoutePhysOpt [get_runs impl_1]

puts "INFO: created [get_property DIRECTORY [current_project]]/$proj_name.xpr"
