# Exploratory user testing: usability and developer experience

Build: `build/release/duckdb` (DuckDB v1.5.6, Variegata), run with `DATAZOO_DISABLE_TELEMETRY=1`.
Every repro starts with

```sql
LOAD '/home/jr/Projects/datazoo/anofox-similarity/build/release/extension/anofox_similarity/anofox_similarity.duckdb_extension';
```

`$COV` stands for `/home/jr/Projects/datazoo/anofox-similarity/test/data/coverage/`. The benchmark
setup (U-0) is used by the coverage findings.

Personas played: (a) a new user copying README.md top to bottom, (b) a data engineer running the
API_REFERENCE.md coverage workflows on the seating benchmark, (c) an ERP analyst with a hand-made
bicycle BOM, (d) an adversarial user (NULLs, empty input, wrong types, views, temp tables,
read-only and attached databases, odd identifiers, unicode, 10 000 materials).

No crash and no INTERNAL error was found; `SELECT 1` worked after every error. Read-only
(`duckdb -readonly`) and `ATTACH ... (READ_ONLY)` work for the coverage functions, and so do
views, temp tables and `db.schema.table` names. Performance was fine: 10 000 materials with
Jaccard search take 0.5 s, `tree_coverage` for 80 requests takes 3.9 s, and every other call
took under 2 s.

Severity: **high** = a documented workflow gives a wrong or empty result without any error;
**medium** = a confusing result or error that costs a user real time; **low** = a polish issue.
Status: **FIXED-DOC** = corrected in this change (examples re-run); **CODE** = needs a C++ change
(not touched here).

---

## U-0 Benchmark setup used below

```sql
SET VARIABLE d = '/home/jr/Projects/datazoo/anofox-similarity/test/data/coverage/';
CREATE TABLE requests AS SELECT * FROM read_csv_auto(getvariable('d') || 'requests.csv.gz');
CREATE TABLE request_nodes AS SELECT * FROM read_csv_auto(getvariable('d') || 'request_nodes.csv.gz');
CREATE TABLE request_attributes AS
SELECT tree_id, node_id, attr_id, name, q.dimension, q.unit, q.lo, q.hi, q.bound, q.category
FROM (SELECT a.*, parse_quantity(a.value_text, r.lang) AS q
      FROM read_csv_auto(getvariable('d') || 'request_attributes.csv.gz') a
      JOIN requests r ON r.request_id = a.tree_id);
CREATE TABLE programme_nodes AS SELECT * FROM read_csv_auto(getvariable('d') || 'programme_nodes.csv.gz');
CREATE TABLE programme_attributes AS SELECT * FROM read_csv_auto(getvariable('d') || 'programme_attributes.csv.gz');
CREATE TABLE programme_class_multiset AS SELECT * FROM read_csv_auto(getvariable('d') || 'programme_class_multiset.csv.gz');
CREATE TABLE vocabulary_overrides AS SELECT wording, source, class_id FROM read_csv_auto(getvariable('d') || 'gold_vocabulary.csv.gz');
CREATE TABLE catalogue_attributes AS SELECT * FROM read_csv_auto(getvariable('d') || 'catalogue_attributes.csv.gz');
CREATE TABLE catalogue AS SELECT class_id, vec::FLOAT[] AS vec, exemplar_tokens::FLOAT[][] AS exemplar_tokens
FROM read_csv_auto(getvariable('d') || 'catalogue_vectors.csv.gz');
CREATE TABLE request_items AS SELECT tree_id, node_id, source, vec::FLOAT[] AS vec
FROM read_csv_auto(getvariable('d') || 'request_item_vectors.csv.gz');
CREATE TABLE request_labels AS SELECT tree_id, node_id, class_id, p
FROM classify_to_catalogue(items_table := 'request_items', catalogue_table := 'catalogue');
```

---

## High

### U-1 `tree_coverage` and friends return 0 rows without any error when the request has no classes — CODE + FIXED-DOC

These doc examples all return an empty result on the benchmark: the gap list of the best
programme, "Large archives", the last step of the vocabulary-review workflow and the LSH
example. The benchmark's `request_nodes` have an empty `class_id` (on purpose, see the coverage
README), and the examples passed no `label_table` for the request side. An unclassified item
matches nothing. When no item of the request has a class, stage 1 has no features and the
function returns nothing. The user gets no hint about the cause.

