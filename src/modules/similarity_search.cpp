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

#include <cmath>
#include <limits>
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
// Shared pieces
//------------------------------------------------------------------------------

static double ReadMinSimilarity(TableFunctionBindInput &input) {
	// Without min_similarity, rows scoring 0 (no shared component) are left out: a list of k
	// zeros reads like a result. min_similarity := 0 keeps them.
	double min_similarity = std::numeric_limits<double>::min();
	if (auto value = duckdb_compat::FindNamedParameter(input.named_parameters, "min_similarity");
	    value && !value->IsNull()) {
		min_similarity = (*value).GetValue<double>();
		if (!std::isfinite(min_similarity)) {
			throw BinderException("min_similarity must be a finite number");
		}
	}
	return min_similarity;
}

static string ReadTableParam(ClientContext &context, TableFunctionBindInput &input, const char *name,
                             const char *fallback, const vector<string> &columns) {
	string table = fallback;
	if (auto value = duckdb_compat::FindNamedParameter(input.named_parameters, name); value && !value->IsNull()) {
		table = (*value).GetValue<string>();
	}
	table = ValidateSQLIdentifierPath(table, name);
	ValidateTableColumns(context, table, columns, name);
	return table;
}

static int64_t ReadK(TableFunctionBindInput &input, const string &fn, const QueryMaterials &q) {
	idx_t k_index = q.batch ? 0 : 1;
	if (input.inputs.size() <= k_index || input.inputs[k_index].IsNull()) {
		throw BinderException("%s: k must not be NULL", fn);
	}
	auto k = input.inputs[k_index].GetValue<int64_t>();
	if (k < 0) { // the batch form would return no rows, the single form a LIMIT error
		throw BinderException("%s: k must not be negative", fn);
	}
	return k;
}

//! The k best rows of `from` (a CTE) per query: the single form keeps its columns and LIMIT; the
//! batch form adds query_id in front and ranks inside each query (an outer filter on the row
//! number, not QUALIFY on an alias, which DuckDB 2.0 fails to bind in some nestings).
static string TopK(const QueryMaterials &q, const string &columns, const string &from, const string &order,
                   int64_t k) {
	if (!q.batch) {
		return StringUtil::Format("SELECT %s FROM %s ORDER BY %s LIMIT %lld", columns, from, order, k);
	}
	return StringUtil::Format(
	    "SELECT query_id, %s FROM (SELECT *, row_number() OVER (PARTITION BY query_id ORDER BY %s) AS search_rank "
	    "FROM %s) ranked WHERE search_rank <= %lld ORDER BY query_id, search_rank",
	    columns, order, from, k);
}

