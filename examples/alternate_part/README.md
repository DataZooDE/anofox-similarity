# Alternate Part Qualification

Rheinwerk Ersatzteile (fictional) has a pump seat unavailable for the next maintenance window. A procurement analyst receives eight proposed alternates from fictional suppliers. The nightly job must decide which can be purchased, which fail a hard requirement, and which need a human to repair the catalogue extract. A matching name or where-used relationship is not an interchangeability approval.

## Data

Run `cd scripts && uv run python -m scripts.examples.alternate_part` to regenerate the checked-in CSVs. The generator uses no randomness and writes sorted, plain CSV rows.

| File | Rows | Purpose |
|------|-----:|---------|
| `data/part_master.csv` | 9 | One request and eight candidate parts; designations contain M8x1,25 and A4-70. |
| `data/part_characteristics.csv` | 73 | Eight request requirements, eight per candidate, and one conflicting duplicate thread key. |
| `data/compatibility_rules.csv` | 2 | Only the approved PN16→PN40 and certificate 3.1→3.2 upgrades. Exact matches need no row. |
| `data/gold_verdicts.csv` | 8 | Expected buyer decisions and reasons. It is used only to check the result. |

The extract mixes `PN 16` with `PN16`, `DN 50` with `DN50`, German decimal commas with English decimal points, and `0.75 in` with millimetres. It includes a missing certificate, a conflicting duplicate key, a tighter `±` tolerance, a bore below its minimum, and a `not to exceed 5 bar` limit violated by 6 bar. The category normalisers are tied to known attribute keys; they do not treat standards codes as quantities.

## Run

From the repository root, with the linked DuckDB 1.5.6 shell:

```bash
DATAZOO_DISABLE_TELEMETRY=1 /home/jr/Projects/datazoo/anofox-similarity/build/release/duckdb < examples/alternate_part/01_qualify.sql
```

The script is self-contained and uses `.bail on`. Read the four sections in order: extract counts and duplicate audit; parser audit; required versus observed values for failed or missing checks; verdicts and the gold comparison. The expected distribution is 2 ACCEPT, 4 REJECT, 2 REVIEW, with zero gold mismatches. A duplicate or missing value takes precedence over a purchase decision. The score is an attribute gate, not a probabilistic confidence score.

## Limits and workarounds

- For hard numeric limits, pass zero tolerance and zero soft band **positionally** to `value_satisfies` on DuckDB 1.5.6. The default 2% tolerance can approve a small overshoot.
- `0.75 in` parses as inches because `in` is the last word; inside a phrase (`3 in 1`) it stays a word. Check unusual unit spellings with `parse_quantity` first.
- The bore requirement is `at least 19.05 mm`, a one-sided limit. A real fit needs a minimum and a maximum, written as two attributes.
- A category must have an attribute key and curated rules. No implicit PN or certificate upgrade is accepted. Missing and duplicate keys go to REVIEW; a procurement owner must resolve them before ordering.
