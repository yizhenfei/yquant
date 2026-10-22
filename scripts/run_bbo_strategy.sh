#!/usr/bin/env bash
set -euo pipefail

usage() {
  printf 'Usage: %s [fill|working]\n' "${0##*/}"
  printf 'Run the Debug BBO strategy; default mode is fill. Stop with Ctrl+C.\n'
}

if [[ $# -eq 1 && ( "$1" == -h || "$1" == --help ) ]]; then
  usage
  exit 0
fi

if [[ $# -gt 1 ]]; then
  usage >&2
  exit 2
fi

mode="${1:-fill}"
case "$mode" in
  fill|working) ;;
  *) usage >&2; exit 2 ;;
esac

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
project_dir="$(cd -- "$script_dir/.." && pwd -P)"
strategyd="$project_dir/build/debug/bin/yquant-strategyd"
strategy="$project_dir/build/debug/strategies/bbo_market_making_strategy.so"
strategy_config="$project_dir/examples/strategies/bbo_market_making_strategy.json"
market_data_config="$project_dir/examples/strategies/market_data.json"
trading_template="$project_dir/examples/tradingd/bbo_mock_trading.json"

if [[ ! -x "$strategyd" || ! -r "$strategy" ]]; then
  printf 'Debug strategy artifacts are missing. Build from %s:\n' \
    "$project_dir" >&2
  printf '  cmake --preset debug\n' >&2
  printf '  cmake --build --preset debug\n' >&2
  exit 1
fi

if ! command -v python3 >/dev/null 2>&1; then
  printf 'python3 is required to prepare the temporary trading config.\n' >&2
  exit 1
fi

run_dir="$(mktemp -d "${TMPDIR:-/tmp}/yquant-bbo-run.XXXXXX")"
trading_config="$run_dir/trading.json"
log_file="$run_dir/run.log"

# Keep the source example unchanged and retain the exact config beside its log.
python3 - "$trading_template" "$mode" > "$trading_config" <<'PY'
import json
import sys

with open(sys.argv[1], encoding="utf-8") as source:
    config = json.load(source)
config["mode"] = sys.argv[2]
json.dump(config, sys.stdout, indent=2)
sys.stdout.write("\n")
PY

printf 'Mode: %s\nConfig: %s\nLog: %s\nStop with Ctrl+C.\n' \
  "$mode" "$trading_config" "$log_file"

python3 "$script_dir/run_gateway_strategy.py" \
  --run-dir "$run_dir" --strategy "$strategy" --config "$strategy_config" \
  --market-data-config "$market_data_config" --trading-config "$trading_config" \
  --marketd-config "$project_dir/examples/marketd/local.json"
