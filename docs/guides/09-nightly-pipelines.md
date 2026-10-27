# Nightly Pipelines

> Run the extension as a scheduled job on a persistent database file: re-runnable, gated, and
> safe for the readers.

**Use this guide to:**
- Keep the extension's own tables out of your database file
- Write a pipeline script that can run again without duplicating rows, and stops on bad data
- Know which derived tables to rebuild when new data arrives
- Serve the results to readers (other shells, Python) while the next run builds

## Keep the Extension's Tables Out of Your File

On a writable database, loading the extension creates `material_embeddings` and three other
tables, and three HNSW indexes ([infrastructure](../api/06-infrastructure.md#database-infrastructure)).
The indexes are created only with a new table, so `DROP INDEX hnsw_combined_idx;` (and the other
two) keeps them out for good; while they exist, the file needs the `vss` extension to write to
`material_embeddings`. If you do not use the embedding tables at all, start the shell without a
file and `ATTACH` your database. The extension then creates its tables in memory:

```bash
./build/release/duckdb -bail < nightly.sql      # no file argument: nightly.sql attaches it
```

```sql
ATTACH 'nightly.duckdb' AS nightly;
USE nightly;
```

All functions resolve table names in the current database, so after `USE` the pipeline is
unchanged.

## A Re-runnable, Gated Script

```sql
-- nightly.sql: run with  ./build/release/duckdb -bail < nightly.sql  (from the repository root)
ATTACH 'nightly.duckdb' AS nightly;
USE nightly;
BEGIN;

-- 1. Load (replace, so a re-run does not duplicate rows)
CREATE OR REPLACE TABLE tree_nodes AS SELECT * FROM read_csv_auto('test/data/coverage/mini/tree_nodes.csv');
CREATE OR REPLACE TABLE tree_attributes AS SELECT * FROM read_csv_auto('test/data/coverage/mini/tree_attributes.csv');

-- 2. Gates: error() stops the script under -bail; the transaction is not committed
SELECT error('gate: tree_nodes is empty') AS gate FROM tree_nodes HAVING count(*) = 0;
SELECT error('gate: ' || count(*) || ' tree errors, e.g. ' || min(issue || ' ' || node_id)) AS gate
FROM validate_trees('tree_nodes', 'tree_attributes') WHERE severity = 'error' HAVING count(*) > 0;

-- 3. Derived tables, in dependency order
CREATE OR REPLACE TABLE class_multiset AS SELECT * FROM tree_class_multiset('tree_nodes');
CREATE OR REPLACE TABLE class_minhash AS SELECT * FROM minhash_index(feature_table := 'class_multiset');
CREATE OR REPLACE TABLE class_bands AS SELECT * FROM minhash_bands('class_minhash');
CREATE OR REPLACE TABLE coverage_q1 AS SELECT * FROM tree_coverage('Q1', k := 3, mu := 0.0);

COMMIT;

SELECT (SELECT count(*) FROM tree_nodes) AS nodes, (SELECT count(*) FROM class_minhash) AS indexed,
       (SELECT count(*) FROM class_bands) AS band_rows, (SELECT count(*) FROM coverage_q1) AS ranked;
```

| nodes | indexed | band_rows | ranked |
|------:|--------:|----------:|-------:|
| 65    | 14      | 3360      | 3      |

A second run gives the same tables. The file does not keep growing: its size alternates between
about one and two copies of the data (1.8 MB and 3.7 MB here), because a replaced table's blocks
are reused on the next run. The results are the same on DuckDB 1.5.6 and 2.0.

When the attribute extract arrives with four rows twice, the `validate_trees` gate stops the run with exit code 1:

```
Invalid Input Error: gate: 4 tree errors, e.g. duplicate_attribute Q1.1
```

Because the script stops inside the transaction, the tables from the last good run stay as they were.

- **Always use `-bail`.** Without it the shell reports the error, runs the remaining statements
  and exits 1 only at the end. A `COMMIT` after a failed statement can then commit part of a run:
  DuckDB does not abort a transaction on a binder error, and most extension argument checks
  (a missing table, a missing column) are binder errors.
- **Why the gates.** The functions skip what they cannot use and do not raise an error.
  An empty extract gives empty result tables. `tree_coverage` silently ignores a node that is
  loaded twice, even with a different class. For an attribute row that is loaded twice with
  different values, it uses the better value. `validate_trees` reports both
  (`duplicate_node`, `duplicate_attribute`), so the gate on its errors stops such a run.
- **Inside a transaction**, the table functions see the tables created and changed earlier in
  the same transaction.

## What to Rebuild When Data Changes

| What changed | What to rebuild | Note |
|---|---|---|
| BOMs, goods movements, trees, attributes | Nothing for `find_similar_materials_*`, `infer_predecessors`, `cold_start_analogs`, `find_covering_entities*`, `tree_coverage`, `tree_coverage_trace` | They read the tables when called. Re-run the queries whose results you store. |
| Trees, class decisions, `overrides_table` | `tree_class_multiset` → `minhash_index` → `minhash_bands`, all three, in this order | A band table that no longer matches its index is an error (`bands_table does not match index_table`), so a skipped step fails the run instead of missing new entities. |
| Item vectors of a source | `classify_to_catalogue` for all items of that source | Centring uses the mean of the source **in the batch**. Classifying only the new items gives other `p` and `margin` values. In the benchmark, one request classified alone moved `p` by up to 0.25. |
| BOMs, for stored embeddings | Upsert into `material_embeddings` for the changed materials | See [incremental updates](../api/06-infrastructure.md#incremental-updates). |

## Serve the Results

DuckDB allows one writing process or any number of read-only processes per file. A reader that
opens the file while the pipeline writes fails with `IO Error: Could not set lock on file`. To
keep readers running, build into a new file and move it over the served one when the run
succeeds:

```bash
./build/release/duckdb -bail < nightly.sql && mv nightly.duckdb serve.duckdb
```

Readers that already have `serve.duckdb` open keep reading the previous version until they
reconnect. Readers open the file with `-readonly` (shell) or `read_only=True` (Python). All
functions work on a read-only database.

- **Versions.** A file created by DuckDB 2.0 cannot be opened by 1.5.6
  (`database file with version number 69`). A file created by 1.5.6 stays readable by 1.5.6
  after 2.0 has written to it. Create the file with the oldest DuckDB version among your readers.
- **Python.** The `duckdb` package must have exactly the version the extension was built for. A
  1.5.6 build fails to load in `duckdb` 1.5.1 (`built specifically for DuckDB version 'v1.5.6'`).
  You need the extension only to call its functions. Stored result tables can be read without it.

```python
import duckdb  # pip install duckdb==1.5.6 for a 1.5.6 build

con = duckdb.connect("serve.duckdb", read_only=True, config={"allow_unsigned_extensions": "true"})
con.execute("LOAD 'build/release/extension/anofox_similarity/anofox_similarity.duckdb_extension'")
print(con.sql("SELECT candidate_id, round(coverage, 3) AS coverage FROM coverage_q1 ORDER BY rank").fetchall())
# [('S1', 1.0), ('S6', 1.0), ('S2', 0.5)]
```

Use `con.sql(...)`, not `duckdb.sql(...)`. The module-level `duckdb.sql` runs on another
connection, which has not loaded the extension.

## Pitfalls

| Pitfall | Solution |
|---|---|
| The file grows on every run | Rewriting rows of a table with HNSW indexes grows the file much faster than rewriting a plain table: 2 000 `combined_embedding` rows rewritten 6 times grew the file to 193 MB (57 MB without the indexes). `PRAGMA hnsw_compact_index` with `CHECKPOINT` did not shrink it. Drop the indexes or keep the extension tables out of the file (see above), or rebuild the file from time to time. |
| `filter_recent_movements` returns no rows | `movement_date` is text such as `'02.01.2023'`. Convert it in a view: `strptime(movement_date, '%d.%m.%Y')::DATE`. |
| `infer_predecessors` / `cold_start_analogs` fail with `-(TIMESTAMP WITH TIME ZONE, INTERVAL)` or `age(BIGINT, BIGINT)` | The built 1.5.6 shell has no ICU extension. Cast `movement_date` to `DATE` or `TIMESTAMP` in a view, or use a DuckDB shell with ICU. |
| `Binder Error: ... does not support lateral join column parameters` | Table functions take constants. For many queries use the batch forms (a LIST of query ids, a queries table) or the [exact batch join](08-scale-and-dedup.md#exact-batch-search). |

## Next Steps

- [Scale and deduplication](08-scale-and-dedup.md): the LSH index this pipeline builds
- [Classification and review](06-classification-and-review.md): the class decisions
- API: [Infrastructure](../api/06-infrastructure.md)
