-- ============================================================================
-- Jaccard Similarity: which materials share the most components?
-- ============================================================================
-- Jaccard similarity compares two component SETS: |A ∩ B| / |A ∪ B|.
-- It ignores quantities and BOM depth: two bikes with the same parts list
-- score 1.0, two bikes with no part in common score 0.0.
--
-- Functions:
--   jaccard_similarity(list_a, list_b)                 scalar, any two lists
--   find_similar_materials_jaccard(material_id, k, ...) top-k over a BOM table
--
-- Data: a tiny inline BOM, then the AdventureWorks bicycle BOM
--       (test/data/adventureworks/bom_items.csv.gz, 2 576 edges, 238 parents)
--
-- Run: ./build/release/duckdb < examples/material_similarity/jaccard_similarity.sql
-- ============================================================================

.print '>>> 1. jaccard_similarity on two literal lists'
-- 2 shared elements (A, B) out of 4 distinct elements (A, B, C, D) = 0.5
SELECT jaccard_similarity(['A', 'B', 'C'], ['A', 'B', 'D']) AS similarity;
-- Expected: 0.5

.print ''
.print '>>> 2. find_similar_materials_jaccard on a tiny inline BOM'
-- The BOM table needs (at least) the columns parent_id and child_id.
CREATE OR REPLACE TABLE pump_bom (parent_id VARCHAR, child_id VARCHAR);
INSERT INTO pump_bom VALUES
    ('PUMP-A', 'SEAL-001'), ('PUMP-A', 'BEARING-X'), ('PUMP-A', 'MOTOR-M1'),
    ('PUMP-B', 'SEAL-001'), ('PUMP-B', 'BEARING-X'), ('PUMP-B', 'MOTOR-M1'), ('PUMP-B', 'GASKET-Y'),
    ('PUMP-C', 'FILTER-F1'), ('PUMP-C', 'VALVE-V2');

-- material id and k are positional; the BOM table is a named argument.
SELECT material_id, round(similarity, 3) AS similarity, shared_components, total_components
FROM find_similar_materials_jaccard('PUMP-A', 3, bom_table := 'pump_bom');
-- Expected:
--   PUMP-B | 0.75 | 3 | 4    (3 shared out of 4 distinct parts)
--   PUMP-C | 0.0  | 0 | 5    (disjoint, still returned; filter with min_similarity)

.print ''
.print '>>> 3. Containment mode: how much of the query BOM does a candidate contain?'
-- Containment = |A ∩ B| / |A|. PUMP-B contains all of PUMP-A's parts -> 1.0.
SELECT material_id, round(similarity, 3) AS containment
FROM find_similar_materials_jaccard('PUMP-A', 3, bom_table := 'pump_bom',
                                    mode := 'containment', min_similarity := 0.1);
-- Expected: PUMP-B | 1.0

.print ''
.print '>>> 4. AdventureWorks: bikes most similar to "Road-150 Red, 62" (AW-749)'
CREATE OR REPLACE TABLE bom_items AS
SELECT * FROM read_csv('test/data/adventureworks/bom_items.csv.gz');

CREATE OR REPLACE TABLE materials AS
SELECT * FROM read_csv('test/data/adventureworks/materials.csv.gz');

-- bom_table defaults to 'bom_items', so it can be omitted here.
SELECT s.material_id, m.description, round(s.similarity, 3) AS similarity,
       s.shared_components, s.total_components
FROM find_similar_materials_jaccard('AW-749', 5) s
JOIN materials m USING (material_id)
ORDER BY s.similarity DESC, s.material_id;
-- Expected: the other four Road-150 frame sizes (AW-750..AW-753) share 13 of
-- 15 distinct components (0.867); the next family, Road-250 (AW-789), 0.647.

.print ''
.print '>>> 5. jaccard_similarity on aggregated component lists'
-- The scalar works on any two lists, e.g. the result of list(child_id).
WITH comps AS (
    SELECT parent_id, list(child_id) AS components
    FROM bom_items GROUP BY parent_id
)
SELECT round(jaccard_similarity(a.components, b.components), 3) AS road150_vs_road250
FROM comps a, comps b
WHERE a.parent_id = 'AW-749' AND b.parent_id = 'AW-789';
-- Expected: 0.647 (the same number find_similar_materials_jaccard reported)
