---
name: anofox-similarity-evaluation-scale
description: >
  Evaluate, tune and scale anofox_similarity matching: `eval_ranking`, `eval_calibration`,
  `threshold_at_precision`, `bootstrap_ci`, `fit_coverage_weights`; MinHash/LSH search with
  `weighted_minhash`, `minhash_similarity`, `lsh_bands`, `minhash_index`, `minhash_bands`,
  `find_covering_entities_lsh`; dedup clustering with `connected_components`, `cluster_matches`.
  Use when asking "how good is my matching", "auto-accept threshold at 95 % precision", "confidence
  interval per customer", "tune label_weight / mu", "is LSH worth it", "merge duplicate records", or
  "deduplicate the customer / supplier / plant master" (cluster scored pairs into entities).
version: 2026.11.13
user-invocable: false
---

# Anofox Similarity — Evaluation, Tuning & Scale Cheat Sheet

**Extension:** `anofox_similarity` v2026.11.13 | **DuckDB:** v2.0 / v1.5.6 | **Reference:** [`docs/api/15-evaluation.md`](https://github.com/DataZooDE/anofox-similarity/blob/main/docs/api/15-evaluation.md), [`docs/api/16-lsh-and-clustering.md`](https://github.com/DataZooDE/anofox-similarity/blob/main/docs/api/16-lsh-and-clustering.md)

> Paths like `test/data/...` are relative to a clone of the anofox-similarity repository; in
> your own project, substitute your tables. Examples that build inline tables run anywhere.

Measure a matching pipeline against reviewed results, pick an operating threshold, put
confidence intervals on the KPIs and fit the coverage kernel's weights; then scale the
candidate search with weighted MinHash/LSH and turn scored pairs into duplicate clusters.
The searches being evaluated (`find_covering_entities`, `tree_coverage`, their parameters and
output) are documented in the **anofox-similarity-coverage** skill; this skill only consumes
their output.

## Critical gotchas

- **LSH `threshold` only sets recall.** It is a containment threshold in mode `'containment'` and a
  Jaccard threshold in mode `'jaccard'`; it tunes the banding, not a filter on the output, and
  fewer than `k` rows per query is normal (only candidates sharing a band are scored).
- **Every eval function takes table *names* (strings), not queries.** A subquery as a table
  function argument is rejected ("Table function cannot contain subqueries"). Materialise with
  `CREATE TABLE`, pass scalars with `SET VARIABLE x = (...)` and `getvariable('x')`.
- **Column names are fixed**: results need `query_id, candidate_id` plus `rank` (or a score
  column, default `coverage`); gold needs `query_id, candidate_id, true_coverage`. Rename first.
- **`rank` beats score.** If the results table has a `rank` column it is used; the score orders
  only when there is no rank column (or pass `rank_col` / `score_col`). Ranks start at 1 (rank 0
  is an error); tied ranks are broken by candidate id.
- **Gold drives the query set.** `eval_ranking` scores the queries that have gold: a gold query
  without results is a miss with full regret; results for queries without gold are ignored. A
  ranked candidate without a gold row counts as true coverage **0**, not as missing.
- **"Best" is a set**: all gold candidates within 1e-9 of the query's gold best. With ties,
  `top_k_accuracy` (any best in top k) and `recall_at_k` (share of the best set in top k) differ.
- **`target` is reserved** — the parameter is `target_precision`. `good` (default 0.9) is the true
  coverage a pick needs to count as correct; both must be in [0, 1].
- **Tune and report on different queries.** Fit on one split, report on the holdout.
- **Cluster the bootstrap** (customer, maker, plant). Requests of one customer are not
  independent; a NULL cluster value is an error. Bounds use DuckDB's `hash`, so they can shift
  slightly between DuckDB versions (same seed → same interval within a version).
- **Exact search first.** `find_covering_entities_lsh` is an approximate accelerator; a batched
  exact join was *faster* on 20 000 and 100 000 BOMs. With a small pivot vocabulary (~20 classes)
  LSH still scores about half the archive. Measure before switching, and measure LSH recall
  against the exact result.
- **`num_hashes` must be a multiple of 8 between 8 and 4096** (`minhash_index`); `lsh_bands`
  needs `len(signature)` divisible by `bands`; `minhash_similarity` needs equal lengths.
- **Store the band table.** Without `bands_table` the index is unnested on every call (linear).
  Rebuild multiset → `minhash_index` → `minhash_bands` together; a band table from another index
  (or seed/num_hashes) is an error ("bands_table does not match index_table").
- **Clustering is transitive.** One bad pair above the threshold merges two clusters. Block
  strictly; entities that occur only in pairs below the threshold still appear as singletons, but
  records with no pair at all are absent — add a self-pair `(id, id, 1.0)`.
- **The extension clusters pairs; it does not score records.** For customer / supplier / plant
  dedup there is no name or address normaliser: block (same country and postal code, same tax
  id), score the pairs with DuckDB's `jaro_winkler_similarity` / `levenshtein` on normalised
  names (strip legal forms such as GmbH, AG, & Co. KG) plus exact matches on tax id and street,
  then `cluster_matches`. Without labelled pairs, check the merges of a sample by hand.
- **Tables can be read from an attached database**, also a `READ_ONLY` one:
  `minhash_index(feature_table := 's.programme_class_multiset')`. Results must still be written
  to a writable (temp) table.
- **LSH has no self-exclusion.** A query that is also in the index usually returns itself, but with
  ties at an estimate of 1.0 not always first: ask for `k + 1` and drop `entity_id = query_id`.

## Input formats

| Table | Required columns | Used by |
|---|---|---|
| results | `query_id, candidate_id`, `rank` (INT ≥ 1) and/or a score (default `coverage`) | `eval_ranking`, `eval_calibration`, `threshold_at_precision` |
| gold | `query_id, candidate_id, true_coverage` (DOUBLE in [0, 1]) | same three |
| metric table | one cluster column + numeric metric columns (one row per query) | `bootstrap_ci` |
| reviewed matches | `query_id, candidate_id, true_coverage` + the trees (as for `tree_coverage`) | `fit_coverage_weights` |
| feature multiset | `entity_id, feature_id[, weight]` (remap with `entity_col`/`feature_col`/`weight_col`) | `minhash_index` |
| queries | `query_id, feature_id[, weight]` — many queries in one table | `find_covering_entities_lsh` |
| pairs | `a, b, score` (remap with `left_col`/`right_col`/`score_col`) | `cluster_matches` |

`tree_coverage` output already has the results shape (`query_id, candidate_id, coverage, rank, ...`).

Sample data used below (results from a matcher, gold from reviewers):

```sql
CREATE TABLE my_results AS SELECT * FROM (VALUES
  ('Q1', 'S2', 1, 0.97), ('Q1', 'S1', 2, 0.95), ('Q1', 'S6', 3, 0.90),
  ('Q2', 'S5', 1, 0.60), ('Q2', 'S1', 2, 0.55)) t(query_id, candidate_id, rank, coverage);
CREATE TABLE my_gold AS SELECT * FROM (VALUES
  ('Q1', 'S2', 1.0), ('Q1', 'S1', 1.0), ('Q1', 'S6', 0.8),
  ('Q2', 'S5', 0.5), ('Q2', 'S1', 0.9)) t(query_id, candidate_id, true_coverage);
```

## `eval_ranking` — ranking quality

```text
eval_ranking(results, gold [, k := [1, 5, 10, 20], rank_col := 'rank', score_col := 'coverage'])
→ k, n_queries, top_k_accuracy, recall_at_k, mrr, mean_regret, share_regret_le_005   (one row per k)
```

```sql
SELECT k, n_queries, top_k_accuracy, recall_at_k, mrr, round(mean_regret, 3) AS mean_regret
FROM eval_ranking('my_results', 'my_gold', k := [1, 2]);
-- k=1: 2 | 0.5 | 0.25 | 0.5  | 0.2
-- k=2: 2 | 1.0 | 1.0  | 0.75 | 0.2
-- => 1 | 2 | 0.5 | 0.25 | 0.5 | 0.2
-- => 2 | 2 | 1.0 | 1.0 | 0.75 | 0.2
```

| Metric | Meaning |
|---|---|
| `top_k_accuracy` | share of queries with a best candidate in the top k (BRD "recall@k of the best candidate") |
| `recall_at_k` | share of each query's best candidates in the top k, averaged |
| `mrr` | mean reciprocal rank of the first best candidate, cut at k |
| `mean_regret` | mean of (gold best − true coverage of the top-1 pick); same on every k row |
| `share_regret_le_005` | share of queries whose top-1 pick is within 0.05 of the best; same on every k row |

Duplicate rows of a pair keep the best rank (or are de-duplicated before score ranking). `k`
values must be positive.

## `eval_calibration` — is the score a usable coverage?

```text
eval_calibration(results, gold [, score_col := 'coverage'])
→ n_pairs, mae, bias, pearson, spearman      (one row, over pairs present in both tables)
```

```sql
SELECT n_pairs, round(mae, 3) AS mae, round(bias, 3) AS bias, round(spearman, 3) AS spearman
FROM eval_calibration('my_results', 'my_gold');
-- 5 | 0.126 | -0.046 | 0.667
-- => 5 | 0.126 | -0.046 | 0.667
```

`bias` = predicted − true (negative: scores too pessimistic). Spearman uses average ranks for
ties; both correlations are NULL for a constant input. High Spearman with large MAE means the
ranking is fine but the number should not be read as a coverage — don't threshold on it blindly.

## `threshold_at_precision` — the auto-accept threshold

```text
threshold_at_precision(results, gold [, target_precision := 0.95, good := 0.9,
                                        rank_col := 'rank', score_col := 'coverage'])
→ threshold, precision, accepted_share, n_queries,
  curve STRUCT(threshold, precision, share)[]
```

Looks only at the top-1 pick per query. A pick is good when its true coverage ≥ `good`. Returns
the **lowest** score threshold whose precision reaches the target, i.e. the largest share of
queries that can skip review. Tied scores enter together. Unreachable target → `threshold` and
`precision` NULL (the `curve` is still filled).

```sql
SELECT threshold, precision, accepted_share, n_queries, len(curve) AS curve_points
FROM threshold_at_precision('my_results', 'my_gold', target_precision := 0.95);
-- 0.97 | 1.0 | 0.5 | 2 | 2   → accept picks with coverage >= 0.97: half the queries, all correct

SELECT c.threshold, c.precision, c.share
FROM threshold_at_precision('my_results', 'my_gold'), unnest(curve) AS u(c);   -- full precision curve
```

Operating rule: apply `coverage >= threshold` on the rank-1 row of new results; route the rest
to review. Re-derive the threshold when the pipeline, vocabulary or data changes. Report the
threshold's precision on a holdout, not on the data it was chosen on. On the seating benchmark
(80 requests) the result is threshold 0.769, precision 0.958, accepted share 0.888.

## `bootstrap_ci` — confidence intervals by cluster

```text
bootstrap_ci(metric_table, cluster_col [, metrics := <all other numeric columns>,
             n := 1000, seed := 7, level := 0.95])
→ metric, estimate, lo, hi, n_boot, n_clusters      (one row per metric)
```

Resamples whole clusters with replacement `n` times; `estimate` is the **pooled row mean**
(not the mean of cluster means); `lo`/`hi` are the percentile interval. Build one row per query
with 0/1 hits and regrets first:

```sql
CREATE TABLE per_query AS SELECT * FROM (VALUES
  ('maker_a', 'q1', 1.0, 0.0), ('maker_a', 'q2', 0.0, 0.1), ('maker_b', 'q3', 1.0, 0.0),
  ('maker_c', 'q4', 1.0, 0.05), ('maker_c', 'q5', 1.0, 0.0)) t(maker, query_id, hit, regret);

SELECT metric, estimate, lo, hi, n_clusters
FROM bootstrap_ci('per_query', 'maker', metrics := ['hit', 'regret']) ORDER BY metric;
-- hit 0.8 [0.5, 1.0], regret 0.03 [0.0, 0.05], 3 clusters
```

A metric name that does not exist is an error naming it. With few clusters the interval is
wide by design; that is the honest answer.

## `fit_coverage_weights` — tune `label_weight` (λ) and `mu` (μ)

```text
fit_coverage_weights(reviewed_matches [, label_weights := [0, 0.1, 0.2, 0.3, 0.5],
                     mus := [0, 0.1, 0.2, 0.3], <tree_coverage parameters: nodes_table,
                     attrs_table, candidate_nodes_table, candidate_attrs_table, overrides_table,
                     label_table, labels, assignment, partition_gate, count_gate, default_bounds,
                     templates_table, rules_table, class_hierarchy, generalization_credit,
                     alternatives, name_vectors, name_floor, tol, soft_band>])
→ label_weight, mu, mae, n_pairs, n_unresolved, rank      (one row per grid point, rank 1 = best)
```

Runs the coverage kernel over the reviewed pairs for every (λ, μ) grid point and ranks by MAE
against `true_coverage`. At most 20 values per axis; values must be in [0, 1].
`n_unresolved` counts reviewed pairs whose trees are missing — check it is 0. Pass the same
tree/option parameters you use for `tree_coverage` (anofox-similarity-coverage skill). The
classifier's `attr_bonus` / `temperature` are **not** tuned here: use `classify_to_catalogue` +
`eval_ranking` (anofox-similarity-classification-review skill).

```sql
CREATE OR REPLACE TABLE tree_nodes AS SELECT * FROM read_csv_auto('test/data/coverage/mini/tree_nodes.csv');
CREATE OR REPLACE TABLE tree_attributes AS SELECT * FROM read_csv_auto('test/data/coverage/mini/tree_attributes.csv');
CREATE TABLE reviewed_matches AS SELECT * FROM (VALUES
    ('Q1', 'S1', 1.0::DOUBLE), ('Q1', 'S6', 1.0), ('Q1', 'S3', 0.625), ('Q1', 'S2', 0.5),
    ('Q1', 'S4', 0.125), ('Q1', 'S5', 0.0)) t(query_id, candidate_id, true_coverage);

SELECT label_weight, mu, round(mae, 4) AS mae, n_pairs, n_unresolved, rank
FROM fit_coverage_weights('reviewed_matches', label_weights := [0.0, 0.3], mus := [0.0, 0.2])
ORDER BY rank;
-- best: label_weight 0.0, mu 0.0, mae 0.0, 6 pairs, 0 unresolved
```

Then rerun `tree_coverage` on the holdout queries with `label_weight := getvariable('best_lw')`,
`mu := getvariable('best_mu')` and compare `eval_calibration` MAE to the defaults (λ 0.0, μ 0.2).
Change the defaults only if the holdout improves.

## Exact search vs LSH — decide first

| Situation | Use |
|---|---|
| One or a few queries, any archive size (~0.15–0.2 s for 100 000 entities × 15 features) | `find_covering_entities` / `find_covering_entities_for` (coverage skill) |
| Many queries at once, join fits in time/memory | exact batch join on shared features (below) — ~0.02 s for 100 queries × 20 000 BOMs |
| Small pivot vocabulary (catalogue classes, ~20) | exact — LSH still scores ~half the archive |
| Large raw vocabulary (part numbers, BOM components) **and** the exact join no longer fits the budget | `minhash_index` + `minhash_bands` + `find_covering_entities_lsh`, recall checked against exact |

Index cost: 128 hashes → `minhash_bands` stores 240 rows per entity (4.8 M rows for 20 000
entities); ~2 s index + 0.7 s bands for 20 000 BOMs, ~5 s at 100 000.

## `weighted_minhash`, `minhash_similarity`, `lsh_bands` — scalars

```text
weighted_minhash(weights MAP(VARCHAR, DOUBLE), num_hashes BIGINT [, seed BIGINT := 7]) → UBIGINT[]
minhash_similarity(signature_a UBIGINT[], signature_b UBIGINT[]) → DOUBLE
lsh_bands(signature UBIGINT[], bands BIGINT) → UBIGINT[]        -- one hash per band of len/bands rows
```

ICWS weighted MinHash: the share of equal positions estimates weighted Jaccard. NULL weight = 1,
zero weight = absent, negative = error; NULL or empty map → NULL. Hashing is the extension's own
(FNV-1a, SplitMix64), stable across versions, so stored signatures stay valid.

```sql
SELECT round(weighted_jaccard(MAP {'a': 1.0, 'b': 1.0}, MAP {'a': 1.0, 'b': 3.0}), 3) AS exact,
       round(minhash_similarity(weighted_minhash(MAP {'a': 1.0, 'b': 1.0}, 1024),
                                weighted_minhash(MAP {'a': 1.0, 'b': 3.0}, 1024)), 3) AS estimate,
       len(lsh_bands(weighted_minhash(MAP {'a': 1.0}, 64), 16)) AS n_bands;
-- 0.5 | 0.525 | 16      (more hashes → closer estimate; error ~ 1/sqrt(num_hashes))
```

**Band/row trade-off.** With `num_hashes = n` split into `b` bands of `r` rows (`n = b·r`), two
sets with Jaccard J share at least one band with probability `1 − (1 − J^r)^b`. Few rows per band
→ high recall, many candidates; more rows → fewer candidates, low-J pairs missed. The S-curve's
midpoint is about `(1/b)^(1/r)`:

```sql
SELECT r, 128 // r AS bands, round((1.0 / (128 // r)) ** (1.0 / r), 2) AS midpoint_j,
       round(1 - (1 - 0.3 ** r) ** (128 // r), 3) AS p_j030,
       round(1 - (1 - 0.5 ** r) ** (128 // r), 3) AS p_j050,
       round(1 - (1 - 0.8 ** r) ** (128 // r), 3) AS p_j080
FROM (VALUES (1), (2), (4), (8)) t(r) ORDER BY r;
-- r=1: every pair with any overlap is a candidate; r=8 (16 bands): J 0.3 almost never, J 0.8 almost always
```

`find_covering_entities_lsh` makes this choice for you (rows 1, 2, 4 or 8 per partition, see
below). Use `lsh_bands` directly only for a custom pipeline, e.g. Jaccard near-duplicate
candidates by joining on equal band hashes:

```sql
CREATE TABLE sigs AS
SELECT entity_id, weighted_minhash(map(list(feature_id), list(weight::DOUBLE)), 64) AS sig
FROM read_csv_auto('test/data/coverage/mini/feature_multiset.csv') GROUP BY entity_id;
CREATE TABLE sig_bands AS
SELECT entity_id, generate_subscripts(lsh_bands(sig, 16), 1) AS j, unnest(lsh_bands(sig, 16)) AS h FROM sigs;
WITH cand AS (
    SELECT DISTINCT x.entity_id AS a, y.entity_id AS b
    FROM sig_bands x JOIN sig_bands y ON x.j = y.j AND x.h = y.h AND x.entity_id < y.entity_id)
SELECT c.a, c.b, round(minhash_similarity(sa.sig, sb.sig), 3) AS j_est
FROM cand c JOIN sigs sa ON sa.entity_id = c.a JOIN sigs sb ON sb.entity_id = c.b
ORDER BY j_est DESC;   -- 16 bands x 4 rows: candidate pairs, then their estimated weighted Jaccard
```

## `minhash_index`, `minhash_bands`, `find_covering_entities_lsh`

```text
minhash_index(feature_table := 'feature_multiset' [, num_hashes := 128, seed := 7, partitions := 4,
              entity_col, feature_col, weight_col])           -- weight_col := NULL → unit weights
→ entity_id, total_weight, partition, signature, band_hashes, num_hashes, seed

minhash_bands(index_table)
→ entity_id, part, r, j, h, num_hashes, seed      -- long form; one row per entity × r ∈ {1,2,4,8} × band

find_covering_entities_lsh(queries_table, index_table [, k := 20, mode := 'containment',
                           threshold := 0.5, bands_table])
→ query_id, entity_id, estimate, jaccard_estimate, rank, n_candidates
```

How it works (LSH-Ensemble style): entities are split into `partitions` size quantiles. Per query
and partition, the `threshold` (containment in mode `'containment'`, Jaccard in mode
`'jaccard'`) is turned into a Jaccard threshold for the
partition's largest set, and the rows per band (1, 2, 4 or 8) are chosen so the S-curve passes
below it. Candidates sharing a band are scored: containment estimate
`J (|q| + |c|) / ((1 + J) |q|)` capped at 1 (`mode := 'jaccard'` ranks by J). `n_candidates` is
the number scored per query — your cost metric. Size partitions let a small query be found
inside a large entity (plain Jaccard banding would miss it).

Knobs: **lower `threshold`** or **raise `num_hashes`** when LSH misses candidates; raise
`threshold` to score fewer. `mode` is `'containment'` or `'jaccard'`; negative weights in the
index or queries are an error; an index mixing `num_hashes`/`seed` is an error.

```sql
-- a raw vocabulary: 2 000 entities x 20 of 5 000 part numbers
CREATE TABLE raw_vocab AS
SELECT DISTINCT 'e' || i AS entity_id, 'f' || (hash(i * 100 + j) % 5000) AS feature_id, 1.0 AS weight
FROM range(2000) t(i), range(20) u(j);
CREATE TABLE raw_idx AS SELECT * FROM minhash_index(feature_table := 'raw_vocab', num_hashes := 128);
CREATE TABLE raw_bands AS SELECT * FROM minhash_bands('raw_idx');          -- store once
-- queries: 12 of e17's features (a partial BOM)
CREATE TABLE raw_q AS SELECT 'q' AS query_id, feature_id, weight
FROM raw_vocab WHERE entity_id = 'e17' ORDER BY feature_id LIMIT 12;

SELECT query_id, entity_id, rank, round(estimate, 2) AS estimate, n_candidates
FROM find_covering_entities_lsh('raw_q', 'raw_idx', k := 3, threshold := 0.8, bands_table := 'raw_bands');
-- one row: e17, rank 1, estimate 0.99, n_candidates 1 (of 2 000). Fewer than k rows is normal:
-- only candidates that share a band are scored; entities far below the threshold are not found
```

**Exact batch reference** (also the recall check for LSH):

```sql
CREATE TABLE exact_hits AS
WITH shared AS (
    SELECT q.query_id, b.entity_id, sum(least(q.weight, b.weight)) AS shared_weight
    FROM raw_q q JOIN raw_vocab b USING (feature_id) GROUP BY q.query_id, b.entity_id),
query_weight AS (SELECT query_id, sum(weight) AS query_weight FROM raw_q GROUP BY query_id),
scored AS (
    SELECT query_id, entity_id, shared_weight / query_weight AS containment
    FROM shared JOIN query_weight USING (query_id)),
ranked AS (
    SELECT *, row_number() OVER (PARTITION BY query_id ORDER BY containment DESC, entity_id) AS rank
    FROM scored)
SELECT * FROM ranked WHERE rank <= 3;

-- recall of the exact best (containment 1.0 here) in the LSH top 3; ~1.0 expected.
-- Do not compare against the exact top 3: its tail (low containment) is below the threshold.
SELECT avg((l.entity_id IS NOT NULL)::INT) AS best_found_by_lsh
FROM exact_hits e
LEFT JOIN find_covering_entities_lsh('raw_q', 'raw_idx', k := 3, threshold := 0.8,
                                     bands_table := 'raw_bands') l
       ON l.query_id = e.query_id AND l.entity_id = e.entity_id
WHERE e.rank = 1;
```

For class-level coverage archives, feed `tree_class_multiset` output (renamed `entity_id AS
query_id`) as the queries table; see anofox-similarity-coverage for building multisets.

## `connected_components`, `cluster_matches` — duplicate clustering

```text
cluster_matches(pairs_table [, threshold := 0.8, left_col := 'a', right_col := 'b', score_col := 'score'])
→ entity_id VARCHAR, cluster_id VARCHAR, cluster_size BIGINT
connected_components(pairs LIST(STRUCT(a, b))) → LIST(STRUCT(entity_id VARCHAR, cluster_id VARCHAR))
```

C++ union-find over pairs with score **≥** threshold (inclusive; threshold in [0, 1]). Every
entity of the pairs table appears; ids become text; `cluster_id` = smallest member id in text
order; NULL ids are ignored; an empty pairs table returns 0 rows. `connected_components` is the
scalar behind it (takes any 2-field struct list, sorted output; 100 000 pairs in one call is fine).

```sql
CREATE TABLE customers AS SELECT * FROM (VALUES
  ('C1', 'Brennholt GmbH',     'Ulm',  'DE111'),
  ('C2', 'Brennholt G.m.b.H.', 'Ulm',  'DE111'),
  ('C3', 'Brenholt GmbH',      'Ulm',  NULL),
  ('C4', 'Aurivon AG',         'Ulm',  'DE222'),
  ('C5', 'Aurivon Seating AG', 'Köln', 'DE333')) t(customer_id, name, city, tax_id);

-- block (same city), normalise, score; self-pairs keep records without candidates
CREATE TABLE cust_pairs AS
SELECT a.customer_id AS a, b.customer_id AS b,
       0.6 * jaro_winkler_similarity(lower(regexp_replace(a.name, '[^A-Za-z ]', '', 'g')),
                                     lower(regexp_replace(b.name, '[^A-Za-z ]', '', 'g')))
     + 0.4 * coalesce((a.tax_id = b.tax_id)::INT, 0) AS score
FROM customers a JOIN customers b ON a.city = b.city AND a.customer_id < b.customer_id
UNION ALL SELECT customer_id, customer_id, 1.0 FROM customers;

SELECT * FROM cluster_matches('cust_pairs', threshold := 0.55) ORDER BY entity_id;
-- C1, C2, C3 → cluster C1 (size 3); C4, C5 singletons

SELECT unnest(connected_components(list({'a': a, 'b': b}))) AS member
FROM cust_pairs WHERE score >= 0.55;                     -- same result via the scalar
```

Dedup recipe (benchmark customer master, `test/sql/coverage/r5/uc_03_merge_duplicate_customer_plants.test`,
`examples/dedup_and_lsh/cluster_matches.sql`): normalise (lower case, umlauts → ae/oe/ue,
`straße` → `str`, legal forms unified), build an identity per record type, block on shared tax
id or city, score `0.6 × jaro_winkler_similarity of the identities + 0.3 × same tax id + 0.1 × same address`,
cluster at 0.85 → all 152 planted duplicates merged, none of 12 near misses. Address agreement
alone is not enough (near misses share the address). To pick the threshold, label some pairs and
reuse `threshold_at_precision` logic, or inspect cluster sizes: a sudden giant cluster means a
transitive bad link. For graph queries, the docs give an optional DuckPGQ recipe (not loaded by
the extension).

## Canonical evaluation pipeline

```sql
-- 1. trees (mini fixture) and the matcher's results with default weights
CREATE OR REPLACE TABLE tree_nodes AS SELECT * FROM read_csv_auto('test/data/coverage/mini/tree_nodes.csv');
CREATE OR REPLACE TABLE tree_attributes AS SELECT * FROM read_csv_auto('test/data/coverage/mini/tree_attributes.csv');
CREATE TABLE results AS SELECT * FROM tree_coverage(['Q1', 'Q2', 'Q3'], k := 4);

-- 2. reviewed gold: the coverage reviewers confirmed per (query, candidate)
CREATE TABLE gold AS SELECT * FROM (VALUES
  ('Q1', 'S1', 1.0), ('Q1', 'S6', 0.9), ('Q1', 'S2', 0.5), ('Q1', 'M3', 0.4),
  ('Q2', 'M1', 1.0), ('Q2', 'S1', 0.9), ('Q2', 'S6', 1.0), ('Q2', 'S5', 0.6),
  ('Q3', 'S4', 0.6), ('Q3', 'M3', 1.0), ('Q3', 'S6', 1.0), ('Q3', 'M2', 0.5)) t(query_id, candidate_id, true_coverage);

-- 3. ranking, calibration, operating threshold
SELECT k, top_k_accuracy, mrr, round(mean_regret, 3) AS mean_regret FROM eval_ranking('results', 'gold', k := [1, 3]);
-- k=1: 0.667 | 0.667 | 0.133 (Q3 picks S4, true 0.6 vs best 1.0); k=3: 1.0 | 0.833
SELECT n_pairs, round(mae, 3) AS mae, round(bias, 3) AS bias FROM eval_calibration('results', 'gold');   -- 12 | 0.081 | -0.014
SELECT threshold, precision, accepted_share FROM threshold_at_precision('results', 'gold', target_precision := 0.9);
-- NULL: the highest-scored pick (Q3 -> S4, coverage 1.0) is wrong, so no threshold reaches 0.9;
-- inspect the curve column, fix the matcher or lower the target

-- 4. per-query KPIs with the customer as cluster, then intervals
CREATE TABLE per_request AS
SELECT r.query_id, n.source AS maker,
       (g.true_coverage >= b.best - 1e-9)::DOUBLE AS hit, b.best - g.true_coverage AS regret
FROM results r
JOIN gold g USING (query_id, candidate_id)
JOIN (SELECT query_id, max(true_coverage) AS best FROM gold GROUP BY ALL) b USING (query_id)
JOIN (SELECT DISTINCT tree_id, source FROM tree_nodes) n ON n.tree_id = r.query_id
WHERE r.rank = 1;
SELECT metric, estimate, lo, hi, n_clusters FROM bootstrap_ci('per_request', 'maker', metrics := ['hit', 'regret']);
-- hit 0.667 [0.0, 1.0] over 3 makers: far too few clusters to claim anything

-- 5. fit lambda / mu on the reviewed pairs, then re-score with the winner
CREATE TABLE fit AS SELECT * FROM fit_coverage_weights('gold', label_weights := [0.0, 0.1, 0.3], mus := [0.0, 0.2]);
SET VARIABLE best_lw = (SELECT label_weight FROM fit WHERE rank = 1);
SET VARIABLE best_mu = (SELECT mu FROM fit WHERE rank = 1);
CREATE TABLE results_fit AS SELECT * FROM tree_coverage(['Q1', 'Q2', 'Q3'], k := 4,
       label_weight := getvariable('best_lw'), mu := getvariable('best_mu'));
SELECT (SELECT round(mae, 3) FROM eval_calibration('results', 'gold')) AS mae_default,
       (SELECT round(mae, 3) FROM eval_calibration('results_fit', 'gold')) AS mae_fitted;   -- 0.081 | 0.053
-- (fitted and scored on the same pairs here; use a holdout in practice)
```

In production, split the queries: fit on one part (step 5) and report steps 3–4 on the holdout
only. The full benchmark version (80 requests, 300 programmes) is in `examples/evaluation/`
and `docs/guides/07-evaluation-and-tuning.md`.

## Canonical scale pipeline

1. Run the exact search (coverage skill) or the exact batch join above on a sample; time it.
2. Only if it is too slow / too large: `minhash_index` → `minhash_bands` (store both).
3. `find_covering_entities_lsh(..., bands_table := ...)` for all queries in one call.
4. Measure recall against the exact top k on a sample and `avg(n_candidates)`; tune
   `threshold` / `num_hashes` until recall is acceptable.
5. Rescore the LSH candidates exactly (`find_covering_entities` / `tree_coverage`) if the
   final ranking must be exact, then evaluate with `eval_ranking` as above.
6. Rebuild multiset, index and bands whenever the archive changes.
