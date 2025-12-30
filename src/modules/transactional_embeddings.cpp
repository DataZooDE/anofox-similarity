#include "modules/transactional_embeddings.hpp"
#include "core/error_handling.hpp"

namespace duckdb {
namespace anofox {

//------------------------------------------------------------------------------
// Soft Dependency Checking
//------------------------------------------------------------------------------

void RegisterCheckAnofoxForecastMacro(Connection &conn) {
	auto result = conn.Query(R"(
		CREATE OR REPLACE MACRO check_anofox_forecast_available() AS (
			SELECT COUNT(*) > 0
			FROM duckdb_functions()
			WHERE function_name = 'anofox_fcst_ts_features'
		)
	)");

	CheckQueryResult(result, "create check_anofox_forecast_available macro");
}

//------------------------------------------------------------------------------
// Transactional Embeddings Macro
//
// Generates 128-D embeddings from 30 key time series features extracted via
// anofox-forecast, with remaining 98 dimensions zero-padded for future expansion.
//
// Feature Categories:
//   1. Consumption Patterns (dims 1-10): mean, median, variance, trend, etc.
//   2. Demand Volatility (dims 11-18): CV, skewness, entropy, etc.
//   3. Frequency Domain (dims 19-26): FFT coefficients and aggregates
//   4. Temporal Patterns (dims 27-30): Autocorrelation at various lags
//   5. Reserved/Zero-Padding (dims 31-128): Future expansion
//------------------------------------------------------------------------------

void RegisterTransactionalEmbeddingMacro(Connection &conn) {
	auto result = conn.Query(R"(
		CREATE OR REPLACE MACRO compute_transactional_embeddings(
			movements_table := 'goods_movements',
			material_column := 'material_id',
			date_column := 'movement_date',
			quantity_column := 'quantity',
			time_window_days := 365
		) AS TABLE

		WITH dependency_check AS (
			-- Verify anofox-forecast is loaded
			SELECT check_anofox_forecast_available() AS is_available
		),
		validated AS (
			-- Validate dependency before proceeding
			SELECT
				CASE
					WHEN NOT is_available THEN
						ERROR('anofox-forecast extension not loaded. ' ||
							  'Please install and load anofox-forecast: ' ||
							  'INSTALL anofox_forecast FROM community; ' ||
							  'LOAD anofox_forecast;')
					ELSE TRUE
				END AS valid
			FROM dependency_check
		),
		material_timeseries AS (
			-- Aggregate movements per material into time series arrays
			SELECT
				material_id,
				LIST(quantity ORDER BY movement_date) AS values_array,
				LIST(movement_date ORDER BY movement_date) AS dates_array,
				COUNT(*) AS num_observations
			FROM query_table(movements_table)
			WHERE movement_date >= CAST(CURRENT_TIMESTAMP AS DATE) - INTERVAL '1 day' * time_window_days
				AND quantity IS NOT NULL
				AND quantity > 0
			GROUP BY material_id
			HAVING COUNT(*) >= 3  -- Minimum requirement for meaningful statistics
		),
		feature_extraction AS (
			-- Extract time series features using anofox-forecast
			-- This produces a STRUCT with 76+ named feature columns
			SELECT
				mt.material_id,
				mt.num_observations,
				anofox_fcst_ts_features(
					UNNEST(mt.values_array),
					UNNEST(mt.dates_array)
				) AS features
			FROM material_timeseries mt
			WHERE (SELECT valid FROM validated LIMIT 1)  -- Only execute if dependency validated
		),
		statistics AS (
			-- Load pre-computed statistics for z-score normalization
			-- Falls back to defaults if statistics table is empty
			SELECT
				feature_name,
				COALESCE(mean_value, 0.0) AS mean_value,
				COALESCE(std_value, 1.0) AS std_value
			FROM transactional_embedding_statistics
			WHERE feature_index < 30  -- Only for the 30 core features (Phase 2A)
		),
		normalized_features AS (
			-- Apply z-score normalization: (x - μ) / σ
			-- Phase 2A: Normalize the 30 core features for better similarity search
			SELECT
				fe.material_id,
				fe.num_observations,
				-- Consumption Patterns (dims 0-9) - z-score normalized
				(COALESCE(fe.features.mean, 0.0) - COALESCE(s_mean.mean_value, 0.0)) / NULLIF(COALESCE(s_mean.std_value, 1.0), 0.0) AS mean_z,
				(COALESCE(fe.features.median, 0.0) - COALESCE(s_median.mean_value, 0.0)) / NULLIF(COALESCE(s_median.std_value, 1.0), 0.0) AS median_z,
				(COALESCE(fe.features.variance, 0.0) - COALESCE(s_variance.mean_value, 0.0)) / NULLIF(COALESCE(s_variance.std_value, 1.0), 0.0) AS variance_z,
				(COALESCE(fe.features.standard_deviation, 0.0) - COALESCE(s_std.mean_value, 0.0)) / NULLIF(COALESCE(s_std.std_value, 1.0), 0.0) AS std_z,
				(COALESCE(fe.features.linear_trend__slope, 0.0) - COALESCE(s_slope.mean_value, 0.0)) / NULLIF(COALESCE(s_slope.std_value, 1.0), 0.0) AS slope_z,
				(COALESCE(fe.features.linear_trend__intercept, 0.0) - COALESCE(s_intercept.mean_value, 0.0)) / NULLIF(COALESCE(s_intercept.std_value, 1.0), 0.0) AS intercept_z,
				(COALESCE(fe.features.sum_values, 0.0) - COALESCE(s_sum.mean_value, 0.0)) / NULLIF(COALESCE(s_sum.std_value, 1.0), 0.0) AS sum_z,
				(COALESCE(fe.features.length, 0.0) - COALESCE(s_length.mean_value, 0.0)) / NULLIF(COALESCE(s_length.std_value, 1.0), 0.0) AS length_z,
				(COALESCE(fe.features.first_location_of_maximum, 0.0) - COALESCE(s_first_max.mean_value, 0.0)) / NULLIF(COALESCE(s_first_max.std_value, 1.0), 0.0) AS first_max_z,
				(COALESCE(fe.features.last_location_of_maximum, 0.0) - COALESCE(s_last_max.mean_value, 0.0)) / NULLIF(COALESCE(s_last_max.std_value, 1.0), 0.0) AS last_max_z,
				-- Demand Volatility (dims 10-17) - z-score normalized
				(COALESCE(fe.features.coefficient_variation, 0.0) - COALESCE(s_cv.mean_value, 0.0)) / NULLIF(COALESCE(s_cv.std_value, 1.0), 0.0) AS cv_z,
				(COALESCE(fe.features.skewness, 0.0) - COALESCE(s_skew.mean_value, 0.0)) / NULLIF(COALESCE(s_skew.std_value, 1.0), 0.0) AS skew_z,
				(COALESCE(fe.features.kurtosis, 0.0) - COALESCE(s_kurt.mean_value, 0.0)) / NULLIF(COALESCE(s_kurt.std_value, 1.0), 0.0) AS kurt_z,
				(COALESCE(fe.features.range_count, 0.0) - COALESCE(s_range.mean_value, 0.0)) / NULLIF(COALESCE(s_range.std_value, 1.0), 0.0) AS range_z,
				(COALESCE(fe.features.ratio_beyond_r_sigma__r_1, 0.0) - COALESCE(s_ratio.mean_value, 0.0)) / NULLIF(COALESCE(s_ratio.std_value, 1.0), 0.0) AS ratio_z,
				(COALESCE(fe.features.approximate_entropy, 0.0) - COALESCE(s_approx_ent.mean_value, 0.0)) / NULLIF(COALESCE(s_approx_ent.std_value, 1.0), 0.0) AS approx_ent_z,
				(COALESCE(fe.features.sample_entropy, 0.0) - COALESCE(s_sample_ent.mean_value, 0.0)) / NULLIF(COALESCE(s_sample_ent.std_value, 1.0), 0.0) AS sample_ent_z,
				(COALESCE(fe.features.benford_correlation, 0.0) - COALESCE(s_benford.mean_value, 0.0)) / NULLIF(COALESCE(s_benford.std_value, 1.0), 0.0) AS benford_z,
				-- Frequency Domain (dims 18-25) - z-score normalized
				(COALESCE(fe.features.fft_coefficient__attr_real__coeff_0, 0.0) - COALESCE(s_fft_r0.mean_value, 0.0)) / NULLIF(COALESCE(s_fft_r0.std_value, 1.0), 0.0) AS fft_r0_z,
				(COALESCE(fe.features.fft_coefficient__attr_imag__coeff_0, 0.0) - COALESCE(s_fft_i0.mean_value, 0.0)) / NULLIF(COALESCE(s_fft_i0.std_value, 1.0), 0.0) AS fft_i0_z,
				(COALESCE(fe.features.fft_coefficient__attr_real__coeff_1, 0.0) - COALESCE(s_fft_r1.mean_value, 0.0)) / NULLIF(COALESCE(s_fft_r1.std_value, 1.0), 0.0) AS fft_r1_z,
				(COALESCE(fe.features.fft_coefficient__attr_imag__coeff_1, 0.0) - COALESCE(s_fft_i1.mean_value, 0.0)) / NULLIF(COALESCE(s_fft_i1.std_value, 1.0), 0.0) AS fft_i1_z,
				(COALESCE(fe.features.fft_coefficient__attr_real__coeff_2, 0.0) - COALESCE(s_fft_r2.mean_value, 0.0)) / NULLIF(COALESCE(s_fft_r2.std_value, 1.0), 0.0) AS fft_r2_z,
				(COALESCE(fe.features.fft_coefficient__attr_imag__coeff_2, 0.0) - COALESCE(s_fft_i2.mean_value, 0.0)) / NULLIF(COALESCE(s_fft_i2.std_value, 1.0), 0.0) AS fft_i2_z,
				(COALESCE(fe.features.fft_aggregated__aggtype_mean, 0.0) - COALESCE(s_fft_mean.mean_value, 0.0)) / NULLIF(COALESCE(s_fft_mean.std_value, 1.0), 0.0) AS fft_mean_z,
				(COALESCE(fe.features.fft_aggregated__aggtype_variance, 0.0) - COALESCE(s_fft_var.mean_value, 0.0)) / NULLIF(COALESCE(s_fft_var.std_value, 1.0), 0.0) AS fft_var_z,
				-- Temporal Patterns (dims 26-29) - z-score normalized
				(COALESCE(fe.features.autocorrelation__lag_1, 0.0) - COALESCE(s_ac1.mean_value, 0.0)) / NULLIF(COALESCE(s_ac1.std_value, 1.0), 0.0) AS ac1_z,
				(COALESCE(fe.features.autocorrelation__lag_7, 0.0) - COALESCE(s_ac7.mean_value, 0.0)) / NULLIF(COALESCE(s_ac7.std_value, 1.0), 0.0) AS ac7_z,
				(COALESCE(fe.features.autocorrelation__lag_14, 0.0) - COALESCE(s_ac14.mean_value, 0.0)) / NULLIF(COALESCE(s_ac14.std_value, 1.0), 0.0) AS ac14_z,
				(COALESCE(fe.features.autocorrelation__lag_28, 0.0) - COALESCE(s_ac28.mean_value, 0.0)) / NULLIF(COALESCE(s_ac28.std_value, 1.0), 0.0) AS ac28_z
			FROM feature_extraction fe
			LEFT JOIN statistics s_mean ON s_mean.feature_name = 'mean'
			LEFT JOIN statistics s_median ON s_median.feature_name = 'median'
			LEFT JOIN statistics s_variance ON s_variance.feature_name = 'variance'
			LEFT JOIN statistics s_std ON s_std.feature_name = 'standard_deviation'
			LEFT JOIN statistics s_slope ON s_slope.feature_name = 'linear_trend__slope'
			LEFT JOIN statistics s_intercept ON s_intercept.feature_name = 'linear_trend__intercept'
			LEFT JOIN statistics s_sum ON s_sum.feature_name = 'sum_values'
			LEFT JOIN statistics s_length ON s_length.feature_name = 'length'
			LEFT JOIN statistics s_first_max ON s_first_max.feature_name = 'first_location_of_maximum'
			LEFT JOIN statistics s_last_max ON s_last_max.feature_name = 'last_location_of_maximum'
			LEFT JOIN statistics s_cv ON s_cv.feature_name = 'coefficient_variation'
			LEFT JOIN statistics s_skew ON s_skew.feature_name = 'skewness'
			LEFT JOIN statistics s_kurt ON s_kurt.feature_name = 'kurtosis'
			LEFT JOIN statistics s_range ON s_range.feature_name = 'range_count'
			LEFT JOIN statistics s_ratio ON s_ratio.feature_name = 'ratio_beyond_r_sigma__r_1'
			LEFT JOIN statistics s_approx_ent ON s_approx_ent.feature_name = 'approximate_entropy'
			LEFT JOIN statistics s_sample_ent ON s_sample_ent.feature_name = 'sample_entropy'
			LEFT JOIN statistics s_benford ON s_benford.feature_name = 'benford_correlation'
			LEFT JOIN statistics s_fft_r0 ON s_fft_r0.feature_name = 'fft_coefficient__attr_real__coeff_0'
			LEFT JOIN statistics s_fft_i0 ON s_fft_i0.feature_name = 'fft_coefficient__attr_imag__coeff_0'
			LEFT JOIN statistics s_fft_r1 ON s_fft_r1.feature_name = 'fft_coefficient__attr_real__coeff_1'
			LEFT JOIN statistics s_fft_i1 ON s_fft_i1.feature_name = 'fft_coefficient__attr_imag__coeff_1'
			LEFT JOIN statistics s_fft_r2 ON s_fft_r2.feature_name = 'fft_coefficient__attr_real__coeff_2'
			LEFT JOIN statistics s_fft_i2 ON s_fft_i2.feature_name = 'fft_coefficient__attr_imag__coeff_2'
			LEFT JOIN statistics s_fft_mean ON s_fft_mean.feature_name = 'fft_aggregated__aggtype_mean'
			LEFT JOIN statistics s_fft_var ON s_fft_var.feature_name = 'fft_aggregated__aggtype_variance'
			LEFT JOIN statistics s_ac1 ON s_ac1.feature_name = 'autocorrelation__lag_1'
			LEFT JOIN statistics s_ac7 ON s_ac7.feature_name = 'autocorrelation__lag_7'
			LEFT JOIN statistics s_ac14 ON s_ac14.feature_name = 'autocorrelation__lag_14'
			LEFT JOIN statistics s_ac28 ON s_ac28.feature_name = 'autocorrelation__lag_28'
		),
		raw_embedding_vectors AS (
			-- Convert z-score normalized features to FLOAT[128] array
			-- Phase 2A: Use z-score normalized features (dims 0-29) + zero-padding (dims 30-127)
			SELECT
				material_id,
				ARRAY[
					-- Consumption Patterns (10 features: dims 0-9) - z-score normalized
					COALESCE(mean_z, 0.0),
					COALESCE(median_z, 0.0),
					COALESCE(variance_z, 0.0),
					COALESCE(std_z, 0.0),
					COALESCE(slope_z, 0.0),
					COALESCE(intercept_z, 0.0),
					COALESCE(sum_z, 0.0),
					COALESCE(length_z, 0.0),
					COALESCE(first_max_z, 0.0),
					COALESCE(last_max_z, 0.0),

					-- Demand Volatility (8 features: dims 10-17) - z-score normalized
					COALESCE(cv_z, 0.0),
					COALESCE(skew_z, 0.0),
					COALESCE(kurt_z, 0.0),
					COALESCE(range_z, 0.0),
					COALESCE(ratio_z, 0.0),
					COALESCE(approx_ent_z, 0.0),
					COALESCE(sample_ent_z, 0.0),
					COALESCE(benford_z, 0.0),

					-- Frequency Domain (8 features: dims 18-25) - z-score normalized
					COALESCE(fft_r0_z, 0.0),
					COALESCE(fft_i0_z, 0.0),
					COALESCE(fft_r1_z, 0.0),
					COALESCE(fft_i1_z, 0.0),
					COALESCE(fft_r2_z, 0.0),
					COALESCE(fft_i2_z, 0.0),
					COALESCE(fft_mean_z, 0.0),
					COALESCE(fft_var_z, 0.0),

					-- Temporal Patterns (4 features: dims 26-29) - z-score normalized
					COALESCE(ac1_z, 0.0),
					COALESCE(ac7_z, 0.0),
					COALESCE(ac14_z, 0.0),
					COALESCE(ac28_z, 0.0),

					-- Reserved / Zero-Padding (98 features: dims 30-127)
					-- Pre-allocated space for future feature expansion
					-- Phase 2B will populate with additional features
					0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0,
					0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0,
					0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0,
					0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0,
					0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0,
					0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0,
					0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0,
					0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0,
					0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0,
					0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0
				]::FLOAT[128] AS raw_embedding
			FROM normalized_features
		),
		l2_normalization AS (
			-- Compute L2 norm of raw embedding
			-- ||v||_2 = sqrt(sum(v_i^2))
			SELECT
				material_id,
				raw_embedding,
				SQRT(SUM(v * v)) AS l2_norm
			FROM raw_embedding_vectors
			CROSS JOIN LATERAL UNNEST(raw_embedding) AS t(v)
			GROUP BY material_id, raw_embedding
		),
		normalized_embedding_vectors AS (
			-- Apply L2 normalization: v_normalized = v / ||v||_2
			-- Converts embedding to unit vector (||v||_2 = 1.0)
			-- Important for cosine similarity in HNSW indexing
			SELECT
				material_id,
				LIST(CASE
					WHEN l2_norm > 1e-8 THEN v / l2_norm
					ELSE v
				END ORDER BY idx)::FLOAT[128] AS transactional_embedding
			FROM l2_normalization
			CROSS JOIN LATERAL UNNEST(raw_embedding) WITH ORDINALITY AS t(v, idx)
			GROUP BY material_id, l2_norm
		)
		SELECT
			material_id,
			transactional_embedding
		FROM normalized_embedding_vectors
	)");

	CheckQueryResult(result, "create compute_transactional_embeddings macro");
}

} // namespace anofox
} // namespace duckdb
