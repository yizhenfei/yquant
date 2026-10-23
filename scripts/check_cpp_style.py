#!/usr/bin/env python3
"""Checks machine-verifiable C++ rules from docs/coding-styles.md."""

import argparse
from pathlib import Path
import re
import subprocess


# Blank literals/comments while retaining line numbers for diagnostics.
NON_CODE = re.compile(
    r'R"([^ ()\\\t\r\n]{0,16})\(.*?\)\1"'
    r'|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\''
    r'|//[^\n]*|/\*.*?\*/', re.DOTALL
)
RULES = (
    (r"\busing\s+namespace\b", "use individual using declarations"),
    (r"^\s*#\s*pragma\s+once\b", "use a project header guard"),
    (r"^thread_local\b", "namespace thread_local requires constinit"),
    (r"\b\d+(?:ns|us|ms|s|min|h)\b", "use an explicit chrono duration"),
    (r"\b(?:std::)?(?:printf|fprintf|snprintf|sprintf)\s*\(",
     "use yquant/base/format.h for type-checked formatting"),
)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--clang-format", default="clang-format")
    options = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    files = sorted(
        path for directory in ("include", "src", "tests", "examples")
        for path in (root / directory).rglob("*")
        if path.suffix in (".cc", ".h")
    )
    violations = 0
    for path in files:
        text = path.read_text()
        if re.search(r'^\s*#\s*include\s*[<"]filesystem[>"]', text, re.MULTILINE):
            print(f"{path.relative_to(root)}: <filesystem> is disallowed")
            violations += 1
        if re.search(r'^\s*#\s*include\s*"\.\.?/', text, re.MULTILINE):
            print(f"{path.relative_to(root)}: use an include-root-relative path")
            violations += 1
        if path.suffix == ".h":
            relative = str(path.relative_to(root))
            if relative.startswith("include/yquant/"):
                relative = relative.removeprefix("include/yquant/")
            elif relative.startswith("src/"):
                relative = relative.removeprefix("src/")
            guard = "YQUANT_" + re.sub(r"[^A-Za-z0-9]", "_", relative).upper() + "_"
            if (f"#ifndef {guard}" not in text or f"#define {guard}" not in text):
                print(f"{path.relative_to(root)}: expected header guard {guard}")
                violations += 1
        code = NON_CODE.sub(lambda m: re.sub(r"[^\n]", " ", m[0]), text)
        for pattern, message in RULES:
            for match in re.finditer(pattern, code, re.MULTILINE):
                line = code.count("\n", 0, match.start()) + 1
                print(f"{path.relative_to(root)}:{line}: {message}")
                violations += 1
    result = subprocess.run(
        [options.clang_format, "--dry-run", "--Werror", *map(str, files)],
        cwd=root,
    )
    print(f"Checked {len(files)} C++ files; {violations} rule violations.")
    return bool(violations or result.returncode)


if __name__ == "__main__":
    raise SystemExit(main())
