# Filtered Redis 6.2 unit tests for cache_server.
# Unsupported command blocks were removed; retained test blocks are unchanged.

test {DEL against a single item} {
        r set x foo
        assert {[r get x] eq "foo"}
        r del x
        r get x
    } {}

test {Vararg DEL} {
        r set foo1 a
        r set foo2 b
        r set foo3 c
        list [r del foo1 foo2 foo3 foo4] [r mget foo1 foo2 foo3]
    } {3 {{} {} {}}}

# Deleted during cache_server port: unsupported command(s): keys; test: KEYS with pattern
# Deleted during cache_server port: unsupported command(s): keys; test: KEYS to get all keys
# Deleted during cache_server port: unsupported command(s): dbsize; test: DBSIZE
# Deleted during cache_server port: unsupported command(s): dbsize, keys; test: DEL all keys
# Deleted during cache_server port: unsupported command(s): debug, setex; test: DEL against expired key
test {EXISTS} {
        set res {}
        r set newkey test
        append res [r exists newkey]
        r del newkey
        append res [r exists newkey]
    } {10}

test {Zero length value in key. SET/GET/EXISTS} {
        r set emptykey {}
        set res [r get emptykey]
        append res [r exists emptykey]
        r del emptykey
        append res [r exists emptykey]
    } {10}

# Deleted during cache_server port: unsupported command(s): channel, read; test: Commands pipelining
# Deleted during cache_server port: unsupported command(s): foobaredcommand; test: Non existing command
# Deleted during cache_server port: unsupported command(s): rename; test: RENAME basic usage
# Deleted during cache_server port: unsupported command family: rename; test: RENAME source key should no longer exist
# Deleted during cache_server port: unsupported command(s): rename; test: RENAME against already existing key
# Deleted during cache_server port: unsupported command(s): renamenx; test: RENAMENX basic usage
# Deleted during cache_server port: unsupported command(s): renamenx; test: RENAMENX against already existing key
# Deleted during cache_server port: unsupported command family: renamenx; test: RENAMENX against already existing key (2)
# Deleted during cache_server port: unsupported command(s): rename; test: RENAME against non existing source key
# Deleted during cache_server port: unsupported command(s): rename; test: RENAME where source and dest key are the same (existing)
# Deleted during cache_server port: unsupported command(s): renamenx; test: RENAMENX where source and dest key are the same (existing)
# Deleted during cache_server port: unsupported command(s): rename; test: RENAME where source and dest key are the same (non existing)
# Deleted during cache_server port: unsupported command(s): rename; test: RENAME with volatile key, should move the TTL as well
# Deleted during cache_server port: unsupported command(s): rename; test: RENAME with volatile key, should not inherit TTL of target key
# Deleted during cache_server port: unsupported command(s): dbsize, keys; test: DEL all keys again (DB 0)
# Deleted during cache_server port: unsupported command(s): dbsize, keys, select; test: DEL all keys again (DB 1)
# Deleted during cache_server port: unsupported command(s): copy, dbsize, select; test: COPY basic usage for string
# Deleted during cache_server port: unsupported command(s): copy; test: COPY for string does not replace an existing key without REPLACE option
# Deleted during cache_server port: unsupported command(s): copy, select; test: COPY for string can replace an existing key with REPLACE option
# Deleted during cache_server port: unsupported command(s): copy, flushdb, select; test: COPY for string ensures that copied data is independent of copying data
# Deleted during cache_server port: unsupported command(s): copy; test: COPY for string does not copy data to no-integer DB
# Deleted during cache_server port: unsupported command(s): copy; test: COPY can copy key expire metadata as well
# Deleted during cache_server port: unsupported command(s): copy; test: COPY does not create an expire if it does not exist
# Deleted during cache_server port: unsupported command(s): copy, debug, lpush, object; test: COPY basic usage for list
# Deleted during cache_server port: unsupported command(s): copy, debug, object; test: COPY basic usage for intset set
# Deleted during cache_server port: unsupported command(s): copy, debug, object; test: COPY basic usage for hashtable set
# Deleted during cache_server port: unsupported command(s): copy, debug, object; test: COPY basic usage for ziplist sorted set
# Deleted during cache_server port: unsupported command(s): config, copy, debug, object; test: COPY basic usage for skiplist sorted set
# Deleted during cache_server port: unsupported command(s): copy, debug, object; test: COPY basic usage for ziplist hash
# Deleted during cache_server port: unsupported command(s): config, copy, debug, object; test: COPY basic usage for hashtable hash
# Deleted during cache_server port: unsupported command(s): copy, debug, object, xadd; test: COPY basic usage for stream
# Deleted during cache_server port: unsupported command(s): copy, flushdb, object, xadd, xdel, xgroup, xinfo, xreadgroup; test: COPY basic usage for stream-cgroups
# Deleted during cache_server port: unsupported command(s): dbsize, move, select; test: MOVE basic usage
# Deleted during cache_server port: unsupported command(s): move; test: MOVE against key existing in the target DB
# Deleted during cache_server port: unsupported command(s): move; test: MOVE against non-integer DB (#1428)
# Deleted during cache_server port: unsupported command(s): flushdb, move, select; test: MOVE can move key expire metadata as well
# Deleted during cache_server port: unsupported command(s): flushdb, move, select; test: MOVE does not create an expire if it does not exist
# Deleted during cache_server port: unsupported command(s): select; test: SET/GET keys in different DBs
# Deleted during cache_server port: unsupported command(s): flushdb, randomkey; test: RANDOMKEY
# Deleted during cache_server port: unsupported command(s): flushdb, randomkey; test: RANDOMKEY against empty DB
# Deleted during cache_server port: unsupported command(s): flushdb, randomkey; test: RANDOMKEY regression 1
# Deleted during cache_server port: unsupported command(s): flushdb, keys; test: KEYS * two times with long key, Github issue #1208
# Deleted during cache_server port: unsupported command(s): flushdb, keys; test: Regression for pattern matching long nested loops
# Deleted during cache_server port: unsupported command(s): flushdb, keys; test: Regression for pattern matching very long nested loops
