-- ============================================================================
-- Second source, step 2: rank the approved foundries per part, explain the
-- gaps, re-check hard limits, and score the RFQ list against the gold file
-- ============================================================================
-- Every part is a tree with one item per capability class (piece_weight,
-- envelope_1..3, material_grade, pressure_class, cert_level, lot_size,
-- roughness, tolerance); so is every supplier. tree_coverage compares the items
-- of the same class: numbers with their direction, codes through the rules
-- table (PN and EN 10204 ladders; grades must be equal). Coverage is the share
-- of a part's requirement classes a supplier meets; n_gaps counts the classes
-- it fails.
--
-- Functions: tree_coverage, tree_coverage_trace
-- Run: DATAZOO_DISABLE_TELEMETRY=1 ./build/release/duckdb < examples/second_source/02_rank_and_score.sql
-- ============================================================================

.read examples/second_source/sql/load.sql

.print '>>> 1. All 40 parts against the 12 approved foundries in one call'
-- mu := 0.0: the wordings differ by design (drawing vs capability sheet); the
-- class already says which values belong together, so names must not discount.
CREATE OR REPLACE TABLE ranking AS
SELECT * FROM tree_coverage(getvariable('part_ids'), candidates := getvariable('approved'),
                            nodes_table := 'part_nodes', attrs_table := 'part_attributes',
                            candidate_nodes_table := 'supplier_nodes', candidate_attrs_table := 'supplier_attributes',
                            rules_table := 'compatibility_rules', mu := 0.0);
SELECT query_id AS part_id, candidate_id AS supplier, round(coverage, 3) AS coverage, n_requirements, n_gaps, rank
FROM ranking WHERE query_id IN ('P-1003', 'P-1005') AND rank <= 3
ORDER BY query_id, rank;
-- Expected:
--   P-1003 | SUP-BRANDT | 1.0 |  7 | 0 | 1
--   P-1003 | SUP-KOVACS | 1.0 |  7 | 0 | 2
--   P-1003 | SUP-LOIRE  | 1.0 |  7 | 0 | 3
--   P-1005 | SUP-BRANDT | 0.9 | 10 | 1 | 1
--   P-1005 | SUP-ALPEN  | 0.8 | 10 | 2 | 2
--   P-1005 | SUP-RHEIN  | 0.8 | 10 | 2 | 3
-- P-1003 has three foundries that meet everything. P-1005's best is 0.9 with a gap.

.print ''
.print '>>> 2. The gap list: why P-1005 has no full cover'
CREATE OR REPLACE TABLE trace AS
SELECT * FROM tree_coverage_trace(getvariable('part_ids'), candidates := getvariable('approved'),
                                  nodes_table := 'part_nodes', attrs_table := 'part_attributes',
                                  candidate_nodes_table := 'supplier_nodes', candidate_attrs_table := 'supplier_attributes',
                                  rules_table := 'compatibility_rules', mu := 0.0);
SELECT candidate_id AS supplier, split_part(req_node_id, ':', 2) AS class, attr_name, req_value, prov_value,
       round(distance, 3) AS distance
FROM trace
WHERE query_id = 'P-1005' AND candidate_id IN ('SUP-BRANDT', 'SUP-ALPEN') AND is_gap
ORDER BY supplier, class;
-- Expected:
--   SUP-ALPEN  | envelope_1   | raw part envelope | at_least 0.7 m  | 0.6 m       | 0.143
--   SUP-ALPEN  | piece_weight | Rohgewicht      | at_least 400 kg   | 300 kg      | 0.25
--   SUP-BRANDT | cert_level   | Zeugnis         | point EN10204-3.2 | EN10204-2.2 | NULL
-- Brandt cannot issue a 3.2 certificate (third-party inspection); Alpenguss can,
-- but its flask is 600 mm and it pours 300 kg at most. The drawing's weight was
-- "ca. 400 kg": read as at_least 400 kg because the class says so.
-- Brandt issues 2.2 and 3.1; the trace shows 2.2 because both score 0 against 3.2
-- and the first one is kept.

.print ''
.print '>>> 3. Hard limits: the default tolerance scores up to 2 % outside a limit as met'
-- value_satisfies' default (tol 0.02, on a log scale for mass: about 4.7 %, plus a
-- soft band) is right for ranking but wrong for a weight or flask limit. The
-- trace shows what it lets through: a score of 1 with a distance above 0.
SELECT query_id AS part_id, candidate_id AS supplier, split_part(req_node_id, ':', 2) AS class,
       req_value, prov_value, value_score, round(distance, 4) AS distance