//! CTEs edges, material_components, queries, query_mat, query_children, candidates and
//! computed_similarity(query_id, material_id, similarity, jaccard, shared_components,
//! total_components) of a Jaccard (or containment) search. A material that shares no component
//! scores 0, so when zero scores are left out the candidates come from an inverted join on the
//! shared components instead of every material; the scores are the same expressions either way.
static string JaccardCTEs(const string &bom_table, const QueryMaterials &q, bool containment, double min_similarity) {
	const bool keep_zero = min_similarity <= 0.0;
	string candidates = keep_zero ? "SELECT qm.query_id, mc.material_id FROM query_mat qm, material_components mc "
	                                "WHERE mc.material_id != qm.query_material"
	                              : "SELECT DISTINCT qc.query_id, e.parent_id AS material_id FROM query_children qc "
	                                "JOIN edges e ON e.child_id = qc.child_id WHERE e.parent_id != qc.query_material";
	string measure = containment ? "containment(qm.query_components, mc.components)"
	                             : "jaccard_similarity(qm.query_components, mc.components)";
	auto quoted = QuoteSQLStringLiteral(bom_table);
	return "\n edges AS MATERIALIZED (SELECT DISTINCT parent_id, child_id FROM query_table(" + quoted +
	       ") WHERE parent_id IS NOT NULL AND child_id IS NOT NULL),"
	       "\n material_components AS MATERIALIZED (SELECT parent_id AS material_id, list(child_id ORDER BY child_id) "
	       "AS components FROM query_table(" +
	       quoted +
	       ") WHERE parent_id IS NOT NULL AND child_id IS NOT NULL GROUP BY parent_id),"
	       "\n queries AS (" +
	       q.QueriesSQL() +
	       "),"
	       "\n query_mat AS (SELECT q.query_id, mc.material_id AS query_material, mc.components AS query_components "
	       "FROM queries q JOIN material_components mc ON " +
	       q.Matches("mc.material_id", "q") +
	       "),"
	       "\n query_children AS (SELECT query_id, query_material, unnest(query_components) AS child_id FROM "
	       "query_mat),"
	       "\n candidates AS (" +
	       candidates +
	       "),"
	       "\n computed_similarity AS (SELECT c.query_id, mc.material_id, " +
	       measure +
	       " AS similarity, jaccard_similarity(qm.query_components, mc.components) AS jaccard, "
	       "len(list_intersect(qm.query_components, mc.components))::BIGINT AS shared_components, "
	       "len(list_distinct(list_concat(qm.query_components, mc.components)))::BIGINT AS total_components "
	       "FROM candidates c JOIN query_mat qm ON qm.query_id = c.query_id "
	       "JOIN material_components mc ON mc.material_id = c.material_id)";
}

//------------------------------------------------------------------------------
// find_similar_materials_jaccard TableFunction
//------------------------------------------------------------------------------

static unique_ptr<TableRef> FindSimilarMaterialsJaccardBindReplace(ClientContext &context,
                                                                   TableFunctionBindInput &input) {
	PostHogTelemetry::Instance().RecordFunctionCall("find_similar_materials_jaccard");
	const string fn = "find_similar_materials_jaccard";

	// (material_id, k) or (k, materials_table := ...); named: bom_table, min_similarity, mode
	auto q = QueryMaterials::FromInput(context, input, fn, 2);
	int64_t k = ReadK(input, fn, q);
	double min_similarity = ReadMinSimilarity(input);
	string mode = "jaccard";
	if (auto value = duckdb_compat::FindNamedParameter(input.named_parameters, "mode"); value && !value->IsNull()) {
		mode = StringUtil::Lower((*value).GetValue<string>());
	}
	if (mode != "jaccard" && mode != "containment") {
		throw BinderException("find_similar_materials_jaccard: mode must be one of 'jaccard', 'containment', got '%s'",
		                      mode.c_str());
	}
	auto bom_table = ReadTableParam(context, input, "bom_table", "bom_items", {"parent_id", "child_id"});

	// Containment (coverage matching, HLD §4.1) is the share of the query's components found in
	// each material; Jaccard breaks its ties so the leaner material comes first. The component
	// aggregation is inlined (not the aggregate_material_components macro) so the function works
	// on a read-only database, where the SQL helper macros are not registered.
	const bool containment = mode == "containment";
	string sql = "WITH " + JaccardCTEs(bom_table, q, containment, min_similarity) +
	             StringUtil::Format(",\n filtered AS (SELECT * FROM computed_similarity WHERE similarity >= %.17g)\n",
	                                min_similarity) +
	             TopK(q, "material_id, similarity, shared_components, total_components", "filtered",
	                  containment ? "similarity DESC, jaccard DESC, material_id" : "similarity DESC, material_id", k);
	return ParseSubquery(sql, context, "Failed to parse find_similar_materials_jaccard query");
}

//------------------------------------------------------------------------------
// find_similar_materials_wl_kernel TableFunction
//------------------------------------------------------------------------------