```sql
-- after U-0
SELECT count(*) FROM tree_coverage('R07', nodes_table := 'request_nodes', attrs_table := 'request_attributes',
       candidate_nodes_table := 'programme_nodes', candidate_attrs_table := 'programme_attributes',
       overrides_table := 'vocabulary_overrides');                                -- 0
SELECT count(*) FROM tree_class_multiset('request_nodes');                        -- 0
SELECT count(*) FROM tree_coverage('R07', nodes_table := 'request_nodes', attrs_table := 'request_attributes',
       label_table := 'request_labels',
       candidate_nodes_table := 'programme_nodes', candidate_attrs_table := 'programme_attributes',
       overrides_table := 'vocabulary_overrides');                                -- 20
```

- Actual: 0 rows. `SET VARIABLE top = (... WHERE rank = 1)` silently becomes NULL, and the next
  trace query is empty too.
- Expected: an error or a warning such as `tree_coverage: none of the 29 items of query 'R07'
  has a class; give class_id, overrides_table or label_table`.
- Fix (code): in `tree_coverage`, `tree_coverage_trace`, `coverage_recall` and
  `tree_class_multiset`, raise an InvalidInput error when a requested query tree has items but
  none of them resolves to a class. Alternatively, return the query with
  `n_requirements`/`n_unclassified` and coverage 0, so the cause shows up.
- Docs: the four examples now pass `label_table := 'request_labels'`, and the text explains it
  (docs/API_REFERENCE.md, gap list, Large archives, vocabulary workflow, LSH).

### U-2 The vocabulary-review workflow says it "runs on the benchmark files" but its last step fails — FIXED-DOC

Run from `$COV`, the block ends with `Binder Error: tree_coverage: nodes_table 'request_nodes'
does not exist`. With the table loaded, it returns 0 rows (U-1), because `programme_labels`
labels only the programme side. The step now loads `request_nodes`, classifies
`request_item_vectors` and unions both label sets. Verified: R12 → P061 0.914 and 20 rows.

### U-3 README "Detecting Predecessors" fails when copied top to bottom, and the example finds nothing — FIXED-DOC

```sql
-- README, copied in order: Quick Start first creates bom_items, then
CREATE TABLE bom_items (parent_id VARCHAR, child_id VARCHAR);
-- Catalog Error: Table with name "bom_items" already exists!
```

The INSERT then appends OLD/NEW-PUMP rows to the PUMP-A/B/C table. Even with a clean table,
the example returns 0 rows. The note admits this ("will typically return zero predecessors").
The flagship example gave a new user an error and an empty result.
Fix applied: `CREATE OR REPLACE`, and dense weekly data built with `range()`. Verified result:
`OLD-PUMP | 0.79 | -1.0 | 38`.

---

## Medium

### U-4 README "Finding Analogs" returns 0 rows — FIXED-DOC

`cold_start_analogs('NEW-PUMP', 5, min_history_months := 12, ...)` returns nothing, because the
README's data has at most 8–9 months of history. Changed to `min_history_months := 6` and added
the expected output (`OLD-PUMP | 1.0 | 9 | 2023-01-02 | 2023-10-02`, verified).

### U-5 Every validation error ends with "Unexpected? Please report it" — CODE

```sql
SELECT weighted_containment(MAP {'a': -1}, MAP {'a': 2});
-- Invalid Input Error: weights must be non-negative, got -1
-- -> Unexpected? Please report it: https://github.com/DataZooDE/anofox-similarity/issues
SELECT parse_quantity('5 mm', 'fr');                 -- same suffix
SELECT tree_from_json('not json');                   -- same suffix
SELECT value_satisfies(parse_quantity('IP54 or better'), parse_quantity('IP65'));  -- same suffix
```

These are expected user errors with clear messages. The suffix tells the user it is a bug, which
is wrong and adds noise. Binder errors do not get the suffix, so it looks arbitrary.
Fix: add the "report it" hint to INTERNAL errors only, not to InvalidInput errors raised on
purpose (the hook seems to sit in the shared feedback/banner code; it is not in `src/`).

