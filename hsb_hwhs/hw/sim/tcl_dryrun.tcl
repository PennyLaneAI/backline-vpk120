# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Xanadu Quantum Technologies Inc.
#
# tcl_dryrun.tcl - run hw/scripts/create_project.tcl in tclsh with Vivado commands mocked.
#
#   tclsh hw/sim/tcl_dryrun.tcl <amd_hw_dir>

set amd_hw [file normalize [lindex $argv 0]]
set repo   [file normalize [file join [file dirname [info script]] .. ..]]

set ::cells   {}        ;# BD cells, full path without leading /
set ::ports   {}        ;# BD external ports
set ::hpins   {}        ;# pins created on hierarchical cells
set ::cur     ""        ;# current BD instance path
set ::errors  0
set ::checked 0

proc fail {msg} { incr ::errors; puts "DRYRUN ERROR: $msg" }

proc path_of {name} {
    set n [string trimleft $name /]
    if {[string index $name 0] eq "/" || $::cur eq ""} { return $n }
    return "$::cur/$n"
}

# --- Vivado project-level commands --------------------------------------------------
foreach c {create_project set_property update_ip_catalog update_compile_order add_files \
           import_files reset_target generate_target launch_runs wait_on_run open_run \
           report_timing_summary report_utilization write_hw_platform save_bd_design \
           close_bd_design validate_bd_design open_project report_clock_interaction} {
    proc $c {args} { return "" }
}
proc current_project {args} { return proj }
proc get_projects {args} { return "" }
# Vivado's get_* take a single pattern
proc one_pattern {cmd argl} {
    set pos {}
    for {set i 0} {$i < [llength $argl]} {incr i} {
        set a [lindex $argl $i]
        if {$a in {-of_objects -filter -compile_order -used_in}} {
            incr i
        } elseif {[string index $a 0] ne "-"} {
            lappend pos $a
        }
    }
    if {[llength $pos] > 1} { fail "$cmd: too many positional options: $pos" }
    return [lindex $argl end]
}
proc get_filesets {args} { one_pattern get_filesets $args }
proc get_files {args} { one_pattern get_files $args }
proc get_runs {args} { one_pattern get_runs $args }
proc get_property {args} { if {[lindex $args 0] eq "TYPE"} { return hier }; return "" }
proc get_ipdefs {args} { return "ipdef" }
proc make_wrapper {args} { return "/tmp/wrapper.v" }
namespace eval common { proc send_gid_msg {args} { puts "  msg: [lindex $args end]" } }

# --- block design ---------------------------------------------------------------------
proc create_bd_design {name} { set ::cells {}; set ::ports {}; set ::hpins {}; set ::cur ""; puts "  BD $name" }
proc current_bd_instance {args} {
    if {[llength $args] == 0 || [lindex $args 0] eq "."} { return "/$::cur" }
    set p [lindex $args 0]
    if {$p eq "/"} { set ::cur "" } else { set ::cur [path_of $p] }
    return "/$::cur"
}
proc get_bd_cells {args} {
    set p [lindex $args end]
    if {$p eq "/"} { return "/" }
    set p [path_of $p]
    if {[lsearch -exact $::cells $p] < 0} { fail "get_bd_cells: no cell $p" }
    return "/$p"
}
proc create_bd_cell {args} {
    set name [lindex $args end]
    set p [path_of $name]
    lappend ::cells $p
    return "/$p"
}
proc create_bd_port {args} { set n [lindex $args end]; lappend ::ports $n; return $n }
proc create_bd_intf_port {args} { set n [lindex $args end]; lappend ::ports $n; return $n }
proc create_bd_pin {args} { lappend ::hpins [path_of [lindex $args end]]; return "" }
proc create_bd_intf_pin {args} { lappend ::hpins [path_of [lindex $args end]]; return "" }

proc check_pin {spec} {
    incr ::checked
    set p [path_of $spec]
    if {[lsearch -exact $::hpins $p] >= 0} { return }
    set cell [file dirname $p]
    if {$cell eq "."} { fail "pin $spec has no cell"; return }
    if {[lsearch -exact $::cells $cell] < 0} { fail "pin $spec: no cell $cell" }
}
proc get_bd_pins {args} { foreach s $args { if {[string index $s 0] ne "-"} { check_pin $s } }; return $args }
proc get_bd_intf_pins {args} { foreach s $args { if {[string index $s 0] ne "-"} { check_pin $s } }; return $args }
proc get_bd_ports {args} {
    foreach s $args {
        incr ::checked
        if {[lsearch -exact $::ports $s] < 0} { fail "no port $s" }
    }
    return $args
}
proc get_bd_intf_ports {args} { return [get_bd_ports {*}$args] }
proc connect_bd_net {args} { return "" }
proc connect_bd_intf_net {args} { return "" }
proc delete_bd_objs {args} { return "" }

proc get_bd_addr_spaces {args} {
    set s [lindex $args end]
    incr ::checked
    set cell [file dirname $s]
    if {[lsearch -exact $::ports $s] < 0 &&
        [lsearch -exact $::cells $cell] < 0 && [lsearch -exact $::cells [file dirname $cell]] < 0} {
        fail "address space $s: no cell"
    }
    return $s
}
proc get_bd_addr_segs {args} {
    set s [lindex $args end]
    incr ::checked
    set head [lindex [split $s /] 0]
    set two [join [lrange [split $s /] 0 1] /]
    if {[lsearch -exact $::ports $head] < 0 && [lsearch -exact $::cells $head] < 0 &&
        [lsearch -exact $::cells $two] < 0} {
        fail "address segment $s: no cell or port"
    }
    return $s
}
proc assign_bd_address {args} { return "" }
proc exclude_bd_addr_seg {args} { return "" }

# --- run ------------------------------------------------------------------------------
set ::argv [list -amd $amd_hw -out /tmp/backline_hsb_dryrun]
set ::argc [llength $::argv]
if {[catch {source [file join $repo hw scripts create_project.tcl]} err]} {
    fail "create_project.tcl: $err\n$::errorInfo"
}
puts "dry run: $::checked references checked, $::errors error(s)"
exit [expr {$::errors ? 1 : 0}]
