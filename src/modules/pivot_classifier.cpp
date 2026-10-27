#include "modules/pivot_classifier.hpp"
#include "modules/coverage_sql.hpp"
#include "modules/property_tree.hpp"
#include "duckdb_compat.hpp"
#include "duckdb_compat/list_vector.hpp"
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

vector<double> Doubles(const Value &list, const char *fn) {
	vector<double> out;
	for (auto &x : ListValue::GetChildren(list)) {
		double v = x.IsNull() ? NAN : x.GetValue<double>();
		if (!std::isfinite(v)) {
			throw InvalidInputException("%s: vectors must not contain non-finite or NULL elements", fn);
		}
		out.push_back(v);
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

//! Direct reader of a LIST(LIST(DOUBLE)) vector (no Value boxing on the hot path).
class TokenLists {
public:
	TokenLists(Vector &input, idx_t count) {
		duckdb_compat::ToUnifiedFormat(input, count, outer);
		auto &tokens = ListVector::GetEntry(input);
		duckdb_compat::ToUnifiedFormat(tokens, ListVector::GetListSize(input), inner);
		auto &values = ListVector::GetEntry(tokens);
		duckdb_compat::ToUnifiedFormat(values, ListVector::GetListSize(tokens), leaf);
	}
	bool IsNull(idx_t row) const {
		return !outer.validity.RowIsValid(outer.sel->get_index(row));
	}
	//! The row's token vectors (NULL tokens skipped); an error on NULL or non-finite elements.
	void Read(idx_t row, vector<vector<double>> &out) const {
		out.clear();
		auto entry = UnifiedVectorFormat::GetData<list_entry_t>(outer)[outer.sel->get_index(row)];
		for (idx_t t = entry.offset; t < entry.offset + entry.length; t++) {
			auto ti = inner.sel->get_index(t);
			if (!inner.validity.RowIsValid(ti)) {
				continue;
			}
			auto token = UnifiedVectorFormat::GetData<list_entry_t>(inner)[ti];
			vector<double> v(token.length);
			for (idx_t k = 0; k < token.length; k++) {
				auto li = leaf.sel->get_index(token.offset + k);
				double x = leaf.validity.RowIsValid(li) ? UnifiedVectorFormat::GetData<double>(leaf)[li] : NAN;
				if (!std::isfinite(x)) {
					throw InvalidInputException("maxsim: vectors must not contain non-finite or NULL elements");
				}
				v[k] = x;
			}
			out.push_back(std::move(v));
		}
	}

private:
	UnifiedVectorFormat outer, inner, leaf;
};

void MaxsimFun(DataChunk &args, ExpressionState &, Vector &result) {
	auto count = args.size();
	TokenLists q(args.data[0], count), d(args.data[1], count);
	auto out = duckdb_compat::FlatData<double>(result);
	auto &validity = duckdb_compat::MutableValidity(result);
	vector<vector<double>> qs, ds;
	for (idx_t row = 0; row < count; row++) {
		if (q.IsNull(row) || d.IsNull(row)) {
			validity.SetInvalid(row);
			continue;
		}
		q.Read(row, qs);
		d.Read(row, ds);
		if (qs.empty() || ds.empty()) {
			validity.SetInvalid(row);
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
		out[row] = sum / double(qs.size());
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
		double cos = Cosine(Doubles(a, "calibrated_cosine"), Doubles(b, "calibrated_cosine"), "calibrated_cosine");
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
	RegisterCoverageScalar(loader, {name, LogicalType::DOUBLE, fn, bind, description, {"coverage", "classification"}},
	                       {{std::move(args), std::move(names), std::move(examples), ""}});
}

//------------------------------------------------------------------------------
// Named parameters of the table functions
//------------------------------------------------------------------------------



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
	CoverageParamReader named {context, input.named_parameters, fn};
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

//! The validated parameters of classify_to_catalogue and the optional columns found.
struct ClassifyParams {
	bool late = false; // mode 'late_interaction'
	bool centre = true;
	double beta = 1.0, tau = 0.3, name_floor = 0.85;
	int64_t top = 0; // 0: all classes
	int64_t min_source_items = 10; // smaller sources are centred with the batch mean
	string items, catalogue, overrides, answers, attrs, templates, name_vectors;
	bool items_tree = false, items_source = false, items_label = false, items_vec = false;
	bool attrs_tree = false, attrs_name = false, overrides_source = false, answers_tree = false;
	vector<string> template_names; // name, name_en, name_de as present
};

ClassifyParams ReadClassifyParams(const CoverageParamReader &r) {
	ClassifyParams p;
	auto mode = StringUtil::Lower(r.Str("mode", "centroid"));
	if (mode != "centroid" && mode != "late_interaction") {
		throw BinderException("%s: mode must be 'centroid' or 'late_interaction'", r.fn);
	}
	p.late = mode == "late_interaction";
	p.items = r.Table("items_table", "item_vectors",
	                  p.late ? vector<string> {"node_id", "tokens"} : vector<string> {"node_id", "vec"});
	if (p.items.empty()) {
		throw BinderException("%s: items_table must not be NULL", r.fn);
	}
	p.catalogue = r.Table("catalogue_table", "catalogue_vectors",
	                      p.late ? vector<string> {"class_id", "exemplar_tokens"} : vector<string> {"class_id", "vec"});
	if (p.catalogue.empty()) {
		throw BinderException("%s: catalogue_table must not be NULL", r.fn);
	}
	if (auto v = r.Get("center_by_source"); v && !v->IsNull()) {
		p.centre = v->GetValue<bool>();
	}
	p.beta = r.Dbl("attr_bonus", p.beta);
	p.tau = r.Dbl("temperature", p.tau);
	if (p.tau <= 0) {
		throw BinderException("%s: temperature must be positive", r.fn);
	}
	if (p.beta < 0) {
		throw BinderException("%s: attr_bonus must not be negative", r.fn);
	}
	p.name_floor = r.Dbl("name_floor", p.name_floor);
	if (p.name_floor < 0 || p.name_floor >= 1) {
		throw BinderException("%s: name_floor must be in [0, 1)", r.fn);
	}
	p.min_source_items = r.Int("min_source_items", p.min_source_items);
	if (p.min_source_items < 1) {
		throw BinderException("%s: min_source_items must be at least 1", r.fn);
	}
	p.top = r.Int("top", 0);
	if (r.Get("top") && !r.Get("top")->IsNull() && p.top <= 0) {
		throw BinderException("%s: top must be positive", r.fn);
	}
	p.overrides = r.Table("overrides_table", "", {"wording", "class_id"});
	p.answers = r.Table("answers_table", "", {"node_id", "class_id"});
	p.attrs = r.Table("attrs_table", "", {"node_id", "dimension"});
	p.templates = r.Table("templates_table", "", {"class_id", "dimension"});
	p.name_vectors = r.Table("name_vectors", "", {"name", "vec"});
	p.items_tree = r.Has(p.items, "tree_id");
	p.items_source = r.Has(p.items, "source");
	p.items_label = r.Has(p.items, "label");
	p.items_vec = r.Has(p.items, "vec");
	p.attrs_tree = !p.attrs.empty() && r.Has(p.attrs, "tree_id");
	p.attrs_name = !p.attrs.empty() && r.Has(p.attrs, "name");
	p.overrides_source = !p.overrides.empty() && r.Has(p.overrides, "source");
	p.answers_tree = !p.answers.empty() && r.Has(p.answers, "tree_id");
	if (!p.templates.empty()) {
		for (auto c : {"name", "name_en", "name_de"}) {
			if (r.Has(p.templates, c)) {
				p.template_names.push_back(StringUtil::Format("CAST(%s AS VARCHAR)", c));
			}
		}
	}
	// node ids repeat across trees: side tables must carry tree_id when the items do
	if (p.items_tree && !p.attrs.empty() && !p.attrs_tree) {
		throw BinderException("%s: attrs_table needs a tree_id column when items_table has one", r.fn);
	}
	if (p.items_tree && !p.answers.empty() && !p.answers_tree) {
		throw BinderException("%s: answers_table needs a tree_id column when items_table has one", r.fn);
	}
	if (!p.overrides.empty() && !p.items_label) {
		throw BinderException("%s: items_table needs a label column to apply overrides_table", r.fn);
	}
	return p;
}

//! items (tree_id, node_id, source, label, vec, tokens): items without a vector are skipped.
string ClassifyItemsSQL(const ClassifyParams &p) {
	return StringUtil::Format(
	    "SELECT %s AS tree_id, CAST(node_id AS VARCHAR) AS node_id, %s AS source, %s AS label, %s AS vec, %s AS tokens "
	    "FROM %s WHERE %s IS NOT NULL",
	    CastOrNullSQL(p.items_tree, "tree_id", "VARCHAR"), CastOrNullSQL(p.items_source, "source", "VARCHAR"),
	    CastOrNullSQL(p.items_label, "label", "VARCHAR"), CastOrNullSQL(p.items_vec, "vec", "DOUBLE[]"),
	    CastOrNullSQL(p.late, "tokens", "DOUBLE[][]"), QuotedIdentifier(p.items), p.late ? "tokens" : "vec");
}

//! catalogue (class_id, vec, tokens). In centroid mode the item and catalogue vectors must share
//! one dimension; the check runs with the catalogue, before any similarity is computed.
string ClassifyCatalogueSQL(const ClassifyParams &p) {
	string dimension_check;
	if (!p.late) {
		dimension_check = StringUtil::Format(
		    " WHERE (SELECT CASE WHEN (SELECT count(DISTINCT len(vec)) FROM items) > 1 OR "
		    "(SELECT count(DISTINCT len(CAST(vec AS DOUBLE[]))) FROM %s) > 1 OR "
		    "(SELECT min(len(vec)) FROM items) <> (SELECT min(len(CAST(vec AS DOUBLE[]))) FROM %s) THEN "
		    "error('classify_to_catalogue: item vectors have dimension ' || (SELECT string_agg(DISTINCT "
		    "CAST(len(vec) AS VARCHAR), ', ') FROM items) || ', catalogue vectors ' || (SELECT string_agg(DISTINCT "
		    "CAST(len(CAST(vec AS DOUBLE[])) AS VARCHAR), ', ') FROM %s)) END) IS NULL",
		    QuotedIdentifier(p.catalogue), QuotedIdentifier(p.catalogue), QuotedIdentifier(p.catalogue));
	}
	return StringUtil::Format("SELECT CAST(class_id AS VARCHAR) AS class_id, %s AS vec, %s AS tokens FROM %s%s",
	                          CastOrNullSQL(!p.late, "vec", "DOUBLE[]"),
	                          CastOrNullSQL(p.late, "exemplar_tokens", "DOUBLE[][]"), QuotedIdentifier(p.catalogue),
	                          dimension_check);
}

//! Step 1: the item vectors, mean-centred in centroid mode. A source with at least
//! min_source_items items is centred with its own mean (this removes its boilerplate); a smaller
//! source with the mean of the whole batch, when the batch has that many items; otherwise the
//! vectors stay as they are (a mean of one or two items would cancel their content).
string CentredVectorsSQL(const ClassifyParams &p) {
	if (p.late) {
		return "SELECT tree_id, node_id, NULL::DOUBLE[] AS vec FROM items";
	}
	if (!p.centre) {
		return "SELECT tree_id, node_id, vec FROM items";
	}
	return StringUtil::Format(R"(
			WITH el AS (SELECT tree_id, node_id, source, unnest(vec) AS v, generate_subscripts(vec, 1) AS i FROM items),
			n_source AS (SELECT source, count(*) AS n FROM items GROUP BY ALL),
			n_batch AS (SELECT count(*) AS n FROM items),
			source_mean AS (SELECT source, i, avg(v) AS m FROM el GROUP BY ALL),
			batch_mean AS (SELECT i, avg(v) AS m FROM el GROUP BY i)
			SELECT e.tree_id, e.node_id,
			       list(e.v - CASE WHEN ns.n >= %lld THEN sm.m WHEN (SELECT n FROM n_batch) >= %lld THEN bm.m ELSE 0 END
			            ORDER BY e.i) AS vec
			FROM el e
			JOIN n_source ns ON ns.source IS NOT DISTINCT FROM e.source
			JOIN source_mean sm ON sm.source IS NOT DISTINCT FROM e.source AND sm.i = e.i
			JOIN batch_mean bm ON bm.i = e.i
			GROUP BY e.tree_id, e.node_id)",
	                          (long long)p.min_source_items, (long long)p.min_source_items);
}

//! Step 2: the similarity of every item to every class.
string SimilaritySQL(const ClassifyParams &p) {
	if (p.late) {
		return "SELECT i.tree_id, i.node_id, k.class_id, maxsim(i.tokens, k.tokens) AS s FROM items i CROSS JOIN catalogue k";
	}
	return "SELECT v.tree_id, v.node_id, k.class_id, CASE WHEN list_dot_product(v.vec, v.vec) > 0 AND "
	       "list_dot_product(k.vec, k.vec) > 0 THEN list_cosine_similarity(v.vec, k.vec) ELSE 0 END AS s "
	       "FROM vectors v CROSS JOIN catalogue k";
}

//! Step 3: the attribute-template bonus: the mean over the item's attributes of the best
//! same-dimension template (name similarity by calibrated cosine, or 1 without vectors).
string TemplateBonusSQL(const ClassifyParams &p) {
	if (p.attrs.empty() || p.templates.empty() || p.beta <= 0) {
		return "SELECT NULL::VARCHAR AS tree_id, NULL::VARCHAR AS node_id, NULL::VARCHAR AS class_id, "
		       "NULL::DOUBLE AS b WHERE false";
	}
	string tree_join = p.attrs_tree ? "i.tree_id IS NOT DISTINCT FROM CAST(a.tree_id AS VARCHAR)" : "true";
	string tnames = p.template_names.empty() ? "[NULL::VARCHAR]" : "[" + StringUtil::Join(p.template_names, ", ") + "]";
	string s_name = "1.0::DOUBLE";
	string vec_joins;
	if (!p.name_vectors.empty()) {
		vec_joins = " LEFT JOIN (" + NameVectorsSQL(p.name_vectors) + ") va ON va.name = ia.name LEFT JOIN (" +
		            NameVectorsSQL(p.name_vectors) + ") vt ON vt.name = t.tname";
		s_name = StringUtil::Format(
		    "CASE WHEN va.vec IS NULL OR vt.vec IS NULL THEN 0.0 ELSE calibrated_cosine(va.vec, vt.vec, %s) END",
		    DoubleLiteral(p.name_floor));
	}
	return StringUtil::Format(R"(
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
	                          CastOrNullSQL(p.attrs_name, "name", "VARCHAR", "a"), QuotedIdentifier(p.attrs), tree_join,
	                          tnames, QuotedIdentifier(p.templates), s_name, vec_joins);
}

//! Classes fixed by review: overrides per wording (BR-32), answers per item (BR-33, preferred).
string ForcedClassesSQL(const ClassifyParams &p) {
	vector<string> parts;
	if (!p.overrides.empty()) {
		parts.push_back(StringUtil::Format(
		    "SELECT i.tree_id, i.node_id, first(CAST(o.class_id AS VARCHAR) ORDER BY %s, CAST(o.class_id AS VARCHAR)) "
		    "AS cls, 2 AS prio FROM items i JOIN %s o ON CAST(o.wording AS VARCHAR) = i.label%s GROUP BY ALL",
		    p.overrides_source ? "o.source IS NULL" : "1", QuotedIdentifier(p.overrides),
		    p.overrides_source ? " AND (o.source IS NULL OR CAST(o.source AS VARCHAR) = i.source)" : ""));
	}
	if (!p.answers.empty()) {
		parts.push_back(StringUtil::Format(
		    "SELECT i.tree_id, i.node_id, first(CAST(d.class_id AS VARCHAR) ORDER BY CAST(d.class_id AS VARCHAR)) AS cls, "
		    "1 AS prio FROM items i JOIN %s d ON CAST(d.node_id AS VARCHAR) = i.node_id%s GROUP BY ALL",
		    QuotedIdentifier(p.answers),
		    p.answers_tree ? " AND i.tree_id IS NOT DISTINCT FROM CAST(d.tree_id AS VARCHAR)" : ""));
	}
	if (parts.empty()) {
		return "SELECT NULL::VARCHAR AS tree_id, NULL::VARCHAR AS node_id, NULL::VARCHAR AS cls WHERE false";
	}
	return "SELECT tree_id, node_id, first(cls ORDER BY prio) AS cls FROM (" + StringUtil::Join(parts, " UNION ALL ") +
	       ") GROUP BY ALL";
}

//! Integrity checks of the inputs, as one value the final SELECT depends on.
string ClassifyGuardSQL(const ClassifyParams &p) {
	string col = p.late ? "tokens" : "vec";
	string bad = p.late ? StringUtil::Format("%s IS NULL OR len([x FOR x IN flatten(%s) IF x IS NULL OR NOT isfinite(x)]) > 0",
	                                         col, col)
	                    : StringUtil::Format("%s IS NULL OR len([x FOR x IN %s IF x IS NULL OR NOT isfinite(x)]) > 0",
	                                         col, col);
	string empty = p.late ? StringUtil::Format(
	                            "%s IS NOT NULL AND len([t FOR t IN %s IF t IS NOT NULL AND len(t) > 0]) = 0", col, col)
	                      : string("false");
	return StringUtil::Format(R"(
			SELECT CASE
				WHEN NOT EXISTS (SELECT 1 FROM catalogue) THEN error('classify_to_catalogue: catalogue_table is empty')
				WHEN EXISTS (SELECT 1 FROM catalogue WHERE class_id IS NULL)
					THEN error('classify_to_catalogue: class_id in catalogue_table must not be NULL')
				WHEN EXISTS (SELECT 1 FROM catalogue GROUP BY class_id HAVING count(*) > 1)
					THEN error('classify_to_catalogue: catalogue_table has duplicate class_id ''' ||
					           (SELECT min(class_id) FROM (SELECT class_id FROM catalogue GROUP BY class_id HAVING count(*) > 1)) || '''')
				WHEN EXISTS (SELECT 1 FROM catalogue WHERE %s)
					THEN error('classify_to_catalogue: catalogue_table has an empty token list')
				WHEN EXISTS (SELECT 1 FROM catalogue WHERE %s)
					THEN error('classify_to_catalogue: catalogue_table has non-finite, NULL or missing vector elements')
				WHEN EXISTS (SELECT 1 FROM items WHERE node_id IS NULL)
					THEN error('classify_to_catalogue: node_id must not be NULL in items_table')
				WHEN EXISTS (SELECT 1 FROM items GROUP BY tree_id, node_id HAVING count(*) > 1)
					THEN error('classify_to_catalogue: items_table has a duplicate item (tree_id, node_id) ''' ||
					           (SELECT min(node_id) FROM (SELECT node_id FROM items GROUP BY tree_id, node_id HAVING count(*) > 1)) || '''')
				WHEN EXISTS (SELECT 1 FROM items WHERE %s)
					THEN error('classify_to_catalogue: items_table has an empty token list')
				WHEN EXISTS (SELECT 1 FROM items WHERE %s)
					THEN error('classify_to_catalogue: items_table has non-finite or NULL vector elements')
				WHEN EXISTS (SELECT 1 FROM forced WHERE cls NOT IN (SELECT class_id FROM catalogue))
					THEN error('classify_to_catalogue: class ''' || (SELECT min(cls) FROM forced WHERE cls NOT IN (SELECT class_id FROM catalogue)) ||
					           ''' from overrides_table or answers_table is not in catalogue_table')
			END AS g)",
	                          empty, bad, empty, bad);
}

unique_ptr<TableRef> ClassifyBindReplace(ClientContext &context, TableFunctionBindInput &input) {
	PostHogTelemetry::Instance().RecordFunctionCall("classify_to_catalogue");
	CoverageParamReader reader {context, input.named_parameters, "classify_to_catalogue"};
	auto p = ReadClassifyParams(reader);
	// steps 3-5: z-score across the catalogue (+ beta x bonus), softmax with temperature, margin;
	// reviewed classes give a one-hot distribution
	string scoring = StringUtil::Format(R"(
		zs AS (
			SELECT tree_id, node_id, class_id,
			       (s - avg(s) OVER w) / CASE WHEN stddev_pop(s) OVER w > 1e-12 THEN stddev_pop(s) OVER w ELSE 1 END AS z
			FROM sims WINDOW w AS (PARTITION BY tree_id, node_id)
		),
		scored AS (
			SELECT z.tree_id, z.node_id, z.class_id, z.z + %s * COALESCE(b.b, 0) AS score
			FROM zs z LEFT JOIN bonus b ON b.tree_id IS NOT DISTINCT FROM z.tree_id AND b.node_id = z.node_id
			                             AND b.class_id = z.class_id
		),
		e AS (
			SELECT *, exp((score - max(score) OVER (PARTITION BY tree_id, node_id)) / %s) AS e FROM scored
		),
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
		))",
	                                    DoubleLiteral(p.beta), DoubleLiteral(p.tau));
	auto sql = "WITH items AS (" + ClassifyItemsSQL(p) + "),\n\t\tcatalogue AS (" + ClassifyCatalogueSQL(p) +
	           "),\n\t\tvectors AS (" + CentredVectorsSQL(p) + "),\n\t\tsims AS (" + SimilaritySQL(p) +
	           "),\n\t\tbonus AS (" + TemplateBonusSQL(p) + "),\n\t\tforced AS (" + ForcedClassesSQL(p) + ")," + scoring +
	           ",\n\t\tguard AS (" + ClassifyGuardSQL(p) + R"()
		SELECT r.tree_id, r.node_id, i.source, r.class_id, r.p, r.rank, m.margin, r.overridden
		FROM ranked r
		JOIN margins m ON m.tree_id IS NOT DISTINCT FROM r.tree_id AND m.node_id = r.node_id
		JOIN items i ON i.tree_id IS NOT DISTINCT FROM r.tree_id AND i.node_id = r.node_id
		CROSS JOIN guard
		WHERE guard.g IS NULL)" +
	           (p.top > 0 ? StringUtil::Format(" AND r.rank <= %lld", (long long)p.top) : string()) +
	           "\n\t\tORDER BY r.tree_id, r.node_id, r.rank";
	return CoverageSubquery(sql, context, reader.fn);
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

	RegisterCoverageTableFunction(loader, "cosine_floor", {LogicalType::VARCHAR, LogicalType::VARCHAR}, {"table", "column"},
	              CosineFloorBindReplace, {{"sample", LogicalType::BIGINT}, {"seed", LogicalType::BIGINT}},
	              "Estimates the cosine floor b of a vector corpus: the median cosine of pairs of distinct vectors "
	              "(HLD §4.4). With at most `sample` pairs (default 10 000) all pairs count; otherwise `sample` pairs "
	              "are drawn deterministically from `seed` (default 7). Returns floor and n_pairs. Use the floor with "
	              "calibrated_cosine or as name_floor.",
	              {"SET VARIABLE name_floor = (SELECT floor FROM cosine_floor('name_vectors', 'vec'));"}, {"coverage", "classification"});

	RegisterCoverageTableFunction(
	    loader, "classify_to_catalogue", {}, {}, ClassifyBindReplace,
	    {{"items_table", LogicalType::VARCHAR},
	     {"catalogue_table", LogicalType::VARCHAR},
	     {"mode", LogicalType::VARCHAR},
	     {"center_by_source", LogicalType::BOOLEAN},
	     {"attr_bonus", LogicalType::DOUBLE},
	     {"temperature", LogicalType::DOUBLE},
	     {"overrides_table", LogicalType::VARCHAR},
	     {"answers_table", LogicalType::VARCHAR},
	     {"attrs_table", LogicalType::VARCHAR},
	     {"templates_table", LogicalType::VARCHAR},
	     {"name_vectors", LogicalType::VARCHAR},
	     {"name_floor", LogicalType::DOUBLE},
	     {"top", LogicalType::BIGINT},
	     {"min_source_items", LogicalType::BIGINT}},
	    "Pivot classifier (HLD §4.4): maps every item onto the catalogue as a distribution p(class). Steps: per-source "
	    "mean-centring of the item vectors (center_by_source, centroid mode; sources with fewer than "
	    "min_source_items items, default 10, use the batch mean); cosine to the class vector, or "
	    "maxsim(tokens, exemplar_tokens) in mode 'late_interaction'; z-score across the catalogue; plus attr_bonus x "
	    "the attribute-template match (mean over the item's attributes of the best same-dimension template, name "
	    "similarity by calibrated cosine of name_vectors, or 1 without vectors); softmax with temperature; margin = p1 "
	    "- p2. Reviewed wordings (overrides_table: wording, source, class_id; matched on the item's label) and "
	    "per-item decisions (answers_table: tree_id, node_id, class_id) give a one-hot p and overridden = true. "
	    "items_table: node_id, vec[, tree_id, source, label, tokens]; catalogue_table: class_id, vec[, "
	    "exemplar_tokens]; attrs_table: [tree_id,] node_id, name, dimension; templates_table: class_id, dimension[, "
	    "name, name_en, name_de]. Returns tree_id, node_id, source, class_id, p, rank, margin, overridden; tree_id, "
	    "node_id, class_id and p make it a label_table for tree_coverage.",
	    {"SELECT * FROM classify_to_catalogue(items_table := 'request_items', catalogue_table := 'catalogue', "
	     "top := 3);",
	     "SELECT * FROM classify_to_catalogue(items_table := 'unseen', catalogue_table := 'catalogue', "
	     "mode := 'late_interaction', top := 1);"},
	    {"coverage", "classification"});
}

} // namespace anofox
} // namespace duckdb
