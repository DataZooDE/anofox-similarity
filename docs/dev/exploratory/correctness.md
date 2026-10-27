# Exploratory findings: correctness

Tested with the supplied release DuckDB and extension on 2026-10-27. Each SQL block runs in a fresh DuckDB session. `SELECT 1` succeeded after each reported binder error.

## 1. Historyless rows returned as cold-start analogs

**Severity:** medium. **Location:** `docs/API_REFERENCE.md:304` describes this function as finding materials with consumption history. The default `min_history_months := 0` lets materials with no movement rows through, assigning `history_months = 0` and NULL dates. A user can therefore select an analog with no demand pattern to forecast from. The README's original `min_history_months := 12` also returned no rows from its sample data; the example now uses 8 and yields `OLD-PUMP`.

```sql
LOAD '/home/jr/Projects/datazoo/anofox-similarity/build/release/extension/anofox_similarity/anofox_similarity.duckdb_extension';
CREATE TABLE bom_items(parent_id VARCHAR, child_id VARCHAR);
INSERT INTO bom_items VALUES ('NEW','part'), ('NO-HISTORY','part'), ('WITH-HISTORY','part');
CREATE TABLE goods_movements(material_id VARCHAR, movement_date DATE, quantity DOUBLE);
INSERT INTO goods_movements VALUES ('WITH-HISTORY', DATE '2024-01-01', 10);
SELECT material_id, history_months, first_usage, last_usage
FROM cold_start_analogs('NEW', 10, bom_table := 'bom_items', movements_table := 'goods_movements');
```

**Actual:** `NO-HISTORY` is returned with `0, NULL, NULL`. **Expected:** only `WITH-HISTORY` is returned when the function promises analogs with consumption history. **Suggested fix:** require at least one movement row independent of the month-span filter; keep `min_history_months` for the additional span condition.

## 2. Valid quoted table identifiers cannot be passed to table functions

**Severity:** low. **Location:** `docs/API_REFERENCE.md:1845` (`cluster_matches`) and `docs/API_REFERENCE.md:222` (`find_similar_materials_jaccard`). Tables whose names contain spaces or hyphens are valid DuckDB objects, but the functions reject both the plain name and SQL-quoted name in their table-name argument. This blocks direct use on ordinary imported table names.

```sql
LOAD '/home/jr/Projects/datazoo/anofox-similarity/build/release/extension/anofox_similarity/anofox_similarity.duckdb_extension';
CREATE TABLE "pairs-hyphen"(a VARCHAR, b VARCHAR, score DOUBLE);
INSERT INTO "pairs-hyphen" VALUES ('a', 'b', 0.9);
SELECT * FROM cluster_matches('pairs-hyphen');
SELECT * FROM cluster_matches('"pairs-hyphen"');
SELECT 1;
```

**Actual:** both calls raise `Binder Error: Invalid identifier for pairs_table`; the final `SELECT 1` works. The same failure occurs with `bom_table := '"bom_items"'` when `bom_items` exists. **Expected:** a quoted SQL identifier naming an existing table is accepted. **Suggested fix:** parse table references as qualified identifiers with SQL quoting, then bind safely; avoid concatenating unvalidated SQL text.

## 3. README quick start failed when pasted top to bottom

**Severity:** low. **Location:** `README.md:91`. The predecessor block previously repeated `CREATE TABLE bom_items` from the first block, so a new user following both examples got `Catalog Error: Table with name "bom_items" already exists!`. The cold-start block's former 12-month threshold returned no rows from the 8-month sample despite promising analogs.

```sql
LOAD '/home/jr/Projects/datazoo/anofox-similarity/build/release/extension/anofox_similarity/anofox_similarity.duckdb_extension';
CREATE TABLE bom_items(parent_id VARCHAR, child_id VARCHAR);
CREATE TABLE bom_items(parent_id VARCHAR, child_id VARCHAR);
```

**Actual:** second `CREATE TABLE` fails. **Expected:** sequential examples run in one session. **Suggested fix:** applied in `README.md`: reuse `bom_items`, describe the sparse dates accurately, and use an 8-month threshold. Replayed the revised example: the predecessor query returns zero rows as documented, and the analog query returns `OLD-PUMP` with `history_months = 8`.

## Design decisions and risks

- Kept C++ and existing tests untouched as requested. Code behavior is reported with isolated SQL.
- Used the mini seating fixture for `tree_coverage` and its trace, and the full vocabulary vectors for classification/review. Both paths executed without an error; the mini LSH index and band table returned the same three candidates with and without the stored bands.
- The odd-identifier limitation may be intentional SQL-injection hardening. A fix needs safe identifier parsing and escaping.
