# Exploratory user testing: verification persona

Build under test: `/home/jr/Projects/datazoo/anofox-similarity/build/release/duckdb` (v1.5.6) with
the release extension. Every repro starts with

```sql
-- DATAZOO_DISABLE_TELEMETRY=1 /home/jr/Projects/datazoo/anofox-similarity/build/release/duckdb
LOAD '/home/jr/Projects/datazoo/anofox-similarity/build/release/extension/anofox_similarity/anofox_similarity.duckdb_extension';
```

Method: every ```` ```sql ```` block of `README.md` and `docs/API_REFERENCE.md` was extracted and run, the
README sequentially in one session (as a copy-paster would). The coverage workflows ran on
`test/data/coverage`. Adversarial probes covered types, NULLs, empty tables, views, temp tables,
attached read-only DBs, unicode and quoted identifiers, and 100k-row inputs. No crash and no INTERNAL
error was found. `SELECT 1` worked after every error. Timings are from a loaded machine, so they
use `user` time.

Severity: **high** = confidently wrong results by default; **medium** = wrong or silent results
in realistic use, or a doc that fails; **low** = rough edges.

---

## Code findings (not fixed: C++ is out of scope)

### F1 [high] `classify_to_catalogue` default `center_by_source := true` mislabels items in small sources

With one item per source (or a NULL source), centring subtracts the item from itself. Two
near-identical items in one source are pushed apart, so one of them gets a confident wrong
class.

```sql
CREATE TABLE cat(class_id VARCHAR, vec FLOAT[]);
INSERT INTO cat VALUES ('pump',[1,0,0]),('valve',[0,1,0]),('motor',[0,0,1]);
CREATE TABLE one(node_id VARCHAR, vec FLOAT[]); INSERT INTO one VALUES ('i1',[1,0,0]);
SELECT class_id, p, margin FROM classify_to_catalogue(items_table := 'one', catalogue_table := 'cat') WHERE rank = 1;
--   actual: motor | 0.333 | 0.0         (the item IS the pump vector)
--   center_by_source := false gives pump | 0.998
CREATE TABLE two(node_id VARCHAR, vec FLOAT[], source VARCHAR);
INSERT INTO two VALUES ('i1',[1,0,0],'s'),('i2',[0.9,0.1,0],'s');
SELECT node_id, class_id, round(p,3) FROM classify_to_catalogue(items_table := 'two', catalogue_table := 'cat') WHERE rank = 1;
--   actual: i1 pump 0.983, i2 valve 0.983  (i2 is a pump)
```

Expected: a pump stays a pump. Fix options: centre only sources with at least N items (for example 20), and leave
smaller ones uncentred. Alternatively, centre with the mean over the whole items table, or raise an error that
names the source. The minimum is to document that centring needs many items per source.

### F2 [medium] `classify_to_catalogue` silently accepts vectors of a different dimension

```sql
CREATE TABLE cat(class_id VARCHAR, vec FLOAT[]);
INSERT INTO cat VALUES ('pump',[1,0,0]),('valve',[0,1,0]),('motor',[0,0,1]);
CREATE TABLE items2(node_id VARCHAR, vec FLOAT[]); INSERT INTO items2 VALUES ('i1',[1,0]);
SELECT class_id, p, rank FROM classify_to_catalogue(items_table := 'items2', catalogue_table := 'cat', center_by_source := false);
--   actual: uniform p = 1/3 for every class, margin 0, no error
```

Expected: an error such as "item 'i1' has 2 dimensions, the catalogue has 3". (A model mix-up
produces exactly this situation.)

### F3 [medium] Coverage on cross-language attribute names caps a fully compliant offer below 1.0

Without `name_vectors`, name similarity is token overlap. In the mini fixture the request
names are English and the offer names German, so `name_sim` = 0. Each satisfied attribute
then scores `1 - mu` = 0.8. S1 meets every value but gets coverage 0.858, with `n_gaps = 0`.

```sql
CREATE TABLE tree_nodes AS SELECT * FROM read_csv_auto('/home/jr/Projects/datazoo/anofox-similarity/test/data/coverage/mini/tree_nodes.csv');
CREATE TABLE tree_attributes AS SELECT * FROM read_csv_auto('/home/jr/Projects/datazoo/anofox-similarity/test/data/coverage/mini/tree_attributes.csv');
SELECT candidate_id, coverage, n_gaps FROM tree_coverage('Q1', candidates := ['S1']);          -- 0.858, 0 gaps
SELECT req_node_id, attr_name, prov_value, name_sim, value_score, attr_score
FROM tree_coverage_trace('Q1', candidates := ['S1']);   -- value_score 1.0, attr_score 0.8
SELECT coverage FROM tree_coverage('Q1', candidates := ['S1'], mu := 0);                        -- 1.0
```

Expected: an offer that meets every requirement reads 100 %. Otherwise users cannot tell "80 %
covered" from "names in another language". The design says names only discount (BR-16).
Possible fixes: use the name similarity only to choose between same-dimension attributes, or
default `mu` to 0 when no `name_vectors` are given. Whichever is chosen, the docs need to say
that `coverage` < 1 with `n_gaps = 0` means a name discount.

### F4 [medium] `tree_coverage` / `coverage_recall` / `tree_coverage_trace` return 0 rows when the query has no classes, or the id is unknown

```sql
CREATE TABLE tree_nodes AS SELECT * EXCLUDE (class_id), CASE WHEN tree_id LIKE 'Q%' THEN NULL ELSE class_id END AS class_id
FROM read_csv_auto('/home/jr/Projects/datazoo/anofox-similarity/test/data/coverage/mini/tree_nodes.csv');
CREATE TABLE tree_attributes AS SELECT * FROM read_csv_auto('/home/jr/Projects/datazoo/anofox-similarity/test/data/coverage/mini/tree_attributes.csv');
SELECT count(*) FROM tree_coverage('Q1');        -- 0
SELECT count(*) FROM tree_coverage('NOPE');      -- 0
```

This is the trap the API reference's own vocabulary workflow fell into (D3): the benchmark's
`request_nodes` have empty `class_id`. Expected: an error, or at least a row per query that says
"0 of N items have a class (pass label_table or overrides_table)". Suggested fix: raise a binder
error when a requested query tree does not exist, and another when none of its items resolves a
class.

### F5 [medium] `parse_quantity` resolves German thousands separators as English by default

```sql
SELECT parse_quantity('1.500 kg').lo, parse_quantity('1.500 kg', 'de').lo, parse_quantity('1.500 kg', ['de','en']).lo;
--   1.5 | 1500.0 | 1500.0
SELECT tree_from_json('{"root": {"kind":"item","node_id":"n1","label":"x","attributes":[{"attr_id":"a1","name":"Gewicht","text":"1.500 kg"}]}}', 'T1')[1].attributes[1].lo;
--   1.5   (a JSON without "lang")
```

The default `['en','de']` silently picks English, which is a factor-1000 error for German specs
with no language tag. This is documented, but it is an easy trap for the main audience (DACH
Mittelstand). Suggested fix: when both languages are allowed and the reading is ambiguous
(`d.ddd` with exactly three digits), return NULL `lo`/`hi` and flag it (for example, with a
`category` or a warning field). Alternatively, make the list order decide, so that `['de','en']`
and `['en','de']` differ.

### F6 [medium] `parse_quantity` misses common ERP notations

```sql
SELECT t, parse_quantity(t).lo, parse_quantity(t).unit, parse_quantity(t).dimension, parse_quantity(t).category
FROM (VALUES ('5 KG'),('2 ST'),('2 PC'),('3 M'),('3 MM'),('IP 54'),('5 in'),('5 Mio. Zyklen'),('3 x M8')) t(t);
```

| text | actual | expected |
|---|---|---|
| `5 KG`, `3 MM`, `3 M` | unit kept, dimension NULL (gate) | mass / length (SAP MEINS uses upper case: KG, MM, M, ST, PC) |
| `2 ST`, `2 PC` | dimension NULL | count (`Stück` already works) |
| `IP 54` | `lo = 54`, dimension count | `category = 'IP54'` (the space is common) |
| `5 in` | dimension NULL | length (inch) |
| `5 Mio. Zyklen` | `lo = 5`, unit `Mio` | 5 000 000 count |

An `IP 54` misread as a count passes `value_satisfies` for any count requirement, where it should
be a categorical gate. Suggested fix: match units case-insensitively when that is unambiguous (not `M` vs `m`
for mega/milli prefixes: map whole SAP ISO codes instead), add the ST/PC/inch aliases,
accept `IP 54`, and support `Mio.`/`Tsd.` multipliers.

### F7 [medium] LOAD writes 4 tables and 3 HNSW indexes into the user's persistent database

```bash
rm -f /tmp/u.db
DATAZOO_DISABLE_TELEMETRY=1 duckdb -unsigned /tmp/u.db -c "LOAD '/home/jr/Projects/datazoo/anofox-similarity/build/release/extension/anofox_similarity/anofox_similarity.duckdb_extension';"
duckdb /tmp/u.db -c "SELECT table_name FROM duckdb_tables();"
#   material_embeddings, material_embeddings_dirty, transactional_embedding_statistics, transactional_feature_mapping
duckdb /tmp/u.db -c "INSERT INTO material_embeddings (material_id) VALUES ('x');"
#   Missing Extension Error: Cannot bind index 'material_embeddings', unknown index type 'HNSW'
```

A user who only wanted `containment()` now has a database that stock DuckDB (without vss) can
no longer write to. Expected: LOAD has no side effects on the catalog. Suggested fix: create
the infrastructure tables lazily, through an explicit `CALL anofox_similarity_init()` or the first
macro that needs them, or in the `temp` catalog.

### F8 [medium] Quoted / non-plain identifiers are rejected by every `*_table` parameter

```sql
CREATE TABLE "BOM-2024"(entity_id VARCHAR, feature_id VARCHAR, weight DOUBLE); INSERT INTO "BOM-2024" VALUES ('E1','x',1),('E2','x',1);
SELECT * FROM find_covering_entities('E1', 5, feature_table := 'BOM-2024');       -- Invalid identifier
SELECT * FROM find_covering_entities('E1', 5, feature_table := '"BOM-2024"');     -- Invalid identifier
CREATE SCHEMA erp; CREATE TABLE erp."BOM Items"(parent_id VARCHAR, child_id VARCHAR);
SELECT * FROM find_similar_materials_jaccard('M1', 3, bom_table := 'erp."BOM Items"');  -- Invalid identifier
```

Unicode plain names (`Stückliste`), `schema.table`, `db.schema.table` and reserved words
(`Order`) work. Exports from SAP and Excel often have hyphens or spaces in table names. The workaround is a
view. Suggested fix: accept double-quoted parts in `ValidateSQLIdentifierPath`, and keep rejecting
`;` and unbalanced quotes.

### F9 [medium] `bom_explosion_multilevel` gives no level and no extended quantity

```sql
CREATE TABLE bom_header(bom_id VARCHAR, parent_material_id VARCHAR);
CREATE TABLE bom_component(bom_id VARCHAR, child_material_id VARCHAR, quantity_per DOUBLE);
INSERT INTO bom_header VALUES ('B1','BIKE'),('B2','WHEEL'),('B4','HUB');
INSERT INTO bom_component VALUES ('B1','WHEEL',2),('B1','SADDLE',1),('B2','SPOKE',36),('B2','HUB',1),('B4','BEARING',2);
SELECT * FROM bom_explosion_multilevel('BIKE');
--   (parent, child, quantity_per) per edge, unordered: WHEEL→SPOKE 36, HUB→BEARING 2, ...
```

An ERP analyst expects `level` and the extended quantity per top unit (72 spokes, 4 bearings),
ordered by path. Neither is available, and the order is nondeterministic. Also,
`max_depth := 0` and `max_depth := -1` return level 1 silently. Suggested fix: add `depth` and
`extended_quantity` columns (product along the path, summed across paths), add an ORDER BY, and
reject `max_depth < 1`.

### F10 [low] `value_satisfies` on an unqualified temperature does not use the documented 1 K tolerance

```sql
SELECT value_satisfies(parse_quantity('20 °C'), parse_quantity('20.5 °C'));     -- score 0.75, distance 0.25, rule point
SELECT value_satisfies(parse_quantity('max. 20 °C'), parse_quantity('20.5 °C')); -- score 1.0, distance 0.5 (K), rule at_most
```

The docs say temperature uses an absolute 1 K tolerance and a 2.5 K soft band, with the distance in K, and that
`tol`/`soft_band` do not apply. The `point` rule ignores that: 0.5 K off scores 0.75 and the distance is a
share. Expected: score 1, distance 0.5. The fix is either code or a doc sentence ("the
temperature rule applies to bounded requirements only").

### F11 [low] `weighted_*` scalars reject `MAP {}` and an untyped `NULL` weight, although the docs allow them

```sql
SELECT weighted_jaccard(MAP {}, MAP {});                       -- Invalid Input: weights must be numeric, got "NULL"
SELECT weighted_containment(MAP {'a': NULL}, MAP {'a': 1});    -- same error
SELECT weighted_containment(MAP {'a': NULL::DOUBLE}, MAP {'a': 1});  -- 1.0 (works)
```

The docs say "both empty → 0.0" and "a NULL weight counts as 1". The literal forms have value type
`"NULL"`. Fix: accept the SQLNULL value type as "all NULL". The message `got "NULL"` reads like
a value, not a type.

### F12 [low] "-> Unexpected? Please report it: …" is appended to expected input-validation errors

```sql
SELECT parse_quantity('5 kg', 'fr');
-- Invalid Input Error: lang must be 'en' and/or 'de', got 'fr'
-- -> Unexpected? Please report it: https://github.com/DataZooDE/anofox-similarity/issues
```

This appears on every `InvalidInputException` raised by the scalars: `tree_from_json` errors,
`value_satisfies` scale and `tol` checks, `minhash_similarity` length, `lsh_bands`, and weights. These errors are the user's
own mistakes, so the hint invites bogus issues. Restrict the hint to INTERNAL errors.

### F13 [low] Inconsistent and leaky `k` handling

```sql
CREATE TABLE bom_items(parent_id VARCHAR, child_id VARCHAR); INSERT INTO bom_items VALUES ('M1','A'),('M2','A');
SELECT * FROM find_similar_materials_jaccard('M1', 0);    -- 0 rows
SELECT * FROM find_similar_materials_jaccard('M1', -1);   -- Binder Error: LIMIT/OFFSET cannot be negative
```

The coverage functions say "k must be positive" for 0 and negative values. The material searches return 0 rows for 0
and leak a LIMIT error for negative values. Align them on "k must be positive".

### F14 [low] Misleading binder messages

```sql
CREATE TABLE g(query_id VARCHAR, candidate_id VARCHAR, true_coverage DOUBLE);
CREATE TABLE r2(query_id VARCHAR, candidate_id VARCHAR, score DOUBLE);
SELECT * FROM eval_ranking('r2','g', rank_col := 'nope');
-- Binder Error: rank_col 'r2' is missing required column(s): nope      (the table is called rank_col)
SELECT * FROM eval_ranking('r2','g');
-- "results needs a rank column (rank_col) or a score column (score_col)", although r2 has a `score`
--  column (the default score column is `coverage`; documented now, D5)
SELECT tree_from_json('{}');
-- "a node has no node_id (under the root)"; there is no root at all
```

```sql
CREATE TABLE pairs(a VARCHAR, b VARCHAR, score DOUBLE); INSERT INTO pairs VALUES ('C1','C2',0.9);
SELECT * FROM cluster_matches('pairs', score_col := 'a');
-- Conversion Error: Could not convert string 'C1' to DOUBLE when casting from source column a
```

### F15 [low] `threshold_at_precision` counts top-1 picks of queries without gold; `eval_ranking` ignores them

```sql
CREATE TABLE r(query_id VARCHAR, candidate_id VARCHAR, rank BIGINT, coverage DOUBLE);
CREATE TABLE g(query_id VARCHAR, candidate_id VARCHAR, true_coverage DOUBLE);
INSERT INTO g VALUES ('Q1','A',1.0),('Q1','B',0.5),('Q2','C',0.9);
INSERT INTO r VALUES ('Q1','B',1,0.9),('Q2','C',1,0.95),('Q9','Z',1,1.0);
SELECT curve FROM threshold_at_precision('r','g');   -- first point: threshold 1.0, precision 0.0 (Q9 counted as a bad pick)
SELECT n_queries FROM eval_ranking('r','g', k := [1]);  -- 2 (Q9 ignored)
```

Pick one rule for both functions. Ignoring queries without gold matches `eval_ranking` and the docs.

### F16 [low] Smaller gaps

- `tree_from_json` accepts `"count": -3`. Only `validate_trees` reports `bad_count`.
- `tree_anchors('tree_nodes', tree_ids := ['Q1'])` reports `n_attributes = 0` even when
  `tree_attributes` exists. `tree_coverage` defaults `attrs_table` to `'tree_attributes'` but
  `tree_anchors` does not.
- `compat_score(dim, req, prov)` without a `compatibility_rules` table raises a Catalog Error.
  `rules_table := NULL` fails with "Cannot use NULL as function argument", so you cannot run the
  "no rule: equal → 1" path without creating an empty table.
- `containment([1,2], [1.0, 2.0])` = 0.0, because the text forms `'1'` and `'1.0'` differ. This is documented,
  but INTEGER vs DECIMAL columns of the same ids silently never match. A note in the doc
  would help.
- `cluster_matches` over integer ids returns `cluster_id = '10'` for `{2, 9, 10}` (text order,
  documented).
- `bom_where_used` is one level only, so `BEARING` lists `HUB` and not `BIKE`. Users expect
  where-used to go up to the top level.
- `find_covering_entities_lsh` on the benchmark (300 programmes, 80 requests) takes about 1.1 s of CPU
  time. Exact scoring with the scalars takes 0.18 s. This matches the doc's "pays off for large raw
  vocabularies". The stored `bands_table` was no faster here: about 1.2 s with it and 1.1 s without.

---

## Doc / example findings (fixed in this change, each verified by running it)

| # | Sev | Where | Problem | Fix |
|---|---|---|---|---|
| D1 | medium | README "Detecting Predecessors" | Copy-pasting top to bottom fails: `CREATE TABLE bom_items` already exists from the previous block | `CREATE OR REPLACE TABLE bom_items` |
| D2 | medium | README "Finding Analogs" | `min_history_months := 12` returns 0 rows on the README's own data (OLD-PUMP has 8 months), and the comment promises results | `min_history_months := 6`, with the expected output in a comment (OLD-PUMP, 1.0, 8 months) |
| D3 | medium | API_REFERENCE "Workflow: review the archive's vocabulary" ("runs on the benchmark files") | The final `tree_coverage('R12', nodes_table := 'request_nodes', …)` fails: the table is never created, and the CSV has empty `class_id`, so the call returns 0 rows (F4) | Added the `request_nodes` load with classes from `gold_item_class` (stands in for the request's review). Now 20 rows, P061 first |
| D4 | low | API_REFERENCE gap-list example | `WHERE is_gap` also lists the passing attributes of a gap requirement (`distance 0.0`), so 6 rows, 4 of them fine | Filter `value_score IS DISTINCT FROM 1 OR count_available < count_needed`, with the expected 2 rows described |
| D5 | low | API_REFERENCE `eval_ranking` | The `rank_col`/`score_col` defaults were not documented, and a `score` column is not picked up | Documented: `rank` if present, else `coverage` |
| D6 | low | API_REFERENCE `uncertain_decisions` | The column contract of `texts_table` (`node_id, text`, plus `tree_id` when decisions have one) and of `catalogue_table` was missing. A natural `label` column fails | Documented |
| D7 | low | README Test Suite | `unittest "[similarity]"` matches no test. `unittest -v "[wl_kernel]"` fails ("Unrecognised verbosity"). The "52 tests / 1179 assertions" count is stale (121 test files) | `"[jaccard]"`, a directory glob, `-s`, and the current count |
| D8 | low | README Configuration | `bom_explosion_multilevel` needs `bom_header`/`bom_component`, which the README never introduces | Comment naming both tables and their columns |

Not changed:
- The DuckPGQ recipe's `SET active_grammar_extensions` fails on 1.5.6 ("unrecognized configuration
  parameter"). It is labelled "DuckDB 2.0", which is correct, but a 1.5.6 user has no
  alternative line.
- The R5 evaluation example uses `req_nodes`/`prog_nodes`/`templates` and
  `getvariable('request_ids')`, while every other example uses `request_nodes`/`programme_nodes`/
  `catalogue_attributes`. It points to r5/uc_01 for the setup. With aliasing views it reproduces
  top-1 0.925, MAE 0.036 and threshold 0.769 at 95.8 % precision. Unifying the names would let it run after
  the other examples.

## What worked well (verified)

- No INTERNAL errors or crashes in about 300 adversarial calls. All `*_table` parameters block injection
  (`'bom_items; DROP TABLE bom_items'` is rejected).
- Read-only attached DB: `tree_coverage`, `find_covering_entities_lsh`, `eval_ranking`,
  `cluster_matches` and `apply_vocabulary_review` all work under `duckdb -readonly`.
- Views, temp tables (`temp.tt`), `db.schema.table` and unicode table names work.
- Scale: Jaccard/containment search on 10k parents/100k rows takes 0.3–0.4 s. WL search on 80k edges takes 1–2 s.
  `find_covering_entities` on 100k rows takes 0.05 s. `tree_coverage` for 80 requests with all options takes about 5 s
  wall-clock on a loaded box.
- The benchmark KPIs in the API reference reproduce: eval_ranking top-1 0.925, MAE 0.0356,
  bootstrap CI [0.906, 0.947] over 8 makers, and the review queue ranks the 4 misleading wordings
  first.
