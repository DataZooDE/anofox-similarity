#include "modules/similarity_search.hpp"
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
	PostHogTelemetry::Instance().RecordFunctionCall("find_similar_materials_jaccard");

	// Parameters:
	// 0: query_material_id (VARCHAR)
	// 1: k (BIGINT)
	// Named: min_similarity (DOUBLE, default 0.0)
	// Named: bom_table (VARCHAR, default 'bom_items')

	if (input.inputs.size() < 2) {
		throw BinderException("find_similar_materials_jaccard requires at least 2 arguments: query_material_id, k");
	}
	if (input.inputs[0].IsNull()) {
		throw BinderException("find_similar_materials_jaccard: query_material_id must not be NULL");
	}
	if (input.inputs[1].IsNull()) {
		throw BinderException("find_similar_materials_jaccard: k must not be NULL");
	}

	string query_material_id = input.inputs[0].GetValue<string>();
	int64_t k = input.inputs[1].GetValue<int64_t>();
	auto query_material_id_sql = QuoteSQLStringLiteral(query_material_id);

	// Get named parameters with defaults
	double min_similarity = 0.0;
	string bom_table = "bom_items";

	if (input.named_parameters.count("min_similarity") && !input.named_parameters.at("min_similarity").IsNull()) {
		min_similarity = input.named_parameters.at("min_similarity").GetValue<double>();
	}
	if (input.named_parameters.count("bom_table") && !input.named_parameters.at("bom_table").IsNull()) {
		bom_table = input.named_parameters.at("bom_table").GetValue<string>();
	}
	bom_table = ValidateSQLIdentifierPath(bom_table, "bom_table");
	ValidateTableColumns(context, bom_table, {"parent_id", "child_id"}, "bom_table");

	// The component aggregation is inlined (rather than calling the aggregate_material_components
	// macro) so this function is self-contained and works even on a read-only database, where the
	// SQL helper macros are not registered.
	string sql = StringUtil::Format(R"(
		WITH
			material_components AS MATERIALIZED (
				SELECT parent_id AS material_id, list(child_id ORDER BY child_id) AS components
				FROM query_table('%s')
				WHERE parent_id IS NOT NULL AND child_id IS NOT NULL
				GROUP BY parent_id
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
		WHERE similarity >= %.17g
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
	PostHogTelemetry::Instance().RecordFunctionCall("find_similar_materials_wl_kernel");

	// Parameters:
	// 0: query_material_id (VARCHAR)
	// 1: k (BIGINT)
	// Named: iterations (BIGINT, default 3)
	// Named: min_similarity (DOUBLE, default 0.0)
	// Named: bom_table (VARCHAR, default 'bom_items')

	if (input.inputs.size() < 2) {
		throw BinderException("find_similar_materials_wl_kernel requires at least 2 arguments: query_material_id, k");
	}
	if (input.inputs[0].IsNull()) {
		throw BinderException("find_similar_materials_wl_kernel: query_material_id must not be NULL");
	}
	if (input.inputs[1].IsNull()) {
		throw BinderException("find_similar_materials_wl_kernel: k must not be NULL");
	}

	string query_material_id = input.inputs[0].GetValue<string>();
	int64_t k = input.inputs[1].GetValue<int64_t>();
	auto query_material_id_sql = QuoteSQLStringLiteral(query_material_id);

	// Get named parameters with defaults
	int64_t iterations = 3;
	double min_similarity = 0.0;
	string bom_table = "bom_items";

	// A NULL iterations (like any NULL named tuning param) falls back to the default.
	if (input.named_parameters.count("iterations") && !input.named_parameters.at("iterations").IsNull()) {
		iterations = input.named_parameters.at("iterations").GetValue<int64_t>();
	}
	// Clamp to at least 1 so iterations <= 0 means "direct children only", consistent with the
	// wl_kernel_similarity scalar macro (which likewise treats <1 as 1).
	if (iterations < 1) {
		iterations = 1;
	}
	if (input.named_parameters.count("min_similarity") && !input.named_parameters.at("min_similarity").IsNull()) {
		min_similarity = input.named_parameters.at("min_similarity").GetValue<double>();
	}
	if (input.named_parameters.count("bom_table") && !input.named_parameters.at("bom_table").IsNull()) {
		bom_table = input.named_parameters.at("bom_table").GetValue<string>();
	}
	bom_table = ValidateSQLIdentifierPath(bom_table, "bom_table");
	ValidateTableColumns(context, bom_table, {"parent_id", "child_id"}, "bom_table");

	// Set-wise WL-kernel search: compute a depth-expanded component fingerprint for EVERY
	// material in a single recursive pass, then score every candidate against the query
	// fingerprint with a bounded weighted-Jaccard (sum(min)/sum(max)). This replaces the old
	// per-candidate correlated scalar-macro call, which crashed DuckDB's decorrelation on any
	// BOM with more than ~14 distinct parents, and it keeps every score in [0, 1].
	string sql = StringUtil::Format(R"(
		WITH RECURSIVE
			all_dfs(material_id, component, depth) AS (
				SELECT DISTINCT parent_id, child_id, 0
				FROM query_table('%s')
				-- UNION (not UNION ALL): dedup (material_id, component, depth) so a dense/cyclic BOM
				-- does not fan out into an O(nodes^4) walk-count explosion. The depth cap bounds depth;
				-- UNION bounds width. COUNT(DISTINCT depth) downstream is unaffected by the dedup.
				UNION
				SELECT a.material_id, b.child_id, a.depth + 1
				FROM all_dfs a
				JOIN query_table('%s') b ON a.component = b.parent_id
				-- Produce depths 0..(iterations-1), capped at depth 4 (5 levels), so this matches the
				-- wl_kernel_similarity scalar macro's bounded wl_fingerprint expansion exactly.
				WHERE a.depth < LEAST(%lld - 1, 4)
			),
			fingerprints AS (
				SELECT material_id, component, COUNT(DISTINCT depth) AS occ
				FROM all_dfs
				GROUP BY material_id, component
			),
			query_fp AS (
				SELECT component, occ FROM fingerprints WHERE material_id = %s
			),
			candidates AS (
				SELECT DISTINCT material_id FROM fingerprints WHERE material_id != %s
			),
			pair_components AS (
				-- candidate-side components (matched against the query where present)
				SELECT f.material_id, f.occ AS cocc, COALESCE(q.occ, 0) AS qocc
				FROM fingerprints f
				LEFT JOIN query_fp q ON f.component = q.component
				WHERE f.material_id != %s
				UNION ALL
				-- query-only components, contributed to every candidate that lacks them
				SELECT c.material_id, 0 AS cocc, q.occ AS qocc
				FROM candidates c
				CROSS JOIN query_fp q
				WHERE NOT EXISTS (
					SELECT 1 FROM fingerprints f2
					WHERE f2.material_id = c.material_id AND f2.component = q.component
				)
			),
			scored AS (
				SELECT
					material_id,
					SUM(LEAST(cocc, qocc))::DOUBLE
						/ NULLIF(SUM(GREATEST(cocc, qocc))::DOUBLE, 0.0) AS similarity
				FROM pair_components
				GROUP BY material_id
			)
		SELECT material_id, COALESCE(similarity, 0.0) AS similarity
		FROM scored
		WHERE COALESCE(similarity, 0.0) >= %.17g
		ORDER BY similarity DESC
		LIMIT %lld
	)",
	                                bom_table, bom_table, iterations, query_material_id_sql, query_material_id_sql,
	                                query_material_id_sql, min_similarity, k);

	return ParseSubquery(sql, context.GetParserOptions(), "Failed to parse find_similar_materials_wl_kernel query");
}

