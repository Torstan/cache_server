set ::passed 0
set ::failed 0

source [file join [file dirname [info script]] resp_client.tcl]

proc r {args} {
    return [resp::command {*}$args]
}

proc assert_equal {expected actual} {
    if {$expected ne $actual} {
        error "assert_equal failed: expected '$expected' got '$actual'"
    }
}

proc assert_match {pattern actual} {
    if {![string match $pattern $actual]} {
        error "assert_match failed: pattern '$pattern' got '$actual'"
    }
}

proc assert_error {pattern body} {
    set err {}
    if {![catch {uplevel 1 $body} err]} {
        error "assert_error failed: command succeeded"
    }
    assert_match $pattern $err
}

proc assert_range {actual min max} {
    if {$actual < $min || $actual > $max} {
        error "assert_range failed: $actual not in $min..$max"
    }
}

proc assert {expr} {
    if {![uplevel 1 [list expr $expr]]} {
        error "assert failed: $expr"
    }
}

proc test {name body {expected __no_expected__}} {
    global passed failed
    set result {}
    set code [catch {uplevel 1 $body} result]
    if {$code != 0} {
        incr failed
        puts stderr "FAIL $name: $result"
        return
    }
    if {$expected ne "__no_expected__"} {
        if {![string match $expected $result]} {
            incr failed
            puts stderr "FAIL $name: expected '$expected' got '$result'"
            return
        }
    }
    incr passed
    puts "PASS $name"
}

proc start_server {args body} {
    uplevel 1 $body
}

proc tags {args body} {
    uplevel 1 $body
}

proc randomInt {max} {
    return [expr {int(rand() * $max)}]
}

proc randstring {min max {type alpha}} {
    set len [expr {$min + [randomInt [expr {$max - $min + 1}]]}]
    set chars abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789
    set out ""
    for {set i 0} {$i < $len} {incr i} {
        append out [string index $chars [randomInt [string length $chars]]]
    }
    return $out
}

proc randomValue {} {
    return [randstring 1 16 alpha]
}

if {$argc < 3} {
    puts stderr "usage: cache_server_unit_runner.tcl host port file ?file...?"
    exit 2
}

set host [lindex $argv 0]
set port [lindex $argv 1]
resp::connect $host $port
foreach file [lrange $argv 2 end] {
    source $file
}
resp::close

puts "Redis unit tests: $::passed passed, $::failed failed"
if {$::failed != 0} {
    exit 1
}
