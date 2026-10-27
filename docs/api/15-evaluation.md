# Evaluation and tuning

> Ranking quality, calibration, auto-accept thresholds, intervals and weight fitting.

[API index](../API_REFERENCE.md) · [Guides](../guides/README.md) · [Examples](../../examples/README.md)

**Evaluation.** `results` holds `query_id, candidate_id` and a `rank` (or a score
column), as `tree_coverage` returns them; `gold` holds `query_id, candidate_id, true_coverage`.

| Function | Returns |
|---|---|
| `eval_ranking(results, gold [, k := [1,5,10,20], rank_col, score_col])` | per k: `n_queries, top_k_accuracy, recall_at_k, mrr, mean_regret, share_regret_le_005` |
| `eval_calibration(results, gold [, score_col := 'coverage'])` | `n_pairs, mae, bias, pearson, spearman` |
| `threshold_at_precision(results, gold [, target_precision := 0.95, good := 0.9])` | `threshold, precision, accepted_share, n_queries, curve` |
| `bootstrap_ci(metric_table, cluster_col [, metrics, n := 1000, seed := 7, level := 0.95])` | per metric: `estimate, lo, hi, n_boot, n_clusters` |
| `fit_coverage_weights(reviewed_matches [, label_weights, mus, <tree_coverage parameters>])` | per grid point: `label_weight, mu, mae, n_pairs, n_unresolved, rank` (at most 20 values per axis) |

- `eval_ranking` scores the queries that have gold: a query without results counts as a miss
  with full regret, and results for queries without gold are ignored. Duplicate rows of a pair
  keep the best rank. Positions follow the given rank (ties by candidate id) or the score
  (highest first). Ranks start at 1.
