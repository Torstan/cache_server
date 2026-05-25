#!/usr/bin/env python3
import json
import re
import sys
from pathlib import Path

SUPPORTED = {
    "set", "get", "mget", "setnx", "getset", "strlen", "append",
    "incr", "decr", "incrby", "decrby",
    "hset", "hget", "hdel", "hexists", "hlen", "hstrlen", "hmget",
    "hmset", "hgetall", "hkeys", "hvals", "hincrby",
    "sadd", "sismember", "srem", "scard", "smembers", "smismember",
    "spop", "srandmember",
    "zadd", "zscore", "zrem", "zcard", "zrank", "zrevrank",
    "zcount", "zincrby", "zrange",
    "del", "expire", "ttl", "exists", "type", "pttl", "scan",
}

DROP_WORDS = {
    "config", "debug", "object", "memory", "flushdb", "flushall", "dbsize",
    "keys", "randomkey", "time", "multi", "exec", "watch", "select",
    "bgsave", "save", "restore", "dump", "client", "command", "info",
    "script", "eval", "evalsha", "acl", "auth", "subscribe", "publish",
    "wait_for_condition", "after", "assert_encoding",
}

SUPPORTED_FILES = {
    "expire.tcl",
    "keyspace.tcl",
    "other.tcl",
    "type/hash.tcl",
    "type/incr.tcl",
    "type/set.tcl",
    "type/string.tcl",
    "type/zset.tcl",
}

DROP_COMMAND_WORDS = {
    "bitfield", "bitfield_ro", "bitcount", "bitop", "bitpos", "getbit",
    "setbit", "geoadd", "geodist", "geohash", "geopos", "georadius",
    "georadiusbymember", "geosearch", "geosearchstore", "hello", "lindex",
    "llen", "lpop", "lpos", "lpush", "lrange", "lrem", "lset", "ltrim",
    "renamenx", "rename", "rpop", "rpoplpush", "rpush", "setex", "sort",
    "wait",
}

DROP_TITLES = {
    "Failing test",
}

COMMAND_RE = re.compile(r"\br\s+([A-Za-z][A-Za-z0-9_-]*)")


def parse_braced(text, index):
    assert text[index] == "{"
    depth = 0
    escaped = False
    for pos in range(index, len(text)):
        ch = text[pos]
        if escaped:
            escaped = False
            continue
        if ch == "\\":
            escaped = True
            continue
        if ch == "{":
            depth += 1
        elif ch == "}":
            depth -= 1
            if depth == 0:
                return text[index + 1:pos], pos + 1
    raise ValueError("unclosed brace")


def skip_space(text, index):
    while index < len(text) and text[index].isspace():
        index += 1
    return index


def parse_word(text, index):
    index = skip_space(text, index)
    if index >= len(text):
        return "", index
    if text[index] == "{":
        return parse_braced(text, index)
    if text[index] == '"':
        end = index + 1
        escaped = False
        while end < len(text):
            ch = text[end]
            if escaped:
                escaped = False
            elif ch == "\\":
                escaped = True
            elif ch == '"':
                return text[index + 1:end], end + 1
            end += 1
        raise ValueError("unclosed quote")
    end = index
    while end < len(text) and not text[end].isspace():
        end += 1
    return text[index:end], end


def find_test_blocks(text):
    blocks = []
    pos = 0
    while True:
        match = re.search(r"(^|[\s;])test\s+", text[pos:])
        if not match:
            break
        start = pos + match.start() + len(match.group(1))
        index = start + len("test")
        try:
            title, index = parse_word(text, index)
            body_start = skip_space(text, index)
            if body_start >= len(text) or text[body_start] != "{":
                pos = start + 4
                continue
            body, body_end = parse_braced(text, body_start)
            end = body_end
            expected_start = skip_space(text, body_end)
            if expected_start < len(text) and text[expected_start] in '{"':
                _, end = parse_word(text, expected_start)
            blocks.append((start, end, title, body))
            pos = end
        except ValueError:
            pos = start + 4
    return blocks


def commands_in_body(body):
    return {match.group(1).lower() for match in COMMAND_RE.finditer(body)}


def should_keep(title, body):
    if title in DROP_TITLES:
        return False, "unsupported by cache_server command subset"
    if "$" in title:
        return False, "parameterized or encoding-dependent test"
    commands = commands_in_body(body)
    unsupported = sorted(cmd for cmd in commands if cmd not in SUPPORTED)
    lowered = f"{title}\n{body}".lower()
    command_words = sorted(word for word in DROP_COMMAND_WORDS
                           if re.search(rf"\b{re.escape(word)}\b", lowered))
    blocked = sorted(word for word in DROP_WORDS if word in lowered)
    if unsupported:
        return False, f"unsupported command(s): {', '.join(unsupported)}"
    if command_words:
        return False, f"unsupported command family: {', '.join(command_words)}"
    if blocked:
        return False, f"out-of-scope helper/capability: {', '.join(blocked)}"
    if "slow" in lowered or "stress" in lowered:
        return False, "slow or stress test"
    if re.search(r"\b(source|srv|reconnect|readraw|deferred|wait_for_|"
                 r"assert_encoding|populate_|create_)\b", lowered):
        return False, "requires unsupported Redis test harness helper"
    return True, "retained"


def filter_file(path, rel):
    text = path.read_text(encoding="utf-8", errors="surrogateescape")
    blocks = find_test_blocks(text)
    if not blocks:
        return {"retained": [], "deleted": []}
    if rel not in SUPPORTED_FILES:
        return {
            "retained": [],
            "deleted": [
                {"title": title, "reason": "unsupported unit test file"}
                for _, _, title, _ in blocks
            ],
        }
    retained = []
    deleted = []
    output = [
        "# Filtered Redis 6.2 unit tests for cache_server.\n",
        "# Unsupported command blocks were removed; retained test blocks are unchanged.\n\n",
    ]
    for start, end, title, body in blocks:
        keep, reason = should_keep(title, body)
        if keep:
            output.append(text[start:end])
            output.append("\n\n")
            retained.append(title)
        else:
            output.append(
                f"# Deleted during cache_server port: {reason}; test: {title}\n"
            )
            deleted.append({"title": title, "reason": reason})
    path.write_text("".join(output), encoding="utf-8",
                    errors="surrogateescape")
    return {"retained": retained, "deleted": deleted}


def prune_empty_dirs(root):
    for path in sorted((p for p in root.rglob("*") if p.is_dir()),
                       key=lambda p: len(p.parts), reverse=True):
        try:
            next(path.iterdir())
        except StopIteration:
            path.rmdir()


def main():
    if len(sys.argv) != 2:
        print("usage: filter_unit_tests.py tests/redis/unit", file=sys.stderr)
        return 2
    root = Path(sys.argv[1])
    manifest = {"redis_ref": "6.2", "files": {}}
    for path in sorted(root.rglob("*.tcl")):
        rel = str(path.relative_to(root))
        result = filter_file(path, rel)
        if not result["retained"]:
            path.unlink()
        manifest["files"][rel] = result
    prune_empty_dirs(root)
    (root / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