### U-6 The column error message has its placeholders swapped — CODE

```sql
CREATE TABLE p2(x INT, y INT, s FLOAT);
SELECT * FROM cluster_matches('p2');
-- Binder Error: left_col 'p2' is missing required column(s): a (expected columns: a)
CREATE TABLE results(query_id VARCHAR, candidate_id VARCHAR, rank INT);
CREATE TABLE gold(query_id VARCHAR, candidate_id VARCHAR, true_coverage DOUBLE);
SELECT * FROM eval_calibration('results', 'gold', score_col := 'nonexistent');
-- Binder Error: score_col 'results' is missing required column(s): nonexistent (expected columns: nonexistent)
```

- Actual: the parameter name is shown as if it were the table, and the table name as if it were
  the parameter. The "expected columns" part repeats the missing list. The function name and
  the available columns are missing.
- Expected, as `find_covering_entities_for` already does it: `cluster_matches: pairs_table 'p2'
  has no column 'a'; pass left_col := '<column>' (columns: x, y, s)`.
- Fix: the shared required-column check helper (it prints `<param> '<table>'`) should print
  `<fn>: <table_param> '<table>' has no column '<col>' (<col_param>); columns: ...`.
  `find_similar_materials_jaccard(..., bom_table := 'bom_header')` uses the same helper and
  gives the readable but redundant `bom_table 'bom_header' is missing required column(s):
  parent_id, child_id (expected columns: parent_id, child_id)`.

### U-7 Jaccard search returns a different top k on every run when scores tie — CODE

```sql
CREATE TABLE bom_items AS SELECT 'M' || (i // 15) AS parent_id, 'C' || ((i * 7919) % 5000) AS child_id
FROM range(150000) t(i);
SELECT string_agg(material_id, ',') FROM find_similar_materials_jaccard('M42', 3);  -- M5042,M4042,M9042
SELECT string_agg(material_id, ',') FROM find_similar_materials_jaccard('M42', 3);  -- M1042,M2042,M6042
SELECT string_agg(material_id, ',') FROM find_similar_materials_jaccard('M42', 3);  -- M4042,M5042,M1042
```

Ten materials tie at similarity 1.0, and every run returns a different three. Results cannot
be reproduced, and tests or reports flap. `find_similar_materials_wl_kernel` returns ties
unordered too, but stayed stable across runs here. `find_covering_entities` already orders
`score DESC, entity_id`.
Fix: add `material_id` as the final sort key in the top-k of the Jaccard, WL and cold-start
searches.

### U-8 An untyped NULL or empty MAP literal is rejected in the weighted measures — CODE

```sql
SELECT weighted_jaccard(MAP {'a': NULL}, MAP {'a': 1});   -- Invalid Input Error: weights must be numeric, got "NULL"
SELECT weighted_jaccard(MAP {}, MAP {});                  -- same error
SELECT weighted_containment(MAP {}, MAP {'a': 1});        -- same error
SELECT weighted_jaccard(MAP {'a': NULL::DOUBLE}, MAP {'a': 1});                                   -- 1.0 (works)
SELECT weighted_jaccard(MAP([]::VARCHAR[], []::DOUBLE[]), MAP([]::VARCHAR[], []::DOUBLE[]));  -- 0.0 (works)
```

The docs say "a NULL weight counts as 1" and "both empty → 0.0". The natural literal spelling
fails, and `got "NULL"` reads as if the value were the string "NULL" (it is the SQLNULL type).
Fix: accept the SQLNULL value type (treat it as DOUBLE) in the bind of the weighted scalars.

### U-9 `compat_score` without a rules table is a catalog error, and the doc said otherwise — CODE + FIXED-DOC

```sql
SELECT compat_score('ip', 'IP54', 'IP54');
-- Catalog Error: Table with name compatibility_rules does not exist! Did you mean "pg_attribute"?
```

The doc said "Without a rule the result is 1 if the categories are equal". A user without
curated rules gets a confusing error. When a rules table exists, an `at_least IP65`
requirement is not met by IP67 unless a rule says so (trace: `Schutzart | at_least IP65 |
IP67 | attr_score 0.0`). `tree_coverage` without `rules_table` behaves the same way.
- Fix (code): with the default `rules_table` missing, fall back to equal → 1 / else → 0 and
  name the parameter in any error. A clearer option: `rules_table := NULL` means "no rules".