static unique_ptr<TableRef> FindSimilarMaterialsWLKernelBindReplace(ClientContext &context,
                                                                    TableFunctionBindInput &input) {
	PostHogTelemetry::Instance().RecordFunctionCall("find_similar_materials_wl_kernel");
	const string fn = "find_similar_materials_wl_kernel";

	// (material_id, k) or (k, materials_table := ...); named: iterations, min_similarity, bom_table
	auto q = QueryMaterials::FromInput(context, input, fn, 2);
	int64_t k = ReadK(input, fn, q);
	int64_t iterations = 3;
	// A NULL iterations (like any NULL named tuning param) falls back to the default.
	if (auto value = duckdb_compat::FindNamedParameter(input.named_parameters, "iterations");
	    value && !value->IsNull()) {
		iterations = (*value).GetValue<int64_t>();
	}
	// Clamp to at least 1 so iterations <= 0 means "direct children only", consistent with the
	// wl_kernel_similarity scalar macro (which likewise treats <1 as 1).
	if (iterations < 1) {
		iterations = 1;
	}
	double min_similarity = ReadMinSimilarity(input);
	auto bom_table = ReadTableParam(context, input, "bom_table", "bom_items", {"parent_id", "child_id"});

	// Set-wise WL-kernel search: a depth-expanded component fingerprint for EVERY material in one
	// recursive pass, then a bounded weighted Jaccard per (query, candidate),
	// Σ min(occ) / (Σ q + Σ c − Σ min(occ)) with Σ min over the shared components, so every score
	// stays in [0, 1]. A candidate sharing no component scores 0: when zero scores are left out,
	// only candidates sharing a component are scored.
	const bool keep_zero = min_similarity <= 0.0;
	string pairs = keep_zero
	                   ? "SELECT qt.query_id, ct.material_id, COALESCE(s.smin, 0) AS smin, qt.qsum, ct.csum "
	                     "FROM query_totals qt JOIN material_totals ct ON ct.material_id != qt.query_material "
	                     "LEFT JOIN shared s ON s.query_id = qt.query_id AND s.material_id = ct.material_id"
	                   : "SELECT s.query_id, s.material_id, s.smin, qt.qsum, ct.csum FROM shared s "
	                     "JOIN query_totals qt ON qt.query_id = s.query_id "
	                     "JOIN material_totals ct ON ct.material_id = s.material_id";
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
			fingerprints AS MATERIALIZED (
				SELECT material_id, component, COUNT(DISTINCT depth) AS occ
				FROM all_dfs
				GROUP BY material_id, component
			),
			queries AS (%s),
			query_fp AS (
				SELECT q.query_id, f.material_id AS query_material, f.component, f.occ
				FROM queries q JOIN fingerprints f ON %s
			),
			query_totals AS (
				SELECT query_id, query_material, SUM(occ) AS qsum FROM query_fp GROUP BY query_id, query_material
			),
			material_totals AS (
				SELECT material_id, SUM(occ) AS csum FROM fingerprints GROUP BY material_id
			),
			shared AS (
				SELECT qf.query_id, f.material_id, SUM(LEAST(qf.occ, f.occ)) AS smin
				FROM query_fp qf JOIN fingerprints f ON f.component = qf.component
				WHERE f.material_id != qf.query_material
				GROUP BY qf.query_id, f.material_id
			),
			pairs AS (%s),
			scored AS (
				SELECT query_id, material_id,
				       COALESCE(smin::DOUBLE / NULLIF((qsum + csum - smin)::DOUBLE, 0.0), 0.0) AS similarity
				FROM pairs
			),
			filtered AS (SELECT * FROM scored WHERE similarity >= %.17g)
		)",
	                                bom_table, bom_table, iterations, q.QueriesSQL(), q.Matches("f.material_id", "q"),
	                                pairs, min_similarity) +
	             TopK(q, "material_id, similarity", "filtered", "similarity DESC, material_id", k);
	return ParseSubquery(sql, context, "Failed to parse find_similar_materials_wl_kernel query");
}

