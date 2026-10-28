-- ============================================================================
-- Stage 2 on the seating benchmark: rank 300 programmes, list the gaps
-- ============================================================================
-- The full pipeline on realistic data: 80 customer specifications (German or
-- English, customer units) against an archive of 300 past seat programmes.
--
--   1. Request items get their classes from a review (here: the benchmark's
--      gold labels stand in for a finished review), archive items from the
--      reviewed internal vocabulary (wording -> class).
--   2. Request values are parsed from text with parse_quantity, in the
--      request's language ('2.500' is 2500 in German).
--   3. tree_coverage: stage 1 keeps the top k programmes by class multiset,
--      stage 2 checks every attribute value and count.
--   4. tree_coverage_trace: the gap list of the best programme.
--
-- Data: test/data/coverage/ (see test/data/coverage/README.md for the schemas)
--
-- Run: ./build/release/duckdb < examples/coverage_matching/tree_coverage_benchmark.sql
-- ============================================================================

.print '>>> 0. Load requests, archive, vocabulary, templates, rules, name vectors'
CREATE OR REPLACE TABLE requests AS
SELECT * FROM read_csv('test/data/coverage/requests.csv.gz');

-- Request trees. class_id is empty on purpose; take the reviewed classes.
CREATE OR REPLACE TABLE req_nodes AS
SELECT n.* EXCLUDE (class_id), g.class_id
FROM read_csv('test/data/coverage/request_nodes.csv.gz') n
LEFT JOIN read_csv('test/data/coverage/gold_item_class.csv.gz') g USING (tree_id, node_id);

-- Request attributes: value_text such as 'max. 33 dB(A)' or 'mindestens 24 cm'.
CREATE OR REPLACE TABLE req_attrs AS
SELECT tree_id, node_id, attr_id, name, q.dimension, q.unit, q.lo, q.hi, q.bound, q.category
FROM (SELECT a.*, parse_quantity(a.value_text, r.lang) AS q
      FROM read_csv('test/data/coverage/request_attributes.csv.gz') a
      JOIN requests r ON r.request_id = a.tree_id);

-- The programme archive (values already in base units).
CREATE OR REPLACE TABLE prog_nodes AS
SELECT * FROM read_csv('test/data/coverage/programme_nodes.csv.gz');
CREATE OR REPLACE TABLE prog_attrs AS
SELECT * FROM read_csv('test/data/coverage/programme_attributes.csv.gz');

-- The reviewed internal vocabulary: archive wording -> class.
CREATE OR REPLACE TABLE vocabulary_overrides AS
SELECT wording, source, class_id FROM read_csv('test/data/coverage/gold_vocabulary.csv.gz');

-- Attribute templates give unqualified values ('38 dB(A)') their direction.
CREATE OR REPLACE TABLE templates AS
SELECT * FROM read_csv('test/data/coverage/catalogue_attributes.csv.gz');
CREATE OR REPLACE TABLE compatibility_rules AS
SELECT * FROM read_csv('test/data/coverage/compatibility_rules.csv.gz');

-- Attribute-name vectors; the floor is the median cosine of unrelated names.
CREATE OR REPLACE TABLE name_vectors AS
SELECT name, vec::FLOAT[] AS vec
FROM read_csv('test/data/coverage/attribute_name_vectors.csv.gz');
SET VARIABLE name_floor = (SELECT median(list_cosine_similarity(a.vec, b.vec))
                           FROM name_vectors a, name_vectors b WHERE a.name < b.name);

SELECT (SELECT count(*) FROM requests) AS requests,
       (SELECT count(DISTINCT tree_id) FROM prog_nodes) AS programmes,
       (SELECT count(*) FROM req_attrs WHERE dimension IS NOT NULL OR category IS NOT NULL) AS parsed_values,
       round(getvariable('name_floor'), 3) AS name_floor;
-- Expected: 80 | 300 | 2736 | 0.853  (every request value parses)

.print ''
.print '>>> 1. Top 5 programmes for request R03'
SELECT candidate_id, round(coverage, 3) AS coverage, round(stage1_score, 3) AS stage1,
       rank, n_requirements, n_gaps
FROM tree_coverage('R03', k := 20,
       nodes_table := 'req_nodes', attrs_table := 'req_attrs',
       candidate_nodes_table := 'prog_nodes', candidate_attrs_table := 'prog_attrs',
       overrides_table := 'vocabulary_overrides', default_bounds := 'catalogue',
       templates_table := 'templates', rules_table := 'compatibility_rules',
       name_vectors := 'name_vectors', name_floor := getvariable('name_floor'))
