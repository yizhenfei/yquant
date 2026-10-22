#!/usr/bin/env python3
"""Run the futures BBO strategy against the SimNow CTP simulated broker.

The CTP trader supplies order reports and authoritative account snapshots.
Market data is recorded independently for later strategy replay. Credentials
remain in bridge environments; local authentication tokens and runtime files
are retained in a new private run directory.
"""

import argparse
import json
import os
from pathlib import Path
import re
import secrets
import signal
import subprocess
import sys
import time

from ctp_quote_bridge import load_instrument
from ctp_trader_bridge import Credentials, SIMNOW_FRONTS
from futures_recording import validate_recording
from run_ctp_strategy import (PROJECT, POLL_SECONDS, Processes, RunError,
                              free_port, positive_seconds, require_artifacts,
                              sha256, utc_now, write_json)


# The quote and trader endpoints belong to SimNow's published two sets.
SIMNOW_MD_FRONTS = frozenset(
    f"tcp://182.254.243.31:{port}" for port in (30011, 30012, 30013, 40011))
SOURCE_ID = 1
CREDENTIAL_ENVIRONMENT = ("YQUANT_CTP_BROKER_ID", "YQUANT_CTP_USER_ID",
                          "YQUANT_CTP_PASSWORD", "YQUANT_CTP_APP_ID",
                          "YQUANT_CTP_AUTH_CODE", "YQUANT_CTP_BRIDGE_TOKEN")


def connection_seconds(value):
    number = positive_seconds(value)
    if number > 60:
        raise argparse.ArgumentTypeError("must not exceed 60 seconds")
    return number


def query_seconds(value):
    number = positive_seconds(value)
    if not 0.01 <= number <= 60:
        raise argparse.ArgumentTypeError("must be in 0.01..60 seconds")
    return number


def parse_arguments():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mode", choices=("live",))
    parser.add_argument("--run-dir", type=Path, required=True,
                        help="new private run directory; existing paths are rejected")
    parser.add_argument("--catalog", type=Path, required=True)
    parser.add_argument("--venue", required=True)
    parser.add_argument("--symbol", required=True)
    parser.add_argument("--md-front", choices=sorted(SIMNOW_MD_FRONTS),
                        default="tcp://182.254.243.31:30011")
    parser.add_argument("--td-front", choices=sorted(SIMNOW_FRONTS),
                        default="tcp://182.254.243.31:30001")
    parser.add_argument("--anonymous-md", action="store_true",
                        help="empty login fields for market data; trader login stays authenticated")
    parser.add_argument("--duration-seconds", type=positive_seconds, default=60)
    parser.add_argument("--connect-timeout-seconds", type=connection_seconds, default=60)
    parser.add_argument("--query-interval-seconds", type=query_seconds, default=1,
                        help="minimum interval between read-only broker queries")
    parser.add_argument("--build-dir", type=Path, default=PROJECT / "build/debug")
    return parser.parse_args()


