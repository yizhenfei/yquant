"""Validate a sealed one-contract recording and export finite strategy replay.

The version-1 layouts are shared with market/recording.cc and market/quote_codec.cc.
Only recorded arrival gaps, event times, BBO and flags enter the CSV. The
strategy replay source assigns fresh sequence numbers in the original order.
"""

import hashlib
import os
from pathlib import Path
import stat
import struct


HEADER = struct.Struct("<8sIIIIIIQQQQ")
RECORD = struct.Struct("<Q80sQ")
FRAME = struct.Struct("<IHHIB3xQQQqqqq8x")
CSV_HEADER = "elapsed_ns,event_time_ns,bid_price,bid_qty,ask_price,ask_qty,flags\n"
# These are the finite strategyd CSV source's initialization limits.
MAX_ROWS = 100000
MAX_CSV_BYTES = 16 * 1024 * 1024
KNOWN_FLAGS = 15


def checksum(data):
    """Return the recording format's unsigned 64-bit FNV-1a checksum."""
    value = 14695981039346656037
    for byte in data:
        value = ((value ^ byte) * 1099511628211) & 0xffffffffffffffff
    return value


def decode_frame(frame, instrument, source_id):
    """Check wire identity, side validity and configured price/lot constraints."""
    (magic, version, size, source, flags, identity, sequence, event_time,
     bid, bid_qty, ask, ask_qty) = FRAME.unpack(frame)
    if (magic, version, size) != (0x31515159, 1, FRAME.size):
        raise ValueError("unsupported quote frame layout")
    if frame[13:16] != bytes(3) or frame[72:80] != bytes(8):
        raise ValueError("nonzero reserved quote bytes")
    if source != source_id or identity != instrument.instrument_id:
        raise ValueError("recording source or instrument differs from capture metadata")
    if sequence == 0 or flags & ~KNOWN_FLAGS or (not flags & 8 and event_time):
        raise ValueError("invalid quote sequence, flags or event time")
    for price, quantity, present in ((bid, bid_qty, flags & 1),
                                     (ask, ask_qty, flags & 2)):
        if (present and quantity <= 0) or (not present and (price or quantity)):
            raise ValueError("invalid quote side")
        if price % instrument.price_tick or quantity % instrument.quantity_step:
            raise ValueError("quote violates the captured contract's tick or lot size")
    return sequence, (event_time, bid, bid_qty, ask, ask_qty, flags)


def _read_recording(recording, instrument, source_id, emit=None, max_rows=None):
    """Stream verified records; emit borrows each normalized row during a call."""
    if type(source_id) is not int or not 1 <= source_id <= 0xffffffff:
        raise ValueError("source_id must be a nonzero uint32")
    descriptor = os.open(recording, os.O_RDONLY | os.O_NONBLOCK)
    with os.fdopen(descriptor, "rb") as stream:
        info = os.fstat(stream.fileno())
        if not stat.S_ISREG(info.st_mode):
            raise ValueError("recording must be a regular file")
        header = stream.read(HEADER.size)
        if len(header) != HEADER.size:
            raise ValueError("truncated recording header")
        (magic, version, header_size, record_size, frame_size, sealed,
         reserved32, count, duration, reserved64, digest) = HEADER.unpack(header)
        if ((magic, version, header_size, record_size, frame_size) !=
                (b"YQREC001", 1, HEADER.size, RECORD.size, FRAME.size) or
                reserved32 or reserved64 or digest != checksum(header[:56])):
            raise ValueError("invalid recording header layout or checksum")
        if sealed != 1:
            raise ValueError("recording has not been sealed by marketd")
        if not count:
            raise ValueError("recording contains no quotes")
        if max_rows is not None and count > max_rows:
            raise ValueError("recording needs 1..100000 quotes; capture a shorter interval")
        if info.st_size != HEADER.size + count * RECORD.size:
            raise ValueError("recording length differs from its declared count")

        recording_hash = hashlib.sha256(header)
        previous_offset = 0
        previous_sequence = 0
        first_sequence = 0
        tradable_rows = 0
        for index in range(count):
            record = stream.read(RECORD.size)
            if len(record) != RECORD.size:
                raise ValueError("truncated recording quote")
            recording_hash.update(record)
            offset, frame, record_digest = RECORD.unpack(record)
            if record_digest != checksum(record[:88]):
                raise ValueError("recording quote checksum mismatch")
            if (index == 0 and offset != 0) or offset < previous_offset:
                raise ValueError("recorded arrival offsets moved backwards")
            sequence, fields = decode_frame(frame, instrument, source_id)
            if index and sequence != previous_sequence + 1:
                raise ValueError("recording contains a source sequence gap or reset")
            if index == 0:
                first_sequence = sequence
            if fields[-1] & 7 == 7 and fields[1] > 0 and fields[3] > fields[1]:
                tradable_rows += 1
            if emit is not None:
                emit((offset, *fields))
            previous_offset, previous_sequence = offset, sequence
        if previous_offset != duration or stream.read(1):
            raise ValueError("recording duration or final length is inconsistent")
    return {"rows": count, "duration_ns": duration,
            "tradable_rows": tradable_rows,
            "first_source_sequence": first_sequence,
            "last_source_sequence": previous_sequence,
            "recording_sha256": recording_hash.hexdigest()}


def validate_recording(recording: Path, instrument, source_id: int):
    """Validate a nonempty capture with bounded storage and report its identity.

    Checks every checksum, arrival offset, source sequence and contract unit.
    The row limit belongs to finite CSV replay and does not limit collection.
    """
    return _read_recording(recording, instrument, source_id)


def export_replay(recording: Path, output: Path, instrument, source_id: int):
    """Export exact recorded BBO/gaps to a new CSV, or leave no output on error.

    Rejects unsealed/truncated/corrupt files, sequence gaps, foreign identities,
    and inputs exceeding the finite replay source's limits. Output never
    overwrites an existing path. The returned hashes identify both artifacts.
    """
    replay_hash = hashlib.sha256()
    csv_bytes = 0
    created = False
    try:
        with Path(output).open("xb") as destination:
            created = True
            encoded_header = CSV_HEADER.encode("ascii")
            destination.write(encoded_header)
            replay_hash.update(encoded_header)
            csv_bytes += len(encoded_header)

            def emit(fields):
                nonlocal csv_bytes
                encoded = (",".join(map(str, fields)) + "\n").encode("ascii")
                csv_bytes += len(encoded)
                if csv_bytes > MAX_CSV_BYTES:
                    raise ValueError("replay exceeds 16 MiB; capture a shorter interval")
                destination.write(encoded)
                replay_hash.update(encoded)

            metadata = _read_recording(recording, instrument, source_id, emit, MAX_ROWS)
    except BaseException:
        if created:
            Path(output).unlink()
        raise
    metadata["replay_sha256"] = replay_hash.hexdigest()
    return metadata
