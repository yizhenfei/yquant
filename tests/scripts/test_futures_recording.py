"""Exercise capture validation against the documented native recording layout."""

import hashlib
from pathlib import Path
import struct
import sys
import tempfile
from types import SimpleNamespace
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "scripts"))
import futures_recording as recording


def fnv(data):
    value = 0xcbf29ce484222325
    for byte in data:
        value = ((value ^ byte) * 0x100000001b3) % (1 << 64)
    return value


def wire_quote(sequence=1, source=2, identity=23622339265, flags=15,
               event_time=1792630800500000000, bid=3662, bid_qty=70,
               ask=3664, ask_qty=53):
    frame = bytearray(80)
    struct.pack_into("<IHHI", frame, 0, 0x31515159, 1, 80, source)
    frame[12] = flags
    struct.pack_into("<QQQqqqq", frame, 16, identity, sequence, event_time,
                     bid, bid_qty, ask, ask_qty)
    return bytes(frame)


def capture(frames, offsets=None, sealed=1, duration=None):
    offsets = offsets if offsets is not None else [0, 749000000][:len(frames)]
    duration = offsets[-1] if duration is None else duration
    header = bytearray(64)
    header[:8] = b"YQREC001"
    struct.pack_into("<IIIII", header, 8, 1, 64, 96, 80, sealed)
    struct.pack_into("<QQ", header, 32, len(frames), duration)
    struct.pack_into("<Q", header, 56, fnv(header[:56]))
    result = bytes(header)
    for offset, frame in zip(offsets, frames):
        record = struct.pack("<Q", offset) + frame
        result += record + struct.pack("<Q", fnv(record))
    return result


class RecordingTest(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        self.input = self.root / "capture.yqr"
        self.output = self.root / "replay.csv"
        self.instrument = SimpleNamespace(instrument_id=23622339265,
                                          price_tick=1, quantity_step=1)

    def export(self, data):
        self.input.write_bytes(data)
        return recording.export_replay(self.input, self.output, self.instrument, 2)

    def test_preserves_arrival_gaps_event_times_and_both_sides(self):
        data = capture([wire_quote(sequence=12),
                        wire_quote(sequence=13, flags=3, event_time=0,
                                   bid=3661, bid_qty=1, ask=3663, ask_qty=2)])
        summary = self.export(data)
        self.assertEqual(self.output.read_text(),
                         "elapsed_ns,event_time_ns,bid_price,bid_qty,ask_price,ask_qty,flags\n"
                         "0,1792630800500000000,3662,70,3664,53,15\n"
                         "749000000,0,3661,1,3663,2,3\n")
        self.assertEqual(summary["rows"], 2)
        self.assertEqual(summary["duration_ns"], 749000000)
        self.assertEqual(summary["tradable_rows"], 1)
        self.assertEqual(summary["first_source_sequence"], 12)
        self.assertEqual(summary["last_source_sequence"], 13)
        self.assertEqual(summary["recording_sha256"], hashlib.sha256(data).hexdigest())
        self.assertEqual(summary["replay_sha256"],
                         hashlib.sha256(self.output.read_bytes()).hexdigest())

    def test_rejects_corruption_and_unsealed_or_inconsistent_capture(self):
        good = capture([wire_quote(), wire_quote(sequence=2)])
        corrupt_record = bytearray(good)
        corrupt_record[105] ^= 1
        for data in (good[:30], good[:-1], good + b"x", bytes(corrupt_record),
                     capture([wire_quote()], sealed=0),
                     capture([wire_quote()], duration=1),
                     capture([wire_quote(), wire_quote(sequence=2)], [0, -1 & ((1 << 64)-1)], duration=0)):
            with self.subTest(data_length=len(data)):
                with self.assertRaises(ValueError):
                    self.export(data)
                self.assertFalse(self.output.exists())

    def test_rejects_identity_sequence_and_contract_mismatches(self):
        for frame in (wire_quote(source=3), wire_quote(identity=123),
                      wire_quote(sequence=4), wire_quote(sequence=1),
                      wire_quote(flags=128), wire_quote(flags=7),
                      wire_quote(bid_qty=0), wire_quote(flags=14)):
            with self.subTest(frame=frame.hex()):
                with self.assertRaises(ValueError):
                    self.export(capture([wire_quote(), frame]))
                self.assertFalse(self.output.exists())
        self.instrument.price_tick = 5
        with self.assertRaisesRegex(ValueError, "tick or lot"):
            self.export(capture([wire_quote()]))

    def test_rejects_reserved_wire_bytes_even_with_valid_record_checksum(self):
        frame = bytearray(wire_quote())
        frame[79] = 1
        with self.assertRaisesRegex(ValueError, "reserved"):
            self.export(capture([bytes(frame)]))

    def test_does_not_overwrite_existing_output(self):
        self.output.write_text("keep")
        with self.assertRaises(FileExistsError):
            self.export(capture([wire_quote()]))
        self.assertEqual(self.output.read_text(), "keep")

    def test_rejects_empty_and_oversized_declared_capture_before_output(self):
        self.input.write_bytes(capture([wire_quote()]))
        for count in (0, 100001):
            header = bytearray(self.input.read_bytes()[:64])
            struct.pack_into("<Q", header, 32, count)
            struct.pack_into("<Q", header, 56, fnv(header[:56]))
            self.input.write_bytes(header)
            with self.assertRaises(ValueError):
                recording.export_replay(self.input, self.output, self.instrument, 2)
            self.assertFalse(self.output.exists())


if __name__ == "__main__":
    unittest.main()
