# C-parts catalogue harmonisation

> A buyer groups duplicate catalogue lines before negotiating prices, while keeping uncertain substitutions for engineering review.

## Decision

A fictional manufacturer, Morgenwerk, receives overlapping DIN and ISO fastener catalogues from several suppliers. Its buyer wants one price comparison per interchangeable part. The trigger is a quarterly purchasing refresh; the output is a set of **candidate** purchasing groups and a review queue. A cluster is not a technical approval.

## Data

Generate the checked-in CSVs with `cd scripts && uv run python -m scripts.examples.c_parts_harmonisation`. The generator is deterministic and writes plain CSV, sorted by its first column.

| File | Rows | Columns and purpose |
|------|-----:|---------------------|
| `data/parts.csv` | 91 | `part_id`, `supplier`, `description`, raw standard/thread/length/grade, `unit_price_eur` |
| `data/standard_synonyms.csv` | 8 | Reviewed `standard_raw` → `standard_family_norm` mapping, including DIN 933 ≈ ISO 4017 |
| `data/gold_clusters.csv` | 91 | `part_id`, reviewed `gold_cluster`, `decision` for checking the result |

There are 40 approved duplicate pairs, eight singleton specials, and a three-line review chain. German and English descriptions, one `hex blot` typo, `M 8` versus `M8`, millimetres versus decimal-comma centimetres, and DIN/ISO synonyms exercise the normalisation. The review chain has M8x25, an unknown length, and M8x30; two descriptions state negated maximum lengths. A blank length on a washer or nut is normal; on the review chain it is missing evidence.

## Run

From the repository root, using the DuckDB 1.5.6 shell with the extension linked in:

```bash
DATAZOO_DISABLE_TELEMETRY=1 /path/to/build/release/duckdb < examples/c_parts_harmonisation/01_guarded_clusters.sql
```

The script loads its own data. It first maps standard synonyms and parses thread, length, and grade keys. It makes exact-key pairs, calls `cluster_matches`, checks those clusters against gold, and prints the five largest supplier price spreads. It then permits a missing length as a wildcard solely to show the transitive trap, and checks every resulting cluster for conflicting known lengths.

Look for **91 lines → 51 exact clusters**, with all **40 approved pairs** recovered. The largest observed price spread is **59.1%**; it is a negotiation lead, not a saving estimate. The loose policy joins `CP-X25`, `CP-XUNK`, and `CP-X30`; the whole-cluster guard marks all three **REVIEW**.

## Limits and workarounds

- `cluster_matches` groups scored edges by transitive closure. It does not enforce cross-member attribute consistency. Keep the guard, and only promote clusters with complete reviewed specifications to an approved catalogue.
- Synonym mappings are maintained data. Validate additions against your standards catalogue; similarity of descriptions does not prove standards equivalence.
- Regex extraction handles this fixture's `mm` and `cm` forms. A production import should reject unrecognised units and malformed threads, and confirm grade and coating rules with engineering.
- The free-text maximum-length phrases are visible to reviewers, but this SQL does not parse them into a compliance decision. The structured `length_raw` column controls grouping.