FROM trace
WHERE rule IN ('at_least', 'at_most') AND value_score = 1 AND distance > 0
ORDER BY ALL;
-- Expected:
--   P-1005 | SUP-ALPEN  | envelope_2   | at_least 0.52 m | 0.5 m | 1.0 | 0.0385
--   P-1010 | SUP-VENETO | piece_weight | at_least 38 kg  | 37 kg | 1.0 | 0.0263
-- Fonderia Veneta pours 37 kg; the part weighs 38 kg.

-- For the RFQ list, rank again with hard limits: tol := 0, soft_band := 0.
CREATE OR REPLACE TABLE strict_ranking AS
SELECT *, n_gaps = 0 AS strict_full
FROM tree_coverage(getvariable('part_ids'), candidates := getvariable('approved'),
                   nodes_table := 'part_nodes', attrs_table := 'part_attributes',
                   candidate_nodes_table := 'supplier_nodes', candidate_attrs_table := 'supplier_attributes',
                   rules_table := 'compatibility_rules', mu := 0.0, tol := 0.0, soft_band := 0.0);
CREATE OR REPLACE TABLE strict_trace AS
SELECT * FROM tree_coverage_trace(getvariable('part_ids'), candidates := getvariable('approved'),
                                  nodes_table := 'part_nodes', attrs_table := 'part_attributes',
                                  candidate_nodes_table := 'supplier_nodes', candidate_attrs_table := 'supplier_attributes',
                                  rules_table := 'compatibility_rules', mu := 0.0, tol := 0.0, soft_band := 0.0);
SELECT query_id AS part_id, candidate_id AS supplier, round(coverage, 3) AS strict_coverage, n_gaps
FROM strict_ranking WHERE (query_id, candidate_id) IN (('P-1010', 'SUP-VENETO'), ('P-1039', 'SUP-ALPEN'))
ORDER BY ALL;
-- Expected:
--   P-1010 | SUP-VENETO | 0.9 | 1
--   P-1039 | SUP-ALPEN  | 0.9 | 1
-- Under hard limits the 37 kg pour and the 650 mm part in a 600 mm flask are gaps.

.print ''
.print '>>> 4. Headline: how many parts have an approved second source?'
SELECT count(DISTINCT query_id) FILTER (WHERE coverage >= 0.9) AS at_coverage_0_9,
       count(DISTINCT query_id) FILTER (WHERE n_gaps = 0) AS full_cover_kernel,
       (SELECT count(DISTINCT query_id) FROM strict_ranking WHERE strict_full) AS full_cover_strict,
       40 - (SELECT count(DISTINCT query_id) FROM strict_ranking WHERE strict_full) AS need_new_supplier
FROM ranking;
-- Expected:
--   at_coverage_0_9 | full_cover_kernel | full_cover_strict | need_new_supplier
--   37              | 34                | 33                | 7
-- "coverage >= 0.9" admits 3 parts whose best foundry fails one class (P-1005 above:
-- 9 of 10 classes met). Coverage is a ranking score, not a compliance verdict:
-- for an RFQ list use n_gaps = 0 under hard limits.

.print ''
.print '>>> 5. The RFQ list and the parts that need a new supplier'
-- Up to three strict full covers per part, best stage-1 fit (least surplus) first
CREATE OR REPLACE TABLE rfq_list AS
SELECT query_id AS part_id, candidate_id AS supplier_id,
       row_number() OVER (PARTITION BY query_id ORDER BY rank) AS rfq_rank
FROM strict_ranking WHERE strict_full
QUALIFY rfq_rank <= 3;
SELECT count(*) AS rfq_lines, count(DISTINCT part_id) AS parts, count(DISTINCT supplier_id) AS foundries
FROM rfq_list;
-- Expected:
--   rfq_lines | parts | foundries
--   81        | 33    | 9
-- Parts without a strict full cover: the best partial supplier and what it fails
SELECT r.query_id AS part_id, p.description, r.candidate_id AS best_partial, round(r.coverage, 3) AS coverage,
       string_agg(DISTINCT split_part(t.req_node_id, ':', 2), ', ') AS fails
