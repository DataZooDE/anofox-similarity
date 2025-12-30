#include "modules/vss_integration.hpp"
#include "core/error_handling.hpp"
#include "duckdb/main/connection.hpp"

namespace duckdb {
namespace anofox {

void InitializeVSSIntegration(Connection &conn) {
	auto vss_result = conn.Query("INSTALL vss; LOAD vss;");
	CheckQueryResult(vss_result, "load VSS extension");

	// HNSW persistence is experimental - continue if unavailable
	vss_result = conn.Query("SET hnsw_enable_experimental_persistence = true;");
	CheckQueryResult(vss_result, "enable HNSW persistence", FailureMode::OPTIONAL);
}

void CreateEmbeddingTables(Connection &conn) {
	// Create main embeddings table with Phase 3 schema (structural + textual + transactional)
	auto schema_result = conn.Query(R"(
		CREATE TABLE IF NOT EXISTS material_embeddings (
			material_id VARCHAR PRIMARY KEY,
			jaccard_embedding FLOAT[128],
			structural_embedding FLOAT[256],
			textual_embedding FLOAT[384],
			transactional_embedding FLOAT[128],
			combined_embedding FLOAT[768],
			updated_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
			num_components INTEGER
		)
	)");
	CheckQueryResult(schema_result, "create material_embeddings table");

	// Create statistics table for transactional embedding normalization (Phase 2A)
	auto stats_result = conn.Query(R"(
		CREATE TABLE IF NOT EXISTS transactional_embedding_statistics (
			feature_name VARCHAR PRIMARY KEY,
			feature_index INTEGER,
			feature_category VARCHAR,
			mean_value DOUBLE,
			std_value DOUBLE,
			min_value DOUBLE,
			max_value DOUBLE,
			num_samples INTEGER,
			updated_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
			version INTEGER DEFAULT 1
		)
	)");
	CheckQueryResult(stats_result, "create transactional_embedding_statistics table");

	// Create feature mapping table for transactional embeddings (Phase 2A)
	auto mapping_result = conn.Query(R"(
		CREATE TABLE IF NOT EXISTS transactional_feature_mapping (
			feature_index INTEGER PRIMARY KEY,
			feature_name VARCHAR,
			category VARCHAR,
			description VARCHAR,
			is_advanced BOOLEAN DEFAULT FALSE,
			created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
		)
	)");
	CheckQueryResult(mapping_result, "create transactional_feature_mapping table");

	auto tracking_result = conn.Query(R"(
		CREATE TABLE IF NOT EXISTS material_embeddings_dirty (
			material_id VARCHAR PRIMARY KEY,
			reason VARCHAR,
			marked_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
		)
	)");
	CheckQueryResult(tracking_result, "create material_embeddings_dirty table");

	// Schema migration: Handle old schema with wl_embedding
	// Check if old column exists and migrate
	auto migration_check = conn.Query(R"(
		SELECT COUNT(*) FROM information_schema.columns
		WHERE table_name = 'material_embeddings' AND column_name = 'wl_embedding'
	)");

	if (migration_check->GetValue(0, 0).GetValue<int64_t>() > 0) {
		// Old schema exists - copy data from wl_embedding to structural_embedding
		auto migration_result = conn.Query(R"(
			UPDATE material_embeddings
			SET structural_embedding = wl_embedding
			WHERE wl_embedding IS NOT NULL AND structural_embedding IS NULL
		)");
		CheckQueryResult(migration_result, "migrate wl_embedding to structural_embedding", FailureMode::OPTIONAL);

		// Drop old columns if migration succeeded
		auto drop_result = conn.Query(R"(
			ALTER TABLE material_embeddings
			DROP COLUMN IF EXISTS wl_embedding
		)");
		CheckQueryResult(drop_result, "drop old wl_embedding column", FailureMode::OPTIONAL);
	}
}

void CreateHNSWIndexes(Connection &conn) {
	// HNSW indexes are performance optimizations - continue if creation fails

	// Jaccard embedding index (128-D, L2 distance)
	auto idx_result = conn.Query(R"(
		CREATE INDEX IF NOT EXISTS jaccard_idx ON material_embeddings
		USING HNSW(jaccard_embedding) WITH (metric = 'l2sq')
	)");
	CheckQueryResult(idx_result, "create jaccard HNSW index", FailureMode::OPTIONAL);

	// Structural embedding index (256-D, L2-squared distance)
	idx_result = conn.Query(R"(
		CREATE INDEX IF NOT EXISTS hnsw_structural_idx ON material_embeddings
		USING HNSW(structural_embedding) WITH (metric = 'l2sq')
	)");
	CheckQueryResult(idx_result, "create structural HNSW index", FailureMode::OPTIONAL);

	// Textual embedding index (384-D, cosine distance)
	idx_result = conn.Query(R"(
		CREATE INDEX IF NOT EXISTS hnsw_textual_idx ON material_embeddings
		USING HNSW(textual_embedding) WITH (metric = 'cosine')
	)");
	CheckQueryResult(idx_result, "create textual HNSW index", FailureMode::OPTIONAL);

	// Combined embedding index (768-D, cosine distance)
	idx_result = conn.Query(R"(
		CREATE INDEX IF NOT EXISTS hnsw_combined_idx ON material_embeddings
		USING HNSW(combined_embedding) WITH (metric = 'cosine')
	)");
	CheckQueryResult(idx_result, "create combined HNSW index", FailureMode::OPTIONAL);

	// Legacy wl_idx for backward compatibility (if old schema exists)
	idx_result = conn.Query(R"(
		CREATE INDEX IF NOT EXISTS wl_idx ON material_embeddings
		USING HNSW(structural_embedding) WITH (metric = 'cosine')
	)");
	CheckQueryResult(idx_result, "create WL legacy HNSW index", FailureMode::OPTIONAL);
}

void RegisterEmbeddingMacros(Connection &conn) {
	auto result = conn.Query(R"(
		CREATE OR REPLACE MACRO compute_jaccard_embeddings(bom_table := 'bom_items') AS TABLE
		WITH
			-- Step 1: Aggregate components per material
			material_components AS (
				SELECT
					parent_id AS material_id,
					COUNT(DISTINCT child_id)::INTEGER AS num_components,
					list(DISTINCT child_id ORDER BY child_id) AS components
				FROM query_table(bom_table)
				GROUP BY parent_id
			),
			-- Step 2: For each material and seed (0-127), compute min-hash value (JACCARD_EMBEDDING_DIM = 128)
			-- Min-hash(A, seed) = MIN(HASH(c || ':' || seed) for c in A)
			-- This preserves Jaccard similarity in expectation
			minhash_by_seed AS (
				SELECT
					mc.material_id,
					t.seed,
					MIN(CAST(ABS(HASH(c || ':' || t.seed::VARCHAR)) AS FLOAT)) AS minhash_value,
					mc.num_components
				FROM material_components mc
				CROSS JOIN generate_series(0, 127) AS t(seed)
				CROSS JOIN UNNEST(mc.components) AS u(c)
				GROUP BY mc.material_id, t.seed, mc.num_components
			)
		SELECT
			material_id,
			seed,
			minhash_value,
			num_components
		FROM minhash_by_seed
		ORDER BY material_id, seed
	)");

	CheckQueryResult(result, "create compute_jaccard_embeddings macro");

	// Phase 2A: Check if transactional embedding statistics are fresh
	result = conn.Query(R"(
		CREATE OR REPLACE MACRO check_statistics_freshness() AS TABLE
		SELECT
			COALESCE(COUNT(*), 0) AS stat_count,
			COALESCE(MAX(num_samples), 0) AS max_samples,
			COALESCE(MAX(version), 0) AS current_version,
			COALESCE(MAX(updated_at), CAST(CURRENT_TIMESTAMP AS TIMESTAMP) - INTERVAL '8 days') AS last_updated,
			(COALESCE(COUNT(*), 0) >= 30  -- At least 30 statistics present
			 AND COALESCE(MAX(updated_at), CAST(CURRENT_TIMESTAMP AS TIMESTAMP) - INTERVAL '8 days') > CAST(CURRENT_TIMESTAMP AS TIMESTAMP) - INTERVAL '7 days') AS is_fresh
		FROM transactional_embedding_statistics
	)");
	CheckQueryResult(result, "create check_statistics_freshness macro");

	// Phase 2A: Recompute transactional embedding statistics from current data
	result = conn.Query(R"(
		CREATE OR REPLACE MACRO recompute_embedding_statistics() AS TABLE
		WITH all_features AS (
			SELECT
				material_id,
				anofox_fcst_ts_features(
					LIST(quantity ORDER BY movement_date),
					LIST(movement_date ORDER BY movement_date)
				) AS features
			FROM goods_movements
			WHERE movement_date >= CAST(CURRENT_TIMESTAMP AS DATE) - INTERVAL '365 days'
			  AND quantity IS NOT NULL
			  AND quantity > 0
			GROUP BY material_id
			HAVING COUNT(*) >= 3
		),
		feature_stats AS (
			SELECT
				'mean' AS feature_name, 0 AS feature_index, 'consumption' AS feature_category,
				AVG(features.mean) AS mean_value, STDDEV_POP(features.mean) AS std_value,
				MIN(features.mean) AS min_value, MAX(features.mean) AS max_value,
				COUNT(*) AS num_samples
			FROM all_features
			UNION ALL
			SELECT 'median', 1, 'consumption', AVG(features.median), STDDEV_POP(features.median),
				MIN(features.median), MAX(features.median), COUNT(*)
			FROM all_features
			UNION ALL
			SELECT 'variance', 2, 'consumption', AVG(features.variance), STDDEV_POP(features.variance),
				MIN(features.variance), MAX(features.variance), COUNT(*)
			FROM all_features
			UNION ALL
			SELECT 'standard_deviation', 3, 'consumption', AVG(features.standard_deviation), STDDEV_POP(features.standard_deviation),
				MIN(features.standard_deviation), MAX(features.standard_deviation), COUNT(*)
			FROM all_features
			UNION ALL
			SELECT 'linear_trend__slope', 4, 'consumption', AVG(features.linear_trend__slope), STDDEV_POP(features.linear_trend__slope),
				MIN(features.linear_trend__slope), MAX(features.linear_trend__slope), COUNT(*)
			FROM all_features
			UNION ALL
			SELECT 'linear_trend__intercept', 5, 'consumption', AVG(features.linear_trend__intercept), STDDEV_POP(features.linear_trend__intercept),
				MIN(features.linear_trend__intercept), MAX(features.linear_trend__intercept), COUNT(*)
			FROM all_features
			UNION ALL
			SELECT 'sum_values', 6, 'consumption', AVG(features.sum_values), STDDEV_POP(features.sum_values),
				MIN(features.sum_values), MAX(features.sum_values), COUNT(*)
			FROM all_features
			UNION ALL
			SELECT 'length', 7, 'consumption', AVG(features.length), STDDEV_POP(features.length),
				MIN(features.length), MAX(features.length), COUNT(*)
			FROM all_features
			UNION ALL
			SELECT 'first_location_of_maximum', 8, 'consumption', AVG(features.first_location_of_maximum), STDDEV_POP(features.first_location_of_maximum),
				MIN(features.first_location_of_maximum), MAX(features.first_location_of_maximum), COUNT(*)
			FROM all_features
			UNION ALL
			SELECT 'last_location_of_maximum', 9, 'consumption', AVG(features.last_location_of_maximum), STDDEV_POP(features.last_location_of_maximum),
				MIN(features.last_location_of_maximum), MAX(features.last_location_of_maximum), COUNT(*)
			FROM all_features
			UNION ALL
			SELECT 'coefficient_variation', 10, 'volatility', AVG(features.coefficient_variation), STDDEV_POP(features.coefficient_variation),
				MIN(features.coefficient_variation), MAX(features.coefficient_variation), COUNT(*)
			FROM all_features
			UNION ALL
			SELECT 'skewness', 11, 'volatility', AVG(features.skewness), STDDEV_POP(features.skewness),
				MIN(features.skewness), MAX(features.skewness), COUNT(*)
			FROM all_features
			UNION ALL
			SELECT 'kurtosis', 12, 'volatility', AVG(features.kurtosis), STDDEV_POP(features.kurtosis),
				MIN(features.kurtosis), MAX(features.kurtosis), COUNT(*)
			FROM all_features
			UNION ALL
			SELECT 'range_count', 13, 'volatility', AVG(features.range_count), STDDEV_POP(features.range_count),
				MIN(features.range_count), MAX(features.range_count), COUNT(*)
			FROM all_features
			UNION ALL
			SELECT 'ratio_beyond_r_sigma__r_1', 14, 'volatility', AVG(features.ratio_beyond_r_sigma__r_1), STDDEV_POP(features.ratio_beyond_r_sigma__r_1),
				MIN(features.ratio_beyond_r_sigma__r_1), MAX(features.ratio_beyond_r_sigma__r_1), COUNT(*)
			FROM all_features
			UNION ALL
			SELECT 'approximate_entropy', 15, 'volatility', AVG(features.approximate_entropy), STDDEV_POP(features.approximate_entropy),
				MIN(features.approximate_entropy), MAX(features.approximate_entropy), COUNT(*)
			FROM all_features
			UNION ALL
			SELECT 'sample_entropy', 16, 'volatility', AVG(features.sample_entropy), STDDEV_POP(features.sample_entropy),
				MIN(features.sample_entropy), MAX(features.sample_entropy), COUNT(*)
			FROM all_features
			UNION ALL
			SELECT 'benford_correlation', 17, 'volatility', AVG(features.benford_correlation), STDDEV_POP(features.benford_correlation),
				MIN(features.benford_correlation), MAX(features.benford_correlation), COUNT(*)
			FROM all_features
			UNION ALL
			SELECT 'fft_coefficient__attr_real__coeff_0', 18, 'frequency', AVG(features.fft_coefficient__attr_real__coeff_0), STDDEV_POP(features.fft_coefficient__attr_real__coeff_0),
				MIN(features.fft_coefficient__attr_real__coeff_0), MAX(features.fft_coefficient__attr_real__coeff_0), COUNT(*)
			FROM all_features
			UNION ALL
			SELECT 'fft_coefficient__attr_imag__coeff_0', 19, 'frequency', AVG(features.fft_coefficient__attr_imag__coeff_0), STDDEV_POP(features.fft_coefficient__attr_imag__coeff_0),
				MIN(features.fft_coefficient__attr_imag__coeff_0), MAX(features.fft_coefficient__attr_imag__coeff_0), COUNT(*)
			FROM all_features
			UNION ALL
			SELECT 'fft_coefficient__attr_real__coeff_1', 20, 'frequency', AVG(features.fft_coefficient__attr_real__coeff_1), STDDEV_POP(features.fft_coefficient__attr_real__coeff_1),
				MIN(features.fft_coefficient__attr_real__coeff_1), MAX(features.fft_coefficient__attr_real__coeff_1), COUNT(*)
			FROM all_features
			UNION ALL
			SELECT 'fft_coefficient__attr_imag__coeff_1', 21, 'frequency', AVG(features.fft_coefficient__attr_imag__coeff_1), STDDEV_POP(features.fft_coefficient__attr_imag__coeff_1),
				MIN(features.fft_coefficient__attr_imag__coeff_1), MAX(features.fft_coefficient__attr_imag__coeff_1), COUNT(*)
			FROM all_features
			UNION ALL
			SELECT 'fft_coefficient__attr_real__coeff_2', 22, 'frequency', AVG(features.fft_coefficient__attr_real__coeff_2), STDDEV_POP(features.fft_coefficient__attr_real__coeff_2),
				MIN(features.fft_coefficient__attr_real__coeff_2), MAX(features.fft_coefficient__attr_real__coeff_2), COUNT(*)
			FROM all_features
			UNION ALL
			SELECT 'fft_coefficient__attr_imag__coeff_2', 23, 'frequency', AVG(features.fft_coefficient__attr_imag__coeff_2), STDDEV_POP(features.fft_coefficient__attr_imag__coeff_2),
				MIN(features.fft_coefficient__attr_imag__coeff_2), MAX(features.fft_coefficient__attr_imag__coeff_2), COUNT(*)
			FROM all_features
			UNION ALL
			SELECT 'fft_aggregated__aggtype_mean', 24, 'frequency', AVG(features.fft_aggregated__aggtype_mean), STDDEV_POP(features.fft_aggregated__aggtype_mean),
				MIN(features.fft_aggregated__aggtype_mean), MAX(features.fft_aggregated__aggtype_mean), COUNT(*)
			FROM all_features
			UNION ALL
			SELECT 'fft_aggregated__aggtype_variance', 25, 'frequency', AVG(features.fft_aggregated__aggtype_variance), STDDEV_POP(features.fft_aggregated__aggtype_variance),
				MIN(features.fft_aggregated__aggtype_variance), MAX(features.fft_aggregated__aggtype_variance), COUNT(*)
			FROM all_features
			UNION ALL
			SELECT 'autocorrelation__lag_1', 26, 'temporal', AVG(features.autocorrelation__lag_1), STDDEV_POP(features.autocorrelation__lag_1),
				MIN(features.autocorrelation__lag_1), MAX(features.autocorrelation__lag_1), COUNT(*)
			FROM all_features
			UNION ALL
			SELECT 'autocorrelation__lag_7', 27, 'temporal', AVG(features.autocorrelation__lag_7), STDDEV_POP(features.autocorrelation__lag_7),
				MIN(features.autocorrelation__lag_7), MAX(features.autocorrelation__lag_7), COUNT(*)
			FROM all_features
			UNION ALL
			SELECT 'autocorrelation__lag_14', 28, 'temporal', AVG(features.autocorrelation__lag_14), STDDEV_POP(features.autocorrelation__lag_14),
				MIN(features.autocorrelation__lag_14), MAX(features.autocorrelation__lag_14), COUNT(*)
			FROM all_features
			UNION ALL
			SELECT 'autocorrelation__lag_28', 29, 'temporal', AVG(features.autocorrelation__lag_28), STDDEV_POP(features.autocorrelation__lag_28),
				MIN(features.autocorrelation__lag_28), MAX(features.autocorrelation__lag_28), COUNT(*)
			FROM all_features
		UNION ALL
		-- Phase 2B: Quantile-based features (5)
		SELECT 'quantile__q_0_1', 30, 'quantile', AVG(features.quantile__q_0_1), STDDEV_POP(features.quantile__q_0_1),
			MIN(features.quantile__q_0_1), MAX(features.quantile__q_0_1), COUNT(*)
		FROM all_features
		UNION ALL
		SELECT 'quantile__q_0_25', 31, 'quantile', AVG(features.quantile__q_0_25), STDDEV_POP(features.quantile__q_0_25),
			MIN(features.quantile__q_0_25), MAX(features.quantile__q_0_25), COUNT(*)
		FROM all_features
		UNION ALL
		SELECT 'quantile__q_0_75', 32, 'quantile', AVG(features.quantile__q_0_75), STDDEV_POP(features.quantile__q_0_75),
			MIN(features.quantile__q_0_75), MAX(features.quantile__q_0_75), COUNT(*)
		FROM all_features
		UNION ALL
		SELECT 'quantile__q_0_9', 33, 'quantile', AVG(features.quantile__q_0_9), STDDEV_POP(features.quantile__q_0_9),
			MIN(features.quantile__q_0_9), MAX(features.quantile__q_0_9), COUNT(*)
		FROM all_features
		UNION ALL
		SELECT 'iqr', 34, 'quantile', AVG(features.iqr), STDDEV_POP(features.iqr),
			MIN(features.iqr), MAX(features.iqr), COUNT(*)
		FROM all_features
		UNION ALL
		-- Phase 2B: Min/Max/Range extensions (5)
		SELECT 'minimum', 35, 'extremes', AVG(features.minimum), STDDEV_POP(features.minimum),
			MIN(features.minimum), MAX(features.minimum), COUNT(*)
		FROM all_features
		UNION ALL
		SELECT 'maximum', 36, 'extremes', AVG(features.maximum), STDDEV_POP(features.maximum),
			MIN(features.maximum), MAX(features.maximum), COUNT(*)
		FROM all_features
		UNION ALL
		SELECT 'absolute_maximum', 37, 'extremes', AVG(features.absolute_maximum), STDDEV_POP(features.absolute_maximum),
			MIN(features.absolute_maximum), MAX(features.absolute_maximum), COUNT(*)
		FROM all_features
		UNION ALL
		SELECT 'range', 38, 'extremes', AVG(features.range), STDDEV_POP(features.range),
			MIN(features.range), MAX(features.range), COUNT(*)
		FROM all_features
		UNION ALL
		SELECT 'absolute_energy', 39, 'extremes', AVG(features.absolute_energy), STDDEV_POP(features.absolute_energy),
			MIN(features.absolute_energy), MAX(features.absolute_energy), COUNT(*)
		FROM all_features
		UNION ALL
		-- Phase 2B: Extended autocorrelation (10)
		SELECT 'autocorrelation__lag_2', 40, 'temporal', AVG(features.autocorrelation__lag_2), STDDEV_POP(features.autocorrelation__lag_2),
			MIN(features.autocorrelation__lag_2), MAX(features.autocorrelation__lag_2), COUNT(*)
		FROM all_features
		UNION ALL
		SELECT 'autocorrelation__lag_3', 41, 'temporal', AVG(features.autocorrelation__lag_3), STDDEV_POP(features.autocorrelation__lag_3),
			MIN(features.autocorrelation__lag_3), MAX(features.autocorrelation__lag_3), COUNT(*)
		FROM all_features
		UNION ALL
		SELECT 'autocorrelation__lag_4', 42, 'temporal', AVG(features.autocorrelation__lag_4), STDDEV_POP(features.autocorrelation__lag_4),
			MIN(features.autocorrelation__lag_4), MAX(features.autocorrelation__lag_4), COUNT(*)
		FROM all_features
		UNION ALL
		SELECT 'autocorrelation__lag_5', 43, 'temporal', AVG(features.autocorrelation__lag_5), STDDEV_POP(features.autocorrelation__lag_5),
			MIN(features.autocorrelation__lag_5), MAX(features.autocorrelation__lag_5), COUNT(*)
		FROM all_features
		UNION ALL
		SELECT 'autocorrelation__lag_6', 44, 'temporal', AVG(features.autocorrelation__lag_6), STDDEV_POP(features.autocorrelation__lag_6),
			MIN(features.autocorrelation__lag_6), MAX(features.autocorrelation__lag_6), COUNT(*)
		FROM all_features
		UNION ALL
		SELECT 'autocorrelation__lag_8', 45, 'temporal', AVG(features.autocorrelation__lag_8), STDDEV_POP(features.autocorrelation__lag_8),
			MIN(features.autocorrelation__lag_8), MAX(features.autocorrelation__lag_8), COUNT(*)
		FROM all_features
		UNION ALL
		SELECT 'autocorrelation__lag_12', 46, 'temporal', AVG(features.autocorrelation__lag_12), STDDEV_POP(features.autocorrelation__lag_12),
			MIN(features.autocorrelation__lag_12), MAX(features.autocorrelation__lag_12), COUNT(*)
		FROM all_features
		UNION ALL
		SELECT 'autocorrelation__lag_24', 47, 'temporal', AVG(features.autocorrelation__lag_24), STDDEV_POP(features.autocorrelation__lag_24),
			MIN(features.autocorrelation__lag_24), MAX(features.autocorrelation__lag_24), COUNT(*)
		FROM all_features
		UNION ALL
		SELECT 'autocorr_strength', 48, 'temporal', AVG(features.autocorr_strength), STDDEV_POP(features.autocorr_strength),
			MIN(features.autocorr_strength), MAX(features.autocorr_strength), COUNT(*)
		FROM all_features
		UNION ALL
		SELECT 'autocorr_tendency', 49, 'temporal', AVG(features.autocorr_tendency), STDDEV_POP(features.autocorr_tendency),
			MIN(features.autocorr_tendency), MAX(features.autocorr_tendency), COUNT(*)
		FROM all_features
		UNION ALL
		-- Phase 2B: Peak and valley detection (8)
		SELECT 'number_peaks', 50, 'peaks', AVG(features.number_peaks), STDDEV_POP(features.number_peaks),
			MIN(features.number_peaks), MAX(features.number_peaks), COUNT(*)
		FROM all_features
		UNION ALL
		SELECT 'number_valleys', 51, 'peaks', AVG(features.number_valleys), STDDEV_POP(features.number_valleys),
			MIN(features.number_valleys), MAX(features.number_valleys), COUNT(*)
		FROM all_features
		UNION ALL
		SELECT 'longest_strike_above_mean', 52, 'peaks', AVG(features.longest_strike_above_mean), STDDEV_POP(features.longest_strike_above_mean),
			MIN(features.longest_strike_above_mean), MAX(features.longest_strike_above_mean), COUNT(*)
		FROM all_features
		UNION ALL
		SELECT 'longest_strike_below_mean', 53, 'peaks', AVG(features.longest_strike_below_mean), STDDEV_POP(features.longest_strike_below_mean),
			MIN(features.longest_strike_below_mean), MAX(features.longest_strike_below_mean), COUNT(*)
		FROM all_features
		UNION ALL
		SELECT 'longest_strike_above_zero', 54, 'peaks', AVG(features.longest_strike_above_zero), STDDEV_POP(features.longest_strike_above_zero),
			MIN(features.longest_strike_above_zero), MAX(features.longest_strike_above_zero), COUNT(*)
		FROM all_features
		UNION ALL
		SELECT 'longest_strike_below_zero', 55, 'peaks', AVG(features.longest_strike_below_zero), STDDEV_POP(features.longest_strike_below_zero),
			MIN(features.longest_strike_below_zero), MAX(features.longest_strike_below_zero), COUNT(*)
		FROM all_features
		UNION ALL
		SELECT 'number_crossing_m', 56, 'peaks', AVG(features.number_crossing_m), STDDEV_POP(features.number_crossing_m),
			MIN(features.number_crossing_m), MAX(features.number_crossing_m), COUNT(*)
		FROM all_features
		UNION ALL
		SELECT 'number_crossing_0', 57, 'peaks', AVG(features.number_crossing_0), STDDEV_POP(features.number_crossing_0),
			MIN(features.number_crossing_0), MAX(features.number_crossing_0), COUNT(*)
		FROM all_features
		UNION ALL
		-- Phase 2B: Entropy and complexity (8)
		SELECT 'permutation_entropy', 58, 'entropy', AVG(features.permutation_entropy), STDDEV_POP(features.permutation_entropy),
			MIN(features.permutation_entropy), MAX(features.permutation_entropy), COUNT(*)
		FROM all_features
		UNION ALL
		SELECT 'spectral_entropy', 59, 'entropy', AVG(features.spectral_entropy), STDDEV_POP(features.spectral_entropy),
			MIN(features.spectral_entropy), MAX(features.spectral_entropy), COUNT(*)
		FROM all_features
		UNION ALL
		SELECT 'shannon_entropy', 60, 'entropy', AVG(features.shannon_entropy), STDDEV_POP(features.shannon_entropy),
			MIN(features.shannon_entropy), MAX(features.shannon_entropy), COUNT(*)
		FROM all_features
		UNION ALL
		SELECT 'approximate_entropy__lag_2', 61, 'entropy', AVG(features.approximate_entropy__lag_2), STDDEV_POP(features.approximate_entropy__lag_2),
			MIN(features.approximate_entropy__lag_2), MAX(features.approximate_entropy__lag_2), COUNT(*)
		FROM all_features
		UNION ALL
		SELECT 'sample_entropy__lag_2', 62, 'entropy', AVG(features.sample_entropy__lag_2), STDDEV_POP(features.sample_entropy__lag_2),
			MIN(features.sample_entropy__lag_2), MAX(features.sample_entropy__lag_2), COUNT(*)
		FROM all_features
		UNION ALL
		SELECT 'complexity_invariant', 63, 'entropy', AVG(features.complexity_invariant), STDDEV_POP(features.complexity_invariant),
			MIN(features.complexity_invariant), MAX(features.complexity_invariant), COUNT(*)
		FROM all_features
		UNION ALL
		SELECT 'hurst_exponent', 64, 'entropy', AVG(features.hurst_exponent), STDDEV_POP(features.hurst_exponent),
			MIN(features.hurst_exponent), MAX(features.hurst_exponent), COUNT(*)
		FROM all_features
		UNION ALL
		SELECT 'detrended_fluctuation_analysis', 65, 'entropy', AVG(features.detrended_fluctuation_analysis), STDDEV_POP(features.detrended_fluctuation_analysis),
			MIN(features.detrended_fluctuation_analysis), MAX(features.detrended_fluctuation_analysis), COUNT(*)
		FROM all_features
		UNION ALL
		-- Phase 2B: FFT extensions (10)
		SELECT 'fft_coefficient__attr_real__coeff_3', 66, 'frequency', AVG(features.fft_coefficient__attr_real__coeff_3), STDDEV_POP(features.fft_coefficient__attr_real__coeff_3),
			MIN(features.fft_coefficient__attr_real__coeff_3), MAX(features.fft_coefficient__attr_real__coeff_3), COUNT(*)
		FROM all_features
		UNION ALL
		SELECT 'fft_coefficient__attr_imag__coeff_3', 67, 'frequency', AVG(features.fft_coefficient__attr_imag__coeff_3), STDDEV_POP(features.fft_coefficient__attr_imag__coeff_3),
			MIN(features.fft_coefficient__attr_imag__coeff_3), MAX(features.fft_coefficient__attr_imag__coeff_3), COUNT(*)
		FROM all_features
		UNION ALL
		SELECT 'fft_coefficient__attr_real__coeff_4', 68, 'frequency', AVG(features.fft_coefficient__attr_real__coeff_4), STDDEV_POP(features.fft_coefficient__attr_real__coeff_4),
			MIN(features.fft_coefficient__attr_real__coeff_4), MAX(features.fft_coefficient__attr_real__coeff_4), COUNT(*)
		FROM all_features
		UNION ALL
		SELECT 'fft_coefficient__attr_imag__coeff_4', 69, 'frequency', AVG(features.fft_coefficient__attr_imag__coeff_4), STDDEV_POP(features.fft_coefficient__attr_imag__coeff_4),
			MIN(features.fft_coefficient__attr_imag__coeff_4), MAX(features.fft_coefficient__attr_imag__coeff_4), COUNT(*)
		FROM all_features
		UNION ALL
		SELECT 'fft_power', 70, 'frequency', AVG(features.fft_power), STDDEV_POP(features.fft_power),
			MIN(features.fft_power), MAX(features.fft_power), COUNT(*)
		FROM all_features
		UNION ALL
		SELECT 'fft_centroid', 71, 'frequency', AVG(features.fft_centroid), STDDEV_POP(features.fft_centroid),
			MIN(features.fft_centroid), MAX(features.fft_centroid), COUNT(*)
		FROM all_features
		UNION ALL
		SELECT 'fft_magnitude_ratio', 72, 'frequency', AVG(features.fft_magnitude_ratio), STDDEV_POP(features.fft_magnitude_ratio),
			MIN(features.fft_magnitude_ratio), MAX(features.fft_magnitude_ratio), COUNT(*)
		FROM all_features
		UNION ALL
		SELECT 'welch_power_spectral_density', 73, 'frequency', AVG(features.welch_power_spectral_density), STDDEV_POP(features.welch_power_spectral_density),
			MIN(features.welch_power_spectral_density), MAX(features.welch_power_spectral_density), COUNT(*)
		FROM all_features
		UNION ALL
		SELECT 'spectral_rolloff', 74, 'frequency', AVG(features.spectral_rolloff), STDDEV_POP(features.spectral_rolloff),
			MIN(features.spectral_rolloff), MAX(features.spectral_rolloff), COUNT(*)
		FROM all_features
		UNION ALL
		SELECT 'spectral_centroid_freq', 75, 'frequency', AVG(features.spectral_centroid_freq), STDDEV_POP(features.spectral_centroid_freq),
			MIN(features.spectral_centroid_freq), MAX(features.spectral_centroid_freq), COUNT(*)
		FROM all_features
		UNION ALL
		-- Phase 2B: Change and trend metrics (8)
		SELECT 'mean_abs_change', 76, 'trend', AVG(features.mean_abs_change), STDDEV_POP(features.mean_abs_change),
			MIN(features.mean_abs_change), MAX(features.mean_abs_change), COUNT(*)
		FROM all_features
		UNION ALL
		SELECT 'mean_second_derivative', 77, 'trend', AVG(features.mean_second_derivative), STDDEV_POP(features.mean_second_derivative),
			MIN(features.mean_second_derivative), MAX(features.mean_second_derivative), COUNT(*)
		FROM all_features
		UNION ALL
		SELECT 'trend_strength', 78, 'trend', AVG(features.trend_strength), STDDEV_POP(features.trend_strength),
			MIN(features.trend_strength), MAX(features.trend_strength), COUNT(*)
		FROM all_features
		UNION ALL
		SELECT 'mean_abs_change_rate', 79, 'trend', AVG(features.mean_abs_change_rate), STDDEV_POP(features.mean_abs_change_rate),
			MIN(features.mean_abs_change_rate), MAX(features.mean_abs_change_rate), COUNT(*)
		FROM all_features
		UNION ALL
		SELECT 'sum_of_absolute_second_derivative', 80, 'trend', AVG(features.sum_of_absolute_second_derivative), STDDEV_POP(features.sum_of_absolute_second_derivative),
			MIN(features.sum_of_absolute_second_derivative), MAX(features.sum_of_absolute_second_derivative), COUNT(*)
		FROM all_features
		UNION ALL
		SELECT 'mean_absolute_second_derivative', 81, 'trend', AVG(features.mean_absolute_second_derivative), STDDEV_POP(features.mean_absolute_second_derivative),
			MIN(features.mean_absolute_second_derivative), MAX(features.mean_absolute_second_derivative), COUNT(*)
		FROM all_features
		UNION ALL
		SELECT 'count_above_mean', 82, 'trend', AVG(features.count_above_mean), STDDEV_POP(features.count_above_mean),
			MIN(features.count_above_mean), MAX(features.count_above_mean), COUNT(*)
		FROM all_features
		UNION ALL
		SELECT 'count_below_mean', 83, 'trend', AVG(features.count_below_mean), STDDEV_POP(features.count_below_mean),
			MIN(features.count_below_mean), MAX(features.count_below_mean), COUNT(*)
		FROM all_features
		UNION ALL
		-- Phase 2B: Statistical extensions (8)
		SELECT 'skewness_lag_1', 84, 'statistics', AVG(features.skewness_lag_1), STDDEV_POP(features.skewness_lag_1),
			MIN(features.skewness_lag_1), MAX(features.skewness_lag_1), COUNT(*)
		FROM all_features
		UNION ALL
		SELECT 'kurtosis_lag_1', 85, 'statistics', AVG(features.kurtosis_lag_1), STDDEV_POP(features.kurtosis_lag_1),
			MIN(features.kurtosis_lag_1), MAX(features.kurtosis_lag_1), COUNT(*)
		FROM all_features
		UNION ALL
		SELECT 'moment_3', 86, 'statistics', AVG(features.moment_3), STDDEV_POP(features.moment_3),
			MIN(features.moment_3), MAX(features.moment_3), COUNT(*)
		FROM all_features
		UNION ALL
		SELECT 'moment_4', 87, 'statistics', AVG(features.moment_4), STDDEV_POP(features.moment_4),
			MIN(features.moment_4), MAX(features.moment_4), COUNT(*)
		FROM all_features
		UNION ALL
		SELECT 'fisher_skewness', 88, 'statistics', AVG(features.fisher_skewness), STDDEV_POP(features.fisher_skewness),
			MIN(features.fisher_skewness), MAX(features.fisher_skewness), COUNT(*)
		FROM all_features
		UNION ALL
		SELECT 'fisher_kurtosis', 89, 'statistics', AVG(features.fisher_kurtosis), STDDEV_POP(features.fisher_kurtosis),
			MIN(features.fisher_kurtosis), MAX(features.fisher_kurtosis), COUNT(*)
		FROM all_features
		UNION ALL
		SELECT 'raw_moment_3', 90, 'statistics', AVG(features.raw_moment_3), STDDEV_POP(features.raw_moment_3),
			MIN(features.raw_moment_3), MAX(features.raw_moment_3), COUNT(*)
		FROM all_features
		UNION ALL
		SELECT 'raw_moment_4', 91, 'statistics', AVG(features.raw_moment_4), STDDEV_POP(features.raw_moment_4),
			MIN(features.raw_moment_4), MAX(features.raw_moment_4), COUNT(*)
		FROM all_features
		UNION ALL
		-- Reserved for Phase 2C: Advanced features (6 features: indices 92-97)
		SELECT 'reserved_phase2c_92', 92, 'reserved', 0.0, 1.0, 0.0, 0.0, 1
		UNION ALL
		SELECT 'reserved_phase2c_93', 93, 'reserved', 0.0, 1.0, 0.0, 0.0, 1
		UNION ALL
		SELECT 'reserved_phase2c_94', 94, 'reserved', 0.0, 1.0, 0.0, 0.0, 1
		UNION ALL
		SELECT 'reserved_phase2c_95', 95, 'reserved', 0.0, 1.0, 0.0, 0.0, 1
		UNION ALL
		SELECT 'reserved_phase2c_96', 96, 'reserved', 0.0, 1.0, 0.0, 0.0, 1
		UNION ALL
		SELECT 'reserved_phase2c_97', 97, 'reserved', 0.0, 1.0, 0.0, 0.0, 1
		)
		INSERT OR REPLACE INTO transactional_embedding_statistics
		SELECT
			feature_name, feature_index, feature_category, mean_value, std_value,
			min_value, max_value, num_samples, CURRENT_TIMESTAMP,
			COALESCE((SELECT MAX(version) FROM transactional_embedding_statistics), 0) + 1
		FROM feature_stats
		RETURNING feature_name, feature_index, feature_category, mean_value, std_value, min_value, max_value, num_samples;
	)");
	CheckQueryResult(result, "create recompute_embedding_statistics macro", FailureMode::OPTIONAL);
}

void CreateIncrementalUpdateTriggers(Connection &conn) {
	auto result = conn.Query(R"(
		CREATE OR REPLACE TRIGGER IF NOT EXISTS bom_items_insert_trigger
		AFTER INSERT ON bom_items
		FOR EACH ROW
		BEGIN
			INSERT INTO material_embeddings_dirty (material_id, reason)
			VALUES (NEW.parent_id, 'insert')
			ON CONFLICT (material_id) DO UPDATE SET
				marked_at = CURRENT_TIMESTAMP;
		END
	)");
	// Continue even if trigger creation fails (may not be supported in test context)

	result = conn.Query(R"(
		CREATE OR REPLACE TRIGGER IF NOT EXISTS bom_items_update_trigger
		AFTER UPDATE ON bom_items
		FOR EACH ROW
		BEGIN
			INSERT INTO material_embeddings_dirty (material_id, reason)
			VALUES (NEW.parent_id, 'update')
			ON CONFLICT (material_id) DO UPDATE SET
				marked_at = CURRENT_TIMESTAMP;
		END
	)");

	result = conn.Query(R"(
		CREATE OR REPLACE TRIGGER IF NOT EXISTS bom_items_delete_trigger
		AFTER DELETE ON bom_items
		FOR EACH ROW
		BEGIN
			INSERT INTO material_embeddings_dirty (material_id, reason)
			VALUES (OLD.parent_id, 'delete')
			ON CONFLICT (material_id) DO UPDATE SET
				marked_at = CURRENT_TIMESTAMP;
		END
	)");

	result = conn.Query(R"(
		CREATE OR REPLACE MACRO refresh_dirty_embeddings(bom_table := 'bom_items') AS TABLE
		WITH
			dirty_materials AS (
				SELECT material_id FROM material_embeddings_dirty
			),
			material_components AS (
				SELECT
					parent_id AS material_id,
					LIST(DISTINCT child_id ORDER BY child_id) AS components
				FROM query_table(bom_table)
				WHERE parent_id IN (SELECT material_id FROM dirty_materials)
				GROUP BY parent_id
			)
		DELETE FROM material_embeddings_dirty
		WHERE material_id IN (SELECT material_id FROM material_components)
		RETURNING material_id
	)");
	// Triggers and refresh macros are optional - continue if they fail
	CheckQueryResult(result, "create refresh_dirty_embeddings macro", FailureMode::OPTIONAL);
}

} // namespace anofox
} // namespace duckdb
