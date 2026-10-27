# Exploratory testing: pragmatic unblocker

Tested the supplied DuckDB 1.5.6 release binary with `DATAZOO_DISABLE_TELEMETRY=1`. SQL below is copy-pasteable in that CLI. The shipped extension is loaded by absolute path. No source build was made.

## Findings

### 1. README walkthrough stopped at the second BOM table (medium; README.md:90)

```sql
LOAD '/home/jr/Projects/datazoo/anofox-similarity/build/release/extension/anofox_similarity/anofox_similarity.duckdb_extension';
CREATE TABLE bom_items (parent_id VARCHAR, child_id VARCHAR);
CREATE TABLE bom_items (parent_id VARCHAR, child_id VARCHAR);
SELECT 1;
```

Actual: `Catalog Error: Table with name "bom_items" already exists!`; the connection remains usable (`SELECT 1` returns 1). Expected: a new user can run the README examples in order in one connection. Suggested fix: use `CREATE OR REPLACE TABLE` for the second example. **Applied and verified** with the following predecessor and cold-start calls in the same connection. This is a documentation fix, not a schema migration recommendation.

### 2. README cold-start example promised analogs but returned none (medium; README.md:132)

```sql
LOAD '/home/jr/Projects/datazoo/anofox-similarity/build/release/extension/anofox_similarity/anofox_similarity.duckdb_extension';
CREATE TABLE bom_items (parent_id VARCHAR, child_id VARCHAR);
INSERT INTO bom_items VALUES ('OLD-PUMP','SEAL-001'),('OLD-PUMP','BEARING-X'),('OLD-PUMP','MOTOR-M1'),('NEW-PUMP','SEAL-001'),('NEW-PUMP','BEARING-X'),('NEW-PUMP','MOTOR-M1');
CREATE TABLE goods_movements (material_id VARCHAR, movement_date DATE, quantity DOUBLE);
INSERT INTO goods_movements VALUES ('OLD-PUMP','2023-01-15',100),('OLD-PUMP','2023-03-15',80),('OLD-PUMP','2023-06-15',40),('OLD-PUMP','2023-09-15',10),('NEW-PUMP','2023-06-01',20),('NEW-PUMP','2023-09-01',60),('NEW-PUMP','2023-12-01',100);
SELECT material_id, similarity, history_months FROM cold_start_analogs('NEW-PUMP',5,min_history_months := 12,bom_table := 'bom_items',movements_table := 'goods_movements');
SELECT material_id, similarity, history_months FROM cold_start_analogs('NEW-PUMP',5,min_history_months := 6,bom_table := 'bom_items',movements_table := 'goods_movements');
```

Actual: the documented `12` returns zero rows; `6` returns `OLD-PUMP | 1.0 | 8`. Expected: the quick-start example demonstrates a returned analog. Suggested fix: use `6` and explain the fixture's eight-month history. **Applied and verified.** For production cold starts, retain the appropriate business history threshold; this fixture value is only a demonstration.

### 3. Table-name parameters reject valid quoted identifiers (medium; docs/API_REFERENCE.md:1310)

```sql
LOAD '/home/jr/Projects/datazoo/anofox-similarity/build/release/extension/anofox_similarity/anofox_similarity.duckdb_extension';
CREATE TABLE "weird view" AS SELECT * FROM read_csv_auto('/home/jr/Projects/datazoo/anofox-similarity/test/data/coverage/mini/feature_multiset.csv');
SELECT count(*) FROM find_covering_entities_for(MAP {'seat_heating': 2.0},3,feature_table := 'weird view');
SELECT count(*) FROM find_covering_entities_for(MAP {'seat_heating': 2.0},3,feature_table := '"weird view"');
SELECT 1;
```

Actual: both calls fail with `Binder Error: Invalid identifier for feature_table`; `SELECT 1` still works. Expected: a valid DuckDB table or view name can be passed as the documented `feature_table` argument, including names that require quoting. Suggested fix: parse and quote qualified identifiers when constructing the table reference, rejecting only malformed names. **Temporary bridge:** create a simple-name view over the table and pass that view name. Ordinary temporary views were tested and work.

