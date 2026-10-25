#include "modules/pivot_classifier.hpp"
#include "modules/coverage_sql.hpp"
#include "modules/property_tree.hpp"
#include "duckdb_compat.hpp"
#include "compat_anofox/scalar_bind.hpp"
#include "core/sql_safety.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/parser/parsed_data/create_scalar_function_info.hpp"
#include "duckdb/parser/parsed_data/create_table_function_info.hpp"
#include "telemetry.hpp"
#include "anofox_similarity_banner.hpp"

#include <cmath>

namespace duckdb {
namespace anofox {

namespace {

//------------------------------------------------------------------------------
// Scalars
//------------------------------------------------------------------------------

vector<double> Doubles(const Value &list) {
	vector<double> out;
	for (auto &x : ListValue::GetChildren(list)) {
		out.push_back(x.IsNull() ? 0.0 : x.GetValue<double>());
	}
	return out;
}

//! Cosine of two vectors of the same dimension; 0 when one has no direction.
double Cosine(const vector<double> &a, const vector<double> &b, const char *fn) {
	if (a.size() != b.size()) {
		throw InvalidInputException("%s: vectors must have the same dimension (%llu vs %llu)", fn,
		                            (unsigned long long)a.size(), (unsigned long long)b.size());
	}
	double dot = 0, na = 0, nb = 0;
	for (idx_t i = 0; i < a.size(); i++) {
		dot += a[i] * b[i];
		na += a[i] * a[i];
		nb += b[i] * b[i];
	}
	if (na <= 0 || nb <= 0) {
		return 0.0;
	}
	return dot / std::sqrt(na * nb);
}

void MaxsimFun(DataChunk &args, ExpressionState &, Vector &result) {
	for (idx_t row = 0; row < args.size(); row++) {
		auto q = args.data[0].GetValue(row);
		auto d = args.data[1].GetValue(row);
		if (q.IsNull() || d.IsNull()) {
			result.SetValue(row, Value(LogicalType::DOUBLE));
			continue;
		}
		vector<vector<double>> qs, ds;
		for (auto &t : ListValue::GetChildren(q)) {
			if (!t.IsNull()) {
				qs.push_back(Doubles(t));
			}
		}
		for (auto &t : ListValue::GetChildren(d)) {
			if (!t.IsNull()) {
				ds.push_back(Doubles(t));
			}
		}
		if (qs.empty() || ds.empty()) {
			result.SetValue(row, Value(LogicalType::DOUBLE));
			continue;
		}
		double sum = 0;
		for (auto &qt : qs) {
			double best = -1;
			for (auto &dt : ds) {
				best = MaxValue(best, Cosine(qt, dt, "maxsim"));
			}
			sum += best;
		}
		result.SetValue(row, Value::DOUBLE(sum / double(qs.size())));
	}
	if (args.AllConstant()) {
		result.SetVectorType(VectorType::CONSTANT_VECTOR);
	}
}

void BhattacharyyaFun(DataChunk &args, ExpressionState &, Vector &result) {
	for (idx_t row = 0; row < args.size(); row++) {
		auto p = args.data[0].GetValue(row);
		auto q = args.data[1].GetValue(row);
		if (p.IsNull() || q.IsNull()) {
			result.SetValue(row, Value(LogicalType::DOUBLE));
			continue;
		}
		auto read = [](const Value &map) {
			std::map<string, double> out;
			for (auto &kv : ListValue::GetChildren(map)) {
				auto &pair = StructValue::GetChildren(kv);
				if (pair[0].IsNull() || pair[1].IsNull()) {
					continue;
				}
				double v = pair[1].GetValue<double>();
				if (v < 0 || !std::isfinite(v)) {
					throw InvalidInputException("bhattacharyya: probabilities must not be negative or non-finite");
				}
				out[pair[0].ToString()] += v;
			}
			return out;
		};
		auto pm = read(p), qm = read(q);
		double s = 0;
		for (auto &kv : pm) {
			auto it = qm.find(kv.first);
			if (it != qm.end()) {
				s += std::sqrt(kv.second * it->second);
			}
		}
		result.SetValue(row, Value::DOUBLE(s));
	}
	if (args.AllConstant()) {
		result.SetVectorType(VectorType::CONSTANT_VECTOR);
	}
}

void CalibratedCosineFun(DataChunk &args, ExpressionState &, Vector &result) {
	for (idx_t row = 0; row < args.size(); row++) {
		auto a = args.data[0].GetValue(row);
		auto b = args.data[1].GetValue(row);
		auto f = args.data[2].GetValue(row);
		if (a.IsNull() || b.IsNull() || f.IsNull()) {
			result.SetValue(row, Value(LogicalType::DOUBLE));
			continue;
		}
		double floor = f.GetValue<double>();
		if (!(floor >= 0 && floor < 1)) {
			throw InvalidInputException("calibrated_cosine: floor must be in [0, 1)");
		}
		double cos = Cosine(Doubles(a), Doubles(b), "calibrated_cosine");
		result.SetValue(row, Value::DOUBLE(MaxValue(0.0, MinValue(1.0, (cos - floor) / (1 - floor)))));
	}
	if (args.AllConstant()) {
		result.SetVectorType(VectorType::CONSTANT_VECTOR);
	}
}

unique_ptr<FunctionData> MaxsimBind() {
	PostHogTelemetry::Instance().RecordFunctionCall("maxsim");
	return nullptr;
}
unique_ptr<FunctionData> BhattacharyyaBind() {
	PostHogTelemetry::Instance().RecordFunctionCall("bhattacharyya");
	return nullptr;
}
unique_ptr<FunctionData> CalibratedCosineBind() {
	PostHogTelemetry::Instance().RecordFunctionCall("calibrated_cosine");
	return nullptr;
}

void RegisterScalar(ExtensionLoader &loader, const string &name, vector<LogicalType> args, scalar_function_t fn,
                    bind_scalar_function_t bind, vector<string> names, const string &description,
                    vector<string> examples) {
	ScalarFunction f(duckdb_compat::FunctionName(name), args, LogicalType::DOUBLE, fn, bind);
	duckdb_compat::SetFallible(f);
	CreateScalarFunctionInfo info(f);
	FunctionDescription d;
	d.description = description;
	d.examples = std::move(examples);
	d.categories = {"coverage", "classification"};
	d.parameter_names = std::move(names);
	d.parameter_types = args;
	info.descriptions.push_back(std::move(d));
	duckdb_compat::NameArgumentsFromDescriptions(info);
	loader.RegisterFunction(std::move(info));
}

//------------------------------------------------------------------------------
// Named parameters of the table functions
//------------------------------------------------------------------------------

struct Named {
	const duckdb_compat::BindNamedParameters &map;
	string fn;
	const Value *Get(const char *name) const {
		return duckdb_compat::FindNamedParameter(map, name);
	}
	//! The parameter's string, the fallback when not given, "" when given as NULL.
	string Str(const char *name, const string &fallback = "") const {
		auto v = Get(name);
		if (!v) {
			return fallback;
		}
		return v->IsNull() ? string() : v->GetValue<string>();
	}
	double Dbl(const char *name, double fallback) const {
		auto v = Get(name);
		if (!v || v->IsNull()) {
			return fallback;
		}
		double d = v->GetValue<double>();
		if (!std::isfinite(d)) {
			throw BinderException("%s: %s must be finite", fn, name);
		}
		return d;
	}
	//! A table parameter: validated identifier, existing table, required columns ("" when not given).
	string Table(ClientContext &context, const char *name, const string &fallback, const vector<string> &columns) const {
		auto table = Str(name, fallback);
		if (table.empty()) {
			return table;
		}
		table = ValidateSQLIdentifierPath(table, name);
		if (!TableExists(context, table)) {
			throw BinderException("%s: %s '%s' does not exist", fn, name, table);
		}
		ValidateTableColumns(context, table, columns, name);
		return table;
	}
};

void RegisterTable(ExtensionLoader &loader, const string &name, vector<LogicalType> positional,
                   vector<string> positional_names, table_function_bind_replace_t fn,
                   vector<std::pair<string, LogicalType>> named, const string &description, vector<string> examples) {
	TableFunction f(duckdb_compat::FunctionName(name), positional, nullptr, nullptr);
	f.bind_replace = fn;
	FunctionDescription desc;
	desc.description = description;
	desc.examples = std::move(examples);
	desc.categories = {"coverage", "classification"};
	desc.parameter_names = std::move(positional_names);
	for (auto &spec : named) {
		duckdb_compat::AddNamedParameter(f, spec.first, spec.second);
		desc.parameter_names.emplace_back(spec.first);
	}
	desc.parameter_types = positional;
	CreateTableFunctionInfo info(f);
	info.descriptions.push_back(std::move(desc));
	duckdb_compat::NameFunctionsFromDescriptions(info);
	loader.RegisterFunction(std::move(info));
}

//------------------------------------------------------------------------------
// cosine_floor
//------------------------------------------------------------------------------

unique_ptr<TableRef> CosineFloorBindReplace(ClientContext &context, TableFunctionBindInput &input) {
	PostHogTelemetry::Instance().RecordFunctionCall("cosine_floor");
	const string fn = "cosine_floor";
	if (input.inputs.size() < 2 || input.inputs[0].IsNull() || input.inputs[1].IsNull()) {
		throw BinderException("%s: table and column must not be NULL", fn);
	}
	auto table = ValidateSQLIdentifierPath(input.inputs[0].GetValue<string>(), "table");
	auto column = input.inputs[1].GetValue<string>();
	ValidateSQLIdentifierPath(column, "column");
	if (column.find('.') != string::npos) {
		throw BinderException("%s: column must be a plain column name", fn);
	}
	if (!TableExists(context, table)) {
		throw BinderException("%s: table '%s' does not exist", fn, table);
	}
	ValidateTableColumns(context, table, {column}, "table");
	Named named {input.named_parameters, fn};
	double sample = named.Dbl("sample", 10000);
	double seed = named.Dbl("seed", 7);
	if (sample < 1 || sample > 1e8) {
		throw BinderException("%s: sample must be between 1 and 10^8", fn);
	}
	auto col = QuotedIdentifier(column);
	// exact median over all pairs when they fit into the sample; otherwise pairs drawn from a
	// seeded hash of the pair number (never a vector with itself)
	auto sql = StringUtil::Format(R"(
		WITH v AS (
			SELECT x.vec, row_number() OVER (ORDER BY hash(CAST(x.vec AS VARCHAR)), CAST(x.vec AS VARCHAR)) - 1 AS i
			FROM (SELECT CAST(%s AS DOUBLE[]) AS vec FROM %s) x
			WHERE x.vec IS NOT NULL AND list_dot_product(x.vec, x.vec) > 0
		),
		n AS (SELECT count(*) AS n FROM v),
		exact AS (
			SELECT list_cosine_similarity(a.vec, b.vec) AS c FROM v a JOIN v b ON a.i < b.i
			WHERE (SELECT n * (n - 1) / 2 FROM n) <= %lld
		),
		drawn AS (
			SELECT CAST(hash(%lld, k, 'a') %% n AS BIGINT) AS a,
			       CAST((hash(%lld, k, 'a') %% n + 1 + hash(%lld, k, 'b') %% (n - 1)) %% n AS BIGINT) AS b
			FROM range(%lld) t(k), n
			WHERE n.n * (n.n - 1) / 2 > %lld
		),
		sampled AS (SELECT list_cosine_similarity(x.vec, y.vec) AS c FROM drawn d JOIN v x ON x.i = d.a JOIN v y ON y.i = d.b),
		pairs AS (SELECT c FROM exact UNION ALL SELECT c FROM sampled)
		SELECT median(c) AS floor, count(*) AS n_pairs FROM pairs
	)",
	                              col, QuotedIdentifier(table), (long long)sample, (long long)seed, (long long)seed,
	                              (long long)seed, (long long)sample, (long long)sample);
	return CoverageSubquery(sql, context, fn);
}

//------------------------------------------------------------------------------
// classify_to_catalogue
//------------------------------------------------------------------------------

unique_ptr<TableRef> ClassifyBindReplace(ClientContext &context, TableFunctionBindInput &input) {
	PostHogTelemetry::Instance().RecordFunctionCall("classify_to_catalogue");
	const string fn = "classify_to_catalogue";
	Named named {input.named_parameters, fn};
	auto mode = StringUtil::Lower(named.Str("mode", "centroid"));
	if (mode != "centroid" && mode != "late_interaction") {
		throw BinderException("%s: mode must be 'centroid' or 'late_interaction'", fn);
	}
	bool late = mode == "late_interaction";
	auto items = named.Table(context, "items_table", "item_vectors",
	                         late ? vector<string> {"node_id", "tokens"} : vector<string> {"node_id", "vec"});
	if (items.empty()) {
		throw BinderException("%s: items_table must not be NULL", fn);
	}
	auto catalogue = named.Table(context, "catalogue_table", "catalogue_vectors",
	                             late ? vector<string> {"class_id", "exemplar_tokens"} : vector<string> {"class_id", "vec"});
	if (catalogue.empty()) {
		throw BinderException("%s: catalogue_table must not be NULL", fn);
	}
	bool centre = true;
	if (auto v = named.Get("center_by_source"); v && !v->IsNull()) {
		centre = v->GetValue<bool>();
	}
	double beta = named.Dbl("attr_bonus", 1.0);
	double tau = named.Dbl("temperature", 0.3);
	if (tau <= 0) {
		throw BinderException("%s: temperature must be positive", fn);
	}
	if (beta < 0) {
		throw BinderException("%s: attr_bonus must not be negative", fn);
	}
	double name_floor = named.Dbl("name_floor", 0.85);
	if (name_floor < 0 || name_floor >= 1) {
		throw BinderException("%s: name_floor must be in [0, 1)", fn);
	}
	int64_t top = 0;
	if (auto v = named.Get("top"); v && !v->IsNull()) {
		top = v->GetValue<int64_t>();
		if (top <= 0) {
			throw BinderException("%s: top must be positive", fn);
		}
	}
	auto overrides = named.Table(context, "overrides_table", "", {"wording", "class_id"});
	auto decisions = named.Table(context, "decisions_table", "", {"node_id", "class_id"});
	auto attrs = named.Table(context, "attrs_table", "", {"node_id", "dimension"});
	auto templates = named.Table(context, "templates_table", "", {"class_id", "dimension"});
	auto name_vectors = named.Table(context, "name_vectors", "", {"name", "vec"});

	auto has = [&](const string &table, const char *col) { return TableHasColumn(context, table, col); };
	bool items_tree = has(items, "tree_id");
	bool items_source = has(items, "source");
	bool items_label = has(items, "label");
	if (!overrides.empty() && !items_label) {
		throw BinderException("%s: items_table needs a label column to apply overrides_table", fn);
	}
	auto qi = QuotedIdentifier(items);
	auto qc = QuotedIdentifier(catalogue);

	string items_sql = StringUtil::Format(
	    "SELECT %s AS tree_id, CAST(node_id AS VARCHAR) AS node_id, %s AS source, %s AS label, %s AS vec, %s AS tokens "
	    "FROM %s",
	    items_tree ? "CAST(tree_id AS VARCHAR)" : "NULL::VARCHAR", items_source ? "CAST(source AS VARCHAR)" : "NULL::VARCHAR",
	    items_label ? "CAST(label AS VARCHAR)" : "NULL::VARCHAR",
	    has(items, "vec") ? "CAST(vec AS DOUBLE[])" : "NULL::DOUBLE[]",
	    late ? "CAST(tokens AS DOUBLE[][])" : "NULL::DOUBLE[][]", qi);
	string cat_sql = StringUtil::Format("SELECT CAST(class_id AS VARCHAR) AS class_id, %s AS vec, %s AS tokens FROM %s",
	                                    late ? "NULL::DOUBLE[]" : "CAST(vec AS DOUBLE[])",
	                                    late ? "CAST(exemplar_tokens AS DOUBLE[][])" : "NULL::DOUBLE[][]", qc);

	// step 1: per-source mean-centring of the item vectors (centroid mode)
	string vectors;
	if (late) {
		vectors = "SELECT tree_id, node_id, NULL::DOUBLE[] AS vec FROM items";
	} else if (centre) {
		vectors = R"(
			SELECT e.tree_id, e.node_id, list(e.v - m.m ORDER BY e.i) AS vec
			FROM (SELECT tree_id, node_id, source, unnest(vec) AS v, generate_subscripts(vec, 1) AS i FROM items) e
			JOIN (SELECT source, i, avg(v) AS m
			      FROM (SELECT source, unnest(vec) AS v, generate_subscripts(vec, 1) AS i FROM items) GROUP BY ALL) m
			  ON m.source IS NOT DISTINCT FROM e.source AND m.i = e.i
			GROUP BY e.tree_id, e.node_id)";
	} else {
		vectors = "SELECT tree_id, node_id, vec FROM items";
	}
	// step 2: similarity to every class
	string similarity =
	    late ? "maxsim(i.tokens, k.tokens)"
	         : "CASE WHEN list_dot_product(v.vec, v.vec) > 0 AND list_dot_product(k.vec, k.vec) > 0 "
	           "THEN list_cosine_similarity(v.vec, k.vec) ELSE 0 END";
	string sims_from = late ? "items i CROSS JOIN catalogue k" : "vectors v CROSS JOIN catalogue k";
	string sims_cols = late ? "i.tree_id, i.node_id" : "v.tree_id, v.node_id";

