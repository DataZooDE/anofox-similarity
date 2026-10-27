-- ============================================================================
-- Stage 1: which past programmes contain what a request asks for?
-- ============================================================================
-- Coverage matching is directional: "does B satisfy what A asks for?".
-- find_covering_entities ranks the entities of a generic (entity, feature,
-- weight) table by how much of the query's weight they cover:
--   containment = Σ min(q, c) / Σ q         (default mode)
--   jaccard     = Σ min(q, c) / Σ max(q, c)
--   score       = containment + 0.01 × jaccard   (the tie-break prefers lean covers)
-- Weights are counts: 4 massage zones are not covered by 2.
--
-- Functions:
--   find_covering_entities(query_entity_id, k, ...)  query = an entity of the table
--   find_covering_entities_for(query_map, k, ...)    query = MAP(feature, weight)
--
-- Data: test/data/coverage/programme_class_multiset.csv.gz
--       300 past seat programmes (P001..P300) as reviewed class multisets
--
-- Run: ./build/release/duckdb < examples/coverage_matching/find_covering_entities.sql
-- ============================================================================

CREATE OR REPLACE TABLE programme_classes AS
SELECT * FROM read_csv('test/data/coverage/programme_class_multiset.csv.gz');

.print '>>> 1. The class multiset of programme P001'
SELECT feature_id, weight FROM programme_classes WHERE entity_id = 'P001' ORDER BY feature_id;
-- Expected: 13 classes, 37 items in total (e.g. seat_heating 8, seat_motor 8)

.print ''
.print '>>> 2. Programmes that contain everything P001 contains'
-- The query entity itself is never returned.
SELECT entity_id, round(score, 4) AS score, containment, round(jaccard, 3) AS jaccard,
       candidate_weight, surplus_weight
FROM find_covering_entities('P001', 5, feature_table := 'programme_classes');
-- Expected:
--   P021 | 1.01   | 1.0 | 1.0   | 37 | 0    exact twins
--   P074 | 1.01   | 1.0 | 1.0   | 37 | 0
--   P016 | 1.0095 | 1.0 | 0.949 | 39 | 2    full cover plus 2 extra items
--   P232 | 1.0095 | 1.0 | 0.949 | 39 | 2
--   P256 | 1.0095 | 1.0 | 0.949 | 39 | 2

.print ''
.print '>>> 3. A new request that is not in the table: pass a MAP'
-- 2 heated zones, 4 massage zones, 2 powered headrests, 1 memory module.
SELECT entity_id, round(score, 4) AS score, containment, round(jaccard, 3) AS jaccard,
       shared_weight, surplus_weight
FROM find_covering_entities_for(
       MAP {'seat_heating': 2, 'massage': 4, 'headrest_powered': 2, 'memory_module': 1}, 5,
       feature_table := 'programme_classes');
-- Expected: P261 first (full cover, 31 surplus items, jaccard 0.225),
-- then P062, P090, P231, P299 (full cover, 35 surplus items, jaccard 0.205).

.print ''
.print '>>> 4. Same request, mode jaccard: similar size beats full cover'
SELECT entity_id, round(score, 3) AS score, round(containment, 3) AS containment,
       shared_weight, candidate_weight
FROM find_covering_entities_for(
       MAP {'seat_heating': 2, 'massage': 4, 'headrest_powered': 2, 'memory_module': 1}, 3,
       feature_table := 'programme_classes', mode := 'jaccard');
-- Expected:
--   P199 | 0.318 | 0.778 | 7 | 20    covers only 7 of 9 items, but is small
--   P068 | 0.238 | 0.556 | 5 | 17
--   P126 | 0.238 | 0.556 | 5 | 17

.print ''
.print '>>> 5. Only full covers: min_score := 1.0'
-- min_score applies to the measure before the tie-break.
SELECT count(*) AS full_covers
FROM find_covering_entities_for(
       MAP {'seat_heating': 2, 'massage': 4, 'headrest_powered': 2, 'memory_module': 1}, 300,
       feature_table := 'programme_classes', min_score := 1.0);
-- Expected: 49   (of the 300 programmes, these contain the whole request)

.print ''
.print '>>> 6. The directional scalars behind it'
SELECT weighted_containment(MAP {'massage': 4}, MAP {'massage': 2})              AS counts_matter,
       containment(['a', 'b'], ['a', 'b', 'c', 'd'])                            AS a_in_b,
       containment(['a', 'b', 'c', 'd'], ['a', 'b'])                            AS b_in_a,
       round(weighted_tversky(MAP {'a': 3}, MAP {'a': 2, 'b': 3}, 1.0, 0.0), 4)  AS tversky;
-- Expected: 0.5 | 1.0 | 0.5 | 0.6667
