#include "modules/transactional_embeddings.hpp"
#include "core/error_handling.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/parser/parser.hpp"
#include "duckdb/parser/tableref/subqueryref.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/connection.hpp"
#include "telemetry.hpp"

namespace duckdb {
namespace anofox {

//------------------------------------------------------------------------------
// Helper: Parse SQL to SubqueryRef
//------------------------------------------------------------------------------

static unique_ptr<SubqueryRef> ParseSubquery(const string &query, const ParserOptions &options, const string &err_msg) {
	Parser parser(options);
	parser.ParseQuery(query);
	if (parser.statements.size() != 1 || parser.statements[0]->type != StatementType::SELECT_STATEMENT) {
		throw ParserException(err_msg);
	}
	auto &select_stmt = parser.statements[0]->Cast<SelectStatement>();
	auto new_stmt = make_uniq<SelectStatement>();
	new_stmt->node = std::move(select_stmt.node);
	return make_uniq<SubqueryRef>(std::move(new_stmt));
}

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
// compute_transactional_embeddings TableFunction
//------------------------------------------------------------------------------

static unique_ptr<TableRef> ComputeTransactionalEmbeddingsBindReplace(ClientContext &context, TableFunctionBindInput &input) {
	PostHogTelemetry::Instance().CaptureFunctionExecution("compute_transactional_embeddings");

	// Parameters (all named with defaults):
	// movements_table := 'goods_movements'
	// material_column := 'material_id'
	// date_column := 'movement_date'
	// quantity_column := 'quantity'
	// time_window_days := 365
	// batch_size := NULL
	// batch_offset := 0

	string movements_table = "goods_movements";
	string material_column = "material_id";
	string date_column = "movement_date";
	string quantity_column = "quantity";
	int64_t time_window_days = 365;
	string batch_size = "NULL";
	int64_t batch_offset = 0;

	if (input.named_parameters.count("movements_table")) {
		movements_table = input.named_parameters.at("movements_table").ToString();
	}
	if (input.named_parameters.count("material_column")) {
		material_column = input.named_parameters.at("material_column").ToString();
	}
	if (input.named_parameters.count("date_column")) {
		date_column = input.named_parameters.at("date_column").ToString();
	}
	if (input.named_parameters.count("quantity_column")) {
		quantity_column = input.named_parameters.at("quantity_column").ToString();
	}
	if (input.named_parameters.count("time_window_days")) {
		time_window_days = input.named_parameters.at("time_window_days").GetValue<int64_t>();
	}
	if (input.named_parameters.count("batch_size") && !input.named_parameters.at("batch_size").IsNull()) {
		batch_size = std::to_string(input.named_parameters.at("batch_size").GetValue<int64_t>());
	}
	if (input.named_parameters.count("batch_offset")) {
		batch_offset = input.named_parameters.at("batch_offset").GetValue<int64_t>();
	}

	// Build the complex SQL query
	string sql = StringUtil::Format(R"(
		WITH dependency_check AS (
			SELECT check_anofox_forecast_available() AS is_available
		),
		validated AS (
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
		filtered_materials AS (
			SELECT DISTINCT material_id
			FROM filter_recent_movements('%s', %lld, 0)
			ORDER BY material_id
			LIMIT CASE WHEN %s IS NOT NULL THEN %s ELSE NULL END
			OFFSET CASE WHEN %s IS NOT NULL THEN %lld ELSE 0 END
		),
		feature_extraction AS (
			SELECT
				material_id,
				NULL::INTEGER AS num_observations,
				features
			FROM extract_ts_features(
				'%s',
				%lld,
				3,
				%s,
				%lld
			)
			WHERE (SELECT valid FROM validated LIMIT 1)
		),
		statistics_lookup AS (
			SELECT
				feature_name,
				COALESCE(mean_value, 0.0) AS mean_value,
				COALESCE(std_value, 1.0) AS std_value
			FROM transactional_embedding_statistics
			WHERE feature_index < 104
		),
		phase2c_features AS (
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
				NULL::FLOAT AS trend_strength,
				NULL::FLOAT AS growth_indicator
			FROM filter_recent_movements('%s', %lld, 0) gm
			INNER JOIN filtered_materials fm ON gm.material_id = fm.material_id
			GROUP BY gm.material_id
			HAVING COUNT(*) >= 3
		),
		normalized_features AS (
			SELECT
				fe.material_id,
				fe.num_observations,
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
				(COALESCE(fe.features.coefficient_variation, 0.0) - (SELECT COALESCE(mean_value, 0.0) FROM statistics_lookup WHERE feature_name = 'coefficient_variation' LIMIT 1)) / NULLIF((SELECT COALESCE(std_value, 1.0) FROM statistics_lookup WHERE feature_name = 'coefficient_variation' LIMIT 1), 0.0) AS cv_z,
				(COALESCE(fe.features.skewness, 0.0) - (SELECT COALESCE(mean_value, 0.0) FROM statistics_lookup WHERE feature_name = 'skewness' LIMIT 1)) / NULLIF((SELECT COALESCE(std_value, 1.0) FROM statistics_lookup WHERE feature_name = 'skewness' LIMIT 1), 0.0) AS skew_z,
				(COALESCE(fe.features.kurtosis, 0.0) - (SELECT COALESCE(mean_value, 0.0) FROM statistics_lookup WHERE feature_name = 'kurtosis' LIMIT 1)) / NULLIF((SELECT COALESCE(std_value, 1.0) FROM statistics_lookup WHERE feature_name = 'kurtosis' LIMIT 1), 0.0) AS kurt_z,
				(COALESCE(fe.features.range_count, 0.0) - (SELECT COALESCE(mean_value, 0.0) FROM statistics_lookup WHERE feature_name = 'range_count' LIMIT 1)) / NULLIF((SELECT COALESCE(std_value, 1.0) FROM statistics_lookup WHERE feature_name = 'range_count' LIMIT 1), 0.0) AS range_z,
				(COALESCE(fe.features.ratio_beyond_r_sigma__r_1, 0.0) - (SELECT COALESCE(mean_value, 0.0) FROM statistics_lookup WHERE feature_name = 'ratio_beyond_r_sigma__r_1' LIMIT 1)) / NULLIF((SELECT COALESCE(std_value, 1.0) FROM statistics_lookup WHERE feature_name = 'ratio_beyond_r_sigma__r_1' LIMIT 1), 0.0) AS ratio_z,
				(COALESCE(fe.features.approximate_entropy, 0.0) - (SELECT COALESCE(mean_value, 0.0) FROM statistics_lookup WHERE feature_name = 'approximate_entropy' LIMIT 1)) / NULLIF((SELECT COALESCE(std_value, 1.0) FROM statistics_lookup WHERE feature_name = 'approximate_entropy' LIMIT 1), 0.0) AS approx_ent_z,
				(COALESCE(fe.features.sample_entropy, 0.0) - (SELECT COALESCE(mean_value, 0.0) FROM statistics_lookup WHERE feature_name = 'sample_entropy' LIMIT 1)) / NULLIF((SELECT COALESCE(std_value, 1.0) FROM statistics_lookup WHERE feature_name = 'sample_entropy' LIMIT 1), 0.0) AS sample_ent_z,
				(COALESCE(fe.features.benford_correlation, 0.0) - (SELECT COALESCE(mean_value, 0.0) FROM statistics_lookup WHERE feature_name = 'benford_correlation' LIMIT 1)) / NULLIF((SELECT COALESCE(std_value, 1.0) FROM statistics_lookup WHERE feature_name = 'benford_correlation' LIMIT 1), 0.0) AS benford_z,
				(COALESCE(fe.features.fft_coefficient__attr_real__coeff_0, 0.0) - (SELECT COALESCE(mean_value, 0.0) FROM statistics_lookup WHERE feature_name = 'fft_coefficient__attr_real__coeff_0' LIMIT 1)) / NULLIF((SELECT COALESCE(std_value, 1.0) FROM statistics_lookup WHERE feature_name = 'fft_coefficient__attr_real__coeff_0' LIMIT 1), 0.0) AS fft_r0_z,
				(COALESCE(fe.features.fft_coefficient__attr_imag__coeff_0, 0.0) - (SELECT COALESCE(mean_value, 0.0) FROM statistics_lookup WHERE feature_name = 'fft_coefficient__attr_imag__coeff_0' LIMIT 1)) / NULLIF((SELECT COALESCE(std_value, 1.0) FROM statistics_lookup WHERE feature_name = 'fft_coefficient__attr_imag__coeff_0' LIMIT 1), 0.0) AS fft_i0_z,
				(COALESCE(fe.features.fft_coefficient__attr_real__coeff_1, 0.0) - (SELECT COALESCE(mean_value, 0.0) FROM statistics_lookup WHERE feature_name = 'fft_coefficient__attr_real__coeff_1' LIMIT 1)) / NULLIF((SELECT COALESCE(std_value, 1.0) FROM statistics_lookup WHERE feature_name = 'fft_coefficient__attr_real__coeff_1' LIMIT 1), 0.0) AS fft_r1_z,
				(COALESCE(fe.features.fft_coefficient__attr_imag__coeff_1, 0.0) - (SELECT COALESCE(mean_value, 0.0) FROM statistics_lookup WHERE feature_name = 'fft_coefficient__attr_imag__coeff_1' LIMIT 1)) / NULLIF((SELECT COALESCE(std_value, 1.0) FROM statistics_lookup WHERE feature_name = 'fft_coefficient__attr_imag__coeff_1' LIMIT 1), 0.0) AS fft_i1_z,
				(COALESCE(fe.features.fft_coefficient__attr_real__coeff_2, 0.0) - (SELECT COALESCE(mean_value, 0.0) FROM statistics_lookup WHERE feature_name = 'fft_coefficient__attr_real__coeff_2' LIMIT 1)) / NULLIF((SELECT COALESCE(std_value, 1.0) FROM statistics_lookup WHERE feature_name = 'fft_coefficient__attr_real__coeff_2' LIMIT 1), 0.0) AS fft_r2_z,
				(COALESCE(fe.features.fft_coefficient__attr_imag__coeff_2, 0.0) - (SELECT COALESCE(mean_value, 0.0) FROM statistics_lookup WHERE feature_name = 'fft_coefficient__attr_imag__coeff_2' LIMIT 1)) / NULLIF((SELECT COALESCE(std_value, 1.0) FROM statistics_lookup WHERE feature_name = 'fft_coefficient__attr_imag__coeff_2' LIMIT 1), 0.0) AS fft_i2_z,
				(COALESCE(fe.features.fft_aggregated__aggtype_mean, 0.0) - (SELECT COALESCE(mean_value, 0.0) FROM statistics_lookup WHERE feature_name = 'fft_aggregated__aggtype_mean' LIMIT 1)) / NULLIF((SELECT COALESCE(std_value, 1.0) FROM statistics_lookup WHERE feature_name = 'fft_aggregated__aggtype_mean' LIMIT 1), 0.0) AS fft_mean_z,
				(COALESCE(fe.features.fft_aggregated__aggtype_variance, 0.0) - (SELECT COALESCE(mean_value, 0.0) FROM statistics_lookup WHERE feature_name = 'fft_aggregated__aggtype_variance' LIMIT 1)) / NULLIF((SELECT COALESCE(std_value, 1.0) FROM statistics_lookup WHERE feature_name = 'fft_aggregated__aggtype_variance' LIMIT 1), 0.0) AS fft_var_z,
				(COALESCE(fe.features.autocorrelation__lag_1, 0.0) - (SELECT COALESCE(mean_value, 0.0) FROM statistics_lookup WHERE feature_name = 'autocorrelation__lag_1' LIMIT 1)) / NULLIF((SELECT COALESCE(std_value, 1.0) FROM statistics_lookup WHERE feature_name = 'autocorrelation__lag_1' LIMIT 1), 0.0) AS ac1_z,
				(COALESCE(fe.features.autocorrelation__lag_7, 0.0) - (SELECT COALESCE(mean_value, 0.0) FROM statistics_lookup WHERE feature_name = 'autocorrelation__lag_7' LIMIT 1)) / NULLIF((SELECT COALESCE(std_value, 1.0) FROM statistics_lookup WHERE feature_name = 'autocorrelation__lag_7' LIMIT 1), 0.0) AS ac7_z,
				(COALESCE(fe.features.autocorrelation__lag_14, 0.0) - (SELECT COALESCE(mean_value, 0.0) FROM statistics_lookup WHERE feature_name = 'autocorrelation__lag_14' LIMIT 1)) / NULLIF((SELECT COALESCE(std_value, 1.0) FROM statistics_lookup WHERE feature_name = 'autocorrelation__lag_14' LIMIT 1), 0.0) AS ac14_z,
				(COALESCE(fe.features.autocorrelation__lag_28, 0.0) - (SELECT COALESCE(mean_value, 0.0) FROM statistics_lookup WHERE feature_name = 'autocorrelation__lag_28' LIMIT 1)) / NULLIF((SELECT COALESCE(std_value, 1.0) FROM statistics_lookup WHERE feature_name = 'autocorrelation__lag_28' LIMIT 1), 0.0) AS ac28_z,
				(COALESCE(p2c.receipt_ratio, 0.0) - (SELECT COALESCE(mean_value, 0.0) FROM statistics_lookup WHERE feature_name = 'movement_type_receipt_ratio' LIMIT 1)) / NULLIF((SELECT COALESCE(std_value, 1.0) FROM statistics_lookup WHERE feature_name = 'movement_type_receipt_ratio' LIMIT 1), 0.0) AS receipt_ratio_z,
				(COALESCE(p2c.reversal_ratio, 0.0) - (SELECT COALESCE(mean_value, 0.0) FROM statistics_lookup WHERE feature_name = 'movement_type_reversal_ratio' LIMIT 1)) / NULLIF((SELECT COALESCE(std_value, 1.0) FROM statistics_lookup WHERE feature_name = 'movement_type_reversal_ratio' LIMIT 1), 0.0) AS reversal_ratio_z,
				(COALESCE(p2c.weekend_ratio, 0.0) - (SELECT COALESCE(mean_value, 0.0) FROM statistics_lookup WHERE feature_name = 'weekday_weekend_ratio' LIMIT 1)) / NULLIF((SELECT COALESCE(std_value, 1.0) FROM statistics_lookup WHERE feature_name = 'weekday_weekend_ratio' LIMIT 1), 0.0) AS weekend_ratio_z,
				(COALESCE(p2c.weekday_concentration, 0.0) - (SELECT COALESCE(mean_value, 0.0) FROM statistics_lookup WHERE feature_name = 'weekday_concentration' LIMIT 1)) / NULLIF((SELECT COALESCE(std_value, 1.0) FROM statistics_lookup WHERE feature_name = 'weekday_concentration' LIMIT 1), 0.0) AS weekday_conc_z,
				(CASE
					WHEN ABS(COALESCE(fe.features.mean, 1.0)) > 1e-8 THEN
						ABS(COALESCE(fe.features.linear_trend__slope, 0.0)) / ABS(COALESCE(fe.features.mean, 1.0))
					ELSE 0.0
				END - (SELECT COALESCE(mean_value, 0.0) FROM statistics_lookup WHERE feature_name = 'lifecycle_trend_strength' LIMIT 1)) / NULLIF((SELECT COALESCE(std_value, 1.0) FROM statistics_lookup WHERE feature_name = 'lifecycle_trend_strength' LIMIT 1), 0.0) AS trend_strength_z,
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
			SELECT
				material_id,
				ARRAY[
					COALESCE(mean_z, 0.0), COALESCE(median_z, 0.0), COALESCE(variance_z, 0.0), COALESCE(std_z, 0.0),
					COALESCE(slope_z, 0.0), COALESCE(intercept_z, 0.0), COALESCE(sum_z, 0.0), COALESCE(length_z, 0.0),
					COALESCE(first_max_z, 0.0), COALESCE(last_max_z, 0.0),
					COALESCE(cv_z, 0.0), COALESCE(skew_z, 0.0), COALESCE(kurt_z, 0.0), COALESCE(range_z, 0.0),
					COALESCE(ratio_z, 0.0), COALESCE(approx_ent_z, 0.0), COALESCE(sample_ent_z, 0.0), COALESCE(benford_z, 0.0),
					COALESCE(fft_r0_z, 0.0), COALESCE(fft_i0_z, 0.0), COALESCE(fft_r1_z, 0.0), COALESCE(fft_i1_z, 0.0),
					COALESCE(fft_r2_z, 0.0), COALESCE(fft_i2_z, 0.0), COALESCE(fft_mean_z, 0.0), COALESCE(fft_var_z, 0.0),
					COALESCE(ac1_z, 0.0), COALESCE(ac7_z, 0.0), COALESCE(ac14_z, 0.0), COALESCE(ac28_z, 0.0),
					0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0,
					0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0,
					0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0,
					0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0,
					0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0,
					0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0,
					0.0, 0.0,
					COALESCE(receipt_ratio_z, 0.0), COALESCE(reversal_ratio_z, 0.0),
					COALESCE(weekend_ratio_z, 0.0), COALESCE(weekday_conc_z, 0.0),
					COALESCE(trend_strength_z, 0.0), COALESCE(growth_indicator_z, 0.0),
					0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0,
					0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0,
					0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0
				]::FLOAT[128] AS raw_embedding
			FROM normalized_features
		),
		l2_normalization AS (
			SELECT
				material_id,
				raw_embedding,
				SQRT(SUM(v * v)) AS l2_norm
			FROM raw_embedding_vectors
			CROSS JOIN LATERAL UNNEST(raw_embedding) AS t(v)
			GROUP BY material_id, raw_embedding
		),
		normalized_embedding_vectors AS (
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
	)", movements_table, time_window_days, batch_size, batch_size, batch_size, batch_offset,
	    movements_table, time_window_days, batch_size, batch_offset,
	    movements_table, time_window_days);

	return ParseSubquery(sql, context.GetParserOptions(), "Failed to parse compute_transactional_embeddings query");
}

//------------------------------------------------------------------------------
// Module Registration
//------------------------------------------------------------------------------

void RegisterTransactionalEmbeddingFunctions(ExtensionLoader &loader) {
	// compute_transactional_embeddings
	TableFunction compute_trans("compute_transactional_embeddings", {}, nullptr, nullptr);
	compute_trans.bind_replace = ComputeTransactionalEmbeddingsBindReplace;
	compute_trans.named_parameters["movements_table"] = LogicalType::VARCHAR;
	compute_trans.named_parameters["material_column"] = LogicalType::VARCHAR;
	compute_trans.named_parameters["date_column"] = LogicalType::VARCHAR;
	compute_trans.named_parameters["quantity_column"] = LogicalType::VARCHAR;
	compute_trans.named_parameters["time_window_days"] = LogicalType::BIGINT;
	compute_trans.named_parameters["batch_size"] = LogicalType::BIGINT;
	compute_trans.named_parameters["batch_offset"] = LogicalType::BIGINT;
	loader.RegisterFunction(compute_trans);
}

} // namespace anofox
} // namespace duckdb
