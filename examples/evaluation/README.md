# Evaluation Examples

> **Measure the pipeline before trusting it.**

Runnable SQL examples for scoring a matching pipeline against ground truth: ranking quality,
calibration of the predicted coverage, an auto-accept threshold, confidence intervals that
respect clustered data, and a grid search over the kernel weights.

## Functions

| Function | Description |
|----------|-------------|
| `eval_ranking(results, gold [, k, rank_col, score_col])` | Per k: `n_queries, top_k_accuracy, recall_at_k, mrr, mean_regret, share_regret_le_005` |
| `eval_calibration(results, gold [, score_col])` | `n_pairs, mae, bias, pearson, spearman` |
| `threshold_at_precision(results, gold [, target_precision, good])` | Lowest score threshold whose top-1 precision reaches the target |
| `bootstrap_ci(metric_table, cluster_col [, metrics, n, seed, level])` | Cluster bootstrap: `estimate, lo, hi` per metric |
| `fit_coverage_weights(reviewed_matches [, label_weights, mus, ...])` | MAE of the coverage kernel per (λ, μ) grid point |

## Example Files

| File | Description | Data Source |
|------|-------------|-------------|
| [`evaluate_pipeline.sql`](evaluate_pipeline.sql) | Toy ranking, then all KPIs for 80 benchmark requests, bootstrap by maker | Inline + `test/data/coverage/` |
| [`fit_coverage_weights.sql`](fit_coverage_weights.sql) | Fit λ and μ on 20 reviewed requests, check on 60 holdout requests | `test/data/coverage/` |

Both benchmark files first run the coverage pipeline of
[coverage_matching/tree_coverage_benchmark.sql](../coverage_matching/tree_coverage_benchmark.sql).
`fit_coverage_weights.sql` re-runs the kernel nine times and takes a few seconds.

## Quick Start

```bash
# From the repository root
./build/release/duckdb < examples/evaluation/evaluate_pipeline.sql
./build/release/duckdb < examples/evaluation/fit_coverage_weights.sql
```

---

## Usage

```sql
-- results: tree_coverage output; gold: (query_id, candidate_id, true_coverage)
SELECT * FROM eval_ranking('results', 'gold', k := [1, 20]);
SELECT mae, spearman FROM eval_calibration('results', 'gold');
SELECT threshold, precision, accepted_share
FROM threshold_at_precision('results', 'gold', target_precision := 0.95);
```

## Benchmark Results (this build)

| KPI | Value | BRD target |
|-----|-------|------------|
| best programme in top 20 | 0.925 | ≥ 0.90 |
| top-1 is a best programme | 0.925 | ≥ 0.85 |
| mean regret of the top-1 pick | 0.0087 | ≤ 0.01 |
| MAE over all 1 600 ranked pairs | 0.0356 | ≤ 0.05 |
| auto-accept threshold at 95 % precision | 0.769 (88.8 % accepted) | |

## Key Concepts

- **Ties.** Several candidates can reach the best true coverage. `top_k_accuracy` counts a
  query as a hit when any best candidate is in the top k; `recall_at_k` is the share of all
  tied best candidates found.
- **Regret** is the true coverage the top-1 pick gives away against the best candidate.
- **Missing results count as misses.** A query with gold but no results has full regret.
- **Cluster the bootstrap.** Requests from one maker are not independent; resample makers.
  The draws use DuckDB's hash, so `lo` and `hi` can differ slightly between DuckDB versions.
- **Holdout before defaults.** Fit weights on reviewed pairs, then compare MAE on requests that
  were not used for fitting.
