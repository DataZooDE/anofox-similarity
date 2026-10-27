#!/usr/bin/env bash
# Run every examples/*/*.sql file through the locally built DuckDB shell.
#
# Usage (from the repository root):
#   scripts/run_examples.sh                 # all examples
#   scripts/run_examples.sh quantities      # only folders whose name contains "quantities"
#   DUCKDB=/path/to/duckdb scripts/run_examples.sh
#
# A file fails when the shell exits non-zero, its output contains "Error" (DuckDB prints e.g.
# "Binder Error:" and keeps going in a script), or a number of its "-- Expected:" comments is not in
# its output (scripts/check_expected.py). The script prints one line per file and a summary, and
# exits 1 on any failure.

set -uo pipefail

cd "$(dirname "$0")/.." || exit 1

DUCKDB="${DUCKDB:-./build/release/duckdb}"
FILTER="${1:-}"

if [[ ! -x "$DUCKDB" ]]; then
    echo "DuckDB shell not found at $DUCKDB (build with 'make release' or set DUCKDB=...)" >&2
    exit 1
fi

export DATAZOO_DISABLE_TELEMETRY=1
# DuckDB 2.0's shell switches to "agent mode" output when it detects an AI agent
unset AI_AGENT CLAUDECODE

# The DuckDB 1.5.6 shell built here has the extension linked in; the 2.0 shell does not, so
# load the extension built next to it.
SHELL_ARGS=()
if ! "$DUCKDB" -noheader -list -c "SELECT 1 FROM duckdb_functions() WHERE function_name = 'tree_coverage' LIMIT 1" 2>/dev/null | grep -q 1; then
    EXT="$(dirname "$DUCKDB")/extension/anofox_similarity/anofox_similarity.duckdb_extension"
    if [[ ! -f "$EXT" ]]; then
        echo "The shell at $DUCKDB has no anofox_similarity and $EXT does not exist" >&2
        exit 1
    fi
    SHELL_ARGS=(-unsigned -cmd "LOAD '$EXT'")
fi

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
    output=$("$DUCKDB" "${SHELL_ARGS[@]}" < "$file" 2>&1)
    status=$?
    elapsed=$(awk -v s="$start" -v e="$(date +%s.%N)" 'BEGIN { printf "%.1f", e - s }')

    expected_check=$(python3 scripts/check_expected.py "$file" <(printf '%s' "$output") 2>&1)
    if [[ $status -ne 0 ]] || grep -qE "Error|exception_type" <<< "$output" || [[ -n "$expected_check" ]]; then
        failed=$((failed + 1))
        failed_files+=("$file")
        printf 'FAIL  %-60s %6ss\n' "$file" "$elapsed"
        grep -n -m 5 "Error" <<< "$output" | sed 's/^/      /'
        [[ -n "$expected_check" ]] && echo "      $expected_check"
    else
        passed=$((passed + 1))
        printf 'ok    %-60s %6ss\n' "$file" "$elapsed"
    fi
done

echo
echo "Examples: $((passed + failed)) run, $passed passed, $failed failed"
if [[ $((passed + failed)) -eq 0 ]]; then
    echo "No example matched '$FILTER'" >&2
    exit 1
fi
if [[ $failed -gt 0 ]]; then
    printf '  failed: %s\n' "${failed_files[@]}"
    exit 1
fi
