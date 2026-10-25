#include "modules/set_measures.hpp"
#include "duckdb_compat.hpp"
#include "duckdb_compat/list_vector.hpp"
#include "duckdb_compat/struct_vector.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"
#include "compat_anofox/scalar_bind.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/parser/parsed_data/create_scalar_function_info.hpp"
#include "telemetry.hpp"
#include "anofox_similarity_banner.hpp"

#include <cmath>
#include <unordered_map>
#include <unordered_set>

namespace duckdb {
namespace anofox {

namespace {

//! The overlap of a query and a candidate. For sets every distinct element weighs 1.
//! missing = Σ(q-c)+ = Σq - Σmin and surplus = Σ(c-q)+ = Σc - Σmin.
struct Overlap {
	double shared = 0;
	double query_total = 0;
	double candidate_total = 0;
	double Missing() const {
		return query_total - shared;
	}
	double Surplus() const {
		return candidate_total - shared;
	}
};

//! Per-chunk access to the elements of a LIST or MAP argument without building a
//! Value per row. Elements are compared by their text form, read straight from the
//! string data for VARCHAR keys.
class Elements {
public:
	Elements(Vector &input, idx_t count, bool weighted) : input(input), weighted(weighted) {
		duckdb_compat::ToUnifiedFormat(input, count, list_format);
		if (input.GetType().id() == LogicalTypeId::SQLNULL) {
			all_null = true; // an untyped NULL literal
			return;
		}
		auto size = ListVector::GetListSize(input);
		auto &child = ListVector::GetEntry(input);
		if (weighted) {
			if (input.GetType().id() != LogicalTypeId::MAP) {
				throw InvalidInputException("weighted set measures expect MAP arguments, got %s",
				                            input.GetType().ToString());
			}
			auto &entries = StructVector::GetEntries(child);
			keys = &duckdb_compat::StructChild(entries[0]);
			auto &values = duckdb_compat::StructChild(entries[1]);
			if (!values.GetType().IsNumeric()) {
				throw InvalidInputException("weights must be numeric, got %s", values.GetType().ToString());
			}
			weights = make_uniq<Vector>(LogicalType::DOUBLE, MaxValue<idx_t>(size, 1));
			VectorOperations::DefaultCast(values, *weights, size);
			duckdb_compat::ToUnifiedFormat(*weights, size, weight_format);
		} else {
			keys = &child;
		}
		duckdb_compat::ToUnifiedFormat(*keys, size, key_format);
		varchar_keys = keys->GetType().InternalType() == PhysicalType::VARCHAR;
	}

	bool IsNull(idx_t row) const {
		return all_null || !list_format.validity.RowIsValid(list_format.sel->get_index(row));
	}
	const list_entry_t &Entry(idx_t row) const {
		return UnifiedVectorFormat::GetData<list_entry_t>(list_format)[list_format.sel->get_index(row)];
	}
	//! The key at child position i, or false for a NULL key.
	bool Key(idx_t i, string &out) const {
		auto idx = key_format.sel->get_index(i);
		if (!key_format.validity.RowIsValid(idx)) {
			return false;
		}
		if (varchar_keys) {
			auto &str = UnifiedVectorFormat::GetData<string_t>(key_format)[idx];
			out.assign(str.GetData(), str.GetSize());
		} else {
			out = keys->GetValue(i).ToString();
		}
		return true;
	}
	//! The weight at child position i: NULL counts as 1; must be finite and non-negative.
	double Weight(idx_t i) const {
		auto idx = weight_format.sel->get_index(i);
		if (!weight_format.validity.RowIsValid(idx)) {
			return 1.0;
		}
		auto w = UnifiedVectorFormat::GetData<double>(weight_format)[idx];
		if (!std::isfinite(w)) {
			throw InvalidInputException("weights must be finite, got %g", w);
		}
		if (w < 0) {
			throw InvalidInputException("weights must be non-negative, got %g", w);
		}
		return w;
	}

