-- ============================================================================
-- Evaluate a matching pipeline against ground truth
-- ============================================================================
-- Before going live, measure the ranking: is a best candidate in the top k,
-- how much coverage does the top-1 pick give away (regret), how well does
-- the predicted coverage match the true one, and above which score can a
-- top-1 pick be accepted without review?
--
-- Inputs: results (query_id, candidate_id, rank and/or a score column, as
-- tree_coverage returns them) and gold (query_id, candidate_id, true_coverage).
--
-- Functions:
--   eval_ranking(results, gold [, k := [1,5,10,20], rank_col, score_col])
--   eval_calibration(results, gold [, score_col := 'coverage'])
--   threshold_at_precision(results, gold [, target_precision := 0.95, good := 0.9])
--   bootstrap_ci(metric_table, cluster_col [, metrics, n := 1000, seed := 7, level := 0.95])
--
-- Data: inline toy ranking, then the seating benchmark (test/data/coverage)
--
-- Run: ./build/release/duckdb < examples/evaluation/evaluate_pipeline.sql
-- ============================================================================

.print '>>> 1. A toy ranking: two queries, three candidates each'
CREATE OR REPLACE TABLE toy_results AS SELECT * FROM (VALUES
    ('q1', 'a', 1, 0.95), ('q1', 'b', 2, 0.90), ('q1', 'c', 3, 0.40),
    ('q2', 'd', 1, 0.80), ('q2', 'e', 2, 0.75), ('q2', 'f', 3, 0.30)
) t(query_id, candidate_id, rank, coverage);
CREATE OR REPLACE TABLE toy_gold AS SELECT * FROM (VALUES
    ('q1', 'a', 1.00), ('q1', 'b', 0.90), ('q1', 'c', 0.40),
    ('q2', 'd', 0.70), ('q2', 'e', 0.90), ('q2', 'f', 0.30)
) t(query_id, candidate_id, true_coverage);

-- q1: the pick is the best candidate. q2: the best (e, 0.9) is ranked 2nd,
-- so the pick gives away 0.9 - 0.7 = 0.2 coverage.
SELECT k, n_queries, top_k_accuracy, mrr, round(mean_regret, 3) AS mean_regret
FROM eval_ranking('toy_results', 'toy_gold', k := [1, 2]);
-- Expected:
--   1 | 2 | 0.5 | 0.5  | 0.1
--   2 | 2 | 1.0 | 0.75 | 0.1   (regret is always that of the top-1 pick)

SELECT n_pairs, round(mae, 4) AS mae, round(bias, 4) AS bias
FROM eval_calibration('toy_results', 'toy_gold');
-- Expected: 6 | 0.05 | -0.0167   (mae = mean |coverage - true_coverage|;
--           bias = mean signed error: here the toy pipeline is slightly pessimistic)

.print ''
.print '>>> 1b. Run the pipeline on all 80 benchmark requests (as in coverage_matching)'
CREATE OR REPLACE TABLE requests AS SELECT * FROM read_csv('test/data/coverage/requests.csv.gz');
-- Reviewed request classes (gold labels stand in for a finished review).
CREATE OR REPLACE TABLE req_nodes AS
SELECT n.* EXCLUDE (class_id), g.class_id
FROM read_csv('test/data/coverage/request_nodes.csv.gz') n
LEFT JOIN read_csv('test/data/coverage/gold_item_class.csv.gz') g USING (tree_id, node_id);
CREATE OR REPLACE TABLE req_attrs AS
SELECT tree_id, node_id, attr_id, name, q.dimension, q.unit, q.lo, q.hi, q.bound, q.category
FROM (SELECT a.*, parse_quantity(a.value_text, r.lang) AS q
      FROM read_csv('test/data/coverage/request_attributes.csv.gz') a
      JOIN requests r ON r.request_id = a.tree_id);
CREATE OR REPLACE TABLE prog_nodes AS SELECT * FROM read_csv('test/data/coverage/programme_nodes.csv.gz');
CREATE OR REPLACE TABLE prog_attrs AS SELECT * FROM read_csv('test/data/coverage/programme_attributes.csv.gz');
CREATE OR REPLACE TABLE vocabulary_overrides AS
SELECT wording, source, class_id FROM read_csv('test/data/coverage/gold_vocabulary.csv.gz');
CREATE OR REPLACE TABLE templates AS SELECT * FROM read_csv('test/data/coverage/catalogue_attributes.csv.gz');
CREATE OR REPLACE TABLE compatibility_rules AS
SELECT * FROM read_csv('test/data/coverage/compatibility_rules.csv.gz');
CREATE OR REPLACE TABLE name_vectors AS
SELECT name, vec::FLOAT[] AS vec FROM read_csv('test/data/coverage/attribute_name_vectors.csv.gz');
SET VARIABLE name_floor = (SELECT median(list_cosine_similarity(a.vec, b.vec))
                           FROM name_vectors a, name_vectors b WHERE a.name < b.name);
