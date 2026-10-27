# Evaluation and Tuning

> Measure a matching pipeline against reviewed results before you trust it, and tune it on data.

**Use this guide to:**
- Score a ranking with `eval_ranking` (top-k accuracy, recall, MRR, regret)
- Check whether the predicted coverage is calibrated with `eval_calibration`
- Find the auto-accept threshold for a target precision with `threshold_at_precision`
- Report KPIs with confidence intervals by cluster with `bootstrap_ci`
- Fit the kernel weights on reviewed pairs with `fit_coverage_weights` and check them on a holdout

All functions take two tables. `results` holds `query_id, candidate_id` and a `rank` or a score
column, as `tree_coverage` returns them. `gold` holds `query_id, candidate_id, true_coverage`: the
coverage a reviewer confirmed.

## A First Example

```sql
CREATE TABLE my_results AS SELECT * FROM (VALUES
  ('Q1', 'S2', 1, 0.97), ('Q1', 'S1', 2, 0.95), ('Q1', 'S6', 3, 0.90),
  ('Q2', 'S5', 1, 0.60), ('Q2', 'S1', 2, 0.55)) t(query_id, candidate_id, rank, coverage);
CREATE TABLE my_gold AS SELECT * FROM (VALUES
  ('Q1', 'S2', 1.0), ('Q1', 'S1', 1.0), ('Q1', 'S6', 0.8),
  ('Q2', 'S5', 0.5), ('Q2', 'S1', 0.9)) t(query_id, candidate_id, true_coverage);

SELECT k, n_queries, top_k_accuracy, recall_at_k, mrr, round(mean_regret, 3) AS mean_regret
FROM eval_ranking('my_results', 'my_gold', k := [1, 2]);
```

| k | n_queries | top_k_accuracy | recall_at_k | mrr  | mean_regret |
|--:|----------:|---------------:|------------:|-----:|------------:|
| 1 | 2         | 0.5            | 0.25        | 0.5  | 0.2         |
| 2 | 2         | 1.0            | 1.0         | 0.75 | 0.2         |

How to read it:

| Metric | Meaning |
|---|---|
| `top_k_accuracy` | Share of queries with a best candidate in the top k. Q1's pick S2 is a best one; Q2's best, S1, is at rank 2. |
| `recall_at_k` | Share of each query's best candidates in the top k. Q1 has two best candidates (S1, S2), only one is at rank 1. |
| `mrr` | Mean reciprocal rank of the first best candidate, cut at k. |
| `mean_regret` | Mean of best coverage − true coverage of the pick. Q2 picked 0.5 instead of 0.9. |
| `share_regret_le_005` | Share of queries whose pick is within 0.05 of the best. |

A query without results counts as a miss with full regret. Results for queries without gold are
ignored.

```sql
SELECT n_pairs, round(mae, 3) AS mae, round(bias, 3) AS bias, round(spearman, 3) AS spearman
FROM eval_calibration('my_results', 'my_gold');
```

| n_pairs | mae   | bias   | spearman |
|--------:|------:|-------:|---------:|
| 5       | 0.126 | -0.046 | 0.667    |

`bias` is predicted − true: negative means the scores are too pessimistic. `score_col` (default
`coverage`) names the predicted column.

## The Benchmark

The seating benchmark in `test/data/coverage` has 80 requests, 300 past programmes and the true
coverage of every pair. Here the request classes come from the benchmark labels and the archive
uses the finished vocabulary review. They stand in for a reviewed setup.

