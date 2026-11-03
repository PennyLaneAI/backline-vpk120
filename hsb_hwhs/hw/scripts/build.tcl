# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Xanadu Quantum Technologies Inc.
#
# build.tcl - synthesize, implement, write the device image and export the XSA.
#
#   vivado -mode batch -source hw/scripts/build.tcl -tclargs \
#       -xpr build/vivado/backline_hsb/backline_hsb.xpr [-jobs 8] [-clean]
#
# Outputs next to the .xpr: backline_hsb.xsa (for PetaLinux), the .pdi from
# impl_1, and timing/utilization reports under reports/.

set xpr   ""
set jobs  4
set clean 0

for {set i 0} {$i < $::argc} {incr i} {
    set opt [lindex $::argv $i]
    switch -exact -- $opt {
        "-xpr"   { incr i; set xpr [file normalize [lindex $::argv $i]] }
        "-jobs"  { incr i; set jobs [lindex $::argv $i] }
        "-clean" { set clean 1 }
        default  { error "build.tcl: unknown option $opt" }
    }
}
if {$xpr eq "" || ![file exists $xpr]} { error "build.tcl: -xpr <project.xpr> is required (run create_project.tcl first)" }

proc check_run {run_name} {
    set status [get_property STATUS [get_runs $run_name]]
    puts "INFO: $run_name status: $status"
    if {![string match "*Complete!" $status]} {
        error "$run_name failed. Status: $status"
    }
}

open_project $xpr
set proj_dir [file dirname $xpr]

if {$clean} {
    foreach bd {mrmac_0_core_support.bd platform.bd} {
        reset_target all [get_files $bd]
        generate_target all [get_files $bd]
    }
    reset_run synth_1
}

if {![string match "*Complete!" [get_property STATUS [get_runs synth_1]]]} {
    launch_runs synth_1 -jobs $jobs
    wait_on_run synth_1
}
check_run synth_1

launch_runs impl_1 -to_step write_device_image -jobs $jobs
wait_on_run impl_1
check_run impl_1

open_run impl_1 -name impl_1
file mkdir [file join $proj_dir reports]
report_timing_summary -max_paths 20 -file [file join $proj_dir reports timing_summary.rpt]
report_utilization -hierarchical -hierarchical_depth 3 -file [file join $proj_dir reports utilization.rpt]
report_clock_interaction -file [file join $proj_dir reports clock_interaction.rpt]

set wns [get_property SLACK [get_timing_paths -max_paths 1 -nworst 1 -setup]]
set whs [get_property SLACK [get_timing_paths -max_paths 1 -nworst 1 -hold]]
puts "INFO: WNS=$wns ns WHS=$whs ns"
if {$wns < 0 || $whs < 0} {
    puts "CRITICAL WARNING: timing is not met (WNS=$wns, WHS=$whs); see reports/timing_summary.rpt."
}

set xsa [file join $proj_dir backline_hsb.xsa]
write_hw_platform -fixed -include_bit -force -file $xsa
puts "INFO: XSA: $xsa"
foreach pdi [glob -nocomplain -directory [get_property DIRECTORY [get_runs impl_1]] *.pdi] {
    puts "INFO: PDI: $pdi"
}