SET VARIABLE request_ids = (SELECT list(request_id ORDER BY request_id) FROM requests);

CREATE OR REPLACE TABLE results AS
SELECT * FROM tree_coverage(getvariable('request_ids'), k := 20,
       nodes_table := 'req_nodes', attrs_table := 'req_attrs',
       candidate_nodes_table := 'prog_nodes', candidate_attrs_table := 'prog_attrs',
       overrides_table := 'vocabulary_overrides', default_bounds := 'catalogue',
       templates_table := 'templates', rules_table := 'compatibility_rules',
       name_vectors := 'name_vectors', name_floor := getvariable('name_floor'));

-- Ground truth: true coverage of all 80 × 300 pairs, and the best per request.
CREATE OR REPLACE TABLE gold AS SELECT * FROM read_csv('test/data/coverage/gold_coverage.csv.gz');
CREATE OR REPLACE TABLE gold_requests AS
SELECT * FROM read_csv('test/data/coverage/gold_requests.csv.gz');

SELECT count(DISTINCT query_id) AS requests, count(*) AS ranked_pairs FROM results;
-- Expected: 80 | 1600  (top 20 per request)

.print ''
.print '>>> 2. Ranking quality'
SELECT k, n_queries, round(top_k_accuracy, 3) AS top_k_accuracy, round(recall_at_k, 3) AS recall_at_k,
       round(mrr, 3) AS mrr, round(mean_regret, 4) AS mean_regret
FROM eval_ranking('results', 'gold', k := [1, 5, 20])
ORDER BY k;
-- Expected:
--    1 | 80 | 0.925 | 0.892 | 0.925 | 0.0087
--    5 | 80 | 0.925 | 0.908 | 0.925 | 0.0087
--   20 | 80 | 0.925 | 0.908 | 0.925 | 0.0087
-- top_k_accuracy: share of requests with a best programme in the top k.
-- recall_at_k: share of ALL tied best programmes in the top k (differs with ties).

.print ''
.print '>>> 3. Calibration of the predicted coverage'
SELECT n_pairs, round(mae, 4) AS mae, round(bias, 4) AS bias,
       round(pearson, 3) AS pearson, round(spearman, 3) AS spearman
FROM eval_calibration('results', 'gold');
-- Expected: 1600 | 0.0356 | 0.0309 | 0.975 | 0.972   (BRD target: MAE <= 0.05)

.print ''
.print '>>> 4. Auto-accept threshold for 95 % precision'
-- A top-1 pick is "good" when its true coverage is >= 0.9.
SELECT round(threshold, 4) AS threshold, round(precision, 3) AS precision,
       round(accepted_share, 3) AS accepted_share, n_queries
FROM threshold_at_precision('results', 'gold', target_precision := 0.95);
-- Expected: 0.769 | 0.958 | 0.888 | 80
-- Accept top-1 picks with coverage >= 0.769 automatically: 88.8 % of the
-- requests need no review, and 95.8 % of the accepted picks are good.

.print ''
.print '>>> 5. Confidence intervals, clustered by vehicle maker'
-- Requests from one maker share habits, so resample makers, not requests.
CREATE OR REPLACE TABLE per_request AS
SELECT r.query_id, q.source AS maker,
       (g.true_coverage >= b.best_coverage - 1e-9)::DOUBLE AS hit,
       b.best_coverage - g.true_coverage AS regret
FROM results r
JOIN gold g ON g.query_id = r.query_id AND g.candidate_id = r.candidate_id
JOIN gold_requests b ON b.request_id = r.query_id
JOIN requests q ON q.request_id = r.query_id
WHERE r.rank = 1;

SELECT metric, round(estimate, 4) AS estimate, round(lo, 4) AS lo, round(hi, 4) AS hi,
       n_boot, n_clusters
FROM bootstrap_ci('per_request', 'maker', metrics := ['hit', 'regret'], n := 1000, seed := 7)
ORDER BY metric;
-- Expected (this build):
--   hit    | 0.925  | 0.9059 | 0.9467 | 1000 | 8
--   regret | 0.0087 | 0.0042 | 0.0127 | 1000 | 8
-- The draws use DuckDB's hash: lo and hi can differ slightly between DuckDB versions.
