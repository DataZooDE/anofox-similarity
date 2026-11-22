#include "modules/embedding_statistics.hpp"
#include "core/error_handling.hpp"
#include "duckdb/main/connection.hpp"
#include "datazoo_function_doc.hpp"

namespace ddoc = datazoo::doc;

namespace duckdb {
namespace anofox {

static const DefaultTableMacro EXTRACT_TS_FEATURES_MACRO = {
    DEFAULT_SCHEMA,
    "extract_ts_features",
    {nullptr},
    {{"movements_table", "'goods_movements'"}, {"time_window_days", "365"}, {"min_observations", "3"}, {"batch_size", "NULL"}, {"batch_offset", "0"}, {nullptr, nullptr}},
    R"(
		WITH filtered_materials AS (
			-- Apply batch filtering if specified (Incremental Update Optimization)
			SELECT DISTINCT material_id
			FROM filter_recent_movements(movements_table, time_window_days, 0)
			ORDER BY material_id
			LIMIT CASE WHEN batch_size IS NOT NULL THEN batch_size ELSE NULL END
			OFFSET CASE WHEN batch_size IS NOT NULL THEN batch_offset ELSE 0 END
		),
		-- The series is weekly consumption (positive quantities), zero-filled between a material's
		-- first and last week in the window: lags and lengths count weeks, not postings.
		weekly AS (
			SELECT gm.material_id, date_trunc('week', gm.movement_date)::TIMESTAMP AS wk,
			       SUM(gm.quantity)::DOUBLE AS q, COUNT(*) AS postings
			FROM filter_recent_movements(movements_table, time_window_days, 0) gm
			INNER JOIN filtered_materials fm ON gm.material_id = fm.material_id
			GROUP BY ALL
		),
		spans AS (
			SELECT material_id, MIN(wk) AS lo, MAX(wk) AS hi
			FROM weekly GROUP BY material_id
			HAVING SUM(postings) >= COALESCE(min_observations, 3)
		),
		series AS (
			SELECT g.material_id, g.wk, COALESCE(w.q, 0.0) AS q
			FROM (SELECT material_id, unnest(generate_series(lo, hi, INTERVAL 7 DAY)) AS wk FROM spans) g
			LEFT JOIN weekly w ON w.material_id = g.material_id AND w.wk = g.wk
		),
		raw AS (
			-- anofox_forecast's aggregate (tsfresh-compatible features), see docs/api/04-embeddings.md
			SELECT material_id, anofox_fcst_ts_features_agg(wk, q) AS f
			FROM series GROUP BY material_id
		)
		-- Our own stable feature names; NaN and infinity (short series) become NULL, so the
		-- statistics stay finite and the z-scores fall back to 0.
		SELECT material_id,
			struct_pack(
					mean := CASE WHEN isfinite(f.mean) THEN f.mean END,
					median := CASE WHEN isfinite(f.median) THEN f.median END,
					variance := CASE WHEN isfinite(f.variance) THEN f.variance END,
					standard_deviation := CASE WHEN isfinite(f.standard_deviation) THEN f.standard_deviation END,
					linear_trend__slope := CASE WHEN isfinite(f.linear_trend_slope) THEN f.linear_trend_slope END,
					linear_trend__intercept := CASE WHEN isfinite(f.linear_trend_intercept) THEN f.linear_trend_intercept END,
					sum_values := CASE WHEN isfinite(f.sum) THEN f.sum END,
					length := CASE WHEN isfinite(f.length) THEN f.length END,
					first_location_of_maximum := CASE WHEN isfinite(f.first_location_of_maximum) THEN f.first_location_of_maximum END,
					last_location_of_maximum := CASE WHEN isfinite(f.last_location_of_maximum) THEN f.last_location_of_maximum END,
					coefficient_variation := CASE WHEN isfinite(f.variation_coefficient) THEN f.variation_coefficient END,
					skewness := CASE WHEN isfinite(f.skewness) THEN f.skewness END,
					kurtosis := CASE WHEN isfinite(f.kurtosis) THEN f.kurtosis END,
					range := CASE WHEN isfinite(f.range) THEN f.range END,
					ratio_beyond_r_sigma__r_1 := CASE WHEN isfinite(f.ratio_beyond_r_sigma_1) THEN f.ratio_beyond_r_sigma_1 END,
					approximate_entropy := CASE WHEN isfinite(f.approximate_entropy) THEN f.approximate_entropy END,
					sample_entropy := CASE WHEN isfinite(f.sample_entropy) THEN f.sample_entropy END,
					benford_correlation := CASE WHEN isfinite(f.benford_correlation) THEN f.benford_correlation END,
					fft_coefficient__attr_real__coeff_0 := CASE WHEN isfinite(f.fft_coefficient_0_real) THEN f.fft_coefficient_0_real END,
					fft_coefficient__attr_imag__coeff_0 := CASE WHEN isfinite(f.fft_coefficient_0_imag) THEN f.fft_coefficient_0_imag END,
					fft_coefficient__attr_real__coeff_1 := CASE WHEN isfinite(f.fft_coefficient_1_real) THEN f.fft_coefficient_1_real END,
					fft_coefficient__attr_imag__coeff_1 := CASE WHEN isfinite(f.fft_coefficient_1_imag) THEN f.fft_coefficient_1_imag END,
					fft_coefficient__attr_real__coeff_2 := CASE WHEN isfinite(f.fft_coefficient_2_real) THEN f.fft_coefficient_2_real END,
					fft_coefficient__attr_imag__coeff_2 := CASE WHEN isfinite(f.fft_coefficient_2_imag) THEN f.fft_coefficient_2_imag END,
					spectral_centroid := CASE WHEN isfinite(f.spectral_centroid) THEN f.spectral_centroid END,
					spectral_variance := CASE WHEN isfinite(f.spectral_variance) THEN f.spectral_variance END,
					autocorrelation__week_1 := CASE WHEN isfinite(f.autocorrelation_lag1) THEN f.autocorrelation_lag1 END,
					autocorrelation__week_2 := CASE WHEN isfinite(f.autocorrelation_lag2) THEN f.autocorrelation_lag2 END,
					autocorrelation__week_4 := CASE WHEN isfinite(f.autocorrelation_lag4) THEN f.autocorrelation_lag4 END,
					autocorrelation__week_8 := CASE WHEN isfinite(f.autocorrelation_lag8) THEN f.autocorrelation_lag8 END
			) AS features
		FROM raw)"};

static const DefaultTableMacro RECOMPUTE_EMBEDDING_STATISTICS_MACRO = {
    DEFAULT_SCHEMA,
    "recompute_embedding_statistics",
    {nullptr},
    {{"time_window_days", "365"}, {"min_observations", "3"}, {"movements_table", "'goods_movements'"}, {nullptr, nullptr}},
    R"(
		WITH all_features AS (
			-- Passing the table as a macro parameter (rather than a literal) defers binding of the
			-- nested extract_ts_features() call to call time, so this macro registers at load even
			-- when the anofox-forecast dependency is not yet present.
			SELECT * FROM extract_ts_features(movements_table, time_window_days, min_observations)
		),
		feature_stats AS (
			SELECT 'mean' AS feature_name, 0 AS feature_index, 'consumption' AS feature_category,
				AVG(features.mean) AS mean_value, STDDEV_POP(features.mean) AS std_value,
				MIN(features.mean) AS min_value, MAX(features.mean) AS max_value, COUNT(features.mean) AS num_samples
			FROM all_features
			UNION ALL
			SELECT 'median', 1, 'consumption', AVG(features.median), STDDEV_POP(features.median),
				MIN(features.median), MAX(features.median), COUNT(features.median)
			FROM all_features
			UNION ALL
			SELECT 'variance', 2, 'consumption', AVG(features.variance), STDDEV_POP(features.variance),
				MIN(features.variance), MAX(features.variance), COUNT(features.variance)
			FROM all_features
			UNION ALL
			SELECT 'standard_deviation', 3, 'consumption', AVG(features.standard_deviation), STDDEV_POP(features.standard_deviation),
				MIN(features.standard_deviation), MAX(features.standard_deviation), COUNT(features.standard_deviation)
			FROM all_features
			UNION ALL
			SELECT 'linear_trend__slope', 4, 'consumption', AVG(features.linear_trend__slope), STDDEV_POP(features.linear_trend__slope),
				MIN(features.linear_trend__slope), MAX(features.linear_trend__slope), COUNT(features.linear_trend__slope)
			FROM all_features
			UNION ALL
			SELECT 'linear_trend__intercept', 5, 'consumption', AVG(features.linear_trend__intercept), STDDEV_POP(features.linear_trend__intercept),
				MIN(features.linear_trend__intercept), MAX(features.linear_trend__intercept), COUNT(features.linear_trend__intercept)
			FROM all_features
			UNION ALL
			SELECT 'sum_values', 6, 'consumption', AVG(features.sum_values), STDDEV_POP(features.sum_values),
				MIN(features.sum_values), MAX(features.sum_values), COUNT(features.sum_values)
			FROM all_features
			UNION ALL
			SELECT 'length', 7, 'consumption', AVG(features.length), STDDEV_POP(features.length),
				MIN(features.length), MAX(features.length), COUNT(features.length)
			FROM all_features
			UNION ALL
			SELECT 'first_location_of_maximum', 8, 'consumption', AVG(features.first_location_of_maximum), STDDEV_POP(features.first_location_of_maximum),
				MIN(features.first_location_of_maximum), MAX(features.first_location_of_maximum), COUNT(features.first_location_of_maximum)
			FROM all_features
			UNION ALL
			SELECT 'last_location_of_maximum', 9, 'consumption', AVG(features.last_location_of_maximum), STDDEV_POP(features.last_location_of_maximum),
				MIN(features.last_location_of_maximum), MAX(features.last_location_of_maximum), COUNT(features.last_location_of_maximum)
			FROM all_features
			UNION ALL
			SELECT 'coefficient_variation', 10, 'volatility', AVG(features.coefficient_variation), STDDEV_POP(features.coefficient_variation),
				MIN(features.coefficient_variation), MAX(features.coefficient_variation), COUNT(features.coefficient_variation)
			FROM all_features
			UNION ALL
			SELECT 'skewness', 11, 'volatility', AVG(features.skewness), STDDEV_POP(features.skewness),
				MIN(features.skewness), MAX(features.skewness), COUNT(features.skewness)
			FROM all_features
			UNION ALL
			SELECT 'kurtosis', 12, 'volatility', AVG(features.kurtosis), STDDEV_POP(features.kurtosis),
				MIN(features.kurtosis), MAX(features.kurtosis), COUNT(features.kurtosis)
			FROM all_features
			UNION ALL
			SELECT 'range', 13, 'volatility', AVG(features.range), STDDEV_POP(features.range),
				MIN(features.range), MAX(features.range), COUNT(features.range)
			FROM all_features
			UNION ALL
			SELECT 'ratio_beyond_r_sigma__r_1', 14, 'volatility', AVG(features.ratio_beyond_r_sigma__r_1), STDDEV_POP(features.ratio_beyond_r_sigma__r_1),
				MIN(features.ratio_beyond_r_sigma__r_1), MAX(features.ratio_beyond_r_sigma__r_1), COUNT(features.ratio_beyond_r_sigma__r_1)
			FROM all_features
			UNION ALL
			SELECT 'approximate_entropy', 15, 'volatility', AVG(features.approximate_entropy), STDDEV_POP(features.approximate_entropy),
				MIN(features.approximate_entropy), MAX(features.approximate_entropy), COUNT(features.approximate_entropy)
			FROM all_features
			UNION ALL
			SELECT 'sample_entropy', 16, 'volatility', AVG(features.sample_entropy), STDDEV_POP(features.sample_entropy),
				MIN(features.sample_entropy), MAX(features.sample_entropy), COUNT(features.sample_entropy)
			FROM all_features
			UNION ALL
			SELECT 'benford_correlation', 17, 'volatility', AVG(features.benford_correlation), STDDEV_POP(features.benford_correlation),
				MIN(features.benford_correlation), MAX(features.benford_correlation), COUNT(features.benford_correlation)
			FROM all_features
			UNION ALL
			SELECT 'fft_coefficient__attr_real__coeff_0', 18, 'frequency', AVG(features.fft_coefficient__attr_real__coeff_0), STDDEV_POP(features.fft_coefficient__attr_real__coeff_0),
				MIN(features.fft_coefficient__attr_real__coeff_0), MAX(features.fft_coefficient__attr_real__coeff_0), COUNT(features.fft_coefficient__attr_real__coeff_0)
			FROM all_features
			UNION ALL
			SELECT 'fft_coefficient__attr_imag__coeff_0', 19, 'frequency', AVG(features.fft_coefficient__attr_imag__coeff_0), STDDEV_POP(features.fft_coefficient__attr_imag__coeff_0),
				MIN(features.fft_coefficient__attr_imag__coeff_0), MAX(features.fft_coefficient__attr_imag__coeff_0), COUNT(features.fft_coefficient__attr_imag__coeff_0)
			FROM all_features
			UNION ALL
			SELECT 'fft_coefficient__attr_real__coeff_1', 20, 'frequency', AVG(features.fft_coefficient__attr_real__coeff_1), STDDEV_POP(features.fft_coefficient__attr_real__coeff_1),
				MIN(features.fft_coefficient__attr_real__coeff_1), MAX(features.fft_coefficient__attr_real__coeff_1), COUNT(features.fft_coefficient__attr_real__coeff_1)
			FROM all_features
			UNION ALL
			SELECT 'fft_coefficient__attr_imag__coeff_1', 21, 'frequency', AVG(features.fft_coefficient__attr_imag__coeff_1), STDDEV_POP(features.fft_coefficient__attr_imag__coeff_1),
				MIN(features.fft_coefficient__attr_imag__coeff_1), MAX(features.fft_coefficient__attr_imag__coeff_1), COUNT(features.fft_coefficient__attr_imag__coeff_1)
			FROM all_features
			UNION ALL
			SELECT 'fft_coefficient__attr_real__coeff_2', 22, 'frequency', AVG(features.fft_coefficient__attr_real__coeff_2), STDDEV_POP(features.fft_coefficient__attr_real__coeff_2),
				MIN(features.fft_coefficient__attr_real__coeff_2), MAX(features.fft_coefficient__attr_real__coeff_2), COUNT(features.fft_coefficient__attr_real__coeff_2)
			FROM all_features
			UNION ALL
			SELECT 'fft_coefficient__attr_imag__coeff_2', 23, 'frequency', AVG(features.fft_coefficient__attr_imag__coeff_2), STDDEV_POP(features.fft_coefficient__attr_imag__coeff_2),
				MIN(features.fft_coefficient__attr_imag__coeff_2), MAX(features.fft_coefficient__attr_imag__coeff_2), COUNT(features.fft_coefficient__attr_imag__coeff_2)
			FROM all_features
			UNION ALL
			SELECT 'spectral_centroid', 24, 'frequency', AVG(features.spectral_centroid), STDDEV_POP(features.spectral_centroid),
				MIN(features.spectral_centroid), MAX(features.spectral_centroid), COUNT(features.spectral_centroid)
			FROM all_features
			UNION ALL
			SELECT 'spectral_variance', 25, 'frequency', AVG(features.spectral_variance), STDDEV_POP(features.spectral_variance),
				MIN(features.spectral_variance), MAX(features.spectral_variance), COUNT(features.spectral_variance)
			FROM all_features
			UNION ALL
			SELECT 'autocorrelation__week_1', 26, 'temporal', AVG(features.autocorrelation__week_1), STDDEV_POP(features.autocorrelation__week_1),
				MIN(features.autocorrelation__week_1), MAX(features.autocorrelation__week_1), COUNT(features.autocorrelation__week_1)
			FROM all_features
			UNION ALL
			SELECT 'autocorrelation__week_2', 27, 'temporal', AVG(features.autocorrelation__week_2), STDDEV_POP(features.autocorrelation__week_2),
				MIN(features.autocorrelation__week_2), MAX(features.autocorrelation__week_2), COUNT(features.autocorrelation__week_2)
			FROM all_features
			UNION ALL
			SELECT 'autocorrelation__week_4', 28, 'temporal', AVG(features.autocorrelation__week_4), STDDEV_POP(features.autocorrelation__week_4),
				MIN(features.autocorrelation__week_4), MAX(features.autocorrelation__week_4), COUNT(features.autocorrelation__week_4)
			FROM all_features
			UNION ALL
			SELECT 'autocorrelation__week_8', 29, 'temporal', AVG(features.autocorrelation__week_8), STDDEV_POP(features.autocorrelation__week_8),
				MIN(features.autocorrelation__week_8), MAX(features.autocorrelation__week_8), COUNT(features.autocorrelation__week_8)
			FROM all_features
		)
		-- Every row the embedding reads: the time-series features (positions 0-29) and the ERP
		-- features (92-97). Table macros cannot contain DML, so this returns the statistics; persist
		-- them with INSERT OR REPLACE INTO transactional_embedding_statistics SELECT *, now(), 1 FROM
		-- recompute_embedding_statistics().
		SELECT feature_name, feature_index, feature_category, mean_value, std_value,
		       min_value, max_value, num_samples
		FROM feature_stats
		UNION ALL
		SELECT * FROM compute_domain_specific_statistics(movements_table, time_window_days, min_observations))"};

static const DefaultTableMacro COMPUTE_DOMAIN_SPECIFIC_STATISTICS_MACRO = {
    DEFAULT_SCHEMA,
    "compute_domain_specific_statistics",
    {nullptr},
    {{"movements_table", "'goods_movements'"}, {"time_window_days", "365"}, {"min_observations", "3"}, {nullptr, nullptr}},
    R"(
		WITH
		-- Aggregate goods_movements by material and compute Domain-Specific ERP feature values
		movement_features AS (
			SELECT
				material_id,
				COUNT(*) AS total_moves,
				COUNT(*) FILTER (WHERE mt = '261') AS issue_count,
				COUNT(*) FILTER (WHERE mt IN ('262', '602')) AS reversal_count,
				COUNT(*) FILTER (WHERE EXTRACT(DOW FROM movement_date) IN (0, 6)) AS weekend_count,
				-- Weekday distribution: compute entropy for concentration metric
				COUNT(*) FILTER (WHERE EXTRACT(DOW FROM movement_date) = 0) AS sun_count,
				COUNT(*) FILTER (WHERE EXTRACT(DOW FROM movement_date) = 1) AS mon_count,
				COUNT(*) FILTER (WHERE EXTRACT(DOW FROM movement_date) = 2) AS tue_count,
				COUNT(*) FILTER (WHERE EXTRACT(DOW FROM movement_date) = 3) AS wed_count,
				COUNT(*) FILTER (WHERE EXTRACT(DOW FROM movement_date) = 4) AS thu_count,
				COUNT(*) FILTER (WHERE EXTRACT(DOW FROM movement_date) = 5) AS fri_count,
				COUNT(*) FILTER (WHERE EXTRACT(DOW FROM movement_date) = 6) AS sat_count
			-- movement_type is optional (NULL without the column); movements of either sign count,
			-- so a reversal posted as a negative quantity (a view signed by SHKZG) is seen
			FROM (SELECT material_id, movement_date, TRIM(movement_type::VARCHAR) AS mt
			      FROM ((SELECT NULL AS movement_type WHERE false) UNION ALL BY NAME (SELECT * FROM query_table(movements_table)))
			      WHERE movement_date >= (SELECT MAX(movement_date) FROM query_table(movements_table))
			                             - (INTERVAL '1 day' * COALESCE(time_window_days, 365))
			        AND quantity IS NOT NULL
			        AND quantity <> 0)
			GROUP BY material_id
			HAVING COUNT(*) >= COALESCE(min_observations, 3)
		),
		domain_specific_features AS (
			SELECT
				material_id,
				-- Feature 92: share of movement type 261 (goods issue to production)
				COALESCE(issue_count::FLOAT / NULLIF(total_moves, 0), 0.0) AS issue_ratio,
				-- Feature 93: share of reversals (262 of an issue, 602 of a delivery)
				COALESCE(reversal_count::FLOAT / NULLIF(total_moves, 0), 0.0) AS reversal_ratio,
				-- Feature 94: Weekday/weekend ratio
				COALESCE(weekend_count::FLOAT / NULLIF(total_moves, 0), 0.0) AS weekend_ratio,
				-- Feature 95: Weekday concentration (1 - normalized entropy)
				-- Normalized entropy of daily distribution
				CASE
					WHEN total_moves > 0 THEN
						1.0 - (
							CASE WHEN sun_count > 0 THEN -(sun_count::FLOAT/total_moves) * LN(sun_count::FLOAT/total_moves) ELSE 0.0 END +
							CASE WHEN mon_count > 0 THEN -(mon_count::FLOAT/total_moves) * LN(mon_count::FLOAT/total_moves) ELSE 0.0 END +
							CASE WHEN tue_count > 0 THEN -(tue_count::FLOAT/total_moves) * LN(tue_count::FLOAT/total_moves) ELSE 0.0 END +
							CASE WHEN wed_count > 0 THEN -(wed_count::FLOAT/total_moves) * LN(wed_count::FLOAT/total_moves) ELSE 0.0 END +
							CASE WHEN thu_count > 0 THEN -(thu_count::FLOAT/total_moves) * LN(thu_count::FLOAT/total_moves) ELSE 0.0 END +
							CASE WHEN fri_count > 0 THEN -(fri_count::FLOAT/total_moves) * LN(fri_count::FLOAT/total_moves) ELSE 0.0 END +
							CASE WHEN sat_count > 0 THEN -(sat_count::FLOAT/total_moves) * LN(sat_count::FLOAT/total_moves) ELSE 0.0 END
						) / LN(7.0)
					ELSE 0.0
				END AS weekday_concentration
			FROM movement_features
		),
		-- Compute lifecycle features using shared extract_ts_features macro (Issue #9 optimization)
		all_features AS (
			SELECT * FROM extract_ts_features(movements_table, time_window_days, min_observations)
		),
		lifecycle_features AS (
			SELECT
				af.material_id,
				-- Feature 96: Lifecycle trend strength = |slope| / mean
				CASE
					WHEN ABS(COALESCE(af.features.mean, 1.0)) > 1e-8 THEN
						ABS(COALESCE(af.features.linear_trend__slope, 0.0)) / ABS(COALESCE(af.features.mean, 1.0))
					ELSE 0.0
				END AS trend_strength,
				-- Feature 97: Lifecycle growth indicator = SIGN(slope) * MIN(1.0, trend_strength)
				CASE
					WHEN ABS(COALESCE(af.features.mean, 1.0)) > 1e-8 THEN
						SIGN(COALESCE(af.features.linear_trend__slope, 0.0)) *
						LEAST(1.0, ABS(COALESCE(af.features.linear_trend__slope, 0.0)) / ABS(COALESCE(af.features.mean, 1.0)))
					ELSE 0.0
				END AS growth_indicator
			FROM all_features af
		),
		-- Combine all domain-specific features and compute statistics
		combined_domain_features AS (
			SELECT
				pf.material_id,
				pf.issue_ratio,
				pf.reversal_ratio,
				pf.weekend_ratio,
				pf.weekday_concentration,
				lf.trend_strength,
				lf.growth_indicator
			FROM domain_specific_features pf
			LEFT JOIN lifecycle_features lf ON pf.material_id = lf.material_id
		),
		domain_stats AS (
			SELECT
				'movement_type_issue_ratio' AS feature_name, 92 AS feature_index, 'advanced' AS feature_category,
				AVG(issue_ratio) AS mean_value, STDDEV_POP(issue_ratio) AS std_value,
				MIN(issue_ratio) AS min_value, MAX(issue_ratio) AS max_value,
				COUNT(*) AS num_samples
			FROM combined_domain_features
			WHERE issue_ratio IS NOT NULL
			UNION ALL
			SELECT
				'movement_type_reversal_ratio', 93, 'advanced',
				AVG(reversal_ratio), STDDEV_POP(reversal_ratio),
				MIN(reversal_ratio), MAX(reversal_ratio), COUNT(*)
			FROM combined_domain_features
			WHERE reversal_ratio IS NOT NULL
			UNION ALL
			SELECT
				'weekday_weekend_ratio', 94, 'advanced',
				AVG(weekend_ratio), STDDEV_POP(weekend_ratio),
				MIN(weekend_ratio), MAX(weekend_ratio), COUNT(*)
			FROM combined_domain_features
			WHERE weekend_ratio IS NOT NULL
			UNION ALL
			SELECT
				'weekday_concentration', 95, 'advanced',
				AVG(weekday_concentration), STDDEV_POP(weekday_concentration),
				MIN(weekday_concentration), MAX(weekday_concentration), COUNT(*)
			FROM combined_domain_features
			WHERE weekday_concentration IS NOT NULL
			UNION ALL
			SELECT
				'lifecycle_trend_strength', 96, 'advanced',
				AVG(trend_strength), STDDEV_POP(trend_strength),
				MIN(trend_strength), MAX(trend_strength), COUNT(*)
			FROM combined_domain_features
			WHERE trend_strength IS NOT NULL
			UNION ALL
			SELECT
				'lifecycle_growth_indicator', 97, 'advanced',
				AVG(growth_indicator), STDDEV_POP(growth_indicator),
				MIN(growth_indicator), MAX(growth_indicator), COUNT(*)
			FROM combined_domain_features
			WHERE growth_indicator IS NOT NULL
		)
		-- Pure computation (table macros cannot contain DML); persist with an explicit
		-- INSERT OR REPLACE INTO transactional_embedding_statistics SELECT ... if desired.
		SELECT
			feature_name, feature_index, feature_category, mean_value, std_value,
			min_value, max_value, num_samples
		FROM domain_stats)"};

void RegisterStatisticsMacros(ExtensionLoader &loader) {
	ddoc::Registrar reg(loader, {"similarity", "embeddings"});
	reg.RegisterTableMacro(EXTRACT_TS_FEATURES_MACRO,
	           {ddoc::Doc()
	                .Describe("The 30 time-series features of the transactional embedding per material, from anofox_forecast's anofox_fcst_ts_features_agg over the weekly series of positive quantities (zero-filled between the first and last week in the window). Returns (material_id, features STRUCT) with stable field names; NaN and infinite values (short histories) are NULL. A material needs min_observations movements. 'batch_size' and 'batch_offset' page through large material sets; NULL batch_size means all of them. Needs anofox_forecast (INSTALL/LOAD anofox_forecast FROM community).")
	                .Example("SELECT * FROM extract_ts_features(movements_table := 'goods_movements', time_window_days := 365)")});
	reg.RegisterTableMacro(RECOMPUTE_EMBEDDING_STATISTICS_MACRO,
	           {ddoc::Doc()
	                .Describe("Recomputes the statistics compute_transactional_embeddings normalises with: one row per feature it reads (positions 0-29 from extract_ts_features, 92-97 from compute_domain_specific_statistics), mean and standard deviation over the finite values. Store them: INSERT OR REPLACE INTO transactional_embedding_statistics SELECT *, now(), 1 FROM recompute_embedding_statistics(). 'time_window_days' is the historical window and 'min_observations' the minimum movements a material needs. Needs anofox_forecast.")
	                .Example("SELECT * FROM recompute_embedding_statistics(time_window_days := 365, min_observations := 3)")});
	reg.RegisterTableMacro(COMPUTE_DOMAIN_SPECIFIC_STATISTICS_MACRO,
	           {ddoc::Doc()
	                .Describe("Statistics of the six ERP features of the transactional embedding (positions 92-97): share of movement type 261, share of reversals (262, 602), weekend share, weekday concentration, lifecycle trend strength and growth. Counts movements of either sign; movement_type is optional (shares 0 without it). Part of recompute_embedding_statistics. Needs anofox_forecast for the lifecycle features.")
	                .Example("SELECT * FROM compute_domain_specific_statistics(movements_table := 'goods_movements')")});
}

} // namespace anofox
} // namespace duckdb
