-- ============================================================================
-- Entity clustering: merge duplicate customer records
-- ============================================================================
-- cluster_matches turns scored pairs (a, b, score) into clusters: every pair
-- at or above the threshold links its two records, and a union-find groups
-- all linked records. Every entity of the pairs table appears in the output;
-- cluster_id is the smallest member id (text order).
-- Scoring the pairs is up to you: block, normalise, compare. The recipe below
-- does it with plain DuckDB SQL (jaro_winkler_similarity, regexp_replace).
--
-- Function:
--   cluster_matches(pairs_table [, threshold := 0.8, left_col := 'a',
--                   right_col := 'b', score_col := 'score'])
--
-- Data: inline pairs, then test/data/coverage/customers.csv.gz
--       (1 910 records: groups, legal entities, plants, contacts; 152 planted
--       duplicates and 12 near misses at the address of another company)
--
-- Run: ./build/release/duckdb < examples/dedup_and_lsh/cluster_matches.sql
-- ============================================================================

.print '>>> 1. Clusters from five scored pairs'
CREATE OR REPLACE TABLE toy_pairs AS SELECT * FROM (VALUES
    ('A', 'B', 0.95), ('B', 'C', 0.90),   -- A-B-C chain: one cluster
    ('C', 'D', 0.40),                     -- below the threshold: D stays alone
    ('E', 'F', 0.85), ('G', 'G', 1.00)    -- a self-pair keeps a singleton in the output
) t(a, b, score);

SELECT entity_id, cluster_id, cluster_size
FROM cluster_matches('toy_pairs', threshold := 0.8)
ORDER BY entity_id;
-- Expected: A, B, C -> cluster A (size 3); D -> D (1); E, F -> E (2); G -> G (1)

.print ''
.print '>>> 2. The customer master: normalise names and addresses'
CREATE OR REPLACE TABLE customers AS
SELECT * FROM read_csv('test/data/coverage/customers.csv.gz');

-- ü -> ue, ß -> ss, strasse -> str, punctuation -> space
CREATE OR REPLACE MACRO norm(s) AS
    regexp_replace(replace(replace(replace(replace(replace(lower(COALESCE(s, '')),
        'ä', 'ae'), 'ö', 'oe'), 'ü', 'ue'), 'ß', 'ss'), 'strasse', 'str'), '[^a-z0-9]+', ' ', 'g');
-- GmbH, G.m.b.H. and GmbH & Co. KG to one spelling each
CREATE OR REPLACE MACRO legal(s) AS
    CASE WHEN norm(s) LIKE '%g m b h%' OR norm(s) LIKE '%gmbh%'
         THEN (CASE WHEN norm(s) LIKE '%kg%' THEN 'gmbh co kg' ELSE 'gmbh' END)
         ELSE norm(s) END;

CREATE OR REPLACE TABLE records AS
SELECT record_id, record_type, COALESCE(tax_id, '') AS tax, lower(COALESCE(email, '')) AS email,
       COALESCE(parent_record_id, '') AS parent,
       regexp_extract(COALESCE(plant_name, ''), '([0-9]+)$', 1) AS plant_no,
       trim(norm(CASE record_type
                     WHEN 'contact' THEN contact_name
                     WHEN 'plant' THEN company_name || ' ' || regexp_replace(norm(plant_name), '^(werk|standort) ', '')
                     WHEN 'legal_entity' THEN company_name || ' ' || legal(legal_form)
                     ELSE group_name END)) AS ident,
       norm(street) || ' ' || COALESCE(CAST(house_no AS VARCHAR), '') || ' ' || norm(city) AS addr,
       norm(city) AS city
FROM customers;

.print ''
.print '>>> 3. Score candidate pairs (blocked by tax id or city) and cluster at 0.85'
CREATE OR REPLACE TABLE pairs AS
SELECT a.record_id AS a, b.record_id AS b,
       CASE WHEN a.record_type = 'contact'
            THEN 0.5 * jaro_winkler_similarity(a.ident, b.ident)
                 + 0.3 * (a.email = b.email AND a.email <> '')::INT
                 + 0.2 * (a.parent = b.parent AND a.parent <> '')::INT
            WHEN a.record_type = 'plant' AND a.plant_no <> b.plant_no THEN 0.0
            ELSE 0.6 * jaro_winkler_similarity(a.ident, b.ident)
                 + 0.3 * (a.tax <> '' AND a.tax = b.tax)::INT
                 + 0.1 * (a.addr = b.addr)::INT END AS score
FROM records a JOIN records b
  ON a.record_type = b.record_type AND a.record_id < b.record_id
 AND ((a.tax = b.tax AND a.tax <> '') OR a.city = b.city);
-- A self-pair per record, so every customer gets a cluster.
INSERT INTO pairs SELECT record_id, record_id, 1.0 FROM records;

CREATE OR REPLACE TABLE clusters AS
SELECT * FROM cluster_matches('pairs', threshold := 0.85);

SELECT count(*) AS records, count(DISTINCT cluster_id) AS clusters,
       count(*) FILTER (WHERE cluster_size > 1) AS records_in_merged_clusters
FROM clusters;
-- Expected: 1910 | 1750 | 307

.print ''
.print '>>> 4. Check against the planted duplicates'
CREATE OR REPLACE TABLE gold_clusters AS
SELECT * FROM read_csv('test/data/coverage/gold_customer_clusters.csv.gz');

-- gold cluster_id = the original record; lookalike_of = what a near miss resembles
SELECT g.dup_kind, count(*) AS planted,
       count(*) FILTER (WHERE c.cluster_id = o.cluster_id) AS merged
FROM gold_clusters g
JOIN clusters c ON c.entity_id = g.record_id
JOIN clusters o ON o.entity_id = CASE WHEN g.dup_kind = 'near_miss' THEN g.lookalike_of ELSE g.cluster_id END
WHERE g.record_id <> g.cluster_id OR g.dup_kind = 'near_miss'
GROUP BY g.dup_kind ORDER BY g.dup_kind;
-- Expected:
--   legal_form    | 30 | 30
--   moved_plant   | 30 | 30
--   near_miss     | 12 | 0    no near miss merged with the company it resembles
--   shared_tax_id | 30 | 30
--   spelling      | 31 | 31
--   umlaut        | 31 | 31
-- All 152 planted duplicates are merged with their original.
