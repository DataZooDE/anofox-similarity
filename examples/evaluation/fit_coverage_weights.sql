-- ============================================================================
-- Tune the kernel weights on reviewed matches
-- ============================================================================
-- Once reviewers have confirmed the true coverage of some (request, programme)
-- pairs, fit_coverage_weights re-runs the coverage kernel over those pairs for
-- every grid point of
--   label_weight (λ): label credit independent of the values (default 0.0)
--   mu (μ)          : weight of the attribute-name similarity (default 0.2)
-- and ranks the grid points by mean absolute error. Check the winner on a
-- holdout before changing the defaults.
--
-- Function:
--   fit_coverage_weights(reviewed_matches [, label_weights, mus, <tree_coverage parameters>])
--     reviewed_matches: (query_id, candidate_id, true_coverage)
--
-- Data: the seating benchmark (test/data/coverage); requests R01..R20 count
--       as reviewed, R21..R80 are the holdout.
--
-- Run: ./build/release/duckdb < examples/evaluation/fit_coverage_weights.sql
-- ============================================================================

.print '>>> 0. Run the pipeline on all 80 benchmark requests'
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

.print ''
.print '>>> 1. The reviewed matches: 20 requests × their top 20 programmes'
CREATE OR REPLACE TABLE reviewed AS
SELECT r.query_id, r.candidate_id, g.true_coverage
FROM results r JOIN gold g USING (query_id, candidate_id)
WHERE r.query_id <= 'R20';
SELECT count(*) AS reviewed_pairs FROM reviewed;
-- Expected: 400

.print ''
.print '>>> 2. Fit a 3 × 3 grid'
-- Pass the same tree_coverage parameters as the production ranking.
CREATE OR REPLACE TABLE fit AS
SELECT * FROM fit_coverage_weights('reviewed',
       nodes_table := 'req_nodes', attrs_table := 'req_attrs',
       candidate_nodes_table := 'prog_nodes', candidate_attrs_table := 'prog_attrs',
       overrides_table := 'vocabulary_overrides', default_bounds := 'catalogue',
       templates_table := 'templates', rules_table := 'compatibility_rules',
       name_vectors := 'name_vectors', name_floor := getvariable('name_floor'),
       label_weights := [0.0, 0.1, 0.3], mus := [0.0, 0.2, 0.4]);

SELECT rank, label_weight, mu, round(mae, 4) AS mae, n_pairs, n_unresolved
FROM fit ORDER BY rank;
-- Expected: 9 grid points, 400 pairs each, none unresolved. The best three:
--   1 | 0.0 | 0.4 | 0.032
--   2 | 0.0 | 0.2 | 0.0361   (the defaults)
--   3 | 0.0 | 0.0 | 0.0434
-- Any label credit (λ > 0) inflates the coverage of programmes whose values miss.

.print ''
.print '>>> 3. Check the winner on the 60 holdout requests'
SET VARIABLE best_lw = (SELECT label_weight FROM fit WHERE rank = 1);
SET VARIABLE best_mu = (SELECT mu FROM fit WHERE rank = 1);
SET VARIABLE holdout = (SELECT list(request_id ORDER BY request_id) FROM requests
                        WHERE request_id > 'R20');

CREATE OR REPLACE TABLE holdout_fitted AS
SELECT * FROM tree_coverage(getvariable('holdout'), k := 20,
       nodes_table := 'req_nodes', attrs_table := 'req_attrs',
       candidate_nodes_table := 'prog_nodes', candidate_attrs_table := 'prog_attrs',
       overrides_table := 'vocabulary_overrides', default_bounds := 'catalogue',
       templates_table := 'templates', rules_table := 'compatibility_rules',
       name_vectors := 'name_vectors', name_floor := getvariable('name_floor'),
       label_weight := getvariable('best_lw'), mu := getvariable('best_mu'));
CREATE OR REPLACE TABLE holdout_default AS SELECT * FROM results WHERE query_id > 'R20';

SELECT 'defaults (λ 0.0, μ 0.2)' AS weights, round(mae, 4) AS holdout_mae
FROM eval_calibration('holdout_default', 'gold')
UNION ALL
SELECT 'fitted (λ ' || getvariable('best_lw') || ', μ ' || getvariable('best_mu') || ')',
       round(mae, 4)
FROM eval_calibration('holdout_fitted', 'gold');
-- Expected:
--   defaults (λ 0.0, μ 0.2) | 0.0354
--   fitted (λ 0.0, μ 0.4)   | 0.0318
-- The fitted weights also help on requests they were not fitted on.
