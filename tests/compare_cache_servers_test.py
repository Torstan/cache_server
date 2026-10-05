"""Behavior checks for the cache_server comparison utility."""

import io
import unittest
from unittest.mock import patch

from compare_cache_servers import CacheServer, Entry, compare, read_reply


class FakeServer:
    def __init__(self, keys, entries):
        self.keys = set(keys)
        self.entries = dict(entries)
        self.batch_sizes = []

    def scan_keys(self):
        return self.keys

    def read_entry(self, key):
        return self.entries.get(key)

    def read_entries(self, keys):
        self.batch_sizes.append(len(keys))
        return {key: self.read_entry(key) for key in keys}


class CompareTests(unittest.TestCase):
    def test_comparison_does_not_open_sqlite(self):
        left = FakeServer([b"key"], {b"key": Entry(b"string", b"value")})
        right = FakeServer([b"key"], {b"key": Entry(b"string", b"value")})

        with patch("sqlite3.connect", side_effect=AssertionError("SQLite must not be used")):
            report = compare(left, right)

        self.assertEqual(report.data_jaccard, 1)

    def test_reads_first_scan_in_small_batches_before_starting_second(self):
        events = []

        class StreamingServer(FakeServer):
            def __init__(self, name, keys, entries):
                super().__init__(keys, entries)
                self.name = name

            def scan_keys(self):
                events.append(f"{self.name}:start")
                for index, key in enumerate(sorted(self.keys)):
                    if index == 16 and not self.batch_sizes:
                        raise AssertionError("SCAN keys were buffered before comparison")
                    yield key
                events.append(f"{self.name}:end")

        entries = {f"k{i}".encode(): Entry(b"string", b"v") for i in range(33)}
        left = StreamingServer("a", entries, entries)
        right = StreamingServer("b", entries, entries)

        report = compare(left, right)

        self.assertEqual(events, ["a:start", "a:end", "b:start", "b:end"])
        self.assertEqual(report.matched, 33)

    def test_symmetric_jaccard_counts_missing_type_and_value_differences(self):
        left = FakeServer(
            [b"same", b"value", b"type", b"left"],
            {
                b"same": Entry(b"string", b"ok"),
                b"value": Entry(b"string", b"old"),
                b"type": Entry(b"string", b"x"),
                b"left": Entry(b"string", b"l"),
            },
        )
        right = FakeServer(
            [b"same", b"value", b"type", b"right"],
            {
                b"same": Entry(b"string", b"ok"),
                b"value": Entry(b"string", b"new"),
                b"type": Entry(b"hash", ((b"f", b"x"),)),
                b"right": Entry(b"string", b"r"),
            },
        )

        report = compare(left, right, retry_delay=0, sleep=lambda _: None)

        self.assertEqual(report.left_count, 4)
        self.assertEqual(report.right_count, 4)
        self.assertEqual(report.matched, 1)
        self.assertEqual(report.a_to_b.missing, 1)
        self.assertEqual(report.b_to_a.missing, 1)
        self.assertEqual(report.a_to_b.type_diff, 1)
        self.assertEqual(report.b_to_a.type_diff, 1)
        self.assertEqual(report.a_to_b.value_diff, 1)
        self.assertEqual(report.b_to_a.value_diff, 1)
        self.assertAlmostEqual(report.key_jaccard, 3 / 5)
        self.assertAlmostEqual(report.data_jaccard, 1 / 7)
        self.assertAlmostEqual(report.diff_rate, 6 / 7)

    def test_retries_all_mismatches_after_one_delay(self):
        left = FakeServer(
            [b"gone", b"lagging"],
            {b"gone": Entry(b"string", b"x"), b"lagging": Entry(b"string", b"new")},
        )
        right = FakeServer([b"lagging"], {b"lagging": Entry(b"string", b"old")})
        delays = []

        def advance(delay):
            delays.append(delay)
            left.entries.pop(b"gone")
            right.entries[b"lagging"] = Entry(b"string", b"new")

        report = compare(left, right, retry_delay=1.5, sleep=advance)

        self.assertEqual(delays, [1.5])
        self.assertEqual(report.left_count, 1)
        self.assertEqual(report.right_count, 1)
        self.assertEqual(report.matched, 1)
        self.assertEqual(report.data_jaccard, 1)

    def test_empty_nodes_have_full_similarity_and_no_retry(self):
        report = compare(FakeServer([], {}), FakeServer([], {}), sleep=lambda _: self.fail("slept"))
        self.assertEqual(report.key_jaccard, 1)
        self.assertEqual(report.data_jaccard, 1)
        self.assertEqual(report.diff_rate, 0)

    def test_example_limit_applies_to_each_difference_category(self):
        left = FakeServer(
            [b"a-left", b"b-type", b"c-value"],
            {b"a-left": Entry(b"string", b"l"),
             b"b-type": Entry(b"string", b"x"),
             b"c-value": Entry(b"string", b"old")},
        )
        right = FakeServer(
            [b"b-type", b"c-value", b"d-right"],
            {b"b-type": Entry(b"hash", ((b"f", b"x"),)),
             b"c-value": Entry(b"string", b"new"),
             b"d-right": Entry(b"string", b"r")},
        )

        report = compare(left, right, retry_delay=0, sleep=lambda _: None,
                         example_limit=1)

        self.assertEqual(set(report.a_to_b.examples), {
            (b"a-left", "missing"),
            (b"b-type", "type"),
            (b"c-value", "value"),
        })
        self.assertEqual(set(report.b_to_a.examples), {
            (b"d-right", "missing"),
            (b"b-type", "type"),
            (b"c-value", "value"),
        })

    def test_value_reads_stay_in_small_batches(self):
        entries = {f"k{i}".encode(): Entry(b"string", b"large-value") for i in range(33)}
        left = FakeServer(entries, entries)
        right = FakeServer(entries, entries)

        report = compare(left, right)

        self.assertEqual(report.matched, 33)
        self.assertLessEqual(max(left.batch_sizes), 16)
        self.assertLessEqual(max(right.batch_sizes), 16)