- The best candidates of a query are all gold candidates within 1e-9 of its gold best. A ranked
  candidate without gold counts as 0. `top_k_accuracy` is the share of queries with a best
  candidate in the top k (the BRD's "recall@k of the best candidate"). `recall_at_k` is the
  share of each query's best candidates in the top k; the two differ only with ties.
  `mean_regret` and `share_regret_le_005` measure the top-1 pick, so they repeat on every k row.
- `threshold_at_precision` looks at the top-1 picks. It returns the lowest score threshold whose
  precision (share of picks with true coverage ≥ `good`) reaches the target, which gives the
  largest auto-accept share. `target` is a reserved word in DuckDB, hence `target_precision`.
- `bootstrap_ci` resamples clusters (for example the maker) with replacement and reports the
  pooled mean with a percentile interval. The draws use DuckDB's hash, so the exact
  bounds can differ between DuckDB versions.
- `fit_coverage_weights` runs the kernel over the reviewed pairs for each (λ, μ) grid point. Tune the classifier's β and τ with `classify_to_catalogue` and `eval_ranking`.

```sql
-- the benchmark files (test/data/coverage): requests, request_nodes with their classes,
-- request_attributes parsed with parse_quantity, programme_nodes/_attributes, vocabulary_overrides,
-- gold_coverage, gold_requests; see examples/evaluation for the full setup
CREATE TABLE results AS SELECT * FROM tree_coverage(getvariable('request_ids'), k := 20,
       nodes_table := 'req_nodes', attrs_table := 'req_attrs',
       candidate_nodes_table := 'prog_nodes', candidate_attrs_table := 'prog_attrs',
       overrides_table := 'vocabulary_overrides', default_bounds := 'catalogue', templates_table := 'templates',
       rules_table := 'compatibility_rules', name_vectors := 'name_vectors', name_floor := getvariable('name_floor'));
SELECT * FROM eval_ranking('results', 'gold', k := [1, 20]);
SELECT mae, spearman FROM eval_calibration('results', 'gold');
SELECT threshold, precision, accepted_share FROM threshold_at_precision('results', 'gold', target_precision := 0.95);

CREATE TABLE per_request AS
SELECT r.query_id, q.source AS maker, (g.true_coverage >= b.best_coverage - 1e-9)::DOUBLE AS hit
FROM results r JOIN gold g ON g.query_id = r.query_id AND g.candidate_id = r.candidate_id
JOIN gold_requests b ON b.request_id = r.query_id JOIN requests q ON q.request_id = r.query_id
WHERE r.rank = 1;
SELECT * FROM bootstrap_ci('per_request', 'maker', metrics := ['hit']);
```

On the benchmark, `eval_ranking` and `eval_calibration` give the same numbers as the BRD
KPI formulas (examples/evaluation).

<!-- generated:functions:start -->

## Function reference

*Generated from the extension's function metadata by `scripts/generate_api_reference.py`.*

### `eval_ranking`

*Table function*

Ranking quality against gold coverage. results: query_id, candidate_id and a rank (rank_col, default 'rank') or a score (score_col, default 'coverage', highest first); gold: query_id, candidate_id, true_coverage. The best candidates of a query are all gold candidates within 1e-9 of its gold best; a ranked candidate without gold counts as 0. Per k (default 1, 5, 10, 20): n_queries, top_k_accuracy (share of queries with a best candidate in the top k, BRD 'recall@k of the best candidate'), recall_at_k (share of each query's best candidates in the top k), mrr (cut at k), mean_regret and share_regret_le_005 of the top pick.

```sql
eval_ranking(results, gold, k, rank_col, score_col)
```

**Examples**

```sql
SELECT * FROM eval_ranking('results', 'gold', k := [1, 20]);
```

### `eval_calibration`

*Table function*

Calibration of the predicted coverage (score_col, default 'coverage') against gold true_coverage over the pairs in both tables: n_pairs, mae, bias (predicted - true), pearson, spearman (average ranks for ties); correlations are NULL for a constant input.

```sql
eval_calibration(results, gold, score_col)
```

**Examples**

```sql
SELECT * FROM eval_calibration('results', 'gold');
```

### `threshold_at_precision`

*Table function*

The auto-accept threshold: over the top-1 picks, the lowest score threshold whose precision (share of picks with true coverage >= good, default 0.9) reaches target_precision (default 0.95; 'target' is a reserved word). Returns threshold (NULL when unreachable), precision, accepted_share, n_queries and the precision curve (threshold, precision, share).

```sql
threshold_at_precision(results, gold, target_precision, good, rank_col, score_col)
```

**Examples**

```sql
SELECT threshold, precision, accepted_share FROM threshold_at_precision('results', 'gold', target_precision := 0.95);
```

### `bootstrap_ci`

*Table function*

Cluster bootstrap intervals: resamples the clusters (cluster_col) with replacement n times (default 1000, seeded) and reports per metric the pooled mean (estimate) and the percentile interval lo/hi at level (default 0.95), with n_boot and n_clusters. metrics defaults to every other numeric column.

```sql
bootstrap_ci(metric_table, cluster_col, metrics, n, seed, level)
```

**Examples**

```sql
SELECT * FROM bootstrap_ci('per_request', 'maker', metrics := ['hit', 'regret']);
```

### `fit_coverage_weights`

*Table function*

Fits the kernel weights on reviewed matches: for every grid point of label_weights (lambda, default 0, 0.1, 0.2, 0.3, 0.5) x mus (default 0, 0.1, 0.2, 0.3), the mean absolute error of the kernel's coverage against reviewed_matches (query_id, candidate_id, true_coverage), with n_pairs, n_unresolved (reviewed pairs whose trees are missing) and rank (best first). At most 20 values per axis. The trees and options are given as for tree_coverage. The classifier's beta and tau are tuned with classify_to_catalogue and eval_ranking instead.

```sql
fit_coverage_weights(reviewed_matches, label_weights, mus, nodes_table, attrs_table, candidate_nodes_table, candidate_attrs_table, overrides_table, label_table, labels, assignment, partition_gate, count_gate, default_bounds, templates_table, rules_table, name_vectors, name_floor)
```

**Examples**

```sql
SELECT * FROM fit_coverage_weights('reviewed_matches', candidate_nodes_table := 'programme_nodes', candidate_attrs_table := 'programme_attributes') LIMIT 3;
```

<!-- generated:functions:end -->