	// step 3: attribute-template bonus, dimension-gated name match (HLD §4.4)
	string bonus = "SELECT NULL::VARCHAR AS tree_id, NULL::VARCHAR AS node_id, NULL::VARCHAR AS class_id, "
	               "NULL::DOUBLE AS b WHERE false";
	if (!attrs.empty() && !templates.empty() && beta > 0) {
		bool attrs_tree = has(attrs, "tree_id");
		string tree_join = attrs_tree ? "i.tree_id IS NOT DISTINCT FROM CAST(a.tree_id AS VARCHAR)" : "true";
		string attr_name = has(attrs, "name") ? "CAST(a.name AS VARCHAR)" : "NULL::VARCHAR";
		vector<string> name_cols;
		for (auto c : {"name", "name_en", "name_de"}) {
			if (has(templates, c)) {
				name_cols.push_back(StringUtil::Format("CAST(%s AS VARCHAR)", c));
			}
		}
		string tnames = name_cols.empty() ? "[NULL::VARCHAR]" : "[" + StringUtil::Join(name_cols, ", ") + "]";
		string s_name = "1.0::DOUBLE"; // no name vectors: the dimension gate alone
		string vec_joins;
		if (!name_vectors.empty()) {
			vec_joins = StringUtil::Format(
			    " LEFT JOIN (SELECT CAST(name AS VARCHAR) AS name, any_value(CAST(vec AS DOUBLE[])) AS vec FROM %s "
			    "GROUP BY ALL) va ON va.name = ia.name"
			    " LEFT JOIN (SELECT CAST(name AS VARCHAR) AS name, any_value(CAST(vec AS DOUBLE[])) AS vec FROM %s "
			    "GROUP BY ALL) vt ON vt.name = t.tname",
			    QuotedIdentifier(name_vectors), QuotedIdentifier(name_vectors));
			s_name = StringUtil::Format(
			    "CASE WHEN va.vec IS NULL OR vt.vec IS NULL THEN 0.0 ELSE calibrated_cosine(va.vec, vt.vec, %s) END",
			    DoubleLiteral(name_floor));
		}
		bonus = StringUtil::Format(R"(
			WITH ia AS (
				SELECT i.tree_id, i.node_id, row_number() OVER () AS a_id, %s AS name, CAST(a.dimension AS VARCHAR) AS dimension
				FROM items i JOIN %s a ON CAST(a.node_id AS VARCHAR) = i.node_id AND %s
			),
			n_attrs AS (SELECT tree_id, node_id, count(*) AS n FROM ia GROUP BY ALL),
			t AS (SELECT DISTINCT CAST(class_id AS VARCHAR) AS class_id, CAST(dimension AS VARCHAR) AS dimension,
			             unnest(%s) AS tname FROM %s),
			best AS (
				SELECT ia.tree_id, ia.node_id, ia.a_id, t.class_id, max(%s) AS s
				FROM ia JOIN t ON t.dimension = ia.dimension%s
				GROUP BY ALL
			)
			SELECT b.tree_id, b.node_id, b.class_id, sum(b.s) / any_value(n.n) AS b
			FROM best b JOIN n_attrs n ON n.tree_id IS NOT DISTINCT FROM b.tree_id AND n.node_id = b.node_id
			GROUP BY ALL)",
		                           attr_name, QuotedIdentifier(attrs), tree_join, tnames, QuotedIdentifier(templates),
		                           s_name, vec_joins);
	}

