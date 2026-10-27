-- ============================================================================
-- Sub-linear recall: weighted MinHash + LSH bands
-- ============================================================================
-- find_covering_entities scans every entity that shares a feature with the
-- query. For large archives with a large raw vocabulary (BOM components,
-- part numbers) an index finds the candidates faster:
--   weighted_minhash  a signature per entity; equal positions estimate
--                     weighted Jaccard
--   minhash_index     signatures + band hashes, split into size partitions
--   minhash_bands     the band hashes in long form; store once, then the
--                     lookup is a hash join
--   find_covering_entities_lsh  many queries in one call, ranked by the
--                     estimated containment (or Jaccard)
-- Exact find_covering_entities stays the reference.
--
-- Data: a synthetic raw vocabulary (range()), then
--       test/data/coverage/programme_class_multiset.csv.gz + request trees
--
-- Run: ./build/release/duckdb < examples/dedup_and_lsh/minhash_lsh.sql
-- ============================================================================

.print '>>> 1. Signatures estimate weighted Jaccard'
SELECT weighted_jaccard(MAP {'a': 1.0, 'b': 1.0}, MAP {'a': 1.0, 'b': 3.0}) AS exact,
       round(minhash_similarity(weighted_minhash(MAP {'a': 1.0, 'b': 1.0}, 1024),
                                weighted_minhash(MAP {'a': 1.0, 'b': 3.0}, 1024)), 3) AS estimate,
       len(lsh_bands(weighted_minhash(MAP {'a': 1.0}, 64), 16)) AS bands;
-- Expected: 0.5 | 0.525 | 16   (the estimate gets closer with more hashes)

.print ''
.print '>>> 2. A large raw vocabulary: 2 000 entities × 20 of 5 000 features'
-- hash() scatters the features; DISTINCT drops the rare repeated feature.
CREATE OR REPLACE TABLE raw_vocab AS
SELECT DISTINCT 'e' || i AS entity_id, 'f' || (hash(i * 100 + j) % 5000) AS feature_id, 1.0 AS weight
FROM range(2000) t(i), range(20) u(j);

CREATE OR REPLACE TABLE raw_idx AS
SELECT * FROM minhash_index(feature_table := 'raw_vocab', num_hashes := 128);
CREATE OR REPLACE TABLE raw_bands AS SELECT * FROM minhash_bands('raw_idx');

-- Queries are (query_id, feature_id[, weight]) rows: here 12 of e17's 20 features.
CREATE OR REPLACE TABLE raw_q AS
SELECT 'q' AS query_id, feature_id, weight
FROM raw_vocab WHERE entity_id = 'e17' ORDER BY feature_id LIMIT 12;

SELECT query_id, entity_id, rank, round(estimate, 2) AS estimate,
       n_candidates < 100 AS scored_under_5_percent
FROM find_covering_entities_lsh('raw_q', 'raw_idx', k := 1, threshold := 0.8,
                                bands_table := 'raw_bands');
-- Expected: q | e17 | 1 | 0.99 | true
-- LSH finds the entity that contains the query and scores fewer than 100 of
-- the 2 000 entities.

.print ''
.print '>>> 3. The programme archive: 80 requests at once'
CREATE OR REPLACE TABLE programme_ms AS
SELECT * FROM read_csv('test/data/coverage/programme_class_multiset.csv.gz');

-- Request class multisets from the request trees (reviewed classes).
CREATE OR REPLACE TABLE req_nodes AS
SELECT n.* EXCLUDE (class_id), g.class_id
FROM read_csv('test/data/coverage/request_nodes.csv.gz') n
LEFT JOIN read_csv('test/data/coverage/gold_item_class.csv.gz') g USING (tree_id, node_id);
CREATE OR REPLACE TABLE request_ms AS
SELECT entity_id AS query_id, feature_id, weight FROM tree_class_multiset('req_nodes');

CREATE OR REPLACE TABLE idx AS
SELECT * FROM minhash_index(feature_table := 'programme_ms', num_hashes := 128);
CREATE OR REPLACE TABLE lsh AS
SELECT * FROM find_covering_entities_lsh('request_ms', 'idx', k := 20, threshold := 0.9);

-- The exact reference: the programmes with the highest containment per request.
CREATE OR REPLACE TABLE exact_best AS
SELECT * FROM (
    SELECT q.query_id, p.entity_id, sum(least(q.weight, p.weight)) / any_value(t.total) AS containment
    FROM request_ms q
    JOIN programme_ms p ON p.feature_id = q.feature_id
    JOIN (SELECT query_id, sum(weight) AS total FROM request_ms GROUP BY ALL) t USING (query_id)
    GROUP BY ALL)
QUALIFY containment >= max(containment) OVER (PARTITION BY query_id) - 1e-9;

SELECT count(*) AS requests,
       round(avg(found::INT), 3) AS best_in_lsh_top20,
       round((SELECT avg(n) FROM (SELECT any_value(n_candidates) AS n FROM lsh GROUP BY query_id)), 1)
           AS avg_programmes_scored
FROM (SELECT b.query_id, bool_or(l.entity_id IS NOT NULL) AS found
      FROM exact_best b
      LEFT JOIN lsh l ON l.query_id = b.query_id AND l.entity_id = b.entity_id
      GROUP BY ALL);
-- Expected: 80 | 1.0 | 164.7
-- A best programme is in the LSH top 20 for every request. With only 23
-- classes the multisets overlap heavily, so LSH still scores about half of
-- the 300 programmes; it pays off for raw vocabularies (section 2).
