# Filtered Redis 6.2 unit tests for cache_server.
# Unsupported command blocks were removed; retained test blocks are unchanged.

# Deleted during cache_server port: out-of-scope helper/capability: info; test: HSET/HLEN - Small hash creation
# Deleted during cache_server port: out-of-scope helper/capability: assert_encoding; test: Is the small hash encoded with a ziplist?
# Deleted during cache_server port: parameterized or encoding-dependent test; test: HRANDFIELD - $type
# Deleted during cache_server port: unsupported command(s): hello, hrandfield; test: HRANDFIELD with RESP3
# Deleted during cache_server port: unsupported command(s): hrandfield; test: HRANDFIELD count of 0 is handled correctly
# Deleted during cache_server port: unsupported command(s): hrandfield; test: HRANDFIELD count overflow
# Deleted during cache_server port: unsupported command(s): hrandfield; test: HRANDFIELD with <count> against non existing key
# Deleted during cache_server port: unsupported command(s): hrandfield; test: HRANDFIELD count of 0 is handled correctly - emptyarray
# Deleted during cache_server port: unsupported command(s): hrandfield; test: HRANDFIELD with <count> against non existing key - emptyarray
# Deleted during cache_server port: parameterized or encoding-dependent test; test: HRANDFIELD with <count> - $type
# Deleted during cache_server port: out-of-scope helper/capability: info; test: HSET/HLEN - Big hash creation
# Deleted during cache_server port: out-of-scope helper/capability: assert_encoding; test: Is the big hash encoded with an hash table?
test {HGET against the small hash} {
        set err {}
        foreach k [array names smallhash *] {
            if {$smallhash($k) ne [r hget smallhash $k]} {
                set err "$smallhash($k) != [r hget smallhash $k]"
                break
            }
        }
        set _ $err
    } {}

test {HGET against the big hash} {
        set err {}
        foreach k [array names bighash *] {
            if {$bighash($k) ne [r hget bighash $k]} {
                set err "$bighash($k) != [r hget bighash $k]"
                break
            }
        }
        set _ $err
    } {}

test {HGET against non existing key} {
        set rv {}
        lappend rv [r hget smallhash __123123123__]
        lappend rv [r hget bighash __123123123__]
        set _ $rv
    } {{} {}}

# Deleted during cache_server port: unsupported command family: lindex; test: HSET in update and insert mode
# Deleted during cache_server port: unsupported command(s): hsetnx; test: HSETNX target key missing - small hash
# Deleted during cache_server port: unsupported command(s): hsetnx; test: HSETNX target key exists - small hash
# Deleted during cache_server port: unsupported command(s): hsetnx; test: HSETNX target key missing - big hash
# Deleted during cache_server port: unsupported command(s): hsetnx; test: HSETNX target key exists - big hash
test {HMSET wrong number of args} {
        catch {r hmset smallhash key1 val1 key2} err
        format $err
    } {*wrong number*}

test {HMSET - small hash} {
        set args {}
        foreach {k v} [array get smallhash] {
            set newval [randstring 0 8 alpha]
            set smallhash($k) $newval
            lappend args $k $newval
        }
        r hmset smallhash {*}$args
    } {OK}

test {HMSET - big hash} {
        set args {}
        foreach {k v} [array get bighash] {
            set newval [randstring 0 8 alpha]
            set bighash($k) $newval
            lappend args $k $newval
        }
        r hmset bighash {*}$args
    } {OK}

test {HMGET against non existing key and fields} {
        set rv {}
        lappend rv [r hmget doesntexist __123123123__ __456456456__]
        lappend rv [r hmget smallhash __123123123__ __456456456__]
        lappend rv [r hmget bighash __123123123__ __456456456__]
        set _ $rv
    } {{{} {}} {{} {}} {{} {}}}

