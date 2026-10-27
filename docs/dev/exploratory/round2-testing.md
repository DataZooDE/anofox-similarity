# Exploratory user testing, round 2: verification persona

Shells under test (run from the repository root `/home/jr/Projects/datazoo/anofox-similarity`):

```bash
# DuckDB 1.5.6, extension linked in
DATAZOO_DISABLE_TELEMETRY=1 ./build/release/duckdb
# DuckDB 2.0, extension loaded explicitly
env -u AI_AGENT -u CLAUDECODE DATAZOO_DISABLE_TELEMETRY=1 ../anofox-similarity-v2/build/release/duckdb -unsigned \
  -cmd "LOAD '/home/jr/Projects/datazoo/anofox-similarity-v2/build/release/extension/anofox_similarity/anofox_similarity.duckdb_extension'"
```

Method:

1. `scripts/run_examples.sh` on both shells, then every example's output compared by hand with its
   `-- Expected:` comments, and the 1.5.6 and 2.0 outputs diffed against each other.
2. Every ```` ```sql ```` block of `README.md` and `docs/guides/01..08` extracted and run in order, one
   session per page, on both shells. The numbers in the documented output tables were checked
   against the real output by a script, and every mismatch was checked by hand.
3. `scripts/generate_api_reference.py --check` against 1.5.6. The extension's catalogue
   (`duckdb_functions()`: names, kinds, parameters, return types) diffed between 1.5.6 and 2.0.
4. Depth check of the round-1 fixes (`test/sql/hardening/user_review_regressions.test`): probes next to
   each tested case.
5. User probes: a hand-written German JSON specification with OR alternatives and counts (bid
   engineer), cold start with returns and ERP date formats (planner), exact versus LSH at 400k and
   2M rows (data scientist).

Severity: **high** = confidently wrong results by default; **medium** = wrong or silent results in
realistic use, or a doc that misleads; **low** = rough edges.

## Summary of what holds

- All 17 examples pass on both shells, and every `-- Expected:` value matches the output on 1.5.6
  and on 2.0. The outputs of the two versions are identical (the bootstrap CI too, although
  `evaluate_pipeline.sql` warns that it may differ).
- All guide and README blocks run on both shells. The only errors are the documented one in
  guide 04 (a query without classes) and the DuckPGQ recipe on the 2.0 dev build (see R2-T9).
  Every documented output table matches the real output.
- The generated per-function reference is up to date (`--check`: 13 pages). The 92 extension
  signatures are identical on 1.5.6 and 2.0; the only difference is that 2.0 quotes the struct field
  `"distance"` in `value_satisfies`' return type.
- Round-1 fixes hold beyond their tests: `tree_coverage_trace` and `coverage_recall` also reject an
  unknown or unclassified query; `classify_to_catalogue` handles a NULL source; `MAP {'a': NULL}`
  works; `PC`, `EA`, `STK`, `CM`, `KM`, `IK 08`, `10 MIN`, `1,5 KG` parse; WL-kernel ties come in id
  order on repeated calls; an unknown id gives 0 rows in both searches.
- No crash and no INTERNAL error; `SELECT 1` worked after every error.

---

## Code findings (not fixed: C++ is out of scope)

### R2-T1 [high] German bound words `mind.`, `ab`, `bis <x>` are not recognised, so met requirements score 0

```sql
SELECT t, parse_quantity(t, 'de').bound
FROM (VALUES ('mind. 50.000 Zyklen'), ('mind. 5 A'), ('ab 5 A'), ('bis 5 A'), ('über 5 A'),
             ('kleiner 5 A'), ('größer gleich 5 A')) v(t);
