#include "modules/predecessor_inference.hpp"
#include "modules/similarity_search.hpp"
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

	// (material_id) or (materials_table := ...), the batch form with a leading query_id column.
	// Named: lookback_months (24), min_similarity (0.3), min_confidence (0.5), lag_weeks (8),
	// min_overlapping_weeks (8), bom_table ('bom_items'), movements_table ('goods_movements')
	auto q = QueryMaterials::FromInput(context, input, "infer_predecessors", 1);

	int64_t lookback_months = 24;
	double min_similarity = 0.3;
	double min_confidence = 0.5;
	int64_t lag_weeks = 8;
	int64_t min_overlapping_weeks = 8;
	string bom_table = "bom_items";
	string movements_table = "goods_movements";

	if (auto value = duckdb_compat::FindNamedParameter(input.named_parameters, "lookback_months");
	    value && !value->IsNull()) {
		lookback_months = (*value).GetValue<int64_t>();
	}
	if (auto value = duckdb_compat::FindNamedParameter(input.named_parameters, "min_similarity");
	    value && !value->IsNull()) {
		min_similarity = (*value).GetValue<double>();
	}
	if (auto value = duckdb_compat::FindNamedParameter(input.named_parameters, "min_confidence");
	    value && !value->IsNull()) {
		min_confidence = (*value).GetValue<double>();
	}
	if (auto value = duckdb_compat::FindNamedParameter(input.named_parameters, "lag_weeks"); value && !value->IsNull()) {
		lag_weeks = (*value).GetValue<int64_t>();
	}
	if (auto value = duckdb_compat::FindNamedParameter(input.named_parameters, "min_overlapping_weeks");
	    value && !value->IsNull()) {
		min_overlapping_weeks = (*value).GetValue<int64_t>();
	}
	if (auto value = duckdb_compat::FindNamedParameter(input.named_parameters, "bom_table"); value && !value->IsNull()) {
		bom_table = (*value).GetValue<string>();
	}
	if (auto value = duckdb_compat::FindNamedParameter(input.named_parameters, "movements_table");
	    value && !value->IsNull()) {
		movements_table = (*value).GetValue<string>();
	}
	bom_table = ValidateSQLIdentifierPath(bom_table, "bom_table");
	ValidateTableColumns(context, bom_table, {"parent_id", "child_id"}, "bom_table");
	movements_table = ValidateSQLIdentifierPath(movements_table, "movements_table");
	ValidateTableColumns(context, movements_table, {"material_id", "movement_date", "quantity"}, "movements_table");
	ValidateDateColumn(context, movements_table, "movement_date", "movements_table");
	ValidateNumericColumn(context, movements_table, "quantity", "movements_table");

	// The 100 most similar materials per query: the batch form of the Jaccard search gives them
	// for every query at once, with a query_id column.
	string similar =
	    q.batch ? StringUtil::Format("SELECT query_id, material_id, similarity FROM find_similar_materials_jaccard("
	                                 "100, materials_table := %s, min_similarity := %.17g, bom_table := %s)",
	                                 QuoteSQLStringLiteral(q.table), min_similarity, QuoteSQLStringLiteral(bom_table))
	            : StringUtil::Format("SELECT %s AS query_id, material_id, similarity FROM find_similar_materials_jaccard("
	                                 "%s, 100, min_similarity := %.17g, bom_table := %s)",
	                                 q.literal, q.literal, min_similarity, QuoteSQLStringLiteral(bom_table));

	string sql = StringUtil::Format(R"(
		SELECT * FROM (
			WITH
				queries AS (%s),
				query_ts AS (
					SELECT q.query_id, m.movement_date, m.quantity
					FROM query_table(%s) m JOIN queries q ON %s
				),
				query_boundaries AS (
					SELECT query_id, MIN(movement_date) AS start_date, MAX(movement_date) AS end_date
					FROM query_ts
					GROUP BY query_id
				),
				similar_mats AS (%s),
				candidate_ts AS (
					SELECT
						s.query_id,
						gm.material_id,
						gm.movement_date,
						gm.quantity,
						MIN(gm.movement_date) OVER (PARTITION BY s.query_id, gm.material_id) AS cand_first_usage,
						MAX(gm.movement_date) OVER (PARTITION BY s.query_id, gm.material_id) AS cand_last_usage
					FROM query_table(%s) gm
					INNER JOIN similar_mats s ON gm.material_id = s.material_id
					INNER JOIN query_boundaries b ON b.query_id = s.query_id
					WHERE gm.movement_date >= b.start_date - INTERVAL (%lld) MONTHS
				),
				query_weekly AS (
					SELECT query_id, DATE_TRUNC('week', movement_date) AS week, SUM(quantity) AS weekly_qty
					FROM query_ts
					GROUP BY query_id, DATE_TRUNC('week', movement_date)
				),
				candidate_weekly AS (
					SELECT
						query_id,
						material_id,
						DATE_TRUNC('week', movement_date) AS week,
						SUM(quantity) AS weekly_qty,
						MIN(cand_first_usage) AS first_usage,
						MAX(cand_last_usage) AS last_usage
					FROM candidate_ts
					GROUP BY query_id, material_id, DATE_TRUNC('week', movement_date)
				),
				lagged_join AS (
					SELECT
						c.query_id,
						c.material_id,
						c.weekly_qty AS cand_qty,
						q.weekly_qty AS query_qty,
						c.first_usage,
						c.last_usage
					FROM candidate_weekly c
					INNER JOIN query_weekly q ON q.query_id = c.query_id AND c.week = q.week - INTERVAL (%lld) WEEKS
				),
				correlations AS (
					SELECT
						query_id,
						material_id,
						CORR(cand_qty, query_qty) AS correlation,
						COUNT(*) AS overlapping_weeks,
						MIN(first_usage) AS first_usage,
						MAX(last_usage) AS last_usage
					FROM lagged_join
					GROUP BY query_id, material_id
					HAVING COUNT(*) >= %lld
				),
				scored AS (
					SELECT
						c.query_id,
						c.material_id,
						c.correlation,
						s.similarity,
						c.first_usage,
						c.last_usage,
						b.start_date AS query_start,
						c.overlapping_weeks,
						CASE
							WHEN c.last_usage >= b.start_date
							 AND c.last_usage <= b.start_date + INTERVAL 6 MONTHS
							THEN 1.0 - (DATEDIFF('day', b.start_date, c.last_usage) / 180.0) * 0.3
							WHEN c.last_usage >= b.start_date - INTERVAL 3 MONTHS
							 AND c.last_usage < b.start_date
							THEN 0.7 - (DATEDIFF('day', c.last_usage, b.start_date) / 90.0) * 0.3
							WHEN c.last_usage > b.start_date + INTERVAL 6 MONTHS
							THEN 0.3
							ELSE 0.0
						END AS temporal_score
					FROM correlations c
					INNER JOIN similar_mats s ON s.query_id = c.query_id AND c.material_id = s.material_id
					INNER JOIN query_boundaries b ON b.query_id = c.query_id
					WHERE c.correlation IS NOT NULL
				),
				with_confidence AS (
					SELECT
						query_id,
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
				%s
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
			ORDER BY %s confidence DESC, material_id
		)
	)",
	                                q.QueriesSQL(), QuoteSQLStringLiteral(movements_table),
	                                q.Matches("m.material_id", "q"), similar, QuoteSQLStringLiteral(movements_table),
	                                lookback_months, lag_weeks, min_overlapping_weeks, q.batch ? "query_id," : "",
	                                min_confidence, q.batch ? "query_id," : "");

	return ParseSubquery(sql, context, "Failed to parse infer_predecessors query");
}

