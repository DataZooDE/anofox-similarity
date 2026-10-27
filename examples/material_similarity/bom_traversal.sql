-- ============================================================================
-- BOM Traversal: explode, where-used and common components
-- ============================================================================
-- The traversal macros read the universal two-table BOM shape that the
-- SAP/Dynamics transforms emit:
--   bom_header    (bom_id, source_system, parent_material_id)
--   bom_component (bom_id, child_material_id, quantity_per)
-- All arguments are NAMED (query_material_id := ..., child_material_id := ...).
--
-- Functions:
--   bom_explosion_1level(query_material_id := ...)                direct children
--   bom_explosion_multilevel(query_material_id := ..., max_depth) all levels
--   bom_where_used(child_material_id := ...)                      direct parents
--   bom_common_components(material_1 := ..., material_2 := ...)   shared children
--
-- Data: AdventureWorks bicycle BOM (test/data/adventureworks/bom_items.csv.gz),
--       reshaped from the flat parent/child edge list into header + component.
--
-- Run: ./build/release/duckdb < examples/material_similarity/bom_traversal.sql
-- ============================================================================

.print '>>> 0. Reshape the AdventureWorks edge list into bom_header + bom_component'
CREATE OR REPLACE TABLE aw_edges AS
SELECT * FROM read_csv('test/data/adventureworks/bom_items.csv.gz');

CREATE OR REPLACE TABLE materials AS
SELECT * FROM read_csv('test/data/adventureworks/materials.csv.gz');

-- One header per BOM; bom_id already identifies the parent.
CREATE OR REPLACE TABLE bom_header AS
SELECT DISTINCT bom_id, 'AdventureWorks' AS source_system, parent_id AS parent_material_id
FROM aw_edges;

-- Some bikes list the same component twice (two positions); add them up.
CREATE OR REPLACE TABLE bom_component AS
SELECT bom_id, child_id AS child_material_id, sum(quantity) AS quantity_per
FROM aw_edges
GROUP BY bom_id, child_id;

SELECT (SELECT count(*) FROM bom_header) AS boms,
       (SELECT count(*) FROM bom_component) AS components;
-- Expected: 238 BOMs, 2387 component rows (2576 edges, 189 repeated positions merged)

.print ''
.print '>>> 1. Single-level explosion of the HL Bottom Bracket (AW-996)'
SELECT e.child_material_id, m.description, e.quantity_per
FROM bom_explosion_1level(query_material_id := 'AW-996') e
JOIN materials m ON m.material_id = e.child_material_id
ORDER BY e.child_material_id;
-- Expected:
--   AW-3   | BB Ball Bearing | 10.0
--   AW-526 | HL Shell        | 1.0

.print ''
.print '>>> 2. Multi-level explosion: the bottom bracket down to raw material'
-- AW-3 (the ball bearing) is itself an assembly, so a second level appears.
SELECT e.parent_material_id, e.child_material_id, m.description, e.quantity_per
FROM bom_explosion_multilevel(query_material_id := 'AW-996', max_depth := 5) e
JOIN materials m ON m.material_id = e.child_material_id
ORDER BY e.parent_material_id DESC, e.child_material_id;
-- Expected: 6 edges, AW-996 -> {AW-3, AW-526} and AW-3 -> {AW-2, AW-461, AW-504, AW-505}

.print ''
.print '>>> 3. Where-used: which assemblies contain the BB Ball Bearing (AW-3)?'
SELECT w.parent_material_id, m.description, w.quantity_per
FROM bom_where_used(child_material_id := 'AW-3') w
JOIN materials m ON m.material_id = w.parent_material_id
ORDER BY w.parent_material_id;
-- Expected: the three bottom brackets AW-994 (LL), AW-995 (ML), AW-996 (HL), 10 each

.print ''
.print '>>> 4. Common components of two road bikes'
-- Road-150 Red 62 (AW-749) against Road-250 Red 44 (AW-789).
SELECT count(*) AS common_components
FROM bom_common_components(material_1 := 'AW-749', material_2 := 'AW-789');
-- Expected: 11 (the same number find_similar_materials_jaccard reports as shared)