--   all NULL   (mindestens / min. / ≥ / höchstens / nicht mehr als are recognised)
SELECT value_satisfies(parse_quantity('mind. 50.000 Zyklen', 'de'), parse_quantity('100.000 Zyklen', 'de'));
--   {'score': 0.0, 'distance': 1.0, 'rule': point}
--   expected {'score': 1.0, 'distance': 0.0, 'rule': at_least}, as with 'mindestens 50.000 Zyklen'
```

`mind.` is the most common German abbreviation in specifications, so a German spec without catalogue
templates reports false gaps. Found by writing a spec by hand (`tree_from_json`, see the bid-engineer
section). The guide claimed `bis` is recognised; it is only recognised as a range (`5 bis 10 A`).
Suggested fix: add `mind.` and `ab` (at_least) and a leading `bis`, `unter`, `kleiner (als)` (at_most),
`über`, `größer (als)`, `größer gleich` (at_least) to the German bound words, with a regression test
per word. Doc workaround added (guide 05).

### R2-T2 [medium] A partially classified query silently counts its unclassified items as missing

```sql
CREATE TABLE tree_nodes AS SELECT * FROM read_csv_auto('test/data/coverage/mini/tree_nodes.csv');
CREATE TABLE tree_attributes AS SELECT * FROM read_csv_auto('test/data/coverage/mini/tree_attributes.csv');
CREATE TABLE partial AS SELECT * EXCLUDE (class_id), CASE WHEN node_id = 'Q1.1' THEN class_id END AS class_id FROM tree_nodes;
SELECT candidate_id, round(coverage, 3), n_requirements, n_gaps
FROM tree_coverage('Q1', candidates := ['S1'], nodes_table := 'partial', candidate_nodes_table := 'tree_nodes');
--   S1 | 0.208 | 4 | 3     (S1 meets all of Q1)
SELECT req_node_id, matched_node_id, is_gap
FROM tree_coverage_trace('Q1', candidates := ['S1'], nodes_table := 'partial', candidate_nodes_table := 'tree_nodes');
--   Q1.2 | NULL | true   ... indistinguishable from a module S1 really lacks
```

The round-1 fix (UR-1 F1–F5) covers only "no item classified". With 1 of 4 items classified, the
result is quietly wrong and the trace gives no hint why. This is the realistic case: a classifier
leaves the uncertain items out, or a vocabulary misses one wording. Suggested fix: add an
`n_unclassified` column to `tree_coverage`, and an `unclassified` flag (or `gap_reason`) to
`tree_coverage_trace`. Regression test: the query above, asserting both. Doc note added (guide 04).

### R2-T3 [medium] `classify_to_catalogue` centring misclassifies every item of a large single-class source

```sql
CREATE TABLE cat3 AS SELECT * FROM (VALUES ('pump', [1.0, 0.0, 0.0]), ('motor', [0.0, 1.0, 0.0]), ('valve', [0.0, 0.0, 1.0])) t(class_id, vec);
CREATE TABLE mixed AS
SELECT 'big' || i AS node_id, 'B' AS source, [1.0, 0.2, 0.0 + i / 100] AS vec FROM range(12) t(i)   -- 12 pumps from one supplier
UNION ALL SELECT 's1', 'S', [0.0, 1.0, 0.1] UNION ALL SELECT 's2', 'S', [0.1, 0.0, 1.0];
SELECT node_id, class_id, round(p, 3) FROM classify_to_catalogue(items_table := 'mixed', catalogue_table := 'cat3', top := 1)
WHERE node_id IN ('big0', 'big5');
--   big0 | motor | 0.5      big5 | motor | 0.5      (they are pumps; center_by_source := false says pump)
```

The round-1 fix (`min_source_items`, default 10) only protects small sources. A source with 10 or
more items that are all of one class (a supplier who sells only pumps, a request that only asks for
seat heating) loses exactly the direction that identifies its class. Suggested fix: centre with a
shrunk mean (blend the source mean with the batch mean by source size or by spread), or skip
centring when the source's items are tightly clustered; at least document that per-source
centring assumes mixed classes per source. Regression test: the query above must return `pump`.

### R2-T4 [low] `threshold_at_precision` ignores queries with gold but no results; `eval_ranking` counts them

```sql
CREATE TABLE g(query_id VARCHAR, candidate_id VARCHAR, true_coverage DOUBLE);
CREATE TABLE r(query_id VARCHAR, candidate_id VARCHAR, rank BIGINT, coverage DOUBLE);
INSERT INTO g VALUES ('Q1','A',1.0), ('Q2','C',0.9), ('Q2','D',0.2);
INSERT INTO r VALUES ('Q1','A',1,0.9);
SELECT n_queries, top_k_accuracy, mean_regret FROM eval_ranking('r', 'g', k := [1]);   -- 2 | 0.5 | 0.45 (Q2 is a miss)
SELECT threshold, precision, accepted_share, n_queries FROM threshold_at_precision('r', 'g');
--   0.9 | 1.0 | 1.0 | 1      "100 % auto-accepted", although Q2 has no pick at all
```

Converse of round-1 F15. `accepted_share` overstates the automation rate when the pipeline drops
requests (for example, a stage-1 filter returns nothing). Suggested fix: count gold queries without
results in the denominator of `accepted_share` (never accepted), as `eval_ranking` does.

### R2-T5 [low] A text `movement_date` leaks a `date_part` binder error

```sql
CREATE TABLE bom_items AS SELECT * FROM (VALUES ('Q','c1'), ('M1','c1')) t(parent_id, child_id);
CREATE TABLE goods_movements AS SELECT * FROM (VALUES ('M1', '20240101', 5.0)) t(material_id, movement_date, quantity);
SELECT * FROM cold_start_analogs('Q', 10);
--   Binder Error: No function matches ... 'date_part(STRING_LITERAL, VARCHAR)' + 14 candidate functions
```

SAP extracts deliver `BUDAT` as `'YYYYMMDD'` text. Expected: "cold_start_analogs: movements_table
column movement_date must be DATE or TIMESTAMP, got VARCHAR". Same check for `infer_predecessors`.
Doc note with the `strptime` conversion added (guide 03).

### R2-T6 [low] `history_months` counts calendar boundaries and every posting, including returns

```sql
CREATE TABLE bom_items AS SELECT * FROM (VALUES ('Q','c1'), ('M1','c1'), ('M2','c1')) t(parent_id, child_id);
CREATE TABLE goods_movements AS SELECT * FROM (VALUES
  ('M1', DATE '2024-01-01', -5.0), ('M1', DATE '2024-09-01', -4.0),     -- only returns
  ('M2', DATE '2024-01-31', 5.0), ('M2', DATE '2024-02-01', 0.0)) t(material_id, movement_date, quantity);
