-- C-parts catalogue harmonisation: exact keys first, then a deliberate wildcard trap.
-- Run from the repository root with the extension-linked DuckDB shell.
.print '>>> 1. Load catalogue and normalise the decision keys'
CREATE TABLE parts AS SELECT * FROM read_csv('examples/c_parts_harmonisation/data/parts.csv', all_varchar := true);
CREATE TABLE standard_synonyms AS SELECT * FROM read_csv('examples/c_parts_harmonisation/data/standard_synonyms.csv', all_varchar := true);
CREATE TABLE gold AS SELECT * FROM read_csv('examples/c_parts_harmonisation/data/gold_clusters.csv', all_varchar := true);

-- The standards equivalences are reviewed input, not inferred from the description.
CREATE TABLE keys AS
SELECT p.part_id, p.supplier, p.description, s.standard_family_norm,
       upper(regexp_replace(p.thread_raw, '[[:space:]]+', '', 'g')) AS thread,
       CASE WHEN trim(p.length_raw) = '' THEN NULL
            WHEN lower(p.length_raw) LIKE '%cm' THEN
                 try_cast(replace(regexp_extract(p.length_raw, '([0-9]+([,.][0-9]+)?)', 1), ',', '.') AS DOUBLE) * 10
            ELSE try_cast(replace(regexp_extract(p.length_raw, '([0-9]+([,.][0-9]+)?)', 1), ',', '.') AS DOUBLE)
       END AS length_mm,
       upper(regexp_replace(p.grade_raw, '[[:space:]]+', '', 'g')) AS grade,
       try_cast(p.unit_price_eur AS DOUBLE) AS unit_price_eur
FROM parts p JOIN standard_synonyms s USING (standard_raw);

SELECT count(*) AS catalogue_lines, count(*) FILTER (WHERE length_mm IS NULL) AS absent_lengths,
       count(*) FILTER (WHERE standard_family_norm IS NULL OR thread IS NULL OR grade IS NULL) AS missing_keys
FROM keys;
-- Expected: 91 | 41 | 0

.print '>>> 2. Form exact-key pairs and cluster'
CREATE TABLE exact_pairs AS
SELECT a.part_id AS a, b.part_id AS b, 1.0 AS score
FROM keys a JOIN keys b ON a.part_id < b.part_id
 AND a.standard_family_norm = b.standard_family_norm
 AND a.thread = b.thread AND a.grade = b.grade
 AND a.length_mm IS NOT DISTINCT FROM b.length_mm
UNION ALL SELECT part_id, part_id, 1.0 FROM keys;
CREATE TABLE exact_clusters AS SELECT * FROM cluster_matches('exact_pairs', threshold := 0.9);

SELECT count(DISTINCT cluster_id) AS clusters,
       count(*) FILTER (WHERE cluster_size > 1) AS lines_in_merged_clusters,
       count(*) FILTER (WHERE cluster_size = 1) AS singletons
FROM exact_clusters;
-- Expected: 51 | 80 | 11

SELECT count(*) AS approved_duplicate_lines,
       count(*) FILTER (WHERE c.cluster_id = anchor.cluster_id) AS recovered
FROM gold g JOIN exact_clusters c ON g.part_id = c.entity_id
JOIN gold g0 ON g0.gold_cluster = g.gold_cluster AND g0.part_id < g.part_id
JOIN exact_clusters anchor ON anchor.entity_id = g0.part_id
WHERE g.decision = 'approved';
-- Expected: 40 | 40

.print '>>> 3. Price spread of approved two-supplier clusters'
SELECT c.cluster_id, count(*) AS lines, round(min(k.unit_price_eur), 3) AS low_eur,
       round(max(k.unit_price_eur), 3) AS high_eur,
       round(100 * (max(k.unit_price_eur) / min(k.unit_price_eur) - 1), 1) AS spread_pct
FROM exact_clusters c JOIN keys k ON k.part_id = c.entity_id
GROUP BY c.cluster_id HAVING count(*) > 1
ORDER BY spread_pct DESC, c.cluster_id LIMIT 5;
-- Expected: CP-B15-1 and CP-B19-1 lead at 59.1 percent.
SELECT count(*) AS priced_clusters,
       round(max(spread_pct), 1) AS largest_spread_pct
FROM (
  SELECT 100 * (max(k.unit_price_eur) / min(k.unit_price_eur) - 1) AS spread_pct
  FROM exact_clusters c JOIN keys k ON k.part_id = c.entity_id
  GROUP BY c.cluster_id HAVING count(*) > 1
);
-- Expected: 40 | 59.1

.print '>>> 4. A missing length can bridge conflicting specifications'
-- This permissive pair policy is a diagnostic only; its clusters need review.
CREATE TABLE loose_pairs AS
SELECT a.part_id AS a, b.part_id AS b, 0.95 AS score
FROM keys a JOIN keys b ON a.part_id < b.part_id
 AND a.standard_family_norm = b.standard_family_norm
 AND a.thread = b.thread AND a.grade = b.grade
 AND (a.length_mm = b.length_mm OR a.length_mm IS NULL OR b.length_mm IS NULL)
UNION ALL SELECT part_id, part_id, 1.0 FROM keys;
CREATE TABLE loose_clusters AS SELECT * FROM cluster_matches('loose_pairs', threshold := 0.9);

-- A whole-cluster guard: two known lengths in one component are a conflict.
CREATE TABLE cluster_guard AS
SELECT c.cluster_id, count(*) AS lines, count(DISTINCT k.length_mm) AS known_lengths,
       min(k.length_mm) AS min_length_mm, max(k.length_mm) AS max_length_mm,
       count(*) FILTER (WHERE k.length_mm IS NULL AND k.standard_family_norm IN ('ISO 4014', 'ISO 4017')) AS missing_required_lengths
FROM loose_clusters c JOIN keys k ON k.part_id = c.entity_id
GROUP BY c.cluster_id;
SELECT cluster_id, lines, known_lengths, min_length_mm, max_length_mm, missing_required_lengths
FROM cluster_guard WHERE known_lengths > 1 ORDER BY cluster_id;
-- Expected: CP-X25 | 3 | 2 | 25.0 | 30.0 | 1

SELECT c.entity_id, c.cluster_id, g.decision,
       CASE WHEN h.known_lengths > 1 OR h.missing_required_lengths > 0 THEN 'REVIEW' ELSE 'APPROVE' END AS disposition
FROM loose_clusters c JOIN gold g ON g.part_id = c.entity_id
JOIN cluster_guard h USING (cluster_id)
WHERE c.cluster_id = 'CP-X25' ORDER BY c.entity_id;
-- Expected: CP-X25, CP-X30, CP-XUNK all REVIEW