//------------------------------------------------------------------------------
// cold_start_analogs TableFunction
//------------------------------------------------------------------------------

static unique_ptr<TableRef> ColdStartAnalogsBindReplace(ClientContext &context, TableFunctionBindInput &input) {
	PostHogTelemetry::Instance().RecordFunctionCall("cold_start_analogs");

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
	if (input.inputs[0].IsNull()) {
		throw BinderException("cold_start_analogs: query_material_id must not be NULL");
	}
	if (input.inputs[1].IsNull()) {
		throw BinderException("cold_start_analogs: k must not be NULL");
	}

	string query_material_id = input.inputs[0].GetValue<string>();
	int64_t k = input.inputs[1].GetValue<int64_t>();
	auto query_material_id_sql = QuoteSQLStringLiteral(query_material_id);

	// Get named parameters with defaults
	int64_t min_history_months = 0;
	double min_similarity = 0.0;
	string bom_table = "bom_items";
	string movements_table = "goods_movements";

	if (input.named_parameters.count("min_history_months") && !input.named_parameters.at("min_history_months").IsNull()) {
		min_history_months = input.named_parameters.at("min_history_months").GetValue<int64_t>();
	}
	if (input.named_parameters.count("min_similarity") && !input.named_parameters.at("min_similarity").IsNull()) {
		min_similarity = input.named_parameters.at("min_similarity").GetValue<double>();
	}
	if (input.named_parameters.count("bom_table") && !input.named_parameters.at("bom_table").IsNull()) {
		bom_table = input.named_parameters.at("bom_table").GetValue<string>();
	}
	if (input.named_parameters.count("movements_table") && !input.named_parameters.at("movements_table").IsNull()) {
		movements_table = input.named_parameters.at("movements_table").GetValue<string>();
	}
	bom_table = ValidateSQLIdentifierPath(bom_table, "bom_table");
	ValidateTableColumns(context, bom_table, {"parent_id", "child_id"}, "bom_table");
	movements_table = ValidateSQLIdentifierPath(movements_table, "movements_table");
	ValidateTableColumns(context, movements_table, {"material_id", "movement_date", "quantity"}, "movements_table");

	string sql = StringUtil::Format(R"(
		SELECT * FROM (
			WITH
				material_components AS MATERIALIZED (
					SELECT parent_id AS material_id, list(child_id ORDER BY child_id) AS components
					FROM query_table('%s')
					WHERE parent_id IS NOT NULL AND child_id IS NOT NULL
					GROUP BY parent_id
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
					WHERE cs.similarity >= %.17g
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
	{
		TableFunction find_similar_jaccard("find_similar_materials_jaccard",
		                                   {LogicalType::VARCHAR, LogicalType::BIGINT}, nullptr, nullptr);
		find_similar_jaccard.bind_replace = FindSimilarMaterialsJaccardBindReplace;
		find_similar_jaccard.named_parameters["min_similarity"] = LogicalType::DOUBLE;
		find_similar_jaccard.named_parameters["bom_table"] = LogicalType::VARCHAR;

		CreateTableFunctionInfo info(find_similar_jaccard);
		FunctionDescription desc;
		desc.description = "Finds the k most structurally similar materials to a query material using exact "
		                   "Jaccard similarity on BOM component sets. Returns material_id, similarity, "
		                   "shared_components, and total_components. Filtered by min_similarity threshold.";
		desc.examples    = {"SELECT * FROM find_similar_materials_jaccard('MAT-001', 10);",
		                    "SELECT * FROM find_similar_materials_jaccard('MAT-001', 20, min_similarity := 0.3);"};
		desc.categories  = {"similarity", "search"};
		desc.parameter_names = {"material_id", "k"};
		desc.parameter_types = {LogicalType::VARCHAR, LogicalType::BIGINT};
		info.descriptions.push_back(std::move(desc));
		loader.RegisterFunction(std::move(info));
	}

	// find_similar_materials_wl_kernel
	{
		TableFunction find_similar_wl("find_similar_materials_wl_kernel",
		                              {LogicalType::VARCHAR, LogicalType::BIGINT}, nullptr, nullptr);
		find_similar_wl.bind_replace = FindSimilarMaterialsWLKernelBindReplace;
		find_similar_wl.named_parameters["iterations"] = LogicalType::BIGINT;
		find_similar_wl.named_parameters["min_similarity"] = LogicalType::DOUBLE;
		find_similar_wl.named_parameters["bom_table"] = LogicalType::VARCHAR;

		CreateTableFunctionInfo info(find_similar_wl);
		FunctionDescription desc;
		desc.description = "Finds the k most structurally similar materials using the Weisfeiler-Lehman graph "
		                   "kernel, which captures multi-hop BOM structure beyond direct component overlap. "
		                   "iterations controls the number of WL refinement rounds (default 3).";
		desc.examples    = {"SELECT * FROM find_similar_materials_wl_kernel('MAT-001', 10);",
		                    "SELECT * FROM find_similar_materials_wl_kernel('MAT-001', 10, iterations := 3, min_similarity := 0.2);"};
		desc.categories  = {"similarity", "search", "graph"};
		desc.parameter_names = {"material_id", "k"};
		desc.parameter_types = {LogicalType::VARCHAR, LogicalType::BIGINT};
		info.descriptions.push_back(std::move(desc));
		loader.RegisterFunction(std::move(info));
	}

	// cold_start_analogs
	{
		TableFunction cold_start("cold_start_analogs", {LogicalType::VARCHAR, LogicalType::BIGINT}, nullptr, nullptr);
		cold_start.bind_replace = ColdStartAnalogsBindReplace;
		cold_start.named_parameters["min_history_months"] = LogicalType::BIGINT;
		cold_start.named_parameters["min_similarity"] = LogicalType::DOUBLE;
		cold_start.named_parameters["bom_table"] = LogicalType::VARCHAR;
		cold_start.named_parameters["movements_table"] = LogicalType::VARCHAR;

		CreateTableFunctionInfo info(cold_start);
		FunctionDescription desc;
		desc.description = "Finds structural analogs for a cold-start material (one with little or no demand "
		                   "history). Returns established materials that are structurally similar and have at "
		                   "least min_history_months of goods movement data, enabling surrogate forecasting.";
		desc.examples    = {"SELECT * FROM cold_start_analogs('NEW-PART', 5);",
		                    "SELECT * FROM cold_start_analogs('NEW-PART', 10, min_history_months := 12, min_similarity := 0.4);"};
		desc.categories  = {"similarity", "search", "forecasting"};
		desc.parameter_names = {"material_id", "k"};
		desc.parameter_types = {LogicalType::VARCHAR, LogicalType::BIGINT};
		info.descriptions.push_back(std::move(desc));
		loader.RegisterFunction(std::move(info));
	}
}

} // namespace anofox
} // namespace duckdb
