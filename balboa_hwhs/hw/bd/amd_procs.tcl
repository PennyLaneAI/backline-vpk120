# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Xanadu Quantum Technologies Inc.
#
# amd_procs.tcl - functions for reuse of AMD's board setup

set ::amd_tcl_lines {}

proc amd_open {tcl_file} {
    if {![file exists $tcl_file]} {
        error "AMD reference design Tcl not found: $tcl_file (run scripts/unpack-amd.sh first)"
    }
    set fh [open $tcl_file r]
    set ::amd_tcl_lines [split [read $fh] "\n"]
    close $fh
}

# Define a proc from AMD's file. `edits` is a list of {old new} string replacements
# applied to the proc text before it is defined; each `old` must occur exactly once.
proc amd_load_proc {name {edits {}}} {
    set body ""
    set found 0
    foreach line $::amd_tcl_lines {
        if {!$found} {
            if {[string match "proc $name *" $line]} {
                set found 1
                set body "$line\n"
            }
            continue
        }
        append body "$line\n"
        if {[info complete $body]} break
    }
    if {!$found} { error "amd_load_proc: proc $name not found" }
    foreach {old new} $edits {
        set first [string first $old $body]
        if {$first < 0 || [string first $old $body [expr {$first + 1}]] >= 0} {
            error "amd_load_proc: edit anchor for $name must occur exactly once: $old"
        }
        set body [string map [list $old $new] $body]
    }
    uplevel #0 $body
}

# AMD's commands that create axi_noc_0/1/2 (LPDDR4), without axi_noc_0's PL ports
# (S06..S11, aclk6), as a script to eval inside the BD.
proc amd_noc_script {} {
    set start -1
    set end -1
    set i 0
    foreach line $::amd_tcl_lines {
        if {$start < 0 && [string match "*# Create instance: axi_noc_0, and set properties*" $line]} {
            set start $i
        } elseif {$start >= 0 && [string match "*# Create instance: axis_data_fifo_0, and set properties*" $line]} {
            set end $i
            break
        }
        incr i
    }
    if {$start < 0 || $end < 0} { error "amd_noc_script: axi_noc block not found" }

    # Group the lines into commands, then drop the ones that configure the PL ports.
    set script ""
    set cmd ""
    set dropped 0
    foreach line [lrange $::amd_tcl_lines $start [expr {$end - 1}]] {
        append cmd "$line\n"
        if {![info complete $cmd]} continue
        if {[regexp {/axi_noc_0/(S0[6-9]|S1[01])_AXI\]} $cmd] || [string match "*/axi_noc_0/aclk6\]*" $cmd]} {
            incr dropped
        } else {
            append script $cmd
        }
        set cmd ""
    }
    if {$dropped != 7} { error "amd_noc_script: expected to drop 7 PL-port commands, dropped $dropped" }

    foreach {old new} {"CONFIG.NUM_SI {12}" "CONFIG.NUM_SI {6}" "CONFIG.NUM_CLKS {7}" "CONFIG.NUM_CLKS {6}"} {
        if {[string first $old $script] < 0} { error "amd_noc_script: $old not found" }
        set script [string map [list $old $new] $script]
    }
    return $script
}

# AMD's address assignments for the PS masters that reach DDR through the NoC.
proc amd_ps_ddr_addr_script {} {
    set script ""
    set n 0
    foreach line $::amd_tcl_lines {
        if {[string match "*assign_bd_address*cips_block/versal_cips_0/*NOC*" $line]} {
            append script "$line\n"
            incr n
        }
    }
    if {$n != 28} { error "amd_ps_ddr_addr_script: expected 28 PS->DDR segments, found $n" }
    return $script
}