WHERE rank <= 5
ORDER BY rank;
-- Expected:
--   P014 | 0.977 | 1.009 | 1 | 10 | 0
--   P097 | 0.823 | 0.93  | 2 | 10 | 4
--   P272 | 0.712 | 0.854 | 3 | 10 | 5
--   P023 | 0.69  | 0.929 | 4 | 10 | 5
--   P086 | 0.671 | 1.009 | 5 | 10 | 6   (complete class multiset, values miss)

.print ''
.print '>>> 2. An unmeetable request: rank, then list the gaps of the best programme'
-- R19 is one of the 10 requests that no programme can fully meet.
SET VARIABLE rid = 'R19';
SET VARIABLE top = (
    SELECT candidate_id FROM tree_coverage(getvariable('rid'), k := 20,
           nodes_table := 'req_nodes', attrs_table := 'req_attrs',
           candidate_nodes_table := 'prog_nodes', candidate_attrs_table := 'prog_attrs',
           overrides_table := 'vocabulary_overrides', default_bounds := 'catalogue',
           templates_table := 'templates', rules_table := 'compatibility_rules',
           name_vectors := 'name_vectors', name_floor := getvariable('name_floor'))
    WHERE rank = 1);
SELECT getvariable('rid') AS request, getvariable('top') AS best_programme;
-- Expected: R19 | P065

-- The trace takes the same parameters, so it explains the same score.
-- is_gap flags the requirement; value_score < 1 marks the attribute that falls
-- short. distance is relative for the limit rules: 2.5 = 250 % over the limit.
SELECT req_node_id, attr_name, req_value, prov_value, rule,
       round(value_score, 2) AS value_score, round(distance, 3) AS distance
FROM tree_coverage_trace(getvariable('rid'), candidates := [getvariable('top')],
       nodes_table := 'req_nodes', attrs_table := 'req_attrs',
       candidate_nodes_table := 'prog_nodes', candidate_attrs_table := 'prog_attrs',
       overrides_table := 'vocabulary_overrides', default_bounds := 'catalogue',
       templates_table := 'templates', rules_table := 'compatibility_rules',
       name_vectors := 'name_vectors', name_floor := getvariable('name_floor'))
WHERE is_gap AND (value_score < 1 OR matched_node_id IS NULL)
ORDER BY req_node_id, attr_name;
-- Expected: 10 failing attributes, e.g.
--   R19.r01 | Gewicht        | at_most 0.4 kg | 1.4 kg | at_most | 0.0 | 2.5
--   R19.r23 | Stromverbrauch | at_most 1.5 A  | 10 A   | at_most | 0.0 | 5.667
--   R19.r24 | Verstellgeschwindigkeit | within 0.017..0.05 m/s | 0.013..0.032 m/s | within | 0.63 | 0.371

.print ''
.print '>>> 3. All 80 requests at once, checked against the ground truth'
-- tree_coverage takes a LIST of query trees; this runs in about a second.
SET VARIABLE request_ids = (SELECT list(request_id ORDER BY request_id) FROM requests);
CREATE OR REPLACE TABLE results AS
SELECT * FROM tree_coverage(getvariable('request_ids'), k := 20,
       nodes_table := 'req_nodes', attrs_table := 'req_attrs',
       candidate_nodes_table := 'prog_nodes', candidate_attrs_table := 'prog_attrs',
       overrides_table := 'vocabulary_overrides', default_bounds := 'catalogue',
       templates_table := 'templates', rules_table := 'compatibility_rules',
       name_vectors := 'name_vectors', name_floor := getvariable('name_floor'));

-- gold_* files are ground truth: use them to score, never as matcher input.
SELECT count(*) AS requests,
       round(avg((g.true_coverage >= q.best_coverage - 1e-9)::INT), 3) AS top1_is_best,
       round(avg(q.best_coverage - g.true_coverage), 4) AS mean_regret,
       round(avg((r.coverage < 0.95)::INT) FILTER (WHERE q.is_unmeetable), 3) AS unmeetable_flagged
FROM results r
JOIN read_csv('test/data/coverage/gold_coverage.csv.gz') g
  ON g.query_id = r.query_id AND g.candidate_id = r.candidate_id
JOIN read_csv('test/data/coverage/gold_requests.csv.gz') q ON q.request_id = r.query_id
WHERE r.rank = 1;
-- Expected: 80 | 0.925 | 0.0087 | 1.0
-- The top-1 pick is a best programme for 92.5 % of the requests, and every
-- unmeetable request scores below 0.95, so none is auto-accepted.