	// overrides (per wording, BR-32) and decisions (per item, BR-33)
	string forced_parts;
	if (!overrides.empty()) {
		bool ov_source = has(overrides, "source");
		forced_parts += StringUtil::Format(
		    "SELECT i.tree_id, i.node_id, first(CAST(o.class_id AS VARCHAR) ORDER BY %s, CAST(o.class_id AS VARCHAR)) "
		    "AS cls, 2 AS prio FROM items i JOIN %s o ON CAST(o.wording AS VARCHAR) = i.label%s GROUP BY ALL",
		    ov_source ? "o.source IS NULL" : "1", QuotedIdentifier(overrides),
		    ov_source ? " AND (o.source IS NULL OR CAST(o.source AS VARCHAR) = i.source)" : "");
	}
	if (!decisions.empty()) {
		bool dec_tree = has(decisions, "tree_id");
		if (!forced_parts.empty()) {
			forced_parts += " UNION ALL ";
		}
		forced_parts += StringUtil::Format(
		    "SELECT i.tree_id, i.node_id, first(CAST(d.class_id AS VARCHAR) ORDER BY CAST(d.class_id AS VARCHAR)) AS cls, "
		    "1 AS prio FROM items i JOIN %s d ON CAST(d.node_id AS VARCHAR) = i.node_id%s GROUP BY ALL",
		    QuotedIdentifier(decisions), dec_tree ? " AND i.tree_id IS NOT DISTINCT FROM CAST(d.tree_id AS VARCHAR)" : "");
	}
	string forced = forced_parts.empty()
	                    ? "SELECT NULL::VARCHAR AS tree_id, NULL::VARCHAR AS node_id, NULL::VARCHAR AS cls WHERE false"
	                    : "SELECT tree_id, node_id, first(cls ORDER BY prio) AS cls FROM (" + forced_parts +
	                          ") GROUP BY ALL";