```sql
CREATE TABLE requests AS SELECT * FROM read_csv_auto('test/data/coverage/requests.csv.gz');
CREATE TABLE req_nodes AS
SELECT n.* EXCLUDE (class_id), g.class_id
FROM read_csv_auto('test/data/coverage/request_nodes.csv.gz') n
LEFT JOIN read_csv_auto('test/data/coverage/gold_item_class.csv.gz') g USING (tree_id, node_id);
CREATE TABLE req_attrs AS
SELECT tree_id, node_id, attr_id, name, q.dimension, q.unit, q.lo, q.hi, q.bound, q.category
FROM (SELECT a.*, parse_quantity(a.value_text, r.lang) AS q
      FROM read_csv_auto('test/data/coverage/request_attributes.csv.gz') a
      JOIN requests r ON r.request_id = a.tree_id);
CREATE TABLE prog_nodes AS SELECT * FROM read_csv_auto('test/data/coverage/programme_nodes.csv.gz');
CREATE TABLE prog_attrs AS SELECT * FROM read_csv_auto('test/data/coverage/programme_attributes.csv.gz');
CREATE TABLE vocabulary_overrides AS
SELECT wording, source, class_id FROM read_csv_auto('test/data/coverage/gold_vocabulary.csv.gz');
CREATE TABLE catalogue_attributes AS SELECT * FROM read_csv_auto('test/data/coverage/catalogue_attributes.csv.gz');
CREATE TABLE compatibility_rules AS SELECT * FROM read_csv_auto('test/data/coverage/compatibility_rules.csv.gz');
CREATE TABLE name_vectors AS
SELECT name, vec::FLOAT[] AS vec FROM read_csv_auto('test/data/coverage/attribute_name_vectors.csv.gz');
SET VARIABLE name_floor = (SELECT floor FROM cosine_floor('name_vectors', 'vec', sample := 20000));
SET VARIABLE request_ids = (SELECT list(request_id ORDER BY request_id) FROM requests);
CREATE TABLE gold AS SELECT * FROM read_csv_auto('test/data/coverage/gold_coverage.csv.gz');
CREATE TABLE gold_requests AS SELECT * FROM read_csv_auto('test/data/coverage/gold_requests.csv.gz');

CREATE TABLE results AS
SELECT * FROM tree_coverage(getvariable('request_ids'), k := 20,
       nodes_table := 'req_nodes', attrs_table := 'req_attrs',
       candidate_nodes_table := 'prog_nodes', candidate_attrs_table := 'prog_attrs',
       overrides_table := 'vocabulary_overrides', default_bounds := 'catalogue',
       rules_table := 'compatibility_rules', name_vectors := 'name_vectors', name_floor := getvariable('name_floor'));

SELECT k, round(top_k_accuracy, 3) AS top_k_accuracy, round(mrr, 3) AS mrr, round(mean_regret, 4) AS mean_regret
FROM eval_ranking('results', 'gold', k := [1, 20]);
```

| k  | top_k_accuracy | mrr   | mean_regret |
|---:|---------------:|------:|------------:|
| 1  | 0.925          | 0.925 | 0.0087      |
| 20 | 0.925          | 0.925 | 0.0087      |

```sql
SELECT n_pairs, round(mae, 4) AS mae, round(bias, 4) AS bias, round(spearman, 3) AS spearman
FROM eval_calibration('results', 'gold');
```

| n_pairs | mae    | bias   | spearman |
|--------:|-------:|-------:|---------:|
| 1600    | 0.0358 | 0.0312 | 0.972    |

## Auto-Accept Threshold

Which top-1 picks can you accept without review? `threshold_at_precision` returns the lowest
coverage threshold at which the picks reach the target precision. A pick counts as good when its
true coverage is at least `good` (default 0.9).

```sql
SELECT round(threshold, 4) AS threshold, round(precision, 3) AS precision, round(accepted_share, 3) AS accepted_share
FROM threshold_at_precision('results', 'gold', target_precision := 0.95);
```

| threshold | precision | accepted_share |
|----------:|----------:|---------------:|
| 0.769     | 0.958     | 0.888          |

Accept picks with coverage ≥ 0.769 automatically: 88.8 % of the requests, 95.8 % of them good.
The `curve` column holds every (threshold, precision, share) point. The parameter is called
`target_precision` because `target` is a reserved word in DuckDB.

## Confidence Intervals by Cluster

Requests from one customer share wording and habits. They are not independent. `bootstrap_ci`
resamples whole clusters:

```sql
CREATE TABLE per_request AS
SELECT r.query_id, q.source AS maker, (g.true_coverage >= b.best_coverage - 1e-9)::DOUBLE AS hit,
       b.best_coverage - g.true_coverage AS regret
FROM results r JOIN gold g USING (query_id, candidate_id)
JOIN gold_requests b ON b.request_id = r.query_id JOIN requests q ON q.request_id = r.query_id
WHERE r.rank = 1;

SELECT metric, round(estimate, 4) AS estimate, round(lo, 4) AS lo, round(hi, 4) AS hi, n_clusters
FROM bootstrap_ci('per_request', 'maker', metrics := ['hit', 'regret']) ORDER BY metric;
```