//------------------------------------------------------------------------------
// Module Registration
//------------------------------------------------------------------------------

void RegisterPredecessorInferenceFunctions(ExtensionLoader &loader) {
	// Two overloads: (material_id), and the batch form (materials_table := ...) with a leading
	// query_id column, because a table function takes constants, not a column of another table.
	TableFunctionSet set("infer_predecessors");
	vector<FunctionDescription> docs;
	for (bool batch : {false, true}) {
		vector<LogicalType> args = batch ? vector<LogicalType> {} : vector<LogicalType> {LogicalType::VARCHAR};
		TableFunction infer_pred("infer_predecessors", args, nullptr, nullptr);
		infer_pred.bind_replace = InferPredecessorsBindReplace;
		FunctionDescription desc;
		desc.description = "Identifies predecessor (superseded) materials for a given successor by combining BOM "
		                   "structural similarity with anti-correlated consumption patterns. Returns predecessor "
		                   "candidates ranked by confidence score along with correlation, similarity, and temporal "
		                   "overlap data. Useful for lifecycle transition planning and inventory run-down. Batch form: "
		                   "infer_predecessors(materials_table := 'tbl') infers them for every material_id of the table "
		                   "in one query and returns query_id (the successor) in front of the same columns.";
		if (batch) {
			desc.examples = {"SELECT * FROM infer_predecessors(materials_table := 'new_products');"};
		} else {
			desc.examples = {"SELECT * FROM infer_predecessors('NEW-PART');",
			                 "SELECT * FROM infer_predecessors('NEW-PART', lookback_months := 24, min_similarity := 0.5, "
			                 "min_confidence := 0.6, lag_weeks := 4);"};
		}
		desc.categories = {"similarity", "predecessor", "lifecycle"};
		// Named parameters must be listed too: a non-empty parameter_names replaces the
		// WHOLE rendered list and pads any shortfall with col2, col3, ... They come out in
		// case_insensitive_map_t order, which is neither sorted nor guaranteed stable, so
		// test/sql/extension/function_documentation.test pins the rendered lists.
		std::initializer_list<NamedParameterSpec> named = {
		    {"movements_table", LogicalType::VARCHAR}, {"bom_table", LogicalType::VARCHAR},
		    {"min_overlapping_weeks", LogicalType::BIGINT}, {"lag_weeks", LogicalType::BIGINT},
		    {"min_confidence", LogicalType::DOUBLE}, {"min_similarity", LogicalType::DOUBLE},
		    {"lookback_months", LogicalType::BIGINT}};
		if (batch) {
			AddDocumentedNamedParameters(infer_pred, desc, {}, named);
			AddDocumentedNamedParameters(infer_pred, desc, {}, {{"materials_table", LogicalType::VARCHAR}});
		} else {
			AddDocumentedNamedParameters(infer_pred, desc, {"material_id"}, named);
		}
		desc.parameter_types = args;
		set.AddFunction(infer_pred);
		docs.push_back(std::move(desc));
	}
	CreateTableFunctionInfo info(set);
	for (auto &d : docs) {
		info.descriptions.push_back(std::move(d));
	}
	duckdb_compat::NameFunctionsFromDescriptions(info);
	loader.RegisterFunction(std::move(info));
}

} // namespace anofox
} // namespace duckdb