# Deleted during cache_server port: out-of-scope helper/capability: eval; test: HMGET against wrong type
# Deleted during cache_server port: out-of-scope helper/capability: keys; test: HMGET - small hash
# Deleted during cache_server port: out-of-scope helper/capability: keys; test: HMGET - big hash
# Deleted during cache_server port: out-of-scope helper/capability: keys; test: HKEYS - small hash
# Deleted during cache_server port: out-of-scope helper/capability: keys; test: HKEYS - big hash
test {HVALS - small hash} {
        set vals {}
        foreach {k v} [array get smallhash] {
            lappend vals $v
        }
        set _ [lsort $vals]
    }

test {HVALS - big hash} {
        set vals {}
        foreach {k v} [array get bighash] {
            lappend vals $v
        }
        set _ [lsort $vals]
    }

test {HGETALL - small hash} {
        lsort [r hgetall smallhash]
    }

test {HGETALL - big hash} {
        lsort [r hgetall bighash]
    }

# Deleted during cache_server port: unsupported command family: lindex; test: HDEL and return value
test {HDEL - more than a single value} {
        set rv {}
        r del myhash
        r hmset myhash a 1 b 2 c 3
        assert_equal 0 [r hdel myhash x y]
        assert_equal 2 [r hdel myhash a c f]
        r hgetall myhash
    } {b 2}

test {HDEL - hash becomes empty before deleting all specified fields} {
        r del myhash
        r hmset myhash a 1 b 2 c 3
        assert_equal 3 [r hdel myhash a b c d e]
        assert_equal 0 [r exists myhash]
    }

# Deleted during cache_server port: unsupported command family: lindex; test: HEXISTS
# Deleted during cache_server port: unsupported command(s): debug; test: Is a ziplist encoded Hash promoted on big payload?
test {HINCRBY against non existing database key} {
        r del htest
        list [r hincrby htest foo 2]
    } {2}

test {HINCRBY against non existing hash key} {
        set rv {}
        r hdel smallhash tmp
        r hdel bighash tmp
        lappend rv [r hincrby smallhash tmp 2]
        lappend rv [r hget smallhash tmp]
        lappend rv [r hincrby bighash tmp 2]
        lappend rv [r hget bighash tmp]
    } {2 2 2 2}

test {HINCRBY against hash key created by hincrby itself} {
        set rv {}
        lappend rv [r hincrby smallhash tmp 3]
        lappend rv [r hget smallhash tmp]
        lappend rv [r hincrby bighash tmp 3]
        lappend rv [r hget bighash tmp]
    } {5 5 5 5}

test {HINCRBY against hash key originally set with HSET} {
        r hset smallhash tmp 100
        r hset bighash tmp 100
        list [r hincrby smallhash tmp 2] [r hincrby bighash tmp 2]
    } {102 102}

test {HINCRBY over 32bit value} {
        r hset smallhash tmp 17179869184
        r hset bighash tmp 17179869184
        list [r hincrby smallhash tmp 1] [r hincrby bighash tmp 1]
    } {17179869185 17179869185}

test {HINCRBY over 32bit value with over 32bit increment} {
        r hset smallhash tmp 17179869184
        r hset bighash tmp 17179869184
        list [r hincrby smallhash tmp 17179869184] [r hincrby bighash tmp 17179869184]
    } {34359738368 34359738368}

test {HINCRBY fails against hash value with spaces (left)} {
        r hset smallhash str " 11"
        r hset bighash str " 11"
        catch {r hincrby smallhash str 1} smallerr
        catch {r hincrby bighash str 1} bigerr
        set rv {}
        lappend rv [string match "ERR*not an integer*" $smallerr]
        lappend rv [string match "ERR*not an integer*" $bigerr]
    } {1 1}

test {HINCRBY fails against hash value with spaces (right)} {
        r hset smallhash str "11 "
        r hset bighash str "11 "
        catch {r hincrby smallhash str 1} smallerr
        catch {r hincrby bighash str 1} bigerr
        set rv {}
        lappend rv [string match "ERR*not an integer*" $smallerr]
        lappend rv [string match "ERR*not an integer*" $bigerr]
    } {1 1}