	auto sql = StringUtil::Format(R"(
		WITH items AS (%s),
		catalogue AS (%s),
		vectors AS (%s),
		sims AS (SELECT %s, k.class_id, %s AS s FROM %s),
		zs AS (
			SELECT tree_id, node_id, class_id,
			       (s - avg(s) OVER w) / CASE WHEN stddev_pop(s) OVER w > 1e-12 THEN stddev_pop(s) OVER w ELSE 1 END AS z
			FROM sims WINDOW w AS (PARTITION BY tree_id, node_id)
		),
		bonus AS (%s),
		scored AS (
			SELECT z.tree_id, z.node_id, z.class_id, z.z + %s * COALESCE(b.b, 0) AS score
			FROM zs z LEFT JOIN bonus b ON b.tree_id IS NOT DISTINCT FROM z.tree_id AND b.node_id = z.node_id
			                             AND b.class_id = z.class_id
		),
		e AS (
			SELECT *, exp((score - max(score) OVER (PARTITION BY tree_id, node_id)) / %s) AS e FROM scored
		),
		forced AS (%s),
		probs AS (
			SELECT e.tree_id, e.node_id, e.class_id,
			       CASE WHEN f.cls IS NULL THEN e.e / sum(e.e) OVER (PARTITION BY e.tree_id, e.node_id)
			            WHEN f.cls = e.class_id THEN 1.0 ELSE 0.0 END AS p,
			       f.cls IS NOT NULL AS overridden
			FROM e LEFT JOIN forced f ON f.tree_id IS NOT DISTINCT FROM e.tree_id AND f.node_id = e.node_id
		),
		ranked AS (
			SELECT *, row_number() OVER (PARTITION BY tree_id, node_id ORDER BY p DESC, class_id) AS rank FROM probs
		),
		margins AS (
			SELECT tree_id, node_id,
			       max(p) FILTER (WHERE rank = 1) - COALESCE(max(p) FILTER (WHERE rank = 2), 0) AS margin
			FROM ranked GROUP BY ALL
		)
		SELECT r.tree_id, r.node_id, i.source, r.class_id, r.p, r.rank, m.margin, r.overridden
		FROM ranked r
		JOIN margins m ON m.tree_id IS NOT DISTINCT FROM r.tree_id AND m.node_id = r.node_id
		JOIN items i ON i.tree_id IS NOT DISTINCT FROM r.tree_id AND i.node_id = r.node_id
		%s
		ORDER BY r.tree_id, r.node_id, r.rank
	)",
	                              items_sql, cat_sql, vectors, sims_cols, similarity, sims_from, bonus, DoubleLiteral(beta),
	                              DoubleLiteral(tau), forced,
	                              top > 0 ? StringUtil::Format("WHERE r.rank <= %lld", (long long)top) : string());
	return CoverageSubquery(sql, context, fn);
}

} // namespace