//------------------------------------------------------------------------------
// cold_start_analogs TableFunction
//------------------------------------------------------------------------------

static unique_ptr<TableRef> ColdStartAnalogsBindReplace(ClientContext &context, TableFunctionBindInput &input) {
	PostHogTelemetry::Instance().RecordFunctionCall("cold_start_analogs");
	const string fn = "cold_start_analogs";

	// (material_id, k) or (k, materials_table := ...); named: min_history_months, min_similarity,
	// bom_table, movements_table
	auto q = QueryMaterials::FromInput(context, input, fn, 2);
	int64_t k = ReadK(input, fn, q);
	int64_t min_history_months = 0;
	if (auto value = duckdb_compat::FindNamedParameter(input.named_parameters, "min_history_months");
	    value && !value->IsNull()) {
		min_history_months = (*value).GetValue<int64_t>();
	}
	double min_similarity = ReadMinSimilarity(input);
	auto bom_table = ReadTableParam(context, input, "bom_table", "bom_items", {"parent_id", "child_id"});
	auto movements_table = ReadTableParam(context, input, "movements_table", "goods_movements",
	                                      {"material_id", "movement_date", "quantity"});
	ValidateDateColumn(context, movements_table, "movement_date", "movements_table");
	ValidateNumericColumn(context, movements_table, "quantity", "movements_table");

	string sql = "WITH " + JaccardCTEs(bom_table, q, false, min_similarity) + StringUtil::Format(R"(,
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
					cs.query_id,
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
				  AND mh.first_usage IS NOT NULL -- an analog needs consumption history
				  AND COALESCE(mh.history_months, 0) >= %lld
			)
		)",
	                                                                                           movements_table,
	                                                                                           min_similarity,
	                                                                                           min_history_months) +
	             TopK(q,
	                  "material_id, similarity, shared_components, total_components, history_months, first_usage, "
	                  "last_usage",
	                  "with_history", "similarity DESC, material_id", k);
	return ParseSubquery(sql, context, "Failed to parse cold_start_analogs query");
}

//------------------------------------------------------------------------------
// Module Registration
//------------------------------------------------------------------------------

//! Two overloads per search: (material_id, k) and the batch form (k, materials_table := ...), which
//! returns a leading query_id column. The batch form exists because a table function takes
//! constants, not a column of another table.
static void RegisterSearch(ExtensionLoader &loader, const string &name, table_function_bind_replace_t bind,
                           const string &description, vector<string> examples, vector<string> batch_examples,
                           vector<string> categories, std::initializer_list<NamedParameterSpec> named) {
	TableFunctionSet set(duckdb_compat::FunctionName(name));
	vector<FunctionDescription> docs;
	for (bool batch : {false, true}) {
		vector<LogicalType> args = batch ? vector<LogicalType> {LogicalType::BIGINT}
		                                 : vector<LogicalType> {LogicalType::VARCHAR, LogicalType::BIGINT};
		TableFunction f(duckdb_compat::FunctionName(name), args, nullptr, nullptr);
		f.bind_replace = bind;
		FunctionDescription desc;
		// one text for both overloads: the reference shows a single description per function
		desc.description = description + " Batch form: " + name +
		                   "(k, materials_table := 'tbl') runs the search for every material_id of the table in one "
		                   "query, k per material, and returns query_id (the material searched for) in front of the "
		                   "same columns.";
		desc.examples = batch ? batch_examples : examples;
		desc.categories = categories;
		// Named parameters must be listed too: a non-empty parameter_names replaces the
		// WHOLE rendered list and pads any shortfall with col2, col3, ... They come out in
		// case_insensitive_map_t order, which is neither sorted nor guaranteed stable, so
		// test/sql/extension/function_documentation.test pins the rendered lists.
		if (batch) {
			AddDocumentedNamedParameters(f, desc, {"k"}, named);
			AddDocumentedNamedParameters(f, desc, {}, {{"materials_table", LogicalType::VARCHAR}});
		} else {
			AddDocumentedNamedParameters(f, desc, {"material_id", "k"}, named);
		}
		desc.parameter_types = args;
		set.AddFunction(f);
		docs.push_back(std::move(desc));
	}
	CreateTableFunctionInfo info(set);
	for (auto &d : docs) {
		info.descriptions.push_back(std::move(d));
	}
	duckdb_compat::NameFunctionsFromDescriptions(info);
	loader.RegisterFunction(std::move(info));
}

