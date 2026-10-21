#!/usr/bin/env python3
"""Reproduce the pinned DCE a1805 quote replay from its original CSV."""

import argparse
import csv
import datetime as dt
from decimal import Decimal
import hashlib
import io
import json
from pathlib import Path
import urllib.request

COMMIT = "13b3bc3f907e4962a4d3ea0e9f95b9015e23c176"
REPOSITORY = "https://github.com/qingtiandalaoye/net1qian-flink-ctp-demo"
UPSTREAM_PATH = "src/main/resources/a1803_few.csv"
URL = f"https://raw.githubusercontent.com/qingtiandalaoye/net1qian-flink-ctp-demo/{COMMIT}/{UPSTREAM_PATH}"
SOURCE_SHA256 = "9d33183e0029e92f1a3951823e63cdaf3dd160836b0856f70d13153ac95f1a75"
DATA_DIR = Path(__file__).resolve().parent.parent / "examples" / "data"
STEM = "a1805_20171227"


def integer_price(text):
    value = Decimal(text)
    if not value.is_finite() or value != value.to_integral_value() or value <= 0:
        raise ValueError(f"invalid integral quote price: {text}")
    return int(value)


def convert(raw):
    if hashlib.sha256(raw).hexdigest() != SOURCE_SHA256:
        raise ValueError("original sample SHA256 mismatch")
    rows = list(csv.reader(io.StringIO(raw.decode("utf-8"))))
    output = io.StringIO(newline="")
    writer = csv.writer(output, lineterminator="\n")
    writer.writerow(["elapsed_ns", "event_time_ns", "bid_price", "bid_qty",
                     "ask_price", "ask_qty", "flags"])
    start = None
    previous = None
    for row in rows:
        if len(row) != 19 or row[1] != "a1805" or row[2] != "20171227":
            raise ValueError("unexpected source contract, date or column count")
        captured = dt.datetime.strptime(row[0], "%Y-%m-%d %H:%M:%S.%f")
        if previous is not None and captured < previous:
            raise ValueError("source capture timestamps moved backwards")
        if start is None:
            start = captured
        previous = captured
        delta = captured - start
        elapsed = ((delta.days * 86400 + delta.seconds) * 1000000
                   + delta.microseconds) * 1000
        bid, ask = integer_price(row[15]), integer_price(row[17])
        bid_qty, ask_qty = int(row[16]), int(row[18])
        if bid >= ask or bid_qty <= 0 or ask_qty <= 0:
            raise ValueError("invalid source best bid/ask")
        # Capture timestamps pace playback; no exchange timestamp is invented.
        writer.writerow([elapsed, 0, bid, bid_qty, ask, ask_qty, 7])
    if len(rows) != 744:
        raise ValueError("unexpected source row count")
    replay = output.getvalue().encode("ascii")
    manifest = {
        "venue": "DCE", "symbol": "a1805", "rows": len(rows),
        "source_repository": REPOSITORY, "source_commit": COMMIT,
        "source_path": UPSTREAM_PATH, "source_url": URL,
        "source_sha256": SOURCE_SHA256,
        "source_license": f"{STEM}.LICENSE",
        "source_capture_begin": rows[0][0],
        "source_capture_end": rows[-1][0],
        "duration_ns": elapsed,
        "pacing": "source column 0 capture-time differences, no resampling",
        "event_time": "unavailable; event_time_ns=0, event-time-valid flag unset",
        "mapping": {"symbol": 1, "bid_price": 15, "bid_qty": 16,
                    "ask_price": 17, "ask_qty": 18},
        "price_decimals": 0, "quantity_unit": "lot",
        "replay_sha256": hashlib.sha256(replay).hexdigest(),
        "notes": "Upstream filename says a1803; all 744 records identify a1805."
    }
    return replay, (json.dumps(manifest, indent=2, ensure_ascii=False) + "\n").encode("utf-8")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--download", action="store_true",
                        help="download the pinned original before conversion")
    parser.add_argument("--check", action="store_true",
                        help="verify the original, replay and manifest without writing")
    args = parser.parse_args()
    source = DATA_DIR / f"{STEM}.source.csv"
    if args.download:
        with urllib.request.urlopen(URL, timeout=30) as response:
            raw = response.read(1024 * 1024)
    else:
        raw = source.read_bytes()
    replay, manifest = convert(raw)
    artifacts = [(source, raw), (DATA_DIR / f"{STEM}.csv", replay),
                 (DATA_DIR / f"{STEM}.json", manifest)]
    if args.check:
        for path, expected in artifacts:
            if path.read_bytes() != expected:
                raise ValueError(f"artifact mismatch: {path}")
    else:
        for path, content in artifacts:
            path.write_bytes(content)
    print(f"DCE/a1805: 744 source quotes verified; {DATA_DIR / (STEM + '.csv')}")


if __name__ == "__main__":
    main()
