#include "modules/predecessor_inference.hpp"
#include "duckdb_compat.hpp"
#include "core/named_parameters.hpp"
#include "core/error_handling.hpp"
#include "core/sql_safety.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/parser/parser.hpp"
#include "duckdb/parser/tableref/subqueryref.hpp"
#include "duckdb/main/client_context.hpp"
#include "telemetry.hpp"
#include "duckdb/parser/parsed_data/create_table_function_info.hpp"

namespace duckdb {
namespace anofox {

//------------------------------------------------------------------------------
// Helper: Parse SQL to SubqueryRef
//------------------------------------------------------------------------------

static unique_ptr<SubqueryRef> ParseSubquery(const string &query, ClientContext &context, const string &err_msg) {
	auto statements = duckdb_compat::ParseStatements(context, query);
	if (statements.size() != 1 || statements[0]->type != StatementType::SELECT_STATEMENT) {
		throw ParserException(err_msg);
	}
	auto &select_stmt = statements[0]->Cast<SelectStatement>();
	auto new_stmt = make_uniq<SelectStatement>();
	new_stmt->node = std::move(select_stmt.node);
	return make_uniq<SubqueryRef>(std::move(new_stmt));
}

//------------------------------------------------------------------------------
// infer_predecessors TableFunction
//------------------------------------------------------------------------------

static unique_ptr<TableRef> InferPredecessorsBindReplace(ClientContext &context, TableFunctionBindInput &input) {
	PostHogTelemetry::Instance().RecordFunctionCall("infer_predecessors");

	// Parameters:
	// 0: query_material_id (VARCHAR)
	// Named: lookback_months (BIGINT, default 24)
	// Named: min_similarity (DOUBLE, default 0.3)
	// Named: min_confidence (DOUBLE, default 0.5)
	// Named: lag_weeks (BIGINT, default 8)
	// Named: bom_table (VARCHAR, default 'bom_items')
	// Named: movements_table (VARCHAR, default 'goods_movements')

	if (input.inputs.size() < 1) {
		throw BinderException("infer_predecessors requires at least 1 argument: query_material_id");
	}
	if (input.inputs[0].IsNull()) {
		throw BinderException("infer_predecessors: query_material_id must not be NULL");
	}

	string query_material_id = input.inputs[0].GetValue<string>();
	auto query_material_id_sql = QuoteSQLStringLiteral(query_material_id);

	// Get named parameters with defaults
	int64_t lookback_months = 24;
	double min_similarity = 0.3;
	double min_confidence = 0.5;
	int64_t lag_weeks = 8;
	int64_t min_overlapping_weeks = 8;
	string bom_table = "bom_items";
	string movements_table = "goods_movements";

	if (auto value = duckdb_compat::FindNamedParameter(input.named_parameters, "lookback_months"); value && !value->IsNull()) {
		lookback_months = (*value).GetValue<int64_t>();
	}
	if (auto value = duckdb_compat::FindNamedParameter(input.named_parameters, "min_similarity"); value && !value->IsNull()) {
		min_similarity = (*value).GetValue<double>();
	}
	if (auto value = duckdb_compat::FindNamedParameter(input.named_parameters, "min_confidence"); value && !value->IsNull()) {
		min_confidence = (*value).GetValue<double>();
	}
	if (auto value = duckdb_compat::FindNamedParameter(input.named_parameters, "lag_weeks"); value && !value->IsNull()) {
		lag_weeks = (*value).GetValue<int64_t>();
	}
	if (auto value = duckdb_compat::FindNamedParameter(input.named_parameters, "min_overlapping_weeks"); value && !value->IsNull()) {
		min_overlapping_weeks = (*value).GetValue<int64_t>();
	}
	if (auto value = duckdb_compat::FindNamedParameter(input.named_parameters, "bom_table"); value && !value->IsNull()) {
		bom_table = (*value).GetValue<string>();
	}
	if (auto value = duckdb_compat::FindNamedParameter(input.named_parameters, "movements_table"); value && !value->IsNull()) {
		movements_table = (*value).GetValue<string>();
	}
	bom_table = ValidateSQLIdentifierPath(bom_table, "bom_table");
	ValidateTableColumns(context, bom_table, {"parent_id", "child_id"}, "bom_table");
	movements_table = ValidateSQLIdentifierPath(movements_table, "movements_table");
	ValidateTableColumns(context, movements_table, {"material_id", "movement_date", "quantity"}, "movements_table");
	ValidateDateColumn(context, movements_table, "movement_date", "movements_table");

	string sql = StringUtil::Format(R"(
		SELECT * FROM (
			WITH
				query_ts AS (
					SELECT
						movement_date,
						quantity,
						MIN(movement_date) OVER () AS query_start,
						MAX(movement_date) OVER () AS query_end
					FROM query_table('%s')
						WHERE material_id = %s
				),
				query_boundaries AS (
					SELECT
						MIN(query_start) AS start_date,
						MAX(query_end) AS end_date
					FROM query_ts
				),
				similar_mats AS (
					SELECT material_id, similarity, shared_components, total_components
					FROM find_similar_materials_jaccard(
							%s, 100,
						min_similarity := %.17g,
						bom_table := '%s'
					)
				),
				candidate_ts AS (
					SELECT
						gm.material_id,
						gm.movement_date,
						gm.quantity,
						MIN(gm.movement_date) OVER (PARTITION BY gm.material_id) AS cand_first_usage,
						MAX(gm.movement_date) OVER (PARTITION BY gm.material_id) AS cand_last_usage
					FROM query_table('%s') gm
					INNER JOIN similar_mats s ON gm.material_id = s.material_id
					WHERE gm.movement_date >= (SELECT start_date - INTERVAL (%lld) MONTHS FROM query_boundaries)
				),
				query_weekly AS (
					SELECT
						DATE_TRUNC('week', movement_date) AS week,
						SUM(quantity) AS weekly_qty
					FROM query_ts
					GROUP BY 1
				),
				candidate_weekly AS (
					SELECT
						material_id,
						DATE_TRUNC('week', movement_date) AS week,
						SUM(quantity) AS weekly_qty,
						MIN(cand_first_usage) AS first_usage,
						MAX(cand_last_usage) AS last_usage
					FROM candidate_ts
					GROUP BY material_id, DATE_TRUNC('week', movement_date)
				),
				lagged_join AS (
					SELECT
						c.material_id,
						c.week AS cand_week,
						c.weekly_qty AS cand_qty,
						q.weekly_qty AS query_qty,
						c.first_usage,
						c.last_usage
					FROM candidate_weekly c
					INNER JOIN query_weekly q ON c.week = q.week - INTERVAL (%lld) WEEKS
				),
				correlations AS (
					SELECT
						material_id,
						CORR(cand_qty, query_qty) AS correlation,
						COUNT(*) AS overlapping_weeks,
						MIN(first_usage) AS first_usage,
						MAX(last_usage) AS last_usage
					FROM lagged_join
					GROUP BY material_id
					HAVING COUNT(*) >= %lld
				),
				scored AS (
					SELECT
						c.material_id,
						c.correlation,
						s.similarity,
						c.first_usage,
						c.last_usage,
						(SELECT start_date FROM query_boundaries) AS query_start,
						c.overlapping_weeks,
						CASE
							WHEN c.last_usage >= (SELECT start_date FROM query_boundaries)
							 AND c.last_usage <= (SELECT start_date FROM query_boundaries) + INTERVAL 6 MONTHS
							THEN 1.0 - (DATEDIFF('day', (SELECT start_date FROM query_boundaries), c.last_usage) / 180.0) * 0.3
							WHEN c.last_usage >= (SELECT start_date FROM query_boundaries) - INTERVAL 3 MONTHS
							 AND c.last_usage < (SELECT start_date FROM query_boundaries)
							THEN 0.7 - (DATEDIFF('day', c.last_usage, (SELECT start_date FROM query_boundaries)) / 90.0) * 0.3
							WHEN c.last_usage > (SELECT start_date FROM query_boundaries) + INTERVAL 6 MONTHS
							THEN 0.3
							ELSE 0.0
						END AS temporal_score
					FROM correlations c
					INNER JOIN similar_mats s ON c.material_id = s.material_id
					WHERE c.correlation IS NOT NULL
				),
				with_confidence AS (
					SELECT
						material_id,
						0.4 * GREATEST(0, -correlation) + 0.3 * similarity + 0.3 * temporal_score AS confidence,
						correlation,
						similarity,
						temporal_score,
						first_usage,
						last_usage,
						query_start,
						overlapping_weeks
					FROM scored
					WHERE correlation < 0
					  AND temporal_score > 0.0
					  AND first_usage < query_start  -- a predecessor was in use before the successor started
				)
			SELECT
				material_id AS predecessor_id,
				ROUND(confidence, 4) AS confidence,
				ROUND(correlation, 4) AS correlation,
				ROUND(similarity, 4) AS similarity,
				ROUND(temporal_score, 4) AS temporal_score,
				first_usage AS predecessor_first_usage,
				last_usage AS predecessor_last_usage,
				query_start AS successor_start,
				overlapping_weeks
			FROM with_confidence
			WHERE confidence >= %f
			ORDER BY confidence DESC
		)
	)",
		                                movements_table, query_material_id_sql, query_material_id_sql, min_similarity, bom_table,
		                                movements_table, lookback_months, lag_weeks, min_overlapping_weeks, min_confidence);

