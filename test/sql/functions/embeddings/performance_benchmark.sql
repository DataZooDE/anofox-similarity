-- Phase 2D Performance Benchmark
-- Measures incremental update speed, batch processing efficiency, and dirty tracking overhead
--
-- Benchmark Scenarios:
-- 1. Dirty tracking overhead
-- 2. Batch processing with various batch sizes
-- 3. Incremental refresh vs full recomputation (estimation)
-- 4. Material throughput rates

-- ============================================================================
-- Setup: Create benchmark test data
-- ============================================================================

-- Create a realistic goods_movements dataset
CREATE TEMP TABLE benchmark_goods_movements AS
SELECT * FROM (
	WITH RECURSIVE material_numbers AS (
		SELECT 'MAT-' || LPAD(CAST(n AS VARCHAR), 5, '0') AS material_id, n
		FROM generate_series(1, 100) AS t(n)
	),
	date_series AS (
		SELECT m.material_id,
		       DATE '2023-01-01' + ((d-1) % 30) * INTERVAL '1 day' AS movement_date,
		       100.0 + (m.n * d) % 50 AS quantity,
		       CASE WHEN (m.n + d) % 3 = 0 THEN '262' WHEN (m.n + d) % 5 = 0 THEN '311' ELSE '261' END AS movement_type
		FROM material_numbers m
		CROSS JOIN generate_series(1, 10) AS ds(d)
	)
	SELECT * FROM date_series
) AS t(material_id, movement_date, quantity, movement_type);

-- ============================================================================
-- Benchmark 1: Dirty Material Tracking Overhead
-- ============================================================================
-- Measure: Time to insert dirty tracking records
-- Expected: <1ms per insert, O(1) operation

SELECT 'BENCHMARK 1: Dirty Material Tracking Overhead' AS benchmark;

-- Insert 100 dirty material records (simulating data change notifications)
INSERT INTO material_embeddings_dirty (material_id, reason)
SELECT material_id, 'benchmark_test'
FROM (
	SELECT DISTINCT 'MAT-' || LPAD(CAST(n AS VARCHAR), 5, '0') AS material_id
	FROM generate_series(1, 100) AS t(n)
);

-- Verify dirty materials are tracked
SELECT COUNT(*) AS dirty_materials_tracked
FROM material_embeddings_dirty
WHERE reason = 'benchmark_test';
-- Expected: 100

-- ============================================================================
-- Benchmark 2: Batch Processing - LIMIT/OFFSET Performance
-- ============================================================================
-- Measure: Time to filter materials using batch parameters
-- Expected: <10ms for batch extraction

SELECT 'BENCHMARK 2: Batch Processing - LIMIT/OFFSET Performance' AS benchmark;

-- Batch 1: Extract first 25 materials
SELECT COUNT(*) AS batch_1_material_count
FROM (
	SELECT DISTINCT material_id FROM benchmark_goods_movements
	ORDER BY material_id
	LIMIT 25
	OFFSET 0
);
-- Expected: 25

-- Batch 2: Extract next 25 materials
SELECT COUNT(*) AS batch_2_material_count
FROM (
	SELECT DISTINCT material_id FROM benchmark_goods_movements
	ORDER BY material_id
	LIMIT 25
	OFFSET 25
);
-- Expected: 25

-- Batch 3: Extract final 50 materials
SELECT COUNT(*) AS batch_3_material_count
FROM (
	SELECT DISTINCT material_id FROM benchmark_goods_movements
	ORDER BY material_id
	LIMIT 50
	OFFSET 50
);
-- Expected: 50

-- ============================================================================
-- Benchmark 3: Dirty Materials Filtering
-- ============================================================================
-- Measure: Time to identify and isolate dirty materials
-- Expected: <5ms for 100-10K dirty materials

SELECT 'BENCHMARK 3: Dirty Materials Filtering' AS benchmark;

-- Get count of dirty materials
SELECT COUNT(*) AS dirty_material_count
FROM material_embeddings_dirty;

-- Filter to specific dirty subset
SELECT COUNT(*) AS filtered_dirty_count
FROM material_embeddings_dirty
WHERE material_id LIKE 'MAT-0001%'
   OR material_id LIKE 'MAT-0002%'
   OR material_id LIKE 'MAT-0003%'
   OR material_id LIKE 'MAT-0004%'
   OR material_id LIKE 'MAT-0005%';

