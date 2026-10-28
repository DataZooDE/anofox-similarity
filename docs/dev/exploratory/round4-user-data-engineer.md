# Round 4: alternate part, data engineer notes

Verdict: the folder demonstrates a usable nightly MRO qualification gate. The CSV extract is small but messy enough to exercise audited parsing, approved category upgrades, hard numeric limits, missing values, conflicting duplicate keys, and a gold comparison. A customer would still ask for supplier approval, effective dates, lot-level certificates, and a maintained unit and category mapping table before treating ACCEPT as a purchase authorisation.

Run each repro from the repository root with the supplied linked DuckDB 1.5.6 shell:

```bash
DATAZOO_DISABLE_TELEMETRY=1 /home/jr/Projects/datazoo/anofox-similarity/build/release/duckdb
```

## Observed surprises

| Severity | Minimal SQL repro | Actual | Expected and suggested fix |
|----------|-------------------|--------|----------------------------|
| High | `SELECT value_satisfies(parse_quantity('19.05 mm'), parse_quantity('19.05 mm'), NULL, 0, 0).score;` | `0.0` | `1.0` for an exact point. Handle zero tolerance and zero distance as a pass. Until fixed, express a one-sided limit explicitly or model a min and max pair. |
| Medium | `SELECT parse_quantity('1.4404').lo, parse_quantity('1.4404').dimension;` | `1.4404`, `count` | A steel designation should not become a scalar count. Return unknown for a designation pattern, or require a declared attribute type before numeric parsing. This example maps material as a category. |
| Medium | `SELECT parse_quantity('0.75 in').dimension;` | `NULL` | `length`, normalised to 0.01905 m. Add `in`/inch to the built-in unit seed or provide a documented user unit table. This example converts the known source field explicitly. |
| Medium | `SELECT value_satisfies(parse_quantity('not to exceed 5 bar'), parse_quantity('6 bar'), NULL, 0, 0).score;` | `0.0` | Correct with strict settings. The default 2% tolerance can pass slight overshoots; an explicit strict mode or named hard-limit preset would reduce accidental approvals. |

The fourth row records a dangerous default rather than an incorrect result in this repro. The example's SQL passes `NULL, 0.0, 0.0` positionally because scalar named parameters do not bind by name on DuckDB 1.5.6.

## Design decisions and risks

- The input remains four flat tables, including a gold file. A duplicate key or a missing attribute produces REVIEW before any ACCEPT/REJECT decision. The output retains both observed and required text on failure rows.
- Category normalisation is keyed by attribute. The rules table contains only two approved upgrades; all other unequal category pairs fail. No similarity score can silently approve a substitute.
- The bore is a minimum, not a full geometric fit. Production qualification needs upper and lower dimensional limits, mating assembly context, and signed supplier rules.
- A missing certificate is a blank cell and a conflicting duplicate is an extra row. Both are ordinary extract defects that should stop an automated purchase.
