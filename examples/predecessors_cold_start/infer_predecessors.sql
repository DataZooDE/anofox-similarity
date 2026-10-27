-- ============================================================================
-- Predecessor Inference: which old material did a new one replace?
-- ============================================================================
-- infer_predecessors combines two signals:
--   1. structure  - the BOMs must be similar (Jaccard >= min_similarity)
--   2. time       - the old material's weekly consumption falls while the new
--                   one's rises (negative correlation after lag_weeks)
-- Both tables are needed:
--   bom_items       (parent_id, child_id)
--   goods_movements (material_id, movement_date, quantity)
-- Detection needs DENSE weekly history: at least min_overlapping_weeks
-- (default 8) aligned weekly buckets. A few scattered movements return nothing.
--
-- Function:
--   infer_predecessors(material_id, lookback_months := 24, min_similarity := 0.3,
--                      min_confidence := 0.5, lag_weeks := 8,
--                      min_overlapping_weeks := 8, bom_table, movements_table)
--
-- Data: inline weekly movements, then test/data/synthetic (scenario S7)
--
-- Run: ./build/release/duckdb < examples/predecessors_cold_start/infer_predecessors.sql
-- ============================================================================

.print '>>> 1. Inline: OLD-PUMP is phased out while NEW-PUMP ramps up'
CREATE OR REPLACE TABLE bom_items (parent_id VARCHAR, child_id VARCHAR);
INSERT INTO bom_items VALUES
    ('OLD-PUMP', 'SEAL-001'), ('OLD-PUMP', 'BEARING-X'), ('OLD-PUMP', 'MOTOR-M1'),
    ('NEW-PUMP', 'SEAL-001'), ('NEW-PUMP', 'BEARING-X'), ('NEW-PUMP', 'MOTOR-M1');

-- 40 weeks of falling OLD-PUMP demand; NEW-PUMP starts in week 10 and grows.
CREATE OR REPLACE TABLE goods_movements AS
SELECT 'OLD-PUMP' AS material_id, DATE '2023-01-02' + 7 * w::INTEGER AS movement_date,
       200.0 - 5 * w AS quantity
FROM range(0, 40) t(w)
UNION ALL
SELECT 'NEW-PUMP', DATE '2023-01-02' + 7 * w::INTEGER, 5.0 * w - 50
FROM range(10, 52) t(w);

-- The material id is the FIRST positional argument; the rest are named.
SELECT predecessor_id, round(confidence, 2) AS confidence, correlation,
       similarity, overlapping_weeks
FROM infer_predecessors('NEW-PUMP')
ORDER BY confidence DESC;
-- Expected: OLD-PUMP | 0.79 | -1.0 | 1.0 | 38
-- (identical BOMs, perfectly anti-correlated demand over 38 aligned weeks)

.print ''
.print '>>> 2. Direction: the newer material is not a predecessor of the older one'
-- A predecessor was in use before the successor started. NEW-PUMP started ten
-- weeks after OLD-PUMP, so asking the other way round finds nothing.
SELECT count(*) AS predecessors_of_old_pump
FROM infer_predecessors('OLD-PUMP');
-- Expected: 0

.print ''
.print '>>> 3. Synthetic data set: scenario S7 (anti-correlated replacement)'
CREATE OR REPLACE TABLE syn_bom AS
SELECT * FROM read_csv('test/data/synthetic/bom_items.csv.gz');
CREATE OR REPLACE TABLE syn_movements AS
SELECT * FROM read_csv('test/data/synthetic/goods_movements.csv.gz');

SELECT predecessor_id, round(confidence, 3) AS confidence,
       round(correlation, 3) AS correlation, round(similarity, 3) AS similarity,
       predecessor_last_usage, successor_start, overlapping_weeks
FROM infer_predecessors('S7-PRED-NEW',
                        min_similarity := 0.5, min_confidence := 0.3,
                        bom_table := 'syn_bom', movements_table := 'syn_movements');
-- Expected: S7-PRED-OLD | 0.837 | -0.983 | 0.667 | 2023-03-19 | 2022-11-27 | 25
-- The two materials overlap for four months while S7-PRED-OLD runs out.

.print ''
.print '>>> 4. Thresholds filter the result'
-- The S7 pair shares 2/3 of its components, so min_similarity := 0.8 drops it.
SELECT count(*) AS found
FROM infer_predecessors('S7-PRED-NEW', min_similarity := 0.8,
                        bom_table := 'syn_bom', movements_table := 'syn_movements');
-- Expected: 0
