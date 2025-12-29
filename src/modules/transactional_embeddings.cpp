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
			WHERE movement_date >= CURRENT_DATE - MAKE_INTERVAL(days => time_window_days)
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
		embedding_vectors AS (
			-- Convert STRUCT features to FLOAT[128] array
			-- Extract 30 key features and pad with zeros to reach 128 dimensions
			SELECT
				material_id,
				ARRAY[
					-- Consumption Patterns (10 features: dims 1-10)
					-- Understanding average usage rates and trends
					COALESCE(features.mean, 0.0),
					COALESCE(features.median, 0.0),
					COALESCE(features.variance, 0.0),
					COALESCE(features.standard_deviation, 0.0),
					COALESCE(features.linear_trend__slope, 0.0),
					COALESCE(features.linear_trend__intercept, 0.0),
					COALESCE(features.sum_values, 0.0),
					COALESCE(features.length, 0.0),
					COALESCE(features.first_location_of_maximum, 0.0),
					COALESCE(features.last_location_of_maximum, 0.0),

					-- Demand Volatility (8 features: dims 11-18)
					-- Measuring unpredictability and irregularity of consumption
					COALESCE(features.coefficient_variation, 0.0),
					COALESCE(features.skewness, 0.0),
					COALESCE(features.kurtosis, 0.0),
					COALESCE(features.range_count, 0.0),
					COALESCE(features.ratio_beyond_r_sigma__r_1, 0.0),
					COALESCE(features.approximate_entropy, 0.0),
					COALESCE(features.sample_entropy, 0.0),
					COALESCE(features.benford_correlation, 0.0),

					-- Frequency Domain (8 features: dims 19-26)
					-- Detecting periodic patterns and oscillations in usage
					COALESCE(features.fft_coefficient__attr_real__coeff_0, 0.0),
					COALESCE(features.fft_coefficient__attr_imag__coeff_0, 0.0),
					COALESCE(features.fft_coefficient__attr_real__coeff_1, 0.0),
					COALESCE(features.fft_coefficient__attr_imag__coeff_1, 0.0),
					COALESCE(features.fft_coefficient__attr_real__coeff_2, 0.0),
					COALESCE(features.fft_coefficient__attr_imag__coeff_2, 0.0),
					COALESCE(features.fft_aggregated__aggtype_mean, 0.0),
					COALESCE(features.fft_aggregated__aggtype_variance, 0.0),

					-- Temporal Patterns (4 features: dims 27-30)
					-- Understanding short-term and long-term autocorrelation
					COALESCE(features.autocorrelation__lag_1, 0.0),
					COALESCE(features.autocorrelation__lag_7, 0.0),
					COALESCE(features.autocorrelation__lag_14, 0.0),
					COALESCE(features.autocorrelation__lag_28, 0.0),

					-- Reserved / Zero-Padding (98 features: dims 31-128)
					-- Pre-allocated space for future feature expansion
					-- Phase 2 will populate these with additional features
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
				]::FLOAT[128] AS transactional_embedding
			FROM feature_extraction
		)
		SELECT
			material_id,
			transactional_embedding
		FROM embedding_vectors
	)");

	CheckQueryResult(result, "create compute_transactional_embeddings macro");
}

} // namespace anofox
} // namespace duckdb
