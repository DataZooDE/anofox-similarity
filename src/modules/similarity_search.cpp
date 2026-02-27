#include "modules/similarity_search.hpp"
#include "core/error_handling.hpp"
#include "core/sql_safety.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/parser/parser.hpp"
#include "duckdb/parser/tableref/subqueryref.hpp"
#include "duckdb/main/client_context.hpp"
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
// find_similar_materials_jaccard TableFunction
//------------------------------------------------------------------------------

static unique_ptr<TableRef> FindSimilarMaterialsJaccardBindReplace(ClientContext &context,
                                                                   TableFunctionBindInput &input) {
	PostHogTelemetry::Instance().CaptureFunctionExecution("find_similar_materials_jaccard");

	// Parameters:
	// 0: query_material_id (VARCHAR)
	// 1: k (BIGINT)
	// Named: min_similarity (DOUBLE, default 0.0)
	// Named: bom_table (VARCHAR, default 'bom_items')

	if (input.inputs.size() < 2) {
		throw BinderException("find_similar_materials_jaccard requires at least 2 arguments: query_material_id, k");
	}

	string query_material_id = input.inputs[0].GetValue<string>();
	int64_t k = input.inputs[1].GetValue<int64_t>();
	auto query_material_id_sql = QuoteSQLStringLiteral(query_material_id);

	// Get named parameters with defaults
	double min_similarity = 0.0;
	string bom_table = "bom_items";

	if (input.named_parameters.count("min_similarity")) {
		min_similarity = input.named_parameters.at("min_similarity").GetValue<double>();
	}
	if (input.named_parameters.count("bom_table")) {
		bom_table = input.named_parameters.at("bom_table").GetValue<string>();
	}
	bom_table = ValidateSQLIdentifierPath(bom_table, "bom_table");

	string sql = StringUtil::Format(R"(
		WITH
			material_components AS MATERIALIZED (
				SELECT * FROM aggregate_material_components('%s')
			),
			query_mat AS (
				SELECT components AS query_components
				FROM material_components
				WHERE material_id = %s
			),
			computed_similarity AS (
				SELECT
					mc.material_id,
					jaccard_similarity(qm.query_components, mc.components) AS similarity,
					len(list_intersect(qm.query_components, mc.components))::BIGINT AS shared_components,
					len(list_distinct(list_concat(qm.query_components, mc.components)))::BIGINT AS total_components
				FROM material_components mc, query_mat qm
				WHERE mc.material_id != %s
			)
		SELECT material_id, similarity, shared_components, total_components
		FROM computed_similarity
		WHERE similarity >= %f
		ORDER BY similarity DESC
		LIMIT %lld
	)",
	                                bom_table, query_material_id_sql, query_material_id_sql, min_similarity, k);

	return ParseSubquery(sql, context.GetParserOptions(), "Failed to parse find_similar_materials_jaccard query");
}

//------------------------------------------------------------------------------
// find_similar_materials_wl_kernel TableFunction
//------------------------------------------------------------------------------

static unique_ptr<TableRef> FindSimilarMaterialsWLKernelBindReplace(ClientContext &context,
                                                                    TableFunctionBindInput &input) {
	PostHogTelemetry::Instance().CaptureFunctionExecution("find_similar_materials_wl_kernel");

	// Parameters:
	// 0: query_material_id (VARCHAR)
	// 1: k (BIGINT)
	// Named: iterations (BIGINT, default 3)
	// Named: min_similarity (DOUBLE, default 0.0)
	// Named: bom_table (VARCHAR, default 'bom_items')

	if (input.inputs.size() < 2) {
		throw BinderException("find_similar_materials_wl_kernel requires at least 2 arguments: query_material_id, k");
	}

	string query_material_id = input.inputs[0].GetValue<string>();
	int64_t k = input.inputs[1].GetValue<int64_t>();
	auto query_material_id_sql = QuoteSQLStringLiteral(query_material_id);

	// Get named parameters with defaults
	int64_t iterations = 3;
	double min_similarity = 0.0;
	string bom_table = "bom_items";

	if (input.named_parameters.count("iterations")) {
		iterations = input.named_parameters.at("iterations").GetValue<int64_t>();
	}
	if (input.named_parameters.count("min_similarity")) {
		min_similarity = input.named_parameters.at("min_similarity").GetValue<double>();
	}
	if (input.named_parameters.count("bom_table")) {
		bom_table = input.named_parameters.at("bom_table").GetValue<string>();
	}
	bom_table = ValidateSQLIdentifierPath(bom_table, "bom_table");

	string sql =
	    StringUtil::Format(R"(
		WITH
			all_materials AS (
				SELECT DISTINCT parent_id AS material_id
				FROM query_table('%s')
			),
			computed_similarity AS (
				SELECT
					am.material_id,
						wl_kernel_similarity(%s, am.material_id, %lld, '%s') AS similarity
				FROM all_materials am
					WHERE am.material_id != %s
			)
		SELECT material_id, similarity
		FROM computed_similarity
		WHERE similarity >= %f
		ORDER BY similarity DESC
		LIMIT %lld
	)",
		                       bom_table, query_material_id_sql, iterations, bom_table, query_material_id_sql, min_similarity,
		                       k);

	return ParseSubquery(sql, context.GetParserOptions(), "Failed to parse find_similar_materials_wl_kernel query");
}

//------------------------------------------------------------------------------
// cold_start_analogs TableFunction
//------------------------------------------------------------------------------

