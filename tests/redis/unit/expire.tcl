# Filtered Redis 6.2 unit tests for cache_server.
# Unsupported command blocks were removed; retained test blocks are unchanged.

# Deleted during cache_server port: out-of-scope helper/capability: multi, time; test: EXPIRE - set timeouts multiple times
test {EXPIRE - It should be still possible to read 'x'} {
        r get x
    } {foobar}

# Deleted during cache_server port: out-of-scope helper/capability: after; test: EXPIRE - After 2.1 seconds the key should no longer be here
# Deleted during cache_server port: unsupported command(s): lpush, lrange; test: EXPIRE - write on expire should work
# Deleted during cache_server port: unsupported command(s): expireat; test: EXPIREAT - Check for EXPIRE alike behavior
# Deleted during cache_server port: unsupported command(s): setex; test: SETEX - Set + Expire combo operation. Check for TTL
# Deleted during cache_server port: unsupported command family: setex; test: SETEX - Check value
# Deleted during cache_server port: unsupported command(s): setex; test: SETEX - Overwrite old key
# Deleted during cache_server port: unsupported command family: setex, wait; test: SETEX - Wait for the key to expire
# Deleted during cache_server port: unsupported command(s): setex; test: SETEX - Wrong time parameter
# Deleted during cache_server port: unsupported command(s): persist; test: PERSIST can undo an EXPIRE
# Deleted during cache_server port: unsupported command(s): persist; test: PERSIST returns 0 against non existing or non volatile keys
# Deleted during cache_server port: unsupported command(s): setex; test: EXPIRE precision is now the millisecond
# Deleted during cache_server port: unsupported command(s): pexpire, pexpireat, psetex, time; test: PEXPIRE/PSETEX/PEXPIREAT can set sub-second expires
# Deleted during cache_server port: unsupported command(s): setex; test: TTL returns time to live in seconds
# Deleted during cache_server port: unsupported command(s): setex; test: PTTL returns time to live in milliseconds
# Deleted during cache_server port: unsupported command family: hello; test: TTL / PTTL return -1 if key has no expire
test {TTL / PTTL return -2 if key does not exit} {
        r del x
        list [r ttl x] [r pttl x]
    } {-2 -2}

# Deleted during cache_server port: unsupported command(s): dbsize, flushdb, psetex; test: Redis should actively expire keys incrementally
# Deleted during cache_server port: unsupported command(s): dbsize, debug, flushdb, psetex; test: Redis should lazy expire keys
# Deleted during cache_server port: unsupported command(s): debug, pexpire; test: EXPIRE should not resurrect keys (issue #1026)
# Deleted during cache_server port: unsupported command(s): flushdb, keys; test: 5 keys in, 5 keys out
test {EXPIRE with empty string as TTL should report an error} {
        r set foo bar
        catch {r expire foo ""} e
        set e
    } {*not an integer*}

test {SET with EX with big integer should report an error} {
        catch {r set foo bar EX 10000000000000000} e
        set e
    } {ERR invalid expire time in set}

test {SET with EX with smallest integer should report an error} {
        catch {r SET foo bar EX -9999999999999999} e
        set e
    } {ERR invalid expire time in set}

# Deleted during cache_server port: unsupported command(s): getex; test: GETEX with big integer should report an error
# Deleted during cache_server port: unsupported command(s): getex; test: GETEX with smallest integer should report an error
# Deleted during cache_server port: out-of-scope helper/capability: time; test: EXPIRE with big integer overflows when converted to milliseconds
# Deleted during cache_server port: unsupported command(s): pexpire; test: PEXPIRE with big integer overflow when basetime is added
# Deleted during cache_server port: out-of-scope helper/capability: time; test: EXPIRE with big negative integer
# Deleted during cache_server port: unsupported command(s): pexpireat; test: PEXPIREAT with big integer works
# Deleted during cache_server port: unsupported command(s): pexpireat; test: PEXPIREAT with big negative integer works
# Deleted during cache_server port: unsupported command(s): config, debug, getex, pexpire, psetex, setex; test: EXPIRE and SET/GETEX EX/PX/EXAT/PXAT option, TTL should not be reset after loadaof
# Deleted during cache_server port: unsupported command(s): expireat, getex, pexpire, pexpireat, psetex, setex; test: EXPIRE relative and absolute propagation to replicas
# Deleted during cache_server port: out-of-scope helper/capability: command; test: SET command will remove expire
test {SET - use KEEPTTL option, TTL should not be removed} {
        r set foo bar EX 100
        r set foo bar KEEPTTL
        set ttl [r ttl foo]
        assert {$ttl <= 100 && $ttl > 90}
    }

# Deleted during cache_server port: unsupported command(s): config, debug; test: SET - use KEEPTTL option, TTL should not be removed after loadaof
# Deleted during cache_server port: unsupported command(s): getex; test: GETEX use of PERSIST option should remove TTL
# Deleted during cache_server port: unsupported command(s): debug, getex; test: GETEX use of PERSIST option should remove TTL after loadaof
# Deleted during cache_server port: unsupported command(s): getex; test: GETEX propagate as to replica as PERSIST, DEL, or nothing
