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
			time_window_days := 365,
			batch_size := NULL,
			batch_offset := 0
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
		all_materials AS (
			-- Phase 2D: Batch processing support
			-- Get distinct materials (optionally filtered by batch size and offset)
			SELECT DISTINCT material_id
			FROM query_table(movements_table)
			WHERE movement_date >= CAST(CURRENT_TIMESTAMP AS DATE) - INTERVAL '1 day' * time_window_days
				AND quantity IS NOT NULL
				AND quantity > 0
			ORDER BY material_id
			LIMIT CASE WHEN batch_size IS NOT NULL THEN batch_size ELSE NULL END
			OFFSET CASE WHEN batch_size IS NOT NULL THEN batch_offset ELSE 0 END
		),
		material_timeseries AS (
			-- Aggregate movements per material into time series arrays
			SELECT
				gm.material_id,
				LIST(gm.quantity ORDER BY gm.movement_date) AS values_array,
				LIST(gm.movement_date ORDER BY gm.movement_date) AS dates_array,
				COUNT(*) AS num_observations
			FROM query_table(movements_table) gm
			INNER JOIN all_materials am ON gm.material_id = am.material_id
			WHERE gm.movement_date >= CAST(CURRENT_TIMESTAMP AS DATE) - INTERVAL '1 day' * time_window_days
				AND gm.quantity IS NOT NULL
				AND gm.quantity > 0
			GROUP BY gm.material_id
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
		statistics_lookup AS (
			-- Create efficient feature lookup table (single scan of statistics)
			-- Instead of 98 LEFT JOINs, load all statistics once and reference by feature_name
			SELECT
				feature_name,
				COALESCE(mean_value, 0.0) AS mean_value,
				COALESCE(std_value, 1.0) AS std_value
			FROM transactional_embedding_statistics
			WHERE feature_index < 104  -- All features including Phase 2C (if available)
		),
		phase2c_features AS (
			-- Phase 2C: Compute domain-specific features from goods_movements
			-- These are optional and only computed if goods_movements table exists
			-- Phase 2D: Apply batch filtering via all_materials
			SELECT
				gm.material_id,
				COUNT(*) AS total_moves,
				COUNT(*) FILTER (WHERE movement_type = '261')::FLOAT / NULLIF(COUNT(*), 0) AS receipt_ratio,
				COUNT(*) FILTER (WHERE movement_type = '262')::FLOAT / NULLIF(COUNT(*), 0) AS reversal_ratio,
				COUNT(*) FILTER (WHERE EXTRACT(DOW FROM movement_date) IN (0, 6))::FLOAT / NULLIF(COUNT(*), 0) AS weekend_ratio,
				CASE
					WHEN COUNT(*) > 0 THEN
						1.0 - (
							CASE WHEN COUNT(*) FILTER (WHERE EXTRACT(DOW FROM movement_date) = 0) > 0 THEN
								-(COUNT(*) FILTER (WHERE EXTRACT(DOW FROM movement_date) = 0)::FLOAT/COUNT(*)) * LN(COUNT(*) FILTER (WHERE EXTRACT(DOW FROM movement_date) = 0)::FLOAT/COUNT(*))
								ELSE 0.0 END +
							CASE WHEN COUNT(*) FILTER (WHERE EXTRACT(DOW FROM movement_date) = 1) > 0 THEN
								-(COUNT(*) FILTER (WHERE EXTRACT(DOW FROM movement_date) = 1)::FLOAT/COUNT(*)) * LN(COUNT(*) FILTER (WHERE EXTRACT(DOW FROM movement_date) = 1)::FLOAT/COUNT(*))
								ELSE 0.0 END +
							CASE WHEN COUNT(*) FILTER (WHERE EXTRACT(DOW FROM movement_date) = 2) > 0 THEN
								-(COUNT(*) FILTER (WHERE EXTRACT(DOW FROM movement_date) = 2)::FLOAT/COUNT(*)) * LN(COUNT(*) FILTER (WHERE EXTRACT(DOW FROM movement_date) = 2)::FLOAT/COUNT(*))
								ELSE 0.0 END +
							CASE WHEN COUNT(*) FILTER (WHERE EXTRACT(DOW FROM movement_date) = 3) > 0 THEN
								-(COUNT(*) FILTER (WHERE EXTRACT(DOW FROM movement_date) = 3)::FLOAT/COUNT(*)) * LN(COUNT(*) FILTER (WHERE EXTRACT(DOW FROM movement_date) = 3)::FLOAT/COUNT(*))
								ELSE 0.0 END +
							CASE WHEN COUNT(*) FILTER (WHERE EXTRACT(DOW FROM movement_date) = 4) > 0 THEN
								-(COUNT(*) FILTER (WHERE EXTRACT(DOW FROM movement_date) = 4)::FLOAT/COUNT(*)) * LN(COUNT(*) FILTER (WHERE EXTRACT(DOW FROM movement_date) = 4)::FLOAT/COUNT(*))
								ELSE 0.0 END +
							CASE WHEN COUNT(*) FILTER (WHERE EXTRACT(DOW FROM movement_date) = 5) > 0 THEN
								-(COUNT(*) FILTER (WHERE EXTRACT(DOW FROM movement_date) = 5)::FLOAT/COUNT(*)) * LN(COUNT(*) FILTER (WHERE EXTRACT(DOW FROM movement_date) = 5)::FLOAT/COUNT(*))
								ELSE 0.0 END +
							CASE WHEN COUNT(*) FILTER (WHERE EXTRACT(DOW FROM movement_date) = 6) > 0 THEN
								-(COUNT(*) FILTER (WHERE EXTRACT(DOW FROM movement_date) = 6)::FLOAT/COUNT(*)) * LN(COUNT(*) FILTER (WHERE EXTRACT(DOW FROM movement_date) = 6)::FLOAT/COUNT(*))
								ELSE 0.0 END
						) / LN(7.0)
					ELSE 0.0
				END AS weekday_concentration,
				-- Lifecycle features from anofox-forecast (computed later via LEFT JOIN)
				NULL::FLOAT AS trend_strength,
				NULL::FLOAT AS growth_indicator
			FROM query_table(movements_table) gm
			INNER JOIN all_materials am ON gm.material_id = am.material_id
			WHERE gm.movement_date >= CAST(CURRENT_TIMESTAMP AS DATE) - INTERVAL '1 day' * time_window_days
			  AND gm.quantity IS NOT NULL
			  AND gm.quantity > 0
			GROUP BY gm.material_id
			HAVING COUNT(*) >= 3
		),
		normalized_features AS (
			-- Apply z-score normalization: (x - μ) / σ
			-- Phase 2A: Normalize the 30 core features for better similarity search
			-- Refactored: Replace 98 individual LEFT JOINs with single efficient lookup
			-- Uses COALESCE with subqueries for feature-specific statistic lookups
			SELECT
				fe.material_id,
				fe.num_observations,
				-- Consumption Patterns (dims 0-9) - z-score normalized
				(COALESCE(fe.features.mean, 0.0) - (SELECT COALESCE(mean_value, 0.0) FROM statistics_lookup WHERE feature_name = 'mean' LIMIT 1)) / NULLIF((SELECT COALESCE(std_value, 1.0) FROM statistics_lookup WHERE feature_name = 'mean' LIMIT 1), 0.0) AS mean_z,
				(COALESCE(fe.features.median, 0.0) - (SELECT COALESCE(mean_value, 0.0) FROM statistics_lookup WHERE feature_name = 'median' LIMIT 1)) / NULLIF((SELECT COALESCE(std_value, 1.0) FROM statistics_lookup WHERE feature_name = 'median' LIMIT 1), 0.0) AS median_z,
				(COALESCE(fe.features.variance, 0.0) - (SELECT COALESCE(mean_value, 0.0) FROM statistics_lookup WHERE feature_name = 'variance' LIMIT 1)) / NULLIF((SELECT COALESCE(std_value, 1.0) FROM statistics_lookup WHERE feature_name = 'variance' LIMIT 1), 0.0) AS variance_z,
				(COALESCE(fe.features.standard_deviation, 0.0) - (SELECT COALESCE(mean_value, 0.0) FROM statistics_lookup WHERE feature_name = 'standard_deviation' LIMIT 1)) / NULLIF((SELECT COALESCE(std_value, 1.0) FROM statistics_lookup WHERE feature_name = 'standard_deviation' LIMIT 1), 0.0) AS std_z,
				(COALESCE(fe.features.linear_trend__slope, 0.0) - (SELECT COALESCE(mean_value, 0.0) FROM statistics_lookup WHERE feature_name = 'linear_trend__slope' LIMIT 1)) / NULLIF((SELECT COALESCE(std_value, 1.0) FROM statistics_lookup WHERE feature_name = 'linear_trend__slope' LIMIT 1), 0.0) AS slope_z,
				(COALESCE(fe.features.linear_trend__intercept, 0.0) - (SELECT COALESCE(mean_value, 0.0) FROM statistics_lookup WHERE feature_name = 'linear_trend__intercept' LIMIT 1)) / NULLIF((SELECT COALESCE(std_value, 1.0) FROM statistics_lookup WHERE feature_name = 'linear_trend__intercept' LIMIT 1), 0.0) AS intercept_z,
				(COALESCE(fe.features.sum_values, 0.0) - (SELECT COALESCE(mean_value, 0.0) FROM statistics_lookup WHERE feature_name = 'sum_values' LIMIT 1)) / NULLIF((SELECT COALESCE(std_value, 1.0) FROM statistics_lookup WHERE feature_name = 'sum_values' LIMIT 1), 0.0) AS sum_z,
				(COALESCE(fe.features.length, 0.0) - (SELECT COALESCE(mean_value, 0.0) FROM statistics_lookup WHERE feature_name = 'length' LIMIT 1)) / NULLIF((SELECT COALESCE(std_value, 1.0) FROM statistics_lookup WHERE feature_name = 'length' LIMIT 1), 0.0) AS length_z,
				(COALESCE(fe.features.first_location_of_maximum, 0.0) - (SELECT COALESCE(mean_value, 0.0) FROM statistics_lookup WHERE feature_name = 'first_location_of_maximum' LIMIT 1)) / NULLIF((SELECT COALESCE(std_value, 1.0) FROM statistics_lookup WHERE feature_name = 'first_location_of_maximum' LIMIT 1), 0.0) AS first_max_z,
				(COALESCE(fe.features.last_location_of_maximum, 0.0) - (SELECT COALESCE(mean_value, 0.0) FROM statistics_lookup WHERE feature_name = 'last_location_of_maximum' LIMIT 1)) / NULLIF((SELECT COALESCE(std_value, 1.0) FROM statistics_lookup WHERE feature_name = 'last_location_of_maximum' LIMIT 1), 0.0) AS last_max_z,
				-- Demand Volatility (dims 10-17) - z-score normalized
				(COALESCE(fe.features.coefficient_variation, 0.0) - (SELECT COALESCE(mean_value, 0.0) FROM statistics_lookup WHERE feature_name = 'coefficient_variation' LIMIT 1)) / NULLIF((SELECT COALESCE(std_value, 1.0) FROM statistics_lookup WHERE feature_name = 'coefficient_variation' LIMIT 1), 0.0) AS cv_z,
				(COALESCE(fe.features.skewness, 0.0) - (SELECT COALESCE(mean_value, 0.0) FROM statistics_lookup WHERE feature_name = 'skewness' LIMIT 1)) / NULLIF((SELECT COALESCE(std_value, 1.0) FROM statistics_lookup WHERE feature_name = 'skewness' LIMIT 1), 0.0) AS skew_z,
				(COALESCE(fe.features.kurtosis, 0.0) - (SELECT COALESCE(mean_value, 0.0) FROM statistics_lookup WHERE feature_name = 'kurtosis' LIMIT 1)) / NULLIF((SELECT COALESCE(std_value, 1.0) FROM statistics_lookup WHERE feature_name = 'kurtosis' LIMIT 1), 0.0) AS kurt_z,
				(COALESCE(fe.features.range_count, 0.0) - (SELECT COALESCE(mean_value, 0.0) FROM statistics_lookup WHERE feature_name = 'range_count' LIMIT 1)) / NULLIF((SELECT COALESCE(std_value, 1.0) FROM statistics_lookup WHERE feature_name = 'range_count' LIMIT 1), 0.0) AS range_z,
				(COALESCE(fe.features.ratio_beyond_r_sigma__r_1, 0.0) - (SELECT COALESCE(mean_value, 0.0) FROM statistics_lookup WHERE feature_name = 'ratio_beyond_r_sigma__r_1' LIMIT 1)) / NULLIF((SELECT COALESCE(std_value, 1.0) FROM statistics_lookup WHERE feature_name = 'ratio_beyond_r_sigma__r_1' LIMIT 1), 0.0) AS ratio_z,
				(COALESCE(fe.features.approximate_entropy, 0.0) - (SELECT COALESCE(mean_value, 0.0) FROM statistics_lookup WHERE feature_name = 'approximate_entropy' LIMIT 1)) / NULLIF((SELECT COALESCE(std_value, 1.0) FROM statistics_lookup WHERE feature_name = 'approximate_entropy' LIMIT 1), 0.0) AS approx_ent_z,
				(COALESCE(fe.features.sample_entropy, 0.0) - (SELECT COALESCE(mean_value, 0.0) FROM statistics_lookup WHERE feature_name = 'sample_entropy' LIMIT 1)) / NULLIF((SELECT COALESCE(std_value, 1.0) FROM statistics_lookup WHERE feature_name = 'sample_entropy' LIMIT 1), 0.0) AS sample_ent_z,
				(COALESCE(fe.features.benford_correlation, 0.0) - (SELECT COALESCE(mean_value, 0.0) FROM statistics_lookup WHERE feature_name = 'benford_correlation' LIMIT 1)) / NULLIF((SELECT COALESCE(std_value, 1.0) FROM statistics_lookup WHERE feature_name = 'benford_correlation' LIMIT 1), 0.0) AS benford_z,
				-- Frequency Domain (dims 18-25) - z-score normalized
				(COALESCE(fe.features.fft_coefficient__attr_real__coeff_0, 0.0) - (SELECT COALESCE(mean_value, 0.0) FROM statistics_lookup WHERE feature_name = 'fft_coefficient__attr_real__coeff_0' LIMIT 1)) / NULLIF((SELECT COALESCE(std_value, 1.0) FROM statistics_lookup WHERE feature_name = 'fft_coefficient__attr_real__coeff_0' LIMIT 1), 0.0) AS fft_r0_z,
				(COALESCE(fe.features.fft_coefficient__attr_imag__coeff_0, 0.0) - (SELECT COALESCE(mean_value, 0.0) FROM statistics_lookup WHERE feature_name = 'fft_coefficient__attr_imag__coeff_0' LIMIT 1)) / NULLIF((SELECT COALESCE(std_value, 1.0) FROM statistics_lookup WHERE feature_name = 'fft_coefficient__attr_imag__coeff_0' LIMIT 1), 0.0) AS fft_i0_z,
				(COALESCE(fe.features.fft_coefficient__attr_real__coeff_1, 0.0) - (SELECT COALESCE(mean_value, 0.0) FROM statistics_lookup WHERE feature_name = 'fft_coefficient__attr_real__coeff_1' LIMIT 1)) / NULLIF((SELECT COALESCE(std_value, 1.0) FROM statistics_lookup WHERE feature_name = 'fft_coefficient__attr_real__coeff_1' LIMIT 1), 0.0) AS fft_r1_z,
				(COALESCE(fe.features.fft_coefficient__attr_imag__coeff_1, 0.0) - (SELECT COALESCE(mean_value, 0.0) FROM statistics_lookup WHERE feature_name = 'fft_coefficient__attr_imag__coeff_1' LIMIT 1)) / NULLIF((SELECT COALESCE(std_value, 1.0) FROM statistics_lookup WHERE feature_name = 'fft_coefficient__attr_imag__coeff_1' LIMIT 1), 0.0) AS fft_i1_z,
				(COALESCE(fe.features.fft_coefficient__attr_real__coeff_2, 0.0) - (SELECT COALESCE(mean_value, 0.0) FROM statistics_lookup WHERE feature_name = 'fft_coefficient__attr_real__coeff_2' LIMIT 1)) / NULLIF((SELECT COALESCE(std_value, 1.0) FROM statistics_lookup WHERE feature_name = 'fft_coefficient__attr_real__coeff_2' LIMIT 1), 0.0) AS fft_r2_z,
				(COALESCE(fe.features.fft_coefficient__attr_imag__coeff_2, 0.0) - (SELECT COALESCE(mean_value, 0.0) FROM statistics_lookup WHERE feature_name = 'fft_coefficient__attr_imag__coeff_2' LIMIT 1)) / NULLIF((SELECT COALESCE(std_value, 1.0) FROM statistics_lookup WHERE feature_name = 'fft_coefficient__attr_imag__coeff_2' LIMIT 1), 0.0) AS fft_i2_z,
				(COALESCE(fe.features.fft_aggregated__aggtype_mean, 0.0) - (SELECT COALESCE(mean_value, 0.0) FROM statistics_lookup WHERE feature_name = 'fft_aggregated__aggtype_mean' LIMIT 1)) / NULLIF((SELECT COALESCE(std_value, 1.0) FROM statistics_lookup WHERE feature_name = 'fft_aggregated__aggtype_mean' LIMIT 1), 0.0) AS fft_mean_z,
				(COALESCE(fe.features.fft_aggregated__aggtype_variance, 0.0) - (SELECT COALESCE(mean_value, 0.0) FROM statistics_lookup WHERE feature_name = 'fft_aggregated__aggtype_variance' LIMIT 1)) / NULLIF((SELECT COALESCE(std_value, 1.0) FROM statistics_lookup WHERE feature_name = 'fft_aggregated__aggtype_variance' LIMIT 1), 0.0) AS fft_var_z,
				-- Temporal Patterns (dims 26-29) - z-score normalized
				(COALESCE(fe.features.autocorrelation__lag_1, 0.0) - (SELECT COALESCE(mean_value, 0.0) FROM statistics_lookup WHERE feature_name = 'autocorrelation__lag_1' LIMIT 1)) / NULLIF((SELECT COALESCE(std_value, 1.0) FROM statistics_lookup WHERE feature_name = 'autocorrelation__lag_1' LIMIT 1), 0.0) AS ac1_z,
				(COALESCE(fe.features.autocorrelation__lag_7, 0.0) - (SELECT COALESCE(mean_value, 0.0) FROM statistics_lookup WHERE feature_name = 'autocorrelation__lag_7' LIMIT 1)) / NULLIF((SELECT COALESCE(std_value, 1.0) FROM statistics_lookup WHERE feature_name = 'autocorrelation__lag_7' LIMIT 1), 0.0) AS ac7_z,
				(COALESCE(fe.features.autocorrelation__lag_14, 0.0) - (SELECT COALESCE(mean_value, 0.0) FROM statistics_lookup WHERE feature_name = 'autocorrelation__lag_14' LIMIT 1)) / NULLIF((SELECT COALESCE(std_value, 1.0) FROM statistics_lookup WHERE feature_name = 'autocorrelation__lag_14' LIMIT 1), 0.0) AS ac14_z,
				(COALESCE(fe.features.autocorrelation__lag_28, 0.0) - (SELECT COALESCE(mean_value, 0.0) FROM statistics_lookup WHERE feature_name = 'autocorrelation__lag_28' LIMIT 1)) / NULLIF((SELECT COALESCE(std_value, 1.0) FROM statistics_lookup WHERE feature_name = 'autocorrelation__lag_28' LIMIT 1), 0.0) AS ac28_z,
				-- Phase 2C: Advanced domain features (92-97)
				-- Feature 92: Movement type receipt ratio
				(COALESCE(p2c.receipt_ratio, 0.0) - (SELECT COALESCE(mean_value, 0.0) FROM statistics_lookup WHERE feature_name = 'movement_type_receipt_ratio' LIMIT 1)) / NULLIF((SELECT COALESCE(std_value, 1.0) FROM statistics_lookup WHERE feature_name = 'movement_type_receipt_ratio' LIMIT 1), 0.0) AS receipt_ratio_z,
				-- Feature 93: Movement type reversal ratio
				(COALESCE(p2c.reversal_ratio, 0.0) - (SELECT COALESCE(mean_value, 0.0) FROM statistics_lookup WHERE feature_name = 'movement_type_reversal_ratio' LIMIT 1)) / NULLIF((SELECT COALESCE(std_value, 1.0) FROM statistics_lookup WHERE feature_name = 'movement_type_reversal_ratio' LIMIT 1), 0.0) AS reversal_ratio_z,
				-- Feature 94: Weekday/weekend ratio
				(COALESCE(p2c.weekend_ratio, 0.0) - (SELECT COALESCE(mean_value, 0.0) FROM statistics_lookup WHERE feature_name = 'weekday_weekend_ratio' LIMIT 1)) / NULLIF((SELECT COALESCE(std_value, 1.0) FROM statistics_lookup WHERE feature_name = 'weekday_weekend_ratio' LIMIT 1), 0.0) AS weekend_ratio_z,
				-- Feature 95: Weekday concentration
				(COALESCE(p2c.weekday_concentration, 0.0) - (SELECT COALESCE(mean_value, 0.0) FROM statistics_lookup WHERE feature_name = 'weekday_concentration' LIMIT 1)) / NULLIF((SELECT COALESCE(std_value, 1.0) FROM statistics_lookup WHERE feature_name = 'weekday_concentration' LIMIT 1), 0.0) AS weekday_conc_z,
				-- Feature 96: Lifecycle trend strength (from anofox-forecast)
				(CASE
					WHEN ABS(COALESCE(fe.features.mean, 1.0)) > 1e-8 THEN
						ABS(COALESCE(fe.features.linear_trend__slope, 0.0)) / ABS(COALESCE(fe.features.mean, 1.0))
					ELSE 0.0
				END - (SELECT COALESCE(mean_value, 0.0) FROM statistics_lookup WHERE feature_name = 'lifecycle_trend_strength' LIMIT 1)) / NULLIF((SELECT COALESCE(std_value, 1.0) FROM statistics_lookup WHERE feature_name = 'lifecycle_trend_strength' LIMIT 1), 0.0) AS trend_strength_z,
				-- Feature 97: Lifecycle growth indicator (from anofox-forecast)
				(CASE
					WHEN ABS(COALESCE(fe.features.mean, 1.0)) > 1e-8 THEN
						SIGN(COALESCE(fe.features.linear_trend__slope, 0.0)) *
						LEAST(1.0, ABS(COALESCE(fe.features.linear_trend__slope, 0.0)) / ABS(COALESCE(fe.features.mean, 1.0)))
					ELSE 0.0
				END - (SELECT COALESCE(mean_value, 0.0) FROM statistics_lookup WHERE feature_name = 'lifecycle_growth_indicator' LIMIT 1)) / NULLIF((SELECT COALESCE(std_value, 1.0) FROM statistics_lookup WHERE feature_name = 'lifecycle_growth_indicator' LIMIT 1), 0.0) AS growth_indicator_z
			FROM feature_extraction fe
			LEFT JOIN phase2c_features p2c ON fe.material_id = p2c.material_id
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

					-- Phase 2B: Extended tsfresh features (62 features: dims 30-91)
					-- Currently zero-padded; will be populated with Phase 2B computations
					0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0,
					0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0,
					0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0,
					0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0,
					0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0,
					0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0,
					0.0, 0.0,
					-- Phase 2C: Advanced domain-specific features (6 features: dims 92-97)
					COALESCE(receipt_ratio_z, 0.0),
					COALESCE(reversal_ratio_z, 0.0),
					COALESCE(weekend_ratio_z, 0.0),
					COALESCE(weekday_conc_z, 0.0),
					COALESCE(trend_strength_z, 0.0),
					COALESCE(growth_indicator_z, 0.0),
					-- Reserved / Zero-Padding (30 features: dims 98-127)
					0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0,
					0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0,
					0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0
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
