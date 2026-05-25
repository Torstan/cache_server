# Filtered Redis 6.2 unit tests for cache_server.
# Unsupported command blocks were removed; retained test blocks are unchanged.

# Deleted during cache_server port: parameterized or encoding-dependent test; test: Check encoding - $encoding
# Deleted during cache_server port: parameterized or encoding-dependent test; test: ZSET basic ZADD and score update - $encoding
# Deleted during cache_server port: parameterized or encoding-dependent test; test: ZSET element can't be set to NaN with ZADD - $encoding
# Deleted during cache_server port: parameterized or encoding-dependent test; test: ZSET element can't be set to NaN with ZINCRBY - $encoding
# Deleted during cache_server port: parameterized or encoding-dependent test; test: ZADD with options syntax error with incomplete pair - $encoding
# Deleted during cache_server port: parameterized or encoding-dependent test; test: ZADD XX option without key - $encoding
# Deleted during cache_server port: parameterized or encoding-dependent test; test: ZADD XX existing key - $encoding
# Deleted during cache_server port: parameterized or encoding-dependent test; test: ZADD XX returns the number of elements actually added - $encoding
# Deleted during cache_server port: parameterized or encoding-dependent test; test: ZADD XX updates existing elements score - $encoding
# Deleted during cache_server port: parameterized or encoding-dependent test; test: ZADD GT updates existing elements when new scores are greater - $encoding
# Deleted during cache_server port: parameterized or encoding-dependent test; test: ZADD LT updates existing elements when new scores are lower - $encoding
# Deleted during cache_server port: parameterized or encoding-dependent test; test: ZADD GT XX updates existing elements when new scores are greater and skips new elements - $encoding
# Deleted during cache_server port: parameterized or encoding-dependent test; test: ZADD LT XX updates existing elements when new scores are lower and skips new elements - $encoding
# Deleted during cache_server port: parameterized or encoding-dependent test; test: ZADD XX and NX are not compatible - $encoding
# Deleted during cache_server port: parameterized or encoding-dependent test; test: ZADD NX with non existing key - $encoding
# Deleted during cache_server port: parameterized or encoding-dependent test; test: ZADD NX only add new elements without updating old ones - $encoding
# Deleted during cache_server port: parameterized or encoding-dependent test; test: ZADD GT and NX are not compatible - $encoding
# Deleted during cache_server port: parameterized or encoding-dependent test; test: ZADD LT and NX are not compatible - $encoding
# Deleted during cache_server port: parameterized or encoding-dependent test; test: ZADD LT and GT are not compatible - $encoding
# Deleted during cache_server port: parameterized or encoding-dependent test; test: ZADD INCR LT/GT replies with nill if score not updated - $encoding
# Deleted during cache_server port: parameterized or encoding-dependent test; test: ZADD INCR LT/GT with inf - $encoding
# Deleted during cache_server port: parameterized or encoding-dependent test; test: ZADD INCR works like ZINCRBY - $encoding
# Deleted during cache_server port: parameterized or encoding-dependent test; test: ZADD INCR works with a single score-elemenet pair - $encoding
# Deleted during cache_server port: parameterized or encoding-dependent test; test: ZADD CH option changes return value to all changed elements - $encoding
# Deleted during cache_server port: parameterized or encoding-dependent test; test: ZINCRBY calls leading to NaN result in error - $encoding
# Deleted during cache_server port: parameterized or encoding-dependent test; test: ZADD - Variadic version base case - $encoding
# Deleted during cache_server port: parameterized or encoding-dependent test; test: ZADD - Return value is the number of actually added items - $encoding
# Deleted during cache_server port: parameterized or encoding-dependent test; test: ZADD - Variadic version does not add nothing on single parsing err - $encoding
# Deleted during cache_server port: parameterized or encoding-dependent test; test: ZADD - Variadic version will raise error on missing arg - $encoding
# Deleted during cache_server port: parameterized or encoding-dependent test; test: ZINCRBY does not work variadic even if shares ZADD implementation - $encoding
# Deleted during cache_server port: parameterized or encoding-dependent test; test: ZCARD basics - $encoding
# Deleted during cache_server port: parameterized or encoding-dependent test; test: ZREM removes key after last element is removed - $encoding
# Deleted during cache_server port: parameterized or encoding-dependent test; test: ZREM variadic version - $encoding
# Deleted during cache_server port: parameterized or encoding-dependent test; test: ZREM variadic version -- remove elements after key deletion - $encoding
# Deleted during cache_server port: parameterized or encoding-dependent test; test: ZRANGE basics - $encoding
# Deleted during cache_server port: parameterized or encoding-dependent test; test: ZREVRANGE basics - $encoding
# Deleted during cache_server port: parameterized or encoding-dependent test; test: ZRANK/ZREVRANK basics - $encoding
# Deleted during cache_server port: parameterized or encoding-dependent test; test: ZRANK - after deletion - $encoding
# Deleted during cache_server port: parameterized or encoding-dependent test; test: ZINCRBY - can create a new sorted set - $encoding
# Deleted during cache_server port: parameterized or encoding-dependent test; test: ZINCRBY - increment and decrement - $encoding
# Deleted during cache_server port: parameterized or encoding-dependent test; test: ZINCRBY return value - $encoding
# Deleted during cache_server port: parameterized or encoding-dependent test; test: ZRANGEBYSCORE/ZREVRANGEBYSCORE/ZCOUNT basics - $encoding
# Deleted during cache_server port: parameterized or encoding-dependent test; test: ZRANGEBYSCORE with WITHSCORES - $encoding
# Deleted during cache_server port: parameterized or encoding-dependent test; test: ZRANGEBYSCORE with LIMIT - $encoding
# Deleted during cache_server port: parameterized or encoding-dependent test; test: ZRANGEBYSCORE with LIMIT and WITHSCORES - $encoding
# Deleted during cache_server port: parameterized or encoding-dependent test; test: ZRANGEBYSCORE with non-value min or max - $encoding
# Deleted during cache_server port: parameterized or encoding-dependent test; test: ZRANGEBYLEX/ZREVRANGEBYLEX/ZLEXCOUNT basics - $encoding
# Deleted during cache_server port: parameterized or encoding-dependent test; test: ZLEXCOUNT advanced - $encoding
# Deleted during cache_server port: parameterized or encoding-dependent test; test: ZRANGEBYSLEX with LIMIT - $encoding
# Deleted during cache_server port: parameterized or encoding-dependent test; test: ZRANGEBYLEX with invalid lex range specifiers - $encoding
# Deleted during cache_server port: parameterized or encoding-dependent test; test: ZREMRANGEBYSCORE basics - $encoding
# Deleted during cache_server port: parameterized or encoding-dependent test; test: ZREMRANGEBYSCORE with non-value min or max - $encoding
# Deleted during cache_server port: parameterized or encoding-dependent test; test: ZREMRANGEBYRANK basics - $encoding
# Deleted during cache_server port: parameterized or encoding-dependent test; test: ZUNIONSTORE against non-existing key doesn't set destination - $encoding
# Deleted during cache_server port: parameterized or encoding-dependent test; test: ZUNION/ZINTER/ZDIFF against non-existing key - $encoding
# Deleted during cache_server port: parameterized or encoding-dependent test; test: ZUNIONSTORE with empty set - $encoding
# Deleted during cache_server port: parameterized or encoding-dependent test; test: ZUNION/ZINTER/ZDIFF with empty set - $encoding
# Deleted during cache_server port: parameterized or encoding-dependent test; test: ZUNIONSTORE basics - $encoding
# Deleted during cache_server port: parameterized or encoding-dependent test; test: ZUNION/ZINTER/ZDIFF with integer members - $encoding
# Deleted during cache_server port: parameterized or encoding-dependent test; test: ZUNIONSTORE with weights - $encoding
# Deleted during cache_server port: parameterized or encoding-dependent test; test: ZUNION with weights - $encoding
# Deleted during cache_server port: parameterized or encoding-dependent test; test: ZUNIONSTORE with a regular set and weights - $encoding
# Deleted during cache_server port: parameterized or encoding-dependent test; test: ZUNIONSTORE with AGGREGATE MIN - $encoding
# Deleted during cache_server port: parameterized or encoding-dependent test; test: ZUNION/ZINTER with AGGREGATE MIN - $encoding
# Deleted during cache_server port: parameterized or encoding-dependent test; test: ZUNIONSTORE with AGGREGATE MAX - $encoding
# Deleted during cache_server port: parameterized or encoding-dependent test; test: ZUNION/ZINTER with AGGREGATE MAX - $encoding
# Deleted during cache_server port: parameterized or encoding-dependent test; test: ZINTERSTORE basics - $encoding
# Deleted during cache_server port: parameterized or encoding-dependent test; test: ZINTER basics - $encoding
# Deleted during cache_server port: parameterized or encoding-dependent test; test: ZINTER RESP3 - $encoding
# Deleted during cache_server port: parameterized or encoding-dependent test; test: ZINTERSTORE with weights - $encoding
# Deleted during cache_server port: parameterized or encoding-dependent test; test: ZINTER with weights - $encoding
# Deleted during cache_server port: parameterized or encoding-dependent test; test: ZINTERSTORE with a regular set and weights - $encoding
# Deleted during cache_server port: parameterized or encoding-dependent test; test: ZINTERSTORE with AGGREGATE MIN - $encoding
# Deleted during cache_server port: parameterized or encoding-dependent test; test: ZINTERSTORE with AGGREGATE MAX - $encoding
# Deleted during cache_server port: parameterized or encoding-dependent test; test: $cmd with +inf/-inf scores - $encoding
# Deleted during cache_server port: parameterized or encoding-dependent test; test: $cmd with NaN weights - $encoding
# Deleted during cache_server port: parameterized or encoding-dependent test; test: ZDIFFSTORE basics - $encoding
# Deleted during cache_server port: parameterized or encoding-dependent test; test: ZDIFF basics - $encoding
# Deleted during cache_server port: parameterized or encoding-dependent test; test: ZDIFFSTORE with a regular set - $encoding
# Deleted during cache_server port: parameterized or encoding-dependent test; test: ZDIFF subtracting set from itself - $encoding
# Deleted during cache_server port: parameterized or encoding-dependent test; test: ZDIFF algorithm 1 - $encoding
# Deleted during cache_server port: parameterized or encoding-dependent test; test: ZDIFF algorithm 2 - $encoding
# Deleted during cache_server port: parameterized or encoding-dependent test; test: ZDIFF fuzzing - $encoding
# Deleted during cache_server port: parameterized or encoding-dependent test; test: Basic ZPOP with a single key - $encoding
# Deleted during cache_server port: parameterized or encoding-dependent test; test: ZPOP with count - $encoding
# Deleted during cache_server port: parameterized or encoding-dependent test; test: BZPOP with a single existing sorted set - $encoding
# Deleted during cache_server port: parameterized or encoding-dependent test; test: BZPOP with multiple existing sorted sets - $encoding
# Deleted during cache_server port: parameterized or encoding-dependent test; test: BZPOP second sorted set has members - $encoding
# Deleted during cache_server port: parameterized or encoding-dependent test; test: Basic ZPOP - $encoding RESP3
# Deleted during cache_server port: parameterized or encoding-dependent test; test: ZPOP with count - $encoding RESP3
# Deleted during cache_server port: parameterized or encoding-dependent test; test: BZPOP - $encoding RESP3
# Deleted during cache_server port: unsupported command(s): zinterstore; test: ZINTERSTORE regression with two sets, intset+hashtable
# Deleted during cache_server port: unsupported command(s): zunionstore; test: ZUNIONSTORE regression, should not create NaN in scores
# Deleted during cache_server port: unsupported command(s): zinterstore; test: ZINTERSTORE #516 regression, mixed sets and ziplist zsets
# Deleted during cache_server port: unsupported command(s): zunionstore; test: ZUNIONSTORE result is sorted
# Deleted during cache_server port: unsupported command(s): zdiffstore, zinterstore, zunionstore; test: ZUNIONSTORE/ZINTERSTORE/ZDIFFSTORE error if using WITHSCORES 
# Deleted during cache_server port: unsupported command(s): zmscore; test: ZMSCORE retrieve
# Deleted during cache_server port: unsupported command(s): zmscore; test: ZMSCORE retrieve from empty set
# Deleted during cache_server port: unsupported command(s): zmscore; test: ZMSCORE retrieve with missing member
# Deleted during cache_server port: unsupported command(s): zmscore; test: ZMSCORE retrieve single member
# Deleted during cache_server port: unsupported command(s): zmscore; test: ZMSCORE retrieve requires one or more members
# Deleted during cache_server port: out-of-scope helper/capability: command; test: ZSET commands don't accept the empty strings as valid score
# Deleted during cache_server port: parameterized or encoding-dependent test; test: ZSCORE - $encoding
# Deleted during cache_server port: parameterized or encoding-dependent test; test: ZMSCORE - $encoding
# Deleted during cache_server port: parameterized or encoding-dependent test; test: ZSCORE after a DEBUG RELOAD - $encoding
# Deleted during cache_server port: parameterized or encoding-dependent test; test: ZSET sorting stresser - $encoding
# Deleted during cache_server port: parameterized or encoding-dependent test; test: ZRANGEBYSCORE fuzzy test, 100 ranges in $elements element sorted set - $encoding
# Deleted during cache_server port: parameterized or encoding-dependent test; test: ZRANGEBYLEX fuzzy test, 100 ranges in $elements element sorted set - $encoding
# Deleted during cache_server port: parameterized or encoding-dependent test; test: ZREMRANGEBYLEX fuzzy test, 100 ranges in $elements element sorted set - $encoding
# Deleted during cache_server port: parameterized or encoding-dependent test; test: ZSETs skiplist implementation backlink consistency test - $encoding
# Deleted during cache_server port: parameterized or encoding-dependent test; test: ZSETs ZRANK augmented skip list stress testing - $encoding
# Deleted during cache_server port: unsupported command(s): exec, multi; test: BZPOPMIN, ZADD + DEL should not awake blocked client
# Deleted during cache_server port: unsupported command(s): exec, multi; test: BZPOPMIN, ZADD + DEL + SET should not awake blocked client
# Deleted during cache_server port: out-of-scope helper/capability: after, client, multi, time; test: BZPOPMIN with same key multiple times should work
# Deleted during cache_server port: unsupported command(s): exec, multi; test: MULTI/EXEC is isolated from the point of view of BZPOPMIN
# Deleted during cache_server port: out-of-scope helper/capability: after, client; test: BZPOPMIN with variadic ZADD
# Deleted during cache_server port: out-of-scope helper/capability: after, client, time; test: BZPOPMIN with zero timeout should block indefinitely
# Deleted during cache_server port: unsupported command(s): config; test: ZSET skiplist order consistency when elements are moved
# Deleted during cache_server port: unsupported command(s): flushall, zrangestore; test: ZRANGESTORE basic
# Deleted during cache_server port: unsupported command(s): hello; test: ZRANGESTORE RESP3
# Deleted during cache_server port: unsupported command(s): zrangestore; test: ZRANGESTORE range
# Deleted during cache_server port: unsupported command(s): zrangestore; test: ZRANGESTORE BYLEX
# Deleted during cache_server port: unsupported command(s): zrangestore; test: ZRANGESTORE BYSCORE
# Deleted during cache_server port: unsupported command(s): zrangestore; test: ZRANGESTORE BYSCORE LIMIT
# Deleted during cache_server port: unsupported command(s): zrangestore; test: ZRANGESTORE BYSCORE REV LIMIT
test {ZRANGE BYSCORE REV LIMIT} {
        r zrange z1 5 0 BYSCORE REV LIMIT 0 2 WITHSCORES
    } {d 4 c 3}

