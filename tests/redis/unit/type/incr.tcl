# Filtered Redis 6.2 unit tests for cache_server.
# Unsupported command blocks were removed; retained test blocks are unchanged.

test {INCR against non existing key} {
        set res {}
        append res [r incr novar]
        append res [r get novar]
    } {11}

test {INCR against key created by incr itself} {
        r incr novar
    } {2}

test {INCR against key originally set with SET} {
        r set novar 100
        r incr novar
    } {101}

test {INCR over 32bit value} {
        r set novar 17179869184
        r incr novar
    } {17179869185}

test {INCRBY over 32bit value with over 32bit increment} {
        r set novar 17179869184
        r incrby novar 17179869184
    } {34359738368}

test {INCR fails against key with spaces (left)} {
        r set novar "    11"
        catch {r incr novar} err
        format $err
    } {ERR*}

test {INCR fails against key with spaces (right)} {
        r set novar "11    "
        catch {r incr novar} err
        format $err
    } {ERR*}

test {INCR fails against key with spaces (both)} {
        r set novar "    11    "
        catch {r incr novar} err
        format $err
    } {ERR*}

# Deleted during cache_server port: unsupported command(s): rpop, rpush; test: INCR fails against a key holding a list
test {DECRBY over 32bit value with over 32bit increment, negative res} {
        r set novar 17179869184
        r decrby novar 17179869185
    } {-1}

# Deleted during cache_server port: unsupported command(s): object; test: INCR uses shared objects in the 0-9999 range
# Deleted during cache_server port: unsupported command(s): debug, object; test: INCR can modify objects in-place
# Deleted during cache_server port: unsupported command(s): incrbyfloat; test: INCRBYFLOAT against non existing key
# Deleted during cache_server port: unsupported command(s): incrbyfloat; test: INCRBYFLOAT against key originally set with SET
# Deleted during cache_server port: unsupported command(s): incrbyfloat; test: INCRBYFLOAT over 32bit value
# Deleted during cache_server port: unsupported command(s): incrbyfloat; test: INCRBYFLOAT over 32bit value with over 32bit increment
# Deleted during cache_server port: unsupported command(s): incrbyfloat; test: INCRBYFLOAT fails against key with spaces (left)
# Deleted during cache_server port: unsupported command(s): incrbyfloat; test: INCRBYFLOAT fails against key with spaces (right)
# Deleted during cache_server port: unsupported command(s): incrbyfloat; test: INCRBYFLOAT fails against key with spaces (both)
# Deleted during cache_server port: unsupported command(s): incrbyfloat, rpush; test: INCRBYFLOAT fails against a key holding a list
# Deleted during cache_server port: unsupported command(s): incrbyfloat; test: INCRBYFLOAT does not allow NaN or Infinity
# Deleted during cache_server port: unsupported command(s): incrbyfloat; test: INCRBYFLOAT decrement
# Deleted during cache_server port: unsupported command(s): incrbyfloat, setrange; test: string to double with null terminator
# Deleted during cache_server port: unsupported command(s): incrbyfloat; test: No negative zero