void RegisterPivotClassifierFunctions(ExtensionLoader &loader) {
	auto tokens = LogicalType::LIST(LogicalType::LIST(LogicalType::DOUBLE));
	auto vec = LogicalType::LIST(LogicalType::DOUBLE);
	auto dist = LogicalType::MAP(LogicalType::VARCHAR, LogicalType::DOUBLE);
	RegisterScalar(loader, "maxsim", {tokens, tokens}, DATAZOO_GUARD(ANOFOX_SIMILARITY_BANNER, MaxsimFun),
	               compat_anofox::ScalarBind<&MaxsimBind>(), {"query_tokens", "document_tokens"},
	               "Late-interaction score (ColBERT-style MaxSim): each query token takes its maximum cosine over the "
	               "document tokens, and the maxima are averaged. Asymmetric. Token vectors must share one dimension; "
	               "a zero token has cosine 0. NULL or an empty list gives NULL.",
	               {"SELECT maxsim([[1.0, 0.0], [0.0, 1.0]], [[1.0, 0.0], [0.6, 0.8]]); -- 0.9"});
	RegisterScalar(loader, "bhattacharyya", {dist, dist}, DATAZOO_GUARD(ANOFOX_SIMILARITY_BANNER, BhattacharyyaFun),
	               compat_anofox::ScalarBind<&BhattacharyyaBind>(), {"p", "q"},
	               "Bhattacharyya coefficient of two class distributions MAP(class, probability): Σ √(p_k q_k). 1 for "
	               "identical distributions, 0 for disjoint ones; the soft label agreement L(q, c) of the coverage "
	               "kernel. Distributions are used as given (tree_coverage normalises them). Negative values are an "
	               "error; NULL gives NULL.",
	               {"SELECT bhattacharyya(MAP {'heating': 0.6, 'ventilation': 0.4}, MAP {'heating': 0.5, 'ventilation': "
	                "0.5}); -- 0.9949"});
	RegisterScalar(loader, "calibrated_cosine", {vec, vec, LogicalType::DOUBLE},
	               DATAZOO_GUARD(ANOFOX_SIMILARITY_BANNER, CalibratedCosineFun),
	               compat_anofox::ScalarBind<&CalibratedCosineBind>(), {"a", "b", "floor"},
	               "Cosine rescaled above a corpus floor: clip((cos - floor) / (1 - floor), 0, 1). Small sentence "
	               "encoders give unrelated texts a cosine around 0.85; estimate the floor with cosine_floor. floor "
	               "must be in [0, 1); NULL gives NULL.",
	               {"SELECT calibrated_cosine([1.0, 0.0], [0.8, 0.6], 0.5); -- 0.6"});

	RegisterTable(loader, "cosine_floor", {LogicalType::VARCHAR, LogicalType::VARCHAR}, {"table", "column"},
	              CosineFloorBindReplace, {{"sample", LogicalType::BIGINT}, {"seed", LogicalType::BIGINT}},
	              "Estimates the cosine floor b of a vector corpus: the median cosine of pairs of distinct vectors "
	              "(HLD §4.4). With at most `sample` pairs (default 10 000) all pairs count; otherwise `sample` pairs "
	              "are drawn deterministically from `seed` (default 7). Returns floor and n_pairs. Use the floor with "
	              "calibrated_cosine or as name_floor.",
	              {"SET VARIABLE name_floor = (SELECT floor FROM cosine_floor('name_vectors', 'vec'));"});

	RegisterTable(
	    loader, "classify_to_catalogue", {}, {}, ClassifyBindReplace,
	    {{"items_table", LogicalType::VARCHAR},
	     {"catalogue_table", LogicalType::VARCHAR},
	     {"mode", LogicalType::VARCHAR},
	     {"center_by_source", LogicalType::BOOLEAN},
	     {"attr_bonus", LogicalType::DOUBLE},
	     {"temperature", LogicalType::DOUBLE},
	     {"overrides_table", LogicalType::VARCHAR},
	     {"decisions_table", LogicalType::VARCHAR},
	     {"attrs_table", LogicalType::VARCHAR},
	     {"templates_table", LogicalType::VARCHAR},
	     {"name_vectors", LogicalType::VARCHAR},
	     {"name_floor", LogicalType::DOUBLE},
	     {"top", LogicalType::BIGINT}},
	    "Pivot classifier (HLD §4.4): maps every item onto the catalogue as a distribution p(class). Steps: per-source "
	    "mean-centring of the item vectors (center_by_source, centroid mode); cosine to the class vector, or "
	    "maxsim(tokens, exemplar_tokens) in mode 'late_interaction'; z-score across the catalogue; plus attr_bonus x "
	    "the attribute-template match (mean over the item's attributes of the best same-dimension template, name "
	    "similarity by calibrated cosine of name_vectors, or 1 without vectors); softmax with temperature; margin = p1 "
	    "- p2. Reviewed wordings (overrides_table: wording, source, class_id; matched on the item's label) and "
	    "per-item decisions (decisions_table: tree_id, node_id, class_id) give a one-hot p and overridden = true. "
	    "items_table: node_id, vec[, tree_id, source, label, tokens]; catalogue_table: class_id, vec[, "
	    "exemplar_tokens]; attrs_table: [tree_id,] node_id, name, dimension; templates_table: class_id, dimension[, "
	    "name, name_en, name_de]. Returns tree_id, node_id, source, class_id, p, rank, margin, overridden; tree_id, "
	    "node_id, class_id and p make it a label_table for tree_coverage.",
	    {"SELECT * FROM classify_to_catalogue(items_table := 'request_items', catalogue_table := 'catalogue', "
	     "top := 3);",
	     "SELECT * FROM classify_to_catalogue(items_table := 'unseen', catalogue_table := 'catalogue', "
	     "mode := 'late_interaction', top := 1);"});
}

} // namespace anofox
} // namespace duckdb
