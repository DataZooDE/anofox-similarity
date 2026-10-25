#include "modules/entity_clustering.hpp"
#include "modules/coverage_sql.hpp"
#include "compat_anofox/scalar_bind.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/parser/parsed_data/create_scalar_function_info.hpp"
#include "telemetry.hpp"
#include "anofox_similarity_banner.hpp"

#include <map>
#include <unordered_map>

namespace duckdb {
namespace anofox {

namespace {

//------------------------------------------------------------------------------
// connected_components: union-find over a list of pairs (HLD §4.9)
//------------------------------------------------------------------------------

struct UnionFind {
	vector<idx_t> parent;
	idx_t Add() {
		parent.push_back(parent.size());
		return parent.size() - 1;
	}
	idx_t Find(idx_t x) {
		while (parent[x] != x) {
			parent[x] = parent[parent[x]];
			x = parent[x];
		}
		return x;
	}
	void Union(idx_t a, idx_t b) {
		a = Find(a);
		b = Find(b);
		if (a != b) {
			parent[MaxValue(a, b)] = MinValue(a, b);
		}
	}
};

LogicalType ComponentType() {
	child_list_t<LogicalType> c {{"entity_id", LogicalType::VARCHAR}, {"cluster_id", LogicalType::VARCHAR}};
	return LogicalType::STRUCT(c);
}

void ConnectedComponentsFun(DataChunk &args, ExpressionState &, Vector &result) {
	{
		// malformed input is a clean error, not an internal one
		auto &t = args.data[0].GetType();
		bool ok = t.id() == LogicalTypeId::SQLNULL;
		if (t.id() == LogicalTypeId::LIST) {
			auto &child = ListType::GetChildType(t);
			if (child.id() == LogicalTypeId::SQLNULL) {
				ok = true;
			} else if (child.id() == LogicalTypeId::STRUCT) {
				if (StructType::GetChildTypes(child).size() < 2) {
					throw InvalidInputException("connected_components: each pair needs at least two fields");
				}
				ok = true;
			}
		}
		if (!ok) {
			throw InvalidInputException("connected_components: pairs must be a LIST of STRUCT(a, b), not %s",
			                            t.ToString());
		}
	}
	for (idx_t row = 0; row < args.size(); row++) {
		auto list = args.data[0].GetValue(row);
		if (list.IsNull()) {
			result.SetValue(row, Value(LogicalType::LIST(ComponentType())));
			continue;
		}
		std::map<string, idx_t> ids; // sorted: the smallest id names its cluster
		UnionFind uf;
		auto id_of = [&](const Value &v) -> optional_idx {
			if (v.IsNull()) {
				return optional_idx();
			}
			auto key = v.ToString();
			auto it = ids.find(key);
			if (it != ids.end()) {
				return it->second;
			}
			auto id = uf.Add();
			ids.emplace(key, id);
			return id;
		};
		for (auto &pair : ListValue::GetChildren(list)) {
			if (pair.IsNull()) {
				continue;
			}
			auto &fields = StructValue::GetChildren(pair);
			if (fields.size() < 2) {
				throw InvalidInputException("connected_components: each pair must be a STRUCT with two fields");
			}
			auto a = id_of(fields[0]);
			auto b = id_of(fields[1]);
			if (a.IsValid() && b.IsValid()) {
				uf.Union(a.GetIndex(), b.GetIndex());
			}
		}
		// cluster id: the smallest entity id of the component
		std::unordered_map<idx_t, string> names;
		for (auto &kv : ids) {
			auto root = uf.Find(kv.second);
			names.emplace(root, kv.first); // first visit in sorted order = smallest id
		}
		vector<Value> out;
		for (auto &kv : ids) {
			child_list_t<Value> s;
			s.emplace_back("entity_id", Value(kv.first));
			s.emplace_back("cluster_id", Value(names[uf.Find(kv.second)]));
			out.push_back(Value::STRUCT(std::move(s)));
		}
		result.SetValue(row, Value::LIST(ComponentType(), std::move(out)));
	}
	if (args.AllConstant()) {
		result.SetVectorType(VectorType::CONSTANT_VECTOR);
	}
}

unique_ptr<FunctionData> ConnectedComponentsBind() {
	PostHogTelemetry::Instance().RecordFunctionCall("connected_components");
	return nullptr;
}

//------------------------------------------------------------------------------
// cluster_matches
//------------------------------------------------------------------------------

unique_ptr<TableRef> ClusterMatchesBindReplace(ClientContext &context, TableFunctionBindInput &input) {
	PostHogTelemetry::Instance().RecordFunctionCall("cluster_matches");
	CoverageParamReader p {context, input.named_parameters, "cluster_matches"};
	auto table = p.Positional(input, 0, "pairs_table", {});
	double threshold = p.Dbl("threshold", 0.8);
	if (threshold < 0 || threshold > 1) {
		throw BinderException("%s: threshold must be between 0 and 1", p.fn);
	}
	auto left = p.Column(table, "left_col", "a");
	auto right = p.Column(table, "right_col", "b");
	auto score = p.Column(table, "score_col", "score");
	// every entity of the pairs table takes part; pairs at or above the threshold join clusters
	auto sql = StringUtil::Format(R"(
		WITH p AS (SELECT CAST(%s AS VARCHAR) AS a, CAST(%s AS VARCHAR) AS b, CAST(%s AS DOUBLE) AS s FROM %s),
		entities AS (SELECT a AS e FROM p WHERE a IS NOT NULL UNION SELECT b FROM p WHERE b IS NOT NULL),
		edges AS (
			SELECT a, b FROM p WHERE s >= %s AND a IS NOT NULL AND b IS NOT NULL
			UNION ALL SELECT e, e FROM entities
		),
		components AS (SELECT unnest(connected_components(list(struct_pack(a := a, b := b)))) AS c FROM edges)
		SELECT c.entity_id AS entity_id, c.cluster_id AS cluster_id, count(*) OVER (PARTITION BY c.cluster_id) AS cluster_size
		FROM components
		ORDER BY entity_id
	)",
	                              left, right, score, QuotedIdentifier(table), DoubleLiteral(threshold));
	return CoverageSubquery(sql, context, p.fn);
}

} // namespace

void RegisterEntityClusteringFunctions(ExtensionLoader &loader) {
	{
		ScalarFunction f(duckdb_compat::FunctionName("connected_components"), {LogicalType::ANY},
		                 LogicalType::LIST(ComponentType()),
		                 DATAZOO_GUARD(ANOFOX_SIMILARITY_BANNER, ConnectedComponentsFun),
		                 compat_anofox::ScalarBind<&ConnectedComponentsBind>());
		duckdb_compat::SetFallible(f);
		CreateScalarFunctionInfo info(f);
		FunctionDescription d;
		d.description = "Union-find over a LIST of pairs STRUCT(a, b): returns LIST(STRUCT(entity_id, cluster_id)), one "
		                "entry per entity sorted by id; cluster_id is the smallest entity id of its connected component. "
		                "NULL ids are ignored. The building block of cluster_matches.";
		d.examples = {"SELECT connected_components([{'a': 'x', 'b': 'y'}, {'a': 'y', 'b': 'z'}]);"};
		d.categories = {"coverage", "clustering"};
		d.parameter_names = {"pairs"};
		d.parameter_types = {LogicalType::ANY};
		info.descriptions.push_back(std::move(d));
		duckdb_compat::NameArgumentsFromDescriptions(info);
		loader.RegisterFunction(std::move(info));
	}
	auto vc = LogicalType(LogicalTypeId::VARCHAR);
	RegisterCoverageTableFunction(
	    loader, "cluster_matches", {vc}, {"pairs_table"}, ClusterMatchesBindReplace,
	    {{"threshold", LogicalType::DOUBLE}, {"left_col", vc}, {"right_col", vc}, {"score_col", vc}},
	    "Entity clustering (HLD §4.9): a C++ union-find over the pairs whose score (score_col, default 'score') is at "
	    "or above threshold (default 0.8). Returns entity_id, cluster_id (the smallest member id) and cluster_size "
	    "for every entity in the pairs table (left_col/right_col, default 'a'/'b'); entities without a pair above "
	    "the threshold form their own cluster. For a graph view, see the DuckPGQ recipe in the API reference.",
	    {"SELECT * FROM cluster_matches('customer_pairs', threshold := 0.85);"}, {"coverage", "clustering"});
}

} // namespace anofox
} // namespace duckdb
