#!/usr/bin/env bash
# Run every examples/*/*.sql file through the locally built DuckDB shell.
#
# Usage (from the repository root):
#   scripts/run_examples.sh                 # all examples
#   scripts/run_examples.sh quantities      # only folders whose name contains "quantities"
#   DUCKDB=/path/to/duckdb scripts/run_examples.sh
#
# A file fails when the shell exits non-zero or its output contains "Error"
# (DuckDB prints e.g. "Binder Error:" and keeps going in a script). The
# script prints one line per file and a summary, and exits 1 on any failure.

set -uo pipefail

cd "$(dirname "$0")/.." || exit 1

DUCKDB="${DUCKDB:-./build/release/duckdb}"
FILTER="${1:-}"

if [[ ! -x "$DUCKDB" ]]; then
    echo "DuckDB shell not found at $DUCKDB (build with 'make release' or set DUCKDB=...)" >&2
    exit 1
fi

export DATAZOO_DISABLE_TELEMETRY=1

passed=0
failed=0
failed_files=()

shopt -s nullglob
files=(examples/*/*.sql)
if [[ ${#files[@]} -eq 0 ]]; then
    echo "No example files found under examples/" >&2
    exit 1
fi

for file in "${files[@]}"; do
    if [[ -n "$FILTER" && "$file" != *"$FILTER"* ]]; then
        continue
    fi
    start=$(date +%s.%N)
    output=$("$DUCKDB" < "$file" 2>&1)
    status=$?
    elapsed=$(awk -v s="$start" -v e="$(date +%s.%N)" 'BEGIN { printf "%.1f", e - s }')

    if [[ $status -ne 0 ]] || grep -q "Error" <<< "$output"; then
        failed=$((failed + 1))
        failed_files+=("$file")
        printf 'FAIL  %-60s %6ss\n' "$file" "$elapsed"
        grep -n -m 5 "Error" <<< "$output" | sed 's/^/      /'
    else
        passed=$((passed + 1))
        printf 'ok    %-60s %6ss\n' "$file" "$elapsed"
    fi
done

echo
echo "Examples: $((passed + failed)) run, $passed passed, $failed failed"
if [[ $failed -gt 0 ]]; then
    printf '  failed: %s\n' "${failed_files[@]}"
    exit 1
fi
