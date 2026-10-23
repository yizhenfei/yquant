#!/usr/bin/env python3
"""Compiles each project header alone using its consumer's build flags."""

import argparse
import concurrent.futures
import json
from pathlib import Path
import shlex
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("build_directory", type=Path)
    parser.add_argument("--jobs", type=int, default=4)
    options = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    commands = json.loads(
        (options.build_directory / "compile_commands.json").read_text()
    )
    consumers = []
    for command in commands:
        source = Path(command["file"])
        if source.is_relative_to(root) and "build" not in source.relative_to(root).parts:
            consumers.append((command, source.read_text()))
    headers = sorted(
        path for directory in ("include", "src", "tests")
        for path in (root / directory).rglob("*.h")
    )

    def check(header):
        names = {header.name}
        for directory in (root, root / "include", root / "src", root / "tests"):
            if header.is_relative_to(directory):
                names.add(str(header.relative_to(directory)))
        candidates = [
            command for command, text in consumers
            if any(f'"{name}"' in text for name in names)
        ]
        command = candidates[0] if candidates else consumers[0][0]
        args = command.get("arguments") or shlex.split(command["command"])
        flags = []
        skip_next = False
        for arg in args:
            if skip_next:
                skip_next = False
                continue
            if arg in ("-o", "-MF", "-MT", "-MQ"):
                skip_next = True
            elif arg not in ("-c", "-MD", "-MMD", "-MP", command["file"]):
                flags.append(arg)
        with tempfile.TemporaryDirectory(prefix="yquant-header-") as directory:
            source = Path(directory) / "header.cc"
            source.write_text(f'#include "{header}"\n')
            result = subprocess.run(
                flags + ["-fsyntax-only", str(source)],
                cwd=command["directory"], capture_output=True, text=True,
            )
        return header, result

    failures = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=options.jobs) as pool:
        for header, result in pool.map(check, headers):
            if result.returncode:
                failures.append(header)
                print(f"{header.relative_to(root)}:\n{result.stderr}")
    print(f"Checked {len(headers)} headers; {len(failures)} failed.")
    return bool(failures)


if __name__ == "__main__":
    raise SystemExit(main())