	return ParseSubquery(sql, context, "Failed to parse infer_predecessors query");
}

//------------------------------------------------------------------------------
// Module Registration
//------------------------------------------------------------------------------

void RegisterPredecessorInferenceFunctions(ExtensionLoader &loader) {
	// infer_predecessors
	TableFunction infer_pred("infer_predecessors", {LogicalType::VARCHAR}, nullptr, nullptr);
	infer_pred.bind_replace = InferPredecessorsBindReplace;
	FunctionDescription desc;
	desc.description = "Identifies predecessor (superseded) materials for a given successor by combining BOM "
	                   "structural similarity with anti-correlated consumption patterns. Returns predecessor "
	                   "candidates ranked by confidence score along with correlation, similarity, and temporal "
	                   "overlap data. Useful for lifecycle transition planning and inventory run-down.";
	desc.examples    = {"SELECT * FROM infer_predecessors('NEW-PART');",
	                    "SELECT * FROM infer_predecessors('NEW-PART', lookback_months := 24, min_similarity := 0.5, "
	                    "min_confidence := 0.6, lag_weeks := 4);"};
	desc.categories  = {"similarity", "predecessor", "lifecycle"};
	// Named parameters must be listed too: a non-empty parameter_names replaces the
	// WHOLE rendered list and pads any shortfall with col2, col3, ... They come out in
	// case_insensitive_map_t order, which is neither sorted nor guaranteed stable, so
	// test/sql/extension/function_documentation.test pins the rendered lists.
	AddDocumentedNamedParameters(infer_pred, desc, {"material_id"},
	                             {{"movements_table", LogicalType::VARCHAR}, {"bom_table", LogicalType::VARCHAR},
	                              {"min_overlapping_weeks", LogicalType::BIGINT}, {"lag_weeks", LogicalType::BIGINT},
	                              {"min_confidence", LogicalType::DOUBLE}, {"min_similarity", LogicalType::DOUBLE},
	                              {"lookback_months", LogicalType::BIGINT}});
	desc.parameter_types = {LogicalType::VARCHAR};
	CreateTableFunctionInfo info(infer_pred);
	info.descriptions.push_back(std::move(desc));
	duckdb_compat::NameFunctionsFromDescriptions(info);
	loader.RegisterFunction(std::move(info));
}

} // namespace anofox
} // namespace duckdb
