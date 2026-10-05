#!/usr/bin/env python3
"""Compare two cache_server instances and report key and data Jaccard scores."""

from __future__ import annotations

import argparse
from dataclasses import dataclass, field
from itertools import islice
import math
import socket
import time
from typing import BinaryIO, Callable, Iterator


VALUE_BATCH_SIZE = 16
RETRY_BATCH_SIZE = 256
MAX_PIPELINE_REQUEST_BYTES = 8192
READ_ONLY_COMMANDS = frozenset({
    b"SCAN", b"TYPE", b"GET", b"HGETALL", b"SMEMBERS", b"ZRANGE",
})


class ServerError(Exception):
    pass


class ReadRace(Exception):
    """A key changed between TYPE and its value read."""


def _line(reader: BinaryIO) -> bytes:
    line = reader.readline()
    if not line.endswith(b"\r\n"):
        raise ConnectionError("incomplete RESP line")
    return line[:-2]


def read_reply(reader: BinaryIO) -> bytes | int | None | list:
    prefix = reader.read(1)
    if prefix == b"+":
        return _line(reader)
    if prefix == b"-":
        raise ServerError(_line(reader).decode("utf-8", "replace"))
    if prefix == b":":
        return int(_line(reader))
    if prefix in (b"$", b"*"):
        length = int(_line(reader))
        if length == -1:
            return None
        if length < -1:
            raise ValueError("invalid RESP length")
        if prefix == b"*":
            return [read_reply(reader) for _ in range(length)]
        payload = reader.read(length + 2)
        if len(payload) != length + 2 or not payload.endswith(b"\r\n"):
            raise ConnectionError("incomplete RESP bulk string")
        return payload[:-2]
    raise ConnectionError("invalid or missing RESP prefix")


@dataclass(frozen=True)
class Entry:
    kind: bytes
    value: bytes | tuple


def _pairs(reply: list) -> tuple[tuple[bytes, bytes], ...]:
    if len(reply) % 2:
        raise ValueError("odd number of field/value elements")
    return tuple(sorted((reply[i], reply[i + 1]) for i in range(0, len(reply), 2)))


def _value_command(kind: bytes, key: bytes) -> tuple[bytes, ...]:
    commands = {
        b"string": (b"GET", key),
        b"hash": (b"HGETALL", key),
        b"set": (b"SMEMBERS", key),
        b"zset": (b"ZRANGE", key, b"0", b"-1", b"WITHSCORES"),
    }
    if kind not in commands:
        raise ValueError(f"unsupported type {kind!r} for key {key!r}")
    return commands[kind]


def _entry_from_replies(kind: bytes, value: object, after_kind: object,
                        key: bytes) -> Entry:
    if isinstance(value, ServerError):
        if str(value).startswith("WRONGTYPE"):
            raise ReadRace(f"type changed for key {key!r}") from value
        raise value
    if isinstance(after_kind, ServerError):
        raise after_kind
    if after_kind != kind:
        raise ReadRace(f"type changed for key {key!r}")
    if kind == b"hash" or kind == b"zset":
        value = _pairs(value)
    elif kind == b"set":
        value = tuple(sorted(value))
    elif value is None:
        raise ReadRace(f"key vanished during read: {key!r}")
    return Entry(kind, value)


