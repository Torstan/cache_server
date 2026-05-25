# Filtered Redis 6.2 unit tests for cache_server.
# Unsupported command blocks were removed; retained test blocks are unchanged.

# Deleted during cache_server port: unsupported by cache_server command subset; test: Failing test
# Deleted during cache_server port: unsupported command(s): lpush, r, save; test: SAVE - make sure there are all the types as values
# Deleted during cache_server port: parameterized or encoding-dependent test; test: FUZZ stresser with data model $fuzztype
# Deleted during cache_server port: unsupported command(s): bgsave, r, save; test: BGSAVE
# Deleted during cache_server port: unsupported command(s): select; test: SELECT an out of range DB
# Deleted during cache_server port: unsupported command(s): debug, flushdb; test: Check consistency of different data types after a reload
# Deleted during cache_server port: unsupported command(s): bgrewriteaof, config, debug, r; test: Same dataset digest if saving/reloading as AOF?
# Deleted during cache_server port: unsupported command(s): bgrewriteaof, debug, flushdb, r, save; test: EXPIRES after a reload (snapshot + append only file rewrite)
# Deleted during cache_server port: unsupported command(s): config, debug, expireat, flushdb, pexpire, pexpireat, psetex, setex; test: EXPIRES after AOF reload (without rewrite)
# Deleted during cache_server port: out-of-scope helper/capability: config, select; test: PIPELINING stresser (also a regression for the old epoll bug)
test {APPEND basics} {
        r del foo
        list [r append foo bar] [r get foo] \
             [r append foo 100] [r get foo]
    } {3 bar 6 bar100}

test {APPEND basics, integer encoded values} {
        set res {}
        r del foo
        r append foo 1
        r append foo 2
        lappend res [r get foo]
        r set foo 1
        r append foo 2
        lappend res [r get foo]
    } {12 12}

test {APPEND fuzzing} {
        set err {}
        foreach type {binary alpha compr} {
            set buf {}
            r del x
            for {set i 0} {$i < 1000} {incr i} {
                set bin [randstring 0 10 $type]
                append buf $bin
                r append x $bin
            }
            if {$buf != [r get x]} {
                set err "Expected '$buf' found '[r get x]'"
                break
            }
        }
        set _ $err
    } {}

# Deleted during cache_server port: unsupported command(s): dbsize, flushdb, select; test: FLUSHDB
# Deleted during cache_server port: unsupported command(s): r; test: Perform a final SAVE to leave a clean DB on disk
# Deleted during cache_server port: unsupported command(s): client, reset; test: RESET clears client state
# Deleted during cache_server port: unsupported command(s): client; test: RESET clears MONITOR state
# Deleted during cache_server port: unsupported command(s): exec, multi, reset; test: RESET clears and discards MULTI state
# Deleted during cache_server port: unsupported command(s): reset, subscribe; test: RESET clears Pub/Sub state
# Deleted during cache_server port: unsupported command(s): acl, auth, reset; test: RESET clears authenticated state
# Deleted during cache_server port: unsupported command(s): bgsave, config, debug, mset; test: Don't rehash if redis has child proecess
# Deleted during cache_server port: unsupported command(s): config; test: Process title set as expected