- Docs: now say the table must exist and that ordinal "at least" is not met without rules.

### U-10 `assignment := 'auto'` was documented as "partition for reviewed candidate trees" — FIXED-DOC (C++ description still wrong)

`coverage_kernel.cpp:857` partitions only when both the query and the candidate are reviewed.
After U-0, with `label_table := 'request_labels'`, `tree_coverage` reports `reviewed = true,
assignment_used = best_match`, which contradicts the parameter table. The parameter prose in
API_REFERENCE is now corrected (a later paragraph already said it right). The
`duckdb_functions()` description at `src/modules/coverage_kernel.cpp:1461-1462` still says
"'auto' uses partition for reviewed candidates". It should say "for reviewed query and
candidate trees".

### U-11 The gap list shows satisfied attributes as gaps — FIXED-DOC

`is_gap` is a property of the requirement, but the trace repeats it on every attribute row. The
documented "gap list" (`WHERE is_gap`) lists `Höhenverstellweg | at_least 0.055 m | 0.06 m |
distance 0.0` next to the real shortfall. The example now prints `attr_score` and a comment says
that `attr_score < 1` marks the failing attribute.
Suggestion (code): add an attribute-level `attr_ok`/`attr_gap` column.

### U-12 Loading the extension writes 4 tables into the user's database file — CODE / design

```sh
DATAZOO_DISABLE_TELEMETRY=1 build/release/duckdb -unsigned /tmp/fresh.db \
  -c "LOAD '.../anofox_similarity.duckdb_extension'; SELECT table_name FROM duckdb_tables();"
# material_embeddings, material_embeddings_dirty, transactional_embedding_statistics, transactional_feature_mapping
```

`LOAD` alone persists these tables into a user's database (they stay after the session). Users
who only use coverage matching get tables they never asked for. They also show up in
`SHOW TABLES` and in the binder's "Did you mean …" suggestions.
Fix: create the tables lazily (on the first `compute_*_embeddings`), or put them in a dedicated
schema such as `anofox_similarity`.

### U-13 Macro errors do not name the parameter that would fix them — CODE

```sql
SELECT * FROM bom_explosion_multilevel('NEW-PUMP', max_depth := 5);
-- Catalog Error: Table with name bom_header does not exist! Did you mean "bom_items"?
SELECT * FROM cold_start_analogs('M42', 5);
-- Catalog Error: Table with name goods_movements does not exist!
```

The C++ table functions say `cluster_matches: pairs_table 'nope' does not exist`. The SQL
macros give raw catalog errors and even suggest `bom_items`, which these macros cannot read.
The README Quick Start builds `bom_items`, then the "Configuration" section calls
`bom_explosion_multilevel`, and that fails like this. A comment in the README now says which
tables it needs (FIXED-DOC).
Fix (code): give the BOM macros a `query_table` guard (or C++ wrappers) that names
`header_table`/`component_table`, and support a `bom_table := 'bom_items'` form.

---

## Low

### U-14 Documented parameter names differ from the real ones — FIXED-DOC

`duckdb_functions().parameters` shows `bom_explosion_1level/_multilevel(query_material_id, …)`
(docs said `parent_material_id`), and `find_similar_materials_wl_kernel/cold_start_analogs(material_id, …)`
(docs said `query_material_id`). The signatures and parameter tables are corrected. Note:
`bom_explosion_*(query_material_id := '')` defaults to `''`, so leaving it out silently returns
0 rows. It should be required.

### U-15 The two "Large archives" and LSH examples both create `programme_index` — FIXED-DOC

When you follow API_REFERENCE in order, `minhash_index` into `programme_index` fails with
"already exists", and `minhash_bands('programme_index')` then complains about missing
`partition, band_hashes, …`. The LSH example now uses `programme_minhash`.

### U-16 `uncertain_decisions` example uses undefined tables `item_texts` and `answers` — FIXED-DOC

Added `CREATE TABLE item_texts AS SELECT tree_id, node_id, label AS text FROM request_nodes
WHERE node_kind = 'item'` and the shape of `answers`. Verified: 228 JSON requests (ceil(0.15 × 1520)).

