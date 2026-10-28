# Round 2 exploratory findings: correctness

Tested 2026-10-27 on DuckDB 1.5.6 (linked-in shell) and 2.0 (explicit LOAD), from the
repository root. `SELECT 1` succeeded after every error below unless stated otherwise.
Round-1 findings in `correctness.md` / REVIEW_LOG UR-1 were re-checked first.

## Round-1 regression check

- **Historyless cold-start analogs (round-1 #1): FIXED but shallow.** A material with zero
  movement rows is no longer returned. A material with a *single* movement row still is,
  with `history_months = 0` (see F1).
- **Quoted table identifiers (round-1 #2): NOT FIXED.** `cluster_matches('pairs-hyphen')`
  still raises `Binder Error: Invalid identifier for pairs_table: pairs-hyphen` on 1.5.6
  (see F2). The quoted form `'"pairs-hyphen"'` fails the same way.
- **README paste failure (round-1 #3): FIXED.** The full README quick-start pasted top to
  bottom in one session now runs clean on both shells and prints exactly the documented
  rows (`PUMP-B 0.75`, `OLD-PUMP 0.79 / -1.0 / 38`, `OLD-PUMP 1.0 / 9`).

## Findings

### F1 [medium] `cold_start_analogs` returns single-movement materials as forecast analogs

Default `min_history_months := 0` admits a candidate whose entire history is one movement
on one day. `history_months` is a first-to-last span, so it reports 0, yet the row is
presented as an analog "with history to forecast from" (guide 03). A planner who trusts
the default gets an analog with no demand pattern.

Repro (1.5.6, also 2.0):

```sql
CREATE TABLE bom_items(parent_id VARCHAR, child_id VARCHAR, quantity DOUBLE);
INSERT INTO bom_items VALUES
  ('NEW-PROD','COMP-A',2),('NEW-PROD','COMP-B',1),
  ('OLD-1','COMP-A',2),('OLD-1','COMP-B',1),('OLD-1','COMP-C',1),
  ('OLD-2','COMP-A',2),('OLD-2','COMP-B',1);
CREATE TABLE goods_movements(material_id VARCHAR, movement_date DATE, quantity DOUBLE);
INSERT INTO goods_movements VALUES
  ('OLD-1', DATE '2024-01-01', 100), ('OLD-1', DATE '2024-03-01', 100),
  ('OLD-2', DATE '2024-03-01', 5);
SELECT material_id, history_months
FROM cold_start_analogs('NEW-PROD', 10, bom_table := 'bom_items', movements_table := 'goods_movements');
-- OLD-1 | 2
-- OLD-2 | 0   <- one movement, no forecastable history
SELECT 1;  -- works
```

Actual: `OLD-2` returned with `history_months = 0`. Expected: either excluded by default,
or the docs state plainly that the default only requires one movement row. Suggested fix
(code, reported not applied): require a minimum number of distinct movement weeks/days
independent of the span, or change the default `min_history_months` to a positive value.
Doc mitigation applied in `docs/guides/03-predecessors-and-cold-start.md`.

### F2 [medium] Table-name arguments still reject valid quoted identifiers (round-1 #2, open)

```sql
CREATE TABLE "pairs-hyphen"(a VARCHAR, b VARCHAR, score DOUBLE);
INSERT INTO "pairs-hyphen" VALUES ('a', 'b', 0.9);
SELECT * FROM cluster_matches('pairs-hyphen');
-- Binder Error: Invalid identifier for pairs_table: pairs-hyphen
SELECT * FROM cluster_matches('"pairs-hyphen"');
-- same error
SELECT 1;  -- works
```

Actual: both forms rejected. Expected: a quoted identifier naming an existing table is
accepted, as for ordinary DuckDB objects. Imported ERP/CSV tables with hyphens or spaces
cannot be used without renaming. Suggested fix (code): parse the argument as a qualified
identifier with SQL quoting and bind safely instead of validating it as a bare identifier.
Doc mitigation applied in `docs/guides/01-getting-started.md` (rename or view workaround).

### F3 [low] `tree_from_json` silently accepts zero and negative item counts

```sql
SELECT unnest(tree_from_json('{"tree_id":"R2","root":{"node_id":"R2","kind":"item","label":"X","count":0}}'));
-- count 0.0, no error
SELECT unnest(tree_from_json('{"tree_id":"R3","root":{"node_id":"R3","kind":"item","label":"X","count":-2}}'));
-- count -2.0, no error
SELECT 1;  -- works
```

Actual: counts 0 and -2 load without complaint. `validate_trees` does flag a zero count,
but a bid engineer who skips validation feeds a meaningless count into `tree_coverage`.
Expected: `tree_from_json` rejects non-positive counts at load, naming the node (as
`tree_multiset` already does for non-finite counts). Suggested fix (code): validate
`count > 0` and finite in `tree_from_json`, consistent with the kernel.

### F4 [low] Empty-set conventions differ between sibling scalars

```sql
SELECT jaccard_similarity([], []) AS j;   -- 0.0
SELECT containment([], []) AS c;          -- NULL
SELECT containment(['a'], []) AS c2;      -- 0.0
SELECT containment(NULL, ['a']) AS c3;    -- NULL
SELECT 1;  -- works
```

Actual: two empty sets score Jaccard 0.0 but containment NULL, with no error. A user
averaging scores silently drops the NULL rows and biases the mean. Expected: one
documented convention for empty/NULL across the set measures. Suggested fix (code/docs):
align on the HLD §11.6 rule and state it per function in `docs/api/01-similarity-scalars.md`
and `docs/api/10-covering-search.md`.

## What was verified clean

- Guides 02, 03, 05, 07 (first examples) and guide 04 (stage 1, `tree_anchors`,
  `tree_from_json` German example, stage 2 with `mu := 0.0` and `0.2`): every documented
  table reproduced exactly on 1.5.6, including `S7-PRED-OLD 0.837 / -0.983 / 25` and the
  `Q1` coverages `1.0 / 1.0 / 0.5` (`0.858` at default `mu`).
- Guide 01 Tversky trap: 1.5.6 swapped call gives 0.5 as documented; 2.0 binds by name
  and gives 1.0, as documented.
- All 17 examples pass via `scripts/run_examples.sh` on both 1.5.6 and 2.0.
- Guide 08 scale path: 20 000-entity MinHash index + bands + 100-query LSH search runs
  in about 1.4 s total on 1.5.6 and finds all 100 sources at rank 1, matching the guide.
- Error liveness: negative MAP weights, categorical `value_satisfies`, malformed JSON,
  NaN struct input, NULL/duplicate/NULL-score pairs in `cluster_matches`, and a cyclic
  tree all produce clean errors or empty results; `SELECT 1` works after each. No
  INTERNAL errors or crashes found. Cycles are reported by `validate_trees`
  (`cycle`, `no_root`, `unreachable`) and `tree_coverage` returns no rows.
- Persona (a) with returns/negative quantities, gaps, duplicate BOM edges and a NULL
  child: no crash; `infer_predecessors` returns no rows on sparse data as the guide warns.

## Verdict per persona

- (a) Planner: reaches the goal with README + guides 01-03 on own data. Most slowed by
  F1: the default analog list can contain a one-movement material with no warning.
- (b) Bid engineer: reaches the goal. Hand-written German JSON, OR alternatives and
  counts load and rank correctly. Most slowed by F3 (silent bad counts) and by needing
  `validate_trees` as a separate, easy-to-skip step.
- (c) Data scientist: reaches the goal. Exact-vs-LSH numbers in guide 08 reproduce;
  no scale blocker at 20k entities. No finding.
- (d) Skeptical reviewer: documented outputs checked above match both shells. Blocked
  only by F2 (still open from round 1) and the F4 convention gap.

Fixes applied (docs only): clarified `history_months` semantics in
`docs/guides/03-predecessors-and-cold-start.md` and the quoted-identifier limitation in
`docs/guides/01-getting-started.md`. No example files needed changes; all 17 re-verified
by the runs above before the doc edits (doc edits touch no SQL).
