#!/usr/bin/env python3
"""Run a strategy example with dedicated market/trading daemons and retained logs."""

import argparse
import json
import signal
import socket
import subprocess
import sys
import time
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("run-dir", "strategy", "config", "market-data-config", "trading-config"):
        parser.add_argument("--" + name, required=True, type=Path)
    parser.add_argument("--marketd-config", type=Path)
    args = parser.parse_args()
    project = Path(__file__).resolve().parent.parent
    binaries = project / "build/debug/bin"
    run = args.run_dir.resolve()
    run.mkdir(parents=True, exist_ok=True)
    children = []
    files = []
    interrupted = False

    def stop(_signum, _frame):
        nonlocal interrupted
        interrupted = True
        # Stop the strategy first so the gateway can finish its cancels.
        if strategy is not None and strategy.poll() is None:
            strategy.terminate()

    def start_daemon(name, config, marker):
        log_path = run / (name + ".log")
        log = log_path.open("w")
        files.append(log)
        process = subprocess.Popen([str(binaries / ("yquant-" + name)), "--config", str(config)],
                                   cwd=project, stdout=log, stderr=subprocess.STDOUT,
                                   start_new_session=True)
        children.append(process)
        deadline = time.monotonic() + 5
        while process.poll() is None and time.monotonic() < deadline:
            if marker in log_path.read_text():
                return
            time.sleep(0.01)
        raise RuntimeError(f"{name} startup failed: {log_path.read_text()}")

    strategy = None
    signal.signal(signal.SIGINT, stop)
    signal.signal(signal.SIGTERM, stop)
    try:
        market_path = args.market_data_config.resolve()
        if args.marketd_config:
            runtime = run / "market-runtime"
            runtime.mkdir()
            producer = json.loads(args.marketd_config.read_text())
            receiver = json.loads(market_path.read_text())
            for output in producer["publishers"]:
                if output["adapter"] == "shm":
                    output["runtime_directory"] = str(runtime)
            receiver["runtime_directory"] = str(runtime)
            producer_path, market_path = run / "marketd.json", run / "market_data.json"
            producer_path.write_text(json.dumps(producer, indent=2) + "\n")
            market_path.write_text(json.dumps(receiver, indent=2) + "\n")
            start_daemon("marketd", producer_path, "Running")
        with socket.socket() as listener:
            listener.bind(("127.0.0.1", 0))
            port = listener.getsockname()[1]
        server = {"listen": {"address": "127.0.0.1", "port": port},
                  "market_data_config": str(market_path),
                  "trading_config": str(args.trading_config.resolve()),
                  "token": "example-gateway", "shutdown_timeout_ms": 5000}
        client = {"adapter": "tcp", "address": "127.0.0.1", "port": port,
                  "client_id": 1, "token": "example-gateway", "shutdown_timeout_ms": 5000}
        server_path, client_path = run / "gateway.json", run / "client.json"
        server_path.write_text(json.dumps(server, indent=2) + "\n")
        client_path.write_text(json.dumps(client, indent=2) + "\n")
        start_daemon("tradingd", server_path, "Running")
        if interrupted:
            return 0
        command = [str(binaries / "yquant-strategyd"), "--strategy", str(args.strategy.resolve()),
                   "--config", str(args.config.resolve()), "--market-data-config",
                   str(market_path), "--trading-config", str(client_path)]
        strategy = subprocess.Popen(command, cwd=project, stdout=subprocess.PIPE,
                                    stderr=subprocess.STDOUT, text=True, start_new_session=True)
        children.append(strategy)
        with (run / "run.log").open("w") as log:
            for line in strategy.stdout:
                print(line, end="", flush=True)
                log.write(line)
        return strategy.wait()
    except (OSError, RuntimeError) as error:
        print(error, file=sys.stderr)
        return 1
    finally:
        for child in reversed(children):
            if child.poll() is None:
                child.terminate()
            try:
                child.wait(timeout=6)
            except subprocess.TimeoutExpired:
                child.kill()
                child.wait()
        for log in files:
            log.close()


if __name__ == "__main__":
    sys.exit(main())