static unique_ptr<TableRef> ColdStartAnalogsBindReplace(ClientContext &context, TableFunctionBindInput &input) {
	PostHogTelemetry::Instance().CaptureFunctionExecution("cold_start_analogs");

	// Parameters:
	// 0: query_material_id (VARCHAR)
	// 1: k (BIGINT)
	// Named: min_history_months (BIGINT, default 0)
	// Named: min_similarity (DOUBLE, default 0.0)
	// Named: bom_table (VARCHAR, default 'bom_items')
	// Named: movements_table (VARCHAR, default 'goods_movements')

	if (input.inputs.size() < 2) {
		throw BinderException("cold_start_analogs requires at least 2 arguments: query_material_id, k");
	}

	string query_material_id = input.inputs[0].GetValue<string>();
	int64_t k = input.inputs[1].GetValue<int64_t>();
	auto query_material_id_sql = QuoteSQLStringLiteral(query_material_id);

	// Get named parameters with defaults
	int64_t min_history_months = 0;
	double min_similarity = 0.0;
	string bom_table = "bom_items";
	string movements_table = "goods_movements";

	if (input.named_parameters.count("min_history_months")) {
		min_history_months = input.named_parameters.at("min_history_months").GetValue<int64_t>();
	}
	if (input.named_parameters.count("min_similarity")) {
		min_similarity = input.named_parameters.at("min_similarity").GetValue<double>();
	}
	if (input.named_parameters.count("bom_table")) {
		bom_table = input.named_parameters.at("bom_table").GetValue<string>();
	}
	if (input.named_parameters.count("movements_table")) {
		movements_table = input.named_parameters.at("movements_table").GetValue<string>();
	}
	bom_table = ValidateSQLIdentifierPath(bom_table, "bom_table");
	movements_table = ValidateSQLIdentifierPath(movements_table, "movements_table");

	string sql = StringUtil::Format(R"(
		SELECT * FROM (
			WITH
				material_components AS MATERIALIZED (
					SELECT * FROM aggregate_material_components('%s')
				),
				query_mat AS (
					SELECT components AS query_components
					FROM material_components
						WHERE material_id = %s
				),
				computed_similarity AS (
					SELECT
						mc.material_id,
						jaccard_similarity(qm.query_components, mc.components) AS similarity,
						len(list_intersect(qm.query_components, mc.components))::BIGINT AS shared_components,
						len(list_distinct(list_concat(qm.query_components, mc.components)))::BIGINT AS total_components
					FROM material_components mc, query_mat qm
						WHERE mc.material_id != %s
				),
				material_history AS (
					SELECT
						material_id,
						MIN(movement_date) AS first_usage,
						MAX(movement_date) AS last_usage,
						(EXTRACT(YEAR FROM MAX(movement_date)) - EXTRACT(YEAR FROM MIN(movement_date))) * 12 +
						(EXTRACT(MONTH FROM MAX(movement_date)) - EXTRACT(MONTH FROM MIN(movement_date))) AS history_months
					FROM query_table('%s')
					GROUP BY material_id
				),
				with_history AS (
					SELECT
						cs.material_id,
						cs.similarity,
						cs.shared_components,
						cs.total_components,
						COALESCE(mh.history_months, 0)::BIGINT AS history_months,
						mh.first_usage,
						mh.last_usage
					FROM computed_similarity cs
					LEFT JOIN material_history mh ON cs.material_id = mh.material_id
					WHERE cs.similarity >= %f
					  AND COALESCE(mh.history_months, 0) >= %lld
				)
			SELECT material_id, similarity, shared_components, total_components,
			       history_months, first_usage, last_usage
			FROM with_history
			ORDER BY similarity DESC
			LIMIT %lld
		)
	)",
	                                bom_table, query_material_id_sql, query_material_id_sql, movements_table, min_similarity,
	                                min_history_months, k);

	return ParseSubquery(sql, context.GetParserOptions(), "Failed to parse cold_start_analogs query");
}

//------------------------------------------------------------------------------
// Module Registration
//------------------------------------------------------------------------------

void RegisterSimilaritySearchFunctions(ExtensionLoader &loader) {
	// find_similar_materials_jaccard
	TableFunction find_similar_jaccard("find_similar_materials_jaccard", {LogicalType::VARCHAR, LogicalType::BIGINT},
	                                   nullptr, nullptr);
	find_similar_jaccard.bind_replace = FindSimilarMaterialsJaccardBindReplace;
	find_similar_jaccard.named_parameters["min_similarity"] = LogicalType::DOUBLE;
	find_similar_jaccard.named_parameters["bom_table"] = LogicalType::VARCHAR;
	loader.RegisterFunction(find_similar_jaccard);

	// find_similar_materials_wl_kernel
	TableFunction find_similar_wl("find_similar_materials_wl_kernel", {LogicalType::VARCHAR, LogicalType::BIGINT},
	                              nullptr, nullptr);
	find_similar_wl.bind_replace = FindSimilarMaterialsWLKernelBindReplace;
	find_similar_wl.named_parameters["iterations"] = LogicalType::BIGINT;
	find_similar_wl.named_parameters["min_similarity"] = LogicalType::DOUBLE;
	find_similar_wl.named_parameters["bom_table"] = LogicalType::VARCHAR;
	loader.RegisterFunction(find_similar_wl);

	// cold_start_analogs
	TableFunction cold_start("cold_start_analogs", {LogicalType::VARCHAR, LogicalType::BIGINT}, nullptr, nullptr);
	cold_start.bind_replace = ColdStartAnalogsBindReplace;
	cold_start.named_parameters["min_history_months"] = LogicalType::BIGINT;
	cold_start.named_parameters["min_similarity"] = LogicalType::DOUBLE;
	cold_start.named_parameters["bom_table"] = LogicalType::VARCHAR;
	cold_start.named_parameters["movements_table"] = LogicalType::VARCHAR;
	loader.RegisterFunction(cold_start);
}

} // namespace anofox
} // namespace duckdb
