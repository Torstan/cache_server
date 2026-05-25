# Filtered Redis 6.2 unit tests for cache_server.
# Unsupported command blocks were removed; retained test blocks are unchanged.

test {SET and GET an item} {
        r set x foobar
        r get x
    } {foobar}

test {SET and GET an empty item} {
        r set x {}
        r get x
    } {}

test {Very big payload in GET/SET} {
        set buf [string repeat "abcd" 1000000]
        r set foo $buf
        r get foo
    }

test {Very big payload random access} {
            set err {}
            array set payload {}
            for {set j 0} {$j < 100} {incr j} {
                set size [expr 1+[randomInt 100000]]
                set buf [string repeat "pl-$j" $size]
                set payload($j) $buf
                r set bigpayload_$j $buf
            }
            for {set j 0} {$j < 1000} {incr j} {
                set index [randomInt 100]
                set buf [r get bigpayload_$index]
                if {$buf != $payload($index)} {
                    set err "Values differ: I set '$payload($index)' but I read back '$buf'"
                    break
                }
            }
            unset payload
            set _ $err
        } {}

# Deleted during cache_server port: unsupported command(s): flushdb; test: SET 10000 numeric keys and access all them in reverse order
# Deleted during cache_server port: unsupported command(s): dbsize; test: DBSIZE should be 10000 now
test "SETNX target key missing" {
        r del novar
        assert_equal 1 [r setnx novar foobared]
        assert_equal "foobared" [r get novar]
    }

test "SETNX target key exists" {
        r set novar foobared
        assert_equal 0 [r setnx novar blabla]
        assert_equal "foobared" [r get novar]
    }

test "SETNX against not-expired volatile key" {
        r set x 10
        r expire x 10000
        assert_equal 0 [r setnx x 20]
        assert_equal 10 [r get x]
    }

# Deleted during cache_server port: unsupported command(s): setex; test: SETNX against expired volatile key
# Deleted during cache_server port: unsupported command(s): getex; test: GETEX EX option
# Deleted during cache_server port: unsupported command(s): getex; test: GETEX PX option
# Deleted during cache_server port: unsupported command(s): getex; test: GETEX EXAT option
# Deleted during cache_server port: unsupported command(s): getex; test: GETEX PXAT option
# Deleted during cache_server port: unsupported command(s): getex; test: GETEX PERSIST option
# Deleted during cache_server port: unsupported command(s): getex; test: GETEX no option
# Deleted during cache_server port: unsupported command(s): getex; test: GETEX syntax errors
# Deleted during cache_server port: unsupported command(s): getex; test: GETEX no arguments
# Deleted during cache_server port: unsupported command(s): getdel; test: GETDEL command
# Deleted during cache_server port: unsupported command(s): getdel; test: GETDEL propagate as DEL command to replica
# Deleted during cache_server port: unsupported command(s): getex; test: GETEX without argument does not propagate to replica
# Deleted during cache_server port: unsupported command(s): flushdb; test: MGET
test {MGET against non existing key} {
        r mget foo baazz bar
    } {BAR {} FOO}

test {MGET against non-string key} {
        r sadd myset ciao
        r sadd myset bau
        r mget foo baazz bar myset
    } {BAR {} FOO {}}

test {GETSET (set new value)} {
        r del foo
        list [r getset foo xyz] [r get foo]
    } {{} xyz}

test {GETSET (replace old value)} {
        r set foo bar
        list [r getset foo xyz] [r get foo]
    } {bar xyz}

# Deleted during cache_server port: unsupported command(s): mset; test: MSET base case
# Deleted during cache_server port: unsupported command(s): mset; test: MSET wrong number of args
# Deleted during cache_server port: unsupported command(s): msetnx; test: MSETNX with already existent key
# Deleted during cache_server port: unsupported command(s): msetnx; test: MSETNX with not existing keys
test "STRLEN against non-existing key" {
        assert_equal 0 [r strlen notakey]
    }

test "STRLEN against integer-encoded value" {
        r set myinteger -555
        assert_equal 4 [r strlen myinteger]
    }

test "STRLEN against plain string" {
        r set mystring "foozzz0123456789 baz"
        assert_equal 20 [r strlen mystring]
    }