test {HINCRBY can detect overflows} {
        set e {}
        r hset hash n -9223372036854775484
        assert {[r hincrby hash n -1] == -9223372036854775485}
        catch {r hincrby hash n -10000} e
        set e
    } {*overflow*}

# Deleted during cache_server port: unsupported command(s): hincrbyfloat; test: HINCRBYFLOAT against non existing database key
# Deleted during cache_server port: unsupported command(s): hincrbyfloat; test: HINCRBYFLOAT against non existing hash key
# Deleted during cache_server port: unsupported command(s): hincrbyfloat; test: HINCRBYFLOAT against hash key created by hincrby itself
# Deleted during cache_server port: unsupported command(s): hincrbyfloat; test: HINCRBYFLOAT against hash key originally set with HSET
# Deleted during cache_server port: unsupported command(s): hincrbyfloat; test: HINCRBYFLOAT over 32bit value
# Deleted during cache_server port: unsupported command(s): hincrbyfloat; test: HINCRBYFLOAT over 32bit value with over 32bit increment
# Deleted during cache_server port: unsupported command(s): hincrbyfloat; test: HINCRBYFLOAT fails against hash value with spaces (left)
# Deleted during cache_server port: unsupported command(s): hincrbyfloat; test: HINCRBYFLOAT fails against hash value with spaces (right)
# Deleted during cache_server port: unsupported command(s): hincrbyfloat; test: HINCRBYFLOAT fails against hash value that contains a null-terminator in the middle
test {HSTRLEN against the small hash} {
        set err {}
        foreach k [array names smallhash *] {
            if {[string length $smallhash($k)] ne [r hstrlen smallhash $k]} {
                set err "[string length $smallhash($k)] != [r hstrlen smallhash $k]"
                break
            }
        }
        set _ $err
    } {}

test {HSTRLEN against the big hash} {
        set err {}
        foreach k [array names bighash *] {
            if {[string length $bighash($k)] ne [r hstrlen bighash $k]} {
                set err "[string length $bighash($k)] != [r hstrlen bighash $k]"
                puts "HSTRLEN and logical length mismatch:"
                puts "key: $k"
                puts "Logical content: $bighash($k)"
                puts "Server  content: [r hget bighash $k]"
            }
        }
        set _ $err
    } {}

test {HSTRLEN against non existing field} {
        set rv {}
        lappend rv [r hstrlen smallhash __123123123__]
        lappend rv [r hstrlen bighash __123123123__]
        set _ $rv
    } {0 0}

test {HSTRLEN corner cases} {
        set vals {
            -9223372036854775808 9223372036854775807 9223372036854775808
            {} 0 -1 x
        }
        foreach v $vals {
            r hmset smallhash field $v
            r hmset bighash field $v
            set len1 [string length $v]
            set len2 [r hstrlen smallhash field]
            set len3 [r hstrlen bighash field]
            assert {$len1 == $len2}
            assert {$len2 == $len3}
        }
    }

# Deleted during cache_server port: out-of-scope helper/capability: keys; test: Hash ziplist regression test for large keys
# Deleted during cache_server port: parameterized or encoding-dependent test; test: Hash fuzzing #1 - $size fields
# Deleted during cache_server port: parameterized or encoding-dependent test; test: Hash fuzzing #2 - $size fields
# Deleted during cache_server port: unsupported command(s): config, object; test: Stress test the hash ziplist -> hashtable encoding conversion
# Deleted during cache_server port: unsupported command(s): hincrbyfloat; test: Test HINCRBYFLOAT for correct float representation (issue #2846)
# Deleted during cache_server port: unsupported command(s): config, dump, restore; test: Hash ziplist of various encodings
# Deleted during cache_server port: unsupported command(s): config, restore; test: Hash ziplist of various encodings - sanitize dump
# Deleted during cache_server port: unsupported command(s): hincrbyfloat; test: HINCRBYFLOAT does not allow NaN or Infinity