class CacheServer:
    def __init__(self, address: str, scan_count: int, sync_timeout: float = 60.0):
        host, separator, port = address.rpartition(":")
        if not separator or not host or not port.isdecimal() or not 0 < int(port) < 65536:
            raise ValueError(f"expected host:port, got {address!r}")
        self.socket = socket.create_connection((host, int(port)), timeout=5)
        self.reader = self.socket.makefile("rb")
        self.scan_count = scan_count
        self.sync_timeout = sync_timeout

    def close(self) -> None:
        self.reader.close()
        self.socket.close()

    def command(self, *args: bytes) -> object:
        result = self.command_many([args])[0]
        if isinstance(result, ServerError):
            raise result
        return result

    def command_many(self, commands: list[tuple[bytes, ...]]) -> list[object]:
        pending = []
        pending_bytes = 0
        replies = []

        def flush() -> None:
            if not pending:
                return
            payload = b"".join(pending)
            retryable = all(args[0] in READ_ONLY_COMMANDS for args in pending_commands)
            deadline = None
            while True:
                self.socket.sendall(payload)
                batch_replies = []
                for _ in pending:
                    try:
                        batch_replies.append(read_reply(self.reader))
                    except ServerError as error:
                        batch_replies.append(error)
                syncing = retryable and any(
                    isinstance(reply, ServerError) and
                    str(reply).startswith("TRYAGAIN slot is syncing")
                    for reply in batch_replies
                )
                if not syncing:
                    replies.extend(batch_replies)
                    break
                if deadline is None:
                    deadline = time.monotonic() + self.sync_timeout
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    raise ServerError(
                        f"replica slot still syncing after {self.sync_timeout:g}s; "
                        "check replication or increase --sync-timeout"
                    )
                time.sleep(min(1.0, remaining))
            pending.clear()
            pending_commands.clear()

        pending_commands = []
        for args in commands:
            parts = [f"*{len(args)}\r\n".encode()]
            for arg in args:
                parts.extend((f"${len(arg)}\r\n".encode(), arg, b"\r\n"))
            encoded = b"".join(parts)
            if pending and pending_bytes + len(encoded) > MAX_PIPELINE_REQUEST_BYTES:
                flush()
                pending_bytes = 0
            pending.append(encoded)
            pending_commands.append(args)
            pending_bytes += len(encoded)
        flush()
        return replies

    def scan_keys(self) -> Iterator[bytes]:
        cursor = b"0"
        while True:
            cursor, page = self.command(
                b"SCAN", cursor, b"COUNT", str(self.scan_count).encode()
            )
            yield from page
            if cursor == b"0":
                return

    def read_entries(self, keys: list[bytes]) -> dict[bytes, Entry | None | ReadRace]:
        kinds = self.command_many([(b"TYPE", key) for key in keys])
        active = []
        requests = []
        result: dict[bytes, Entry | None | ReadRace] = {}
        for key, kind in zip(keys, kinds):
            if isinstance(kind, ServerError):
                raise kind
            if kind == b"none":
                result[key] = None
                continue
            active.append((key, kind))
            requests.extend((_value_command(kind, key), (b"TYPE", key)))
        replies = self.command_many(requests) if requests else []
        for index, (key, kind) in enumerate(active):
            try:
                result[key] = _entry_from_replies(
                    kind, replies[2 * index], replies[2 * index + 1], key
                )
            except ReadRace as error:
                result[key] = error
        return result


@dataclass
class DirectionStats:
    total: int = 0
    shared: int = 0
    matched: int = 0
    missing: int = 0
    type_diff: int = 0
    value_diff: int = 0
    examples: list[tuple[bytes, str]] = field(default_factory=list)


@dataclass(frozen=True)
class Report:
    a_to_b: DirectionStats
    b_to_a: DirectionStats

    @property
    def left_count(self) -> int:
        return self.a_to_b.total

    @property
    def right_count(self) -> int:
        return self.b_to_a.total

    @property
    def shared(self) -> int:
        # The two scans are not atomic; use the lower observed intersection.
        return min(self.a_to_b.shared, self.b_to_a.shared,
                   self.left_count, self.right_count)

    @property
    def matched(self) -> int:
        return min(self.a_to_b.matched, self.b_to_a.matched, self.shared)

    @property
    def key_jaccard(self) -> float:
        union = self.left_count + self.right_count - self.shared
        return self.shared / union if union else 1.0

    @property
    def data_jaccard(self) -> float:
        union = self.left_count + self.right_count - self.matched
        return self.matched / union if union else 1.0

    @property
    def diff_rate(self) -> float:
        return 1.0 - self.data_jaccard


def _record(stats: DirectionStats, key: bytes, source: Entry | None,
            target: Entry | None,
            example_limit: int) -> None:
    if source is None:
        return  # Expired on the scanned side before the final read.
    stats.total += 1
    reason = ""
    if target is None:
        stats.missing += 1
        reason = "missing"
    else:
        stats.shared += 1
        if source.kind != target.kind:
            stats.type_diff += 1
            reason = "type"
        elif source.value != target.value:
            stats.value_diff += 1
            reason = "value"
        else:
            stats.matched += 1
    if reason and sum(saved_reason == reason for _, saved_reason in stats.examples) < example_limit:
        stats.examples.append((key, reason))