class SimNowProcesses(Processes):
    """Own CTP bridge environments and the broker-dependent shutdown order."""

    def __init__(self, run, environment, shutdown_seconds):
        super().__init__(run)
        self.environment = environment
        self.shutdown_seconds = shutdown_seconds
        self.shutdown_order = []

    def start(self, name, command, environment=None):
        self.check_signal()
        stdout = (self.run / f"{name}.stdout.log").open("wb")
        self.logs.append(stdout)
        stderr = (self.run / f"{name}.stderr.log").open("wb")
        self.logs.append(stderr)
        child = subprocess.Popen(command, cwd=PROJECT, stdout=stdout, stderr=stderr,
                                 env=environment if environment is not None else self.environment,
                                 start_new_session=True)
        self.children[name] = child
        return child

    def stop(self, name, natural=False):
        child = self.children.get(name)
        if child is None:
            return None
        self.shutdown_order.append(name)
        if natural and child.poll() is None:
            try:
                child.wait(timeout=self.shutdown_seconds)
            except subprocess.TimeoutExpired:
                pass
        if child.poll() is None:
            try:
                os.killpg(child.pid, signal.SIGTERM)
            except ProcessLookupError:
                pass
            try:
                child.wait(timeout=self.shutdown_seconds)
            except subprocess.TimeoutExpired:
                try:
                    os.killpg(child.pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
                child.wait()
                self.stopped_at[name] = utc_now()
                return f"{name} did not complete broker shutdown"
        self.stopped_at[name] = utc_now()
        if child.returncode != 0:
            return f"{name} exited with status {child.returncode}"
        return None

    def reap_after_cleanup_error(self, name):
        """Reap an owned child by PID if its normal group shutdown failed."""
        child = self.children.get(name)
        if child is None:
            return None
        try:
            if child.poll() is None:
                try:
                    child.terminate()
                except ProcessLookupError:
                    pass
            try:
                child.wait(timeout=self.shutdown_seconds)
            except subprocess.TimeoutExpired:
                try:
                    child.kill()
                except ProcessLookupError:
                    pass
                child.wait(timeout=self.shutdown_seconds)
        except (OSError, subprocess.TimeoutExpired) as failure:
            return f"{name} could not be reaped after shutdown failed: {failure}"
        self.stopped_at[name] = utc_now()
        return None

    def cleanup(self):
        errors = []
        # Strategy cancels depend on the gateway and authenticated SDK bridge.
        # Gateway STOP must receive DRAINED before the SDK bridge can exit.
        for name in ("strategyd", "tradingd", "trader_bridge", "marketd", "bridge"):
            try:
                if name == "marketd" and name in self.children and self.children[name].poll() is None:
                    try:
                        self.stop_request_file.touch(exist_ok=False)
                    except OSError as failure:
                        errors.append(f"cannot request market-data bridge shutdown: {failure}")
                natural = name == "bridge" and self.stop_request_file.is_file()
                if name == "trader_bridge":
                    gateway = self.children.get("tradingd")
                    natural = gateway is not None and gateway.returncode == 0
                failure = self.stop(name, natural=natural)
            except OSError as error:
                errors.append(f"{name} cleanup failed: {error}")
                failure = self.reap_after_cleanup_error(name)
            if failure:
                errors.append(failure)
        for log in self.logs:
            try:
                log.close()
            except OSError as failure:
                errors.append(f"cannot close process log: {failure}")
        return errors


def prepare(args, run, manifest):
    """Validate the selected simulated broker and retain the shared catalog."""
    if args.md_front not in SIMNOW_MD_FRONTS or args.td_front not in SIMNOW_FRONTS:
        raise RunError("only the published SimNow endpoints are accepted")
    Credentials.from_environment()
    catalog = run / "instruments.json"
    catalog.write_bytes(args.catalog.resolve().read_bytes())
    instrument = load_instrument(catalog, args.venue, args.symbol)
    manifest.update({
        "source": {"adapter": "ctp", "front": args.md_front, "source_id": SOURCE_ID,
                   "auth_mode": "anonymous" if args.anonymous_md else "credentials"},
        "trading": {"adapter": "ctp", "front": args.td_front,
                    "broker_id": "9999", "auth_mode": "credentials"},
        "instrument": {"venue": args.venue, "symbol": args.symbol},
        "catalog": catalog.name, "catalog_sha256": sha256(catalog),
        "recording": "capture.yqr", "duration_seconds": args.duration_seconds,
        "connect_timeout_seconds": args.connect_timeout_seconds,
        "query_interval_seconds": args.query_interval_seconds})
    return catalog, instrument


def start_bridges(args, run, manifest, processes, catalog, token):
    trader_port = free_port()
    trader_environment = dict(os.environ, YQUANT_CTP_BROKER_ID="9999",
                              YQUANT_CTP_BRIDGE_TOKEN=token)
    command = [sys.executable, "-u", str(PROJECT / "scripts/ctp_trader_bridge.py"),
               "--front", args.td_front, "--catalog", str(catalog),
               "--venue", args.venue, "--symbol", args.symbol,
               "--listen-port", str(trader_port),
               "--connect-timeout-seconds", str(args.connect_timeout_seconds),
               "--query-interval-seconds", str(args.query_interval_seconds)]
    processes.start("trader_bridge", command, trader_environment)
    processes.wait_marker("trader_bridge", "ctp_trader_listening",
                          args.connect_timeout_seconds)
    quote_port = free_port()
    quote_environment = dict(os.environ, YQUANT_CTP_BROKER_ID="9999")
    quote_environment.pop("YQUANT_CTP_BRIDGE_TOKEN", None)
    if args.anonymous_md:
        for name in CREDENTIAL_ENVIRONMENT:
            quote_environment.pop(name, None)
    command = [sys.executable, "-u", str(PROJECT / "scripts/ctp_quote_bridge.py"),
               "--front", args.md_front, "--catalog", str(catalog),
               "--venue", args.venue, "--symbol", args.symbol,
               "--listen-port", str(quote_port), "--source-id", str(SOURCE_ID),
               "--stop-request-file", str(processes.stop_request_file),
               "--connect-timeout-seconds", str(args.connect_timeout_seconds)]
    if args.anonymous_md:
        command.append("--anonymous")
    processes.start("bridge", command, quote_environment)
    processes.wait_marker("bridge", "CTP bridge listening", args.connect_timeout_seconds,
                          ("trader_bridge",))
    return trader_port, quote_port


def start_market(args, run, manifest, processes, binaries, quote_port, catalog):
    runtime = run / "market-runtime"
    runtime.mkdir()
    producer = {"source": {"adapter": "tcp", "address": "127.0.0.1", "port": quote_port},
                "publishers": [
                    {"adapter": "shm", "runtime_directory": str(runtime), "stream": "quotes",
                     "capacity": 16384, "max_payload_size": 80},
                    {"adapter": "record", "path": str(run / "capture.yqr"),
                     "queue_capacity": 1048576}]}
    consumer = {"adapter": "shm", "source_id": SOURCE_ID,
                "instrument_catalog": str(catalog), "instruments": [manifest["instrument"]],
                "runtime_directory": str(runtime), "stream": "quotes", "poll_interval_us": 100}
    write_json(run / "marketd.json", producer)
    write_json(run / "market_data.json", consumer)
    processes.start("marketd", [str(binaries / "yquant-marketd"),
                                "--config", str(run / "marketd.json")])
    processes.wait_marker("marketd", "yquant-marketd: Running", args.connect_timeout_seconds,
                          ("bridge", "trader_bridge"))
    manifest["capture_started_at_utc"] = utc_now()
    write_json(run / "manifest.json", manifest)


def start_strategy(args, run, manifest, processes, binaries, strategy, trader_port, token):
    timeout_ms = max(1, round(args.connect_timeout_seconds * 1000))
    broker = {"adapter": "ctp", "address": "127.0.0.1", "port": trader_port, "token": token,
              "connect_timeout_ms": timeout_ms, "heartbeat_timeout_ms": 5000,
              "shutdown_timeout_ms": timeout_ms}
    write_json(run / "ctp_trading.json", broker)
    strategy_config = json.loads((PROJECT / "examples/strategies/"
                                 "futures_bbo_strategy.json").read_text())
    strategy_config["parameters"].update(manifest["instrument"])
    write_json(run / "strategy.json", strategy_config)
    port = free_port()
    client_token = secrets.token_hex(24)
    gateway = {"listen": {"address": "127.0.0.1", "port": port},
               "market_data_config": str(run / "market_data.json"),
               "trading_config": str(run / "ctp_trading.json"),
               "token": client_token, "shutdown_timeout_ms": timeout_ms}
    client = {"adapter": "tcp", "address": "127.0.0.1", "port": port,
              "client_id": 1, "token": client_token, "shutdown_timeout_ms": timeout_ms}
    write_json(run / "gateway.json", gateway)
    write_json(run / "client.json", client)
    processes.start("tradingd", [str(binaries / "yquant-tradingd"),
                                 "--config", str(run / "gateway.json")])
    processes.wait_marker("tradingd", "yquant-tradingd: Running", args.connect_timeout_seconds,
                          ("bridge", "marketd", "trader_bridge"))
    processes.wait_marker("trader_bridge", "ctp_trader_ready", args.connect_timeout_seconds,
                          ("bridge", "marketd", "tradingd"))
    processes.wait_marker("trader_bridge", "ctp_trader_connected", args.connect_timeout_seconds,
                          ("bridge", "marketd", "tradingd"))
    manifest["trader_ready"] = True
    processes.start("strategyd", [str(binaries / "yquant-strategyd"),
                                  "--strategy", str(strategy),
                                  "--config", str(run / "strategy.json"),
                                  "--market-data-config", str(run / "market_data.json"),
                                  "--trading-config", str(run / "client.json")])


def run_live(args, run, manifest, processes, catalog, binaries, strategy, token):
    trader_port, quote_port = start_bridges(args, run, manifest, processes, catalog, token)
    start_market(args, run, manifest, processes, binaries, quote_port, catalog)
    start_strategy(args, run, manifest, processes, binaries, strategy, trader_port, token)
    for name, marker in (("strategyd", "yquant-strategyd: Running"),
                         ("bridge", "CTP first quote"),
                         ("strategyd", "futures_bbo: both sides submitted"),
                         ("trader_bridge", "ctp_trader_buy_accepted"),
                         ("trader_bridge", "ctp_trader_sell_accepted")):
        processes.wait_marker(name, marker, args.connect_timeout_seconds,
                              tuple(other for other in processes.children if other != name))
    manifest.update(received_valid_quote=True, strategy_submitted_quotes=True,
                    buy_report_received=True, sell_report_received=True,
                    broker_reports_at_utc=utc_now())
    write_json(run / "manifest.json", manifest)
    print(f"SimNow CTP: {args.venue}/{args.symbol}; run directory: {run}", flush=True)
    deadline = time.monotonic() + args.duration_seconds
    while time.monotonic() < deadline:
        processes.check_alive(tuple(processes.children))
        time.sleep(min(POLL_SECONDS, max(0, deadline - time.monotonic())))
    processes.check_alive(tuple(processes.children))


def main():
    args = parse_arguments()
    run = args.run_dir.resolve()
    previous_umask = os.umask(0o077)
    try:
        run.mkdir(mode=0o700, parents=True, exist_ok=False)
    except OSError as failure:
        os.umask(previous_umask)
        print(f"cannot create new run directory {run}: {failure}", file=sys.stderr)
        return 1
    environment = {key: value for key, value in os.environ.items()
                   if key not in CREDENTIAL_ENVIRONMENT}
    processes = SimNowProcesses(run, environment, min(60, max(8, args.connect_timeout_seconds + 2)))
    handlers = {number: signal.signal(number, processes.handle_signal)
                for number in (signal.SIGINT, signal.SIGTERM)}
    manifest = {"schema_version": 1, "mode": "live", "execution": "ctp_simnow",
                "started_at_utc": utc_now(), "success": False,
                "received_valid_quote": False, "trader_ready": False,
                "strategy_submitted_quotes": False,
                "buy_report_received": False, "sell_report_received": False,
                "trader_drained": False}
    token = secrets.token_hex(32)
    instrument = None
    failure = None
    try:
        write_json(run / "manifest.json", manifest)
        binaries, strategy = require_artifacts(args)
        catalog, instrument = prepare(args, run, manifest)
        write_json(run / "manifest.json", manifest)
        run_live(args, run, manifest, processes, catalog, binaries, strategy, token)
    except Exception as error:
        failure = str(error)
    finally:
        errors = processes.cleanup()
        manifest["ended_at_utc"] = utc_now()
        manifest["capture_ended_at_utc"] = processes.stopped_at.get("marketd")
        manifest["shutdown_order"] = processes.shutdown_order
        manifest["process_returncodes"] = {
            name: child.returncode for name, child in processes.children.items()}
        if "trader_bridge" in processes.children:
            try:
                output = processes.output("trader_bridge")
            except OSError as error:
                output = ""
                errors.append(f"cannot read SimNow trader diagnostics: {error}")
            manifest["buy_report_received"] = "ctp_trader_buy_accepted" in output
            manifest["sell_report_received"] = "ctp_trader_sell_accepted" in output
            manifest["trader_drained"] = "ctp_trader_drained" in output
            if not manifest["trader_drained"]:
                errors.append("SimNow trader did not confirm DRAINED")
        recording = run / "capture.yqr"
        if recording.is_file() and instrument is not None:
            try:
                metadata = validate_recording(recording, instrument, SOURCE_ID)
                manifest["recording_metadata"] = metadata
                manifest["recording_sha256"] = metadata["recording_sha256"]
                manifest["received_valid_quote"] = metadata["tradable_rows"] > 0
                count = re.search(r"Stopped events=(\d+)", processes.output("marketd"))
                manifest["recorded_events"] = int(count[1]) if count else None
                if not manifest["received_valid_quote"] or metadata["rows"] != manifest["recorded_events"]:
                    errors.append("recording is empty or differs from marketd's sealed count")
            except Exception as error:
                errors.append(f"recording validation failed: {error}")
        elif failure is None:
            errors.append("market-data recording is missing")
        if processes.signum is not None:
            errors.append(f"received signal {processes.signum}")
        if errors:
            failure = "; ".join(filter(None, [failure, *errors]))
        manifest["success"] = failure is None
        if failure:
            for name in ("YQUANT_CTP_PASSWORD", "YQUANT_CTP_AUTH_CODE"):
                value = os.environ.get(name)
                if value:
                    failure = failure.replace(value, "[redacted]")
            failure = failure.replace(token, "[redacted]")
            manifest["error"] = failure
        try:
            write_json(run / "manifest.json", manifest)
        except OSError as error:
            failure = f"cannot write run manifest: {error}"
        for number, handler in handlers.items():
            signal.signal(number, handler)
        os.umask(previous_umask)
    if failure:
        print(failure, file=sys.stderr)
        return 128 + processes.signum if processes.signum is not None else 1
    print(f"Completed; manifest: {run / 'manifest.json'}", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
