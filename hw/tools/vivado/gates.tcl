# Signoff gates for the xc7 flow, sourced by build.tcl: a routed design
# gets a bitstream only when every gate passes. Vivado itself reports a
# failing design and writes its bitstream anyway.
#
# - Timing: setup, hold and pulse width met on every clock. Vivado times
#   setup at the slow corner (85 C, minimum voltage) and hold at the fast
#   one, with each clock's jitter charged to every path, so zero slack is
#   the floor. A design that wants margin puts it in its XDC as clock
#   uncertainty, which the tools then optimize against.
# - Coverage: check_timing finds every sequential cell clocked, every
#   endpoint constrained, and every I/O port given a delay or an explicit
#   false path. Slack says nothing about a path Vivado was never asked
#   to time.
# - Clock-domain crossings: report_cdc finds none unsafe or unknown, and
#   every synchronizer flop carries ASYNC_REG.
# - Methodology and DRC: no violation at critical-warning severity or
#   above.
# - No critical warning anywhere in the run: some real faults, such as
#   an MMCM VCO outside its band (AVAL-46), are only critical warnings.
#   The run's log is the record, because get_msg_config's counts miss
#   messages raised inside some commands (route_design's among them).
# - Memory mapping: every memory built the way its ram_style or
#   rom_style declares (log_failures, below).
# - Thermal: the design carries the thermal guard's XADC with its
#   over-temperature shutdown armed, and the bitstream allows the
#   shutdown (thermal_failures, below). No bitstream leaves the flow
#   able to run the part past its rated junction temperature unchecked.
#
# The methodology and DRC gates read the results of the last
# report_methodology and report_drc, so build.tcl runs those first.

# The failures, one line each; empty when every gate passes. log is
# this run's log file.
proc gate_failures {log} {
    set fail {}

    set summary [report_timing_summary -no_detailed_paths -no_check_timing \
                     -return_string]
    if {![string match "*All user specified timing constraints are met.*" $summary]} {
        lappend fail "timing: constraints not met"
    }

    foreach line [split [check_timing -return_string] "\n"] {
        if {[regexp {There (?:are|is) ([0-9]+) } $line -> n] && $n > 0
                && ![string match "*false path constraint*" $line]} {
            lappend fail "check_timing:[string trimright $line]"
        }
    }

    # The CDC summary table ends each row with its Endpoints, Safe,
    # Unsafe, Unknown and No ASYNC_REG counts; the columns before them
    # hold free text.
    set in_table 0
    foreach line [split [report_cdc -return_string] "\n"] {
        if {[string match "Severity *" $line]} {
            set in_table 1
            continue
        }
        if {!$in_table || [string match "--*" $line]} {
            continue
        }
        if {[string trim $line] eq ""} {
            break
        }
        lassign [lrange [regexp -all -inline {\S+} $line] end-2 end] unsafe unknown no_async
        if {![string is integer -strict $unsafe] || ![string is integer -strict $no_async]} {
            lappend fail "cdc: unreadable report row: $line"
        } elseif {$unsafe > 0 || $unknown > 0 || $no_async > 0} {
            lappend fail "cdc: [string trim $line]"
        }
    }

    lappend fail {*}[severe_by_rule methodology [get_methodology_violations -quiet]]
    lappend fail {*}[severe_by_rule drc [get_drc_violations -quiet]]
    lappend fail {*}[log_failures $log]
    lappend fail {*}[thermal_failures]

    return $fail
}

# The thermal guard (xc7_thermal_guard.sv): exactly one XADC, its
# over-temperature alarm not disabled (INIT_41 bit 0), its shutdown
# armed (INIT_53's four LSBs 0011b), and the bitstream allowing the
# shutdown. Any one missing leaves the part without its protection.
proc thermal_failures {} {
    set fail {}
    if {[get_property BITSTREAM.CONFIG.OVERTEMPPOWERDOWN [current_design]] ne "ENABLE"} {
        lappend fail "thermal: BITSTREAM.CONFIG.OVERTEMPPOWERDOWN is not ENABLE"
    }
    set xadc [get_cells -quiet -hierarchical -filter {REF_NAME == XADC}]
    if {[llength $xadc] != 1} {
        lappend fail "thermal: [llength $xadc] XADC instances; the thermal guard provides one"
        return $fail
    }
    if {[xadc_init $xadc 41] & 0x1} {
        lappend fail "thermal: the XADC's over-temperature alarm is disabled (INIT_41)"
    }
    set ot_limit [xadc_init $xadc 53]
    if {($ot_limit & 0xF) != 0x3} {
        lappend fail [format "thermal: the XADC's shutdown is not armed (INIT_53 %04X)" $ot_limit]
    }
    return $fail
}

# The guard's thresholds in degrees C, read back from the XADC the
# design synthesized: empty when there is not exactly one.
proc thermal_lines {} {
    set xadc [get_cells -quiet -hierarchical -filter {REF_NAME == XADC}]
    if {[llength $xadc] != 1} {
        return {}
    }
    return [list [format "shutdown at %.1f C (released below %.1f C), warning at %.1f C (clears below %.1f C)" \
                      [xadc_celsius [xadc_init $xadc 53]] [xadc_celsius [xadc_init $xadc 57]] \
                      [xadc_celsius [xadc_init $xadc 50]] [xadc_celsius [xadc_init $xadc 54]]]]
}

