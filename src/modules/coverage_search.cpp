#include "modules/coverage_search.hpp"
#include "duckdb_compat.hpp"
#include "core/named_parameters.hpp"
#include "core/sql_safety.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/parser/parsed_data/create_table_function_info.hpp"
#include "duckdb/parser/tableref/subqueryref.hpp"
#include "duckdb/main/client_context.hpp"
#include "telemetry.hpp"

namespace duckdb {
namespace anofox {

namespace {

unique_ptr<SubqueryRef> ParseSubquery(const string &query, ClientContext &context, const string &err_msg) {
	auto statements = duckdb_compat::ParseStatements(context, query);
	if (statements.size() != 1 || statements[0]->type != StatementType::SELECT_STATEMENT) {
		throw ParserException(err_msg);
	}
	auto &select_stmt = statements[0]->Cast<SelectStatement>();
	auto new_stmt = make_uniq<SelectStatement>();
	new_stmt->node = std::move(select_stmt.node);
	return make_uniq<SubqueryRef>(std::move(new_stmt));
}

//! A column name parameter: one identifier segment, returned double-quoted.
string QuotedColumn(const string &column, const string &param_name) {
	ValidateSQLIdentifierPath(column, param_name);
	if (column.find('.') != string::npos) {
		throw BinderException("Invalid identifier for %s: %s", param_name.c_str(), column.c_str());
	}
	return "\"" + column + "\"";
}

string FormatDouble(double value) {
	return StringUtil::Format("%.17g", value);
}

struct CoverageParams {
	int64_t k = 0;
	string mode = "containment";
	double alpha = 1.0;
	double beta = 0.0;
	bool weighted = true;
	double tie_break = 0.01;
	double min_score = 0.0;
	string feature_table = "feature_multiset";
	string entity_col = "entity_id";
	string feature_col = "feature_id";
	string weight_col = "weight";
};

CoverageParams ReadParams(ClientContext &context, TableFunctionBindInput &input, const string &fn) {
	CoverageParams p;
	if (input.inputs.size() < 2) {
		throw BinderException("%s requires 2 arguments: query, k", fn.c_str());
	}
	if (input.inputs[1].IsNull()) {
		throw BinderException("%s: k must not be NULL", fn.c_str());
	}
	p.k = input.inputs[1].GetValue<int64_t>();
	if (p.k <= 0) {
		throw BinderException("%s: k must be positive", fn.c_str());
	}
	auto &named = input.named_parameters;
	if (auto v = duckdb_compat::FindNamedParameter(named, "mode"); v && !v->IsNull()) {
		p.mode = StringUtil::Lower(v->GetValue<string>());
	}
	if (p.mode != "jaccard" && p.mode != "containment" && p.mode != "tversky") {
		throw BinderException("%s: mode must be one of 'jaccard', 'containment', 'tversky', got '%s'", fn.c_str(),
		                      p.mode.c_str());
	}
	if (auto v = duckdb_compat::FindNamedParameter(named, "alpha"); v && !v->IsNull()) {
		p.alpha = v->GetValue<double>();
	}
	if (auto v = duckdb_compat::FindNamedParameter(named, "beta"); v && !v->IsNull()) {
		p.beta = v->GetValue<double>();
	}
	if (p.alpha < 0 || p.beta < 0) {
		throw BinderException("%s: alpha and beta must be non-negative", fn.c_str());
	}
	if (auto v = duckdb_compat::FindNamedParameter(named, "weighted"); v && !v->IsNull()) {
		p.weighted = v->GetValue<bool>();
	}
	if (auto v = duckdb_compat::FindNamedParameter(named, "tie_break"); v && !v->IsNull()) {
		p.tie_break = v->GetValue<double>();
	}
	if (auto v = duckdb_compat::FindNamedParameter(named, "min_score"); v && !v->IsNull()) {
		p.min_score = v->GetValue<double>();
	}
	if (auto v = duckdb_compat::FindNamedParameter(named, "feature_table"); v && !v->IsNull()) {
		p.feature_table = v->GetValue<string>();
	}
	if (auto v = duckdb_compat::FindNamedParameter(named, "entity_col"); v && !v->IsNull()) {
		p.entity_col = v->GetValue<string>();
	}
	if (auto v = duckdb_compat::FindNamedParameter(named, "feature_col"); v && !v->IsNull()) {
		p.feature_col = v->GetValue<string>();
	}
	if (auto v = duckdb_compat::FindNamedParameter(named, "weight_col"); v) {
		if (v->IsNull()) {
			p.weighted = false; // no weight column: every (entity, feature) weighs 1
		} else {
			p.weight_col = v->GetValue<string>();
		}
	}
	p.feature_table = ValidateSQLIdentifierPath(p.feature_table, "feature_table");
	QuotedColumn(p.entity_col, "entity_col");
	QuotedColumn(p.feature_col, "feature_col");
	vector<string> required {p.entity_col, p.feature_col};
	if (p.weighted) {
		QuotedColumn(p.weight_col, "weight_col");
		required.push_back(p.weight_col);
	}
	ValidateTableColumns(context, p.feature_table, required, "feature_table");
	return p;
}

//! The feature multiset CTE: one row per (entity, feature) with a summed,
//! non-negative weight (NULL counts as 1); weight 1 when unweighted.
string FeatureMultisetSQL(const CoverageParams &p) {
	auto entity = "\"" + p.entity_col + "\"";
	auto feature = "\"" + p.feature_col + "\"";
	string weight = "1.0";
	if (p.weighted) {
		auto col = "\"" + p.weight_col + "\"";
		weight = StringUtil::Format(
		    "SUM(CASE WHEN CAST(%s AS DOUBLE) < 0 THEN error('weights must be non-negative') "
		    "ELSE COALESCE(CAST(%s AS DOUBLE), 1.0) END)",
		    col, col);
	}
	return StringUtil::Format("SELECT CAST(%s AS VARCHAR) AS entity_id, CAST(%s AS VARCHAR) AS feature_id, %s AS weight "
	                          "FROM %s WHERE %s IS NOT NULL AND %s IS NOT NULL GROUP BY ALL",
	                          entity, feature, weight, p.feature_table, entity, feature);
}

//! Rank candidates against the query CTE `q(feature_id, weight)`.
string RankingSQL(const CoverageParams &p, const string &query_cte, const string &exclude_entity_sql) {
	string measure;
	if (p.mode == "containment") {
		measure = "shared_weight / NULLIF(query_weight, 0)";
	} else if (p.mode == "jaccard") {
		measure = "shared_weight / NULLIF(query_weight + candidate_weight - shared_weight, 0)";
	} else {
		measure = StringUtil::Format("shared_weight / NULLIF(shared_weight + %s * (query_weight - shared_weight) + "
		                             "%s * (candidate_weight - shared_weight), 0)",
		                             FormatDouble(p.alpha), FormatDouble(p.beta));
	}
	string score = p.mode == "jaccard" ? "jaccard" : StringUtil::Format("measure + %s * jaccard", FormatDouble(p.tie_break));
	string exclude = exclude_entity_sql.empty() ? "" : " AND fm.entity_id <> " + exclude_entity_sql;
	return StringUtil::Format(R"(
		WITH
			fm AS MATERIALIZED (%s),
			q AS (%s),
			qt AS (SELECT SUM(weight) AS query_weight FROM q),
			shared AS (
				SELECT fm.entity_id, SUM(LEAST(fm.weight, q.weight)) AS shared_weight
				FROM fm JOIN q USING (feature_id)
				WHERE true%s
				GROUP BY fm.entity_id
			),
			totals AS (
				SELECT entity_id, SUM(weight) AS candidate_weight
				FROM fm WHERE entity_id IN (SELECT entity_id FROM shared)
				GROUP BY entity_id
			),
			m AS (
				SELECT s.entity_id, s.shared_weight, qt.query_weight, t.candidate_weight,
				       t.candidate_weight - s.shared_weight AS surplus_weight,
				       s.shared_weight / NULLIF(qt.query_weight, 0) AS containment,
				       s.shared_weight / NULLIF(qt.query_weight + t.candidate_weight - s.shared_weight, 0) AS jaccard
				FROM shared s JOIN totals t USING (entity_id), qt
			),
			scored AS (SELECT *, %s AS measure FROM m)
		SELECT entity_id, %s AS score, containment, jaccard, shared_weight, query_weight, candidate_weight,
		       surplus_weight
		FROM scored
		WHERE measure IS NOT NULL AND measure >= %s
		ORDER BY score DESC, entity_id
		LIMIT %lld
	)",
	                          FeatureMultisetSQL(p), query_cte, exclude, measure, score, FormatDouble(p.min_score),
	                          (long long)p.k);
}

unique_ptr<TableRef> FindCoveringEntitiesBindReplace(ClientContext &context, TableFunctionBindInput &input) {
	PostHogTelemetry::Instance().RecordFunctionCall("find_covering_entities");
	const string fn = "find_covering_entities";
	auto p = ReadParams(context, input, fn);
	if (input.inputs[0].IsNull()) {
		throw BinderException("%s: query_entity_id must not be NULL", fn.c_str());
	}
	auto query_sql = QuoteSQLStringLiteral(input.inputs[0].GetValue<string>());
	auto q = "SELECT feature_id, weight FROM fm WHERE entity_id = " + query_sql;
	return ParseSubquery(RankingSQL(p, q, query_sql), context, "Failed to parse find_covering_entities query");
}

unique_ptr<TableRef> FindCoveringEntitiesForBindReplace(ClientContext &context, TableFunctionBindInput &input) {
	PostHogTelemetry::Instance().RecordFunctionCall("find_covering_entities_for");
	const string fn = "find_covering_entities_for";
	auto p = ReadParams(context, input, fn);
	auto &query = input.inputs[0];
	string rows;
	if (!query.IsNull()) {
		if (query.type().id() != LogicalTypeId::MAP) {
			throw BinderException("%s: query must be a MAP(feature, weight), got %s", fn.c_str(),
			                      query.type().ToString());
		}
		for (auto &entry : ListValue::GetChildren(query)) {
			auto &kv = StructValue::GetChildren(entry);
			if (kv[0].IsNull()) {
				continue;
			}
			double w = 1.0;
			if (!kv[1].IsNull()) {
				if (!kv[1].type().IsNumeric()) {
					throw BinderException("%s: weights must be numeric, got '%s'", fn.c_str(), kv[1].ToString());
				}
				w = kv[1].DefaultCastAs(LogicalType::DOUBLE).GetValue<double>();
			}
			if (w < 0) {
				throw BinderException("%s: weights must be non-negative", fn.c_str());
			}
			if (!p.weighted) {
				w = 1.0;
			}
			auto key = QuoteSQLStringLiteral(kv[0].ToString());
			rows += (rows.empty() ? "" : ", ") + StringUtil::Format("(%s, %s::DOUBLE)", key, FormatDouble(w));
		}
	}
	string q = rows.empty() ? "SELECT NULL::VARCHAR AS feature_id, NULL::DOUBLE AS weight WHERE false"
	                        : "SELECT * FROM (VALUES " + rows + ") v(feature_id, weight)";
	return ParseSubquery(RankingSQL(p, q, ""), context, "Failed to parse find_covering_entities_for query");
}

const char *NAMED_DOC = "Named parameters (bind by position on DuckDB 1.5.6, so pass them in this order): mode "
                        "('containment' | 'jaccard' | 'tversky'), alpha, beta (Tversky weights), weighted (use the "
                        "weight column, default true), tie_break (weight of Jaccard added to the score, default "
                        "0.01), min_score (threshold on the measure itself), feature_table (default "
                        "'feature_multiset'), entity_col, feature_col, weight_col.";

void AddNamed(TableFunction &f, FunctionDescription &desc, std::initializer_list<const char *> positional) {
	AddDocumentedNamedParameters(f, desc, positional,
	                             {{"mode", LogicalType::VARCHAR},
	                              {"alpha", LogicalType::DOUBLE},
	                              {"beta", LogicalType::DOUBLE},
	                              {"weighted", LogicalType::BOOLEAN},
	                              {"tie_break", LogicalType::DOUBLE},
	                              {"min_score", LogicalType::DOUBLE},
	                              {"feature_table", LogicalType::VARCHAR},
	                              {"entity_col", LogicalType::VARCHAR},
	                              {"feature_col", LogicalType::VARCHAR},
	                              {"weight_col", LogicalType::VARCHAR}});
}

} // namespace

void RegisterCoverageSearchFunctions(ExtensionLoader &loader) {
	{
		TableFunction f("find_covering_entities", {LogicalType::VARCHAR, LogicalType::BIGINT}, nullptr, nullptr);
		f.bind_replace = FindCoveringEntitiesBindReplace;
		FunctionDescription desc;
		desc.description =
		    string("Ranks the entities of an (entity, feature, weight) table by how well they cover a query entity of "
		           "the same table: weighted containment Σmin/Σq by default, so a richer candidate is not penalised, "
		           "with weighted Jaccard as tie-break so the leanest full cover comes first. Returns entity_id, "
		           "score, containment, jaccard, shared_weight, query_weight, candidate_weight, surplus_weight. ") +
		    NAMED_DOC;
		desc.examples = {"SELECT * FROM find_covering_entities('P-100', 10);",
		                 "SELECT * FROM find_covering_entities('F-100', 5, feature_table := 'frame_bom', "
		                 "entity_col := 'frame_no', feature_col := 'part_no', weight_col := 'quantity_per');"};
		desc.categories = {"similarity", "search", "coverage"};
		AddNamed(f, desc, {"query_entity_id", "k"});
		desc.parameter_types = {LogicalType::VARCHAR, LogicalType::BIGINT};
		CreateTableFunctionInfo info(f);
		info.descriptions.push_back(std::move(desc));
		duckdb_compat::NameFunctionsFromDescriptions(info);
		loader.RegisterFunction(std::move(info));
	}
	{
		TableFunction f("find_covering_entities_for", {LogicalType::ANY, LogicalType::BIGINT}, nullptr, nullptr);
		f.bind_replace = FindCoveringEntitiesForBindReplace;
		FunctionDescription desc;
		desc.description =
		    string("Like find_covering_entities, for a query that is not in the table: query is a MAP(feature, "
		           "weight), e.g. the class multiset of a new specification. The argument must be constant (a "
		           "literal or getvariable(...)). ") +
		    NAMED_DOC;
		desc.examples = {"SELECT * FROM find_covering_entities_for(MAP {'seat_heating': 2, 'massage': 4}, 10);",
		                 "SELECT * FROM find_covering_entities_for(getvariable('request'), 20, mode := 'jaccard');"};
		desc.categories = {"similarity", "search", "coverage"};
		AddNamed(f, desc, {"query", "k"});
		desc.parameter_types = {LogicalType::ANY, LogicalType::BIGINT};
		CreateTableFunctionInfo info(f);
		info.descriptions.push_back(std::move(desc));
		duckdb_compat::NameFunctionsFromDescriptions(info);
		loader.RegisterFunction(std::move(info));
	}
}

} // namespace anofox
} // namespace duckdb
