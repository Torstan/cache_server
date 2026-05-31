if {$argc < 3} {
    puts stderr "usage: resp_command.tcl host port command ?arg...?"
    exit 2
}

source [file join [file dirname [info script]] resp_client.tcl]

set host [lindex $argv 0]
set port [lindex $argv 1]
set command [lrange $argv 2 end]

set code [catch {
    resp::connect $host $port
    resp::command {*}$command
} result]
catch {resp::close}

if {$code != 0} {
    puts stderr $result
    exit 1
}

puts $result