### U-17 Positional versus named arguments are inconsistent across the coverage functions — CODE

`eval_ranking('results', 'gold')` and `cluster_matches('pairs')` are positional.
`cluster_matches(pairs_table := 'pairs')` fails with "No function matches". Meanwhile
`uncertain_decisions('decided')` and `vocabulary_review_queue('programme_nodes', …)` require
`decisions :=`/`trees_table :=`. A user cannot predict which is which.
Fix: accept both forms for the leading table argument, as `classify_to_catalogue` does with
named-only.

### U-18 Quoted identifiers are rejected as table names — CODE

```sql
CREATE SCHEMA s; CREATE TABLE s."My Pairs"(a VARCHAR, b VARCHAR, score DOUBLE);
SELECT * FROM cluster_matches('s."My Pairs"');   -- Binder Error: Invalid identifier for pairs_table: s."My Pairs"
```

The injection guard is right to reject `pairs; DROP TABLE pairs`, but it also rejects legal
quoted names (spaces, umlauts in quotes, mixed case). Workaround: create a view.
Fix: parse the name with DuckDB's qualified-name parser and quote each part, rather than
whitelisting characters.

### U-19 Unknown ids score 0 or vanish silently — CODE / doc

`wl_kernel_similarity('BIKE-CITY', 'NOPE')` = 0.0 (not NULL).
`tree_coverage('Q1', candidates := ['S1', 'NOPE'])` returns 1 row and drops `NOPE` silently.
`tree_coverage('NOPE')` returns 0 rows. This is consistent with `find_covering_entities`, but a
typo looks like "no match". Suggest NULL for an unknown material in the scalar, and a NOTICE
or error for unknown explicit `candidates`.

### U-20 Without `candidates`, other requests in the same table compete as candidates

With the hand-made `mini/tree_nodes.csv` as `tree_nodes`, `tree_coverage('Q1')` ranks request
`Q3` and modules `M*` next to the programmes `S*`, because the candidate tables default to the
query tables. The docs mention the default. A one-line warning in the parameter table ("put
requests and offers in separate tables or pass candidates") would prevent this surprise.

### U-21 The minimal `tree_coverage` call gives much worse scores than the recommended setup

Without `default_bounds`, `rules_table` and `name_vectors`, R07's top match P019 scores 0.686
instead of 0.949. Unqualified values become points (`Haltekraft point 9500 N` vs 11500 N →
0), and IP67 fails `at_least IP65`. The defaults are the strict HLD semantics, but the short
happy path is the wrong one. Consider `default_bounds := 'catalogue'` as the default whenever
`templates_table` exists, or document the full recommended call in the
`duckdb_functions().examples` entry. The current example
`tree_coverage('R01', k := 20, candidate_nodes_table := …)` also relies on a `tree_nodes`
table.

### U-22 Smaller doc and README nits — FIXED-DOC where marked

- README: "52 tests (1179 assertions)" is stale; there are 121 `.test` files (FIXED-DOC: numbers removed).
- README: links to `LICENSE.md` and `CONTRIBUTING.md` are broken; the file is `LICENSE` and
  there is no CONTRIBUTING (FIXED-DOC).
- README/API_REFERENCE: "about 87 functions" vs the coverage matrix "~84" (not changed).
- `tversky_similarity(l, r, beta := 0.0, alpha := 1.0)` silently swaps on 1.5.6
  (`['a','b','c']` vs `['a']`: 1.0 instead of 0.333). This is documented, and it is a DuckDB
  1.5.6 limitation, but a C++ check (fail when the names come out of order) would turn a silent
  wrong answer into an error.
- `normalize_quantity(5, NULL)` returns a struct with a NULL unit, while `normalize_quantity(NULL, 'mm')`
  returns NULL. Minor inconsistency.
- `tree_from_json('{}')` says "a node has no node_id (under the root)", but the document has
  no root. "expected an object with 'root' or a node" would be clearer.
- `parse_quantity('2.500 mm')` with the default `['en','de']` gives 2.5 mm, and
  `'65.000 Zyklen'` gives 65. This is documented, but a user with German data who leaves out
  `lang` gets numbers that are wrong by ×1000. Consider a doc callout in the README coverage
  section.