class ReadTests(unittest.TestCase):
    def test_retries_syncing_slot_then_returns_successful_read(self):
        class RecordingSocket:
            def __init__(self):
                self.sends = 0

            def sendall(self, _payload):
                self.sends += 1

        server = object.__new__(CacheServer)
        server.socket = RecordingSocket()
        server.reader = io.BytesIO(b"-TRYAGAIN slot is syncing\r\n+string\r\n")
        server.sync_timeout = 5

        with patch("compare_cache_servers.time.sleep") as sleep:
            result = server.command_many([(b"TYPE", b"key")])

        self.assertEqual(result, [b"string"])
        self.assertEqual(server.socket.sends, 2)
        sleep.assert_called_once_with(1.0)

    def test_sync_wait_has_bound_and_does_not_retry_other_errors(self):
        class RecordingSocket:
            def __init__(self):
                self.sends = 0

            def sendall(self, _payload):
                self.sends += 1

        server = object.__new__(CacheServer)
        server.socket = RecordingSocket()
        server.reader = io.BytesIO(b"-TRYAGAIN slot is syncing\r\n")
        server.sync_timeout = 0

        with self.assertRaisesRegex(Exception, "still syncing"):
            server.command(b"SCAN", b"0", b"COUNT", b"1")
        self.assertEqual(server.socket.sends, 1)

        server.reader = io.BytesIO(b"-READONLY replica reads are disabled\r\n")
        with self.assertRaisesRegex(Exception, "reads are disabled"):
            server.command(b"TYPE", b"key")
        self.assertEqual(server.socket.sends, 2)

    def test_pipeline_bounds_request_bytes_before_reading_replies(self):
        class BoundedSocket:
            def __init__(self):
                self.sends = []

            def sendall(self, payload):
                self.sends.append(len(payload))
                if len(payload) > 8192:
                    raise AssertionError("client sent too much before reading")

        server = object.__new__(CacheServer)
        server.socket = BoundedSocket()
        server.reader = io.BytesIO(b"+OK\r\n" * 3)
        commands = [(b"TYPE", b"k" * 3500)] * 3

        self.assertEqual(server.command_many(commands), [b"OK"] * 3)
        self.assertGreater(len(server.socket.sends), 1)

    def test_batch_reads_types_then_values_with_type_rechecks(self):
        server = object.__new__(CacheServer)
        batches = []

        def command_many(commands):
            batches.append(commands)
            if len(batches) == 1:
                return [b"string", b"hash", b"set", b"zset", b"none"]
            return [b"a\x00b", b"string", [b"z", b"2", b"a", b"1"], b"hash",
                    [b"z", b"a"], b"set", [b"z", b"2", b"a", b"1"], b"zset"]

        server.command_many = command_many
        result = server.read_entries([b"s", b"h", b"set", b"zset", b"missing"])

        self.assertEqual(result, {
            b"s": Entry(b"string", b"a\x00b"),
            b"h": Entry(b"hash", ((b"a", b"1"), (b"z", b"2"))),
            b"set": Entry(b"set", (b"a", b"z")),
            b"zset": Entry(b"zset", ((b"a", b"1"), (b"z", b"2"))),
            b"missing": None,
        })
        self.assertEqual(batches, [
            [(b"TYPE", b"s"), (b"TYPE", b"h"), (b"TYPE", b"set"),
             (b"TYPE", b"zset"), (b"TYPE", b"missing")],
            [(b"GET", b"s"), (b"TYPE", b"s"),
             (b"HGETALL", b"h"), (b"TYPE", b"h"),
             (b"SMEMBERS", b"set"), (b"TYPE", b"set"),
             (b"ZRANGE", b"zset", b"0", b"-1", b"WITHSCORES"),
             (b"TYPE", b"zset")],
        ])

    def test_scan_streams_pages_using_returned_slot_cursor(self):
        server = object.__new__(CacheServer)
        server.scan_count = 2
        pages = {
            b"0": [b"71", [b"first"]],
            b"71": [b"0", [b"second", b"first"]],
        }
        server.command = lambda _scan, cursor, _count, _hint: pages[cursor]

        self.assertEqual(list(server.scan_keys()), [b"first", b"second", b"first"])

    def test_resp_parser_preserves_nested_binary_values(self):
        reply = io.BytesIO(b"*2\r\n$1\r\n0\r\n*2\r\n$3\r\na\x00b\r\n$0\r\n\r\n")
        self.assertEqual(read_reply(reply), [b"0", [b"a\x00b", b""]])


if __name__ == "__main__":
    unittest.main()