| metric | estimate | lo     | hi     | n_clusters |
|--------|---------:|-------:|-------:|-----------:|
| hit    | 0.925    | 0.9059 | 0.9467 | 8          |
| regret | 0.0087   | 0.0042 | 0.0127 | 8          |

Defaults: 1 000 draws, seed 7, level 0.95. The same seed gives the same interval. The draws use
DuckDB's hash function, so the bounds can differ slightly between DuckDB versions.

## Fit the Kernel Weights

`fit_coverage_weights` runs the kernel over reviewed pairs for a grid of `label_weight` (λ) and
`mu` (the attribute-name discount) and ranks the grid points by MAE. Fit on part of the data and
check on the rest:

```sql
CREATE TABLE reviewed AS
SELECT r.query_id, r.candidate_id, g.true_coverage
FROM results r JOIN gold g USING (query_id, candidate_id) WHERE r.query_id <= 'R20';

CREATE TABLE fit AS
SELECT * FROM fit_coverage_weights('reviewed', nodes_table := 'req_nodes', attrs_table := 'req_attrs',
       candidate_nodes_table := 'prog_nodes', candidate_attrs_table := 'prog_attrs',
       overrides_table := 'vocabulary_overrides', default_bounds := 'catalogue',
       rules_table := 'compatibility_rules', name_vectors := 'name_vectors', name_floor := getvariable('name_floor'),
       label_weights := [0.0, 0.1, 0.3], mus := [0.0, 0.2, 0.4]);

SELECT label_weight, mu, round(mae, 4) AS mae, n_pairs, rank FROM fit ORDER BY rank LIMIT 3;
```

| label_weight | mu  | mae    | n_pairs | rank |
|-------------:|----:|-------:|--------:|-----:|
| 0.0          | 0.4 | 0.0321 | 400     | 1    |
| 0.0          | 0.2 | 0.0363 | 400     | 2    |
| 0.0          | 0.0 | 0.0435 | 400     | 3    |

Check the best point on the 60 requests that were not used for the fit:

```sql
SET VARIABLE best_lw = (SELECT label_weight FROM fit WHERE rank = 1);
SET VARIABLE best_mu = (SELECT mu FROM fit WHERE rank = 1);
SET VARIABLE holdout = (SELECT list(request_id ORDER BY request_id) FROM requests WHERE request_id > 'R20');

CREATE TABLE holdout_fit AS
SELECT * FROM tree_coverage(getvariable('holdout'), k := 20,
       nodes_table := 'req_nodes', attrs_table := 'req_attrs',
       candidate_nodes_table := 'prog_nodes', candidate_attrs_table := 'prog_attrs',
       overrides_table := 'vocabulary_overrides', default_bounds := 'catalogue',
       rules_table := 'compatibility_rules', name_vectors := 'name_vectors', name_floor := getvariable('name_floor'),
       label_weight := getvariable('best_lw'), mu := getvariable('best_mu'));
CREATE TABLE holdout_default AS SELECT * FROM results WHERE query_id > 'R20';

SELECT (SELECT round(mae, 4) FROM eval_calibration('holdout_default', 'gold')) AS mae_default,
       (SELECT round(mae, 4) FROM eval_calibration('holdout_fit', 'gold')) AS mae_fitted;
```

| mae_default | mae_fitted |
|------------:|-----------:|
| 0.0357      | 0.0321     |

The fitted weights hold on the holdout. Only then change the defaults in your pipeline. The grid
takes at most 20 values per axis. Tune the classifier's `attr_bonus` and `temperature` separately
with `classify_to_catalogue` and `eval_ranking`.

## Pitfalls

| Pitfall | Solution |
|---|---|
| Tuning and reporting on the same requests | Keep a holdout. Report the holdout numbers. |
| Narrow intervals from correlated requests | Bootstrap by cluster (customer, maker, plant). |
| Ties in gold | Several candidates can be best. `top_k_accuracy` counts any of them. |
| `target :=` raises a parser error | The parameter is `target_precision`. |
| A subquery as a table function argument | Not allowed. Store the value with `SET VARIABLE` and pass `getvariable('name')`. |
| Gold for only some pairs | A ranked candidate without gold counts as true coverage 0. |

## Next Steps

- [Classification and review](06-classification-and-review.md): improve the labels
- [Scale and dedup](08-scale-and-dedup.md): large archives
- API: [Evaluation](../api/15-evaluation.md)
- [Examples](../../examples/evaluation)
