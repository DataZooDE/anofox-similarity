# Round 2: performance and reliability

## Finding P1 — medium: the scale guide extrapolated 100 separate exact calls

**Location:** `docs/guides/08-scale-and-dedup.md:101` (corrected in this worktree).

**Goal:** Search 100 new BOMs against 20 000 archived BOMs and choose the faster approach.

**Repro:** From this worktree root, start either supplied DuckDB shell and paste the SQL below.
Use `.timer on` in the shell to see statement times. The `exact_hits` statement is the
batched exact comparison; the `hits` statement is the indexed LSH comparison.

```sql
.timer on
CREATE TABLE part_bom AS
SELECT DISTINCT 'A' || lpad(i::VARCHAR, 5, '0') AS entity_id,
       'P' || (hash(i, j) % 50000) AS feature_id, 1.0 AS weight
FROM range(20000) t(i), range(20) u(j);
CREATE TABLE new_boms AS
SELECT 'NEW-' || e.entity_id AS query_id, b.feature_id, b.weight
FROM (SELECT DISTINCT entity_id FROM part_bom ORDER BY entity_id LIMIT 100) e
JOIN part_bom b USING (entity_id)
WHERE hash(b.feature_id) % 4 <> 0;
CREATE TABLE part_minhash AS SELECT * FROM minhash_index(feature_table := 'part_bom');
CREATE TABLE part_bands AS SELECT * FROM minhash_bands('part_minhash');
CREATE TABLE hits AS SELECT * FROM find_covering_entities_lsh(
  'new_boms', 'part_minhash', k := 3, threshold := 0.8,
  bands_table := 'part_bands');
CREATE TABLE exact_hits AS
SELECT q.query_id, p.entity_id,
       sum(least(q.weight, p.weight)) / any_value(t.total) AS containment
FROM new_boms q JOIN part_bom p USING (feature_id)
JOIN (SELECT query_id, sum(weight) AS total FROM new_boms GROUP BY query_id) t USING (query_id)
GROUP BY q.query_id, p.entity_id;
SELECT count(*) AS scored_pairs, count(DISTINCT query_id) AS queries FROM exact_hits;
SELECT count(*) AS found, avg(n_candidates) AS avg_candidates
FROM hits WHERE rank = 1 AND 'NEW-' || entity_id = query_id;
SELECT count(*) AS index_rows FROM part_minhash;
SELECT count(*) AS band_rows FROM part_bands;
SELECT 1 AS still_alive;
```

**Actual:** Both DuckDB 1.5.6 and 2.0 returned `12068,100` exact matching pairs,
`100,1.03` for planted LSH hits, `20000` index rows, `4800000` band rows, and `still_alive=1`.
On this host the LSH search took 0.07–0.16 s and the exact batch join 0.015–0.020 s.
Index construction took about 0.7–0.8 s and band construction 0.26–0.36 s. In a fresh
1.5.6 process, peak RSS for the exact path was about 188 MB; index plus bands reached
about 1.33 GB (Python `resource.getrusage(RUSAGE_CHILDREN).ru_maxrss`). Those are process
peaks with default DuckDB settings, not persistent table sizes.

**Expected:** The guide should compare batch with batch, account for index refresh cost,
and disclose the 240 band rows per entity at 128 hashes. The earlier claim that exact
search costs roughly 8 s for 100 queries multiplied a single-query timing by 100 and
could lead this user to choose the slower, more memory-intensive path.

**Suggested fix:** Benchmark a batched exact join against stored-band LSH on each workload;
document the index/band footprint. **Applied:** The guide now includes this exact query,
its measured comparison, and the band row count.

## Verification and verdict

Both supplied shells completed `examples/evaluation/evaluate_pipeline.sql`,
`fit_coverage_weights.sql`, `minhash_lsh.sql`, and `cluster_matches.sql` with their
published outputs. A 100 000-edge chain produced one 100 001-member cluster in both
versions (about 0.56–0.70 s for the clustering query); `SELECT 1` still worked afterward.
No crash or `INTERNAL` error was observed. The round 1 benchmark fixes sampled by those
examples still produced their documented results.

**Verdict:** A data scientist can evaluate and search this archive, but the former guide
comparison would have pushed them toward an unnecessary index. The substantial band-table
footprint is the main scaling risk to measure before trying 100 000 entities.