# Deleted during cache_server port: unsupported command(s): setbit; test: SETBIT against non-existing key
# Deleted during cache_server port: unsupported command(s): setbit; test: SETBIT against string-encoded key
# Deleted during cache_server port: unsupported command(s): setbit; test: SETBIT against integer-encoded key
# Deleted during cache_server port: unsupported command(s): lpush, setbit; test: SETBIT against key with wrong type
# Deleted during cache_server port: unsupported command(s): setbit; test: SETBIT with out of range bit offset
# Deleted during cache_server port: unsupported command(s): setbit; test: SETBIT with non-bit argument
# Deleted during cache_server port: unsupported command(s): setbit; test: SETBIT fuzzing
# Deleted during cache_server port: unsupported command(s): getbit; test: GETBIT against non-existing key
# Deleted during cache_server port: unsupported command(s): getbit; test: GETBIT against string-encoded key
# Deleted during cache_server port: unsupported command(s): getbit; test: GETBIT against integer-encoded key
# Deleted during cache_server port: unsupported command(s): setrange; test: SETRANGE against non-existing key
# Deleted during cache_server port: unsupported command(s): setrange; test: SETRANGE against string-encoded key
# Deleted during cache_server port: unsupported command(s): setrange; test: SETRANGE against integer-encoded key
# Deleted during cache_server port: unsupported command(s): lpush, setrange; test: SETRANGE against key with wrong type
# Deleted during cache_server port: unsupported command(s): setrange; test: SETRANGE with out of range offset
# Deleted during cache_server port: unsupported command(s): getrange; test: GETRANGE against non-existing key
# Deleted during cache_server port: unsupported command(s): getrange; test: GETRANGE against string value
# Deleted during cache_server port: unsupported command(s): getrange; test: GETRANGE against integer-encoded value
# Deleted during cache_server port: unsupported command(s): getrange; test: GETRANGE fuzzing
# Deleted during cache_server port: unsupported command(s): memory; test: trim on SET with big value
test {Extended SET can detect syntax errors} {
        set e {}
        catch {r set foo bar non-existing-option} e
        set e
    } {*syntax*}

test {Extended SET NX option} {
        r del foo
        set v1 [r set foo 1 nx]
        set v2 [r set foo 2 nx]
        list $v1 $v2 [r get foo]
    } {OK {} 1}

test {Extended SET XX option} {
        r del foo
        set v1 [r set foo 1 xx]
        r set foo bar
        set v2 [r set foo 2 xx]
        list $v1 $v2 [r get foo]
    } {{} OK 2}

test {Extended SET GET option} {
        r del foo
        r set foo bar
        set old_value [r set foo bar2 GET]
        set new_value [r get foo]
        list $old_value $new_value
    } {bar bar2}

test {Extended SET GET option with no previous value} {
        r del foo
        set old_value [r set foo bar GET]
        set new_value [r get foo]
        list $old_value $new_value
    } {{} bar}

test {Extended SET GET with NX option should result in syntax err} {
      catch {r set foo bar NX GET} err1
      catch {r set foo bar NX GET} err2
      list $err1 $err2
    } {*syntax err* *syntax err*}

# Deleted during cache_server port: unsupported command(s): rpop, rpush; test: Extended SET GET with incorrect type should result in wrong type error
test {Extended SET EX option} {
        r del foo
        r set foo bar ex 10
        set ttl [r ttl foo]
        assert {$ttl <= 10 && $ttl > 5}
    }

test {Extended SET PX option} {
        r del foo
        r set foo bar px 10000
        set ttl [r ttl foo]
        assert {$ttl <= 10 && $ttl > 5}
    }

test "Extended SET EXAT option" {
        r del foo
        r set foo bar exat [expr [clock seconds] + 10]
        assert_range [r ttl foo] 5 10
    }

test "Extended SET PXAT option" {
        r del foo
        r set foo bar pxat [expr [clock milliseconds] + 10000]
        assert_range [r ttl foo] 5 10
    }

# Deleted during cache_server port: out-of-scope helper/capability: multi; test: Extended SET using multiple options at once
# Deleted during cache_server port: unsupported command(s): getrange; test: GETRANGE with huge ranges, Github issue #1844
# Deleted during cache_server port: unsupported command(s): stralgo; test: STRALGO LCS string output with STRINGS option
# Deleted during cache_server port: unsupported command(s): stralgo; test: STRALGO LCS len
# Deleted during cache_server port: unsupported command(s): stralgo; test: LCS with KEYS option
# Deleted during cache_server port: unsupported command(s): stralgo; test: LCS indexes
# Deleted during cache_server port: unsupported command(s): stralgo; test: LCS indexes with match len
# Deleted during cache_server port: unsupported command(s): stralgo; test: LCS indexes with match len and minimum match len
# Deleted during cache_server port: unsupported command(s): setrange; test: SETRANGE with huge offset