	Vector &input;
	bool weighted;

private:
	UnifiedVectorFormat list_format;
	UnifiedVectorFormat key_format;
	UnifiedVectorFormat weight_format;
	Vector *keys = nullptr;
	unique_ptr<Vector> weights;
	bool varchar_keys = false;
	bool all_null = false;
};

using WeightMap = std::unordered_map<string, double>;

//! The query side as a weight map (sets: weight 1 per distinct element).
WeightMap QueryWeights(const Elements &q, idx_t row) {
	WeightMap out;
	auto &entry = q.Entry(row);
	string key;
	for (idx_t j = 0; j < entry.length; j++) {
		auto i = entry.offset + j;
		if (!q.Key(i, key)) {
			continue;
		}
		if (q.weighted) {
			out[key] += q.Weight(i);
		} else {
			out[key] = 1.0;
		}
	}
	return out;
}

Overlap Compare(const WeightMap &query, const Elements &c, idx_t row) {
	Overlap o;
	for (auto &kv : query) {
		o.query_total += kv.second;
	}
	auto &entry = c.Entry(row);
	string key;
	if (c.weighted) {
		WeightMap candidate;
		for (idx_t j = 0; j < entry.length; j++) {
			auto i = entry.offset + j;
			if (c.Key(i, key)) {
				candidate[key] += c.Weight(i);
			}
		}
		for (auto &kv : candidate) {
			o.candidate_total += kv.second;
			auto it = query.find(kv.first);
			if (it != query.end()) {
				o.shared += std::min(it->second, kv.second);
			}
		}
	} else {
		std::unordered_set<string> seen;
		for (idx_t j = 0; j < entry.length; j++) {
			if (c.Key(entry.offset + j, key) && seen.insert(key).second) {
				o.candidate_total += 1;
				o.shared += query.count(key) ? 1 : 0;
			}
		}
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
	double denominator = o.shared + alpha * o.Missing() + beta * o.Surplus();
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

template <bool WEIGHTED, Measure MEASURE>
void SetMeasureFun(DataChunk &args, ExpressionState &, Vector &result) {
	auto count = args.size();
	Elements query(args.data[0], count, WEIGHTED);
	Elements candidate(args.data[1], count, WEIGHTED);
	bool with_parameters = args.ColumnCount() == 4;
	UnifiedVectorFormat alpha_format, beta_format;
	if (with_parameters) {
		duckdb_compat::ToUnifiedFormat(args.data[2], count, alpha_format);
		duckdb_compat::ToUnifiedFormat(args.data[3], count, beta_format);
	}
	// a constant query (the usual case: one request against many rows) is read once
	bool constant_query = args.data[0].GetVectorType() == VectorType::CONSTANT_VECTOR;
	WeightMap cached;
	bool cached_ready = false;

	result.SetVectorType(VectorType::FLAT_VECTOR);
	auto result_data = duckdb_compat::FlatData<double>(result);
	auto &validity = duckdb_compat::MutableValidity(result);
	for (idx_t row = 0; row < count; row++) {
		double alpha = 1.0, beta = 1.0;
		if (with_parameters) {
			auto ai = alpha_format.sel->get_index(row);
			auto bi = beta_format.sel->get_index(row);
			if (!alpha_format.validity.RowIsValid(ai) || !beta_format.validity.RowIsValid(bi)) {
				validity.SetInvalid(row);
				continue;
			}
			alpha = UnifiedVectorFormat::GetData<double>(alpha_format)[ai];
			beta = UnifiedVectorFormat::GetData<double>(beta_format)[bi];
			if (!std::isfinite(alpha) || !std::isfinite(beta)) {
				throw InvalidInputException("alpha and beta must be finite");
			}
			if (alpha < 0 || beta < 0) {
				throw InvalidInputException("alpha and beta must be non-negative");
			}
		}
		if (query.IsNull(row) || candidate.IsNull(row)) {
			validity.SetInvalid(row);
			continue;
		}
		WeightMap local;
		const WeightMap *q = &local;
		if (constant_query) {
			if (!cached_ready) {
				cached = QueryWeights(query, row);
				cached_ready = true;
			}
			q = &cached;
		} else {
			local = QueryWeights(query, row);
		}
		double value;
		if (!Compute(Compare(*q, candidate, row), MEASURE, alpha, beta, value)) {
			validity.SetInvalid(row);
			continue;
		}
		result_data[row] = value;
	}
	if (args.AllConstant()) {
		result.SetVectorType(VectorType::CONSTANT_VECTOR);
	}
}

void TverskyFun(DataChunk &args, ExpressionState &state, Vector &result) {
	SetMeasureFun<false, Measure::TVERSKY>(args, state, result);
}
void ContainmentFun(DataChunk &args, ExpressionState &state, Vector &result) {
	SetMeasureFun<false, Measure::CONTAINMENT>(args, state, result);
}
void WeightedTverskyFun(DataChunk &args, ExpressionState &state, Vector &result) {
	SetMeasureFun<true, Measure::TVERSKY>(args, state, result);
}
void WeightedContainmentFun(DataChunk &args, ExpressionState &state, Vector &result) {
	SetMeasureFun<true, Measure::CONTAINMENT>(args, state, result);
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
