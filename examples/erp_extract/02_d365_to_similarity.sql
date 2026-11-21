-- ============================================================================
-- From a Dynamics 365 extract to similar materials
-- ============================================================================
-- The same pump maker runs Dynamics 365 Supply Chain Management for two legal
-- entities (usmf, demf). The export holds the tables BOMVersion (which BOM an
-- item uses, from when, approved or not) and BOM (the lines). Which pumps of
-- usmf are built like the new PUMP-65?
--
-- The extract has what a real one has: PUMP-40 with an expired 2019 version and
-- a current 2024 one, PUMP-50 with an unapproved draft next to its approved
-- BOM, a paint line given per 100 pumps (BOMQtySerie), and a PUMP-40 of the
-- other company with its own BOM.
--
-- Functions: dynamics365_to_bom_header, dynamics365_to_bom_component,
--            bom_to_items, find_similar_materials_jaccard
-- Data:      examples/erp_extract/data/bomversion.csv, bom.csv (synthetic)
-- Run:       DATAZOO_DISABLE_TELEMETRY=1 ./build/release/duckdb < examples/erp_extract/02_d365_to_similarity.sql
-- ============================================================================

-- D365 table field names (BOMVersion.ItemId, FromDate, ...). A data-entity
-- (OData) export names them differently: map it with a view (docs/api/05).
CREATE OR REPLACE TABLE BOMVersion AS SELECT * FROM read_csv('examples/erp_extract/data/bomversion.csv');
CREATE OR REPLACE TABLE BOM AS SELECT * FROM read_csv('examples/erp_extract/data/bom.csv');

.print '>>> 1. One BOM per company and item on the reference date (default 9999-12-31: the newest)'
CREATE OR REPLACE TABLE bom_header AS SELECT * FROM dynamics365_to_bom_header(bom_version := 'BOMVersion');
CREATE OR REPLACE TABLE bom_component AS
SELECT * FROM dynamics365_to_bom_component(bom_lines := 'BOM', bom_version := 'BOMVersion');
SELECT company, parent_material_id, source_bom_id, is_approved, valid_from
FROM bom_header ORDER BY company, parent_material_id;
-- Expected:
--   company | parent_material_id | source_bom_id | is_approved | valid_from
--   demf    | PUMP-40            | B-40-DE       | true        | 2019-03-12
--   usmf    | PUMP-40            | B-40-2024     | true        | 2024-01-01   (B-40-2019 ended 2023-12-31)
--   usmf    | PUMP-50            | B-50          | true        | 2020-06-05   (the newer draft is neither active nor approved)
--   usmf    | PUMP-65            | B-65          | true        | 2025-10-20

.print ''
.print '>>> 2. Quantities per one pump'
SELECT child_material_id, quantity_per, quantity_uom FROM bom_component
WHERE bom_id = 'usmf/PUMP-65/B-65' ORDER BY child_material_id;
-- Expected:
--   CAS-SS        | 1.000000 | ea
--   IMP-200       | 1.000000 | ea
--   MOT-2K2       | 1.000000 | ea
--   PAINT-RAL5015 | 2.500000 | g     (250 g per series of 100)
--   SEAL-28B      | 1.000000 | ea

.print ''
.print '>>> 3. Flatten one company to bom_items and search'
-- bom_to_items has no company column: filter first, or PUMP-40 of usmf and
-- demf become one parent with the children of both.
CREATE OR REPLACE VIEW bom_header_usmf AS SELECT * FROM bom_header WHERE company = 'usmf';
CREATE OR REPLACE TABLE bom_items AS SELECT * FROM bom_to_items('bom_header_usmf', 'bom_component');
SELECT (SELECT count(*) FROM bom_items) AS edges,
       (SELECT count(*) FROM bom_to_items('bom_header', 'bom_component')) AS edges_both_companies;
-- Expected:
--   edges | edges_both_companies
--   13    | 17
SELECT material_id, round(similarity, 4) AS similarity, shared_components, total_components
FROM find_similar_materials_jaccard('PUMP-65', 5);
-- Expected:
--   material_id | similarity | shared_components | total_components
--   PUMP-50     | 0.5        | 3                 | 6
--   PUMP-40     | 0.125      | 1                 | 8
-- PUMP-50 shares seal, impeller and motor; the expired 2019 BOM of PUMP-40 (with
-- its bearings) and the draft of PUMP-50 (with a stainless casing) play no part.

.print ''
.print '>>> 4. The same question on an earlier date'
SELECT company, parent_material_id, source_bom_id
FROM dynamics365_to_bom_header(bom_version := 'BOMVersion', reference_date := '2022-06-30')
WHERE company = 'usmf' ORDER BY parent_material_id;
-- Expected:
--   usmf | PUMP-40 | B-40-2019
--   usmf | PUMP-50 | B-50
-- PUMP-65 did not exist yet. Pass the same reference_date to
-- dynamics365_to_bom_component, so header and lines agree.
