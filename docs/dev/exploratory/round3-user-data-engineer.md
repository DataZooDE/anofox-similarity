# Round 3: data engineer, nightly pipeline on a persistent file

**Goal.** Run the extension as a nightly job on a database file, using
`duckdb db.duckdb < pipeline.sql` and `-bail`. The job loads the seating benchmark and
synthetic BOMs/movements and builds the derived tables (`node_class_decisions` from
`classify_to_catalogue`, `tree_class_multiset` → `minhash_index` → `minhash_bands`, LSH
candidates, `material_embeddings`, `tree_coverage` for 80 requests). Then: re-runs, incremental
updates, read-only reopen, concurrent readers, file growth, error surfacing, and the Python client.

**Setup.** DuckDB 1.5.6 build shell (rebuilt by someone else at 11:30; every finding below was
re-checked on the new binary), DuckDB 2.0 dev shell with `LOAD`, Python `duckdb` 1.5.1 (installed)
and 1.5.6 (venv). Run from the repository root. Scripts are in
`build/tmp/crew3/user-data-engineer/` (`pipeline.sql`, `inc.sql`, `cls.sql`, `val.sql`, `ts.sql`, ...).

## What worked

- **Idempotency.** Three runs of the 80-request pipeline produce the same row counts and the same
  per-table fingerprints (`sum(hash(...))` over each derived table). The fingerprints are also the
  same between 1.5.6 and 2.0. The file size stays bounded (15.7–19 MB over 8 runs).
- **Read-only.** `tree_coverage`, `find_covering_entities_lsh`, `classify_to_catalogue`,
  `vocabulary_review_queue` and `apply_vocabulary_review` all run on a `-readonly` file on both shells.
- **Concurrency.** Two read-only processes at once work. Eight Python threads (cursors on one
  read-only connection) give the same `tree_coverage` result. A reader that opens the file while
  the writer runs fails with DuckDB's lock error. That is expected, and the new guide documents it.
- **Transactions.** Inside `BEGIN … COMMIT`, the table functions see tables created or changed
  earlier in the same transaction. With `-bail`, a failed run leaves the previous tables as they were.
- **Empty extracts.** Every derived step returns 0 rows on empty inputs. Nothing crashes.
- **UR-2 fixes beyond the regression input:**
  - G5 holds for `find_similar_materials_jaccard`, `find_similar_materials_wl_kernel`,
    `find_covering_entities_for` (unit weights) and `minhash_index` + LSH, with duplicates on the
    query side and on the candidate side.
  - G6 holds for all four entry points (`tree_coverage` and `tree_coverage_trace`, query side and
    candidate side).
  - G8 holds for `infer_predecessors` and `cold_start_analogs` with VARCHAR dates, but not for
    `filter_recent_movements` (F3), TIMESTAMPTZ (F6) or a VARCHAR quantity (F9).

## Findings

### F1 [medium-high] The HNSW indexes cannot be switched off. They come back on every load and make the file grow

Every load on a writable file runs `CREATE INDEX IF NOT EXISTS` for three HNSW indexes on
`material_embeddings` (`src/modules/vss_integration.cpp:143`). No extension function queries
them: `compute_fused_embeddings` only reads the table.

```bash
T=build/tmp/crew3/user-data-engineer
./build/release/duckdb $T/h4.duckdb -c "DROP INDEX hnsw_combined_idx; DROP INDEX hnsw_structural_idx; DROP INDEX hnsw_textual_idx;
  INSERT INTO material_embeddings(material_id, combined_embedding)
  SELECT 'm'||i, list_transform(range(768), lambda x: random())::FLOAT[768] FROM range(20000) t(i);"
time ./build/release/duckdb $T/h4.duckdb -c "SELECT count(*) FROM duckdb_indexes()"
# 3 indexes again; real 12.4 s, user 2 min 2 s; file 52 MB -> 117 MB
```

File growth when 2 000 `combined_embedding` rows are rewritten 6 times (`INSERT OR REPLACE`, or
`DELETE` + `INSERT`):

