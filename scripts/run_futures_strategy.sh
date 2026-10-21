#!/usr/bin/env bash
set -euo pipefail

usage() {
  printf 'Usage: %s [speed]\n' "${0##*/}"
  printf 'Replay DCE/a1805 quotes with the simulated futures exchange.\n'
  printf 'Speed: integer 1..1000, default 100. Exits after replay and order draining.\n'
}

if [[ $# -eq 1 && ( "$1" == -h || "$1" == --help ) ]]; then
  usage
  exit 0
fi
if [[ $# -gt 1 ]]; then usage >&2; exit 2; fi
speed="${1:-100}"
if [[ ! "$speed" =~ ^[0-9]{1,4}$ ]]; then usage >&2; exit 2; fi
speed=$((10#$speed))
if [[ "$speed" -lt 1 || "$speed" -gt 1000 ]]; then usage >&2; exit 2; fi

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
project_dir="$(cd -- "$script_dir/.." && pwd -P)"
strategyd="$project_dir/build/debug/bin/yquant-strategyd"
strategy="$project_dir/build/debug/strategies/futures_bbo_strategy.so"
if [[ ! -x "$strategyd" || ! -r "$strategy" ]]; then
  printf 'Build Debug strategy artifacts from %s:\n' "$project_dir" >&2
  printf '  cmake --preset debug\n  cmake --build --preset debug\n' >&2
  exit 1
fi
if ! command -v python3 >/dev/null 2>&1; then
  printf 'python3 is required to prepare the replay configuration.\n' >&2
  exit 1
fi
run_dir="$(mktemp -d "${TMPDIR:-/tmp}/yquant-futures-run.XXXXXX")"

# Keep the exact strategy/account/market configuration with the retained log.
python3 - "$project_dir" "$run_dir" "$speed" <<'PY'
import json
from pathlib import Path
import sys

project, run = map(Path, sys.argv[1:3])
for name in ("futures_bbo_strategy", "futures_trading", "futures_market_data"):
    template = project / "examples" / "strategies" / (name + ".json")
    config = json.loads(template.read_text(encoding="utf-8"))
    if name == "futures_market_data":
        config["replay_path"] = str((template.parent / config["replay_path"]).resolve())
        config["replay_speed"] = int(sys.argv[3])
        catalog = (template.parent / config["instrument_catalog"]).resolve()
        (run / "instruments.json").write_bytes(catalog.read_bytes())
        config["instrument_catalog"] = str(run / "instruments.json")
    (run / (name + ".json")).write_text(json.dumps(config, indent=2) + "\n", encoding="utf-8")
PY

printf 'Instrument: DCE/a1805\nReplay speed: %s\nRun directory: %s\nLog: %s/run.log\n' \
  "$speed" "$run_dir" "$run_dir"
"$strategyd" \
  --strategy "$strategy" \
  --config "$run_dir/futures_bbo_strategy.json" \
  --market-data-config "$run_dir/futures_market_data.json" \
  --trading-config "$run_dir/futures_trading.json" \
  2>&1 | tee -i "$run_dir/run.log"