-- ============================================================================
-- Benchmark 4: INNER JOIN Performance (Batch Filtering)
-- ============================================================================
-- Measure: Time for batch filtering via INNER JOIN
-- This simulates what compute_transactional_embeddings does

SELECT 'BENCHMARK 4: INNER JOIN Performance (Batch Filtering)' AS benchmark;

-- Create a temporary batch set
CREATE TEMP TABLE batch_materials AS
SELECT DISTINCT material_id FROM benchmark_goods_movements
ORDER BY material_id
LIMIT 50
OFFSET 0;

-- Inner join with goods_movements (like all_materials CTE does)
SELECT COUNT(*) AS filtered_movement_count
FROM benchmark_goods_movements gm
INNER JOIN batch_materials bm ON gm.material_id = bm.material_id;
-- Expected: ~500 (50 materials × 10 movements each)

-- Get per-material movement counts
SELECT gm.material_id, COUNT(*) AS movement_count
FROM benchmark_goods_movements gm
INNER JOIN batch_materials bm ON gm.material_id = bm.material_id
GROUP BY gm.material_id
ORDER BY gm.material_id;

-- ============================================================================
-- Benchmark 5: Dirty Material Update Simulation
-- ============================================================================
-- Measure: Time to update dirty materials
-- Simulates refresh_transactional_embeddings() UPDATE logic

SELECT 'BENCHMARK 5: Dirty Material Update Simulation' AS benchmark;

-- Count dirty materials before update
SELECT COUNT(*) AS pre_update_dirty_count
FROM material_embeddings_dirty;

-- Simulate clearing dirty materials (delete first 50)
DELETE FROM material_embeddings_dirty
WHERE material_id IN (
	SELECT material_id FROM material_embeddings_dirty
	WHERE reason = 'benchmark_test'
	LIMIT 50
);

-- Count dirty materials after partial clear
SELECT COUNT(*) AS post_update_dirty_count
FROM material_embeddings_dirty;

-- ============================================================================
-- Benchmark 6: Statistics Freshness Check Performance
-- ============================================================================
-- Measure: Time to check statistics freshness
-- Expected: <100ms for any dataset size

SELECT 'BENCHMARK 6: Statistics Freshness Check Performance' AS benchmark;

-- Check statistics freshness
SELECT
	stat_count,
	max_samples,
	current_version,
	is_fresh,
	last_updated
FROM check_statistics_freshness();

-- Get statistics coverage
SELECT
	COUNT(*) AS total_statistics,
	COUNT(CASE WHEN feature_index < 30 THEN 1 END) AS core_features,
	COUNT(CASE WHEN feature_index >= 30 AND feature_index < 92 THEN 1 END) AS phase2b_features,
	COUNT(CASE WHEN feature_index >= 92 AND feature_index <= 97 THEN 1 END) AS phase2c_features
FROM transactional_embedding_statistics;

-- ============================================================================
-- Benchmark 7: Batch Size Impact Analysis
-- ============================================================================
-- Measure: How batch size affects query performance
-- Test multiple batch sizes: 10, 25, 50, 100

SELECT 'BENCHMARK 7: Batch Size Impact Analysis' AS benchmark;

-- Small batch (10 materials)
SELECT COUNT(*) AS small_batch_count
FROM (
	SELECT DISTINCT material_id FROM benchmark_goods_movements
	ORDER BY material_id
	LIMIT 10
);

-- Medium batch (25 materials)
SELECT COUNT(*) AS medium_batch_count
FROM (
	SELECT DISTINCT material_id FROM benchmark_goods_movements
	ORDER BY material_id
	LIMIT 25
);

-- Large batch (50 materials)
SELECT COUNT(*) AS large_batch_count
FROM (
	SELECT DISTINCT material_id FROM benchmark_goods_movements
	ORDER BY material_id
	LIMIT 50
);

-- Extra large batch (all materials)
SELECT COUNT(*) AS all_materials_count
FROM (
	SELECT DISTINCT material_id FROM benchmark_goods_movements
	ORDER BY material_id
);

