#include "modules/set_measures.hpp"
#include "duckdb_compat.hpp"
#include "compat_anofox/scalar_bind.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/parser/parsed_data/create_scalar_function_info.hpp"
#include "telemetry.hpp"
#include "anofox_similarity_banner.hpp"

#include <unordered_map>
#include <unordered_set>

namespace duckdb {
namespace anofox {

namespace {

struct ValueHash {
	size_t operator()(const Value &val) const {
		return val.Hash();
	}
};

struct ValueEqual {
	bool operator()(const Value &a, const Value &b) const {
		return a == b;
	}
};

using ValueSet = std::unordered_set<Value, ValueHash, ValueEqual>;
using WeightMap = std::unordered_map<Value, double, ValueHash, ValueEqual>;

//! The overlap of a query and a candidate: Σmin, Σ(q-c)+ and Σ(c-q)+, plus the
//! totals. For sets every element weighs 1.
struct Overlap {
	double shared = 0;
	double missing = 0;
	double surplus = 0;
	double query_total = 0;
	double candidate_total = 0;
};

ValueSet ToSet(const Value &list) {
	ValueSet set;
	for (auto &child : ListValue::GetChildren(list)) {
		if (!child.IsNull()) {
			set.insert(child);
		}
	}
	return set;
}

Overlap SetOverlap(const Value &query, const Value &candidate) {
	auto q = ToSet(query);
	auto c = ToSet(candidate);
	Overlap o;
	for (auto &v : q) {
		if (c.count(v)) {
			o.shared += 1;
		}
	}
	o.query_total = static_cast<double>(q.size());
	o.candidate_total = static_cast<double>(c.size());
	o.missing = o.query_total - o.shared;
	o.surplus = o.candidate_total - o.shared;
	return o;
}

WeightMap ToWeights(const Value &map) {
	if (map.type().id() != LogicalTypeId::MAP) {
		throw InvalidInputException("weighted set measures expect MAP arguments, got %s", map.type().ToString());
	}
	WeightMap weights;
	for (auto &entry : ListValue::GetChildren(map)) {
		auto &kv = StructValue::GetChildren(entry);
		auto &key = kv[0];
		auto &weight = kv[1];
		if (key.IsNull()) {
			continue;
		}
		double w = 1.0;
		if (!weight.IsNull()) {
			if (!weight.type().IsNumeric()) {
				throw InvalidInputException("weights must be numeric, got '%s'", weight.ToString());
			}
			w = weight.DefaultCastAs(LogicalType::DOUBLE).GetValue<double>();
		}
		if (w < 0) {
			throw InvalidInputException("weights must be non-negative, got %g", w);
		}
		weights[key] += w;
	}
	return weights;
}

Overlap WeightedOverlap(const Value &query, const Value &candidate) {
	auto q = ToWeights(query);
	auto c = ToWeights(candidate);
	Overlap o;
	for (auto &kv : q) {
		auto it = c.find(kv.first);
		double cw = it == c.end() ? 0.0 : it->second;
		o.shared += std::min(kv.second, cw);
		o.missing += std::max(kv.second - cw, 0.0);
		o.query_total += kv.second;
	}
	for (auto &kv : c) {
		auto it = q.find(kv.first);
		double qw = it == q.end() ? 0.0 : it->second;
		o.surplus += std::max(kv.second - qw, 0.0);
		o.candidate_total += kv.second;
	}
	return o;
}

enum class Measure { TVERSKY, CONTAINMENT };

//! Result for one row, or false for NULL.
bool Compute(const Overlap &o, Measure measure, double alpha, double beta, double &out) {
	if (measure == Measure::CONTAINMENT) {
		if (o.query_total == 0) {
			return false; // an empty request is not contained "fully"
		}
		out = o.shared / o.query_total;
		return true;
	}
	double denominator = o.shared + alpha * o.missing + beta * o.surplus;
	if (denominator == 0) {
		if (o.query_total == 0 && o.candidate_total == 0) {
			out = 0.0; // two empty inputs: the jaccard_similarity convention
			return true;
		}
		return false;
	}
	out = o.shared / denominator;
	return true;
}

template <Overlap (*OVERLAP)(const Value &, const Value &), Measure MEASURE>
void SetMeasureFun(DataChunk &args, ExpressionState &, Vector &result) {
	auto count = args.size();
	bool with_weights = args.ColumnCount() == 4;
	result.SetVectorType(VectorType::FLAT_VECTOR);
	auto result_data = duckdb_compat::FlatData<double>(result);
	auto &validity = duckdb_compat::MutableValidity(result);
	for (idx_t i = 0; i < count; i++) {
		auto query = args.data[0].GetValue(i);
		auto candidate = args.data[1].GetValue(i);
		double alpha = 1.0, beta = 1.0;
		bool null_parameter = false;
		if (with_weights) {
			auto a = args.data[2].GetValue(i);
			auto b = args.data[3].GetValue(i);
			null_parameter = a.IsNull() || b.IsNull();
			if (!null_parameter) {
				alpha = a.GetValue<double>();
				beta = b.GetValue<double>();
				if (alpha < 0 || beta < 0) {
					throw InvalidInputException("alpha and beta must be non-negative");
				}
			}
		}
		double value;
		if (query.IsNull() || candidate.IsNull() || null_parameter ||
		    !Compute(OVERLAP(query, candidate), MEASURE, alpha, beta, value)) {
			validity.SetInvalid(i);
			continue;
		}
		result_data[i] = value;
	}
	if (args.AllConstant()) {
		result.SetVectorType(VectorType::CONSTANT_VECTOR);
	}
}

void TverskyFun(DataChunk &args, ExpressionState &state, Vector &result) {
	SetMeasureFun<SetOverlap, Measure::TVERSKY>(args, state, result);
}
void ContainmentFun(DataChunk &args, ExpressionState &state, Vector &result) {
	SetMeasureFun<SetOverlap, Measure::CONTAINMENT>(args, state, result);
}
void WeightedTverskyFun(DataChunk &args, ExpressionState &state, Vector &result) {
	SetMeasureFun<WeightedOverlap, Measure::TVERSKY>(args, state, result);
}
void WeightedContainmentFun(DataChunk &args, ExpressionState &state, Vector &result) {
	SetMeasureFun<WeightedOverlap, Measure::CONTAINMENT>(args, state, result);
}

static unique_ptr<FunctionData> TverskyBind() {
	PostHogTelemetry::Instance().RecordFunctionCall("tversky_similarity");
	return nullptr;
}
static unique_ptr<FunctionData> ContainmentBind() {
	PostHogTelemetry::Instance().RecordFunctionCall("containment");
	return nullptr;
}
static unique_ptr<FunctionData> WeightedJaccardBind() {
	PostHogTelemetry::Instance().RecordFunctionCall("weighted_jaccard");
	return nullptr;
}
static unique_ptr<FunctionData> WeightedContainmentBind() {
	PostHogTelemetry::Instance().RecordFunctionCall("weighted_containment");
	return nullptr;
}
static unique_ptr<FunctionData> WeightedTverskyBind() {
	PostHogTelemetry::Instance().RecordFunctionCall("weighted_tversky");
	return nullptr;
}

struct Overload {
	vector<LogicalType> arguments;
	vector<string> names;
	string description;
	vector<string> examples;
};

void Register(ExtensionLoader &loader, const string &name, scalar_function_t fn, bind_scalar_function_t bind,
              const vector<Overload> &overloads) {
	ScalarFunctionSet set(duckdb_compat::FunctionName(name));
	for (auto &o : overloads) {
		ScalarFunction f(duckdb_compat::FunctionName(name), o.arguments, LogicalType::DOUBLE, fn, bind);
		duckdb_compat::SetFallible(f);
		set.AddFunction(f);
	}
	CreateScalarFunctionInfo info(set);
	for (auto &o : overloads) {
		FunctionDescription desc;
		desc.description = o.description;
		desc.examples = o.examples;
		desc.categories = {"similarity", "coverage"};
		desc.parameter_names = o.names;
		desc.parameter_types = o.arguments;
		info.descriptions.push_back(std::move(desc));
	}
	duckdb_compat::NameArgumentsFromDescriptions(info);
	loader.RegisterFunction(std::move(info));
}

} // namespace

void RegisterSetMeasureFunctions(ExtensionLoader &loader) {
	auto list = LogicalType::LIST(LogicalType::ANY);
	auto any = LogicalType::ANY;
	auto dbl = LogicalType::DOUBLE;

	const string tversky_desc =
	    "Asymmetric Tversky similarity of two lists treated as sets: |q∩c| / (|q∩c| + alpha*|q\\c| + "
	    "beta*|c\\q|). alpha = beta = 1 is Jaccard, alpha = 1 and beta = 0 is containment. Named arguments "
	    "bind by position on DuckDB 1.5.6: pass alpha and beta together, in this order. NULL in gives NULL; "
	    "two empty lists give 0.0.";
	Register(loader, "tversky_similarity",
	         DATAZOO_GUARD(ANOFOX_SIMILARITY_BANNER, TverskyFun),
	         compat_anofox::ScalarBind<&TverskyBind>(),
	         {{{list, list}, {"query", "candidate"}, tversky_desc,
	           {"SELECT tversky_similarity(['a','b','c'], ['b','c','d']); -- 0.5"}},
	          {{list, list, dbl, dbl}, {"query", "candidate", "alpha", "beta"}, tversky_desc,
	           {"SELECT tversky_similarity(['a','b'], ['a','b','c','d'], 1.0, 0.25); -- 0.8"}}});

	Register(loader, "containment",
	         DATAZOO_GUARD(ANOFOX_SIMILARITY_BANNER, ContainmentFun),
	         compat_anofox::ScalarBind<&ContainmentBind>(),
	         {{{list, list}, {"query", "candidate"},
	           "Share of the query set contained in the candidate set: |q∩c| / |q|. Directional: a richer "
	           "candidate is not penalised. Duplicates collapse. NULL in gives NULL; an empty query gives NULL.",
	           {"SELECT containment(['a','b'], ['a','b','c','d']); -- 1.0",
	            "SELECT containment(['a','b','c'], ['b','c','d']); -- 0.6667"}}});

	Register(loader, "weighted_jaccard",
	         DATAZOO_GUARD(ANOFOX_SIMILARITY_BANNER, WeightedTverskyFun),
	         compat_anofox::ScalarBind<&WeightedJaccardBind>(),
	         {{{any, any}, {"query", "candidate"},
	           "Weighted Jaccard similarity of two MAP(key, weight) multisets: Σ min(q_k, c_k) / Σ max(q_k, c_k). "
	           "Weights must be non-negative; a NULL weight counts as 1. Two empty maps give 0.0.",
	           {"SELECT weighted_jaccard(MAP {'heating': 2, 'massage': 4}, MAP {'heating': 4, 'ventilation': 2}); "
	            "-- 0.2"}}});

	Register(loader, "weighted_containment",
	         DATAZOO_GUARD(ANOFOX_SIMILARITY_BANNER, WeightedContainmentFun),
	         compat_anofox::ScalarBind<&WeightedContainmentBind>(),
	         {{{any, any}, {"query", "candidate"},
	           "Share of a MAP(key, weight) query multiset covered by the candidate: Σ min(q_k, c_k) / Σ q_k. Counts "
	           "matter: 4 requested zones are half covered by 2. Weights must be non-negative; a NULL weight counts "
	           "as 1. An empty query gives NULL.",
	           {"SELECT weighted_containment(MAP {'a': 3, 'b': 1}, MAP {'a': 2, 'b': 5}); -- 0.75"}}});

	const string wtversky_desc =
	    "Weighted Tversky similarity of two MAP(key, weight) multisets: Σmin / (Σmin + alpha*Σ(q-c)+ + "
	    "beta*Σ(c-q)+). alpha = beta = 1 is weighted Jaccard, alpha = 1 and beta = 0 is weighted containment. "
	    "Named arguments bind by position on DuckDB 1.5.6: pass alpha and beta together, in this order.";
	Register(loader, "weighted_tversky",
	         DATAZOO_GUARD(ANOFOX_SIMILARITY_BANNER, WeightedTverskyFun),
	         compat_anofox::ScalarBind<&WeightedTverskyBind>(),
	         {{{any, any}, {"query", "candidate"}, wtversky_desc,
	           {"SELECT weighted_tversky(MAP {'a': 3, 'b': 1}, MAP {'a': 2, 'c': 2}); -- 0.3333"}},
	          {{any, any, dbl, dbl}, {"query", "candidate", "alpha", "beta"}, wtversky_desc,
	           {"SELECT weighted_tversky(MAP {'a': 3, 'b': 1}, MAP {'a': 2, 'c': 2}, 0.5, 0.5); -- 0.5"}}});
}

} // namespace anofox
} // namespace duckdb
