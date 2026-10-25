#include "modules/coverage_search.hpp"
#include "modules/coverage_sql.hpp"
#include "duckdb_compat.hpp"
#include "core/named_parameters.hpp"
#include "core/sql_safety.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/parser/parsed_data/create_table_function_info.hpp"
#include "duckdb/parser/tableref/subqueryref.hpp"
#include "duckdb/main/client_context.hpp"
#include "telemetry.hpp"

#include <cmath>

namespace duckdb {
namespace anofox {

namespace {


//! A column name parameter: one identifier segment (validated, so quoting it is safe).
void ValidateColumnName(const string &column, const string &param_name) {
	ValidateSQLIdentifierPath(column, param_name);
	if (column.find('.') != string::npos) {
		throw BinderException("Invalid identifier for %s: %s", param_name.c_str(), column.c_str());
	}
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
	if (!std::isfinite(p.alpha) || !std::isfinite(p.beta)) {
		throw BinderException("%s: alpha and beta must be finite", fn.c_str());
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
	if (!std::isfinite(p.tie_break) || !std::isfinite(p.min_score)) {
		throw BinderException("%s: tie_break and min_score must be finite", fn.c_str());
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
			auto w = duckdb_compat::FindNamedParameter(named, "weighted");
			if (w && !w->IsNull() && w->GetValue<bool>()) {
				throw BinderException("%s: weighted := true needs a weight_col", fn.c_str());
			}
			p.weighted = false; // no weight column: every (entity, feature) weighs 1
		} else {
			p.weight_col = v->GetValue<string>();
		}
	}
	p.feature_table = ValidateSQLIdentifierPath(p.feature_table, "feature_table");
	ValidateColumnName(p.entity_col, "entity_col");
	ValidateColumnName(p.feature_col, "feature_col");
	vector<string> required {p.entity_col, p.feature_col};
	if (p.weighted) {
		ValidateColumnName(p.weight_col, "weight_col");
		required.push_back(p.weight_col);
	}
	try {
		ValidateTableColumns(context, p.feature_table, required, "feature_table");
	} catch (BinderException &e) {
		bool weight_named = duckdb_compat::FindNamedParameter(named, "weight_col") != nullptr;
		if (p.weighted && !weight_named && string(e.what()).find("column(s): " + p.weight_col) != string::npos) {
			throw BinderException("%s: feature_table '%s' has no column '%s'; pass weight_col := '<column>', or "
			                      "weight_col := NULL for unit weights",
			                      fn.c_str(), p.feature_table.c_str(), p.weight_col.c_str());
		}
		throw;
	}
	return p;
}

//! The feature multiset CTE: one row per (entity, feature) with a summed,
//! non-negative weight (NULL counts as 1); weight 1 when unweighted.
string FeatureMultisetSQL(const CoverageParams &p) {
	auto entity = "\"" + p.entity_col + "\"";
	auto feature = "\"" + p.feature_col + "\"";
	auto table = QuotedIdentifier(p.feature_table);
	if (!p.weighted) {
		return StringUtil::Format("SELECT CAST(%s AS VARCHAR) AS entity_id, CAST(%s AS VARCHAR) AS feature_id, "
		                          "1.0::DOUBLE AS weight FROM %s WHERE %s IS NOT NULL AND %s IS NOT NULL GROUP BY ALL",
		                          entity, feature, table, entity, feature);
	}
	// Every row's weight is validated, including rows with a NULL entity or feature: the
	// check is an uncorrelated scalar subquery over the whole table, so no filter is pushed
	// below it, and the error names the offending row.
	auto col = "CAST(\"" + p.weight_col + "\" AS DOUBLE)";
	auto where = StringUtil::Format("' (entity ' || coalesce(CAST(%s AS VARCHAR), 'NULL') || ', feature ' || "
	                                "coalesce(CAST(%s AS VARCHAR), 'NULL') || ' in %s)'",
	                                entity, feature, EscapeSQLStringLiteral(p.feature_table));
	auto check = StringUtil::Format("(SELECT count(CASE WHEN NOT isfinite(%s) THEN error('weights must be finite' || %s) "
	                                "WHEN %s < 0 THEN error('weights must be non-negative' || %s) END) FROM %s)",
	                                col, where, col, where, table);
	return StringUtil::Format("SELECT CAST(%s AS VARCHAR) AS entity_id, CAST(%s AS VARCHAR) AS feature_id, "
	                          "SUM(COALESCE(%s, 1.0::DOUBLE)) AS weight FROM %s "
	                          "WHERE %s IS NOT NULL AND %s IS NOT NULL AND %s = 0 GROUP BY ALL",
	                          entity, feature, col, table, entity, feature, check);
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
		                             DoubleLiteral(p.alpha), DoubleLiteral(p.beta));
	}
	string score = p.mode == "jaccard" ? "jaccard" : StringUtil::Format("measure + %s * jaccard", DoubleLiteral(p.tie_break));
	string exclude = exclude_entity_sql.empty() ? "" : " AND fm.entity_id <> " + exclude_entity_sql;
	return StringUtil::Format(R"(
		WITH
			fm AS MATERIALIZED (%s),
			q AS (%s),
			qt AS (SELECT SUM(weight) AS query_weight FROM q),
			shared AS (
				SELECT fm.entity_id, SUM(LEAST(fm.weight, q.weight)) AS shared_weight
				FROM fm JOIN q USING (feature_id)
				WHERE fm.weight > 0 AND q.weight > 0%s
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
	                          FeatureMultisetSQL(p), query_cte, exclude, measure, score, DoubleLiteral(p.min_score),
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
	return CoverageSubquery(RankingSQL(p, q, query_sql), context, "find_covering_entities");
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
			if (!std::isfinite(w)) {
				throw BinderException("%s: weights must be finite", fn.c_str());
			}
			if (w < 0) {
				throw BinderException("%s: weights must be non-negative", fn.c_str());
			}
			if (w == 0) {
				continue; // a zero-weight request entry asks for nothing
			}
			if (!p.weighted) {
				w = 1.0;
			}
			auto key = QuoteSQLStringLiteral(kv[0].ToString());
			rows += (rows.empty() ? "" : ", ") + StringUtil::Format("(%s, %s)", key, DoubleLiteral(w));
		}
	}
	string q = rows.empty() ? "SELECT NULL::VARCHAR AS feature_id, NULL::DOUBLE AS weight WHERE false"
	                        : "SELECT * FROM (VALUES " + rows + ") v(feature_id, weight)";
	return CoverageSubquery(RankingSQL(p, q, ""), context, "find_covering_entities_for");
}

const char *NAMED_DOC = "Named parameters: mode ('containment' | 'jaccard' | 'tversky'), alpha, beta (Tversky "
                        "weights, used in mode 'tversky' only), weighted (use the weight column, default true), "
                        "tie_break (weight of Jaccard added to the score in modes 'containment' and 'tversky', "
                        "default 0.01), min_score (threshold on the measure itself), feature_table (default "
                        "'feature_multiset'), entity_col, feature_col, weight_col (NULL for unit weights).";

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