-- ============================================================================
-- Benchmark 8: Offset Skipping Performance
-- ============================================================================
-- Measure: Cost of OFFSET with various offset values

SELECT 'BENCHMARK 8: Offset Skipping Performance' AS benchmark;

-- Offset 0 (no skip)
SELECT COUNT(*) AS offset_0_count
FROM (
	SELECT DISTINCT material_id FROM benchmark_goods_movements
	ORDER BY material_id
	LIMIT 25
	OFFSET 0
);

-- Offset 25
SELECT COUNT(*) AS offset_25_count
FROM (
	SELECT DISTINCT material_id FROM benchmark_goods_movements
	ORDER BY material_id
	LIMIT 25
	OFFSET 25
);

-- Offset 50
SELECT COUNT(*) AS offset_50_count
FROM (
	SELECT DISTINCT material_id FROM benchmark_goods_movements
	ORDER BY material_id
	LIMIT 25
	OFFSET 50
);

-- Offset 75
SELECT COUNT(*) AS offset_75_count
FROM (
	SELECT DISTINCT material_id FROM benchmark_goods_movements
	ORDER BY material_id
	LIMIT 25
	OFFSET 75
);

-- ============================================================================
-- Benchmark 9: Dirty Tracking Per-Material Cost
-- ============================================================================
-- Measure: Average cost per dirty material tracked

SELECT 'BENCHMARK 9: Dirty Tracking Per-Material Cost' AS benchmark;

-- Statistics on dirty materials
SELECT
	COUNT(*) AS total_dirty,
	COUNT(DISTINCT material_id) AS unique_dirty_materials,
	COUNT(*) / NULLIF(COUNT(DISTINCT material_id), 0) AS avg_reasons_per_material,
	MIN(marked_at) AS oldest_dirty,
	MAX(marked_at) AS newest_dirty
FROM material_embeddings_dirty;

-- Distribution of dirty reasons
SELECT
	reason,
	COUNT(*) AS count
FROM material_embeddings_dirty
GROUP BY reason
ORDER BY count DESC;

-- ============================================================================
-- Benchmark 10: Macro Availability Check
-- ============================================================================
-- Measure: Presence of all Phase 2D macros/functions

SELECT 'BENCHMARK 10: Phase 2D Macro Availability' AS benchmark;

-- Verify critical Phase 2D infrastructure exists
SELECT
	(SELECT COUNT(*) FROM duckdb_functions() WHERE function_name = 'compute_transactional_embeddings') AS has_compute_macro,
	(SELECT COUNT(*) FROM duckdb_functions() WHERE function_name = 'check_statistics_freshness') AS has_freshness_macro,
	(SELECT COUNT(*) FROM information_schema.tables WHERE table_name = 'material_embeddings_dirty') AS has_dirty_table,
	(SELECT COUNT(*) FROM information_schema.tables WHERE table_name = 'transactional_embedding_statistics') AS has_statistics_table;

-- ============================================================================
-- Performance Summary
-- ============================================================================

SELECT 'PERFORMANCE SUMMARY' AS benchmark;

SELECT
	'Phase 2D Performance Profile' AS metric,
	'All benchmarks completed' AS status,
	CURRENT_TIMESTAMP AS completion_time;

-- Key Performance Indicators
SELECT 'KPI: Dirty Material Tracking' AS kpi, 'O(1)' AS complexity, '<1ms' AS expected_time;
SELECT 'KPI: Batch LIMIT/OFFSET' AS kpi, 'O(N)' AS complexity, '<10ms for N=100' AS expected_time;
SELECT 'KPI: Statistics Freshness Check' AS kpi, 'O(1)' AS complexity, '<100ms' AS expected_time;
SELECT 'KPI: Material Throughput' AS kpi, 'O(N)' AS complexity, '~1000 materials/sec' AS expected_time;
SELECT 'KPI: Incremental Refresh' AS kpi, 'O(M)' AS complexity, '<5 seconds for M=100' AS expected_time;

-- ============================================================================
-- Cleanup
-- ============================================================================

-- Clear remaining benchmark dirty materials
DELETE FROM material_embeddings_dirty WHERE reason = 'benchmark_test';

SELECT COUNT(*) AS remaining_dirty
FROM material_embeddings_dirty;
-- Expected: 0 (if cleaned up properly)
