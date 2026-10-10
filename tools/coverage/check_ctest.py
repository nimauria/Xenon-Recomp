#!/usr/bin/env python3
"""Require a nonempty CTest JUnit report with no failed, errored, or skipped tests."""

from __future__ import annotations

import argparse
import sys
import xml.etree.ElementTree as ET
from pathlib import Path


def check(path: Path) -> tuple[int, int, int, int]:
    root = ET.parse(path).getroot()
    tests = root.findall(".//testcase")
    if not tests:
        raise ValueError("CTest report has no test cases")
    failures = sum(bool(test.findall("failure")) for test in tests)
    errors = sum(bool(test.findall("error")) for test in tests)
    skipped = sum(bool(test.findall("skipped")) for test in tests)
    return len(tests), failures, errors, skipped


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("report", type=Path)
    args = parser.parse_args()
    try:
        total, failed, errors, skipped = check(args.report)
    except (OSError, ValueError, ET.ParseError) as exc:
        print(f"CTest report invalid: {exc}", file=sys.stderr)
        return 1
    print(f"CTest: {total} tests, {failed} failed, {errors} errors, {skipped} skipped")
    return int(bool(failed or errors or skipped))


if __name__ == "__main__":
    raise SystemExit(main())