def _scan_direction(source: CacheServer, target: CacheServer, retry_delay: float,
                    sleep: Callable[[float], None], example_limit: int) -> DirectionStats:
    stats = DirectionStats()
    pending: list[bytes] = []

    def retry_pending() -> None:
        if not pending:
            return
        sleep(retry_delay)
        for offset in range(0, len(pending), VALUE_BATCH_SIZE):
            names = pending[offset:offset + VALUE_BATCH_SIZE]
            source_entries = source.read_entries(names)
            target_entries = target.read_entries(names)
            for key in names:
                source_entry = source_entries[key]
                target_entry = target_entries[key]
                if isinstance(source_entry, ReadRace):
                    raise source_entry
                if isinstance(target_entry, ReadRace):
                    raise target_entry
                _record(stats, key, source_entry, target_entry, example_limit)
        pending.clear()

    keys = iter(source.scan_keys())
    while names := list(islice(keys, VALUE_BATCH_SIZE)):
        source_entries = source.read_entries(names)
        target_entries = target.read_entries(names)
        for key in names:
            source_entry = source_entries[key]
            target_entry = target_entries[key]
            if isinstance(source_entry, ReadRace) or isinstance(target_entry, ReadRace) \
                    or (source_entry is not None and source_entry != target_entry):
                pending.append(key)
            else:
                _record(stats, key, source_entry, target_entry, example_limit)
        if len(pending) >= RETRY_BATCH_SIZE:
            retry_pending()
    retry_pending()
    return stats


def compare(left: CacheServer, right: CacheServer, *, retry_delay: float = 1.5,
            sleep: Callable[[float], None] = time.sleep,
            example_limit: int = 20) -> Report:
    a_to_b = _scan_direction(left, right, retry_delay, sleep, example_limit)
    b_to_a = _scan_direction(right, left, retry_delay, sleep, example_limit)
    return Report(a_to_b, b_to_a)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("left", help="first cache_server as host:port")
    parser.add_argument("right", help="second cache_server as host:port")
    parser.add_argument("--scan-count", type=int, default=64, help="SCAN COUNT hint per request")
    parser.add_argument("--retry-delay", type=float, default=1.5, help="seconds before rechecking differences")
    parser.add_argument("--sync-timeout", type=float, default=60.0,
                        help="maximum seconds to wait for replica slots to become readable")
    parser.add_argument("--show-diffs", type=int, default=20, help="maximum example keys per difference type")
    args = parser.parse_args()
    if args.scan_count < 1 or not math.isfinite(args.retry_delay) or args.retry_delay < 0 \
            or not math.isfinite(args.sync_timeout) or args.sync_timeout < 0 \
            or args.show_diffs < 0:
        parser.error("scan-count must be positive; delays, timeout and show-diffs must be nonnegative")
    left = right = None
    try:
        left = CacheServer(args.left, args.scan_count, args.sync_timeout)
        right = CacheServer(args.right, args.scan_count, args.sync_timeout)
        report = compare(left, right, retry_delay=args.retry_delay,
                         example_limit=args.show_diffs)
    except (OSError, ValueError, ServerError, ReadRace) as error:
        parser.exit(2, f"comparison failed: {error}\n")
    finally:
        if left is not None:
            left.close()
        if right is not None:
            right.close()
    print(f"keys: left={report.left_count} right={report.right_count} matched={report.matched}")
    print(f"a_to_b: missing={report.a_to_b.missing} type={report.a_to_b.type_diff} "
          f"value={report.a_to_b.value_diff} matched={report.a_to_b.matched}")
    print(f"b_to_a: missing={report.b_to_a.missing} type={report.b_to_a.type_diff} "
          f"value={report.b_to_a.value_diff} matched={report.b_to_a.matched}")
    print(f"key_jaccard={report.key_jaccard:.6f} "
          f"data_jaccard={report.data_jaccard:.6f} diff_rate={report.diff_rate:.6f}")
    for label, stats in (("a_to_b", report.a_to_b), ("b_to_a", report.b_to_a)):
        for key, reason in stats.examples:
            print(f"  {label} {key!r}: {reason}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