FROM strict_ranking r JOIN parts p ON p.part_id = r.query_id
JOIN strict_trace t ON t.query_id = r.query_id AND t.candidate_id = r.candidate_id AND t.is_gap
WHERE r.rank = 1 AND r.query_id NOT IN (SELECT part_id FROM rfq_list)
GROUP BY ALL ORDER BY part_id;
-- Expected:
--   P-1001 | Konsole       | SUP-NORDGUSS | 0.833 | envelope_1      (2.8 m long; the largest flask is 2.5 m)
--   P-1005 | Ventilgehäuse | SUP-BRANDT   | 0.9   | cert_level      (3.2 at 400 kg)
--   P-1010 | Ventilgehäuse | SUP-ALPEN    | 0.9   | material_grade  (Veneta's 37 kg for 38 kg is a gap now too)
--   P-1016 | Spiralgehäuse | SUP-NORDGUSS | 0.9   | piece_weight    (6 t; Nordguss pours 5 t)
--   P-1022 | Laufrad       | SUP-ALPEN    | 0.889 | roughness       (Ra 0.8 µm at 200 kg)
--   P-1031 | Laufrad       | SUP-BRANDT   | 0.889 | material_grade  (GJS-800 at 500 kg)
--   P-1039 | Pumpengehäuse | SUP-ALPEN    | 0.9   | envelope_1      (650 mm for a 600 mm flask)
-- These 7 parts go to supplier search.

.print ''
.print '>>> 6. Score against the gold file'
-- gold_second_source.csv: for every part and approved foundry, whether it truly
-- covers (computed from the clean values with strict limits) and what it fails.
CREATE OR REPLACE TABLE gold AS SELECT * FROM read_csv('examples/second_source/data/gold_second_source.csv');
WITH truth AS (SELECT part_id, bool_or(covers) AS has_source FROM gold GROUP BY part_id),
listed AS (  -- the top 3 of each list, kernel only vs. with the strict re-check
    SELECT 'kernel' AS list, query_id AS part_id, candidate_id AS supplier_id FROM ranking
    WHERE n_gaps = 0 QUALIFY row_number() OVER (PARTITION BY query_id ORDER BY rank) <= 3
    UNION ALL
    SELECT 'strict', part_id, supplier_id FROM rfq_list)
SELECT l.list,
       round(count(DISTINCT l.part_id) FILTER (WHERE g.covers) / (SELECT count(*) FROM truth WHERE has_source), 3)
           AS recall_at_3,
       round(avg(g.covers::INT), 3) AS precision_of_listed,
       count(*) FILTER (WHERE NOT g.covers) AS false_sources,
       (SELECT count(*) FROM truth WHERE NOT has_source
          AND part_id NOT IN (SELECT part_id FROM listed l2 WHERE l2.list = l.list)) AS no_source_found,
       (SELECT count(*) FROM truth WHERE NOT has_source) AS no_source_gold
FROM listed l JOIN gold g USING (part_id, supplier_id)
GROUP BY l.list ORDER BY l.list;
-- Expected:
--   list   | recall_at_3 | precision_of_listed | false_sources | no_source_found | no_source_gold
--   kernel | 1.0         | 0.988               | 1             | 6               | 7
--   strict | 1.0         | 1.0                 | 0             | 7               | 7
-- recall@3: every part that has a capable foundry gets one in its RFQ list.
-- Hard limits remove the one false source and find the 7th part that needs a new
-- supplier.

.print ''
.print '>>> 7. What the vendor filters prevent'
-- The same ranking over every vendor record that has a capability sheet:
-- including the unapproved Novaguss and Brandt's second vendor record.
SET VARIABLE all_records = (SELECT list(DISTINCT tree_id ORDER BY tree_id) FROM supplier_nodes);
WITH r AS (
    SELECT * FROM tree_coverage(getvariable('part_ids'), candidates := getvariable('all_records'),
                                nodes_table := 'part_nodes', attrs_table := 'part_attributes',
                                candidate_nodes_table := 'supplier_nodes',
                                candidate_attrs_table := 'supplier_attributes',
                                rules_table := 'compatibility_rules', mu := 0.0)
), top3 AS (SELECT * FROM r WHERE n_gaps = 0 QUALIFY row_number() OVER (PARTITION BY query_id ORDER BY rank) <= 3)
SELECT (SELECT count(DISTINCT query_id) FROM top3) AS parts_with_full_cover,
       (SELECT count(*) FROM top3 WHERE candidate_id = 'SUP-NOVA') AS novaguss_in_top3,
       (SELECT count(*) FROM (SELECT query_id FROM top3 WHERE candidate_id IN ('SUP-BRANDT', 'SUP-BRANDT2')
                              GROUP BY query_id HAVING count(*) = 2)) AS brandt_listed_twice;
-- Expected:
--   parts_with_full_cover | novaguss_in_top3 | brandt_listed_twice
--   40                    | 19               | 21
-- Without the approval filter every part looks sourced, because Novaguss can make
-- everything. Without the vendor dedup, Brandt takes two of three RFQ slots on
-- 21 parts and the third foundry is never asked.