| table | sizes after each run (MB) |
|---|---|
| plain table, same schema | 6.6, 17, 27, 37, 47, 57 |
| `material_embeddings` (auto HNSW) | 13.6, 37.8, 66.9, 102.8, 145.0, 193.5 |

`PRAGMA hnsw_compact_index('hnsw_combined_idx'); CHECKPOINT;` leaves it at 193 MB (637 used blocks).

**Expected:** a user who does not use the embedding tables can keep them, or at least the
indexes, out of their file. **Suggested fix:** add an option such as
`SET anofox_embedding_tables = false`, or create the indexes only together with a newly created
table, so that a dropped index stays dropped. **Documented** for now (workaround: start without a
file and `ATTACH`): `docs/api/06-infrastructure.md`, `docs/guides/09-nightly-pipelines.md`.

### F2 [medium] A stale `minhash_bands` table is not detected: LSH silently misses new entities

```sql
-- in a copy of the pipeline file: P901 = copy of P014, then P014 is removed
-- (build/tmp/crew3/user-data-engineer/inc.sql)
CREATE OR REPLACE TABLE programme_ms AS SELECT * FROM tree_class_multiset('prog_nodes', overrides_table := 'vocabulary_overrides');
CREATE OR REPLACE TABLE programme_minhash AS SELECT * FROM minhash_index(feature_table := 'programme_ms');
-- programme_bands NOT rebuilt
SELECT entity_id, round(estimate,3), rank FROM find_covering_entities_lsh('request_ms', 'programme_minhash', k := 3, bands_table := 'programme_bands') WHERE query_id = 'R03';
-- P162 1.0 | P086 0.992 | P097 0.933     (P901 missing, no error)
-- after rebuilding the bands:  P162 | P086 | P901 0.992
```

The num_hashes/seed check (UR-1 F14) only catches different parameters, not a band table from an
older build. In a nightly job a failed or skipped band step gives wrong recall and no error.
**Suggested fix:** `minhash_index` writes a build fingerprint (for example a hash of the
entity ids and signatures) into each row, `minhash_bands` copies it, and
`find_covering_entities_lsh` compares them. A cheaper check: compare the number of distinct
entities in the bands table and in the index. **Documented:** the guide 08 pitfall now says the
check does not happen; guide 09 has a rebuild-order table.

### F3 [medium] `filter_recent_movements` returns 0 rows for German text dates (the G8 pattern not applied)

```sql
CREATE TABLE g AS SELECT 'M' material_id, strftime(DATE '2023-01-02' + 7*w::INT, '%d.%m.%Y') movement_date, 1.0 quantity FROM range(52) t(w);
SELECT count(*) FROM filter_recent_movements('g', 30, 0);   -- 0 (1.5.6 and 2.0)
-- ISO text dates: 5; DATE: 5
```

**Expected:** the G8 error (`column movement_date is VARCHAR, expected DATE or TIMESTAMP; …`), as
`infer_predecessors` and `cold_start_analogs` give. **Suggested fix:** run the same type check in
the macro. **Documented** in the guide 09 pitfalls.

### F4 [medium] `validate_trees` misses duplicate attribute rows and NULL `tree_id`, and `tree_coverage` silently uses the better value

```sql
-- in the pipeline file (val.sql / dup3.sql)
CREATE TEMP TABLE pa_dup AS SELECT * FROM prog_attrs UNION ALL
  SELECT tree_id, node_id, attr_id, name, dimension, unit, 0.1, 0.1, role, bound, qualifier, category
  FROM prog_attrs WHERE node_id = 'P065.B01' AND attr_id = 'mass';
SELECT * FROM validate_trees('prog_nodes', 'temp.pa_dup');      -- 0 rows
SELECT prov_value, value_score FROM tree_coverage_trace('R19', candidates := ['P065'], nodes_table := 'req_nodes', attrs_table := 'req_attrs',
  candidate_nodes_table := 'prog_nodes', candidate_attrs_table := 'temp.pa_dup', overrides_table := 'vocabulary_overrides',
  default_bounds := 'catalogue', templates_table := 'templates', rules_table := 'compatibility_rules', mu := 0.0)
WHERE attr_name = 'Gewicht';                                      -- 0.1 kg | 1.0  (was 1.4 kg | 0.0), whatever the row order
-- a row with tree_id NULL appended to the nodes: validate_trees reports nothing either
```

