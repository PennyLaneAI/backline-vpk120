# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Xanadu Quantum Technologies Inc.
#
# create_project.tcl - create the backline-hsb Vivado project (Vivado 2024.2).
#
#   vivado -mode batch -source hw/scripts/create_project.tcl -tclargs \
#       -amd build/amd/hw -out build/vivado [-hif_mhz 250] [-hsb build/hsb/nv_hsb_ip]
#
#   -amd      AMD's hw files, from scripts/unpack-amd.sh (build/amd/hw)
#   -out      where the project goes (build/vivado/backline_hsb/backline_hsb.xpr)
#   -hif_mhz  HSB host/sensor clock and hwhs clock
#   -hsb      the HSB IP to build (make hsb-src: third_party's copy with patches/ applied)

set repo    [file normalize [file join [file dirname [info script]] .. ..]]
set amd_hw  [file join $repo build amd hw]
set out_dir [file join $repo build vivado]
set hif_mhz 250.0
set hsb_dir [file join $repo third_party hsb nv_hsb_ip]

for {set i 0} {$i < $::argc} {incr i} {
    set opt [lindex $::argv $i]
    switch -exact -- $opt {
        "-amd"     { incr i; set amd_hw  [file normalize [lindex $::argv $i]] }
        "-out"     { incr i; set out_dir [file normalize [lindex $::argv $i]] }
        "-hif_mhz" { incr i; set hif_mhz [lindex $::argv $i] }
        "-hsb"     { incr i; set hsb_dir [file normalize [lindex $::argv $i]] }
        default    { error "create_project.tcl: unknown option $opt" }
    }
}

set proj_name backline_hsb
set part      xcvp1202-vsva2785-2MP-e-S
set hif_hz    [expr {round($hif_mhz * 1000000.0)}]
set build_id  [scan [clock format [clock seconds] -format %y%m%d%H] %x]

foreach f [list vpk120m_ernic_ref_design.tcl hdl/mrmac_0_exdes_ernic.sv hdl/mrmac_0_axis_stream_mux.sv \
               clk_wizard_0.xci default.xdc v350_ddr4_single_lp0.xdc] {
    if {![file exists [file join $amd_hw $f]]} {
        error "missing AMD reference design file $amd_hw/$f (run scripts/unpack-amd.sh)"
    }
}

puts "INFO: repo=$repo amd=$amd_hw out=$out_dir hif=${hif_mhz} MHz build_id=[format 0x%08x $build_id]"

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

# HSB
set hsb_rtl [lsort [glob [file join $hsb_dir * *.sv]]]
set hsb_pkg [lsearch -all -inline $hsb_rtl *pkg.sv]
set hsb_mod [lsearch -all -inline -not $hsb_rtl *pkg.sv]
add_files -norecurse -fileset $src [concat $hsb_pkg $hsb_mod]

# HSB build configuration and RTL
add_files -norecurse -fileset $src [file join $repo hw rtl hsb_cfg HOLOLINK_def.svh]
set_property file_type {Verilog Header} [get_files HOLOLINK_def.svh]
add_files -norecurse -fileset $src [concat \
    [lsort [glob [file join $repo hw rtl hwhs *.v]]] \
    [list [file join $repo hw rtl demo transport_demo.v] \
          [file join $repo hw rtl top backline_hsb_top.sv]] ]

set_property verilog_define [list HIF_CLK_FREQ=$hif_hz BACKLINE_BUILD_ID=$build_id] $src

#################################################
# Block designs
#################################################
source [file join $repo hw bd amd_procs.tcl]
amd_open [file join $amd_hw vpk120m_ernic_ref_design.tcl]
amd_load_proc cr_bd_mrmac_0_core_support
# Turn on M_AXI_LPD in AMD's CIPS config, for the BRAM windows at 0x8000_0000
amd_load_proc create_hier_cell_cips_block [list \
    "      PS_USE_M_AXI_FPD {1} \\\n" \
    "      PS_USE_M_AXI_FPD {1} \\\n      PS_USE_M_AXI_LPD {1} \\\n" ]
source [file join $repo hw bd platform.tcl]

cr_bd_mrmac_0_core_support ""
cr_bd_platform $hif_mhz $build_id

foreach bd {mrmac_0_core_support platform} {
    set f [get_files $bd.bd]
    set_property REGISTERED_WITH_MANAGER 1 $f
    set_property SYNTH_CHECKPOINT_MODE "Hierarchical" $f
    add_files -norecurse -fileset $src [make_wrapper -fileset $src -files $f -top]
}

set_property top backline_hsb_top $src
set_property top_auto_set 0 $src
update_compile_order -fileset $src

#################################################
# Constraints
#################################################
set con [get_filesets constrs_1]
import_files -fileset $con [list [file join $amd_hw default.xdc] [file join $amd_hw v350_ddr4_single_lp0.xdc]]
set_property used_in_synthesis 0 [get_files v350_ddr4_single_lp0.xdc]

add_files -fileset $con [list \
    [file join $repo hw constraints backline_hsb.xdc] \
    [file join $repo hw constraints hsb_ram_style.xdc] ]
set_property used_in_synthesis 0      [get_files backline_hsb.xdc]
set_property used_in_implementation 0 [get_files hsb_ram_style.xdc]
set_property target_constrs_file [get_files backline_hsb.xdc] $con

# Explore directives and post-route phys_opt, to close timing at 250 MHz (~2x run time)
set_property strategy Performance_ExplorePostRoutePhysOpt [get_runs impl_1]

puts "INFO: created [get_property DIRECTORY [current_project]]/$proj_name.xpr"
