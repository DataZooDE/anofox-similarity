#!/usr/bin/env python3
"""Check that the rows an example documents in its `-- Expected:` comments are rows of its output.

    python3 scripts/check_expected.py examples/quantities/value_satisfies.sql output.txt

An expected block is a comment line containing "Expected:" and the lines right after it that
contain '|' (table rows). A table row is reduced to the list of its numbers, in order, single digits
included (trailing zeros ignored: 0.50 matches 0.5); text in parentheses, percentages and a note
after the last cell (three or more spaces) are explanation and skipped, in the row and in the output
alike. It passes when an output line (the script runs in -list mode, one line per row) has the same
numbers in the same order, or contains them as one run (at least two: a row may leave out
leading or trailing columns). A row without numbers (a header) is not checked. Prose after "Expected:" (no '|') only
needs each of its numbers with two or more digits, or a decimal point, somewhere in the output.
"(varies)" in a line excludes it. Exits 1 and lists what is missing.
"""

import re
import sys

NUMBER = re.compile(r"(?<![\w.])-?\d+(?:\.\d+)?(?:e[-+]?\d+)?(?![\w.])")


def normal(n):
    if "e" in n.lower():
        return repr(float(n))
    return n.rstrip("0").rstrip(".") if "." in n else n


def numbers(text):
    return [normal(n) for n in NUMBER.findall(text)]


def strip_explanations(text):
    text = re.sub(r"\([^)]*\)", " ", text)
    return re.sub(r"-?\d+(\.\d+)?\s*%", " ", text)


def clean(row):
    """The cells of an expected row without its explanations and its trailing note."""
    row = strip_explanations(row)
    head, sep, last = row.rpartition("|")
    last = re.split(r"\s{3,}", last.strip(), maxsplit=1)[0]
    return head + sep + " " + last


def expected_rows(sql):
    """(text, numbers, is_table) per expected line."""
    rows, in_block = [], False
    for line in sql.splitlines():
        stripped = line.strip()
        if not stripped.startswith("--"):
            in_block = False
            continue
        if "Expected:" in stripped:
            in_block = True
            text = stripped.split("Expected:", 1)[1]
            if not text.strip():
                continue
        elif in_block and "|" in stripped:
            text = stripped[2:]
        else:
            in_block = False
            continue
        if "(varies)" in text:
            continue
        if re.fullmatch(r"[\s|:-]*", text):  # a markdown separator row
            continue
        if "|" in text:
            nums = numbers(clean(text))
            if nums:
                rows.append((text.strip(), nums, True))
        else:
            prose = strip_explanations(text)
            nums = [n for n in numbers(prose) if "." in n or len(n.lstrip("-")) >= 2]
            if nums:
                rows.append((text.strip(), nums, False))
    return rows


def contains(line, run):
    """Is run a contiguous part of line? (a row may leave out leading or trailing columns)"""
    return any(line[i:i + len(run)] == run for i in range(len(line) - len(run) + 1))


def missing_rows(sql, output):
    """The expected lines of sql that the output does not show."""
    lines = [tuple(numbers(strip_explanations(line))) for line in output.splitlines()]
    anywhere = {n for line in lines for n in line} | set(numbers(output))
    missing = []
    for text, nums, is_table in expected_rows(sql):
        if is_table:
            t = tuple(nums)
            if not any(l == t or (len(t) >= 2 and contains(l, t)) for l in lines):
                missing.append(text)
        elif any(n not in anywhere for n in nums):
            missing.append(text)
    return missing


def main():
    sql = open(sys.argv[1], encoding="utf-8").read()
    output = open(sys.argv[2], encoding="utf-8").read()
    missing = missing_rows(sql, output)
    if missing:
        print("expected but not in the output: " + " ;; ".join(missing[:8]))
        sys.exit(1)


if __name__ == "__main__":
    main()
