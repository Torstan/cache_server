namespace eval resp {
    variable sock

    proc connect {host port} {
        variable sock
        set sock [socket $host $port]
        fconfigure $sock -translation binary -encoding binary -buffering none
    }

    proc close {} {
        variable sock
        if {[info exists sock]} {
            catch {::close $sock}
            unset sock
        }
    }

    proc write_command {args} {
        variable sock
        puts -nonewline $sock "*[llength $args]\r\n"
        foreach arg $args {
            set bytes [encoding convertto utf-8 $arg]
            puts -nonewline $sock "\$[string length $bytes]\r\n"
            puts -nonewline $sock $bytes
            puts -nonewline $sock "\r\n"
        }
        flush $sock
    }

    proc read_line {} {
        variable sock
        set line [gets $sock]
        if {[string index $line end] eq "\r"} {
            set line [string range $line 0 end-1]
        }
        return $line
    }

    proc read_exact {n} {
        variable sock
        set data ""
        while {[string length $data] < $n} {
            append data [read $sock [expr {$n - [string length $data]}]]
        }
        return $data
    }

    proc read_reply {} {
        variable sock
        set prefix [read $sock 1]
        switch -- $prefix {
            "+" {
                return [read_line]
            }
            "-" {
                error [read_line]
            }
            ":" {
                return [read_line]
            }
            "$" {
                set len [read_line]
                if {$len < 0} {
                    return {}
                }
                set data [read_exact $len]
                read_exact 2
                return $data
            }
            "*" {
                set count [read_line]
                if {$count < 0} {
                    return {}
                }
                set out {}
                for {set i 0} {$i < $count} {incr i} {
                    lappend out [read_reply]
                }
                return $out
            }
            default {
                error "invalid RESP prefix '$prefix'"
            }
        }
    }

    proc command {args} {
        write_command {*}$args
        return [read_reply]
    }
}