Duplicate nodes are reported (`duplicate_node`). A duplicate node with a different class is
silently ignored by `tree_coverage`. A duplicate attribute row with a better value makes the
programme look better than the archive says. **Suggested fix:** add `duplicate_attribute` and
`null_key` errors to `validate_trees`. **Documented:** guide 09 shows a `GROUP BY … HAVING`
gate for duplicate attributes.

### F5 [medium] `classify_to_catalogue` results depend on the batch: classifying new items alone does not reproduce a full run

```sql
-- cls.sql: R80's 27 items (one source) alone vs the stored full-batch decisions
CREATE TEMP TABLE new_items AS SELECT * FROM request_items WHERE tree_id = 'R80';
... classify_to_catalogue(items_table := 'new_items', catalogue_table := 'catalogue', top := 1) ...
-- items 27 | class_changed 0 | max_p_diff 0.249
```

Centring uses the mean of the source in the batch. An incremental nightly run therefore gives
other `p` and `margin` values than a full rebuild. This moves items across the auto-accept
threshold and in and out of `uncertain_decisions`. Guide 06 said only "classify items in
batches". **Suggested fix (code):** a `source_means_table` parameter, or a way to return and
reuse stored centroids, so that an incremental run uses fixed means. **Documented:** guide 06
pitfall, guide 09 rebuild table.

### F6 [low-medium] TIMESTAMPTZ `movement_date` on the built 1.5.6 shell: raw binder errors

The build shell has no ICU. `cold_start_analogs` then fails with:

```sql
CREATE TABLE gm_tz AS SELECT material_id, movement_date::TIMESTAMP::TIMESTAMPTZ AS movement_date, quantity FROM gm_date;  -- see ts.sql
SELECT * FROM infer_predecessors('NEW-PUMP', movements_table := 'gm_tz');
-- Binder Error: No function matches ... '-(TIMESTAMP WITH TIME ZONE, INTERVAL)' (+ 40 candidate lines)
SELECT * FROM cold_start_analogs('NEW-PUMP', 5, min_history_months := 6, movements_table := 'gm_tz');
-- Binder Error: No function matches ... 'age(BIGINT, BIGINT)'
```

The same SQL works on 2.0 and on the stock `/usr/bin/duckdb` 1.5.6 with ICU. TIMESTAMPTZ is what
a Postgres or warehouse extract usually has. `filter_recent_movements` on TIMESTAMPTZ also
returns a window shifted by one day on the build shell compared with 2.0 (min date 2023-11-28 vs
2023-11-27). **Suggested fix:** cast `movement_date` to TIMESTAMP inside the macros, or add `icu`
to `extension_config.cmake`. **Documented** in the guide 09 pitfalls.

### F7 [low-medium] On 2.0, `material_embeddings` is created without HNSW indexes. A 1.5.6 file with the indexes cannot be written on 2.0

```bash
# 2.0 shell with LOAD, new file: tables are created, duckdb_indexes() is empty; vss: loaded=false, installed=false
# the pipeline file created by 1.5.6:
INSERT INTO material_embeddings(material_id) VALUES ('zz');
-- Missing Extension Error: Cannot bind index "material_embeddings", unknown index type 'HNSW'.
```

