# Filtered Redis 6.2 unit tests for cache_server.
# Unsupported command blocks were removed; retained test blocks are unchanged.

# Deleted during cache_server port: out-of-scope helper/capability: assert_encoding; test: SADD, SCARD, SISMEMBER, SMISMEMBER, SMEMBERS basics - regular set
# Deleted during cache_server port: out-of-scope helper/capability: assert_encoding; test: SADD, SCARD, SISMEMBER, SMISMEMBER, SMEMBERS basics - intset
# Deleted during cache_server port: unsupported command(s): lpush; test: SMISMEMBER against non set
test {SMISMEMBER non existing key} {
        assert_equal {0} [r smismember myset1 foo]
        assert_equal {0 0} [r smismember myset1 foo bar]
    }

test {SMISMEMBER requires one or more members} {
        r del zmscoretest
        r zadd zmscoretest 10 x
        r zadd zmscoretest 20 y
        
        catch {r smismember zmscoretest} e
        assert_match {*ERR*wrong*number*arg*} $e
    }

# Deleted during cache_server port: unsupported command(s): lpush; test: SADD against non set
# Deleted during cache_server port: out-of-scope helper/capability: assert_encoding; test: SADD a non-integer against an intset
# Deleted during cache_server port: out-of-scope helper/capability: assert_encoding; test: SADD an integer larger than 64 bits
# Deleted during cache_server port: out-of-scope helper/capability: assert_encoding; test: SADD overflows the maximum allowed integers in an intset
test {Variadic SADD} {
        r del myset
        assert_equal 3 [r sadd myset a b c]
        assert_equal 2 [r sadd myset A a b c B]
        assert_equal [lsort {A a b c B}] [lsort [r smembers myset]]
    }

# Deleted during cache_server port: unsupported command(s): debug; test: Set encoding after DEBUG RELOAD
# Deleted during cache_server port: out-of-scope helper/capability: assert_encoding; test: SREM basics - regular set
# Deleted during cache_server port: out-of-scope helper/capability: assert_encoding; test: SREM basics - intset
# Deleted during cache_server port: out-of-scope helper/capability: multi; test: SREM with multiple arguments
test {SREM variadic version with more args needed to destroy the key} {
        r del myset
        r sadd myset 1 2 3
        r srem myset 1 2 3 4 5 6 7 8
    } {3}

# Deleted during cache_server port: parameterized or encoding-dependent test; test: Generated sets must be encoded as $type
# Deleted during cache_server port: parameterized or encoding-dependent test; test: SINTER with two sets - $type
# Deleted during cache_server port: parameterized or encoding-dependent test; test: SINTERSTORE with two sets - $type
# Deleted during cache_server port: parameterized or encoding-dependent test; test: SINTERSTORE with two sets, after a DEBUG RELOAD - $type
# Deleted during cache_server port: parameterized or encoding-dependent test; test: SUNION with two sets - $type
# Deleted during cache_server port: parameterized or encoding-dependent test; test: SUNIONSTORE with two sets - $type
# Deleted during cache_server port: parameterized or encoding-dependent test; test: SINTER against three sets - $type
# Deleted during cache_server port: parameterized or encoding-dependent test; test: SINTERSTORE with three sets - $type
# Deleted during cache_server port: parameterized or encoding-dependent test; test: SUNION with non existing keys - $type
# Deleted during cache_server port: parameterized or encoding-dependent test; test: SDIFF with two sets - $type
# Deleted during cache_server port: parameterized or encoding-dependent test; test: SDIFF with three sets - $type
# Deleted during cache_server port: parameterized or encoding-dependent test; test: SDIFFSTORE with three sets - $type
# Deleted during cache_server port: unsupported command(s): sdiff; test: SDIFF with first set empty
# Deleted during cache_server port: unsupported command(s): sdiff; test: SDIFF with same set two times
# Deleted during cache_server port: unsupported command(s): sdiff; test: SDIFF fuzzing
# Deleted during cache_server port: unsupported command(s): sdiff; test: SDIFF against non-set should throw error
# Deleted during cache_server port: unsupported command(s): sdiff; test: SDIFF should handle non existing key as empty
# Deleted during cache_server port: unsupported command(s): sdiffstore; test: SDIFFSTORE against non-set should throw error
# Deleted during cache_server port: unsupported command(s): sdiffstore; test: SDIFFSTORE should handle non existing key as empty
# Deleted during cache_server port: unsupported command(s): sinter; test: SINTER against non-set should throw error
# Deleted during cache_server port: unsupported command(s): sinter; test: SINTER should handle non existing key as empty
# Deleted during cache_server port: unsupported command(s): sinter; test: SINTER with same integer elements but different encoding
# Deleted during cache_server port: unsupported command(s): sinterstore; test: SINTERSTORE against non-set should throw error
# Deleted during cache_server port: unsupported command(s): sinterstore; test: SINTERSTORE against non existing keys should delete dstkey
# Deleted during cache_server port: unsupported command(s): sunion; test: SUNION against non-set should throw error
# Deleted during cache_server port: unsupported command(s): sunion; test: SUNION should handle non existing key as empty
# Deleted during cache_server port: unsupported command(s): sunionstore; test: SUNIONSTORE against non-set should throw error
# Deleted during cache_server port: unsupported command(s): sunionstore; test: SUNIONSTORE should handle non existing key as empty
# Deleted during cache_server port: unsupported command(s): sunionstore; test: SUNIONSTORE against non existing keys should delete dstkey
# Deleted during cache_server port: parameterized or encoding-dependent test; test: SPOP basics - $type
# Deleted during cache_server port: parameterized or encoding-dependent test; test: SPOP with <count>=1 - $type
# Deleted during cache_server port: parameterized or encoding-dependent test; test: SRANDMEMBER - $type
# Deleted during cache_server port: out-of-scope helper/capability: assert_encoding; test: SPOP with <count>
# Deleted during cache_server port: out-of-scope helper/capability: assert_encoding; test: SPOP using integers, testing Knuth's and Floyd's algorithm
test "SPOP using integers with Knuth's algorithm" {
        r spop nonexisting_key 100
    } {}