SELECT material_id, history_months FROM cold_start_analogs('Q', 10);
--   M1 | 8     M2 | 1     (M2 has one day of history)
```

`src/modules/similarity_search.cpp:354` computes the year/month difference of first and last
posting over all rows. A planner reads "8 months of consumption history". Suggested fix: count
distinct months with positive consumption, or document it (done in guide 03).

### R2-T7 [low] `parse_quantity` leaves SAP upper-case `M` and `G` without a dimension; `IP 6K9K` has no category

```sql
SELECT t, parse_quantity(t).unit, parse_quantity(t).dimension, parse_quantity(t).category
FROM (VALUES ('3 M'), ('5 G'), ('IP 6K9K'), ('IP6K9K')) v(t);
--   3 M -> M, NULL     5 G -> G, NULL     IP 6K9K -> category NULL
```

Round 1 (UR-1 F10) added `KG`, `MM`, `CM`, `KM`, `ST`, `PC`, but not `M` (metre) and `G` (gram), the
two most common SAP ISO codes. They are ambiguous with prefixes only as prefixes, not as whole
units. Low because the unit is a gate (score 0), not a silent wrong value.

### R2-T8 [low] Building a MinHash index over 2 million rows peaks at about 5 GB RSS

```sql
CREATE TABLE part_bom AS
SELECT DISTINCT 'A' || lpad(i::VARCHAR, 5, '0') AS entity_id, 'P' || (hash(i, j) % 200000) AS feature_id, 1.0 AS weight
FROM range(100000) t(i), range(20) u(j);
CREATE TABLE part_minhash AS SELECT * FROM minhash_index(feature_table := 'part_bom');   -- 3.8 s, 39 s CPU
CREATE TABLE part_bands AS SELECT * FROM minhash_bands('part_minhash');                  -- 1.7 s
```

Peak RSS of the shell process 5.0 GB (sampled from `/proc/<pid>/status`). The exact batch join over
the same data needs a fraction of that and is faster (R2-T10). Suggested fix: profile
`minhash_index`'s memory; document the cost in guide 08.

### R2-T9 [low] DuckPGQ recipe cannot run on the DuckDB 2.0 dev build

`INSTALL duckpgq FROM community` returns HTTP 404 for the 2.0 dev commit (`d54cbbe3df`), so the
recipe fails with `syntax error at or near "PROPERTY"`. Environmental (no 2.0 build of DuckPGQ yet),
but guide 08 promises a 2.0 path (`SET active_grammar_extensions`) that no one could verify. The
1.5.6 run works.

---

## Doc / example findings (fixed in this change, each verified on 1.5.6 and 2.0)

| # | Sev | Where | Problem | Fix |
|---|---|---|---|---|
| R2-T10 | medium | guide 08 "Exact Search First", "Search Many Queries" | Says LSH pays off for many queries and that exact search for 100 queries takes "about 8 s". A user cannot loop `find_covering_entities_for` (no LATERAL), and a plain exact batch join runs the 100 queries in 0.014–0.034 s, ten times faster than LSH (0.22 s). At 100k BOMs / 2M rows: exact 0.02–0.06 s, LSH 0.47 s plus 5.5 s for index and bands | New section "Exact Batch Search" with the join, its output (100 of 100 found) and measured times; the LSH advice now says "measure before you switch" |
| R2-T11 | medium | guide 05 "Rules to know" | Claimed `bis` is a bound word; `bis 5 A` gives `bound` NULL. `mind.`, `ab` not mentioned (R2-T1) | Exact list of recognised and unrecognised words, plus a Pitfalls row with a `regexp_replace` workaround (verified: `mind.` → at_least, `ab` → at_least, `bis` → at_most, `5 bis 10 A` stays within) |
| R2-T12 | medium | guide 04 "Where Classes Come From" | Did not say that a partially classified query is scored with its unclassified items as gaps (R2-T2) | Paragraph and a `tree_anchors(...) WHERE class_id IS NULL` check with its output |
| R2-T13 | low | guide 03 table setup and cold start | `movement_date` type, `history_months` definition and the role of returns were undocumented (R2-T5, R2-T6) | Notes with the `strptime` conversion and the exact definition |

## Verification gaps (tooling)

- `scripts/run_examples.sh` fails only on "Error" in the output. It never compares the
  `-- Expected:` comments, so a changed number goes unnoticed. Every example matched today, but only
  because it was checked by hand. Nothing runs the script in CI (`grep -rn run_examples .github` finds
  nothing), and the guides' SQL blocks are not run by any test.
- `test/sql/hardening/doc_examples.test` and `readme_quickstart.test` pin the README and API pages;
  the guides 01–08 have no such test.

## Verdicts per user

- **(d) Skeptical reviewer**: reached the goal. Every documented number in examples, guides and the
  README reproduces on 1.5.6 and 2.0, and the function catalogue matches the docs. What slowed the
  check: no automated comparison, so it took a script and a manual pass.
- **(b) Bid engineer**: reached the goal with the guide 04 example. With a hand-written German spec, OR
  alternatives, counts, ranges, `IP 54` and `°C` intervals parse correctly and `validate_trees` is
  clean, but `mind. 50.000 Zyklen` silently becomes a point value that fails a better offer (R2-T1).
  Blocker: R2-T1.
- **(c) Data scientist**: evaluation and fitting reproduce exactly. Scaling advice was the
  problem: guide 08 led to LSH where a simple exact join is faster and exact (R2-T10, fixed in the
  doc). `accepted_share` overstates automation when queries drop out (R2-T4).
- **(a) Planner**: the guides are followable; analogs and predecessors work on own data with
  DATE/TIMESTAMP dates. Slowed by an opaque error for text dates (R2-T5) and by a `history_months`
  that counts returns and month boundaries (R2-T6).