### 4. Scalar named arguments silently use position on DuckDB 1.5.6 (medium; docs/API_REFERENCE.md:1255)

```sql
LOAD '/home/jr/Projects/datazoo/anofox-similarity/build/release/extension/anofox_similarity/anofox_similarity.duckdb_extension';
SELECT tversky_similarity(['a'],['a','b'],beta := 0.0,alpha := 1.0) AS reordered,
       tversky_similarity(['a'],['a','b'],1.0,0.0) AS positional;
SELECT 1;
```

Actual: `reordered = 0.5`, `positional = 1.0`, and the connection stays usable. Expected: the names `beta` and `alpha` determine the values. The API reference already warns about this, so the **temporary bridge** is to pass both trailing values in documented order. Suggested proper fix: use binding that honors scalar parameter names on 1.5.6, or explicitly reject reordered named arguments if that cannot be supported. This can silently change coverage decisions.

### 5. A fractional coverage score can correctly have no gaps, but the wording obscured why (low; docs/API_REFERENCE.md:1571)

```sql
LOAD '/home/jr/Projects/datazoo/anofox-similarity/build/release/extension/anofox_similarity/anofox_similarity.duckdb_extension';
CREATE TABLE tree_nodes AS SELECT * FROM read_csv_auto('/home/jr/Projects/datazoo/anofox-similarity/test/data/coverage/mini/tree_nodes.csv');
CREATE TABLE tree_attributes AS SELECT * FROM read_csv_auto('/home/jr/Projects/datazoo/anofox-similarity/test/data/coverage/mini/tree_attributes.csv');
SELECT candidate_id, coverage, n_gaps FROM tree_coverage('Q1',candidates := ['S1']);
SELECT req_node_id, value_score, name_sim, req_score, is_gap FROM tree_coverage_trace('Q1',candidates := ['S1']) ORDER BY req_node_id;
```

Actual: `S1` has coverage `0.8583333333333334` and `n_gaps = 0`; its required values have `value_score = 1`, while some name similarity and requirement scores are below 1. Expected: documentation makes clear that a soft name match can reduce coverage without setting `is_gap`. Suggested fix: say `attribute value_score below 1` instead of the ambiguous `value below 1`. **Applied** to the API reference and verified against the trace.

### 6. README described monthly observations as weekly and the query product as history-free (low; README.md:102,128)

```sql
LOAD '/home/jr/Projects/datazoo/anofox-similarity/build/release/extension/anofox_similarity/anofox_similarity.duckdb_extension';
CREATE TABLE goods_movements (material_id VARCHAR, movement_date DATE, quantity DOUBLE);
INSERT INTO goods_movements VALUES ('OLD-PUMP','2023-01-15',100),('OLD-PUMP','2023-03-15',80),('OLD-PUMP','2023-06-15',40),('OLD-PUMP','2023-09-15',10),('NEW-PUMP','2023-06-01',20),('NEW-PUMP','2023-09-01',60),('NEW-PUMP','2023-12-01',100);
SELECT material_id, count(*) AS observations, min(movement_date) AS first_usage, max(movement_date) AS last_usage FROM goods_movements GROUP BY material_id;
```

Actual: `NEW-PUMP` already has three observations, and both products' observations are months apart. Expected: comments accurately describe the fixture. Suggested fix: call the observations sparse and describe the analog use case as limited history. **Applied** to README; the shown SQL verifies the fixture facts.

## Coverage and remaining risk

Also exercised the README SAP conversion, the mini fixture's `tree_coverage`, trace and LSH flow, classifier and vocabulary review, `eval_ranking`, `eval_calibration`, Unicode `cluster_matches`, null and empty material searches, and a custom two-level ERP BOM. These sampled calls produced no crash or INTERNAL error. This exploration does not establish behavior on large archives, concurrent sessions, or all documentation examples.