test "SPOP new implementation: code path #1" {
        set content {1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20}
        create_set myset $content
        set res [r spop myset 30]
        assert {[lsort $content] eq [lsort $res]}
    }

test "SPOP new implementation: code path #2" {
        set content {1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20}
        create_set myset $content
        set res [r spop myset 2]
        assert {[llength $res] == 2}
        assert {[r scard myset] == 18}
        set union [concat [r smembers myset] $res]
        assert {[lsort $union] eq [lsort $content]}
    }

test "SPOP new implementation: code path #3" {
        set content {1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20}
        create_set myset $content
        set res [r spop myset 18]
        assert {[llength $res] == 18}
        assert {[r scard myset] == 2}
        set union [concat [r smembers myset] $res]
        assert {[lsort $union] eq [lsort $content]}
    }

test "SRANDMEMBER count of 0 is handled correctly" {
        r srandmember myset 0
    } {}

test "SRANDMEMBER with <count> against non existing key" {
        r srandmember nonexisting_key 100
    } {}

test "SRANDMEMBER count overflow" {
        r sadd myset a
        assert_error {*value is out of range*} {r srandmember myset -9223372036854775808}
    } {}

test "SRANDMEMBER count of 0 is handled correctly - emptyarray" {
        r srandmember myset 0
    } {*0}

test "SRANDMEMBER with <count> against non existing key - emptyarray" {
        r srandmember nonexisting_key 100
    } {*0}

# Deleted during cache_server port: parameterized or encoding-dependent test; test: SRANDMEMBER with <count> - $type
# Deleted during cache_server port: parameterized or encoding-dependent test; test: SRANDMEMBER histogram distribution - $type
# Deleted during cache_server port: unsupported command(s): bgsave, config, debug, r, save; test: SRANDMEMBER with a dict containing long chain
# Deleted during cache_server port: unsupported command(s): smove; test: SMOVE basics - from regular set to intset
# Deleted during cache_server port: unsupported command(s): smove; test: SMOVE basics - from intset to regular set
# Deleted during cache_server port: unsupported command(s): smove; test: SMOVE non existing key
# Deleted during cache_server port: unsupported command(s): smove; test: SMOVE non existing src set
# Deleted during cache_server port: unsupported command(s): smove; test: SMOVE from regular set to non existing destination set
# Deleted during cache_server port: unsupported command(s): smove; test: SMOVE from intset to non existing destination set
# Deleted during cache_server port: unsupported command(s): smove; test: SMOVE wrong src key type
# Deleted during cache_server port: unsupported command(s): smove; test: SMOVE wrong dst key type
# Deleted during cache_server port: unsupported command(s): smove; test: SMOVE with identical source and destination
# Deleted during cache_server port: unsupported command(s): exec, multi, watch; test: SMOVE only notify dstset when the addition is successful
# Deleted during cache_server port: out-of-scope helper/capability: info; test: intsets implementation stress testing
