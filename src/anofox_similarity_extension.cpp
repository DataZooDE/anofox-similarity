#define DUCKDB_EXTENSION_MAIN

#include "anofox_similarity_extension.hpp"
#include "duckdb.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/common/vector_operations/binary_executor.hpp"
#include "duckdb/function/scalar_function.hpp"
#include <duckdb/parser/parsed_data/create_scalar_function_info.hpp>

#include <unordered_set>

// OpenSSL linked through vcpkg
#include <openssl/opensslv.h>

namespace duckdb {

inline void AnofoxSimilarityScalarFun(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &name_vector = args.data[0];
	UnaryExecutor::Execute<string_t, string_t>(name_vector, result, args.size(), [&](string_t name) {
		return StringVector::AddString(result, "AnofoxSimilarity " + name.GetString() + " 🐥");
	});
}

inline void AnofoxSimilarityOpenSSLVersionScalarFun(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &name_vector = args.data[0];
	UnaryExecutor::Execute<string_t, string_t>(name_vector, result, args.size(), [&](string_t name) {
		return StringVector::AddString(result, "AnofoxSimilarity " + name.GetString() + ", my linked OpenSSL version is " +
		                                           OPENSSL_VERSION_TEXT);
	});
}

//------------------------------------------------------------------------------
// Jaccard Similarity Function
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
}

static void LoadInternal(ExtensionLoader &loader) {
	// Register a scalar function
	auto anofox_similarity_scalar_function = ScalarFunction("anofox_similarity", {LogicalType::VARCHAR}, LogicalType::VARCHAR, AnofoxSimilarityScalarFun);
	loader.RegisterFunction(anofox_similarity_scalar_function);

	// Register another scalar function
	auto anofox_similarity_openssl_version_scalar_function = ScalarFunction("anofox_similarity_openssl_version", {LogicalType::VARCHAR},
	                                                            LogicalType::VARCHAR, AnofoxSimilarityOpenSSLVersionScalarFun);
	loader.RegisterFunction(anofox_similarity_openssl_version_scalar_function);

	// Register jaccard_similarity function for computing set similarity
	// Accepts two lists of any type and returns Jaccard coefficient (0.0 to 1.0)
	auto jaccard_similarity_function = ScalarFunction(
	    "jaccard_similarity",
	    {LogicalType::LIST(LogicalType::ANY), LogicalType::LIST(LogicalType::ANY)},
	    LogicalType::DOUBLE,
	    JaccardSimilarityFun
	);
	loader.RegisterFunction(jaccard_similarity_function);
}

void AnofoxSimilarityExtension::Load(ExtensionLoader &loader) {
	LoadInternal(loader);
}
std::string AnofoxSimilarityExtension::Name() {
	return "anofox_similarity";
}

std::string AnofoxSimilarityExtension::Version() const {
#ifdef EXT_VERSION_ANOFOX_SIMILARITY
	return EXT_VERSION_ANOFOX_SIMILARITY;
#else
	return "";
#endif
}

} // namespace duckdb

extern "C" {

DUCKDB_CPP_EXTENSION_ENTRY(anofox_similarity, loader) {
	duckdb::LoadInternal(loader);
}
}
