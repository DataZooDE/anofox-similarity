-- ============================================================================
-- Stage 2 on a hand-made fixture: coverage, ranking and the gap list
-- ============================================================================
-- Requests and offers are trees: tree_nodes(tree_id, node_id, parent_id,
-- node_kind, edge_type, label, class_id, count, ...) and tree_attributes with
-- values in base units. tree_coverage compares the ITEM nodes: per requirement
-- it finds the best offered item of the same class, checks every attribute
-- value with its direction (at_least / at_most / ...) and the count, and
-- averages the requirement scores into a coverage in [0, 1].
-- tree_coverage_trace explains the same score row by row.
--
-- Functions:
--   tree_coverage(query_tree_id, candidates := [...], k := 20, ...)
--   tree_coverage_trace(query_tree_id, candidates := [...], ...)
--
-- Data: test/data/coverage/mini (classes already reviewed, values in base units)
--   Q1: 2 x seat heating (<= 180 s to 37 °C, <= 8 A), 4-zone massage,
--       powered headrest (>= 0.06 m travel), memory (>= 8 ways)
--   S1..S6: past programmes
--
-- Run: ./build/release/duckdb < examples/coverage_matching/tree_coverage_mini.sql
-- ============================================================================

-- The default table names are tree_nodes and tree_attributes.
CREATE OR REPLACE TABLE tree_nodes AS
SELECT * FROM read_csv('test/data/coverage/mini/tree_nodes.csv');
CREATE OR REPLACE TABLE tree_attributes AS
SELECT * FROM read_csv('test/data/coverage/mini/tree_attributes.csv');

.print '>>> 1. Rank programmes S1..S6 against request Q1'
-- mu := 0.0 switches off the attribute-name discount so only the values count.
SELECT candidate_id, coverage, round(stage1_score, 4) AS stage1, rank,
       n_requirements, n_gaps, assignment_used
FROM tree_coverage('Q1', candidates := ['S1', 'S2', 'S3', 'S4', 'S5', 'S6'], mu := 0.0)
ORDER BY rank;
-- Expected:
--   S1 | 1.0   | 1.0089 | 1 | 4 | 0 | partition   meets everything
--   S6 | 1.0   | 1.0067 | 2 | 4 | 0 | partition   meets everything, more surplus
--   S3 | 0.625 | 0.6313 | 3 | 4 | 2 | partition   2 of 4 massage zones, no memory
--   S2 | 0.5   | 1.01   | 4 | 4 | 2 | partition   right classes, values miss
--   S4 | 0.125 | 0.2517 | 5 | 4 | 4 | partition
--   S5 | 0.0   | 0.1261 | 6 | 4 | 4 | partition
-- S2 has the best class multiset (stage 1) but its values fail: stage 2 ranks it 4th.

.print ''
.print '>>> 2. The gap list for S2: what fails, and by how much'
SELECT req_node_id, matched_node_id, attr_id, rule, req_value, prov_value,
       value_score, round(distance, 4) AS distance, is_gap
FROM tree_coverage_trace('Q1', candidates := ['S2'], mu := 0.0)
ORDER BY req_node_id, attr_id;
-- Expected:
--   Q1.1 heating: a1 at_most, 240 s provided for 180 s  -> distance 0.3333, gap
--   Q1.1 heating: a2 at_most, 9 A provided for 8 A      -> distance 0.125,  gap
--   Q1.2 massage: no attributes, 4 zones available      -> not a gap
--   Q1.3 headrest: a1 at_least, 0.05 m for 0.06 m       -> distance 0.1667, gap
--   Q1.4 memory:  a1 at_least, 8 ways for 8             -> not a gap

.print ''
.print '>>> 3. Let stage 1 choose the candidates'
-- Without candidates, stage 1 (class-multiset containment) keeps the top k
-- trees of the candidate table. The query tree is never its own candidate.
SELECT candidate_id, coverage, rank
FROM tree_coverage('Q1', k := 3, mu := 0.0)
ORDER BY rank;
-- Expected: S1 1.0, S6 1.0, S2 0.5

.print ''
.print '>>> 4. OR alternatives: the best variant of module M1 counts'
-- Q2 asks for a seat track with >= 0.24 m travel and <= 38 dB(A).
-- Module M1 offers three variants; one of them meets both limits.
SELECT candidate_id, round(coverage, 4) AS coverage
FROM tree_coverage('Q2', candidates := ['M1', 'S5'], mu := 0.0)
ORDER BY rank;
-- Expected: M1 1.0, S5 0.6167 (S5's track travels only 0.22 m)
