#include "modules/jaccard_similarity.hpp"
#include "core/error_handling.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/common/vector_operations/binary_executor.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/main/connection.hpp"
#include "telemetry.hpp"

#include <unordered_set>

namespace duckdb {
namespace anofox {

//------------------------------------------------------------------------------
// Hash/Equality for Value types in unordered_set
//------------------------------------------------------------------------------

// Custom hash function for duckdb::Value
struct ValueHash {
	size_t operator()(const Value &val) const {
		return val.Hash();
	}
};

// Custom equality function for duckdb::Value
struct ValueEqual {
	bool operator()(const Value &a, const Value &b) const {
		// Use strict equality - same type and same value
		return a == b;
	}
};

//------------------------------------------------------------------------------
// Jaccard Similarity Scalar Function
//------------------------------------------------------------------------------

static void JaccardSimilarityFun(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &list_a = args.data[0];
	auto &list_b = args.data[1];
	auto count = args.size();

	// Get child vectors for both lists
	auto &child_a = ListVector::GetEntry(list_a);
	auto &child_b = ListVector::GetEntry(list_b);

	// Flatten vectors for uniform access
	UnifiedVectorFormat vdata_a, vdata_b;
	list_a.ToUnifiedFormat(count, vdata_a);
	list_b.ToUnifiedFormat(count, vdata_b);

	UnifiedVectorFormat child_vdata_a, child_vdata_b;
	auto child_count_a = ListVector::GetListSize(list_a);
	auto child_count_b = ListVector::GetListSize(list_b);
	child_a.ToUnifiedFormat(child_count_a, child_vdata_a);
	child_b.ToUnifiedFormat(child_count_b, child_vdata_b);

	auto list_entries_a = UnifiedVectorFormat::GetData<list_entry_t>(vdata_a);
	auto list_entries_b = UnifiedVectorFormat::GetData<list_entry_t>(vdata_b);

	result.SetVectorType(VectorType::FLAT_VECTOR);
	auto result_data = FlatVector::GetData<double>(result);
	auto &result_validity = FlatVector::Validity(result);

	for (idx_t i = 0; i < count; i++) {
		auto idx_a = vdata_a.sel->get_index(i);
		auto idx_b = vdata_b.sel->get_index(i);

		// Handle NULL inputs
		if (!vdata_a.validity.RowIsValid(idx_a) || !vdata_b.validity.RowIsValid(idx_b)) {
			result_validity.SetInvalid(i);
			continue;
		}

		auto &entry_a = list_entries_a[idx_a];
		auto &entry_b = list_entries_b[idx_b];

		// Handle empty lists: Jaccard(empty, empty) = 0.0
		if (entry_a.length == 0 && entry_b.length == 0) {
			result_data[i] = 0.0;
			continue;
		}

		// Build set from list A using Value for generic comparison
		std::unordered_set<Value, ValueHash, ValueEqual> set_a;
		for (idx_t j = 0; j < entry_a.length; j++) {
			auto child_idx = child_vdata_a.sel->get_index(entry_a.offset + j);
			if (child_vdata_a.validity.RowIsValid(child_idx)) {
				set_a.insert(child_a.GetValue(child_idx));
			}
		}

		// Build set from list B
		std::unordered_set<Value, ValueHash, ValueEqual> set_b;
		for (idx_t j = 0; j < entry_b.length; j++) {
			auto child_idx = child_vdata_b.sel->get_index(entry_b.offset + j);
			if (child_vdata_b.validity.RowIsValid(child_idx)) {
				set_b.insert(child_b.GetValue(child_idx));
			}
		}

		// Count intersection from deduplicated sets
		idx_t intersection_count = 0;
		for (const auto &val : set_a) {
			if (set_b.find(val) != set_b.end()) {
				intersection_count++;
			}
		}

		idx_t union_size = set_a.size() + set_b.size() - intersection_count;

		if (union_size == 0) {
			result_data[i] = 0.0;
		} else {
			result_data[i] = static_cast<double>(intersection_count) / static_cast<double>(union_size);
		}
	}

	if (args.AllConstant()) {
		result.SetVectorType(VectorType::CONSTANT_VECTOR);
	}
}

//------------------------------------------------------------------------------
// Telemetry Bind Function
//------------------------------------------------------------------------------

static unique_ptr<FunctionData> JaccardSimilarityBind(ClientContext &context, ScalarFunction &bound_function,
                                                      vector<unique_ptr<Expression>> &arguments) {
	PostHogTelemetry::Instance().CaptureFunctionExecution("jaccard_similarity");
	return nullptr;
}

//------------------------------------------------------------------------------
// Module Registration
//------------------------------------------------------------------------------

void RegisterJaccardFunctions(ExtensionLoader &loader) {
	// Register scalar function
	auto jaccard_similarity_function =
	    ScalarFunction("jaccard_similarity", {LogicalType::LIST(LogicalType::ANY), LogicalType::LIST(LogicalType::ANY)},
	                   LogicalType::DOUBLE, JaccardSimilarityFun, JaccardSimilarityBind);
	loader.RegisterFunction(jaccard_similarity_function);
}

} // namespace anofox
} // namespace duckdb
