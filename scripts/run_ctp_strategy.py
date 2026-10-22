#!/usr/bin/env python3
"""Capture CTP quotes or run the futures example with simulated execution.

Each run owns a new output directory containing its copied instrument catalog,
configuration, manifest, and separate stdout/stderr logs for every process.
The live and replay modes always use the simulated_futures trading adapter.
CTP credentials are inherited from the environment and never written to disk.
"""

import argparse
from datetime import datetime, timezone
import hashlib
import json
import math
import os
from pathlib import Path
import re
import signal
import socket
import subprocess
import sys
import time
from urllib.parse import urlsplit


PROJECT = Path(__file__).resolve().parent.parent
SHUTDOWN_SECONDS = 8
POLL_SECONDS = 0.02


class RunError(Exception):
    """A failed configuration, startup, collection, or child-process shutdown."""


class InterruptedRun(RunError):
    """A user signal requested an orderly shutdown."""


def utc_now():
    return datetime.now(timezone.utc).isoformat().replace("+00:00", "Z")


def write_json(path, content):
    """Replace a run-owned JSON file without exposing a partial manifest."""
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(json.dumps(content, indent=2) + "\n", encoding="utf-8")
    temporary.replace(path)


def sha256(path):
    checksum = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            checksum.update(block)
    return checksum.hexdigest()


def free_port():
    with socket.socket() as listener:
        listener.bind(("127.0.0.1", 0))
        return listener.getsockname()[1]


def positive_seconds(value):
    number = float(value)
    if not math.isfinite(number) or number <= 0:
        raise argparse.ArgumentTypeError("must be a finite positive number")
    return number


def source_id(value):
    number = int(value)
    if not 1 <= number <= 4294967295:
        raise argparse.ArgumentTypeError("must be an integer in 1..4294967295")
    return number


def replay_speed(value):
    number = int(value)
    if not 1 <= number <= 1000:
        raise argparse.ArgumentTypeError("must be an integer in 1..1000")
    return number


def parse_arguments():
    parser = argparse.ArgumentParser(description=__doc__)
    modes = parser.add_subparsers(dest="mode", required=True)
    for mode in ("live", "record", "replay"):
        command = modes.add_parser(mode)
        command.add_argument("--run-dir", type=Path, required=True,
                             help="new, exclusive run directory")
        command.add_argument("--build-dir", type=Path,
                             default=PROJECT / "build/debug")
        if mode == "replay":
            command.add_argument("--capture-dir", type=Path, required=True,
                                 help="successful live or record run")
            command.add_argument("--speed", type=replay_speed, default=1)
        else:
            command.add_argument("--front", required=True,
                                 help="CTP market-data front: tcp://host:port")
            command.add_argument("--anonymous", action="store_true",
                                 help="use empty login fields on a front that supports it")
            command.add_argument("--catalog", type=Path, required=True)
            command.add_argument("--venue", required=True)
            command.add_argument("--symbol", required=True)
            command.add_argument("--source-id", type=source_id, default=1)
            command.add_argument("--duration-seconds", type=positive_seconds,
                                 default=60)
            command.add_argument("--connect-timeout-seconds",
                                 type=positive_seconds, default=30,
                                 help="deadline for login and first valid quote")
    return parser.parse_args()