# Deleted during cache_server port: unsupported command(s): zrangestore; test: ZRANGESTORE - src key missing
# Deleted during cache_server port: unsupported command(s): zrangestore; test: ZRANGESTORE - src key wrong type
# Deleted during cache_server port: unsupported command(s): zrangestore; test: ZRANGESTORE - empty range
# Deleted during cache_server port: unsupported command(s): zrangestore; test: ZRANGESTORE BYLEX - empty range
# Deleted during cache_server port: unsupported command(s): zrangestore; test: ZRANGESTORE BYSCORE - empty range
test {ZRANGE BYLEX} {
        r zrange z1 \[b \[c BYLEX
    } {b c}

# Deleted during cache_server port: unsupported command(s): zrangestore; test: ZRANGESTORE invalid syntax
# Deleted during cache_server port: unsupported command(s): config, zrangestore; test: ZRANGESTORE with zset-max-ziplist-entries 0 #10767 case
# Deleted during cache_server port: unsupported command(s): config, zrangestore; test: ZRANGESTORE with zset-max-ziplist-entries 1 dst key should use skiplist encoding
# Deleted during cache_server port: unsupported command(s): zrangebyscore, zrevrange; test: ZRANGE invalid syntax
# Deleted during cache_server port: parameterized or encoding-dependent test; test: ZRANDMEMBER - $type
# Deleted during cache_server port: unsupported command(s): hello, zrandmember; test: ZRANDMEMBER with RESP3
# Deleted during cache_server port: unsupported command(s): zrandmember; test: ZRANDMEMBER count of 0 is handled correctly
# Deleted during cache_server port: unsupported command(s): zrandmember; test: ZRANDMEMBER with <count> against non existing key
# Deleted during cache_server port: unsupported command(s): zrandmember; test: ZRANDMEMBER count overflow
# Deleted during cache_server port: unsupported command(s): zrandmember; test: ZRANDMEMBER count of 0 is handled correctly - emptyarray
# Deleted during cache_server port: unsupported command(s): zrandmember; test: ZRANDMEMBER with <count> against non existing key - emptyarray
# Deleted during cache_server port: parameterized or encoding-dependent test; test: ZRANDMEMBER with <count> - $type