The documented Jaccard-embedding upsert (`docs/api/04-embeddings.md`) therefore fails in a 2.0
nightly run against a file that 1.5.6 created. On 2.0 the indexes are skipped without a message.
This may change once `vss` is published for 2.0. **Suggested fix:** print a notice when the
indexes are skipped (F1's option would also cover this). **Documented** in
`docs/api/06-infrastructure.md`.

### F8 [low] `vocabulary_review_queue` returns an empty queue silently when the decisions match no node (the F1 pattern from UR-1)

```sql
SELECT count(*) FROM vocabulary_review_queue(trees_table := 'prog_nodes', decisions := 'node_class_decisions');  -- 0
-- node_class_decisions holds the request items; with trees_table := 'req_nodes': 1513
```

A nightly check like "review queue is empty → nothing to do" passes. **Suggested fix:** raise an
error when no decision joins any node of `trees_table`, as UR-1 F1 did for unknown query ids.

### F9 [low] `infer_predecessors` with a VARCHAR `quantity`: raw `sum(VARCHAR)` binder error

`cold_start_analogs` accepts the same table. **Suggested fix:** extend the G8 type check to
`quantity`.

### F10 [low] Binder errors do not abort a transaction, so a script without `-bail` can commit part of a run

```sql
BEGIN; CREATE OR REPLACE TABLE coverage_results AS SELECT 1 AS x;
SELECT * FROM tree_coverage('R03', nodes_table := 'nope');   -- Binder Error
COMMIT;                                                       -- without -bail: commits coverage_results = 1 row
```

This is DuckDB behaviour, and most of the extension's argument checks are binder errors.
**Documented:** guide 09 says to always use `-bail`.

### F11 [low, doc] No guide for scheduled jobs, and the README Python one-liner does not run

`duckdb.sql(...)` runs on the default connection. A `LOAD` on a connection opened with
`allow_unsigned_extensions` does not apply to it (`Catalog Error: Scalar Function with name
jaccard_similarity does not exist!`). The client version must match the build exactly (installed
1.5.1: `built specifically for DuckDB version 'v1.5.6'`). **Fixed:** README Python row and note;
new `docs/guides/09-nightly-pipelines.md`; guides index.

Not verified: when `LOAD vss` fails, the extension load runs `INSTALL vss`
(`vss_integration.cpp:48`), a network access on an air-gapped scheduler. Seen in the source only.

## Doc changes (verified on both shells)

- `docs/guides/09-nightly-pipelines.md` (new). The SQL block was extracted verbatim and run twice
  on 1.5.6 and twice on 2.0: the output matches, and the file does not keep growing (it alternates
  between 1.8 and 3.7 MB). The gate failure (`gate: 4 duplicate attribute rows`, exit 1, previous
  tables kept) was reproduced on both shells. The Python snippet was run with `duckdb` 1.5.6. The
  build-then-`mv` swap was tested with a long-running reader.
- `docs/guides/README.md`: row 09 and a where-to-start row. `README.md`: the guides list, the
  Python row and the client-version note.
- `docs/api/06-infrastructure.md`: indexes come back on every load, file growth, the ATTACH
  workaround, 2.0 without vss.
- `docs/guides/06-classification-and-review.md`: pitfall for batch-dependent `p`/`margin`.
- `docs/guides/08-scale-and-dedup.md`: the stale-index pitfall now says a stale band table is not
  detected.

`python3 scripts/generate_api_reference.py --check` reports `04-embeddings.md` and
`16-lsh-and-clustering.md` as out of date against the rebuilt 1.5.6 binary. I did not edit those
pages. The drift comes from the rebuild, so regenerate them after the code changes of this round.

## Verdict

Yes. A data engineer gets an idempotent, gated nightly pipeline on 1.5.6 and 2.0 within one
sitting, and the results are deterministic across runs, versions and threads. Two things
slowed me most. First, the extension's own tables and HNSW indexes write to my file, cannot be
switched off, and make the file grow much faster (F1). Second, several stale or dirty inputs
give silently wrong results instead of errors: an old band table, duplicate attribute rows,
German text dates in `filter_recent_movements`, an empty review queue (F2–F4, F8). Before this
round nothing told me what to rebuild when new data arrives. Guide 09 now does.
