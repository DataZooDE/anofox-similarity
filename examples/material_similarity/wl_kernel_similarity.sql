-- ============================================================================
-- WL Kernel Similarity: compare BOMs by structure, not just by parts list
-- ============================================================================
-- The Weisfeiler-Lehman kernel walks each BOM down several levels and compares
-- the components reached at each depth. Two products whose direct children
-- differ can still be close when their sub-assemblies contain the same parts;
-- the top-level Jaccard score cannot see that.
--
-- Functions:
--   wl_kernel_similarity(material_a, material_b, iterations, bom_table)   scalar
--   find_similar_materials_wl_kernel(material_id, k, iterations := 3, ...) top-k
--
-- Data: a tiny inline two-level BOM, then the AdventureWorks bicycle BOM
--
-- Run: ./build/release/duckdb < examples/material_similarity/wl_kernel_similarity.sql
-- ============================================================================

.print '>>> 1. Two drives with different sub-assemblies that contain the same parts'
-- DRIVE-1 and DRIVE-2 use different gearbox part numbers (GBX-10, GBX-20),
-- but both gearboxes are built from the same gears and housing.
-- DRIVE-3 uses a completely different gearbox.
CREATE OR REPLACE TABLE bom_items (parent_id VARCHAR, child_id VARCHAR);
INSERT INTO bom_items VALUES
    ('DRIVE-1', 'MOTOR-M1'), ('DRIVE-1', 'GBX-10'),
    ('DRIVE-2', 'MOTOR-M1'), ('DRIVE-2', 'GBX-20'),
    ('DRIVE-3', 'MOTOR-M1'), ('DRIVE-3', 'GBX-30'),
    ('GBX-10', 'GEAR-Z12'), ('GBX-10', 'GEAR-Z48'), ('GBX-10', 'HOUSING-H1'),
    ('GBX-20', 'GEAR-Z12'), ('GBX-20', 'GEAR-Z48'), ('GBX-20', 'HOUSING-H1'),
    ('GBX-30', 'BELT-B7'),  ('GBX-30', 'PULLEY-P2');

-- Top-level Jaccard sees only {MOTOR-M1, GBX-x}: both pairs score 1/3.
SELECT 'DRIVE-2' AS candidate,
       round(jaccard_similarity(['MOTOR-M1', 'GBX-10'], ['MOTOR-M1', 'GBX-20']), 3) AS jaccard,
       round(wl_kernel_similarity('DRIVE-1', 'DRIVE-2'), 3) AS wl_kernel
UNION ALL
SELECT 'DRIVE-3',
       round(jaccard_similarity(['MOTOR-M1', 'GBX-10'], ['MOTOR-M1', 'GBX-30']), 3),
       round(wl_kernel_similarity('DRIVE-1', 'DRIVE-3'), 3)
ORDER BY candidate;
-- Expected:
--   DRIVE-2 | 0.333 | 0.667   (same parts one level down)
--   DRIVE-3 | 0.333 | 0.125   (nothing in common below the motor)

.print ''
.print '>>> 2. iterations controls how deep the walk goes'
-- iterations := 1 looks at the direct children only, so the shared gears
-- one level down are invisible and the score drops to the Jaccard level.
SELECT it AS iterations, round(wl_kernel_similarity('DRIVE-1', 'DRIVE-2', it), 3) AS wl_kernel
FROM (VALUES (1), (2), (3)) t(it)
ORDER BY it;
-- Expected: 1 -> 0.333, 2 -> 0.667, 3 -> 0.667

.print ''
.print '>>> 3. find_similar_materials_wl_kernel on AdventureWorks'
CREATE OR REPLACE TABLE aw_bom AS
SELECT * FROM read_csv('test/data/adventureworks/bom_items.csv.gz');

CREATE OR REPLACE TABLE materials AS
SELECT * FROM read_csv('test/data/adventureworks/materials.csv.gz');

-- Bottom brackets: HL (AW-996) against every other BOM, ranked by structure.
SELECT s.material_id, m.description, round(s.similarity, 3) AS wl_similarity
FROM find_similar_materials_wl_kernel('AW-996', 5, bom_table := 'aw_bom') s
JOIN materials m USING (material_id)
ORDER BY s.similarity DESC, s.material_id;
-- Expected:
--   AW-995 | ML Bottom Bracket | 1.0     (identical: bearing AW-3 + HL Shell)
--   AW-994 | LL Bottom Bracket | 0.714   (same bearing, LL Shell instead)
--   AW-3   | BB Ball Bearing   | 0.667   (the bearing sub-assembly itself)
--   AW-401 | HL Hub            | 0.143
--   AW-749 | Road-150 Red, 62  | 0.08