# An XADC cell's INIT_<reg> as an integer (Vivado reports 16'hXXXX).
proc xadc_init {cell reg} {
    set v [get_property INIT_$reg $cell]
    if {![regexp {([0-9A-Fa-f]+)$} $v -> hex]} {
        error "unreadable INIT_$reg on $cell: $v"
    }
    return [expr {"0x$hex" + 0}]
}

# A 16-bit, MSB-justified XADC temperature code in degrees C (UG480,
# Equation 4-2).
proc xadc_celsius {code} {
    return [expr {($code >> 4) * 503.975 / 4096 - 273.15}]
}

# The gates read from the run's log: every critical warning, and every
# memory synthesis did not build the way its ram_style or rom_style
# declares. Vivado
# only warns and falls back to another kind of storage (Synth 8-6849,
# an infeasible style; 8-7186, a style ignored because the array was not
# inferred as a memory; 8-4767, a memory it could not build as RAM at
# all and built from registers), where yosys fails the build, so the
# same RTL is held to the same mapping by both flows.
proc log_failures {log} {
    set critical [dict create]
    set style [dict create]
    set f [open $log r]
    foreach line [split [read $f] "\n"] {
        if {[regexp {^CRITICAL WARNING: \[([^\]]+)\]} $line -> id]} {
            dict incr critical $id
        } elseif {[regexp {\[Synth 8-6849\] Infeasible attribute ((?:ram|rom)_style = "[a-z]+") set for RAM "([^"]+)"} \
                       $line -> attr mem]} {
            dict set style $mem "$attr is infeasible"
        } elseif {[regexp {\[Synth 8-7186\] Applying attribute ((?:ram|rom)_style = "[a-z]+") is ignored, object '([^'\[]+)} \
                       $line -> attr mem]} {
            dict set style $mem "$attr was ignored: not inferred as a memory"
        } elseif {[regexp {\[Synth 8-4767\] Trying to implement RAM '([^']+)' in registers} \
                       $line -> mem]} {
            dict set style $mem "built from registers: not mappable as a RAM"
        }
    }
    close $f

    set fail {}
    dict for {id n} $critical {
        lappend fail "critical warning \[$id\] raised $n time(s); see the log"
    }
    dict for {mem why} $style {
        lappend fail "memory $mem: $why"
    }
    return $fail
}

# One line per rule among the violations at critical-warning severity or
# above: a design missing a clock trips the same rule on every flop.
proc severe_by_rule {kind violations} {
    set count [dict create]
    set first [dict create]
    foreach v $violations {
        if {[get_property SEVERITY $v] ni {{Critical Warning} Error}} {
            continue
        }
        set rule [lindex [split [get_property NAME $v] #] 0]
        if {![dict exists $first $rule]} {
            dict set first $rule [get_property DESCRIPTION $v]
        }
        dict incr count $rule
    }
    set lines {}
    dict for {rule n} $count {
        set what [dict get $first $rule]
        if {$n > 1} {
            set what "$n violations, the first: $what"
        }
        lappend lines "$kind: $rule: $what"
    }
    return $lines
}

# Each clock with timed paths: its period, the worst setup slack of its
# own paths and the frequency that path would allow, then the worst
# setup slack of the paths other clocks launch into it. An own path's
# requirement is the period or a multiple or half of it, so the fmax
# scales that requirement with the period. A crossing's requirement
# comes from its constraint or from the two clocks' edges instead, so
# it gets a slack and no fmax.
proc clock_fmax_lines {} {
    set lines {}
    foreach c [get_clocks] {
        set own [worst_timed_path -from $c -to $c]
        set others [get_clocks -quiet -filter "NAME != [get_property NAME $c]"]
        set into {}
        if {[llength $others] > 0} {
            set into [worst_timed_path -from $others -to $c]
        }
        if {[llength $own] == 0 && [llength $into] == 0} {
            continue
        }
        set period [get_property PERIOD $c]
        set line [format "%s: period %.3f ns" $c $period]
        if {[llength $own] > 0} {
            set wns [get_property SLACK $own]
            set req [get_property REQUIREMENT $own]
            append line [format ", WNS %.3f ns, fmax %.1f MHz" $wns \
                             [expr {1000.0 * $req / ($period * ($req - $wns))}]]
        }
        if {[llength $into] > 0} {
            append line [format "; from other clocks WNS %.3f ns" \
                             [get_property SLACK $into]]
        }
        lappend lines $line
    }
    return $lines
}

# The worst setup path between the given startpoints and endpoints, or
# nothing when every such path is untimed: with no timed path to
# offer, get_timing_paths can return an unconstrained one, which has
# no slack.
proc worst_timed_path {args} {
    set path [get_timing_paths -quiet -delay_type max -max_paths 1 {*}$args]
    if {[llength $path] == 0 || [get_property SLACK $path] eq ""} {
        return {}
    }
    return $path
}
