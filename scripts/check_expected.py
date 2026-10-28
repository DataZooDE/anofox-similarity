#!/usr/bin/env python3
"""Check that the numbers an example documents in its `-- Expected:` comments appear in its output.

    python3 scripts/check_expected.py examples/quantities/value_satisfies.sql output.txt

An expected block is a comment line containing "Expected:" and the table rows (comment lines
with '|') right after it; text in parentheses and percentages are explanation and skipped. Every
decimal number and every integer with two or more digits in it must occur in the output (trailing zeros ignored: 0.50 matches 0.5). Exits 1 and lists the missing numbers.
A number that legitimately differs between runs is excluded by writing "(varies)" in its line.
"""

import re
import sys

NUMBER = re.compile(r"-?\d+\.\d+|(?<![\w.])-?\d{2,}(?![\w.])")


def normal(n):
    return n.rstrip("0").rstrip(".") if "." in n else n


def expected_numbers(sql):
    """Numbers of the Expected: line and of the table rows (lines with '|') that follow it.
    Explanations are skipped: text in parentheses and percentages."""
    numbers, in_block = [], False
    for line in sql.splitlines():
        stripped = line.strip()
        if not stripped.startswith("--"):
            in_block = False
            continue
        if "Expected:" in stripped:
            in_block = True
            text = stripped.split("Expected:", 1)[1]
        elif in_block and "|" in stripped:
            text = stripped
        else:
            in_block = False
            continue
        if "(varies)" in text:
            continue
        text = re.sub(r"\([^)]*\)", " ", text)
        text = re.sub(r"-?\d+(\.\d+)?\s*%", " ", text)
        numbers += NUMBER.findall(text)
    return numbers


def main():
    sql = open(sys.argv[1], encoding="utf-8").read()
    output = open(sys.argv[2], encoding="utf-8").read()
    found = {normal(n) for n in NUMBER.findall(output)}
    missing = sorted({n for n in expected_numbers(sql) if normal(n) not in found})
    if missing:
        print("expected but not in the output: " + ", ".join(missing[:20]))
        sys.exit(1)


if __name__ == "__main__":
    main()
