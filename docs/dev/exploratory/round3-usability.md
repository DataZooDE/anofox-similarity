# Exploratory user testing, round 3: generic ERP newcomer

Tested from the README first screen, then guides 01, 02 and 04 and API pages 02, 10, 12 and 13. The persona has a flat Dynamics-style BOM `(ProductNumber, ComponentNumber, QtyPer)` and offer lines `(OfferNumber, RequirementCode, Units)`. Both DuckDB 1.5.6 (linked extension) and 2.0 (explicit LOAD) gave the same results below. The only files changed are documentation; no C++ or tests were changed.

Approximate hands-on time, excluding repository build: README to first BOM result 5 minutes (finding the expected column names and making a view); offer lines to first coverage ranking 7 minutes (finding the feature table parameters and distinguishing `score` from `containment`); finding the way to check value limits and gaps 6 minutes (property-tree schema and class IDs). Each SQL execution was under one second. The key guesses were whether quantities affect Jaccard (no), whether a requirement ID can live in the same feature table (yes), and whether `score = 1` means complete coverage (no; use `containment` or `min_score := 1.0`).

## Findings

### U3-1 — High — a common minimum abbreviation without punctuation becomes a point

Adjacent to the fixed UR-2 G1 cases, `mind` without the period is still silently accepted as an unbounded point. In an offer check this marks a satisfying value as a gap.

```sql
SELECT t, parse_quantity(t, 'de').bound AS bound,
       value_satisfies(parse_quantity(t, 'de'), parse_quantity('7 cm')).score AS score
FROM (VALUES ('mind. 6 cm'), ('mind 6 cm'), ('mindestens 6 cm'), ('min 6 cm')) v(t);
```

Actual on both shells: `mind. 6 cm | at_least | 1`; `mind 6 cm | NULL | 0`; `mindestens 6 cm | at_least | 1`; `min 6 cm | at_least | 1`. Expected: `mind 6 cm` also has `at_least` and scores 1. Suggested code fix: accept the unpunctuated abbreviation, then test it through `tree_from_json` and `tree_coverage_trace` as well as `parse_quantity`.

### U3-2 — Medium — the README left the first ERP column mapping to inference (fixed in docs)

The README demo required literal `bom_items(parent_id, child_id)` and fixture property trees. A newcomer with `ProductNumber` and `ComponentNumber` had to discover `bom_table` in the material guide; for an offer list, they had to discover `feature_table`, `entity_col`, `feature_col`, and `weight_col` in the covering-search API. The README now includes a runnable mapping for both common tasks and explains the raw coverage measure.

Before-fix repro for the BOM path:

```sql
CREATE TABLE erp_bom AS SELECT * FROM (VALUES ('P-100', 'MOTOR'), ('P-200', 'MOTOR'))
  t(ProductNumber, ComponentNumber);
SELECT * FROM find_similar_materials_jaccard('P-100', 5, bom_table := 'erp_bom');
```

Actual: `Binder Error: bom_table 'erp_bom' is missing required column(s): parent_id, child_id`. This error states the expected names but does not show that a view is sufficient. Expected user path: make a mapping view and pass its name. Suggested fix: done in `README.md`; the new complete examples return `P-200 | 0.667` and `OFFER-A | 1.0`, `OFFER-B | 0.333` on both shells with `-bail`.

### U3-3 — Low — intentional JSON validation errors tell users to report a bug

```sql
SELECT len(tree_from_json('{"tree_id":"X","root":{"node_id":"X","kind":"container","childs":[]}}'));
SELECT len(tree_from_json('{"tree_id":"X","root":{"node_id":"X","kind":"item","count":0}}'));
SELECT 1;
```

Actual on both shells: each `Invalid Input Error` correctly names the node and correction (for `childs`: `did you mean 'children'?`), then appends `-> Unexpected? Please report it: https://github.com/DataZooDE/anofox-similarity/issues`. `SELECT 1` still works. Expected: the actionable validation message alone. Suggested code fix: remove the unexpected-error suffix from deliberate input validation.

### U3-4 — Low — the SAP tutorial index still promised HNSW after its page was corrected (fixed in docs)

`docs/guides/tutorials/sap/README.md` described guide 04 as “HNSW indexing, embeddings” while the linked page explicitly says there is no HNSW workflow for Jaccard and teaches exact search plus MinHash/LSH. Expected: the index describes the page and functions a reader can run. Suggested fix: done; the index now says “Exact Jaccard search, MinHash and LSH.”

## UR-2 fix adjacency checks

- **G1, G2, G12:** `mind.`, `ab`, `über`, `größer als`, `kleiner gleich`, `bis` retain bounds; `1 000 N`, NBSP and narrow NBSP parse as 1000 N; `Sek.`, `U/min`, `Zoll` normalize. U3-1 is the remaining neighboring form found.
- **G6:** With the default `tree_attributes` present, `tree_coverage` and `tree_coverage_trace` both reject an explicit `nodes_table := 'alt_nodes'` without `attrs_table`, naming the missing parameter and `attrs_table := NULL` escape. Both shells agree.
- **G7:** `childs` and `count := 0` both fail with a node-specific error; see U3-3 for the misleading suffix.
- **G8:** `cold_start_analogs` and `infer_predecessors` both name a VARCHAR `movement_date` and recommend conversion in a view. The example `strptime(budat, '%Y%m%d')` remains SAP-specific; Dynamics users must substitute their date column and format.
- **G4:** The rewritten SAP scale page is discoverable and describes available functions; its parent index needed U3-4.
- **G5:** With duplicate `('c','a')` BOM lines, unit-weight `minhash_index` estimates full containment for `c` (1.0) and the exact `find_covering_entities` score is also 1.0; this held on both shells. A candidate with only `a` scored about 0.484 estimated versus 0.5 exact, within sampling error.
- **G10:** `scripts/run_examples.sh` passed all 17 examples, including their expected-output checks, on both shells (17/17 each).
- **G3, G9, G11:** Not independently exercised beyond the passing examples in this persona's small ERP walkthrough.

## Verdict

Both goals are reachable in under 30 minutes when component/class IDs are already comparable. BOM search and count-based requirement coverage work on both shells. Value limits and attribute-level gaps require a property tree, class IDs and separate node/attribute tables; the README now points directly to that step. The largest remaining risk is the silent `mind 6 cm` direction error.