void RegisterSimilaritySearchFunctions(ExtensionLoader &loader) {
	RegisterSearch(loader, "find_similar_materials_jaccard", FindSimilarMaterialsJaccardBindReplace,
	               "Finds the k most structurally similar materials to a query material using exact "
	               "Jaccard similarity on BOM component sets. Returns material_id, similarity, "
	               "shared_components, and total_components. Rows below min_similarity are left out; without it, rows "
	               "scoring 0 (no shared component) are left out, and min_similarity := 0 keeps them. "
	               "mode := 'containment' ranks by the share of the query's components each material "
	               "contains instead (default 'jaccard').",
	               {"SELECT * FROM find_similar_materials_jaccard('MAT-001', 10);",
	                "SELECT * FROM find_similar_materials_jaccard('MAT-001', 20, min_similarity := 0.3);",
	                "SELECT * FROM find_similar_materials_jaccard('MAT-001', 10, mode := 'containment');"},
	               {"SELECT * FROM find_similar_materials_jaccard(5, materials_table := 'new_products');"},
	               {"similarity", "search"},
	               {{"bom_table", LogicalType::VARCHAR},
	                {"min_similarity", LogicalType::DOUBLE},
	                {"mode", LogicalType::VARCHAR}});

	RegisterSearch(loader, "find_similar_materials_wl_kernel", FindSimilarMaterialsWLKernelBindReplace,
	               "Finds the k most structurally similar materials using the Weisfeiler-Lehman graph "
	               "kernel, which captures multi-hop BOM structure beyond direct component overlap. "
	               "iterations controls the number of WL refinement rounds (default 3). Without min_similarity, rows "
	               "scoring 0 are left out; min_similarity := 0 keeps them.",
	               {"SELECT * FROM find_similar_materials_wl_kernel('MAT-001', 10);",
	                "SELECT * FROM find_similar_materials_wl_kernel('MAT-001', 10, iterations := 3, min_similarity := "
	                "0.2);"},
	               {"SELECT * FROM find_similar_materials_wl_kernel(5, materials_table := 'new_products');"},
	               {"similarity", "search", "graph"},
	               {{"bom_table", LogicalType::VARCHAR},
	                {"min_similarity", LogicalType::DOUBLE},
	                {"iterations", LogicalType::BIGINT}});

	RegisterSearch(loader, "cold_start_analogs", ColdStartAnalogsBindReplace,
	               "Finds structural analogs for a cold-start material (one with little or no demand "
	               "history). Returns established materials that are structurally similar and have at "
	               "least min_history_months of goods movement data, enabling surrogate forecasting. Without "
	               "min_similarity, materials scoring 0 are left out; min_similarity := 0 keeps them.",
	               {"SELECT * FROM cold_start_analogs('NEW-PART', 5);",
	                "SELECT * FROM cold_start_analogs('NEW-PART', 10, min_history_months := 12, min_similarity := "
	                "0.4);"},
	               {"SELECT * FROM cold_start_analogs(5, materials_table := 'new_products', min_history_months := 12);"},
	               {"similarity", "search", "forecasting"},
	               {{"movements_table", LogicalType::VARCHAR},
	                {"bom_table", LogicalType::VARCHAR},
	                {"min_similarity", LogicalType::DOUBLE},
	                {"min_history_months", LogicalType::BIGINT}});
}

} // namespace anofox
} // namespace duckdb
