-- ============================================================================
-- Cold-Start Analogs: borrow history for a material that has none
-- ============================================================================
-- A new material has no consumption history to forecast from. cold_start_analogs
-- ranks existing materials by BOM similarity (Jaccard) and keeps only those with
-- enough history (min_history_months), so their demand curves can stand in for
-- the new one.
--
-- Function:
--   cold_start_analogs(material_id, k, min_history_months := 0,
--                      min_similarity := 0.0, bom_table, movements_table)
--   material_id and k are positional.
--
-- Data: inline BOM + weekly movements, then test/data/synthetic (product families)
--
-- Run: ./build/release/duckdb < examples/predecessors_cold_start/cold_start_analogs.sql
-- ============================================================================

.print '>>> 1. Inline: a new pump without any movements'
CREATE OR REPLACE TABLE bom_items (parent_id VARCHAR, child_id VARCHAR);
INSERT INTO bom_items VALUES
    ('NEW-PUMP', 'SEAL-001'), ('NEW-PUMP', 'BEARING-X'), ('NEW-PUMP', 'MOTOR-M2'),
    ('PUMP-A',   'SEAL-001'), ('PUMP-A',   'BEARING-X'), ('PUMP-A',   'MOTOR-M1'),
    ('PUMP-B',   'SEAL-001'), ('PUMP-B',   'BEARING-X'), ('PUMP-B',   'MOTOR-M2'),
    ('PUMP-B',   'GASKET-Y'),
    ('FAN-C',    'MOTOR-M2'), ('FAN-C',    'BLADE-B1');

-- PUMP-A: 2 years of weekly history. PUMP-B: only 3 months. FAN-C: 1 year.
CREATE OR REPLACE TABLE goods_movements AS
SELECT 'PUMP-A' AS material_id, DATE '2023-01-02' + 7 * w::INTEGER AS movement_date,
       100.0 + w AS quantity
FROM range(0, 104) t(w)
UNION ALL
SELECT 'PUMP-B', DATE '2024-10-07' + 7 * w::INTEGER, 40.0 FROM range(0, 13) t(w)
UNION ALL
SELECT 'FAN-C',  DATE '2024-01-01' + 7 * w::INTEGER, 25.0 FROM range(0, 52) t(w);

-- All candidates with any history:
SELECT material_id, round(similarity, 3) AS similarity, shared_components,
       history_months, first_usage, last_usage
FROM cold_start_analogs('NEW-PUMP', 5)
ORDER BY similarity DESC, material_id;
-- Expected:
--   PUMP-B | 0.75 | 3 | 2  | 2024-10-07 | 2024-12-30   (closest BOM, little history)
--   PUMP-A | 0.5  | 2 | 23 | 2023-01-02 | 2024-12-23
--   FAN-C  | 0.25 | 1 | 11 | 2024-01-01 | 2024-12-23

.print ''
.print '>>> 2. Require at least 12 months of history to forecast from'
SELECT material_id, round(similarity, 3) AS similarity, history_months
FROM cold_start_analogs('NEW-PUMP', 5, min_history_months := 12);
-- Expected: PUMP-A | 0.5 | 23   (PUMP-B and FAN-C have too little history)

.print ''
.print '>>> 3. Synthetic product families: analogs for FAMILY-00-BASE'
CREATE OR REPLACE TABLE syn_bom AS
SELECT * FROM read_csv('test/data/synthetic/bom_items.csv.gz');
CREATE OR REPLACE TABLE syn_movements AS
SELECT * FROM read_csv('test/data/synthetic/goods_movements.csv.gz');

-- min_similarity removes unrelated materials that only have long histories.
SELECT material_id, round(similarity, 3) AS similarity, shared_components,
       total_components, history_months, first_usage
FROM cold_start_analogs('FAMILY-00-BASE', 5,
                        min_history_months := 6, min_similarity := 0.3,
                        bom_table := 'syn_bom', movements_table := 'syn_movements');
-- Expected:
--   FAMILY-00-VAR-1 | 0.818 | 9 | 11 | 17 | 2022-07-01
--   FAMILY-00-VAR-2 | 0.538 | 7 | 13 | 23 | 2022-01-01
-- FAMILY-00-VAR-3 (similarity 0.333) is dropped: only 5 months of history.
