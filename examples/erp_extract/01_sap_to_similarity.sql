-- ============================================================================
-- From a SAP extract to similar materials and cold-start analogs
-- ============================================================================
-- A pump maker exports MARA, MAKT, MAST, STKO, STPO, STAS and MSEG from SAP
-- (as CSV, one file per table, all columns as text). The new pump KP-65
-- (000000000000100300) has no sales history yet: which existing pumps are
-- built like it, and whose history can stand in for its forecast?
--
-- The extract carries what a real one carries: two clients (100 production,
-- 200 a test copy), leading zeros in every material number, a document BOM
-- item, a BOM item deleted in 2023, a pump with two alternatives (01 grey
-- iron, 02 stainless) whose items only STAS tells apart, a deleted material
-- (lvorm), German-only texts, and goods receipts next to deliveries in MSEG.
--
-- Functions: sap_to_bom_items, sap_to_materials_with_desc,
--            find_similar_materials_jaccard, cold_start_analogs
-- Data:      examples/erp_extract/data (synthetic, invented parts)
-- Run:       DATAZOO_DISABLE_TELEMETRY=1 ./build/release/duckdb < examples/erp_extract/01_sap_to_similarity.sql
-- ============================================================================

-- Load as text: material numbers keep their leading zeros, dates stay YYYYMMDD
-- (the functions read that form). Only the quantities become numbers.
CREATE OR REPLACE TABLE mara AS SELECT * FROM read_csv('examples/erp_extract/data/mara.csv', all_varchar = true);
CREATE OR REPLACE TABLE makt AS SELECT * FROM read_csv('examples/erp_extract/data/makt.csv', all_varchar = true);
CREATE OR REPLACE TABLE mast AS SELECT * FROM read_csv('examples/erp_extract/data/mast.csv', all_varchar = true);
CREATE OR REPLACE TABLE stko AS SELECT * FROM read_csv('examples/erp_extract/data/stko.csv', all_varchar = true);
CREATE OR REPLACE TABLE stpo AS
SELECT * REPLACE (menge::DECIMAL(13, 3) AS menge) FROM read_csv('examples/erp_extract/data/stpo.csv', all_varchar = true);
CREATE OR REPLACE TABLE stas AS SELECT * FROM read_csv('examples/erp_extract/data/stas.csv', all_varchar = true);
CREATE OR REPLACE TABLE mseg AS
SELECT * REPLACE (menge::DECIMAL(13, 3) AS menge) FROM read_csv('examples/erp_extract/data/mseg.csv', all_varchar = true);

.print '>>> 1. BOM edges: production usage, STAS item selection, one client'
-- sap_to_bom_items keeps the clients apart (mandt in every table) and returns
-- them in `client`; the searches do not see it, so keep one client.
CREATE OR REPLACE TABLE bom_items AS
SELECT parent_id, child_id, qty AS quantity, unit
FROM sap_to_bom_items('mast', 'stko', 'stpo', stas_table := 'stas', bom_usage := '1')
WHERE client = '100';
SELECT (SELECT count(*) FROM bom_items) AS edges,
       (SELECT count(*) FROM sap_to_bom_items('mast', 'stko', 'stpo', bom_usage := '1') WHERE client = '100') AS edges_without_stas,
       (SELECT count(*) FROM sap_to_bom_items('mast', 'stko', 'stpo', stas_table := 'stas', bom_usage := '1',
                                               reference_date := '2022-06-30') WHERE client = '100') AS edges_in_2022,
       (SELECT sum(qty) FROM sap_to_bom_items('mast', 'stko', 'stpo', stas_table := 'stas', bom_usage := '1',
                                               reference_date := '2022-06-30') WHERE client = '100') AS pieces_in_2022;
-- Expected:
--   edges | edges_without_stas | edges_in_2022 | pieces_in_2022
--   12    | 13                 | 9             | 10.000
-- On 2022-06-30 KP-40 still has its two bearings (the item was deleted in 2023)
-- and KP-65 (from 2025) does not exist yet.
-- Without STAS the stainless casing of alternative 02 leaks into KP-50's
-- alternative 01. The drawing (document BOM item DRW-100100) never appears.

.print ''
.print '>>> 2. Materials with descriptions (client 100; lvorm X is dropped)'
-- The client column (mandt) keeps clients apart: KP-40 exists in 100 and 200.
SELECT client, count(*) AS materials FROM sap_to_materials_with_desc('mara', 'makt') GROUP BY client ORDER BY client;
-- Expected:
--   100 | 11
--   200 | 1
CREATE OR REPLACE TABLE materials AS
SELECT * FROM sap_to_materials_with_desc('mara', 'makt', language := 'E') WHERE client = '100';
SELECT material_id, material_type, description, created_date FROM materials
WHERE material_type = 'FERT' ORDER BY material_id;
-- Expected:
--   000000000000100100 | FERT | Centrifugal pump KP-40 | 2019-03-12
--   000000000000100200 | FERT | Centrifugal pump KP-50 | 2020-06-05
--   000000000000100300 | FERT |                        | 2025-10-20   (only a German text: language := 'D' has it)
SELECT count(*) AS materials FROM materials;
-- Expected: 11

.print ''
.print '>>> 3. Pumps built like the new KP-65'
SELECT material_id, round(similarity, 4) AS similarity, shared_components, total_components
FROM find_similar_materials_jaccard('000000000000100300', 5);
-- Expected:
--   material_id        | similarity | shared_components | total_components
--   000000000000100200 | 0.6        | 3                 | 5     (seal, impeller D200, motor 2.2 kW)
--   000000000000100100 | 0.1429     | 1                 | 7     (seal only)
-- With the edges read without STAS, KP-50 scores 0.8: its stainless alternative
-- would look like KP-65's casing.

.print ''
.print '>>> 4. Consumption from MSEG: deliveries and issues, reversals netted'
-- 601 delivery and 261 issue post as H (stock out): +; their reversals 602/262
-- as S: -. The goods receipt 101 and client 200 are left out.
CREATE OR REPLACE VIEW goods_movements AS
SELECT matnr AS material_id,
       strptime(budat_mkpf, '%Y%m%d')::DATE AS movement_date,
       CASE shkzg WHEN 'H' THEN menge ELSE -menge END AS quantity
FROM mseg
WHERE mandt = '100' AND bwart IN ('201', '202', '261', '262', '601', '602');
SELECT material_id, count(*) AS movements, sum(quantity) AS net_quantity
FROM goods_movements GROUP BY ALL ORDER BY material_id;
-- Expected:
--   000000000000100100 | 5 | 45.000   (12 + 10 + 14 - 2 + 11)
--   000000000000100200 | 4 | 30.000
--   000000000000200010 | 2 | 22.000   (25 issued, 3 reversed)

.print ''
.print '>>> 5. Whose history can stand in for KP-65'
SELECT material_id, round(similarity, 4) AS similarity, history_months, first_usage, last_usage
FROM cold_start_analogs('000000000000100300', 3);
-- Expected:
--   000000000000100200 | 0.6    | 3 | 2025-01-20 | 2025-04-14
--   000000000000100100 | 0.1429 | 3 | 2025-01-13 | 2025-04-07
-- KP-50 is the analog: built like KP-65, with three months of deliveries.
