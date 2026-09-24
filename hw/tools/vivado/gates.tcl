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
# - Memory mapping: every memory built the way its ram_style declares
#   (log_failures, below).
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

    return $fail
}

# The gates read from the run's log: every critical warning, and every
# memory synthesis did not build the way its ram_style declares. Vivado
# only warns and falls back to another kind of storage (Synth 8-6849,
# an infeasible style; 8-7186, a style ignored because the array was not
# inferred as a memory), where yosys fails the build, so the same RTL
# is held to the same mapping by both flows.
proc log_failures {log} {
    set critical [dict create]
    set style [dict create]
    set f [open $log r]
    foreach line [split [read $f] "\n"] {
        if {[regexp {^CRITICAL WARNING: \[([^\]]+)\]} $line -> id]} {
            dict incr critical $id
        } elseif {[regexp {\[Synth 8-6849\] Infeasible attribute (ram_style = "[a-z]+") set for RAM "([^"]+)"} \
                       $line -> attr mem]} {
            dict set style $mem "$attr is infeasible"
        } elseif {[regexp {\[Synth 8-7186\] Applying attribute (ram_style = "[a-z]+") is ignored, object '([^'\[]+)} \
                       $line -> attr mem]} {
            dict set style $mem "$attr was ignored: not inferred as a memory"
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

# Each clock with timed paths: its period, worst setup slack, and the
# frequency its worst path would allow.
proc clock_fmax_lines {} {
    set lines {}
    foreach c [get_clocks] {
        set path [get_timing_paths -quiet -delay_type max -max_paths 1 -group $c]
        if {[llength $path] == 0} {
            continue
        }
        set period [get_property PERIOD $c]
        set wns [get_property SLACK $path]
        lappend lines [format "%s: period %.3f ns, WNS %.3f ns, fmax %.1f MHz" \
                           $c $period $wns [expr {1000.0 / ($period - $wns)}]]
    }
    return $lines
}