class Processes:
    """Own child sessions and stop dependents before their input and gateway."""

    def __init__(self, run):
        self.run = run
        self.stop_request_file = run / "stop_requested"
        self.children = {}
        self.logs = []
        self.stopped_at = {}
        self.signum = None

    def handle_signal(self, signum, _frame):
        self.signum = signum

    def check_signal(self):
        if self.signum is not None:
            raise InterruptedRun(f"received signal {self.signum}")

    def start(self, name, command):
        self.check_signal()
        stdout = (self.run / f"{name}.stdout.log").open("wb")
        self.logs.append(stdout)
        stderr = (self.run / f"{name}.stderr.log").open("wb")
        self.logs.append(stderr)
        process = subprocess.Popen(command, cwd=PROJECT, stdout=stdout,
                                   stderr=stderr, start_new_session=True)
        self.children[name] = process
        return process

    def output(self, name):
        return (self.run / f"{name}.stdout.log").read_text(
            encoding="utf-8", errors="replace")

    def check_alive(self, names):
        self.check_signal()
        for name in names:
            process = self.children.get(name)
            if process is not None and process.poll() is not None:
                raise RunError(f"{name} exited with status {process.returncode}; "
                               f"see {self.run / (name + '.stderr.log')}")

    def wait_marker(self, name, marker, timeout, supervised=()):
        deadline = time.monotonic() + timeout
        while True:
            self.check_alive((*supervised, name))
            if marker in self.output(name):
                return
            if time.monotonic() >= deadline:
                raise RunError(f"timed out waiting for {name}: {marker}; "
                               f"see {self.run / (name + '.stderr.log')}")
            time.sleep(POLL_SECONDS)

    def stop(self, name):
        """Return a shutdown error after trying graceful stop and forced kill."""
        process = self.children.get(name)
        if process is None:
            return None
        if (name == "bridge" and self.stop_request_file.is_file() and
                process.poll() is None):
            # Intentional consumer EOF already asks the bridge to finish. Let
            # SDK release and Python finalization complete before any signal;
            # SIGTERM during finalization may use the restored default handler.
            try:
                process.wait(timeout=SHUTDOWN_SECONDS)
            except subprocess.TimeoutExpired:
                pass
        if process.poll() is None:
            try:
                os.killpg(process.pid, signal.SIGTERM)
            except ProcessLookupError:
                pass
            try:
                process.wait(timeout=SHUTDOWN_SECONDS)
            except subprocess.TimeoutExpired:
                try:
                    os.killpg(process.pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
                process.wait()
                self.stopped_at[name] = utc_now()
                return f"{name} did not stop within {SHUTDOWN_SECONDS} seconds"
        self.stopped_at[name] = utc_now()
        if process.returncode != 0:
            return f"{name} exited with status {process.returncode}"
        return None

    def cleanup(self):
        errors = []
        # The strategy needs live market data and tradingd to drain its cancels.
        # Request marketd shutdown before releasing the upstream connection.
        for name in ("strategyd", "marketd", "bridge", "tradingd"):
            if (name == "marketd" and "bridge" in self.children and
                    name in self.children and self.children[name].poll() is None):
                # marketd closes TCP before its writer finishes sealing/fsync.
                # Let the bridge recognize this intentional consumer EOF even
                # when sealing takes longer than a fixed grace period.
                try:
                    self.stop_request_file.touch(exist_ok=False)
                except OSError as failure:
                    errors.append(f"cannot request bridge shutdown: {failure}")
            error = self.stop(name)
            if error:
                errors.append(error)
        for log in self.logs:
            log.close()
        return errors


def require_artifacts(args):
    binaries = args.build_dir.resolve() / "bin"
    names = ["marketd"] if args.mode == "record" else ["tradingd", "strategyd"]
    if args.mode == "live":
        names.append("marketd")
    for name in names:
        binary = binaries / ("yquant-" + name)
        if not binary.is_file() or not os.access(binary, os.X_OK):
            raise RunError(f"missing executable: {binary}; build the project first")
    strategy = args.build_dir.resolve() / "strategies/futures_bbo_strategy.so"
    if args.mode != "record" and not strategy.is_file():
        raise RunError(f"missing strategy library: {strategy}")
    return binaries, strategy


def instrument_identity(venue, symbol):
    if not isinstance(venue, str) or not isinstance(symbol, str):
        raise RunError("capture manifest has invalid instrument identity")
    return {"venue": venue, "symbol": symbol}


def prepare_live(args, run, manifest):
    from ctp_quote_bridge import load_instrument

    front = urlsplit(args.front)
    if (front.scheme != "tcp" or not front.hostname or not front.port or
            front.username is not None or front.password is not None or
            front.path or front.query or front.fragment):
        raise RunError("--front must be tcp://host:port without credentials")
    required = ("YQUANT_CTP_BROKER_ID", "YQUANT_CTP_USER_ID", "YQUANT_CTP_PASSWORD")
    missing = [] if args.anonymous else [name for name in required if not os.environ.get(name)]
    if missing:
        raise RunError("missing environment variables: " + ", ".join(missing))
    catalog = run / "instruments.json"
    catalog.write_bytes(args.catalog.resolve().read_bytes())
    instrument = load_instrument(catalog, args.venue, args.symbol)
    manifest.update({
        "source": {"adapter": "ctp", "front": args.front,
                   "source_id": args.source_id,
                   "auth_mode": "anonymous" if args.anonymous else "credentials"},
        "instrument": instrument_identity(args.venue, args.symbol),
        "catalog": catalog.name,
        "catalog_sha256": sha256(catalog),
        "recording": "capture.yqr",
        "duration_seconds": args.duration_seconds,
        "connect_timeout_seconds": args.connect_timeout_seconds,
    })
    return catalog, instrument


def prepare_replay(args, run, manifest):
    from ctp_quote_bridge import load_instrument
    from futures_recording import export_replay

    capture = args.capture_dir.resolve()
    origin = json.loads((capture / "manifest.json").read_text(encoding="utf-8"))
    if (not isinstance(origin, dict) or origin.get("schema_version") != 1 or
            origin.get("mode") not in ("live", "record") or
            origin.get("success") is not True or
            origin.get("received_valid_quote") is not True or
            origin.get("catalog") != "instruments.json" or
            origin.get("recording") != "capture.yqr"):
        raise RunError("capture manifest must describe a successful CTP capture")
    identity, source = origin.get("instrument"), origin.get("source")
    if not isinstance(identity, dict) or not isinstance(source, dict):
        raise RunError("capture manifest is missing source or instrument identity")
    identity = instrument_identity(identity.get("venue"), identity.get("symbol"))
    number = source.get("source_id")
    if (source.get("adapter") != "ctp" or type(number) is not int or
            not 1 <= number <= 4294967295):
        raise RunError("capture manifest has an invalid source identity")
    catalog, recording = capture / "instruments.json", capture / "capture.yqr"
    if sha256(catalog) != origin.get("catalog_sha256"):
        raise RunError("capture instrument catalog checksum does not match manifest")
    if sha256(recording) != origin.get("recording_sha256"):
        raise RunError("capture recording checksum does not match manifest")
    copied = run / "instruments.json"
    copied.write_bytes(catalog.read_bytes())
    instrument = load_instrument(copied, identity["venue"], identity["symbol"])
    replay = run / "replay.csv"
    metadata = export_replay(recording, replay, instrument, number)
    manifest.update({"source": source, "instrument": identity,
                     "catalog": copied.name,
                     "catalog_sha256": sha256(copied),
                     "capture_directory": str(capture),
                     "capture_manifest_sha256": sha256(capture / "manifest.json"),
                     "replay": replay.name, "replay_speed": args.speed,
                     "replay_metadata": metadata})
    return copied, metadata


def market_config(catalog, manifest):
    return {"source_id": manifest["source"]["source_id"],
            "instrument_catalog": str(catalog),
            "instruments": [manifest["instrument"]]}


def start_strategy(args, run, manifest, processes, binaries, strategy, market):
    """Start a simulated gateway and the existing futures quoting strategy."""
    strategy_config = json.loads((PROJECT / "examples/strategies/"
                                 "futures_bbo_strategy.json").read_text())
    strategy_config["parameters"].update(manifest["instrument"])
    trading_config = json.loads((PROJECT / "examples/tradingd/"
                                "futures_trading.json").read_text())
    if trading_config.get("adapter") != "simulated_futures":
        raise RunError("futures trading template must use simulated_futures")
    market_path = run / "market_data.json"
    strategy_path = run / "strategy.json"
    trading_path = run / "simulated_futures.json"
    write_json(market_path, market)
    write_json(strategy_path, strategy_config)
    write_json(trading_path, trading_config)
    port = free_port()
    token = "ctp-simulated-example"
    gateway = {"listen": {"address": "127.0.0.1", "port": port},
               "market_data_config": str(market_path),
               "trading_config": str(trading_path), "token": token,
               "shutdown_timeout_ms": 5000}
    client = {"adapter": "tcp", "address": "127.0.0.1", "port": port,
              "client_id": 1, "token": token, "shutdown_timeout_ms": 5000}
    gateway_path, client_path = run / "gateway.json", run / "client.json"
    write_json(gateway_path, gateway)
    write_json(client_path, client)
    processes.start("tradingd", [str(binaries / "yquant-tradingd"),
                                 "--config", str(gateway_path)])
    supervised = ("bridge", "marketd") if args.mode == "live" else ()
    processes.wait_marker("tradingd", "yquant-tradingd: Running", 10, supervised)
    processes.start("strategyd", [str(binaries / "yquant-strategyd"),
                                  "--strategy", str(strategy),
                                  "--config", str(strategy_path),
                                  "--market-data-config", str(market_path),
                                  "--trading-config", str(client_path)])


def capture_quotes(args, run, manifest, processes, binaries, strategy, catalog):
    port = free_port()
    command = [sys.executable, "-u", str(PROJECT / "scripts/ctp_quote_bridge.py"),
               "--front", args.front, "--catalog", str(catalog),
               "--venue", args.venue, "--symbol", args.symbol,
               "--listen-port", str(port), "--source-id", str(args.source_id),
               "--stop-request-file", str(processes.stop_request_file),
               "--connect-timeout-seconds", str(args.connect_timeout_seconds)]
    if args.anonymous:
        command.append("--anonymous")
    processes.start("bridge", command)
    processes.wait_marker("bridge", "CTP bridge listening",
                          args.connect_timeout_seconds)
    runtime = run / "market-runtime"
    runtime.mkdir()
    producer = {"source": {"adapter": "tcp", "address": "127.0.0.1",
                           "port": port},
                "publishers": [
                    {"adapter": "shm", "runtime_directory": str(runtime),
                     "stream": "quotes", "capacity": 16384,
                     "max_payload_size": 80},
                    {"adapter": "record", "path": str(run / "capture.yqr"),
                     "queue_capacity": 1048576}]}
    producer_path = run / "marketd.json"
    write_json(producer_path, producer)
    processes.start("marketd", [str(binaries / "yquant-marketd"),
                                "--config", str(producer_path)])
    processes.wait_marker("marketd", "yquant-marketd: Running", 10, ("bridge",))
    manifest["capture_started_at_utc"] = utc_now()
    write_json(run / "manifest.json", manifest)
    if args.mode == "live":
        market = market_config(catalog, manifest)
        market.update({"adapter": "shm", "runtime_directory": str(runtime),
                       "stream": "quotes", "poll_interval_us": 100})
        start_strategy(args, run, manifest, processes, binaries, strategy, market)
        processes.wait_marker("strategyd", "yquant-strategyd: Running", 10,
                              ("bridge", "marketd", "tradingd"))
    supervised = tuple(name for name in processes.children if name != "bridge")
    processes.wait_marker("bridge", "CTP login succeeded",
                          args.connect_timeout_seconds, supervised)
    processes.wait_marker("bridge", "CTP first quote",
                          args.connect_timeout_seconds, supervised)
    manifest["received_valid_quote"] = True
    manifest["first_quote_at_utc"] = utc_now()
    if args.mode == "live":
        processes.wait_marker("strategyd", "futures_bbo: both sides submitted",
                              args.connect_timeout_seconds,
                              ("bridge", "marketd", "tradingd"))
        manifest["strategy_submitted_quotes"] = True
    write_json(run / "manifest.json", manifest)
    print(f"CTP quotes: {args.venue}/{args.symbol}; run directory: {run}", flush=True)
    deadline = time.monotonic() + args.duration_seconds
    supervised = tuple(processes.children)
    while time.monotonic() < deadline:
        processes.check_alive(supervised)
        time.sleep(min(POLL_SECONDS, max(0, deadline - time.monotonic())))
    processes.check_alive(supervised)


def replay_quotes(args, run, manifest, processes, binaries, strategy, catalog,
                  metadata):
    market = market_config(catalog, manifest)
    market.update({"adapter": "replay", "replay_path": str(run / "replay.csv"),
                   "replay_speed": args.speed, "replay_tail_ms": 1000})
    start_strategy(args, run, manifest, processes, binaries, strategy, market)
    print(f"Replay: {manifest['instrument']['venue']}/"
          f"{manifest['instrument']['symbol']} at {args.speed}x; "
          f"run directory: {run}", flush=True)
    deadline = time.monotonic() + metadata["duration_ns"] / 1e9 / args.speed + 30
    strategy_process = processes.children["strategyd"]
    while strategy_process.poll() is None:
        processes.check_alive(("tradingd",))
        if time.monotonic() >= deadline:
            raise RunError("strategy did not finish after the replay EOF deadline")
        time.sleep(POLL_SECONDS)
    if strategy_process.returncode != 0:
        raise RunError(f"strategyd exited with status {strategy_process.returncode}; "
                       f"see {run / 'strategyd.stderr.log'}")
    if "futures_bbo: both sides submitted" not in processes.output("strategyd"):
        raise RunError("replay finished without driving the strategy to submit both sides")
    manifest["received_valid_quote"] = True
    manifest["strategy_submitted_quotes"] = True
    manifest["replay_completed_at_utc"] = utc_now()


def main():
    args = parse_arguments()
    run = args.run_dir.resolve()
    try:
        run.mkdir(parents=True, exist_ok=False)
    except OSError as error:
        print(f"cannot create new run directory {run}: {error}", file=sys.stderr)
        return 1
    processes = Processes(run)
    handlers = {number: signal.signal(number, processes.handle_signal)
                for number in (signal.SIGINT, signal.SIGTERM)}
    manifest = {"schema_version": 1, "mode": args.mode,
                "started_at_utc": utc_now(), "received_valid_quote": False,
                "success": False, "execution": "simulated_futures"}
    error = None
    instrument = None
    try:
        write_json(run / "manifest.json", manifest)
        binaries, strategy = require_artifacts(args)
        if args.mode == "replay":
            catalog, metadata = prepare_replay(args, run, manifest)
            write_json(run / "manifest.json", manifest)
            replay_quotes(args, run, manifest, processes, binaries, strategy,
                          catalog, metadata)
        else:
            catalog, instrument = prepare_live(args, run, manifest)
            write_json(run / "manifest.json", manifest)
            capture_quotes(args, run, manifest, processes, binaries, strategy,
                           catalog)
    except Exception as failure:
        # Imported bridge/recording validators have their own error classes.
        # Every failed preparation must leave a failed manifest as well.
        error = str(failure)
    finally:
        cleanup_errors = processes.cleanup()
        manifest["ended_at_utc"] = utc_now()
        if args.mode != "replay":
            manifest["capture_ended_at_utc"] = processes.stopped_at.get("marketd")
            recording = run / "capture.yqr"
            if "marketd" in processes.children:
                count = re.search(r"Stopped events=(\d+)", processes.output("marketd"))
                manifest["recorded_events"] = int(count[1]) if count else None
                if not count or int(count[1]) == 0:
                    cleanup_errors.append("marketd did not seal a nonempty capture")
            if recording.is_file() and instrument is not None:
                try:
                    from futures_recording import validate_recording

                    metadata = validate_recording(recording, instrument,
                                                  args.source_id)
                    manifest["recording_metadata"] = metadata
                    manifest["recording_sha256"] = metadata["recording_sha256"]
                    manifest["received_valid_quote"] = metadata["tradable_rows"] > 0
                    if not manifest["received_valid_quote"]:
                        cleanup_errors.append("capture contains no tradable two-sided quote")
                    if metadata["rows"] != manifest.get("recorded_events"):
                        cleanup_errors.append("sealed quote count differs from marketd output")
                except Exception as failure:
                    manifest["received_valid_quote"] = False
                    cleanup_errors.append(f"recording validation failed: {failure}")
            elif error is None:
                cleanup_errors.append("capture recording is missing")
        manifest["process_returncodes"] = {
            name: child.returncode for name, child in processes.children.items()}
        if cleanup_errors:
            error = "; ".join(filter(None, [error, *cleanup_errors]))
        if processes.signum is not None and error is None:
            error = f"received signal {processes.signum}"
        manifest["success"] = error is None
        if error:
            manifest["error"] = error
        try:
            write_json(run / "manifest.json", manifest)
        except OSError as failure:
            error = f"cannot write run manifest: {failure}"
        for number, handler in handlers.items():
            signal.signal(number, handler)
    if error:
        print(error, file=sys.stderr)
        return 128 + processes.signum if processes.signum is not None else 1
    print(f"Completed; manifest: {run / 'manifest.json'}", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
